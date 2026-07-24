#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Phase 14: Level Design Core — Volumes, Properties, NavMesh, Selection, Snapping (7 actions)
 *
 * spawn_volume          — Spawn trigger/kill/pain/blocking/nav_modifier/audio/post_process volumes
 * get_actor_properties  — Read arbitrary UPROPERTY values via FProperty reflection
 * copy_actor_properties — Copy properties from source actor to targets
 * build_navmesh         — Trigger navigation mesh rebuild (synchronous)
 * select_actors         — Control editor selection + camera focus
 * snap_to_surface       — Directional trace with surface normal alignment
 * set_collision_preset  — Set collision profile on actor's root primitive component
 */
class FMonolithMeshVolumeActions
{
public:
	/** Register all 7 level design core actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	/**
	 * Resolve a volume `type` token to its UClass. Returns nullptr + an OutError that
	 * lists every valid token. Public because callers that VALIDATE a spawn before
	 * committing to it (mesh.apply_level_layout's `volume` kind) must get exactly the
	 * answer the spawn itself would give.
	 */
	static UClass* ResolveVolumeClass(const FString& TypeStr, FString& OutError);

	/**
	 * Every `type` token ResolveVolumeClass accepts, in schema order. Public so a
	 * caller can walk the table in REVERSE (class -> token) without restating it;
	 * mesh.capture_level_layout needs that to recognise a volume actor already in
	 * the level. Also the single source of the token list quoted in error text.
	 */
	static const TArray<FString>& GetVolumeTokens();

	/**
	 * The keys `spawn_volume`'s `properties` bag actually honours for a volume class.
	 *
	 * Unlike mesh.place_light / mesh.spawn_actor, this bag is NOT a reflection channel:
	 * the keys are curated snake_case aliases (`damage_per_sec`), not UPROPERTY names,
	 * so the reflection walker cannot validate them. This table is what makes them
	 * checkable instead of silently ignored.
	 */
	static TArray<FString> GetHonouredVolumePropertyKeys(const UClass* VolumeClass);

	/**
	 * Validate a `properties` bag against a volume class WITHOUT touching the world.
	 * An unhonoured key is an error listing the keys this volume type does honour —
	 * a silently dropped key would make mesh.apply_level_layout claim it applied a
	 * document it did not.
	 */
	static bool ValidateVolumeProperties(
		const UClass* VolumeClass, const TSharedPtr<FJsonObject>& Params, FString& OutError);

private:
	static FMonolithActionResult SpawnVolume(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetActorProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CopyActorProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BuildNavmesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SelectActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SnapToSurface(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetCollisionPreset(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---

	/** Read a single FProperty value as string via ExportText_Direct */
	static bool ExportPropertyValue(const FProperty* Prop, const void* ContainerPtr, FString& OutValue);

	/** Import a single FProperty value from string via ImportText_Direct */
	static bool ImportPropertyValue(FProperty* Prop, void* ContainerPtr, const FString& Value, UObject* OwnerObject);
};
