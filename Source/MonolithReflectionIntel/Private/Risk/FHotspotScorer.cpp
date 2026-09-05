// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 2 — v0.17.0).
//
// FHotspotScorer — joins git_file_churn × files.line_count → risk_hotspot_scores.
// Pattern: 1) collect churn (LEFT side of join), 2) collect complexity proxy
// from the existing `files` table populated by MonolithSourceSubsystem, 3)
// normalise both signals 0..1, 4) multiply for the composite score.
//
// The `files` table schema (per `MonolithSourceSchema.h:29-37`):
//   files(id INTEGER PK, path TEXT UNIQUE, module_id INTEGER,
//         file_type TEXT, line_count INTEGER, last_modified REAL)
//
// `path` is stored as the absolute filesystem path of the C++ source file
// (e.g. `<ProjectDir>/Plugins/<Name>/Source/...`), which this file relativises
// against the project root. `git_file_churn.file_path` is ALSO project-relative
// forward-slashed — `git log` emits repo-relative paths and FGitCoChangeIndexer
// rebases them into the project-relative space before writing (see its header's
// PATH SPACE note). Both sides therefore key on the same shape, which is what
// makes this join produce a non-zero score at all.

#include "Risk/FHotspotScorer.h"
#include "Risk/RiskSchema.h"
#include "Risk/FRiskMiningWorkerContext.h"
#include "MonolithReflectionIntelModule.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

namespace
{
	// NOTE: Deliberately NOT the shared RIToProjectRelative (Private/Shared/
	// RIPathUtils.h). This variant operates on the RAW input path and does NOT
	// call FPaths::ConvertRelativePathToFull — the hotspot join needs the path
	// shapes left as the DB / git-churn rows stored them, not resolved to full
	// absolute paths. Unifying with the shared helper would change behaviour.
	// File-unique name clears the unity-build collision with the three indexers.
	/** Convert any input path to project-relative forward-slashed form (raw, no full-path resolve). */
	FString HotspotToProjectRelative(const FString& Path, const FString& ProjectRoot)
	{
		FString Full = Path;
		Full.ReplaceInline(TEXT("\\"), TEXT("/"));
		FString Root = ProjectRoot;
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!Root.EndsWith(TEXT("/"))) Root += TEXT("/");

		if (Full.StartsWith(Root, ESearchCase::IgnoreCase))
		{
			FString Rel = Full.Mid(Root.Len());
			while (!Rel.IsEmpty() && Rel[0] == TEXT('/')) { Rel.RightChopInline(1); }
			return Rel;
		}
		// Not under project root — keep as-is (forward-slashed).
		return Full;
	}
}

bool FHotspotScorer::Run(FSQLiteDatabase& DB, FString& OutStatus)
{
	if (!ensure(IsInGameThread())) return false;
	FRiskMiningWorkerContext Context;
	Context.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	return RunInternal(DB, Context, OutStatus);
}

bool FHotspotScorer::RunOwnedDatabase(FSQLiteDatabase& DB, const FRiskMiningWorkerContext& Context, FString& OutStatus)
{
	if (!ensure(!IsInGameThread())) return false;
	if (Context.ProjectRoot.IsEmpty() || FPaths::IsRelative(Context.ProjectRoot))
	{ OutStatus = TEXT("HotspotScorer: captured absolute project root is required"); return false; }
	return RunInternal(DB, Context, OutStatus);
}

bool FHotspotScorer::RunInternal(FSQLiteDatabase& DB, const FRiskMiningWorkerContext& Context, FString& OutStatus)
{
	if (Context.IsCancelled()) { OutStatus = TEXT("Risk mining cancelled"); return false; }

	if (!EnsureSchema(DB))
	{
		OutStatus = TEXT("FHotspotScorer: schema bootstrap failed");
		UE_LOG(LogMonolithReflectionIntel, Error, TEXT("%s"), *OutStatus);
		return false;
	}

	// Wipe — Phase 2 §3 wipe-and-rewrite policy.
	{
		FSQLitePreparedStatement Del;
		if (!Del.Create(DB, TEXT("DELETE FROM risk_hotspot_scores;")) || !Del.Execute())
		{ OutStatus = TEXT("HotspotScorer: could not clear scores"); return false; }
	}

	TMap<FString, FFileSignals> Signals;

	if (!LoadChurn(Context, DB, Signals) || !LoadComplexity(Context, DB, Signals))
	{
		OutStatus = Context.IsCancelled() ? TEXT("HotspotScorer: cancelled") : TEXT("HotspotScorer: signal read failed");
		return false;
	}

	if (!WriteScores(Context, DB, Signals))
	{
		OutStatus = TEXT("FHotspotScorer: write failed");
		return false;
	}

	OutStatus = FString::Printf(TEXT("HotspotScorer: %d scored files"), Signals.Num());
	UE_LOG(LogMonolithReflectionIntel, Log, TEXT("%s"), *OutStatus);
	return true;
}

bool FHotspotScorer::EnsureSchema(FSQLiteDatabase& DB)
{
	auto Exec = [&DB](const TCHAR* Sql) -> bool
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(DB, Sql))
		{
			UE_LOG(LogMonolithReflectionIntel, Error,
				TEXT("HotspotScorer DDL prepare failed: %s"), Sql);
			return false;
		}
		return Stmt.Execute();
	};
	if (!Exec(MonolithRiskSchema::GetCreateHotspotScoresTableSQL())) { return false; }
	return Exec(MonolithRiskSchema::GetCreateHotspotScoresIndexScoreSQL());
}

bool FHotspotScorer::LoadChurn(const FRiskMiningWorkerContext& Context, FSQLiteDatabase& DB, TMap<FString, FFileSignals>& InOut)
{
	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(DB, TEXT(
		"SELECT file_path, SUM(commit_count) FROM git_file_churn GROUP BY file_path;")))
	{
		// Missing inputs or SQL failure must not publish a complete-looking snapshot.
		return false;
	}
	ESQLitePreparedStatementStepResult Step;
	while ((Step = Stmt.Step()) == ESQLitePreparedStatementStepResult::Row)
	{
		if (Context.IsCancelled()) return false;
		FString Path;
		int32 Sum = 0;
		if (!Stmt.GetColumnValueByIndex(0, Path) || !Stmt.GetColumnValueByIndex(1, Sum)) return false;
		FFileSignals& S = InOut.FindOrAdd(Path);
		S.ChurnCount = Sum;
	}
	return Step == ESQLitePreparedStatementStepResult::Done && !Context.IsCancelled();
}

bool FHotspotScorer::LoadComplexity(const FRiskMiningWorkerContext& Context, FSQLiteDatabase& DB, TMap<FString, FFileSignals>& InOut)
{
	// Only join against .cpp / .h / .hpp / .inl source files — not binary
	// artefacts. The `files.file_type` column is the indexer's classification.
	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(DB, TEXT(
		"SELECT path, line_count FROM files "
		"WHERE file_type IN ('cpp','hpp','h','inl','c','hh','cxx','cc');")))
	{
		return false;
	}

	const FString& ProjectRoot = Context.ProjectRoot;

	ESQLitePreparedStatementStepResult Step;
	while ((Step = Stmt.Step()) == ESQLitePreparedStatementStepResult::Row)
	{
		if (Context.IsCancelled()) return false;
		FString AbsPath;
		int32 LineCount = 0;
		if (!Stmt.GetColumnValueByIndex(0, AbsPath) || !Stmt.GetColumnValueByIndex(1, LineCount)) return false;

		const FString Rel = HotspotToProjectRelative(AbsPath, ProjectRoot);
		// Only retain rows under the project tree — engine files have no
		// project-relative path and we don't score engine churn.
		if (Rel.IsEmpty() || Rel.StartsWith(TEXT("/"))) { continue; }
		// "Not under project root" returns the input path unchanged; skip
		// those — they're engine / outside-tree files.
		if (Rel.Contains(TEXT(":/")) || Rel.Contains(TEXT(":"))) { continue; }

		FFileSignals& S = InOut.FindOrAdd(Rel);
		S.ComplexityProxy = LineCount;
	}
	return Step == ESQLitePreparedStatementStepResult::Done && !Context.IsCancelled();
}

bool FHotspotScorer::WriteScores(const FRiskMiningWorkerContext& Context, FSQLiteDatabase& DB, const TMap<FString, FFileSignals>& Signals)
{
	// Normalise both signals — find max for each then divide. Composite =
	// product of the two normalised components, range [0,1].
	int32 MaxChurn = 0;
	int32 MaxComplexity = 0;
	for (const TPair<FString, FFileSignals>& E : Signals)
	{
		if (Context.IsCancelled()) return false;
		MaxChurn = FMath::Max(MaxChurn, E.Value.ChurnCount);
		MaxComplexity = FMath::Max(MaxComplexity, E.Value.ComplexityProxy);
	}
	const double InvMaxChurn = (MaxChurn > 0) ? (1.0 / static_cast<double>(MaxChurn)) : 0.0;
	const double InvMaxCmplx = (MaxComplexity > 0) ? (1.0 / static_cast<double>(MaxComplexity)) : 0.0;

	FSQLitePreparedStatement Ins;
	if (!Ins.Create(DB, TEXT(
		"INSERT OR REPLACE INTO risk_hotspot_scores "
		"(file_path, churn, complexity_proxy, normalised_churn, "
		" normalised_complexity, score) VALUES (?, ?, ?, ?, ?, ?);")))
	{
		UE_LOG(LogMonolithReflectionIntel, Error,
			TEXT("HotspotScorer: INSERT prepare failed"));
		return false;
	}

	if (!DB.Execute(TEXT("SAVEPOINT risk_hotspots;"))) return false;
	int32 OkCount = 0;
	for (const TPair<FString, FFileSignals>& E : Signals)
	{
		if (Context.IsCancelled()) break;
		const double NormChurn = static_cast<double>(E.Value.ChurnCount) * InvMaxChurn;
		const double NormCmplx = static_cast<double>(E.Value.ComplexityProxy) * InvMaxCmplx;
		const double Score = NormChurn * NormCmplx;

		Ins.Reset();
		Ins.ClearBindings();
		if (Ins.SetBindingValueByIndex(1, E.Key)
			&& Ins.SetBindingValueByIndex(2, E.Value.ChurnCount)
			&& Ins.SetBindingValueByIndex(3, E.Value.ComplexityProxy)
			&& Ins.SetBindingValueByIndex(4, NormChurn)
			&& Ins.SetBindingValueByIndex(5, NormCmplx)
			&& Ins.SetBindingValueByIndex(6, Score)
			&& Ins.Execute()) { ++OkCount; }
		else
		{
			UE_LOG(LogMonolithReflectionIntel, Verbose,
				TEXT("HotspotScorer: INSERT failed for %s"), *E.Key);
		}
	}
	Ins.Destroy();
	if (OkCount != Signals.Num() || Context.IsCancelled())
	{
		DB.Execute(TEXT("ROLLBACK TO risk_hotspots;"));
		DB.Execute(TEXT("RELEASE risk_hotspots;"));
		return false;
	}
	if (!DB.Execute(TEXT("RELEASE risk_hotspots;"))) return false;

	UE_LOG(LogMonolithReflectionIntel, Verbose,
		TEXT("HotspotScorer: wrote %d / %d rows (max_churn=%d max_complexity=%d)"),
		OkCount, Signals.Num(), MaxChurn, MaxComplexity);
	return OkCount == Signals.Num();
}
