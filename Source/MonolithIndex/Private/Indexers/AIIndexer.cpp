#include "Indexers/AIIndexer.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "MonolithJsonUtils.h"
#include "UObject/UnrealType.h"

bool FMonolithAIAssetIndexer::IndexAsset(const FAssetData&, UObject* Asset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!Asset || AssetId < 0) return false;
	bool bSuccess = true;
	TMap<UObject*, int64> Seen;
	auto Add = [&](UObject* Object, const TCHAR* Type) -> int64
	{
		if (!Object) return -1;
		if (const int64* Existing = Seen.Find(Object)) return *Existing;
		auto Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("object_path"), Object->GetPathName());
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Transient)) continue;
			FString Value;
			It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
			Props->SetStringField(It->GetName(), Value);
		}
		FIndexedNode Node;
		Node.AssetId = AssetId;
		Node.NodeName = Object->GetName();
		Node.NodeClass = Object->GetClass()->GetName();
		Node.NodeType = Type;
		Node.Properties = FMonolithJsonUtils::Serialize(Props);
		int64 Id = DB.InsertNode(Node);
		bSuccess &= Id >= 0;
		Seen.Add(Object, Id);
		return Id;
	};
	auto Connect = [&](int64 Parent, int64 Child, const FString& Relation)
	{
		if (Parent < 0 || Child < 0) { bSuccess = false; return; }
		FIndexedConnection Edge;
		Edge.SourceNodeId = Parent;
		Edge.TargetNodeId = Child;
		Edge.SourcePin = Relation;
		Edge.TargetPin = TEXT("Self");
		Edge.PinType = TEXT("AI_Structure");
		bSuccess &= DB.InsertConnection(Edge) >= 0;
	};

	if (UBehaviorTree* Tree = Cast<UBehaviorTree>(Asset))
	{
		const int64 TreeId = Add(Tree, TEXT("BehaviorTree"));
		TSet<UBTNode*> Expanded;
		TFunction<int64(UBTNode*)> Walk = [&](UBTNode* Node) -> int64
		{
			if (!Node) return -1;
			const int64 Id = Add(Node, TEXT("BehaviorTreeNode"));
			if (Expanded.Contains(Node)) return Id;
			Expanded.Add(Node);
			if (UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
			{
				for (int32 I = 0; I < Composite->Children.Num(); ++I)
				{
					const FBTCompositeChild& Child = Composite->Children[I];
					UBTNode* ChildNode = Child.ChildComposite ? static_cast<UBTNode*>(Child.ChildComposite.Get()) : static_cast<UBTNode*>(Child.ChildTask.Get());
					if (ChildNode) Connect(Id, Walk(ChildNode), FString::Printf(TEXT("child_%d"), I));
					for (int32 D = 0; D < Child.Decorators.Num(); ++D)
						if (Child.Decorators[D]) Connect(Id, Add(Child.Decorators[D], TEXT("BehaviorTreeDecorator")), FString::Printf(TEXT("child_%d_decorator_%d"), I, D));
				}
				for (UBTService* Service : Composite->Services)
					if (Service) Connect(Id, Add(Service, TEXT("BehaviorTreeService")), TEXT("service"));
			}
			if (UBTTaskNode* Task = Cast<UBTTaskNode>(Node))
				for (UBTService* Service : Task->Services)
					if (Service) Connect(Id, Add(Service, TEXT("BehaviorTreeService")), TEXT("service"));
			return Id;
		};
		if (Tree->RootNode) Connect(TreeId, Walk(Tree->RootNode), TEXT("root"));
		for (UBTDecorator* Decorator : Tree->RootDecorators)
			if (Decorator) Connect(TreeId, Add(Decorator, TEXT("BehaviorTreeDecorator")), TEXT("root_decorator"));
		// Record the actual blackboard reference even when the field is not CPF_Edit.
		if (Tree->BlackboardAsset) Connect(TreeId, Add(Tree->BlackboardAsset, TEXT("BlackboardReference")), TEXT("blackboard"));
	}
	else if (UBlackboardData* Blackboard = Cast<UBlackboardData>(Asset))
	{
		Add(Blackboard, TEXT("Blackboard"));
		TSet<FName> Names;
		TSet<UBlackboardData*> Parents;
		for (UBlackboardData* Current = Blackboard; Current && !Parents.Contains(Current); Current = Current->Parent)
		{
			Parents.Add(Current);
			for (const FBlackboardEntry& Entry : Current->Keys)
			{
				if (Names.Contains(Entry.EntryName)) continue;
				Names.Add(Entry.EntryName);
				FIndexedVariable Var;
				Var.AssetId = AssetId;
				Var.VarName = Entry.EntryName.ToString();
				Var.VarType = Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("None");
				Var.Category = Current == Blackboard ? TEXT("Blackboard") : TEXT("BlackboardInherited");
				Var.bIsReplicated = false; // Instance sync is not network replication.
				auto Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("owner"), Current->GetPathName());
				Details->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced);
				Var.DefaultValue = FMonolithJsonUtils::Serialize(Details);
				bSuccess &= DB.InsertVariable(Var) >= 0;
			}
		}
	}
	else if (UEnvQuery* Query = Cast<UEnvQuery>(Asset))
	{
		int64 QueryId = Add(Query, TEXT("EnvQuery"));
		int32 I = 0;
		for (UEnvQueryOption* Option : Query->GetOptions())
		{
			if (!Option) continue;
			int64 OptionId = Add(Option, TEXT("EnvQueryOption"));
			Connect(QueryId, OptionId, FString::Printf(TEXT("option_%d"), I++));
			if (Option->Generator) Connect(OptionId, Add(Option->Generator, TEXT("EnvQueryGenerator")), TEXT("generator"));
			for (int32 T = 0; T < Option->Tests.Num(); ++T)
				if (Option->Tests[T]) Connect(OptionId, Add(Option->Tests[T], TEXT("EnvQueryTest")), FString::Printf(TEXT("test_%d"), T));
		}
	}
	else return false;
	return bSuccess;
}
