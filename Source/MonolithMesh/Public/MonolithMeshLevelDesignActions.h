#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Phase 13: Level Design Quick Wins (9 actions)
 * Lights, materials, mesh swap, LOD, instancing, component property reflection.
 * High-frequency actions for level design sessions.
 */
class FMonolithMeshLevelDesignActions
{
public:
	/** Register all 9 level design actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	/** Spawn point/spot/rect/directional/sky light with full properties */
	static FMonolithActionResult PlaceLight(const TSharedPtr<FJsonObject>& Params);

	/** Modify properties on an existing light actor */
	static FMonolithActionResult SetLightProperties(const TSharedPtr<FJsonObject>& Params);

	/** Assign a material to an actor's mesh component by slot index or name */
	static FMonolithActionResult SetActorMaterial(const TSharedPtr<FJsonObject>& Params);

	/** Bulk replace material X with Y across actors or entire level */
	static FMonolithActionResult SwapMaterialInLevel(const TSharedPtr<FJsonObject>& Params);

	/** Swap all instances of static mesh X with mesh Y */
	static FMonolithActionResult FindReplaceMesh(const TSharedPtr<FJsonObject>& Params);

	/** Set per-LOD screen size thresholds on a static mesh asset */
	static FMonolithActionResult SetLodScreenSizes(const TSharedPtr<FJsonObject>& Params);

	/** Identify meshes used many times that could be HISM-converted */
	static FMonolithActionResult FindInstancingCandidates(const TSharedPtr<FJsonObject>& Params);

	/** Convert grouped StaticMeshActors into a single HISM actor */
	static FMonolithActionResult ConvertToHism(const TSharedPtr<FJsonObject>& Params);

	/** Read arbitrary component properties via FProperty reflection */
	static FMonolithActionResult GetActorComponentProperties(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---

	/**
	 * Apply the curated light params (intensity, color, cone angles, ...) from JSON to a
	 * light component. Returns the list of properties set.
	 *
	 * Takes ULightComponentBase, NOT ULightComponent — USkyLightComponent derives from the
	 * base only, so a ULightComponent parameter silently excluded every sky light. Params
	 * that only exist on ULightComponent / ULocalLightComponent stay cast-guarded inside.
	 *
	 * The open-ended `preset` / `properties` channel is handled separately by
	 * FMonolithMeshLightActions::ApplyPropertyTree (UE reflection, no per-property C++).
	 */
	static TArray<FString> ApplyLightProperties(class ULightComponentBase* LightComp, const TSharedPtr<FJsonObject>& Params);
};
