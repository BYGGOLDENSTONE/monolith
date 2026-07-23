// SPDX-License-Identifier: MIT
// Faz 1 — `jobs` namespace implementation. Header (incl. placement rationale):
// Private/MonolithJobActions.h
//
// SCOPE: this file only EXPOSES the registry. Nothing here starts real work — converting
// blocking actions (PoseSearch build_search_index, source reindex, ...) into jobs is the
// next Faz 1 step and lives with those actions, not here.

#include "MonolithJobActions.h"

#include "MonolithJobManager.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"

namespace MonolithJobActionsDetail
{
	/** Namespace token — declared once, used by registration AND module shutdown. */
	static const TCHAR* NamespaceToken = TEXT("jobs");

	/** Canonical param key for the job identifier (aliases are rewritten by the registry). */
	static const TCHAR* ParamJobId = TEXT("job_id");

	/**
	 * `state` filter tokens for jobs_list. The four concrete states reuse
	 * LexMonolithJobState so the filter vocabulary can never drift from the wire
	 * vocabulary; "finished" and "all" are the two aggregate tokens.
	 */
	static const TCHAR* FilterAll = TEXT("all");
	static const TCHAR* FilterFinished = TEXT("finished");

	static TArray<FString> GetStateFilterOptions()
	{
		TArray<FString> Options;
		Options.Add(FilterAll);
		Options.Add(FilterFinished);
		for (const EMonolithJobState State : {
				EMonolithJobState::Running,
				EMonolithJobState::Complete,
				EMonolithJobState::Error,
				EMonolithJobState::Cancelled })
		{
			Options.Add(LexMonolithJobState(State));
		}
		return Options;
	}

	static FString JoinOptions(const TArray<FString>& Options)
	{
		return FString::Join(Options, TEXT(", "));
	}

	/** True when Job passes the (already validated, lowercase) state filter token. */
	static bool PassesStateFilter(const FMonolithJob& Job, const FString& Filter)
	{
		if (Filter.IsEmpty() || Filter == FilterAll)
		{
			return true;
		}
		if (Filter == FilterFinished)
		{
			return Job.IsFinished();
		}
		return Filter == LexMonolithJobState(Job.State);
	}

	/** Shared "no such job" failure — the one place the UNKNOWN_JOB shape is built. */
	static FMonolithActionResult UnknownJobError(const FString& JobId)
	{
		TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
		ErrData->SetStringField(TEXT("error_code"), TEXT("UNKNOWN_JOB"));
		ErrData->SetStringField(ParamJobId, JobId);
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("No job with id '%s'. It never existed, or it finished and was already evicted by the retention policy. Call jobs_query(\"list\") for live job ids."),
				*JobId),
			FMonolithJsonUtils::ErrInvalidParams
		).WithErrorData(ErrData);
	}

	/** Read + validate the required job_id param. Returns false with OutError populated. */
	static bool ReadJobIdParam(const TSharedPtr<FJsonObject>& Params, FString& OutJobId, FMonolithActionResult& OutError)
	{
		OutJobId.Reset();
		if (Params.IsValid())
		{
			Params->TryGetStringField(ParamJobId, OutJobId);
		}
		OutJobId.TrimStartAndEndInline();

		if (OutJobId.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
			ErrData->SetStringField(TEXT("error_code"), TEXT("MISSING_JOB_ID"));
			OutError = FMonolithActionResult::Error(
				TEXT("Missing required parameter 'job_id'. Call jobs_query(\"list\") to see live job ids."),
				FMonolithJsonUtils::ErrInvalidParams
			).WithErrorData(ErrData);
			return false;
		}
		return true;
	}
}

const TCHAR* FMonolithJobActions::GetNamespace()
{
	return MonolithJobActionsDetail::NamespaceToken;
}

// ============================================================================
// Serialization — ONE job shape, shared by list / poll / cancel.
// ============================================================================

TSharedPtr<FJsonObject> FMonolithJobActions::JobToJson(const FMonolithJob& Job, bool bIncludeResult)
{
	const double Now = FPlatformTime::Seconds();

	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(MonolithJobActionsDetail::ParamJobId, Job.Id);
	Obj->SetNumberField(TEXT("serial"), static_cast<double>(Job.Serial));
	Obj->SetStringField(TEXT("namespace"), Job.Namespace);
	Obj->SetStringField(TEXT("action"), Job.Action);
	// State token comes from LexMonolithJobState so poll/list/cancel can never disagree.
	Obj->SetStringField(TEXT("state"), LexMonolithJobState(Job.State));
	Obj->SetBoolField(TEXT("finished"), Job.IsFinished());
	Obj->SetStringField(TEXT("progress"), Job.ProgressMessage);
	Obj->SetBoolField(TEXT("cancel_requested"), Job.bCancelRequested);

	// Durations, not raw stamps: FPlatformTime::Seconds() is a process-relative monotonic
	// clock, so the absolute values are meaningless to an MCP client. Deltas are not.
	const double EndTime = Job.IsFinished() ? Job.FinishedTimeSeconds : Now;
	Obj->SetNumberField(TEXT("elapsed_seconds"), FMath::Max(0.0, EndTime - Job.CreatedTimeSeconds));
	if (Job.IsFinished())
	{
		Obj->SetNumberField(TEXT("finished_seconds_ago"), FMath::Max(0.0, Now - Job.FinishedTimeSeconds));
	}

	if (Job.State == EMonolithJobState::Error)
	{
		Obj->SetStringField(TEXT("error_message"), Job.ErrorMessage);
		Obj->SetNumberField(TEXT("error_code"), Job.ErrorCode);
	}

	// `result` exists only on the Complete path. list omits it by default (a page of
	// full payloads can be huge); poll/cancel always carry it.
	if (bIncludeResult && Job.Result.IsValid())
	{
		Obj->SetField(TEXT("result"), Job.Result);
	}
	else if (Job.Result.IsValid())
	{
		Obj->SetBoolField(TEXT("result_omitted"), true);
	}

	return Obj;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithJobActions::RegisterAll()
{
	using namespace MonolithJobActionsDetail;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FString Namespace = NamespaceToken;

	const FString StateOptions = JoinOptions(GetStateFilterOptions());

	Registry.RegisterAction(Namespace, TEXT("list"),
		TEXT("List every job the server is tracking (running jobs plus finished jobs still inside the retention window), oldest-issued first. "
			 "Result payloads are omitted by default; poll a single job for its result."),
		FMonolithActionHandler::CreateStatic(&FMonolithJobActions::HandleList),
		FParamSchemaBuilder()
			.Optional(TEXT("state"), TEXT("string"),
				FString::Printf(TEXT("Filter by lifecycle state. One of: %s. Default 'all'."), *StateOptions),
				FString(FilterAll))
			.Optional(TEXT("namespace"), TEXT("string"),
				TEXT("Filter to jobs issued by this namespace (case-insensitive exact match, e.g. 'animation')."))
			.Optional(TEXT("include_result"), TEXT("boolean"),
				TEXT("Inline each completed job's full result payload. Default false; jobs with a suppressed payload are marked result_omitted=true."),
				TEXT("false"))
			.Build());
	// Read-only enumeration of server state.
	Registry.SetActionAnnotations(Namespace, TEXT("list"),
		/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true,
		TEXT("List Monolith jobs"));

	Registry.RegisterAction(Namespace, TEXT("poll"),
		TEXT("Get the current state of one job: progress message while running, result payload once complete, error message/code on failure. "
			 "A job id that never existed (or was already evicted by the retention policy) is a clean error, never a crash."),
		FMonolithActionHandler::CreateStatic(&FMonolithJobActions::HandlePoll),
		FParamSchemaBuilder()
			.Required(ParamJobId, TEXT("string"),
				TEXT("Job identifier returned when the work was queued (e.g. 'job_12')."),
				{ TEXT("id"), TEXT("jobid") })
			.Build());
	// Pure read of job state.
	Registry.SetActionAnnotations(Namespace, TEXT("poll"),
		/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true,
		TEXT("Poll a Monolith job"));

	Registry.RegisterAction(Namespace, TEXT("cancel"),
		TEXT("Request cancellation of a RUNNING job. The job moves to 'cancelled' immediately and its cooperative cancel flag is raised; "
			 "the worker stops at its next checkpoint. Cancelling a job that already finished is an error: terminal states are final."),
		FMonolithActionHandler::CreateStatic(&FMonolithJobActions::HandleCancel),
		FParamSchemaBuilder()
			.Required(ParamJobId, TEXT("string"),
				TEXT("Job identifier of a RUNNING job (e.g. 'job_12')."),
				{ TEXT("id"), TEXT("jobid") })
			.Build());
	// Destroys in-flight work, and a second cancel of the same job is an error, so
	// neither read-only nor idempotent. Honest values per the annotation guidance.
	Registry.SetActionAnnotations(Namespace, TEXT("cancel"),
		/*bReadOnly=*/false, /*bDestructive=*/true, /*bIdempotent=*/false,
		TEXT("Cancel a Monolith job"));

	Registry.RegisterAction(Namespace, TEXT("clear"),
		TEXT("Evict every FINISHED job (complete / error / cancelled) from the registry, freeing their result payloads. "
			 "Running jobs are never touched and stay pollable."),
		FMonolithActionHandler::CreateStatic(&FMonolithJobActions::HandleClear));
	// Drops retained results (destructive of history) but re-running it is a no-op.
	Registry.SetActionAnnotations(Namespace, TEXT("clear"),
		/*bReadOnly=*/false, /*bDestructive=*/true, /*bIdempotent=*/true,
		TEXT("Clear finished Monolith jobs"));

	// Dispatcher-level annotations are DELIBERATELY not set: siblings disagree
	// (list/poll are read-only, cancel/clear are destructive), which is exactly the
	// case FMonolithToolRegistry::GetDispatcherAnnotations documents as "leave defaulted".
}

// ============================================================================
// jobs_list
// ============================================================================

FMonolithActionResult FMonolithJobActions::HandleList(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithJobActionsDetail;

	FString StateFilter;
	FString NamespaceFilter;
	bool bIncludeResult = false;

	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("state"), StateFilter);
		Params->TryGetStringField(TEXT("namespace"), NamespaceFilter);
		Params->TryGetBoolField(TEXT("include_result"), bIncludeResult);
	}

	StateFilter = StateFilter.TrimStartAndEnd().ToLower();
	NamespaceFilter.TrimStartAndEndInline();

	if (!StateFilter.IsEmpty())
	{
		const TArray<FString> Options = GetStateFilterOptions();
		if (!Options.Contains(StateFilter))
		{
			TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
			ErrData->SetStringField(TEXT("error_code"), TEXT("INVALID_STATE_FILTER"));
			TArray<TSharedPtr<FJsonValue>> OptionValues;
			for (const FString& Option : Options)
			{
				OptionValues.Add(MakeShared<FJsonValueString>(Option));
			}
			ErrData->SetArrayField(TEXT("valid_options"), OptionValues);
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown state filter '%s'. Valid values: %s."), *StateFilter, *JoinOptions(Options)),
				FMonolithJsonUtils::ErrInvalidParams
			).WithErrorData(ErrData);
		}
	}

	FMonolithJobManager& Manager = FMonolithJobManager::Get();
	const TArray<FMonolithJob> AllJobs = Manager.GetAllJobs();   // already oldest-issued first

	TArray<TSharedPtr<FJsonValue>> JobValues;
	JobValues.Reserve(AllJobs.Num());
	int32 RunningCount = 0;

	for (const FMonolithJob& Job : AllJobs)
	{
		if (!Job.IsFinished())
		{
			++RunningCount;
		}
		if (!PassesStateFilter(Job, StateFilter))
		{
			continue;
		}
		if (!NamespaceFilter.IsEmpty() && !Job.Namespace.Equals(NamespaceFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		JobValues.Add(MakeShared<FJsonValueObject>(JobToJson(Job, bIncludeResult)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("jobs"), JobValues);
	Result->SetNumberField(TEXT("count"), JobValues.Num());
	Result->SetNumberField(TEXT("total_count"), AllJobs.Num());
	Result->SetNumberField(TEXT("running_count"), RunningCount);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// jobs_poll
// ============================================================================

FMonolithActionResult FMonolithJobActions::HandlePoll(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithJobActionsDetail;

	FString JobId;
	FMonolithActionResult ParamError;
	if (!ReadJobIdParam(Params, JobId, ParamError))
	{
		return ParamError;
	}

	FMonolithJob Job;
	if (!FMonolithJobManager::Get().GetJob(JobId, Job))
	{
		return UnknownJobError(JobId);
	}

	// Poll is the single-job read, so it always carries the payload.
	return FMonolithActionResult::Success(JobToJson(Job, /*bIncludeResult=*/true));
}

// ============================================================================
// jobs_cancel
// ============================================================================

FMonolithActionResult FMonolithJobActions::HandleCancel(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithJobActionsDetail;

	FString JobId;
	FMonolithActionResult ParamError;
	if (!ReadJobIdParam(Params, JobId, ParamError))
	{
		return ParamError;
	}

	FMonolithJobManager& Manager = FMonolithJobManager::Get();

	if (Manager.CancelJob(JobId))
	{
		FMonolithJob Job;
		Manager.GetJob(JobId, Job);   // cannot fail immediately after a successful cancel
		return FMonolithActionResult::Success(JobToJson(Job, /*bIncludeResult=*/true));
	}

	// CancelJob returns false for BOTH "unknown id" and "already terminal" — split the two
	// so the caller gets an actionable message. Re-reading also covers the race where the
	// job finished between the cancel attempt and this lookup.
	FMonolithJob Job;
	if (!Manager.GetJob(JobId, Job))
	{
		return UnknownJobError(JobId);
	}

	TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
	ErrData->SetStringField(TEXT("error_code"), TEXT("JOB_NOT_RUNNING"));
	ErrData->SetStringField(ParamJobId, Job.Id);
	ErrData->SetStringField(TEXT("state"), LexMonolithJobState(Job.State));
	return FMonolithActionResult::Error(
		FString::Printf(
			TEXT("Job '%s' is not running (state='%s'), so it cannot be cancelled; a finished job is final. Poll it for its result."),
			*Job.Id, LexMonolithJobState(Job.State)),
		FMonolithJsonUtils::ErrInvalidParams
	).WithErrorData(ErrData);
}

// ============================================================================
// jobs_clear
// ============================================================================

FMonolithActionResult FMonolithJobActions::HandleClear(const TSharedPtr<FJsonObject>& /*Params*/)
{
	FMonolithJobManager& Manager = FMonolithJobManager::Get();

	// ClearFinishedJobs, never Reset(): Reset() would also drop RUNNING jobs and uninstall
	// the shared pump, orphaning live workers. `clear` is history maintenance only.
	const int32 Cleared = Manager.ClearFinishedJobs();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("cleared"), Cleared);
	Result->SetNumberField(TEXT("remaining"), Manager.GetJobCount());
	Result->SetNumberField(TEXT("running"), Manager.GetRunningJobCount());
	return FMonolithActionResult::Success(Result);
}
