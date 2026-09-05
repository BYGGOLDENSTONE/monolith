// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "NiagaraSystem.h"
#include "NiagaraScript.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace MonolithNiagaraRoundTrip
{
	struct FFixture
	{
		FAutomationTestBase& Test;
		const FString Folder = TEXT("/Game/Tests/Monolith/Niagara/RoundTrip_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Path = Folder / TEXT("NS_Health");
		explicit FFixture(FAutomationTestBase& InTest) : Test(InTest) {}
		FString Filename() const { return FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension()); }
		UNiagaraSystem* Get() const { return FindObject<UNiagaraSystem>(nullptr, *(Path + TEXT(".NS_Health"))); }
		~FFixture()
		{
			if (UNiagaraSystem* System = Get())
			{
				System->WaitForCompilationComplete(false, false);
				IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
				if (Watcher) Watcher->Tick(-1.f);
				IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
				Registry.ScanModifiedAssetFiles({Filename()});
				Registry.WaitForCompletion();
				Test.TestEqual(TEXT("GUID Niagara asset deleted"), ObjectTools::ForceDeleteObjects({System}, false), 1);
				if (Watcher) Watcher->Tick(-1.f);
				Registry.WaitForCompletion();
			}
			Test.TestFalse(TEXT("No Niagara fixture file remains"), IFileManager::Get().FileExists(*Filename()));
		}
		bool Snapshot(TArray<uint8>& Out) const
		{
			return Test.TestTrue(TEXT("Read nonempty Niagara package bytes"), FFileHelper::LoadFileToArray(Out, *Filename()) && Out.Num() > 0);
		}
		bool Compile()
		{
			auto Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), Path);
			Params->SetBoolField(TEXT("force"), true);
			Params->SetBoolField(TEXT("synchronous"), true);
			const auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("niagara"), TEXT("request_compile"), Params);
			if (!Test.TestTrue(TEXT("Real Niagara compile action succeeds"), Result.bSuccess)) return false;
			UNiagaraSystem* System = Get();
			if (!Test.TestNotNull(TEXT("Compiled system exists"), System)) return false;
			bool bValid = Test.TestFalse(TEXT("Niagara compilation has completed"), System->HasOutstandingCompilationRequests());
			for (UNiagaraScript* Script : {System->GetSystemSpawnScript(), System->GetSystemUpdateScript()})
			{
				if (!Test.TestNotNull(TEXT("Factory produced real system script"), Script)) { bValid = false; continue; }
				const auto Status = Script->GetLastCompileStatus();
				bValid &= Test.TestTrue(TEXT("System script compiled successfully"), Status == ENiagaraScriptCompileStatus::NCS_UpToDate || Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings);
				bValid &= Test.TestTrue(TEXT("Compiled Niagara VM executable is valid"), Script->GetVMExecutableData().IsValid());
			}
			return bValid;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraRoundTripTest,
	"Monolith.Niagara.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraRoundTripTest::RunTest(const FString& Parameters)
{
	MonolithNiagaraRoundTrip::FFixture Fixture(*this);
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	auto Create = MakeShared<FJsonObject>();
	Create->SetStringField(TEXT("save_path"), Fixture.Path);
	if (!TestTrue(TEXT("Create system through actual action"), Registry.ExecuteAction(TEXT("niagara"), TEXT("create_system"), Create).bSuccess)) return false;
	if (!TestNotNull(TEXT("Created Niagara system is loaded"), Fixture.Get())) return false;
	TArray<uint8> Initial;
	if (!Fixture.Snapshot(Initial) || !Fixture.Compile()) return false;
	auto Save = MakeShared<FJsonObject>();
	Save->SetStringField(TEXT("asset_path"), Fixture.Path);
	Save->SetBoolField(TEXT("only_if_dirty"), false);
	const auto Settled = Registry.ExecuteAction(TEXT("niagara"), TEXT("save_system"), Save);
	if (!TestTrue(TEXT("Save initial compiled system"), Settled.bSuccess && Settled.Result->GetBoolField(TEXT("saved")))) return false;
	TArray<uint8> Baseline;
	if (!Fixture.Snapshot(Baseline)) return false;
	TestFalse(TEXT("Initial compiled package is clean"), Fixture.Get()->GetPackage()->IsDirty());

	for (int32 Step = 0; Step < 3; ++Step)
	{
		const bool bSave = Step == 2;
		const int32 Seed = 314159 + Step;
		auto Mutate = MakeShared<FJsonObject>();
		Mutate->SetStringField(TEXT("asset_path"), Fixture.Path);
		Mutate->SetStringField(TEXT("property"), TEXT("RandomSeed"));
		Mutate->SetStringField(TEXT("value"), LexToString(Seed));
		if (Step > 0) Mutate->SetBoolField(TEXT("save"), bSave);
		const auto Changed = Registry.ExecuteAction(TEXT("niagara"), TEXT("set_system_property"), Mutate);
		if (!TestTrue(TEXT("Actual system property mutation succeeds"), Changed.bSuccess)) return false;
		TestEqual(TEXT("Mutation reports actual save"), Changed.Result->GetBoolField(TEXT("saved")), bSave);
		TestEqual(TEXT("Niagara package is dirty unless saved"), Fixture.Get()->GetPackage()->IsDirty(), !bSave);
		auto Read = MakeShared<FJsonObject>();
		Read->SetStringField(TEXT("asset_path"), Fixture.Path);
		Read->SetStringField(TEXT("property"), TEXT("RandomSeed"));
		const auto Readback = Registry.ExecuteAction(TEXT("niagara"), TEXT("get_system_property"), Read);
		if (!TestTrue(TEXT("Read mutated property through actual action"), Readback.bSuccess)) return false;
		TestEqual(TEXT("System property readback matches mutation"), static_cast<int32>(Readback.Result->GetNumberField(TEXT("value"))), Seed);
		TArray<uint8> After;
		if (!Fixture.Snapshot(After)) return false;
		TestEqual(TEXT("Niagara disk bytes change only with save true"), After != Baseline, bSave);
		if (!bSave)
		{
			if (!Fixture.Compile() || !Fixture.Snapshot(After)) return false;
			TestTrue(TEXT("Compile preserves unsaved Niagara disk bytes"), After == Baseline);
			TestTrue(TEXT("Compile leaves unsaved Niagara package dirty"), Fixture.Get()->GetPackage()->IsDirty());
		}
	}
	// Prove the final saved state also compiles, then persist its generated data.
	if (!Fixture.Compile()) return false;
	const auto FinalSave = Registry.ExecuteAction(TEXT("niagara"), TEXT("save_system"), Save);
	TestTrue(TEXT("Persist final compiled Niagara system"), FinalSave.bSuccess && FinalSave.Result->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Final Niagara package is clean"), Fixture.Get()->GetPackage()->IsDirty());
	auto Typo = MakeShared<FJsonObject>();
	Typo->SetStringField(TEXT("asset_path"), Fixture.Folder / TEXT("NS_Helath"));
	Typo->SetStringField(TEXT("property"), TEXT("RandomSeed"));
	AddExpectedError(TEXT("Failed to load Niagara system: ") + (Fixture.Folder / TEXT("NS_Helath")), EAutomationExpectedErrorFlags::Contains, 1);
	const auto Missing = Registry.ExecuteAction(TEXT("niagara"), TEXT("get_system_property"), Typo);
	TestEqual(TEXT("System typo returns NotFound"), Missing.ErrorCode, FMonolithJsonUtils::ErrNotFound);
	if (!TestTrue(TEXT("System typo provides structured evidence"), Missing.ErrorData.IsValid())) return false;
	const auto Data = Missing.ErrorData->AsObject();
	TestEqual(TEXT("System typo error class"), Data->GetStringField(TEXT("class")), FString(TEXT("not_found")));
	TestFalse(TEXT("Typo did not execute a mutation"), Data->GetBoolField(TEXT("executed")));
	const auto& Suggestions = Data->GetArrayField(TEXT("suggestions"));
	if (!TestTrue(TEXT("System typo suggests a real asset"), Suggestions.Num() > 0)) return false;
	TestEqual(TEXT("Suggestion identifies the created system"), Suggestions[0]->AsObject()->GetStringField(TEXT("system")), Fixture.Path);
	return true;
}
#endif
