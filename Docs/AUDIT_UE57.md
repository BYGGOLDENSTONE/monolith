# UE 5.7.4 and multi-agent readiness audit

This is a source-backed engineering audit, not certification of every Monolith action or a guarantee of AAA quality. The immediate work targets shared-editor reliability, AI-facing documentation and reproducible verification. Runtime compatibility must be established with the user's exact engine build and project. A source review or green Python suite cannot establish UE binary compatibility.

## Findings and disposition

| Area | Evidence in this repository | Disposition / remaining work |
|---|---|---|
| Proxy request concurrency | Both proxies originally forwarded requests in the input loop; native `Tools/MonolithProxy/monolith_proxy.cpp` also rejected independent same-argument calls | Bounded concurrent forwarding and request-identity behavior are covered by proxy regression tests. Keep game-thread mutation ordering separate from transport concurrency. |
| Shared editor workflow ownership | `Source/MonolithCore/Private/MonolithToolRegistry.cpp` is the common dispatch path; independent multi-call workflows need ownership beyond sequential individual calls | `MonolithCoordination.cpp` introduces process-local cooperative acquire/renew/release with opaque tokens and guarded dispatch. Verify nested dispatch, stale tokens and multiple clients in the actual editor. |
| AI skill discovery | Existing references were named `Skills/<name>/<name>.md`, without standard `SKILL.md` entrypoints | Added concise entrypoints, a multi-agent skill, explicit installer and host configuration examples. Legacy reference paths remain available. Installation does not grant tool visibility to a worker. |
| Build guidance | Old `unreal-build.md` inferred a closed editor from MCP failure and used a fixed wait; debugging denied the source include-path action | Corrected state diagnosis, bounded completion polling and schema-first lookup. Structural rebuild guidance now distinguishes conservative practice from Live Coding's supported reinstancing. |
| Performance guidance | Old material/Niagara references pointed to absent `Docs/references/` files and asserted universal particle/cycle limits; performance guide confused graph count with compiled instructions | Replaced unsupported absolutes with available versioned references and measurement requirements; uses `get_compilation_stats` for compiled material information. Other long domain references remain examples, not an exhaustively revalidated API catalog. |
| Reflection test coverage | `Source/MonolithCore/Private/Reflection/Tests/MonolithReflectionWalkerTest.cpp` originally contained five deferred test bodies using unconditional `TestTrue(..., true)` | Replaced with behavior-based tests in this improvement. Their runtime validation remains an engine Automation gate; the old green placeholder results were not coverage evidence. |
| AI discovery completeness | `Source/MonolithAI/Private/MonolithAIDiscoveryActions.cpp` originally returned successful empty State Tree / EQS results with a not-implemented note | Now returns an explicit capability error with `reason=not_implemented`, `system` and `implemented=false`; validates system/category input. New Automation tests check unavailable systems, malformed inputs and actual engine BT task discovery. Completing State Tree/EQS enumeration remains future work. |
| Logic Driver indexing | `Source/MonolithLogicDriver/Private/MonolithLogicDriverIndexer.cpp` logs no-op/index-stub behavior | Do not use that index as evidence of complete state-machine coverage. Optional plugin integration needs dedicated fixtures and licensed dependency availability. |
| UI generated behavior | `Source/MonolithUI/Private/Actions/MonolithUISpecActions.cpp` can return `stub` / `partial_stub`; `CommonUI/MonolithCommonUIButtonActions.cpp` includes discoverable stub responses; `MonolithUISettingsActions.cpp` emits persistence TODOs | Treat schema generation and scaffolds as incomplete until bindings, focus, input, persistence and runtime behavior are implemented and tested. |
| GAS generated behavior | `Source/MonolithGAS/Private/MonolithGASAbilityActions.cpp` emits ability-task activation and cleanup TODO bodies | Generated source needs implementation and compile/runtime verification, including cancellation and delegate cleanup. |
| Risk/index latency | `Source/MonolithReflectionIntel/Public/Risk/FGitCoChangeIndexer.h` documents a game-thread bootstrap path and blocking subprocess poll loop | Profile first-use latency on a large project. Moving this work to a background job requires separate thread-safety and SQLite ownership work; it is not solved by adding proxy workers. |
| Release/engine identity | `Monolith.uplugin` has no engine patch pin; `Scripts/make_release.ps1` deliberately avoids adding EngineVersion | Verify `Engine/Build/Build.version`, rebuild from source for 5.7.4, and test the actual packaged artifact. A missing descriptor pin is not by itself a compatibility bug. |

Some stubs are intentional availability adapters for missing optional plugins; these are different from present-but-unimplemented behavior. Inspect returned availability, per-item failures and `status` fields rather than using the total discoverable action count as a quality measure. This audit samples concrete risks; it does not claim an exhaustive review of every action's body.

## Required local validation

1. Confirm the engine reports 5.7.4 and build the plugin against that installation. Use the project's actual target/platform and record the engine build identity, source revision and build result.
2. Run relevant UE Automation tests with meaningful assertions. Epic's [Automation Test Framework](https://dev.epicgames.com/documentation/unreal-engine/automation-test-framework-in-unreal-engine?application_version=5.7) exercises engine-dependent behavior; mocked proxy tests cover transport contracts only.
3. In a disposable test project, connect at least two clients. Have A acquire a lease and edit a test Blueprint; confirm B cannot read/change protected state, can inspect discovery, and can acquire after release. Verify stale tokens, expiry, editor restart, nested actions, compile/save/readback and a deliberately interrupted request. Inspect execution counts and asset state, not just response IDs.
4. Exercise representative Blueprint, material, animation, Niagara, mesh, UI and gameplay workflows used by the real game. Compile/save/reload assets and observe their behavior. Test optional modules enabled and disabled if they are shipped/supported.
5. Cook and package a representative game, launch it outside the editor, and test save/load, input, audio, loading, crash recovery and networking when applicable. Packaging is a separate gate from an Editor target build.

Do not reuse production assets for destructive timeout, restart or contention tests. No lease can cancel an already executed editor mutation or undo a partially completed multi-action workflow.

## Production and commercial gates

Define a short vertical slice whose controls, pacing and visual direction can be evaluated by real players. Set a budget for each feature's implementation, content creation, integration and testing. Prefer tools and generated assets that reduce the measured cost of reaching those acceptance criteria.

For each milestone, record:

- **Gameplay:** expected behavior, representative scenarios, failure/recovery paths, and playtest findings.
- **Visual and audio quality:** scene captures plus actual in-game inspection of animation, VFX, materials, lighting, readability and sound. A generated asset or successful compile is not a quality review.
- **Performance:** target hardware, RHI, resolution, scalability, CPU/GPU frame-time distribution, hitches, memory and loading measurements. Use [Unreal Insights](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-insights-in-unreal-engine?application_version=5.7) for trace evidence rather than universal CVar savings.
- **Release readiness:** packaged-build tests, asset dependency integrity, controls/settings/save behavior, and reproduction steps for remaining failures.
- **Commercial scope:** production cost, player feedback on the core experience, scope changes and the reason each next feature deserves investment. Tool improvements do not establish demand or predict revenue.

The useful objective is consistent, verifiable production work at a sustainable cost. AAA is a quality ambition that requires art direction, experienced review, runtime testing and substantial content work; no MCP can make it an automatic outcome.
