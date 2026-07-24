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
 *             "location": [0,0,-50], "scale": [8,8,1] },
 *           { "id": "sun", "kind": "light", "type": "directional",
 *             "preset": "sun_golden_hour",
 *             "location": [0,0,900], "rotation": [-38,145,0] },
 *           { "id": "fog", "kind": "atmosphere", "type": "height_fog",
 *             "preset": "fog_morning_haze", "location": [0,0,0] }
 *         ]
 *       }
 *     }
 *   }
 *
 *   `kind` selects WHICH EXISTING ACTION places the entry:
 *       actor      -> mesh.spawn_actor
 *       light      -> mesh.place_light
 *       atmosphere -> mesh.spawn_atmosphere
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
 *     actor classes and mesh assets, light/atmosphere type tokens, preset names and
 *     preset/type compatibility). Any problem fails the call, reports EVERY failing
 *     entry with its reason, and leaves the level untouched — the common failure
 *     (a typo) costs nothing and is reported all at once.
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
 */
class FMonolithMeshLayoutActions
{
public:
	/** Register the five layout actions with the tool registry. */
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

private:
	static FMonolithActionResult ListLevelLayouts(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeLevelLayout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ApplyLevelLayout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveLevelLayout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SaveLevelLayout(const TSharedPtr<FJsonObject>& Params);
};
