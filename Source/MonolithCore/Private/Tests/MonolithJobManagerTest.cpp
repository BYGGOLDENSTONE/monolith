// SPDX-License-Identifier: MIT
// Faz 1 — FMonolithJobManager state-machine automation tests.
// Roadmap: Docs/GOLDENSTONE_ROADMAP.md "Faz 1" step 1.
//
// Placement follows the module precedent: `Source/MonolithCore/Private/Tests/...` so UBT's
// auto-include of `Private/` picks the file up without a Build.cs change (same reasoning as
// MonolithFuzzyMatchTest.cpp / MonolithResponseShapingTest.cpp).
//
// DETERMINISM: no wall-clock sleeps and no editor interactivity. The retention age rule is
// exercised through FMonolithJobManager::EnforceRetention(NowSecondsOverride) — an explicit
// clock value — and the pump is driven with PumpOnce() instead of waiting for engine ticks.
// Nothing here touches the asset registry, PIE or EngineSource.db, so the tests behave
// identically in a headless `-nullrhi` run.
//
// SHARED-SINGLETON HYGIENE: the manager is process-lifetime, so every test removes the jobs
// it created before returning (Reset() is deliberately NOT used — it would nuke jobs owned
// by anything else running in the same editor session).

#include "Misc/AutomationTest.h"
#include "MonolithJobManager.h"
#include "MonolithSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithJobManagerTestDetail
{
	static const TCHAR* TestNamespace = TEXT("test_jobs");

	/** RAII override of the job retention settings, restored on scope exit. */
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

	/** Drop a batch of ids from the shared registry (test cleanup). */
	static void RemoveAll(const TArray<FString>& Ids)
	{
		for (const FString& Id : Ids)
		{
			FMonolithJobManager::Get().RemoveJob(Id);
		}
	}

	/** Build a small object payload so completion results are inspectable. */
	static TSharedPtr<FJsonValue> MakePayload(const FString& Key, int32 Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(Key, Value);
		return MakeShared<FJsonValueObject>(Obj);
	}
}

// ---------------------------------------------------------------------------
// Test 1: create -> poll shows Running, with the descriptive fields echoed back.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobManagerCreateAndPollTest,
	"Monolith.JobManager.CreateAndPoll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobManagerCreateAndPollTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobManagerTestDetail;
	FMonolithJobManager& Mgr = FMonolithJobManager::Get();

	const int32 RunningBefore = Mgr.GetRunningJobCount();

	const FString IdA = Mgr.CreateJob(TestNamespace, TEXT("first"), TEXT("queued"));
	const FString IdB = Mgr.CreateJob(TestNamespace, TEXT("second"));

	TestFalse(TEXT("CreateJob returns a non-empty id"), IdA.IsEmpty());
	TestTrue(TEXT("ids are unique (monotonic issue)"), IdA != IdB);

	FMonolithJob Job;
	TestTrue(TEXT("freshly created job is pollable"), Mgr.GetJob(IdA, Job));
	TestEqual(TEXT("state is Running"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Running));
	TestEqual(TEXT("state token is 'running'"), FString(LexMonolithJobState(Job.State)), FString(TEXT("running")));
	TestEqual(TEXT("namespace echoed"), Job.Namespace, FString(TestNamespace));
	TestEqual(TEXT("action echoed"), Job.Action, FString(TEXT("first")));
	TestEqual(TEXT("initial progress message stored"), Job.ProgressMessage, FString(TEXT("queued")));
	TestFalse(TEXT("running job carries no result payload"), Job.Result.IsValid());
	TestTrue(TEXT("running job has no finish stamp"), Job.FinishedTimeSeconds < 0.0);
	TestFalse(TEXT("cancel flag starts clear"), Job.bCancelRequested);
	TestFalse(TEXT("IsFinished() false while Running"), Job.IsFinished());

	TestEqual(TEXT("running count grew by 2"), Mgr.GetRunningJobCount(), RunningBefore + 2);

	// Monotonic ordering is observable through the listing.
	FMonolithJob JobB;
	TestTrue(TEXT("second job pollable"), Mgr.GetJob(IdB, JobB));
	TestTrue(TEXT("second job has the higher serial"), JobB.Serial > Job.Serial);

	RemoveAll({ IdA, IdB });
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: progress message updates are observable; updates stop once terminal.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobManagerProgressUpdatesTest,
	"Monolith.JobManager.ProgressUpdates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobManagerProgressUpdatesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobManagerTestDetail;
	FMonolithJobManager& Mgr = FMonolithJobManager::Get();

	const FString Id = Mgr.CreateJob(TestNamespace, TEXT("progress"), TEXT("step 0"));

	FMonolithJob Job;
	Mgr.GetJob(Id, Job);
	const double CreatedStamp = Job.UpdatedTimeSeconds;

	TestTrue(TEXT("first progress update accepted"), Mgr.UpdateProgress(Id, TEXT("step 1 of 3")));
	TestTrue(TEXT("job still pollable after update"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("progress message observable"), Job.ProgressMessage, FString(TEXT("step 1 of 3")));
	TestEqual(TEXT("still Running after progress"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Running));
	TestTrue(TEXT("update stamp did not go backwards"), Job.UpdatedTimeSeconds >= CreatedStamp);

	TestTrue(TEXT("second progress update accepted"), Mgr.UpdateProgress(Id, TEXT("step 2 of 3")));
	Mgr.GetJob(Id, Job);
	TestEqual(TEXT("latest progress message wins"), Job.ProgressMessage, FString(TEXT("step 2 of 3")));

	// After a terminal transition the progress message freezes.
	TestTrue(TEXT("completion accepted"), Mgr.CompleteJob(Id, MakePayload(TEXT("n"), 1)));
	TestFalse(TEXT("progress update rejected on a finished job"), Mgr.UpdateProgress(Id, TEXT("too late")));
	Mgr.GetJob(Id, Job);
	TestEqual(TEXT("progress message unchanged after finish"), Job.ProgressMessage, FString(TEXT("step 2 of 3")));

	RemoveAll({ Id });
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: completion path -> Complete + readable result payload.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobManagerCompletionTest,
	"Monolith.JobManager.CompletionPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobManagerCompletionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobManagerTestDetail;
	FMonolithJobManager& Mgr = FMonolithJobManager::Get();

	const FString Id = Mgr.CreateJob(TestNamespace, TEXT("complete"), TEXT("working"));
	TestTrue(TEXT("CompleteJob accepted on a Running job"), Mgr.CompleteJob(Id, MakePayload(TEXT("assets_indexed"), 42)));

	FMonolithJob Job;
	TestTrue(TEXT("finished job is still pollable"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("state is Complete"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Complete));
	TestEqual(TEXT("state token is 'complete'"), FString(LexMonolithJobState(Job.State)), FString(TEXT("complete")));
	TestTrue(TEXT("IsFinished() true"), Job.IsFinished());
	TestTrue(TEXT("finish stamp recorded"), Job.FinishedTimeSeconds >= 0.0);
	TestTrue(TEXT("no error message on the success path"), Job.ErrorMessage.IsEmpty());

	// Payload is readable straight off the snapshot.
	TestTrue(TEXT("result payload present"), Job.Result.IsValid());
	if (Job.Result.IsValid())
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		TestTrue(TEXT("result payload is an object"), Job.Result->TryGetObject(Obj) && Obj && Obj->IsValid());
		if (Obj && Obj->IsValid())
		{
			double Value = 0.0;
			TestTrue(TEXT("payload field readable"), (*Obj)->TryGetNumberField(TEXT("assets_indexed"), Value));
			TestEqual(TEXT("payload value round-trips"), static_cast<int32>(Value), 42);
		}
	}

	// A second completion is a no-op — terminal states are final.
	TestFalse(TEXT("double completion rejected"), Mgr.CompleteJob(Id, MakePayload(TEXT("assets_indexed"), 999)));
	Mgr.GetJob(Id, Job);
	const TSharedPtr<FJsonObject>* Obj2 = nullptr;
	if (Job.Result.IsValid() && Job.Result->TryGetObject(Obj2) && Obj2 && Obj2->IsValid())
	{
		double Value = 0.0;
		(*Obj2)->TryGetNumberField(TEXT("assets_indexed"), Value);
		TestEqual(TEXT("original payload preserved after rejected re-complete"), static_cast<int32>(Value), 42);
	}

	RemoveAll({ Id });
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: error path -> Error + readable error message/code.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobManagerErrorPathTest,
	"Monolith.JobManager.ErrorPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobManagerErrorPathTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobManagerTestDetail;
	FMonolithJobManager& Mgr = FMonolithJobManager::Get();

	const FString Id = Mgr.CreateJob(TestNamespace, TEXT("fail"), TEXT("working"));
	TestTrue(TEXT("FailJob accepted on a Running job"), Mgr.FailJob(Id, TEXT("source index missing"), -32001));

	FMonolithJob Job;
	TestTrue(TEXT("errored job is still pollable"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("state is Error"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Error));
	TestEqual(TEXT("state token is 'error'"), FString(LexMonolithJobState(Job.State)), FString(TEXT("error")));
	TestEqual(TEXT("error message readable"), Job.ErrorMessage, FString(TEXT("source index missing")));
	TestEqual(TEXT("error code readable"), Job.ErrorCode, -32001);
	TestFalse(TEXT("no result payload on the error path"), Job.Result.IsValid());
	TestTrue(TEXT("finish stamp recorded"), Job.FinishedTimeSeconds >= 0.0);

	// Terminal is terminal: a late success does not overwrite the failure.
	TestFalse(TEXT("completion rejected after error"), Mgr.CompleteJob(Id, MakePayload(TEXT("n"), 1)));
	Mgr.GetJob(Id, Job);
	TestEqual(TEXT("state stays Error"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Error));

	RemoveAll({ Id });
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: cancellation -> Cancelled, cooperative flag raised, and a late
//         completion must NOT resurrect the job.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobManagerCancellationTest,
	"Monolith.JobManager.CancellationIsFinal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobManagerCancellationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobManagerTestDetail;
	FMonolithJobManager& Mgr = FMonolithJobManager::Get();

	const FString Id = Mgr.CreateJob(TestNamespace, TEXT("cancel"), TEXT("working"));
	TestFalse(TEXT("cancel not requested before CancelJob"), Mgr.IsCancelRequested(Id));

	TestTrue(TEXT("CancelJob accepted on a Running job"), Mgr.CancelJob(Id));
	TestTrue(TEXT("cooperative cancel flag is raised"), Mgr.IsCancelRequested(Id));

	FMonolithJob Job;
	TestTrue(TEXT("cancelled job is still pollable"), Mgr.GetJob(Id, Job));
	TestEqual(TEXT("state is Cancelled"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Cancelled));
	TestEqual(TEXT("state token is 'cancelled'"), FString(LexMonolithJobState(Job.State)), FString(TEXT("cancelled")));
	TestTrue(TEXT("IsFinished() true for Cancelled"), Job.IsFinished());

	// The worker races on and reports success/failure afterwards — both must bounce.
	TestFalse(TEXT("late completion rejected"), Mgr.CompleteJob(Id, MakePayload(TEXT("n"), 7)));
	TestFalse(TEXT("late failure rejected"), Mgr.FailJob(Id, TEXT("boom")));
	TestFalse(TEXT("progress update rejected after cancel"), Mgr.UpdateProgress(Id, TEXT("still going")));
	TestFalse(TEXT("double cancel rejected"), Mgr.CancelJob(Id));

	Mgr.GetJob(Id, Job);
	TestEqual(TEXT("state stays Cancelled (not resurrected)"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Cancelled));
	TestFalse(TEXT("no result payload leaked in from the late completion"), Job.Result.IsValid());
	TestTrue(TEXT("no error message leaked in from the late failure"), Job.ErrorMessage.IsEmpty());

	RemoveAll({ Id });
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: unknown job ids poll cleanly — no crash, sensible failure on every
//         id-taking method.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobManagerUnknownIdTest,
	"Monolith.JobManager.UnknownJobId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobManagerUnknownIdTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithJobManager& Mgr = FMonolithJobManager::Get();

	const FString Bogus = TEXT("job_does_not_exist_9999");

	FMonolithJob Job;
	Job.Id = TEXT("sentinel");
	TestFalse(TEXT("GetJob on unknown id returns false"), Mgr.GetJob(Bogus, Job));
	TestEqual(TEXT("GetJob leaves the out-param untouched on miss"), Job.Id, FString(TEXT("sentinel")));

	TestFalse(TEXT("UpdateProgress on unknown id returns false"), Mgr.UpdateProgress(Bogus, TEXT("x")));
	TestFalse(TEXT("CompleteJob on unknown id returns false"), Mgr.CompleteJob(Bogus, nullptr));
	TestFalse(TEXT("FailJob on unknown id returns false"), Mgr.FailJob(Bogus, TEXT("x")));
	TestFalse(TEXT("CancelJob on unknown id returns false"), Mgr.CancelJob(Bogus));
	TestFalse(TEXT("IsCancelRequested on unknown id returns false"), Mgr.IsCancelRequested(Bogus));
	TestFalse(TEXT("RemoveJob on unknown id returns false"), Mgr.RemoveJob(Bogus));

	// Empty id is the same clean miss, not a special case.
	TestFalse(TEXT("GetJob on empty id returns false"), Mgr.GetJob(FString(), Job));
	TestFalse(TEXT("CancelJob on empty id returns false"), Mgr.CancelJob(FString()));

	return true;
}

// ---------------------------------------------------------------------------
// Test 7: retention + removal actually evict finished jobs, driven by the
//         MonolithSettings values (count cap, age cap) and by explicit removal.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobManagerRetentionTest,
	"Monolith.JobManager.RetentionEviction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobManagerRetentionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobManagerTestDetail;
	FMonolithJobManager& Mgr = FMonolithJobManager::Get();

	// --- Explicit removal ---------------------------------------------------
	{
		const FString Id = Mgr.CreateJob(TestNamespace, TEXT("explicit_remove"));
		Mgr.CompleteJob(Id, MakePayload(TEXT("n"), 1));
		TestTrue(TEXT("RemoveJob evicts a finished job"), Mgr.RemoveJob(Id));

		FMonolithJob Job;
		TestFalse(TEXT("removed job no longer pollable"), Mgr.GetJob(Id, Job));
	}

	// --- Count cap (JobRetentionCount) --------------------------------------
	{
		// Age rule disabled so the count rule is the only thing under test.
		FScopedRetentionSettings Guard(/*Count*/ 1, /*Seconds*/ 0.0f);

		const FString Running = Mgr.CreateJob(TestNamespace, TEXT("count_running"));
		const FString Old     = Mgr.CreateJob(TestNamespace, TEXT("count_old"));
		const FString Mid     = Mgr.CreateJob(TestNamespace, TEXT("count_mid"));
		const FString New     = Mgr.CreateJob(TestNamespace, TEXT("count_new"));

		// Finish oldest-first so eviction order is well defined.
		Mgr.CompleteJob(Old, MakePayload(TEXT("n"), 1));
		Mgr.FailJob(Mid, TEXT("mid failed"));
		Mgr.CancelJob(New);

		// >= 2 rather than == 2: the registry is process-wide, so a stray finished job from
		// elsewhere in the session would also be swept. The per-id checks below are the
		// real signal.
		const int32 Evicted = Mgr.EnforceRetention();
		TestTrue(FString::Printf(TEXT("count cap evicted the two older finished jobs (evicted=%d)"), Evicted), Evicted >= 2);

		FMonolithJob Job;
		TestFalse(TEXT("oldest finished job evicted"), Mgr.GetJob(Old, Job));
		TestFalse(TEXT("middle finished job evicted"), Mgr.GetJob(Mid, Job));
		TestTrue(TEXT("newest finished job retained"), Mgr.GetJob(New, Job));
		TestTrue(TEXT("RUNNING job never evicted by retention"), Mgr.GetJob(Running, Job));
		TestEqual(TEXT("surviving running job is still Running"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Running));

		RemoveAll({ Running, Old, Mid, New });
	}

	// --- Age cap (JobRetentionSeconds), deterministic via the clock override --
	{
		// Generous count cap so the age rule is the only thing under test.
		FScopedRetentionSettings Guard(/*Count*/ 1024, /*Seconds*/ 30.0f);

		const FString Id = Mgr.CreateJob(TestNamespace, TEXT("age_evict"));
		Mgr.CompleteJob(Id, MakePayload(TEXT("n"), 1));

		FMonolithJob Job;
		TestTrue(TEXT("finished job present before the age sweep"), Mgr.GetJob(Id, Job));
		const double FinishedAt = Job.FinishedTimeSeconds;

		// Not yet old enough — an injected clock just under the limit keeps it.
		Mgr.EnforceRetention(FinishedAt + 29.0);
		TestTrue(TEXT("job survives the sweep before the age limit"), Mgr.GetJob(Id, Job));

		// Past the limit — evicted, with no wall-clock wait anywhere.
		const int32 AgeEvicted = Mgr.EnforceRetention(FinishedAt + 31.0);
		TestTrue(FString::Printf(TEXT("age sweep past the limit evicted at least this job (evicted=%d)"), AgeEvicted), AgeEvicted >= 1);
		TestFalse(TEXT("aged-out job is gone"), Mgr.GetJob(Id, Job));
	}

	// --- ClearFinishedJobs + the pump both use the same policy ---------------
	{
		FScopedRetentionSettings Guard(/*Count*/ 0, /*Seconds*/ 0.0f);

		const FString Running = Mgr.CreateJob(TestNamespace, TEXT("clear_running"));
		const FString Done    = Mgr.CreateJob(TestNamespace, TEXT("clear_done"));
		Mgr.CompleteJob(Done, MakePayload(TEXT("n"), 1));

		// The shared pump enforces retention; with a zero cap the finished job goes.
		const bool bJobsRemain = Mgr.PumpOnce();
		TestTrue(TEXT("pump reports work remaining while a job is Running"), bJobsRemain);

		FMonolithJob Job;
		TestFalse(TEXT("pump evicted the finished job (count cap 0)"), Mgr.GetJob(Done, Job));
		TestTrue(TEXT("pump kept the running job"), Mgr.GetJob(Running, Job));

		// ClearFinishedJobs drops finished jobs regardless of the retention cap.
		const FString Done2 = Mgr.CreateJob(TestNamespace, TEXT("clear_done2"));
		Mgr.FailJob(Done2, TEXT("nope"));
		TestTrue(TEXT("ClearFinishedJobs removed at least the one finished job"), Mgr.ClearFinishedJobs() >= 1);
		TestFalse(TEXT("cleared job is gone"), Mgr.GetJob(Done2, Job));
		TestTrue(TEXT("ClearFinishedJobs left the running job alone"), Mgr.GetJob(Running, Job));

		RemoveAll({ Running });
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
