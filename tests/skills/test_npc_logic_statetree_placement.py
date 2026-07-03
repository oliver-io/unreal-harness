"""Skill-test loop TASK-6 — /npc_logic: StateTree Layer-3 placement guard.

The /npc_logic skill's Layer-3 architecture (SKILL.md:89-92) makes three
structural claims about where things LIVE in a StateTree authored for AI:

  1. the decision layer runs under the AI component schema (SKILL.md:89 — a
     schema guaranteeing the AIController context);
  2. "Conditions gate transitions" (SKILL.md:90) — a condition node attaches
     to a TRANSITION's condition list, not to a state's task list;
  3. "Parallel tasks compose a mode … two tasks under one state, not a
     sub-tree" (SKILL.md:92) — two task nodes sit flat in ONE state's Tasks.

This battery pins all three against the real authoring surface: arrange via
the typed statetree_* mutators, observe via the independent statetree_read
serializer (a different primitive re-walking the live UStateTreeEditorData).

Handler ground truth (read this task):
  `StateTreeNodeMgr.cpp:118-124` — target.slot vocabulary ("task",
  "enter_condition", "consideration", "condition", "evaluator",
  "global_task"). `:193-234` — slot "task" inserts into `State->Tasks`
  (:208-211), slot "enter_condition" into `State->EnterConditions`
  (:213-217): distinct arrays on the same state. `:236-284` — slot
  "condition" requires `target.transition` (a GUID) and inserts into that
  transition's `Trans.Conditions` (:270-273) — structurally impossible to
  land a transition condition on a state, which is exactly the placement
  discipline the skill teaches.
  `MCPStateTreeCommands.cpp` (HandleReadStateTree) — the observer:
  `schema_class` = EditorData->Schema class name (:261); per-state `tasks[]`
  with node `id` (:341-357); `enter_conditions[]` (:364-381); `depth` (:333)
  and `child_count` (:334); per-transition entries with `id`, `trigger`,
  `target_id` and `conditions[]` with node `id` (:407-488, conditions at
  :467-484).
Engine ground truth (UE 5.7 source, read this task):
  `Engine/Plugins/Runtime/StateTree/Source/StateTreeEditorModule/Public/
  StateTreeState.h:422` — `UStateTreeState::Tasks`
  (TArray<FStateTreeEditorNode>); `:419` — `EnterConditions`; `:161` —
  `FStateTreeTransition::Conditions` — three separate arrays, so the
  placement the skill teaches is the engine's own data model.
  `Engine/Plugins/Runtime/GameplayStateTree/Source/GameplayStateTreeModule/
  Public/Components/StateTreeAIComponentSchema.h:15,18-19` —
  `UStateTreeAIComponentSchema : public UStateTreeComponentSchema`,
  documented as guaranteeing access to an AIController ("It guarantees
  access to an AIController and the Actor context value can be used to
  access the controlled pawn") — the schema the skill's decision layer
  targets. (NOTE: the read primitive surfaces only the schema CLASS name;
  the schema's context-data descriptors — the AIController guarantee itself
  — have no read primitive. Ledgered in TASKS.md DEFERRED / PROPOSALS.md.)

Deliberately NOT compiled: `statetree_compile` is covered by
`tests/integration/test_statetree.py::test_compile_verify_and_save`, and a
compile verdict on stock engine nodes with unbound context data would test
the compiler, not the placement claims.

Headless-safe (no render/gui_only markers), bounded, self-cleaning: one
StateTree asset under /Game/__MCPTest__/skills_npc_logic, ensure-absent
before create, asset_delete force in teardown (plus the session-end
Content/__MCPTest__ disk wipe backstop). The statetree_* mutators are
PIE-blocked at the server gate (domains/statetree.ts blockedDuringPie;
statetree_create also in the C++ blocklist, MCPCommonUtils.cpp:312), so if
PIE is live on the shared editor we skip rather than collide with someone
else's session. Transient `engine_busy` handled with the bounded retry
pattern from the sibling skills tests.
"""

import time

import pytest

from harness.mcp_client import MCPToolError
from harness.ops import ensure_absent

NS = "/Game/__MCPTest__/skills_npc_logic"
ST_PATH = f"{NS}/ST_NpcLogicPlacement"
SCHEMA = "StateTreeAIComponentSchema"


# ── helpers ──────────────────────────────────────────────────────────────────

def _pie_is_running(client) -> bool:
    r = client.call("pie_get_state", {})
    inner = r.get("result") if isinstance(r.get("result"), dict) else r
    return bool(inner.get("is_running"))


def _retry_busy(fn, attempts=3, delay=2.0):
    """Bounded retry for the shared live editor's transient `engine_busy`
    (bridge receive timeout while another agent's op holds the game thread).
    Safe for every step here: the reads are idempotent, and a retried add
    after a timed-out-but-landed first attempt is caught by the assertions
    (we match nodes by the GUID the successful call returned, and a duplicate
    state/task would surface in the read-back)."""
    last = None
    for _ in range(attempts):
        try:
            return fn()
        except MCPToolError as e:
            payload = e.payload if isinstance(e.payload, dict) else {}
            if payload.get("error_code") != "engine_busy":
                raise
            last = e
            time.sleep(delay)
    raise last


def _first_type(mcp, family: str):
    """Discover one concrete node type of the given family via the same
    primitive the skill's discovery step names (statetree_list_node_types).
    The taxonomy battery (test_npc_logic_statetree_taxonomy.py) pins these
    families non-empty; skip (not fail) here so an empty family reads as its
    finding, not this test's."""
    result = _retry_busy(lambda: mcp.expect(
        "statetree_list_node_types", {"base_class": family}))
    types = result.get("types") or []
    name = types[0].get("name") if types and isinstance(types[0], dict) else None
    if not name:
        pytest.skip(f"editor exposes no StateTree {family} node types "
                    f"(taxonomy guard should have caught this)")
    return name


def _add_state(mcp, name: str) -> str:
    result = _retry_busy(lambda: mcp.expect("statetree_state_add", {
        "asset_path": ST_PATH, "name": name,
        "state_type": "State", "enabled": True,
    }))
    sid = result.get("state_id")
    assert sid, f"statetree_state_add returned no state_id: {result}"
    return sid


def _add_node(mcp, node_type: str, target: dict) -> str:
    result = _retry_busy(lambda: mcp.expect("statetree_node_add", {
        "asset_path": ST_PATH, "node_type": node_type, "target": target,
    }))
    nid = result.get("node_id")
    assert nid, f"statetree_node_add returned no node_id: {result}"
    return nid


def _ids(nodes) -> list:
    return [n.get("id") for n in (nodes or []) if isinstance(n, dict)]


# ── fixture ──────────────────────────────────────────────────────────────────

@pytest.fixture(scope="module")
def placement_st(_mcp_client):
    """One StateTree asset under the AI component schema for the module.
    Deleted in teardown; the shared editor may outlive this run."""
    if _pie_is_running(_mcp_client):
        pytest.skip("PIE is running on the shared editor — the statetree_* "
                    "mutators are PIE-blocked (domains/statetree.ts "
                    "blockedDuringPie); not stopping someone else's session")
    ensure_absent(_mcp_client, ST_PATH)
    _mcp_client.expect("statetree_create", {
        "asset_path": ST_PATH, "schema_class": SCHEMA,
    })
    try:
        yield ST_PATH
    finally:
        try:
            _mcp_client.command("asset_delete",
                                {"asset_path": ST_PATH, "force": True})
        except Exception:
            pass


# ── the placement battery ────────────────────────────────────────────────────

def test_layer3_placement_round_trip(mcp, placement_st):
    """One test, one story — the skill's Engage/Searching example built for
    real: two parallel tasks flat under Engage, an OnTick Engage→Searching
    transition gated by a condition ON THE TRANSITION, all read back through
    the independent statetree_read serializer under the AI schema."""
    task_type = _first_type(mcp, "task")
    cond_type = _first_type(mcp, "condition")

    # Arrange — the skill's Layer-3 shape.
    engage = _add_state(mcp, "Engage")
    searching = _add_state(mcp, "Searching")

    # "Parallel tasks compose a mode": TWO tasks on the ONE flat Engage state
    # (StateTreeNodeMgr.cpp:208-211 → UStateTreeState::Tasks,
    # StateTreeState.h:422).
    task_a = _add_node(mcp, task_type, {"slot": "task", "state": engage})
    task_b = _add_node(mcp, task_type, {"slot": "task", "state": engage})
    assert task_a != task_b, "two adds must yield two distinct node GUIDs"

    # "Conditions gate transitions": Engage→Searching on tick, condition
    # attached to the TRANSITION (StateTreeNodeMgr.cpp:236-273 →
    # FStateTreeTransition::Conditions, StateTreeState.h:161).
    added = _retry_busy(lambda: mcp.expect("statetree_transition_add", {
        "asset_path": ST_PATH, "state": engage,
        "trigger": "OnTick", "target": searching, "priority": "Normal",
    }))
    trans_id = added.get("transition_id")
    assert trans_id, added
    cond = _add_node(mcp, cond_type,
                     {"slot": "condition", "transition": trans_id})

    # Observe — different primitive: statetree_read re-serializes the live
    # editor data (MCPStateTreeCommands.cpp HandleReadStateTree).
    read = _retry_busy(lambda: mcp.expect("statetree_read", {
        "asset_path": ST_PATH,
        "include_node_properties": True,
    }))

    # (1) the AI schema round-trips (:261 emits the schema's class name).
    assert read.get("schema_class") == SCHEMA, read

    states = read.get("states") or []
    engage_entry = next((s for s in states if s.get("id") == engage), None)
    assert engage_entry, f"Engage {engage} missing from read-back: {states}"

    # (2) both parallel tasks live in Engage's tasks[] (:341-357) — and the
    # state is FLAT: no children, root depth (:333-334). "Two tasks under one
    # state, not a sub-tree."
    engage_tasks = _ids(engage_entry.get("tasks"))
    assert task_a in engage_tasks and task_b in engage_tasks, (
        f"expected both task GUIDs in Engage.tasks; got {engage_tasks}")
    assert engage_entry.get("child_count") == 0, engage_entry
    assert engage_entry.get("depth") == 0, engage_entry

    # (3) the condition sits on the transition (:467-484), which targets
    # Searching by GUID (:454-456).
    trans = next((t for t in (engage_entry.get("transitions") or [])
                  if t.get("id") == trans_id), None)
    assert trans, f"transition {trans_id} missing: {engage_entry}"
    assert trans.get("trigger") == "OnTick", trans
    assert trans.get("target_id") == searching, trans
    assert cond in _ids(trans.get("conditions")), (
        f"condition GUID {cond} not in transition conditions: {trans}")

    # (4) negative placement: the transition condition is NOT a state task
    # and NOT an enter condition — three distinct arrays in the engine's own
    # model (StateTreeState.h:161,419,422), kept distinct by the harness.
    assert cond not in engage_tasks, (
        "transition condition leaked into Engage.tasks")
    assert cond not in _ids(engage_entry.get("enter_conditions")), (
        "transition condition leaked into Engage.enter_conditions")
