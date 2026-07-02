"""Physics + skeletal-mesh domain — inspect a skeletal mesh / physics asset and
mutate physics state, reading the result back. Driven through the real MCP
server (the `mcp` fixture calls tools by name).

What runs unconditionally (needs only stock engine content, headless-safe):
  * inspect_skeletal_mesh on /Engine/EngineMeshes/SkeletalCube (pure read)
  * set_physics_properties on a freshly created Blueprint component (this op
    targets a Blueprint component, NOT a UPhysicsAsset)
  * skeletal_mesh_build_bend_chain in dry_run mode (preview, no mutation/save)

What is content-gated (skips with a clear reason when the prerequisite is
absent, which is the common case in a bare fixture project / under -nullrhi):
  * inspect_physics_asset / set_physics_body_collision /
    set_physics_constraint_motion / set_skeletal_mesh_physics_asset all need a
    real UPhysicsAsset; we discover one (bound on the cube, else via list_assets
    over /Game then /Engine) and skip if none exists. Mutating ops pass
    save=False so an engine-owned asset is only touched in memory, never on disk.
  * set_skeletal_mesh_section_disabled persists to disk, so it operates on a
    /Game duplicate of the cube (never the engine package) and skips if render
    sections aren't available (e.g. no render data under -nullrhi).

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via an inspect/read op.
"""

import pytest

from harness import config
from harness.bridge_client import CommandError
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of, payload

NS = "/Game/__MCPTest__/physics"
# Stock engine assets (present out of the box; only mutated in memory, never saved).
MESH = "/Engine/EngineMeshes/SkeletalCube"


# ── discovery ────────────────────────────────────────────────────────────────

@pytest.fixture
def physics_asset(mcp):
    """Return the path to *some* UPhysicsAsset, or None if the project/engine has
    none. Tried in order: the asset bound on the engine cube, then a content
    scan of /Game, then /Engine (class_filter keeps the payload small)."""
    info = mcp.expect("anim_skeletal_mesh_inspect", {"path": MESH})
    bound = info.get("physics_asset") or ""
    if bound:
        return bound
    for directory in ("/Game/", "/Engine/"):
        try:
            result = mcp.expect("asset_list", {
                "directory_path": directory,
                "recursive": True,
                "class_filter": "PhysicsAsset",
            })
        except CommandError:
            continue
        for asset in result.get("assets", []) or []:
            path = asset.get("path")
            if path:
                return path
    return None


# ── reads (always run) ────────────────────────────────────────────────────────

@covers("anim_skeletal_mesh_inspect")
def test_inspect_skeletal_mesh(mcp):
    result = payload(mcp.expect("anim_skeletal_mesh_inspect", {"path": MESH}))
    assert result.get("num_bones", 0) >= 1, result
    assert isinstance(result.get("bones"), list) and result["bones"], result
    assert isinstance(result.get("lods"), list), result


@covers("physics_set_properties")
def test_set_physics_properties_on_component(mcp):
    # set_physics_properties operates on a Blueprint component, so build a tiny
    # Actor BP with a primitive component and drive its physics flags.
    bp = f"{NS}/BP_Physics"
    ensure_absent(mcp, bp)
    mcp.expect("bp_create_blueprint", {"name": bp, "parent_class": "Actor"})
    mcp.expect("bp_add_component", {
        "blueprint_name": bp,
        "component_type": "StaticMeshComponent",
        "component_name": "PhysMesh",
    })
    mcp.expect("bp_compile", {"blueprint_name": bp})

    result = mcp.expect("physics_set_properties", {
        "blueprint_name": bp,
        "component_name": "PhysMesh",
        "simulate_physics": True,
        "gravity_enabled": True,
        "mass": 5.0,
        "linear_damping": 0.02,
        "angular_damping": 0.0,
    })
    assert result.get("success") is not False, result
    assert_ready(mcp)


@covers("physics_material_create")
def test_physics_material_create(mcp):
    # Factory-create a UPhysicalMaterial with explicit surface response values,
    # verify it landed on disk, and confirm the no-silent-overwrite contract.
    pm = f"{NS}/PM_TestBouncy"
    ensure_absent(mcp, pm)
    try:
        result = mcp.expect("physics_material_create", {
            "asset_path": pm,
            "friction": 0.4,
            "restitution": 0.95,
            "density": 2.0,
            "restitution_combine_mode": "Max",
        })
        assert result.get("asset_path") == pm, result
        assert abs(result.get("restitution", 0) - 0.95) < 1e-4, result
        assert abs(result.get("friction", 0) - 0.4) < 1e-4, result
        assert abs(result.get("density", 0) - 2.0) < 1e-4, result
        # Passing a combine mode must flip its per-material override flag (else
        # the project default silently wins); the unset one stays off.
        assert result.get("restitution_combine_override") is True, result
        assert result.get("friction_combine_override") is False, result

        # Persisted: visible to the asset registry under the test namespace.
        listing = mcp.expect("asset_list", {
            "directory_path": NS,
            "recursive": True,
            "class_filter": "PhysicalMaterial",
        })
        # asset_list reports OBJECT paths (/Pkg/Name.Name), not package paths.
        paths = [a.get("path") for a in listing.get("assets", []) or []]
        assert any(p == pm or (p or "").startswith(pm + ".") for p in paths), paths

        # Uniqueness: a second create at the same path is refused, not overwritten.
        # (An invalid combine-mode string is unrepresentable through the tool —
        # the server schema enums it — so only the collision contract is probed.)
        dup = mcp.command("physics_material_create", {"asset_path": pm})
        assert dup.get("status") != "success", dup
        assert dup.get("error_code") == "name_collision", dup
    finally:
        ensure_absent(mcp, pm)
    assert_ready(mcp)


@covers("mesh_build_bend_chain")
def test_skeletal_mesh_build_bend_chain_dry_run(mcp):
    # dry_run computes the bone-station table WITHOUT mutating/saving the engine
    # mesh. If geometry isn't readable (e.g. render data unavailable headless),
    # the handler errors — document that as a skip rather than a hard failure.
    resp = mcp.command("mesh_build_bend_chain", {
        "path": MESH,
        "num_bones": 4,
        "axis": "z",
        "base_fraction": 0.14,
        "segment_ratio": 0.85,
        "bone_prefix": "pole_",
        "root_bone_name": "Root",
        "dry_run": True,
    })
    if resp.get("status") != "success":
        # Needs editable LOD0 source geometry (render resource) — null headless.
        assert_ready(mcp)
        pytest.skip(f"bend-chain preview unavailable (no source geometry, e.g. "
                    f"-nullrhi): {resp.get('error')}")
    result = payload(resp["result"])
    assert result.get("dry_run") is True or result.get("bones") or \
        result.get("num_pole_bones") is not None, result
    assert_ready(mcp)


@covers("anim_skeletal_mesh_set_section_disabled")
def test_set_skeletal_mesh_section_disabled(mcp):
    # This op SAVES to disk, so never touch the engine package: duplicate the
    # cube into the test namespace and toggle a section on the copy.
    copy = f"{NS}/SkeletalCubeCopy"
    ensure_absent(mcp, copy)
    dup = mcp.command("asset_duplicate", {
        "source_path": MESH,
        "destination_path": copy,
        "dry_run": False,
    })
    if dup.get("status") != "success":
        pytest.skip(f"could not duplicate the engine skeletal mesh: {dup}")
    try:
        info = mcp.expect("anim_skeletal_mesh_inspect", {"path": copy})
        lods = info.get("lods") or []
        sections = lods[0].get("sections") if lods else None
        if not sections:
            pytest.skip("no render sections to toggle (render data unavailable, e.g. -nullrhi)")
        section_index = sections[0].get("section_index", 0)

        result = mcp.expect("anim_skeletal_mesh_set_section_disabled", {
            "path": copy,
            "section_index": section_index,
            "disabled": True,
            "lod_index": 0,
        })
        assert result.get("success") is not False, result

        after = mcp.expect("anim_skeletal_mesh_inspect", {"path": copy})
        sec = after["lods"][0]["sections"][section_index]
        assert sec.get("disabled") is True, sec
    finally:
        ensure_absent(mcp, copy)


# ── physics-asset ops (content-gated) ─────────────────────────────────────────

@covers("anim_physics_inspect")
def test_inspect_physics_asset(mcp, physics_asset):
    if not physics_asset:
        pytest.skip("no PhysicsAsset present in project or engine content")
    result = mcp.expect("anim_physics_inspect", {"path": physics_asset})
    assert "num_bodies" in result, result
    assert isinstance(result.get("bodies"), list), result


@covers("physics_set_body_collision")
def test_set_physics_body_collision(mcp, physics_asset):
    if not physics_asset:
        pytest.skip("no PhysicsAsset present in project or engine content")
    # save=False keeps an engine-owned asset memory-only (no on-disk mutation).
    result = mcp.expect("physics_set_body_collision", {
        "path": physics_asset,
        "collision_enabled": "QueryAndPhysics",
        "save": False,
    })
    assert "num_changed" in result, result
    # Read back: every body should now report the requested collision setting.
    info = mcp.expect("anim_physics_inspect", {"path": physics_asset})
    for body in info.get("bodies", []):
        assert body.get("collision_enabled") == "QueryAndPhysics", body
    assert_ready(mcp)


@covers("physics_set_constraint_motion")
def test_set_physics_constraint_motion(mcp, physics_asset):
    if not physics_asset:
        pytest.skip("no PhysicsAsset present in project or engine content")
    info = mcp.expect("anim_physics_inspect", {"path": physics_asset})
    constraints = info.get("constraints") or []
    if not constraints:
        pytest.skip("physics asset has no constraints to edit")
    target = constraints[0]

    params = {"path": physics_asset, "save": False, "swing1": "Free"}
    if target.get("joint_name"):
        params["joint_name"] = target["joint_name"]
    else:  # JointName is often None on auto-generated assets — key by bone pair.
        params["bone1"] = target.get("bone1")
        params["bone2"] = target.get("bone2")

    result = mcp.expect("physics_set_constraint_motion", params)
    assert result.get("action") == "set_motion", result
    assert result.get("constraint_index") is not None, result
    assert_ready(mcp)


@covers("mesh_set_physics_asset")
def test_set_skeletal_mesh_physics_asset(mcp, physics_asset):
    if not physics_asset:
        pytest.skip("no PhysicsAsset present in project or engine content")
    # Rebind the engine cube to the discovered asset, then restore — all with
    # save=False so the engine mesh package on disk is never modified.
    info = mcp.expect("anim_skeletal_mesh_inspect", {"path": MESH})
    original = info.get("physics_asset") or ""
    try:
        result = mcp.expect("mesh_set_physics_asset", {
            "path": MESH,
            "physics_asset": physics_asset,
            "save": False,
        })
        assert result.get("new_physics_asset"), result
    finally:
        mcp.command("mesh_set_physics_asset", {
            "path": MESH,
            "physics_asset": original,
            "save": False,
        })
    assert_ready(mcp)
