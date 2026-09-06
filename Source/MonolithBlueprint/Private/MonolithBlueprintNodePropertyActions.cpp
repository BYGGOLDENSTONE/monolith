#include "MonolithBlueprintNodeActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithBlueprintCompileActions.h"
#include "MonolithPinTypeGrammar.h"
#include "MonolithPackagePathValidator.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"

namespace MonolithNodePropertyActions
{
bool Editable(const FProperty* Property)
{
	return Property && Property->HasAnyPropertyFlags(CPF_Edit) &&
		!Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_DisableEditOnInstance | CPF_InstancedReference | CPF_ContainsInstancedReference);
}

FMonolithActionResult Resolve(const TSharedPtr<FJsonObject>& Params, UBlueprint*& Blueprint, UEdGraphNode*& Node)
{
	FString AssetPath, GraphName, NodeId;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath == TEXT("$current"))
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		Blueprint = World && World->PersistentLevel ? World->PersistentLevel->GetLevelScriptBlueprint(/*bDontCreate=*/true) : nullptr;
	}
	else
	{
		Blueprint = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	}
	if (!Blueprint) return FMonolithActionResult::NotFound(TEXT("Blueprint"), AssetPath);
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Params->TryGetStringField(TEXT("node_id"), NodeId);
	if (GraphName.IsEmpty() || NodeId.IsEmpty())
		return FMonolithActionResult::Error(TEXT("graph_name and node_id are required"));
	Node = MonolithBlueprintInternal::FindNodeById(Blueprint, GraphName, NodeId);
	// Most legacy Blueprint actions call the object name an ID. This API also returns
	// and accepts real GUIDs, so resolve those explicitly inside the requested graph.
	if (!Node)
	{
		FGuid Guid;
		UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(Blueprint, GraphName);
		if (Graph && FGuid::Parse(NodeId, Guid) && Guid.IsValid())
		{
			for (UEdGraphNode* Candidate : Graph->Nodes)
			{
				if (Candidate && Candidate->NodeGuid == Guid)
				{
					if (Node) return FMonolithActionResult::Error(TEXT("Multiple nodes share this GUID in the requested graph; use the exact node name"));
					Node = Candidate;
				}
			}
		}
	}
	if (!Node) return FMonolithActionResult::NotFound(TEXT("Graph node"), NodeId);
	return FMonolithActionResult::Success(MakeShared<FJsonObject>());
}

TSharedPtr<FJsonObject> Describe(UEdGraphNode* Node)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
	Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
	TArray<TSharedPtr<FJsonValue>> Properties, Pins;
	for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
	{
		if (!Editable(*It)) continue;
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), It->GetName());
		Entry->SetStringField(TEXT("type"), It->GetCPPType());
		FString Value;
		It->ExportText_InContainer(0, Value, Node, Node, Node, PPF_None);
		Entry->SetStringField(TEXT("value"), Value);
		Properties.Add(MakeShared<FJsonValueObject>(Entry));
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Entry->SetStringField(TEXT("id"), Pin->PinId.ToString());
		Entry->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		Entry->SetStringField(TEXT("type"), MonolithPinTypeGrammar::ContainerPrefix(Pin->PinType) + MonolithPinTypeGrammar::PinTypeToString(Pin->PinType));
		Entry->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
		Entry->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
		Pins.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Result->SetArrayField(TEXT("properties"), Properties);
	Result->SetArrayField(TEXT("pins"), Pins);
	return Result;
}
}

FMonolithActionResult FMonolithBlueprintNodeActions::HandleGetGraphNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraphNode* Node = nullptr;
	auto Resolution = MonolithNodePropertyActions::Resolve(Params, Blueprint, Node);
	if (!Resolution.bSuccess) return Resolution;
	return FMonolithActionResult::Success(MonolithNodePropertyActions::Describe(Node));
}

FMonolithActionResult FMonolithBlueprintNodeActions::HandleSetGraphNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraphNode* Node = nullptr;
	auto Resolution = MonolithNodePropertyActions::Resolve(Params, Blueprint, Node);
	if (!Resolution.bSuccess) return Resolution;
	FString WritableError;
	const FString PackagePath = Blueprint->GetOutermost()->GetName();
	if (!MonolithCore::EnsureWritablePackagePath(PackagePath, WritableError))
		return MonolithCore::WritablePathError(PackagePath, WritableError);

	FString Path, Value;
	if (!Params->TryGetStringField(TEXT("property_name"), Path) || Path.IsEmpty() || !Params->TryGetStringField(TEXT("value"), Value))
		return FMonolithActionResult::Error(TEXT("property_name and string value are required"));
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."), false);
	UStruct* Owner = Node->GetClass();
	void* Container = Node;
	FProperty* Leaf = nullptr;
	FProperty* Member = nullptr;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		Leaf = Owner->FindPropertyByName(FName(*Segments[Index]));
		if (!MonolithNodePropertyActions::Editable(Leaf))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Property '%s' is missing or not editable"), *Segments[Index]));
		if (!Member) Member = Leaf;
		if (Index + 1 < Segments.Num())
		{
			FStructProperty* Struct = CastField<FStructProperty>(Leaf);
			if (!Struct) return FMonolithActionResult::Error(TEXT("Only struct traversal is supported; object and container traversal is refused"));
			Container = Struct->ContainerPtrToValuePtr<void>(Container);
			Owner = Struct->Struct;
		}
	}
	if (!Leaf) return FMonolithActionResult::Error(TEXT("Empty property path"));
	if (CastField<FStructProperty>(Leaf) || CastField<FArrayProperty>(Leaf) || CastField<FSetProperty>(Leaf) || CastField<FMapProperty>(Leaf))
		return FMonolithActionResult::Error(TEXT("Edit a concrete scalar struct field through its dotted path; whole structs and containers are refused to protect non-editable members"));
	void* Destination = Leaf->ContainerPtrToValuePtr<void>(Container);
	void* Scratch = FMemory::Malloc(Leaf->GetSize(), Leaf->GetMinAlignment());
	Leaf->InitializeValue(Scratch);
	ON_SCOPE_EXIT { Leaf->DestroyValue(Scratch); FMemory::Free(Scratch); };
	Leaf->CopyCompleteValue(Scratch, Destination);
	FString OldValue;
	Leaf->ExportText_Direct(OldValue, Destination, Destination, Node, PPF_None);
	const TCHAR* End = Leaf->ImportText_Direct(*Value, Scratch, Node, PPF_None);
	if (!End || !FString(End).TrimStartAndEnd().IsEmpty())
		return FMonolithActionResult::Error(TEXT("Invalid property value; node was not modified"));
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("Set Blueprint Node Property")));
		Blueprint->Modify();
		Node->GetGraph()->Modify();
		Node->Modify();
		Node->PreEditChange(Member);
		Leaf->CopyCompleteValue(Destination, Scratch);
		FPropertyChangedEvent Event(Leaf, EPropertyChangeType::ValueSet);
		Event.SetActiveMemberProperty(Member);
		Node->PostEditChangeProperty(Event);
		if (const UEdGraphSchema* Schema = Node->GetSchema()) Schema->ReconstructNode(*Node);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	auto Result = MonolithNodePropertyActions::Describe(Node);
	Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("property_name"), Path);
	Result->SetStringField(TEXT("old_value"), OldValue);
	Result->SetBoolField(TEXT("saved"), false);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	if (bSave)
	{
		auto SaveParams = MakeShared<FJsonObject>();
		SaveParams->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		auto Saved = FMonolithBlueprintCompileActions::HandleSaveAsset(SaveParams);
		Result->SetBoolField(TEXT("saved"), Saved.bSuccess);
		if (!Saved.bSuccess)
		{
			Result->SetStringField(TEXT("save_error"), Saved.ErrorMessage);
			Result->SetBoolField(TEXT("mutation_applied"), true);
			Result->SetBoolField(TEXT("executed"), true);
			return FMonolithActionResult::Error(TEXT("Node property changed, but saving the Blueprint failed: ") + Saved.ErrorMessage).WithErrorData(Result);
		}
	}
	return FMonolithActionResult::Success(Result);
}
