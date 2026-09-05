// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"

namespace MonolithStateMachineHonestyTest
{
TSharedPtr<FJsonValue> State(const TCHAR* Name)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), Name);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> Transition(const TCHAR* From, const TCHAR* To, const TSharedPtr<FJsonValue>& Rule)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("from"), From);
	Out->SetStringField(TEXT("to"), To);
	Out->SetField(TEXT("rule"), Rule);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonObject> Compare(const TCHAR* Variable)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("kind"), TEXT("compare"));
	Out->SetStringField(TEXT("lhs"), Variable);
	Out->SetStringField(TEXT("op"), TEXT(">"));
	Out->SetNumberField(TEXT("rhs"), 1.0);
	return Out;
}

UAnimationStateMachineGraph* FindMachine(UAnimationGraph* AnimGraph, const FString& GraphName)
{
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		const auto* Machine = Cast<UAnimGraphNode_StateMachine>(Node);
		if (Machine && Machine->EditorStateMachineGraph && Machine->EditorStateMachineGraph->GetName() == GraphName)
			return Cast<UAnimationStateMachineGraph>(Machine->EditorStateMachineGraph);
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithStateMachineHonestyTest,
	"Monolith.Animation.Honesty.DeferredRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStateMachineHonestyTest::RunTest(const FString&)
{
	using namespace MonolithStateMachineHonestyTest;
	const FString Name = TEXT("ABP_StateMachineHonesty_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* Package = CreatePackage(*(TEXT("/Game/Tests/Monolith/Animation/") + Name));
	USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!TestNotNull(TEXT("Create fixture skeleton"), Skeleton)) return false;
	{
		FReferenceSkeletonModifier Modifier(Skeleton);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
	}
	UAnimBlueprint* ABP = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), Package, FName(*Name), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(), NAME_None));
	ON_SCOPE_EXIT
	{
		if (ABP)
		{
			FAssetRegistryModule::AssetDeleted(ABP);
			ABP->ClearFlags(RF_Public | RF_Standalone);
			ABP->MarkAsGarbage();
		}
		Package->SetDirtyFlag(false);
		Package->ClearFlags(RF_Public | RF_Standalone);
		Package->MarkAsGarbage();
		Skeleton->MarkAsGarbage();
	};
	if (!TestNotNull(TEXT("Create GUID-owned Animation Blueprint"), ABP)) return false;
	FAssetRegistryModule::AssetCreated(ABP);
	ABP->TargetSkeleton = Skeleton;
	if (auto* Generated = Cast<UAnimBlueprintGeneratedClass>(ABP->GeneratedClass)) Generated->TargetSkeleton = Skeleton;
	if (auto* Generated = Cast<UAnimBlueprintGeneratedClass>(ABP->SkeletonGeneratedClass)) Generated->TargetSkeleton = Skeleton;
	FEdGraphPinType IntegerType;
	IntegerType.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!TestTrue(TEXT("Declare wrong-type boolean-rule operand"),
		FBlueprintEditorUtils::AddMemberVariable(ABP, FName(TEXT("FixtureNumber")), IntegerType))) return false;
	FKismetEditorUtilities::CompileBlueprint(ABP);
	if (!TestTrue(TEXT("Fixture Animation Blueprint compiles"), ABP->Status == BS_UpToDate || ABP->Status == BS_UpToDateWithWarnings)) return false;
	UAnimationGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if ((AnimGraph = Cast<UAnimationGraph>(Graph))) break;
	}
	if (!TestNotNull(TEXT("Actual Animation Blueprint has an AnimGraph"), AnimGraph)) return false;

	auto Expression = MakeShared<FJsonObject>();
	Expression->SetStringField(TEXT("kind"), TEXT("expression"));
	Expression->SetStringField(TEXT("combine"), TEXT("and"));
	Expression->SetArrayField(TEXT("terms"), {MakeShared<FJsonValueObject>(Compare(TEXT("FixtureNumber")))});
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), ABP->GetPathName());
	Params->SetStringField(TEXT("state_machine_name"), TEXT("DeferredFixture"));
	Params->SetStringField(TEXT("entry_state"), TEXT("A"));
	Params->SetArrayField(TEXT("states"), {State(TEXT("A")), State(TEXT("B")), State(TEXT("C")), State(TEXT("D")), State(TEXT("E"))});
	Params->SetArrayField(TEXT("transitions"), {
		Transition(TEXT("A"), TEXT("B"), MakeShared<FJsonValueObject>(Expression)),
		Transition(TEXT("B"), TEXT("C"), MakeShared<FJsonValueObject>(Compare(TEXT("MissingNumericOperand")))),
		Transition(TEXT("C"), TEXT("D"), MakeShared<FJsonValueString>(TEXT("MissingBoolOperand"))),
		Transition(TEXT("D"), TEXT("E"), MakeShared<FJsonValueString>(TEXT("FixtureNumber"))),
		Transition(TEXT("E"), TEXT("A"), MakeShared<FJsonValueString>(TEXT("auto")))});
	const auto Partial = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("build_state_machine"), Params);
	if (!TestTrue(TEXT("Builder retains actual work when rules are deferred"), Partial.bSuccess && Partial.Result.IsValid())) return false;
	TestTrue(TEXT("Builder reports partial completion at top level"), Partial.Result->GetBoolField(TEXT("partial")));
	TestEqual(TEXT("All four deferred-rule paths are counted"), Partial.Result->GetIntegerField(TEXT("deferred_rules")), 4);
	TestTrue(TEXT("Entry state was wired"), Partial.Result->GetBoolField(TEXT("entry_wired")));
	TestTrue(TEXT("Builder leaves package dirty"), Package->IsDirty());
	TestTrue(TEXT("Builder leaves an up-to-date compiled Blueprint"), ABP->Status == BS_UpToDate || ABP->Status == BS_UpToDateWithWarnings);
	const auto& Reports = Partial.Result->GetArrayField(TEXT("transitions_report"));
	if (TestEqual(TEXT("Every transition has a report"), Reports.Num(), 5))
	{
		const TCHAR* Reasons[] = {TEXT("kind:expression"), TEXT("not a usable numeric variable"), TEXT("not a known ABP variable"), TEXT("not bool")};
		for (int32 Index = 0; Index < 5; ++Index)
		{
			const auto Report = Reports[Index]->AsObject();
			TestTrue(TEXT("Transition creation succeeded even for deferred rules"), Report->GetBoolField(TEXT("created")));
			if (Index < 4)
			{
				TestTrue(TEXT("Per-transition message identifies each deferred branch"), Report->GetStringField(TEXT("rule_deferred")).Contains(Reasons[Index]));
				TestFalse(TEXT("Deferred rule is not reported applied"), Report->HasField(TEXT("rule_applied")));
			}
			else TestEqual(TEXT("Supported automatic rule is applied"), Report->GetStringField(TEXT("rule_applied")), FString(TEXT("automatic")));
		}
	}
	auto CheckGraph = [&](const FMonolithActionResult& Result, int32 ExpectedStates, int32 ExpectedTransitions)
	{
		UAnimationStateMachineGraph* Machine = FindMachine(AnimGraph, Result.Result->GetStringField(TEXT("state_machine_graph")));
		if (!TestNotNull(TEXT("Result identifies an actual created state-machine graph"), Machine)) return;
		int32 States = 0, Transitions = 0, AutomaticRules = 0;
		for (UEdGraphNode* Node : Machine->Nodes)
		{
			if (Cast<UAnimStateNode>(Node)) ++States;
			if (auto* TransitionNode = Cast<UAnimStateTransitionNode>(Node))
			{
				++Transitions;
				TestNotNull(TEXT("Actual transition has previous state"), TransitionNode->GetPreviousState());
				TestNotNull(TEXT("Actual transition has next state"), TransitionNode->GetNextState());
				TestNotNull(TEXT("Actual transition owns a rule graph"), TransitionNode->GetBoundGraph());
				if (TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState) ++AutomaticRules;
			}
		}
		TestEqual(TEXT("State graph contains every requested state"), States, ExpectedStates);
		TestEqual(TEXT("State graph contains every requested transition"), Transitions, ExpectedTransitions);
		TestEqual(TEXT("Automatic rule flag is really authored"), AutomaticRules, 1);
	};
	CheckGraph(Partial, 5, 5);

	Params->SetStringField(TEXT("state_machine_name"), TEXT("CompleteFixture"));
	Params->SetArrayField(TEXT("states"), {State(TEXT("A")), State(TEXT("B"))});
	Params->SetArrayField(TEXT("transitions"), {
		Transition(TEXT("A"), TEXT("B"), MakeShared<FJsonValueString>(TEXT("auto")))});
	const auto Complete = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("build_state_machine"), Params);
	if (!TestTrue(TEXT("Supported builder call succeeds"), Complete.bSuccess && Complete.Result.IsValid())) return false;
	TestFalse(TEXT("Supported builder call is not marked partial"), Complete.Result->GetBoolField(TEXT("partial")));
	TestEqual(TEXT("Supported builder call has zero deferred rules"), Complete.Result->GetIntegerField(TEXT("deferred_rules")), 0);
	TestTrue(TEXT("Second builder leaves an up-to-date compiled Blueprint"), ABP->Status == BS_UpToDate || ABP->Status == BS_UpToDateWithWarnings);
	CheckGraph(Complete, 2, 1);
	TestTrue(TEXT("First machine survives the second builder call"), FindMachine(AnimGraph, Partial.Result->GetStringField(TEXT("state_machine_graph"))) != nullptr);
	return true;
}

#endif
