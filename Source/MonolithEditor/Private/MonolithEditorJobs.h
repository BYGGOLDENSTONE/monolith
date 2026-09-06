#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** Game-thread owned subprocess jobs: ticks poll without blocking or accessing UObjects. */
class FMonolithEditorJobs
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);
	static void Shutdown();
	static FMonolithActionResult StartEncoder(const FString& Encoder, const FString& OutputDir,
		const TArray<FString>& Frames, int32 FPS, int32 Resolution, const TSharedPtr<FJsonObject>& CaptureResult);
	static FMonolithActionResult Status(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Cancel(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Result(const TSharedPtr<FJsonObject>& Params);
};
