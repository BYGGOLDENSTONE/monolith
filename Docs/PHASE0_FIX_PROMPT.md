# Phase 0 fix prompt (for an AI coding agent)

Copy everything below the line into the agent's prompt. It assumes the agent has the repository checked out and can build the plugin and run the Python tests. The full analysis this is derived from is `Docs/IMPROVEMENT_ANALYSIS.md`.

---

You are working in the Monolith repository, an Unreal Engine 5.7 editor plugin that exposes ~1,400 editor actions over MCP through a small set of namespace-dispatch tools. Read `README.md`, `Docs/MULTI_AGENT.md`, `Docs/AUDIT_UE57.md`, `CONTRIBUTING.md`, and `Docs/IMPROVEMENT_ANALYSIS.md` before changing anything. The analysis document is the source of truth for this task; this prompt is its Phase 0 work order.

## Ground rules

- Work on a new branch off `feat/multi-agent-reliability` named `fix/phase0-quick-wins`. Do not touch `master`.
- One commit per numbered fix below, in the order given. Commit subject format: `fix(<area>): <what>` (areas: `proxy`, `core`, `http`, `niagara`, `ui`, `logicdriver`, `skills`, `docs`, `ci`). Body: one paragraph on why, then which test proves it.
- Every fix must be verified before its commit. Verification tiers:
  - Python: `python -m unittest discover -s Scripts/tests -v` must stay green. If `Binaries/monolith_proxy.exe` can be rebuilt (`powershell -File Scripts/build_proxy.ps1`), set `MONOLITH_TEST_NATIVE_PROXY` to its path so the native contract tests run too.
  - C++: the plugin must compile against UE 5.7 (Development Editor). If an editor is available, run the Automation filter `Monolith.` and report pass/fail counts. If no editor is available, say so explicitly in the final report; do not claim a test ran.
  - For each fix, add or extend a test when the fix is testable without an editor (proxy, registry, HTTP protocol, coordination tests all have existing fixtures).
- Do not widen scope. Fixes that need design decisions (job framework, per-asset locks, error taxonomy migration, path guard, `save` defaults) are Phase 1+ and are out of bounds here. If a fix turns out to require one of those, stop that fix, leave a note in the report, and continue with the next one.
- Do not edit `CHANGELOG.md` per commit. Add one `## Unreleased` block at the end listing all landed fixes.
- Keep `.ps1` files ASCII-only. Use `LogMonolith`, never `LogTemp`. Match surrounding code style; no whitespace-only churn.
- Before each edit, read the cited lines and confirm the described behaviour is still present. Line numbers are from the analysis date and may have drifted. If the code already behaves correctly, skip the fix and say why.

## Fixes

### Transport and coordination

**1. Python proxy health probe honours `HTTP_PROXY`, forwarded calls do not.**
`Scripts/monolith_proxy.py`: `_check_monolith_up` (around line 528) uses `urllib.request.urlopen` with the default opener, which follows `HTTP_PROXY`/`HTTPS_PROXY` and redirects. `_post_monolith` (around line 271) deliberately uses `ProxyHandler({})`. On a machine with a corporate proxy the health check reports "down" while calls succeed, producing a `notifications/tools/list_changed` storm. Make both paths use the same no-proxy opener with redirects disabled. Also: a health-probe timeout should be treated as "busy" (no state change) and only connection-refused as "down". Test: extend `Scripts/tests/test_proxy_transport.py` with `HTTP_PROXY` set to an unreachable address and assert the health poll still reports up.

**2. Native proxy caches the split-editor rewritten tool list under the shared key.**
`Tools/MonolithProxy/monolith_proxy.cpp` around lines 1112-1159: with `MONOLITH_SPLIT_EDITOR_QUERY=1` the `tools/list` result is rewritten (`editor_read_query`, `editor_build_query`) and then published to the cache keyed only by `sha1(URL|project_root)` (around line 757). A Python proxy or a native proxy without the flag then serves tools the editor cannot dispatch. Fix by caching the raw upstream list and applying the rewrite on read, so the key stays shared. Test: native-only case that writes a cache with the flag on, reads with the flag off, and asserts no `editor_read_query` entry is served.

**3. Non-JSON-RPC rejection bodies make the proxy report "unknown outcome".**
`Source/MonolithCore/Private/MonolithHttpServer.cpp`: the Origin rejection (around line 1036), protocol-version rejection (around line 233) and 413 body-too-large (around line 244) return a bare `{"error": "..."}`. The proxies treat any non-JSON-RPC upstream body as "invalid upstream response, execution outcome unknown". These requests were never executed. Return a JSON-RPC error object (`-32600` for Origin/version, `-32600` with a size message for 413) with `error.data.executed = false`. Test: extend `MonolithHttpProtocolTest.cpp` for all three paths and assert the body parses as JSON-RPC with `executed:false`.

**4. `-32010` is used with two meanings.**
`Source/MonolithCore/Public/MonolithJsonUtils.h` reserves `-32010` as `ErrOptionalDepUnavailable` (around line 91) and documents `-32011..-32019` as reserved for optional-dependency codes. `Source/MonolithCore/Private/MonolithCoordination.cpp` uses `-32010` for `lease_busy` / `lease_executing` / `editor_executing` (around lines 117, 141, 192) and `-32011` for `invalid_lease`. Move the coordination codes to a new range (`-32020` busy/retryable, `-32021` invalid lease), add named constants in `MonolithJsonUtils.h`, update `Docs/MULTI_AGENT.md`, `Skills/monolith-multi-agent/SKILL.md`, `Scripts/tests/test_live_mcp.py` and any Automation test asserting the old numbers. Keep `retryable` derived from the constant, not from a literal.

**5. Legacy JSON-RPC batch can lose the lease between items.**
`MonolithHttpServer.cpp` around lines 321-329 processes batch items sequentially; each goes through `CheckAccess` separately and `ActiveExecutions` returns to zero between items, so the lease can expire mid-batch and items k+1..n fail with invalid-lease. Pin the lease for the duration of the batch (increment `ActiveExecutions` once around the loop when the first item carried a valid token, or evaluate access once per HTTP request). Test: `MonolithCoordinationTest.cpp` with the injected clock, a two-item batch whose clock crosses `ExpiresAt` between items, assert both execute.

**6. `renew` without `ttl_seconds` shortens a long lease to 120 s.**
`MonolithCoordination.cpp` around line 125: `TTL` defaults to 120 regardless of the TTL the lease was acquired with. Store the acquired TTL and use it as the renew default. Also add a small grace window: when a leased action finishes after `ExpiresAt`, extend `ExpiresAt` to `now + min(30, TTL/4)` in `FExecutionScope`'s destructor so the owner can release cleanly. Test: existing coordination test file, two new cases.

**7. Body-size check runs after the body is fully buffered and NUL-scanned.**
`MonolithHttpServer.cpp` around lines 216-249: the O(n) NUL scan happens before the `MaxRequestBodyMB` comparison. Move the size comparison before the scan. (The engine still buffers the body; note in the commit body that this is ordering only, not a memory bound.)

**8. Proxy `initialize.instructions` differs from the server's.**
`Tools/MonolithProxy/monolith_proxy.cpp` around line 1090 and `Scripts/monolith_proxy.py` answer `initialize` locally with a short string; the server's text (`MonolithHttpServer.cpp` around line 538) is the one that mentions `monolith_guide` and the discover/describe chain. Make both proxies fetch and cache the server's `instructions` alongside the tools cache, falling back to the server string copied verbatim when offline. Test: fixture returns a custom instructions string; assert the proxy relays it.

**9. Python and native seed catalogs differ.**
`Scripts/monolith_proxy.py` `_seed_tools` (around line 396) lists `monolith_reindex`; the native `CORE_QUERY_TOOLS` (around line 826) does not. Reconcile to the server's actual core tool set (check `MonolithCoreTools.cpp` registrations) and add a Python test that loads both seed lists and asserts equality of tool names.

### Core registry and discovery

**10. Unknown-param warnings are dropped when the action fails.**
`Source/MonolithCore/Private/MonolithToolRegistry.cpp` around line 540: post-handler warnings (including "unknown param key") are only attached on success. On failure the caller loses the one hint that explains a typo. Attach warnings into `error.data.warnings` on the failure path as well. Test: extend the existing registry/param-kind tests.

**11. Top-level `monolith_discover` prints every action name.**
`Source/MonolithCore/Private/MonolithCoreTools.cpp` around lines 487-550: the namespace inventory embeds the full action-name array per namespace, roughly 1,580 names. Return only `namespace`, `action_count`, description and `categories[]` by default; add `include_action_names=true` to restore the old shape. Update `Docs/MONOLITH_GUIDE.md` and `Docs/API_REFERENCE.md` wording. Test: `MonolithDiscoverTerseTest.cpp` asserts the default payload has no `actions` array and the flag restores it.

**12. No `did_you_mean` for a misspelled namespace or action in discovery.**
`MonolithCoreTools.cpp` around lines 344-347 (`discover` with unknown namespace) and `Source/MonolithCore/Private/Actions/MonolithBulkFillActions.cpp` around lines 274-279 (`action_schema` with unknown action) return a plain error. The dispatcher already computes fuzzy suggestions (`MonolithToolRegistry.cpp` around line 280, `MonolithFuzzyMatch`). Reuse it and emit `error.data.suggestions[]` in both places. Test: extend `MonolithFuzzyMatchTest.cpp` or `MonolithDiscoverTerseTest.cpp`.

**13. Success responses carry no `structuredContent`.**
`MonolithHttpServer.cpp` around lines 908-935: the error path sets `structuredContent`, the success path does not. Set `structuredContent` to the result object on success as well (MCP 2025-06-18 clients use it). Test: `MonolithHttpProtocolTest.cpp`.

### Domain honesty

**14. Five Niagara actions are labelled "Phase 0 stub. Not yet implemented" but are implemented.**
`Source/MonolithNiagara/Private/MonolithNiagaraTimingActions.cpp` around lines 64-90 and `MonolithNiagaraActions.cpp` around line 2590: `get_system_timing`, `set_warmup_profile`, `set_fixed_tick_delta`, `set_require_current_frame_data`, `create_stateless_emitter`. Read each handler body, confirm it does real work, and rewrite the schema description to describe what it does. If any of them really is a stub, leave the label and say so in the report.

**15. UI spec builder and CommonUI button binding return Success for work they did not do.**
`Source/MonolithUI/Private/Actions/MonolithUISpecActions.cpp` around lines 1086-1213 and 1317-1321: `layers`, `focus_table`, `nav_overrides` are accepted, echoed and not applied; screens without a spec get `status="stub"` inside a Success. `Source/MonolithUI/Private/CommonUI/MonolithCommonUIButtonActions.cpp` around lines 878-894 and 1456-1465: two binding actions validate and probe, then return Success with `status="stub"`. Follow the pattern already used in `MonolithAIDiscoveryActions.cpp` (around line 349): return an explicit error with `error.data.reason = "not_implemented"`, `implemented = false`, and the name of the unimplemented part. For the spec builder, if the rest of the screen was built, return the error only for the unimplemented keys and keep a `partial:true` flag plus the list of applied keys in `error.data`. Remove the three unimplemented keys from the schema description or mark them `not implemented` there. Add or extend a UI Automation test for each.

**16. LogicDriver discovery reports `sm_component_count = -1` inside a Success.**
`Source/MonolithLogicDriver/Private/MonolithLogicDriverDiscoveryActions.cpp` around lines 147-156. Replace the sentinel integer and empty array with `component_scan: "not_indexed"` and omit the numeric field, or return the same `not_implemented` error shape as fix 15. This module compiles only with Logic Driver Pro present; if you cannot build it, make the change, keep it syntactically minimal, and flag it as unverified.

### Skills and docs

**17. `Skills/unreal-logicdriver/unreal-logicdriver.md` names about 40 actions that do not exist.**
Regenerate its action table from the actual `RegisterAction` calls in `Source/MonolithLogicDriver/Private/*.cpp` (for example `runtime_stop_sm`, `runtime_restart_sm`, `runtime_switch_state`, not `runtime_stop` / `runtime_restart` / `runtime_send_event`). While there, remove hard-coded action counts from `Skills/unreal-blueprints/unreal-blueprints.md`, `unreal-ui.md`, `unreal-mesh.md`, `unreal-cpp.md` (they are stale) and move `capture_material_grid`, `capture_scene_preview`, `inspect_material_pbr` in `unreal-materials.md` to the `editor` namespace where they actually live. Write a small script `Scripts/check_skill_actions.py` that extracts backticked `namespace.action` or `action` names from `Skills/**/*.md` and checks them against `RegisterAction` calls; make it exit non-zero on unknown names and run it in CI (fix 19).

**18. Dangling references.**
`Docs/SPEC_CORE.md` around line 896 links `Docs/references/MCP.md` and `UE57Gotchas.md` (directory does not exist). `Docs/specs/SPEC_MonolithReflectionIntel.md` around line 1302 references `.claude/rules/scoped/monolith-release.md` (not in repo). About 70 source files carry comments pointing at `Docs/plans/2026-05-27-mcp-llm-ergonomics.md` (not in repo). Remove or redirect the doc links; for the source comments, replace the path with a one-line summary of the referenced decision or drop the reference. Also delete `Docs/SPEC.md` (103-byte redirect stub) and update any link to it.

### CI, contributing, hygiene

**19. `Source/**` changes trigger no CI.**
`.github/workflows/proxy-tests.yml` lines 3-17 restrict triggers to proxy and test paths. Remove the path filter so every push and PR runs the existing jobs. Add a third hosted job `lint` that runs: `python -c "import json; json.load(open('Monolith.uplugin'))"`; JSON and TOML parse of `Templates/*`; `Scripts/check_skill_actions.py` (fix 17); a version-consistency check that `Monolith.uplugin` `VersionName`, `MONOLITH_VERSION` in `Source/MonolithCore/Public/MonolithCoreModule.h`, and the version line at the top of `Docs/API_REFERENCE.md` agree; a grep that fails on `LogTemp` in `Source/`; a grep that fails on non-ASCII bytes in `Scripts/*.ps1`; and a `git ls-files` guard that fails if any path listed as private in `.gitignore` (lines 16-21 and 33-40) is tracked. Keep the total under five minutes. Drop Python 3.8 from the matrix since `CONTRIBUTING.md` requires 3.10+.

**20. `CONTRIBUTING.md` shows an action registration API that does not compile.**
Lines 88-127 show a handler returning `TSharedPtr<FJsonObject>` and `RegisterAction(ns, action, desc, "{json}", &Handler)`. The real signature is in `Source/MonolithCore/Public/MonolithToolRegistry.h` (around lines 49 and 111): a `FMonolithActionHandler` delegate returning `FMonolithActionResult`, a `TSharedPtr<FJsonObject>` schema built with `FParamSchemaBuilder`, and a category. Rewrite the example using a real action from `MonolithEditorActions.cpp` as the model (for instance the `get_crash_context` registration around line 517). Update the error-handling section (lines 214-221) to use `FMonolithActionResult::Error(message, code)` with the codes in `MonolithJsonUtils.h`, and list every place a new action touches (handler, registration, spec table, `API_REFERENCE.md`, skill table, CHANGELOG, and for a new namespace both proxy seed lists and the `.uplugin` description).

**21. README carries version history and stale claims.**
Move the "New in v0.17 / v0.18 / v0.18.1 / v0.19" and "Unreleased" paragraphs (README lines 37-47) into `CHANGELOG.md` where they belong; the "Unreleased" AnimGraph pack already shipped in 0.20.0. Replace them with one sentence and a link to the changelog. Resolve the "1M+ symbols" versus "~967K symbols" contradiction by using the measured figure. Leave the development-branch banner at the top in place; it is removed at merge time, not now.

**22. `.gitignore` excludes `Tools/` while ten files under it are tracked.**
`.gitignore` line 30. Replace the blanket `Tools/` rule with the build-output subpaths that should be ignored (check `git status --ignored Tools` to see what is currently hidden, for example `_compile.bat` and CMake build dirs). Also delete the empty template `.github/FUNDING.yml`.

## Final report

Produce `Docs/PHASE0_REPORT.md` with one row per fix: number, status (`done`, `skipped: already correct`, `skipped: needs design`, `unverified: no editor`), commit hash, which tests ran and their result, and any behaviour change a user would notice. End with the exact commands you ran for the Python suite, the plugin build, and the Automation run, with their pass/fail counts. Do not merge the branch and do not push to `master`.
