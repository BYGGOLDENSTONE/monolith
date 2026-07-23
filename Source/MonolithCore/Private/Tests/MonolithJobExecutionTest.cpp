// SPDX-License-Identifier: MIT
// Faz 1 — FMonolithJobManager EXECUTION layer automation tests (background threads +
// tick-sliced game-thread jobs). Roadmap: Docs/GOLDENSTONE_ROADMAP.md "Faz 1" item 5.
// State-machine coverage lives next door in MonolithJobManagerTest.cpp.
//
// DETERMINISM (the hard requirement for this file):
//   - No FPlatformProcess::Sleep anywhere. Background tests synchronise on FEvents that the
//     test itself owns: the body signals "I am inside the work", the test signals "you may
//     finish". Every wait is on a real synchronisation object with a bounded timeout, never
//     on elapsed time.
//   - Sliced jobs are advanced ONLY by explicit FMonolithJobManager::PumpOnce() calls. The
//     engine's own ticker cannot interfere: RunTest runs to completion inside one game-thread
//     call, so no engine tick happens in the middle of a test.
//   - Terminal state is asserted only AFTER PumpOnce(), which is the documented contract
//     ("wait, then pump, then read"). The tests deliberately do NOT assert that the state has
//     *not* been applied yet — that would be asserting the task graph did not run, which is a
//     scheduling detail, not a contract.
//
// SHARED-SINGLETON HYGIENE: the manager is process-lifetime. Every test removes the jobs it
// created, joins every worker it started, and never calls Reset().

#include "Misc/AutomationTest.h"
#include "MonolithJobManager.h"
#include "MonolithSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeBool.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithJobExecutionTestDetail
{
	static const TCHAR* TestNamespace = TEXT("test_jobexec");

	/**
	 * Upper bound on every wait in this file. It is a DEADLOCK GUARD, not a timing
	 * assumption: in a healthy run each wait is satisfied as soon as the other side signals,
	 * typically in microseconds. If one ever expires the test fails loudly instead of hanging
	 * the whole suite.
	 */
	static constexpr uint32 WaitGuardMs = 30000;
	static constexpr double WaitGuardSeconds = 30.0;

	/** RAII override of the retention settings so pumps cannot evict the job under test. */
	struct FScopedRetentionSettings
	{
		UMonolithSettings* Settings = nullptr;
		int32 OldCount = 0;
		float OldSeconds = 0.0f;

		FScopedRetentionSettings(int32 NewCount, float NewSeconds)
			: Settings(GetMutableDefault<UMonolithSettings>())
		{
			if (Settings)
			{
				OldCount = Settings->JobRetentionCount;
				OldSeconds = Settings->JobRetentionSeconds;
				Settings->JobRetentionCount = NewCount;
				Settings->JobRetentionSeconds = NewSeconds;
			}
		}

		~FScopedRetentionSettings()
		{
			if (Settings)
			{
				Settings->JobRetentionCount = OldCount;
				Settings->JobRetentionSeconds = OldSeconds;
			}
		}
	};

	/** Manual-reset event pair from the engine pool, always returned even on a failed path. */
	struct FPooledTestEvent
	{
		FEvent* Event = nullptr;

		FPooledTestEvent()
			: Event(FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/true))
		{
		}

		~FPooledTestEvent()
		{
			if (Event)
			{
				FPlatformProcess::ReturnSynchEventToPool(Event);
				Event = nullptr;
			}
		}

		FPooledTestEvent(const FPooledTestEvent&) = delete;
		FPooledTestEvent& operator=(const FPooledTestEvent&) = delete;
	};

	static TSharedPtr<FJsonValue> MakePayload(const FString& Key, int32 Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(Key, Value);
		return MakeShared<FJsonValueObject>(Obj);
	}

	/** Read one integer field out of a completed job's payload. Returns INDEX_NONE on miss. */
	static int32 ReadPayloadInt(const TSharedPtr<FJsonValue>& Value, const FString& Key)
	{
		if (!Value.IsValid())
		{
			return INDEX_NONE;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
		{
			return INDEX_NONE;
		}
		double Number = 0.0;
		if (!(*Obj)->TryGetNumberField(Key, Number))
		{
			return INDEX_NONE;
		}
		return static_cast<int32>(Number);
	}

	/**
	 * Background jobs need a REAL worker thread. Without multithreading support
	 * FRunnableThread::Create hands back a fake thread driven from the main tick, so a body
	 * that waits for the game thread would deadlock — the tests skip instead of hanging.
	 */
	static bool SkipIfSingleThreaded(FAutomationTestBase& Test)
	{
		if (FPlatformProcess::SupportsMultithreading())
		{
			return false;
		}
		Test.AddInfo(TEXT("Skipping: platform reports no multithreading support, background jobs would run as fake threads."));
		return true;
	}
}

// ---------------------------------------------------------------------------
// Test 1: a background body runs off the game thread, its progress is observable
//         after a pump, and its result payload is readable once complete.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobExecutionBackgroundCompletesTest,
	"Monolith.JobExecution.BackgroundCompletes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobExecutionBackgroundCompletesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobExecutionTestDetail;
	if (SkipIfSingleThreaded(*this))
	{
		return true;
	}

	FMonolithJobManager& Mgr = FMonolithJobManager::Get();
	FScopedRetentionSettings Guard(/*Count*/ 1024, /*Seconds*/ 0.0f);

	const int32 WorkersBefore = Mgr.GetBackgroundWorkerCount();

	FPooledTestEvent InsideBody;
	FPooledTestEvent MayFinish;
	FEvent* const InsideBodyEvent = InsideBody.Event;
	FEvent* const MayFinishEvent = MayFinish.Event;

	const FString Id = Mgr.StartBackgroundJob(TestNamespace, TEXT("bg_complete"),
		[InsideBodyEvent, MayFinishEvent](const FMonolithJobContext& Context) -> FMonolithJobOutcome
		{
			Context.ReportProgress(TEXT("halfway"));
			InsideBodyEvent->Trigger();
			MayFinishEvent->Wait(WaitGuardMs);
			return FMonolithJobOutcome::Complete(MakePayload(TEXT("rows"), 7));
		},
		TEXT("queued"));

	TestFalse(TEXT("StartBackgroundJob returned a job id"), Id.IsEmpty());
	TestEqual(TEXT("one background worker is tracked"), Mgr.GetBackgroundWorkerCount(), WorkersBefore + 1);

	// Deterministic hand-off: the body tells us it is inside the work.
	TestTrue(TEXT("worker body entered the work"), InsideBodyEvent->Wait(WaitGuardMs));

	Mgr.PumpOnce();   // applies the marshalled progress update on the game thread

	FMonolithJob Job;
	TestTrue(TEXT("job pollable while the body runs"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("job is Running while the body is in flight"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Running));
	TestEqual(TEXT("worker progress reached the registry via the game thread"),
		Job.ProgressMessage, FString(TEXT("halfway")));

	// Release the body and join it — an event wait, never a sleep.
	MayFinishEvent->Trigger();
	TestTrue(TEXT("background body returned before the guard expired"), Mgr.WaitForBackgroundJob(Id, WaitGuardSeconds));

	Mgr.PumpOnce();   // applies the outcome and reaps the worker thread

	TestTrue(TEXT("finished job still pollable"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("state is Complete"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Complete));
	TestEqual(TEXT("result payload is readable"), ReadPayloadInt(Job.Result, TEXT("rows")), 7);
	TestTrue(TEXT("finish stamp recorded"), Job.FinishedTimeSeconds >= 0.0);
	TestEqual(TEXT("pump joined and reaped the worker thread"), Mgr.GetBackgroundWorkerCount(), WorkersBefore);

	Mgr.RemoveJob(Id);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: a background body that fails lands in Error with its message and code.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobExecutionBackgroundFailsTest,
	"Monolith.JobExecution.BackgroundFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobExecutionBackgroundFailsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobExecutionTestDetail;
	if (SkipIfSingleThreaded(*this))
	{
		return true;
	}

	FMonolithJobManager& Mgr = FMonolithJobManager::Get();
	FScopedRetentionSettings Guard(/*Count*/ 1024, /*Seconds*/ 0.0f);

	const int32 WorkersBefore = Mgr.GetBackgroundWorkerCount();

	const FString Id = Mgr.StartBackgroundJob(TestNamespace, TEXT("bg_fail"),
		[](const FMonolithJobContext& /*Context*/) -> FMonolithJobOutcome
		{
			return FMonolithJobOutcome::Failure(TEXT("search index source missing"), -32001);
		});

	TestFalse(TEXT("StartBackgroundJob returned a job id"), Id.IsEmpty());
	TestTrue(TEXT("background body returned before the guard expired"), Mgr.WaitForBackgroundJob(Id, WaitGuardSeconds));

	Mgr.PumpOnce();

	FMonolithJob Job;
	TestTrue(TEXT("failed job is pollable"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("state is Error"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Error));
	TestEqual(TEXT("error message survived the thread hop"), Job.ErrorMessage, FString(TEXT("search index source missing")));
	TestEqual(TEXT("error code survived the thread hop"), Job.ErrorCode, -32001);
	TestFalse(TEXT("no result payload on the error path"), Job.Result.IsValid());
	TestEqual(TEXT("pump joined and reaped the worker thread"), Mgr.GetBackgroundWorkerCount(), WorkersBefore);

	Mgr.RemoveJob(Id);
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: a background body observes cancellation cooperatively and the job
//         lands in Cancelled — with no result leaking in from the late return.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobExecutionBackgroundCancellationTest,
	"Monolith.JobExecution.BackgroundCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobExecutionBackgroundCancellationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobExecutionTestDetail;
	if (SkipIfSingleThreaded(*this))
	{
		return true;
	}

	FMonolithJobManager& Mgr = FMonolithJobManager::Get();
	FScopedRetentionSettings Guard(/*Count*/ 1024, /*Seconds*/ 0.0f);

	FPooledTestEvent InsideBody;
	FPooledTestEvent MayFinish;
	FEvent* const InsideBodyEvent = InsideBody.Event;
	FEvent* const MayFinishEvent = MayFinish.Event;

	// Written by the worker, read by the game thread after the join — hence thread-safe.
	TSharedPtr<FThreadSafeBool> SawCancel = MakeShared<FThreadSafeBool>(false);

	const FString Id = Mgr.StartBackgroundJob(TestNamespace, TEXT("bg_cancel"),
		[InsideBodyEvent, MayFinishEvent, SawCancel](const FMonolithJobContext& Context) -> FMonolithJobOutcome
		{
			InsideBodyEvent->Trigger();
			MayFinishEvent->Wait(WaitGuardMs);

			// Cooperative checkpoint — exactly what a real chunked body does between chunks.
			if (Context.IsCancelRequested())
			{
				*SawCancel = true;
				return FMonolithJobOutcome::Cancelled();
			}
			return FMonolithJobOutcome::Complete(MakePayload(TEXT("rows"), 99));
		});

	TestFalse(TEXT("StartBackgroundJob returned a job id"), Id.IsEmpty());
	TestTrue(TEXT("worker body entered the work"), InsideBodyEvent->Wait(WaitGuardMs));

	// Cancel strictly BEFORE releasing the body, so the checkpoint below cannot race.
	TestTrue(TEXT("CancelJob accepted while the body is in flight"), Mgr.CancelJob(Id));
	MayFinishEvent->Trigger();

	TestTrue(TEXT("background body returned before the guard expired"), Mgr.WaitForBackgroundJob(Id, WaitGuardSeconds));
	Mgr.PumpOnce();

	TestTrue(TEXT("the body observed the cooperative cancel flag"), (bool)*SawCancel);

	FMonolithJob Job;
	TestTrue(TEXT("cancelled job is pollable"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("state is Cancelled"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Cancelled));
	TestTrue(TEXT("cooperative cancel flag stayed raised"), Job.bCancelRequested);
	TestFalse(TEXT("no result payload leaked in from the late return"), Job.Result.IsValid());
	TestTrue(TEXT("no error message leaked in from the late return"), Job.ErrorMessage.IsEmpty());

	Mgr.RemoveJob(Id);
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: a tick-sliced job advances one slice per pump on the game thread and
//         completes with a readable payload — no extra ticker, no inline first slice.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobExecutionSlicedAdvancesTest,
	"Monolith.JobExecution.SlicedAdvancesAndCompletes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobExecutionSlicedAdvancesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobExecutionTestDetail;

	FMonolithJobManager& Mgr = FMonolithJobManager::Get();
	FScopedRetentionSettings Guard(/*Count*/ 1024, /*Seconds*/ 0.0f);

	const int32 SlicedBefore = Mgr.GetSlicedJobCount();

	// Game-thread-only counter: slices are invoked from the pump, i.e. from this thread.
	TSharedRef<int32> Slices = MakeShared<int32>(0);

	const FString Id = Mgr.StartSlicedJob(TestNamespace, TEXT("sliced_complete"),
		[Slices](const FMonolithJobContext& Context) -> FMonolithJobOutcome
		{
			++(*Slices);
			Context.ReportProgress(FString::Printf(TEXT("slice %d of 3"), *Slices));
			if (*Slices < 3)
			{
				return FMonolithJobOutcome::Pending();
			}
			return FMonolithJobOutcome::Complete(MakePayload(TEXT("slices"), *Slices));
		},
		TEXT("queued"));

	TestFalse(TEXT("StartSlicedJob returned a job id"), Id.IsEmpty());
	TestEqual(TEXT("no slice runs inline at submission"), *Slices, 0);
	TestEqual(TEXT("sliced job registered with the shared pump"), Mgr.GetSlicedJobCount(), SlicedBefore + 1);

	FMonolithJob Job;

	Mgr.PumpOnce();
	TestEqual(TEXT("first pump ran exactly one slice"), *Slices, 1);
	TestTrue(TEXT("job pollable mid-slicing"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("still Running after slice 1"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Running));
	TestEqual(TEXT("slice progress is observable immediately (game thread)"),
		Job.ProgressMessage, FString(TEXT("slice 1 of 3")));

	Mgr.PumpOnce();
	TestEqual(TEXT("second pump ran exactly one more slice"), *Slices, 2);
	Mgr.GetJob(Id, Job);
	TestEqual(TEXT("still Running after slice 2"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Running));
	TestEqual(TEXT("progress advanced with the slice"), Job.ProgressMessage, FString(TEXT("slice 2 of 3")));

	Mgr.PumpOnce();
	TestEqual(TEXT("third pump ran the final slice"), *Slices, 3);
	TestTrue(TEXT("completed sliced job is pollable"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("state is Complete"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Complete));
	TestEqual(TEXT("result payload is readable"), ReadPayloadInt(Job.Result, TEXT("slices")), 3);
	TestEqual(TEXT("step deregistered from the pump on completion"), Mgr.GetSlicedJobCount(), SlicedBefore);

	Mgr.PumpOnce();
	TestEqual(TEXT("no further slice after the terminal outcome"), *Slices, 3);

	Mgr.RemoveJob(Id);
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: cancellation of a sliced job — externally between slices, and by the
//         slice itself. Neither may run another slice afterwards.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobExecutionSlicedCancellationTest,
	"Monolith.JobExecution.SlicedCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobExecutionSlicedCancellationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobExecutionTestDetail;

	FMonolithJobManager& Mgr = FMonolithJobManager::Get();
	FScopedRetentionSettings Guard(/*Count*/ 1024, /*Seconds*/ 0.0f);

	const int32 SlicedBefore = Mgr.GetSlicedJobCount();
	FMonolithJob Job;

	// --- Cancelled from outside, between two slices ---------------------------
	{
		TSharedRef<int32> Slices = MakeShared<int32>(0);
		const FString Id = Mgr.StartSlicedJob(TestNamespace, TEXT("sliced_cancel_external"),
			[Slices](const FMonolithJobContext& /*Context*/) -> FMonolithJobOutcome
			{
				++(*Slices);
				return FMonolithJobOutcome::Pending();   // never finishes on its own
			});

		Mgr.PumpOnce();
		TestEqual(TEXT("one slice ran before the cancel"), *Slices, 1);

		TestTrue(TEXT("CancelJob accepted between slices"), Mgr.CancelJob(Id));
		TestTrue(TEXT("cooperative cancel flag readable from the manager"), Mgr.IsCancelRequested(Id));

		Mgr.PumpOnce();
		TestEqual(TEXT("no slice runs after cancellation"), *Slices, 1);

		TestTrue(TEXT("cancelled sliced job is pollable"), Mgr.GetJob(Id, Job));
		TestEqual(TEXT("state is Cancelled"),
			static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Cancelled));
		TestEqual(TEXT("step deregistered from the pump"), Mgr.GetSlicedJobCount(), SlicedBefore);

		Mgr.RemoveJob(Id);
	}

	// --- The slice cancels itself --------------------------------------------
	{
		TSharedRef<int32> Slices = MakeShared<int32>(0);
		const FString Id = Mgr.StartSlicedJob(TestNamespace, TEXT("sliced_cancel_self"),
			[Slices](const FMonolithJobContext& /*Context*/) -> FMonolithJobOutcome
			{
				++(*Slices);
				return (*Slices < 2)
					? FMonolithJobOutcome::Pending()
					: FMonolithJobOutcome::Cancelled();
			});

		Mgr.PumpOnce();
		Mgr.PumpOnce();
		TestEqual(TEXT("the self-cancelling slice ran twice"), *Slices, 2);

		TestTrue(TEXT("self-cancelled sliced job is pollable"), Mgr.GetJob(Id, Job));
		TestEqual(TEXT("state is Cancelled"),
			static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Cancelled));

		Mgr.PumpOnce();
		TestEqual(TEXT("no slice after the self-cancel"), *Slices, 2);
		TestEqual(TEXT("step deregistered from the pump"), Mgr.GetSlicedJobCount(), SlicedBefore);

		Mgr.RemoveJob(Id);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 6: shutdown-safety proxy — a background body whose job is dropped from
//         the registry mid-flight must still join cleanly, and its late outcome
//         must be a no-op rather than a crash or a resurrected job.
//
//         This is the same "the registry moved under the worker" path Reset()
//         relies on. Reset() itself is not called: it is game-thread teardown of
//         a registry shared with every other test in the session.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobExecutionOrphanedWorkerTest,
	"Monolith.JobExecution.OrphanedWorkerIsSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobExecutionOrphanedWorkerTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobExecutionTestDetail;
	if (SkipIfSingleThreaded(*this))
	{
		return true;
	}

	FMonolithJobManager& Mgr = FMonolithJobManager::Get();
	FScopedRetentionSettings Guard(/*Count*/ 1024, /*Seconds*/ 0.0f);

	const int32 WorkersBefore = Mgr.GetBackgroundWorkerCount();

	FPooledTestEvent InsideBody;
	FPooledTestEvent MayFinish;
	FEvent* const InsideBodyEvent = InsideBody.Event;
	FEvent* const MayFinishEvent = MayFinish.Event;

	const FString Id = Mgr.StartBackgroundJob(TestNamespace, TEXT("bg_orphan"),
		[InsideBodyEvent, MayFinishEvent](const FMonolithJobContext& Context) -> FMonolithJobOutcome
		{
			InsideBodyEvent->Trigger();
			MayFinishEvent->Wait(WaitGuardMs);

			// Both of these run against a registry entry that no longer exists.
			Context.ReportProgress(TEXT("still going"));
			return FMonolithJobOutcome::Complete(MakePayload(TEXT("rows"), 5));
		});

	TestFalse(TEXT("StartBackgroundJob returned a job id"), Id.IsEmpty());
	TestTrue(TEXT("worker body entered the work"), InsideBodyEvent->Wait(WaitGuardMs));

	// Drop the job while the body is still running — the worker is now orphaned.
	TestTrue(TEXT("RemoveJob dropped the running job"), Mgr.RemoveJob(Id));

	MayFinishEvent->Trigger();
	TestTrue(TEXT("orphaned body still returned and was joinable"), Mgr.WaitForBackgroundJob(Id, WaitGuardSeconds));

	Mgr.PumpOnce();   // applies the late progress + outcome against a job that is gone

	FMonolithJob Job;
	TestFalse(TEXT("the removed job was not resurrected by the late outcome"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("the orphaned worker thread was still joined and reaped"),
		Mgr.GetBackgroundWorkerCount(), WorkersBefore);

	return true;
}

// ---------------------------------------------------------------------------
// Test 7: submissions the manager must refuse, cleanly and without a job.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobExecutionInvalidSubmissionsTest,
	"Monolith.JobExecution.InvalidSubmissions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobExecutionInvalidSubmissionsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobExecutionTestDetail;

	FMonolithJobManager& Mgr = FMonolithJobManager::Get();

	const int32 JobsBefore = Mgr.GetJobCount();
	const int32 SlicedBefore = Mgr.GetSlicedJobCount();
	const int32 WorkersBefore = Mgr.GetBackgroundWorkerCount();

	TestTrue(TEXT("a null background body yields no job id"),
		Mgr.StartBackgroundJob(TestNamespace, TEXT("null_body"), FMonolithBackgroundJobBody()).IsEmpty());
	TestTrue(TEXT("a null slice step yields no job id"),
		Mgr.StartSlicedJob(TestNamespace, TEXT("null_step"), FMonolithSlicedJobStep()).IsEmpty());

	TestEqual(TEXT("no job was registered for a refused submission"), Mgr.GetJobCount(), JobsBefore);
	TestEqual(TEXT("no slice step was registered for a refused submission"), Mgr.GetSlicedJobCount(), SlicedBefore);
	TestEqual(TEXT("no worker thread was started for a refused submission"), Mgr.GetBackgroundWorkerCount(), WorkersBefore);

	// Waiting on an id that has no background worker is a clean immediate success, not a hang.
	TestTrue(TEXT("waiting on an unknown job returns immediately"),
		Mgr.WaitForBackgroundJob(TEXT("job_does_not_exist_9999"), 0.0));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
