#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "Materials/Material.h"
#include "MonolithToolRegistry.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithMaterialSaveTests
{
	struct FScopedMaterial
	{
		FAutomationTestBase& Test;
		const FString Path = TEXT("/Game/Tests/Monolith/Material/M_Save_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);

		explicit FScopedMaterial(FAutomationTestBase& InTest) : Test(InTest) {}

		~FScopedMaterial()
		{
			if (UMaterial* Material = FindObject<UMaterial>(nullptr, *ObjectPath()))
			{
				// Deliver pending additions before deletion, as in the UI asset fixtures.
				IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
				if (Watcher) Watcher->Tick(-1.f);
				IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
				Registry.ScanModifiedAssetFiles({ Filename() });
				Registry.WaitForCompletion();
				Test.TestEqual(TEXT("GUID material fixture deleted"), ObjectTools::ForceDeleteObjects({ Material }, false), 1);
				if (Watcher) Watcher->Tick(-1.f);
				Registry.WaitForCompletion();
			}
			Test.TestFalse(TEXT("No fixture file remains"), IFileManager::Get().FileExists(*Filename()));
		}

		FString ObjectPath() const { return Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path); }
		FString Filename() const { return FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension()); }
		UMaterial* Get() const { return FindObject<UMaterial>(nullptr, *ObjectPath()); }

		bool Create()
		{
			auto Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), Path);
			const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("create_material"), Params);
			return Test.TestTrue(TEXT("Create fixture material"), Result.bSuccess)
				&& Test.TestNotNull(TEXT("Fixture material loaded"), Get())
				&& Test.TestTrue(TEXT("New material saved to disk"), IFileManager::Get().FileExists(*Filename()));
		}

		bool Snapshot(TArray<uint8>& Bytes) const
		{
			return Test.TestTrue(TEXT("Read actual material package bytes"), FFileHelper::LoadFileToArray(Bytes, *Filename()))
				&& Test.TestTrue(TEXT("Material package is nonempty"), Bytes.Num() > 0);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialSaveContractTest,
	"Monolith.Material.SaveContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialSaveContractTest::RunTest(const FString& Parameters)
{
	using namespace MonolithMaterialSaveTests;
	FScopedMaterial First(*this);
	FScopedMaterial Second(*this);
	if (!First.Create() || !Second.Create()) return false;

	TArray<uint8> BeforeSingle;
	if (!First.Snapshot(BeforeSingle)) return false;
	for (int32 Step = 0; Step < 3; ++Step)
	{
		const bool bSave = Step == 2;
		const bool bValue = Step != 1;
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), First.Path);
		Params->SetBoolField(TEXT("two_sided"), bValue);
		if (Step > 0) Params->SetBoolField(TEXT("save"), bSave);
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("set_material_property"), Params);
		if (!TestTrue(TEXT("Single material mutation succeeds"), Result.bSuccess)) return false;
		TestEqual(TEXT("Single mutation reports actual save"), Result.Result->GetBoolField(TEXT("saved")), bSave);
		TestEqual(TEXT("Single mutation changes in-memory property"), static_cast<bool>(First.Get()->TwoSided), bValue);
		TestEqual(TEXT("Single package stays dirty unless saved"), First.Get()->GetPackage()->IsDirty(), !bSave);
		TArray<uint8> After;
		if (!First.Snapshot(After)) return false;
		TestEqual(TEXT("Single package disk bytes change only with save true"), After != BeforeSingle, bSave);
	}

	TArray<uint8> BeforeBatchFirst, BeforeBatchSecond;
	if (!First.Snapshot(BeforeBatchFirst) || !Second.Snapshot(BeforeBatchSecond)) return false;
	for (int32 Step = 0; Step < 3; ++Step)
	{
		const bool bSave = Step == 2;
		const bool bValue = Step != 1;
		auto Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("asset_paths"), { MakeShared<FJsonValueString>(First.Path), MakeShared<FJsonValueString>(Second.Path) });
		Params->SetBoolField(TEXT("fully_rough"), bValue);
		if (Step > 0) Params->SetBoolField(TEXT("save"), bSave);
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("batch_set_material_property"), Params);
		if (!TestTrue(TEXT("Batch material mutation succeeds"), Result.bSuccess)) return false;
		const TArray<TSharedPtr<FJsonValue>>& Rows = Result.Result->GetArrayField(TEXT("results"));
		TestEqual(TEXT("Both materials returned"), Rows.Num(), 2);
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			TestTrue(TEXT("Batch material row succeeds"), Row->AsObject()->GetBoolField(TEXT("success")));
			TestEqual(TEXT("Batch material row reports actual save"), Row->AsObject()->GetBoolField(TEXT("saved")), bSave);
		}
		for (const FScopedMaterial* Fixture : { &First, &Second })
		{
			TestEqual(TEXT("Batch changes in-memory property"), static_cast<bool>(Fixture->Get()->bFullyRough), bValue);
			TestEqual(TEXT("Batch package stays dirty unless saved"), Fixture->Get()->GetPackage()->IsDirty(), !bSave);
			TArray<uint8> After;
			if (!Fixture->Snapshot(After)) return false;
			const TArray<uint8>& Before = Fixture == &First ? BeforeBatchFirst : BeforeBatchSecond;
			TestEqual(TEXT("Batch disk bytes change only with save true"), After != Before, bSave);
		}
	}
	return true;
}

#endif
