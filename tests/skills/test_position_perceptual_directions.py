"""Skill-test loop TASK-9 — /position directional semantics (perceptual).

Validates `.claude/skills/position/SKILL.md` §1.1's *perceptual* consequences —
"+Y is right of forward, +Z is up" — end to end through a real render:

- **Design A (left/right)**: a RED cube at (0, +150, 50) and a BLUE cube at
  (0, −150, 50); the fixed rig looks down +X from (−450, 0, 120). If the
  skill's handedness claim is true, the camera's screen-right is world +Y, so
  RED must land on the RIGHT half of the frame and BLUE on the LEFT. A sign
  flip in the convention (or in the rig) fails both.
- **Design B (up/down)**: the same two cubes moved to RED high (200, 0, 250)
  vs BLUE low (200, 0, 20), level camera at (−400, 0, 135). +Z up ⇒ RED
  renders ABOVE BLUE. (One pair of cubes is REUSED and moved between frames so
  a second red/blue pair can never contaminate the first frame's question.)

Rig choice (deviation from the original TASK-9 spec, deliberate): the spec
named `pie_capture_from_pose`, but live debugging (2026-07-03) showed its
PrintWindow capture does not track the requested pose — frames come back from
a fixed origin viewpoint no matter what pose is passed (details in
docs/loops/skills/PROPOSALS.md; implementation bug reported for BUGS.md, not
fixed here). The rig used instead is deterministic, navigation-free, and
occlusion-immune:

  1. `level_new` from the engine's lit Default template (the fixture project
     deliberately opens an EMPTY unlit Untitled map, which renders pure black
     — nothing perceptual can be asked about it);
  2. camera pose set EXPLICITLY via the sanctioned `py` console hatch
     (`UnrealEditorSubsystem().set_level_viewport_camera_info`) — zero
     stateful navigation, a computed pose per VERBOTEN rule 3 — and verified
     through the independent read primitive `editor_viewport_get_camera`;
  3. `editor_screenshot mode=viewport` — `FScreenshotRequest` through the real
     render pipeline, confirmed on disk by the bridge (GAP-007) even when the
     editor window is occluded (unlike the PrintWindow paths).

Arrangement is fully deterministic via typed primitives (actor_spawn +
reflection mesh assignment + material-instance coloring — the sanctioned
replacement for the DISABLED mesh_set_mesh_material_color, GAP-009; colors
drive BOTH BaseColor and EmissiveColor so verdicts don't hinge on lighting).
The verifier is the Gemini vision critic (tests/harness/vision_critic.py,
TASK-8): every assertion ships a control question with the OPPOSITE expected
answer, so a rubber-stamping critic fails the run instead of passing it.
5 Gemini calls per run, temperature 0, pinned model.

No PIE session is used (nothing to lease or leak). All waits are bounded, and
every actor + material asset + screenshot file is cleaned up in the fixture's
finally block.
"""

from __future__ import annotations

import glob
import os
import time

import pytest

from harness.ops import assert_ready, ensure_absent
from harness.vision_critic import ask, requires_gemini

pytestmark = [pytest.mark.render, pytest.mark.perceptual, requires_gemini]

NS = "/Game/__MCPTest__/posdir"
PARENT_MAT = f"{NS}/M_PosDirParent"
MI_RED = f"{NS}/MI_PosDirRed"
MI_BLUE = f"{NS}/MI_PosDirBlue"

CUBE_OBJ = "/Engine/BasicShapes/Cube.Cube"
SMA_CLASS = "/Script/Engine.StaticMeshActor"
LIT_TEMPLATE = "/Engine/Maps/Templates/Template_Default"

RED_ACTOR = "MCPSkillTest_PosDir_Red"
BLUE_ACTOR = "MCPSkillTest_PosDir_Blue"

# Design A: reference viewpoint at −X of the origin, facing +X (yaw 0).
A_RED_LOC = [0.0, 150.0, 50.0]     # +Y  → expected screen RIGHT
A_BLUE_LOC = [0.0, -150.0, 50.0]   # −Y  → expected screen LEFT
A_CAM = {"location": (-450.0, 0.0, 120.0), "pitch": -8.0, "yaw": 0.0}

# Design B: same cubes moved; level camera. RED high vs BLUE low.
B_RED_LOC = [200.0, 0.0, 250.0]    # +Z high → expected ABOVE
B_BLUE_LOC = [200.0, 0.0, 20.0]    # low     → expected BELOW
B_CAM = {"location": (-400.0, 0.0, 135.0), "pitch": 0.0, "yaw": 0.0}


# ── rig helpers ──────────────────────────────────────────────────────────────

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
    (real pipeline, works with an occluded window); the bridge confirms the
    file server-side before flipping status to 'captured' (GAP-007)."""
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


def _make_colored_instances(client) -> None:
    """Parent material: VectorParameter 'Tint' → BaseColor AND EmissiveColor
    (emissive keeps the color readable regardless of scene lighting).
    Instances override Tint pure red / pure blue. All under /Game/__MCPTest__."""
    for path in (MI_RED, MI_BLUE, PARENT_MAT):
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

    for inst, rgb in ((MI_RED, (1.0, 0.0, 0.0)), (MI_BLUE, (0.0, 0.0, 1.0))):
        client.expect("material_create_instance", {
            "asset_path": inst,
            "parent_material": PARENT_MAT,
            "force_overwrite": True,
        })
        client.expect("material_instance_set_parameter", {
            "instance_path": inst,
            "parameter_name": "Tint",
            "parameter_type": "vector",
            "r": rgb[0], "g": rgb[1], "b": rgb[2], "a": 1.0,
        })
        client.expect("asset_save", {"asset_paths": [inst]})


def _spawn_cube(client, name: str, location, mi_path: str) -> None:
    _delete_actor_if_present(client, name)
    x, y, z = location
    spawned = client.expect("actor_spawn", {
        "class_path": SMA_CLASS, "name": name,
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
        "actor_name": actor, "material_path": mi_path, "material_slot": 0,
    })
    assert applied.get("success") is not False, applied


def _pie_is_running(client) -> bool:
    r = client.call("pie_get_state", {})
    inner = r.get("result") if isinstance(r.get("result"), dict) else r
    return bool(inner.get("is_running"))


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


# ── one arrangement, two frames ──────────────────────────────────────────────

@pytest.fixture(scope="module")
def frames(_mcp_client):
    """Open a lit template level, arrange + capture both fixed-rig frames,
    tear everything down. Yields {'lr': <pathA>, 'ud': <pathB>}."""
    client = _mcp_client
    if _pie_is_running(client):
        pytest.skip("PIE is running on the shared editor — this battery swaps "
                    "the editor level (level_new is PIE-blocked); not stopping "
                    "someone else's session")

    # The fixture project opens an EMPTY unlit map that renders pure black;
    # replace the transient editor world with the engine's lit Default
    # template (floor + sun + sky). The new world is itself transient (/Temp).
    made = client.expect("level_new", {"template": LIT_TEMPLATE})
    assert made.get("created") is True, made

    try:
        _make_colored_instances(client)
        _spawn_cube(client, RED_ACTOR, A_RED_LOC, MI_RED)
        _spawn_cube(client, BLUE_ACTOR, A_BLUE_LOC, MI_BLUE)

        # Frame A — left/right. Camera behind the origin looking down +X.
        _set_viewport_camera(client, A_CAM["location"], A_CAM["pitch"], A_CAM["yaw"])
        lr = _capture_viewport(client, "MCPTest_PosDir_LeftRight")

        # Frame B — up/down. Move the SAME cubes (typed transform primitive),
        # so no second red/blue pair ever coexists with the first frame.
        client.expect("actor_set_transform", {"name": RED_ACTOR, "location": B_RED_LOC})
        client.expect("actor_set_transform", {"name": BLUE_ACTOR, "location": B_BLUE_LOC})
        _set_viewport_camera(client, B_CAM["location"], B_CAM["pitch"], B_CAM["yaw"])
        ud = _capture_viewport(client, "MCPTest_PosDir_UpDown")

        yield {"lr": lr, "ud": ud}
    finally:
        for name in (RED_ACTOR, BLUE_ACTOR):
            _delete_actor_if_present(client, name)
        for path in (MI_RED, MI_BLUE, PARENT_MAT):
            ensure_absent(client, path)
        for base in ("MCPTest_PosDir_LeftRight", "MCPTest_PosDir_UpDown"):
            _remove_screenshot_files(base)
        assert_ready(client)


# ── Design A: +Y renders screen-RIGHT of a +X-facing viewpoint ───────────────

def test_plus_y_is_screen_right_of_forward(frames):
    """SKILL.md §1.1: with forward=+X and +Z up, +Y is RIGHT. Camera looks down
    +X, RED sits at +Y, BLUE at −Y ⇒ RED right / BLUE left. The mirrored
    control (RED left?) must come back False — an always-yes critic fails."""
    img = frames["lr"]
    assert ask(img, "Is the RED cube in the RIGHT half of the image?") is True
    # Control arm (opposite expected answer — anti-rubber-stamp rule).
    assert ask(img, "Is the RED cube in the LEFT half of the image?") is False
    assert ask(img, "Is the BLUE cube in the LEFT half of the image?") is True


# ── Design B: +Z renders UP ──────────────────────────────────────────────────

def test_plus_z_is_screen_up(frames):
    """SKILL.md §1.1: +Z is up. RED at z=250 over BLUE at z=20, level camera ⇒
    RED above BLUE on screen; the inverted control must fail."""
    img = frames["ud"]
    assert ask(img, "Is the RED cube ABOVE the BLUE cube in the image?") is True
    # Control arm (opposite expected answer).
    assert ask(img, "Is the BLUE cube ABOVE the RED cube in the image?") is False
