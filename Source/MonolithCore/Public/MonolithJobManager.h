// SPDX-License-Identifier: MIT
// Faz 1 — Job System core. See Docs/GOLDENSTONE_ROADMAP.md "Faz 1".
//
// Every Monolith action currently runs synchronously on the editor game thread, so a
// long operation freezes both the editor and the MCP HTTP server. FMonolithJobManager is
// the generalisation of FPieSmokeSessionManager
// (Source/MonolithEditor/Private/MonolithPieSmokeSession.h): a process-lifetime registry
// of asynchronous units of work plus ONE shared FTSTicker pump, so an action can hand
// long work off, return a job id immediately, and let the caller poll.
//
// Difference from FPieSmokeSessionManager: that manager is game-thread-only and needs no
// locking because both its MCP handlers and its observer run on the game thread. Jobs
// here are expected to be advanced from worker threads (FRunnableThread / Async), so the
// registry is guarded by an FCriticalSection — the same primitive FMonolithToolRegistry
// uses — and every accessor copies data OUT rather than handing out pointers into the map.
//
// SCOPE: core only. No action bindings / `jobs` namespace here (separate step).

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"

/** Lifecycle state of a job. Terminal states are Complete / Error / Cancelled. */
enum class EMonolithJobState : uint8
{
	Running,
	Complete,
	Error,
	Cancelled
};

/** Stable lowercase token for a job state (wire/report friendly). */
MONOLITHCORE_API const TCHAR* LexMonolithJobState(EMonolithJobState State);

/**
 * One unit of asynchronous work.
 *
 * Copyable by value on purpose: FMonolithJobManager never hands out a pointer into its
 * registry, it hands out snapshots, so a poller can read a consistent job even while a
 * worker thread mutates the live entry.
 *
 * PAYLOAD OWNERSHIP CONTRACT: `Result` is written exactly once, by CompleteJob. The
 * producer must not mutate the pointed-to FJsonValue after handing it over — snapshots
 * share the same reference, and FJsonValue itself is not synchronised.
 */
struct FMonolithJob
{
	/** Registry key, monotonically issued ("job_1", "job_2", ...). */
	FString Id;

	/** Monotonic issue order. Used for stable listing + oldest-first retention eviction. */
	uint64 Serial = 0;

	/** Originating namespace + action, purely descriptive (e.g. "animation" / "build_search_index"). */
	FString Namespace;
	FString Action;

	EMonolithJobState State = EMonolithJobState::Running;

	/** Human-readable progress text; may be replaced any number of times while Running. */
	FString ProgressMessage;

	/** Result payload, set on the Complete transition. Null in every other state. */
	TSharedPtr<FJsonValue> Result;

	/** Error info, set on the Error transition. ErrorCode follows JSON-RPC conventions. */
	FString ErrorMessage;
	int32 ErrorCode = 0;

	/**
	 * Cooperative cancellation flag. Set by CancelJob; long-running workers are expected
	 * to poll FMonolithJobManager::IsCancelRequested and bail out. Nothing in the manager
	 * kills a worker — cancellation is advisory to the producer and immediate to the state.
	 */
	bool bCancelRequested = false;

	/** FPlatformTime::Seconds() stamps. FinishedTimeSeconds stays < 0 while Running. */
	double CreatedTimeSeconds = 0.0;
	double UpdatedTimeSeconds = 0.0;
	double FinishedTimeSeconds = -1.0;

	bool IsFinished() const { return State != EMonolithJobState::Running; }
};

/**
 * Process-lifetime registry of jobs plus the single shared FTSTicker pump.
 * Meyers singleton, mirroring FPieSmokeSessionManager::Get().
 *
 * THREADING CONTRACT (per method, documented at each declaration):
 *   - Every public method is safe to call from ANY thread unless stated otherwise; the
 *     registry is guarded by JobsLock and callers only ever receive copies.
 *   - The lock is never held while calling into FTSTicker, so there is no lock inversion
 *     between the pump and a producer thread.
 *   - The lock is never held across user code — the manager owns no callbacks.
 */
class MONOLITHCORE_API FMonolithJobManager
{
public:
	/** Any thread. */
	static FMonolithJobManager& Get();

	// --- Producer side -----------------------------------------------------------

	/**
	 * Any thread. Register a new Running job and return its id. Installs the shared pump
	 * if it is not already running (FTSTicker is thread-safe; the pump callback itself
	 * fires on the game thread).
	 */
	FString CreateJob(const FString& Namespace, const FString& Action, const FString& InitialMessage = FString());

	/**
	 * Any thread. Replace the progress message of a Running job.
	 * Returns false when the job is unknown or already finished.
	 */
	bool UpdateProgress(const FString& JobId, const FString& Message);

	/**
	 * Any thread. Running -> Complete, storing the result payload.
	 * Returns false when the job is unknown or already finished — in particular a
	 * CANCELLED job is never resurrected by a late completion.
	 */
	bool CompleteJob(const FString& JobId, const TSharedPtr<FJsonValue>& Result);

	/**
	 * Any thread. Running -> Error. Returns false when the job is unknown or finished.
	 * ErrorCode defaults to the JSON-RPC internal-error code used across the action layer.
	 */
	bool FailJob(const FString& JobId, const FString& ErrorMessage, int32 ErrorCode = -32603);

	/**
	 * Any thread. Running -> Cancelled and raises the cooperative cancel flag.
	 * Returns false when the job is unknown or already finished.
	 */
	bool CancelJob(const FString& JobId);

	/**
	 * Any thread. True when cancellation has been requested for this job. Workers poll
	 * this between slices. Unknown ids return false.
	 */
	bool IsCancelRequested(const FString& JobId) const;

	// --- Consumer side -----------------------------------------------------------

	/**
	 * Any thread. Copy a job snapshot out of the registry.
	 * Returns false (and leaves OutJob untouched) for an unknown id — polling an unknown
	 * job is a clean, non-crashing failure by design.
	 */
	bool GetJob(const FString& JobId, FMonolithJob& OutJob) const;

	/** Any thread. Snapshots of every job, oldest-issued first. */
	TArray<FMonolithJob> GetAllJobs() const;

	/** Any thread. Number of jobs currently in the Running state. */
	int32 GetRunningJobCount() const;

	/** Any thread. Total number of jobs held (running + retained finished). */
	int32 GetJobCount() const;

	// --- Lifetime management -----------------------------------------------------

	/**
	 * Any thread. Explicitly drop a job regardless of state. Returns true if one was
	 * removed. Removing a Running job orphans its worker — the worker's later
	 * Update/Complete calls then simply return false.
	 */
	bool RemoveJob(const FString& JobId);

	/** Any thread. Drop every finished job. Returns how many were removed. */
	int32 ClearFinishedJobs();

	/**
	 * Any thread. Apply the retention policy for finished jobs, driven by
	 * UMonolithSettings::JobRetentionCount (bounded count, oldest finished evicted first)
	 * and UMonolithSettings::JobRetentionSeconds (max age since finishing; 0 disables the
	 * age rule). Running jobs are never evicted. Returns how many were removed.
	 *
	 * @param NowSecondsOverride  < 0 (default) uses FPlatformTime::Seconds(). Tests pass an
	 *                            explicit clock value so age eviction is deterministic and
	 *                            needs no wall-clock sleep.
	 */
	int32 EnforceRetention(double NowSecondsOverride = -1.0);

	/**
	 * Any thread. Run one pump iteration synchronously (retention sweep). Exists so
	 * headless automation can drive the pump deterministically instead of waiting for
	 * engine ticks. Unlike the ticker callback it never uninstalls the pump, so calling it
	 * can never desynchronise the installed ticker. Returns true while jobs remain.
	 */
	bool PumpOnce();

	/**
	 * Game thread. Drop every job and uninstall the pump. Called from module shutdown and
	 * by tests that need a clean registry.
	 */
	void Reset();

private:
	FMonolithJobManager() = default;
	~FMonolithJobManager() = default;
	FMonolithJobManager(const FMonolithJobManager&) = delete;
	FMonolithJobManager& operator=(const FMonolithJobManager&) = delete;

	/** FTSTicker callback. Enforces retention and self-unregisters once the registry empties. */
	bool OnPump(float DeltaTime);

	/** Installs the pump if not already installed. MUST be called with JobsLock NOT held. */
	void EnsurePump();

	/** Shared helper: finish transition under an already-held lock. */
	bool FinishJobInternal(
		const FString& JobId,
		EMonolithJobState NewState,
		const TSharedPtr<FJsonValue>& Result,
		const FString& ErrorMessage,
		int32 ErrorCode);

	TMap<FString, FMonolithJob> Jobs;
	uint64 NextJobSerial = 1;

	FTSTicker::FDelegateHandle PumpHandle;
	bool bPumpActive = false;

	mutable FCriticalSection JobsLock;
};
