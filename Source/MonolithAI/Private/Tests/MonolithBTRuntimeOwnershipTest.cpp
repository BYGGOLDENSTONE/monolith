#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "UObject/UObjectHash.h"
#include "ObjectTools.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBTRuntimeOwnershipTest, "Monolith.AI.BehaviorTree.RuntimeOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithBTRuntimeOwnershipTest::RunTest(const FString&)
{
	const FString Base = TEXT("/Game/Tests/Monolith/AI/Ownership_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = Base + TEXT("_Manual"), SpecPath = Base + TEXT("_Spec");
	TArray<FString> Paths = { Path, SpecPath };
	ON_SCOPE_EXIT
	{
		for (const FString& OwnedPath : Paths)
		{
			const FString ObjectPath = OwnedPath + TEXT(".") + FPackageName::GetLongPackageAssetName(OwnedPath);
			UBehaviorTree* Owned = FindObject<UBehaviorTree>(nullptr, *ObjectPath);
			if (IsValid(Owned))
			{
				TArray<UObject*> Assets = { Owned };
				TestEqual(TEXT("delete exact owned in-memory BT fixture"), ObjectTools::ForceDeleteObjects(Assets, false), 1);
			}
			TestFalse(TEXT("no saved fixture remains"), IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(OwnedPath, FPackageName::GetAssetPackageExtension())));
		}
	};
	auto Invoke = [&](const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), Action, Params);
		TestTrue(FString::Printf(TEXT("%s succeeds: %s"), Action, *R.ErrorMessage), R.bSuccess);
		return R;
	};
	auto VerifyRuntimeOwners = [&](UBehaviorTree* BT)
	{
		if (!TestNotNull(TEXT("runtime BT root exists"), BT ? BT->RootNode.Get() : nullptr)) return;
		TArray<UObject*> Objects;
		GetObjectsWithOuter(BT, Objects, true);
		int32 RuntimeNodes = 0;
		for (UObject* Object : Objects)
		{
			UBTNode* Node = Cast<UBTNode>(Object);
			if (!Node) continue;
			++RuntimeNodes;
			TestTrue(FString::Printf(TEXT("runtime node %s is directly owned by asset"), *Node->GetName()), Node->GetOuter() == BT);
			TestFalse(TEXT("runtime instance is not transient"), Node->HasAnyFlags(RF_Transient));
			TestNull(TEXT("runtime instance has no editor graph owner"), Node->GetTypedOuter<UEdGraph>());
			for (UObject* Outer = Node; Outer && Outer != BT->GetOuter(); Outer = Outer->GetOuter())
				TestFalse(TEXT("runtime owner chain is not editor-only"), Outer->IsEditorOnly());
		}
		TestTrue(TEXT("root/task/decorator/service instances present"), RuntimeNodes >= 4);
	};
	auto Create = MakeShared<FJsonObject>(); Create->SetStringField(TEXT("save_path"), Path);
	if (!Invoke(TEXT("create_behavior_tree"), Create).bSuccess) return false;
	UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *(Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path)));
	if (!TestNotNull(TEXT("load created tree"), BT)) return false;
	auto Add = MakeShared<FJsonObject>(); Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("node_class"), TEXT("BTComposite_Sequence"));
	auto Composite = Invoke(TEXT("add_bt_node"), Add);
	if (!Composite.bSuccess || !Composite.Result.IsValid()) return false;
	const FString RootGuid = Composite.Result->GetStringField(TEXT("node_id"));
	Add->SetStringField(TEXT("node_class"), UBTTask_Wait::StaticClass()->GetName()); Add->SetStringField(TEXT("parent_id"), RootGuid);
	auto Task = Invoke(TEXT("add_bt_node"), Add);
	if (!Task.bSuccess || !Task.Result.IsValid()) return false;
	auto Decorator = MakeShared<FJsonObject>(); Decorator->SetStringField(TEXT("asset_path"), Path);
	Decorator->SetStringField(TEXT("node_id"), Task.Result->GetStringField(TEXT("node_id")));
	Decorator->SetStringField(TEXT("decorator_class"), UBTDecorator_Cooldown::StaticClass()->GetName());
	if (!Invoke(TEXT("add_bt_decorator"), Decorator).bSuccess) return false;
	auto Service = MakeShared<FJsonObject>(); Service->SetStringField(TEXT("asset_path"), Path);
	Service->SetStringField(TEXT("node_id"), RootGuid); Service->SetStringField(TEXT("service_class"), UBTService_DefaultFocus::StaticClass()->GetName());
	if (!Invoke(TEXT("add_bt_service"), Service).bSuccess) return false;
	VerifyRuntimeOwners(BT);
	if (!BT->RootNode || !TestEqual(TEXT("runtime child retained"), BT->RootNode->Children.Num(), 1)) return false;
	TestEqual(TEXT("runtime decorator retained"), BT->RootNode->Children[0].Decorators.Num(), 1);
	TestEqual(TEXT("runtime service retained"), BT->RootNode->Services.Num(), 1);
	// Reproduce the old bad Outer on an existing asset. A mutating rebuild must
	// repair it; read-only loading does not silently rewrite users' packages.
	UBehaviorTreeGraphNode* RootGraphNode = nullptr;
	for (UEdGraphNode* Node : BT->BTGraph->Nodes)
	{
		UBehaviorTreeGraphNode* Candidate = Cast<UBehaviorTreeGraphNode>(Node);
		if (Candidate && Candidate->NodeInstance.Get() == BT->RootNode.Get()) { RootGraphNode = Candidate; break; }
	}
	if (!TestNotNull(TEXT("root graph node found"), RootGraphNode)) return false;
	if (!TestTrue(TEXT("simulate legacy editor-owned root"), BT->RootNode->Rename(nullptr, RootGraphNode, REN_DontCreateRedirectors))) return false;
	if (!Invoke(TEXT("add_bt_node"), Add).bSuccess) return false;
	VerifyRuntimeOwners(BT);
	TestEqual(TEXT("legacy repair preserves and extends runtime children"), BT->RootNode->Children.Num(), 2);
	// Spec authoring uses a separate allocation helper; exercise that actual path.
	auto SpecParams = MakeShared<FJsonObject>(); SpecParams->SetStringField(TEXT("save_path"), SpecPath);
	SpecParams->SetObjectField(TEXT("spec"), FMonolithJsonUtils::Parse(TEXT(R"JSON({"root":{"type":"BTComposite_Sequence","services":[{"class":"BTService_DefaultFocus"}],"children":[{"type":"BTTask_Wait","decorators":[{"class":"BTDecorator_Cooldown"}]}]}})JSON")));
	if (!Invoke(TEXT("build_behavior_tree_from_spec"), SpecParams).bSuccess) return false;
	UBehaviorTree* SpecBT = LoadObject<UBehaviorTree>(nullptr, *(SpecPath + TEXT(".") + FPackageName::GetLongPackageAssetName(SpecPath)));
	VerifyRuntimeOwners(SpecBT);
	return true;
}
#endif
