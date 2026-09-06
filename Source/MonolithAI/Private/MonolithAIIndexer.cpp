#include "MonolithAIIndexer.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

DEFINE_LOG_CATEGORY_STATIC(LogAIIndexer, Log, All);

namespace
{
	int64 InsertAIAsset(FSQLiteDatabase* Raw, const FString& Path, const FString& Type, const FString& Name)
	{
		if (!Raw) return -1;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT("INSERT INTO ai_assets(path,type,name) VALUES(?,?,?);"))) return -1;
		Stmt.SetBindingValueByIndex(1, Path);
		Stmt.SetBindingValueByIndex(2, Type);
		Stmt.SetBindingValueByIndex(3, Name);
		return Stmt.Execute() ? Raw->GetLastInsertRowId() : -1;
	}

	bool InsertBBKey(FSQLiteDatabase* Raw, int64 BBId, const FString& Name, const FString& Type)
	{
		if (!Raw || BBId < 0) return false;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT("INSERT INTO ai_bb_keys(bb_id,key_name,key_type) VALUES(?,?,?);"))) return false;
		Stmt.SetBindingValueByIndex(1, BBId);
		Stmt.SetBindingValueByIndex(2, Name);
		Stmt.SetBindingValueByIndex(3, Type);
		return Stmt.Execute();
	}

	bool InsertSummary(FMonolithIndexDatabase& DB, const FString& Path, const FString& Name,
		const FString& Class, const TSharedPtr<FJsonObject>& Props)
	{
		const int64 AssetId = DB.GetAssetId(Path);
		if (AssetId <= 0) return true; // Outside the main index's asset filter; private catalog still useful.
		FIndexedNode Node;
		Node.AssetId = AssetId;
		Node.NodeName = Name;
		Node.NodeClass = Class;
		Node.NodeType = TEXT("AIAssetSummary"); // Exclusive ownership tag; never erase the structure indexer's rows.
		Node.Properties = FMonolithJsonUtils::Serialize(Props);
		return DB.InsertNode(Node) >= 0;
	}
}

bool FAIIndexer::PrepareIndex(FMonolithIndexDatabase& DB)
{
	FSQLiteDatabase* Raw = DB.GetRawDatabase();
	if (!Raw || !DB.IsOpen()) return false;
	// Do not cache schema setup across databases or rollback: CREATE IF NOT EXISTS
	// is cheap, while an object-level bTablesCreated becomes stale after either.
	const TCHAR* Statements[] = {
		TEXT("CREATE TABLE IF NOT EXISTS ai_assets(id INTEGER PRIMARY KEY AUTOINCREMENT,path TEXT NOT NULL UNIQUE,type TEXT NOT NULL,name TEXT NOT NULL);"),
		TEXT("CREATE TABLE IF NOT EXISTS ai_cross_refs(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER NOT NULL,target_id INTEGER NOT NULL,ref_type TEXT NOT NULL,FOREIGN KEY(source_id) REFERENCES ai_assets(id),FOREIGN KEY(target_id) REFERENCES ai_assets(id));"),
		TEXT("CREATE TABLE IF NOT EXISTS ai_bb_keys(id INTEGER PRIMARY KEY AUTOINCREMENT,bb_id INTEGER NOT NULL,key_name TEXT NOT NULL,key_type TEXT NOT NULL,FOREIGN KEY(bb_id) REFERENCES ai_assets(id));"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_ai_assets_type ON ai_assets(type);"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_ai_cross_refs_source ON ai_cross_refs(source_id);"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_ai_cross_refs_target ON ai_cross_refs(target_id);"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_ai_bb_keys_bb ON ai_bb_keys(bb_id);"),
		TEXT("DELETE FROM ai_bb_keys;"),
		TEXT("DELETE FROM ai_cross_refs;"),
		TEXT("DELETE FROM ai_assets;"),
		// Legacy summaries had shared node_type names. Narrow migration cleanup by
		// their old exact class labels; the new structure index uses UClass names without U.
		TEXT("DELETE FROM nodes WHERE node_type='AIAssetSummary' OR (node_type='BehaviorTree' AND node_class='UBehaviorTree') OR (node_type='BlackboardData' AND node_class='UBlackboardData') OR node_type='AIController';")
	};
	for (const TCHAR* SQL : Statements)
		if (!Raw->Execute(SQL)) return false;
	return true;
}

int32 FAIIndexer::CountBTNodes(const UBTCompositeNode* Root)
{
	TSet<const UBTNode*> Seen;
	TFunction<int32(const UBTNode*)> Visit = [&](const UBTNode* Node) -> int32
	{
		if (!Node || Seen.Contains(Node)) return 0;
		Seen.Add(Node);
		int32 Count = 1;
		if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
		{
			for (const auto& Child : Composite->Children)
			{
				Count += Child.ChildComposite ? Visit(Child.ChildComposite) : Visit(Child.ChildTask);
				for (const UBTDecorator* Decorator : Child.Decorators) Count += Visit(Decorator);
			}
			for (const UBTService* Service : Composite->Services) Count += Visit(Service);
		}
		if (const UBTTaskNode* Task = Cast<UBTTaskNode>(Node))
			for (const UBTService* Service : Task->Services) Count += Visit(Service);
		return Count;
	};
	return Visit(Root);
}

bool FAIIndexer::IndexAsset(const FAssetData&, UObject*, FMonolithIndexDatabase& DB, int64)
{
	// Full-index dispatch owns the transaction and compiler-idle game-thread gate.
	check(IsInGameThread());
	PendingReferences.Empty();
	if (!PrepareIndex(DB)) return false;
	FSQLiteDatabase* Raw = DB.GetRawDatabase();
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	for (FName Path : UMonolithSettings::GetIndexedContentPaths()) Filter.PackagePaths.Add(Path);
	Filter.bRecursivePaths = true;
	int32 BTCount = 0, BBCount = 0, ControllerCount = 0;
	{
		FARFilter BBFilter = Filter;
		BBFilter.ClassPaths.Add(UBlackboardData::StaticClass()->GetClassPathName());
		TArray<FAssetData> Assets; Registry.GetAssets(BBFilter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			UBlackboardData* BB = Cast<UBlackboardData>(Asset.GetAsset());
			if (!BB) continue;
			if (!IndexBlackboard(BB, Asset.PackageName.ToString(), DB)) return false;
			++BBCount;
		}
	}
	{
		FARFilter BTFilter = Filter;
		BTFilter.ClassPaths.Add(UBehaviorTree::StaticClass()->GetClassPathName());
		TArray<FAssetData> Assets; Registry.GetAssets(BTFilter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			UBehaviorTree* BT = Cast<UBehaviorTree>(Asset.GetAsset());
			if (!BT) continue;
			if (!IndexBehaviorTree(BT, Asset.PackageName.ToString(), DB)) return false;
			++BTCount;
		}
	}
	{
		FARFilter BPFilter = Filter;
		BPFilter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		TArray<FAssetData> Assets; Registry.GetAssets(BPFilter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
			if (!BP || !BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(AAIController::StaticClass())) continue;
			if (!IndexAIController(BP, Asset.PackageName.ToString(), DB)) return false;
			++ControllerCount;
		}
	}
	// Resolve only after EVERY catalog row exists: AssetRegistry enumeration order
	// must not decide whether child->parent Blackboard relationships are indexed.
	for (const auto& Ref : PendingReferences)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT("INSERT INTO ai_cross_refs(source_id,target_id,ref_type) SELECT s.id,t.id,? FROM ai_assets s JOIN ai_assets t ON t.path=? WHERE s.path=?;"))) return false;
		Stmt.SetBindingValueByIndex(1, Ref.Type);
		Stmt.SetBindingValueByIndex(2, Ref.Target);
		Stmt.SetBindingValueByIndex(3, Ref.Source);
		if (!Stmt.Execute()) return false;
	}
	PendingReferences.Empty();
	UE_LOG(LogAIIndexer, Log, TEXT("AIIndexer: indexed %d BTs, %d Blackboards, %d Controllers"), BTCount, BBCount, ControllerCount);
	return true;
}

bool FAIIndexer::IndexBehaviorTree(UBehaviorTree* BT, const FString& Path, FMonolithIndexDatabase& DB)
{
	if (!BT || InsertAIAsset(DB.GetRawDatabase(), Path, TEXT("BehaviorTree"), BT->GetName()) < 0) return false;
	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("asset_type"), TEXT("BehaviorTree"));
	Props->SetNumberField(TEXT("node_count"), CountBTNodes(BT->RootNode));
	if (BT->BlackboardAsset)
	{
		Props->SetStringField(TEXT("blackboard"), BT->BlackboardAsset->GetPathName());
		PendingReferences.Add({ Path, BT->BlackboardAsset->GetOutermost()->GetName(), TEXT("uses_blackboard") });
	}
	return InsertSummary(DB, Path, BT->GetName(), TEXT("UBehaviorTree"), Props);
}

bool FAIIndexer::IndexBlackboard(UBlackboardData* BB, const FString& Path, FMonolithIndexDatabase& DB)
{
	if (!BB) return false;
	FSQLiteDatabase* Raw = DB.GetRawDatabase();
	int64 Id = InsertAIAsset(Raw, Path, TEXT("BlackboardData"), BB->GetName());
	if (Id < 0) return false;
	for (const FBlackboardEntry& Key : BB->Keys)
		if (!InsertBBKey(Raw, Id, Key.EntryName.ToString(), Key.KeyType ? Key.KeyType->GetClass()->GetName() : TEXT("Unknown"))) return false;
	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("asset_type"), TEXT("BlackboardData"));
	Props->SetNumberField(TEXT("key_count"), BB->Keys.Num());
	if (BB->Parent)
	{
		Props->SetStringField(TEXT("parent_blackboard"), BB->Parent->GetOutermost()->GetName());
		PendingReferences.Add({ Path, BB->Parent->GetOutermost()->GetName(), TEXT("inherits_blackboard") });
	}
	return InsertSummary(DB, Path, BB->GetName(), TEXT("UBlackboardData"), Props);
}

bool FAIIndexer::IndexAIController(UBlueprint* BP, const FString& Path, FMonolithIndexDatabase& DB)
{
	if (!BP || !BP->GeneratedClass || InsertAIAsset(DB.GetRawDatabase(), Path, TEXT("AIController"), BP->GetName()) < 0) return false;
	AAIController* CDO = Cast<AAIController>(BP->GeneratedClass->GetDefaultObject(false));
	const FString ParentClass = BP->GeneratedClass->GetSuperClass() ? BP->GeneratedClass->GetSuperClass()->GetName() : TEXT("AAIController");
	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("asset_type"), TEXT("AIController"));
	Props->SetStringField(TEXT("parent_class"), ParentClass);
	Props->SetBoolField(TEXT("cdo_available"), CDO != nullptr);
	if (CDO)
	{
		Props->SetBoolField(TEXT("has_perception"), CDO->PerceptionComponent != nullptr);
		Props->SetNumberField(TEXT("team_id"), static_cast<int32>(CDO->GetGenericTeamId().GetId()));
	}
	return InsertSummary(DB, Path, BP->GetName(), ParentClass, Props);
}
