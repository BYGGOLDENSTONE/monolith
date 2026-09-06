#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "MonolithAIIndexer.h"
#include "MonolithIndexDatabase.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	int64 AIIndexCount(FMonolithIndexDatabase& DB, const TCHAR* Table)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*DB.GetRawDatabase(), *FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), Table))) return -1;
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row) return -1;
		int64 Count = -1; Stmt.GetColumnValueByIndex(0, Count); return Count;
	}
	void DeleteTestDB(const FString& Path)
	{
		IFileManager::Get().Delete(*Path);
		IFileManager::Get().Delete(*(Path + TEXT("-wal")));
		IFileManager::Get().Delete(*(Path + TEXT("-shm")));
		IFileManager::Get().Delete(*(Path + TEXT("-journal")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAILegacyIndexRecoveryTest, "Monolith.AI.Index.LegacySentinelRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAILegacyIndexRecoveryTest::RunTest(const FString&)
{
	FMonolithIndexDatabase DB, SecondDB;
	const FString Base = FPaths::ProjectSavedDir() / TEXT("MonolithTests") / (TEXT("LegacyAI_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Path = Base + TEXT(".db"), SecondPath = Base + TEXT("_second.db");
	ON_SCOPE_EXIT { DB.Close(); SecondDB.Close(); DeleteTestDB(Path); DeleteTestDB(SecondPath); };
	if (!TestTrue(TEXT("open fixture"), DB.Open(Path))) return false;
	if (!TestTrue(TEXT("prepare schema"), FAIIndexer::PrepareIndex(DB))) return false;
	FIndexedAsset Asset; Asset.PackagePath = TEXT("/Game/MonolithTests/AI"); Asset.AssetName = TEXT("AI"); Asset.AssetClass = TEXT("BehaviorTree");
	int64 Id = DB.InsertAsset(Asset);
	auto AddNode = [&](const TCHAR* Type, const TCHAR* Class)
	{
		FIndexedNode Node; Node.AssetId = Id; Node.NodeName = Type; Node.NodeType = Type; Node.NodeClass = Class;
		return DB.InsertNode(Node);
	};
	AddNode(TEXT("BehaviorTree"), TEXT("BehaviorTree")); // new structure indexer root
	AddNode(TEXT("BehaviorTreeNode"), TEXT("BTTask_Wait"));
	AddNode(TEXT("Function"), TEXT("K2Node_FunctionEntry"));
	AddNode(TEXT("BehaviorTree"), TEXT("UBehaviorTree")); // pre-migration legacy summary
	AddNode(TEXT("AIAssetSummary"), TEXT("UBehaviorTree"));
	FSQLiteDatabase* Raw = DB.GetRawDatabase();
	TestTrue(TEXT("seed catalog"), Raw->Execute(TEXT("INSERT INTO ai_assets(id,path,type,name) VALUES(1,'/Game/BB','BlackboardData','BB'),(2,'/Game/BT','BehaviorTree','BT');")));
	TestTrue(TEXT("seed key"), Raw->Execute(TEXT("INSERT INTO ai_bb_keys(bb_id,key_name,key_type) VALUES(1,'Speed','Float');")));
	TestTrue(TEXT("seed reference"), Raw->Execute(TEXT("INSERT INTO ai_cross_refs(source_id,target_id,ref_type) VALUES(2,1,'uses_blackboard');")));
	TestTrue(TEXT("begin replacement"), DB.BeginTransaction());
	TestTrue(TEXT("replacement succeeds"), FAIIndexer::PrepareIndex(DB));
	TestEqual(TEXT("only three unrelated structure/Blueprint rows remain"), DB.GetNodesForAsset(Id).Num(), 3);
	TestEqual(TEXT("catalog cleared after children"), AIIndexCount(DB, TEXT("ai_assets")), int64(0));
	TestEqual(TEXT("references cleared"), AIIndexCount(DB, TEXT("ai_cross_refs")), int64(0));
	TestTrue(TEXT("rollback replacement"), DB.RollbackTransaction());
	TestEqual(TEXT("old summaries restored"), DB.GetNodesForAsset(Id).Num(), 5);
	TestEqual(TEXT("catalog restored"), AIIndexCount(DB, TEXT("ai_assets")), int64(2));
	TestEqual(TEXT("references restored"), AIIndexCount(DB, TEXT("ai_cross_refs")), int64(1));
	TestEqual(TEXT("keys restored"), AIIndexCount(DB, TEXT("ai_bb_keys")), int64(1));
	// Force a real SQLite write failure after earlier deletes; the caller can roll
	// back the entire pass and must not mistake a partial reset for success.
	TestTrue(TEXT("install failure trigger"), Raw->Execute(TEXT("CREATE TRIGGER reject_ai_clear BEFORE DELETE ON ai_assets BEGIN SELECT RAISE(ABORT,'test write failure'); END;")));
	TestTrue(TEXT("begin failed pass"), DB.BeginTransaction());
	TestFalse(TEXT("write failure propagates"), FAIIndexer::PrepareIndex(DB));
	TestTrue(TEXT("rollback failed pass"), DB.RollbackTransaction());
	TestEqual(TEXT("failed replacement preserves refs after rollback"), AIIndexCount(DB, TEXT("ai_cross_refs")), int64(1));
	TestTrue(TEXT("remove failure trigger"), Raw->Execute(TEXT("DROP TRIGGER reject_ai_clear;")));
	TestTrue(TEXT("repeat reset"), FAIIndexer::PrepareIndex(DB));
	AddNode(TEXT("AIAssetSummary"), TEXT("UBehaviorTree"));
	TestTrue(TEXT("resumed reset removes previous pass summary"), FAIIndexer::PrepareIndex(DB));
	TestTrue(TEXT("repeated reset is idempotent"), FAIIndexer::PrepareIndex(DB));
	TestEqual(TEXT("never accumulates summary duplicates or erases graphs"), DB.GetNodesForAsset(Id).Num(), 3);
	TestTrue(TEXT("open different database"), SecondDB.Open(SecondPath));
	TestTrue(TEXT("begin schema creation"), SecondDB.BeginTransaction());
	TestTrue(TEXT("schema setup after database switch"), FAIIndexer::PrepareIndex(SecondDB));
	TestTrue(TEXT("rollback schema creation"), SecondDB.RollbackTransaction());
	TestTrue(TEXT("schema is recreated after rollback"), FAIIndexer::PrepareIndex(SecondDB));
	TestEqual(TEXT("second DB has fresh catalog"), AIIndexCount(SecondDB, TEXT("ai_assets")), int64(0));
	SecondDB.Close();
	TestFalse(TEXT("closed database fails cleanly"), FAIIndexer::PrepareIndex(SecondDB));
	return true;
}
#endif
