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
// SCOPE: core + execution layer. The `jobs` MCP namespace lives in Private/MonolithJobActions.*.
//
// EXECUTION LAYER (Faz 1, roadmap item 5) — two shapes, one registry, one pump:
//   1. BACKGROUND jobs (StartBackgroundJob): CPU-bound work with NO UObject access. The body
//      runs on its own FRunnableThread; progress and the final outcome are marshalled back to
//      the game thread (AsyncTask(ENamedThreads::GameThread, ...) + the shared pump) so every
//      observer of CompleteJob/FailJob/UpdateProgress sees a consistent registry.
//   2. SLICED jobs (StartSlicedJob): UObject-safe work cut into slices. Each slice is invoked
//      from the SAME FTSTicker pump that does retention — no second ticker is ever installed.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Function.h"

/** Background worker runnable. Defined in Private/MonolithJobManager.cpp; never exposed. */
class FMonolithJobWorker;

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

// =============================================================================
// Execution layer — what a unit of submitted work reports back.
// =============================================================================

/** What a job body (background) or one slice (sliced) reports back to the manager. */
enum class EMonolithJobOutcomeKind : uint8
{
	/**
	 * SLICED JOBS ONLY: this slice did part of the work, call me again on the next pump.
	 * A background body that returns Pending is a programming error and fails its job —
	 * a background body runs to completion, it is not resumed.
	 */
	Pending,

	/** Work finished successfully; Result carries the payload (may be null). */
	Complete,

	/** Work failed; ErrorMessage / ErrorCode carry the reason. */
	Error,

	/** Work observed a cancellation request and bailed out cooperatively. */
	Cancelled
};

/**
 * Return value of a job body / slice. Deliberately a value type: it is copied across the
 * thread boundary into the game-thread apply queue, so it must own everything it carries.
 */
struct FMonolithJobOutcome
{
	EMonolithJobOutcomeKind Kind = EMonolithJobOutcomeKind::Complete;

	/** Complete only. Handed straight to CompleteJob, so the write-once contract applies. */
	TSharedPtr<FJsonValue> Result;

	/** Error only. */
	FString ErrorMessage;
	int32 ErrorCode = -32603;

	/** SLICED only — "more to do, call me again". */
	static FMonolithJobOutcome Pending()
	{
		FMonolithJobOutcome Out;
		Out.Kind = EMonolithJobOutcomeKind::Pending;
		return Out;
	}

	static FMonolithJobOutcome Complete(const TSharedPtr<FJsonValue>& InResult = nullptr)
	{
		FMonolithJobOutcome Out;
		Out.Kind = EMonolithJobOutcomeKind::Complete;
		Out.Result = InResult;
		return Out;
	}

	/** Named Failure (not Error) so it cannot be confused with EMonolithJobOutcomeKind::Error. */
	static FMonolithJobOutcome Failure(const FString& InMessage, int32 InErrorCode = -32603)
	{
		FMonolithJobOutcome Out;
		Out.Kind = EMonolithJobOutcomeKind::Error;
		Out.ErrorMessage = InMessage;
		Out.ErrorCode = InErrorCode;
		return Out;
	}

	static FMonolithJobOutcome Cancelled()
	{
		FMonolithJobOutcome Out;
		Out.Kind = EMonolithJobOutcomeKind::Cancelled;
		return Out;
	}
};

/**
 * Handed to every job body and every slice. It is the ONLY thing the submitted work needs
 * to know about the manager: it hides both the job id bookkeeping and the marshalling rule.
 *
 * Copyable and cheap; never outlives the call it was passed to.
 */
struct MONOLITHCORE_API FMonolithJobContext
{
	explicit FMonolithJobContext(const FString& InJobId)
		: JobId(InJobId)
	{
	}

	/** The job this work belongs to. Useful for logging; the helpers below cover the rest. */
	FString JobId;

	/**
	 * Any thread. True once cancellation has been requested (jobs_cancel, or shutdown).
	 * Background bodies must poll this between chunks; sliced steps get it checked for them
	 * between slices but may also poll it inside a long slice.
	 */
	bool IsCancelRequested() const;

	/**
	 * Any thread. Publish a human-readable progress message.
	 * Called ON the game thread  -> applied immediately.
	 * Called from a worker thread -> queued and applied on the game thread, so a poller
	 * never observes a half-updated job. Progress on a finished job is silently dropped.
	 */
	void ReportProgress(const FString& Message) const;
};

/**
 * Body of a BACKGROUND job. Runs once, on its own FRunnableThread, to completion.
 * MUST NOT touch UObjects, the asset registry, or anything else that is game-thread-only.
 * Must return Complete / Error / Cancelled — never Pending.
 */
using FMonolithBackgroundJobBody = TFunction<FMonolithJobOutcome(const FMonolithJobContext&)>;

/**
 * One slice of a SLICED job. Invoked on the GAME THREAD from the shared pump, so UObject
 * access is safe. Do a bounded amount of work and return Pending to be called again, or a
 * terminal outcome to finish. Not called again after any terminal outcome, and not called
 * again once the job has been cancelled.
 */
using FMonolithSlicedJobStep = TFunction<FMonolithJobOutcome(const FMonolithJobContext&)>;

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
	 * GAME THREAD. Register a job AND start `Body` on a dedicated FRunnableThread.
	 * Returns the new job id, or an EMPTY string if the job could not be started
	 * (null body, or the manager is shutting down) — callers must check.
	 *
	 * The body runs off the game thread and MUST NOT touch UObjects. Its progress and its
	 * final outcome are marshalled back to the game thread before they are applied, so the
	 * registry only ever mutates from Running -> terminal on the game thread.
	 *
	 * Single-threaded platforms / `-nothreading`: FRunnableThread::Create returns a fake
	 * thread that is driven from the main tick, so a body that blocks would block the editor.
	 * Bodies must be written to make progress without waiting on the game thread.
	 *
	 * Cancellation is cooperative: jobs_cancel flips the job to Cancelled immediately and the
	 * body sees FMonolithJobContext::IsCancelRequested() at its next checkpoint. A late
	 * outcome from a cancelled body is rejected by the write-once terminal rule.
	 */
	FString StartBackgroundJob(
		const FString& Namespace,
		const FString& Action,
		FMonolithBackgroundJobBody Body,
		const FString& InitialMessage = FString());

	/**
	 * GAME THREAD. Register a job whose `Step` is invoked once per pump iteration ON THE
	 * GAME THREAD until it returns a terminal outcome. UObject access is safe inside a slice.
	 * Returns the new job id, or an EMPTY string when the step is null / the manager is
	 * shutting down.
	 *
	 * The first slice runs on the NEXT pump iteration, never inline — so the caller can
	 * return a job id to its client before any work happens.
	 *
	 * This reuses the manager's single shared pump; no extra ticker is created. Slice cadence
	 * comes from UMonolithSettings::JobSliceIntervalSeconds (0 = every frame) and is applied
	 * by upgrading the one installed ticker, never by adding a second one.
	 */
	FString StartSlicedJob(
		const FString& Namespace,
		const FString& Action,
		FMonolithSlicedJobStep Step,
		const FString& InitialMessage = FString());

	/**
	 * Any thread EXCEPT the job's own worker thread. Blocks until the background body of
	 * JobId has RETURNED and queued its outcome. Returns false only on timeout; an id with
	 * no live background worker (unknown, sliced, or already reaped) returns true at once.
	 *
	 * This does NOT run the game-thread apply step: after it returns, call PumpOnce() (or let
	 * the pump tick) before reading the terminal state. Exists so automation can synchronise
	 * on a real event instead of sleeping; production code should poll the job instead.
	 *
	 * Do not call this from the game thread while the body is waiting on the game thread.
	 */
	bool WaitForBackgroundJob(const FString& JobId, double TimeoutSeconds);

	/** Any thread. Number of background worker threads the manager is currently tracking. */
	int32 GetBackgroundWorkerCount() const;

	/** Any thread. Number of tick-sliced jobs still registered with the pump. */
	int32 GetSlicedJobCount() const;

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
	 * GAME THREAD. Run one pump iteration synchronously: apply queued worker results, advance
	 * every tick-sliced job by one slice, reap finished worker threads, sweep retention.
	 * Exists so headless automation can drive the pump deterministically instead of waiting
	 * for engine ticks. Unlike the ticker callback it never uninstalls the pump, so calling it
	 * can never desynchronise the installed ticker. Returns true while work remains.
	 */
	bool PumpOnce();

	/**
	 * GAME THREAD. Full teardown: stop handing out slices, request cancellation of every
	 * running job, WAIT (bounded by UMonolithSettings::JobShutdownWaitSeconds) for every
	 * in-flight background body to return, join and delete its thread, discard queued
	 * game-thread work, drop every job and uninstall the pump.
	 *
	 * A body that ignores cancellation and outlives the wait is DETACHED, never force-killed:
	 * its FRunnable is moved to a list that is intentionally never freed, so the still-running
	 * thread can never touch freed memory, and editor shutdown is never blocked indefinitely.
	 *
	 * Called from FMonolithCoreModule::ShutdownModule. Tests must NOT call it — the registry
	 * is shared, and Reset() would drop jobs owned by everything else in the session.
	 */
	void Reset();

private:
	FMonolithJobManager() = default;
	~FMonolithJobManager();
	FMonolithJobManager(const FMonolithJobManager&) = delete;
	FMonolithJobManager& operator=(const FMonolithJobManager&) = delete;

	/** The background worker runnable — defined in the .cpp, never exposed. */
	friend class FMonolithJobWorker;

	/** The context needs the game-thread hand-off queue to marshal worker progress. */
	friend struct FMonolithJobContext;

	/** FTSTicker callback. Runs one pump iteration and self-unregisters once idle. */
	bool OnPump(float DeltaTime);

	/**
	 * Installs the pump if not already installed, or upgrades the installed ticker to the
	 * faster slice cadence when bWantsSliceCadence is set. Exactly ONE ticker exists at any
	 * moment: the upgrade removes the old handle before adding the new one.
	 * MUST be called with JobsLock NOT held, and never from inside OnPump.
	 */
	void EnsurePump(bool bWantsSliceCadence);

	/** Shared helper: finish transition under an already-held lock. */
	bool FinishJobInternal(
		const FString& JobId,
		EMonolithJobState NewState,
		const TSharedPtr<FJsonValue>& Result,
		const FString& ErrorMessage,
		int32 ErrorCode);

	/** Any thread. Queue work for the game thread and ask the task graph to drain it. */
	void EnqueueGameThreadWork(TFunction<void()>&& Work);

	/** GAME THREAD. Run everything queued by EnqueueGameThreadWork. Re-entrancy safe. */
	void DrainGameThreadWork();

	/** GAME THREAD. Turn a body/slice outcome into the matching registry transition. */
	void ApplyOutcome(const FString& JobId, const FMonolithJobOutcome& Outcome);

	/** GAME THREAD. Invoke one slice of every registered sliced job, in registration order. */
	void AdvanceSlicedJobs();

	/** GAME THREAD. Join + delete the threads of workers whose body has returned. */
	void ReapFinishedWorkers();

	/** GAME THREAD. Bounded wait for every worker, then join or detach. Used by Reset(). */
	void ShutdownWorkers();

	/** Any thread. True while jobs, sliced steps, workers or queued results remain. */
	bool HasWorkRemaining() const;

	TMap<FString, FMonolithJob> Jobs;
	uint64 NextJobSerial = 1;

	FTSTicker::FDelegateHandle PumpHandle;
	bool bPumpActive = false;
	/** Interval the currently installed ticker was created with; < 0 when none is installed. */
	float InstalledPumpInterval = -1.0f;

	mutable FCriticalSection JobsLock;

	/** One registered sliced job. An array (not a map) so slices run in registration order. */
	struct FSlicedJobEntry
	{
		FString JobId;
		/** Shared so a snapshot can outlive concurrent registry edits while the slice runs. */
		TSharedPtr<FMonolithSlicedJobStep> Step;
	};
	TArray<FSlicedJobEntry> SlicedJobs;
	mutable FCriticalSection SlicedLock;

	/** Live background workers, and the ones deliberately leaked at shutdown (see Reset). */
	TArray<TSharedPtr<FMonolithJobWorker>> Workers;
	TArray<TSharedPtr<FMonolithJobWorker>> DetachedWorkers;
	mutable FCriticalSection WorkersLock;

	/** Worker -> game thread hand-off queue. */
	TArray<TFunction<void()>> PendingGameThreadWork;
	bool bDrainingGameThreadWork = false;
	mutable FCriticalSection GameThreadWorkLock;

	/** Set for the duration of Reset(): refuses new work and stops task-graph scheduling. */
	FThreadSafeBool bShuttingDown;

	/** True while OnPump is executing, so EnsurePump never re-installs from inside the tick. */
	FThreadSafeBool bInPump;
};
