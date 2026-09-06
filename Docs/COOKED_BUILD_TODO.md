# Cooked-Build Support for Runtime Classes — TODO

**Status:** UNRESOLVED integration coverage. Affects Phase I1, I2, I3 features (UI↔GAS binding, BT↔GAS task, Audio→AI stimulus).

**2026-09-06 RecycleCo evidence:** A Win64 Development package built from Monolith commit `45283ac` loaded `MonolithAudioRuntime`, initialized its perception subsystem, and hooked one AudioComponent. The earlier blanket claim that this runtime module cannot ship is incorrect for that configuration. The test did **not** exercise UI↔GAS, BT↔GAS, or Audio→AI stimulus binding behavior, so their end-to-end cooked coverage remains open. The permanent host-project report is `D:/UnrealProjects/RecycleCo/Docs/MONOLITH_TEST_SONUCLARI.md`; this is a local evidence path, not a file shipped in this repository.

**Decision (2026-04-25):** Path 3 — accept PIE/editor-only for now, document loudly, revisit before Steam launch.

---

## The Problem

Monolith consists primarily of **Editor** modules, plus `MonolithAudioRuntime`, a **Runtime** module that can ship when the plugin is included in the game target. Three Phase I features introduce classes that content assets reference; their module placement and actual cooked behavior must be considered separately:

| Phase | Runtime class | Module type | Cooked-build behaviour |
|-------|---------------|-------------|------------------------|
| **I1** | `UMonolithGASAttributeBindingClassExtension` | MonolithGAS (Editor) | Class missing → WBP load warning, bindings silently dropped |
| **I2** | `UBTTask_TryActivateAbility` | MonolithAI (Editor) | Class missing → BehaviorTree load fails, task node unresolvable |
| **I3** | `UMonolithSoundPerceptionUserData` + `UMonolithAudioPerceptionSubsystem` | MonolithAudioRuntime (Runtime) | Module/subsystem loading verified in RecycleCo Win64 Development; user-data persistence and actual AI stimulus delivery not yet verified |

If the Monolith plugin is excluded from the game target entirely, its runtime module is absent too. Do not infer that exclusion merely from the other modules being Editor-only.

## Current Effect

- **PIE / editor:** All three features work as designed. Author content via Monolith MCP actions; runtime behaviour fires as expected during PIE.
- **Cooked build:** I1/I2 classes remain in Editor modules and are unresolved shipping dependencies for content that uses them. I3's runtime module can load, as observed in RecycleCo; its full binding/stimulus behavior still needs a dedicated cooked test. The RecycleCo result covers Win64 Development, not a Shipping configuration.

## Resolution Options (deferred)

### Option A — Drop runtime classes entirely (cleanest)
Replace each Phase I feature with stock-UE-only mechanisms:

- **I1:** Use UMG `PropertyBindings` + a runtime delegate-driven binding system that exists in stock UE 5.7. Monolith action stamps `FDelegateRuntimeBinding` entries onto the WBP — these are engine-native and survive cooking.
- **I2:** Use the GAS-companion plugin's `BTTask_RunGameplayAbility` if available, or a stock engine BTTask if present. If neither exists, emit a Blueprint-graph BT that uses stock nodes (RunBehaviorTree → custom event → ASC->TryActivateAbility).
- **I3:** Stamp metadata onto the SoundCue via stock `UAssetUserData` subclass that lives in the *project's own* runtime module (a project gameplay module), not in Monolith. The audio component listener subsystem also lives in project code.

**Effort:** Substantial — likely re-architects all three features (~30-40h).
**Trade-off:** Less feature flexibility; some H-plan capabilities may not be expressible via stock UE.

### Option B — Sibling shipping plugin
A new runtime sibling plugin (e.g. `MyProjectRuntimeBindings`) — Type: Runtime, ships in the cooked game. Holds the three runtime classes. Monolith authors INTO this plugin's classes.

**Effort:** ~2-3 days of refactor.
**Trade-off:** New plugin to maintain; users of Monolith outside this project would also need this plugin or write their own equivalent.

### Option C — Status quo (CURRENT)
Document I1/I2's Editor-module limitation and I3's unverified stimulus behavior. Useful for prototyping. Required shipping features must pass a dedicated cooked behavior test; loading the I3 module alone does not close that gate.

## Pre-Steam-Launch Checklist

Before Steam release, REVISIT this TODO:

- [ ] Audit which I-phase features are actually used in shipping game content
- [ ] If unused: delete authoring actions from Monolith, keep editor functionality
- [ ] If used: test each feature in the cooked game; pick Option A or B and schedule a refactor where the existing runtime path fails or depends on Editor classes
- [ ] Add cooked-build smoke test that loads representative WBP/BT/SoundCue assets and verifies no warnings

## Cross-References

- Phase I1 plan: `Docs/plans/2026-04-26-ui-gas-attribute-binding.md`
- Phase I2 plan: `Docs/plans/2026-04-26-bt-gas-ability-task.md`
- Phase I3 plan: `Docs/plans/2026-04-26-audio-ai-stimulus-binding.md`
- Comprehensive fix plan: `Plugins/Monolith/Docs/plans/2026-04-25-comprehensive-fix-plan.md`

## Owner

TBD — flag at next planning checkpoint.
