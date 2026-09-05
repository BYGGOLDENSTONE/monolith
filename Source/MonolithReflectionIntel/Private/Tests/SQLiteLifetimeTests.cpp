#include "Misc/AutomationTest.h"
#include "MonolithSQLiteDatabase.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSQLiteLifetimeTest,
	"Monolith.ReflectionIntel.SQLite.CloseWithOutstandingStatement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSQLiteLifetimeTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("SQLiteLifetime"), TEXT(".db"));
	FMonolithSQLiteDatabase Db;
	if (!TestTrue(TEXT("Open fixture"), Db.Open(*Path))) return false;
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
		TestTrue(TEXT("Prepare statement"), Stmt.Create(Db, TEXT("SELECT 1;")));
		TestFalse(TEXT("Outstanding statement prevents close"), Db.Close());
		TestTrue(TEXT("Failed close retains handle"), Db.IsValid());
	}
	TestTrue(TEXT("Close succeeds after statement destruction"), Db.Close());
	TestEqual(TEXT("Close guard fired once"), GuardCount, 1);
	TestFalse(TEXT("Handle closed"), Db.IsValid());
	IFileManager::Get().Delete(*Path);
	return true;
}
#endif
