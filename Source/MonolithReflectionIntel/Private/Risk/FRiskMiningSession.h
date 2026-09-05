// SPDX-License-Identifier: MIT
#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Dom/JsonObject.h"

class FSQLiteDatabase;
class FMonolithSQLiteDatabase;

struct FRiskComplexityRow
{
	FString Path;
	int32 LineCount = 0;
	FString FileType;
};

/** Immutable input snapshot; no UObject or borrowed database references. */
struct FRiskMiningInputs
{
	FString ProjectRoot;
	FString DatabaseDirectory;
	TArray<FString> GitRoots;
	TArray<FString> GateRoots;
	TArray<TPair<FString, FString>> SkippedRoots;
	TArray<FRiskComplexityRow> ComplexityRows;
	TArray<FString> NoiseFilter;
	int32 MaxCommitWindow = 200;
	int32 MaxCommitFileCount = 20;
	int32 ConfigFingerprint = 0;
	int32 CodeVersion = 2;
	FString SnapshotAt;
	double SnapshotCaptureMs = 0.0;
#if WITH_DEV_AUTOMATION_TESTS
	/** Fixture gate before DB open, never substitutes mining output. */
	TFunction<void(const TAtomic<bool>&)> BeforeWorker;
#endif
};

/** One explicit mining pass at a time; SQLite writes belong to its worker. */
class FRiskMiningSession
{
public:
	FRiskMiningSession();
	~FRiskMiningSession();
	bool Start(const FRiskMiningInputs& Inputs, FString& OutError);
	void Poll();
	bool LoadCache(const FRiskMiningInputs& Inputs, FString& OutError);
	static bool IsCacheValid(FSQLiteDatabase& DB, const FRiskMiningInputs& Inputs);
	bool IsRunning() const;
	TSharedPtr<FJsonObject> GetStatus();
	FSQLiteDatabase* GetQueryDB();
	void Invalidate();
	void Shutdown();

private:
	struct FWorkerState;
	TSharedPtr<FWorkerState, ESPMode::ThreadSafe> Worker;
	TFuture<void> Future;
	TUniquePtr<FMonolithSQLiteDatabase> QueryDB;
	FRiskMiningInputs LastInputs;
	FString State = TEXT("idle");
	FString LastStatus;
	FString PublishedPath;
	int32 PublishedComplexityRows = 0;
	uint64 Generation = 0;
	bool bInvalidated = false;
	bool bStopping = false;
	void ClearPublished();
};
