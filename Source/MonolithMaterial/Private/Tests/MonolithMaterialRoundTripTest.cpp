// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "RHI.h"
#include "RHIShaderPlatform.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace MonolithMaterialRoundTrip
{
	struct FFixture
	{
		FAutomationTestBase& Test;
		const FString Folder = TEXT("/Game/Tests/Monolith/Material/RoundTrip_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Path = Folder / TEXT("M_Health");
		bool bReportedMissingShaderMap = false;
		explicit FFixture(FAutomationTestBase& InTest) : Test(InTest) {}
		FString Filename() const { return FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension()); }
		UMaterial* Get() const { return FindObject<UMaterial>(nullptr, *(Path + TEXT(".M_Health"))); }
		~FFixture()
		{
			if (UMaterial* Material = Get())
			{
				if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform)) Resource->FinishCompilation();
				IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
				if (Watcher) Watcher->Tick(-1.f);
				IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
				Registry.ScanModifiedAssetFiles({Filename()});
				Registry.WaitForCompletion();
				Test.TestEqual(TEXT("GUID round-trip material deleted"), ObjectTools::ForceDeleteObjects({Material}, false), 1);
				if (Watcher) Watcher->Tick(-1.f);
				Registry.WaitForCompletion();
			}
			Test.TestFalse(TEXT("No material round-trip file remains"), IFileManager::Get().FileExists(*Filename()));
		}
		bool Snapshot(TArray<uint8>& Out) const
		{
			return Test.TestTrue(TEXT("Read nonempty material package bytes"), FFileHelper::LoadFileToArray(Out, *Filename()) && Out.Num() > 0);
		}
		bool Compile()
		{
			auto Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), Path);
			Params->SetBoolField(TEXT("include_stats"), true);
			const auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("recompile_material"), Params);
			if (!Test.TestTrue(TEXT("Actual material recompile action succeeds"), Result.bSuccess)) return false;
			FMaterialResource* Resource = Get()->GetMaterialResource(GMaxRHIShaderPlatform);
			if (Resource)
			{
				Resource->FinishCompilation();
				if (!Test.TestEqual(TEXT("Real material compilation has no errors"), Resource->GetCompileErrors().Num(), 0)) return false;
			}
			if (GUsingNullRHI && (!Resource || !Resource->GetGameThreadShaderMap()))
			{
				if (!bReportedMissingShaderMap)
				{
					Test.AddInfo(TEXT("UNVERIFIED SUBCHECK: NullRHI supplied no material shader map; actual compile action and create/mutate/read/save checks run, but GPU shader output requires the separate RHI run"));
					bReportedMissingShaderMap = true;
				}
				return true;
			}
			if (!Test.TestNotNull(TEXT("Compiled material resource exists"), Resource)) return false;
			if (!Test.TestNotNull(TEXT("Real compiled material shader map exists"), Resource->GetGameThreadShaderMap())) return false;
			return Test.TestTrue(TEXT("Real material shader map compilation is complete"), Resource->IsGameThreadShaderMapComplete());
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialRoundTripTest,
	"Monolith.Material.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialRoundTripTest::RunTest(const FString& Parameters)
{
	MonolithMaterialRoundTrip::FFixture Fixture(*this);
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	auto Create = MakeShared<FJsonObject>();
	Create->SetStringField(TEXT("asset_path"), Fixture.Path);
	if (!TestTrue(TEXT("Create material through actual action"), Registry.ExecuteAction(TEXT("material"), TEXT("create_material"), Create).bSuccess)) return false;
	if (!TestNotNull(TEXT("Created material is loaded"), Fixture.Get())) return false;
	TArray<uint8> Initial;
	if (!Fixture.Snapshot(Initial) || !Fixture.Compile()) return false;
	auto Save = MakeShared<FJsonObject>();
	Save->SetStringField(TEXT("asset_path"), Fixture.Path);
	Save->SetBoolField(TEXT("only_if_dirty"), false);
	const auto Settled = Registry.ExecuteAction(TEXT("material"), TEXT("save_material"), Save);
	if (!TestTrue(TEXT("Save initial compiled material"), Settled.bSuccess && Settled.Result->GetBoolField(TEXT("saved")))) return false;
	TArray<uint8> Baseline;
	if (!Fixture.Snapshot(Baseline)) return false;
	TestFalse(TEXT("Initial material package is clean"), Fixture.Get()->GetPackage()->IsDirty());
	for (int32 Step = 0; Step < 3; ++Step)
	{
		const bool bSave = Step == 2;
		const bool bTwoSided = Step != 1;
		auto Mutate = MakeShared<FJsonObject>();
		Mutate->SetStringField(TEXT("asset_path"), Fixture.Path);
		Mutate->SetBoolField(TEXT("two_sided"), bTwoSided);
		if (Step > 0) Mutate->SetBoolField(TEXT("save"), bSave);
		const auto Changed = Registry.ExecuteAction(TEXT("material"), TEXT("set_material_property"), Mutate);
		if (!TestTrue(TEXT("Actual material property mutation succeeds"), Changed.bSuccess)) return false;
		TestEqual(TEXT("Material mutation reports actual save"), Changed.Result->GetBoolField(TEXT("saved")), bSave);
		TestEqual(TEXT("Material package is dirty unless saved"), Fixture.Get()->GetPackage()->IsDirty(), !bSave);
		auto Read = MakeShared<FJsonObject>();
		Read->SetStringField(TEXT("asset_path"), Fixture.Path);
		const auto Readback = Registry.ExecuteAction(TEXT("material"), TEXT("get_material_properties"), Read);
		if (!TestTrue(TEXT("Read material through actual action"), Readback.bSuccess)) return false;
		TestEqual(TEXT("Material readback matches mutation"), Readback.Result->GetBoolField(TEXT("two_sided")), bTwoSided);
		TArray<uint8> After;
		if (!Fixture.Snapshot(After)) return false;
		TestEqual(TEXT("Material disk bytes change only with save true"), After != Baseline, bSave);
		if (!bSave)
		{
			if (!Fixture.Compile() || !Fixture.Snapshot(After)) return false;
			TestTrue(TEXT("Compile preserves unsaved material disk bytes"), After == Baseline);
			TestTrue(TEXT("Compile leaves unsaved material package dirty"), Fixture.Get()->GetPackage()->IsDirty());
		}
	}
	if (!Fixture.Compile()) return false;
	const auto FinalSave = Registry.ExecuteAction(TEXT("material"), TEXT("save_material"), Save);
	TestTrue(TEXT("Persist final compiled material"), FinalSave.bSuccess && FinalSave.Result->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Final material package is clean"), Fixture.Get()->GetPackage()->IsDirty());
	auto Typo = MakeShared<FJsonObject>();
	Typo->SetStringField(TEXT("asset_path"), Fixture.Folder / TEXT("M_Helath"));
	AddExpectedError(TEXT("LoadAsset failed:"), EAutomationExpectedErrorFlags::Contains, 1);
	const auto Missing = Registry.ExecuteAction(TEXT("material"), TEXT("get_material_properties"), Typo);
	TestEqual(TEXT("Material typo returns NotFound"), Missing.ErrorCode, FMonolithJsonUtils::ErrNotFound);
	if (!TestTrue(TEXT("Material typo provides structured evidence"), Missing.ErrorData.IsValid())) return false;
	const auto Data = Missing.ErrorData->AsObject();
	TestEqual(TEXT("Material typo error class"), Data->GetStringField(TEXT("class")), FString(TEXT("not_found")));
	TestFalse(TEXT("Material typo did not execute a mutation"), Data->GetBoolField(TEXT("executed")));
	const auto& Suggestions = Data->GetArrayField(TEXT("suggestions"));
	if (!TestTrue(TEXT("Material typo suggests a real asset"), Suggestions.Num() > 0)) return false;
	TestEqual(TEXT("Suggestion identifies created material"), Suggestions[0]->AsObject()->GetStringField(TEXT("material")), Fixture.Path);
	return true;
}
#endif
