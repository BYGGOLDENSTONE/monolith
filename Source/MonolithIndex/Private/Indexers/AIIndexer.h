#pragma once
#include "MonolithIndexer.h"

/** Read-only runtime structure of BehaviorTree, BlackboardData and EnvQuery assets. */
class FMonolithAIAssetIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override { return { TEXT("BehaviorTree"), TEXT("BlackboardData"), TEXT("EnvQuery") }; }
	virtual FString GetName() const override { return TEXT("AIAssetIndexer"); }
	virtual bool IndexAsset(const FAssetData&, UObject* Asset, FMonolithIndexDatabase& DB, int64 AssetId) override;
};
