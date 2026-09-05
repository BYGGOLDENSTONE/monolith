// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAINodeDiscoveryUnavailableTest, "Monolith.AI.Discovery.UnimplementedSystems",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAINodeDiscoveryUnavailableTest::RunTest(const FString& Parameters)
{
	using namespace MonolithAIDiscoveryTest;
	for (const FString System : { FString(TEXT("st")), FString(TEXT("eqs")) })
	{
		const auto Result = Invoke(Params(System));
		TestFalse(TEXT("Unimplemented enumeration is not a successful empty list"), Result.bSuccess);
		TestEqual(TEXT("Capability unavailable error"), Result.ErrorCode, FMonolithJsonUtils::ErrNotImplemented);
		TestFalse(TEXT("No successful node payload"), Result.Result.IsValid());
		if (!TestTrue(TEXT("Structured error data present"), Result.ErrorData.IsValid())) { continue; }
		const auto Data = Result.ErrorData->AsObject();
		if (!TestTrue(TEXT("Structured data is an object"), Data.IsValid())) { continue; }
		TestEqual(TEXT("Machine-readable reason"), Data->GetStringField(TEXT("reason")), FString(TEXT("not_implemented")));
		TestEqual(TEXT("Requested system retained"), Data->GetStringField(TEXT("system")), System);
		TestFalse(TEXT("Explicit implementation state"), Data->GetBoolField(TEXT("implemented")));
		TestFalse(TEXT("Error is actionable"), Result.ErrorMessage.IsEmpty());
	}
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
