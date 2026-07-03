"""Skill-test loop TASK-3 — /npc_logic: StateTree taxonomy guard.

The /npc_logic skill's Layer-3 advice ("conditions gate transitions; tasks own
behavior; evaluators compute shared values" — SKILL.md:90) and its discovery
step (SKILL.md:144 points agents at `statetree_list_node_types` /
`statetree_list_schemas`) presume that vocabulary is real in this engine
build. This tiny battery pins it: the three node families all exist and are
discoverable, and the AI component schema the skill's architecture targets is
exposed distinctly from the plain component schema.

Handler ground truth (read this task):
  `StateTreeTypeCache.cpp:38-48` — categories are exactly the lowercase
  strings "task" / "condition" / "evaluator" (plus optional "consideration"),
  assigned by IsChildOf against FStateTree{Task,Condition,Evaluator}Base;
  the three abstract bases themselves are skipped (:63-68).
  `StateTreeTypeCache.cpp:119-193` (ListNodeTypes) — response shape
  `{types: [{name, category, ...}], count}`; `base_class` filters by exact
  category string (:133). `StateTreeTypeCache.cpp:196-227` (ListSchemaTypes)
  — non-abstract UStateTreeSchema subclasses, `{schemas: [{class_name}],
  count}` with class_name = UClass::GetName() (no U prefix).
  Dispatch: MCPStateTreeCommands.cpp:750-767.
Engine ground truth (UE 5.7 source, read this task):
  `Engine/Plugins/Runtime/GameplayStateTree/Source/GameplayStateTreeModule/
  Public/Components/StateTreeAIComponentSchema.h:18` —
  `class UStateTreeAIComponentSchema : public UStateTreeComponentSchema`
  (the plain schema: `.../StateTreeComponentSchema.h:29`). Both concrete, so
  both list. The GameplayStateTree plugin is force-enabled by the harness
  plugin itself (`src/Plugin/UnrealMCP/UnrealMCP.uplugin` Plugins block), so
  the class is present in any editor this harness drives. Guaranteed-present
  evaluators even in a contentless fixture: the StateTree module's own
  `FStateTreeBlueprintEvaluatorWrapper` (StateTreeEvaluatorBlueprintBase.h:49)
  and `FStateTreeEvaluatorCommonBase` (StateTreeEvaluatorBase.h:54).

Pure introspection: no PIE, no assets created, nothing to clean up.
Headless-safe (no render/gui_only markers). Both tools are read-only; the
only shared-editor hazard is the transient `engine_busy` receive-timeout
(observed live while authoring this test), handled with the bounded retry
pattern from test_networking_authoring.py — safe here because both ops are
idempotent reads.
"""

import time

from harness.mcp_client import MCPToolError


def _retry_busy(fn, attempts=3, delay=2.0):
    """Bounded retry for the shared live editor's transient `engine_busy`
    (bridge receive timeout while another agent's op holds the game thread).
    Safe here: both tools under test are pure idempotent reads."""
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


# ── 1. node-type taxonomy: task / condition / evaluator all real ─────────────

def test_node_type_taxonomy_has_all_three_families(mcp):
    """`statetree_list_node_types` must return non-empty task, condition, AND
    evaluator categories — the exact vocabulary the skill's Layer-3 advice is
    written in. Also pins the envelope shape the skill's discovery step
    depends on (types[].name / types[].category, count == len(types))."""
    result = _retry_busy(lambda: mcp.expect(
        "statetree_list_node_types", {"base_class": "all"}))
    types = result.get("types")
    assert isinstance(types, list) and types, result
    assert result.get("count") == len(types), result

    by_category = {}
    for entry in types:
        assert isinstance(entry, dict) and entry.get("name"), entry
        by_category.setdefault(entry.get("category"), []).append(entry["name"])

    for family in ("task", "condition", "evaluator"):
        assert by_category.get(family), (
            f"no '{family}' node types listed — the /npc_logic Layer-3 "
            f"vocabulary is not discoverable; categories seen: "
            f"{sorted(by_category)}")


def test_base_class_filter_selects_one_family(mcp):
    """The `base_class` filter the skill's discovery step uses must actually
    narrow to the named family (exact category match —
    StateTreeTypeCache.cpp:133), independently of the 'all' call above."""
    result = _retry_busy(lambda: mcp.expect(
        "statetree_list_node_types", {"base_class": "evaluator"}))
    types = result.get("types")
    assert isinstance(types, list) and types, result
    assert all(t.get("category") == "evaluator" for t in types), types


# ── 2. schemas: AI component schema distinct from the plain one ──────────────

def test_ai_component_schema_listed_distinct_from_component_schema(mcp):
    """`statetree_list_schemas` exposes `StateTreeAIComponentSchema` (the
    schema the skill's decision layer runs under — it guarantees the
    AIController context) as a distinct entry alongside the plain
    `StateTreeComponentSchema` it subclasses. class_name is the U-prefix-less
    UClass name (StateTreeTypeCache.cpp:210); engine class confirmed at
    StateTreeAIComponentSchema.h:18."""
    result = _retry_busy(lambda: mcp.expect("statetree_list_schemas", {}))
    schemas = result.get("schemas")
    assert isinstance(schemas, list) and schemas, result
    assert result.get("count") == len(schemas), result

    names = [s.get("class_name") for s in schemas if isinstance(s, dict)]
    assert "StateTreeAIComponentSchema" in names, names
    assert "StateTreeComponentSchema" in names, names
    # Distinct entries, not one blurred listing.
    assert names.count("StateTreeAIComponentSchema") == 1, names
    assert names.count("StateTreeComponentSchema") == 1, names
