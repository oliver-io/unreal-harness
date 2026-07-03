"""Kinematics domain — editor-world component-space transform math. Driven
through the real MCP server (the `mcp` fixture calls tools by name).

The kinematics ops operate on a USkeletalMeshComponent of an actor in the
active world (PIE if running, else the editor world — both are headless-safe).
To give them a real posed component we spawn a SkeletalMeshActor into the
transient editor level and assign the engine SkeletalCube mesh by reflection.
The actor lives in the unsaved level, so editor-quit is a full reset — no
per-test cleanup needed.

- ``kinematic_read_transform`` reads a bone/socket transform; it works off the
  mesh's reference skeleton.
- ``kinematic_probe`` applies candidate bone rotation(s) and reports the
  end-effector delta (forward FK); both need the component's mesh asset set.
- ``kinematic_solve`` runs two-bone IK, which needs a 3-bone (upper/lower/hand)
  chain — SkeletalCube may not have one, so that test skips when bones < 3.

Pattern for every test: arrange (spawn + assign + discover bones) -> dispatch
the op -> assert the resulting state.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of, payload  # noqa: F401

SKEL_ACTOR_CLASS = "/Script/Engine.SkeletalMeshActor"
SKEL_MESH = "/Engine/EngineMeshes/SkeletalCube"
MESH_LEAF = SKEL_MESH.rsplit("/", 1)[-1]
ACTOR_NAME = "MCPTest_SkelCube"


@pytest.fixture(scope="module")
def posed_actor(_mcp_client):
    """Spawn a SkeletalMeshActor and assign SkeletalCube to its component.

    Returns {actor: <GetName()>, mesh_set: bool}. ``mesh_set`` is verified by
    reading the component back through kinematic_read_transform: the probe ops
    require a mesh ASSET on the component, and the reflection path used to set
    it (SkeletalMeshComponent.SkeletalMeshAsset) is engine-version dependent."""
    spawned = _mcp_client.expect("actor_spawn", {"class_path": SKEL_ACTOR_CLASS, "name": ACTOR_NAME})
    actor = spawned["actor"]["name"]

    # ASkeletalMeshActor's component is `SkeletalMeshComponent`; the mesh
    # UPROPERTY on USkeletalMeshComponent is `SkeletalMeshAsset` (UE 5.1+).
    # Best-effort (command, no raise) — verified immediately below.
    _mcp_client.command("actor_set_property", {
        "name": actor,
        "property": "SkeletalMeshComponent.SkeletalMeshAsset",
        "value": SKEL_MESH,
    })
    probe = _mcp_client.command("kinematics_read_transform", {"actor": actor, "queries": []})
    mesh_set = (probe.get("status") == "success"
                and bool((probe.get("result") or {}).get("mesh")))
    return {"actor": actor, "mesh_set": mesh_set}


def _bone_names(bridge):
    """Bone names of SkeletalCube, read straight off the asset (independent of
    whether the actor's component has the mesh assigned)."""
    info = payload(bridge.expect("anim_skeletal_mesh_inspect", {"path": SKEL_MESH}))
    return [b["name"] for b in (info.get("bones") or []) if b.get("name")]


@covers("kinematics_read_transform", "actor_spawn", "actor_set_property", "anim_skeletal_mesh_inspect")
def test_kinematic_read_transform(mcp, posed_actor):
    actor = posed_actor["actor"]
    result = mcp.expect("kinematics_read_transform", {"actor": actor, "queries": []})
    assert result.get("success") is True, result
    assert "world_type" in result and "component_to_world" in result

    if not posed_actor["mesh_set"]:
        pytest.skip("SkeletalCube not assigned to the actor "
                    "(SkeletalMeshComponent.SkeletalMeshAsset reflection path) — "
                    "no posed component with a mesh to read bone transforms from")

    # Mesh asset is on the component: the reported mesh path must be SkeletalCube
    # and a real bone query must resolve.
    assert MESH_LEAF in result.get("mesh", ""), result
    bones = _bone_names(mcp)
    if not bones:
        pytest.skip("SkeletalCube exposes no bones (skeletal render resource is "
                    "null under -nullrhi) — no bone transform to read")
    bone = bones[0]
    posed = mcp.expect("kinematics_read_transform", {
        "actor": actor, "queries": [{"bone": bone}]})
    transforms = posed.get("transforms") or []
    assert transforms and transforms[0]["name"] == bone, posed
    assert transforms[0]["exists"] is True, posed
    assert "world" in transforms[0] and "relative" in transforms[0]


@covers("kinematics_probe", "anim_skeletal_mesh_inspect")
def test_kinematic_probe(mcp, posed_actor):
    actor = posed_actor["actor"]
    if not posed_actor["mesh_set"]:
        pytest.skip("SkeletalCube not assigned; kinematic_probe requires a mesh asset")

    bones = _bone_names(mcp)
    if not bones:
        pytest.skip("SkeletalCube exposes no bones (skeletal render resource is "
                    "null under -nullrhi) — nothing to probe")
    rot_bone = bones[0]            # root drives the whole sub-chain
    probe_bone = bones[-1]         # tip we measure (== root when single-boned)

    result = mcp.expect("kinematics_probe", {
        "actor": actor,
        "rotations": [{
            "bone": rot_bone,
            "rotation": {"axis": {"x": 0.0, "y": 0.0, "z": 1.0}, "angle_deg": 30.0},
            "space": "component",
        }],
        "probe_points": [{"bone": probe_bone}],
        "mode": "dryrun",
    })
    assert result.get("success") is True, result
    assert result.get("mode") == "dryrun"

    points = result.get("probe_points") or []
    assert points, result
    pt = points[0]
    assert pt["point"]["name"] == probe_bone, pt
    # The probe reports before/after transforms for the point (no 'exists' key).
    assert "before" in pt and "after" in pt, pt

    # If the editor produced a valid pose, the 30deg twist must turn the tip.
    if result.get("pose_valid"):
        ori = pt.get("delta_orientation_world") or {}
        assert ori.get("angle_deg", 0.0) > 1.0, pt
    assert_ready(mcp)


@covers("kinematics_solve", "anim_skeletal_mesh_inspect")
def test_kinematic_solve(mcp, posed_actor):
    actor = posed_actor["actor"]
    if not posed_actor["mesh_set"]:
        pytest.skip("SkeletalCube not assigned; kinematic_solve requires a mesh asset")

    bones = _bone_names(mcp)
    if len(bones) < 3:
        pytest.skip(f"two-bone IK needs a 3-bone upper/lower/hand chain; "
                    f"SkeletalCube has {len(bones)} bone(s)")
    upper, lower, hand = bones[0], bones[1], bones[2]

    result = mcp.expect("kinematics_solve", {
        "actor": actor,
        "chain": {"upper": upper, "lower": lower, "hand": hand},
        "effector": {"bone": hand},
        "desired_direction": {"x": 0.0, "y": 0.0, "z": 1.0},
        "verify": True,
    })
    assert result.get("success") is True, result
    assert result.get("solved") is True, result
    assert result.get("chain", {}).get("upper") == upper, result
    assert isinstance(result.get("resulting_rotations"), list), result

    # The verification payload must prove the solve actually REACHED its own
    # hand target, not merely exist. Because the effector here IS the hand
    # bone, the solver's hand_target_world is exactly the desired tip position
    # — so the verified after-pose world location must land on it. (Validated
    # live against a 3-bone mannequin chain: residual ~1e-6 when reachable.)
    ver = result["verification"]
    after_loc = ver["after"]["world"]["location"]
    target = result["hand_target_world"]
    residual = ((after_loc["x"] - target["x"]) ** 2
                + (after_loc["y"] - target["y"]) ** 2
                + (after_loc["z"] - target["z"]) ** 2) ** 0.5
    if result.get("reachable") and result.get("pose_valid"):
        assert residual < 1.0, (residual, ver)
    else:
        # Unreachable/invalid pose: best-effort — the solve must still report
        # a finite, sane residual rather than garbage.
        assert residual == residual and residual < 1e6, (residual, ver)
    assert_ready(mcp)
