#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithToolRegistry.h"
#include "AI/NavigationSystemBase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NavigationSystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNavigationSaveContractTest,
	"Monolith.AI.SaveContract.Navigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNavigationSaveContractTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor exists"), GEditor)) return false;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			AddInfo(TEXT("Skipping: active PIE takes precedence over the disposable editor world"));
			return true;
		}
	}

	const FString Name = TEXT("Map_Save_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = TEXT("/Game/Tests/Monolith/AI/") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetMapPackageExtension());
	const FString WrongExtensionFilename = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	UPackage* Package = CreatePackage(*Path);
	UWorld::InitializationValues Values;
	Values.AllowAudioPlayback(false).ShouldSimulatePhysics(false).CreateNavigation(true).CreateAISystem(true).CreateFXSystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, FName(*Name), Package, true, ERHIFeatureLevel::Num, &Values);
	if (!TestNotNull(TEXT("Disposable world created"), World)) return false;
	World->SetFlags(RF_Public | RF_Standalone);
	FWorldContext& EditorContext = GEditor->GetEditorWorldContext();
	UWorld* OriginalWorld = EditorContext.World();
	EditorContext.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		EditorContext.SetCurrentWorld(OriginalWorld);
		IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
		if (Watcher) Watcher->Tick(-1.f);
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		if (IFileManager::Get().FileExists(*Filename)) Registry.ScanModifiedAssetFiles({ Filename });
		Registry.WaitForCompletion();
		FAssetRegistryModule::AssetDeleted(World);
		World->DestroyWorld(false);
		Package->SetDirtyFlag(false);
		World->MarkAsGarbage();
		IFileManager::Get().Delete(*Filename);
		IFileManager::Get().Delete(*WrongExtensionFilename);
		if (Watcher) Watcher->Tick(-1.f);
		Registry.WaitForCompletion();
		TestFalse(TEXT("Disposable map file removed"), IFileManager::Get().FileExists(*Filename));
	};

	FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
	if (!TestNotNull(TEXT("Real navigation system available"), FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))) return false;
	FAssetRegistryModule::AssetCreated(World);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("Save initial map fixture"), UPackage::SavePackage(Package, World, *Filename, Args))) return false;
	auto ReadBytes = [&]()
	{
		TArray<uint8> Bytes;
		TestTrue(TEXT("Read serialized map"), FFileHelper::LoadFileToArray(Bytes, *Filename));
		return Bytes;
	};
	auto DirtyFixture = [&]()
	{
		// Keep a deterministic serialized change pending while the real nav rebuild runs.
		World->GetWorldSettings()->WorldToMeters += 1.0f;
		Package->MarkPackageDirty();
	};
	auto Invoke = [&](const TCHAR* SaveKey)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("timeout_seconds"), 1.0);
		if (SaveKey) Params->SetBoolField(SaveKey, true);
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ai"), TEXT("rebuild_navigation"), Params);
		TestTrue(FString::Printf(TEXT("Navigation rebuild succeeds: %s"), *Result.ErrorMessage), Result.bSuccess);
		if (Result.Result.IsValid()) TestTrue(TEXT("Navigation generation completes before save"), Result.Result->GetBoolField(TEXT("generation_complete")));
		return Result;
	};

	const TArray<uint8> Baseline = ReadBytes();
	DirtyFixture();
	Invoke(nullptr);
	TestTrue(TEXT("Default rebuild leaves package dirty"), Package->IsDirty());
	TestTrue(TEXT("Default rebuild preserves disk bytes"), ReadBytes() == Baseline);
	const FMonolithActionResult Saved = Invoke(TEXT("save"));
	if (Saved.Result.IsValid() && Saved.Result->HasTypedField<EJson::Object>(TEXT("save_status")))
	{
		TestEqual(TEXT("Explicit save has no failed packages"), Saved.Result->GetObjectField(TEXT("save_status"))->GetIntegerField(TEXT("failed_count")), 0);
	}
	const TArray<uint8> SavedBytes = ReadBytes();
	TestFalse(TEXT("Explicit save updates the existing map file"), SavedBytes == Baseline);
	TestFalse(TEXT("Explicit save clears package dirty flag"), Package->IsDirty());
	TestFalse(TEXT("Map save creates no asset-extension duplicate"), IFileManager::Get().FileExists(*WrongExtensionFilename));
	DirtyFixture();
	Invoke(TEXT("save_after"));
	TestFalse(TEXT("Legacy save_after alias still updates map bytes"), ReadBytes() == SavedBytes);
	TestFalse(TEXT("Legacy save_after alias clears package dirty flag"), Package->IsDirty());
	return true;
}
#endif
