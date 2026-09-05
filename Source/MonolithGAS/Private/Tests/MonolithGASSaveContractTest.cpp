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
#include "Engine/Blueprint.h"
#include "GameplayEffect.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGASSaveContractTest, "Monolith.GAS.SaveContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASSaveContractTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/GAS/SaveContract_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
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
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), Action, Params);
	};
	TSharedPtr<FJsonObject> Create = MakeShared<FJsonObject>();
	Create->SetStringField(TEXT("save_path"), AssetPath);
	Create->SetStringField(TEXT("duration_policy"), TEXT("infinite"));
	const FMonolithActionResult Created = Call(TEXT("create_gameplay_effect"), Create);
	if (!TestTrue(TEXT("Fixture asset created: ") + Created.ErrorMessage, Created.bSuccess)) return false;
	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!TestNotNull(TEXT("Created asset loads"), Asset)) return false;
	TArray<uint8> Before, WithoutSave, AfterSave;
	if (!TestTrue(TEXT("Fixture exists on disk"), FFileHelper::LoadFileToArray(Before, *Filename))) return false;
	TestFalse(TEXT("New fixture package is clean"), Asset->GetPackage()->IsDirty());
	TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
	Mutation->SetStringField(TEXT("asset_path"), AssetPath);
	Mutation->SetNumberField(TEXT("period"), 1.25);
	const FMonolithActionResult Unsaved = Call(TEXT("set_period"), Mutation);
	if (!TestTrue(TEXT("Mutation without save succeeds: ") + Unsaved.ErrorMessage, Unsaved.bSuccess)) return false;
	TestTrue(TEXT("Mutation without save leaves package dirty"), Asset->GetPackage()->IsDirty());
	TestTrue(TEXT("Existing file remains readable"), FFileHelper::LoadFileToArray(WithoutSave, *Filename));
	TestTrue(TEXT("Omitted save leaves disk bytes unchanged"), Before == WithoutSave);
	Mutation->SetNumberField(TEXT("period"), 2.5);
	Mutation->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult Saved = Call(TEXT("set_period"), Mutation);
	if (!TestTrue(TEXT("Mutation with save succeeds: ") + Saved.ErrorMessage, Saved.bSuccess)) return false;
	TestFalse(TEXT("Explicit save leaves package clean"), Asset->GetPackage()->IsDirty());
	TestTrue(TEXT("Saved file remains readable"), FFileHelper::LoadFileToArray(AfterSave, *Filename));
	TestTrue(TEXT("Explicit save changes disk bytes"), Before != AfterSave);
	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult Read = Call(TEXT("get_gameplay_effect"), ReadParams);
	if (!TestTrue(TEXT("GameplayEffect readback succeeds"), Read.bSuccess)) return false;
	TestEqual(TEXT("Saved period reads back"), Read.Result->GetNumberField(TEXT("period")), 2.5);
	return true;
}
#endif
