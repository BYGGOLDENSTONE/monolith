#pragma once
#include "MonolithIndexer.h"

/** Full-pass AI summaries and cross-asset tables, complementary to per-asset AI structure. */
class FAIIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override { return { TEXT("__AI__") }; }
	virtual bool IndexAsset(const FAssetData&, UObject*, FMonolithIndexDatabase& DB, int64) override;
	virtual FString GetName() const override { return TEXT("AI"); }
	virtual bool IsSentinel() const override { return true; }

	/** Schema + ownership-safe replacement, inside the caller's transaction. Also used by DB regression tests. */
	static bool PrepareIndex(FMonolithIndexDatabase& DB);

private:
	bool IndexBehaviorTree(class UBehaviorTree* BT, const FString& Path, FMonolithIndexDatabase& DB);
	bool IndexBlackboard(class UBlackboardData* BB, const FString& Path, FMonolithIndexDatabase& DB);
	bool IndexAIController(class UBlueprint* BP, const FString& Path, FMonolithIndexDatabase& DB);
	static int32 CountBTNodes(const class UBTCompositeNode* RootNode);

	struct FPendingReference { FString Source, Target, Type; };
	TArray<FPendingReference> PendingReferences;
};
