// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_CurrentLocation.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Querier.h"
#if WITH_STATETREE
#include "Tasks/StateTreeRunParallelStateTreeTask.h"
#include "Conditions/StateTreeCommonConditions.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace MonolithAIDiscoveryTest
{
	static TSharedPtr<FJsonObject> Params(const FString& System)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("system"), System);
		return Result;
	}

	static FMonolithActionResult Invoke(const TSharedPtr<FJsonObject>& Arguments)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), TEXT("list_ai_node_types"), Arguments);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAINodeDiscoveryEQSTest, "Monolith.AI.Discovery.EQSNativeTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAINodeDiscoveryEQSTest::RunTest(const FString& Parameters)
{
	using namespace MonolithAIDiscoveryTest;
	const TArray<TPair<FString, UClass*>> Expected = {
		{ TEXT("generator"), UEnvQueryGenerator_CurrentLocation::StaticClass() },
		{ TEXT("test"), UEnvQueryTest_Distance::StaticClass() },
		{ TEXT("context"), UEnvQueryContext_Querier::StaticClass() }
	};
	for (const auto& Pair : Expected)
	{
		auto Args = Params(TEXT("eqs")); Args->SetStringField(TEXT("category"), Pair.Key);
		const auto Result = Invoke(Args);
		if (!TestTrue(TEXT("EQS category succeeds"), Result.bSuccess && Result.Result.IsValid())) continue;
		const auto& Nodes = Result.Result->GetArrayField(TEXT("node_types"));
		TestEqual(TEXT("count matches entries"), Result.Result->GetIntegerField(TEXT("count")), Nodes.Num());
		bool bFound = false;
		TSet<FString> Paths;
		for (const auto& Node : Nodes)
		{
			const auto Entry = Node->AsObject();
			TestEqual(TEXT("category filter excludes other types"), Entry->GetStringField(TEXT("category")), Pair.Key);
			const FString Path = Entry->GetStringField(TEXT("class_path"));
			TestFalse(TEXT("unique class path"), Paths.Contains(Path)); Paths.Add(Path);
			UClass* Class = FindObject<UClass>(nullptr, *Path);
			TestTrue(TEXT("entry resolves to concrete loaded class"), Class && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists));
			bFound |= Path == Pair.Value->GetPathName();
		}
		TestTrue(FString::Printf(TEXT("native %s type discovered"), *Pair.Key), bFound);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAINodeDiscoveryStateTreeTest, "Monolith.AI.Discovery.StateTreeNativeTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAINodeDiscoveryStateTreeTest::RunTest(const FString& Parameters)
{
	using namespace MonolithAIDiscoveryTest;
#if WITH_STATETREE
	const TArray<TPair<FString, UScriptStruct*>> Expected = {
		{ TEXT("task"), FStateTreeRunParallelStateTreeTask::StaticStruct() },
		{ TEXT("condition"), FStateTreeCompareIntCondition::StaticStruct() },
		{ TEXT("evaluator"), FStateTreeBlueprintEvaluatorWrapper::StaticStruct() }
	};
	for (const auto& Pair : Expected)
	{
		auto Args = Params(TEXT("st")); Args->SetStringField(TEXT("category"), Pair.Key);
		const auto Result = Invoke(Args);
		if (!TestTrue(TEXT("ST category succeeds"), Result.bSuccess && Result.Result.IsValid())) continue;
		const auto& Nodes = Result.Result->GetArrayField(TEXT("node_types"));
		TestEqual(TEXT("count matches entries"), Result.Result->GetIntegerField(TEXT("count")), Nodes.Num());
		bool bFound = false;
		for (const auto& Node : Nodes)
		{
			const auto Entry = Node->AsObject();
			TestEqual(TEXT("ST category isolation"), Entry->GetStringField(TEXT("category")), Pair.Key);
			TestEqual(TEXT("ST representation is struct"), Entry->GetStringField(TEXT("kind")), FString(TEXT("struct")));
			const FString Path = Entry->GetStringField(TEXT("struct_path"));
			UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *Path);
			TestTrue(TEXT("entry resolves to non-hidden struct"), Struct && !Struct->HasMetaData(TEXT("Hidden")));
			bFound |= Path == Pair.Value->GetPathName();
		}
		TestTrue(FString::Printf(TEXT("native %s struct discovered"), *Pair.Key), bFound);
	}
#else
	const auto Result = Invoke(Params(TEXT("st")));
	TestFalse(TEXT("missing dependency is not successful empty enumeration"), Result.bSuccess);
	TestEqual(TEXT("typed optional dependency error"), Result.ErrorCode, FMonolithJsonUtils::ErrOptionalDepUnavailable);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAINodeDiscoveryInputsTest, "Monolith.AI.Discovery.InvalidInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAINodeDiscoveryInputsTest::RunTest(const FString& Parameters)
{
	using namespace MonolithAIDiscoveryTest;
	TArray<TPair<FString, TSharedPtr<FJsonObject>>> Cases;
	Cases.Emplace(TEXT("unknown system"), Params(TEXT("unknown")));
	Cases.Emplace(TEXT("blank system"), Params(TEXT("  ")));
	Cases.Emplace(TEXT("missing system"), MakeShared<FJsonObject>());
	auto NumericSystem = MakeShared<FJsonObject>();
	NumericSystem->SetNumberField(TEXT("system"), 7);
	Cases.Emplace(TEXT("numeric system"), NumericSystem);
	auto NumericCategory = Params(TEXT("bt"));
	NumericCategory->SetNumberField(TEXT("category"), 7);
	Cases.Emplace(TEXT("numeric category"), NumericCategory);
	auto UnknownCategory = Params(TEXT("bt"));
	UnknownCategory->SetStringField(TEXT("category"), TEXT("typo"));
	Cases.Emplace(TEXT("unknown category"), UnknownCategory);
	for (const FString& System : { FString(TEXT("eqs")), FString(TEXT("st")) })
	{
		auto InvalidCategory = Params(System); InvalidCategory->SetStringField(TEXT("category"), TEXT("typo"));
		Cases.Emplace(System + TEXT(" unknown category"), InvalidCategory);
	}
	for (const auto& Case : Cases)
	{
		const auto Result = Invoke(Case.Value);
		TestFalse(FString::Printf(TEXT("%s is rejected"), *Case.Key), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s produces invalid params (%s)"), *Case.Key, *Result.ErrorMessage), Result.ErrorCode, -32602);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAINodeDiscoveryBehaviorTreeTest, "Monolith.AI.Discovery.BehaviorTreeDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAINodeDiscoveryBehaviorTreeTest::RunTest(const FString& Parameters)
{
	using namespace MonolithAIDiscoveryTest;
	// Force a known non-abstract engine task class to be loaded before discovery.
	const FString ExpectedClass = UBTTask_Wait::StaticClass()->GetName();
	const auto Result = Invoke(Params(TEXT("bt")));
	if (!TestTrue(TEXT("BT discovery works without optional category"), Result.bSuccess && Result.Result.IsValid())) { return false; }
	const auto& Nodes = Result.Result->GetArrayField(TEXT("node_types"));
	TestEqual(TEXT("Count matches returned entries"), Result.Result->GetIntegerField(TEXT("count")), Nodes.Num());
	bool bFoundWait = false;
	for (const auto& Node : Nodes)
	{
		const auto Entry = Node->AsObject();
		if (Entry.IsValid() && Entry->GetStringField(TEXT("class_name")) == ExpectedClass)
		{
			bFoundWait = true;
			TestEqual(TEXT("Engine task is correctly categorized"), Entry->GetStringField(TEXT("category")), FString(TEXT("task")));
		}
	}
	TestTrue(TEXT("Available engine Wait task is enumerated"), bFoundWait);
	return true;
}
#endif
