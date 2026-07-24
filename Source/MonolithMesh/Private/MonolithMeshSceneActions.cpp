#include "MonolithMeshSceneActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithMeshLightActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"

#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/BlockingVolume.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Selection.h"
#include "UObject/UObjectGlobals.h"
#include "CollisionQueryParams.h"

// ============================================================================
// Static member
// ============================================================================

bool FMonolithMeshSceneActions::bBatchTransactionActive = false;

// ============================================================================
// Helpers
// ============================================================================

namespace SceneActionHelpers
{
	/** Make a JSON array from a FVector */
	TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/** Scoped undo transaction that respects bBatchTransactionActive */
	struct FScopedMeshTransaction
	{
		bool bOwnsTransaction;

		FScopedMeshTransaction(const FText& Description)
			: bOwnsTransaction(!FMonolithMeshSceneActions::bBatchTransactionActive)
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->BeginTransaction(Description);
			}
		}

		~FScopedMeshTransaction()
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->EndTransaction();
			}
		}

		void Cancel()
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->CancelTransaction(0);
				bOwnsTransaction = false; // prevent EndTransaction in destructor
			}
		}
	};

	/**
	 * Levenshtein distance, used only to build a did-you-mean list for a class name
	 * that did not resolve. Same shape as the property suggester in
	 * MonolithMeshLightActions.cpp; it is not shared because that one lives in a
	 * translation-unit-private namespace and hoisting it would touch a file this
	 * change has no other reason to open.
	 */
	int32 EditDistance(const FString& A, const FString& B)
	{
		const int32 LenA = A.Len();
		const int32 LenB = B.Len();
		if (LenA == 0) { return LenB; }
		if (LenB == 0) { return LenA; }

		TArray<int32> Prev, Curr;
		Prev.SetNumUninitialized(LenB + 1);
		Curr.SetNumUninitialized(LenB + 1);
		for (int32 j = 0; j <= LenB; ++j) { Prev[j] = j; }

		for (int32 i = 1; i <= LenA; ++i)
		{
			Curr[0] = i;
			for (int32 j = 1; j <= LenB; ++j)
			{
				const int32 Cost = (A[i - 1] == B[j - 1]) ? 0 : 1;
				Curr[j] = FMath::Min3(Curr[j - 1] + 1, Prev[j] + 1, Prev[j - 1] + Cost);
			}
			Prev = Curr;
		}
		return Prev[LenB];
	}

	/**
	 * Actor class names close to Query, best first. Walks the classes that are
	 * actually loaded — which is exactly the set a class NAME can resolve against, so
	 * the suggestion can never name something the caller could not then spawn.
	 */
	TArray<FString> SuggestActorClassNames(const FString& Query, int32 MaxResults = 5)
	{
		TArray<FString> Out;
		if (Query.IsEmpty())
		{
			return Out;
		}

		struct FCandidate { FString Name; int32 Score; int32 Distance; };
		TArray<FCandidate> Candidates;

		const FString Lower = Query.ToLower();
		const int32 Threshold = FMath::Max(2, Lower.Len() / 4);

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(AActor::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}
			const FString Name = Class->GetName();
			const FString NameLower = Name.ToLower();

			int32 Score = 3;
			if (NameLower == Lower)                                               { Score = 0; }
			else if (NameLower.StartsWith(Lower) || Lower.StartsWith(NameLower))  { Score = 1; }
			else if (NameLower.Contains(Lower) || Lower.Contains(NameLower))      { Score = 2; }

			const int32 Distance = EditDistance(Lower, NameLower);
			if (Score == 3 && Distance > Threshold)
			{
				continue;
			}
			Candidates.Add({ Name, Score, Distance });
		}

		Candidates.Sort([](const FCandidate& A, const FCandidate& B)
		{
			if (A.Score != B.Score) { return A.Score < B.Score; }
			if (A.Distance != B.Distance) { return A.Distance < B.Distance; }
			return A.Name < B.Name;
		});

		for (const FCandidate& C : Candidates)
		{
			if (Out.Num() >= MaxResults) { break; }
			Out.Add(C.Name);
		}
		return Out;
	}

	/**
	 * Everything a spawnable actor class must satisfy, whatever route produced it (a
	 * class NAME or a Blueprint class PATH). One place, so the two routes cannot
	 * drift apart on what they refuse.
	 */
	bool ValidateSpawnableActorClass(UClass* Class, const FString& Token, FString& OutError)
	{
		if (!Class)
		{
			OutError = FString::Printf(TEXT("Class not found: %s"), *Token);
			return false;
		}
		if (!Class->IsChildOf(AActor::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("'%s' resolves to %s, which is not an Actor class and cannot be placed in a level."),
				*Token, *Class->GetName());
			return false;
		}
		if (Class->IsChildOf(ABlockingVolume::StaticClass()))
		{
			OutError = TEXT("Spawning BlockingVolume is not allowed via Monolith. Use the editor directly.");
			return false;
		}
		return true;
	}

	/** Convert a mobility enum to string */
	FString MobilityToString(EComponentMobility::Type Mobility)
	{
		switch (Mobility)
		{
		case EComponentMobility::Static:     return TEXT("Static");
		case EComponentMobility::Stationary: return TEXT("Stationary");
		case EComponentMobility::Movable:    return TEXT("Movable");
		default:                             return TEXT("Unknown");
		}
	}

	/** Parse mobility string to enum, returns false if invalid */
	bool ParseMobility(const FString& Str, EComponentMobility::Type& Out)
	{
		if (Str.Equals(TEXT("Static"), ESearchCase::IgnoreCase))         { Out = EComponentMobility::Static; return true; }
		if (Str.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase))     { Out = EComponentMobility::Stationary; return true; }
		if (Str.Equals(TEXT("Movable"), ESearchCase::IgnoreCase))        { Out = EComponentMobility::Movable; return true; }
		return false;
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshSceneActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_actor_info"),
		TEXT("Get comprehensive info for an actor in the editor world (class, transform, mesh, materials, tags, components, bounds)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::GetActorInfo),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("spawn_actor"),
		TEXT("Spawn an actor in the editor world. `class_or_mesh` takes three spellings: a StaticMesh asset path "
			 "(spawns a StaticMeshActor with that mesh), an object path to a class — a Blueprint as "
			 "'/Game/Props/BP_Barrel' or '/Game/Props/BP_Barrel.BP_Barrel_C' — or a native class NAME "
			 "('PointLight'). Name a Blueprint by PATH: a short name only resolves while that Blueprint is "
			 "already loaded. Any UPROPERTY can be set at spawn time through two bags: "
			 "`properties` targets the ACTOR (e.g. bEnableAutoLODGeneration, Tags) and `component_properties` "
			 "targets its ROOT COMPONENT (e.g. Mobility, CastShadow, bReceivesDecals — the StaticMeshComponent "
			 "for a mesh path). Both are validated against the class BEFORE anything spawns: an unknown name is "
			 "an error listing did-you-mean candidates, there are never partial writes, and a rejected bag leaves "
			 "no actor behind."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::SpawnActor),
		FParamSchemaBuilder()
			.Required(TEXT("class_or_mesh"), TEXT("string"),
				TEXT("StaticMesh asset path ('/Engine/BasicShapes/Cube.Cube'), Blueprint actor class path "
					 "('/Game/Props/BP_Barrel' or '/Game/Props/BP_Barrel.BP_Barrel_C'), or native class name ('PointLight')"))
			.Required(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"), TEXT("[0,0,0]"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("Scale [x, y, z]"), TEXT("[1,1,1]"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Optional label for the spawned actor"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Actor folder path in the outliner"))
			.Optional(TEXT("properties"), TEXT("object"),
				TEXT("UPROPERTYs to set on the spawned ACTOR itself, {name: value} — e.g. {\"bEnableAutoLODGeneration\": false}"))
			.Optional(TEXT("component_properties"), TEXT("object"),
				TEXT("UPROPERTYs to set on the actor's ROOT COMPONENT, {name: value} — e.g. {\"Mobility\": \"Movable\", \"CastShadow\": false}"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("move_actor"),
		TEXT("Move/rotate/scale an actor. Set relative=true to offset from current transform."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::MoveActor),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("Location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"))
			.Optional(TEXT("scale"), TEXT("array"), TEXT("Scale [x, y, z]"))
			.Optional(TEXT("relative"), TEXT("boolean"), TEXT("If true, add to current transform instead of replacing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("duplicate_actor"),
		TEXT("Duplicate an actor in the editor world with an optional position offset"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::DuplicateActor),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label to duplicate"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Label for the duplicate"))
			.Optional(TEXT("offset"), TEXT("array"), TEXT("Position offset [x, y, z]"), TEXT("[0,0,0]"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("delete_actors"),
		TEXT("Delete one or more actors from the editor world. Validates ALL exist before deleting ANY. Does NOT delete asset files."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::DeleteActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names or labels to delete"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("group_actors"),
		TEXT("Move actors into an outliner folder (creates folder hierarchy automatically)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::GroupActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names or labels"))
			.Required(TEXT("group_name"), TEXT("string"), TEXT("Folder path (e.g. 'Furniture/Kitchen')"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("set_actor_properties"),
		TEXT("Set properties on an actor (mobility, physics, collision, shadows, tags, mass)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::SetActorProperties),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Optional(TEXT("mobility"), TEXT("string"), TEXT("Static, Stationary, or Movable"))
			.Optional(TEXT("simulate_physics"), TEXT("boolean"), TEXT("Enable/disable physics simulation"))
			.Optional(TEXT("collision_preset"), TEXT("string"), TEXT("Collision profile name"))
			.Optional(TEXT("cast_shadow"), TEXT("boolean"), TEXT("Enable/disable shadow casting"))
			.Optional(TEXT("tags"), TEXT("array"), TEXT("Array of tag strings to set on the actor"))
			.Optional(TEXT("mass_kg"), TEXT("number"), TEXT("Mass override in kg"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("batch_execute"),
		TEXT("Execute multiple mesh actions in a single undo transaction. Max 200 actions. No nested batch_execute allowed."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::BatchExecute),
		FParamSchemaBuilder()
			.Required(TEXT("actions"), TEXT("array"), TEXT("Array of {action, params} objects"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("align_actors"),
		TEXT("Align multiple actors along an axis. Modes: min/max (snap to extremes), center (average), distribute (spread evenly between min/max)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::AlignActors),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names or labels to align"))
			.Required(TEXT("axis"), TEXT("string"), TEXT("Axis to align on: X, Y, or Z"))
			.Required(TEXT("mode"), TEXT("string"), TEXT("Alignment mode: min, max, center, or distribute"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("snap_to_floor"),
		TEXT("Snap actors to the floor by tracing downward. Places the bottom of each actor on the hit surface."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::SnapToFloor),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names or labels to snap"))
			.Optional(TEXT("trace_distance"), TEXT("number"), TEXT("Maximum downward trace distance"), TEXT("10000"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("manage_folders"),
		TEXT("Manage World Outliner folders: list, delete (move actors to root), rename, or move folders."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSceneActions::ManageFolders),
		FParamSchemaBuilder()
			.Required(TEXT("sub_action"), TEXT("string"), TEXT("Action: list, delete, rename, move"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Target folder path (for delete/rename/move)"))
			.OptionalAssetPath(TEXT("new_folder"), TEXT("New folder path (for rename/move destination)"))
			.Optional(TEXT("include_subfolders"), TEXT("boolean"), TEXT("Include actors in subfolders"), TEXT("true"))
			.Build());
}

// ============================================================================
// 1. get_actor_info
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::GetActorInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Actor->GetFName().ToString());
	Result->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	Result->SetStringField(TEXT("label"), Actor->GetActorLabel());

	// Transform
	Result->SetObjectField(TEXT("transform"), MonolithMeshUtils::TransformToJson(Actor->GetActorTransform()));

	// Mesh path and materials (if StaticMeshActor or has StaticMeshComponent)
	UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
	if (SMC)
	{
		if (UStaticMesh* Mesh = SMC->GetStaticMesh())
		{
			Result->SetStringField(TEXT("mesh"), Mesh->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> MatArr;
		for (int32 i = 0; i < SMC->GetNumMaterials(); ++i)
		{
			UMaterialInterface* Mat = SMC->GetMaterial(i);
			MatArr.Add(MakeShared<FJsonValueString>(Mat ? Mat->GetPathName() : TEXT("None")));
		}
		Result->SetArrayField(TEXT("materials"), MatArr);
	}

	// Mobility
	USceneComponent* Root = Actor->GetRootComponent();
	if (Root)
	{
		Result->SetStringField(TEXT("mobility"), SceneActionHelpers::MobilityToString(Root->Mobility));
	}

	// Tags
	TArray<TSharedPtr<FJsonValue>> TagArr;
	for (const FName& Tag : Actor->Tags)
	{
		TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Result->SetArrayField(TEXT("tags"), TagArr);

	// Components
	TArray<TSharedPtr<FJsonValue>> CompArr;
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	for (UActorComponent* Comp : Components)
	{
		auto CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Comp->GetFName().ToString());
		CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
		CompArr.Add(MakeShared<FJsonValueObject>(CompObj));
	}
	Result->SetArrayField(TEXT("components"), CompArr);

	// World bounds (for actors with a mesh)
	if (SMC && SMC->GetStaticMesh())
	{
		FBoxSphereBounds WorldBounds = SMC->Bounds;
		Result->SetObjectField(TEXT("world_bounds"), MonolithMeshUtils::BoundsToJson(WorldBounds));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// spawn_actor's free-form property channel
//
// Same machinery and same contract as mesh.place_light's `properties` channel:
// InspectTree (strict, scratch buffer) -> bail on ANY error -> Modify +
// PreEditChange per key -> WriteTree -> PostEditChangeProperty per key -> dirty.
// "No partial writes" comes free from validating the whole tree first.
//
// The one thing that is NOT shared with place_light is the addressing: a light
// action can only mean the light component, while spawn_actor spawns arbitrary
// classes, so it names the two targets explicitly — `properties` for the actor,
// `component_properties` for its root component. See the header for why.
// ============================================================================

bool FMonolithMeshSceneActions::ReadPropertyBag(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Key,
	TSharedPtr<FJsonObject>& OutTree,
	FString& OutError)
{
	OutTree.Reset();
	if (!Params.IsValid() || !Params->HasField(Key))
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (!Params->TryGetObjectField(Key, Obj) || !Obj || !(*Obj).IsValid())
	{
		OutError = FString::Printf(
			TEXT("Param '%s' must be a JSON object of {UPROPERTY name: value}, e.g. "
				 "{\"Mobility\": \"Movable\", \"CastShadow\": false}."), Key);
		return false;
	}

	// An empty bag is a no-op, not an error — a generated document may legitimately
	// emit `"properties": {}` for an entry that ended up with no overrides.
	if ((*Obj)->Values.Num() > 0)
	{
		OutTree = *Obj;
	}
	return true;
}

bool FMonolithMeshSceneActions::InspectPropertyTree(
	const TSharedPtr<FJsonObject>& Tree,
	UStruct* TargetStruct,
	const void* Container,
	const FString& WhatLabel,
	FString& OutError)
{
	OutError.Reset();
	if (!Tree.IsValid() || Tree->Values.Num() == 0 || !TargetStruct || !Container)
	{
		return true;
	}

	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("mesh");
	Spec.Tree = Tree;
	Spec.bStrict = true;

	const FDryRunReport DryRun = FMonolithReflectionWalker::InspectTree(Tree, TargetStruct, Container, Spec);
	if (DryRun.Errors == 0)
	{
		return true;
	}

	TArray<FString> Problems;
	for (const FBulkFillFieldWrite& W : DryRun.FieldWrites)
	{
		if (W.bOk)
		{
			continue;
		}
		FString Line = FString::Printf(TEXT("'%s': %s"), *W.Path, *W.Reason);
		const TArray<FString> Hints = FMonolithMeshLightActions::SuggestPropertyNames(TargetStruct, W.Path);
		if (W.Reason.Contains(TEXT("unknown field")) && Hints.Num() > 0)
		{
			Line += FString::Printf(TEXT(" (did you mean: %s?)"), *FString::Join(Hints, TEXT(", ")));
		}
		Problems.Add(Line);
	}

	OutError = FString::Printf(
		TEXT("%d %s property write(s) rejected on %s — NOTHING was applied and no actor was left behind. %s. "
			 "Use mesh.get_actor_properties to list the exact UPROPERTY names this %s supports."),
		Problems.Num(), *WhatLabel, *TargetStruct->GetName(),
		*FString::Join(Problems, TEXT("; ")), *WhatLabel);
	return false;
}

bool FMonolithMeshSceneActions::ApplyPropertyTree(
	UObject* Target,
	const TSharedPtr<FJsonObject>& Tree,
	const FString& WhatLabel,
	TArray<FString>& OutApplied,
	FString& OutError)
{
	OutApplied.Reset();
	OutError.Reset();

	if (!Tree.IsValid() || Tree->Values.Num() == 0)
	{
		return true; // nothing asked for — not an error
	}
	if (!Target)
	{
		OutError = FString::Printf(TEXT("No %s to write %d property/properties onto."),
			*WhatLabel, Tree->Values.Num());
		return false;
	}

	UClass* TargetClass = Target->GetClass();

	// --- Validate BEFORE mutating. ---
	if (!InspectPropertyTree(Tree, TargetClass, Target, WhatLabel, OutError))
	{
		return false;
	}

	// --- Commit inside the edit cradle. ---
	Target->Modify();

	TArray<FProperty*> Touched;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Tree->Values)
	{
		if (FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(TargetClass, Pair.Key))
		{
			Touched.Add(Prop);
#if WITH_EDITOR
			Target->PreEditChange(Prop);
#endif
		}
	}

	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("mesh");
	Spec.Tree = Tree;
	Spec.bStrict = true;

	const FDryRunReport Report = FMonolithReflectionWalker::WriteTree(Tree, TargetClass, Target, Target, Spec);

#if WITH_EDITOR
	for (FProperty* Prop : Touched)
	{
		FPropertyChangedEvent Event(Prop);
		Target->PostEditChangeProperty(Event);
	}
#endif

	if (UActorComponent* AsComponent = Cast<UActorComponent>(Target))
	{
		AsComponent->MarkRenderStateDirty();
		if (AActor* Owner = AsComponent->GetOwner())
		{
			Owner->MarkPackageDirty();
		}
	}
	else
	{
		Target->MarkPackageDirty();
	}

	if (Report.Errors > 0)
	{
		// The dry run passed, so the live write disagreed with the scratch write.
		// Surface it rather than reporting a success the caller cannot trust.
		TArray<FString> Problems;
		for (const FBulkFillFieldWrite& W : Report.FieldWrites)
		{
			if (!W.bOk)
			{
				Problems.Add(FString::Printf(TEXT("'%s': %s"), *W.Path, *W.Reason));
			}
		}
		OutError = FString::Printf(
			TEXT("%d %s property write(s) failed after validation passed on %s: %s"),
			Problems.Num(), *WhatLabel, *TargetClass->GetName(), *FString::Join(Problems, TEXT("; ")));
		return false;
	}

	for (const FBulkFillFieldWrite& W : Report.FieldWrites)
	{
		if (W.bOk && !W.Path.Contains(TEXT(".")) && !W.Path.Contains(TEXT("[")))
		{
			OutApplied.AddUnique(W.Path);
		}
	}
	return true;
}

bool FMonolithMeshSceneActions::ValidateSpawnProperties(
	const TSharedPtr<FJsonObject>& Params, UClass* ActorClass, FString& OutError)
{
	OutError.Reset();

	TSharedPtr<FJsonObject> ActorTree;
	TSharedPtr<FJsonObject> ComponentTree;
	if (!ReadPropertyBag(Params, TEXT("properties"), ActorTree, OutError)) { return false; }
	if (!ReadPropertyBag(Params, TEXT("component_properties"), ComponentTree, OutError)) { return false; }

	if (!ActorTree.IsValid() && !ComponentTree.IsValid())
	{
		return true;
	}
	if (!ActorClass)
	{
		// No class resolved yet — the caller reports that failure on its own.
		return true;
	}

	AActor* CDO = Cast<AActor>(ActorClass->GetDefaultObject());
	if (!CDO)
	{
		return true;
	}

	if (!InspectPropertyTree(ActorTree, ActorClass, CDO, TEXT("actor"), OutError))
	{
		return false;
	}

	if (ComponentTree.IsValid())
	{
		// Native classes build their root in the constructor, so the CDO has one and
		// the whole bag is checkable before anything spawns. A Blueprint class whose
		// root comes from the SimpleConstructionScript does not: that case falls
		// through to the post-spawn write, which rolls the spawn back on rejection.
		if (USceneComponent* Root = CDO->GetRootComponent())
		{
			if (!InspectPropertyTree(ComponentTree, Root->GetClass(), Root, TEXT("root component"), OutError))
			{
				return false;
			}
		}
	}

	return true;
}

bool FMonolithMeshSceneActions::ApplySpawnProperties(
	AActor* Actor,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& OutActorApplied,
	TArray<FString>& OutComponentApplied,
	FString& OutError)
{
	OutActorApplied.Reset();
	OutComponentApplied.Reset();

	TSharedPtr<FJsonObject> ActorTree;
	TSharedPtr<FJsonObject> ComponentTree;
	if (!ReadPropertyBag(Params, TEXT("properties"), ActorTree, OutError)) { return false; }
	if (!ReadPropertyBag(Params, TEXT("component_properties"), ComponentTree, OutError)) { return false; }

	if (!ApplyPropertyTree(Actor, ActorTree, TEXT("actor"), OutActorApplied, OutError))
	{
		return false;
	}

	if (ComponentTree.IsValid())
	{
		USceneComponent* Root = Actor ? Actor->GetRootComponent() : nullptr;
		if (!Root)
		{
			OutError = FString::Printf(
				TEXT("component_properties was given but '%s' has no root component to write them onto. "
					 "Use `properties` for UPROPERTYs that live on the actor itself."),
				Actor ? *Actor->GetClass()->GetName() : TEXT("<null>"));
			return false;
		}
		if (!ApplyPropertyTree(Root, ComponentTree, TEXT("root component"), OutComponentApplied, OutError))
		{
			return false;
		}
	}

	return true;
}

// ============================================================================
// 2. spawn_actor
// ============================================================================

bool FMonolithMeshSceneActions::ResolveSpawnTarget(
	const FString& ClassOrMesh, UClass*& OutClass, UStaticMesh*& OutMesh, FString& OutError)
{
	OutClass = nullptr;
	OutMesh = nullptr;
	OutError.Reset();

	const FString Token = ClassOrMesh.TrimStartAndEnd();
	if (Token.IsEmpty())
	{
		OutError = TEXT("Empty class_or_mesh — expected an actor class name (e.g. 'PointLight'), a "
						"StaticMesh asset path (e.g. '/Engine/BasicShapes/Cube.Cube') or a Blueprint "
						"actor class path (e.g. '/Game/Props/BP_Barrel').");
		return false;
	}

	// ------------------------------------------------------------------
	// An OBJECT PATH: a StaticMesh asset, or a class.
	//
	// One generic load answers both spellings of a Blueprint, because that is what
	// the two paths actually name: the object at '/Game/Props/BP_Barrel' is the
	// UBlueprint (so its GeneratedClass is what "spawn this" means), while the
	// object at '/Game/Props/BP_Barrel.BP_Barrel_C' IS the generated class. Native
	// class paths ('/Script/Engine.PointLight') resolve through the same load.
	//
	// Naming a Blueprint by PATH rather than by short name is the whole point: a
	// short name only resolves while that Blueprint happens to already be loaded,
	// which is why mesh.capture_level_layout used to refuse Blueprint actors
	// outright. A path always resolves, so capture can express them.
	// ------------------------------------------------------------------
	if (Token.StartsWith(TEXT("/")))
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Token);
		if (!Asset)
		{
			OutError = FString::Printf(
				TEXT("Nothing found at asset path '%s'. Give a StaticMesh asset path (e.g. "
					 "'/Engine/BasicShapes/Cube.Cube'), a Blueprint actor class path ('/Game/Props/BP_Barrel' "
					 "or '/Game/Props/BP_Barrel.BP_Barrel_C'), or a native actor class name (e.g. 'PointLight')."),
				*Token);
			return false;
		}

		if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
		{
			OutMesh = Mesh;
			return true;
		}

		UClass* Class = Cast<UClass>(Asset);
		if (!Class)
		{
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
			{
				Class = Blueprint->GeneratedClass;
				if (!Class)
				{
					OutError = FString::Printf(
						TEXT("Blueprint '%s' has no generated class, so there is nothing to spawn — it has "
							 "never compiled successfully. Open and compile it, then try again."), *Token);
					return false;
				}
			}
		}
		if (!Class)
		{
			OutError = FString::Printf(
				TEXT("Asset '%s' is a %s. mesh.spawn_actor takes a StaticMesh asset path, a Blueprint actor "
					 "class path, or a native actor class name."), *Token, *Asset->GetClass()->GetName());
			return false;
		}

		if (!SceneActionHelpers::ValidateSpawnableActorClass(Class, Token, OutError))
		{
			return false;
		}
		OutClass = Class;
		return true;
	}

	// ------------------------------------------------------------------
	// A class NAME. Tried verbatim, then with the 'A' prefix added/removed.
	// ------------------------------------------------------------------

	// Block ABlockingVolume
	if (Token.Equals(TEXT("BlockingVolume"), ESearchCase::IgnoreCase) ||
		Token.Equals(TEXT("ABlockingVolume"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Spawning BlockingVolume is not allowed via Monolith. Use the editor directly.");
		return false;
	}

	UClass* Found = FindFirstObject<UClass>(*Token, EFindFirstObjectOptions::NativeFirst);
	if (!Found)
	{
		const FString AltName = Token.StartsWith(TEXT("A")) ? Token.Mid(1) : (TEXT("A") + Token);
		Found = FindFirstObject<UClass>(*AltName, EFindFirstObjectOptions::NativeFirst);
	}
	if (!Found)
	{
		const TArray<FString> Hints = SceneActionHelpers::SuggestActorClassNames(Token);
		OutError = FString::Printf(
			TEXT("Class not found: %s.%s A Blueprint class must be named by its asset path "
				 "('/Game/Props/BP_Barrel'), not by its short name — a short name only resolves while that "
				 "Blueprint happens to be loaded."),
			*Token,
			Hints.Num() > 0
				? *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Hints, TEXT(", ")))
				: TEXT(""));
		return false;
	}

	if (!SceneActionHelpers::ValidateSpawnableActorClass(Found, Token, OutError))
	{
		return false;
	}
	OutClass = Found;
	return true;
}

FMonolithActionResult FMonolithMeshSceneActions::SpawnActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassOrMesh;
	if (!Params->TryGetStringField(TEXT("class_or_mesh"), ClassOrMesh))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: class_or_mesh"));
	}

	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location (array of 3 numbers)"));
	}

	FRotator Rotation(0, 0, 0);
	MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);

	FVector Scale(1, 1, 1);
	MonolithMeshUtils::ParseVector(Params, TEXT("scale"), Scale);

	FString OptionalName;
	Params->TryGetStringField(TEXT("name"), OptionalName);

	FString Folder;
	Params->TryGetStringField(TEXT("folder"), Folder);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Validate inputs before opening a transaction
	UStaticMesh* MeshToSpawn = nullptr;
	UClass* ClassToSpawn = nullptr;
	{
		FString ResolveError;
		if (!ResolveSpawnTarget(ClassOrMesh, ClassToSpawn, MeshToSpawn, ResolveError))
		{
			return FMonolithActionResult::Error(ResolveError);
		}
	}

	// The property bags are checked against the class default object here — BEFORE
	// the transaction opens and before anything is spawned — so the common failure
	// (a typo) costs exactly nothing and mutates nothing.
	{
		UClass* EffectiveClass = MeshToSpawn ? AStaticMeshActor::StaticClass() : ClassToSpawn;
		FString PropertyError;
		if (!ValidateSpawnProperties(Params, EffectiveClass, PropertyError))
		{
			return FMonolithActionResult::Error(PropertyError);
		}
	}

	// All validation passed — open transaction and spawn
	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Spawn Actor")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* SpawnedActor = nullptr;
	FString SpawnedClassName;

	if (MeshToSpawn)
	{
		AStaticMeshActor* SMActor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
		if (!SMActor)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(TEXT("Failed to spawn StaticMeshActor"));
		}
		SMActor->GetStaticMeshComponent()->SetStaticMesh(MeshToSpawn);
		SMActor->SetActorScale3D(Scale);
		SpawnedActor = SMActor;
		SpawnedClassName = TEXT("StaticMeshActor");
	}
	else
	{
		SpawnedActor = World->SpawnActor(ClassToSpawn, &Location, &Rotation, SpawnParams);
		if (!SpawnedActor)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to spawn actor of class '%s'"), *ClassOrMesh));
		}
		SpawnedActor->SetActorScale3D(Scale);
		SpawnedClassName = ClassToSpawn->GetName();
	}

	// Free-form property channel. A rejection here rolls the spawn BACK: destroy
	// first, then cancel, so the undo buffer never keeps a reference to a
	// half-configured actor regardless of which side actually rolls it back. Under
	// mesh.batch_execute / mesh.apply_level_layout the transaction is owned by the
	// caller, so Cancel() is a deliberate no-op there and only the destroy runs —
	// that is what keeps the layout's single outer transaction balanced.
	TArray<FString> ActorPropsApplied;
	TArray<FString> ComponentPropsApplied;
	{
		FString PropertyError;
		if (!ApplySpawnProperties(SpawnedActor, Params, ActorPropsApplied, ComponentPropsApplied, PropertyError))
		{
			World->DestroyActor(SpawnedActor);
			Transaction.Cancel();
			return FMonolithActionResult::Error(PropertyError);
		}
	}

	// Set optional label
	if (!OptionalName.IsEmpty())
	{
		SpawnedActor->SetActorLabel(OptionalName);
	}

	// Set folder — default to /Spawned if none specified
	if (!Folder.IsEmpty())
	{
		SpawnedActor->SetFolderPath(FName(*Folder));
	}
	else
	{
		SpawnedActor->SetFolderPath(FName(TEXT("Spawned")));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), SpawnedActor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("class"), SpawnedClassName);
	Result->SetArrayField(TEXT("location"), SceneActionHelpers::VectorToJsonArray(SpawnedActor->GetActorLocation()));

	auto ToStringArray = [](const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& S : In) { Arr.Add(MakeShared<FJsonValueString>(S)); }
		return Arr;
	};
	Result->SetArrayField(TEXT("properties_set"), ToStringArray(ActorPropsApplied));
	Result->SetArrayField(TEXT("component_properties_set"), ToStringArray(ComponentPropsApplied));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. move_actor
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::MoveActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bRelative = false;
	Params->TryGetBoolField(TEXT("relative"), bRelative);

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Move Actor")));

	FVector Location;
	if (MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		if (bRelative)
		{
			Actor->SetActorLocation(Actor->GetActorLocation() + Location);
		}
		else
		{
			Actor->SetActorLocation(Location);
		}
	}

	FRotator Rotation;
	if (MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation))
	{
		if (bRelative)
		{
			Actor->SetActorRotation(Actor->GetActorRotation() + Rotation);
		}
		else
		{
			Actor->SetActorRotation(Rotation);
		}
	}

	FVector Scale;
	if (MonolithMeshUtils::ParseVector(Params, TEXT("scale"), Scale))
	{
		if (bRelative)
		{
			Actor->SetActorScale3D(Actor->GetActorScale3D() + Scale);
		}
		else
		{
			Actor->SetActorScale3D(Scale);
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetObjectField(TEXT("new_transform"), MonolithMeshUtils::TransformToJson(Actor->GetActorTransform()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. duplicate_actor
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::DuplicateActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* SourceActor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!SourceActor)
	{
		return FMonolithActionResult::Error(Error);
	}

	FVector Offset(0, 0, 0);
	MonolithMeshUtils::ParseVector(Params, TEXT("offset"), Offset);

	FString NewName;
	Params->TryGetStringField(TEXT("new_name"), NewName);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Duplicate Actor")));

	// Select the source actor and duplicate via editor
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(SourceActor, true, false, true);

	// Use editor duplication
	GEditor->edactDuplicateSelected(World->GetCurrentLevel(), false);

	// The duplicated actor is now the only selected actor
	AActor* DupActor = nullptr;
	USelection* SelectedActors = GEditor->GetSelectedActors();
	for (int32 i = 0; i < SelectedActors->Num(); ++i)
	{
		AActor* Selected = Cast<AActor>(SelectedActors->GetSelectedObject(i));
		if (Selected && Selected != SourceActor)
		{
			DupActor = Selected;
			break;
		}
	}

	if (!DupActor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Failed to duplicate actor"));
	}

	// Apply offset
	DupActor->SetActorLocation(DupActor->GetActorLocation() + Offset);

	// Set label if requested
	if (!NewName.IsEmpty())
	{
		DupActor->SetActorLabel(NewName);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("original"), SourceActor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("duplicate"), DupActor->GetActorNameOrLabel());
	Result->SetArrayField(TEXT("location"), SceneActionHelpers::VectorToJsonArray(DupActor->GetActorLocation()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. delete_actors
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::DeleteActors(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), NamesArr) || NamesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names (array of strings)"));
	}

	// Phase 1: resolve ALL actors first, fail if any are missing
	TArray<AActor*> Actors;
	Actors.Reserve(NamesArr->Num());

	for (const auto& Val : *NamesArr)
	{
		FString Name = Val->AsString();
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor not found: %s. No actors deleted."), *Name));
		}
		Actors.Add(Actor);
	}

	// Phase 2: delete all via editor selection (undo-compatible)
	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Delete Actors")));

	TArray<TSharedPtr<FJsonValue>> DeletedNames;

	// Collect names before deletion
	for (AActor* Actor : Actors)
	{
		DeletedNames.Add(MakeShared<FJsonValueString>(Actor->GetActorNameOrLabel()));
	}

	// Select all targets and delete via editor (participates in undo)
	GEditor->SelectNone(false, true, false);
	for (AActor* Actor : Actors)
	{
		GEditor->SelectActor(Actor, true, false, true);
	}
	// bVerifyDeletionCanHappen=false to suppress editor confirmation dialogs
	// bWarnAboutReferences=false, bWarnAboutSoftReferences=false for programmatic use
	GEditor->edactDeleteSelected(MonolithMeshUtils::GetEditorWorld(), false, false, false);

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("deleted"), DeletedNames.Num());
	Result->SetArrayField(TEXT("actors"), DeletedNames);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. group_actors
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::GroupActors(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), NamesArr) || NamesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names"));
	}

	FString GroupName;
	if (!Params->TryGetStringField(TEXT("group_name"), GroupName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: group_name"));
	}

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Group Actors")));

	int32 Count = 0;
	FString FirstError;
	for (const auto& Val : *NamesArr)
	{
		FString Name = Val->AsString();
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
		if (!Actor)
		{
			if (FirstError.IsEmpty())
			{
				FirstError = Error;
			}
			continue;
		}
		Actor->SetFolderPath(FName(*GroupName));
		Count++;
	}

	if (Count == 0 && !FirstError.IsEmpty())
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FirstError);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("group"), GroupName);
	Result->SetNumberField(TEXT("actors"), Count);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. set_actor_properties
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::SetActorProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	USceneComponent* Root = Actor->GetRootComponent();
	UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Root);

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Set Actor Properties")));

	TArray<TSharedPtr<FJsonValue>> PropertiesSet;

	// Mobility
	FString MobilityStr;
	if (Params->TryGetStringField(TEXT("mobility"), MobilityStr))
	{
		if (!Root)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(TEXT("Actor has no root component to set mobility on"));
		}
		EComponentMobility::Type Mob;
		if (!SceneActionHelpers::ParseMobility(MobilityStr, Mob))
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mobility: '%s'. Use Static, Stationary, or Movable."), *MobilityStr));
		}
		Root->SetMobility(Mob);
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("mobility")));
	}

	// Simulate physics (must check mobility is Movable)
	bool bSimPhysics;
	if (Params->TryGetBoolField(TEXT("simulate_physics"), bSimPhysics))
	{
		if (!Primitive)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(TEXT("Actor's root component is not a PrimitiveComponent — cannot set physics"));
		}
		if (bSimPhysics && Root->Mobility != EComponentMobility::Movable)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(TEXT("Mobility must be Movable before enabling SimulatePhysics. Set mobility first."));
		}
		Primitive->SetSimulatePhysics(bSimPhysics);
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("simulate_physics")));
	}

	// Collision preset
	FString CollisionPreset;
	if (Params->TryGetStringField(TEXT("collision_preset"), CollisionPreset))
	{
		if (!Primitive)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(TEXT("Actor's root component is not a PrimitiveComponent — cannot set collision"));
		}
		Primitive->SetCollisionProfileName(FName(*CollisionPreset));
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("collision_preset")));
	}

	// Cast shadow
	bool bCastShadow;
	if (Params->TryGetBoolField(TEXT("cast_shadow"), bCastShadow))
	{
		if (!Primitive)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(TEXT("Actor's root component is not a PrimitiveComponent — cannot set shadow"));
		}
		Primitive->SetCastShadow(bCastShadow);
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("cast_shadow")));
	}

	// Tags
	const TArray<TSharedPtr<FJsonValue>>* TagsArr;
	if (Params->TryGetArrayField(TEXT("tags"), TagsArr))
	{
		Actor->Tags.Empty();
		for (const auto& TagVal : *TagsArr)
		{
			Actor->Tags.Add(FName(*TagVal->AsString()));
		}
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("tags")));
	}

	// Mass override
	double MassKg;
	if (Params->TryGetNumberField(TEXT("mass_kg"), MassKg))
	{
		if (!Primitive)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(TEXT("Actor's root component is not a PrimitiveComponent — cannot set mass"));
		}
		Primitive->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), true);
		PropertiesSet.Add(MakeShared<FJsonValueString>(TEXT("mass_kg")));
	}

	if (PropertiesSet.Num() == 0)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("No properties provided to set. Specify at least one of: mobility, simulate_physics, collision_preset, cast_shadow, tags, mass_kg"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetArrayField(TEXT("properties_set"), PropertiesSet);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. batch_execute
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::BatchExecute(const TSharedPtr<FJsonObject>& Params)
{
	// Reject nested batch_execute
	if (bBatchTransactionActive)
	{
		return FMonolithActionResult::Error(TEXT("Nested batch_execute is not allowed"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ActionsArr;
	if (!Params->TryGetArrayField(TEXT("actions"), ActionsArr) || ActionsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actions"));
	}

	if (ActionsArr->Num() > 200)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("batch_execute is capped at 200 actions, got %d"), ActionsArr->Num()));
	}

	// Begin single outer transaction
	if (GEditor)
	{
		GEditor->BeginTransaction(FText::FromString(TEXT("Monolith: Batch Execute")));
	}
	bBatchTransactionActive = true;

	int32 Succeeded = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	bool bHadFailure = false;

	for (int32 i = 0; i < ActionsArr->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* ActionObj;
		if (!(*ActionsArr)[i]->TryGetObject(ActionObj))
		{
			auto ItemResult = MakeShared<FJsonObject>();
			ItemResult->SetStringField(TEXT("action"), TEXT("unknown"));
			ItemResult->SetBoolField(TEXT("success"), false);
			ItemResult->SetStringField(TEXT("error"), TEXT("Invalid action entry — expected object with 'action' and 'params' fields"));
			ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			bHadFailure = true;
			break;
		}

		FString ActionName;
		if (!(*ActionObj)->TryGetStringField(TEXT("action"), ActionName))
		{
			auto ItemResult = MakeShared<FJsonObject>();
			ItemResult->SetStringField(TEXT("action"), TEXT("unknown"));
			ItemResult->SetBoolField(TEXT("success"), false);
			ItemResult->SetStringField(TEXT("error"), TEXT("Missing 'action' field in batch item"));
			ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			bHadFailure = true;
			break;
		}

		// Reject nested batch_execute
		if (ActionName == TEXT("batch_execute"))
		{
			auto ItemResult = MakeShared<FJsonObject>();
			ItemResult->SetStringField(TEXT("action"), ActionName);
			ItemResult->SetBoolField(TEXT("success"), false);
			ItemResult->SetStringField(TEXT("error"), TEXT("Nested batch_execute is not allowed"));
			ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			bHadFailure = true;
			break;
		}

		// Get params (optional, default to empty object)
		TSharedPtr<FJsonObject> ActionParams = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject>* ParamsObj;
		if ((*ActionObj)->TryGetObjectField(TEXT("params"), ParamsObj))
		{
			ActionParams = *ParamsObj;
		}

		// Dispatch via registry
		FMonolithActionResult ActionResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), ActionName, ActionParams);

		auto ItemResult = MakeShared<FJsonObject>();
		ItemResult->SetStringField(TEXT("action"), ActionName);
		ItemResult->SetBoolField(TEXT("success"), ActionResult.bSuccess);
		if (!ActionResult.bSuccess)
		{
			ItemResult->SetStringField(TEXT("error"), ActionResult.ErrorMessage);
			ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			bHadFailure = true;
			break; // Cancel on first failure
		}
		Succeeded++;
		ResultsArr.Add(MakeShared<FJsonValueObject>(ItemResult));
	}

	// End or cancel transaction
	bBatchTransactionActive = false;
	if (GEditor)
	{
		if (bHadFailure)
		{
			GEditor->CancelTransaction(0);
		}
		else
		{
			GEditor->EndTransaction();
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), ActionsArr->Num());
	Result->SetNumberField(TEXT("succeeded"), Succeeded);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetArrayField(TEXT("results"), ResultsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. align_actors
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::AlignActors(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), NamesArr) || NamesArr->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Missing or insufficient actor_names (need at least 2 actors to align)"));
	}

	FString AxisStr;
	if (!Params->TryGetStringField(TEXT("axis"), AxisStr))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: axis (X, Y, or Z)"));
	}
	AxisStr = AxisStr.ToUpper();
	int32 AxisIndex = -1;
	if (AxisStr == TEXT("X")) AxisIndex = 0;
	else if (AxisStr == TEXT("Y")) AxisIndex = 1;
	else if (AxisStr == TEXT("Z")) AxisIndex = 2;
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid axis: '%s'. Use X, Y, or Z."), *AxisStr));
	}

	FString ModeStr;
	if (!Params->TryGetStringField(TEXT("mode"), ModeStr))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: mode (min, max, center, or distribute)"));
	}
	ModeStr = ModeStr.ToLower();
	if (ModeStr != TEXT("min") && ModeStr != TEXT("max") && ModeStr != TEXT("center") && ModeStr != TEXT("distribute"))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mode: '%s'. Use min, max, center, or distribute."), *ModeStr));
	}

	// Resolve all actors first — fail if any missing
	TArray<AActor*> Actors;
	Actors.Reserve(NamesArr->Num());
	for (const auto& Val : *NamesArr)
	{
		FString Name = Val->AsString();
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor not found: %s"), *Name));
		}
		Actors.Add(Actor);
	}

	// Lambdas for axis access
	auto GetAxisValue = [AxisIndex](const FVector& V) -> double
	{
		return (AxisIndex == 0) ? V.X : (AxisIndex == 1) ? V.Y : V.Z;
	};

	auto SetAxisValue = [AxisIndex](FVector& V, double Val)
	{
		if (AxisIndex == 0) V.X = Val;
		else if (AxisIndex == 1) V.Y = Val;
		else V.Z = Val;
	};

	// Compute stats along the target axis
	double MinVal = TNumericLimits<double>::Max();
	double MaxVal = TNumericLimits<double>::Lowest();
	double Sum = 0.0;
	for (AActor* Actor : Actors)
	{
		double Val = GetAxisValue(Actor->GetActorLocation());
		MinVal = FMath::Min(MinVal, Val);
		MaxVal = FMath::Max(MaxVal, Val);
		Sum += Val;
	}

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Align Actors")));

	TArray<TSharedPtr<FJsonValue>> ResultArr;

	if (ModeStr == TEXT("distribute"))
	{
		// Sort actors by current position on the target axis, then spread evenly
		struct FActorSortEntry
		{
			AActor* Actor;
			double AxisVal;
		};
		TArray<FActorSortEntry> Sorted;
		Sorted.Reserve(Actors.Num());
		for (AActor* Actor : Actors)
		{
			Sorted.Add({ Actor, GetAxisValue(Actor->GetActorLocation()) });
		}
		Sorted.Sort([](const FActorSortEntry& A, const FActorSortEntry& B) { return A.AxisVal < B.AxisVal; });

		double Range = MaxVal - MinVal;
		int32 Count = Sorted.Num();

		for (int32 i = 0; i < Count; ++i)
		{
			AActor* Actor = Sorted[i].Actor;
			double NewVal = (Count > 1) ? (MinVal + (Range * i) / (Count - 1)) : MinVal;
			FVector Loc = Actor->GetActorLocation();
			SetAxisValue(Loc, NewVal);
			Actor->SetActorLocation(Loc);

			auto Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());
			Entry->SetArrayField(TEXT("new_location"), SceneActionHelpers::VectorToJsonArray(Loc));
			ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	else
	{
		double Target = 0.0;
		if (ModeStr == TEXT("min")) Target = MinVal;
		else if (ModeStr == TEXT("max")) Target = MaxVal;
		else /* center */ Target = Sum / Actors.Num();

		for (AActor* Actor : Actors)
		{
			FVector Loc = Actor->GetActorLocation();
			SetAxisValue(Loc, Target);
			Actor->SetActorLocation(Loc);

			auto Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());
			Entry->SetArrayField(TEXT("new_location"), SceneActionHelpers::VectorToJsonArray(Loc));
			ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("axis"), AxisStr);
	Result->SetStringField(TEXT("mode"), ModeStr);
	Result->SetNumberField(TEXT("aligned"), ResultArr.Num());
	Result->SetArrayField(TEXT("actors"), ResultArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 10. snap_to_floor
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::SnapToFloor(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* NamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), NamesArr) || NamesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_names"));
	}

	double TraceDistance = 10000.0;
	Params->TryGetNumberField(TEXT("trace_distance"), TraceDistance);
	if (TraceDistance <= 0.0)
	{
		return FMonolithActionResult::Error(TEXT("trace_distance must be positive"));
	}

	// Resolve all actors first — fail if any missing
	TArray<AActor*> Actors;
	Actors.Reserve(NamesArr->Num());
	for (const auto& Val : *NamesArr)
	{
		FString Name = Val->AsString();
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor not found: %s"), *Name));
		}
		Actors.Add(Actor);
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Snap to Floor")));

	TArray<TSharedPtr<FJsonValue>> ResultArr;
	int32 Snapped = 0;
	int32 Missed = 0;

	for (AActor* Actor : Actors)
	{
		// Get actor bounds to compute half-height (distance from center to bottom)
		FVector BoundsOrigin, BoundsExtent;
		Actor->GetActorBounds(false, BoundsOrigin, BoundsExtent);
		double ActorHalfHeight = BoundsExtent.Z; // Extent is half-size per axis

		FVector ActorLoc = Actor->GetActorLocation();

		// Trace downward from actor location
		FVector TraceStart = ActorLoc;
		FVector TraceEnd = TraceStart - FVector(0, 0, TraceDistance);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SnapToFloor), true);
		QueryParams.AddIgnoredActor(Actor);

		FHitResult Hit;
		bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams);

		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());

		if (bHit)
		{
			// Place actor so its bottom sits on the hit point.
			// Account for pivot offset: the actor's pivot may not be at bounds center.
			// PivotOffset = ActorLocation.Z - BoundsOrigin.Z
			// NewZ = HitPoint.Z + ActorHalfHeight + PivotOffset
			double PivotOffset = ActorLoc.Z - BoundsOrigin.Z;
			double NewZ = Hit.ImpactPoint.Z + ActorHalfHeight + PivotOffset;
			FVector NewLoc = ActorLoc;
			NewLoc.Z = NewZ;
			Actor->SetActorLocation(NewLoc);

			Entry->SetArrayField(TEXT("new_location"), SceneActionHelpers::VectorToJsonArray(NewLoc));
			Entry->SetNumberField(TEXT("floor_z"), Hit.ImpactPoint.Z);
			Entry->SetBoolField(TEXT("snapped"), true);
			Snapped++;
		}
		else
		{
			Entry->SetBoolField(TEXT("snapped"), false);
			Entry->SetStringField(TEXT("reason"), TEXT("No floor found within trace distance"));
			Missed++;
		}

		ResultArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("snapped"), Snapped);
	Result->SetNumberField(TEXT("missed"), Missed);
	Result->SetNumberField(TEXT("trace_distance"), TraceDistance);
	Result->SetArrayField(TEXT("actors"), ResultArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// manage_folders
// ============================================================================

FMonolithActionResult FMonolithMeshSceneActions::ManageFolders(const TSharedPtr<FJsonObject>& Params)
{
	FString SubAction;
	if (!Params->TryGetStringField(TEXT("sub_action"), SubAction))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: sub_action (list, delete, rename, move)"));
	}
	SubAction = SubAction.ToLower();

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// ---- LIST: enumerate all folder paths in use ----
	if (SubAction == TEXT("list"))
	{
		TMap<FString, int32> FolderCounts;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			FName FolderPath = It->GetFolderPath();
			if (!FolderPath.IsNone())
			{
				FolderCounts.FindOrAdd(FolderPath.ToString())++;
			}
		}

		TArray<TSharedPtr<FJsonValue>> FolderArr;
		FolderCounts.KeySort([](const FString& A, const FString& B) { return A < B; });
		for (const auto& Pair : FolderCounts)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("folder"), Pair.Key);
			Obj->SetNumberField(TEXT("actor_count"), Pair.Value);
			FolderArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("folders"), FolderArr);
		Result->SetNumberField(TEXT("total_folders"), FolderArr.Num());
		return FMonolithActionResult::Success(Result);
	}

	// ---- Remaining sub-actions need a folder param ----
	FString Folder;
	if (!Params->TryGetStringField(TEXT("folder"), Folder) || Folder.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: folder (for delete/rename/move)"));
	}

	bool bIncludeSubfolders = true;
	Params->TryGetBoolField(TEXT("include_subfolders"), bIncludeSubfolders);

	// Collect actors in the target folder
	TArray<AActor*> FolderActors;
	FName FolderFName(*Folder);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		FName ActorFolder = It->GetFolderPath();
		if (ActorFolder == FolderFName)
		{
			FolderActors.Add(*It);
		}
		else if (bIncludeSubfolders && !ActorFolder.IsNone())
		{
			FString ActorFolderStr = ActorFolder.ToString();
			if (ActorFolderStr.StartsWith(Folder + TEXT("/")))
			{
				FolderActors.Add(*It);
			}
		}
	}

	// ---- DELETE: move all actors to root (folder vanishes when empty) ----
	if (SubAction == TEXT("delete"))
	{
		SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Delete Folder")));

		for (AActor* Actor : FolderActors)
		{
			Actor->SetFolderPath(NAME_None);
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("deleted_folder"), Folder);
		Result->SetNumberField(TEXT("actors_moved_to_root"), FolderActors.Num());
		return FMonolithActionResult::Success(Result);
	}

	// ---- RENAME: change folder path for all actors ----
	if (SubAction == TEXT("rename"))
	{
		FString NewFolder;
		if (!Params->TryGetStringField(TEXT("new_folder"), NewFolder) || NewFolder.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: new_folder (for rename)"));
		}

		SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Rename Folder")));

		int32 Renamed = 0;
		for (AActor* Actor : FolderActors)
		{
			FString ActorFolderStr = Actor->GetFolderPath().ToString();
			if (ActorFolderStr == Folder)
			{
				Actor->SetFolderPath(FName(*NewFolder));
			}
			else if (bIncludeSubfolders)
			{
				// Preserve subfolder structure: "OldFolder/Sub" -> "NewFolder/Sub"
				FString Remainder = ActorFolderStr.Mid(Folder.Len());
				Actor->SetFolderPath(FName(*(NewFolder + Remainder)));
			}
			Renamed++;
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("old_folder"), Folder);
		Result->SetStringField(TEXT("new_folder"), NewFolder);
		Result->SetNumberField(TEXT("actors_renamed"), Renamed);
		return FMonolithActionResult::Success(Result);
	}

	// ---- MOVE: move all actors into a different folder ----
	if (SubAction == TEXT("move"))
	{
		FString NewFolder;
		if (!Params->TryGetStringField(TEXT("new_folder"), NewFolder) || NewFolder.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: new_folder (for move destination)"));
		}

		SceneActionHelpers::FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Move Folder")));

		for (AActor* Actor : FolderActors)
		{
			Actor->SetFolderPath(FName(*NewFolder));
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source_folder"), Folder);
		Result->SetStringField(TEXT("destination_folder"), NewFolder);
		Result->SetNumberField(TEXT("actors_moved"), FolderActors.Num());
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(FString::Printf(
		TEXT("Unknown sub_action: '%s'. Valid: list, delete, rename, move"), *SubAction));
}
