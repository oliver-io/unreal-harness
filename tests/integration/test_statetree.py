"""StateTree domain — author a StateTree asset, mutate its state hierarchy,
nodes, transitions and property bindings, compile/verify it, and read the
resulting state back. Driven through the real MCP server (the `mcp` fixture
calls tools by name; the tool layer maps kwargs→bridge params, etc.).

Self-contained: StateTree is authorable from scratch, so no imported content is
required. A module-scoped sample StateTree is created once under the test
namespace and reused by the mutation tests (each adds its own uniquely-named
states and references them by GUID, so the tests stay independent of one
another's ordering); the heavier create/compile tests build their own assets
idempotently. The session-end disk wipe (Content/__MCPTest__) cleans up.

Pattern for every test: arrange prerequisite state (idempotently — every create
is preceded by ensure_absent so the suite re-runs against a long-lived editor)
-> dispatch the op (raises on a non-success envelope) -> assert the resulting
state via a read/inspect op.

NOTE — tool names and kwarg keys are taken straight from the MCP tool layer
(tests/harness/tool_schemas.json is the authority). States/nodes/transitions are
referenced by GUID, so duplicate display-names across tests never cause
ambiguity.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready

NS = "/Game/__MCPTest__/statetree"
SAMPLE = f"{NS}/ST_Sample"

# Cache the discovered schema / task-node-type across the module so we hit the
# introspection ops once rather than per test.
_DISCOVERY: dict = {}


def _pick_schema(bridge) -> str:
    """Discover a usable StateTree schema class. create_state_tree requires one
    (the factory returns nullptr if the schema is unset before asset creation).
    Prefer the plain component schema; fall back to whatever the editor exposes."""
    if "schema" in _DISCOVERY:
        return _DISCOVERY["schema"]
    result = bridge.expect("statetree_list_schemas", {})
    names = [s.get("class_name") for s in (result.get("schemas") or [])
             if isinstance(s, dict)]
    schema = next((p for p in ("StateTreeComponentSchema", "StateTreeAIComponentSchema")
                   if p in names), None)
    schema = schema or (names[0] if names else "StateTreeComponentSchema")
    _DISCOVERY["schema"] = schema
    return schema


def _first_task_type(bridge):
    """Return the canonical short name of an available task node type (e.g.
    "STTask_...") or None if the editor exposes no StateTree task structs."""
    if "task" in _DISCOVERY:
        return _DISCOVERY["task"]
    result = bridge.expect("statetree_list_node_types", {"base_class": "task"})
    types = result.get("types") or []
    name = types[0].get("name") if types and isinstance(types[0], dict) else None
    _DISCOVERY["task"] = name
    return name


def _add_state(bridge, asset, name, parent=None) -> str:
    """Add a state and return its GUID. States are always referenced by GUID
    afterwards so duplicate display-names across tests never cause ambiguity."""
    params = {"asset_path": asset, "name": name, "state_type": "State", "enabled": True}
    if parent:
        params["parent"] = parent
    result = bridge.expect("statetree_state_add", params)
    sid = result.get("state_id")
    assert sid, f"st_add_state returned no state_id: {result}"
    return sid


def _add_task(bridge, asset, state_id, node_type) -> str:
    """Attach a task node to a state and return its node GUID."""
    result = bridge.expect("statetree_node_add", {
        "asset_path": asset,
        "node_type": node_type,
        "target": {"slot": "task", "state": state_id},
    })
    nid = result.get("node_id")
    assert nid, f"st_add_node returned no node_id: {result}"
    return nid


def _settable_property(props):
    """Pick a trivially-settable (scalar) property from a node's serialized
    instance_data, returning (name, value, matcher) — matcher validates the
    re-read serialized text form ("7.250000", "True", "4211", "MCPTest").
    Values are DISTINCTIVE so a readback can never pass on defaults. Avoids
    struct/enum props whose text import is finicky. Returns None if the node
    exposes no scalar."""
    for p in props:
        if not isinstance(p, dict):
            continue
        name, cpp = p.get("name"), p.get("type")
        if not name:
            continue
        if cpp == "bool":
            return name, True, (lambda v: str(v) == "True")
        if cpp in ("float", "double"):
            return name, 7.25, (lambda v: abs(float(v) - 7.25) < 1e-4)
        if cpp in ("int32", "int64", "int", "uint8", "uint32"):
            return name, 42, (lambda v: int(float(v)) == 42)
        if cpp in ("FString", "FName"):
            return name, "MCPTest", (lambda v: str(v) == "MCPTest")
    return None


def _scalar_task_type(bridge):
    """A task node type whose instance data carries a plain scalar, so the
    set-property test can actually WRITE and RE-READ a value. Prefers the
    engine-shipped StateTreeDelayTask (Duration float); falls back to the first
    type advertising a scalar in statetree_list_node_types. None when the
    editor exposes no such task (the generic types[0] pick is often
    StateTreeBlueprintTaskWrapper, which has no instance properties)."""
    if "scalar_task" in _DISCOVERY:
        return _DISCOVERY["scalar_task"]
    result = bridge.expect("statetree_list_node_types", {"base_class": "task"})
    types = [t for t in (result.get("types") or []) if isinstance(t, dict)]
    scalar = {"bool", "float", "double", "int32", "int64", "int", "uint8", "uint32",
              "FString", "FName"}

    def has_scalar(t):
        return any(isinstance(p, dict) and p.get("type") in scalar
                   for p in (t.get("properties") or []))

    name = None
    if any(t.get("name") == "StateTreeDelayTask" and has_scalar(t) for t in types):
        name = "StateTreeDelayTask"
    else:
        name = next((t.get("name") for t in types if has_scalar(t)), None)
    _DISCOVERY["scalar_task"] = name
    return name


@pytest.fixture(scope="module")
def sample_st(_mcp_client):
    """Create one StateTree asset for the whole module."""
    ensure_absent(_mcp_client, SAMPLE)
    _mcp_client.expect("statetree_create", {
        "asset_path": SAMPLE,
        "schema_class": _pick_schema(_mcp_client),
    })
    return SAMPLE


# ── introspection ───────────────────────────────────────────────────────────

@covers("statetree_list_schemas")
def test_list_state_tree_schemas(mcp):
    result = mcp.expect("statetree_list_schemas", {})
    schemas = result.get("schemas")
    assert isinstance(schemas, list) and schemas, result
    assert all("class_name" in s for s in schemas), result


@covers("statetree_list_node_types")
def test_list_state_tree_node_types(mcp):
    result = mcp.expect("statetree_list_node_types", {"base_class": "all"})
    types = result.get("types")
    assert isinstance(types, list), result
    assert result.get("count") == len(types), result


# ── creation: persisted assets land on disk ─────────────────────────────────

@covers("statetree_create", "statetree_save")
def test_create_state_tree_writes_uasset_on_disk(mcp):
    path = f"{NS}/ST_Created"
    ensure_absent(mcp, path)
    result = mcp.expect("statetree_create", {
        "asset_path": path,
        "schema_class": _pick_schema(mcp),
    })
    assert result.get("success") is True, result
    # create_state_tree saves the package itself, so the .uasset must exist now.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after create_state_tree"
    # Raw save so the disk-write is re-confirmed; save must report success.
    saved = mcp.expect("statetree_save", {"asset_path": path})
    assert saved.get("success") is True, saved


# ── state hierarchy ─────────────────────────────────────────────────────────

@covers("st_add_state", "st_list_states")
def test_add_state_then_list(mcp, sample_st):
    sid = _add_state(mcp, sample_st, "S_AddList")
    result = mcp.expect("statetree_state_list", {"asset_path": sample_st})
    states = result.get("states") or []
    ids = {s.get("id") for s in states}
    assert sid in ids, f"new state {sid} not present in {ids}"


@covers("statetree_read")
def test_read_state_tree(mcp, sample_st):
    sid = _add_state(mcp, sample_st, "S_Read")
    result = mcp.expect("statetree_read", {
        "asset_path": sample_st,
        "include_node_properties": True,
        "include_bindings": True,
    })
    assert sid in str(result.get("states")), result
    assert result.get("asset_path"), result


@covers("st_rename_state", "st_add_state")
def test_rename_state(mcp, sample_st):
    sid = _add_state(mcp, sample_st, "S_RenameBefore")
    result = mcp.expect("statetree_state_rename", {
        "asset_path": sample_st, "state": sid, "new_name": "S_RenameAfter",
    })
    assert result.get("new_name") == "S_RenameAfter", result
    states = mcp.expect("statetree_state_list", {"asset_path": sample_st}).get("states") or []
    entry = next((s for s in states if s.get("id") == sid), None)
    assert entry and entry.get("name") == "S_RenameAfter", entry


@covers("st_set_state_properties", "st_add_state")
def test_set_state_properties(mcp, sample_st):
    sid = _add_state(mcp, sample_st, "S_SetProps")
    mcp.expect("statetree_state_set_properties", {
        "asset_path": sample_st, "state": sid,
        "selection_behavior": "TrySelectChildrenInOrder",
        "enabled": False,
        "weight": 2.0,
    })
    states = mcp.expect("statetree_state_list", {"asset_path": sample_st}).get("states") or []
    entry = next((s for s in states if s.get("id") == sid), None)
    assert entry, f"state {sid} vanished"
    assert entry.get("selection_behavior") == "TrySelectChildrenInOrder", entry
    assert entry.get("enabled") is False, entry
    assert entry.get("weight") == pytest.approx(2.0), entry


@covers("st_move_state", "st_add_state")
def test_move_state(mcp, sample_st):
    parent = _add_state(mcp, sample_st, "S_MoveParent")
    child = _add_state(mcp, sample_st, "S_MoveChild", parent=parent)
    # Reparent the child up to the root level (omit new_parent -> root).
    result = mcp.expect("statetree_state_move", {"asset_path": sample_st, "state": child})
    assert result.get("state_id") == child, result
    states = mcp.expect("statetree_state_list", {"asset_path": sample_st}).get("states") or []
    entry = next((s for s in states if s.get("id") == child), None)
    assert entry, f"moved state {child} vanished"
    # Root-level states carry no parent_id and sit at depth 0.
    assert "parent_id" not in entry, entry
    assert entry.get("depth") == 0, entry


@covers("st_duplicate_state", "st_add_state")
def test_duplicate_state(mcp, sample_st):
    sid = _add_state(mcp, sample_st, "S_DupSource")
    result = mcp.expect("statetree_state_duplicate", {
        "asset_path": sample_st, "state": sid, "new_name": "S_DupCopy",
    })
    new_id = result.get("state_id")
    assert new_id and new_id != sid, result
    assert isinstance(result.get("guid_mapping"), dict), result
    states = mcp.expect("statetree_state_list", {"asset_path": sample_st}).get("states") or []
    names = {s.get("name") for s in states}
    assert "S_DupCopy" in names, names


@covers("st_remove_state", "st_add_state")
def test_remove_state(mcp, sample_st):
    sid = _add_state(mcp, sample_st, "S_ToRemove")
    result = mcp.expect("statetree_state_remove", {"asset_path": sample_st, "state": sid})
    assert result.get("removed_count", 0) >= 1, result
    assert_ready(mcp)
    states = mcp.expect("statetree_state_list", {"asset_path": sample_st}).get("states") or []
    assert sid not in {s.get("id") for s in states}, "removed state still listed"


# ── transitions ─────────────────────────────────────────────────────────────

@covers("st_add_transition", "st_set_transition_properties", "st_remove_transition",
        "st_add_state")
def test_transition_lifecycle(mcp, sample_st):
    src = _add_state(mcp, sample_st, "S_TransSrc")
    dst = _add_state(mcp, sample_st, "S_TransDst")
    # Add a GotoState transition (target referenced by GUID).
    added = mcp.expect("statetree_transition_add", {
        "asset_path": sample_st, "state": src,
        "trigger": "OnStateCompleted", "target": dst, "priority": "Normal",
    })
    tid = added.get("transition_id")
    assert tid, added

    # Mutate it.
    mcp.expect("statetree_transition_set_properties", {
        "asset_path": sample_st, "transition_id": tid,
        "priority": "High", "trigger": "OnTick",
    })

    # Read back and confirm the mutated transition is attached to the source.
    read = mcp.expect("statetree_read", {"asset_path": sample_st, "state_id": src})
    src_entry = next((s for s in (read.get("states") or []) if s.get("id") == src), None)
    assert src_entry, read
    trans = next((t for t in (src_entry.get("transitions") or []) if t.get("id") == tid), None)
    assert trans, src_entry
    assert trans.get("priority") == "High", trans
    assert trans.get("trigger") == "OnTick", trans

    # Remove it.
    removed = mcp.expect("statetree_transition_remove", {
        "asset_path": sample_st, "transition_id": tid,
    })
    assert removed.get("success") is True, removed
    read2 = mcp.expect("statetree_read", {"asset_path": sample_st, "state_id": src})
    src_entry2 = next((s for s in (read2.get("states") or []) if s.get("id") == src), None)
    assert tid not in str(src_entry2.get("transitions")), src_entry2


# ── nodes ───────────────────────────────────────────────────────────────────

@covers("st_add_node", "st_get_node_properties", "st_list_bindable_properties",
        "st_remove_node", "st_add_state")
def test_node_lifecycle(mcp, sample_st):
    node_type = _first_task_type(mcp)
    if not node_type:
        pytest.skip("editor exposes no StateTree task node types")
    sid = _add_state(mcp, sample_st, "S_NodeHost")
    nid = _add_task(mcp, sample_st, sid, node_type)

    props = mcp.expect("statetree_node_get_properties", {"asset_path": sample_st, "node_id": nid})
    assert props.get("node_id") == nid, props
    assert "instance_data" in props, props

    bindable = mcp.expect("statetree_binding_list_bindable", {
        "asset_path": sample_st, "node_id": nid,
    })
    assert "node_properties" in bindable, bindable
    assert "available_sources" in bindable, bindable

    removed = mcp.expect("statetree_node_remove", {"asset_path": sample_st, "node_id": nid})
    assert removed.get("success") is True, removed
    assert_ready(mcp)
    # The node should no longer resolve.
    gone = mcp.command("statetree_node_get_properties", {"asset_path": sample_st, "node_id": nid})
    assert gone.get("status") == "error", gone


@covers("st_set_node_property", "st_add_node", "st_add_state", "st_get_node_properties")
def test_set_node_property(mcp, sample_st):
    # A scalar-bearing task type (e.g. StateTreeDelayTask.Duration) — the
    # generic first-type pick is often StateTreeBlueprintTaskWrapper, whose
    # instance data has no scalar, which used to silently skip this test.
    node_type = _scalar_task_type(mcp)
    if not node_type:
        pytest.skip("editor exposes no StateTree task type with a scalar property")
    sid = _add_state(mcp, sample_st, "S_SetNodeProp")
    nid = _add_task(mcp, sample_st, sid, node_type)

    props = mcp.expect("statetree_node_get_properties", {"asset_path": sample_st, "node_id": nid})
    instance = props.get("instance_data") or {}
    chosen = _settable_property(instance.get("properties") or [])
    assert chosen, f"scalar task type {node_type} exposed no scalar instance property: {props}"
    name, value, matches = chosen
    result = mcp.expect("statetree_node_set_property", {
        "asset_path": sample_st, "node_id": nid,
        "property_name": name, "property_value": value,
    })
    assert result.get("success") is True, result
    assert result.get("property_name") == name, result
    # Independent readback via statetree_node_get_properties (the pattern the
    # node-lifecycle test above already uses): the instance-data property must
    # now serialize the distinctive written value, not its default.
    reread = mcp.expect("statetree_node_get_properties", {
        "asset_path": sample_st, "node_id": nid})
    entry = next((p for p in ((reread.get("instance_data") or {}).get("properties") or [])
                  if p.get("name") == name), None)
    assert entry, reread
    assert matches(entry.get("value")), f"{name} re-read as {entry.get('value')!r}, wrote {value!r}"


# ── property bindings ───────────────────────────────────────────────────────

@covers("st_add_property_binding", "st_list_property_bindings",
        "st_remove_property_binding", "st_add_node", "st_add_state")
def test_property_binding_lifecycle(mcp, sample_st):
    node_type = _first_task_type(mcp)
    if not node_type:
        pytest.skip("editor exposes no StateTree task node types")
    sid = _add_state(mcp, sample_st, "S_BindHost")
    src_node = _add_task(mcp, sample_st, sid, node_type)
    dst_node = _add_task(mcp, sample_st, sid, node_type)

    mcp.expect("statetree_binding_add", {
        "asset_path": sample_st,
        "source_node_id": src_node, "source_property": "Output",
        "target_node_id": dst_node, "target_property": "MCPBoundValue",
    })

    listed = mcp.expect("statetree_binding_list", {"asset_path": sample_st})
    bindings = listed.get("bindings") or []
    match = next((b for b in bindings
                  if b.get("target_node_id") == dst_node
                  and "MCPBoundValue" in str(b.get("target_property"))), None)
    assert match, f"expected binding onto {dst_node} not found in {bindings}"

    mcp.expect("statetree_binding_remove", {
        "asset_path": sample_st,
        "target_node_id": dst_node, "target_property": "MCPBoundValue",
    })
    listed2 = mcp.expect("statetree_binding_list", {"asset_path": sample_st})
    still = [b for b in (listed2.get("bindings") or [])
             if b.get("target_node_id") == dst_node
             and "MCPBoundValue" in str(b.get("target_property"))]
    assert not still, f"binding survived removal: {still}"


# ── compile / verify ────────────────────────────────────────────────────────

@covers("statetree_compile", "statetree_verify", "statetree_read", "statetree_save")
def test_compile_verify_and_save(mcp):
    """statetree_compile is observed through statetree_verify (an independent,
    non-mutating diagnostic): after mutation the tree verifies STALE (author
    hash diverged), after compile it verifies OK with hash_matches true and the
    stored hash equal to the compile's stamped editor_data_hash. Note (verified
    live): ready_to_run does NOT discriminate — it stays true even when stale,
    because the previously-linked compiled data remains loaded; hash_matches /
    status is the compile's real observable."""
    path = f"{NS}/ST_Compile"
    ensure_absent(mcp, path)
    mcp.expect("statetree_create", {"asset_path": path, "schema_class": _pick_schema(mcp)})
    sid = _add_state(mcp, path, "S_Compiled")

    # Before: the state-add diverged the author-time hash from the compiled stamp.
    before = mcp.expect("statetree_verify", {"asset_path": path})
    assert before.get("hash_matches") is not True, before
    assert before.get("status") in ("stale", "never_compiled"), before

    compiled = mcp.expect("statetree_compile", {"asset_path": path, "auto_save": False})
    assert compiled.get("success") is True, compiled
    assert compiled.get("compilation_status") == "success", compiled
    assert "editor_data_hash" in compiled, compiled
    assert_ready(mcp)

    # The author-time read reflects the state regardless of compile outcome.
    read = mcp.expect("statetree_read", {"asset_path": path})
    assert sid in str(read.get("states")), read

    # After: the independent diagnostic reports the compile landed.
    verified = mcp.expect("statetree_verify", {"asset_path": path})
    assert verified.get("status") == "ok", verified
    assert verified.get("hash_matches") is True, verified
    assert verified.get("ready_to_run") is True, verified
    assert verified.get("stored_hash") == compiled.get("editor_data_hash"), (verified, compiled)

    saved = mcp.expect("statetree_save", {"asset_path": path})
    assert saved.get("success") is True, saved
