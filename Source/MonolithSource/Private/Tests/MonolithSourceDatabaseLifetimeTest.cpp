#include "Misc/AutomationTest.h"
#include "MonolithSourceDatabase.h"
#include "SQLiteDatabase.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceDatabaseLifetimeTest,
	"Monolith.Source.Database.RefuseReplacementWithOutstandingStatement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceDatabaseLifetimeTest::RunTest(const FString& /*Parameters*/)
{
	const FString OriginalPath = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("SourceLifetime"), TEXT(".db"));
	const FString ReplacementPath = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("SourceReplacement"), TEXT(".db"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*OriginalPath);
		IFileManager::Get().Delete(*ReplacementPath);
	};

	FMonolithSourceDatabase Db;
	if (!TestTrue(TEXT("Open original fixture"), Db.OpenForWriting(OriginalPath))) return false;
	FSQLiteDatabase* OriginalHandle = Db.GetRawHandle();
	const FString OriginalFilename = OriginalHandle->GetFilename();
	int32 GuardCount = 0;
	auto PreviousHandler = SetEnsureHandler([&GuardCount](const FEnsureHandlerArgs& Args)
	{
		if (!FString(Args.Message).Contains(TEXT("Monolith SQLite Close failed"))) return false;
		++GuardCount;
		return true;
	});
	ON_SCOPE_EXIT { SetEnsureHandler(MoveTemp(PreviousHandler)); };
	{
		FSQLitePreparedStatement Stmt;
		TestTrue(TEXT("Prepare outstanding statement"), Stmt.Create(*OriginalHandle, TEXT("SELECT 1;")));
		TestFalse(TEXT("Writer replacement refused while statement is open"), Db.OpenForWriting(ReplacementPath));
		TestFalse(TEXT("Reader replacement refused while statement is open"), Db.Open(ReplacementPath));
		TestTrue(TEXT("Refused replacement retains original handle"), Db.GetRawHandle() == OriginalHandle);
		TestEqual(TEXT("Refused replacement retains original database"), OriginalHandle->GetFilename(), OriginalFilename);
		TestFalse(TEXT("Refused replacement creates no database file"), IFileManager::Get().FileExists(*ReplacementPath));
		TestTrue(TEXT("Original statement still executes"), Stmt.Step() == ESQLitePreparedStatementStepResult::Row);
	}
	Db.Close();
	TestEqual(TEXT("Close guard fired for each refused replacement"), GuardCount, 2);
	TestFalse(TEXT("Close succeeds after statement finalizes"), Db.IsOpen());
	TestTrue(TEXT("Writer replacement succeeds after close"), Db.OpenForWriting(ReplacementPath));
	Db.Close();
	TestTrue(TEXT("Reader reopen succeeds after close"), Db.Open(ReplacementPath));
	Db.Close();
	return true;
}
#endif
