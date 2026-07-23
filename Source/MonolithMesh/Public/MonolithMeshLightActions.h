#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class AActor;
class ULightComponentBase;
class UWorld;

/**
 * Phase 2 (Goldenstone fork roadmap) — typed light surface for level design.
 *
 * WHAT ALREADY EXISTED (do not duplicate it):
 *   `mesh.place_light` / `mesh.set_light_properties` (MonolithMeshLevelDesignActions)
 *   already spawn and configure point/spot/rect/directional lights through a
 *   curated, hand-written parameter list. `mesh.spawn_actor` can spawn any actor
 *   class by name, and `mesh.get_actor_component_properties` reads arbitrary
 *   component UPROPERTYs.
 *
 * WHAT THIS FILE ADDS:
 *   1. SkyLight support. ASkyLight's component is a USkyLightComponent, which
 *      derives from ULightComponentBase and is NOT a ULightComponent, so every
 *      pre-existing light path (`FindComponentByClass<ULightComponent>()`) missed
 *      it entirely.
 *   2. A reflection-backed `properties` channel shared by place_light /
 *      set_light_properties: any UPROPERTY on the light component can be set by
 *      name through FMonolithReflectionWalker instead of a C++ switch, so the
 *      surface tracks the engine across upgrades.
 *   3. Data-driven presets and read-back sets loaded from JSON
 *      (Config/MonolithLightPresets.json + Saved/Monolith/LightPresets/*.json) —
 *      no light values are hardcoded in C++.
 *   4. `mesh.get_light_properties` — typed, light-aware read-back that errors on
 *      an unknown property name instead of silently returning nothing.
 *   5. `mesh.list_light_presets` — discovery for (3).
 *
 * Everything here is a synchronous game-thread call. Spawning a light and writing
 * a handful of UPROPERTYs is microseconds; there is nothing to hand to the Phase 1
 * job system.
 */
class FMonolithMeshLightActions
{
public:
	/** One named preset, loaded from JSON. Values are canonical UPROPERTY names. */
	struct FPreset
	{
		FString Name;
		/** Canonical light type token this preset targets ("point", "sky", ...). Empty = any light. */
		FString TypeToken;
		FString Description;
		/** Absolute path of the file the preset came from (built-in or user override). */
		FString SourceFile;
		/** UPROPERTY name -> value, written through FMonolithReflectionWalker. */
		TSharedPtr<FJsonObject> Properties;
	};

	/** Register the light actions with the tool registry. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// ------------------------------------------------------------------
	// Shared helpers — also used by FMonolithMeshLevelDesignActions and tests.
	// ------------------------------------------------------------------

	/** Canonical light type tokens in schema order: directional, point, spot, rect, sky. */
	static const TArray<FString>& GetLightTypeTokens();

	/**
	 * Resolve a light type token to its ACTOR class.
	 * Accepts the canonical token, the same token with a "light" suffix
	 * ("pointlight"), and the actor class name ("APointLight"), case-insensitively.
	 * Returns nullptr and fills OutError with the full list of valid tokens on a miss.
	 */
	static UClass* ResolveLightActorClass(const FString& Token, FString& OutError);

	/**
	 * Find the light component on an actor. Uses ULightComponentBase (not
	 * ULightComponent) so sky lights resolve too. Returns nullptr + actionable
	 * OutError when the actor carries no light component.
	 */
	static ULightComponentBase* ResolveLightComponent(AActor* Actor, FString& OutError);

	/** Reverse map: light component -> canonical token. Empty string when unrecognised. */
	static FString TokenForLightComponent(const ULightComponentBase* Comp);

	/**
	 * Guard every light action shares: false + actionable OutError when no level
	 * is open (World == nullptr).
	 */
	static bool RequireEditorWorld(const UWorld* World, FString& OutError);

	// --- Data-driven presets -------------------------------------------------

	/** Absolute path of the built-in preset file that ships with the plugin. */
	static FString GetBuiltinPresetFile();

	/** Absolute path of the user preset directory (may not exist). */
	static FString GetUserPresetDirectory();

	/**
	 * Load every preset: built-ins first, then user files (which override by name).
	 * OutWarnings collects per-file parse problems; a broken user file never
	 * prevents the rest from loading.
	 */
	static TMap<FString, FPreset> LoadPresets(TArray<FString>& OutWarnings);

	/** Default read-back property names for a light type token (data-driven, common + per-type). */
	static TArray<FString> LoadReadbackKeys(const FString& TypeToken);

	/** Resolve one preset by name. OutError lists the available names on a miss. */
	static bool ResolvePreset(const FString& Name, FPreset& OutPreset, FString& OutError);

	// --- Reflection write path ----------------------------------------------

	/**
	 * Apply the optional `preset` + `properties` params onto a light component
	 * through UE reflection.
	 *
	 * Validation runs first (FMonolithReflectionWalker::InspectTree against a
	 * scratch buffer), so an unknown or mistyped key fails the whole call WITHOUT
	 * a partial write. On success the edit cradle (Modify / PreEditChange /
	 * PostEditChangeProperty / MarkRenderStateDirty) is fired for each key.
	 *
	 * Returns false + actionable OutError. Applying nothing (neither param
	 * present) is success with an empty OutApplied.
	 */
	static bool ApplyPropertyTree(
		ULightComponentBase* Comp,
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutApplied,
		FString& OutError);

	/**
	 * Closest property-name candidates on a class, for did-you-mean text.
	 * Case-insensitive substring match either way, ranked prefix-first.
	 */
	static TArray<FString> SuggestPropertyNames(const UStruct* Struct, const FString& Query, int32 MaxResults = 5);

private:
	static FMonolithActionResult GetLightProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListLightPresets(const TSharedPtr<FJsonObject>& Params);
};
