// SPDX-License-Identifier: MIT
// =============================================================================
// MonolithPoseSearchIndexReadTest.cpp
//
// 2026-07-24 (Faz 1 follow-up) — READ-CONTRACT coverage for the two PoseSearch reads that
// used to freeze the editor: `animation.get_database_stats` and
// `animation.validate_pose_search_database`.
//
// Both used to ask the engine for the search-index state with
//   ERequestAsyncBuildFlag::ContinueRequest | ERequestAsyncBuildFlag::WaitForCompletion
// which blocks the game thread — and therefore the whole single-threaded MCP server — for the
// length of an index build (minutes on a real database). They are READS, so they were NOT
// converted into jobs: the default now drops WaitForCompletion, answers about the world as it
// is, and marks the answer incomplete when a build is in flight. `wait: true` (same spelling
// and default as `rebuild_pose_search_index`) restores the old blocking read.
//
// WHAT IS PROVEN HERE
//   1. Both reads answer with the full status block, and the status fields are internally
//      consistent (built / building / failed / no_schema are mutually exclusive, and the
//      index-dependent fields are null/false unless the index is actually built).
//   2. The default sets `waited: false`; `wait: true` sets `waited: true` and takes the old
//      blocking path. The flag is honoured on BOTH actions with the same spelling.
//   3. A database with no schema is answered WITHOUT ever calling into the engine indexer:
//      `index_status: "no_schema"`, no build started, no pose counts invented. This is the
//      one fixture whose status is fully deterministic headless.
//   4. Discoverability: while a Monolith rebuild job is running for a database, an incomplete
//      read names THAT job in `index_build_job_id`; once the job finishes, the field prunes
//      itself back to null. A read never points at a job belonging to another database.
//   5. `validate_pose_search_database` distinguishes "no problems found" from "did not finish
//      looking" via `validation_complete`, and an in-flight build is NOT reported as a
//      validation failure.
//   6. Error paths stay clean on both actions: an unknown database path is a plain error, and
//      no job is created or named.
//
// WHAT IS *NOT* PROVEN HERE, AND WHY
//   That a read taken DURING a real, long, in-flight index build returns promptly with
//   `index_status: "building"`. Driving FAsyncPoseSearchDatabasesManagement from Prestarted to
//   Ended needs the editor's own FTickableGameObject tick, and an automation test runs to
//   completion inside ONE game-thread call — no engine tick can happen between two of our
//   calls, so a real build can neither be completed nor held open on demand. Whatever status
//   the engine happens to report for a disposable fixture is therefore treated as an input,
//   not as an expectation: every assertion below holds for ANY status the engine returns.
//   Live-editor verification against a real motion-matching database is the human step.
//
// DETERMINISM: no sleeps, no wall-clock waits. Test 4 dispatches a rebuild job and never
// pumps it, so no slice runs and the engine indexer is never driven by us.
//
// SHARED-SINGLETON HYGIENE: FMonolithJobManager is process-lifetime. Assertions are relative,
// every created job is removed again, and Reset() is never called.
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
#include "MonolithToolRegistry.h"
#include "MonolithPoseSearchActions.h"

#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchSchema.h"

namespace MonolithPoseSearchIndexReadTest
{
	static const TCHAR* const ActionNamespace = TEXT("animation");
	static const TCHAR* const StatsAction     = TEXT("get_database_stats");
	static const TCHAR* const ValidateAction  = TEXT("validate_pose_search_database");
	static const TCHAR* const RebuildAction   = TEXT("rebuild_pose_search_index");

	/** Root for the disposable in-memory fixtures. Never saved to disk. */
	static const TCHAR* const FixtureRoot = TEXT("/Game/Tests/Monolith/PoseSearchIndexRead");

	/**
	 * Build a disposable UPoseSearchDatabase entirely in memory.
	 *
	 * Deliberately NOT shared with MonolithPoseSearchIndexJobTest.cpp: this file needs a second
	 * shape (a database with NO schema — the only fixture whose index status is deterministic
	 * headless), and destabilising a green test file to hoist a 30-line helper is a bad trade.
	 *
	 * The package is created but never saved; FMonolithAssetUtils::LoadAssetByPath resolves
	 * freshly-created unsaved assets, which is the lookup the actions perform. FullyLoad()
	 * follows the repo-wide CreatePackage rule so a later save of the same path cannot hit the
	 * partial-load fatal.
	 *
	 * Returns the object path to feed to the action, or an empty string on failure.
	 */
	static FString CreateDisposableDatabase(const FString& AssetName, bool bWithSchema)
	{
		const FString PackagePath = FString::Printf(TEXT("%s/%s"), FixtureRoot, *AssetName);
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return FString();
		}
		Package->FullyLoad();

		UPoseSearchDatabase* Database = NewObject<UPoseSearchDatabase>(
			Package, FName(*AssetName), RF_Public | RF_Standalone);
		if (!Database)
		{
			return FString();
		}

		if (bWithSchema)
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

			UPoseSearchSchema* Schema = NewObject<UPoseSearchSchema>(
				Package, FName(*(AssetName + TEXT("_Schema"))), RF_Public | RF_Standalone);
			if (!Schema)
			{
				return FString();
			}
			Schema->AddSkeleton(Skeleton);
			Database->Schema = Schema;
		}

		// Nothing here is ever written to disk; keep the package out of the save prompt.
		Package->SetDirtyFlag(false);

		return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
	}

	/** Register the PoseSearch actions on demand — module startup returns early in a commandlet. */
	static FMonolithToolRegistry& EnsureRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(ActionNamespace, StatsAction))
		{
			FMonolithPoseSearchActions::RegisterActions(Registry);
		}
		return Registry;
	}

	static FMonolithActionResult CallRead(const TCHAR* Action, const TCHAR* PathField,
		const FString& AssetPath, bool bSetWait, bool bWaitValue)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(PathField, AssetPath);
		if (bSetWait)
		{
			Params->SetBoolField(TEXT("wait"), bWaitValue);
		}
		return EnsureRegistered().ExecuteAction(ActionNamespace, Action, Params);
	}

	static FMonolithActionResult Stats(const FString& AssetPath, bool bSetWait = false, bool bWaitValue = false)
	{
		return CallRead(StatsAction, TEXT("asset_path"), AssetPath, bSetWait, bWaitValue);
	}

	static FMonolithActionResult Validate(const FString& AssetPath, bool bSetWait = false, bool bWaitValue = false)
	{
		return CallRead(ValidateAction, TEXT("database_path"), AssetPath, bSetWait, bWaitValue);
	}

	/**
	 * See MonolithPoseSearchIndexJobTest.cpp: the engine answers "BuildIndex Failed because of
	 * invalid Schema" at Error verbosity for a bare fixture, and the automation framework fails
	 * any test that emits an Error. The engine's build VERDICT is not under test here (the
	 * contract under test is the response shape and the never-block guarantee), so errors naming
	 * this fixture are tolerated. Occurrences < 0 means "tolerate, do not require".
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

	static bool IsNullField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		return Value.IsValid() && Value->Type == EJson::Null;
	}

	/**
	 * The invariants of the shared status block. They hold for EVERY status the engine can
	 * return, which is what makes them assertable headless (see the header note).
	 *
	 * Returns the reported status string so the caller can log it.
	 */
	static FString CheckStatusBlockInvariants(FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Root, bool bExpectedWaited)
	{
		const FString Status = GetString(Root, TEXT("index_status"));
		const bool bBuilt = GetBool(Root, TEXT("index_built"));
		const bool bBuilding = GetBool(Root, TEXT("index_build_in_progress"));

		const bool bKnownStatus =
			Status == TEXT("built") || Status == TEXT("building") ||
			Status == TEXT("failed") || Status == TEXT("no_schema") ||
			Status == TEXT("unavailable");
		Test.TestTrue(FString::Printf(TEXT("index_status is a known value (got '%s')"), *Status), bKnownStatus);

		Test.TestEqual(TEXT("index_built agrees with index_status"), bBuilt, Status == TEXT("built"));
		Test.TestEqual(TEXT("index_build_in_progress agrees with index_status"),
			bBuilding, Status == TEXT("building"));
		Test.TestFalse(TEXT("a built index is never also 'building'"), bBuilt && bBuilding);

		Test.TestEqual(TEXT("waited reports the path that was taken"),
			GetBool(Root, TEXT("waited"), !bExpectedWaited), bExpectedWaited);

		Test.TestTrue(TEXT("the response explains the state in prose as well as in fields"),
			!GetString(Root, TEXT("index_note")).IsEmpty());

		// A job id may only ever accompany an in-flight build.
		if (!bBuilding)
		{
			Test.TestTrue(TEXT("no build job is named when no build is in flight"),
				IsNullField(Root, TEXT("index_build_job_id")));
		}

		return Status;
	}
}

// ---------------------------------------------------------------------------
// Test 1: get_database_stats does not wait by default, and its index-dependent
//         fields are honest about what it could not determine.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchStatsDefaultNeverWaitsTest,
	"Monolith.PoseSearchIndexRead.StatsDefaultNeverWaits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchStatsDefaultNeverWaitsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexReadTest;

	TolerateEngineBuildErrorsFor(*this, TEXT("PSDB_ReadStatsDefault"));

	const FString AssetPath = CreateDisposableDatabase(TEXT("PSDB_ReadStatsDefault"), /*bWithSchema=*/true);
	if (AssetPath.IsEmpty())
	{
		AddError(TEXT("Could not build the disposable PoseSearch database fixture."));
		return false;
	}

	const double Start = FPlatformTime::Seconds();
	const FMonolithActionResult Result = Stats(AssetPath);
	const double ElapsedSeconds = FPlatformTime::Seconds() - Start;
	AddInfo(FString::Printf(TEXT("get_database_stats (default) returned in %.4f s."), ElapsedSeconds));

	TestTrue(TEXT("get_database_stats succeeded"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	const FString Status = CheckStatusBlockInvariants(*this, Result.Result, /*bExpectedWaited=*/false);
	AddInfo(FString::Printf(TEXT("Engine index status for the disposable fixture: '%s'."), *Status));

	// The pre-existing stats fields must still be there and must never be invented.
	TestEqual(TEXT("the response echoes the asset path"),
		GetString(Result.Result, TEXT("asset_path")), AssetPath);
	TestTrue(TEXT("sequence_count is still reported"), Result.Result->HasField(TEXT("sequence_count")));
	TestTrue(TEXT("search_mode is still reported"), Result.Result->HasField(TEXT("search_mode")));

	if (GetBool(Result.Result, TEXT("index_built")))
	{
		TestFalse(TEXT("a built index reports a real pose count, not null"),
			IsNullField(Result.Result, TEXT("total_pose_count")));
	}
	else
	{
		TestTrue(TEXT("an unbuilt/unknown index reports total_pose_count as null, never a guess"),
			IsNullField(Result.Result, TEXT("total_pose_count")));
		TestFalse(TEXT("an unbuilt/unknown index is never reported as valid"),
			GetBool(Result.Result, TEXT("is_valid"), true));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: `wait: true` is the explicit opt-in to the OLD blocking read, on both
//         actions, with the same spelling `rebuild_pose_search_index` uses.
//
//         This DOES call the engine indexer synchronously — against a database
//         with zero animation assets, the cheapest real request available. The
//         engine's verdict is deliberately not asserted.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchReadWaitOptInTest,
	"Monolith.PoseSearchIndexRead.WaitFlagTakesBlockingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchReadWaitOptInTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexReadTest;

	TolerateEngineBuildErrorsFor(*this, TEXT("PSDB_ReadWaitOptIn"));

	const FString AssetPath = CreateDisposableDatabase(TEXT("PSDB_ReadWaitOptIn"), /*bWithSchema=*/true);
	if (AssetPath.IsEmpty())
	{
		AddError(TEXT("Could not build the disposable PoseSearch database fixture."));
		return false;
	}

	const FMonolithActionResult StatsResult = Stats(AssetPath, /*bSetWait=*/true, /*bWaitValue=*/true);
	TestTrue(TEXT("get_database_stats with wait=true succeeded"), StatsResult.bSuccess);
	if (!StatsResult.bSuccess)
	{
		AddError(StatsResult.ErrorMessage);
		return false;
	}
	const FString StatsStatus = CheckStatusBlockInvariants(*this, StatsResult.Result, /*bExpectedWaited=*/true);
	AddInfo(FString::Printf(TEXT("Blocking get_database_stats settled on '%s'."), *StatsStatus));

	const FMonolithActionResult ValidateResult = Validate(AssetPath, /*bSetWait=*/true, /*bWaitValue=*/true);
	TestTrue(TEXT("validate_pose_search_database with wait=true succeeded"), ValidateResult.bSuccess);
	if (!ValidateResult.bSuccess)
	{
		AddError(ValidateResult.ErrorMessage);
		return false;
	}
	const FString ValidateStatus = CheckStatusBlockInvariants(*this, ValidateResult.Result, /*bExpectedWaited=*/true);
	AddInfo(FString::Printf(TEXT("Blocking validate_pose_search_database settled on '%s'."), *ValidateStatus));

	// Explicit wait=false must be identical to omitting the flag.
	const FMonolithActionResult ExplicitFalse = Stats(AssetPath, /*bSetWait=*/true, /*bWaitValue=*/false);
	TestTrue(TEXT("explicit wait=false succeeded"), ExplicitFalse.bSuccess);
	TestFalse(TEXT("explicit wait=false does not block"), GetBool(ExplicitFalse.Result, TEXT("waited"), true));

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: a database with NO schema is answered without ever asking the engine
//         indexer anything. This is the one fixture whose status is fully
//         deterministic headless, so it is where the "prompt, honest, nothing
//         invented" contract is pinned down hard.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchReadNoSchemaTest,
	"Monolith.PoseSearchIndexRead.SchemalessDatabaseAnsweredWithoutIndexer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchReadNoSchemaTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexReadTest;

	const FString AssetPath = CreateDisposableDatabase(TEXT("PSDB_ReadNoSchema"), /*bWithSchema=*/false);
	if (AssetPath.IsEmpty())
	{
		AddError(TEXT("Could not build the disposable PoseSearch database fixture."));
		return false;
	}

	// --- get_database_stats -------------------------------------------------
	const FMonolithActionResult StatsResult = Stats(AssetPath);
	TestTrue(TEXT("get_database_stats succeeded on a schemaless database"), StatsResult.bSuccess);
	if (!StatsResult.bSuccess)
	{
		AddError(StatsResult.ErrorMessage);
		return false;
	}

	CheckStatusBlockInvariants(*this, StatsResult.Result, /*bExpectedWaited=*/false);
	TestEqual(TEXT("a schemaless database reports index_status 'no_schema'"),
		GetString(StatsResult.Result, TEXT("index_status")), FString(TEXT("no_schema")));
	TestFalse(TEXT("no index is claimed"), GetBool(StatsResult.Result, TEXT("index_built")));
	TestFalse(TEXT("no build is claimed to be running"),
		GetBool(StatsResult.Result, TEXT("index_build_in_progress")));
	TestTrue(TEXT("no pose count is invented"),
		IsNullField(StatsResult.Result, TEXT("total_pose_count")));
	TestFalse(TEXT("a schemaless database is never reported valid"),
		GetBool(StatsResult.Result, TEXT("is_valid"), true));
	TestTrue(TEXT("the note tells the caller a schema is missing"),
		GetString(StatsResult.Result, TEXT("index_note")).Contains(TEXT("Schema")));

	// --- validate_pose_search_database --------------------------------------
	const FMonolithActionResult ValidateResult = Validate(AssetPath);
	TestTrue(TEXT("validate_pose_search_database succeeded on a schemaless database"), ValidateResult.bSuccess);
	if (!ValidateResult.bSuccess)
	{
		AddError(ValidateResult.ErrorMessage);
		return false;
	}

	CheckStatusBlockInvariants(*this, ValidateResult.Result, /*bExpectedWaited=*/false);
	TestEqual(TEXT("validate agrees: index_status 'no_schema'"),
		GetString(ValidateResult.Result, TEXT("index_status")), FString(TEXT("no_schema")));
	TestFalse(TEXT("a missing schema IS a validation failure"),
		GetBool(ValidateResult.Result, TEXT("valid"), true));
	TestTrue(TEXT("stale_index stays true when nothing is built"),
		GetBool(ValidateResult.Result, TEXT("stale_index")));
	TestTrue(TEXT("validation is COMPLETE — nothing was left undetermined"),
		GetBool(ValidateResult.Result, TEXT("validation_complete")));

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: an in-flight Monolith rebuild is DISCOVERABLE from an incomplete read,
//         and the id prunes itself once the job finishes.
//
//         The rebuild job is dispatched and NEVER pumped, so no slice runs and
//         the engine indexer is never driven by this test.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchReadJobDiscoverabilityTest,
	"Monolith.PoseSearchIndexRead.InFlightRebuildJobIsDiscoverable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchReadJobDiscoverabilityTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexReadTest;

	TolerateEngineBuildErrorsFor(*this, TEXT("PSDB_ReadJobDiscovery"));
	TolerateEngineBuildErrorsFor(*this, TEXT("PSDB_ReadJobBystander"));

	const FString AssetPath = CreateDisposableDatabase(TEXT("PSDB_ReadJobDiscovery"), /*bWithSchema=*/true);
	const FString BystanderPath = CreateDisposableDatabase(TEXT("PSDB_ReadJobBystander"), /*bWithSchema=*/true);
	if (AssetPath.IsEmpty() || BystanderPath.IsEmpty())
	{
		AddError(TEXT("Could not build the disposable PoseSearch database fixtures."));
		return false;
	}

	FMonolithJobManager& Manager = FMonolithJobManager::Get();

	// Dispatch the rebuild as a job and do NOT pump: it stays Running, and its first slice
	// (the only thing that would touch the engine indexer) never executes.
	TSharedPtr<FJsonObject> RebuildParams = MakeShared<FJsonObject>();
	RebuildParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult Dispatch =
		EnsureRegistered().ExecuteAction(ActionNamespace, RebuildAction, RebuildParams);

	TestTrue(TEXT("the rebuild dispatched"), Dispatch.bSuccess);
	if (!Dispatch.bSuccess)
	{
		AddError(Dispatch.ErrorMessage);
		return false;
	}
	const FString JobId = GetString(Dispatch.Result, TEXT("job_id"));
	if (JobId.IsEmpty())
	{
		// The jobs namespace can be switched off in settings; then there is no id to discover
		// and this test has nothing to say.
		AddWarning(TEXT("No job id was handed back (jobs disabled?) — discoverability not exercised."));
		return true;
	}

	// --- the read while the job is running ----------------------------------
	const FMonolithActionResult DuringStats = Stats(AssetPath);
	TestTrue(TEXT("get_database_stats succeeded while a rebuild job is running"), DuringStats.bSuccess);
	if (!DuringStats.bSuccess)
	{
		AddError(DuringStats.ErrorMessage);
		Manager.CancelJob(JobId);
		Manager.PumpOnce();
		Manager.RemoveJob(JobId);
		return false;
	}

	const FString DuringStatus = CheckStatusBlockInvariants(*this, DuringStats.Result, /*bExpectedWaited=*/false);
	AddInfo(FString::Printf(TEXT("Status seen while the rebuild job was queued: '%s'."), *DuringStatus));

	const bool bIncomplete = GetBool(DuringStats.Result, TEXT("index_build_in_progress"));
	if (bIncomplete)
	{
		// THE claim: an incomplete answer names the job the caller can poll.
		TestEqual(TEXT("the incomplete read names the running rebuild job"),
			GetString(DuringStats.Result, TEXT("index_build_job_id")), JobId);
		TestTrue(TEXT("the note tells the caller to poll that job"),
			GetString(DuringStats.Result, TEXT("index_note")).Contains(TEXT("jobs_query")));

		// A read about a DIFFERENT database must never borrow this job id.
		const FMonolithActionResult Bystander = Stats(BystanderPath);
		TestTrue(TEXT("the bystander read succeeded"), Bystander.bSuccess);
		TestNotEqual(TEXT("a read about another database never names this job"),
			GetString(Bystander.Result, TEXT("index_build_job_id")), JobId);

		// validate agrees with stats — one probe, one answer.
		const FMonolithActionResult DuringValidate = Validate(AssetPath);
		TestTrue(TEXT("validate succeeded while a rebuild job is running"), DuringValidate.bSuccess);
		TestEqual(TEXT("validate names the same job as stats"),
			GetString(DuringValidate.Result, TEXT("index_build_job_id")), JobId);
		TestFalse(TEXT("an in-flight build leaves validation INCOMPLETE"),
			GetBool(DuringValidate.Result, TEXT("validation_complete"), true));
		TestTrue(TEXT("an in-flight build is not itself a validation failure"),
			GetBool(DuringValidate.Result, TEXT("valid")));
	}
	else
	{
		// The engine settled the fixture before we could look (see the header note on the
		// headless ceiling). The complementary invariant still holds and is asserted by
		// CheckStatusBlockInvariants: no job id is named when no build is in flight.
		AddInfo(TEXT("The engine reported a settled index, so the in-flight branch was not exercised; ")
			TEXT("the 'no job id unless building' invariant was checked instead."));
	}

	// --- the id prunes itself once the job is finished ----------------------
	TestTrue(TEXT("cancel accepted while the job is queued"), Manager.CancelJob(JobId));
	Manager.PumpOnce();

	FMonolithJob Job;
	TestTrue(TEXT("the cancelled job is still pollable"), Manager.GetJob(JobId, Job));
	TestTrue(TEXT("the job is finished"), Job.IsFinished());

	const FMonolithActionResult AfterStats = Stats(AssetPath);
	TestTrue(TEXT("get_database_stats succeeded after the job finished"), AfterStats.bSuccess);
	TestTrue(TEXT("a finished job is no longer named as the in-flight build"),
		IsNullField(AfterStats.Result, TEXT("index_build_job_id")));

	Manager.RemoveJob(JobId);
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: error paths stay clean on both reads. An unknown database is a plain
//         synchronous error — no status block, no job, nothing invented.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithPoseSearchReadUnknownDatabaseTest,
	"Monolith.PoseSearchIndexRead.UnknownDatabaseIsCleanError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPoseSearchReadUnknownDatabaseTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPoseSearchIndexReadTest;

	const FString Missing = TEXT("/Game/Tests/Monolith/PoseSearchIndexRead/PSDB_DoesNotExist.PSDB_DoesNotExist");

	FMonolithJobManager& Manager = FMonolithJobManager::Get();
	const int32 JobsBefore = Manager.GetJobCount();

	const FMonolithActionResult StatsResult = Stats(Missing);
	TestFalse(TEXT("get_database_stats fails cleanly on an unknown database"), StatsResult.bSuccess);
	TestTrue(TEXT("the stats error names the missing database"),
		StatsResult.ErrorMessage.Contains(TEXT("PoseSearchDatabase not found")));

	const FMonolithActionResult ValidateResult = Validate(Missing);
	TestFalse(TEXT("validate fails cleanly on an unknown database"), ValidateResult.bSuccess);
	TestTrue(TEXT("the validate error names the missing database"),
		ValidateResult.ErrorMessage.Contains(TEXT("PoseSearchDatabase not found")));

	// wait=true must not change the error path either.
	const FMonolithActionResult WaitingStats = Stats(Missing, /*bSetWait=*/true, /*bWaitValue=*/true);
	TestFalse(TEXT("wait=true on an unknown database is still a clean error"), WaitingStats.bSuccess);

	TestEqual(TEXT("a failed read creates no job"), Manager.GetJobCount(), JobsBefore);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
