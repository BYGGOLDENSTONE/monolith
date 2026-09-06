// SPDX-License-Identifier: MIT
// Automation tests for full-index interruption recovery (issue #117).
// Plan: Plugins/Monolith/Docs/plans/2026-08-01-pr-issue-sweep.md (Phase 6, T29)
//
// Goals:
//   - The v3 lifecycle markers survive a close/reopen, and completing an index
//     clears the in-progress state. This is what stops a crash from turning into
//     a wipe-and-rebuild on the next launch.
//   - A checkpoint and the child rows it vouches for are ATOMIC. If a rollback
//     could keep the checkpoint, a resume would trust data that is not there.
//   - The v2 -> v3 migration adds the two columns WITHOUT destroying existing
//     rows. This is the case that can silently cost a user their whole index.
//   - The poison pill: two interrupted attempts take an asset out of the queue,
//     and -- the half that matters -- the attempt counter SURVIVES a rolled-back
//     work transaction. A counter written inside that transaction would be rolled
//     back with it, read 0 on resume, and the crash would repeat forever. A test
//     that only checks "the counter increments" passes under that broken design,
//     so the rollback assertion is the one that closes the hole.
//
// These are pure database tests against their own temp SQLite file: no project
// index, no editor subsystem, no indexing state.

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#include "SQLiteDatabase.h"
#include "MonolithIndexDatabase.h"
#include "Indexers/AIIndexer.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_CurrentLocation.h"
#include "AssetRegistry/AssetData.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithIndexRecoveryTestDetail
{
	static const TCHAR* AssetPath = TEXT("/Game/Tests/Monolith/MonolithRecoveryFixtureAsset");
	static const TCHAR* SecondAssetPath = TEXT("/Game/Tests/Monolith/MonolithRecoveryFixtureAssetB");
	static const TCHAR* ContentHash = TEXT("0123456789abcdef0123456789abcdef01234567");

	/** Temp index database that can be closed and reopened in place. */
	struct FFixture
	{
		FMonolithIndexDatabase Database;
		FString DbPath;

		bool Open()
		{
			DbPath = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("MonolithTests"),
				FString::Printf(TEXT("IndexRecovery_%s.db"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

			return Database.Open(DbPath);
		}

		/** Close and reopen the same file, so on-disk state is what is asserted. */
		bool Reopen()
		{
			Database.Close();
			return Database.Open(DbPath);
		}

		int64 InsertAsset(const TCHAR* PackagePath, const TCHAR* SavedHash)
		{
			FIndexedAsset Asset;
			Asset.PackagePath = PackagePath;
			Asset.AssetName = TEXT("MonolithRecoveryFixtureAsset");
			Asset.AssetClass = TEXT("Blueprint");
			Asset.ModuleName = TEXT("Game");
			Asset.Description = TEXT("index recovery fixture");
			Asset.SavedHash = SavedHash;
			return Database.InsertAsset(Asset);
		}

		void Destroy()
		{
			Database.Close();
			if (!DbPath.IsEmpty())
			{
				FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
			}
		}
	};

	/** Count rows in a table, for the child-data assertions. */
	static int64 CountRows(FMonolithIndexDatabase& Database, const TCHAR* Table)
	{
		FSQLiteDatabase* Raw = Database.GetRawDatabase();
		if (!Raw)
		{
			return -1;
		}

		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Raw, *FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), Table));
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, Count);
			return Count;
		}
		return -1;
	}

	/** True when `assets` carries the named column. */
	static bool HasAssetColumn(FMonolithIndexDatabase& Database, const TCHAR* ColumnName)
	{
		FSQLiteDatabase* Raw = Database.GetRawDatabase();
		if (!Raw)
		{
			return false;
		}

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT("PRAGMA table_info(assets);")))
		{
			return false;
		}

		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString ColName;
			Stmt.GetColumnValueByIndex(1, ColName);
			if (ColName == ColumnName)
			{
				return true;
			}
		}
		return false;
	}
}

// ---------------------------------------------------------------------------
// Test 1: the full-index lifecycle markers, including across a reopen.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoveryLifecycleTest,
	"Monolith.Index.Recovery.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoveryLifecycleTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	TestEqual(TEXT("a fresh database is schema v3"), Fixture.Database.ReadMeta(TEXT("schema_version")), FString(TEXT("3")));
	TestTrue(TEXT("a fresh database supports resume"), Fixture.Database.SupportsIndexResume());
	TestFalse(TEXT("no index is in progress on a fresh database"), Fixture.Database.IsFullIndexInProgress());

	TestTrue(TEXT("BeginFullIndex succeeds"), Fixture.Database.BeginFullIndex());
	TestTrue(TEXT("the index reads as in progress"), Fixture.Database.IsFullIndexInProgress());
	TestTrue(TEXT("BeginFullIndex clears any previous completion stamp"),
		Fixture.Database.ReadMeta(TEXT("last_full_index")).IsEmpty());

	// The whole point: the marker is on disk, not in memory. This is the state a
	// crashed editor leaves behind.
	if (!TestTrue(TEXT("database reopens"), Fixture.Reopen()))
	{
		return false;
	}
	TestTrue(TEXT("the in-progress marker survives a close/reopen"), Fixture.Database.IsFullIndexInProgress());

	TestTrue(TEXT("CompleteFullIndex succeeds"), Fixture.Database.CompleteFullIndex(TEXT("2026.08.01-00.00.00")));
	TestFalse(TEXT("completion clears the in-progress marker"), Fixture.Database.IsFullIndexInProgress());
	TestEqual(TEXT("completion writes the timestamp"),
		Fixture.Database.ReadMeta(TEXT("last_full_index")), FString(TEXT("2026.08.01-00.00.00")));

	// An explicit force-reindex still wipes: that behaviour is deliberate and is
	// the documented recovery from a skipped asset.
	TestTrue(TEXT("BeginFullIndex re-arms the marker"), Fixture.Database.BeginFullIndex());
	TestTrue(TEXT("ResetDatabase succeeds"), Fixture.Database.ResetDatabase());
	TestFalse(TEXT("a reset clears the in-progress marker"), Fixture.Database.IsFullIndexInProgress());
	TestEqual(TEXT("a reset restates the schema version"),
		Fixture.Database.ReadMeta(TEXT("schema_version")), FString(TEXT("3")));

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: a checkpoint and the child rows it vouches for are atomic.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoveryCheckpointAtomicityTest,
	"Monolith.Index.Recovery.CheckpointAtomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoveryCheckpointAtomicityTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	const int64 AssetId = Fixture.InsertAsset(AssetPath, ContentHash);
	if (!TestTrue(TEXT("fixture asset inserts"), AssetId > 0))
	{
		return false;
	}

	// Exactly what a deep-index batch does: child rows plus the checkpoint that
	// says those rows exist, in one transaction.
	TestTrue(TEXT("work transaction begins"), Fixture.Database.BeginTransaction());

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeName = TEXT("MonolithRecoveryFixtureNode");
	Node.NodeClass = TEXT("K2Node_IfThenElse");
	Node.NodeType = TEXT("flow");
	TestTrue(TEXT("child row inserts"), Fixture.Database.InsertNode(Node) > 0);
	TestTrue(TEXT("checkpoint writes"), Fixture.Database.SetDeepIndexedHash(AssetId, ContentHash));

	// The crash.
	TestTrue(TEXT("work transaction rolls back"), Fixture.Database.RollbackTransaction());

	TestEqual(TEXT("the child rows are gone"), CountRows(Fixture.Database, TEXT("nodes")), (int64)0);

	TOptional<FIndexedAsset> Reloaded = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("the asset row itself survives"), Reloaded.IsSet()))
	{
		return false;
	}
	// A checkpoint that outlived its child rows would tell the resume to skip an
	// asset whose data was discarded -- silent, permanent data loss.
	TestTrue(TEXT("the checkpoint is gone with them"), Reloaded->DeepIndexedHash.IsEmpty());

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: the v2 -> v3 migration is additive and non-destructive.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoverySchemaMigrationTest,
	"Monolith.Index.Recovery.SchemaMigrationV2ToV3",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoverySchemaMigrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	const int64 AssetId = Fixture.InsertAsset(AssetPath, ContentHash);
	if (!TestTrue(TEXT("fixture asset inserts"), AssetId > 0))
	{
		return false;
	}

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeName = TEXT("MonolithRecoveryFixtureNode");
	Node.NodeClass = TEXT("K2Node_IfThenElse");
	Node.NodeType = TEXT("flow");
	TestTrue(TEXT("fixture child row inserts"), Fixture.Database.InsertNode(Node) > 0);

	// Rewind the schema to what v0.21.3 shipped: no resume columns, version 2.
	FSQLiteDatabase* Raw = Fixture.Database.GetRawDatabase();
	if (!TestNotNull(TEXT("raw database is available"), Raw))
	{
		return false;
	}
	if (!TestTrue(TEXT("deep_indexed_hash can be dropped"),
		Raw->Execute(TEXT("ALTER TABLE assets DROP COLUMN deep_indexed_hash;"))))
	{
		return false;
	}
	if (!TestTrue(TEXT("deep_index_attempts can be dropped"),
		Raw->Execute(TEXT("ALTER TABLE assets DROP COLUMN deep_index_attempts;"))))
	{
		return false;
	}
	TestTrue(TEXT("schema version rewinds to 2"), Fixture.Database.WriteMeta(TEXT("schema_version"), TEXT("2")));
	TestFalse(TEXT("a v2 database does not claim resume support"), Fixture.Database.SupportsIndexResume());

	// Reopen: this is the upgrade a user gets by launching the new build.
	if (!TestTrue(TEXT("database reopens"), Fixture.Reopen()))
	{
		return false;
	}

	TestTrue(TEXT("deep_indexed_hash is added"), HasAssetColumn(Fixture.Database, TEXT("deep_indexed_hash")));
	TestTrue(TEXT("deep_index_attempts is added"), HasAssetColumn(Fixture.Database, TEXT("deep_index_attempts")));
	TestEqual(TEXT("the schema version is stamped 3"),
		Fixture.Database.ReadMeta(TEXT("schema_version")), FString(TEXT("3")));
	TestTrue(TEXT("resume is available after migration"), Fixture.Database.SupportsIndexResume());

	// The assertion that matters: the migration must not cost the user the index
	// they already have.
	TOptional<FIndexedAsset> Reloaded = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("the pre-existing asset row survives"), Reloaded.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("its id is unchanged"), Reloaded->Id, AssetId);
	TestEqual(TEXT("its class is intact"), Reloaded->AssetClass, FString(TEXT("Blueprint")));
	TestEqual(TEXT("its saved hash is intact"), Reloaded->SavedHash, FString(ContentHash));
	TestEqual(TEXT("its new checkpoint column defaults to empty"), Reloaded->DeepIndexedHash, FString());
	TestEqual(TEXT("its new attempt column defaults to zero"), Reloaded->DeepIndexAttempts, 0);
	TestEqual(TEXT("its child rows survive"), CountRows(Fixture.Database, TEXT("nodes")), (int64)1);

	// And the migrated columns are usable, not just present.
	TestTrue(TEXT("the migrated checkpoint column accepts a write"),
		Fixture.Database.SetDeepIndexedHash(AssetId, ContentHash));
	Reloaded = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("the asset still reads back after the write"), Reloaded.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the checkpoint reads back"), Reloaded->DeepIndexedHash, FString(ContentHash));

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: the poison pill -- skip contract AND the durability it rests on.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoveryPoisonPillTest,
	"Monolith.Index.Recovery.PoisonPillSkipsAfterTwoAttempts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoveryPoisonPillTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	const int64 PoisonId = Fixture.InsertAsset(AssetPath, ContentHash);
	const int64 HealthyId = Fixture.InsertAsset(SecondAssetPath, ContentHash);
	if (!TestTrue(TEXT("fixture assets insert"), PoisonId > 0 && HealthyId > 0))
	{
		return false;
	}

	// --- Part 1: the skip contract. ---------------------------------------

	// One interrupted attempt is not enough -- a single crash could have been the
	// editor being killed for any reason, and dropping an asset on that evidence
	// would lose data for nothing.
	TestTrue(TEXT("first attempt records"), Fixture.Database.BumpDeepIndexAttempts({ PoisonId, HealthyId }));
	TOptional<FIndexedAsset> AfterOne = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("asset reads back after one attempt"), AfterOne.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("one attempt is recorded"), AfterOne->DeepIndexAttempts, 1);
	TestEqual(TEXT("an asset at one attempt is still queued"),
		MonolithDecideDeepIndexQueueEntry(AfterOne->DeepIndexedHash, AfterOne->DeepIndexAttempts, ContentHash),
		EMonolithDeepIndexQueueDecision::Queue);

	// Two is the line.
	TestTrue(TEXT("second attempt records"), Fixture.Database.BumpDeepIndexAttempts({ PoisonId }));
	TOptional<FIndexedAsset> AfterTwo = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("asset reads back after two attempts"), AfterTwo.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("two attempts are recorded"), AfterTwo->DeepIndexAttempts, 2);
	TestEqual(TEXT("an asset at two attempts is skipped as poisonous"),
		MonolithDecideDeepIndexQueueEntry(AfterTwo->DeepIndexedHash, AfterTwo->DeepIndexAttempts, ContentHash),
		EMonolithDeepIndexQueueDecision::SkipPoisonAsset);

	// The healthy asset shares the batch but was only bumped once, so it stays.
	TOptional<FIndexedAsset> Healthy = Fixture.Database.GetAssetByPath(SecondAssetPath);
	if (!TestTrue(TEXT("healthy asset reads back"), Healthy.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the healthy asset is still queued"),
		MonolithDecideDeepIndexQueueEntry(Healthy->DeepIndexedHash, Healthy->DeepIndexAttempts, ContentHash),
		EMonolithDeepIndexQueueDecision::Queue);

	// Skipping stamps the current hash so the asset leaves the queue for good
	// rather than being re-evaluated (and re-logged) on every later resume.
	TestTrue(TEXT("skipping stamps the current hash"), Fixture.Database.SetDeepIndexedHash(PoisonId, ContentHash));
	TOptional<FIndexedAsset> AfterSkip = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("asset reads back after the skip"), AfterSkip.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the skip is recorded on the row"), AfterSkip->DeepIndexedHash, FString(ContentHash));
	TestEqual(TEXT("the skipped asset stays out of the queue"),
		MonolithDecideDeepIndexQueueEntry(AfterSkip->DeepIndexedHash, AfterSkip->DeepIndexAttempts, ContentHash),
		EMonolithDeepIndexQueueDecision::SkipAlreadyIndexed);

	// The counter is left at its limit rather than cleared, so the asset is held
	// out on BOTH gates. An asset whose Asset Registry hash is empty would stamp
	// an empty checkpoint, fail the hash comparison next run, and start crashing
	// again if the counter were the only thing keeping it out.
	TestEqual(TEXT("the counter still holds it out when the hash stamp cannot"),
		MonolithDecideDeepIndexQueueEntry(FString(), AfterSkip->DeepIndexAttempts, FString()),
		EMonolithDeepIndexQueueDecision::SkipPoisonAsset);
	TestEqual(TEXT("even a content change does not revive it while the counter stands"),
		MonolithDecideDeepIndexQueueEntry(AfterSkip->DeepIndexedHash, AfterSkip->DeepIndexAttempts, TEXT("ffffffffffffffffffffffffffffffffffffffff")),
		EMonolithDeepIndexQueueDecision::SkipPoisonAsset);

	// A force reindex is the documented recovery: it wipes the row, which is
	// equivalent to a cleared counter and no checkpoint.
	TestTrue(TEXT("clearing the counter succeeds"), Fixture.Database.ClearDeepIndexAttempts(PoisonId));
	TOptional<FIndexedAsset> AfterClear = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("asset reads back after the clear"), AfterClear.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the counter is back to zero"), AfterClear->DeepIndexAttempts, 0);
	TestEqual(TEXT("a cleared, changed asset is queued again"),
		MonolithDecideDeepIndexQueueEntry(AfterClear->DeepIndexedHash, AfterClear->DeepIndexAttempts, TEXT("ffffffffffffffffffffffffffffffffffffffff")),
		EMonolithDeepIndexQueueDecision::Queue);

	// --- Part 2: durability. This half is the mechanism. ------------------
	//
	// The marker only works because it is committed BEFORE the work transaction
	// opens. Written inside that transaction it would be rolled back with it on
	// the next open, the counter would read 0 on resume, the same batch would
	// re-queue, and the editor would crash again -- forever. A skip test alone
	// has no transaction boundary and passes under that broken design, so this
	// sequence is what actually locks the fix in.
	const int64 DurabilityId = Fixture.InsertAsset(TEXT("/Game/Tests/Monolith/MonolithRecoveryDurability"), ContentHash);
	if (!TestTrue(TEXT("durability fixture asset inserts"), DurabilityId > 0))
	{
		return false;
	}

	// The attempt marker: its own transaction, committed.
	TestTrue(TEXT("marker transaction begins"), Fixture.Database.BeginTransaction());
	TestTrue(TEXT("marker writes"), Fixture.Database.BumpDeepIndexAttempts({ DurabilityId }));
	TestTrue(TEXT("marker transaction commits"), Fixture.Database.CommitTransaction());

	// The batch work, which the crash discards.
	TestTrue(TEXT("work transaction begins"), Fixture.Database.BeginTransaction());
	FIndexedNode WorkNode;
	WorkNode.AssetId = DurabilityId;
	WorkNode.NodeName = TEXT("MonolithRecoveryDurabilityNode");
	WorkNode.NodeClass = TEXT("K2Node_IfThenElse");
	WorkNode.NodeType = TEXT("flow");
	TestTrue(TEXT("work row inserts"), Fixture.Database.InsertNode(WorkNode) > 0);
	TestTrue(TEXT("work transaction rolls back"), Fixture.Database.RollbackTransaction());

	TOptional<FIndexedAsset> AfterRollback = Fixture.Database.GetAssetByPath(TEXT("/Game/Tests/Monolith/MonolithRecoveryDurability"));
	if (!TestTrue(TEXT("durability asset reads back"), AfterRollback.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the attempt counter SURVIVES the rolled-back work transaction"),
		AfterRollback->DeepIndexAttempts, 1);

	// And it survives the process restart the rollback actually happens on.
	if (!TestTrue(TEXT("database reopens"), Fixture.Reopen()))
	{
		return false;
	}
	AfterRollback = Fixture.Database.GetAssetByPath(TEXT("/Game/Tests/Monolith/MonolithRecoveryDurability"));
	if (!TestTrue(TEXT("durability asset reads back after reopen"), AfterRollback.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the attempt counter survives a close/reopen"), AfterRollback->DeepIndexAttempts, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSentinelOwnedRowsTest, "Monolith.Index.Recovery.SentinelOwnedRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithSentinelOwnedRowsTest::RunTest(const FString&)
{
	using namespace MonolithIndexRecoveryTestDetail;
	FFixture Fixture;
	if (!TestTrue(TEXT("open"), Fixture.Open())) return false;
	ON_SCOPE_EXIT { Fixture.Destroy(); };
	auto& DB = Fixture.Database;
	int64 BP = Fixture.InsertAsset(AssetPath, ContentHash);
	FIndexedAsset Meta;
	Meta.PackagePath = TEXT("/Game/Tests/Monolith/MS_Test"); Meta.AssetName = TEXT("MS_Test"); Meta.AssetClass = TEXT("MetaSoundSource");
	int64 MS = DB.InsertAsset(Meta);
	auto Add = [&](int64 Asset, const TCHAR* Type) { FIndexedNode N; N.AssetId = Asset; N.NodeName = Type; N.NodeType = Type; return DB.InsertNode(N); };
	Add(BP, TEXT("Function"));
	int64 Ability = Add(BP, TEXT("GameplayAbility"));
	int64 Effect = Add(BP, TEXT("GameplayEffect"));
	FIndexedConnection Edge; Edge.SourceNodeId = Ability; Edge.TargetNodeId = Effect; Edge.SourcePin = TEXT("CostEffect"); Edge.TargetPin = TEXT("Self"); DB.InsertConnection(Edge);
	Add(MS, TEXT("Node")); Add(MS, TEXT("Metadata"));
	FIndexedVariable V; V.AssetId = MS; V.VarName = TEXT("Gain"); V.VarType = TEXT("Float"); V.Category = TEXT("MetaSound"); DB.InsertVariable(V);
	V.Category = TEXT("Other"); DB.InsertVariable(V);
	TestTrue(TEXT("begin replacement"), DB.BeginTransaction());
	TestTrue(TEXT("clear GAS"), DB.ClearGASIndexRows());
	TestTrue(TEXT("clear MetaSound"), DB.ClearMetaSoundIndexRows());
	TestEqual(TEXT("only BP graph preserved"), DB.GetNodesForAsset(BP).Num(), 1);
	TestEqual(TEXT("only MetaSound metadata preserved"), DB.GetNodesForAsset(MS).Num(), 1);
	TestEqual(TEXT("only unrelated variable preserved"), DB.GetVariablesForAsset(MS).Num(), 1);
	TestEqual(TEXT("GAS connections cascade"), CountRows(DB, TEXT("connections")), int64(0));
	DB.RollbackTransaction();
	TestEqual(TEXT("rollback restores old GAS data"), DB.GetNodesForAsset(BP).Num(), 3);
	TestEqual(TEXT("rollback restores edges"), CountRows(DB, TEXT("connections")), int64(1));
	TestEqual(TEXT("rollback restores MetaSound data"), DB.GetNodesForAsset(MS).Num(), 2);
	TestTrue(TEXT("repeat GAS clear"), DB.ClearGASIndexRows());
	TestTrue(TEXT("idempotent GAS clear"), DB.ClearGASIndexRows());
	TestEqual(TEXT("still preserves Blueprint"), DB.GetNodesForAsset(BP).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAIStructureTest, "Monolith.Index.AI.Structure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAIStructureTest::RunTest(const FString&)
{
	using namespace MonolithIndexRecoveryTestDetail;
	FFixture Fixture;
	if (!TestTrue(TEXT("open"), Fixture.Open())) return false;
	ON_SCOPE_EXIT { Fixture.Destroy(); };
	auto& DB = Fixture.Database;
	int64 Id = Fixture.InsertAsset(AssetPath, ContentHash);
	FMonolithAIAssetIndexer Indexer;
	UBehaviorTree* Tree = NewObject<UBehaviorTree>();
	Tree->RootNode = NewObject<UBTComposite_Sequence>(Tree);
	FBTCompositeChild Child;
	Child.ChildTask = NewObject<UBTTask_Wait>(Tree);
	Tree->RootNode->Children.Add(Child);
	TestTrue(TEXT("BT index succeeds"), Indexer.IndexAsset(FAssetData(), Tree, DB, Id));
	TestEqual(TEXT("tree, root and task"), DB.GetNodesForAsset(Id).Num(), 3);
	TestEqual(TEXT("root and ordered child edges"), CountRows(DB, TEXT("connections")), int64(2));
	DB.DeleteChildDataForAsset(Id);
	UBlackboardData* BB = NewObject<UBlackboardData>();
	BB->Parent = NewObject<UBlackboardData>();
	FBlackboardEntry Key;
	Key.EntryName = TEXT("Speed"); Key.KeyType = NewObject<UBlackboardKeyType_Float>(BB); BB->Keys.Add(Key);
	Key.KeyType = NewObject<UBlackboardKeyType_Int>(BB->Parent);
	BB->Parent->Keys.Add(Key); // child Float overrides parent Int
	Key.EntryName = TEXT("Distance"); BB->Parent->Keys.Add(Key);
	TestTrue(TEXT("blackboard index succeeds"), Indexer.IndexAsset(FAssetData(), BB, DB, Id));
	const auto Vars = DB.GetVariablesForAsset(Id);
	// UE creates the persistent SelfActor key in PostInitProperties. Include
	// engine-supplied keys rather than treating them as duplicate user keys.
	TSet<FString> ExpectedNames;
	for (const auto& Entry : BB->Keys) ExpectedNames.Add(Entry.EntryName.ToString());
	for (const auto& Entry : BB->Parent->Keys) ExpectedNames.Add(Entry.EntryName.ToString());
	TestEqual(TEXT("all inherited/local/engine keys deduplicated"), Vars.Num(), ExpectedNames.Num());
	TSet<FString> ActualNames;
	int32 SpeedCount = 0, DistanceCount = 0;
	for (const auto& Var : Vars)
	{
		TestFalse(TEXT("no duplicate key identity"), ActualNames.Contains(Var.VarName));
		ActualNames.Add(Var.VarName);
		TestTrue(TEXT("every key belongs to the fixture or its engine defaults"), ExpectedNames.Contains(Var.VarName));
		if (Var.VarName == TEXT("Speed"))
		{
			++SpeedCount;
			TestEqual(TEXT("override uses child Float type"), Var.VarType, UBlackboardKeyType_Float::StaticClass()->GetName());
			TestEqual(TEXT("override belongs to child"), Var.Category, FString(TEXT("Blackboard")));
		}
		if (Var.VarName == TEXT("Distance"))
		{
			++DistanceCount;
			TestEqual(TEXT("inherited key keeps parent Int type"), Var.VarType, UBlackboardKeyType_Int::StaticClass()->GetName());
			TestEqual(TEXT("inherited key belongs to parent"), Var.Category, FString(TEXT("BlackboardInherited")));
		}
	}
	TestEqual(TEXT("one Speed override"), SpeedCount, 1);
	TestEqual(TEXT("one inherited Distance"), DistanceCount, 1);
	DB.DeleteChildDataForAsset(Id);
	UEnvQuery* Query = NewObject<UEnvQuery>();
	UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query);
	Option->Generator = NewObject<UEnvQueryGenerator_CurrentLocation>(Option);
	Query->GetOptionsMutable().Add(Option);
	TestTrue(TEXT("EQS index succeeds"), Indexer.IndexAsset(FAssetData(), Query, DB, Id));
	TestEqual(TEXT("query, option, generator"), DB.GetNodesForAsset(Id).Num(), 3);
	TestEqual(TEXT("option and generator edges"), CountRows(DB, TEXT("connections")), int64(2));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
