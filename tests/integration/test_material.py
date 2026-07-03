"""Material domain — author Material / MaterialInstance / MaterialFunction / MPC
assets, mutate a material's expression graph, wire materials onto actors and
Blueprint components, and read the resulting state back.

Mostly self-contained: a module-scoped sample Material is created once under the
test namespace and reused by the read-only tests; the heavier graph/instance
tests build their own assets idempotently. The session-end disk wipe
(Content/__MCPTest__) cleans everything up.

Pattern for every test: arrange prerequisite state (idempotently — every create
is preceded by ensure_absent so the suite re-runs against a long-lived editor)
-> dispatch the op (raises on a non-success envelope) -> assert the resulting
state via a read/inspect op, or by saving and checking the .uasset on disk.

Driven through the real MCP server (the `mcp` fixture calls tools by name with
their TOOL kwargs; the tool layer maps those kwargs onto the bridge wire params).
"""

import os
import time
from pathlib import Path

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready

NS = "/Game/__MCPTest__/material"
SAMPLE = f"{NS}/M_Sample"


def _live_uasset_disk_path(client, game_path: str) -> Path:
    """Attach-safe /Game/... -> Content/....uasset mapping using the live
    editor's own project root (B6 precedent — see test_asset.py)."""
    ctx = client.expect("project_context", {})
    root = Path(ctx["settings_paths"][0])
    pkg = game_path.split(".")[0]
    assert pkg.startswith("/Game/"), game_path
    return root / "Content" / (pkg[len("/Game/"):] + ".uasset")


def _expr_by_name(graph: dict, name: str) -> dict:
    """Find one expression entry in a material_read graph by node name."""
    matches = [e for e in graph.get("expressions", []) if e.get("name") == name]
    assert matches, f"expression {name} missing from graph: {graph}"
    return matches[0]


def _expr_name(result: dict) -> str:
    """add_material_expression returns the new node's name under both the
    canonical `expression_name` and the reader-shape alias `name`."""
    name = result.get("expression_name") or result.get("name")
    assert name, f"add_material_expression returned no expression name: {result}"
    return name


@pytest.fixture(scope="module")
def sample_material(_mcp_client):
    """Create one bare UMaterial for the whole module and compile it."""
    ensure_absent(_mcp_client, SAMPLE)
    _mcp_client.expect("material_create", {"material_path": SAMPLE})
    _mcp_client.expect("material_compile", {"material_path": SAMPLE})
    _mcp_client.expect("asset_save", {"asset_paths": [SAMPLE]})
    return SAMPLE


# ── creation: persisted assets land on disk ─────────────────────────────────

@covers("material_create", "asset_save")
def test_create_material_writes_uasset_on_disk(mcp):
    path = f"{NS}/M_Created"
    ensure_absent(mcp, path)
    result = mcp.expect("material_create", {
        "material_path": path,
        "shading_model": "Unlit",
    })
    assert result.get("success") is not False, result
    mcp.expect("asset_save", {"asset_paths": [path]})
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after create+save"


@covers("material_compile")
def test_compile_material(mcp):
    """material_compile recompiles AND persists (GAP-062: it saves the package).
    Deep observation: a freshly created, never-saved material has no .uasset on
    disk; after compile the package exists, and the independent reader reports a
    valid graph. The echoed errors[] alone would be the mutator's own report."""
    path = f"{NS}/M_CompileProbe"
    ensure_absent(mcp, path)
    try:
        mcp.expect("material_create", {"material_path": path})
        disk = _live_uasset_disk_path(mcp, path)
        # A stale .uasset may survive prior sessions against the live project
        # (ensure_absent only clears the registry entry when the asset isn't
        # loaded) — key on the write timestamp, not bare existence.
        mtime_before = disk.stat().st_mtime if disk.is_file() else None

        result = mcp.expect("material_compile", {"material_path": path})
        # A bare material compiles clean — success not False and no errors reported.
        assert result.get("success") is not False, result
        assert not result.get("errors"), result

        # Independent observation 1: compile's save path wrote the package.
        assert disk.is_file(), f"expected {disk} after material_compile (GAP-062 save)"
        if mtime_before is not None:
            assert disk.stat().st_mtime > mtime_before, \
                f"material_compile did not rewrite {disk}"
        # Independent observation 2: the reader sees a valid compiled graph.
        graph = mcp.expect("material_read", {"material_path": path})
        assert graph.get("success") is True, graph
        assert isinstance(graph.get("expressions"), list), graph
    finally:
        ensure_absent(mcp, path)


@covers("material_read")
def test_read_material_graph(mcp, sample_material):
    result = mcp.expect("material_read", {"material_path": sample_material})
    assert isinstance(result, dict) and result
    # The graph reader always reports an expressions collection (possibly empty).
    assert "expressions" in result, result


@covers("material_create", "material_set_property", "material_read")
def test_set_material_property_flips_flags(mcp):
    """material_set_property flips top-level UMaterial flags after creation;
    material_read (which reports blend_mode as the raw enum value and two_sided
    as a bool) proves the mutation landed. The asset lands in the live project
    in attach mode, so teardown deletes it even on failure."""
    path = f"{NS}/M_SetProp"
    ensure_absent(mcp, path)
    try:
        mcp.expect("material_create", {"material_path": path})

        # Factory defaults: Opaque (BLEND_Opaque == 0), single-sided.
        before = mcp.expect("material_read", {"material_path": path})
        assert before.get("two_sided") is False, before
        assert before.get("blend_mode") == 0, before

        result = mcp.expect("material_set_property", {
            "material_path": path,
            "two_sided": True,
            "blend_mode": "Translucent",
        })
        applied = result.get("applied", {})
        assert applied.get("two_sided") is True, result
        assert applied.get("blend_mode") == "Translucent", result

        # Observe through the independent reader: both flags flipped
        # (BLEND_Translucent == 2 in EBlendMode).
        after = mcp.expect("material_read", {"material_path": path})
        assert after.get("two_sided") is True, after
        assert after.get("blend_mode") == 2, after
    finally:
        ensure_absent(mcp, path)


# ── full graph authoring round-trip ─────────────────────────────────────────

@covers(
    "material_create",
    "material_add_expression",
    "material_connect",
    "material_set_expression_property",
    "material_read",
    "material_delete_expression",
)
def test_material_graph_build_and_teardown(mcp):
    path = f"{NS}/M_Graph"
    ensure_absent(mcp, path)
    mcp.expect("material_create", {"material_path": path})

    # Two expressions: an RGB constant and a Multiply node.
    c3 = _expr_name(mcp.expect("material_add_expression", {
        "material_path": path,
        "expression_type": "Constant3Vector",
        "position_x": -400.0, "position_y": 0.0,
        "r": 1.0, "g": 0.0, "b": 0.0,
    }))
    mul = _expr_name(mcp.expect("material_add_expression", {
        "material_path": path,
        "expression_type": "Multiply",
        "position_x": -150.0, "position_y": 0.0,
    }))

    # Wire: Constant3Vector -> Multiply.A (Mode 2), then Multiply -> BaseColor (Mode 1).
    mcp.expect("material_connect", {
        "material_path": path,
        "source_expression": c3,
        "target_input": "A",
        "target_expression": mul,
        "source_output_index": 0,
    })
    mcp.expect("material_connect", {
        "material_path": path,
        "source_expression": mul,
        "target_input": "BaseColor",
        "target_expression": "Material",
        "source_output_index": 0,
    })

    # Mutate the constant's colour. NOTE (verified live): the C++ setter GATES
    # on the `r` key (TryBuildLinearColorFromJson) — a g-only payload is a
    # silent success-shaped NO-OP. The old echo-only assertion hid exactly
    # that; always send r (g/b then zero-fill if omitted).
    mcp.expect("material_set_expression_property", {
        "material_path": path,
        "expression_name": c3,
        "r": 0.25, "g": 0.5, "b": 0.75,
    })

    # Read back through the independent graph reader — structured, not a blob
    # match. Both expressions present; the Multiply's "A" input sources the
    # constant; the material's BaseColor input sources the Multiply; and the
    # constant carries the mutated value.
    graph = mcp.expect("material_read", {"material_path": path})
    c3_node = _expr_by_name(graph, c3)
    mul_node = _expr_by_name(graph, mul)

    a_input = next(i for i in mul_node["inputs"] if i.get("name") == "A")
    assert a_input.get("connected_expression") == c3, mul_node
    assert a_input.get("connected_output_index") == 0, mul_node

    base_color = graph.get("material_inputs", {}).get("BaseColor")
    assert base_color, graph.get("material_inputs")
    assert base_color.get("connected_expression") == mul, base_color

    props = c3_node.get("properties", {})
    assert abs(props.get("r", -1.0) - 0.25) < 1e-6, props
    assert abs(props.get("g", -1.0) - 0.5) < 1e-6, props
    assert abs(props.get("b", -1.0) - 0.75) < 1e-6, props

    # Delete the Multiply node; it must disappear from the graph.
    mcp.expect("material_delete_expression", {
        "material_path": path,
        "expression_name": mul,
    })
    graph_after = mcp.expect("material_read", {"material_path": path})
    names_after = [e.get("name") for e in graph_after.get("expressions", [])]
    assert mul not in names_after, f"{mul} still present after delete: {names_after}"
    assert_ready(mcp)


# ── material instances ──────────────────────────────────────────────────────

@covers("material_create", "asset_save", "material_create_instance")
def test_create_material_instance_writes_uasset_on_disk(mcp):
    parent = f"{NS}/M_InstParent"
    inst = f"{NS}/MI_Created"
    ensure_absent(mcp, parent)
    ensure_absent(mcp, inst)
    mcp.expect("material_create", {"material_path": parent})
    mcp.expect("asset_save", {"asset_paths": [parent]})

    result = mcp.expect("material_create_instance", {
        "asset_path": inst,
        "parent_material": parent,
        "force_overwrite": True,
    })
    assert result.get("success") is not False, result
    mcp.expect("asset_save", {"asset_paths": [inst]})
    disk = config.uasset_disk_path(inst)
    assert disk.is_file(), f"expected {disk} to exist after create+save"


@covers(
    "material_create",
    "material_add_expression",
    "material_connect",
    "material_compile",
    "asset_save",
    "material_create_instance",
    "material_instance_set_parameter",
    "material_read_instance",
)
def test_material_instance_parameter_round_trip(mcp):
    parent = f"{NS}/M_Param"
    inst = f"{NS}/MI_Param"
    ensure_absent(mcp, parent)
    ensure_absent(mcp, inst)

    # Parent material exposes a VectorParameter "Tint" driving BaseColor.
    mcp.expect("material_create", {"material_path": parent})
    tint = _expr_name(mcp.expect("material_add_expression", {
        "material_path": parent,
        "expression_type": "VectorParameter",
        "parameter_name": "Tint",
        "r": 1.0, "g": 0.0, "b": 0.0, "a": 1.0,
    }))
    mcp.expect("material_connect", {
        "material_path": parent,
        "source_expression": tint,
        "target_input": "BaseColor",
        "target_expression": "Material",
        "source_output_index": 0,
    })
    mcp.expect("material_compile", {"material_path": parent})
    mcp.expect("asset_save", {"asset_paths": [parent]})

    # Instance overrides "Tint" green.
    mcp.expect("material_create_instance", {
        "asset_path": inst,
        "parent_material": parent,
        "force_overwrite": True,
    })
    mcp.expect("material_instance_set_parameter", {
        "instance_path": inst,
        "parameter_name": "Tint",
        "parameter_type": "vector",
        "r": 0.0, "g": 1.0, "b": 0.0, "a": 1.0,
    })

    # Read the VALUE back off the instance's own override array — the parameter
    # NAME alone also exists on the parent, so it proves nothing about the set.
    read = mcp.expect("material_read_instance", {"instance_path": inst})
    tints = [p for p in read.get("vector_parameters", []) if p.get("name") == "Tint"]
    assert tints, read
    tint_val = tints[0]
    assert abs(tint_val["r"] - 0.0) < 1e-6, tint_val
    assert abs(tint_val["g"] - 1.0) < 1e-6, tint_val
    assert abs(tint_val["b"] - 0.0) < 1e-6, tint_val
    assert abs(tint_val["a"] - 1.0) < 1e-6, tint_val


@covers(
    "material_create",
    "asset_save",
    "material_create_instance",
    "material_reparent_instance",
    "material_read_instance",
)
def test_reparent_material_instance(mcp):
    parent_a = f"{NS}/M_ParentA"
    parent_b = f"{NS}/M_ParentB"
    inst = f"{NS}/MI_Reparent"
    for p in (parent_a, parent_b, inst):
        ensure_absent(mcp, p)

    for name, path in (("M_ParentA", parent_a), ("M_ParentB", parent_b)):
        mcp.expect("material_create", {"material_path": path})
        mcp.expect("asset_save", {"asset_paths": [path]})

    mcp.expect("material_create_instance", {
        "asset_path": inst,
        "parent_material": parent_a,
        "force_overwrite": True,
    })
    mcp.expect("material_reparent_instance", {
        "instance_path": inst,
        "new_parent_path": parent_b,
    })

    read = mcp.expect("material_read_instance", {"instance_path": inst})
    assert "M_ParentB" in str(read.get("parent_chain", read)), read


# ── material function + parameter collection factories ──────────────────────

@covers("material_function_create", "material_read_function")
def test_material_function_create_and_read(mcp):
    path = f"{NS}/MF_Sample"
    ensure_absent(mcp, path)
    result = mcp.expect("material_function_create", {
        "asset_path": path,
        "description": "MCP test function",
        "expose_to_library": False,
    })
    assert result.get("success") is not False, result
    # Auto-saves on success -> the package file must exist on disk.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} after material_function_create"

    read = mcp.expect("material_read_function", {"function_path": path})
    assert isinstance(read, dict) and "expressions" in read, read


@covers("mpc_create")
def test_mpc_create(mcp):
    path = f"{NS}/MPC_Sample"
    ensure_absent(mcp, path)
    result = mcp.expect("mpc_create", {
        "asset_path": path,
        "parameters": [
            {"type": "scalar", "name": "Strength", "default_value": 0.5},
            {"type": "vector", "name": "Tint",
             "default_value": {"r": 1.0, "g": 0.0, "b": 0.0, "a": 1.0}},
        ],
    })
    assert result.get("success") is not False, result
    # Auto-saves on success -> the package file must exist on disk.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} after mpc_create"


# ── listing + applying materials ────────────────────────────────────────────

@covers("material_get_available")
def test_get_available_materials(mcp, sample_material):
    result = mcp.expect("material_get_available", {
        "search_path": "/Game/",
        "include_engine_materials": True,
    })
    assert isinstance(result, dict) and result, result


@covers("material_apply_to_actor", "mesh_get_actor_material_info")
def test_apply_material_to_actor(mcp, sample_material, bridge):
    """Apply the sample material to a mesh-bearing actor, then read slot 0 back
    via the independent material-info reader. The actor is spawned at the BRIDGE
    level with the engine cube (spawn_actor's static_mesh kwarg — house pattern
    from test_mesh.py): a mesh-less StaticMeshActor reports zero slots, so the
    old "info is a non-empty dict" assertion could never see the applied path."""
    # Unique per-run name: a fixed name collides with the FName of a previously
    # deleted actor still pending GC (spawn_actor then fails with name-in-use —
    # the known FName-reuse-until-GC wart), so don't reuse names at all.
    actor_name = f"MCPTest_MatActor_{os.getpid()}_{int(time.time())}"
    bridge.expect("spawn_actor", {
        "name": actor_name,
        "type": "StaticMeshActor",
        "static_mesh": "/Engine/BasicShapes/Cube.Cube",
    })
    try:
        result = mcp.expect("material_apply_to_actor", {
            "actor_name": actor_name,
            "material_path": sample_material,
            "material_slot": 0,
        })
        assert result.get("success") is not False, result

        info = mcp.expect("mesh_get_actor_material_info", {"actor_name": actor_name})
        assert info.get("total_slots", 0) >= 1, info
        slot0 = info["material_slots"][0]
        assert slot0.get("slot") == 0, info
        # GetPathName() is the full object path (/Game/.../M_Sample.M_Sample).
        assert slot0.get("material_path", "").split(".")[0] == sample_material, info
    finally:
        bridge.command("actor_delete", {"name": actor_name})


@covers(
    "bp_create_blueprint",
    "bp_add_component",
    "mesh_set_static_mesh_properties",
    "bp_compile",
    "material_apply_to_blueprint",
)
def test_apply_material_to_blueprint(mcp, sample_material, bridge):
    bp = f"{NS}/BP_MatHost"
    ensure_absent(mcp, bp)
    mcp.expect("bp_create_blueprint", {"name": bp, "parent_class": "Actor"})
    mcp.expect("bp_add_component", {
        "blueprint_name": bp,
        "component_type": "StaticMeshComponent",
        "component_name": "Mesh",
        "location": [], "rotation": [], "scale": [],
        "component_properties": {},
    })
    # A mesh gives the component a material slot to target.
    mcp.expect("mesh_set_static_mesh_properties", {
        "blueprint_name": bp,
        "component_name": "Mesh",
        "static_mesh": "/Engine/BasicShapes/Cube.Cube",
    })
    mcp.expect("bp_compile", {"blueprint_name": bp})

    result = mcp.expect("material_apply_to_blueprint", {
        "blueprint_name": bp,
        "component_name": "Mesh",
        "material_path": sample_material,
        "material_slot": 0,
    })
    assert result.get("success") is not False, result

    # get_blueprint_material_info is a bridge-internal command (no standalone MCP
    # tool), so the read-back goes through the bridge. Assert the applied path
    # landed on slot 0 of the component template — not just "info non-empty".
    info = bridge.expect("get_blueprint_material_info", {
        "blueprint_name": bp,
        "component_name": "Mesh",
    })
    assert info.get("has_static_mesh") is True, info
    assert info.get("total_slots", 0) >= 1, info
    slot0 = info["material_slots"][0]
    assert slot0.get("slot") == 0, info
    assert slot0.get("material_path", "").split(".")[0] == sample_material, info


@covers(
    "bp_create_blueprint",
    "bp_add_component",
    "mesh_set_static_mesh_properties",
    "bp_compile",
    "mesh_set_mesh_material_color",
)
def test_set_mesh_material_color(mcp):
    bp = f"{NS}/BP_ColorHost"
    ensure_absent(mcp, bp)
    mcp.expect("bp_create_blueprint", {"name": bp, "parent_class": "Actor"})
    mcp.expect("bp_add_component", {
        "blueprint_name": bp,
        "component_type": "StaticMeshComponent",
        "component_name": "Mesh",
        "location": [], "rotation": [], "scale": [],
        "component_properties": {},
    })
    mcp.expect("mesh_set_static_mesh_properties", {
        "blueprint_name": bp,
        "component_name": "Mesh",
        "static_mesh": "/Engine/BasicShapes/Cube.Cube",
    })
    mcp.expect("bp_compile", {"blueprint_name": bp})

    # GAP-009: this targets a Blueprint component *template* and now REFUSES the
    # dynamic-instance path (a runtime MID in a saved template corrupts level saves).
    # Use call() — the refusal is intentional, carrying error_code feature_disabled.
    result = mcp.call("mesh_set_mesh_material_color", {
        "blueprint_name": bp,
        "component_name": "Mesh",
        "color": [1.0, 0.0, 0.0, 1.0],
        "material_path": "/Engine/BasicShapes/BasicShapeMaterial",
        "parameter_name": "BaseColor",
        "material_slot": 0,
    })
    assert result.get("status") == "error", result
    assert result.get("error_code") == "feature_disabled", result
    assert "material_create_instance" in str(result), result
    assert_ready(mcp)
