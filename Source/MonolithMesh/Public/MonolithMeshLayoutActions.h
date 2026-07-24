#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "MonolithMeshJsonPresets.h"

class AActor;
class UWorld;

/**
 * Phase 2 (Goldenstone fork roadmap), slice 4 — the DATA-DRIVEN LEVEL LAYOUT system.
 *
 * Roadmap wording: "level layouts are defined by data (DataAsset/DataTable), not
 * hardcoded; Claude produces that data, applies it, and verifies it visually."
 *
 * ---------------------------------------------------------------------------
 * WHAT ALREADY EXISTED (verified before writing a line — do not duplicate it)
 * ---------------------------------------------------------------------------
 *   - `mesh.spawn_actor`      — spawn any actor class by name, or a StaticMeshActor
 *                               from a mesh asset path. Generic, one at a time.
 *                               (It had NO property channel when this file was
 *                               written, which is why `kind: "actor"` could not set
 *                               arbitrary properties. Fixed upstream since: it now
 *                               takes `properties` + `component_properties`, and the
 *                               layout gained them for free via pass-through.)
 *   - `mesh.place_light`      — typed light spawn incl. presets + free-form
 *                               reflection properties (slice 1).
 *   - `mesh.spawn_atmosphere` — post_process / height_fog / sky_atmosphere with
 *                               presets + override-aware writes (slice 2).
 *   - `mesh.spawn_volume`     — brush volumes (trigger/post_process/...).
 *   - `mesh.apply_room_template` (MonolithMeshTemplateActions) — the closest prior
 *     art. It IS data-driven (JSON in Saved/Monolith/Templates) but it is a
 *     different thing: it fills ONE blockout volume with grey-box primitives at
 *     volume-relative percentages. It cannot place real assets, lights or
 *     atmosphere, has no identity for what it created, is not idempotent (applying
 *     twice doubles the furniture) and cannot be removed.
 *   - The SP6 spatial registry (`register_building` / `query_room_at` / ...) is an
 *     ANNOTATION model, not a placement model: it records semantics (rooms, doors,
 *     adjacency) about geometry that already exists and never spawns anything. It
 *     is also gated off by default (bEnableProceduralTownGen). So it is not a
 *     layout representation and this file does not build on it.
 *   - NOTHING in the plugin drove placement from a UDataAsset or UDataTable.
 *
 * ---------------------------------------------------------------------------
 * WHY JSON AND NOT UDataAsset / UDataTable
 * ---------------------------------------------------------------------------
 *   1. The author is a language model. A layout has to be producible, diffable and
 *      reviewable as TEXT. A .uasset is opaque binary: Claude could only ever build
 *      one indirectly through more actions, and `git diff` would show nothing.
 *   2. Entries need heterogeneous free-form property bags. A `properties` object is
 *      handed straight to FMonolithReflectionWalker, so an entry can set ANY
 *      UPROPERTY on the thing it places. A UDataTable is a fixed column set by
 *      definition and cannot express that; a UDataAsset could, but only by
 *      re-inventing a JSON-like value union in USTRUCTs.
 *   3. Precedent + reuse. Both preset libraries shipped in this phase are JSON
 *      (Config/*.json built-in + Saved/Monolith/<Kind>/*.json user overrides) and
 *      the loader for that pattern, FMonolithMeshJsonPresets, is already generic.
 *      A layout library is the same shape, so it costs one call to reuse.
 *   4. No recompile, no cook, no redirectors. A user can add a layout to a shipped
 *      plugin by dropping a file.
 *
 *   Honest cost of the choice: no Content Browser presence, no editor property UI,
 *   no asset reference so nothing stops a mesh a layout names from being deleted,
 *   and no cook-time validation. Mitigated by `mesh.describe_level_layout` (which
 *   resolves every entry against the live engine and reports per-entry problems)
 *   and by the shipped-data automation test, which fails the build if a shipped
 *   layout ever names a class, asset or preset that no longer resolves.
 *
 * ---------------------------------------------------------------------------
 * DOCUMENT FORMAT  (Config/MonolithLevelLayouts.json, and user files in
 *                   Plugins/Monolith/Saved/Monolith/LevelLayouts/*.json)
 * ---------------------------------------------------------------------------
 *   {
 *     "layouts": {
 *       "<layout_id>": {
 *         "description": "...",
 *         "folder": "Monolith/Layouts/<layout_id>",   // optional outliner folder
 *         "origin": [0, 0, 0],                        // optional document offset
 *         "entries": [
 *           { "id": "floor", "kind": "actor",
 *             "class": "/Engine/BasicShapes/Cube.Cube",
 *             "location": [0,0,-50], "scale": [8,8,1],
 *             "component_properties": { "Mobility": "Static", "CastShadow": true } },
 *           { "id": "sun", "kind": "light", "type": "directional",
 *             "preset": "sun_golden_hour",
 *             "location": [0,0,900], "rotation": [-38,145,0] },
 *           { "id": "fog", "kind": "atmosphere", "type": "height_fog",
 *             "preset": "fog_morning_haze", "location": [0,0,0] },
 *           { "id": "entry_trigger", "kind": "volume", "type": "trigger",
 *             "location": [-480,0,100], "extent": [60,200,100] }
 *         ]
 *       }
 *     }
 *   }
 *
 *   `kind` selects WHICH EXISTING ACTION places the entry:
 *       actor      -> mesh.spawn_actor
 *       light      -> mesh.place_light
 *       atmosphere -> mesh.spawn_atmosphere
 *       volume     -> mesh.spawn_volume   (type: trigger | blocking | kill | pain |
 *                                          nav_modifier | audio | post_process)
 *   Every other key on an entry is FORWARDED VERBATIM to that action. That is the
 *   whole mapping: there is no per-parameter translation table to drift out of
 *   date, and a layout automatically gains any parameter those actions gain. The
 *   only rewrites are `class` -> `class_or_mesh` (readability) and `name`/`label`,
 *   which the layout owns because it assigns actor labels itself.
 *
 *   Composability is therefore free: an entry says `"preset": "bulb_warm_60w"`
 *   rather than restating fifteen light properties, because `preset` is just a
 *   forwarded parameter of mesh.place_light.
 *
 *   FREE-FORM PROPERTIES. Pass-through means a layout can only set what the target
 *   action exposes, so the property channels are per-kind and each one is the target
 *   action's own:
 *       light      -> `properties`  = UPROPERTYs on the light COMPONENT
 *       atmosphere -> `properties`  = fields of the atmosphere settings struct
 *       actor      -> `properties`            = UPROPERTYs on the ACTOR
 *                     `component_properties`  = UPROPERTYs on its ROOT COMPONENT
 *                     (Mobility, CastShadow, ... — the StaticMeshComponent for a
 *                      mesh path). Both were added to mesh.spawn_actor for this;
 *                      the layout gets them for free because it forwards verbatim.
 *       volume     -> `properties`  = spawn_volume's CURATED keys (damage_per_sec,
 *                     pain_causing, priority, unbound, blend_radius, blend_weight),
 *                     which are aliases rather than UPROPERTY names.
 *   All four are validated in phase 1 against the real engine class (or, for volumes,
 *   against spawn_volume's honoured-key table), so a mistyped property name fails the
 *   apply before anything is placed — see the partial-failure contract below.
 *
 * ---------------------------------------------------------------------------
 * IDENTITY / IDEMPOTENCY
 * ---------------------------------------------------------------------------
 *   Every actor an apply creates carries two actor tags:
 *       Monolith.Layout:<layout_id>
 *       Monolith.LayoutEntry:<entry_id>
 *   Tags are the identity because they live ON the actor: they survive save/load,
 *   travel with a copied actor, are visible to the user in the details panel, and
 *   cannot drift out of sync with the level the way a side-car registry file would.
 *   Labels and outliner folders are cosmetics derived from them, never the identity.
 *
 *   `mesh.apply_level_layout` with the default on_existing="replace" removes the
 *   ENTIRE previous instance of that layout id and places the document fresh, so
 *   applying twice is a no-op in count and the world always matches the document
 *   (entries deleted from the document disappear from the level). on_existing="skip"
 *   places only entries with no tagged actor and leaves everything else untouched.
 *   `mesh.remove_level_layout` deletes by tag alone, so it works even if the
 *   document is gone.
 *
 * ---------------------------------------------------------------------------
 * PARTIAL-FAILURE CONTRACT: ALL-OR-NOTHING
 * ---------------------------------------------------------------------------
 *   A layout is a composition. Half of one is not a smaller correct layout, it is a
 *   broken one the author then has to diff by hand. So:
 *     Phase 1 resolves and validates EVERY entry without touching the world (ids,
 *     kinds, required params of the target action, unknown params, vector shapes,
 *     actor classes and mesh assets, light/atmosphere/volume type tokens, preset
 *     names and preset/type compatibility, and every key of an actor or volume
 *     entry's property bags — the actor bags against the class default object, the
 *     volume bag against spawn_volume's honoured-key table). Any problem fails the
 *     call, reports EVERY failing entry with its reason, and leaves the level
 *     untouched — the common failure (a typo) costs nothing and is reported at once.
 *     Note the entry-level unknown-key check (FMonolithParamSchema::FindUnknownKeys)
 *     is TOP-LEVEL only, so declaring `properties` in the target action's schema
 *     makes the bag legal without relaxing the check on any sibling key.
 *     Phase 2 places. The previous instance is removed only AFTER every entry has
 *     been created, so if the engine still refuses a spawn, everything this call
 *     created is destroyed, the transaction is cancelled, and the previous instance
 *     is still standing. Either the whole document landed or nothing changed.
 *
 * ---------------------------------------------------------------------------
 * SCALE
 * ---------------------------------------------------------------------------
 *   Applying is spawn + tag + a handful of reflection writes per entry, all on the
 *   game thread. Measured by MonolithMeshLayoutActionsTests (LargeLayoutTiming):
 *   see that test's logged ms/entry. It is far below the threshold where the Phase 1
 *   job system would earn its complexity, so apply stays synchronous and returns
 *   `elapsed_ms` so a caller can see the cost of its own document. The cost that
 *   WOULD dominate a huge layout is loading unique mesh assets, which is disk I/O
 *   the job system cannot move off the game thread anyway (UStaticMesh loads and
 *   actor spawns are both game-thread-only).
 *
 * ---------------------------------------------------------------------------
 * CAPTURE — the reverse direction (mesh.capture_level_layout)
 * ---------------------------------------------------------------------------
 *   Apply alone makes the data a one-way street: nudge a light in the viewport
 *   because it looks better and the next apply silently discards the nudge (that
 *   is the deliberate consequence of "the document is the source of truth").
 *   Capture closes the loop — arrange or tweak by hand, capture to a document,
 *   re-apply anywhere.
 *
 *   SOURCES (`source`)
 *     layout     (default) every actor tagged Monolith.Layout:<id>. This is the
 *                loop that matters: apply -> tweak -> capture -> save overwrite.
 *     actors     an explicit list of actor names/labels.
 *     selection  whatever is selected in the editor right now, read through
 *                GEditor->GetSelectedActors() — the same call mesh.select_actors
 *                already uses, so there is no second notion of "selected".
 *
 *   WHAT IS WRITTEN PER ENTRY — two filters, both deliberate
 *     1. An ALLOWLIST of property names, taken from data, never from a C++ table:
 *          light       -> Config/MonolithLightPresets.json      `readback.<type>`
 *          atmosphere  -> Config/MonolithAtmospherePresets.json `readback.<type>`
 *          actor       -> Config/MonolithLevelLayouts.json      `readback.actor`
 *                         and `readback.actor_component`
 *          post-process actor knobs -> `readback.atmosphere_actor`
 *        Dumping every UPROPERTY instead would produce hundreds of engine defaults
 *        per actor: unreadable, undiffable, and full of values the layout format
 *        cannot honour. The allowlist is also how AActor::Tags is kept OUT — it is
 *        simply not listed, and re-emitting it would fight the layout's own tags.
 *     2. ONLY WHAT DIFFERS FROM THE DEFAULT. "Default" is the object's ARCHETYPE
 *        (Comp->GetArchetype() / Actor->GetArchetype()), not the class CDO, because
 *        that is exactly what a fresh spawn produces — an APointLight's component
 *        template, not UPointLightComponent's CDO. The comparison is
 *        FProperty::Identical, i.e. exact engine value equality, not JSON text.
 *        `include_defaults=true` turns filter 2 off; filter 1 always applies.
 *
 *     FPostProcessSettings is the ONE exception, and it has to be: every field
 *     there is inert unless its sibling bOverride_<Field> bit is set. So a
 *     post-process volume is captured by walking the override bits — every field
 *     whose bit is ON, whatever the readback list says, and nothing whose bit is
 *     OFF. That is both more faithful (nothing that is actually in effect is lost)
 *     and more honest (a value sitting behind a disabled override is not in effect,
 *     and re-applying it would silently turn the override ON).
 *
 *   PRESETS ARE PREFERRED, AND ONLY ON AN EXACT MATCH. Before writing raw values a
 *   capture asks every type-compatible preset: "if I applied you, would every one
 *   of your keys end up byte-identical to what is in the level?" The question is
 *   answered by running the preset's JSON value through the REAL write path
 *   (FMonolithReflectionWalker::WriteLeaf) into a scratch buffer and comparing with
 *   FProperty::Identical — so it is exact by construction and immune to the
 *   float-literal trap (0.5357 in JSON is not == (double)0.5357f). Nothing is lost
 *   by a match: keys the preset covers are dropped from the raw bag only because
 *   they were proved equal, and every other captured key is still written.
 *
 *   WHERE THE ROUND TRIP IS LOSSY — the capture SAYS SO, it never drops silently.
 *   Every one of these produces a `warnings` line, and the same list is embedded in
 *   the document under `capture.warnings` so it survives being saved:
 *     - an actor the format cannot express is REFUSED, not half-written: Blueprint
 *       classes (a layout names classes by short name, which only resolves if the
 *       Blueprint happens to be loaded), a StaticMeshActor whose mesh lives in the
 *       transient package, a class name that resolves to a different class. Those
 *       actors come back in `skipped` with a per-actor reason.
 *     - a light/atmosphere/volume actor with a non-unit scale (place_light,
 *       spawn_atmosphere and spawn_volume have no `scale` parameter).
 *     - a volume brush that is not the box builder those actions create, so its
 *       `extent` cannot be recovered exactly.
 *     - spawn_volume's curated property aliases (damage_per_sec, ...), which are
 *       not UPROPERTY names and therefore cannot be read back through reflection.
 *     - a property whose captured value the write path would refuse (checked with
 *       InspectTree, the same validation the real write runs).
 *   A captured document is ALSO resolved through ResolveLayout before it is
 *   returned, so `valid` / `problems` say up front whether it would apply.
 *
 *   OUTPUT. The document body is always returned in the response. `save=true`
 *   writes it through mesh.save_level_layout — the existing writer, which validates
 *   before writing — rather than a second serialiser.
 */
class FMonolithMeshLayoutActions
{
public:
	/** Register the six layout actions with the tool registry. */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// ------------------------------------------------------------------
	// Tag identity — shared with the tests.
	// ------------------------------------------------------------------

	/** Tag prefix marking which layout an actor belongs to ("Monolith.Layout:"). */
	static const TCHAR* LayoutTagPrefix();
	/** Tag prefix marking which entry of that layout an actor is ("Monolith.LayoutEntry:"). */
	static const TCHAR* EntryTagPrefix();

	static FName MakeLayoutTag(const FString& LayoutId);
	static FName MakeEntryTag(const FString& EntryId);

	/** Every actor in World tagged as belonging to LayoutId. */
	static TArray<AActor*> FindLayoutActors(UWorld* World, const FString& LayoutId);

	/** The entry id an actor was placed as, or empty when it carries no entry tag. */
	static FString GetEntryId(const AActor* Actor);

	// ------------------------------------------------------------------
	// Layout kinds — token -> the existing action that places it.
	// ------------------------------------------------------------------

	struct FKind
	{
		/** Layout `kind` token. */
		FString Token;
		/** Namespace + action this kind delegates placement to. */
		FString Namespace;
		FString Action;
	};

	/** All supported kinds, in document order. */
	static const TArray<FKind>& GetKinds();
	/** Kind by token, or nullptr. */
	static const FKind* FindKind(const FString& Token);

	// ------------------------------------------------------------------
	// Data library
	// ------------------------------------------------------------------

	/** The shared layout library instance (built-in file + user override directory). */
	static const FMonolithMeshJsonPresets& Library();

	/** Every layout body by id, built-ins first then user files (which override by id). */
	static TMap<FString, FMonolithNamedJsonObject> LoadLayouts(TArray<FString>& OutWarnings);

	// ------------------------------------------------------------------
	// Validation (phase 1) — no world mutation, ever.
	// ------------------------------------------------------------------

	/** One entry, resolved and ready to place. */
	struct FResolvedEntry
	{
		FString EntryId;
		FString KindToken;
		FString Namespace;
		FString Action;
		/** Params handed to the target action verbatim (already offset + foldered). */
		TSharedPtr<FJsonObject> Params;
		/** Label the placed actor gets. */
		FString Label;
	};

	/**
	 * Resolve a whole layout body against the live engine.
	 *
	 * @param LayoutId   Identity used for tagging and for the default folder/labels.
	 * @param Body       The object under `layouts.<id>`.
	 * @param Origin     Extra world offset added to every entry location.
	 * @param FolderOverride Outliner folder for entries that do not name their own.
	 * @param OutEntries Resolved entries, document order (only when the return is true).
	 * @param OutProblems One "<entry id>: <reason>" line per rejected entry.
	 * @return true only when EVERY entry resolved.
	 */
	static bool ResolveLayout(
		const FString& LayoutId,
		const TSharedPtr<FJsonObject>& Body,
		const FVector& Origin,
		const FString& FolderOverride,
		TArray<FResolvedEntry>& OutEntries,
		TArray<FString>& OutProblems);

	/** Guard every layout action shares: false + actionable OutError when no level is open. */
	static bool RequireEditorWorld(const UWorld* World, FString& OutError);

	// ------------------------------------------------------------------
	// Capture (level -> document). See the CAPTURE section of the file header.
	// ------------------------------------------------------------------

	/**
	 * Capture-time property allowlists that live in the layout library itself
	 * (Config/MonolithLevelLayouts.json, section `readback`). Sections:
	 *   actor             UPROPERTYs on the ACTOR                 (-> entry.properties)
	 *   actor_component   UPROPERTYs on its ROOT COMPONENT        (-> entry.component_properties)
	 *   atmosphere_actor  actor-level knobs of a post-process volume (-> entry.actor_properties)
	 * A name that does not exist on a particular class is skipped, not an error:
	 * these lists span several classes on purpose (CastShadow exists on a primitive
	 * root, not on a bare USceneComponent).
	 */
	static TArray<FString> LoadCaptureKeys(const FString& SectionToken, TArray<FString>& OutWarnings);

	/** One actor the capture refused to express, with the reason the caller is told. */
	struct FCaptureSkip
	{
		FString ActorName;
		FString ActorClass;
		FString Reason;
	};

	/** Knobs of a capture. Defaults are the ones the action advertises. */
	struct FCaptureOptions
	{
		/** Id the produced document gets (tags, folder and labels on re-apply). */
		FString LayoutId;
		/** World offset SUBTRACTED from every captured location and stored as the document origin. */
		FVector Origin = FVector::ZeroVector;
		/** Outliner folder recorded in the document (empty = let apply pick its default). */
		FString Folder;
		FString Description;
		/** Write every allowlisted property, including those still at their archetype value. */
		bool bIncludeDefaults = false;
		/** Reference a preset when applying it would reproduce the live values exactly. */
		bool bUsePresets = true;
		/** Entry ids in this order come first (the previous document's order, for clean diffs). */
		TArray<FString> PreferredOrder;
	};

	/** Everything a capture produced, including what it could not express. */
	struct FCaptureResult
	{
		/** Layout body: {description?, folder?, origin?, capture?, entries:[...]}. */
		TSharedPtr<FJsonObject> Body;
		TArray<FString> Warnings;
		TArray<FCaptureSkip> Skipped;
		/** Preset names referenced instead of raw values, in entry order. */
		TArray<FString> PresetsUsed;
		int32 EntryCount = 0;
	};

	/**
	 * Build a layout document body from live actors. Reads only — never spawns,
	 * never writes a property, never opens a transaction.
	 *
	 * @return false + OutError only for a whole-call problem (no actors at all).
	 *         Per-actor refusals are reported in OutResult.Skipped, never fatal.
	 */
	static bool CaptureActors(
		const TArray<AActor*>& Actors,
		const FCaptureOptions& Options,
		FCaptureResult& OutResult,
		FString& OutError);

private:
	/** Registers mesh.capture_level_layout (implemented in MonolithMeshLayoutCapture.cpp). */
	static void RegisterCaptureAction(FMonolithToolRegistry& Registry);
	static FMonolithActionResult CaptureLevelLayout(const TSharedPtr<FJsonObject>& Params);


	static FMonolithActionResult ListLevelLayouts(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeLevelLayout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ApplyLevelLayout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveLevelLayout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SaveLevelLayout(const TSharedPtr<FJsonObject>& Params);
};
