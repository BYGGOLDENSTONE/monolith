// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** Editor-wide cooperative lease shared by every transport/client of this process.
 * This coordinates workflows, not parallel UObject execution or security identities.
 * All registry execution remains on the game thread. Time is monotonic.
 */
class FMonolithCoordination
{
public:
	static FMonolithCoordination& Get();
	explicit FMonolithCoordination(TFunction<double()> InClock = {});
	static void RegisterTool();
	FMonolithActionResult Handle(const TSharedPtr<FJsonObject>& Params);
	FMonolithActionResult CheckAccess(const FString& Namespace, const FString& Action,
		const TSharedPtr<FJsonObject>& Params, FString& OutExecutionToken, bool bInheritLeaseContext = true);

	class FExecutionScope;
	/** Pins the first validated leased execution until the HTTP batch finishes.
	 * Each item still validates its own token; no dispatch context is inherited.
	 */
	class FBatchScope
	{
	public:
		explicit FBatchScope(FMonolithCoordination& InCoordinator);
		~FBatchScope();
		FBatchScope(const FBatchScope&) = delete;
		FBatchScope& operator=(const FBatchScope&) = delete;
	private:
		friend class FExecutionScope;
		FMonolithCoordination& Coordinator;
		FBatchScope* PreviousBatch;
		bool bPinned = false;
	};

	/** Synchronous nested registry dispatch inherits the validated parent's token.
	 * Active calls finish even if their lease expires, then receive a short release
	 * grace window. Never propagate this context to asynchronous/background work.
	 */
	class FExecutionScope
	{
	public:
		FExecutionScope(FMonolithCoordination& InCoordinator, const FString& Token);
		~FExecutionScope();
		FExecutionScope(const FExecutionScope&) = delete;
		FExecutionScope& operator=(const FExecutionScope&) = delete;
	private:
		FMonolithCoordination& Coordinator;
		FString PreviousToken;
		bool bLeased;
	};

private:
	void ExpireLocked();
	void EndLeasedExecutionLocked();
	TSharedPtr<FJsonObject> StatusLocked() const;
	FMonolithActionResult ErrorLocked(const TCHAR* Reason, const TCHAR* Message, int32 Code) const;
	static bool IsExempt(const FString& Namespace, const FString& Action);
	TFunction<double()> Clock;
	FCriticalSection Mutex;
	FString Owner;
	FString LeaseToken;
	double ExpiresAt = 0;
	double LeaseTTLSeconds = 120;
	// One release grace per deadline; renew/acquire re-arm it. Without this an
	// owner that keeps overrunning its deadline never has to renew.
	bool bGraceGranted = false;
	int32 ActiveExecutions = 0;
	int32 DispatchDepth = 0;
	// Accessed only on the game thread, never by transport worker threads.
	FString ExecutionToken;
	FBatchScope* CurrentBatch = nullptr;
};
