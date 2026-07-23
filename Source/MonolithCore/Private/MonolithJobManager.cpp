// SPDX-License-Identifier: MIT
// Faz 1 — Job System core. Header: Public/MonolithJobManager.h

#include "MonolithJobManager.h"

#include "MonolithJsonUtils.h"   // LogMonolith
#include "MonolithSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace MonolithJobManagerDetail
{
	// Fallback retention policy used only when the settings CDO is unavailable (very early
	// module init / stripped builds). The authoritative values live on UMonolithSettings —
	// these mirror its declared defaults so behaviour does not change silently.
	static constexpr int32 FallbackRetentionCount = 64;
	static constexpr double FallbackRetentionSeconds = 900.0;
	static constexpr float FallbackPumpIntervalSeconds = 1.0f;
}

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
	EnsurePump();

	UE_LOG(LogMonolith, Verbose, TEXT("Job %s created (%s.%s)"), *NewId, *Namespace, *Action);
	return NewId;
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

bool FMonolithJobManager::PumpOnce()
{
	EnforceRetention();

	FScopeLock Lock(&JobsLock);
	return Jobs.Num() > 0;
}

void FMonolithJobManager::Reset()
{
	FTSTicker::FDelegateHandle Handle;
	{
		FScopeLock Lock(&JobsLock);
		Jobs.Empty();
		Handle = PumpHandle;
		PumpHandle.Reset();
		bPumpActive = false;
	}
	if (Handle.IsValid())
	{
		FTSTicker::RemoveTicker(Handle);
	}
}

// -----------------------------------------------------------------------------
// Pump
// -----------------------------------------------------------------------------

void FMonolithJobManager::EnsurePump()
{
	using namespace MonolithJobManagerDetail;

	{
		FScopeLock Lock(&JobsLock);
		if (bPumpActive)
		{
			return;
		}
		bPumpActive = true;
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const float Interval = Settings
		? FMath::Max(0.0f, Settings->JobPumpIntervalSeconds)
		: FallbackPumpIntervalSeconds;

	// One ticker for the WHOLE manager (never one per job) — the pump walks the registry.
	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("MonolithJobPump"), Interval,
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
	EnforceRetention();

	FScopeLock Lock(&JobsLock);
	if (Jobs.Num() == 0)
	{
		// Nothing left to manage — self-unregister (returning false removes the ticker)
		// and let the next CreateJob reinstall it.
		bPumpActive = false;
		PumpHandle.Reset();
		return false;
	}
	return true;
}
