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
#include "MonolithJsonUtils.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Engine/Blueprint.h"
#include "GameplayEffect.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGASRoundTripTest, "Monolith.GAS.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASRoundTripTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/GAS/RoundTrip_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Filename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
	ON_SCOPE_EXIT
	{
		UObject* Owned = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Owned && IFileManager::Get().FileExists(*Filename)) Owned = LoadObject<UObject>(nullptr, *ObjectPath);
		if (Owned)
		{
			IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
			if (Watcher) Watcher->Tick(-1.f);
			IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
			Registry.ScanModifiedAssetFiles({ Filename });
			Registry.WaitForCompletion();
			TestEqual(TEXT("GUID-owned GameplayEffect deleted"), ObjectTools::ForceDeleteObjects({ Owned }, false), 1);
			if (Watcher) Watcher->Tick(-1.f);
			Registry.WaitForCompletion();
		}
		TestFalse(TEXT("GameplayEffect fixture file removed"), IFileManager::Get().FileExists(*Filename));
	};
	auto Call = [](const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), Action, Params);
	};
	auto ReadPeriod = [&](double Expected)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		const auto Read = Call(TEXT("get_gameplay_effect"), Params);
		if (!TestTrue(TEXT("GameplayEffect action readback succeeds: ") + Read.ErrorMessage, Read.bSuccess && Read.Result.IsValid())) return false;
		return TestEqual(TEXT("GameplayEffect period readback"), Read.Result->GetNumberField(TEXT("period")), Expected);
	};
	auto Create = MakeShared<FJsonObject>();
	Create->SetStringField(TEXT("save_path"), AssetPath);
	Create->SetStringField(TEXT("duration_policy"), TEXT("infinite"));
	const auto Created = Call(TEXT("create_gameplay_effect"), Create);
	if (!TestTrue(TEXT("GameplayEffect creation succeeds: ") + Created.ErrorMessage, Created.bSuccess && Created.Result.IsValid())) return false;
	if (!TestTrue(TEXT("Creator confirms disk save"), Created.Result->GetBoolField(TEXT("saved")))) return false;
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Created GameplayEffect Blueprint loads"), Blueprint)) return false;
	if (!TestNotNull(TEXT("Created Blueprint has generated class"), Blueprint->GeneratedClass.Get())) return false;
	TestFalse(TEXT("Created GameplayEffect package is clean"), Blueprint->GetPackage()->IsDirty());
	TArray<uint8> Original, UnsavedBytes, SavedBytes;
	if (!TestTrue(TEXT("Created GameplayEffect has disk bytes"), FFileHelper::LoadFileToArray(Original, *Filename))) return false;

	auto Mutation = MakeShared<FJsonObject>();
	Mutation->SetStringField(TEXT("asset_path"), AssetPath);
	Mutation->SetNumberField(TEXT("period"), 1.25);
	const auto Unsaved = Call(TEXT("set_period"), Mutation);
	if (!TestTrue(TEXT("Omitted-save GAS mutation succeeds: ") + Unsaved.ErrorMessage, Unsaved.bSuccess)) return false;
	TestTrue(TEXT("Omitted-save GAS mutation dirties package"), Blueprint->GetPackage()->IsDirty());
	if (!ReadPeriod(1.25)) return false;
	TestTrue(TEXT("Unsaved GameplayEffect disk bytes readable"), FFileHelper::LoadFileToArray(UnsavedBytes, *Filename));
	TestTrue(TEXT("Omitted save preserves GameplayEffect disk bytes"), Original == UnsavedBytes);
	{
		FCompilerResultsLog CompilerResults;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &CompilerResults);
		TestEqual(TEXT("Real GameplayEffect Blueprint compiler has no errors"), CompilerResults.NumErrors, 0);
		TestTrue(TEXT("Compiled GameplayEffect Blueprint is valid"), Blueprint->Status != BS_Error && Blueprint->GeneratedClass != nullptr);
	}
	if (!Blueprint->GeneratedClass) return false;
	const UGameplayEffect* Effect = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
	if (!TestNotNull(TEXT("Compiled GameplayEffect CDO exists"), Effect)) return false;
	TestEqual(TEXT("Compiler retained the unsaved period"), Effect->Period.GetValueAtLevel(1.f), 1.25f);
	TestTrue(TEXT("Explicit compile keeps package dirty"), Blueprint->GetPackage()->IsDirty());
	TestTrue(TEXT("Disk bytes readable after compile"), FFileHelper::LoadFileToArray(UnsavedBytes, *Filename));
	TestTrue(TEXT("Explicit compile does not save GameplayEffect"), Original == UnsavedBytes);

	Mutation->SetNumberField(TEXT("period"), 2.5);
	Mutation->SetBoolField(TEXT("save"), true);
	const auto Saved = Call(TEXT("set_period"), Mutation);
	if (!TestTrue(TEXT("Explicit-save GAS mutation succeeds: ") + Saved.ErrorMessage, Saved.bSuccess)) return false;
	TestFalse(TEXT("Explicit save cleans GameplayEffect package"), Blueprint->GetPackage()->IsDirty());
	TestTrue(TEXT("Saved GameplayEffect disk bytes readable"), FFileHelper::LoadFileToArray(SavedBytes, *Filename));
	TestTrue(TEXT("Explicit save changes GameplayEffect disk bytes"), Original != SavedBytes);
	if (!ReadPeriod(2.5)) return false;
	UPackage* Package = Blueprint->GetPackage();
	const TWeakObjectPtr<UBlueprint> BeforeUnload = Blueprint;
	Blueprint = nullptr;
	Effect = nullptr;
	FText UnloadError;
	const bool bUnloaded = UPackageTools::UnloadPackages({ Package }, UnloadError);
	if (!TestTrue(TEXT("Saved GameplayEffect package unloads: ") + UnloadError.ToString(), bUnloaded)) return false;
	if (!TestFalse(TEXT("GameplayEffect readback cannot reuse original object"), BeforeUnload.IsValid())) return false;
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("GameplayEffect reloads from saved package"), Blueprint)) return false;
	if (!ReadPeriod(2.5)) return false;
	TestFalse(TEXT("Reloaded GameplayEffect is clean"), Blueprint->GetPackage()->IsDirty());

	auto Typo = MakeShared<FJsonObject>();
	Typo->SetStringField(TEXT("asset_path"), AssetPath.LeftChop(1) + (AssetPath.EndsWith(TEXT("a")) ? TEXT("b") : TEXT("a")));
	const auto Missing = Call(TEXT("get_gameplay_effect"), Typo);
	TestFalse(TEXT("Misspelled GameplayEffect is rejected"), Missing.bSuccess);
	TestEqual(TEXT("Missing GameplayEffect is NotFound"), Missing.ErrorCode, FMonolithJsonUtils::ErrNotFound);
	if (!TestTrue(TEXT("Missing GameplayEffect has structured data"), Missing.ErrorData.IsValid())) return false;
	const auto Data = Missing.ErrorData->AsObject();
	if (!TestTrue(TEXT("Missing GameplayEffect data is an object"), Data.IsValid())) return false;
	TestEqual(TEXT("GameplayEffect error class"), Data->GetStringField(TEXT("class")), FString(TEXT("not_found")));
	TestFalse(TEXT("Missing GameplayEffect was not executed"), Data->GetBoolField(TEXT("executed")));
	bool bSuggestedFixture = false;
	for (const auto& Suggestion : Data->GetArrayField(TEXT("suggestions")))
	{
		bSuggestedFixture |= Suggestion->AsObject()->GetStringField(TEXT("GameplayEffect Blueprint")) == AssetPath;
	}
	TestTrue(TEXT("GameplayEffect typo suggests the real GUID fixture"), bSuggestedFixture);
	return true;
}
#endif
