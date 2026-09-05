# Phase 1 work order (for an AI coding agent)

Copy everything below the line into the agent's prompt. Phase 0 (`Docs/PHASE0_FIX_PROMPT.md`, results in `Docs/PHASE0_REPORT.md`) is merged into `feat/multi-agent-reliability`. Phase 1 is the "safety and honesty" phase from `Docs/IMPROVEMENT_ANALYSIS.md` section 3, plus the leftovers Phase 0 surfaced.

---

You are working in the Monolith repository, an Unreal Engine 5.7 editor plugin that exposes ~1,400 editor actions over MCP through namespace-dispatch tools. Read `README.md`, `Docs/MULTI_AGENT.md`, `Docs/AUDIT_UE57.md`, `CONTRIBUTING.md`, `Docs/IMPROVEMENT_ANALYSIS.md` (sections 2 and 3), `Docs/PHASE0_FIX_PROMPT.md` and `Docs/PHASE0_REPORT.md` before changing anything. Phase 0 established the conventions you must keep: `FMonolithJsonUtils::Err*` constants, `error.data.executed`, `ErrNotImplemented` with `reason/implemented/part`, the repository lint in `Scripts/check_repo_lint.py`, and `Scripts/check_skill_actions.py`.

## Ground rules

- Branch `fix/phase1-safety-honesty` off `feat/multi-agent-reliability`. Never touch `master`. Do not push; the reviewer pushes.
- One commit per numbered item, in order. Subject `fix(<area>): <what>` or `feat(<area>): <what>`; body says why and names the test that proves it.
- Verification tiers, every commit:
  - `python -m unittest discover -s Scripts/tests -v` with `MONOLITH_TEST_NATIVE_PROXY` pointing at a freshly built `Binaries/monolith_proxy.exe` (`powershell -File Scripts/build_proxy.ps1`) whenever a proxy changes.
  - `python Scripts/check_repo_lint.py`.
  - Plugin build against UE 5.7 (Development Editor) for any C++ change. Use the disposable validation project pattern from `Docs/PHASE0_REPORT.md`; never a production project.
  - UE Automation: after item 1 the full `Monolith.` filter must be green; run it at the end of every later C++ item and report counts. Until item 1 lands, run the focused filter from the Phase 0 report.
  - Never claim a test ran if it did not. Mark `unverified: <reason>` in the report.
- Scope discipline: no job framework, no per-asset locks, no MCP resources, no recipes namespace, no auth. Those are Phase 3. If an item needs one of them, stop that item, note it, continue.
- Behaviour changes that alter an action's default (item 3) must be listed in `CHANGELOG.md` under `## Unreleased` with a `**Breaking:**` prefix.
- Keep `.ps1` ASCII-only, use `LogMonolith`, no whitespace churn. Read the cited code before editing; line numbers may have drifted. If a cited behaviour no longer exists, skip and say why.
- Work in the main checkout. If you create a `git worktree` (or any copy of the repository) for an isolated build or test, remove it with `git worktree remove` as soon as that item is committed, and never leave one under `Saved/`, the system temp directory, or next to the repository. Phase 0 left five behind. `git worktree list` must show only the main checkout in your final report.

## Items

### A. Unblock the full test suite

**1. Fix the SQLite test-fixture lifetime crash so the full `Monolith.` filter can run.**
`Source/MonolithReflectionIntel/Private/Tests/CppReflectQueryTests.cpp` keeps prepared statements (`Stmt`, `LowerStmt`) alive while calling `Db.Close()`; the database destructor then crashes the editor and 46 tests never start. The Phase 0 report names three more instances of the same pattern: `Monolith.ReflectionIntel.Decision.HeuristicAccuracy`, `Monolith.ReflectionIntel.Decision.StalenessFlag`, `Monolith.ReflectionIntel.Risk.HotspotScoreFormula`. Finalize statements before close (or scope them so they are destroyed first) in all four, and add a `check`/`ensure` in the SQLite wrapper's `Close()` that fires when statements are still open, so this cannot silently regress. Then run the full `Monolith.` filter. `Monolith.CursorPagination.QueryMismatchRejection` fails when no source index exists; make that test self-skip with an explicit `AddInfo` when the index is absent instead of failing. Target: full filter reports `failed=0, notRun=0`, with the number of self-skips listed.

### B. Write safety

**2. Central writable-path guard.**
`Source/MonolithCore` has `ValidatePackagePath` (`MonolithPackagePathValidator.h`) that only validates shape; its test asserts `/Engine/` is writable. Add `MonolithCore::EnsureWritablePackagePath(const FString& PackagePath, FString& OutError)` that accepts `/Game/...`, paths under `UMonolithSettings::AdditionalContentPaths`, and `/Game/Tests/Monolith/...`, and rejects `/Engine/`, `/Script/`, and any plugin mount not listed in a new `UMonolithSettings::WritablePluginContentRoots` array (default empty). Wire it into every write path that saves or creates an asset: the 22 `ValidatePackagePath` call sites, the create/save helpers in Material, Niagara, GAS, Audio, UI (`MonolithCommonUIHelpers.cpp` `CompileAndSaveWidgetBlueprint`), Animation, Mesh and Blueprint. Rejections use `ErrInvalidParams` with `error.data.reason = "path_not_writable"` and the accepted roots. Automation test: an attempt to write under `/Engine/EngineMaterials/` via `material.set_material_property` is rejected before any mutation, and a `/Game/Tests/Monolith/` path passes.

**3. Uniform `save` contract (default false) for existing-asset mutations.**
Blueprint, Mesh and Editor already take `save` (default false). Bring these in line: `material.set_material_property` and `batch_set_material_property` (`MonolithMaterialActions.cpp` around lines 3010 and 7570 save unconditionally), GAS attribute/effect/tag/UI-binding mutations (`MonolithGASAttributeActions.cpp` ~683/864, `MonolithGASEffectActions.cpp` ~121, `MonolithGASTagActions.cpp` ~337, `MonolithGASUIBindingActions.cpp` ~462), CommonUI activatable/button/input/list writes that go through `CompileAndSaveWidgetBlueprint`, `MonolithAudioSoundCueActions.cpp` ~591, `MonolithAINavigationActions.cpp` ~2115, and Animation `add_compatible_skeleton` / `remove_compatible_skeleton` (default currently true). Rule: actions that mutate an existing asset take `save` (boolean, default false) and leave the package dirty otherwise; actions that create a new asset may still save unconditionally. Every changed action's schema gets `.Optional("save", "boolean", ..., "false")`. Update the affected `Docs/specs/SPEC_*.md` tables and `Docs/API_REFERENCE.md`. Automation test per module: mutation without `save` leaves the package dirty and the file unchanged on disk; with `save=true` the file updates.

### C. Error taxonomy

**4. Error helpers and a structured error class.**
Add to `FMonolithActionResult` (in `MonolithToolRegistry.h`) static constructors `NotFound(kind, needle, candidates)`, `InvalidParam(name, why)`, `PreconditionFailed(what, next_action)`, `NotImplemented(part)`, `EngineError(message)` and `OptionalDepUnavailable(dep)`. Each sets the matching `Err*` code and `error.data{class, executed:false, retryable}`; `NotFound` adds up to three `suggestions[]` via `MonolithFuzzyMatchDetail::ScoreFuzzyMatches` when candidates are given. `class` values: `invalid_param | not_found | precondition_failed | not_implemented | optional_dep_unavailable | engine_error | lease_busy | invalid_lease | not_sent | unknown_outcome`. The HTTP server always emits `structuredContent` for errors (it does) and now also `error.data.class`. Document in `CONTRIBUTING.md`. Automation test for each helper's payload shape.

**5. Migrate the "not found" and "missing param" call sites.**
There are roughly 786 `Error(...)` calls whose message contains "not found" and roughly 300 that report a missing or invalid parameter, almost all defaulting to `-32603`. Write `Scripts/codemod_error_classes.py` that rewrites the mechanical cases (`Error(FString::Printf(TEXT("... '%s' not found ..."), *X))` and the `Missing required param` family) to the item 4 helpers, run it, and hand-fix what it cannot classify in the Core, Blueprint, Material, Niagara and Animation modules. Commit the script with the change. Report the before/after counts of `-32603` errors per module. Do not chase perfection: the goal is that a typo'd asset path or graph name gets `class:"not_found"` plus suggestions in the five biggest namespaces.

### D. Schema drift

**6. Schema-drift lint in CI.**
Write `Scripts/check_schema_drift.py`: for every `RegisterAction` call, collect the keys declared by its `FParamSchemaBuilder` chain (and aliases), then scan the handler body for direct `Params->TryGet*/Get*/HasField(TEXT("key"))` reads and report keys that are read but not declared. Support an allowlist file for handlers that forward `Params` to helpers. Fail on any undeclared key. Also fail on any `RegisterAction` without a schema (13 today, for example `material.end_transaction`, `monolith.status`, `monolith.reindex`). Fix all current violations: declare the hidden params (`material.connect_expressions` `from_pin/to_pin`, `material.render_preview` `background_color`, `blueprint.resolve_node` `event_name/macro_blueprint/macro_name`, `blueprint.add_nodes_bulk` `position`, `blueprint.set_function_thread_safe` `name`, `editor.get_recent_logs` `max`, `editor.capture_scene_preview` `background_color/uv_tiling`, and whatever else the script finds) and give the 13 schema-less registrations an explicit (possibly empty) builder. Add the script to the `lint` job in `.github/workflows/proxy-tests.yml` and to `Scripts/check_repo_lint.py`.

### E. Transport evidence

**7. End-to-end request id and a per-request log line.**
Both proxies generate a UUID per forwarded `tools/call` and send it as `X-Monolith-Request-Id`, plus `X-Monolith-Client: <name>/<pid>` where name comes from `MONOLITH_CLIENT_NAME` (default `proxy`). The server reads them, writes one `Log`-level line per request (`[req=... client=... lease_owner=...] ns.action ok=... ms=...`), and returns them in `result._meta.monolith.{request_id, server_instance, lease_owner}` on success and `error.data.{request_id, server_instance}` on failure. `server_instance` is a `FGuid` created at module startup and also exposed by `monolith_status` and `/health`. Proxies write the same UUID into the JSONL call log. Tests: HTTP protocol Automation test asserts the `_meta` block; proxy contract test asserts the header is sent and the UUID appears in the call log.

**8. Distinguish "not sent" from "unknown outcome"; release the lease on proxy exit.**
In both proxies, a failure before the request is written to the socket (connection refused, DNS, WinHTTP connect error) is reported as a tool error with `error.data.class = "not_sent"`, `executed:false`, `retryable:true`; only a failure after the request was sent (timeout, reset mid-response, malformed body) keeps `unknown_outcome` with `executed:"unknown"`. Give the native proxy a separate 5-second connect timeout. On stdin EOF, each proxy sends a best-effort `monolith_coordination release` for any `_lease_token` it saw in an `acquire` response during its lifetime, bounded to 2 seconds. Tests: fixture that refuses connections; fixture that accepts then hangs; EOF-after-acquire releases.

**9. Call-log parity and rotation.**
The native proxy has no JSONL call log; add one matching the Python proxy's fields plus `outcome: ok|error|not_sent|unknown|cancelled`, `client`, `request_uuid` and millisecond timestamps. Both proxies rotate at `MONOLITH_CALL_LOG_MAX_MB` (default 16) and delete `MonolithCalls-*.jsonl` older than 14 days at startup. Update `SECURITY.md` and `Docs/MULTI_AGENT.md`. Tests: rotation and cleanup in `test_proxy_transport.py` for both flavours.

### F. Domain honesty and availability

**10. `risk.*` must not block the editor on first use.**
`FRiskQueryAdapter.cpp` (~230-275) bootstraps git mining synchronously on the game thread, up to 30 seconds per repository. Change the contract: if the co-change tables are not mined, `risk.*` queries return `PreconditionFailed("risk index not mined", next_action: "risk.mine")` immediately. Add `risk.mine` that starts mining on a background task (the git subprocess and SQLite writes off the game thread; results merged on the game thread) and `risk.get_mining_status` reports `state: idle|running|done|failed` with progress. Automation test with a fixture repository: query before mining fails fast with the precondition class; after `mine` completes, the same query succeeds.

**11. One availability contract for optional-plugin namespaces.**
Today LogicDriver, ComboGraph, CommonUI and MetaSound namespaces vanish entirely when their plugin is absent (pattern A), Chooser and GeometryScript stay registered and return a clear error (pattern B), and Blueprint Assist falls back silently (pattern C). Adopt pattern B everywhere: register the actions unconditionally with a handler that returns `OptionalDepUnavailable(dep)` when the plugin is missing, and add `availability: {available, reason, required_plugin}` per namespace to the top-level `monolith_discover` inventory (extend `GetKnownOptionalModules` in `MonolithCoreTools.cpp` to cover `logicdriver`, `combograph`, `chooser`, MetaSound audio and CommonUI). Keep `WITH_*` compile gates for the implementation bodies; only registration and the error path become unconditional. Update `Docs/MONOLITH_GUIDE.md` troubleshooting ("Unknown namespace" no longer means "plugin missing"). Automation test: with the plugin absent, the namespace is discoverable and an action returns `-32010` with `dep_name`.

**12. Remove or relabel the remaining "success but fake" results.**
`mesh.integration_hooks_stub` (`MonolithMeshQualityActions.cpp` ~146, ~1535) is a registered action that returns a not-implemented Success; unregister it and move its content to the spec. `mesh.analyze_co_op_balance` (~1519) returns numeric scores labelled "P3 placeholder"; either implement the scoring it claims or return `NotImplemented("co_op_balance_scoring")`. `mesh` collision compute-but-not-persist (`MonolithMeshOperationActions.cpp` ~496/514): finish the persist path or return `PreconditionFailed` telling the caller to pass `save_handle`. `animation.build_state_machine` (`MonolithAnimationActions.cpp` ~8417-8466): add top-level `partial:true` and `deferred_rules:N` when any transition rule was deferred. Each gets an Automation test.

### G. Tests for zero-coverage modules

**13. One create → mutate → read-back → compile → save round-trip Automation test each for Niagara, GAS, Material and Audio.**
Use `/Game/Tests/Monolith/<Module>/` (the cleanup allowlist already accepts it) and GUID-suffixed asset names; delete the assets in a scope guard. Each test must exercise the item 3 `save` contract (dirty without `save`, on disk with `save=true`) and one `NotFound` path with suggestions from item 4. Register them under `Monolith.<Module>.RoundTrip`.

## Final report

Write `Docs/PHASE1_REPORT.md` with one row per item: status (`done`, `skipped: already correct`, `skipped: needs design`, `unverified: <reason>`), commit hash, tests run and results, and user-visible behaviour changes. Include the before/after `-32603` counts from item 5, the schema-drift violations found and fixed in item 6, and the full `Monolith.` Automation counts (succeeded, self-skipped, failed, not run) after item 1 and again at the end. End with the exact commands you ran. Do not merge, do not push.
