// SPDX-License-Identifier: MIT
// Faz 1 — Job System core. Header: Public/MonolithJobManager.h

#include "MonolithJobManager.h"

#include "MonolithJsonUtils.h"   // LogMonolith
#include "MonolithSettings.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"

namespace MonolithJobManagerDetail
{
	// Fallback policy used only when the settings CDO is unavailable (very early module init /
	// stripped builds). The authoritative values live on UMonolithSettings — these mirror its
	// declared defaults so behaviour does not change silently.
	static constexpr int32 FallbackRetentionCount = 64;
	static constexpr double FallbackRetentionSeconds = 900.0;
	static constexpr float FallbackPumpIntervalSeconds = 1.0f;
	static constexpr float FallbackSliceIntervalSeconds = 0.0f;
	static constexpr float FallbackShutdownWaitSeconds = 5.0f;

	/** Retention/upkeep cadence of the shared pump when no sliced job is registered. */
	static float GetRetentionPumpInterval()
	{
		const UMonolithSettings* Settings = UMonolithSettings::Get();
		return Settings ? FMath::Max(0.0f, Settings->JobPumpIntervalSeconds) : FallbackPumpIntervalSeconds;
	}

	/** Cadence the shared pump switches to while at least one sliced job is registered. */
	static float GetSlicePumpInterval()
	{
		const UMonolithSettings* Settings = UMonolithSettings::Get();
		return Settings ? FMath::Max(0.0f, Settings->JobSliceIntervalSeconds) : FallbackSliceIntervalSeconds;
	}

	/** How long Reset() waits for one in-flight background body before detaching it. */
	static double GetShutdownWaitSeconds()
	{
		const UMonolithSettings* Settings = UMonolithSettings::Get();
		return Settings
			? FMath::Max(0.0, static_cast<double>(Settings->JobShutdownWaitSeconds))
			: static_cast<double>(FallbackShutdownWaitSeconds);
	}

	/** Clamp a seconds value into the millisecond wait FEvent takes (0 stays 0 = poll). */
	static uint32 ToWaitMilliseconds(double Seconds)
	{
		const double Ms = FMath::Clamp(Seconds * 1000.0, 0.0, static_cast<double>(MAX_uint32 - 1));
		return static_cast<uint32>(Ms);
	}
}

// -----------------------------------------------------------------------------
// FMonolithJobWorker — one background job body on one FRunnableThread.
//
// LIFETIME: held by TSharedPtr. The manager owns the reference in FMonolithJobManager::Workers
// and drops it only after the thread has been joined (ReapFinishedWorkers / ShutdownWorkers),
// or moves it into DetachedWorkers, which is never freed. Either way the FRunnable outlives
// its thread, so the thread can never touch freed memory.
//
// The Run() body captures NOTHING by pointer except FMonolithJobManager::Get(), a Meyers
// singleton with process lifetime, so a late-finishing body is safe even after Reset().
// -----------------------------------------------------------------------------
class FMonolithJobWorker : public FRunnable
{
public:
	FMonolithJobWorker(const FString& InJobId, FMonolithBackgroundJobBody&& InBody)
		: JobId(InJobId)
		, Body(MoveTemp(InBody))
	{
		// Manual reset: every later waiter (Reset(), WaitForBackgroundJob) must see the
		// signal, not just the first one.
		DoneEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/true);
	}

	virtual ~FMonolithJobWorker()
	{
		// Thread is deleted by whoever joined it (the manager). Reaching the destructor with
		// a live Thread would mean nobody joined — deliberately leak rather than delete a
		// running thread out from under itself.
		if (Thread != nullptr)
		{
			UE_LOG(LogMonolith, Warning, TEXT("Job %s: worker destroyed without being joined; leaking its thread object."), *JobId);
			Thread = nullptr;
		}
		if (DoneEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
			DoneEvent = nullptr;
		}
	}

	virtual uint32 Run() override
	{
		FMonolithJobManager& Manager = FMonolithJobManager::Get();

		const FMonolithJobContext Context(JobId);
		FMonolithJobOutcome Outcome = Body
			? Body(Context)
			: FMonolithJobOutcome::Failure(TEXT("Background job started with an empty body."));

		if (Outcome.Kind == EMonolithJobOutcomeKind::Pending)
		{
			// Pending is a sliced-job concept; a background body runs once and is not resumed.
			Outcome = FMonolithJobOutcome::Failure(
				TEXT("Background job body returned Pending. Only tick-sliced jobs may report partial progress."));
		}

		// ORDER MATTERS: queue the outcome BEFORE signalling, so anything that wakes on
		// DoneEvent is guaranteed to find the result already queued for the game thread.
		Manager.EnqueueGameThreadWork([JobIdCopy = JobId, OutcomeCopy = MoveTemp(Outcome)]()
		{
			FMonolithJobManager::Get().ApplyOutcome(JobIdCopy, OutcomeCopy);
		});

		bBodyReturned = true;
		DoneEvent->Trigger();
		return 0;
	}

	/** Called by FRunnableThread::Kill. Cancellation is cooperative — the flag is on the job. */
	virtual void Stop() override
	{
		bStopRequested = true;
	}

	FString JobId;
	FMonolithBackgroundJobBody Body;

	/** Owned by the manager, deleted only after the body returned (or leaked on detach). */
	FRunnableThread* Thread = nullptr;

	FEvent* DoneEvent = nullptr;
	FThreadSafeBool bBodyReturned;
	FThreadSafeBool bStopRequested;
};

const TCHAR* LexMonolithJobState(EMonolithJobState State)
{
	switch (State)
	{
	case EMonolithJobState::Running:   return TEXT("running");
	case EMonolithJobState::Complete:  return TEXT("complete");
	case EMonolithJobState::Error:     return TEXT("error");
	case EMonolithJobState::Cancelled: return TEXT("cancelled");
	default:                           return TEXT("unknown");
	}
}

FMonolithJobManager& FMonolithJobManager::Get()
{
	static FMonolithJobManager Instance;
	return Instance;
}

// Out of line because Workers/DetachedWorkers hold TSharedPtr<FMonolithJobWorker>, which is
// only complete in this translation unit.
FMonolithJobManager::~FMonolithJobManager() = default;

// -----------------------------------------------------------------------------
// FMonolithJobContext — the only surface submitted work talks to.
// -----------------------------------------------------------------------------

bool FMonolithJobContext::IsCancelRequested() const
{
	return FMonolithJobManager::Get().IsCancelRequested(JobId);
}

void FMonolithJobContext::ReportProgress(const FString& Message) const
{
	FMonolithJobManager& Manager = FMonolithJobManager::Get();

	if (IsInGameThread())
	{
		// Sliced jobs (and game-thread callers generally) apply straight away, so a caller
		// that reports progress and then polls sees its own update.
		Manager.UpdateProgress(JobId, Message);
		return;
	}

	// Worker thread: marshal, so the registry only ever mutates on the game thread while a
	// background body is running.
	Manager.EnqueueGameThreadWork([JobIdCopy = JobId, MessageCopy = Message]()
	{
		FMonolithJobManager::Get().UpdateProgress(JobIdCopy, MessageCopy);
	});
}

// -----------------------------------------------------------------------------
// Producer side
// -----------------------------------------------------------------------------

FString FMonolithJobManager::CreateJob(const FString& Namespace, const FString& Action, const FString& InitialMessage)
{
	FString NewId;
	{
		FScopeLock Lock(&JobsLock);

		FMonolithJob Job;
		Job.Serial = NextJobSerial++;
		Job.Id = FString::Printf(TEXT("job_%llu"), Job.Serial);
		Job.Namespace = Namespace;
		Job.Action = Action;
		Job.State = EMonolithJobState::Running;
		Job.ProgressMessage = InitialMessage;
		Job.CreatedTimeSeconds = FPlatformTime::Seconds();
		Job.UpdatedTimeSeconds = Job.CreatedTimeSeconds;
		Job.FinishedTimeSeconds = -1.0;

		NewId = Job.Id;
		Jobs.Add(NewId, MoveTemp(Job));
	}

	// Installed outside the lock: FTSTicker takes its own lock, and taking it while
	// holding JobsLock would invert against the pump callback's JobsLock acquisition.
	EnsurePump(/*bWantsSliceCadence=*/false);

	UE_LOG(LogMonolith, Verbose, TEXT("Job %s created (%s.%s)"), *NewId, *Namespace, *Action);
	return NewId;
}

// -----------------------------------------------------------------------------
// Execution layer — background jobs
// -----------------------------------------------------------------------------

FString FMonolithJobManager::StartBackgroundJob(
	const FString& Namespace,
	const FString& Action,
	FMonolithBackgroundJobBody Body,
	const FString& InitialMessage)
{
	if (!Body)
	{
		UE_LOG(LogMonolith, Warning, TEXT("StartBackgroundJob(%s.%s) refused: null body."), *Namespace, *Action);
		return FString();
	}
	if (bShuttingDown)
	{
		UE_LOG(LogMonolith, Warning, TEXT("StartBackgroundJob(%s.%s) refused: job manager is shutting down."), *Namespace, *Action);
		return FString();
	}

	// Cheap upkeep: a previous job's thread may be sitting joinable already.
	ReapFinishedWorkers();

	const FString JobId = CreateJob(Namespace, Action, InitialMessage);

	TSharedPtr<FMonolithJobWorker> Worker = MakeShared<FMonolithJobWorker>(JobId, MoveTemp(Body));

	{
		// The thread is created UNDER WorkersLock so Worker->Thread is never observed null by
		// the reaper while the body is already running. Safe against deadlock because nothing
		// the body can call ever takes WorkersLock.
		FScopeLock Lock(&WorkersLock);
		Worker->Thread = FRunnableThread::Create(
			Worker.Get(),
			*FString::Printf(TEXT("MonolithJob_%s"), *JobId),
			0,
			TPri_BelowNormal);

		if (Worker->Thread != nullptr)
		{
			Workers.Add(Worker);
		}
	}

	if (Worker->Thread == nullptr)
	{
		// Platform refused a thread: fail cleanly instead of silently never finishing.
		FailJob(JobId, TEXT("Could not create a worker thread for this job."));
		UE_LOG(LogMonolith, Error, TEXT("Job %s: FRunnableThread::Create failed."), *JobId);
		return JobId;
	}

	UE_LOG(LogMonolith, Verbose, TEXT("Job %s started on a background thread (%s.%s)"), *JobId, *Namespace, *Action);
	return JobId;
}

bool FMonolithJobManager::WaitForBackgroundJob(const FString& JobId, double TimeoutSeconds)
{
	using namespace MonolithJobManagerDetail;

	TSharedPtr<FMonolithJobWorker> Worker;
	{
		FScopeLock Lock(&WorkersLock);
		for (const TSharedPtr<FMonolithJobWorker>& Candidate : Workers)
		{
			if (Candidate.IsValid() && Candidate->JobId == JobId)
			{
				Worker = Candidate;
				break;
			}
		}
	}

	if (!Worker.IsValid())
	{
		// No live worker: unknown id, a sliced job, or one already reaped — nothing to wait on.
		return true;
	}

	// The shared reference above keeps the runnable (and therefore DoneEvent) alive even if
	// the pump reaps the worker while we are blocked here.
	const bool bSignalled = Worker->DoneEvent->Wait(ToWaitMilliseconds(TimeoutSeconds));
	return bSignalled || Worker->bBodyReturned;
}

int32 FMonolithJobManager::GetBackgroundWorkerCount() const
{
	FScopeLock Lock(&WorkersLock);
	return Workers.Num();
}

void FMonolithJobManager::ReapFinishedWorkers()
{
	TArray<TSharedPtr<FMonolithJobWorker>> Reaped;
	{
		FScopeLock Lock(&WorkersLock);
		for (int32 Index = Workers.Num() - 1; Index >= 0; --Index)
		{
			const TSharedPtr<FMonolithJobWorker>& Worker = Workers[Index];
			if (Worker.IsValid() && Worker->bBodyReturned && Worker->Thread != nullptr)
			{
				Reaped.Add(Worker);
				Workers.RemoveAt(Index);
			}
		}
	}

	// Join outside the lock. The body has already returned, so WaitForCompletion returns as
	// soon as the thread proc unwinds; deleting afterwards is the codebase's existing idiom
	// (UMonolithIndexSubsystem::OnIndexingFinished).
	for (const TSharedPtr<FMonolithJobWorker>& Worker : Reaped)
	{
		Worker->Thread->WaitForCompletion();
		delete Worker->Thread;
		Worker->Thread = nullptr;
	}
}

void FMonolithJobManager::ShutdownWorkers()
{
	using namespace MonolithJobManagerDetail;

	TArray<TSharedPtr<FMonolithJobWorker>> Pending;
	{
		FScopeLock Lock(&WorkersLock);
		Pending = MoveTemp(Workers);
		Workers.Reset();
	}

	if (Pending.Num() == 0)
	{
		return;
	}

	const uint32 WaitMs = ToWaitMilliseconds(GetShutdownWaitSeconds());

	for (const TSharedPtr<FMonolithJobWorker>& Worker : Pending)
	{
		if (!Worker.IsValid())
		{
			continue;
		}

		// Cooperative stop first (the job's cancel flag was already raised by Reset()).
		if (Worker->Thread != nullptr)
		{
			Worker->Stop();
		}

		const bool bDone = Worker->bBodyReturned || Worker->DoneEvent->Wait(WaitMs);
		if (bDone && Worker->Thread != nullptr)
		{
			Worker->Thread->WaitForCompletion();
			delete Worker->Thread;
			Worker->Thread = nullptr;
			continue;
		}

		if (!bDone)
		{
			// The body ignored cancellation. Killing a thread mid-flight corrupts whatever it
			// holds, and waiting forever would hang editor shutdown — so DETACH: keep the
			// FRunnable (and its event and thread object) alive forever. Bounded leak, no
			// use-after-free, no hang.
			UE_LOG(LogMonolith, Error,
				TEXT("Job %s: background body did not return within the shutdown wait; detaching its thread (it will be leaked, not killed)."),
				*Worker->JobId);

			FScopeLock Lock(&WorkersLock);
			DetachedWorkers.Add(Worker);
		}
	}
}

// -----------------------------------------------------------------------------
// Execution layer — tick-sliced jobs
// -----------------------------------------------------------------------------

FString FMonolithJobManager::StartSlicedJob(
	const FString& Namespace,
	const FString& Action,
	FMonolithSlicedJobStep Step,
	const FString& InitialMessage)
{
	if (!Step)
	{
		UE_LOG(LogMonolith, Warning, TEXT("StartSlicedJob(%s.%s) refused: null step."), *Namespace, *Action);
		return FString();
	}
	if (bShuttingDown)
	{
		UE_LOG(LogMonolith, Warning, TEXT("StartSlicedJob(%s.%s) refused: job manager is shutting down."), *Namespace, *Action);
		return FString();
	}

	const FString JobId = CreateJob(Namespace, Action, InitialMessage);

	{
		FScopeLock Lock(&SlicedLock);
		FSlicedJobEntry Entry;
		Entry.JobId = JobId;
		Entry.Step = MakeShared<FMonolithSlicedJobStep>(MoveTemp(Step));
		SlicedJobs.Add(MoveTemp(Entry));
	}

	// Upgrade the ONE installed ticker to the slice cadence (no second ticker is added).
	EnsurePump(/*bWantsSliceCadence=*/true);

	UE_LOG(LogMonolith, Verbose, TEXT("Job %s registered as tick-sliced (%s.%s)"), *JobId, *Namespace, *Action);
	return JobId;
}

int32 FMonolithJobManager::GetSlicedJobCount() const
{
	FScopeLock Lock(&SlicedLock);
	return SlicedJobs.Num();
}

void FMonolithJobManager::AdvanceSlicedJobs()
{
	// Snapshot under the lock; the steps are invoked with NO lock held, because a slice is
	// user code and may legitimately create, cancel or poll jobs.
	TArray<FSlicedJobEntry> Snapshot;
	{
		FScopeLock Lock(&SlicedLock);
		if (SlicedJobs.Num() == 0)
		{
			return;
		}
		Snapshot = SlicedJobs;
	}

	TArray<FString> Retired;

	for (const FSlicedJobEntry& Entry : Snapshot)
	{
		FMonolithJob Job;
		if (!GetJob(Entry.JobId, Job) || Job.IsFinished())
		{
			// Removed from the registry, or already terminal (this is the cancel-between-slices
			// path: CancelJob flips the state immediately, so the step is simply never called
			// again).
			Retired.Add(Entry.JobId);
			continue;
		}

		const FMonolithJobContext Context(Entry.JobId);
		const FMonolithJobOutcome Outcome = (*Entry.Step)(Context);

		if (Outcome.Kind == EMonolithJobOutcomeKind::Pending)
		{
			// Still going — unless the slice itself raised/observed a cancel.
			if (IsCancelRequested(Entry.JobId))
			{
				CancelJob(Entry.JobId);   // no-op if already Cancelled
				Retired.Add(Entry.JobId);
			}
			continue;
		}

		ApplyOutcome(Entry.JobId, Outcome);
		Retired.Add(Entry.JobId);
	}

	if (Retired.Num() > 0)
	{
		FScopeLock Lock(&SlicedLock);
		SlicedJobs.RemoveAll([&Retired](const FSlicedJobEntry& Entry)
		{
			return Retired.Contains(Entry.JobId);
		});
	}
}

// -----------------------------------------------------------------------------
// Execution layer — worker -> game thread hand-off
// -----------------------------------------------------------------------------

void FMonolithJobManager::EnqueueGameThreadWork(TFunction<void()>&& Work)
{
	{
		FScopeLock Lock(&GameThreadWorkLock);
		PendingGameThreadWork.Add(MoveTemp(Work));
	}

	// Do not hand anything to the task graph once teardown started (or once the engine is
	// exiting): the queue is drained by the pump, and Reset() discards whatever is left.
	if (bShuttingDown || IsEngineExitRequested())
	{
		return;
	}

	AsyncTask(ENamedThreads::GameThread, []()
	{
		// Captures nothing: the manager is a process-lifetime singleton, so this task is safe
		// even if it lands after the module shut the registry down.
		FMonolithJobManager::Get().DrainGameThreadWork();
	});
}

void FMonolithJobManager::DrainGameThreadWork()
{
	TArray<TFunction<void()>> Batch;
	{
		FScopeLock Lock(&GameThreadWorkLock);
		if (bDrainingGameThreadWork || PendingGameThreadWork.Num() == 0)
		{
			// Re-entrant call (a work item pumped the manager): the outer drain owns the batch,
			// and anything queued meanwhile is picked up by the next drain.
			return;
		}
		Batch = MoveTemp(PendingGameThreadWork);
		PendingGameThreadWork.Reset();
		bDrainingGameThreadWork = true;
	}

	for (const TFunction<void()>& Work : Batch)
	{
		Work();
	}

	FScopeLock Lock(&GameThreadWorkLock);
	bDrainingGameThreadWork = false;
}

void FMonolithJobManager::ApplyOutcome(const FString& JobId, const FMonolithJobOutcome& Outcome)
{
	switch (Outcome.Kind)
	{
	case EMonolithJobOutcomeKind::Complete:
		CompleteJob(JobId, Outcome.Result);
		break;

	case EMonolithJobOutcomeKind::Error:
		FailJob(JobId, Outcome.ErrorMessage, Outcome.ErrorCode);
		break;

	case EMonolithJobOutcomeKind::Cancelled:
		// Usually a no-op: the job is already Cancelled because CancelJob raised the flag the
		// body observed. It matters when the work cancels ITSELF without an external request.
		CancelJob(JobId);
		break;

	case EMonolithJobOutcomeKind::Pending:
	default:
		FailJob(JobId, TEXT("Job step returned Pending outside a tick-sliced job."));
		break;
	}
}

bool FMonolithJobManager::UpdateProgress(const FString& JobId, const FString& Message)
{
	FScopeLock Lock(&JobsLock);

	FMonolithJob* Job = Jobs.Find(JobId);
	if (!Job || Job->IsFinished())
	{
		return false;
	}
	Job->ProgressMessage = Message;
	Job->UpdatedTimeSeconds = FPlatformTime::Seconds();
	return true;
}

bool FMonolithJobManager::FinishJobInternal(
	const FString& JobId,
	EMonolithJobState NewState,
	const TSharedPtr<FJsonValue>& Result,
	const FString& ErrorMessage,
	int32 ErrorCode)
{
	FMonolithJob* Job = Jobs.Find(JobId);
	if (!Job || Job->IsFinished())
	{
		// Unknown id, or already terminal. A terminal job is never revived — this is what
		// stops a worker that finished after being cancelled from resurrecting the job.
		return false;
	}

	Job->State = NewState;
	Job->Result = Result;
	Job->ErrorMessage = ErrorMessage;
	Job->ErrorCode = ErrorCode;
	Job->FinishedTimeSeconds = FPlatformTime::Seconds();
	Job->UpdatedTimeSeconds = Job->FinishedTimeSeconds;
	return true;
}

bool FMonolithJobManager::CompleteJob(const FString& JobId, const TSharedPtr<FJsonValue>& Result)
{
	FScopeLock Lock(&JobsLock);
	return FinishJobInternal(JobId, EMonolithJobState::Complete, Result, FString(), 0);
}

bool FMonolithJobManager::FailJob(const FString& JobId, const FString& ErrorMessage, int32 ErrorCode)
{
	FScopeLock Lock(&JobsLock);
	return FinishJobInternal(JobId, EMonolithJobState::Error, nullptr, ErrorMessage, ErrorCode);
}

bool FMonolithJobManager::CancelJob(const FString& JobId)
{
	FScopeLock Lock(&JobsLock);

	FMonolithJob* Job = Jobs.Find(JobId);
	if (!Job || Job->IsFinished())
	{
		return false;
	}
	// Raise the cooperative flag first so a worker polling IsCancelRequested between the
	// state flip and its next slice sees a consistent picture.
	Job->bCancelRequested = true;
	return FinishJobInternal(JobId, EMonolithJobState::Cancelled, nullptr, FString(), 0);
}

bool FMonolithJobManager::IsCancelRequested(const FString& JobId) const
{
	FScopeLock Lock(&JobsLock);

	const FMonolithJob* Job = Jobs.Find(JobId);
	return Job ? Job->bCancelRequested : false;
}

// -----------------------------------------------------------------------------
// Consumer side
// -----------------------------------------------------------------------------

bool FMonolithJobManager::GetJob(const FString& JobId, FMonolithJob& OutJob) const
{
	FScopeLock Lock(&JobsLock);

	const FMonolithJob* Job = Jobs.Find(JobId);
	if (!Job)
	{
		return false;
	}
	OutJob = *Job; // snapshot copy — callers never hold a pointer into the registry
	return true;
}

TArray<FMonolithJob> FMonolithJobManager::GetAllJobs() const
{
	TArray<FMonolithJob> Out;
	{
		FScopeLock Lock(&JobsLock);
		Out.Reserve(Jobs.Num());
		for (const TPair<FString, FMonolithJob>& Pair : Jobs)
		{
			Out.Add(Pair.Value);
		}
	}
	Out.Sort([](const FMonolithJob& A, const FMonolithJob& B) { return A.Serial < B.Serial; });
	return Out;
}

int32 FMonolithJobManager::GetRunningJobCount() const
{
	FScopeLock Lock(&JobsLock);

	int32 Count = 0;
	for (const TPair<FString, FMonolithJob>& Pair : Jobs)
	{
		if (!Pair.Value.IsFinished())
		{
			++Count;
		}
	}
	return Count;
}

int32 FMonolithJobManager::GetJobCount() const
{
	FScopeLock Lock(&JobsLock);
	return Jobs.Num();
}

// -----------------------------------------------------------------------------
// Lifetime management
// -----------------------------------------------------------------------------

bool FMonolithJobManager::RemoveJob(const FString& JobId)
{
	FScopeLock Lock(&JobsLock);
	return Jobs.Remove(JobId) > 0;
}

int32 FMonolithJobManager::ClearFinishedJobs()
{
	FScopeLock Lock(&JobsLock);

	TArray<FString> Doomed;
	for (const TPair<FString, FMonolithJob>& Pair : Jobs)
	{
		if (Pair.Value.IsFinished())
		{
			Doomed.Add(Pair.Key);
		}
	}
	for (const FString& Id : Doomed)
	{
		Jobs.Remove(Id);
	}
	return Doomed.Num();
}

int32 FMonolithJobManager::EnforceRetention(double NowSecondsOverride)
{
	using namespace MonolithJobManagerDetail;

	// Settings are read outside the lock — the CDO is independent of our registry.
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const int32 MaxFinished = Settings
		? FMath::Max(0, Settings->JobRetentionCount)
		: FallbackRetentionCount;
	const double MaxAgeSeconds = Settings
		? FMath::Max(0.0, static_cast<double>(Settings->JobRetentionSeconds))
		: FallbackRetentionSeconds;

	const double Now = (NowSecondsOverride >= 0.0) ? NowSecondsOverride : FPlatformTime::Seconds();

	FScopeLock Lock(&JobsLock);

	// Finished jobs, oldest-finished first (Serial breaks ties for same-frame finishes).
	TArray<const FMonolithJob*> Finished;
	Finished.Reserve(Jobs.Num());
	for (const TPair<FString, FMonolithJob>& Pair : Jobs)
	{
		if (Pair.Value.IsFinished())
		{
			Finished.Add(&Pair.Value);
		}
	}
	Finished.Sort([](const FMonolithJob& A, const FMonolithJob& B)
	{
		if (A.FinishedTimeSeconds != B.FinishedTimeSeconds)
		{
			return A.FinishedTimeSeconds < B.FinishedTimeSeconds;
		}
		return A.Serial < B.Serial;
	});

	TArray<FString> Doomed;

	// Rule 1 — age. MaxAgeSeconds == 0 disables the age rule entirely.
	TArray<const FMonolithJob*> Survivors;
	Survivors.Reserve(Finished.Num());
	for (const FMonolithJob* Job : Finished)
	{
		if (MaxAgeSeconds > 0.0 && (Now - Job->FinishedTimeSeconds) >= MaxAgeSeconds)
		{
			Doomed.Add(Job->Id);
		}
		else
		{
			Survivors.Add(Job);
		}
	}

	// Rule 2 — bounded count. Evict oldest finished until the cap is met.
	int32 Index = 0;
	while (Survivors.Num() - Index > MaxFinished)
	{
		Doomed.Add(Survivors[Index]->Id);
		++Index;
	}

	for (const FString& Id : Doomed)
	{
		Jobs.Remove(Id);
	}
	return Doomed.Num();
}

bool FMonolithJobManager::HasWorkRemaining() const
{
	{
		FScopeLock Lock(&JobsLock);
		if (Jobs.Num() > 0)
		{
			return true;
		}
	}
	{
		FScopeLock Lock(&SlicedLock);
		if (SlicedJobs.Num() > 0)
		{
			return true;
		}
	}
	{
		FScopeLock Lock(&WorkersLock);
		if (Workers.Num() > 0)
		{
			return true;
		}
	}
	FScopeLock Lock(&GameThreadWorkLock);
	return PendingGameThreadWork.Num() > 0;
}

bool FMonolithJobManager::PumpOnce()
{
	// ORDER: results queued by workers land first (so a job that finished off-thread is
	// terminal before anything else looks at it), then slices advance, then finished worker
	// threads are joined, then retention sweeps.
	DrainGameThreadWork();
	AdvanceSlicedJobs();
	ReapFinishedWorkers();
	EnforceRetention();

	return HasWorkRemaining();
}

void FMonolithJobManager::Reset()
{
	// Ordering is the whole point of this function — see the header contract.
	bShuttingDown = true;

	// 1. Stop handing out slices. Nothing new is scheduled from here on.
	{
		FScopeLock Lock(&SlicedLock);
		SlicedJobs.Empty();
	}

	// 2. Raise the cooperative cancel flag on every RUNNING job BEFORE waiting, so bodies
	//    that poll IsCancelRequested have a reason to return during the wait below. The
	//    state is left alone — the registry is emptied in step 5 anyway.
	{
		FScopeLock Lock(&JobsLock);
		for (TPair<FString, FMonolithJob>& Pair : Jobs)
		{
			if (!Pair.Value.IsFinished())
			{
				Pair.Value.bCancelRequested = true;
			}
		}
	}

	// 3. Wait (bounded) for every in-flight body, then join+delete its thread — or detach it.
	//    No lock is held across this wait, so a body can still reach the registry while it
	//    unwinds. After this returns, no worker thread the manager owns is unjoined except
	//    the deliberately detached ones, which keep their own FRunnable alive forever.
	ShutdownWorkers();

	// 4. Drop queued game-thread work unrun: its jobs are about to disappear, and the task
	//    graph may already be tearing down.
	{
		FScopeLock Lock(&GameThreadWorkLock);
		PendingGameThreadWork.Reset();
	}

	// 5. Empty the registry and uninstall the pump.
	FTSTicker::FDelegateHandle Handle;
	{
		FScopeLock Lock(&JobsLock);
		Jobs.Empty();
		Handle = PumpHandle;
		PumpHandle.Reset();
		bPumpActive = false;
		InstalledPumpInterval = -1.0f;
	}
	if (Handle.IsValid())
	{
		FTSTicker::RemoveTicker(Handle);
	}

	// Cleared last so a module reload (or a second Startup in the same process) gets a
	// manager that accepts work again.
	bShuttingDown = false;
}

// -----------------------------------------------------------------------------
// Pump
// -----------------------------------------------------------------------------

void FMonolithJobManager::EnsurePump(bool bWantsSliceCadence)
{
	using namespace MonolithJobManagerDetail;

	// Settings are read outside the lock — the CDO is independent of our registry.
	const float DesiredInterval = bWantsSliceCadence ? GetSlicePumpInterval() : GetRetentionPumpInterval();

	FTSTicker::FDelegateHandle StaleHandle;
	{
		FScopeLock Lock(&JobsLock);
		if (bPumpActive)
		{
			// Already installed. Only ever upgrade to a FASTER cadence, and never touch the
			// ticker from inside its own callback.
			if (!bWantsSliceCadence || DesiredInterval >= InstalledPumpInterval || bInPump)
			{
				return;
			}
			// Upgrade: the old handle is removed below, BEFORE the new one is added, so there
			// is exactly one MonolithJobPump ticker at every instant.
			StaleHandle = PumpHandle;
			PumpHandle.Reset();
		}
		bPumpActive = true;
		InstalledPumpInterval = DesiredInterval;
	}

	if (StaleHandle.IsValid())
	{
		FTSTicker::RemoveTicker(StaleHandle);
	}

	// One ticker for the WHOLE manager (never one per job) — the pump walks the registry.
	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("MonolithJobPump"), DesiredInterval,
		[](float DeltaTime) -> bool
		{
			return FMonolithJobManager::Get().OnPump(DeltaTime);
		});

	FScopeLock Lock(&JobsLock);
	if (bPumpActive)
	{
		PumpHandle = Handle;
	}
}

bool FMonolithJobManager::OnPump(float /*DeltaTime*/)
{
	// Guards EnsurePump against re-installing the ticker from inside the tick (a slice is
	// allowed to start further jobs).
	bInPump = true;
	DrainGameThreadWork();
	AdvanceSlicedJobs();
	ReapFinishedWorkers();
	EnforceRetention();
	bInPump = false;

	if (!HasWorkRemaining())
	{
		// Nothing left to manage — self-unregister (returning false removes the ticker)
		// and let the next CreateJob reinstall it at the then-current cadence.
		FScopeLock Lock(&JobsLock);
		bPumpActive = false;
		PumpHandle.Reset();
		InstalledPumpInterval = -1.0f;
		return false;
	}
	return true;
}
