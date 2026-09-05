// SPDX-License-Identifier: MIT
#pragma once
#include "CoreMinimal.h"
#include "Templates/Atomic.h"

/** Captured values for indexers owning their database on a single worker. */
struct FRiskMiningWorkerContext
{
	FString ProjectRoot;
	const TAtomic<bool>* CancelRequested = nullptr;
	TAtomic<int32>* CompletedRepos = nullptr;
	TAtomic<int32>* CompletedFiles = nullptr;
	bool IsCancelled() const { return CancelRequested && CancelRequested->Load(); }
};
