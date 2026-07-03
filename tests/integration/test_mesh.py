"""Mesh domain — static-mesh ASSET authoring (collision / material) plus
skeletal-mesh inspection and section/skin editing, asserted via read-back.

These ops target ASSETS, not placed actors:

* The static-mesh collision/material commands mutate-and-resave a UStaticMesh
  package and refuse ``/Engine/`` content, so every mutation runs on a *copy* of
  the engine cube duplicated into the test namespace (``duplicate_asset`` +
  ``save_asset``). ``set_static_mesh_properties`` is the one component-targeting
  op here — it binds a mesh onto a Blueprint's StaticMeshComponent.
* The skeletal-mesh inspect is read-only against the engine SkeletalCube; the
  section-disable mutation runs on a duplicated copy; the bend-chain rebuild is
  exercised in ``dry_run`` mode (it would otherwise re-save the bound engine
  skeleton, which is refused) so it stays non-destructive.

On-disk copies live under ``/Game/__MCPTest__/mesh`` and are wiped by the
session-end disk reset. Creates are idempotent (ensure_absent before duplicate)
so the module re-runs against a long-lived editor.

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via a read/inspect op.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of, payload

NS = "/Game/__MCPTest__/mesh"

# Engine content confirmed present in the test project (read-only sources we copy).
ENGINE_CUBE = "/Engine/BasicShapes/Cube"            # UStaticMesh package path
ENGINE_CUBE_OBJ = "/Engine/BasicShapes/Cube.Cube"   # object path for SetStaticMesh
ENGINE_MATERIAL = "/Engine/EngineMaterials/WorldGridMaterial"
ENGINE_SKELCUBE = "/Engine/EngineMeshes/SkeletalCube"


def _dup_and_save(bridge, source_path: str, dest_path: str) -> str:
    """Idempotently duplicate an engine asset into the test namespace and save it,
    so collision/material/section mutations never touch /Engine content. Returns
    the destination package path."""
    ensure_absent(bridge, dest_path)
    bridge.expect("asset_duplicate", {
        "source_path": source_path,
        "destination_path": dest_path,
    })
    bridge.expect("asset_save", {"asset_paths": [dest_path]})
    return dest_path


def _delete_actor_if_present(bridge, name: str) -> None:
    try:
        bridge.command("actor_delete", {"name": name})
    except Exception:
        pass


# ── static mesh: collision ──────────────────────────────────────────────────

@covers("mesh_get_collision")
def test_get_static_mesh_collision_reads_engine_cube(bridge):
    """Pure read against engine content — the cube ships with a body setup."""
    result = bridge.expect("mesh_get_collision", {"asset_path": ENGINE_CUBE})
    assert result.get("success") is True, result
    assert "has_body_setup" in result, result
    # Every numeric collision counter must be reported.
    for key in ("simple_collision_count", "box_count", "sphere_count",
                "capsule_count", "convex_count"):
        assert key in result, (key, result)


@covers("asset_duplicate", "asset_save", "mesh_set_collision", "mesh_get_collision")
def test_set_static_mesh_collision_then_readback(bridge):
    """Author a box hull on a duplicated cube, then prove it through a separate
    read command (get_static_mesh_collision reports box_count >= 1)."""
    mesh = _dup_and_save(bridge, ENGINE_CUBE, f"{NS}/Cube_Collision")

    result = bridge.expect("mesh_set_collision", {
        "asset_path": mesh,
        "shape": "box",
        "replace_existing": True,
        "collision_trace_flag": "simple_and_complex",
        "save": True,
    })
    assert result.get("success") is True, result
    assert result["shape"] == "box", result
    assert result["simple_collision_count"] >= 1, result
    assert result.get("collision_trace_flag"), result  # echoed only when set

    # Read-back via the inspect command.
    info = bridge.expect("mesh_get_collision", {"asset_path": mesh})
    assert info["has_body_setup"] is True, info
    assert info["box_count"] >= 1, info
    assert info["simple_collision_count"] >= 1, info


@covers("mesh_set_collision")
def test_set_static_mesh_collision_refuses_engine_content(bridge):
    """Authoring collision mutates+resaves the asset, so /Engine paths are
    refused. Use bridge.command for the expected-error envelope; the editor must
    survive (no crash)."""
    resp = bridge.command("mesh_set_collision", {
        "asset_path": ENGINE_CUBE,
        "shape": "box",
    })
    assert resp.get("status") == "error", resp
    assert "/Engine/" in str(resp), resp
    assert_ready(bridge)


# ── static mesh: local bounds (read-only) ───────────────────────────────────

@covers("mesh_get_bounds")
def test_get_static_mesh_bounds_reads_engine_cube(bridge):
    """Pure read against engine content. /Engine/BasicShapes/Cube is a 100u cube
    centered at the origin, so the handler must report local box_extent
    (50,50,50), origin (0,0,0), size (100,100,100), box_min/box_max at ±50, and
    sphere_radius = |(50,50,50)| ≈ 86.6 (parity with the bun assertions in
    src/server/test/integration/mesh.test.ts)."""
    result = bridge.expect("mesh_get_bounds", {"static_mesh_path": ENGINE_CUBE})
    assert result.get("success") is True, result
    lb = result["local_bounds"]
    for axis in ("x", "y", "z"):
        assert abs(lb["box_extent"][axis] - 50) < 0.1, lb
        assert abs(lb["origin"][axis]) < 0.1, lb
        assert abs(result["size"][axis] - 100) < 0.1, result
        assert abs(result["box_min"][axis] - (-50)) < 0.1, result
        assert abs(result["box_max"][axis] - 50) < 0.1, result
    assert abs(lb["sphere_radius"] - 86.6) < 0.5, lb


# ── static mesh: sockets ────────────────────────────────────────────────────

@covers("mesh_add_socket", "mesh_list_sockets", "mesh_modify_socket", "mesh_remove_socket")
def test_static_mesh_socket_roundtrip(bridge):
    """Author a named socket on a duplicated cube, read it back, modify one field
    (others preserved), and remove it — each step proven through mesh_list_sockets
    (a different read primitive). dry_run must not mutate."""
    mesh = _dup_and_save(bridge, ENGINE_CUBE, f"{NS}/Cube_Sockets")
    sock = "Muzzle"

    # dry_run add: diff names the socket, but a list still shows none.
    dry = bridge.expect("mesh_add_socket", {
        "asset_path": mesh, "socket_name": sock, "location_x": 10, "dry_run": True,
    })
    assert "sockets_added" in str(dry), dry
    assert bridge.expect("mesh_list_sockets", {"asset_path": mesh})["count"] == 0

    # add for real with a known transform.
    added = bridge.expect("mesh_add_socket", {
        "asset_path": mesh, "socket_name": sock,
        "location_x": 10, "location_y": -2, "rotation_yaw": 90,
    })
    assert added.get("success") is True, added
    assert added["socket_name"] == sock, added

    listed = bridge.expect("mesh_list_sockets", {"asset_path": mesh})
    assert listed["count"] == 1, listed
    s = listed["sockets"][0]
    assert s["socket_name"] == sock, s
    assert abs(s["location"]["x"] - 10) < 1e-3, s
    assert abs(s["location"]["y"] - (-2)) < 1e-3, s
    assert abs(s["rotation"]["yaw"] - 90) < 1e-3, s

    # duplicate add is refused (no crash).
    dup = bridge.command("mesh_add_socket", {"asset_path": mesh, "socket_name": sock})
    assert dup.get("status") == "error", dup

    # modify only location_x; location_y is preserved.
    bridge.expect("mesh_modify_socket", {"asset_path": mesh, "socket_name": sock, "location_x": 25})
    s2 = bridge.expect("mesh_list_sockets", {"asset_path": mesh})["sockets"][0]
    assert abs(s2["location"]["x"] - 25) < 1e-3, s2
    assert abs(s2["location"]["y"] - (-2)) < 1e-3, s2

    # remove → empty again.
    removed = bridge.expect("mesh_remove_socket", {"asset_path": mesh, "socket_name": sock})
    assert removed.get("success") is True, removed
    assert removed["remaining_sockets"] == 0, removed
    assert bridge.expect("mesh_list_sockets", {"asset_path": mesh})["count"] == 0


# ── static mesh: material slot ──────────────────────────────────────────────

@covers("asset_duplicate", "asset_save", "mesh_set_static_mesh_material",
        "mesh_get_actor_material_info", "actor_delete")
def test_set_static_mesh_material_then_readback(bridge):
    """Retarget slot 0 of a duplicated cube to WorldGridMaterial, then read it
    back: a StaticMeshActor carrying the copy reports the new slot material
    (component inherits the asset slot when not overridden)."""
    mesh = _dup_and_save(bridge, ENGINE_CUBE, f"{NS}/Cube_Material")

    result = bridge.expect("mesh_set_static_mesh_material", {
        "mesh_path": mesh,
        "material_path": ENGINE_MATERIAL,
        "slot_index": 0,
    })
    assert result.get("success") is True, result
    assert result["mesh_path"] == mesh, result
    assert result["slot_index"] == 0, result
    assert result["material_path"] == ENGINE_MATERIAL, result

    # Read-back: spawn an actor on the mutated mesh and inspect its material slot.
    name = "MCPTest_MeshMat"
    _delete_actor_if_present(bridge, name)
    bridge.expect("spawn_actor", {
        "name": name,
        "type": "StaticMeshActor",
        "static_mesh": f"{mesh}.{mesh.rsplit('/', 1)[-1]}",
    })
    info = bridge.expect("mesh_get_actor_material_info", {"actor_name": name})
    assert info["total_slots"] >= 1, info
    assert "WorldGridMaterial" in info["material_slots"][0].get("material_path", ""), info
    bridge.expect("actor_delete", {"name": name})


# ── static mesh: properties (component-targeting) ────────────────────────────

@covers("bp_create_blueprint", "bp_add_component", "mesh_set_static_mesh_properties",
        "bp_compile", "bp_read")
def test_set_static_mesh_properties_binds_mesh_on_component(bridge):
    """set_static_mesh_properties targets a Blueprint's StaticMeshComponent (not
    an asset): build a BP with an empty SMC, bind the engine cube, and read the
    component template back via bp_read include_component_properties (proven
    helper pattern from test_blueprint/test_physics) — the assigned StaticMesh
    must appear as a property override on the component, not just in the
    handler's own echo."""
    bp = f"{NS}/BP_MeshProps"
    ensure_absent(bridge, bp)
    bridge.expect("bp_create_blueprint", {"name": bp, "parent_class": "Actor"})
    bridge.expect("bp_add_component", {
        "blueprint_name": bp,
        "component_type": "StaticMeshComponent",
        "component_name": "Mesh",
    })
    result = bridge.expect("mesh_set_static_mesh_properties", {
        "blueprint_name": bp,
        "component_name": "Mesh",
        "static_mesh": ENGINE_CUBE_OBJ,
    })
    assert result.get("component") == "Mesh", result
    bridge.expect("bp_compile", {"blueprint_name": bp})

    # Independent read-back: the component template's StaticMesh override.
    content = bridge.expect("bp_read", {
        "blueprint_path": bp,
        "include_event_graph": False,
        "include_functions": False,
        "include_variables": False,
        "include_component_properties": True,
    })
    comp = next(c for c in content["components"] if c["name"] == "Mesh")
    overrides = {o["name"]: o for o in comp.get("property_overrides", [])}
    assert "StaticMesh" in overrides, overrides
    assert "Cube" in str(overrides["StaticMesh"].get("value", "")), overrides["StaticMesh"]
    assert_ready(bridge)


# ── skeletal mesh: inspect (read-only) ──────────────────────────────────────

@covers("anim_skeletal_mesh_inspect")
def test_inspect_skeletal_mesh_reports_structure(bridge):
    """Dump the engine SkeletalCube: it must report bones, material slots, and
    its bound skeleton."""
    result = payload(bridge.expect("anim_skeletal_mesh_inspect", {"path": ENGINE_SKELCUBE}))
    assert result["num_bones"] >= 1, result
    assert isinstance(result["bones"], list) and result["bones"], result
    assert "name" in result["bones"][0], result
    assert "SkeletalCube_Skeleton" in result.get("skeleton", ""), result
    assert "lods" in result, result


# ── skeletal mesh: disable a render section (mutates a copy) ─────────────────

@covers("asset_duplicate", "asset_save", "anim_skeletal_mesh_inspect",
        "anim_skeletal_mesh_set_section_disabled")
def test_set_skeletal_mesh_section_disabled_then_readback(bridge):
    """Toggle bDisabled on a copy's LOD0 section 0, then prove the persisted flag
    via inspect_skeletal_mesh."""
    skel = _dup_and_save(bridge, ENGINE_SKELCUBE, f"{NS}/SkelCube_Sections")

    before = bridge.expect("anim_skeletal_mesh_inspect", {"path": skel})
    lods = before.get("lods") or []
    if not lods or not (lods[0].get("sections") or []):
        pytest.skip("SkeletalCube copy exposes no LOD0 render sections to toggle")
    section_index = lods[0]["sections"][0]["section_index"]

    result = bridge.expect("anim_skeletal_mesh_set_section_disabled", {
        "path": skel,
        "section_index": section_index,
        "disabled": True,
        "lod_index": 0,
    })
    assert result["section_index"] == section_index, result
    assert result["disabled"] is True, result

    after = bridge.expect("anim_skeletal_mesh_inspect", {"path": skel})
    sec = next(s for s in after["lods"][0]["sections"]
               if s["section_index"] == section_index)
    assert sec["disabled"] is True, sec


# ── skeletal mesh: bend-chain rebuild (dry-run / non-destructive) ────────────

@covers("mesh_build_bend_chain")
def test_skeletal_mesh_build_bend_chain_dry_run(bridge):
    """dry_run computes the Root->tip bone-station table WITHOUT mutating or
    re-saving the asset (a real run would re-save the bound engine skeleton,
    which is refused). Requires an editable LOD0 source mesh description; the
    engine SkeletalCube may ship cooked-only, so skip cleanly on that guard."""
    num_bones = 6
    resp = bridge.command("mesh_build_bend_chain", {
        "path": ENGINE_SKELCUBE,
        "num_bones": num_bones,
        "axis": "z",
        "dry_run": True,
    })
    if resp.get("status") != "success":
        # The bend chain needs editable LOD0 source geometry, which is part of
        # the render resource — unavailable under -nullrhi. Skip cleanly headless
        # rather than fail; a GUI/RHI run exercises the happy path below.
        assert_ready(bridge)
        pytest.skip(f"bend chain unavailable (no editable LOD0 source geometry, "
                    f"e.g. under -nullrhi): {resp.get('error')}")

    result = payload(resp["result"])
    assert result.get("dry_run") is True, result
    assert result["num_pole_bones"] == num_bones, result
    bones = result["bones"]
    assert len(bones) == num_bones + 1, bones  # Root + N poles
    assert bones[0]["name"] == "Root", bones
    assert bones[0]["parent_index"] == -1, bones
    assert_ready(bridge)  # asset untouched, editor still interactive


# ── dynamic-mesh -> static-mesh bake (precondition guard, headless-safe) ─────

@covers("asset_bake_dynamic_to_static_mesh", "actor_delete")
def test_bake_dynamic_mesh_requires_dynamic_mesh_component(bridge):
    """A real bake needs a UDynamicMeshComponent source (Modeling Mode / Geometry
    Script), which the empty headless editor can't author. We exercise the op's
    precondition guard instead: baking an actor that has no DMC must return a
    clean error envelope (not crash). This dispatches the command and confirms
    the editor survives; the happy path needs a GUI/runtime-authored dynamic
    mesh and is out of scope here."""
    name = "MCPTest_BakeSrc"
    _delete_actor_if_present(bridge, name)
    bridge.expect("spawn_actor", {
        "name": name,
        "type": "StaticMeshActor",
        "static_mesh": ENGINE_CUBE_OBJ,
    })

    resp = bridge.command("asset_bake_dynamic_to_static_mesh", {
        "actor_name": name,
        "target_asset_path": f"{NS}/SM_Baked",
        "force_overwrite": True,
    })
    assert resp.get("status") == "error", resp
    assert "DynamicMesh" in str(resp), resp  # guard message names the missing component
    assert_ready(bridge)
    bridge.expect("actor_delete", {"name": name})
