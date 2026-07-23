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

## Known issues

**Current baseline: 177/177 green.** Nothing in this section is an open failure —
everything below is either fixed history kept for its diagnosis, or a live caveat.

### Fixed

- ~~Full-suite crash at `Assertion failed: !Database`~~ (2026-07-23) — four tests
  held `FSQLitePreparedStatement` locals alive across `Db.Close()`
  (CppReflectQueryTests, DecisionRecordIndexerTests ×2, RiskQueryTests);
  statements now finalize in nested scopes.
- ~~Full-suite crash on every run after the first~~ (2026-07-24) —
  `Asset '…/Tests/Monolith/UI/…' cannot be saved as it has only been partially loaded`,
  which `run_tests.ps1` surfaced as the misleading `passed=0, failed=-1,
  reason="UE report index.json missing"`. Cause: fixture-creating code called
  `CreatePackage()` + `UPackage::SavePackage()` without `Package->FullyLoad()`, so a
  fixture left on disk by the previous run was only half-loaded at save time. Fixed by
  routing `UIErrorFormattingTests::CreateScratchWBP` through the shared
  `MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint` helper (which already did
  the `FullyLoad()` + `FindObject` reclaim) and by adding the same reclaim to
  `FUISpecBuilder`'s get-or-create path — the latter is what the Roundtrip and
  SpecBuilder fixtures go through, and is a production bug too (`build_ui_from_spec`
  with `overwrite=true` over an unloaded existing asset). Manually deleting
  `Content/Tests/Monolith/UI/{ErrorFormatting,Roundtrip,SpecBuilder}` before each suite
  run is no longer necessary.
- ~~4 pre-existing test failures the SQLite crash was masking~~ — all four green as of
  2026-07-24. Their real causes, which differ from the guesses first recorded here:
  - `Monolith.ReflectionIntel.Decision.HeuristicAccuracy` — **not** simply "heuristic too
    broad". Two causes: a lookahead that ran past section boundaries, plus a fixture whose
    own prose spelled out the trigger tokens it was meant to be a negative case for.
  - `Monolith.CursorPagination.QueryMismatchRejection` — **not** "error lacks ErrorData".
    The rejection was never produced at all: the DB-availability guard ran ahead of the
    cursor-validation branch, so in a `-nullrhi` run (engine source DB closed) the handler
    returned "Engine source DB not available." before reaching `INVALID_CURSOR`.
  - `MonolithUI.Allowlist.UnknownTypeDenied` — the allowlist cache injected 7 common
    `UWidget` base properties for every token, registered or not.
  - `MonolithUI.Reflection.ParseLinearColorHex` — hex parse skipped the sRGB→linear
    degamma branch (dead code) for widget `FLinearColor` properties.

### Live caveats

- First `capture_scene_preview` right after boot renders very dark (preview
  scene lighting warmup); capability works, tune params per task.
- `run_tests.ps1` reports `passed=0, failed=-1, reason="UE report index.json missing"`
  for *any* editor crash, not just a missing report — read `logTail` in
  `results/run_tests.json` before blaming your own change.
- `exitCode 255` in `run_tests.json` only means "some test failed"; the suite still ran.
  Confirm real completion with
  `(Select-String -Path results\run_tests.log -Pattern 'Test Completed' -AllMatches).Count`.
- `results/` is a shared single slot (`run_tests.ps1` starts with
  `Stop-MonolithProcesses`). Harness runs must be sequential — two concurrent workers
  kill each other's editor and overwrite each other's verdict.
