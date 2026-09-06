# Completion runtime fixtures

These are temporary host-project test inputs, not plugin runtime dependencies.
The recorded run and its cleanup are documented in
[`Docs/COMPLETION_2026_09_06.md`](../../Docs/COMPLETION_2026_09_06.md).

The `.h.in` / `.cpp.in` pair defines a small GAS pawn, BT controller, abilities
and a runtime probe actor. Copy them into a dedicated C++ test project only after
recording and backing up its original Source, Config, Content, descriptor and
editor settings. The generated key/menu probes are included as `.inl` files.
Use `ui.scaffold_game_user_settings` to generate `UCompletionKeySettings` in
the host module; the fixture expects the resulting `CompletionKeySettings.h`.
Host dependencies include MonolithRuntime, MonolithAudioRuntime, GameplayAbilities,
GameplayTags, GameplayTasks, AIModule, UMG, Slate, SlateCore, EnhancedInput and CommonUI.
Compile with the required host plugins enabled, including GameplayAbilities.

`Scripts/validate_completion_runtime.py --mode prepare` checks the running MCP
project identity, acquires a lease and authors the isolated RecycleCo test assets.
It never starts the editor or copies host source automatically. Run the generated
map in PIE or as a separate cooked game process. In PIE, wait for the result
then call `editor.stop_pie`; the actor does not exit the editor. A separate game
process exits automatically. Each run writes
`Saved/MonolithCompletionRuntime.json`. Do not add this actor to a user map. Runtime audio tests must omit `-benchmark`,
which disables audio in UE; `-muteaudio` keeps processing active without audible output
in Development. Uncooked `-game` may regenerate widget classes without editor compiler
extensions, so use PIE and a cooked package for binding validation.

`Scripts/validate_completion_jobs.py` tests encoder jobs in a dedicated editor.
It owns GUID-named fixture assets/output directories and restores its starting map.

Before restoring host files, use `--mode cleanup` to remove the runtime assets,
close the editor/game, restore original files and remove test-created artifacts.
The snapshot-specific `restore_completion_host.ps1` validates target paths and
backup hashes, avoids junction traversal, and requires `-Apply` to perform restoration.
Rebuild the editor with original host configuration after removing fixture source;
do not retain DLLs built with temporary optional-plugin flags.
