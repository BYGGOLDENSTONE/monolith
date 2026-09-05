# Phase 0 report

Validated on 2026-09-05 on Windows, branch `fix/phase0-quick-wins`, created from `feat/multi-agent-reliability` at `9956227`. The 22 numbered fixes are separate commits in order; the last is `d511d2e`. No merge or push was performed. The user-provided, initially untracked analysis and work-order files were preserved outside the commits.

The configured plugin builds successfully against UE **5.7.4** (CL 51494982, `D:/UE_5.7`). Final Python verification on **3.12.10** passed **91/91**, with **zero failures and zero skips**, using the rebuilt native proxy and a disposable live editor. Local repository lint passed. Remote CI was not run.

The complete `Monolith.` Automation run is **not green**: an existing test failed and an unchanged SQLite fixture crashed. Focused verification passed **57/57** with no warnings; a separate supplemental run passed **42/42**, including two warning-bearing successes. Counts and exclusions are detailed below. These runs are not an exhaustive validation of all plugin actions.

`unverified: no editor` for fix 16 means **no editor with Logic Driver Pro available**. UE itself was available, but `WITH_LOGICDRIVER=0` excludes the changed handler from compilation and execution. CommonUI was enabled; TokenforgeRuntime was absent.

## Numbered fixes

Python counts below are unittest cases, including inherited native contract cases when enabled. Every numbered commit has a passing pre-commit and post-commit suite in `Saved/phase0-NN-{pre,post}.log`. Through fix 18, the ten skips were live-editor cases. Fixes 19–21 also skipped the TOML fixture under the default Python 3.10; it passed separately on 3.12. Fix 22 used 3.12 with native and live tests enabled, so it had no skips. UE counts refer to the focused or supplemental filter stated in each row.

| Fix | Status | Commit | Verification and result | User-visible behavior / scope finding |
|---|---|---|---|---|
| 1 | done | `89854d2` | pre/post Python: 59 total, 49 passed, 10 live skips; native rebuilt | Health polling bypasses environment proxies and redirects; only refusal marks down, timeouts preserve state. |
| 2 | done | `1fca682` | pre/post Python: 60 total, 50 passed, 10 live skips; native rebuilt | Split-editor tool views no longer contaminate shared caches; cached and seed views are rewritten per client. |
| 3 | done | `07dac95` | pre/post Python: 64 total, 54 passed, 10 live skips; native rebuild; UE build succeeded; Monolith.Core.HttpProtocol: 1 passed, 0 failed | Origin/version/size rejections expose executed:false; both proxies retain validated pre-execution evidence. |
| 4 | done | `1f38e18` | pre/post Python: 64 total, 54 passed, 10 live skips; UE build succeeded; coordination + HTTP Automation: 5 passed, 0 failed | Coordination busy/invalid codes are -32020/-32021; optional dependency remains -32010. |
| 5 | done | `0932e2d` | pre/post Python: 64 total, 54 passed, 10 live skips; UE build succeeded; coordination + HTTP Automation: 6 passed, 0 failed | Validated lease remains pinned across legacy batch items; per-item ownership checks remain; release after batch response. |
| 6 | done | `4f70180` | pre/post Python: 64 total, 54 passed, 10 live skips; UE build succeeded; coordination + HTTP Automation: 8 passed, 0 failed | Omitted renew TTL retains duration; overlong actions/batches receive min(30, TTL/4) release grace. |
| 7 | done | `b5a2ebc` | pre/post Python: 64 total, 54 passed, 10 live skips; UE build succeeded; HTTP Automation: 1 passed, 0 failed | Oversized bodies return 413 before NUL scanning. Ordering only; UE still buffers full bodies. |
| 8 | done | `41f2484` | pre/post Python: 78 total, 68 passed, 10 live skips; native rebuilt successfully | Both proxies fetch/cache server instructions with exact offline fallback; bounded metadata workers keep ping responsive and share active-ID fences. |
| 9 | done | `b4963fb` | pre/post Python: 80 total, 70 passed, 10 live skips; native rebuilt successfully; seed parity and registration completeness tests | Both seeds include guide/coordination and core lease-token schemas. Reindex mismatch was already correct and left unchanged. |
| 10 | done | `1373926` | pre/post Python: 80 total, 70 passed, 10 live skips; UE build; ParamKind + ResponseShaping: 20 passed, 0 failed | Failure error.data retains parameter warnings and existing object or scalar data; success-only shaping stays unchanged. |
| 11 | done | `aad67ae` | pre/post Python: 80 total, 70 passed, 10 live skips; native rebuild; UE build; Discover: 11 passed, 0 failed | Namespace inventory defaults to counts, description and categories; include_action_names restores names. |
| 12 | done | `4564ddf` | pre/post Python: 80 total, 70 passed, 10 live skips; UE build; FuzzyMatch + Discover + Describe: 21 passed, 0 failed | Discovery and action_schema typos return ranked suggestions with existing codes preserved. |
| 13 | done | `15d8ffd` | pre/post Python: 80 total, 70 passed, 10 live skips; UE build; HTTP: 1 passed, 0 warnings, 0 failed | Successful calls include structuredContent matching the existing JSON text, including empty results. |
| 14 | done | `aea8a14` | pre/post Python: 80 total, 70 passed, 10 live skips; UE build; Discover: 11 passed, 0 failed; all five handler bodies inspected | Five implemented Niagara actions no longer claim to be stubs; fixed_delta_time omission correctly documents retaining its value. |
| 15 | done | `c7142b9` | pre/post Python: 80 total, 70 passed, 10 live skips; UE build; UI.Honesty: 6 passed, 0 warnings, 0 failed | Unsupported menu work and token graph binding return not_implemented errors. Partial screen results and applied_keys retained; dry runs claim no applied writes. One binding action exists, not two. Controlled provider-state test is not Tokenforge integration. |
| 16 | unverified: no editor | `aedfff4` | pre/post Python: 80 total, 70 passed, 10 live skips; UE build succeeds WITH_LOGICDRIVER=0; guarded code uncompiled/unexecuted because no LogicDriver-Pro-capable editor is installed. Full Monolith. interrupted: see limitations. | LogicDriver overview emits component_scan:not_indexed and omits sentinel count/empty actors. Status means no editor with required dependency, not absence of UE. |
| 17 | done | `a8787c3` | pre/post Python: 86 total, 76 passed, 10 live skips; 6 validator regressions; 1075 skill references verified | LogicDriver catalog matches registrations; four skills omit stale counts; action checker detects unknown names. Three materials editor actions were already correct. |
| 18 | done | `a99dbd4` | pre/post Python: 86 total, 76 passed, 10 live skips; target link/plan checks; 1075 skill references; UE build (235 actions); supplemental Automation: 40 clean + 2 warning passes, 0 failed | Cited missing references replaced with public docs; 11 source comment files cleaned; obsolete SPEC redirect removed; stale core coordination prose corrected. |
| 19 | done | `89572a6` | pre/post Python: 91 total, 80 passed, 11 skips (10 live + TOML on Python 3.10); Python 3.12 lint and 5 lint regressions pass; UE build; focused Automation: 57/57, zero warnings; generated logging fixture included | Every push/PR triggers tests; Python3.8 removed; 5-minute lint guards templates, versions, skill names, logging, ASCII scripts and private tracked paths. Generated runtime logging avoids unity collisions. Remote CI not run. |
| 20 | done | `4a980d4` | pre/post Python: 91 total, 80 passed, 11 skips; repository lint; example reviewed against compiled declarations and real editor registration. Live MCP separately 10/10 passed | Contributor action example uses actual result/delegate/schema/category API; error constants and all required documentation/seed changes listed. |
| 21 | done | `47d30cc` | pre/post Python: 91 total, 80 passed, 11 skips; repository lint; README history and banner checks | Release history lives in shipped changelog sections; AnimGraph is 0.20.0, measured index is ~967K, development banner retained; overview matches terse discovery. |
| 22 | done | `d511d2e` | pre/post Python 3.12: 91 passed, 0 failed, 0 skipped, native + live enabled; native rebuilt; repository lint and source/build ignore checks | Tools sources can be tracked; local helper/CMake outputs remain ignored; empty funding template removed. |

## Full Automation limitations

The final exact `Monolith.` filter discovered **127 tests**. The log records **81 started**, **80 completed**: **79 engine-reported Success** and **1 Fail**, followed by **1 crash**; **46 tests never started**. No complete JSON report was produced. Twelve of those 79 Success results explicitly self-skipped: nine `Editor.Preview` cases under NullRHI, and three `CursorPagination` cases because the source index was unavailable. They are not counted here as exercised behavior.

- `Monolith.CursorPagination.QueryMismatchRejection` failed because the expected `ErrorData` was absent. Source-index unavailability is the likely environment-related cause, inferred from the adjacent tests and handler prerequisites; a baseline runtime comparison was not performed. Its test logic was unchanged by Phase 0 (fix 18 only removed a comment reference).
- `Monolith.ReflectionIntel.CppReflect.FindSpecifier` crashed in the SQLite database destructor. The fixture keeps `Stmt` and `LowerStmt` alive while calling `Db.Close()`. This file is unchanged from baseline `9956227`; the crash stack identifies `CppReflectQueryTests.cpp`.
- The supplemental filter ran 42 of the original 45 never-started tests. It deliberately omitted three additional, source-confirmed instances of the same preexisting statement/close lifetime defect: `Monolith.ReflectionIntel.Decision.HeuristicAccuracy`, `Monolith.ReflectionIntel.Decision.StalenessFlag`, and `Monolith.ReflectionIntel.Risk.HotspotScoreFormula`. These omissions were not silently marked as passes.
- Supplemental results: **40 clean successes + 2 successes with warnings, 0 failures, 0 not run**. Both warnings concern empty git-repository discovery in risk fixtures. The later `Monolith.UI.GeneratedLogging` test is included in the final focused **57 clean successes**, which also re-exercised all changed core and UI contracts.

The original full-filter attempt at fix 16 discovered 126 tests and likewise recorded 79 Success, one Fail and one crash, leaving 45 not started. Fix 19 added the generated-logging test. No unrelated cursor or SQLite fixture changes were made to hide these limitations.

UI tests created and removed GUID-owned assets only in the disposable validation project. Binding coverage uses real widget/property validation with controlled provider availability; it does **not** establish Tokenforge integration. The LogicDriver change is syntactically minimal but remains unverified with its proprietary dependency. NullRHI does not validate rendering or GPU behavior. No Phase 1+ job framework, per-asset locking, path guard, error-taxonomy migration or save-default redesign was introduced.

## Commands and evidence

Commands were run from `D:/UnrealProjects/monolith`. Logs and disposable host files are machine-local and ignored; no binaries or temporary test assets were committed. The final native build is in `Saved/phase0-final-proxy-build.log`; Python and native proxy contracts ran, rather than being skipped.

### Python and native proxy

The required suite was run before and after every numbered commit. The default interpreter was Python 3.10; each invocation set the native-proxy environment variable explicitly:

```powershell
powershell -File Scripts/build_proxy.ps1
$env:MONOLITH_TEST_NATIVE_PROXY = (Resolve-Path Binaries/monolith_proxy.exe).Path
python -m unittest discover -s Scripts/tests -v
```

Final pre/post-fix-22 commands selected the installed Python 3.12 interpreter and enabled the live endpoint:

```powershell
$env:PATH = 'C:/Users/PC/AppData/Local/Programs/Python/Python312;' + $env:PATH
$env:MONOLITH_TEST_NATIVE_PROXY = (Resolve-Path Binaries/monolith_proxy.exe).Path
$env:MONOLITH_LIVE_URL = 'http://127.0.0.1:19316/mcp'
python -m unittest discover -s Scripts/tests -v
```

Result: **91 passed, 0 failed, 0 skipped** in both `Saved/phase0-22-pre.log` and `Saved/phase0-22-post.log`. The ten live cases also passed separately with:

```powershell
py -3.12 -m unittest discover -s Scripts/tests -p test_live_mcp.py -v
```

Local CI-equivalent checks:

```powershell
py -3.12 -c "import json; json.load(open('Monolith.uplugin'))"
py -3.12 Scripts/check_repo_lint.py
py -3.12 -m unittest discover -s Scripts/tests -p test_repo_lint.py -v
python Scripts/check_skill_actions.py
```

Result: lint passed, **5/5** negative lint fixtures passed, and **1,075** skill references verified. The lint job has a five-minute timeout; the local lint command completed in about one second. No hosted run is claimed.

### Plugin build

`Saved/phase0_verify_ue.ps1` invoked this Development Editor build against the disposable host:

```powershell
& 'D:/UE_5.7/Engine/Build/BatchFiles/Build.bat' MonolithValidation57Editor Win64 Development '-Project=C:/Users/PC/AppData/Local/Temp/MonolithValidation57/MonolithValidation57.uproject' -WaitMutex -NoHotReloadFromIDE
```

Result: **succeeded**. The fix-18 rebuild compiled/linked 235 build actions, the last native changes at fix 19 built successfully in 25 actions, and the final build verified the target was up to date. Evidence: `Saved/phase0-18-plugin-build.log`, `Saved/phase0-19-plugin-build.log`, `Saved/phase0-final-plugin-build.log`. This builds all enabled plugin modules; it does not compile the excluded LogicDriver implementation.

### UE Automation

The final required invocation was `powershell -File Saved/phase0_verify_ue.ps1 -Fix final -Filter Monolith.`. Its expanded editor command was:

```powershell
& 'D:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/PC/AppData/Local/Temp/MonolithValidation57/MonolithValidation57.uproject' -unattended -nop4 -NullRHI -nosplash -nosound -NoLiveCoding '-ExecCmds=Automation RunTests Monolith.' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=D:/UnrealProjects/monolith/Saved/Phase0Automation-final' '-abslog=D:/UnrealProjects/monolith/Saved/phase0-final-automation.log'
```

Result: **79 reported Success (12 self-skips), 1 Fail, 1 crash, 46 not started** out of 127 discovered. Editor exit status **3**; the wrapper returned failure because no final report existed. Evidence: `Saved/phase0-final-automation.log`. Do not interpret the wrapper's missing-report message as a successful or unrun test suite.

Focused final native verification used the same build/editor helper with:

```powershell
powershell -File Saved/phase0_verify_ue.ps1 -Fix 19 -Filter Monolith.Coordination+Monolith.Core+Monolith.Discover+Monolith.Describe+Monolith.FuzzyMatch+Monolith.ParamKind+Monolith.ResponseShaping+Monolith.UI
```

Result: **57 passed, 0 warnings, 0 failed, 0 not run**, including generated logging and UI honesty. Evidence: `Saved/Phase0Automation-19/index.json`.

The supplemental command was:

```powershell
$filter = (Get-Content Saved/phase0-supplemental-filter.txt -Raw).Trim()
powershell -File Saved/phase0_verify_ue.ps1 -Fix 18 -Filter $filter
```

The exact value of `$filter` was:

```text
Monolith.ReflectionIntel.CppReflect.UHTArtefactParse+Monolith.ReflectionIntel.Decision.SchemaBootstrap+Monolith.ReflectionIntel.Decision.SupersessionChain+Monolith.ReflectionIntel.Risk.CoChangeWeighting+Monolith.ReflectionIntel.Risk.ConditionalGateSweep+Monolith.ReflectionIntel.Risk.GitCoChangeSchema+Monolith.ReflectionIntel.Risk.QueryRegistration+Monolith.ReflectionIntel.SourceAudit.Registration+Monolith.ReflectionIntel.SourceAudit.SampleBuildCsParse+Monolith.ReflectionIntel.SourceAudit.SuggestBuildCsDepsForward+Monolith.ReflectionIntel.DocRealityDrift+Monolith.ResponseShaping.CompactJson+Monolith.ResponseShaping.EmptyFieldsNoOp+Monolith.ResponseShaping.FieldsWhitelist+Monolith.ResponseShaping.MutuallyExclusive+Monolith.ResponseShaping.OmitBlacklist+Monolith.ResponseShaping.PathFields+Monolith.ResponseShaping.PathFieldsMissing+Monolith.ResponseShaping.PathFieldsNoMatch+Monolith.ResponseShaping.RowFields+Monolith.ResponseShaping.RowFieldsAmbiguous+Monolith.ResponseShaping.RowFieldsNoList+Monolith.ResponseShaping.RowFieldsNoMatch+Monolith.ResponseShaping.RowFieldsPartialMatch+Monolith.ResponseShaping.StrictParamsAllowlist+Monolith.Source.CppErgonomics.AllmanClassIndexing+Monolith.Source.CppErgonomics.DeprecationIndexExtraction+Monolith.Source.CppErgonomics.DeprecationSchemaBootstrap+Monolith.Source.CppErgonomics.FindExampleUsagePagination+Monolith.Source.CppErgonomics.GenerateClassStubNeverWrites+Monolith.Source.CppErgonomics.GenerateClassStubText+Monolith.Source.CppErgonomics.IncludePathDerivation+Monolith.Source.CppErgonomics.LintHeaderRuleTable+Monolith.Source.CppErgonomics.SignatureCompaction+Monolith.Source.CppErgonomics.VerifySymbolsComposition+Monolith.Source.Indexer.WriterOpenFailureBroadcastsCompletion+Monolith.UI.Honesty.MenuMissingSpec+Monolith.UI.Honesty.MenuPartialBuild+Monolith.UI.Honesty.MenuSupportedBuild+Monolith.UI.Honesty.MenuUnsupportedKeys+Monolith.UI.Honesty.TokenBindingLiveProbe+Monolith.UI.Honesty.TokenBindingProviderState
```

Result: **42 passed (2 with warnings), 0 failed, 0 not run**. Evidence: `Saved/Phase0Automation-18/index.json`. These supplemental results do not replace the failed/interrupted full-filter results.
