// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "MonolithJsonUtils.h"
#include "MonolithSQLiteDatabase.h"
#include "Risk/FRiskMiningSession.h"
#include "Risk/FRiskQueryAdapter.h"

namespace MonolithAsyncRiskTest
{

struct FGate
{
	TAtomic<bool> Release{false};
	TAtomic<bool> CancelSetup{false};
	TAtomic<bool> WorkerWasGameThread{false};
	TAtomic<int32> Entries{0};
};

// Every command is confined to the GUID fixture and uses no shell or user hooks.
bool Git(const FString& Repo, const FString& Arguments, const TAtomic<bool>& Cancel, FString& Error)
{
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
	{
		Error = TEXT("Fixture git pipe creation failed");
		return false;
	}
	const FString Params = FString::Printf(
		TEXT("-C \"%s\" -c user.name=MonolithFixture -c user.email=fixture@example.invalid "
			"-c commit.gpgsign=false -c core.hooksPath=\"%s\" %s"),
		*Repo, *(Repo / TEXT("EmptyHooks")), *Arguments);
	FProcHandle Process = FPlatformProcess::CreateProc(TEXT("git"), *Params,
		false, true, true, nullptr, 0, nullptr, WritePipe, nullptr, WritePipe);
	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		Error = TEXT("Fixture git executable could not start");
		return false;
	}
	FString Output;
	const double Deadline = FPlatformTime::Seconds() + 10.0;
	bool bStopped = false;
	while (FPlatformProcess::IsProcRunning(Process))
	{
		Output += FPlatformProcess::ReadPipe(ReadPipe);
		if (Cancel.Load() || FPlatformTime::Seconds() > Deadline)
		{
			FPlatformProcess::TerminateProc(Process, true);
			bStopped = true;
			break;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	FPlatformProcess::WaitForProc(Process);
	Output += FPlatformProcess::ReadPipe(ReadPipe);
	int32 ExitCode = -1;
	FPlatformProcess::GetProcReturnCode(Process, &ExitCode);
	FPlatformProcess::CloseProc(Process);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
	if (bStopped || ExitCode != 0)
	{
		Error = FString::Printf(TEXT("Fixture git %s failed (%d): %s"), *Arguments, ExitCode, *Output);
		return false;
	}
	if (Arguments == TEXT("rev-list --count HEAD") && Output.TrimStartAndEnd() != TEXT("2"))
	{
		Error = TEXT("Fixture repository must contain exactly two commits: ") + Output;
		return false;
	}
	return true;
}

FString CreateRepository(const FString& Repo, const TAtomic<bool>& Cancel)
{
	IFileManager::Get().MakeDirectory(*(Repo / TEXT("Source")), true);
	IFileManager::Get().MakeDirectory(*(Repo / TEXT("EmptyHooks")), true);
	IFileManager::Get().MakeDirectory(*(Repo / TEXT("EmptyTemplate")), true);
	FString Error;
	if (!Git(Repo, FString::Printf(TEXT("init --template=\"%s\""), *(Repo / TEXT("EmptyTemplate"))), Cancel, Error))
	{
		return Error;
	}
	for (int32 Commit = 1; Commit <= 2; ++Commit)
	{
		const FString Heavy = FString::Printf(TEXT("#if WITH_MONOLITH_RISK_FIXTURE\nint Heavy = %d;\n#endif\n"), Commit);
		const FString Light = FString::Printf(TEXT("int Light = %d;\n"), Commit);
		if (!FFileHelper::SaveStringToFile(Heavy, *(Repo / TEXT("Source/Heavy.cpp"))) ||
			!FFileHelper::SaveStringToFile(Light, *(Repo / TEXT("Source/Light.cpp"))))
		{
			return TEXT("Could not write fixture source files");
		}
		if (!Git(Repo, TEXT("add -- Source/Heavy.cpp Source/Light.cpp"), Cancel, Error) ||
			!Git(Repo, FString::Printf(TEXT("commit --no-gpg-sign -m fixture-%d"), Commit), Cancel, Error))
		{
			return Error;
		}
	}
	if (!Git(Repo, TEXT("rev-list --count HEAD"), Cancel, Error)) return Error;
	return FString();
}

class FMiningCommand : public IAutomationLatentCommand
{
public:
	explicit FMiningCommand(FAutomationTestBase& InTest)
		: Test(InTest), Gate(MakeShared<FGate, ESPMode::ThreadSafe>())
	{
		Root = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()) /
			(TEXT("MonolithRisk-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Inputs.ProjectRoot = Root / TEXT("Repo");
		Inputs.DatabaseDirectory = Root / TEXT("RiskOutput");
		Inputs.GitRoots = {Inputs.ProjectRoot};
		Inputs.GateRoots = {Inputs.ProjectRoot / TEXT("Source")};
		Inputs.ComplexityRows = {
			{TEXT("Source/Heavy.cpp"), 100, TEXT("cpp")},
			{TEXT("Source/Light.cpp"), 10, TEXT("cpp")}};
		Inputs.MaxCommitWindow = 10;
		Inputs.MaxCommitFileCount = 10;
		Inputs.ConfigFingerprint = 12345;
		Inputs.SnapshotAt = TEXT("2026-09-05T00:00:00Z");
		Inputs.BeforeWorker = [SharedGate = Gate](const TAtomic<bool>& Cancel)
		{
			SharedGate->WorkerWasGameThread.Store(IsInGameThread());
			++SharedGate->Entries;
			while (!SharedGate->Release.Load() && !Cancel.Load()) FPlatformProcess::Sleep(0.001f);
		};
		Setup = Async(EAsyncExecution::Thread, [Repo = Inputs.ProjectRoot, SharedGate = Gate]()
		{
			return CreateRepository(Repo, SharedGate->CancelSetup);
		});
		Deadline = FPlatformTime::Seconds() + 30.0;
	}

	virtual ~FMiningCommand() override
	{
		Gate->CancelSetup.Store(true);
		Gate->Release.Store(true);
		Override.Reset();
		Session.Shutdown();
		if (Setup.IsValid()) Setup.Wait();
		// Root was constructed once from the automation directory plus our GUID.
		const FString Allowed = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir());
		if (Root.StartsWith(Allowed / TEXT("MonolithRisk-")))
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
			Test.TestFalse(TEXT("Owned risk fixture directory cleaned after handles close"), IFileManager::Get().DirectoryExists(*Root));
		}
	}

	virtual bool Update() override
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			Test.AddError(TEXT("Risk mining fixture exceeded its 30-second deadline"));
			return true;
		}
		if (Phase == 0)
		{
			if (!Setup.IsReady()) return false;
			const FString Error = Setup.Get();
			if (!Error.IsEmpty()) { Test.AddError(Error); return true; }
			Override = MakeUnique<FRiskQueryAdapter::FScopedMiningTestOverride>(Session,
				[this](FRiskMiningInputs& Out, FString&) { Out = Inputs; return true; });
			ExpectPrecondition();
			Test.TestEqual(TEXT("Unmined status is idle"), Status(), FString(TEXT("idle")));
			Test.TestEqual(TEXT("Queries do not start a worker"), Gate->Entries.Load(), 0);
			const double Started = FPlatformTime::Seconds();
			const FMonolithActionResult Mine = Call(TEXT("mine"));
			if (!Test.TestTrue(TEXT("Explicit mine starts successfully"), Mine.bSuccess)) return true;
			Test.TestTrue(TEXT("Mine returns without waiting for worker"), FPlatformTime::Seconds() - Started < 0.5);
			Phase = 1;
			return false;
		}
		if (Phase == 1)
		{
			if (Gate->Entries.Load() == 0) return false;
			Test.TestFalse(TEXT("Mining worker runs off the game thread"), Gate->WorkerWasGameThread.Load());
			const double Started = FPlatformTime::Seconds();
			Test.TestEqual(TEXT("Status responsive while worker gated"), Status(), FString(TEXT("running")));
			ExpectPrecondition();
			Test.TestTrue(TEXT("Repeated mine remains successful"), Call(TEXT("mine")).bSuccess);
			Test.TestTrue(TEXT("Queries and duplicate mine do not wait for worker"), FPlatformTime::Seconds() - Started < 0.5);
			Test.TestEqual(TEXT("Only one mining worker started"), Gate->Entries.Load(), 1);
			Test.TestTrue(TEXT("No query handle published while worker gated"), Session.GetQueryDB() == nullptr);
			// The worker must own the snapshot already supplied to Start, not borrow
			// this input array while the game thread changes its next-run inputs.
			Inputs.ComplexityRows[0].LineCount = 1;
			Gate->Release.Store(true);
			Phase = 2;
			return false;
		}
		if (Phase == 2)
		{
			const FString Current = Status();
			if (Current == TEXT("running")) return false;
			if (!Test.TestEqual(TEXT("Mining completes successfully"), Current, FString(TEXT("done")))) return true;
			CheckRows();
			Inputs.ComplexityRows[0].LineCount = 100;
			Session.Invalidate();
			ExpectPrecondition();
			Test.TestEqual(TEXT("Invalidation does not launch another worker"), Gate->Entries.Load(), 1);
			if (!Test.TestTrue(TEXT("Completed durable snapshot remains after invalidation"),
				FFileHelper::LoadFileToArray(CommittedBytes, *(Inputs.DatabaseDirectory / TEXT("Risk.db"))))) return true;
			const FString BrokenRepo = Root / TEXT("BrokenRepo");
			IFileManager::Get().MakeDirectory(*BrokenRepo, true);
			if (!Test.TestTrue(TEXT("Create owned invalid git fixture"), FFileHelper::SaveStringToFile(
				TEXT("gitdir: missing-fixture-git-directory\n"), *(BrokenRepo / TEXT(".git"))))) return true;
			Inputs.GitRoots = {BrokenRepo};
			if (!Test.TestTrue(TEXT("Explicit failing refresh starts"), Call(TEXT("mine")).bSuccess)) return true;
			Phase = 3;
			return false;
		}
		if (Phase == 3)
		{
			const FString Current = Status();
			if (Current == TEXT("running")) return false;
			if (!Test.TestEqual(TEXT("Actual git failure is reported as failed"), Current, FString(TEXT("failed")))) return true;
			ExpectPrecondition();
			TArray<uint8> AfterFailure;
			Test.TestTrue(TEXT("Failed refresh preserves durable database"),
				FFileHelper::LoadFileToArray(AfterFailure, *(Inputs.DatabaseDirectory / TEXT("Risk.db"))));
			Test.TestTrue(TEXT("Failed mining rolls back to exact previous snapshot bytes"), AfterFailure == CommittedBytes);
			CheckCacheRejections();
			{
				FRiskMiningSession FreshSession;
				FRiskMiningInputs CacheInputs = Inputs;
				CacheInputs.GitRoots = {Inputs.ProjectRoot};
				FString CacheError;
				if (Test.TestTrue(TEXT("Fresh session can load previous committed snapshot"), FreshSession.LoadCache(CacheInputs, CacheError)))
				{
					FRiskQueryAdapter::FScopedMiningTestOverride CacheOverride(FreshSession,
						[CacheInputs](FRiskMiningInputs& Out, FString&) { Out = CacheInputs; return true; });
					const FMonolithActionResult Cached = Call(TEXT("get_hotspot_score"), TEXT("Source/Heavy.cpp"));
					const TSharedPtr<FJsonObject>* Row = nullptr;
					if (Test.TestTrue(TEXT("Durable snapshot serves real query in fresh session"), Cached.bSuccess &&
						Cached.Result.IsValid() && Cached.Result->TryGetObjectField(TEXT("hotspot"), Row)))
						Test.TestEqual(TEXT("Failed refresh preserved committed hotspot"), (*Row)->GetNumberField(TEXT("score")), 1.0);
				}
			}
			// A real SQLite trigger fails the final stage after git and hotspot writes.
			// Include the trigger in our baseline: rollback must preserve the entire DB.
			{
				FMonolithSQLiteDatabase FixtureDB;
				if (!Test.TestTrue(TEXT("Open owned snapshot to install late-failure fixture"),
					FixtureDB.Open(*(Inputs.DatabaseDirectory / TEXT("Risk.db")), ESQLiteDatabaseOpenMode::ReadWrite))) return true;
				const bool bInstalled = FixtureDB.Execute(TEXT(
					"CREATE TRIGGER monolith_fixture_fail_gates BEFORE INSERT ON reflect_conditional_gates "
					"BEGIN SELECT RAISE(FAIL, 'monolith fixture late gate failure'); END;"));
				Test.TestTrue(TEXT("Close owned trigger fixture handle"), FixtureDB.Close());
				if (!Test.TestTrue(TEXT("Install late SQLite failure trigger"), bInstalled)) return true;
			}
			if (!Test.TestTrue(TEXT("Read full snapshot with fixture trigger"),
				FFileHelper::LoadFileToArray(CommittedBytes, *(Inputs.DatabaseDirectory / TEXT("Risk.db"))))) return true;
			Inputs.GitRoots = {Inputs.ProjectRoot};
			Inputs.ComplexityRows[0].LineCount = 1;
			if (!Test.TestTrue(TEXT("Late-failing refresh starts"), Call(TEXT("mine")).bSuccess)) return true;
			Phase = 4;
			return false;
		}
		if (Phase == 4)
		{
			const FString Current = Status();
			if (Current == TEXT("running")) return false;
			if (!Test.TestEqual(TEXT("Late SQLite write failure is reported"), Current, FString(TEXT("failed")))) return true;
			const FMonolithActionResult FailedStatus = Call(TEXT("get_mining_status"));
			const TSharedPtr<FJsonObject>* Progress = nullptr;
			if (Test.TestTrue(TEXT("Failed status retains completed work progress"), FailedStatus.Result.IsValid() &&
				FailedStatus.Result->TryGetObjectField(TEXT("progress"), Progress)))
			{
				Test.TestEqual(TEXT("Git stage completed before injected failure"), (*Progress)->GetNumberField(TEXT("repos_completed")), 1.0);
				Test.TestEqual(TEXT("Gate files read before injected write failure"), (*Progress)->GetNumberField(TEXT("files_scanned")), 2.0);
			}
			TArray<uint8> AfterLateFailure;
			Test.TestTrue(TEXT("Read snapshot after late failure"),
				FFileHelper::LoadFileToArray(AfterLateFailure, *(Inputs.DatabaseDirectory / TEXT("Risk.db"))));
			Test.TestTrue(TEXT("Late failure rolls back all earlier stage writes"), AfterLateFailure == CommittedBytes);
			CheckNoSidecars();
			ExpectPrecondition();
			// Start another real operation, then cancel while its pre-DB gate is held.
			Gate->Release.Store(false);
			if (!Test.TestTrue(TEXT("Mine after late failed refresh starts"), Call(TEXT("mine")).bSuccess)) return true;
			Phase = 5;
			return false;
		}
		if (Gate->Entries.Load() < 4) return false;
		const double Started = FPlatformTime::Seconds();
		Session.Shutdown();
		Test.TestTrue(TEXT("Shutdown cancels gated worker without GT continuation"), FPlatformTime::Seconds() - Started < 1.0);
		Test.TestFalse(TEXT("Shutdown leaves no running worker"), Session.IsRunning());
		Test.TestTrue(TEXT("Shutdown closes the query handle"), Session.GetQueryDB() == nullptr);
		TArray<uint8> AfterCancellation;
		Test.TestTrue(TEXT("Cancellation preserves durable snapshot"),
			FFileHelper::LoadFileToArray(AfterCancellation, *(Inputs.DatabaseDirectory / TEXT("Risk.db"))));
		Test.TestTrue(TEXT("Cancelled operation leaves previous snapshot bytes intact"), AfterCancellation == CommittedBytes);
		CheckNoSidecars();
		return true;
	}

private:
	FMonolithActionResult Call(const TCHAR* Action, const TCHAR* Path = nullptr)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (Path) Params->SetStringField(TEXT("file_path"), Path);
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("risk"), Action, Params);
	}
	FString Status()
	{
		const FMonolithActionResult Result = Call(TEXT("get_mining_status"));
		if (!Test.TestTrue(TEXT("Mining status action succeeds"), Result.bSuccess && Result.Result.IsValid())) return TEXT("invalid");
		return Result.Result->GetStringField(TEXT("state"));
	}
	void ExpectPrecondition()
	{
		for (const TCHAR* Action : {TEXT("get_hotspot_score"), TEXT("get_cochange_pairs"),
			TEXT("get_file_churn"), TEXT("get_release_window_hotspots"), TEXT("list_conditional_gates")})
		{
			const bool bNeedsPath = FCString::Strcmp(Action, TEXT("get_release_window_hotspots")) != 0 &&
				FCString::Strcmp(Action, TEXT("list_conditional_gates")) != 0;
			const double Started = FPlatformTime::Seconds();
			const FMonolithActionResult Result = Call(Action, bNeedsPath ? TEXT("Source/Heavy.cpp") : nullptr);
			Test.TestTrue(TEXT("Unmined query fails fast"), FPlatformTime::Seconds() - Started < 0.5);
			Test.TestFalse(TEXT("Unmined query is an error"), Result.bSuccess);
			Test.TestEqual(TEXT("Unmined query uses precondition code"), Result.ErrorCode, FMonolithJsonUtils::ErrPreconditionFailed);
			if (Test.TestTrue(TEXT("Precondition has structured data"), Result.ErrorData.IsValid()))
			{
				const TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
				if (!Test.TestTrue(TEXT("Precondition data is an object"), Data.IsValid())) return;
				Test.TestEqual(TEXT("Precondition class"), Data->GetStringField(TEXT("class")), FString(TEXT("precondition_failed")));
				Test.TestEqual(TEXT("Precondition points to explicit mining"), Data->GetStringField(TEXT("next_action")), FString(TEXT("risk.mine")));
				Test.TestFalse(TEXT("Unmined query executed no mutation"), Data->GetBoolField(TEXT("executed")));
			}
		}
	}
	void CheckCacheRejections()
	{
		for (int32 Mismatch = 0; Mismatch < 3; ++Mismatch)
		{
			FRiskMiningSession RejectedSession;
			FRiskMiningInputs Mismatched = Inputs;
			if (Mismatch == 0) ++Mismatched.ConfigFingerprint;
			else if (Mismatch == 1) ++Mismatched.CodeVersion;
			else Mismatched.ProjectRoot = Root / TEXT("DifferentProject");
			FString CacheError;
			Test.TestFalse(TEXT("Cache rejects mismatched configuration, code or project identity"),
				RejectedSession.LoadCache(Mismatched, CacheError));
			Test.TestTrue(TEXT("Rejected cache explains explicit remine"), CacheError.Contains(TEXT("risk.mine")));
			Test.TestTrue(TEXT("Rejected cache publishes no handle"), RejectedSession.GetQueryDB() == nullptr);
		}
	}
	void CheckNoSidecars()
	{
		for (const TCHAR* Suffix : {TEXT("-journal"), TEXT("-wal"), TEXT("-shm")})
			Test.TestFalse(TEXT("Completed worker leaves no SQLite sidecar"),
				IFileManager::Get().FileExists(*(Inputs.DatabaseDirectory / (FString(TEXT("Risk.db")) + Suffix))));
	}
	void CheckRows()
	{
		const FMonolithActionResult MiningStatus = Call(TEXT("get_mining_status"));
		const TSharedPtr<FJsonObject>* Progress = nullptr;
		if (Test.TestTrue(TEXT("Done status includes real progress"), MiningStatus.Result.IsValid() &&
			MiningStatus.Result->TryGetObjectField(TEXT("progress"), Progress)))
		{
			Test.TestEqual(TEXT("One actual repository completed"), (*Progress)->GetNumberField(TEXT("repos_completed")), 1.0);
			Test.TestEqual(TEXT("Only two fixture source files scanned"), (*Progress)->GetNumberField(TEXT("files_scanned")), 2.0);
		}
		CheckNoSidecars();
		const FMonolithActionResult Heavy = Call(TEXT("get_hotspot_score"), TEXT("Source/Heavy.cpp"));
		if (!Test.TestTrue(TEXT("Original query succeeds after mine"), Heavy.bSuccess && Heavy.Result.IsValid())) return;
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (Test.TestTrue(TEXT("Heavy hotspot is present"), Heavy.Result->TryGetObjectField(TEXT("hotspot"), Row)))
		{
			Test.TestEqual(TEXT("Actual two-commit churn"), (*Row)->GetNumberField(TEXT("churn")), 2.0);
			Test.TestEqual(TEXT("Captured complexity is joined"), (*Row)->GetNumberField(TEXT("complexity_proxy")), 100.0);
			Test.TestEqual(TEXT("Heavy hotspot score"), (*Row)->GetNumberField(TEXT("score")), 1.0);
		}
		const FMonolithActionResult Light = Call(TEXT("get_hotspot_score"), TEXT("Source/Light.cpp"));
		if (Test.TestTrue(TEXT("Light hotspot is present"), Light.bSuccess && Light.Result.IsValid() && Light.Result->TryGetObjectField(TEXT("hotspot"), Row)))
			Test.TestTrue(TEXT("Light score follows captured complexity"), FMath::IsNearlyEqual((*Row)->GetNumberField(TEXT("score")), 0.1));
		const FMonolithActionResult Pairs = Call(TEXT("get_cochange_pairs"), TEXT("Source/Heavy.cpp"));
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Test.TestTrue(TEXT("Co-change query succeeds"), Pairs.bSuccess && Pairs.Result.IsValid() && Pairs.Result->TryGetArrayField(TEXT("partners"), Rows)))
		{
			if (Test.TestEqual(TEXT("Exactly one partner"), Rows->Num(), 1))
			{
				Test.TestEqual(TEXT("Actual git partner"), (*Rows)[0]->AsObject()->GetStringField(TEXT("partner")), FString(TEXT("Source/Light.cpp")));
				Test.TestEqual(TEXT("Actual git pair weight"), (*Rows)[0]->AsObject()->GetNumberField(TEXT("count")), 2.0);
			}
		}
		const FMonolithActionResult Churn = Call(TEXT("get_file_churn"), TEXT("Source/Heavy.cpp"));
		if (Test.TestTrue(TEXT("Churn query succeeds"), Churn.bSuccess && Churn.Result.IsValid() && Churn.Result->TryGetArrayField(TEXT("churn_by_repo"), Rows)))
			if (Test.TestEqual(TEXT("Exactly one fixture repository"), Rows->Num(), 1))
				Test.TestEqual(TEXT("Two real commits counted"), (*Rows)[0]->AsObject()->GetNumberField(TEXT("commit_count")), 2.0);
		const FMonolithActionResult Window = Call(TEXT("get_release_window_hotspots"));
		if (Test.TestTrue(TEXT("Release-window query succeeds"), Window.bSuccess && Window.Result.IsValid() &&
			Window.Result->TryGetArrayField(TEXT("hotspots"), Rows)))
		{
			if (Test.TestEqual(TEXT("Both newly committed source files fall in release window"), Rows->Num(), 2))
				Test.TestEqual(TEXT("Release window sorts highest hotspot first"),
					(*Rows)[0]->AsObject()->GetStringField(TEXT("file_path")), FString(TEXT("Source/Heavy.cpp")));
		}
		const FMonolithActionResult Gates = Call(TEXT("list_conditional_gates"));
		if (Test.TestTrue(TEXT("Gate query succeeds"), Gates.bSuccess && Gates.Result.IsValid() && Gates.Result->TryGetArrayField(TEXT("gates"), Rows)))
		{
			bool bFound = false;
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
				bFound |= Value->AsObject()->GetStringField(TEXT("macro_name")) == TEXT("WITH_MONOLITH_RISK_FIXTURE");
			Test.TestTrue(TEXT("Actual source conditional was mined"), bFound);
		}
	}

	FAutomationTestBase& Test;
	FString Root;
	FRiskMiningInputs Inputs;
	FRiskMiningSession Session;
	TSharedPtr<FGate, ESPMode::ThreadSafe> Gate;
	TUniquePtr<FRiskQueryAdapter::FScopedMiningTestOverride> Override;
	TFuture<FString> Setup;
	TArray<uint8> CommittedBytes;
	double Deadline = 0.0;
	int32 Phase = 0;
};

} // namespace MonolithAsyncRiskTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithRiskAsyncMiningTest,
	"Monolith.ReflectionIntel.Risk.AsyncMining",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRiskAsyncMiningTest::RunTest(const FString&)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<MonolithAsyncRiskTest::FMiningCommand>(*this));
	return true;
}

#endif
