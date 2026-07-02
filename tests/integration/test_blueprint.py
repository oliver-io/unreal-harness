"""Blueprint domain — create an asset, mutate its graph/components/variables,
read the state back. Driven through the real MCP server (the `mcp` fixture calls
tools by name; the tool layer maps kwargs→bridge params, etc.).

Self-contained: needs no imported content. A module-scoped sample Blueprint is
created once under the test namespace and reused; the session-end disk wipe
(Content/__MCPTest__) cleans it up.

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via a read/inspect op.
"""

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


@covers("bp_compile")
def test_compile_blueprint(mcp, sample_bp):
    result = mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    assert result.get("success") is not False


@covers("bp_brief")
def test_bp_brief(mcp, sample_bp):
    result = mcp.expect("bp_brief", {"bp_path": sample_bp})
    assert isinstance(result, dict) and result


@covers("bp_get_parent_class")
def test_bp_get_parent_class(mcp, sample_bp):
    result = mcp.expect("bp_get_parent_class", {"bp_path": sample_bp})
    # Parent was Actor; the reported parent class should mention it.
    blob = str(result).lower()
    assert "actor" in blob, result


@covers("bp_read")
def test_read_blueprint_content(mcp, sample_bp):
    result = mcp.expect("bp_read", {"blueprint_path": sample_bp})
    assert isinstance(result, dict) and result


@covers("bp_list_graphs")
def test_list_blueprint_graphs(mcp, sample_bp):
    result = mcp.expect("bp_list_graphs", {"blueprint_path": sample_bp})
    assert isinstance(result, dict) and result


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


@covers("bp_create_variable")
def test_create_variable_dry_run_does_not_mutate(mcp, sample_bp):
    result = mcp.expect("bp_create_variable", {
        "blueprint_name": sample_bp,
        "variable_name": "ShouldNotExist",
        "variable_type": "bool",
        "dry_run": True,
    })
    assert result.get("dry_run") is True
