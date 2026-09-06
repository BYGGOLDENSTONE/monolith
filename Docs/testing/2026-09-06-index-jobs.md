# Index coverage and editor encoding jobs — 2026-09-06

The coordinated RecycleCo Editor build 8 succeeded. Automation5 completed with **173 tests: 154 clean successes, 19 successes with warnings, zero failures, zero not-run tests**. The eight earlier index/jobs/discovery tests passed cleanly; the additional BehaviorTree ownership regression passed with 12 optional-field JSON warnings, detailed below. Live Python/ffmpeg encoding, observed running-job cancellation, real actor undo/redo, and this smoke's fixture/output cleanup also passed. The [canonical completion report](../COMPLETION_2026_09_06.md) records the final whole-project restoration and overall outcome.

## Index behavior

- Full-index postpasses now dispatch the registered GAS, optional MetaSound, and legacy AI summary indexers through the compiler-idle game-thread gate. Their previous registration alone never executed them.
- Failed sentinel return values roll back their enclosing transaction and prevent a full-index completion marker.
- GAS replacement clears only its four node types, preserving Blueprint graphs on the same assets. MetaSound replacement clears its own graph rows/variables, preserving generic metadata. Foreign-key cascades remove associated connections. This makes repeated/resumed passes safe against duplicate rows; rollback retains the previous data.
- Native AttributeSets now receive a real synthetic asset row keyed by the native class path. The previous `asset_id=-1` violated the node foreign key.
- New per-asset AI indexer covers BehaviorTree roots, ordered child edges, decorators/services, blackboard reference; BlackboardData local/inherited keys with child overrides; EnvQuery options, generators, and tests. Editable node properties are exported read-only. Registration follows both `bEnableAI` and `bIndexAI`. Existing per-asset full/live dispatch provides its lifecycle.
- Existing complete databases need a forced full index to obtain the previously omitted sentinel data. GAS/MetaSound remain full-pass indexers; this change does not promise incremental cross-asset updates or per-sentinel checkpoints.

## Encoder behavior and compatibility

`editor.capture_system_gif` still captures/simulates frames synchronously on the game thread. `frames_only` returns those paths as before. External ffmpeg/python encoding now starts a hidden subprocess without waiting and returns `job_id`, `encoding_state`, and `encoding_job`. Callers requesting an encoded GIF must poll rather than expecting an immediate top-level `gif_path`.

```json
{"action":"get_job_status","job_id":"<capture response job_id>"}
{"action":"get_job_result","job_id":"<capture response job_id>"}
{"action":"cancel_job","job_id":"<capture response job_id>"}
```

States: `running`, `completed`, `failed`, `cancelled`, `timed_out`. Result retrieval while running returns a clear error. A terminal result includes the captured PNG paths and, on completed encoding, `gif_path`. Unknown/expired IDs fail. Cancellation is idempotent. Records live only in the editor session; at most 64 are retained, with oldest terminal records evicted. At most four processes run simultaneously. A 300-second timeout and module shutdown terminate running encoder processes. No worker holds UObjects or module callbacks after shutdown.

Each encoded capture gets a unique `encode_<GUID>` directory beneath the requested output directory, preventing concurrent requests from overwriting input frames. GIF outputs are also unique. Encoder manifests/scripts are deleted after completion, cancellation, timeout, or launch failure; captured PNGs remain available. The subprocess does not pipe unlimited output into memory; terminal failures report the exit code and dependency guidance.

ffmpeg consumes a UTF-8 concat manifest containing the actual returned filenames (capture names include timestamps; the former `%04d` input pattern did not match). Python/Pillow consumes a UTF-8 JSON manifest (explicit millisecond frame durations) and a fixed script; paths are never interpolated into Python code. Unknown encoder names are rejected before loading/capturing assets. Per-frame failures propagate rather than being reported as encoded success. Capture is bounded to 600 frames and 2048 pixels per side.

## Undo/redo

New `editor.undo` and `editor.redo` apply the latest global editor transaction, including manually created transactions. They refuse during PIE or an active transaction and return remaining undo/redo availability. These actions do not save packages or provide a plugin-exclusive history.

## Verification

Initially verified in `Saved/Completion20260906/Automation4/index.json` (171 tests), then rerun successfully in `Automation5/index.json`, report time `2026.09.06-10.05.11` (173 tests, UE 5.7.4, coordinated Editor build 8). The full suite's 19 warning-bearing successes are separate from the eight clean passing tests below. No failed or unexecuted tests occurred in either run.

Index/jobs automation filters — all **Success, zero warnings, zero errors**:

- `Monolith.Index.Recovery.SentinelOwnedRows`: real temporary SQLite fixture; preserves unrelated nodes/variables, cascades GAS edges, restores data on rollback, repeated clearing.
- `Monolith.Index.AI.Structure`: transient BehaviorTree, inherited/overridden Blackboard keys, and EnvQuery topology; no saved project assets.
- `Monolith.Editor.Jobs.Validation`: missing IDs, invalid encoder before asset loading, empty frame prevention.
- `Monolith.AI.Index.LegacySentinelRecovery`: summary ownership, private-table reset/rollback, write-failure propagation, idempotence and per-database schema lifecycle.
- `Monolith.AI.Discovery.BehaviorTreeDefaults`: existing BehaviorTree discovery contract.
- `Monolith.AI.Discovery.EQSNativeTypes`: known native generator/test/context types, category isolation and reflected class identity.
- `Monolith.AI.Discovery.StateTreeNativeTypes`: task/evaluator/condition struct discovery when compiled in; typed dependency error when disabled.
- `Monolith.AI.Discovery.InvalidInputs`: system/category validation across BT, EQS and StateTree.

Source checks executed: `py -3.12 Scripts/check_repo_lint.py` passed (1,585 action registrations, zero schema drift/errors, 1,074 skill references); `git diff --check -- Source/MonolithIndex Source/MonolithEditor` passed. A first invocation using PATH Python 3.10 could not import `tomllib`; rerunning with installed Python 3.12 passed. An extracted production Python/Pillow encoder script was also executed with temporary PNGs under an apostrophe/Unicode path: two GIF frames and 200 ms duration were verified; temporary outputs were removed. The coordinated Editor builds and Automation4/Automation5 runs subsequently passed as recorded above. The separate live jobs smoke subsequently verified real encoder subprocesses and successful editor undo/redo as detailed below.

Remaining practical limits: frame simulation/capture can still occupy the game thread; only external encoding became asynchronous. Encoders must be installed on PATH, with Pillow for Python. Job state is not durable across editor restart. AI structure is an index representation, not execution/behavior verification, and does not yet serialize Boolean decorator operation expressions as explicit graph nodes.


## AI indexer ownership review

The per-asset class is named `FMonolithAIAssetIndexer` to avoid an ODR/name collision with the pre-existing global `FAIIndexer` implementation in the separate `MonolithAI` module. The existing sentinel registers `__AI__` and writes BT/BB summary nodes, controller perception/team metadata, and private `ai_assets`, `ai_bb_keys`, `ai_cross_refs` tables. Its full-pass sentinel dispatch was also absent. It does not walk full BT node structure, inherited key overrides, or EQS graphs; the new per-asset implementation supplies those independently and participates in the existing per-asset checkpoint/live-index path. These are complementary representations, not equivalent duplicate implementations.

The follow-up now dispatches `__AI__` through the same compiler-idle, caller-owned transaction gate. Its summaries have the exclusive `AIAssetSummary` node type; reset removes those plus precisely identified legacy summary rows while preserving the per-asset structure graph. Schema creation runs for every database/pass rather than caching a stale object-level flag. Schema/reset/insert/reference failures propagate and cause dispatcher rollback. Cross-references are resolved after all catalog rows exist, removing AssetRegistry-order dependence for Blackboard inheritance. The sentinel remains a full-pass summary/cross-reference index, complementary to the per-asset live structure representation.

Added `Monolith.AI.Index.LegacySentinelRecovery`: temporary SQLite test covers old/new summary ownership, unrelated graph preservation, private table reset and rollback, injected write failure propagation, repeated passes, and switching databases. This test passed cleanly in Automation4, including the injected failure/rollback assertions.

The first `Monolith.Index.AI.Structure` runtime attempt exposed a fixture assertion error: UE automatically inserts the persistent SelfActor key in `UBlackboardData::PostInitProperties`, so the test's hard-coded total of two was incorrect. The updated fixture verifies the exact union of inherited/local/engine-default key identities, no duplicates, and a child Float overriding a parent Int with the correct ownership category. This corrects the test expectation without suppressing engine-defined keys in production indexing.


## Repeatable live jobs smoke

`python Scripts/validate_completion_jobs.py` reuses the completion RPC/coordination helper. Use a Python installation with Pillow available for local GIF verification; this machine's successful run used PATH Python 3.10 with Pillow 12.1. The editor-launched Python encoder likewise needs Pillow on its PATH Python. It requires a dedicated RecycleCo editor with the new modules loaded and a saved, clean starting map for undo/redo. It duplicates the installed DirectionalBurst Niagara template into a GUID-owned fixture; captures small PNG sequences, verifies Python/Pillow and ffmpeg GIFs, polls status/result, checks input/output isolation, and attempts immediate cancellation. A process that already completed is explicitly reported as a cancellation race rather than a passed running-cancel test.

Undo/redo uses a uniquely labelled actor in a temporary unsaved map and verifies its label before/after each action, then restores the previous saved map. `--skip-undo` explicitly records that coverage as untested; `--system` can select another real Niagara template. The script deletes only its exact fixture asset/empty owned folder and GUID output tree, retaining RPC evidence and the JSON report under `Monolith/Saved/Completion20260906/jobs_<GUID>`. Output cleanup is deferred if encoder process state cannot be confirmed terminal. The script was subsequently executed successfully in the coordinated live editor run described below.


## AI node discovery

`ai.list_ai_node_types` now returns loaded concrete EQS generator/test/context classes and loaded StateTree task/evaluator/condition structs. StateTree uses the existing compile-time dependency gate and returns `OptionalDepUnavailable` when unavailable. Class/struct paths and a `kind` field identify the reflected representation. Abstract/deprecated classes, hidden structs and reinstancing artifacts are excluded. The BT branch retains its previous response contract. This is loaded-type discovery, not Blueprint asset loading or per-schema StateTree node-picker filtering. The four discovery tests passed cleanly in Automation4.


## Observed live jobs result

Evidence: `Saved/Completion20260906/jobs_395214cb06aa429bbd0a2416357d2d9b/report.json` and its `calls.jsonl`. Result: `passed: true`, no errors.

- Python/Pillow: valid decoded 128×128 GIF, three frames, 100 ms recorded last-frame duration, 795 bytes; capture/encode/check took approximately 0.406 s.
- ffmpeg: valid decoded 128×128 GIF, three frames, 100 ms recorded last-frame duration, 1,489 bytes; capture/encode/check took approximately 0.453 s.
- Both encoders returned stable job results and preserved the captured frame paths. Input directories were distinct despite the same requested output root.
- A running encoder was actually cancelled (`observed_running_cancellation: true`, state `cancelled`); repeated cancellation was idempotent. This was not an already-completed cancellation race.
- Undo changed the test-owned actor label from its `_changed` value back to the original; redo restored `_changed`. Availability flags reflected the history: undo reported redo available, and the final redo reported no remaining redo.
- The exact Niagara fixture and output tree were removed. The temporary map/actor were discarded and `/Game/MonolithTests/Completion20260906/L_Runtime` was restored. This is the jobs smoke's local cleanup result; whole-RecycleCo restoration belongs to the canonical completion report.

The first live script invocation used Python 3.12, which lacked Pillow for local GIF validation. The encoder itself worked, but that validation import failed; its fixture and outputs were cleaned. Rerunning with PATH Python 3.10/Pillow 12.1 passed in roughly two seconds. This environment mismatch is recorded separately from encoder correctness.


## BehaviorTree cook ownership follow-up

A later real Development-package run (`runtime-development.log`) found two failures despite the earlier Editor automation success: `cooked_behavior_tree` and `task_runtime_module`. The asset existed, but its root composite had an editor graph node as Outer. UE's `UBehaviorTreeGraph::CreateBTFromGraph` assigns/initializes the root without reparenting it; child nodes are reparented by `CreateChildren`. Cooking strips the editor graph ownership chain, so the root and reachable task disappeared.

All runtime BT node construction paths now use the `UBehaviorTree` asset as Outer, with transactional flags: manual nodes/decorators/services, spec builder, and EQS/SmartObject/GAS task helpers. A write-only ownership repair also moves legacy graph-owned runtime nodes under their BT asset before rebuilding/duplicating, using collision-safe names. Read-only loading does not rewrite existing assets; applying a BT edit/rebuild and saving repairs legacy content for its next cook.

Added `Monolith.AI.BehaviorTree.RuntimeOwnership`, which exercises real authoring actions, verifies root/task/decorator/service outer chains have no editor-only object, exercises spec construction, and simulates then repairs an old graph-owned root. The ownership follow-up compiled in Editor build 8 and passed `Monolith.AI.BehaviorTree.RuntimeOwnership` in Automation5 (0.597 s, zero errors). Its log contains **12 warnings**: missing optional `name`, `blackboard_path`, and `parent_id` fields and the related null-as-string messages. These warnings were not ownership assertion failures and are retained in the report; this test is not described as a clean pass. The full Automation5 run had 173 successes: 154 clean and 19 warning-bearing, zero failures/not-run. The subsequent reauthored Development and Shipping packages both passed all 24 runtime checks, including the cooked BT root and task module checks (see final package evidence below). Source schema/whitespace checks passed. No engine or host changes were made by the source author.


## Live full-index and PIE confirmation

The final positive full index completed with `indexing: false`: **9 assets (7 project assets + 2 native assets), 33 nodes, 9 connections, zero skipped assets**. Evidence: `Saved/Completion20260906/index-live-2.json` and the accompanying live RPC records.

The actual `MS_Index` MetaSoundSource produced ten index nodes: one Asset, one Page, four graph Nodes, and four Dependencies. This positively exercises the newly dispatched MetaSound sentinel; it is not an empty-project or registration-only check. `BT_Runtime` produced four rows covering the tree/root-composite/task structure and the complementary `AIAssetSummary`, confirming the per-asset and legacy full-pass representations both populated.

After reauthoring the BehaviorTree with corrected runtime ownership, normal PIE passed **24/24 runtime checks** (three hearing events observed). PIE was stopped and the editor exited cleanly. This confirms the repaired BT/GAS behavior in PIE. The subsequent Development and Shipping runs also passed, as recorded below; the canonical completion report owns final project-restoration status.


## Final packaged BehaviorTree verification

The reauthored **Development package (second run) and Shipping package both passed 24/24 runtime checks**, with zero failures, three observed hearing events, and process exit code 0. `cooked_behavior_tree` and `task_runtime_module` now pass in both builds, resolving the earlier editor-owned root stripping failure with actual cooked-game evidence rather than an Editor-only assertion.

Build/cook evidence: `Saved/Completion20260906/package-development-2.log` and `package-shipping.log`: both builds succeeded; cook reported zero errors and zero warnings. The canonical completion report records the paired runtime logs and the broader runtime behavior checks.

Whole-RecycleCo restoration is now verified in the canonical completion report: 22/22 preserved file hashes match, no new host files or Content assets remain, and the original host configuration passed the final 173-test automation run. The clean host index was rebuilt without fixture records.
