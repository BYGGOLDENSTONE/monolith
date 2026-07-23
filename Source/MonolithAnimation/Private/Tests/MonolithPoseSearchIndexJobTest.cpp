// SPDX-License-Identifier: MIT
// =============================================================================
// MonolithPoseSearchIndexJobTest.cpp
//
// Faz 1 (Docs/GOLDENSTONE_ROADMAP.md "Faz 1" item 3) — DISPATCH-CONTRACT coverage for
// `animation.rebuild_pose_search_index` after it was converted to a tick-sliced job.
//
// WHAT IS PROVEN HERE
//   1. Async by default: the action returns a `job_id` having done ZERO indexing work, and
//      the job is registered with the shared pump as a sliced job.
//   2. Cancellation before the first slice: the build is never kicked at all, and the job
//      lands in Cancelled.
//   3. `wait: true` bypasses the job system entirely and answers on the old synchronous
//      shape (`waited: true`, no `job_id`, no new job in the registry).
//   4. Fallback: when a job id would not be pollable (the `jobs` namespace is disabled) the
//      action runs inline and says so, instead of failing the call.
//
// WHAT IS *NOT* PROVEN HERE, AND WHY
//   The actual index build is not exercised end-to-end. A real build needs animation assets,
//   a populated DDC and — decisively — the editor's own FTickableGameObject tick to advance
//   FAsyncPoseSearchDatabasesManagement from Prestarted to Ended. A headless automation test
//   runs to completion inside ONE game-thread call, so no engine tick can happen between two
//   FMonolithJobManager::PumpOnce() calls; a "poll until Success" loop would spin forever.
//   Progress-message *content* past the first slice is therefore also unprovable here.
//   Live-editor verification of a real rebuild is the human acceptance step.
//
// DETERMINISM: no sleeps. Test 1 cancels before the first pump, so no slice ever calls into
// the engine indexer. Tests 3 and 4 DO make one non-blocking / one blocking engine request
// against a database with zero entries — the cheapest real request that exists.
//
// SHARED-SINGLETON HYGIENE: FMonolithJobManager is process-lifetime. Every test asserts
// RELATIVE counts, removes exactly the ids it created, and never calls Reset().
// =============================================================================

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "MonolithJobManager.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"
#include "MonolithPoseSearchActions.h"

#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchSchema.h"

namespace MonolithPoseSearchIndexJobTest
{
	static const TCHAR* const ActionNamespace = TEXT("animation");
	static const TCHAR* const ActionName = TEXT("rebuild_pose_search_index");

	/** Root for the disposable in-memory fixtures. Never saved to disk. */
	static const TCHAR* const FixtureRoot = TEXT("/Game/Tests/Monolith/PoseSearchIndexJob");

	/**
	 * RAII override of the job retention policy. PumpOnce() also sweeps retention, so without
	 * this a pump could evict the job under test before the assertions read it.
	 */
	struct FScopedRetentionSettings
	{
		UMonolithSettings* Settings = nullptr;
		int32 OldCount = 0;
		float OldSeconds = 0.0f;

		FScopedRetentionSettings()
			: Settings(GetMutableDefault<UMonolithSettings>())
		{
			if (Settings)
			{
				OldCount = Settings->JobRetentionCount;
				OldSeconds = Settings->JobRetentionSeconds;
				Settings->JobRetentionCount = 1024;
				Settings->JobRetentionSeconds = 0.0f;   // 0 disables age eviction
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

		FScopedRetentionSettings(const FScopedRetentionSettings&) = delete;
		FScopedRetentionSettings& operator=(const FScopedRetentionSettings&) = delete;
	};

	/** RAII override of the jobs-namespace toggle, used to force the inline fallback. */
	struct FScopedJobsEnabled
	{
		UMonolithSettings* Settings = nullptr;
		bool bOld = true;

		explicit FScopedJobsEnabled(bool bNewValue)
			: Settings(GetMutableDefault<UMonolithSettings>())
		{
			if (Settings)
			{
				bOld = Settings->bEnableJobs;
				Settings->bEnableJobs = bNewValue;
			}
		}

		~FScopedJobsEnabled()
		{
			if (Settings)
			{
				Settings->bEnableJobs = bOld;
			}
		}

		FScopedJobsEnabled(const FScopedJobsEnabled&) = delete;
		FScopedJobsEnabled& operator=(const FScopedJobsEnabled&) = delete;
	};

	/**
	 * Build a disposable, resolvable UPoseSearchDatabase entirely in memory.
	 *
	 * The package is created but NEVER saved: FMonolithAssetUtils::LoadAssetByPath tier 3
	 * (FindPackage + FindObject) resolves freshly-created unsaved assets, which is exactly the
	 * lookup the action performs. FullyLoad() follows the repo-wide CreatePackage rule so a
	 * later save of the same path (by any other test) cannot hit the partial-load fatal.
	 *
	 * The database deliberately has ZERO animation assets: every test here is about DISPATCH,
	 * and an empty database is the cheapest thing the engine indexer can be handed.
	 *
	 * Returns the object path to feed to `asset_path`, or an empty string on failure.
	 */
	static FString CreateDisposableDatabase(const FString& AssetName)
	{
		USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!Skeleton)
		{
			return FString();
		}
		{
			FReferenceSkeletonModifier Modifier(Skeleton);
			Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
			Modifier.Add(FMeshBoneInfo(FName(TEXT("pelvis")), TEXT("pelvis"), 0), FTransform::Identity);
		}

		const FString PackagePath = FString::Printf(TEXT("%s/%s"), FixtureRoot, *AssetName);
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return FString();
		}
		Package->FullyLoad();

		UPoseSearchSchema* Schema = NewObject<UPoseSearchSchema>(
			Package, FName(*(AssetName + TEXT("_Schema"))), RF_Public | RF_Standalone);
		if (!Schema)
		{
			return FString();
		}
		Schema->AddSkeleton(Skeleton);

		UPoseSearchDatabase* Database = NewObject<UPoseSearchDatabase>(
			Package, FName(*AssetName), RF_Public | RF_Standalone);
		if (!Database)
		{
			return FString();
		}
		Database->Schema = Schema;

		// Nothing here is ever written to disk; keep the package out of the save prompt.
		Package->SetDirtyFlag(false);

		return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
	}

	/** Register the PoseSearch actions on demand — module startup returns early in a commandlet. */
	static FMonolithToolRegistry& EnsureRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(ActionNamespace, ActionName))
		{
			FMonolithPoseSearchActions::RegisterActions(Registry);
		}
		return Registry;
	}

	static FMonolithActionResult Rebuild(const FString& AssetPath, bool bSetWait, bool bWaitValue)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (bSetWait)
		{
			Params->SetBoolField(TEXT("wait"), bWaitValue);
		}
		return EnsureRegistered().ExecuteAction(ActionNamespace, ActionName, Params);
	}

	/**
	 * Tolerate whatever the ENGINE logs about our disposable fixture.
	 *
	 * Tests 3 and 4 deliberately let the real engine indexer see a database with a bare
	 * schema and zero entries, so it answers "BuildIndex Failed because of invalid Schema" at
	 * Error verbosity — and the automation framework fails any test that emits an Error.
	 * The engine's BUILD VERDICT is explicitly not under test here (the contract under test is
	 * dispatch: which path ran, and what the response looks like), so errors naming this
	 * fixture are silently ignored. Occurrences < 0 means "tolerate, do not require", so the
	 * tests keep passing if a future engine build of an empty database stops complaining.
	 * The toleration is scoped to the fixture's own name: an error about anything else still
	 * fails the test.
	 */
	static void TolerateEngineBuildErrorsFor(FAutomationTestBase& Test, const TCHAR* FixtureName)
	{
		Test.AddExpectedErrorPlain(FixtureName, EAutomationExpectedErrorFlags::Contains, /*Occurrences=*/ -1);
	}

	static FString GetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Out;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(Field, Out);
		}
		return Out;
	}

	static bool GetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool bDefault = false)
	{
		bool Out = bDefault;
		if (Obj.IsValid())
		{
			Obj->TryGetBoolField(Field, Out);
		}
		return Out;
	}
}

// ---------------------------------------------------------------------------
// Test 1: async is the DEFAULT. The action returns a job id having done no indexing,
//         the job is a sliced job on the shared pump, and cancelling it before the
//         first slice means the engine indexer is never touched at all.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchIndexJobAsyncDefaultTest,
	"Monolith.PoseSearchIndexJob.AsyncByDefaultDispatchesJob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchIndexJobAsyncDefaultTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexJobTest;

	const FString AssetPath = CreateDisposableDatabase(TEXT("PSDB_JobAsyncDefault"));
	if (AssetPath.IsEmpty())
	{
		AddError(TEXT("Could not build the disposable PoseSearch database fixture."));
		return false;
	}

	FMonolithJobManager& Manager = FMonolithJobManager::Get();
	FScopedRetentionSettings RetentionGuard;

	const int32 SlicedBefore = Manager.GetSlicedJobCount();
	const int32 JobsBefore = Manager.GetJobCount();

	const double Start = FPlatformTime::Seconds();
	const FMonolithActionResult Result = Rebuild(AssetPath, /*bSetWait=*/false, /*bWaitValue=*/false);
	const double ElapsedSeconds = FPlatformTime::Seconds() - Start;
	AddInfo(FString::Printf(TEXT("Dispatch returned in %.4f s."), ElapsedSeconds));

	TestTrue(TEXT("rebuild_pose_search_index succeeded"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	const FString JobId = GetString(Result.Result, TEXT("job_id"));
	TestEqual(TEXT("the default mode is async"), GetString(Result.Result, TEXT("mode")), FString(TEXT("async")));
	TestFalse(TEXT("a job id was handed back"), JobId.IsEmpty());
	TestFalse(TEXT("the async response reports it did not wait"), GetBool(Result.Result, TEXT("waited"), true));
	TestEqual(TEXT("the response names the originating namespace"),
		GetString(Result.Result, TEXT("job_namespace")), FString(TEXT("animation")));
	TestEqual(TEXT("the response names the originating action"),
		GetString(Result.Result, TEXT("job_action")), FString(TEXT("rebuild_pose_search_index")));
	TestTrue(TEXT("the message tells the caller how to poll"),
		GetString(Result.Result, TEXT("message")).Contains(TEXT("jobs_query")));
	TestFalse(TEXT("the async response carries no synchronous build result"),
		Result.Result->HasField(TEXT("result")));

	// Registry side effects: exactly one new job, registered as a SLICED job.
	TestEqual(TEXT("exactly one job was created"), Manager.GetJobCount(), JobsBefore + 1);
	TestEqual(TEXT("the job is registered with the shared pump as a sliced job"),
		Manager.GetSlicedJobCount(), SlicedBefore + 1);

	FMonolithJob Job;
	TestTrue(TEXT("the new job is pollable"), Manager.GetJob(JobId, Job));
	TestEqual(TEXT("the job is Running"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Running));
	TestEqual(TEXT("the job records the originating namespace"), Job.Namespace, FString(TEXT("animation")));
	TestEqual(TEXT("the job records the originating action"), Job.Action, FString(TEXT("rebuild_pose_search_index")));

	// THE non-blocking proof: the first slice runs on the NEXT pump, never inline, so the
	// progress message is still the queued text and no engine indexing has happened yet.
	TestTrue(TEXT("no slice ran inline — progress is still the queued message"),
		Job.ProgressMessage.StartsWith(TEXT("Queued:")));
	TestTrue(TEXT("the queued message names the database"),
		Job.ProgressMessage.Contains(TEXT("PSDB_JobAsyncDefault")));

	// Cancel BEFORE the first pump: the slice bails at its cancellation checkpoint, so the
	// engine index build is never requested. This keeps the test free of any real DDC work.
	TestTrue(TEXT("cancel accepted while the job is queued"), Manager.CancelJob(JobId));
	Manager.PumpOnce();

	TestTrue(TEXT("the cancelled job is still pollable"), Manager.GetJob(JobId, Job));
	TestEqual(TEXT("the job landed in Cancelled"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Cancelled));
	TestTrue(TEXT("the cooperative cancel flag stayed raised"), Job.bCancelRequested);
	TestFalse(TEXT("no result payload on the cancelled path"), Job.Result.IsValid());
	TestEqual(TEXT("the sliced step was deregistered from the pump"),
		Manager.GetSlicedJobCount(), SlicedBefore);

	Manager.PumpOnce();
	TestTrue(TEXT("a second pump does not resurrect or re-slice the job"), Manager.GetJob(JobId, Job));
	TestEqual(TEXT("still Cancelled after another pump"),
		static_cast<int32>(Job.State), static_cast<int32>(EMonolithJobState::Cancelled));

	Manager.RemoveJob(JobId);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: the async response is a pure dispatch — it must not pretend to carry
//         build results, and a bad asset path must still fail BEFORE any job is
//         created (a job id for a database that does not exist would be a lie).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchIndexJobUnknownAssetTest,
	"Monolith.PoseSearchIndexJob.UnknownAssetCreatesNoJob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchIndexJobUnknownAssetTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexJobTest;

	FMonolithJobManager& Manager = FMonolithJobManager::Get();
	const int32 JobsBefore = Manager.GetJobCount();
	const int32 SlicedBefore = Manager.GetSlicedJobCount();

	const FMonolithActionResult Result = Rebuild(
		TEXT("/Game/Tests/Monolith/PoseSearchIndexJob/PSDB_DoesNotExist.PSDB_DoesNotExist"),
		/*bSetWait=*/false, /*bWaitValue=*/false);

	TestFalse(TEXT("an unknown database is still a clean synchronous error"), Result.bSuccess);
	TestTrue(TEXT("the error names the missing database"),
		Result.ErrorMessage.Contains(TEXT("PoseSearchDatabase not found")));
	TestEqual(TEXT("no job was created for a request that cannot run"),
		Manager.GetJobCount(), JobsBefore);
	TestEqual(TEXT("no sliced step was registered for a request that cannot run"),
		Manager.GetSlicedJobCount(), SlicedBefore);

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: wait=true is the documented sync opt-out. It must bypass the job
//         system completely and answer on the pre-Faz-1 shape.
//
//         This DOES call the engine indexer (blocking) — against a database with
//         zero animation assets, which is the cheapest real request available.
//         The engine's verdict (Success / Failed) is deliberately not asserted:
//         the contract under test is the DISPATCH, not the build outcome.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchIndexJobSyncOptOutTest,
	"Monolith.PoseSearchIndexJob.WaitFlagBypassesJobSystem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchIndexJobSyncOptOutTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexJobTest;

	TolerateEngineBuildErrorsFor(*this, TEXT("PSDB_JobSyncOptOut"));

	const FString AssetPath = CreateDisposableDatabase(TEXT("PSDB_JobSyncOptOut"));
	if (AssetPath.IsEmpty())
	{
		AddError(TEXT("Could not build the disposable PoseSearch database fixture."));
		return false;
	}

	FMonolithJobManager& Manager = FMonolithJobManager::Get();
	const int32 JobsBefore = Manager.GetJobCount();
	const int32 SlicedBefore = Manager.GetSlicedJobCount();

	const FMonolithActionResult Result = Rebuild(AssetPath, /*bSetWait=*/true, /*bWaitValue=*/true);

	TestTrue(TEXT("the synchronous path succeeded"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("mode reports sync"), GetString(Result.Result, TEXT("mode")), FString(TEXT("sync")));
	TestTrue(TEXT("waited is true, as it was before Faz 1"), GetBool(Result.Result, TEXT("waited")));
	TestFalse(TEXT("no job id is handed back on the synchronous path"),
		Result.Result->HasField(TEXT("job_id")));
	TestTrue(TEXT("the pre-Faz-1 `result` field is still present"),
		Result.Result->HasField(TEXT("result")));
	TestTrue(TEXT("the pre-Faz-1 `total_poses` field is still present"),
		Result.Result->HasField(TEXT("total_poses")));
	TestEqual(TEXT("the response echoes the asset path"),
		GetString(Result.Result, TEXT("asset_path")), AssetPath);
	AddInfo(FString::Printf(TEXT("Engine build verdict on an empty database: %s"),
		*GetString(Result.Result, TEXT("result"))));

	TestEqual(TEXT("wait=true created no job"), Manager.GetJobCount(), JobsBefore);
	TestEqual(TEXT("wait=true registered no sliced step"), Manager.GetSlicedJobCount(), SlicedBefore);

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: when a job id would not be pollable the action FALLS BACK to running
//         inline rather than failing the call, and reports why.
//
//         The trigger used here is the jobs-namespace toggle: with bEnableJobs
//         off the `jobs` namespace is not registered, so a job id could never be
//         polled. It is the one refusal reason a test can produce without calling
//         FMonolithJobManager::Reset(), which would drop jobs owned by the rest of
//         the session. The OTHER refusal reason (manager shutting down) reaches
//         exactly the same fallback branch.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchIndexJobFallbackTest,
	"Monolith.PoseSearchIndexJob.UnstartableJobFallsBackInline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchIndexJobFallbackTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexJobTest;

	TolerateEngineBuildErrorsFor(*this, TEXT("PSDB_JobFallback"));

	const FString AssetPath = CreateDisposableDatabase(TEXT("PSDB_JobFallback"));
	if (AssetPath.IsEmpty())
	{
		AddError(TEXT("Could not build the disposable PoseSearch database fixture."));
		return false;
	}

	FMonolithJobManager& Manager = FMonolithJobManager::Get();
	const int32 JobsBefore = Manager.GetJobCount();
	const int32 SlicedBefore = Manager.GetSlicedJobCount();

	FMonolithActionResult Result;
	{
		FScopedJobsEnabled JobsOff(false);
		Result = Rebuild(AssetPath, /*bSetWait=*/false, /*bWaitValue=*/false);
	}

	TestTrue(TEXT("the call succeeds instead of failing when no job can be started"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("the fallback runs the synchronous path"),
		GetString(Result.Result, TEXT("mode")), FString(TEXT("sync")));
	TestFalse(TEXT("no job id is invented for an unstartable job"),
		Result.Result->HasField(TEXT("job_id")));
	TestFalse(TEXT("job_started is explicitly false"), GetBool(Result.Result, TEXT("job_started"), true));
	TestFalse(TEXT("the fallback explains itself"),
		GetString(Result.Result, TEXT("job_fallback_reason")).IsEmpty());
	TestFalse(TEXT("the fallback did not block — waited stays false"),
		GetBool(Result.Result, TEXT("waited"), true));
	TestTrue(TEXT("the fallback still answers on the synchronous shape"),
		Result.Result->HasField(TEXT("result")));

	TestEqual(TEXT("no job was created on the fallback path"), Manager.GetJobCount(), JobsBefore);
	TestEqual(TEXT("no sliced step was registered on the fallback path"),
		Manager.GetSlicedJobCount(), SlicedBefore);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
