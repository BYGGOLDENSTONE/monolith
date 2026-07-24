#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "MonolithMeshJsonPresets.h"

class AActor;
class UWorld;
class UStruct;
class FProperty;

/**
 * Phase 2 (Goldenstone fork roadmap), slice 2 — atmosphere + global illumination.
 *
 * WHAT ALREADY EXISTED (verified, not duplicated):
 *   - `mesh.spawn_volume type=post_process` (MonolithMeshVolumeActions) spawns an
 *     APostProcessVolume with brush geometry. It exposes exactly FOUR hand-written
 *     actor-level knobs — bUnbound / BlendRadius / BlendWeight / Priority — and
 *     CANNOT touch APostProcessVolume::Settings at all. No exposure, no bloom, no
 *     colour grading, no Lumen. That action is left untouched.
 *   - `mesh.get_actor_properties` / `mesh.copy_actor_properties` read and copy
 *     arbitrary UPROPERTYs as ExportText strings.
 *   - NOTHING existed for ExponentialHeightFog, SkyAtmosphere, or Lumen. Greps for
 *     "ExponentialHeightFog", "SkyAtmosphere" and "Lumen" across Source/ returned
 *     only comments and a scene-capture mention in MonolithMeshLightingActions.
 *
 * WHAT THIS FILE ADDS:
 *   1. A typed atmosphere surface over four actor types — `post_process`
 *      (APostProcessVolume), `height_fog` (AExponentialHeightFog),
 *      `sky_atmosphere` (ASkyAtmosphere) and `volumetric_cloud`
 *      (AVolumetricCloud) — spawn + configure + read back.
 *   2. Override-aware writes. Every FPostProcessSettings field is inert unless its
 *      sibling `bOverride_<Field>` bit is set; a naive reflection write therefore
 *      "succeeds" and changes nothing on screen. Writing a key through this surface
 *      also enables its override bit (unless the caller sets it explicitly), and the
 *      response reports which bits were flipped.
 *   3. A Lumen read/write pair scoped to the post-process volume surface, plus a
 *      READ-ONLY project-level diagnostic block from URendererSettings.
 *   4. Data-driven presets and read-back sets from JSON
 *      (Config/MonolithAtmospherePresets.json + Saved/Monolith/AtmospherePresets/).
 *
 * WHICH LUMEN SURFACE IS COVERED — and which is not:
 *   Lumen is configurable in three places. This slice covers ONE of them for writes:
 *     (a) PER-VOLUME: FPostProcessSettings on a post-process volume. COVERED
 *         (read + write). This is the only place a level designer changes Lumen
 *         per area, it takes effect immediately with no restart, and it is the one
 *         that belongs in a level's saved data.
 *     (b) PROJECT: URendererSettings (r.DynamicGlobalIlluminationMethod,
 *         r.Lumen.HardwareRayTracing, r.GenerateMeshDistanceFields, ...).
 *         READ ONLY here, exposed as the `project` block of mesh.get_lumen_settings
 *         because "my Lumen does nothing" is almost always answered by it. NOT
 *         written: these are `config` properties that persist to DefaultEngine.ini,
 *         several are ConfigRestartRequired, and silently rewriting a user's project
 *         config from a level-design call is the wrong contract.
 *     (c) CVARS / scalability (r.Lumen.*): NOT covered at all. Transient,
 *         session-scoped, and not part of a level.
 *
 * THE VOLUMETRIC CLOUD'S MATERIAL DEPENDENCY (the one thing clouds have that the
 * other three types do not):
 *   A cloud renders through UVolumetricCloudComponent::Material — a Volume domain
 *   material asset. With no loadable material the actor exists, every call reports
 *   success, and the sky stays empty.
 *   DECISION: inherit the engine default, do NOT add a `material` parameter.
 *     - The component constructor already assigns
 *       /Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst as a SOFT
 *       reference, so a bare spawn is already a visible cloud and nothing is
 *       force-loaded that the caller did not ask for.
 *     - `Material` is a plain reflected FSoftObjectProperty, so the EXISTING
 *       `properties` channel sets it by path already:
 *       properties={"Material": "/Game/Sky/M_MyCloud"}. A dedicated parameter would
 *       be a second spelling of a thing that works, and layouts forward `properties`
 *       for free while a new parameter would need plumbing on both sides.
 *     - The write really takes effect: PreEditChange installs an
 *       FComponentReregisterContext and PostEditChangeProperty tears it down, so
 *       UVolumetricCloudComponent::OnRegister re-runs Material.LoadSynchronous().
 *   HONESTY: spawn_atmosphere / set_atmosphere_properties / get_atmosphere_properties
 *   return a `material` block for cloud actors — {path, loaded, note}. If the engine
 *   default is missing, or a caller names a path that does not exist, `loaded` is
 *   false and the note says the cloud will render nothing. Nothing is silently empty.
 *
 * Everything here is a synchronous game-thread call — spawning an actor and writing
 * a handful of UPROPERTYs is microseconds, so nothing is handed to the job system.
 */
class FMonolithMeshAtmosphereActions
{
public:
	/** Register the atmosphere actions with the tool registry. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// ------------------------------------------------------------------
	// Type tables — shared with the tests.
	// ------------------------------------------------------------------

	/** Canonical SPAWNABLE atmosphere tokens: post_process, height_fog, sky_atmosphere, volumetric_cloud. */
	static const TArray<FString>& GetAtmosphereTokens();

	/**
	 * Every token that names a readback section / preset target, including the two
	 * that are not spawnable actors: "lumen" (a restricted view of
	 * FPostProcessSettings) and "project" (read-only URendererSettings).
	 */
	static const TArray<FString>& GetSectionTokens();

	/**
	 * Resolve an atmosphere type token to its ACTOR class. Accepts the canonical
	 * token plus common aliases ("fog", "postprocess", "APostProcessVolume", ...).
	 * Returns nullptr + actionable OutError on a miss.
	 */
	static UClass* ResolveAtmosphereActorClass(const FString& Token, FString& OutError);

	/**
	 * Resolve a SECTION token to the UStruct its property names must resolve against.
	 * post_process/lumen -> FPostProcessSettings, height_fog/sky_atmosphere/
	 * volumetric_cloud -> the component class, project -> URendererSettings.
	 * nullptr for an unknown token.
	 * This is what the shipped-data validation test walks.
	 */
	static UStruct* ResolveSectionStruct(const FString& Token);

	/** Canonical token for an already-spawned actor, or empty when it is not one of ours. */
	static FString TokenForActor(const AActor* Actor);

	// ------------------------------------------------------------------
	// Write target
	// ------------------------------------------------------------------

	/**
	 * Where an atmosphere actor's settings actually live.
	 *
	 * For a post-process volume the settings are a STRUCT MEMBER of the actor
	 * (APostProcessVolume::Settings), not a component — so Struct/Container address
	 * the struct while CradleObject/OuterStructProp carry the actor-level edit
	 * cradle. For fog and sky the target is the component itself.
	 */
	struct FTarget
	{
		FString Token;
		AActor* Actor = nullptr;
		/** Object that receives Modify/PreEditChange/PostEditChangeProperty. */
		UObject* CradleObject = nullptr;
		/** Struct/class the JSON keys are resolved against. */
		UStruct* Struct = nullptr;
		/** Address the keys are written into. */
		void* Container = nullptr;
		/** For struct-member targets: the owning FStructProperty on the actor class. */
		FProperty* OuterStructProp = nullptr;
		/** True when this struct uses the FPostProcessSettings bOverride_<Key> convention. */
		bool bOverrideAware = false;
	};

	/** Resolve an actor to its settings target. False + actionable OutError on a miss. */
	static bool ResolveTarget(AActor* Actor, FTarget& OutTarget, FString& OutError);

	/** Guard shared by every action: false + actionable OutError when no level is open. */
	static bool RequireEditorWorld(const UWorld* World, FString& OutError);

	// ------------------------------------------------------------------
	// Data-driven presets
	// ------------------------------------------------------------------

	/** The shared preset library instance (built-in file + user override directory). */
	static const FMonolithMeshJsonPresets& Presets();

	/** Read-back property names for a section token. */
	static TArray<FString> LoadReadbackKeys(const FString& SectionToken, TArray<FString>& OutWarnings);

	/**
	 * True when a preset of type PresetToken may be applied to a TargetToken actor.
	 * Identical tokens always match; "lumen" presets additionally apply to
	 * "post_process" targets because they write the same struct.
	 */
	static bool IsPresetCompatible(const FString& PresetToken, const FString& TargetToken);

	// ------------------------------------------------------------------
	// Reflection write path
	// ------------------------------------------------------------------

	/**
	 * Apply a merged {preset + properties} tree onto a target through UE reflection.
	 *
	 * Validation runs first (FMonolithReflectionWalker::InspectTree against scratch
	 * buffers) so an unknown or mistyped key fails the whole call WITHOUT a partial
	 * write. On an override-aware target, every written key K also gets
	 * bOverride_K = true unless the caller supplied bOverride_K itself; those keys
	 * come back in OutOverrides.
	 *
	 * @param RestrictToKeys  When non-empty, any key outside this set is refused
	 *                        before anything is written (used by set_lumen_settings).
	 */
	static bool ApplyPropertyTree(
		const FTarget& Target,
		const TSharedPtr<FJsonObject>& Params,
		const FString& PresetField,
		const FString& PropertiesField,
		const TArray<FString>& RestrictToKeys,
		TArray<FString>& OutApplied,
		TArray<FString>& OutOverrides,
		FString& OutError);

	/** Write an `actor_properties` tree straight onto the actor's own UPROPERTYs. */
	static bool ApplyActorPropertyTree(
		AActor* Actor,
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutApplied,
		FString& OutError);

private:
	static FMonolithActionResult SpawnAtmosphere(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetAtmosphereProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetAtmosphereProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListAtmospherePresets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetLumenSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetLumenSettings(const TSharedPtr<FJsonObject>& Params);
};
