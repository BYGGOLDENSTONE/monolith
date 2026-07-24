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

	// ------------------------------------------------------------------
	// `spawn_volume`'s `properties` bag
	//
	// It IS a reflection channel (FMonolithReflectionWalker over the volume ACTOR),
	// exactly like mesh.place_light's and mesh.spawn_actor's. It did not start that
	// way: it began as six curated snake_case aliases read by hand-written
	// if-statements, and because those are not UPROPERTY names, nothing could read
	// them back — mesh.capture_level_layout had to warn that a volume's settings were
	// invisible to it, so a volume could not survive capture -> re-apply.
	//
	// COMPATIBILITY DECISION: the aliases were NOT removed. They are translated to the
	// properties they always meant (see GetVolumePropertyAliases), so every existing
	// call and every saved layout document keeps working unchanged, while capture
	// writes the canonical names that actually round-trip. Deleting a working call
	// shape to save six lines of mapping would be a worse trade.
	// ------------------------------------------------------------------

	/** One curated snake_case key and the UPROPERTY it is a spelling of. */
	struct FVolumePropertyAlias
	{
		const TCHAR* Alias;
		const TCHAR* Property;
	};

	/** The whole alias table. Adding a row here is the only edit a new alias needs. */
	static const TArray<FVolumePropertyAlias>& GetVolumePropertyAliases();

	/** Key -> the UPROPERTY name it means. A name that is not an alias is returned as-is. */
	static FString CanonicalVolumePropertyName(const FString& Key);

	/** A `properties` bag with every alias key renamed to its UPROPERTY name. */
	static TSharedPtr<FJsonObject> TranslateVolumeProperties(const TSharedPtr<FJsonObject>& InBag);

	/**
	 * The curated aliases that are still meaningful on a volume class — i.e. those
	 * whose UPROPERTY really exists on it. Derived from the class, never restated per
	 * class, so it cannot drift. Used in error text and in the docs.
	 */
	static TArray<FString> GetHonouredVolumePropertyKeys(UClass* VolumeClass);

	/**
	 * Validate a `properties` bag against a volume class WITHOUT touching the world:
	 * key names (after alias translation, but reported in the caller's own spelling,
	 * with did-you-mean) and then values, through the same coercion the real write
	 * runs. A silently dropped key would make mesh.apply_level_layout claim it applied
	 * a document it did not.
	 */
	static bool ValidateVolumeProperties(
		UClass* VolumeClass, const TSharedPtr<FJsonObject>& Params, FString& OutError);

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
