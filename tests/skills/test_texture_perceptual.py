"""Skill-test loop TASK-10 — /texture + material authoring perceptual quality.

Validates that the harness's material-authoring primitives produce what they
claim to produce ON SCREEN — not just a success envelope — for the two graph
idioms the /texture skill family leans on:

- **Checkerboard (Custom-HLSL)**: an UNLIT material whose EmissiveColor is a
  Custom expression (`fmod(floor(u*N)+floor(v*N), 2)` parity) fed by a
  TextureCoordinate node. The critic must see a repeating pattern.
- **Solid-gray control**: an UNLIT material with a Constant3Vector 0.5-gray
  emissive. The critic must see a single solid color and NO pattern — the
  differential arm that a rubber-stamping critic fails.
- **Stripes (pure-node chain)**: TextureCoordinate → Multiply(×N) →
  ComponentMask(r) → Frac → If(>0.5 ? white : black) → EmissiveColor. This
  exercises the exact node chain TASKS.md names (including the ComponentMask
  channel select — masking r alone yields parallel stripes, never a checker,
  so "stripes yes / checkerboard no" is itself a differential pair).

Unlit + emissive deliberately sidesteps the editor-viewport-vs-PIE brightness
trap (see memory: visual-compare skill) — the pixels are the shader's output,
independent of scene lighting.

Rig (binding correction from the orchestrator, 2026-07-03): NOT
`pie_capture_from_pose` — its PrintWindow capture does not track the requested
pose (open bug, docs/BUGS.md). Instead the proven editor-viewport rig from
tests/skills/test_position_perceptual_directions.py:

  1. `level_new` from the engine's lit Default template (the fixture project's
     blank startup map renders pure black);
  2. camera pose set EXPLICITLY via the `py` console hatch
     (`UnrealEditorSubsystem().set_level_viewport_camera_info`) and verified
     through the independent read primitive `editor_viewport_get_camera`;
  3. `editor_screenshot mode=viewport` — real render pipeline, file confirmed
     server-side by the bridge (GAP-007), occlusion-immune.

One cube actor is reused across all three frames (materials swapped via
`material_apply_to_actor` between captures) so no second patterned surface can
ever contaminate a frame. No PIE. Exactly 6 Gemini calls per run (2 per
frame), temperature 0, pinned model. All assets live under
/Game/__MCPTest__/texperc; actors, assets, and screenshot files are cleaned in
the fixture's finally block.

Footgun encoded (verified in test_material.py): `material_set_expression_property`
gates on the `r` key — this test avoids the setter entirely by passing r/g/b at
expression-creation time, and never sends a g-only payload.
"""

from __future__ import annotations

import glob
import os
import time

import pytest

from harness.ops import assert_ready, ensure_absent
from harness.vision_critic import ask, requires_gemini

pytestmark = [pytest.mark.render, pytest.mark.perceptual, requires_gemini]

NS = "/Game/__MCPTest__/texperc"
MAT_CHECKER = f"{NS}/M_TexPercChecker"
MAT_GRAY = f"{NS}/M_TexPercGray"
MAT_STRIPE = f"{NS}/M_TexPercStripe"
ALL_MATS = (MAT_CHECKER, MAT_GRAY, MAT_STRIPE)

CUBE_OBJ = "/Engine/BasicShapes/Cube.Cube"
SMA_CLASS = "/Script/Engine.StaticMeshActor"
LIT_TEMPLATE = "/Engine/Maps/Templates/Template_Default"

ACTOR = "MCPSkillTest_TexPerc_Cube"
CUBE_LOC = [0.0, 0.0, 150.0]     # 3x-scaled 100-cube: bottom face on the floor
CUBE_SCALE = [3.0, 3.0, 3.0]
CAM = {"location": (-400.0, 0.0, 150.0), "pitch": 0.0, "yaw": 0.0}

TILES = 6.0  # checker/stripe frequency across one cube face

CHECKER_HLSL = (
    "float p = fmod(floor(uv.x * {n}) + floor(uv.y * {n}), 2.0); "
    "return float3(p, p, p);"
).format(n=TILES)


# ── rig helpers (idioms from test_position_perceptual_directions.py) ─────────

def _set_viewport_camera(client, location, pitch: float, yaw: float) -> None:
    """Explicit camera pose via the py hatch, verified through the independent
    read primitive. unreal.Rotator's positional ctor is (roll, pitch, yaw)."""
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
    """editor_screenshot viewport path: FScreenshotRequest through the real
    pipeline; the bridge confirms the file on disk (GAP-007)."""
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


def _remove_screenshot_files(basename: str) -> None:
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


def _pie_is_running(client) -> bool:
    r = client.call("pie_get_state", {})
    inner = r.get("result") if isinstance(r.get("result"), dict) else r
    return bool(inner.get("is_running"))


# ── material authoring ───────────────────────────────────────────────────────

def _expr_name(result: dict) -> str:
    name = result.get("expression_name") or result.get("name")
    assert name, f"material_add_expression returned no expression name: {result}"
    return name


def _add(client, mat: str, expr_type: str, **extra) -> str:
    payload = {"material_path": mat, "expression_type": expr_type}
    payload.update(extra)
    return _expr_name(client.expect("material_add_expression", payload))


def _connect(client, mat: str, src: str, dst: str, dst_input: str) -> None:
    client.expect("material_connect", {
        "material_path": mat,
        "source_expression": src,
        "target_expression": dst,
        "target_input": dst_input,
        "source_output_index": 0,
    })


def _finish(client, mat: str) -> None:
    client.expect("material_compile", {"material_path": mat})
    client.expect("asset_save", {"asset_paths": [mat]})


def _make_checker_material(client) -> None:
    """UNLIT checkerboard: TextureCoordinate → Custom(frac-parity) → Emissive."""
    client.expect("material_create", {
        "material_path": MAT_CHECKER, "shading_model": "Unlit",
    })
    uv = _add(client, MAT_CHECKER, "TextureCoordinate",
              position_x=-600.0, position_y=0.0)
    custom = _add(client, MAT_CHECKER, "Custom",
                  position_x=-300.0, position_y=0.0,
                  code=CHECKER_HLSL, output_type="Float3",
                  inputs=[{"name": "uv"}])
    _connect(client, MAT_CHECKER, uv, custom, "uv")
    _connect(client, MAT_CHECKER, custom, "Material", "EmissiveColor")
    _finish(client, MAT_CHECKER)


def _make_gray_material(client) -> None:
    """UNLIT solid mid-gray control (r/g/b all sent — the r-key footgun means a
    partial color payload can be a silent no-op)."""
    client.expect("material_create", {
        "material_path": MAT_GRAY, "shading_model": "Unlit",
    })
    gray = _add(client, MAT_GRAY, "Constant3Vector",
                position_x=-300.0, position_y=0.0, r=0.5, g=0.5, b=0.5)
    _connect(client, MAT_GRAY, gray, "Material", "EmissiveColor")
    _finish(client, MAT_GRAY)


def _make_stripe_material(client) -> None:
    """UNLIT stripes via the pure-node chain named in TASKS.md:
    TextureCoordinate → Multiply(×TILES) → ComponentMask(r) → Frac →
    If(frac > 0.5 ? white : black) → EmissiveColor. Masking r ALONE is what
    makes this stripes rather than a checker — the perceptual assertion
    'stripes, not checkerboard' therefore observes the channel select."""
    client.expect("material_create", {
        "material_path": MAT_STRIPE, "shading_model": "Unlit",
    })
    uv = _add(client, MAT_STRIPE, "TextureCoordinate",
              position_x=-1100.0, position_y=0.0)
    tiles = _add(client, MAT_STRIPE, "Constant",
                 position_x=-1100.0, position_y=150.0, value=TILES)
    mul = _add(client, MAT_STRIPE, "Multiply",
               position_x=-900.0, position_y=0.0)
    mask = _add(client, MAT_STRIPE, "ComponentMask",
                position_x=-700.0, position_y=0.0,
                r=True, g=False, b=False, a=False)
    frac = _add(client, MAT_STRIPE, "Frac",
                position_x=-500.0, position_y=0.0)
    half = _add(client, MAT_STRIPE, "Constant",
                position_x=-500.0, position_y=150.0, value=0.5)
    white = _add(client, MAT_STRIPE, "Constant3Vector",
                 position_x=-500.0, position_y=300.0, r=1.0, g=1.0, b=1.0)
    black = _add(client, MAT_STRIPE, "Constant3Vector",
                 position_x=-500.0, position_y=450.0, r=0.0, g=0.0, b=0.0)
    branch = _add(client, MAT_STRIPE, "If",
                  position_x=-300.0, position_y=0.0)

    _connect(client, MAT_STRIPE, uv, mul, "A")
    _connect(client, MAT_STRIPE, tiles, mul, "B")
    _connect(client, MAT_STRIPE, mul, mask, "Input")
    _connect(client, MAT_STRIPE, mask, frac, "Input")
    _connect(client, MAT_STRIPE, frac, branch, "A")
    _connect(client, MAT_STRIPE, half, branch, "B")
    _connect(client, MAT_STRIPE, white, branch, "AGreaterThanB")
    _connect(client, MAT_STRIPE, black, branch, "ALessThanB")
    _connect(client, MAT_STRIPE, black, branch, "AEqualsB")
    _connect(client, MAT_STRIPE, branch, "Material", "EmissiveColor")
    _finish(client, MAT_STRIPE)


# ── arrangement ──────────────────────────────────────────────────────────────

def _delete_actor_if_present(client, name: str) -> None:
    try:
        client.command("actor_delete", {"name": name})
    except Exception:
        pass


def _spawn_cube(client) -> str:
    _delete_actor_if_present(client, ACTOR)
    x, y, z = CUBE_LOC
    spawned = client.expect("actor_spawn", {
        "class_path": SMA_CLASS, "name": ACTOR,
        "location": {"x": x, "y": y, "z": z},
    })
    actor = spawned["actor"]["name"]
    client.expect("actor_set_property", {
        "name": actor, "property": "StaticMeshComponent.StaticMesh",
        "value": CUBE_OBJ,
    })
    client.expect("actor_set_transform", {
        "name": actor, "location": CUBE_LOC, "scale": CUBE_SCALE,
    })
    info = client.expect("mesh_get_actor_material_info", {"actor_name": actor})
    assert info.get("total_slots", 0) >= 1, info
    return actor


def _apply_and_capture(client, actor: str, mat: str, basename: str) -> str:
    applied = client.expect("material_apply_to_actor", {
        "actor_name": actor, "material_path": mat, "material_slot": 0,
    })
    assert applied.get("success") is not False, applied
    return _capture_viewport(client, basename)


# ── one arrangement, three frames ────────────────────────────────────────────

FRAME_BASENAMES = {
    "checker": "MCPTest_TexPerc_Checker",
    "gray": "MCPTest_TexPerc_Gray",
    "stripe": "MCPTest_TexPerc_Stripe",
}


@pytest.fixture(scope="module")
def frames(_mcp_client):
    """Open a lit template level, author the three unlit materials, swap them
    onto ONE cube under a fixed camera, capture a frame per material, then
    tear everything down. Yields {'checker': path, 'gray': path, 'stripe': path}."""
    client = _mcp_client
    if _pie_is_running(client):
        pytest.skip("PIE is running on the shared editor — this battery swaps "
                    "the editor level (level_new is PIE-blocked); not stopping "
                    "someone else's session")

    made = client.expect("level_new", {"template": LIT_TEMPLATE})
    assert made.get("created") is True, made

    try:
        for path in ALL_MATS:
            ensure_absent(client, path)
        _make_checker_material(client)
        _make_gray_material(client)
        _make_stripe_material(client)

        actor = _spawn_cube(client)
        _set_viewport_camera(client, CAM["location"], CAM["pitch"], CAM["yaw"])

        out = {}
        for key, mat in (("checker", MAT_CHECKER),
                         ("gray", MAT_GRAY),
                         ("stripe", MAT_STRIPE)):
            out[key] = _apply_and_capture(client, actor, mat,
                                          FRAME_BASENAMES[key])
        yield out
    finally:
        _delete_actor_if_present(client, ACTOR)
        for path in ALL_MATS:
            ensure_absent(client, path)
        for base in FRAME_BASENAMES.values():
            _remove_screenshot_files(base)
        assert_ready(client)


# ── perceptual assertions (6 Gemini calls; every frame carries a control) ────

Q_PATTERN = ("Does the large cube in the center of the image show a repeating "
             "pattern (such as a checkerboard or stripes) of alternating "
             "light and dark areas on its surface?")
Q_SOLID = ("Is the large cube in the center of the image a single uniform "
           "solid color, with no visible pattern, stripes, or checkerboard "
           "on its surface?")


def test_custom_hlsl_checker_reads_as_pattern(frames):
    """The Custom-HLSL frac-parity material must render an actual pattern —
    and must NOT read as a solid color (control arm on the same frame)."""
    img = frames["checker"]
    assert ask(img, Q_PATTERN) is True
    assert ask(img, Q_SOLID) is False


def test_solid_gray_control_reads_as_solid(frames):
    """The unlit gray control is the anti-rubber-stamp arm: the exact
    questions flip their expected answers on this frame."""
    img = frames["gray"]
    assert ask(img, Q_SOLID) is True
    assert ask(img, Q_PATTERN) is False


def test_pure_node_chain_reads_as_stripes_not_checker(frames):
    """The TextureCoordinate→Multiply→ComponentMask(r)→Frac→If chain must
    produce parallel STRIPES — and specifically NOT a checkerboard, which is
    what a broken/ignored ComponentMask channel select (both channels
    contributing) would look like."""
    img = frames["stripe"]
    assert ask(img, "Does the large cube in the center of the image show "
                    "parallel STRIPES of alternating light and dark bands "
                    "on its front face?") is True
    assert ask(img, "Does the large cube in the center of the image show a "
                    "CHECKERBOARD of alternating light and dark SQUARES "
                    "(a 2D grid, not parallel stripes) on its front "
                    "face?") is False
