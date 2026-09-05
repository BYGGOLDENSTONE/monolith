#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "MonolithToolRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MonolithSkeletonSaveContractTest
{
	struct FFixture
	{
		FAutomationTestBase& Test;
		TArray<USkeleton*> Skeletons;
		TArray<FString> Filenames;

		explicit FFixture(FAutomationTestBase& InTest) : Test(InTest) {}

		USkeleton* Create()
		{
			const FString Name = TEXT("SK_Save_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Path = TEXT("/Game/Tests/Monolith/Animation/") + Name;
			UPackage* Package = CreatePackage(*Path);
			USkeleton* Skeleton = NewObject<USkeleton>(Package, *Name, RF_Public | RF_Standalone);
			if (!Skeleton) return nullptr;
			Skeletons.Add(Skeleton);
			Filenames.Add(FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension()));
			{
				FReferenceSkeletonModifier Modifier(Skeleton);
				Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
			}
			FAssetRegistryModule::AssetCreated(Skeleton);
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			if (!Test.TestTrue(TEXT("Save initial skeleton fixture"), UPackage::SavePackage(Package, Skeleton, *Filenames.Last(), Args))) return nullptr;
			return Skeleton;
		}

		~FFixture()
		{
			IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
			if (Watcher) Watcher->Tick(-1.f);
			IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
			Registry.ScanModifiedAssetFiles(Filenames);
			Registry.WaitForCompletion();
			TArray<UObject*> Assets;
			for (USkeleton* Skeleton : Skeletons) Assets.Add(Skeleton);
			if (!Assets.IsEmpty()) Test.TestEqual(TEXT("Delete GUID-owned skeleton fixtures"), ObjectTools::ForceDeleteObjects(Assets, false), Assets.Num());
			if (Watcher) Watcher->Tick(-1.f);
			Registry.WaitForCompletion();
			for (const FString& Filename : Filenames) Test.TestFalse(TEXT("Skeleton fixture file removed"), IFileManager::Get().FileExists(*Filename));
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSkeletonSaveContractTest,
	"Monolith.Animation.SaveContract.CompatibleSkeletons",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSkeletonSaveContractTest::RunTest(const FString& /*Parameters*/)
{
	MonolithSkeletonSaveContractTest::FFixture Fixture(*this);
	USkeleton* Target = Fixture.Create();
	USkeleton* Compatible = Fixture.Create();
	if (!Target || !Compatible) return false;
	const FString Filename = Fixture.Filenames[0];
	auto ReadBytes = [&]()
	{
		TArray<uint8> Bytes;
		TestTrue(TEXT("Read serialized skeleton"), FFileHelper::LoadFileToArray(Bytes, *Filename));
		return Bytes;
	};
	auto Invoke = [&](const TCHAR* Action, bool bExplicitSave)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Target->GetPathName());
		Params->SetStringField(TEXT("compatible_with"), Compatible->GetPathName());
		if (bExplicitSave) Params->SetBoolField(TEXT("save"), true);
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), Action, Params);
		TestTrue(FString::Printf(TEXT("%s succeeds: %s"), Action, *Result.ErrorMessage), Result.bSuccess);
	};

	const TArray<uint8> EmptyBaseline = ReadBytes();
	Invoke(TEXT("add_compatible_skeleton"), false);
	TestEqual(TEXT("Default add changes compatible skeletons"), Target->GetCompatibleSkeletons().Num(), 1);
	TestTrue(TEXT("Default add leaves package dirty"), Target->GetOutermost()->IsDirty());
	TestTrue(TEXT("Default add leaves disk bytes unchanged"), ReadBytes() == EmptyBaseline);
	Invoke(TEXT("remove_compatible_skeleton"), false);
	TestEqual(TEXT("Default remove changes compatible skeletons"), Target->GetCompatibleSkeletons().Num(), 0);
	TestTrue(TEXT("Default remove leaves package dirty"), Target->GetOutermost()->IsDirty());
	TestTrue(TEXT("Default remove leaves disk bytes unchanged"), ReadBytes() == EmptyBaseline);

	Invoke(TEXT("add_compatible_skeleton"), true);
	const TArray<uint8> AddedBaseline = ReadBytes();
	TestFalse(TEXT("Explicit add updates disk bytes"), AddedBaseline == EmptyBaseline);
	TestFalse(TEXT("Explicit add clears package dirty flag"), Target->GetOutermost()->IsDirty());
	Invoke(TEXT("remove_compatible_skeleton"), false);
	TestTrue(TEXT("Unsaved removal preserves persisted compatibility"), ReadBytes() == AddedBaseline);
	Invoke(TEXT("add_compatible_skeleton"), false);
	Invoke(TEXT("remove_compatible_skeleton"), true);
	TestFalse(TEXT("Explicit remove updates disk bytes"), ReadBytes() == AddedBaseline);
	TestFalse(TEXT("Explicit remove clears package dirty flag"), Target->GetOutermost()->IsDirty());
	return true;
}
#endif
