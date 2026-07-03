"""Blueprint domain — create an asset, mutate its graph/components/variables,
read the state back. Driven through the real MCP server (the `mcp` fixture calls
tools by name; the tool layer maps kwargs→bridge params, etc.).

Self-contained: needs no imported content. A module-scoped sample Blueprint is
created once under the test namespace and reused; the session-end disk wipe
(Content/__MCPTest__) cleans it up.

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via a read/inspect op.
"""

import re

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent

NS = "/Game/__MCPTest__/blueprint"
SAMPLE = f"{NS}/BP_Sample"


@pytest.fixture(scope="module")
def sample_bp(_mcp_client):
    """Create one Actor Blueprint for the whole module and compile it."""
    ensure_absent(_mcp_client, SAMPLE)
    _mcp_client.expect("bp_create_blueprint", {"name": SAMPLE, "parent_class": "Actor"})
    _mcp_client.expect("bp_compile", {"blueprint_name": SAMPLE})
    return SAMPLE


@covers("bp_create_blueprint", "asset_save")
def test_create_blueprint_writes_uasset_on_disk(mcp):
    path = f"{NS}/BP_Created"
    ensure_absent(mcp, path)
    result = mcp.expect("bp_create_blueprint", {"name": path, "parent_class": "Actor"})
    assert result.get("success") is True
    # State assertion (per the spec's "expect a new uasset on disk where we asked"):
    # save it, then the package file must exist at the mapped Content path.
    mcp.expect("asset_save", {"asset_paths": [path]})
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after create+save"


def _generated_class_has_function(mcp, bp_path: str, function_name: str) -> bool:
    """Independent readback for bp_compile: does the GENERATED class carry a
    UFunction of this name? The function subobject's path is
    `<pkg>.<Name>_C:<Function>` — it exists only after a real kismet compile
    (bp_add_custom_event marks the BP structurally modified, which regenerates
    the SKELETON class only). Observed via the sanctioned py console hatch."""
    name = bp_path.rsplit("/", 1)[-1]
    probe = mcp.expect("editor_console_exec", {"command": (
        "py import unreal; "
        f"f = unreal.find_object(None, '{bp_path}.{name}_C:{function_name}'); "
        "print('MCPTEST_FN=' + ('present' if f else 'absent'))"
    )})
    m = re.search(r"MCPTEST_FN=(\w+)", probe.get("output", ""))
    assert m, probe
    return m.group(1) == "present"


@covers("bp_compile")
def test_compile_blueprint(mcp):
    """bp_compile must actually recompile the generated class — not just return
    success. Arrange: a custom event added via bp_add_custom_event exists only
    on the graph/skeleton (no full compile). Observe: the generated class gains
    the event's UFunction only after bp_compile (py-hatch find_object probe)."""
    path = f"{NS}/BP_CompileProbe"
    ensure_absent(mcp, path)
    try:
        mcp.expect("bp_create_blueprint", {"name": path, "parent_class": "Actor"})
        mcp.expect("bp_compile", {"blueprint_name": path})
        mcp.expect("bp_add_custom_event", {
            "blueprint_name": path,
            "event_name": "MCPCompileProbeEvent",
        })
        # Before the compile under test: graph node exists, generated class
        # does NOT yet carry the function (proves the observation is not vacuous).
        assert not _generated_class_has_function(mcp, path, "MCPCompileProbeEvent")

        result = mcp.expect("bp_compile", {"blueprint_name": path})
        assert result.get("compiled") is True, result

        # Independent readback: the freshly compiled generated class now
        # exposes the event as a UFunction.
        assert _generated_class_has_function(mcp, path, "MCPCompileProbeEvent")
    finally:
        ensure_absent(mcp, path)


@covers("bp_brief")
def test_bp_brief(mcp, sample_bp):
    result = mcp.expect("bp_brief", {"bp_path": sample_bp})
    # Concrete known fields (not just "non-empty dict"): the parent class path
    # resolves to Actor and the ubergraph is listed by its real name.
    assert "Actor" in str(result.get("parent_class", "")), result
    graph_names = [g.get("name") for g in result.get("graphs", [])]
    assert "EventGraph" in graph_names, result
    assert isinstance(result.get("variables_count"), (int, float)), result


@covers("bp_get_parent_class")
def test_bp_get_parent_class(mcp, sample_bp):
    result = mcp.expect("bp_get_parent_class", {"bp_path": sample_bp})
    # Parent was Actor; the reported parent class should mention it.
    blob = str(result).lower()
    assert "actor" in blob, result


@covers("bp_read")
def test_read_blueprint_content(mcp, sample_bp):
    result = mcp.expect("bp_read", {"blueprint_path": sample_bp})
    # Concrete known fields: the parent class and the asset's own name.
    assert result.get("parent_class") == "Actor", result
    assert result.get("blueprint_name") == "BP_Sample", result


@covers("bp_list_graphs")
def test_list_blueprint_graphs(mcp, sample_bp):
    result = mcp.expect("bp_list_graphs", {"blueprint_path": sample_bp})
    # Concrete known fields: parent class + the ubergraph entry by name/category.
    assert result.get("parent_class") == "Actor", result
    graphs = result.get("graphs", [])
    event_graph = next((g for g in graphs if g.get("name") == "EventGraph"), None)
    assert event_graph is not None, result
    assert event_graph.get("category") == "ubergraph", event_graph


@covers("bp_add_component", "bp_list_components")
def test_add_component_then_list(mcp, sample_bp):
    mcp.expect("bp_add_component", {
        "blueprint_name": sample_bp,
        "component_type": "StaticMeshComponent",
        "component_name": "MCPTestMesh",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    comps = mcp.expect("bp_list_components", {"bp_path": sample_bp})
    blob = str(comps)
    assert "MCPTestMesh" in blob, comps


@covers("bp_create_variable")
def test_create_variable_then_read(mcp, sample_bp):
    mcp.expect("bp_create_variable", {
        "blueprint_name": sample_bp,
        "variable_name": "MCPTestHealth",
        "variable_type": "float",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    content = mcp.expect("bp_read", {"blueprint_path": sample_bp})
    assert "MCPTestHealth" in str(content), content


def _component_overrides(mcp, bp_path: str, component: str) -> dict:
    """Independent readback: bp_read's archetype-diff override list for one SCS
    component template. Returns {property_name: override_entry}."""
    content = mcp.expect("bp_read", {
        "blueprint_path": bp_path,
        "include_event_graph": False,
        "include_functions": False,
        "include_variables": False,
        "include_component_properties": True,
    })
    comp = next(c for c in content["components"] if c["name"] == component)
    return {o["name"]: o for o in comp.get("property_overrides", [])}


@covers("bp_set_component_property")
def test_set_component_property_then_read_back(mcp):
    """Write a component-template UPROPERTY, then prove the value landed via
    bp_read's archetype-diff overrides (a different read primitive), not the
    setter's echo."""
    path = f"{NS}/BP_CompProp"
    ensure_absent(mcp, path)
    try:
        mcp.expect("bp_create_blueprint", {"name": path, "parent_class": "Actor"})
        mcp.expect("bp_add_component", {
            "blueprint_name": path,
            "component_type": "StaticMeshComponent",
            "component_name": "PropMesh",
        })
        mcp.expect("bp_set_component_property", {
            "blueprint_name": path,
            "component_name": "PropMesh",
            "property": "BoundsScale",
            "value": 3.5,
        })
        overrides = _component_overrides(mcp, path, "PropMesh")
        assert "BoundsScale" in overrides, overrides
        entry = overrides["BoundsScale"]
        assert float(entry["value"]) == pytest.approx(3.5), entry
        # The archetype default is 1.0 — proves this is a real template override.
        assert float(entry["archetype_value"]) == pytest.approx(1.0), entry
    finally:
        ensure_absent(mcp, path)


@covers("bp_set_component_transform")
def test_set_component_transform_then_read_back(mcp):
    """Set the relative transform on a component template, then read the
    RelativeLocation/Rotation/Scale3D overrides back through bp_read."""
    path = f"{NS}/BP_CompXform"
    ensure_absent(mcp, path)
    try:
        mcp.expect("bp_create_blueprint", {"name": path, "parent_class": "Actor"})
        mcp.expect("bp_add_component", {
            "blueprint_name": path,
            "component_type": "StaticMeshComponent",
            "component_name": "XformMesh",
        })
        mcp.expect("bp_set_component_transform", {
            "blueprint_name": path,
            "component_name": "XformMesh",
            "location": [10.0, 20.0, 30.0],
            "rotation": [0.0, 90.0, 0.0],
            "scale": [2.0, 2.0, 2.0],
        })
        overrides = _component_overrides(mcp, path, "XformMesh")
        # Values are the engine's on-disk export text, e.g. "(X=10.000000,...)".
        assert overrides["RelativeLocation"]["value"] == \
            "(X=10.000000,Y=20.000000,Z=30.000000)", overrides
        assert "Yaw=90.000000" in overrides["RelativeRotation"]["value"], overrides
        assert overrides["RelativeScale3D"]["value"] == \
            "(X=2.000000,Y=2.000000,Z=2.000000)", overrides
    finally:
        ensure_absent(mcp, path)


@covers("bp_set_class_replication")
def test_set_class_replication_observed_on_spawned_instance(mcp):
    """Flip bReplicates/bAlwaysRelevant on the Blueprint CDO, then observe the
    flags on a FRESHLY SPAWNED instance of the generated class via
    actor_set_property's dry-run diff (its `before` field re-exports the live
    property by reflection and never mutates) — an independent readback.
    reflection_class_properties can't resolve BP generated classes (its
    FindFirstObject uses ExactClass, which excludes UBlueprintGeneratedClass),
    so the spawned-instance read is the observation path."""
    path = f"{NS}/BP_ClassRepl"
    actor_name = "MCPTest_BP_ClassRepl"
    ensure_absent(mcp, path)
    try:
        mcp.expect("bp_create_blueprint", {"name": path, "parent_class": "Actor"})
        mcp.expect("bp_compile", {"blueprint_name": path})
        result = mcp.expect("bp_set_class_replication", {
            "blueprint_name": path,
            "replicates": True,
            "always_relevant": True,
        })
        assert result.get("success") is True, result

        # Observe: a new instance initializes from the mutated CDO.
        try:
            mcp.command("actor_delete", {"name": actor_name})
        except Exception:
            pass
        mcp.expect("actor_spawn", {
            "class_path": f"{path}.BP_ClassRepl_C",
            "name": actor_name,
        })
        for prop in ("bReplicates", "bAlwaysRelevant"):
            probe = mcp.expect("actor_set_property", {
                "name": actor_name,
                "property": prop,
                "value": True,
                "dry_run": True,
            })
            before = probe["diff"]["properties_changed"][0]["before"]
            assert before == "True", f"{prop}: {probe}"
    finally:
        try:
            mcp.command("actor_delete", {"name": actor_name})
        except Exception:
            pass
        ensure_absent(mcp, path)


@covers("bp_create_variable")
def test_create_variable_dry_run_does_not_mutate(mcp, sample_bp):
    result = mcp.expect("bp_create_variable", {
        "blueprint_name": sample_bp,
        "variable_name": "ShouldNotExist",
        "variable_type": "bool",
        "dry_run": True,
    })
    assert result.get("dry_run") is True
    # The dry-run diff must describe the would-be variable...
    added = result.get("diff", {}).get("variables_added", [])
    assert added and added[0].get("variable_name") == "ShouldNotExist", result
    # ...and the variable must be ABSENT — proved via an independent read.
    content = mcp.expect("bp_read", {"blueprint_path": sample_bp})
    names = [v.get("name") for v in content.get("variables", [])]
    assert "ShouldNotExist" not in names, names
