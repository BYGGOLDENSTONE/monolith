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
#include "UObject/Package.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeWaveParam.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioRoundTripTest, "Monolith.Audio.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioRoundTripTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/Audio/RoundTrip_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
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
			TestEqual(TEXT("GUID-owned SoundCue deleted"), ObjectTools::ForceDeleteObjects({ Owned }, false), 1);
			if (Watcher) Watcher->Tick(-1.f);
			Registry.WaitForCompletion();
		}
		TestFalse(TEXT("SoundCue fixture file removed"), IFileManager::Get().FileExists(*Filename));
	};
	auto Call = [](const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), Action, Params);
	};
	auto AssetParams = MakeShared<FJsonObject>();
	AssetParams->SetStringField(TEXT("asset_path"), AssetPath);
	const auto Created = Call(TEXT("create_sound_cue"), AssetParams);
	if (!TestTrue(TEXT("SoundCue creation succeeds: ") + Created.ErrorMessage, Created.bSuccess)) return false;
	USoundCue* Cue = LoadObject<USoundCue>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("Created SoundCue loads"), Cue)) return false;
	TestFalse(TEXT("Created SoundCue package is clean"), Cue->GetPackage()->IsDirty());
	TArray<uint8> Original, UnsavedBytes, SavedBytes;
	if (!TestTrue(TEXT("Created SoundCue has disk bytes"), FFileHelper::LoadFileToArray(Original, *Filename))) return false;

	auto Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), AssetPath);
	Add->SetStringField(TEXT("node_type"), TEXT("WaveParam"));
	const auto Added = Call(TEXT("add_sound_cue_node"), Add);
	if (!TestTrue(TEXT("Omitted-save SoundCue node creation succeeds: ") + Added.ErrorMessage, Added.bSuccess && Added.Result.IsValid())) return false;
	const FString NodeId = Added.Result->GetStringField(TEXT("node_id"));
	auto NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("asset_path"), AssetPath);
	NodeParams->SetStringField(TEXT("node_id"), NodeId);
	const auto Root = Call(TEXT("set_sound_cue_first_node"), NodeParams);
	if (!TestTrue(TEXT("SoundCue root is authored through its action"), Root.bSuccess)) return false;
	auto Mutation = MakeShared<FJsonObject>();
	Mutation->SetStringField(TEXT("asset_path"), AssetPath);
	Mutation->SetStringField(TEXT("node_id"), NodeId);
	Mutation->SetStringField(TEXT("property_name"), TEXT("WaveParameterName"));
	Mutation->SetStringField(TEXT("value"), TEXT("RoundTripInput"));
	const auto Unsaved = Call(TEXT("set_sound_cue_node_property"), Mutation);
	if (!TestTrue(TEXT("Omitted-save SoundCue property mutation succeeds: ") + Unsaved.ErrorMessage, Unsaved.bSuccess)) return false;
	TestTrue(TEXT("Omitted-save SoundCue mutations leave package dirty"), Cue->GetPackage()->IsDirty());
	TestTrue(TEXT("Unsaved SoundCue disk bytes readable"), FFileHelper::LoadFileToArray(UnsavedBytes, *Filename));
	TestTrue(TEXT("Omitted save preserves SoundCue disk bytes"), Original == UnsavedBytes);
	auto ReadGraph = [&](const FString& ExpectedName)
	{
		const auto Read = Call(TEXT("get_sound_cue_graph"), AssetParams);
		if (!TestTrue(TEXT("SoundCue graph action readback succeeds: ") + Read.ErrorMessage, Read.bSuccess && Read.Result.IsValid())) return false;
		const auto& Nodes = Read.Result->GetArrayField(TEXT("nodes"));
		if (!TestEqual(TEXT("RoundTrip graph has exactly its authored node"), Nodes.Num(), 1)) return false;
		const auto Node = Nodes[0]->AsObject();
		TestEqual(TEXT("RoundTrip graph node type"), Node->GetStringField(TEXT("type")), FString(TEXT("SoundNodeWaveParam")));
		TestEqual(TEXT("RoundTrip graph root is its authored node"), Read.Result->GetStringField(TEXT("first_node")), Node->GetStringField(TEXT("node_id")));
		return TestEqual(TEXT("Wave parameter property reads back"), Node->GetObjectField(TEXT("properties"))->GetStringField(TEXT("WaveParameterName")), ExpectedName);
	};
	if (!ReadGraph(TEXT("RoundTripInput"))) return false;
	// Real AudioEditor graph-to-runtime compilation, without playback or a fake
	// success-only compiler substitute. WaveParam resolves a wave at runtime.
	Cue->FirstNode = nullptr;
	Cue->CompileSoundNodesFromGraphNodes();
	Cue->CacheAggregateValues();
	USoundNodeWaveParam* CompiledRoot = Cast<USoundNodeWaveParam>(Cue->FirstNode);
	if (!TestNotNull(TEXT("Real SoundCue compiler produces the authored root"), CompiledRoot)) return false;
	TestEqual(TEXT("Compiled SoundCue root retains wave parameter"), CompiledRoot->WaveParameterName, FName(TEXT("RoundTripInput")));
	auto Validate = [&]()
	{
		const auto Result = Call(TEXT("validate_sound_cue"), AssetParams);
		if (!TestTrue(TEXT("SoundCue validation action succeeds"), Result.bSuccess && Result.Result.IsValid())) return false;
		TestTrue(TEXT("Compiled SoundCue validates"), Result.Result->GetBoolField(TEXT("valid")));
		for (const auto& Issue : Result.Result->GetArrayField(TEXT("issues")))
		{
			TestTrue(TEXT("SoundCue validator reports no error issues"), Issue->AsObject()->GetStringField(TEXT("severity")) != TEXT("error"));
		}
		return true;
	};
	if (!Validate()) return false;
	TestTrue(TEXT("Explicit SoundCue compile leaves package dirty"), Cue->GetPackage()->IsDirty());
	TestTrue(TEXT("SoundCue disk bytes readable after compile"), FFileHelper::LoadFileToArray(UnsavedBytes, *Filename));
	TestTrue(TEXT("Explicit SoundCue compile does not save"), Original == UnsavedBytes);

	Mutation->SetStringField(TEXT("value"), TEXT("PersistedRoundTrip"));
	Mutation->SetBoolField(TEXT("save"), true);
	const auto Saved = Call(TEXT("set_sound_cue_node_property"), Mutation);
	if (!TestTrue(TEXT("Explicit-save SoundCue mutation succeeds: ") + Saved.ErrorMessage, Saved.bSuccess)) return false;
	TestFalse(TEXT("Explicit save cleans SoundCue package"), Cue->GetPackage()->IsDirty());
	TestTrue(TEXT("Saved SoundCue disk bytes readable"), FFileHelper::LoadFileToArray(SavedBytes, *Filename));
	TestTrue(TEXT("Explicit save changes SoundCue disk bytes"), Original != SavedBytes);
	if (!ReadGraph(TEXT("PersistedRoundTrip"))) return false;
	UPackage* Package = Cue->GetPackage();
	const TWeakObjectPtr<USoundCue> BeforeUnload = Cue;
	Cue = nullptr;
	CompiledRoot = nullptr;
	FText UnloadError;
	const bool bUnloaded = UPackageTools::UnloadPackages({ Package }, UnloadError);
	if (!TestTrue(TEXT("Saved SoundCue package unloads: ") + UnloadError.ToString(), bUnloaded)) return false;
	if (!TestFalse(TEXT("SoundCue readback cannot reuse original object"), BeforeUnload.IsValid())) return false;
	Cue = LoadObject<USoundCue>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("SoundCue reloads from saved package"), Cue)) return false;
	if (!ReadGraph(TEXT("PersistedRoundTrip")) || !Validate()) return false;
	TestFalse(TEXT("Reloaded SoundCue is clean"), Cue->GetPackage()->IsDirty());

	auto Typo = MakeShared<FJsonObject>();
	Typo->SetStringField(TEXT("asset_path"), AssetPath.LeftChop(1) + (AssetPath.EndsWith(TEXT("a")) ? TEXT("b") : TEXT("a")));
	const auto Missing = Call(TEXT("get_sound_cue_graph"), Typo);
	TestFalse(TEXT("Misspelled SoundCue is rejected"), Missing.bSuccess);
	TestEqual(TEXT("Missing SoundCue is NotFound"), Missing.ErrorCode, FMonolithJsonUtils::ErrNotFound);
	if (!TestTrue(TEXT("Missing SoundCue has structured data"), Missing.ErrorData.IsValid())) return false;
	const auto Data = Missing.ErrorData->AsObject();
	if (!TestTrue(TEXT("Missing SoundCue data is an object"), Data.IsValid())) return false;
	TestEqual(TEXT("SoundCue error class"), Data->GetStringField(TEXT("class")), FString(TEXT("not_found")));
	TestFalse(TEXT("Missing SoundCue was not executed"), Data->GetBoolField(TEXT("executed")));
	bool bSuggestedFixture = false;
	for (const auto& Suggestion : Data->GetArrayField(TEXT("suggestions")))
	{
		bSuggestedFixture |= Suggestion->AsObject()->GetStringField(TEXT("Sound Cue")) == AssetPath;
	}
	TestTrue(TEXT("SoundCue typo suggests the real GUID fixture"), bSuggestedFixture);
	return true;
}
#endif
