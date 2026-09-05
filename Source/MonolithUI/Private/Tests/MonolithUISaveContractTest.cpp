#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "MonolithToolRegistry.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "Engine/DataTable.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUISaveContractTest, "Monolith.UI.SaveContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISaveContractTest::RunTest(const FString& Parameters)
{
#if !WITH_COMMONUI
	AddInfo(TEXT("Self-skip: CommonUI is unavailable"));
	return true;
#else
	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/SaveContract_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Filename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath))
		{
			IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
			if (Watcher) Watcher->Tick(-1.f);
			IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
			Registry.ScanModifiedAssetFiles({ Filename });
			Registry.WaitForCompletion();
			TestEqual(TEXT("GUID-owned fixture deleted"), ObjectTools::ForceDeleteObjects({ Asset }, false), 1);
			if (Watcher) Watcher->Tick(-1.f);
			Registry.WaitForCompletion();
		}
		TestFalse(TEXT("No fixture file remains"), IFileManager::Get().FileExists(*Filename));
	};
	auto Call = [](const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), Action, Params);
	};
	TSharedPtr<FJsonObject> Create = MakeShared<FJsonObject>();
	Create->SetStringField(TEXT("package_path"), FPackageName::GetLongPackagePath(AssetPath));
	Create->SetStringField(TEXT("asset_name"), FPackageName::GetShortName(AssetPath));
	const FMonolithActionResult Created = Call(TEXT("create_input_action_data_table"), Create);
	if (!TestTrue(TEXT("Fixture asset created: ") + Created.ErrorMessage, Created.bSuccess)) return false;
	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!TestNotNull(TEXT("Created asset loads"), Asset)) return false;
	TArray<uint8> Before, WithoutSave, AfterSave;
	if (!TestTrue(TEXT("Fixture exists on disk"), FFileHelper::LoadFileToArray(Before, *Filename))) return false;
	TestFalse(TEXT("New fixture package is clean"), Asset->GetPackage()->IsDirty());
	TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
	Mutation->SetStringField(TEXT("table_path"), AssetPath);
	Mutation->SetStringField(TEXT("row_name"), TEXT("First"));
	Mutation->SetStringField(TEXT("display_name"), TEXT("First action"));
	const FMonolithActionResult Unsaved = Call(TEXT("add_input_action_row"), Mutation);
	if (!TestTrue(TEXT("Mutation without save succeeds: ") + Unsaved.ErrorMessage, Unsaved.bSuccess)) return false;
	TestTrue(TEXT("Mutation without save leaves package dirty"), Asset->GetPackage()->IsDirty());
	TestTrue(TEXT("Existing file remains readable"), FFileHelper::LoadFileToArray(WithoutSave, *Filename));
	TestTrue(TEXT("Omitted save leaves disk bytes unchanged"), Before == WithoutSave);
	Mutation->SetStringField(TEXT("row_name"), TEXT("Second"));
	Mutation->SetStringField(TEXT("display_name"), TEXT("Second action"));
	Mutation->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult Saved = Call(TEXT("add_input_action_row"), Mutation);
	if (!TestTrue(TEXT("Mutation with save succeeds: ") + Saved.ErrorMessage, Saved.bSuccess)) return false;
	TestFalse(TEXT("Explicit save leaves package clean"), Asset->GetPackage()->IsDirty());
	TestTrue(TEXT("Saved file remains readable"), FFileHelper::LoadFileToArray(AfterSave, *Filename));
	TestTrue(TEXT("Explicit save changes disk bytes"), Before != AfterSave);
	UDataTable* Table = Cast<UDataTable>(Asset);
	if (!TestNotNull(TEXT("Fixture is a DataTable"), Table)) return false;
	TestTrue(TEXT("Unsaved first row retained by explicit save"), Table->GetRowNames().Contains(FName(TEXT("First"))));
	TestTrue(TEXT("Saved second row reads back"), Table->GetRowNames().Contains(FName(TEXT("Second"))));
	return true;
#endif
}
#endif
