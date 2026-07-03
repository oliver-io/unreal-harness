"""Skill-test loop TASK-1 — /position coordinate-convention battery.

Pins the numeric convention claims made by `.claude/skills/position/SKILL.md`
against the live editor, so the skill's "non-negotiable UE conventions" (§1)
are machine-checked instead of asserted from memory:

1. **Actor forward is +X; +yaw rotates forward toward +Y** (SKILL.md §1.1).
   Arrange with ``actor_set_transform`` (yaw 0, then yaw 90) on a spawned
   SkeletalMeshActor; observe with ``kinematics_read_transform`` — the engine
   itself computes ``forward_world`` as the +X axis of the world rotation
   (MCPKinematicsCommands.cpp:548, ``W.GetRotation().GetAxisX()``).

2. **Default ACharacter capsule is radius 34 / half-height 88** (SKILL.md
   §"Actor & the default Character"). Engine-source oracle verified this run:
   ``Engine/Source/Runtime/Engine/Private/Character.cpp:77``
   ``CapsuleComponent->InitCapsuleSize(34.0f, 88.0f);``. Arrange with
   ``actor_spawn`` of /Script/Engine.Character; observe with a NON-mutating
   reflective export — ``actor_set_property`` in ``dry_run`` mode returns the
   leaf's ``before`` text without writing (MCPEditorCommands.cpp:645-690).

3. **Composition identity: World == Relative * ComponentToWorld (LHS-first)**
   (SKILL.md §1.3). Engine-source oracle: ``Transform.isph:199-209`` —
   ``Q(AxB) = Q(B)*Q(A)`` and ``T(AxB) = Q(B)*S(B)*T(A)*-Q(B) + T(B)``.
   The tiny quaternion helpers below mirror the engine formulas verbatim:
   FRotator→FQuat from UnrealMath.cpp:461-465, RotateVector from
   Quat.h:1238-1250.

Headless-safe (no render/gui_only markers), bounded, and self-cleaning: every
actor spawned here is deleted in fixture teardown / finally. The kinematics
tools resolve the PIE world when PIE is live (MCPKinematicsCommands.cpp
GetTargetWorld) — our actors live in the EDITOR world, so tests skip (never
disrupt someone else's session) if PIE is running on the shared editor.
"""

import math

import pytest

from harness.ops import payload

SKEL_ACTOR_CLASS = "/Script/Engine.SkeletalMeshActor"
SKEL_MESH = "/Engine/EngineMeshes/SkeletalCube"
CHARACTER_CLASS = "/Script/Engine.Character"
FWD_ACTOR = "MCPSkillTest_PosConv_Skel"
CHAR_ACTOR = "MCPSkillTest_PosConv_Char"


# ── engine-formula helpers (verified against UE 5.7 source this task) ────────

def _v(o):
    return (o["x"], o["y"], o["z"])


def _rot_to_quat(pitch, yaw, roll):
    """FRotator→FQuat, exactly UnrealMath.cpp:453-465 (x, y, z, w)."""
    half = math.pi / 360.0  # deg → rad, halved
    sp, cp = math.sin(pitch * half), math.cos(pitch * half)
    sy, cy = math.sin(yaw * half), math.cos(yaw * half)
    sr, cr = math.sin(roll * half), math.cos(roll * half)
    return (
        cr * sp * sy - sr * cp * cy,
        -cr * sp * cy - sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    )


def _quat_mul(a, b):
    """Hamilton product a⊗b (apply b's rotation first, then a's)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def _quat_rotate(q, v):
    """FQuat::RotateVector, exactly Quat.h:1238-1250: V' = V + 2w(QxV) + 2Qx(QxV)."""
    qv = q[:3]
    w = q[3]

    def cross(a, b):
        return (a[1] * b[2] - a[2] * b[1],
                a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0])

    t = tuple(2.0 * c for c in cross(qv, v))
    ct = cross(qv, t)
    return tuple(v[i] + w * t[i] + ct[i] for i in range(3))


# ── shared arrangement ───────────────────────────────────────────────────────

def _pie_is_running(client) -> bool:
    r = client.call("pie_get_state", {})
    inner = r.get("result") if isinstance(r.get("result"), dict) else r
    return bool(inner.get("is_running"))


def _delete_actor_if_present(client, name):
    try:
        client.command("actor_delete", {"name": name})
    except Exception:
        pass


@pytest.fixture(scope="module")
def skel_actor(_mcp_client):
    """SkeletalMeshActor + SkeletalCube in the transient editor level (the
    posed_actor pattern from tests/integration/test_kinematics.py). Deleted in
    teardown — the shared editor may outlive this run, so we never rely on
    editor-quit cleanup alone."""
    if _pie_is_running(_mcp_client):
        pytest.skip("PIE is running on the shared editor — kinematics_* resolves "
                    "the PIE world, so our editor-world actor would not be found; "
                    "not stopping someone else's session")

    _delete_actor_if_present(_mcp_client, FWD_ACTOR)
    spawned = _mcp_client.expect("actor_spawn", {
        "class_path": SKEL_ACTOR_CLASS, "name": FWD_ACTOR})
    actor = spawned["actor"]["name"]
    # Best-effort mesh assignment (UE 5.1+ property name); verified below.
    _mcp_client.command("actor_set_property", {
        "name": actor,
        "property": "SkeletalMeshComponent.SkeletalMeshAsset",
        "value": SKEL_MESH,
    })
    probe = _mcp_client.command("kinematics_read_transform", {"actor": actor, "queries": []})
    mesh_set = (probe.get("status") == "success"
                and bool((probe.get("result") or {}).get("mesh")))
    try:
        yield {"actor": actor, "mesh_set": mesh_set}
    finally:
        _delete_actor_if_present(_mcp_client, actor)


def _read_root(mcp, actor):
    """Full kinematics_read_transform payload + the root-bone entry (or skip)."""
    info = payload(mcp.expect("anim_skeletal_mesh_inspect", {"path": SKEL_MESH}))
    bones = [b["name"] for b in (info.get("bones") or []) if b.get("name")]
    if not bones:
        pytest.skip("SkeletalCube exposes no bones (skeletal render resource is "
                    "null under -nullrhi) — no bone transform to observe")
    result = mcp.expect("kinematics_read_transform", {
        "actor": actor, "queries": [{"bone": bones[0]}]})
    entries = result.get("transforms") or []
    assert entries and entries[0].get("exists") is True, result
    return result, entries[0]


# ── 1. forward axis + yaw handedness ─────────────────────────────────────────

def test_forward_axis_and_yaw_handedness(mcp, skel_actor):
    """SKILL.md §1.1: 'forward' is the +X axis of a rotation, and +yaw turns
    forward toward +Y (left-handed, +Z up). Arrange: actor_set_transform.
    Observe: the engine-computed forward_world from kinematics_read_transform.

    Fixture reality (measured this task, and a TASK-1 proposal note): the
    SkeletalCube ROOT BONE is authored pitched ~+90° — its local +X points at
    component +Z — so at actor rotation zero the bone forward is ~(0,0,1), NOT
    (1,0,0). The battery therefore (a) pins forward == the +X axis of the
    reported world rotation, then (b) cancels the authored pitch with actor
    pitch −90 (which also pins '+pitch noses up') so the forward lies on world
    +X, and finally (c) adds +90 yaw and requires the forward to land on +Y."""
    if not skel_actor["mesh_set"]:
        pytest.skip("SkeletalCube not assigned to the actor "
                    "(SkeletalMeshComponent.SkeletalMeshAsset reflection path)")
    actor = skel_actor["actor"]

    # (a) forward_world IS the world rotation applied to local +X.
    mcp.expect("actor_set_transform", {
        "name": actor, "location": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0]})
    _, e0 = _read_root(mcp, actor)
    f0 = _v(e0["forward_world"])
    w0q = _rot_to_quat(**e0["world"]["rotation"])
    for got, want in zip(f0, _quat_rotate(w0q, (1.0, 0.0, 0.0))):
        assert got == pytest.approx(want, abs=2e-3), (f0, e0["world"]["rotation"])
    if not (abs(f0[2]) > 0.95):
        pytest.skip(f"SkeletalCube root-bone authoring changed (forward {f0}, "
                    "expected ~ +Z); the pitch-cancel step below would not isolate yaw")

    # (b) actor pitch −90 cancels the bone's authored +90: forward lands on +X.
    # (+pitch noses UP: pitch −90 maps a +Z-pointing forward onto +X.)
    # rotation kwarg is [pitch, yaw, roll] (actor_set_transform tool schema).
    mcp.expect("actor_set_transform", {"name": actor, "rotation": [-90.0, 0.0, 0.0]})
    _, ex = _read_root(mcp, actor)
    fx = _v(ex["forward_world"])
    assert fx[0] == pytest.approx(1.0, abs=0.02), fx
    assert fx[1] == pytest.approx(0.0, abs=0.02), fx
    assert fx[2] == pytest.approx(0.0, abs=0.02), fx

    # (c) +90 yaw on a +X-pointing forward → +Y, not −Y (the handedness claim).
    mcp.expect("actor_set_transform", {"name": actor, "rotation": [-90.0, 90.0, 0.0]})
    _, ey = _read_root(mcp, actor)
    fy = _v(ey["forward_world"])
    assert fy[0] == pytest.approx(0.0, abs=0.02), fy
    assert fy[1] == pytest.approx(1.0, abs=0.02), fy
    assert fy[2] == pytest.approx(0.0, abs=0.02), fy


# ── 2. default ACharacter capsule 34 / 88 ────────────────────────────────────

def _reflective_read_float(mcp, actor, prop):
    """Non-mutating reflective read: actor_set_property dry_run returns the
    leaf's exported 'before' text without writing (MCPEditorCommands.cpp:652)."""
    result = mcp.expect("actor_set_property", {
        "name": actor, "property": prop, "value": 123.0, "dry_run": True})
    assert result.get("dry_run") is True, result
    entry = result["diff"]["properties_changed"][0]
    assert entry["property"] == prop, entry
    return float(entry["before"])


def test_character_capsule_defaults(mcp):
    """SKILL.md §'Actor & the default Character': InitCapsuleSize(34, 88).
    Engine oracle verified this run: Character.cpp:77 (UE 5.7 source).
    Arrange: actor_spawn of a bare native Character. Observe: reflective
    dry-run export of the capsule leaves (a different primitive)."""
    _delete_actor_if_present(mcp, CHAR_ACTOR)
    spawned = mcp.expect("actor_spawn", {
        "class_path": CHARACTER_CLASS, "name": CHAR_ACTOR})
    actor = spawned["actor"]["name"]
    try:
        half_height = _reflective_read_float(
            mcp, actor, "CapsuleComponent.CapsuleHalfHeight")
        radius = _reflective_read_float(
            mcp, actor, "CapsuleComponent.CapsuleRadius")
        assert half_height == pytest.approx(88.0, abs=1e-3), half_height
        assert radius == pytest.approx(34.0, abs=1e-3), radius
    finally:
        _delete_actor_if_present(mcp, actor)


# ── 3. composition identity (LHS-first) ──────────────────────────────────────

def test_transform_composition_identity(mcp, skel_actor):
    """SKILL.md §1.3: A * B applies A first — World = Relative * ComponentToWorld.
    Engine oracle: Transform.isph:199-209 (Q(AxB)=Q(B)*Q(A)). At a non-identity
    actor transform, the root bone's reported world transform must equal its
    component-relative transform composed with component_to_world (scale 1)."""
    if not skel_actor["mesh_set"]:
        pytest.skip("SkeletalCube not assigned to the actor "
                    "(SkeletalMeshComponent.SkeletalMeshAsset reflection path)")
    actor = skel_actor["actor"]

    mcp.expect("actor_set_transform", {
        "name": actor, "location": [120.0, -40.0, 260.0],
        "rotation": [10.0, 35.0, -20.0]})
    result, entry = _read_root(mcp, actor)

    ctw = result["component_to_world"]
    ctw_q = _rot_to_quat(**ctw["rotation"])
    rel_q = _rot_to_quat(**entry["relative"]["rotation"])
    rel_loc = _v(entry["relative"]["location"])

    # T(Rel x CTW) = Q(CTW)·T(Rel) + T(CTW)   (unit scale)
    expected_loc = tuple(
        a + b for a, b in zip(_quat_rotate(ctw_q, rel_loc), _v(ctw["location"])))
    world_loc = _v(entry["world"]["location"])
    for got, want in zip(world_loc, expected_loc):
        assert got == pytest.approx(want, abs=0.05), (world_loc, expected_loc)

    # Q(Rel x CTW) = Q(CTW) ⊗ Q(Rel); q and -q are the same rotation → |dot|≈1.
    expected_q = _quat_mul(ctw_q, rel_q)
    world_q = _rot_to_quat(**entry["world"]["rotation"])
    dot = abs(sum(a * b for a, b in zip(expected_q, world_q)))
    assert dot == pytest.approx(1.0, abs=1e-4), (expected_q, world_q)
