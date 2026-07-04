"""Skill-test loop TASK-11 — /capture-pose framing differential (perceptual).

Validates the core claim behind `.claude/skills/capture-pose/SKILL.md`: that a
requested camera pose actually governs the captured pixels. The design is a
FRAMING DIFFERENTIAL — the anti-rubber-stamp pattern for capture claims:

- One vivid emissive RED cube at a known location in a lit template level.
- **Pose A** stands 300 units in front of the cube looking straight at it —
  the critic must say the cube is CENTERED.
- **Pose B** stands at the SAME location yawed 90° away — the critic must say
  the cube is ABSENT (EDGE tolerated). Two frames, opposite expected answers:
  a capture path that ignores the pose (or an always-agreeable critic) fails.

Two halves:

1. **Green half — the editor-viewport rig** (the sanctioned workaround from
   docs/BUGS.md "`pie_capture_from_pose` pixels do not track the requested
   pose"): the proven rig from tests/skills/test_position_perceptual_directions.py
   / test_texture_perceptual.py — `level_new` from `Template_Default`, camera
   pose set EXPLICITLY via the `py` hatch
   (`UnrealEditorSubsystem().set_level_viewport_camera_info`, verified through
   the independent read primitive `editor_viewport_get_camera`), then
   `editor_screenshot mode=viewport` (FScreenshotRequest through the real
   render pipeline, bridge-confirmed on disk, occlusion-immune). This proves
   camera-pose→pixels tracking end to end at the editor level and closes the
   iteration-2 capture-pose observability gap.
2. **xfail half — the same differential through `pie_capture_from_pose`**
   (pie_start → two pose captures → the same critic questions), marked
   `xfail(strict=False)` on the open BUGS.md defect: `MCPCaptureGameViewportToFile`
   (`MCPAutomationCommands.cpp:658`) PrintWindow-grabs the game viewport and
   returns stale fixed-viewpoint content regardless of the requested pose.
   The test self-unblocks (starts passing) when the capture path is fixed —
   if it unexpectedly PASSES, that is the signal the bug is closed.

PIE lease discipline (docs/USAGE.md §2.18): `pie_busy` is a queue position,
not a failure — `pie_start` is re-called to hold the place; `pie_stop` runs in
finally ONLY if this test acquired the lease.

Gemini budget: 4 calls per run (one `ask_choice` per frame, 2 editor + 2 PIE).
Each half is internally differential (opposite expected answers), satisfying
the anti-rubber-stamp rule without extra control calls.

Cleanup: actor + material assets + editor screenshots removed in finally; the
PIE captures land in pytest's tmp_path (auto-reaped).
"""

from __future__ import annotations

import glob
import os
import time

import pytest

from harness.ops import assert_ready, ensure_absent
from harness.vision_critic import ask_choice, requires_gemini

pytestmark = [pytest.mark.render, pytest.mark.perceptual, requires_gemini]

NS = "/Game/__MCPTest__/capturepose"
PARENT_MAT = f"{NS}/M_CapPoseParent"
MI_RED = f"{NS}/MI_CapPoseRed"

CUBE_OBJ = "/Engine/BasicShapes/Cube.Cube"
SMA_CLASS = "/Script/Engine.StaticMeshActor"
LIT_TEMPLATE = "/Engine/Maps/Templates/Template_Default"

CUBE_ACTOR = "MCPSkillTest_CapPose_Cube"

# The cube sits 300 units down +X of the camera stand; a 100-unit cube at that
# range fills a healthy centered chunk of a 90° frame.
CUBE_LOC = (400.0, 0.0, 60.0)
CAM_LOC = (100.0, 0.0, 60.0)
POSE_A = {"pitch": 0.0, "yaw": 0.0}    # facing +X, straight at the cube
POSE_B = {"pitch": 0.0, "yaw": 90.0}   # same stand, facing +Y — cube out of frame

FRAMING_Q = "Where is the vivid RED cube in this image?"
CHOICES = ("CENTERED", "EDGE", "ABSENT")

XFAIL_REASON = (
    "docs/BUGS.md '`pie_capture_from_pose` pixels do not track the requested "
    "pose' — PrintWindow capture (MCPCaptureGameViewportToFile, "
    "MCPAutomationCommands.cpp:658) returns stale fixed-viewpoint frames; the "
    "view-target swap itself works. strict=False so this half self-unblocks "
    "(XPASS) when the capture path is fixed."
)


# ── rig helpers (proven idioms from test_position_perceptual_directions.py) ──

def _set_viewport_camera(client, location, pitch: float, yaw: float) -> None:
    """Fixed capture rig: place the editor viewport camera at an EXPLICIT pose
    (no navigation), then verify the applied pose through the independent read
    primitive editor_viewport_get_camera. NOTE unreal.Rotator's positional
    ctor is (roll, pitch, yaw)."""
    x, y, z = location
    client.expect("editor_console_exec", {"command": (
        "py import unreal; "
        "unreal.UnrealEditorSubsystem().set_level_viewport_camera_info("
        f"unreal.Vector({x}, {y}, {z}), unreal.Rotator(0.0, {pitch}, {yaw}))"
    )})
    cam = client.expect("editor_viewport_get_camera", {})
    got_loc = cam["location"]
    for axis, want in zip(("x", "y", "z"), (x, y, z)):
        assert got_loc[axis] == pytest.approx(want, abs=1.0), cam
    assert cam["rotation"]["pitch"] == pytest.approx(pitch, abs=0.5), cam
    assert cam["rotation"]["yaw"] == pytest.approx(yaw, abs=0.5), cam


def _capture_viewport(client, filename: str) -> str:
    """editor_screenshot's viewport path renders through FScreenshotRequest
    (real pipeline, occlusion-immune); the bridge confirms the file
    server-side before flipping status to 'captured'."""
    result = client.expect("editor_screenshot", {
        "mode": "viewport", "filename": filename,
    })
    assert result.get("status") == "captured", result
    path = result.get("path") or result.get("file_path")
    assert path, result
    deadline = time.monotonic() + 15.0
    while not os.path.isfile(path) and time.monotonic() < deadline:
        time.sleep(0.25)
    assert os.path.isfile(path) and os.path.getsize(path) > 0, (path, result)
    return path


# ── arrangement helpers ──────────────────────────────────────────────────────

def _delete_actor_if_present(client, name: str) -> None:
    try:
        client.command("actor_delete", {"name": name})
    except Exception:
        pass


def _make_red_instance(client) -> None:
    """Parent material: VectorParameter 'Tint' → BaseColor AND EmissiveColor
    (emissive keeps the color vivid regardless of scene lighting); one pure-red
    instance. All under /Game/__MCPTest__."""
    for path in (MI_RED, PARENT_MAT):
        ensure_absent(client, path)
    client.expect("material_create", {"material_path": PARENT_MAT})
    added = client.expect("material_add_expression", {
        "material_path": PARENT_MAT,
        "expression_type": "VectorParameter",
        "parameter_name": "Tint",
    })
    tint = added.get("expression_name") or added.get("name")
    assert tint, added
    for target in ("BaseColor", "EmissiveColor"):
        client.expect("material_connect", {
            "material_path": PARENT_MAT,
            "source_expression": tint,
            "target_input": target,
            "target_expression": "Material",
            "source_output_index": 0,
        })
    client.expect("material_compile", {"material_path": PARENT_MAT})
    client.expect("asset_save", {"asset_paths": [PARENT_MAT]})

    client.expect("material_create_instance", {
        "asset_path": MI_RED,
        "parent_material": PARENT_MAT,
        "force_overwrite": True,
    })
    client.expect("material_instance_set_parameter", {
        "instance_path": MI_RED,
        "parameter_name": "Tint",
        "parameter_type": "vector",
        "r": 1.0, "g": 0.0, "b": 0.0, "a": 1.0,
    })
    client.expect("asset_save", {"asset_paths": [MI_RED]})


def _spawn_red_cube(client) -> None:
    _delete_actor_if_present(client, CUBE_ACTOR)
    x, y, z = CUBE_LOC
    spawned = client.expect("actor_spawn", {
        "class_path": SMA_CLASS, "name": CUBE_ACTOR,
        "location": {"x": x, "y": y, "z": z},
    })
    actor = spawned["actor"]["name"]
    client.expect("actor_set_property", {
        "name": actor, "property": "StaticMeshComponent.StaticMesh",
        "value": CUBE_OBJ,
    })
    # Independent readback that the mesh landed (a slot exists to paint).
    info = client.expect("mesh_get_actor_material_info", {"actor_name": actor})
    assert info.get("total_slots", 0) >= 1, info
    applied = client.expect("material_apply_to_actor", {
        "actor_name": actor, "material_path": MI_RED, "material_slot": 0,
    })
    assert applied.get("success") is not False, applied


def _pie_is_running(client) -> bool:
    r = client.call("pie_get_state", {})
    inner = r.get("result") if isinstance(r.get("result"), dict) else r
    return bool(inner.get("is_running"))


def _wait_for_pie(client, want_running: bool, timeout: float = 30.0) -> bool:
    """PIE start/stop is asynchronous — poll until is_running matches."""
    deadline = time.monotonic() + timeout
    state = {}
    while time.monotonic() < deadline:
        state = client.expect("pie_get_state", {})
        if bool(state.get("is_running")) == want_running:
            return want_running
        time.sleep(0.5)
    return bool(state.get("is_running"))


def _acquire_pie(client, requester: str, timeout: float = 120.0) -> None:
    """Acquire the shared PIE lease, honoring the queue protocol: pie_busy is
    a queue position, not a failure — re-call pie_start to hold the place
    until promoted or the deadline passes (docs/USAGE.md §2.18)."""
    deadline = time.monotonic() + timeout
    while True:
        resp = client.call("pie_start", {"requester": requester})
        # pie_start's success envelope is inconsistent across paths
        # ("success" vs "starting") — accept both (PROPOSALS.md, TASK-9).
        if resp.get("status") in ("success", "starting"):
            return
        if resp.get("error_code") != "pie_busy" or time.monotonic() >= deadline:
            pytest.fail(f"could not acquire the PIE lease: {resp}")
        time.sleep(2.0)


def _remove_screenshot_files(basename: str) -> None:
    """Best-effort cleanup of our namespaced captures from Saved/Screenshots."""
    project = os.environ.get(
        "UE_MCP_TEST_PROJECT",
        os.path.abspath(os.path.join(os.path.dirname(__file__),
                                     "..", "fixtures", "TestProject")))
    pattern = os.path.join(project, "Saved", "Screenshots", "**", basename + "*.png")
    for p in glob.glob(pattern, recursive=True):
        try:
            os.remove(p)
        except OSError:
            pass


# ── shared arrangement: lit level + one red cube (module-scoped) ─────────────

@pytest.fixture(scope="module")
def rig(_mcp_client):
    """Open a lit template level and place the red cube; tear everything down
    after both halves. Yields the client (frames are captured per-test so the
    PIE half can reuse the identical arrangement)."""
    client = _mcp_client
    if _pie_is_running(client):
        pytest.skip("PIE is running on the shared editor — this battery swaps "
                    "the editor level (level_new is PIE-blocked); not stopping "
                    "someone else's session")

    # The fixture project opens an EMPTY unlit map that renders pure black;
    # replace it with the engine's lit Default template (floor + sun + sky).
    made = client.expect("level_new", {"template": LIT_TEMPLATE})
    assert made.get("created") is True, made

    try:
        _make_red_instance(client)
        _spawn_red_cube(client)
        yield client
    finally:
        _delete_actor_if_present(client, CUBE_ACTOR)
        for path in (MI_RED, PARENT_MAT):
            ensure_absent(client, path)
        for base in ("MCPTest_CapPose_AtCube", "MCPTest_CapPose_Away"):
            _remove_screenshot_files(base)
        assert_ready(client)


# ── green half: the editor-viewport rig honors the pose ─────────────────────

def test_editor_viewport_rig_framing_tracks_pose(rig):
    """Pose A (aimed at the cube) must frame it CENTERED; pose B (same stand,
    yawed 90° away) must lose it (ABSENT, EDGE tolerated). Opposite expected
    answers on the two frames = built-in anti-rubber-stamp differential."""
    client = rig

    _set_viewport_camera(client, CAM_LOC, POSE_A["pitch"], POSE_A["yaw"])
    frame_a = _capture_viewport(client, "MCPTest_CapPose_AtCube")

    _set_viewport_camera(client, CAM_LOC, POSE_B["pitch"], POSE_B["yaw"])
    frame_b = _capture_viewport(client, "MCPTest_CapPose_Away")

    verdict_a = ask_choice(frame_a, FRAMING_Q, CHOICES)
    verdict_b = ask_choice(frame_b, FRAMING_Q, CHOICES)
    assert verdict_a == "CENTERED", (verdict_a, frame_a)
    assert verdict_b in ("ABSENT", "EDGE"), (verdict_b, frame_b)
    # The differential itself: the two poses must NOT produce the same verdict.
    assert verdict_a != verdict_b, (verdict_a, verdict_b)


# ── xfail half: the same differential through pie_capture_from_pose ─────────

@pytest.mark.xfail(strict=False, reason=XFAIL_REASON)
def test_pie_capture_from_pose_framing_tracks_pose(rig, tmp_path):
    """Identical differential, captured inside PIE via pie_capture_from_pose.
    Mechanical assertions first (status/file), then the perceptual verdicts.
    Currently expected to fail on the open BUGS.md PrintWindow defect; an
    unexpected PASS (XPASS) means the capture path has been fixed."""
    client = rig
    acquired = False
    try:
        _acquire_pie(client, requester="pytest capture-pose framing (TASK-11)")
        acquired = True
        assert _wait_for_pie(client, want_running=True), "PIE never reached is_running"

        frames = {}
        for tag, pose in (("at_cube", POSE_A), ("away", POSE_B)):
            result = client.expect("pie_capture_from_pose", {
                "location": {"x": CAM_LOC[0], "y": CAM_LOC[1], "z": CAM_LOC[2]},
                "rotation": {"pitch": pose["pitch"], "yaw": pose["yaw"], "roll": 0.0},
                "fov": 90.0,
                "filename": f"pytest_capframe_{tag}",
                "directory": str(tmp_path),
            })
            # Mechanical half (per the capture-pose proposal): the op reported
            # a capture and the file really exists — this part is NOT expected
            # to fail even with the open bug.
            assert result.get("status") == "captured", result
            path = result.get("path") or result.get("file_path")
            assert path and os.path.isfile(path), (path, result)
            assert os.path.getsize(path) > 1000, os.path.getsize(path)
            frames[tag] = path

        # Perceptual half: the pose differential. This is where the open
        # PrintWindow bug bites (both frames show the same fixed viewpoint).
        verdict_a = ask_choice(frames["at_cube"], FRAMING_Q, CHOICES)
        verdict_b = ask_choice(frames["away"], FRAMING_Q, CHOICES)
        assert verdict_a == "CENTERED", (verdict_a, frames["at_cube"])
        assert verdict_b in ("ABSENT", "EDGE"), (verdict_b, frames["away"])
        assert verdict_a != verdict_b, (verdict_a, verdict_b)
    finally:
        if acquired:
            client.command("pie_stop", {})
            _wait_for_pie(client, want_running=False)
        assert_ready(client)
