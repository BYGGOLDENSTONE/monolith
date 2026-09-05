#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWritablePackagePathTest,
	"Monolith.Core.WritablePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWritablePackagePathTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	const TArray<FString> OldWritable = Settings->WritablePluginContentRoots;
	const TArray<FString> OldAdditional = Settings->AdditionalContentPaths;
	Settings->WritablePluginContentRoots.Empty();
	Settings->AdditionalContentPaths.Empty();
	const FString Root = TEXT("/MonolithWritableFixture/");
	FPackageName::RegisterMountPoint(Root, FPaths::ProjectSavedDir() / TEXT("WritableFixture/"));
	ON_SCOPE_EXIT
	{
		Settings->WritablePluginContentRoots = OldWritable;
		Settings->AdditionalContentPaths = OldAdditional;
		FPackageName::UnRegisterMountPoint(Root, FPaths::ProjectSavedDir() / TEXT("WritableFixture/"));
	};
	FString Error;
	TestTrue(TEXT("Game test asset writable"), MonolithCore::EnsureWritablePackagePath(TEXT("/Game/Tests/Monolith/Asset"), Error));
	TestTrue(TEXT("Object path accepted"), MonolithCore::EnsureWritablePackagePath(TEXT("/Game/Tests/Monolith/Asset.Asset"), Error));
	TestFalse(TEXT("Engine protected"), MonolithCore::EnsureWritablePackagePath(TEXT("/Engine/EngineMaterials/DefaultMaterial"), Error));
	TestFalse(TEXT("Script protected"), MonolithCore::EnsureWritablePackagePath(TEXT("/Script/Engine"), Error));
	TestFalse(TEXT("Traversal rejected"), MonolithCore::EnsureWritablePackagePath(TEXT("/Game/../Engine/Asset"), Error));
	TestFalse(TEXT("Plugin denied by default"), MonolithCore::EnsureWritablePackagePath(Root + TEXT("Asset"), Error));
	Settings->AdditionalContentPaths.Add(TEXT("/MonolithWritableFixture"));
	TestFalse(TEXT("Indexing alone grants no plugin write permission"), MonolithCore::EnsureWritablePackagePath(Root + TEXT("Asset"), Error));
	Settings->WritablePluginContentRoots.Add(TEXT("/MonolithWritableFixture/"));
	TestTrue(TEXT("Explicit plugin opt-in accepted"), MonolithCore::EnsureWritablePackagePath(Root + TEXT("Asset"), Error));
	Settings->WritablePluginContentRoots = {TEXT("/Engine"), TEXT("/Script"), TEXT("/Game/../Engine")};
	Settings->AdditionalContentPaths = {TEXT("/Engine"), TEXT("/Script")};
	TestFalse(TEXT("Settings cannot grant engine permission"), MonolithCore::EnsureWritablePackagePath(TEXT("/Engine/EngineMaterials/DefaultMaterial"), Error));
	const FMonolithActionResult Result = MonolithCore::WritablePathError(TEXT("/Engine/EngineMaterials/DefaultMaterial"), Error);
	TestEqual(TEXT("Invalid params code"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	const TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
	TestEqual(TEXT("Reason"), Data->GetStringField(TEXT("reason")), FString(TEXT("path_not_writable")));
	TestFalse(TEXT("Rejected before execution"), Data->GetBoolField(TEXT("executed")));
	TestTrue(TEXT("Accepted roots provided"), Data->HasField(TEXT("accepted_roots")));
	return true;
}
#endif
