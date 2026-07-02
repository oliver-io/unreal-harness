"""Animation state-machine domain — author a state machine inside an Anim
Blueprint's AnimGraph, mutate its states/transitions/entry, set an inner
AnimNode property, and read the resulting graph topology back. Driven through
the real MCP server (the `mcp` fixture calls tools by name; the tool layer maps
kwargs→bridge params, etc.).

What these ops operate on
-------------------------
``create_state_machine`` and friends operate on a **UAnimBlueprint**. A state
machine is a ``UAnimGraphNode_StateMachine`` placed in the AnimBP's *AnimGraph*;
its ``EditorStateMachineGraph`` (a ``UAnimationStateMachineGraph``) holds
``UAnimStateNode`` states and ``UAnimStateTransitionNode`` transitions. So every
test needs an Anim Blueprint first — built from scratch via
``create_anim_blueprint`` against the engine ``SkeletalCube_Skeleton`` (no
imported content required).

Read-back
---------
``list_blueprint_graphs`` is the authoritative reader here: it recursively
enumerates the state machine sub-graphs and emits one entry per
  * state machine  -> category "state_machine", name == machine graph name
  * state          -> category "state", parent_state_machine == machine name,
                      name == the state's inner (bound) graph name
  * transition     -> category "transition", parent_state_machine == machine name,
                      carries from_state / to_state
That lets each mutation be confirmed structurally rather than only by the op's
own success envelope.

Conventions (mirrors test_blueprint.py / test_statetree.py)
-----------------------------------------------------------
Tool names and kwarg keys are taken straight from the MCP tool layer
(tests/harness/tool_schemas.json is the authority): ``blueprint_name``,
``state_machine_graph``, ``state_name``, ``from_state`` / ``to_state``,
``transition_id``, ``node_id`` / ``property_name`` / ``property_value``.

Each test builds its own freshly-named state machine inside the shared module
Anim Blueprint and references it by the machine's returned graph name, so the
tests stay independent of ordering. The session-end disk wipe
(Content/__MCPTest__) cleans up.

Headless-safe: every op is a pure graph/asset mutation via NewObject / factory /
reflection — none open an asset-editor window — so no ``gui_only`` marker.
"""

import pytest

from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of

NS = "/Game/__MCPTest__/statemachine"
SAMPLE = f"{NS}/ABP_Sample"

# Confirmed-present engine skeleton — lets an Anim Blueprint be authored from
# scratch with no imported project content.
ENGINE_SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton"


# ── helpers ──────────────────────────────────────────────────────────────────

def _new_machine(bridge, abp, name: str) -> str:
    """Create a state machine in the AnimBP's AnimGraph and return the actual
    machine graph name (post-rename, authoritative for all later ops)."""
    result = bridge.expect("anim_state_machine_create", {
        "blueprint_name": abp["path"],
        "machine_name": name,
        "graph_name": abp["anim_graph"],
    })
    assert result.get("success") is True, result
    machine = result.get("machine_name")
    assert machine, f"create_state_machine returned no machine_name: {result}"
    # node_id is the UAnimGraphNode_StateMachine name (used by set_inner_node_property).
    return machine, result.get("node_id")


def _add_state(bridge, abp, machine: str, name: str):
    """Add a state; return (node_id, inner_graph_name)."""
    result = bridge.expect("anim_state_machine_state_add", {
        "blueprint_name": abp["path"],
        "state_machine_graph": machine,
        "state_name": name,
    })
    assert result.get("success") is True, result
    nid, inner = result.get("node_id"), result.get("inner_graph")
    assert nid and inner, f"add_state returned no node_id/inner_graph: {result}"
    return nid, inner


def _graphs(bridge, abp):
    """All graph entries reported for the Anim Blueprint."""
    listed = bridge.expect("bp_list_graphs", {"blueprint_path": abp["path"]})
    graphs = listed.get("graphs")
    assert isinstance(graphs, list), listed
    return graphs


def _machine_graphs(bridge, abp, machine: str):
    """Entries whose parent_state_machine is the given machine (states + transitions)."""
    return [g for g in _graphs(bridge, abp) if g.get("parent_state_machine") == machine]


# ── fixture: the shared Anim Blueprint ───────────────────────────────────────

@pytest.fixture(scope="module")
def sample_abp(_mcp_client):
    """Create one Anim Blueprint (from the engine skeleton) for the whole module.
    Skips the module if no USkeleton can be resolved to author against."""
    from harness.mcp_client import MCPError, MCPToolError

    name = SAMPLE.rsplit("/", 1)[1]
    pkg = SAMPLE.rsplit("/", 1)[0]
    ensure_absent(_mcp_client, SAMPLE)

    # Prefer the always-present engine skeleton; fall back to any project skeleton.
    candidates = [ENGINE_SKELETON]
    found = first_asset_of(_mcp_client, "anim_list_skeletons", {}, items_key="skeletons")
    if isinstance(found, dict) and found.get("path"):
        candidates.append(found["path"])

    last_err = None
    for sk in candidates:
        try:
            result = _mcp_client.expect("anim_blueprint_create", {
                "name": name, "skeleton_path": sk, "package_path": pkg,
            })
            return {"path": SAMPLE, "anim_graph": result.get("anim_graph") or "AnimGraph"}
        except (MCPError, MCPToolError) as e:
            last_err = e
            ensure_absent(_mcp_client, SAMPLE)
    pytest.skip(f"could not create an Anim Blueprint from any skeleton: {last_err}")


# ── creation ─────────────────────────────────────────────────────────────────

@covers("anim_state_machine_create")
def test_create_state_machine(mcp, sample_abp):
    machine, node_id = _new_machine(mcp, sample_abp, "SM_Create")
    assert node_id, "create_state_machine returned no node_id"
    # Read back: the machine appears as a category=="state_machine" graph.
    sm = next((g for g in _graphs(mcp, sample_abp)
               if g.get("category") == "state_machine" and g.get("name") == machine), None)
    assert sm, f"state machine {machine} not listed among graphs"


# ── states ───────────────────────────────────────────────────────────────────

@covers("anim_state_machine_state_add", "anim_state_machine_create")
def test_add_state_then_list(mcp, sample_abp):
    machine, _ = _new_machine(mcp, sample_abp, "SM_AddState")
    _, inner = _add_state(mcp, sample_abp, machine, "S_Idle")
    # Read back: the new state's inner graph is listed under the machine.
    states = [g for g in _machine_graphs(mcp, sample_abp, machine)
              if g.get("category") == "state"]
    assert any(g.get("name") == inner for g in states), \
        f"state inner-graph {inner} not found under {machine}: {states}"


@covers("anim_state_machine_state_remove", "anim_state_machine_state_add", "anim_state_machine_create")
def test_remove_state(mcp, sample_abp):
    machine, _ = _new_machine(mcp, sample_abp, "SM_RemoveState")
    _, inner = _add_state(mcp, sample_abp, machine, "S_Doomed")
    assert any(g.get("name") == inner for g in _machine_graphs(mcp, sample_abp, machine)), \
        "state did not appear before removal"

    result = mcp.expect("anim_state_machine_state_remove", {
        "blueprint_name": sample_abp["path"],
        "state_machine_graph": machine,
        "state_name": "S_Doomed",
    })
    assert result.get("success") is True, result
    assert_ready(mcp)
    # Read back: the inner graph is gone.
    names = {g.get("name") for g in _machine_graphs(mcp, sample_abp, machine)}
    assert inner not in names, f"removed state {inner} still listed: {names}"


# ── transitions ──────────────────────────────────────────────────────────────

@covers("anim_state_machine_transition_add", "anim_state_machine_state_add", "anim_state_machine_create")
def test_add_transition(mcp, sample_abp):
    machine, _ = _new_machine(mcp, sample_abp, "SM_AddTrans")
    _add_state(mcp, sample_abp, machine, "S_A")
    _add_state(mcp, sample_abp, machine, "S_B")
    result = mcp.expect("anim_state_machine_transition_add", {
        "blueprint_name": sample_abp["path"],
        "state_machine_graph": machine,
        "from_state": "S_A",
        "to_state": "S_B",
    })
    assert result.get("success") is True, result
    assert result.get("node_id"), result
    # Read back: a transition graph A->B exists under the machine.
    trans = [g for g in _machine_graphs(mcp, sample_abp, machine)
             if g.get("category") == "transition"]
    assert any(g.get("from_state") == "S_A" and g.get("to_state") == "S_B" for g in trans), \
        f"transition S_A->S_B not listed: {trans}"


@covers("anim_state_machine_modify_transition", "anim_state_machine_transition_add", "anim_state_machine_state_add", "anim_state_machine_create")
def test_modify_transition(mcp, sample_abp):
    machine, _ = _new_machine(mcp, sample_abp, "SM_ModTrans")
    _add_state(mcp, sample_abp, machine, "S_M0")
    _add_state(mcp, sample_abp, machine, "S_M1")
    added = mcp.expect("anim_state_machine_transition_add", {
        "blueprint_name": sample_abp["path"],
        "state_machine_graph": machine,
        "from_state": "S_M0",
        "to_state": "S_M1",
    })
    tid = added.get("node_id")
    assert tid, added

    # Modify by transition_id (the handler also accepts from_state+to_state).
    modified = mcp.expect("anim_state_machine_modify_transition", {
        "blueprint_name": sample_abp["path"],
        "state_machine_graph": machine,
        "transition_id": tid,
        "blend_duration": 0.35,
        "priority_order": 2,
    })
    assert modified.get("success") is True, modified
    assert modified.get("node_id") == tid, modified
    # The transition still resolves after the edit.
    trans = [g for g in _machine_graphs(mcp, sample_abp, machine)
             if g.get("category") == "transition"]
    assert any(g.get("from_state") == "S_M0" and g.get("to_state") == "S_M1" for g in trans), trans


@covers("anim_state_machine_transition_remove", "anim_state_machine_transition_add", "anim_state_machine_state_add", "anim_state_machine_create")
def test_remove_transition(mcp, sample_abp):
    machine, _ = _new_machine(mcp, sample_abp, "SM_RemoveTrans")
    _add_state(mcp, sample_abp, machine, "S_R0")
    _add_state(mcp, sample_abp, machine, "S_R1")
    mcp.expect("anim_state_machine_transition_add", {
        "blueprint_name": sample_abp["path"],
        "state_machine_graph": machine,
        "from_state": "S_R0",
        "to_state": "S_R1",
    })
    assert any(g.get("category") == "transition" and g.get("from_state") == "S_R0"
               for g in _machine_graphs(mcp, sample_abp, machine)), "transition not added"

    # Remove by from_state+to_state (alternative to transition_id).
    removed = mcp.expect("anim_state_machine_transition_remove", {
        "blueprint_name": sample_abp["path"],
        "state_machine_graph": machine,
        "from_state": "S_R0",
        "to_state": "S_R1",
    })
    assert removed.get("success") is True, removed
    assert_ready(mcp)
    trans = [g for g in _machine_graphs(mcp, sample_abp, machine)
             if g.get("category") == "transition"]
    assert not any(g.get("from_state") == "S_R0" and g.get("to_state") == "S_R1" for g in trans), \
        f"transition survived removal: {trans}"


# ── entry state ──────────────────────────────────────────────────────────────

@covers("anim_state_machine_set_entry", "anim_state_machine_state_add", "anim_state_machine_create")
def test_set_entry_state(mcp, sample_abp):
    machine, _ = _new_machine(mcp, sample_abp, "SM_Entry")
    _add_state(mcp, sample_abp, machine, "S_Entry")
    result = mcp.expect("anim_state_machine_set_entry", {
        "blueprint_name": sample_abp["path"],
        "state_machine_graph": machine,
        "state_name": "S_Entry",
    })
    assert result.get("success") is True, result
    assert result.get("entry_state") == "S_Entry", result
    assert_ready(mcp)


# ── inner AnimNode property ──────────────────────────────────────────────────

@covers("bp_set_inner_node_property", "anim_state_machine_create")
def test_set_inner_node_property(mcp, sample_abp):
    # Target the state machine node itself — a UAnimGraphNode_StateMachine is a
    # UAnimGraphNode_Base, so set_inner_node_property writes onto its inner
    # FAnimNode_StateMachine struct. MaxTransitionsPerFrame is a plain int32
    # that ImportText parses cleanly.
    _, node_id = _new_machine(mcp, sample_abp, "SM_InnerProp")
    assert node_id, "create_state_machine returned no node_id"
    result = mcp.expect("bp_set_inner_node_property", {
        "blueprint_name": sample_abp["path"],
        "node_id": node_id,
        "property_name": "MaxTransitionsPerFrame",
        "property_value": "5",
        "graph_name": sample_abp["anim_graph"],
    })
    assert result.get("success") is True, result
    assert result.get("value") == "5", result
    assert_ready(mcp)
