// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 2 — v0.17.0).
//
// FHotspotScorer — composite churn × complexity scorer. Reads `git_file_churn`
// (Phase 2 GitCoChangeIndexer output) and the existing `files` table (Monolith
// source indexer — `line_count` is the complexity proxy in Phase 2) and writes
// the join into `risk_hotspot_scores`. Plain C++ worker. Idempotent: wipes +
// rewrites the table on each Run().
//
// Threading: Run is game-thread-only. RunOwnedDatabase operates exclusively
// on a worker-owned database using captured project inputs.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Map.h"

class FSQLiteDatabase;
struct FRiskMiningWorkerContext;

class MONOLITHREFLECTIONINTEL_API FHotspotScorer
{
public:
	/**
	 * Compute hotspot scores joining churn × complexity for every file with
	 * both signals. Writes `risk_hotspot_scores`. Returns true on full success.
	 *
	 * @param DB         Open writable handle. Caller has enforced
	 *                   `PRAGMA journal_mode=DELETE`.
	 * @param OutStatus  One-line summary.
	 */
	bool Run(FSQLiteDatabase& DB, FString& OutStatus);

	/** Worker-only entry: DB must be exclusively owned by this worker; inputs are captured on the game thread. */
	bool RunOwnedDatabase(FSQLiteDatabase& DB, const FRiskMiningWorkerContext& Context, FString& OutStatus);

private:
	bool RunInternal(FSQLiteDatabase& DB, const FRiskMiningWorkerContext& Context, FString& OutStatus);

	bool EnsureSchema(FSQLiteDatabase& DB);

	struct FFileSignals
	{
		int32 ChurnCount = 0;
		int32 ComplexityProxy = 0; // line_count from MonolithSource.files
	};

	bool LoadChurn(const FRiskMiningWorkerContext& Context, FSQLiteDatabase& DB, TMap<FString, FFileSignals>& InOut);
	bool LoadComplexity(const FRiskMiningWorkerContext& Context, FSQLiteDatabase& DB, TMap<FString, FFileSignals>& InOut);
	bool WriteScores(const FRiskMiningWorkerContext& Context, FSQLiteDatabase& DB, const TMap<FString, FFileSignals>& Signals);
};
