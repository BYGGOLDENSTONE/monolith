// SPDX-License-Identifier: MIT
#include "Risk/FRiskMiningSession.h"
#include "Risk/FRiskMiningWorkerContext.h"
#include "Risk/FGitCoChangeIndexer.h"
#include "Risk/FHotspotScorer.h"
#include "Risk/FConditionalGateIndexer.h"
#include "MonolithRIMetaTable.h"
#include "MonolithSQLiteDatabase.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"

struct FRiskMiningSession::FWorkerState
{
	TAtomic<bool> CancelRequested{false};
	TAtomic<int32> Stage{0};
	TAtomic<int32> CompletedRepos{0};
	TAtomic<int32> CompletedFiles{0};
	FString DatabasePath;
	FString Status;
	bool bSuccess = false;
	bool bCreatedNew = false;
	bool bClosed = true;
	uint64 Generation = 0;
};

FRiskMiningSession::FRiskMiningSession() = default;
FRiskMiningSession::~FRiskMiningSession() { Shutdown(); }

bool FRiskMiningSession::Start(const FRiskMiningInputs& Inputs, FString& OutError)
{
	check(IsInGameThread());
	Poll();
	if (bStopping || IsRunning())
	{
		OutError = bStopping ? TEXT("risk mining is shutting down") : TEXT("risk mining is already running");
		return false;
	}
	if (Inputs.ProjectRoot.IsEmpty() || Inputs.DatabaseDirectory.IsEmpty()
		|| FPaths::IsRelative(Inputs.ProjectRoot) || FPaths::IsRelative(Inputs.DatabaseDirectory))
	{
		OutError = TEXT("risk mining requires an absolute project root and database directory");
		return false;
	}
	ClearPublished();
	LastInputs = Inputs;
	if (LastInputs.SnapshotAt.IsEmpty()) LastInputs.SnapshotAt = FDateTime::UtcNow().ToIso8601();
	bInvalidated = false;
	State = TEXT("running");
	LastStatus = TEXT("risk mining started from the captured source/configuration snapshot");
	Worker = MakeShared<FWorkerState, ESPMode::ThreadSafe>();
	Worker->DatabasePath = Inputs.DatabaseDirectory / TEXT("Risk.db");
	Worker->Generation = ++Generation;
	const auto CurrentWorker = Worker.ToSharedRef();
	Future = Async(EAsyncExecution::ThreadPool, [CurrentWorker, Snapshot = LastInputs]()
	{
		check(!IsInGameThread());
#if WITH_DEV_AUTOMATION_TESTS
		if (Snapshot.BeforeWorker) Snapshot.BeforeWorker(CurrentWorker->CancelRequested);
#endif
		if (CurrentWorker->CancelRequested.Load())
		{
			CurrentWorker->Status = TEXT("risk mining cancelled before database creation");
			return;
		}
		FMonolithSQLiteDatabase DB;
		const bool bExisted = IFileManager::Get().FileExists(*CurrentWorker->DatabasePath);
		const auto Build = [&]() -> bool
		{
			if (!IFileManager::Get().MakeDirectory(*Snapshot.DatabaseDirectory, true))
			{
				CurrentWorker->Status = TEXT("risk mining could not create its database directory");
				return false;
			}
			// Even a failed Open may have created a file. Only a path absent before
			// this worker opened it is eligible for failed-pass cleanup.
			CurrentWorker->bCreatedNew = !bExisted;
			if (!DB.Open(*CurrentWorker->DatabasePath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
			{
				CurrentWorker->Status = TEXT("risk mining could not open its database: ") + DB.GetLastError();
				return false;
			}
			if (!DB.Execute(TEXT("PRAGMA journal_mode=DELETE;"))
				|| !DB.Execute(TEXT("BEGIN IMMEDIATE;"))
				|| !DB.Execute(TEXT("CREATE TABLE IF NOT EXISTS files(path TEXT, line_count INTEGER, file_type TEXT);"))
				|| !DB.Execute(TEXT("DELETE FROM files;")))
			{
				CurrentWorker->Status = TEXT("risk mining could not create its complexity snapshot");
				return false;
			}
			{
				FSQLitePreparedStatement Insert;
				if (!Insert.Create(DB, TEXT("INSERT INTO files(path,line_count,file_type) VALUES(?,?,?);")))
				{
					CurrentWorker->Status = TEXT("risk complexity INSERT prepare failed");
					return false;
				}
				for (const FRiskComplexityRow& Row : Snapshot.ComplexityRows)
				{
					if (CurrentWorker->CancelRequested.Load())
					{
						CurrentWorker->Status = TEXT("risk mining cancelled while writing complexity snapshot");
						return false;
					}
					Insert.Reset();
					Insert.ClearBindings();
					if (!Insert.SetBindingValueByIndex(1, Row.Path)
						|| !Insert.SetBindingValueByIndex(2, Row.LineCount)
						|| !Insert.SetBindingValueByIndex(3, Row.FileType)
						|| !Insert.Execute())
					{
						CurrentWorker->Status = TEXT("risk complexity INSERT failed");
						return false;
					}
				}
			}
			FRiskMiningWorkerContext Context;
			Context.ProjectRoot = Snapshot.ProjectRoot;
			Context.CancelRequested = &CurrentWorker->CancelRequested;
			Context.CompletedRepos = &CurrentWorker->CompletedRepos;
			Context.CompletedFiles = &CurrentWorker->CompletedFiles;
			FString GitStatus, HotspotStatus, GateStatus;
			CurrentWorker->Stage = 1;
			FGitCoChangeIndexer Git;
			if (!Git.RunOwnedDatabase(DB, Context, Snapshot.GitRoots, Snapshot.MaxCommitWindow,
				Snapshot.NoiseFilter, Snapshot.MaxCommitFileCount, GitStatus))
			{
				CurrentWorker->Status = GitStatus;
				return false;
			}
			CurrentWorker->Stage = 2;
			FHotspotScorer Hotspot;
			if (!Hotspot.RunOwnedDatabase(DB, Context, HotspotStatus))
			{
				CurrentWorker->Status = HotspotStatus;
				return false;
			}
			CurrentWorker->Stage = 3;
			FConditionalGateIndexer Gates;
			if (!Gates.RunOwnedDatabase(DB, Context, Snapshot.GateRoots, GateStatus))
			{
				CurrentWorker->Status = GateStatus;
				return false;
			}
			if (Context.IsCancelled())
			{
				CurrentWorker->Status = TEXT("risk mining cancelled before publication");
				return false;
			}
			if (!MonolithRIMeta::WriteStoredVersion(DB, TEXT("risk"), Snapshot.CodeVersion)
				|| !MonolithRIMeta::WriteStoredVersion(DB, MonolithRIMeta::GetRiskConfigSubsystemKey(), Snapshot.ConfigFingerprint))
			{
				CurrentWorker->Status = TEXT("risk mining completion stamps could not be written");
				return false;
			}
			if (!DB.Execute(TEXT("CREATE TABLE IF NOT EXISTS risk_snapshot(project_root TEXT, snapshot_at TEXT, complexity_rows INTEGER);"))
				|| !DB.Execute(TEXT("DELETE FROM risk_snapshot;")))
			{
				CurrentWorker->Status = TEXT("risk snapshot metadata schema failed");
				return false;
			}
			{
				FSQLitePreparedStatement Meta;
				if (!Meta.Create(DB, TEXT("INSERT INTO risk_snapshot VALUES(?,?,?);")))
				{
					CurrentWorker->Status = TEXT("risk snapshot metadata INSERT prepare failed");
					return false;
				}
				if (!Meta.SetBindingValueByIndex(1, Snapshot.ProjectRoot)
					|| !Meta.SetBindingValueByIndex(2, Snapshot.SnapshotAt)
					|| !Meta.SetBindingValueByIndex(3, Snapshot.ComplexityRows.Num())
					|| !Meta.Execute())
				{
					CurrentWorker->Status = TEXT("risk snapshot metadata write failed");
					return false;
				}
			}
			if (Context.IsCancelled() || !DB.Execute(TEXT("COMMIT;")))
			{
				CurrentWorker->Status = TEXT("risk mining cancelled or final transaction commit failed");
				return false;
			}
			CurrentWorker->Status = FString::Printf(TEXT("git=%s | hotspot=%s | gates=%s"),
				*GitStatus, *HotspotStatus, *GateStatus);
			return true;
		};
		const bool bBuilt = Build();
		if (!bBuilt && DB.IsValid())
		{
			const FString SqlError = DB.GetLastError();
			if (!SqlError.IsEmpty() && SqlError != TEXT("not an error")) CurrentWorker->Status += TEXT(" | SQLite: ") + SqlError;
			DB.Execute(TEXT("ROLLBACK;"));
		}
		CurrentWorker->bClosed = DB.Close();
		if (!CurrentWorker->bClosed) CurrentWorker->Status = TEXT("risk mining database close failed");
		CurrentWorker->bSuccess = bBuilt && CurrentWorker->bClosed;
		CurrentWorker->Stage = 4;
	});
	return true;
}

void FRiskMiningSession::Poll()
{
	check(IsInGameThread());
	if (!Future.IsValid() || !Future.IsReady()) return;
	Future.Get();
	Future = TFuture<void>();
	LastStatus = Worker->Status;
	if (bStopping || bInvalidated || Worker->Generation != Generation || !Worker->bSuccess)
	{
		if (!Worker->bSuccess && Worker->bCreatedNew && Worker->bClosed) IFileManager::Get().Delete(*Worker->DatabasePath, false, true);
		State = bInvalidated ? TEXT("idle") : TEXT("failed");
		return;
	}
	QueryDB = MakeUnique<FMonolithSQLiteDatabase>();
	if (!QueryDB->Open(*Worker->DatabasePath, ESQLiteDatabaseOpenMode::ReadOnly))
	{
		QueryDB->Close();
		QueryDB.Reset();
		State = TEXT("failed");
		LastStatus = TEXT("completed risk database could not be opened for queries");
		return;
	}
	PublishedPath = Worker->DatabasePath;
	PublishedComplexityRows = LastInputs.ComplexityRows.Num();
	State = TEXT("done");
}

bool FRiskMiningSession::IsRunning() const { return State == TEXT("running"); }

TSharedPtr<FJsonObject> FRiskMiningSession::GetStatus()
{
	check(IsInGameThread());
	Poll();
	auto Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("state"), State);
	Out->SetStringField(TEXT("last_status"), LastStatus);
	Out->SetBoolField(TEXT("snapshot_available"), QueryDB.IsValid());
	Out->SetStringField(TEXT("snapshot_at"), LastInputs.SnapshotAt);
	Out->SetNumberField(TEXT("snapshot_capture_ms"), LastInputs.SnapshotCaptureMs);
	Out->SetStringField(TEXT("snapshot_semantics"), TEXT("source complexity and settings captured when risk.mine starts; git and gates read during that pass"));
	Out->SetNumberField(TEXT("complexity_rows"), (QueryDB ? PublishedComplexityRows : LastInputs.ComplexityRows.Num()));
	Out->SetNumberField(TEXT("config_fingerprint"), LastInputs.ConfigFingerprint);
	auto Progress = MakeShared<FJsonObject>();
	const TCHAR* Stages[] = {TEXT("snapshot"), TEXT("git"), TEXT("hotspots"), TEXT("gates"), TEXT("publish")};
	Progress->SetStringField(TEXT("stage"), Worker ? Stages[FMath::Clamp(Worker->Stage.Load(), 0, 4)] : TEXT("idle"));
	Progress->SetNumberField(TEXT("repos_completed"), Worker ? Worker->CompletedRepos.Load() : 0);
	Progress->SetNumberField(TEXT("repos_total"), LastInputs.GitRoots.Num());
	Progress->SetNumberField(TEXT("files_scanned"), Worker ? Worker->CompletedFiles.Load() : 0);
	Out->SetObjectField(TEXT("progress"), Progress);
	TArray<TSharedPtr<FJsonValue>> Roots;
	for (const FString& Root : LastInputs.GitRoots) Roots.Add(MakeShared<FJsonValueString>(Root));
	Out->SetArrayField(TEXT("repos_requested"), Roots);
	Out->SetArrayField(TEXT("repos_scanned"), State == TEXT("done") ? Roots : TArray<TSharedPtr<FJsonValue>>());
	TArray<TSharedPtr<FJsonValue>> Skipped;
	for (const auto& Pair : LastInputs.SkippedRoots)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Pair.Key);
		Entry->SetStringField(TEXT("reason"), Pair.Value);
		Skipped.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Out->SetArrayField(TEXT("repos_skipped"), Skipped);
	return Out;
}

FSQLiteDatabase* FRiskMiningSession::GetQueryDB()
{
	Poll();
	return QueryDB.Get();
}

void FRiskMiningSession::ClearPublished()
{
	if (QueryDB) { QueryDB->Close(); QueryDB.Reset(); }
	PublishedPath.Reset();
}

void FRiskMiningSession::Invalidate()
{
	check(IsInGameThread());
	bInvalidated = true;
	++Generation;
	if (Worker) Worker->CancelRequested = true;
	ClearPublished();
	if (!IsRunning()) State = TEXT("idle");
	LastStatus = TEXT("risk snapshot invalidated; call risk.mine explicitly");
}

void FRiskMiningSession::Shutdown()
{
	check(IsInGameThread());
	if (bStopping) return;
	bStopping = true;
	if (Worker) Worker->CancelRequested = true;
	if (Future.IsValid()) { Future.Wait(); Poll(); }
	ClearPublished();
	Worker.Reset();
	State = TEXT("idle");
}


bool FRiskMiningSession::IsCacheValid(FSQLiteDatabase& DB, const FRiskMiningInputs& Inputs)
{
	check(IsInGameThread());
	int32 Version = 0, Config = 0;
	if (!MonolithRIMeta::ReadStoredVersion(DB, TEXT("risk"), Version) || Version != Inputs.CodeVersion
		|| !MonolithRIMeta::ReadStoredVersion(DB, MonolithRIMeta::GetRiskConfigSubsystemKey(), Config)
		|| Config != Inputs.ConfigFingerprint) return false;
	FSQLitePreparedStatement Tables;
	if (!Tables.Create(DB, TEXT("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('git_file_churn','git_cochange_pairs','risk_hotspot_scores','reflect_conditional_gates');"))
		|| Tables.Step() != ESQLitePreparedStatementStepResult::Row) return false;
	int32 Count = 0;
	return Tables.GetColumnValueByIndex(0, Count) && Count == 4;
}

bool FRiskMiningSession::LoadCache(const FRiskMiningInputs& Inputs, FString& OutError)
{
	check(IsInGameThread());
	Poll();
	if (bInvalidated || bStopping || IsRunning()) return false;
	ClearPublished();
	State = TEXT("idle");
	const FString Path = Inputs.DatabaseDirectory / TEXT("Risk.db");
	if (!IFileManager::Get().FileExists(*Path)) return false;
	QueryDB = MakeUnique<FMonolithSQLiteDatabase>();
	if (!QueryDB->Open(*Path, ESQLiteDatabaseOpenMode::ReadOnly))
	{
		QueryDB->Close();
		QueryDB.Reset();
		OutError = TEXT("risk cache could not be opened; call risk.mine");
		LastStatus = OutError;
		return false;
	}
	bool bValid = IsCacheValid(*QueryDB, Inputs);
	FString StoredRoot, StoredAt;
	int32 StoredRows = 0;
	{
		FSQLitePreparedStatement Meta;
		bValid = bValid && Meta.Create(*QueryDB, TEXT("SELECT project_root,snapshot_at,complexity_rows FROM risk_snapshot LIMIT 1;"))
			&& Meta.Step() == ESQLitePreparedStatementStepResult::Row;
		if (bValid)
		{
			bValid = Meta.GetColumnValueByIndex(0, StoredRoot)
				&& Meta.GetColumnValueByIndex(1, StoredAt)
				&& Meta.GetColumnValueByIndex(2, StoredRows)
				&& StoredRoot == Inputs.ProjectRoot && !StoredAt.IsEmpty() && StoredRows >= 0;
		}
	}
	if (!bValid)
	{
		ClearPublished();
		OutError = TEXT("risk cache is stale or incomplete; call risk.mine");
		LastStatus = OutError;
		return false;
	}
	LastInputs = Inputs;
	LastInputs.SnapshotAt = StoredAt;
	PublishedComplexityRows = StoredRows;
	PublishedPath = Path;
	State = TEXT("done");
	LastStatus = TEXT("loaded completed risk snapshot from disk");
	return true;
}
