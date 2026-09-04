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

	/** Synchronous nested registry dispatch inherits the validated parent's token.
	 * Active calls finish even if their lease expires; expiration applies before the
	 * next top-level call. Never propagate this context to asynchronous/background work.
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
	TSharedPtr<FJsonObject> StatusLocked() const;
	FMonolithActionResult ErrorLocked(const TCHAR* Reason, const TCHAR* Message, int32 Code) const;
	static bool IsExempt(const FString& Namespace, const FString& Action);
	TFunction<double()> Clock;
	FCriticalSection Mutex;
	FString Owner;
	FString LeaseToken;
	double ExpiresAt = 0;
	int32 ActiveExecutions = 0;
	int32 DispatchDepth = 0;
	// Accessed only on the game thread, never by transport worker threads.
	FString ExecutionToken;
};
