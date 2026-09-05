// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#if WITH_GEOMETRYSCRIPT
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshIntegrationHooksRemovedTest,
	"Monolith.Mesh.Honesty.IntegrationHooksRemoved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshIntegrationHooksRemovedTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	for (const FMonolithActionInfo& Action : Registry.GetActions(TEXT("mesh")))
	{
		TestNotEqual(TEXT("Planned integrations are not callable actions"), Action.Action, FString(TEXT("integration_hooks_stub")));
	}
	const auto Result = Registry.ExecuteAction(TEXT("mesh"), TEXT("integration_hooks_stub"), MakeShared<FJsonObject>());
	TestFalse(TEXT("Removed action cannot return successful plans"), Result.bSuccess);
	TestEqual(TEXT("Removed action uses method-not-found"), Result.ErrorCode, FMonolithJsonUtils::ErrMethodNotFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshCoOpUnavailableTest,
	"Monolith.Mesh.Honesty.CoOpScoringUnavailable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshCoOpUnavailableTest::RunTest(const FString& Parameters)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("player_positions"), {
		MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0)}),
		MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(1000), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(0)}) });
	const auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("analyze_co_op_balance"), Params);
	TestFalse(TEXT("Unimplemented scoring does not succeed"), Result.bSuccess);
	TestEqual(TEXT("Scoring returns not-implemented code"), Result.ErrorCode, FMonolithJsonUtils::ErrNotImplemented);
	TestFalse(TEXT("No fabricated score result"), Result.Result.IsValid());
	if (!TestTrue(TEXT("Capability error contains data"), Result.ErrorData.IsValid())) return false;
	const auto Data = Result.ErrorData->AsObject();
	TestEqual(TEXT("Capability class"), Data->GetStringField(TEXT("class")), FString(TEXT("not_implemented")));
	TestEqual(TEXT("Missing implementation identified"), Data->GetStringField(TEXT("part")), FString(TEXT("co_op_balance_scoring")));
	TestEqual(TEXT("Capability reason"), Data->GetStringField(TEXT("reason")), FString(TEXT("not_implemented")));
	TestFalse(TEXT("Scoring is not implemented"), Data->GetBoolField(TEXT("implemented")));
	TestFalse(TEXT("World evaluation was not executed"), Data->GetBoolField(TEXT("executed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshCollisionRequiresSaveTest,
	"Monolith.Mesh.Honesty.CollisionRequiresSave", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshCollisionRequiresSaveTest::RunTest(const FString& Parameters)
{
#if !WITH_GEOMETRYSCRIPT
	AddInfo(TEXT("SKIP: GeometryScripting is unavailable; collision handle fixture requires the real mesh implementation"));
	return true;
#else
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Handle = TEXT("MonolithCollision_") + Guid;
	const FString Path = TEXT("/Game/Tests/Monolith/Mesh/SM_Collision_") + Guid;
	const FString ObjectPath = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
	const FString Filename = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	auto HandleParams = MakeShared<FJsonObject>();
	HandleParams->SetStringField(TEXT("handle"), Handle);
	auto CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("handle"), Handle);
	CreateParams->SetStringField(TEXT("source"), TEXT("primitive:box"));
	const auto Created = Registry.ExecuteAction(TEXT("mesh"), TEXT("create_handle"), CreateParams);
	if (!TestTrue(TEXT("Real box handle created"), Created.bSuccess)) return false;
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Owned handle released"), Registry.ExecuteAction(TEXT("mesh"), TEXT("release_handle"), HandleParams).bSuccess);
		if (UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, *ObjectPath))
		{
			IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
			if (Watcher) Watcher->Tick(-1.f);
			IAssetRegistry& Assets = FAssetRegistryModule::GetRegistry();
			Assets.ScanModifiedAssetFiles({Filename});
			Assets.WaitForCompletion();
			TestEqual(TEXT("Owned collision asset deleted"), ObjectTools::ForceDeleteObjects({Mesh}, false), 1);
			if (Watcher) Watcher->Tick(-1.f);
			Assets.WaitForCompletion();
		}
		TestFalse(TEXT("Owned collision asset file removed"), IFileManager::Get().FileExists(*Filename));
	};
	const int32 TrianglesBefore = static_cast<int32>(Created.Result->GetNumberField(TEXT("triangle_count")));
	TestTrue(TEXT("Handle contains actual geometry"), TrianglesBefore > 0);
	HandleParams->SetStringField(TEXT("method"), TEXT("invalid_method"));
	const auto InvalidMethod = Registry.ExecuteAction(TEXT("mesh"), TEXT("generate_collision"), HandleParams);
	TestEqual(TEXT("Invalid method is rejected before persistence precondition"), InvalidMethod.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	auto MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("handle"), Handle + TEXT("_missing"));
	const auto MissingHandle = Registry.ExecuteAction(TEXT("mesh"), TEXT("generate_collision"), MissingParams);
	TestEqual(TEXT("Missing handle is rejected before persistence precondition"), MissingHandle.ErrorCode, FMonolithJsonUtils::ErrNotFound);
	HandleParams->SetStringField(TEXT("method"), TEXT("auto_box"));
	const auto Rejected = Registry.ExecuteAction(TEXT("mesh"), TEXT("generate_collision"), HandleParams);
	HandleParams->RemoveField(TEXT("method"));
	TestFalse(TEXT("Discarded collision is not reported as successful"), Rejected.bSuccess);
	TestEqual(TEXT("Collision requires persistence"), Rejected.ErrorCode, FMonolithJsonUtils::ErrPreconditionFailed);
	TestFalse(TEXT("No generated shape-count result"), Rejected.Result.IsValid());
	if (!TestTrue(TEXT("Precondition has structured data"), Rejected.ErrorData.IsValid())) return false;
	const auto Data = Rejected.ErrorData->AsObject();
	TestEqual(TEXT("Precondition class"), Data->GetStringField(TEXT("class")), FString(TEXT("precondition_failed")));
	TestEqual(TEXT("Actionable persistence step"), Data->GetStringField(TEXT("next_action")), FString(TEXT("mesh.save_handle")));
	TestFalse(TEXT("Collision computation did not execute"), Data->GetBoolField(TEXT("executed")));

	const auto Listed = Registry.ExecuteAction(TEXT("mesh"), TEXT("list_handles"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("Handles remain inspectable"), Listed.bSuccess)) return false;
	bool bFound = false;
	for (const auto& Value : Listed.Result->GetArrayField(TEXT("handles")))
	{
		const auto Row = Value->AsObject();
		if (Row->GetStringField(TEXT("handle")) == Handle)
		{
			bFound = true;
			TestEqual(TEXT("Rejected request leaves geometry unchanged"), static_cast<int32>(Row->GetNumberField(TEXT("triangle_count"))), TrianglesBefore);
			TestEqual(TEXT("Rejected request retains source"), Row->GetStringField(TEXT("source")), FString(TEXT("primitive:box")));
		}
	}
	TestTrue(TEXT("Original handle is retained"), bFound);
	auto SaveParams = MakeShared<FJsonObject>();
	SaveParams->SetStringField(TEXT("handle"), Handle);
	SaveParams->SetStringField(TEXT("target_path"), Path);
	SaveParams->SetStringField(TEXT("collision"), TEXT("box"));
	const auto Saved = Registry.ExecuteAction(TEXT("mesh"), TEXT("save_handle"), SaveParams);
	if (!TestTrue(TEXT("Recommended action saves collision asset"), Saved.bSuccess)) return false;
	UStaticMesh* SavedMesh = FindObject<UStaticMesh>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Saved StaticMesh exists"), SavedMesh)) return false;
	UBodySetup* Body = SavedMesh->GetBodySetup();
	if (!TestNotNull(TEXT("Saved StaticMesh has BodySetup"), Body)) return false;
	TestEqual(TEXT("Recommended box collision is applied"), Body->AggGeom.BoxElems.Num(), 1);
	TArray<uint8> Bytes;
	TestTrue(TEXT("StaticMesh package is written to disk"), FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes.Num() > 0);
	return true;
#endif
}
#endif
