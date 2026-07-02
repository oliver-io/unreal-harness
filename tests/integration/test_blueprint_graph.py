"""Blueprint graph domain — node / function / variable / component graph ops.

Builds on the same pattern as ``test_blueprint.py`` (the gold reference): a
module-scoped sample Actor Blueprint is created once under the test namespace,
then each test arranges prerequisite graph state, dispatches the op under test
(``mcp.expect`` raises on a non-success envelope), and asserts the resulting
state by reading it back (list_node_pins, analyze_blueprint_graph,
get_blueprint_function_details, get_blueprint_variable_details,
read_blueprint_content, bp_list_components, actor_query / level_inspect).

Driven through the real MCP server (the `mcp` fixture calls tools by name; the
tool layer maps kwargs→bridge params, etc.).

The graph/function/variable/component CRUD ops covered here are distinct from
the asset-level ops in test_blueprint.py — see OPS in that file for the ones
deliberately not re-tested as dedicated cases.

Self-contained: needs no imported content. The session-end disk wipe
(Content/__MCPTest__) cleans up everything created here.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready

NS = "/Game/__MCPTest__/bpgraph"
SAMPLE = f"{NS}/BP_GraphSample"


@pytest.fixture(scope="module")
def sample_bp(_mcp_client):
    """Create one Actor Blueprint for the whole module and compile it."""
    ensure_absent(_mcp_client, SAMPLE)
    _mcp_client.expect("bp_create_blueprint", {"name": SAMPLE, "parent_class": "Actor"})
    _mcp_client.expect("bp_compile", {"blueprint_name": SAMPLE})
    return SAMPLE


# ── helpers ─────────────────────────────────────────────────────────────────

def _list_pins(bridge, bp_path, node_id):
    """Return the pins[] array for a node via list_node_pins."""
    result = bridge.expect("bp_list_node_pins", {
        "blueprint_name": bp_path,
        "node_id": node_id,
    })
    return result.get("pins", []) if isinstance(result, dict) else []


def _exec_pin(pins, direction):
    """Name of the first exec pin in a given direction ("Input"/"Output").

    Falls back to UE's canonical exec pin names ("execute" input / "then"
    output) if the category field isn't reported as expected."""
    for p in pins:
        if p.get("direction") == direction and p.get("category") == "exec":
            return p.get("name")
    return "execute" if direction == "Input" else "then"


def _pin_connected(pins, name):
    for p in pins:
        if p.get("name") == name:
            return bool(p.get("is_connected"))
    return False


# ── node graph ops ───────────────────────────────────────────────────────────

@covers("bp_add_event_node", "bp_list_node_pins")
def test_add_event_node_and_list_pins(mcp, sample_bp):
    res = mcp.expect("bp_add_event_node", {
        "blueprint_name": sample_bp,
        "event_name": "ReceiveBeginPlay",
        "pos_x": 0,
        "pos_y": 0,
    })
    node_id = res.get("node_id")
    assert node_id, res
    # Readback: the event node exposes at least one output exec pin.
    pins = _list_pins(mcp, sample_bp, node_id)
    assert any(p.get("direction") == "Output" for p in pins), pins


@covers("add_blueprint_node")
def test_add_blueprint_node_print(mcp, sample_bp):
    res = mcp.expect("bp_add_node", {
        "blueprint_name": sample_bp,
        "node_type": "Print",
        "pos_x": 400, "pos_y": 0, "message": "MCP graph test",
    })
    assert res.get("node_id"), res
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})


@covers("bp_add_event_node", "add_blueprint_node", "bp_list_node_pins",
        "bp_connect_pins", "bp_inspect")
def test_connect_nodes(mcp, sample_bp):
    event = mcp.expect("bp_add_event_node", {
        "blueprint_name": sample_bp, "event_name": "ReceiveTick",
        "pos_x": 0, "pos_y": 300,
    })["node_id"]
    printer = mcp.expect("bp_add_node", {
        "blueprint_name": sample_bp, "node_type": "Print",
        "pos_x": 400, "pos_y": 300, "message": "tick",
    })["node_id"]

    src_pin = _exec_pin(_list_pins(mcp, sample_bp, event), "Output")
    dst_pin = _exec_pin(_list_pins(mcp, sample_bp, printer), "Input")

    mcp.expect("bp_connect_pins", {
        "blueprint_name": sample_bp,
        "source_node_id": event,
        "source_pin_name": src_pin,
        "target_node_id": printer,
        "target_pin_name": dst_pin,
        "dry_run": False,
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})

    # Readback: the source exec pin now reports as connected.
    assert _pin_connected(_list_pins(mcp, sample_bp, event), src_pin)
    # And the graph analysis returns structured node data.
    analysis = mcp.expect("bp_inspect", {
        "blueprint_path": sample_bp,
        "graph_name": "EventGraph",
        "include_node_details": True,
        "include_pin_connections": True,
    })
    assert isinstance(analysis, dict) and analysis


@covers("bp_add_event_node", "add_blueprint_node", "bp_list_node_pins",
        "bp_connect_pins", "bp_disconnect_pin")
def test_disconnect_pin(mcp, sample_bp):
    event = mcp.expect("bp_add_event_node", {
        "blueprint_name": sample_bp, "event_name": "ReceiveEndPlay",
        "pos_x": 0, "pos_y": 600,
    })["node_id"]
    printer = mcp.expect("bp_add_node", {
        "blueprint_name": sample_bp, "node_type": "Print",
        "pos_x": 400, "pos_y": 600, "message": "endplay",
    })["node_id"]
    src_pin = _exec_pin(_list_pins(mcp, sample_bp, event), "Output")
    dst_pin = _exec_pin(_list_pins(mcp, sample_bp, printer), "Input")
    mcp.expect("bp_connect_pins", {
        "blueprint_name": sample_bp, "source_node_id": event,
        "source_pin_name": src_pin, "target_node_id": printer,
        "target_pin_name": dst_pin, "dry_run": False,
    })

    mcp.expect("bp_disconnect_pin", {
        "bp_path": sample_bp,
        "node_id": event,
        "pin_name": src_pin,
        "dry_run": False,
    })
    # Readback: the pin is no longer connected.
    assert not _pin_connected(_list_pins(mcp, sample_bp, event), src_pin)


@covers("add_blueprint_node", "bp_set_node_property", "bp_list_node_pins")
def test_set_node_property(mcp, sample_bp):
    printer = mcp.expect("bp_add_node", {
        "blueprint_name": sample_bp, "node_type": "Print",
        "pos_x": 800, "pos_y": 0, "message": "before",
    })["node_id"]
    mcp.expect("bp_set_node_property", {
        "blueprint_name": sample_bp,
        "node_id": printer,
        "property_name": "message",
        "property_value": "MCP_AFTER_TOKEN",
        "dry_run": False,
    })
    # Independent read-back (NOT the setter's own echo): the Print "message"
    # lands as the InString pin default. Confirms the write actually took
    # effect — a no-op handler returning success would fail here.
    pins = mcp.expect("bp_list_node_pins",
                      {"blueprint_name": sample_bp, "node_id": printer})["pins"]
    instr = next((p for p in pins if p.get("name") == "InString"), None)
    assert instr is not None, pins
    assert instr.get("default_value") == "MCP_AFTER_TOKEN", instr


@covers("add_blueprint_node", "bp_delete_node")
def test_delete_node(mcp, sample_bp):
    printer = mcp.expect("bp_add_node", {
        "blueprint_name": sample_bp, "node_type": "Print",
        "pos_x": 800, "pos_y": 300, "message": "doomed",
    })["node_id"]
    res = mcp.expect("bp_delete_node", {
        "blueprint_name": sample_bp,
        "node_id": printer,
        "dry_run": False,
    })
    assert res.get("deleted_node_id") or res.get("success") is not False, res
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})


@covers("bp_set_default_value", "bp_create_variable", "bp_read")
def test_set_blueprint_default_value(mcp, sample_bp):
    # Set a variable's CDO default, then read it back independently and confirm
    # the new value actually landed (not just that the op returned success).
    mcp.expect("bp_create_variable", {
        "blueprint_name": sample_bp, "variable_name": "MCPDefaultInt",
        "variable_type": "int", "dry_run": False,
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    mcp.expect("bp_set_default_value", {
        "blueprint_name": sample_bp,
        "property": "MCPDefaultInt",
        "value": 7,
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    content = mcp.expect("bp_read", {
        "blueprint_path": sample_bp, "include_variable_defaults": True})
    var = next((v for v in content.get("variables", [])
                if v.get("name") == "MCPDefaultInt"), None)
    assert var is not None, content
    # read_blueprint_content reports the CDO default under 'cdo_value'.
    assert str(var.get("cdo_value")) == "7", var


# ── function ops ─────────────────────────────────────────────────────────────

@covers("bp_create_function", "bp_get_function_details")
def test_create_function(mcp, sample_bp):
    mcp.expect("bp_create_function", {
        "blueprint_name": sample_bp,
        "function_name": "MCPGraphFunc",
        "return_type": "void",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    details = mcp.expect("bp_get_function_details", {
        "blueprint_path": sample_bp,
        "function_name": "MCPGraphFunc",
        "include_graph": True,
    })
    assert "MCPGraphFunc" in str(details), details


@covers("bp_create_function", "bp_add_function_input", "bp_add_function_output",
        "bp_get_function_details", "bp_remove_function_input",
        "bp_remove_function_output")
def test_function_io_roundtrip(mcp, sample_bp):
    fn = "MCPGraphIOFunc"
    mcp.expect("bp_create_function", {
        "blueprint_name": sample_bp, "function_name": fn, "return_type": "void",
    })
    mcp.expect("bp_add_function_input", {
        "blueprint_name": sample_bp, "function_name": fn,
        "param_name": "InVal", "param_type": "float", "is_array": False,
    })
    mcp.expect("bp_add_function_output", {
        "blueprint_name": sample_bp, "function_name": fn,
        "param_name": "OutVal", "param_type": "bool", "is_array": False,
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})

    details = mcp.expect("bp_get_function_details", {
        "blueprint_path": sample_bp, "function_name": fn, "include_graph": True,
    })
    blob = str(details)
    assert "InVal" in blob and "OutVal" in blob, details

    # Remove them and confirm they are gone.
    mcp.expect("bp_remove_function_input", {
        "blueprint_name": sample_bp, "function_name": fn, "param_name": "InVal",
    })
    mcp.expect("bp_remove_function_output", {
        "blueprint_name": sample_bp, "function_name": fn, "param_name": "OutVal",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    after = str(mcp.expect("bp_get_function_details", {
        "blueprint_path": sample_bp, "function_name": fn, "include_graph": True,
    }))
    assert "InVal" not in after and "OutVal" not in after, after


@covers("bp_create_function", "bp_rename_function", "bp_get_function_details")
def test_rename_function(mcp, sample_bp):
    mcp.expect("bp_create_function", {
        "blueprint_name": sample_bp, "function_name": "MCPRenameSrc",
        "return_type": "void",
    })
    mcp.expect("bp_rename_function", {
        "blueprint_name": sample_bp,
        "old_function_name": "MCPRenameSrc",
        "new_function_name": "MCPRenameDst",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    details = str(mcp.expect("bp_get_function_details", {
        "blueprint_path": sample_bp, "include_graph": False,
    }))
    assert "MCPRenameDst" in details, details


@covers("bp_create_function", "bp_delete_function", "bp_get_function_details")
def test_delete_function(mcp, sample_bp):
    mcp.expect("bp_create_function", {
        "blueprint_name": sample_bp, "function_name": "MCPDeleteMe",
        "return_type": "void",
    })
    mcp.expect("bp_delete_function", {
        "blueprint_name": sample_bp, "function_name": "MCPDeleteMe",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    details = str(mcp.expect("bp_get_function_details", {
        "blueprint_path": sample_bp, "include_graph": False,
    }))
    assert "MCPDeleteMe" not in details, details


@covers("bp_create_function", "bp_function_references")
def test_bp_function_references(mcp, sample_bp):
    mcp.expect("bp_create_function", {
        "blueprint_name": sample_bp, "function_name": "MCPRefFunc",
        "return_type": "void",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    res = mcp.expect("bp_function_references", {
        "bp_path": sample_bp,
        "function_name": "MCPRefFunc",
        "direction": "callees",
    })
    assert isinstance(res, dict) and res, res


# ── event dispatcher (multicast delegate) ────────────────────────────────────

@covers("bp_create_dispatcher")
def test_create_dispatcher(mcp, sample_bp):
    # dry_run must preview, not create.
    preview = mcp.expect("bp_create_dispatcher", {
        "blueprint_name": sample_bp,
        "dispatcher_name": "OnMCPScored",
        "params": [{"name": "NewScore", "type": "int"}],
        "dry_run": True,
    })
    assert "dispatchers_added" in str(preview), preview

    # Real authoring: PC_MCDelegate member + signature graph + one typed arg.
    res = mcp.expect("bp_create_dispatcher", {
        "blueprint_name": sample_bp,
        "dispatcher_name": "OnMCPScored",
        "params": [{"name": "NewScore", "type": "int"}],
    })
    assert res.get("dispatcher_name") == "OnMCPScored", res
    assert res.get("params_added") == 1, res
    assert res.get("graph_id"), res

    # The dispatcher must survive a structural recompile and show up on the BP.
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    blob = str(mcp.expect("bp_read", {"blueprint_path": sample_bp}))
    assert "OnMCPScored" in blob, blob


# ── custom event authoring + replication (GAP-055) ───────────────────────────

@covers("bp_add_custom_event", "bp_set_event_replication")
def test_custom_event_create_and_replicate(mcp, sample_bp):
    # Author a fresh custom event with one typed input parameter (an output pin on
    # the node = the event's input signature).
    res = mcp.expect("bp_add_custom_event", {
        "blueprint_name": sample_bp,
        "event_name": "MCPServerFire",
        "params": [{"name": "Amount", "type": "int"}],
        "pos_x": 0, "pos_y": 600,
    })
    assert res.get("node_id"), res
    assert res.get("event_name") == "MCPServerFire", res
    assert res.get("num_params") == 1, res
    assert "Amount" in res.get("params_added", []), res

    # The parameter is exposed as an OUTPUT pin named Amount on the event node.
    pins = _list_pins(mcp, sample_bp, res["node_id"])
    assert any(p.get("name") == "Amount" and p.get("direction") == "Output" for p in pins), pins

    # Re-creating the same name is rejected deterministically (no auto-rename).
    dup = mcp.call("bp_add_custom_event", {
        "blueprint_name": sample_bp,
        "event_name": "MCPServerFire",
    })
    assert dup.get("error_code") == "name_collision", dup

    # The event survives a structural recompile and is visible in the BP.
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    blob = str(mcp.expect("bp_read", {"blueprint_path": sample_bp}))
    assert "MCPServerFire" in blob, blob

    # Now the create-side primitive enables a passing bp_set_event_replication test:
    # turn the freshly-authored custom event into a server RPC and read the flags back.
    rep = mcp.expect("bp_set_event_replication", {
        "blueprint_name": sample_bp,
        "event_name": "MCPServerFire",
        "replication": "server",
        "reliable": True,
    })
    assert rep.get("replication") == "server", rep
    assert rep.get("reliable") is True, rep
    # FUNC_Net (0x40) | FUNC_NetServer (0x200000) | FUNC_NetReliable (0x80) must be set.
    flags = int(rep.get("function_flags", 0))
    assert flags & 0x40 and flags & 0x200000 and flags & 0x80, rep

    # Still compiles clean after flipping the net specifiers.
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})


# ── variable ops ─────────────────────────────────────────────────────────────

@covers("bp_create_variable", "bp_get_variable_details",
        "bp_set_variable_properties")
def test_variable_details_and_properties(mcp, sample_bp):
    mcp.expect("bp_create_variable", {
        "blueprint_name": sample_bp,
        "variable_name": "MCPGraphScore",
        "variable_type": "int",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})

    details = mcp.expect("bp_get_variable_details", {
        "blueprint_path": sample_bp, "variable_name": "MCPGraphScore",
    })
    assert "MCPGraphScore" in str(details), details

    mcp.expect("bp_set_variable_properties", {
        "blueprint_name": sample_bp,
        "variable_name": "MCPGraphScore",
        "category": "MCPStats",
        "tooltip": "graph test score",
        "dry_run": False,
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    after = str(mcp.expect("bp_get_variable_details", {
        "blueprint_path": sample_bp, "variable_name": "MCPGraphScore",
    }))
    assert "MCPStats" in after, after


@covers("bp_create_variable", "bp_delete_variable", "bp_read")
def test_delete_variable(mcp, sample_bp):
    mcp.expect("bp_create_variable", {
        "blueprint_name": sample_bp,
        "variable_name": "MCPDoomedVar",
        "variable_type": "bool",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    mcp.expect("bp_delete_variable", {
        "blueprint_name": sample_bp, "variable_name": "MCPDoomedVar",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    content = str(mcp.expect("bp_read", {
        "blueprint_path": sample_bp,
    }))
    assert "MCPDoomedVar" not in content, content


# ── component ops ────────────────────────────────────────────────────────────

@covers("bp_add_component", "bp_remove_component", "bp_list_components")
def test_remove_component(mcp, sample_bp):
    mcp.expect("bp_add_component", {
        "blueprint_name": sample_bp,
        "component_type": "StaticMeshComponent",
        "component_name": "MCPDoomedComp",
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    assert "MCPDoomedComp" in str(mcp.expect(
        "bp_list_components", {"bp_path": sample_bp}))

    mcp.expect("bp_remove_component", {
        "bp_path": sample_bp,
        "component_name": "MCPDoomedComp",
        "reparent_children": True,
        "dry_run": False,
    })
    mcp.expect("bp_compile", {"blueprint_name": sample_bp})
    after = str(mcp.expect("bp_list_components", {"bp_path": sample_bp}))
    assert "MCPDoomedComp" not in after, after


# ── reparent ─────────────────────────────────────────────────────────────────

@covers("bp_create_blueprint", "bp_reparent", "bp_get_parent_class")
def test_reparent_blueprint(mcp):
    path = f"{NS}/BP_Reparent"
    ensure_absent(mcp, path)
    mcp.expect("bp_create_blueprint", {"name": path, "parent_class": "Actor"})
    mcp.expect("bp_reparent", {
        "blueprint_path": path,
        "new_parent_class": "Pawn",
    })
    parent = str(mcp.expect("bp_get_parent_class", {"bp_path": path})).lower()
    assert "pawn" in parent, parent


# ── spawn ────────────────────────────────────────────────────────────────────

@covers("bp_create_blueprint", "spawn_blueprint_actor", "actor_query", "level_inspect")
def test_spawn_blueprint_actor(mcp, bridge):
    path = f"{NS}/BP_Spawnable"
    ensure_absent(mcp, path)
    mcp.expect("bp_create_blueprint", {"name": path, "parent_class": "Actor"})
    mcp.expect("bp_compile", {"blueprint_name": path})

    # spawn_blueprint_actor is a bridge-internal command with no standalone MCP
    # tool (only the higher-level actor_spawn_physics wraps it), so this
    # one op goes through the bridge; everything else here is through the MCP.
    bridge.expect("spawn_blueprint_actor", {
        "blueprint_name": path,
        "actor_name": "MCPSpawnedActor",
        "location": [0, 0, 0],
        "rotation": [0, 0, 0],
    })
    assert_ready(mcp)

    # Readback via actor_query: a matching actor is now in the level.
    found = mcp.expect("actor_query", {
        "name_pattern": "MCPSpawned",
        "direct_only": False,
        "label": "",
        "cursor": 0,
        "limit": 200,
    })
    actors = found.get("actors", []) if isinstance(found, dict) else []
    assert any("MCPSpawned" in str(a.get("name", "")) or
               "MCPSpawned" in str(a.get("label", "")) for a in actors), found

    # level_inspect is the broader level readback used as a crash/sanity probe.
    level = mcp.expect("level_inspect", {})
    assert isinstance(level, dict) and level, level
