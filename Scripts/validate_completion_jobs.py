"""Live encoder/undo smoke for a dedicated RecycleCo test editor.

Reuses validate_completion_runtime RPC/lease/evidence helpers. Run only after the
new editor modules have been built. Creates one GUID-owned Niagara duplicate,
uses a temporary unsaved map for undo/redo, restores the previous saved map,
and removes its own project asset and output directory in finally blocks.
No engine/build/HTTP activity occurs on import or --help.
"""
import argparse
import json
from pathlib import Path
import shutil
import time
import uuid

import validate_completion_runtime as rpc


MARKER = "MONOLITH_JOBS_JSON:"


def py_json(command):
    result = rpc.python(command)
    for row in reversed(result.get("output", [])):
        text = row.get("output", "")
        if MARKER in text:
            return json.loads(text.split(MARKER, 1)[1].strip())
    raise AssertionError(f"Missing structured Python evidence: {result}")


def owned_file(path, output_root):
    resolved = Path(path).resolve()
    assert resolved.is_relative_to(output_root), (resolved, output_root)
    assert resolved.is_file() and resolved.stat().st_size > 0, resolved
    return resolved


def clean_owned_files(output_root, run_id):
    # Windows recursive deletion stays entirely inside our exact GUID-owned
    # absolute output root. Never delete a path returned by the engine directly.
    expected = (rpc.EVIDENCE / "files").resolve()
    assert output_root == expected and output_root.is_relative_to(rpc.ROOT.resolve())
    marker = output_root / "owner.json"
    assert json.loads(marker.read_text(encoding="utf-8"))["run_id"] == run_id
    for entry in [output_root, *output_root.rglob("*")]:
        assert not entry.is_symlink(), entry
        assert not (hasattr(entry, "is_junction") and entry.is_junction()), entry
    shutil.rmtree(output_root)
    assert not output_root.exists()


def verify_capture(capture, output_root):
    paths = capture["frame_paths"]
    assert len(paths) == capture["frame_count"] and paths, capture
    for path in paths:
        file = owned_file(path, output_root)
        assert file.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", file
    directory = Path(capture["output_dir"]).resolve()
    assert directory.is_relative_to(output_root) and directory.name.startswith("encode_"), capture
    return directory


def poll(job_id, timeout=90):
    deadline = time.monotonic() + timeout
    history = []
    while time.monotonic() < deadline:
        status = rpc.act("editor", "get_job_status", job_id=job_id)
        history.append(status)
        assert status["job_id"] == job_id
        if status["done"]:
            result = rpc.act("editor", "get_job_result", job_id=job_id)
            return status, result, history
        time.sleep(0.1)
    raise TimeoutError(f"Encoder job {job_id} did not finish within {timeout}s")


def verify_gif(result, output_root):
    assert result["state"] == "completed" and result["done"], result
    assert result["exit_code"] == 0, result
    file = owned_file(result["gif_path"], output_root)
    assert file.read_bytes()[:6] in (b"GIF87a", b"GIF89a"), file
    # Pillow is used only for local artifact verification, never for engine image edits.
    from PIL import Image
    with Image.open(file) as image:
        assert image.width == 128 and image.height == 128, image.size
        assert image.n_frames >= 1
        frame_count = image.n_frames
        for index in range(frame_count):
            image.seek(index)
            image.load()
        duration = image.info.get("duration")
    return dict(path=str(file), bytes=file.stat().st_size, decoded_frames=frame_count,
                last_frame_duration_ms=duration)


def setup_undo(state_key, actor_label):
    # No prior dirty maps may be discarded. This is a dedicated smoke editor:
    # opening/closing the temporary map resets that map's transaction history.
    return py_json(f"""import unreal, builtins, json
assert not unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages(), 'Save your dedicated test map before undo smoke, or use --skip-undo'
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
original = world.get_outermost().get_name() if world else None
assert original and unreal.EditorAssetLibrary.does_asset_exist(original), 'Undo smoke requires a saved starting map (or --skip-undo)'
assert not hasattr(builtins, {state_key!r}), 'Unexpected existing fixture state'
setattr(builtins, {state_key!r}, dict(original_map=original, temporary_map_open=False, actor=None))
state = getattr(builtins, {state_key!r})
unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
state['temporary_map_open'] = True
actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0,0,0), unreal.Rotator(0,0,0), True)
assert actor
state['actor'] = actor
actor.set_actor_label({actor_label!r})
with unreal.ScopedEditorTransaction({('Monolith jobs ' + actor_label)!r}):
    actor.modify()
    actor.set_actor_label({(actor_label + '_changed')!r})
assert actor.get_actor_label() == {(actor_label + '_changed')!r}
print({MARKER!r} + json.dumps(dict(original_map=original, actor=actor.get_path_name(), label=actor.get_actor_label())))
""")


def assert_actor_label(state_key, label):
    return py_json(f"""import builtins, json
actor = getattr(builtins, {state_key!r})['actor']
assert actor and actor.get_actor_label() == {label!r}, actor.get_actor_label() if actor else 'missing actor'
print({MARKER!r} + json.dumps(dict(label=actor.get_actor_label())))
""")


def cleanup_engine(state_key, fixture):
    return py_json(f"""import unreal, builtins, json
state = getattr(builtins, {state_key!r}, None)
restored = None
if state:
    actor = state.get('actor')
    if actor:
        assert unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(actor)
    original = state['original_map']
    temporary_open = state['temporary_map_open']
    delattr(builtins, {state_key!r})
    actor = None
    state = None
    if temporary_open:
        unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
        assert unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(original), original
        restored = original
# The only asset deletion is the exact GUID path we created, never an enumerated
# project-wide folder or a user-provided source/template asset.
if unreal.EditorAssetLibrary.does_asset_exist({fixture!r}):
    assert unreal.EditorAssetLibrary.delete_asset({fixture!r}), {fixture!r}
assert not unreal.EditorAssetLibrary.does_asset_exist({fixture!r})
folder = {fixture.rsplit('/', 1)[0]!r}
assert not unreal.EditorAssetLibrary.list_assets(folder, recursive=True, include_folder=False)
if unreal.EditorAssetLibrary.does_directory_exist(folder):
    assert unreal.EditorAssetLibrary.delete_directory(folder), folder
print({MARKER!r} + json.dumps(dict(fixture_removed=True, restored_map=restored)))
""")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system", default="/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst",
                        help="Existing real Niagara system duplicated into this run's owned fixture")
    parser.add_argument("--encoders", nargs="+", choices=["python", "ffmpeg"], default=["python", "ffmpeg"])
    parser.add_argument("--skip-undo", action="store_true", help="Explicitly record undo/redo as untested; useful if starting editor map is dirty/unsaved")
    options = parser.parse_args()
    assert len(options.encoders) == len(set(options.encoders)), "Do not repeat encoders"
    run_id = uuid.uuid4().hex
    rpc.EVIDENCE = rpc.EVIDENCE / ("jobs_" + run_id)
    rpc.EVIDENCE.mkdir(parents=True, exist_ok=False)
    output_root = (rpc.EVIDENCE / "files").resolve()
    output_root.mkdir()
    (output_root / "owner.json").write_text(json.dumps(dict(run_id=run_id)), encoding="utf-8")
    fixture = "/Game/MonolithTests/CompletionJobs_" + run_id + "/NS_Capture"
    state_key = "monolith_completion_jobs_" + run_id
    actor_label = "MonolithJobs_" + run_id
    report = dict(run_id=run_id, fixture=fixture, source_system=options.system,
                  checks={}, jobs=[], cleanup={}, passed=False)
    all_jobs = []
    lease_acquired = False
    errors = []
    try:
        status = rpc.call("monolith_status")
        assert status["project_name"] == "RecycleCo", status
        rpc.TOKEN = rpc.call("monolith_coordination", operation="acquire", owner="completion-jobs-" + run_id,
                             ttl_seconds=600)["_lease_token"]
        lease_acquired = True
        for namespace, action in [("editor", "capture_system_gif"), ("editor", "get_job_status"),
                                  ("editor", "get_job_result"), ("editor", "cancel_job"),
                                  ("editor", "undo"), ("editor", "redo"), ("niagara", "create_system")]:
            rpc.act("describe", "action_schema", target_namespace=namespace, target_action=action)
        rpc.python(f"import unreal; assert not unreal.EditorAssetLibrary.does_asset_exist({fixture!r}); assert isinstance(unreal.load_asset({options.system!r}), unreal.NiagaraSystem)")
        rpc.act("niagara", "create_system", save_path=fixture, template=options.system)
        rpc.act("niagara", "request_compile", asset_path=fixture, force=False, synchronous=True)
        previous_dirs = set()
        for encoder in options.encoders:
            start = time.monotonic()
            capture = rpc.act("editor", "capture_system_gif", asset_path=fixture,
                              duration_seconds=0.3, fps=10, resolution=128,
                              output_path=str(output_root), encoder=encoder)
            job_id = capture["job_id"]
            all_jobs.append(job_id)
            directory = verify_capture(capture, output_root)
            assert directory not in previous_dirs, "Two captures share an input directory"
            previous_dirs.add(directory)
            status, result, history = poll(job_id)
            gif = verify_gif(result, output_root)
            assert result["capture"]["frame_paths"] == capture["frame_paths"]
            # Repeat result reads must remain stable and must not mutate capture JSON.
            assert rpc.act("editor", "get_job_result", job_id=job_id) == result
            assert not list(directory.glob(".monolith_encode_*")), "Temporary encoder files leaked"
            report["jobs"].append(dict(encoder=encoder, capture=capture, status=status, result=result,
                                       poll_history=history, gif=gif, elapsed_seconds=time.monotonic() - start))
        report["checks"]["positive_encoders"] = list(options.encoders)
        report["checks"]["unique_capture_directories"] = True
        # A fast encoder may finish before the cancel RPC reaches the editor. That
        # outcome proves terminal idempotence, never a false claim of live cancellation.
        capture = rpc.act("editor", "capture_system_gif", asset_path=fixture,
                          duration_seconds=0.3, fps=10, resolution=128,
                          output_path=str(output_root), encoder=options.encoders[0])
        job_id = capture["job_id"]; all_jobs.append(job_id)
        directory = verify_capture(capture, output_root)
        assert directory not in previous_dirs
        cancelled = rpc.act("editor", "cancel_job", job_id=job_id)
        assert cancelled["state"] in ("cancelled", "completed"), cancelled
        again = rpc.act("editor", "cancel_job", job_id=job_id)
        assert again == cancelled, (cancelled, again)
        terminal = rpc.act("editor", "get_job_result", job_id=job_id)
        assert terminal["done"] and terminal["state"] == cancelled["state"]
        assert terminal["capture"]["frame_paths"] == capture["frame_paths"]
        report["checks"]["cancellation"] = dict(observed_running_cancellation=cancelled["state"] == "cancelled",
                                                  outcome=cancelled["state"], idempotent=True)
        if options.skip_undo:
            report["checks"]["undo_redo"] = dict(tested=False, reason="explicit --skip-undo")
        else:
            setup = setup_undo(state_key, actor_label)
            undo = rpc.act("editor", "undo")
            assert undo["success"] and undo["can_redo"], undo
            undone = assert_actor_label(state_key, actor_label)
            redo = rpc.act("editor", "redo")
            assert redo["success"] and redo["can_undo"], redo
            redone = assert_actor_label(state_key, actor_label + "_changed")
            report["checks"]["undo_redo"] = dict(tested=True, setup=setup, undo=undo, undone=undone, redo=redo, redone=redone)
        report["passed"] = True
    except Exception as error:
        errors.append(repr(error))
    finally:
        jobs_terminal = True
        if lease_acquired:
            for job_id in all_jobs:
                try:
                    last = rpc.act("editor", "cancel_job", job_id=job_id)
                    assert last["done"], last
                except Exception as error:
                    jobs_terminal = False
                    errors.append("job cleanup: " + repr(error))
            try:
                report["cleanup"]["engine"] = cleanup_engine(state_key, fixture)
            except Exception as error:
                errors.append("engine cleanup: " + repr(error))
            try:
                rpc.call("monolith_coordination", operation="release", _lease_token=rpc.TOKEN)
            except Exception as error:
                errors.append("lease release: " + repr(error))
            rpc.TOKEN = None
        if jobs_terminal:
            try:
                clean_owned_files(output_root, run_id)
                report["cleanup"]["outputs_removed"] = True
            except Exception as error:
                errors.append("output cleanup: " + repr(error))
        else:
            report["cleanup"]["outputs_retained_due_to_unknown_process_state"] = str(output_root)
        report["errors"] = errors
        report["passed"] = report["passed"] and not errors
        (rpc.EVIDENCE / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(dict(passed=report["passed"], report=str(rpc.EVIDENCE / "report.json"), errors=errors), indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
