# Blueprint / Animation completion — 2026-09-06

Status: UE 5.7 RecycleCo editor build 8 succeeded. Full automation run 5 performed 173 tests: 154 clean passes, 19 passes with warnings, and zero failures. The Blueprint, Animation, and async GAS extension regressions passed cleanly. Normal PIE, cooked Development run 2, and Shipping each passed all 24 runtime checks with three hearing events. Both packaged processes exited with code 0; Development run 2 had no error or ensure, confirming the async-constructor fix. Final host restoration is verified in the [canonical completion report](../COMPLETION_2026_09_06.md): 22/22 preserved hashes match and no new host files or Content assets remain.

## Implemented

- Strict type parsing for `set_variable_type`, `add_local_variable`, `add_replicated_variable`, and user-defined struct fields. Struct fields are validated before package creation.
- Component property name/editability and complete value validation before inherited override creation. Imports use scratch storage, so a failed or partially consumed import cannot change the component. Scaffold mesh assignment loads its asset before requesting a writable override.
- Root component alias class constraints and `asset_path` aliases for scaffold `bp_path` inputs; inherited read accepts `component_name`.
- Asset/graph-anchored `get_graph_node_properties` and `set_graph_node_property`, editable scalar leaves, dotted struct traversal, property notifications, pin reconstruction and returned pin metadata; default `save:false`.
- State-machine builder uses the standalone structured expression authoring helper. Invalid rules and failed comparison authoring are reported as partial/deferred, without claiming rule application.
- Native animation layer typed inputs through `input_poses:[{name,inputs:[{name,type}]}]`, preflight validation, `parameter_count`, native/interface `layer_kind`, and reconstruction after compilation.

## Verification

| Check | Status / coverage |
|---|---|
| `git diff --check -- Source/MonolithBlueprint Source/MonolithAnimation` | Passed before handoff |
| `python Scripts/check_schema_drift.py` | Passed: 1,585 registrations, zero drift actions/keys and zero parser errors (current source snapshot). |
| UE editor build 4 | Passed; `Saved/Completion20260906/build-editor-4.log` ends `Result: Succeeded`. |
| Full `Monolith.` automation run 4 | 171 tests performed, 152 clean + 19 with warnings, zero failures. `Saved/Completion20260906/automation-4.log`. |
| Full `Monolith.` automation run 5 | 173 tests performed, 154 clean + 19 with warnings, zero failures. `Saved/Completion20260906/automation-5.log`; clean editor exit after subsequent normal PIE. |
| `Monolith.GAS.Runtime.AsyncExtensionConstruction` | Passed cleanly in run 5: worker-thread UObject construction and safe destruction without tick registration. |
| Normal PIE after async tick fix | `Saved/Completion20260906/runtime-pie.json`: `{"failures":0,"heard":3,"checks":24}`. Explicit `stop_pie` followed by editor quit completed cleanly. Includes generated GAS initial/delegate/retry/smoothing/tick behavior and hosted-widget soft-reference/owner resolution. |
| Cooked Development run 2 | `Saved/Completion20260906/UserDevelopment2/Saved/MonolithCompletionRuntime.json`: 24 checks, zero failures, three hearing events. `runtime-development-2-exit.txt`: 0. Runtime log has no error/ensure, including the former async-loader tick-constructor ensure. |
| Cooked Shipping | `Saved/Completion20260906/UserShipping/Saved/MonolithCompletionRuntime.json`: 24 checks, zero failures, three hearing events. `runtime-shipping-exit.txt`: 0. |
| Development and Shipping package builds | `Saved/Completion20260906/package-development-2.log` and `package-shipping.log`: build successful; cook completed with zero errors and zero warnings. |
| `Monolith.Blueprint.Writes.Preflight` | Passed cleanly in run 4 (09:44:58 UTC). Checks no override/dirty state on invalid inherited writes, required root class, no mutation for invalid member/local/replicated types, actual SwitchInteger case pin reconstruction, protected node identity, Engine-package mutation refusal, no default save, and no package created for invalid struct fields. |
| `Monolith.Animation.Honesty.DeferredRules` | Passed cleanly in run 4 (09:44:56 UTC). Checks real comparison/boolean nodes and transition-result wiring, invalid-rule partial reporting, invalid native-layer parameter preflight, typed input-pose pin, and compiled linked-layer input pin. |
| Runtime migration references | Searched `Source`, `Scripts`, `Docs/specs`, and `Config`. Old `/Script/MonolithAI.*` and `/Script/MonolithGAS.*` references remain only in intentional runtime redirects and the legacy redirect probe. Production AI/GAS creation uses moved class C++ symbols, so no stale hardcoded production path required replacement. |

Fixtures use GUID-owned in-memory Blueprint packages and clear their dirty/asset-registration state on exit. This agent did not run the editor, build Unreal, save test assets, or alter RecycleCo. Root owns shared engine validation and final project cleanup.

## Deliberate scope limits

The general node property action refuses whole structs/containers, object traversal and non-editable fields; editable scalar leaves can be addressed through dotted struct paths. A save failure returns an error with `mutation_applied:true` and the changed node metadata; it does not pretend the requested save succeeded. Transient properties and non-writable packages are refused before mutation. It does not compile graphs automatically.

Compound transition rules use structured numeric comparisons folded through AND/OR. Arbitrary freeform expressions are outside the standalone and builder grammar.

This report verifies the listed editor authoring regressions and runtime probe checks. The 24-check packaged probe is scoped coverage, not exhaustive validation of every gameplay feature or optional engine integration. Root's unified validation report records the wider durability checks and final host cleanup.

## First engine-run findings and fixes

`Saved/Completion20260906/automation-2.log` showed two failing assertions. Both were retained and the behavior corrected:

- The new graph-property API returned GUID node IDs, but its reused legacy resolver accepted only object names. Added asset/graph-scoped GUID matching and duplicate-GUID rejection. The Engine-write regression additionally verifies the refusal reason is `path_not_writable`, preventing a missing-node error from satisfying the test accidentally.
- Typed native layer parameters existed in the compiled signature, but UE hides linked-layer custom parameter pins by default (`CustomPinProperties.bShowPin=false`). `add_linked_anim_layer` now exposes engine-discovered parameter pins and reconstructs them around compilation. The test uses a separate linked-layer request, removing irrelevant `compile`/`input_poses` keys from that action.

Both unchanged regression assertions passed in automation run 4. No warning/error events were recorded for either test in that run.

## GAS runtime investigation after build 5

The shared runtime probe distinguishes compiled binding data from manually created runtime extensions. `runtime-editor-game-3.log` showed zero generated-class extensions/rows in an uncooked `UnrealEditor -game` process; all five authored-binding checks failed. The manually constructed runtime extension passed, including a hosted `UWidgetComponent`, `SelfActor`, and valid soft class references paired with deliberately stale legacy strings.

Normal editor reload rebuilt the same asset with one runtime extension containing all three authored rows. In `editor-gas-diagnostic.log`, the PIE probe reported the initial value `0.25`, immediate delegate update to `0.75`, owner retry, smooth interpolation, and tick policy all passing. That diagnostic process later crashed during editor teardown; this is evidence of the individual assertions, not a clean end-to-end PIE run. Its audio assertions used benchmark mode, which disabled audio playback; the separate normal `-game` audio run passed.

Local UE 5.7 source supports an uncooked loading limitation: `Runtime/Projects/Private/ModuleDescriptor.cpp:724` only loads an `Editor` module while `GIsEditor`; Monolith's widget compiler extension lives in the `Editor` module `MonolithGAS`. `Runtime/CoreUObject/Private/Blueprint/BlueprintSupport.cpp:703` can regenerate an uncooked Blueprint during load, while `Editor/UMGEditor/Private/WidgetBlueprintCompiler.cpp:468` clears generated-class extensions before compiler callbacks rebuild them. Missing editor compiler glue during uncooked `-game` regeneration is therefore the supported explanation, although that exact call sequence was not traced. The generated-class `Extensions` array itself is persistent, and normal editor compiler callbacks correctly install the data.

No speculative persistence workaround or production compiler change was made. Final cooked Development and Shipping runs both passed the binding checks. The uncooked `-game` issue above is a diagnostic-only loading limitation and does not describe the verified packaged outcome.

### First cooked Development run and async-loading fix

`runtime-development.log` confirmed that cooked generated-class binding data survives: initial/delegate/retry/smoothed/tick checks and the hosted-widget soft-reference check all passed. It also exposed a handled ensure from the `FTickableGameObject` constructor when the async package loader constructed the runtime extension. This run was not clean (two unrelated Behavior Tree checks also failed).

The runtime extension now owns a separate tick helper created lazily by `Construct` on the game thread, rather than inheriting `FTickableGameObject` and registering during UObject construction. The helper holds a weak extension reference, forwards the same game delta, and preserves the existing per-widget game-world filter. Final widget destruction and extension `BeginDestroy` remove the helper and subscriptions. `Monolith.GAS.Runtime.AsyncExtensionConstruction` constructs the extension on a worker thread with GC protection and verifies safe cleanup; the old inheritance triggered the reported ensure on this path. Editor build 8 and automation run 5 verified the fix and regression. Normal PIE, cooked Development run 2, and Shipping subsequently passed all 24 checks with clean exits. Development run 2 confirmed the original async-loading ensure is absent. The earlier async and Behavior Tree failures are historical findings fixed before these final runs.
