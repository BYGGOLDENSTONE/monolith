#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class UClass;
class UStaticMesh;

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

private:
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
