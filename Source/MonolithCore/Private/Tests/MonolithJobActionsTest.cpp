// SPDX-License-Identifier: MIT
// Faz 1 — automation coverage for the `jobs` namespace (list / poll / cancel / clear).
// Implementation + placement rationale: Private/MonolithJobActions.h
//
// The handlers are exercised DIRECTLY (FMonolithJobActions::Handle*) so the tests do not
// depend on module startup having registered the namespace; one dedicated test covers
// registration and the registry dispatch path (aliases + required-param validation).
//
// DETERMINISM: no wall-clock sleeps, no asset registry, no PIE, no EngineSource.db — the
// suite behaves identically in a headless `-nullrhi` run.
//
// SHARED-SINGLETON HYGIENE: FMonolithJobManager is process-lifetime and shared with the
// MonolithJobManager tests, so every test here
//   (a) tags its jobs with a private namespace token and filters listings by it,
//   (b) asserts relative counts (>=) where the global registry could contain strays, and
//   (c) removes exactly the ids it created. FMonolithJobManager::Reset() is never called.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithJobActions.h"
#include "MonolithJobManager.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MonolithJobActionsTestDetail
{
	/** Private tag so listings can be isolated from jobs owned by other tests. */
	static const TCHAR* TestNamespace = TEXT("test_job_actions");

	static void RemoveAll(const TArray<FString>& Ids)
	{
		for (const FString& Id : Ids)
		{
			FMonolithJobManager::Get().RemoveJob(Id);
		}
	}

	static TSharedPtr<FJsonObject> Params()
	{
		return MakeShared<FJsonObject>();
	}

	static TSharedPtr<FJsonObject> JobIdParams(const FString& JobId)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("job_id"), JobId);
		return P;
	}

	static TSharedPtr<FJsonObject> MakePayload(const FString& Key, int32 Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(Key, Value);
		return Obj;
	}

	/** Read `error.data.error_code` off a failed action result (empty when absent). */
	static FString GetErrorCodeTag(const FMonolithActionResult& Result)
	{
		if (!Result.ErrorData.IsValid())
		{
			return FString();
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Result.ErrorData->TryGetObject(Obj) || !Obj || !Obj->IsValid())
		{
			return FString();
		}
		FString Code;
		(*Obj)->TryGetStringField(TEXT("error_code"), Code);
		return Code;
	}

	/** Fetch the `jobs` array out of a jobs_list payload. */
	static TArray<TSharedPtr<FJsonObject>> GetListedJobs(const TSharedPtr<FJsonObject>& Result)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		if (!Result.IsValid())
		{
			return Out;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Result->TryGetArrayField(TEXT("jobs"), Arr) || !Arr)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid())
			{
				Out.Add(*Obj);
			}
		}
		return Out;
	}

	/** List only the jobs this test file created. */
	static TSharedPtr<FJsonObject> ListOwn(const FString& StateFilter = FString(), bool bIncludeResult = false)
	{
		TSharedPtr<FJsonObject> P = Params();
		P->SetStringField(TEXT("namespace"), TestNamespace);
		if (!StateFilter.IsEmpty())
		{
			P->SetStringField(TEXT("state"), StateFilter);
		}
		P->SetBoolField(TEXT("include_result"), bIncludeResult);

		const FMonolithActionResult Result = FMonolithJobActions::HandleList(P);
		return Result.bSuccess ? Result.Result : nullptr;
	}

	static FString GetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Value;
		if (Obj.IsValid()) { Obj->TryGetStringField(Field, Value); }
		return Value;
	}

	static double GetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		double Value = 0.0;
		if (Obj.IsValid()) { Obj->TryGetNumberField(Field, Value); }
		return Value;
	}

	static bool GetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		bool Value = false;
		if (Obj.IsValid()) { Obj->TryGetBoolField(Field, Value); }
		return Value;
	}
}

// ---------------------------------------------------------------------------
// Test 1: jobs_list — empty (nothing of ours yet), then populated + filtered.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobActionsListTest,
	"Monolith.JobActions.List",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobActionsListTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobActionsTestDetail;
	FMonolithJobManager& Manager = FMonolithJobManager::Get();

	// --- Empty listing -------------------------------------------------------
	{
		TSharedPtr<FJsonObject> Result = ListOwn();
		TestTrue(TEXT("empty list still succeeds"), Result.IsValid());
		TestEqual(TEXT("no jobs of ours yet"), static_cast<int32>(GetNumber(Result, TEXT("count"))), 0);
		TestEqual(TEXT("empty listing carries an empty jobs array"), GetListedJobs(Result).Num(), 0);
		// The envelope fields are always present, even with nothing to show.
		TestTrue(TEXT("total_count field present"), Result.IsValid() && Result->HasField(TEXT("total_count")));
		TestTrue(TEXT("running_count field present"), Result.IsValid() && Result->HasField(TEXT("running_count")));
	}

	// --- Populated listing ---------------------------------------------------
	const FString RunningId = Manager.CreateJob(TestNamespace, TEXT("list_running"), TEXT("step 1"));
	const FString DoneId    = Manager.CreateJob(TestNamespace, TEXT("list_done"));
	const FString FailedId  = Manager.CreateJob(TestNamespace, TEXT("list_failed"));
	Manager.CompleteJob(DoneId, MakeShared<FJsonValueObject>(MakePayload(TEXT("rows"), 7)));
	Manager.FailJob(FailedId, TEXT("list boom"), -32001);

	{
		TSharedPtr<FJsonObject> Result = ListOwn();
		const TArray<TSharedPtr<FJsonObject>> Listed = GetListedJobs(Result);

		TestEqual(TEXT("all three of our jobs listed"), Listed.Num(), 3);
		TestEqual(TEXT("count matches the array length"), static_cast<int32>(GetNumber(Result, TEXT("count"))), Listed.Num());
		TestTrue(TEXT("total_count covers at least our jobs"), GetNumber(Result, TEXT("total_count")) >= 3.0);
		TestTrue(TEXT("running_count counts our running job"), GetNumber(Result, TEXT("running_count")) >= 1.0);

		if (Listed.Num() == 3)
		{
			// GetAllJobs sorts by serial, so listing order is issue order.
			TestEqual(TEXT("listed oldest-issued first (1/3)"), GetString(Listed[0], TEXT("job_id")), RunningId);
			TestEqual(TEXT("listed oldest-issued first (2/3)"), GetString(Listed[1], TEXT("job_id")), DoneId);
			TestEqual(TEXT("listed oldest-issued first (3/3)"), GetString(Listed[2], TEXT("job_id")), FailedId);

			TestEqual(TEXT("running job state token"), GetString(Listed[0], TEXT("state")), FString(TEXT("running")));
			TestEqual(TEXT("complete job state token"), GetString(Listed[1], TEXT("state")), FString(TEXT("complete")));
			TestEqual(TEXT("errored job state token"), GetString(Listed[2], TEXT("state")), FString(TEXT("error")));

			TestFalse(TEXT("running job not marked finished"), GetBool(Listed[0], TEXT("finished")));
			TestTrue(TEXT("complete job marked finished"), GetBool(Listed[1], TEXT("finished")));
			TestEqual(TEXT("descriptive namespace echoed"), GetString(Listed[0], TEXT("namespace")), FString(TestNamespace));
			TestEqual(TEXT("descriptive action echoed"), GetString(Listed[0], TEXT("action")), FString(TEXT("list_running")));
			TestEqual(TEXT("progress message echoed"), GetString(Listed[0], TEXT("progress")), FString(TEXT("step 1")));
			TestEqual(TEXT("error message surfaced on the errored row"), GetString(Listed[2], TEXT("error_message")), FString(TEXT("list boom")));
			TestEqual(TEXT("error code surfaced on the errored row"), static_cast<int32>(GetNumber(Listed[2], TEXT("error_code"))), -32001);

			// Result payloads are suppressed by default and advertised as suppressed.
			TestFalse(TEXT("result payload omitted from a default listing"), Listed[1]->HasField(TEXT("result")));
			TestTrue(TEXT("suppression is advertised"), GetBool(Listed[1], TEXT("result_omitted")));
		}
	}

	// --- include_result=true opts the payload back in -------------------------
	{
		TSharedPtr<FJsonObject> Result = ListOwn(FString(), /*bIncludeResult=*/true);
		const TArray<TSharedPtr<FJsonObject>> Listed = GetListedJobs(Result);
		TestEqual(TEXT("same three jobs with payloads inlined"), Listed.Num(), 3);
		if (Listed.Num() == 3)
		{
			TestTrue(TEXT("completed job carries its result when asked"), Listed[1]->HasField(TEXT("result")));
			TestFalse(TEXT("result_omitted not set when the payload is present"), GetBool(Listed[1], TEXT("result_omitted")));
		}
	}

	// --- state filters --------------------------------------------------------
	{
		TestEqual(TEXT("state=running returns only the running job"), GetListedJobs(ListOwn(TEXT("running"))).Num(), 1);
		TestEqual(TEXT("state=complete returns only the completed job"), GetListedJobs(ListOwn(TEXT("complete"))).Num(), 1);
		TestEqual(TEXT("state=error returns only the errored job"), GetListedJobs(ListOwn(TEXT("error"))).Num(), 1);
		TestEqual(TEXT("state=cancelled returns none here"), GetListedJobs(ListOwn(TEXT("cancelled"))).Num(), 0);
		TestEqual(TEXT("state=finished aggregates complete+error"), GetListedJobs(ListOwn(TEXT("finished"))).Num(), 2);
		TestEqual(TEXT("state=all is the same as no filter"), GetListedJobs(ListOwn(TEXT("all"))).Num(), 3);
		// Case-insensitive by design (the token is lowercased before validation).
		TestEqual(TEXT("state filter is case-insensitive"), GetListedJobs(ListOwn(TEXT("RUNNING"))).Num(), 1);
	}

	// --- namespace filter isolates ------------------------------------------
	{
		TSharedPtr<FJsonObject> P = Params();
		P->SetStringField(TEXT("namespace"), TEXT("namespace_that_owns_nothing"));
		const FMonolithActionResult Result = FMonolithJobActions::HandleList(P);
		TestTrue(TEXT("namespace filter with no matches still succeeds"), Result.bSuccess);
		TestEqual(TEXT("namespace filter excludes foreign jobs"), GetListedJobs(Result.Result).Num(), 0);
	}

	// --- invalid state token is a clean parameter error -----------------------
	{
		TSharedPtr<FJsonObject> P = Params();
		P->SetStringField(TEXT("state"), TEXT("halfway"));
		const FMonolithActionResult Result = FMonolithJobActions::HandleList(P);
		TestFalse(TEXT("unknown state filter rejected"), Result.bSuccess);
		TestEqual(TEXT("rejection uses the invalid-params code"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestEqual(TEXT("rejection is tagged INVALID_STATE_FILTER"), GetErrorCodeTag(Result), FString(TEXT("INVALID_STATE_FILTER")));
	}

	RemoveAll({ RunningId, DoneId, FailedId });
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: jobs_poll — running, finished (complete + error), unknown id.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobActionsPollTest,
	"Monolith.JobActions.Poll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobActionsPollTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobActionsTestDetail;
	FMonolithJobManager& Manager = FMonolithJobManager::Get();

	const FString Id = Manager.CreateJob(TestNamespace, TEXT("poll_target"), TEXT("queued"));

	// --- Running -------------------------------------------------------------
	{
		const FMonolithActionResult Result = FMonolithJobActions::HandlePoll(JobIdParams(Id));
		TestTrue(TEXT("polling a running job succeeds"), Result.bSuccess);
		TestEqual(TEXT("id echoed"), GetString(Result.Result, TEXT("job_id")), Id);
		TestEqual(TEXT("state is running"), GetString(Result.Result, TEXT("state")), FString(TEXT("running")));
		TestFalse(TEXT("not marked finished"), GetBool(Result.Result, TEXT("finished")));
		TestEqual(TEXT("progress message visible"), GetString(Result.Result, TEXT("progress")), FString(TEXT("queued")));
		TestFalse(TEXT("no result payload while running"), Result.Result.IsValid() && Result.Result->HasField(TEXT("result")));
		TestFalse(TEXT("no finish timestamp while running"), Result.Result.IsValid() && Result.Result->HasField(TEXT("finished_seconds_ago")));
		TestFalse(TEXT("cancel flag clear"), GetBool(Result.Result, TEXT("cancel_requested")));
		TestTrue(TEXT("elapsed_seconds is non-negative"), GetNumber(Result.Result, TEXT("elapsed_seconds")) >= 0.0);
	}

	// Progress moves; poll reflects it.
	Manager.UpdateProgress(Id, TEXT("step 2 of 3"));
	TestEqual(TEXT("poll shows the latest progress"),
		GetString(FMonolithJobActions::HandlePoll(JobIdParams(Id)).Result, TEXT("progress")),
		FString(TEXT("step 2 of 3")));

	// --- Complete ------------------------------------------------------------
	Manager.CompleteJob(Id, MakeShared<FJsonValueObject>(MakePayload(TEXT("assets"), 12)));
	{
		const FMonolithActionResult Result = FMonolithJobActions::HandlePoll(JobIdParams(Id));
		TestTrue(TEXT("polling a finished job succeeds"), Result.bSuccess);
		TestEqual(TEXT("state is complete"), GetString(Result.Result, TEXT("state")), FString(TEXT("complete")));
		TestTrue(TEXT("marked finished"), GetBool(Result.Result, TEXT("finished")));
		TestTrue(TEXT("finish age reported"), Result.Result.IsValid() && Result.Result->HasField(TEXT("finished_seconds_ago")));

		const TSharedPtr<FJsonObject>* Payload = nullptr;
		TestTrue(TEXT("result payload delivered by poll"),
			Result.Result.IsValid() && Result.Result->TryGetObjectField(TEXT("result"), Payload) && Payload && Payload->IsValid());
		if (Payload && Payload->IsValid())
		{
			TestEqual(TEXT("payload round-trips through the wire shape"),
				static_cast<int32>(GetNumber(*Payload, TEXT("assets"))), 12);
		}
	}

	// --- Errored -------------------------------------------------------------
	const FString FailedId = Manager.CreateJob(TestNamespace, TEXT("poll_failed"));
	Manager.FailJob(FailedId, TEXT("index unavailable"), -32001);
	{
		const FMonolithActionResult Result = FMonolithJobActions::HandlePoll(JobIdParams(FailedId));
		// A FAILED job still polls SUCCESSFULLY — the failure is data, not a transport error.
		TestTrue(TEXT("polling an errored job is itself a success"), Result.bSuccess);
		TestEqual(TEXT("state is error"), GetString(Result.Result, TEXT("state")), FString(TEXT("error")));
		TestEqual(TEXT("error message carried"), GetString(Result.Result, TEXT("error_message")), FString(TEXT("index unavailable")));
		TestEqual(TEXT("error code carried"), static_cast<int32>(GetNumber(Result.Result, TEXT("error_code"))), -32001);
	}

	// --- Unknown id ----------------------------------------------------------
	{
		const FMonolithActionResult Result = FMonolithJobActions::HandlePoll(JobIdParams(TEXT("job_no_such_thing")));
		TestFalse(TEXT("polling an unknown id fails cleanly"), Result.bSuccess);
		TestEqual(TEXT("unknown id uses the invalid-params code"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestEqual(TEXT("unknown id is tagged UNKNOWN_JOB"), GetErrorCodeTag(Result), FString(TEXT("UNKNOWN_JOB")));
		TestTrue(TEXT("unknown id error names the id"), Result.ErrorMessage.Contains(TEXT("job_no_such_thing")));
	}

	// --- Missing / blank id --------------------------------------------------
	{
		const FMonolithActionResult Missing = FMonolithJobActions::HandlePoll(Params());
		TestFalse(TEXT("poll without job_id fails"), Missing.bSuccess);
		TestEqual(TEXT("missing id is tagged MISSING_JOB_ID"), GetErrorCodeTag(Missing), FString(TEXT("MISSING_JOB_ID")));

		const FMonolithActionResult Blank = FMonolithJobActions::HandlePoll(JobIdParams(TEXT("   ")));
		TestFalse(TEXT("whitespace-only job_id fails"), Blank.bSuccess);
		TestEqual(TEXT("blank id is tagged MISSING_JOB_ID"), GetErrorCodeTag(Blank), FString(TEXT("MISSING_JOB_ID")));

		// A null params object must not crash either.
		const FMonolithActionResult Null = FMonolithJobActions::HandlePoll(nullptr);
		TestFalse(TEXT("null params fails cleanly instead of crashing"), Null.bSuccess);
	}

	RemoveAll({ Id, FailedId });
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: jobs_cancel — running job cancels; finished job is an error.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobActionsCancelTest,
	"Monolith.JobActions.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobActionsCancelTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobActionsTestDetail;
	FMonolithJobManager& Manager = FMonolithJobManager::Get();

	// --- Cancelling a RUNNING job -------------------------------------------
	const FString RunningId = Manager.CreateJob(TestNamespace, TEXT("cancel_target"), TEXT("working"));
	{
		const FMonolithActionResult Result = FMonolithJobActions::HandleCancel(JobIdParams(RunningId));
		TestTrue(TEXT("cancelling a running job succeeds"), Result.bSuccess);
		TestEqual(TEXT("response reports the cancelled state"), GetString(Result.Result, TEXT("state")), FString(TEXT("cancelled")));
		TestTrue(TEXT("response reports the cooperative flag"), GetBool(Result.Result, TEXT("cancel_requested")));
		TestTrue(TEXT("response reports the job as finished"), GetBool(Result.Result, TEXT("finished")));
		TestEqual(TEXT("response echoes the id"), GetString(Result.Result, TEXT("job_id")), RunningId);

		// The manager agrees — the worker's IsCancelRequested poll will see it.
		TestTrue(TEXT("manager raised the cooperative cancel flag"), Manager.IsCancelRequested(RunningId));
		FMonolithJob Job;
		TestTrue(TEXT("cancelled job stays pollable"), Manager.GetJob(RunningId, Job));
		TestEqual(TEXT("manager state is Cancelled"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Cancelled));
	}

	// --- Cancelling the SAME job again is now a not-running error ------------
	{
		const FMonolithActionResult Result = FMonolithJobActions::HandleCancel(JobIdParams(RunningId));
		TestFalse(TEXT("double cancel rejected"), Result.bSuccess);
		TestEqual(TEXT("double cancel uses the invalid-params code"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestEqual(TEXT("double cancel tagged JOB_NOT_RUNNING"), GetErrorCodeTag(Result), FString(TEXT("JOB_NOT_RUNNING")));
	}

	// --- Cancelling an already-COMPLETED job is an error, not a silent no-op --
	const FString DoneId = Manager.CreateJob(TestNamespace, TEXT("cancel_completed"));
	Manager.CompleteJob(DoneId, MakeShared<FJsonValueObject>(MakePayload(TEXT("n"), 1)));
	{
		const FMonolithActionResult Result = FMonolithJobActions::HandleCancel(JobIdParams(DoneId));
		TestFalse(TEXT("cancelling a finished job is an error"), Result.bSuccess);
		TestEqual(TEXT("finished-job cancel tagged JOB_NOT_RUNNING"), GetErrorCodeTag(Result), FString(TEXT("JOB_NOT_RUNNING")));

		// The error payload tells the caller WHICH terminal state it hit.
		const TSharedPtr<FJsonObject>* ErrObj = nullptr;
		if (Result.ErrorData.IsValid() && Result.ErrorData->TryGetObject(ErrObj) && ErrObj && ErrObj->IsValid())
		{
			TestEqual(TEXT("error data carries the actual state"), GetString(*ErrObj, TEXT("state")), FString(TEXT("complete")));
			TestEqual(TEXT("error data echoes the id"), GetString(*ErrObj, TEXT("job_id")), DoneId);
		}

		// And the completed job is untouched by the failed cancel.
		FMonolithJob Job;
		Manager.GetJob(DoneId, Job);
		TestEqual(TEXT("failed cancel did not mutate the finished job"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Complete));
		TestFalse(TEXT("failed cancel did not raise the cancel flag"), Job.bCancelRequested);
	}

	// --- Cancelling an unknown id -------------------------------------------
	{
		const FMonolithActionResult Result = FMonolithJobActions::HandleCancel(JobIdParams(TEXT("job_never_existed")));
		TestFalse(TEXT("cancelling an unknown id fails"), Result.bSuccess);
		TestEqual(TEXT("unknown id tagged UNKNOWN_JOB, not JOB_NOT_RUNNING"), GetErrorCodeTag(Result), FString(TEXT("UNKNOWN_JOB")));
	}

	RemoveAll({ RunningId, DoneId });
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: jobs_clear — evicts finished jobs, leaves running ones alone.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobActionsClearTest,
	"Monolith.JobActions.Clear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobActionsClearTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobActionsTestDetail;
	FMonolithJobManager& Manager = FMonolithJobManager::Get();

	const FString RunningId = Manager.CreateJob(TestNamespace, TEXT("clear_running"), TEXT("still going"));
	const FString DoneId    = Manager.CreateJob(TestNamespace, TEXT("clear_done"));
	const FString FailedId  = Manager.CreateJob(TestNamespace, TEXT("clear_failed"));
	const FString CancelId  = Manager.CreateJob(TestNamespace, TEXT("clear_cancelled"));
	Manager.CompleteJob(DoneId, MakeShared<FJsonValueObject>(MakePayload(TEXT("n"), 1)));
	Manager.FailJob(FailedId, TEXT("nope"));
	Manager.CancelJob(CancelId);

	const FMonolithActionResult Result = FMonolithJobActions::HandleClear(Params());
	TestTrue(TEXT("clear succeeds"), Result.bSuccess);

	// >= 3 rather than == 3: the registry is process-wide and a stray finished job from
	// elsewhere in the session would be swept too. The per-id checks below are the signal.
	TestTrue(TEXT("clear evicted our three finished jobs"), GetNumber(Result.Result, TEXT("cleared")) >= 3.0);
	TestTrue(TEXT("running count still covers our running job"), GetNumber(Result.Result, TEXT("running")) >= 1.0);
	TestTrue(TEXT("remaining is at least the running job"), GetNumber(Result.Result, TEXT("remaining")) >= 1.0);
	// Nothing finished may survive a clear.
	TestEqual(TEXT("remaining equals running after a clear"),
		GetNumber(Result.Result, TEXT("remaining")), GetNumber(Result.Result, TEXT("running")));

	FMonolithJob Job;
	TestFalse(TEXT("completed job evicted"), Manager.GetJob(DoneId, Job));
	TestFalse(TEXT("errored job evicted"), Manager.GetJob(FailedId, Job));
	TestFalse(TEXT("cancelled job evicted"), Manager.GetJob(CancelId, Job));

	// The running job is untouched — clear must never call FMonolithJobManager::Reset().
	TestTrue(TEXT("running job survived the clear"), Manager.GetJob(RunningId, Job));
	TestEqual(TEXT("surviving job is still Running"), static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Running));
	TestTrue(TEXT("running job is still pollable through the action layer"),
		FMonolithJobActions::HandlePoll(JobIdParams(RunningId)).bSuccess);

	// A second clear with nothing finished left is a clean no-op for our jobs.
	const FMonolithActionResult Again = FMonolithJobActions::HandleClear(Params());
	TestTrue(TEXT("repeat clear succeeds"), Again.bSuccess);
	TestEqual(TEXT("repeat clear evicts nothing"), static_cast<int32>(GetNumber(Again.Result, TEXT("cleared"))), 0);
	TestTrue(TEXT("repeat clear kept the running job"), Manager.GetJob(RunningId, Job));

	RemoveAll({ RunningId });
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: the namespace is actually registered, and dispatching through the
//         registry (aliases + required-param validation) works end to end.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithJobActionsRegistrationTest,
	"Monolith.JobActions.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithJobActionsRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithJobActionsTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FString Namespace = FMonolithJobActions::GetNamespace();

	TestEqual(TEXT("namespace token is 'jobs'"), Namespace, FString(TEXT("jobs")));

	// FMonolithCoreModule::StartupModule registers these, but it returns early in a
	// commandlet and is gated on UMonolithSettings::bEnableJobs — so register on demand
	// rather than asserting on a startup path this test does not control.
	if (!Registry.HasAction(Namespace, TEXT("list")))
	{
		FMonolithJobActions::RegisterAll();
	}

	for (const TCHAR* ActionName : { TEXT("list"), TEXT("poll"), TEXT("cancel"), TEXT("clear") })
	{
		TestTrue(FString::Printf(TEXT("jobs.%s registered"), ActionName), Registry.HasAction(Namespace, ActionName));
	}

	// The namespace surfaces in enumeration, which is what monolith_discover walks.
	TestTrue(TEXT("jobs namespace is discoverable"), Registry.GetNamespaces().Contains(Namespace));
	TestEqual(TEXT("exactly four actions in the namespace"), Registry.GetActions(Namespace).Num(), 4);

	FMonolithJobManager& Manager = FMonolithJobManager::Get();
	const FString Id = Manager.CreateJob(TestNamespace, TEXT("registry_dispatch"), TEXT("queued"));

	// Dispatch through the registry with the CANONICAL key.
	{
		const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, TEXT("poll"), JobIdParams(Id));
		TestTrue(TEXT("registry dispatch reaches the poll handler"), Result.bSuccess);
		TestEqual(TEXT("dispatched poll returns the right job"), GetString(Result.Result, TEXT("job_id")), Id);
	}

	// ... and with the declared ALIAS, which the registry rewrites before dispatch.
	{
		TSharedPtr<FJsonObject> P = Params();
		P->SetStringField(TEXT("id"), Id);
		const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, TEXT("poll"), P);
		TestTrue(TEXT("'id' alias rewrites to job_id"), Result.bSuccess);
		TestEqual(TEXT("aliased poll returns the right job"), GetString(Result.Result, TEXT("job_id")), Id);
	}

	// Required-param validation happens in the registry, before the handler runs.
	{
		const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, TEXT("poll"), Params());
		TestFalse(TEXT("registry rejects poll without job_id"), Result.bSuccess);
	}

	RemoveAll({ Id });
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
