#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class AActor;
class UClass;
class UObject;
class UStaticMesh;
class UStruct;

/**
 * Phase 2: Scene Manipulation Actions (8 actions)
 * Actor CRUD operations - spawn, move, duplicate, delete, query info.
 * Foundation for blockout system.
 */
class FMonolithMeshSceneActions
{
public:
	/** Register all 8 scene manipulation actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	/** True when batch_execute is running — sub-actions skip their own undo transactions */
	static bool bBatchTransactionActive;

	/**
	 * Resolve a `spawn_actor` style `class_or_mesh` token WITHOUT touching the world.
	 *
	 * A token starting with '/' is an asset path and must load as a UStaticMesh
	 * (spawned as an AStaticMeshActor); anything else is an actor class name, tried
	 * verbatim then with the 'A' prefix added/removed. Blocking volumes are refused.
	 *
	 * Exactly one of OutClass / OutMesh is non-null on success. Returns false with an
	 * actionable OutError on a miss. Split out of SpawnActor so callers that need to
	 * VALIDATE a spawn before committing to it (mesh.apply_level_layout) get exactly
	 * the same answer the spawn itself would give.
	 */
	static bool ResolveSpawnTarget(const FString& ClassOrMesh, UClass*& OutClass, UStaticMesh*& OutMesh, FString& OutError);

	// ------------------------------------------------------------------
	// `spawn_actor` free-form property channel
	//
	// mesh.spawn_actor takes TWO property bags, because a spawned actor is two
	// objects a level designer cares about and they have disjoint UPROPERTY sets:
	//
	//   properties            -> the ACTOR      (AActor subclass: bEnableAutoLODGeneration,
	//                                            Tags, bNetLoadOnClient, ...)
	//   component_properties  -> its ROOT COMPONENT (USceneComponent subclass: Mobility,
	//                                            CastShadow, bReceivesDecals, ... — for a
	//                                            mesh path this is the StaticMeshComponent)
	//
	// Two named bags rather than one bag with a mode flag, because the split is
	// static per key and the schema then advertises both channels in tools/list.
	// This is the same addressing mesh.place_light uses (its `properties` bag targets
	// the light COMPONENT, which is the only object that action's caller can mean);
	// spawn_actor spawns arbitrary classes, so it has to say which of the two it means.
	//
	// Semantics match place_light / spawn_atmosphere exactly: strict validation
	// first (FMonolithReflectionWalker::InspectTree against a scratch buffer), an
	// unknown name is an error carrying did-you-mean candidates, and there are never
	// partial writes. A rejected tree ALSO destroys the actor that was spawned for it
	// (the rollback contract) — a half-configured actor the caller cannot identify is
	// worse than no actor, and mesh.apply_level_layout depends on it: the layout only
	// cleans up actors from entries that SUCCEEDED, so a failing entry has to clean up
	// after itself or an all-or-nothing apply would leak one actor.
	// ------------------------------------------------------------------

	/**
	 * Validate the `properties` / `component_properties` bags of a spawn_actor param
	 * object against a class WITHOUT touching the world — the same coercion the real
	 * write performs, run against the class default object.
	 *
	 * @param Params      spawn_actor style params (bags are optional; absent = success).
	 * @param ActorClass  The class that WOULD be spawned (AStaticMeshActor for a mesh path).
	 * @return false + actionable OutError naming every rejected key with did-you-mean.
	 *
	 * A Blueprint class whose root component comes from its SimpleConstructionScript
	 * has no root on the CDO; `component_properties` then cannot be pre-validated and
	 * is deferred to the write (which still rolls the spawn back on rejection).
	 */
	static bool ValidateSpawnProperties(const TSharedPtr<FJsonObject>& Params, UClass* ActorClass, FString& OutError);

	/**
	 * Write one JSON {UPROPERTY name: value} tree onto one object: validate the WHOLE
	 * tree, bail on any error, then Modify + PreEditChange per key, WriteTree,
	 * PostEditChangeProperty per key, render state + package dirty.
	 *
	 * @param WhatLabel Human name of the target used in error text ("actor", "root component").
	 * @return false + OutError on the first problem; nothing is written in that case.
	 */
	static bool ApplyPropertyTree(
		UObject* Target,
		const TSharedPtr<FJsonObject>& Tree,
		const FString& WhatLabel,
		TArray<FString>& OutApplied,
		FString& OutError);

private:
	/** Read an optional object-valued param. false + OutError when present but not an object. */
	static bool ReadPropertyBag(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Key,
		TSharedPtr<FJsonObject>& OutTree,
		FString& OutError);

	/** InspectTree + did-you-mean formatting, no mutation. */
	static bool InspectPropertyTree(
		const TSharedPtr<FJsonObject>& Tree,
		UStruct* TargetStruct,
		const void* Container,
		const FString& WhatLabel,
		FString& OutError);

	/** Apply both bags onto a freshly spawned actor. Used by SpawnActor only. */
	static bool ApplySpawnProperties(
		AActor* Actor,
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutActorApplied,
		TArray<FString>& OutComponentApplied,
		FString& OutError);

	static FMonolithActionResult GetActorInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SpawnActor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult MoveActor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DuplicateActor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GroupActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetActorProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchExecute(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AlignActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SnapToFloor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ManageFolders(const TSharedPtr<FJsonObject>& Params);
};
