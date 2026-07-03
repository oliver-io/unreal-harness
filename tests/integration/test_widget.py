"""UMG Widget domain — create a Widget Blueprint, build its widget tree, mutate a
widget property, read the tree back, and bind an event handler.

Self-contained: needs no imported content. A freshly created UWidgetBlueprint has
an empty WidgetTree, so the first ``widget_add_child`` (no parent_name) installs
the root panel; a second call parents a Button under it. ``widget_add_child``
recompiles + auto-saves, which is also the prerequisite for ``widget_bind_handler``
(a newly-added widget only appears on the generated class after a compile).

Driven through the real MCP server (the `mcp` fixture calls tools by name; the
tool layer maps kwargs→bridge params, etc.).

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on a
non-success envelope) -> assert the resulting state via a read/inspect op.
"""

import re

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready

# The UMG authoring ops touch the widget editor; under -nullrhi that schedules a
# deferred Slate layout-save which fatals on the headless generic window. Run the
# whole module in GUI mode only. (Coverage is counted by static @covers scan, so
# these ops still count toward the manifest.)
pytestmark = pytest.mark.gui_only

NS = "/Game/__MCPTest__/widget"
SAMPLE = f"{NS}/WBP_Sample"
ROOT = "RootCanvas"
BUTTON = "MCPTestButton"


@pytest.fixture(scope="module")
def sample_widget(_mcp_client):
    """Create one WidgetBlueprint with a CanvasPanel root + a Button child."""
    ensure_absent(_mcp_client, SAMPLE)
    _mcp_client.expect("widget_create", {"asset_path": SAMPLE})
    # Empty tree -> this child becomes the root (parent_name omitted).
    root = _mcp_client.expect("widget_add_child", {
        "widget_path": SAMPLE,
        "child_class": "/Script/UMG.CanvasPanel",
        "child_name": ROOT,
    })
    root_name = root.get("child_name", ROOT)
    child = _mcp_client.expect("widget_add_child", {
        "widget_path": SAMPLE,
        "child_class": "/Script/UMG.Button",
        "parent_name": root_name,
        "child_name": BUTTON,
    })
    return {"path": SAMPLE, "root": root_name, "button": child.get("child_name", BUTTON)}


@covers("widget_create")
def test_widget_create_writes_uasset_on_disk(mcp):
    path = f"{NS}/WBP_Created"
    ensure_absent(mcp, path)
    result = mcp.expect("widget_create", {"asset_path": path})
    assert result.get("success") is True
    # widget_create auto-saves on success, so the package must exist on disk.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after widget_create"


@covers("widget_add_child", "widget_tree_read")
def test_widget_tree_read_lists_child(mcp, sample_widget):
    result = mcp.expect("widget_tree_read", {"widget_path": sample_widget["path"]})
    blob = str(result)
    assert sample_widget["root"] in blob, result
    assert sample_widget["button"] in blob, result
    # The button's class should surface in the dumped tree.
    assert "Button" in blob, result


def _read_button_enabled(mcp, widget_path: str, button: str) -> str:
    """Independent readback for widget_set_property: read bIsEnabled straight
    off the WidgetTree template subobject (path `<pkg>.<WBP>:WidgetTree.<name>`)
    via the sanctioned py console hatch — widget_tree_read exports only
    name/class/parent/slot, not widget-side properties."""
    name = widget_path.rsplit("/", 1)[-1]
    probe = mcp.expect("editor_console_exec", {"command": (
        "py import unreal; "
        f"b = unreal.find_object(None, '{widget_path}.{name}:WidgetTree.{button}'); "
        "print('MCPTEST_EN=' + (str(b.get_editor_property('is_enabled')) if b else 'NOTFOUND'))"
    )})
    m = re.search(r"MCPTEST_EN=(\w+)", probe.get("output", ""))
    assert m, probe
    return m.group(1)


@covers("widget_set_property")
def test_widget_set_property_then_readback(mcp, sample_widget):
    """Disable the Button template, prove the value landed via an independent
    py readback of the WidgetTree subobject (NOT the setter's own before/after),
    and restore it in finally (the sample WBP is shared by the module).
    Note: the FProperty's real FName is `bIsEnabled` — the un-prefixed
    "IsEnabled" is rejected with pin_not_found (verified live)."""
    try:
        result = mcp.expect("widget_set_property", {
            "widget_path": sample_widget["path"],
            "widget_name": sample_widget["button"],
            "property_name": "bIsEnabled",
            "property_value": False,
            "target": "widget",
        })
        assert result.get("success") is not False, result
        assert result.get("property_name") == "bIsEnabled", result
        # Independent readback: the Button template itself now reports disabled.
        assert _read_button_enabled(
            mcp, sample_widget["path"], sample_widget["button"]) == "False"
    finally:
        mcp.command("widget_set_property", {
            "widget_path": sample_widget["path"],
            "widget_name": sample_widget["button"],
            "property_name": "bIsEnabled",
            "property_value": True,
            "target": "widget",
        })


@covers("widget_bind_handler")
def test_widget_bind_handler(mcp, sample_widget):
    # widget_add_child already recompiled the WBP, so the Button is present on the
    # generated class as required by the bind path.
    result = mcp.expect("widget_bind_handler", {
        "widget_path": sample_widget["path"],
        "widget_name": sample_widget["button"],
        "event_name": "OnClicked",
    })
    assert result.get("success") is not False, result
    assert result.get("event_name") == "OnClicked", result
    # Independent readback (not the event_name echo): the bind authored a
    # K2Node_ComponentBoundEvent in the WBP's event graph wired to this
    # button's OnClicked delegate — bp_inspect serializes delegate_name +
    # component_name for exactly that node class.
    analysis = mcp.expect("bp_inspect", {
        "blueprint_path": sample_widget["path"],
        "graph_name": "EventGraph",
        "include_node_details": True,
        "include_pin_connections": False,
    })
    nodes = analysis.get("graph_data", {}).get("nodes", [])
    bound = next((n for n in nodes
                  if n.get("class") == "K2Node_ComponentBoundEvent"
                  and n.get("delegate_name") == "OnClicked"
                  and n.get("component_name") == sample_widget["button"]), None)
    assert bound is not None, nodes
    assert_ready(mcp)
