# UE 5.7.4 validation record

Validated locally on 2026-09-05 (Europe/Istanbul) on branch `feat/multi-agent-reliability`, based on upstream `e67544c`. This records a tested development build, not a published release or certification of every action.

## Environment and results

- Windows 11 x64; Unreal Engine **5.7.4**, changelist **51494982**, installed at `D:/UE_5.7`.
- Visual Studio 2022 / MSVC 14.44.35225; Development Editor target in a disposable `MonolithValidation57` project. Full plugin compilation and linking succeeded. The native stdio proxy was also built successfully.
- **52/52 UE Automation tests passed**, with zero failures or test warnings: coordination, HTTP protocol, AI discovery, reflection writes, schema discovery, parameter kinds, response shaping and fuzzy matching. This includes five replacement reflection tests that exercise actual UObject/container behavior instead of placeholder assertions.
- **56/56 Python unittest cases passed**, with no skips when both optional native-proxy and live-editor environments were enabled: 41 proxy contracts, 5 installer tests, and 10 live MCP tests. These exercise both proxy implementations, bounded concurrency, duplicate IDs versus independent identical calls, unknown outcomes, malformed responses, notifications, cache publication and independent client processes. The 10 live tests also passed again after the final protocol fixes.
- **Blueprint authoring smoke passed** in the disposable editor: an unowned creation call was rejected before execution; the lease owner created an Actor Blueprint, added integer `AgentValue=42`, compiled with zero errors/warnings, saved, and read back the value. After a full editor restart, the value loaded from disk correctly. Asset: `/Game/MonolithValidation/BP_AgentFixture_60d4e87c` in the temporary project only.
- All **19 SKILL.md** entrypoints passed the skill validator. JSON/TOML configuration templates parsed; installer listing and dry-run succeeded.

The existing game project and its editor were left untouched. Test editors used NullRHI, port `19316`, disabled automatic update and deferred first indexing. Existing user MCP/skill configuration was not overwritten.

## Reproduce

Build the proxy from a Visual Studio C++ developer installation:

```powershell
./Scripts/build_proxy.ps1
$env:MONOLITH_TEST_NATIVE_PROXY = (Resolve-Path Binaries/monolith_proxy.exe).Path
python -m unittest discover -s Scripts/tests -v
```

Without a running editor the live suite deliberately skips. To include it, launch a disposable project with the freshly built plugin and set its actual endpoint:

```powershell
$env:MONOLITH_LIVE_URL = 'http://127.0.0.1:19316/mcp'
python -m unittest discover -s Scripts/tests -v
```

The live unittest suite only reads diagnostics and exercises temporary leases. It does not create game assets. Do not run it against an editor currently used by another workflow. The separate Blueprint smoke above was performed explicitly in the disposable project.

For the engine-dependent tests, build the host project's Editor target with UnrealBuildTool, then run this filter in the editor's Automation system:

```text
Monolith.Coordination+Monolith.Core+Monolith.AI.Discovery+Leviathan.Monolith.Reflection+Monolith.ResponseShaping+Monolith.Describe+Monolith.ParamKind+Monolith.Discover+Monolith.FuzzyMatch
```

Local evidence is under ignored `Saved/`: `UE57-full-build.log`, `UE57-final-build.log`, `UE57-automation.log`, `AutomationReport/index.json`, `all-contract-tests.log`, `live-contract-tests-final.log`, `blueprint-smoke-result.json`, and `blueprint-reload-result.json`. Build products and these machine-local logs are not committed. CI configuration was added; a remote CI run is not claimed.

## Limits and next production gates

No packaged-game/cook test, GPU/render quality assessment, multiplayer validation, UE 5.8/macOS runtime test, or exhaustive test of all domain actions was performed. NullRHI cannot establish lighting, VFX, animation or frame-time quality. State Tree/EQS discovery and the other identified incomplete integrations remain explicit limitations in [the audit](AUDIT_UE57.md).

Multiple clients can now submit work without the old proxy bottleneck, and leases protect cooperative multi-call editor workflows. UObject operations still execute on the game thread. A host must independently expose MCP tools to its subagents; plugin code cannot grant that access. Follow [the multi-agent setup](MULTI_AGENT.md) and validate representative assets plus a packaged vertical slice in the actual game before relying on this branch in production.
