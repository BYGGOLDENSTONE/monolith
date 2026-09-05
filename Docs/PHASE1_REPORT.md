# Phase 1 safety and honesty report

**Completed 2026-09-05. All thirteen numbered items are done and verified on UE 5.7.4.**

Work was performed in `D:/UnrealProjects/monolith` on `fix/phase1-safety-honesty`, continuing the Phase 0 base on `feat/multi-agent-reliability`. The thirteen numbered implementation commits below are in order. No merge or push was performed. `git worktree list` at completion reports only the main checkout. The disposable validation project's plugin junction points into this checkout; it is not a second repository copy.

Validation uses UE **5.7.4** (CL **51494982**, `D:/UE_5.7`) Development Editor and Python **3.12** on Windows. All UE tests run against `C:/Users/PC/AppData/Local/Temp/MonolithValidation57`, using NullRHI for the full suite plus a separate offscreen D3D12 Material.RoundTrip run, without touching the user's separate UnhallowedGround project/editor. One UE pipeline runs at a time, with `-MaxParallelActions=2` after the user's performance pause. Native Python contract tests were enabled using the rebuilt proxy; item 10 also enabled the rebuilt native query tool. The ten live-editor Python cases were explicitly skipped because no separate live MCP test endpoint was configured.

## Numbered work order

UE totals below include both clean and warning-bearing engine successes. Explicit self-skips are separately reported and are included in those successes; they must not be added to the total a second time. Every completed item has passing pre-commit and post-commit Python, lint, UE build and full `Monolith.` checks. Initial failures and corrected reruns are recorded separately below.

| Item | Status | Commit | Actual verification | User-visible behavior and scope findings |
|---|---|---|---|---|
| 1 | done | `f5cc746` | Pre/post Python: 92 total, 82 passed, 10 live skips; lint passed; UE build passed; final full filter **128 successes (120 clean + 8 warning), 13 explicit self-skips, 0 failed, 0 not run**. | Four SQLite fixture lifetimes corrected. Plugin-owned database close guards retain a failed-close handle and assert on outstanding statements. Missing source index makes CursorPagination.QueryMismatchRejection explicitly self-skip. No installed engine source was patched. |
| 2 | done | `3ea815a` | Pre/post Python: 92/82/10; lint/build passed; full **130 (122+8), 13 self-skips, 0 failed/not run**. Real Core.WritablePackagePath and Material.WriteSafety checks. | Central writable-root guard covers the cited 22 validators and shared/create/save paths. `/Game` is writable, `/Engine` and `/Script` are rejected, plugin mounts require explicit `WritablePluginContentRoots`. Interpretation recorded: indexing `AdditionalContentPaths` alone does not grant plugin write authorization. |
| 3 | done | `d1119e9` | Pre/post Python: 92/82/10; lint/build passed; full **136 (126+10), 13 self-skips, 0 failed/not run**. Six real SaveContract fixtures: Material, GAS, UI, Audio, Animation, AI. | Existing asset mutations use explicit `save:false` defaults and retain dirty packages; `save:true` persists. Breaking defaults are listed in CHANGELOG. Creator saves stay intact. The AI fixture proves map persistence, not nav-tile generation. Proprietary GBA implementation changes remain uncompiled. |
| 4 | done | `b23de69` | Pre/post Python: 92/82/10; lint/build passed; full **140 (130+10), 13 self-skips, 0 failed/not run**. Four ErrorHelpers fixtures plus expanded HTTP error-shape/evidence checks. | Typed NotFound/InvalidParam/PreconditionFailed/NotImplemented/EngineError/OptionalDepUnavailable helpers expose class, executed and retryable. Legacy errors gain class without invented execution evidence; caller data and scalar payloads are preserved. |
| 5 | done | `8e148d7` | Pre/post Python: 107 total, 97 passed, 10 live skips; lint/build passed; full **146 (133+13), 13 self-skips, 0 failed/not run**. Fifteen codemod parser tests and six new Automation cases for assets, Core, Blueprint/Animation, Material and Niagara suggestions. | Conservative codemod and hand review classify mechanical missing-param/lookup cases. Generic internal callsites decrease **5,718 to 4,171** at this item's frozen revisions; full per-module table below. Ambiguous post-mutation failures retain existing semantics. Suggestions use class-filtered AssetRegistry metadata without loading candidate assets. |
| 6 | done | `ecc3221` | Pre/post Python: 129 total, 119 passed, 10 live skips; lint/build passed; full **146 (133+13), 13 self-skips, 0 failed/not run**. Twenty-two schema-checker regression cases. Baseline **236 violations**, final **1,578 registrations, zero errors**. | CI and repository lint enforce literal direct/helper input declarations and non-null schemas. Fixed 209 missing action/key pairs on 88 actions plus 27 missing/null schemas. The full action table appears below. Existing parameter aliases and nested descriptor fields were not duplicated as phantom top-level inputs. |
| 7 | done | `1728616` | Fresh native proxy pre/post; Python: 135 total, 125 passed, 10 live skips; lint/build passed; full **146 (133+13), 13 self-skips, 0 failed/not run** after corrected fixture rerun. | Both proxies generate request UUIDs and client identities, forwarded to HTTP and JSONL logs. Server response metadata/error data and one per-request log line expose correlation and the module instance UUID. Protocol, response and proxy header/log parity tests execute. |
| 8 | done | `4254860` | Fresh native proxy pre/post; Python: 151 total, 141 passed, 10 live skips; lint/build passed; full **146 (133+13), 13 self-skips, 0 failed/not run**. Refused/hanging fixtures plus EOF ownership/drain/deadline cases in both flavors. | Proven failures before request transmission return `not_sent`, executed:false, retryable:true. Ambiguous/transmitted failures remain `unknown_outcome`. Native connect cap is five seconds or the shorter request timeout. EOF best-effort releases only owned observed leases within one total two-second deadline; no release confirmation or rollback is claimed. |
| 9 | done | `88530e0` | Fresh native proxy pre/post; Python: 167 total, 157 passed, 10 live skips; lint/build passed; full **146 (133+13), 13 self-skips, 0 failed/not run**. Windows symlink cleanup tests actually ran. | Existing native logger extended to Python outcome/client/UUID/millisecond parity, bounded rotation (default 16 MiB), and 14-day regular-file retention. Directories/symlinks are preserved; logging failure does not alter action results. `cancelled` is reserved for confirmed cancellation, not inferred from EOF/timeouts. |
| 10 | done | `c9f9633` | Fresh native query pre/post and source-hash freshness **0323c7efd940f355**; Python: 178 total, 168 passed, 10 live skips, including 11 offline routing fixtures; lint: 1,579 registrations/zero errors; UE build/full pre/post **147 (133+14), 13 self-skips, 0 failed/not run**. Real AsyncMining fixture completed with an expected git-failure warning. | Unmined risk queries return immediate PreconditionFailed suggesting risk.mine. Explicit mine owns one background task; status reports idle/running/done/failed and progress. Worker SQLite/git stages commit atomically into durable Risk.db beside configured source DB; failure/cancellation rolls back and preserves the prior disk snapshot. The game thread opens and publishes its own read handle after the worker closes its database. Both offline readers use Risk.db with absent-file legacy fallback. No job framework was added. |
| 11 | done | `b2f3bc3` | Python pre/post: 178/168/10; lint: 1,579 registrations/zero errors; normal final pre/post build/full **153 (139+14), 13 self-skips, 0 failed/not run**. Separate forced optional-off build/full **151 (136+15), 13 self-skips, 0 failed/not run**; normal configuration restored before commit. | LogicDriver/ComboGraph/CommonUI/MetaSound actions remain discoverable with structured dependency errors when compiled unavailable. Discovery emits per-namespace availability plus mixed UI/audio optional_dependencies; base UMG/SoundCue stays usable. Explicit requests for absent Blueprint Assist report its dependency error while default formatting fallback remains available. Actual absent CommonUI/MetaSound and base namespace behavior were executed, not merely inspected. |
| 12 | done | `c69de93` | Pre Python: 178/168/10; lint: 1,578 registrations/zero errors. Initial pre build/full **157 (143+14), 14 self-skips, 0 failed/not run**: collision fixture self-skipped because GeometryScripting was disabled. After explicitly enabling GeometryScripting and Metasound in the disposable host, forced build passed (152 actions, 116.7s); enabled pre full **157 (142+15), 13 self-skips, 0 failed/not run**. All four new Honesty tests passed cleanly. Post build/full passed with the same 157 (142+15), 13 self-skips and zero failed/not run; post Python 178/168/10 and lint 1,578/zero also passed. | Removed mesh.integration_hooks_stub from registration and moved its design guidance into the spec. Co-op scoring returns NotImplemented without placeholder scores. Mesh collision action returns PreconditionFailed directing callers to mesh.save_handle without discarding computed shapes. Animation state-machine result exposes partial and deferred_rules. The enabled-host rerun also compiled/executed MetaSound's available branch; it passed with existing parameter/deprecation warnings. |
| 13 | done | `9586eac` | Pre/post Python: **178 total, 168 passed, 10 live skips**; lint: **1,578 registrations, zero errors**; UE build/full pre/post **161 (143 clean + 18 warning), 13 self-skips, 0 failed/not run**. Separate pre/post D3D12 Material.RoundTrip: **1 clean success, 0 self-skips, 0 failed/not run**. | Four required RoundTrip fixtures exercise real creation, mutation, action readback, compilation and save semantics. Niagara checks actual CPU VM executables and gains explicit checked save:true while preserving its dirty-only default. GAS/Audio verify real package unload before disk reload; Audio reconstructs its cleared runtime root from the editor graph. Niagara/Material verify actual disk-byte persistence with in-memory property readback. All four assert typo suggestions and delete GUID-owned assets. Selected GAS/Audio missing loaders now use typed AssetNotFound. |

## Item 5 frozen error counts

Baseline is `b23de69`, immediately before item 5. Final is the frozen item-5 implementation committed as `8e148d7`, not the moving current checkout. Sources: `Saved/phase1-item5-error-baseline.json` and `Saved/phase1-05-frozen-counts.json`; the committed `Scripts/codemod_error_classes.py` documents/reproduces the counting rules.

Counts cover balanced, fully-qualified `FMonolithActionResult::Error` calls in production Source `.cpp/.h`: omitted code, explicit `-32603`, or `FMonolithJsonUtils::ErrInternalError`. Tests directories/test filenames and helper definitions are excluded; inline Automation blocks in production files remain included. Comments/literals are masked. Dynamic or other symbolic codes are not assumed to be internal. Typed EngineError callsites are zero at both frozen boundaries, so none of the reduction is merely renaming internal-error calls.

| Module | Before item 5 | After item 5 | Reduction |
|---|---:|---:|---:|
| MonolithAI | 689 | 551 | 138 |
| MonolithAnimation | 978 | 648 | 330 |
| MonolithAudio | 300 | 286 | 14 |
| MonolithAudioRuntime | 0 | 0 | 0 |
| MonolithBABridge | 0 | 0 | 0 |
| MonolithBlueprint | 642 | 339 | 303 |
| MonolithComboGraph | 65 | 48 | 17 |
| MonolithConfig | 11 | 11 | 0 |
| MonolithCore | 11 | 6 | 5 |
| MonolithEditor | 159 | 152 | 7 |
| MonolithGAS | 375 | 316 | 59 |
| MonolithIndex | 19 | 17 | 2 |
| MonolithLevelSequence | 40 | 40 | 0 |
| MonolithLogicDriver | 266 | 176 | 90 |
| MonolithMaterial | 182 | 114 | 68 |
| MonolithMesh | 1,012 | 755 | 257 |
| MonolithNiagara | 512 | 299 | 213 |
| MonolithReflectionIntel | 52 | 52 | 0 |
| MonolithSource | 43 | 42 | 1 |
| MonolithUI | 362 | 319 | 43 |
| **Total** | **5,718** | **4,171** | **1,547** |

The reduction is **1,547 callsites**. This is a source classification count, not a runtime error rate or proof that every generic error is incorrect. The five largest requested domains gained real suggestion tests. Candidate lookup is metadata-only, class-filtered, capped at 256 package names with a bounded nearby-folder fallback; absent suitable metadata can validly yield no suggestions.

## Item 6 complete schema violations fixed

The authoritative final-coverage baseline at `8e148d7` found **209 missing key pairs on 88 actions** plus **27 missing or null schema registrations**, for **236 errors**. Categories overlap: an action without a schema can also read undeclared keys. Final coverage at `ecc3221` reports **1,578 registrations and zero errors**. Item 11 had 1,579 registrations after risk.mine was added in item 10. Item 12 removed the integration stub, returning the final surface to **1,578 registrations and zero errors**.

The checker consumes reviewed helper forwarding metadata (56 entries at item 6), which adds helper inputs and never exempts direct reads. It checks all compile-gated implementations and fails on unresolved supported syntax. It does not infer arbitrary dynamic keys or general C++ data flow. `material.end_transaction` already had a non-null explicit empty schema, `blueprint.set_function_thread_safe.name` was already an alias, and `blueprint.add_nodes_bulk.position` belonged to an already-documented nested descriptor; these cited examples were already correct.

The following complete table is copied from `Saved/phase1-schema-report-table.md`, generated from `Saved/phase6_schema_probe/baseline_finalcoverage.json` and `current_finalcoverage.json`. No rows are omitted.

| Action | Schema fixes |
| --- | --- |
| `ai.duplicate_blackboard` | `overwrite` |
| `animation.add_save_cached_pose` | `state_name` |
| `blueprint.resolve_node` | `event_name`, `macro_blueprint`, `macro_name` |
| `editor.capture_scene_preview` | `background_color`, `uv_tiling` |
| `editor.get_recent_logs` | `max` |
| `material.connect_expressions` | `from_pin`, `to_pin` |
| `material.render_preview` | `background_color` |
| `mesh.create_building_from_grid` | `snap_to_floor` |
| `mesh.create_city_block` | `facade_style` |
| `mesh.generate_floor_plan` | `exclude_templates` |
| `monolith.reindex` | `force`, RegisterAction has no schema |
| `monolith.status` | RegisterAction has no schema |
| `niagara.add_emitter` | `emitter_path`, `template_path`, `template` |
| `niagara.add_renderer` | `renderer_class`, `renderer_type` |
| `niagara.add_user_parameter` | `default_value`, `parameter_name` |
| `niagara.duplicate_emitter` | `emitter` |
| `niagara.get_emitter_property` | `property_name` |
| `niagara.get_module_inputs` | `module_name`, `module` |
| `niagara.get_static_switch_value` | `module_name` |
| `niagara.get_system_property` | `property_name` |
| `niagara.request_compile` | `force`, `synchronous` |
| `niagara.set_curve_value` | `input_name`, `module_name`, `module` |
| `niagara.set_emitter_property` | `property_name` |
| `niagara.set_module_input_binding` | `input_name`, `module_name`, `module` |
| `niagara.set_module_input_di` | `input_name`, `module_name`, `module` |
| `niagara.set_module_input_value` | `input_name`, `module_name`, `module` |
| `niagara.set_renderer_mesh` | `mesh_path` |
| `niagara.set_renderer_property` | `property_name` |
| `niagara.set_static_switch_value` | `input_name`, `module_name` |
| `niagara.set_system_property` | `property_name` |
| `project.find_by_type` | `asset_class` |
| `project.find_references` | `package_path` |
| `project.get_asset_details` | `package_path` |
| `project.get_saved_asset_state` | `package_path` |
| `source.get_class_hierarchy` | `class_name` |
| `ui.add_animation_event_track` | `animation_name`, `asset_path`, `events`, RegisterAction has no schema |
| `ui.add_bezier_eased_segment` | `animation_name`, `asset_path`, `bezier`, `end_time`, `from_value`, `property`, `start_time`, `to_value`, `widget_name`, RegisterAction has no schema |
| `ui.add_widget_variable` | `asset_path` |
| `ui.apply_box_shadow` | `asset_path`, `compile`, `shadow_material_path`, `shadow_mid_destination`, `shadow`, `shadows`, `target_size`, `widget_name`, RegisterAction has no schema |
| `ui.apply_effect_surface_preset` | `asset_path`, `compile`, `parent_material`, `preset_name`, `widget_name`, RegisterAction has no schema |
| `ui.apply_style_to_widget` | `asset_path` |
| `ui.apply_token_binding` | `asset_path` |
| `ui.audit_commonui_widget` | `asset_path` |
| `ui.audit_focus_chain` | `asset_path` |
| `ui.bake_spring_animation` | `animation_name`, `asset_path`, `compile_once`, `damping`, `duration`, `fps`, `from_value`, `mass`, `property`, `stiffness`, `to_value`, `widget_name`, RegisterAction has no schema |
| `ui.bind_animation_to_event` | `animation_event`, `animation_name`, `asset_path`, `widget_event`, RegisterAction has no schema |
| `ui.bind_common_action_widget` | `asset_path` |
| `ui.configure_activatable` | `asset_path` |
| `ui.configure_animated_switcher` | `asset_path` |
| `ui.configure_common_border` | `asset_path` |
| `ui.configure_common_button` | `asset_path` |
| `ui.configure_common_text` | `asset_path` |
| `ui.configure_modal_overlay` | `asset_path` |
| `ui.configure_numeric_text` | `asset_path` |
| `ui.configure_rotator` | `asset_path` |
| `ui.convert_border_to_common` | `asset_path` |
| `ui.convert_button_to_common` | `asset_path` |
| `ui.convert_textblock_to_common` | `asset_path` |
| `ui.create_activatable_stack` | `asset_path` |
| `ui.create_activatable_switcher` | `asset_path` |
| `ui.create_animation_v2` | `animation_name`, `asset_path`, `compile_once`, `duration_sec`, `tracks`, RegisterAction has no schema |
| `ui.create_bound_action_bar` | `asset_path` |
| `ui.create_gradient_mid_from_spec` | `destination`, `parent_material`, `save`, `spec`, RegisterAction has no schema |
| `ui.create_hardware_visibility_border` | `asset_path` |
| `ui.create_lazy_image` | `asset_path` |
| `ui.create_load_guard` | `asset_path` |
| `ui.create_widget_carousel` | `asset_path` |
| `ui.dump_action_router_state` | RegisterAction has null schema |
| `ui.dump_widget_navigation` | `asset_path` |
| `ui.get_active_input_type` | RegisterAction has null schema |
| `ui.get_focus_path` | RegisterAction has null schema |
| `ui.hot_reload_styles` | RegisterAction has null schema |
| `ui.import_font_family` | `destination`, `faces`, `family_name`, `hinting`, `loading_policy`, `save`, RegisterAction has no schema |
| `ui.import_texture_from_bytes` | `bytes_b64`, `destination`, `format_hint`, `save`, `settings`, RegisterAction has no schema |
| `ui.list_platform_input_tables` | RegisterAction has null schema |
| `ui.rename_widget` | `asset_path` |
| `ui.reparent_widget_root` | `asset_path` |
| `ui.set_action_bar_button_class` | `asset_path` |
| `ui.set_activatable_transition` | `asset_path` |
| `ui.set_effect_surface_backdropBlur` | `asset_path`, `compile`, `strength`, `widget_name`, RegisterAction has no schema |
| `ui.set_effect_surface_border` | `asset_path`, `color`, `compile`, `glow_color`, `glow`, `offset`, `widget_name`, `width`, RegisterAction has no schema |
| `ui.set_effect_surface_corners` | `asset_path`, `compile`, `corner_radii`, `smoothness`, `widget_name`, RegisterAction has no schema |
| `ui.set_effect_surface_dropShadow` | `asset_path`, `compile`, `layers`, `widget_name`, RegisterAction has no schema |
| `ui.set_effect_surface_fill` | `angle`, `asset_path`, `color`, `compile`, `mode`, `radial_center`, `stops`, `widget_name`, RegisterAction has no schema |
| `ui.set_effect_surface_filter` | `asset_path`, `brightness`, `compile`, `contrast`, `saturation`, `widget_name`, RegisterAction has no schema |
| `ui.set_effect_surface_glow` | `asset_path`, `color`, `compile`, `inner_outer_mix`, `intensity`, `radius`, `widget_name`, RegisterAction has no schema |
| `ui.set_effect_surface_innerShadow` | `asset_path`, `compile`, `layers`, `widget_name`, RegisterAction has no schema |
| `ui.set_effect_surface_insetHighlight` | `asset_path`, `blur`, `color`, `compile`, `edge_mask`, `intensity`, `offset`, `widget_name`, RegisterAction has no schema |
| `ui.set_initial_focus_target` | `asset_path` |
| `ui.set_rounded_corners` | `asset_path`, `compile`, `corner_radii`, `fill_color`, `outline_color`, `outline_width`, `widget_name`, RegisterAction has no schema |
| `ui.set_widget_navigation` | `asset_path` |
| `ui.set_widget_navigation_bulk` | `asset_path` |
| `ui.set_widget_property` | `property_value` |
| `ui.setup_common_list_view` | `asset_path` |

## Automation accounting and remaining verification limits

| Checkpoint | Clean successes | Warning successes | Total successes | Explicit self-skips (included in successes) | Failed | Not run |
|---|---:|---:|---:|---:|---:|---:|
| Item 1 post | 120 | 8 | 128 | 13 | 0 | 0 |
| Item 10 post | 133 | 14 | 147 | 13 | 0 | 0 |
| Item 11 normal post | 139 | 14 | 153 | 13 | 0 | 0 |
| Item 11 optional-off | 136 | 15 | 151 | 13 | 0 | 0 |
| Item 12 initial pre (GeometryScripting disabled) | 143 | 14 | 157 | 14 | 0 | 0 |
| Item 12 enabled pre | 142 | 15 | 157 | 13 | 0 | 0 |
| Item 12 enabled post | 142 | 15 | 157 | 13 | 0 | 0 |
| Final after item 13 | 143 | 18 | 161 | 13 | 0 | 0 |
| Item 13 Material D3D12 pre/post (focused) | 1 | 0 | 1 | 0 | 0 | 0 |

The repeated 13 explicit self-skips are four source-index-dependent CursorPagination cases and nine NullRHI rendering/preview cases. Engine warnings and explicit self-skips are different classifications. Direct inspection of item 1's report confirms **10 self-skips with Info events and zero warnings** (the nine preview cases plus QueryMismatchRejection), and **three self-skips with Warning events** (HardCap, PageBoundaryNoNextCursor, TotalEstimatePageZeroOnly). Thus its 120 clean + 8 warning successes legitimately include 13 self-skips: 110 clean + 5 warning successes exercised their bodies, while 10 clean + 3 warning successes self-skipped. Item 11 optional-off contains two fewer discovered tests because the existing CommonUI token-binding fixtures require the compiled CommonUI implementation; the new absence tests execute in that configuration.

The item-11 restored normal build was specifically **WITH_COMMONUI=1, WITH_METASOUND=0**. Generated `Intermediate/Build/Win64/x64/UnrealEditor/Development/MonolithUI/Definitions.MonolithUI.h:18` and the corresponding `MonolithAudio/Definitions.MonolithAudio.h:18` defined those values; each module's `Monolith*OptionalAvailabilityTest.cpp.obj.rsp` forcibly includes its generated definitions header. The actual item-11-post editor log confirms `MonolithAudio: Loaded (98 actions, MetaSound=not compiled)`. Its JSON report records zero-warning successes for Monolith.UI.OptionalAvailability, Monolith.Audio.OptionalAvailability and both CommonUI-only TokenBinding fixtures. Therefore that normal run executed the CommonUI **enabled** branch and MetaSound **absent** branch, while the forced optional-off run exercised CommonUI absence too.

During item 12, the disposable host explicitly enabled GeometryScripting and Metasound and the forced rebuild changed **WITH_METASOUND to 1**, while CommonUI remained 1. This is confirmed by the regenerated definitions and `Saved/Phase1Automation-12-enabled-pre/index.json`. Monolith.Audio.OptionalAvailability then exercised the enabled MetaSound enumeration handlers and passed with five warnings from existing missing optional string reads/deprecated registry API usage. All four new item-12 Honesty tests passed with zero warnings, including CollisionRequiresSave, which had self-skipped in the initial GeometryScripting-disabled pre-run. These supplemental results close the earlier enabled-MetaSound compilation/test gap; they do not establish asset-authoring or playback coverage for MetaSound.

- UE 5.7.4 was built and tested. UE 5.8, packaging/cooking, rendered visual output, runtime Niagara simulation and actual audio playback were not verified. The additional D3D12 run proves Material shader-map compilation, not visible rendering quality.
- The proprietary Logic Driver enabled implementation and GBA code excluded by the available configuration remain uncompiled/unexecuted. Absence behavior is tested; this must not be described as proprietary integration validation. CommonUI is exercised both compiled in and forced out. MetaSound absence is exercised in item 11 and its enabled enumeration branch in the item-12 enabled-host rerun, with the warning limitation above.
- POSIX-specific log rotation/cleanup and TLS EOF-release branches were not exercised on this Windows run. Windows symlink tests did execute. The ten Python live-MCP tests are skipped throughout this phase; native transport fixtures are real subprocess/socket tests, not live-editor multi-client tests.
- Risk complexity/configuration capture still occurs on the game thread under the source database owner's lock. UE's single-open VFS prevents a second same-process source handle; only immutable rows/settings are passed to the worker. No project-file row cap was introduced. Large-project capture latency is exposed as telemetry, not benchmarked or promised bounded by these fixtures. Git and conditional-gate files are observed during the mining pass, not at one atomic filesystem snapshot instant.
- Risk snapshot publication is a game-thread read-handle handover after worker close, not a shared writable handle or generic job framework. Failed refresh leaves the prior durable file, while current live queries remain unavailable until a valid snapshot is published. Legacy contents are revalidated across source-handle reopen. The AsyncMining fixture checks real git/cochange/gates, frozen inputs, git and late SQL failure rollback bytes, cache identity/config/code, restart readback and shutdown cancellation.
- Item 3 retains creator save behavior and changes only applicable existing-asset defaults. The cited CommonUI helper's existing save:false caller was already correct; many cited nearby saves were creator paths. Audio's cited CreateEmptySoundCue was also a creator; actual existing node/perception mutation paths were reviewed instead. AI verification proves a saved test map, not generated nav tiles.
- Item 13's NullRHI Material fixture executes create/mutate/read/save but explicitly reports one unverified shader-output subcheck. The separate pre/post D3D12 runs execute that same fixture with mandatory resource, zero compiler errors, non-null shader map and complete compilation assertions, closing that specific gap. Niagara CPU VM compilation does execute under NullRHI. GAS/Audio reload evidence is distinct from Niagara/Material byte comparisons; no reload is claimed for the latter. All four new tests actually execute, so the full-suite self-skip count remains 13. Intentional missing-asset loads account for warnings in the new GAS/Audio/Niagara tests; their exact expected error diagnostics are scoped to deliberate typos.
- Item 12's state-machine fixture proves graph authoring and Blueprint compilation with four deferred-rule branches, not runtime transitions through the disconnected builder-created machines. Its collision fixture proves an actual box BodySetup and nonempty asset file through save_handle; it does not claim a clean package or reload.
- No generic job framework, per-asset locks, MCP resources, auth, additional repository worktree, merge or push was introduced.

## Corrected failed attempts

Successful final gates above follow actual fixes and reruns. Item 1 initially had three Automation failures involving the negative decision fixture and expected-ensure capture; all were corrected before the full passing gate. Item 3 initially failed compilation on an Audio C4456 shadowing error. Item 6 initially missed the CoreTools parameter-schema include. Item 7 initially failed HTTP fixture assumptions about case-insensitive header replacement and log capture; the fixtures were corrected and the full filter rerun. Item 8 initially failed the native build on a JSON initializer brace, then built successfully. Item 10's original build.bat attempt failed during VS environment initialization before native query compilation; a CMake VS2019 build of the actual checkout sources succeeded and passed the native fixtures/freshness guard. Item 11 initially missed unconditional Json link dependencies for LogicDriver/ComboGraph; after correction, full normal, optional-off, restored-normal and post-commit gates passed. Item 13 initially failed to compile its GAS test because TestNotNull requires a raw pointer rather than TSubclassOf; the fixture now passes GeneratedClass.Get(). The next full run passed all functional assertions but failed on the deliberate Niagara typo's unaccounted error log; an exact one-occurrence, GUID-path expected error fixed that test contract. The local Python wrapper initially treated unittest's normal stderr as a terminating PowerShell error; its corrected wrapper uses native exit codes, and the entire suite was rerun successfully. Initial reports are retained in Saved rather than represented as successful runs.

## Exact commands and local evidence

All commands below ran from `D:/UnrealProjects/monolith`. Saved logs/reports, disposable host files and compiled binaries remain local/ignored. Remote CI was not run.

Python/native proxy verification uses these exact commands (the proxy rebuild applies initially and after changed proxy sources in items 7-9):

```powershell
powershell -File Scripts/build_proxy.ps1
$env:PATH = 'C:/Users/PC/AppData/Local/Programs/Python/Python312;' + $env:PATH
$env:MONOLITH_TEST_NATIVE_PROXY = (Resolve-Path Binaries/monolith_proxy.exe).Path
python -m unittest discover -s Scripts/tests -v
python Scripts/check_repo_lint.py
```

Item 10 additionally rebuilt and enabled the native offline query executable:

```powershell
powershell -File Saved/build_phase1_query.ps1
python Scripts/check_offline_exe_fresh.py
$env:MONOLITH_TEST_NATIVE_QUERY = (Resolve-Path Binaries/monolith_query.exe).Path
python -m unittest discover -s Scripts/tests -v
python Scripts/check_repo_lint.py
```

The local query helper's actual build commands are:

```powershell
cmake -S "D:/UnrealProjects/monolith/Saved/Phase1QueryProject" -B "D:/UnrealProjects/monolith/Saved/QueryBuild" -A x64
cmake --build "D:/UnrealProjects/monolith/Saved/QueryBuild" --config Release --parallel 2
Copy-Item -LiteralPath "D:/UnrealProjects/monolith/Saved/QueryBuild/Release/monolith_query.exe" -Destination "D:/UnrealProjects/monolith/Binaries/monolith_query.exe" -Force
```

The complete post-commit UE gate invocations were:

```powershell
powershell -File Saved/phase1_verify_ue.ps1 -Fix 01-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 02-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 03-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 04-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 05-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 06-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 07-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 08-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 09-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 10-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 11-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 12-post -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 13-post -Filter Monolith.
```

Item 11 additionally compiled and executed the absent optional implementation configuration, then restored normal:

```powershell
powershell -File Saved/phase1_verify_optional.ps1 -Fix 11-absent -OptionalOff 1
powershell -File Saved/phase1_verify_optional.ps1 -Fix 11-restore -OptionalOff 0
powershell -File Saved/phase1_verify_ue.ps1 -Fix 11-pre-final -Filter Monolith.
```

`phase1_verify_optional.ps1` sets `MONOLITH_RELEASE_BUILD` to the supplied 1/0, runs the same build with `-gather` to force rules reevaluation, then invokes the normal build/full-filter helper. Its subprocess environment does not alter the user's project settings.

The standard helper expands to this Development Editor build and editor invocation (shown with actual item-13-post paths):

```powershell
& 'D:/UE_5.7/Engine/Build/BatchFiles/Build.bat' MonolithValidation57Editor Win64 Development '-Project=C:/Users/PC/AppData/Local/Temp/MonolithValidation57/MonolithValidation57.uproject' -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=2
& 'D:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/PC/AppData/Local/Temp/MonolithValidation57/MonolithValidation57.uproject' -unattended -nop4 -NullRHI -nosplash -nosound -NoLiveCoding '-ExecCmds=Automation RunTests Monolith.' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=D:/UnrealProjects/monolith/Saved/Phase1Automation-13-post' '-abslog=D:/UnrealProjects/monolith/Saved/phase1-13-post-automation.log'
git worktree list
```

Passing report evidence is `Saved/Phase1Automation-01-post/index.json` through `13-post/index.json`; item 11 also has `11-absent`, `11-restore` and `11-pre-final`. Item 12 has `12-enabled-pre`; item 13 has `13-pre-final` and `13-material-rhi-pre/post`. Python/build/lint output uses matching `Saved/phase1-<tag>-*.log` names. Item-5 frozen JSONs and item-6 final-coverage JSONs/table are named above. Earlier failed/retry tags remain in the same directory.

For the item-12 enabled implementation checks, the disposable `.uproject` plugin list was set to Monolith, GeometryScripting and Metasound, all Enabled:true. No production project was edited. The final host keeps that configuration. These additional exact invocations ran:

```powershell
powershell -File Saved/phase1_verify_ue.ps1 -Fix 12-pre -Filter Monolith.
powershell -File Saved/phase1_verify_optional.ps1 -Fix 12-enabled-pre -OptionalOff 0
powershell -File Saved/phase1_verify_ue.ps1 -Fix 13-pre -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 13-pre-retry -Filter Monolith.
powershell -File Saved/phase1_verify_ue.ps1 -Fix 13-pre-final -Filter Monolith.
powershell -File Saved/phase1_verify_python.ps1 -Fix 13-pre-retry
powershell -File Saved/phase1_verify_python.ps1 -Fix 13-post
powershell -File Saved/phase1_verify_material_rhi.ps1 -Fix 13-material-rhi-pre
powershell -File Saved/phase1_verify_material_rhi.ps1 -Fix 13-material-rhi-post
```

The Python helper sets the Python 3.12 PATH plus both MONOLITH_TEST_NATIVE_PROXY and MONOLITH_TEST_NATIVE_QUERY to the main checkout's Binaries executables, then runs the full unittest and repository-lint commands shown above, capturing and checking each exit code. It does not omit native tests. The RHI helper expands to this command (pre uses the corresponding `-pre` output paths), checks the editor exit code and requires exactly one successful test with zero failed/notRun:

```powershell
& 'D:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/PC/AppData/Local/Temp/MonolithValidation57/MonolithValidation57.uproject' -unattended -nop4 -RenderOffscreen -noshaderworker -nosplash -nosound -NoLiveCoding '-ExecCmds=Automation RunTests Monolith.Material.RoundTrip' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=D:/UnrealProjects/monolith/Saved/Phase1Automation-13-material-rhi-post' '-abslog=D:/UnrealProjects/monolith/Saved/phase1-13-material-rhi-post-automation.log'
git worktree list
```

`-noshaderworker` keeps this focused check to one in-process shader compiler. The actual selected RHI was D3D12. Final worktree inventory contains only `D:/UnrealProjects/monolith` on `fix/phase1-safety-honesty`; no merge or push was performed.

```powershell
git status --short
git worktree list
```
