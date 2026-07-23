# nightrun — unattended harness (Night Protocol Faz 0)

Scripts night-run worker agents call to build, boot, probe and shut down the
MonolithDev host project without a human present. Every script writes a JSON
verdict into `results/` (gitignored) — that file, not the console output, is
what the verification agent reads. Design + policies: `Docs/NIGHT_PROTOCOL.md`.

Paths (engine `D:\UE_5.7`, project `D:\UnrealProjects\MonolithDev`, port 9316)
are centralized in `nightrun_common.ps1`.

## Scripts

| Script | What it does | Exit 0 means |
|---|---|---|
| `build.ps1` | Kills stale UE/UBT processes, runs UBT with a hard timeout (default 45 min), parses the log | build green (`Result: Succeeded`) |
| `launch_editor.ps1` | Kills stale editors, starts the editor unattended, polls `GET /health` until the MCP server answers, leaves the editor running | server up (JSON has pid/version/tool count) |
| `smoke.ps1` | Against a running editor: `/health` → MCP `initialize` → `tools/list` → `monolith_status` → `monolith_discover` (+ optional `-ExtraToolCalls 'tool|{"json":"args"}'`) | all steps passed (`isError` inspected per call) |
| `run_tests.ps1` | Headless automation tests via `UnrealEditor-Cmd` (`-nullrhi`), parses the UE JSON report (default `-Filter Monolith` = whole suite) | all matched tests passed |
| `teardown.ps1` | Kills every Monolith-related process, verifies port closed, records crash folders. Always exit 0 — verdict is in the JSON. | (see `status: clean/dirty` in JSON) |

## Typical worker cycle

```powershell
.\build.ps1                     # red? report blocked, run teardown, stop
.\launch_editor.ps1             # rendered (night default; screenshots work)
.\smoke.ps1 -ExtraToolCalls 'editor_query|{"action":"...","params":{}}'
.\run_tests.ps1 -Filter Monolith.<area>
.\teardown.ps1                  # ALWAYS, success or failure
```

## Modes

- **Rendered (default)** — real RHI; editor window may appear (fine overnight).
  Required for screenshots / VFX / lighting checks. Verified boot: ~15 s warm.
- **`launch_editor.ps1 -NullRhi`** — no rendering, for logic-only sessions.
  Screenshot/preview captures will NOT work in this mode.

## Known issues (2026-07-23 validation)

- ~~Full-suite crash at `Assertion failed: !Database`~~ FIXED same day: four
  tests held `FSQLitePreparedStatement` locals alive across `Db.Close()`
  (CppReflectQueryTests, DecisionRecordIndexerTests ×2, RiskQueryTests) —
  statements now finalize in nested scopes. Suite runs all 177 tests to completion.
- 4 pre-existing test failures the crash was masking (genuine behavior bugs,
  triage as separate work items): `Monolith.ReflectionIntel.Decision.HeuristicAccuracy`
  (indexer ingests 2 rows from the non-decision fixture), `Monolith.CursorPagination.QueryMismatchRejection`
  (error lacks ErrorData), `MonolithUI.Allowlist.UnknownTypeDenied`,
  `MonolithUI.Reflection.ParseLinearColorHex`. Baseline: 173/177 green.
- First `capture_scene_preview` right after boot renders very dark (preview
  scene lighting warmup); capability works, tune params per task.
