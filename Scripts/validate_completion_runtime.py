"""Reproducible Monolith completion fixture authoring; requires temporary host C++ fixtures.

Use --mode prepare with RecycleCo Editor running, --mode cleanup before restoring host backup.
All RPC evidence is written under Monolith/Saved, never hidden in another project's Docs.
"""
import argparse
import json
from pathlib import Path
import time
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "Saved/Completion20260906"
BASE = "/Game/MonolithTests/Completion20260906"
TOKEN = None
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def call(name, **arguments):
    payload = dict(jsonrpc="2.0", id=str(uuid.uuid4()), method="tools/call",
                   params=dict(name=name, arguments=arguments))
    req = urllib.request.Request("http://127.0.0.1:9316/mcp", json.dumps(payload).encode(),
                                 {"Content-Type": "application/json", "Accept": "application/json"})
    with OPENER.open(req, timeout=180) as response:
        reply = json.load(response)
    with (EVIDENCE / "calls.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(dict(request=payload, response=reply)) + "\n")
    assert "error" not in reply, reply
    body = reply["result"]
    assert not body.get("isError"), body
    return json.loads(next(item["text"] for item in body["content"] if item["type"] == "text"))


def act(namespace, action, **params):
    if TOKEN:
        params["_lease_token"] = TOKEN
    return call(namespace + "_query", action=action, params=params)


def python(command):
    result = act("editor", "run_python", command=command, unattended=True)
    assert result.get("success"), result
    return result


def prepare():
    for ns, action in [("ui", "create_widget_blueprint"), ("ui", "add_widget"),
                       ("gas", "bind_widget_to_attribute"), ("ai", "create_behavior_tree"),
                       ("ai", "add_bt_node"), ("ai", "add_bt_use_ability_task"),
                       ("audio", "create_test_wave"), ("audio", "bind_sound_to_perception"),
                       ("editor", "run_python")]:
        act("describe", "action_schema", target_namespace=ns, target_action=action)
    python(f"import unreal; assert not unreal.EditorAssetLibrary.does_directory_exist({BASE!r}), 'Fixture already exists; inspect before retry'")
    widget = BASE + "/WBP_Runtime"
    tree = BASE + "/BT_Runtime"
    sound = BASE + "/SW_Runtime"
    act("ui", "create_widget_blueprint", save_path=widget, root_widget="VerticalBox")
    for name, policy in [("HealthBar", "on_change"), ("SmoothBar", "on_change_smoothed:6"), ("TickBar", "tick")]:
        act("ui", "add_widget", asset_path=widget, widget_class="ProgressBar", widget_name=name)
        act("gas", "bind_widget_to_attribute", wbp_path=widget, widget_name=name, target_property="Percent",
            attribute="CompletionVitals.Health", max_attribute="CompletionVitals.MaxHealth",
            owner_resolver="named_socket:CompletionOwner", update_policy=policy, save=True)
    act("ai", "create_behavior_tree", save_path=tree)
    composite = act("ai", "add_bt_node", asset_path=tree, node_class="BTComposite_Sequence")
    print("COMPOSITE", json.dumps(composite))
    parent = composite.get("node_id") or composite.get("guid") or composite.get("node_guid")
    assert parent, composite
    act("ai", "add_bt_use_ability_task", asset_path=tree, parent_id=parent,
        ability_class="/Script/RecycleCo.CompletionInstantAbility", wait_for_end=True)
    act("audio", "create_test_wave", path=sound, duration_seconds=2, amplitude=0.15)
    act("audio", "bind_sound_to_perception", asset_path=sound, tag="CompletionNoise", loudness=1,
        max_range=4000, save=True)
    act("describe", "action_schema", target_namespace="audio", target_action="create_metasound_source")
    act("audio", "create_metasound_source", asset_path=BASE + "/MS_Index", format="Mono", one_shot=True)
    act("describe", "action_schema", target_namespace="ui", target_action="build_menu_from_spec")
    act("ui", "build_menu_from_spec",
        screens=[dict(id="main", asset_path=BASE + "/WBP_MainMenu", kind="main_menu")],
        menu_asset_path=BASE + "/WBP_MenuHost",
        layers=[dict(id="MainStack", screens=["main"])],
        focus_table=[dict(screen="main", target="StartButton")],
        nav_overrides=[dict(screen="main", widget="StartButton", direction="down", target="SettingsButton"),
                       dict(screen="main", widget="SettingsButton", direction="up", target="StartButton")],
        overwrite=False)
    python(f"""import unreal
level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assert level.new_level({BASE + '/L_Runtime'!r})
actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(unreal.load_class(None, '/Script/RecycleCo.CompletionRuntimeProbe'), unreal.Vector(0,0,0))
actor.set_actor_label('MonolithCompletionProbe')
actor.set_editor_property('widget_class', unreal.EditorAssetLibrary.load_blueprint_class({widget!r}))
actor.set_editor_property('menu_class', unreal.EditorAssetLibrary.load_blueprint_class({BASE + '/WBP_MenuHost'!r}))
actor.set_editor_property('tree', unreal.load_asset({tree!r}))
actor.set_editor_property('sound', unreal.load_asset({sound!r}))
assert unreal.EditorAssetLibrary.save_directory({BASE!r}, only_if_is_dirty=False, recursive=True)
assert level.save_current_level()
print('COMPLETION_FIXTURE_SAVED')
""")
    print("PREPARED", BASE)


def cleanup():
    python(f"""import unreal
unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
paths = unreal.EditorAssetLibrary.list_assets({BASE!r}, recursive=True, include_folder=False)
for p in paths:
    assert unreal.EditorAssetLibrary.delete_asset(p), p
assert not unreal.EditorAssetLibrary.list_assets({BASE!r}, recursive=True, include_folder=False)
if unreal.EditorAssetLibrary.does_directory_exist({BASE!r}):
    assert unreal.EditorAssetLibrary.delete_directory({BASE!r})
print('COMPLETION_ASSET_CLEANUP_OK', len(paths))
""")


def main():
    global TOKEN
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["prepare", "cleanup"], required=True)
    options = parser.parse_args()
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    status = call("monolith_status")
    assert status["project_name"] == "RecycleCo", status
    TOKEN = call("monolith_coordination", operation="acquire", owner="completion-validation", ttl_seconds=600)["_lease_token"]
    try:
        globals()[options.mode]()
    finally:
        call("monolith_coordination", operation="release", _lease_token=TOKEN)


if __name__ == "__main__":
    main()
