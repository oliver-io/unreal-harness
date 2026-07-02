/**
 * Domain: statetree — StateTree asset authoring (states, nodes, transitions,
 * property bindings) plus discovery/compile/verify.
 *
 * Port of the `statetree_*` canonical tools in `src/MCP/server.py`. The state/
 * node/transition/binding mutators forward to legacy wire commands (`st_*`);
 * the lifecycle/read tools forward to `statetree_*` wire commands. Wire param
 * names and omit-when-unset behavior mirror `helpers/ai_state_tree.py` exactly.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";
import { dryRun } from "./_schemas.ts";

// ── Lifecycle / discovery (statetree_* wire commands) ──────────────────────

const statetreeCreate = bridgeTool({
  name: "statetree_create",
  domain: "statetree",
  description:
    "Create a new StateTree asset. Returns asset_path, root_state_id. " +
    "schema_class defaults to StateTreeComponentSchema.",
  input: z.object({
    asset_path: z.string().min(1).describe("Content path for the new StateTree asset."),
    schema_class: z
      .string()
      .default("StateTreeComponentSchema")
      .describe("Schema class (see statetree_list_schemas)."),
  }),
  annotations: { blockedDuringPie: true },
});

const statetreeRead = bridgeTool({
  name: "statetree_read",
  domain: "statetree",
  description:
    "Read the full structure of a StateTree — states, tasks, conditions, " +
    "transitions, bindings. state_id scopes to a subtree; max_depth limits depth.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    include_node_properties: z
      .boolean()
      .default(true)
      .describe("Include each node's instance-data properties."),
    include_bindings: z
      .boolean()
      .default(true)
      .describe("Include property bindings."),
    state_id: z
      .string()
      .optional()
      .describe("Optional state name/GUID to scope the read to a subtree."),
    max_depth: z
      .number()
      .int()
      .optional()
      .describe("Optional maximum hierarchy depth."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      include_node_properties: a.include_node_properties,
      include_bindings: a.include_bindings,
    };
    if (a.state_id) p.state_id = a.state_id;
    if (a.max_depth !== undefined) p.max_depth = a.max_depth;
    return p;
  },
});

const statetreeCompile = bridgeTool({
  name: "statetree_compile",
  domain: "statetree",
  description:
    "Compile a StateTree and return structured compiler messages. " +
    "auto_save saves the asset on success.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    auto_save: z.boolean().default(false).describe("Save the asset after a successful compile."),
  }),
  annotations: { blockedDuringPie: true },
});

const statetreeSave = bridgeTool({
  name: "statetree_save",
  domain: "statetree",
  description: "Save a StateTree asset to disk.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
  }),
  annotations: { blockedDuringPie: true },
});

const statetreeVerify = bridgeTool({
  name: "statetree_verify",
  domain: "statetree",
  description:
    "Non-mutating StateTree health check. Returns status (ok / stale / not_ready / " +
    "never_compiled), hash_matches, ready_to_run, stored_hash, fresh_hash, and a hint. " +
    "Use after a C++ rebuild touching StateTree task InstanceData or schema struct layouts.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const statetreeListNodeTypes = bridgeTool({
  name: "statetree_list_node_types",
  domain: "statetree",
  description:
    "Discover registered C++ StateTree node types (tasks, conditions, evaluators, " +
    "considerations). base_class filters by family; name_pattern narrows by name.",
  input: z.object({
    base_class: z
      .string()
      .default("all")
      .describe('Node family filter, or "all".'),
    name_pattern: z
      .string()
      .default("")
      .describe("Optional substring/pattern filter on the type name."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = { base_class: a.base_class };
    if (a.name_pattern) p.name_pattern = a.name_pattern;
    return p;
  },
});

const statetreeListSchemas = bridgeTool({
  name: "statetree_list_schemas",
  domain: "statetree",
  description: "List available StateTree schema classes.",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

// ── States (st_* wire commands) ────────────────────────────────────────────

const statetreeStateAdd = bridgeTool({
  name: "statetree_state_add",
  domain: "statetree",
  command: "st_add_state",
  description:
    "Add a state to a StateTree, optionally with inline tasks and enter conditions. " +
    'tasks: [{"type":"STTask_FireAtTarget","properties":{...}}]; enter_conditions: ' +
    '[{"type":"STCond_TargetVisible","operand":"Or","indent":0,"properties":{...}}]. ' +
    'state_type: "State"|"Group"|"Subtree". selection_behavior (optional): ' +
    '"TryEnterState"|"TrySelectChildrenInOrder"|"TryFollowTransitions". Supports dry_run.',
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    name: z.string().min(1).describe("New state name."),
    parent: z.string().optional().describe("Parent state name/GUID; omit for root level."),
    state_type: z
      .string()
      .default("State")
      .describe('"State" | "Group" | "Subtree".'),
    selection_behavior: z
      .string()
      .optional()
      .describe('"TryEnterState" | "TrySelectChildrenInOrder" | "TryFollowTransitions".'),
    enabled: z.boolean().default(true).describe("Whether the state is enabled."),
    insert_index: z.number().int().optional().describe("Position among siblings."),
    tasks: z
      .array(z.record(z.string(), z.unknown()))
      .optional()
      .describe("Inline tasks: [{type, properties}]."),
    enter_conditions: z
      .array(z.record(z.string(), z.unknown()))
      .optional()
      .describe("Inline enter conditions: [{type, operand, indent, properties}]."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      name: a.name,
      type: a.state_type,
      enabled: a.enabled,
      dry_run: a.dry_run,
    };
    if (a.parent) p.parent = a.parent;
    if (a.selection_behavior) p.selection_behavior = a.selection_behavior;
    if (a.insert_index !== undefined) p.insert_index = a.insert_index;
    if (a.tasks && a.tasks.length) p.tasks = a.tasks;
    if (a.enter_conditions && a.enter_conditions.length) p.enter_conditions = a.enter_conditions;
    return p;
  },
});

const statetreeStateRemove = bridgeTool({
  name: "statetree_state_remove",
  domain: "statetree",
  command: "st_remove_state",
  description:
    "Remove a state and all its descendants (name or GUID). dry_run previews the " +
    "cascading state set plus any GotoState transitions that would be orphaned.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    state: z.string().min(1).describe("State name or GUID."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const statetreeStateRename = bridgeTool({
  name: "statetree_state_rename",
  domain: "statetree",
  command: "st_rename_state",
  description: "Rename a state (name or GUID).",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    state: z.string().min(1).describe("State name or GUID."),
    new_name: z.string().min(1).describe("New state name."),
  }),
  annotations: { blockedDuringPie: true },
});

const statetreeStateMove = bridgeTool({
  name: "statetree_state_move",
  domain: "statetree",
  command: "st_move_state",
  description: "Reparent a state within the hierarchy. Omit new_parent for root level.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    state: z.string().min(1).describe("State name or GUID."),
    new_parent: z.string().optional().describe("New parent state name/GUID; omit for root level."),
    insert_index: z.number().int().optional().describe("Position among new siblings."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = { asset_path: a.asset_path, state: a.state };
    if (a.new_parent) p.new_parent = a.new_parent;
    if (a.insert_index !== undefined) p.insert_index = a.insert_index;
    return p;
  },
});

const statetreeStateDuplicate = bridgeTool({
  name: "statetree_state_duplicate",
  domain: "statetree",
  command: "st_duplicate_state",
  description:
    "Deep-copy a state and all descendants with fresh GUIDs. Returns guid_mapping.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    state: z.string().min(1).describe("State name or GUID to duplicate."),
    new_name: z.string().optional().describe("Optional name for the copy."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = { asset_path: a.asset_path, state: a.state };
    if (a.new_name) p.new_name = a.new_name;
    return p;
  },
});

const statetreeStateSetProperties = bridgeTool({
  name: "statetree_state_set_properties",
  domain: "statetree",
  command: "st_set_state_properties",
  description:
    "Modify properties of an existing state (only the fields you pass). " +
    'state_type: "State"|"Group"|"Subtree"; selection_behavior: ' +
    '"TryEnterState"|"TrySelectChildrenInOrder"|"TryFollowTransitions". Setter is atomic.',
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    state: z.string().min(1).describe("State name or GUID."),
    name: z.string().optional().describe("New state name."),
    state_type: z.string().optional().describe('"State" | "Group" | "Subtree".'),
    selection_behavior: z
      .string()
      .optional()
      .describe('"TryEnterState" | "TrySelectChildrenInOrder" | "TryFollowTransitions".'),
    enabled: z.boolean().optional().describe("Whether the state is enabled."),
    weight: z.number().optional().describe("Utility selection weight."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = { asset_path: a.asset_path, state: a.state };
    if (a.name !== undefined) p.name = a.name;
    if (a.state_type !== undefined) p.type = a.state_type;
    if (a.selection_behavior !== undefined) p.selection_behavior = a.selection_behavior;
    if (a.enabled !== undefined) p.enabled = a.enabled;
    if (a.weight !== undefined) p.weight = a.weight;
    return p;
  },
});

const statetreeStateList = bridgeTool({
  name: "statetree_state_list",
  domain: "statetree",
  command: "st_list_states",
  description: "List all states in a StateTree with hierarchy metadata (no node details).",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

// ── Nodes (st_* wire commands) ─────────────────────────────────────────────

const statetreeNodeAdd = bridgeTool({
  name: "statetree_node_add",
  domain: "statetree",
  command: "st_add_node",
  description:
    "Add a task/condition/consideration/evaluator/global_task node. " +
    'target: {"state":"Engage","slot":"task"} or {"slot":"evaluator"}. Slots: ' +
    "task/enter_condition/consideration (require target.state), condition " +
    '(requires target.transition GUID), evaluator/global_task (global). ' +
    'operand "And"|"Or" applies to expression slots (default "And").',
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    node_type: z.string().min(1).describe('Node type, no F-prefix (e.g. "STTask_AnimalWander").'),
    target: z
      .record(z.string(), z.string())
      .describe('e.g. {"state":"Engage","slot":"task"} or {"slot":"evaluator"}.'),
    properties: z
      .record(z.string(), z.unknown())
      .optional()
      .describe("Initial property overrides for the node instance data."),
    operand: z.string().default("And").describe('"And" | "Or" (expression slots only).'),
    indent: z.number().int().default(0).describe("Expression-tree indent (condition slots)."),
    insert_index: z.number().int().optional().describe("Position within the slot."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      node_type: a.node_type,
      target: a.target,
    };
    if (a.properties) p.properties = a.properties;
    if (a.operand !== "And") p.operand = a.operand;
    if (a.indent) p.indent = a.indent;
    if (a.insert_index !== undefined) p.insert_index = a.insert_index;
    return p;
  },
});

const statetreeNodeRemove = bridgeTool({
  name: "statetree_node_remove",
  domain: "statetree",
  command: "st_remove_node",
  description: "Remove a node by GUID from anywhere in the tree.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    node_id: z.string().min(1).describe("Node GUID."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const statetreeNodeSetProperty = bridgeTool({
  name: "statetree_node_set_property",
  domain: "statetree",
  command: "st_set_node_property",
  description: "Set a property on any node's instance data via FProperty::ImportText.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    node_id: z.string().min(1).describe("Node GUID."),
    property_name: z.string().min(1).describe("Property name on the node instance data."),
    property_value: z.unknown().describe("Value to import (string/number/bool/struct)."),
  }),
  annotations: { blockedDuringPie: true },
});

const statetreeNodeGetProperties = bridgeTool({
  name: "statetree_node_get_properties",
  domain: "statetree",
  command: "st_get_node_properties",
  description: "Read all editable properties of a node's instance data.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    node_id: z.string().min(1).describe("Node GUID."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

// ── Transitions (st_* wire commands) ───────────────────────────────────────

const statetreeTransitionAdd = bridgeTool({
  name: "statetree_transition_add",
  domain: "statetree",
  command: "st_add_transition",
  description:
    "Add a transition to a state. trigger: OnStateCompleted/OnStateSucceeded/" +
    "OnStateFailed/OnTick/OnEvent. target: state name/GUID or keyword (Succeeded, " +
    "Failed, NextSelectableState, None). priority: Low/Normal/Medium/High/Critical. " +
    "event_tag (registered gameplay tag) only applies when trigger is OnEvent. Supports dry_run.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    state: z.string().min(1).describe("Owning state name or GUID."),
    trigger: z
      .string()
      .describe("OnStateCompleted | OnStateSucceeded | OnStateFailed | OnTick | OnEvent."),
    target: z.string().describe("State name/GUID or keyword (Succeeded/Failed/NextSelectableState/None)."),
    event_tag: z
      .string()
      .optional()
      .describe("Registered gameplay tag for RequiredEvent.Tag (OnEvent only)."),
    priority: z
      .string()
      .default("Normal")
      .describe("Low | Normal | Medium | High | Critical."),
    delay: z.number().default(0).describe("Transition delay seconds."),
    delay_variance: z.number().default(0).describe("Random delay variance seconds."),
    conditions: z
      .array(z.record(z.string(), z.unknown()))
      .optional()
      .describe("Inline conditions: [{type, operand, indent, properties}]."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      state: a.state,
      trigger: a.trigger,
      target: a.target,
      priority: a.priority,
      dry_run: a.dry_run,
    };
    if (a.event_tag) p.event_tag = a.event_tag;
    if (a.delay) p.delay = a.delay;
    if (a.delay_variance) p.delay_variance = a.delay_variance;
    if (a.conditions && a.conditions.length) p.conditions = a.conditions;
    return p;
  },
});

const statetreeTransitionRemove = bridgeTool({
  name: "statetree_transition_remove",
  domain: "statetree",
  command: "st_remove_transition",
  description:
    "Remove a transition by GUID. dry_run previews the removal (owning state, " +
    "trigger, resolved target, priority, delays, conditions).",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    transition_id: z.string().min(1).describe("Transition GUID."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const statetreeTransitionSetProperties = bridgeTool({
  name: "statetree_transition_set_properties",
  domain: "statetree",
  command: "st_set_transition_properties",
  description:
    "Modify a transition's properties (only the fields you pass). trigger/target/" +
    "priority validated atomically. event_tag: pass \"\" to clear the required-event tag.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    transition_id: z.string().min(1).describe("Transition GUID."),
    trigger: z
      .string()
      .optional()
      .describe("OnStateCompleted | OnStateSucceeded | OnStateFailed | OnTick | OnEvent."),
    target: z.string().optional().describe("State name/GUID or keyword target."),
    priority: z.string().optional().describe("Low | Normal | Medium | High | Critical."),
    event_tag: z
      .string()
      .optional()
      .describe('Registered gameplay tag (OnEvent); "" clears it.'),
    delay: z.number().optional().describe("Transition delay seconds."),
    delay_variance: z.number().optional().describe("Random delay variance seconds."),
    enabled: z.boolean().optional().describe("Whether the transition is enabled."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      transition_id: a.transition_id,
    };
    if (a.trigger !== undefined) p.trigger = a.trigger;
    if (a.target !== undefined) p.target = a.target;
    if (a.priority !== undefined) p.priority = a.priority;
    if (a.event_tag !== undefined) p.event_tag = a.event_tag;
    if (a.delay !== undefined) p.delay = a.delay;
    if (a.delay_variance !== undefined) p.delay_variance = a.delay_variance;
    if (a.enabled !== undefined) p.enabled = a.enabled;
    return p;
  },
});

// ── Property bindings (st_* wire commands) ─────────────────────────────────

const statetreeBindingAdd = bridgeTool({
  name: "statetree_binding_add",
  domain: "statetree",
  command: "st_add_property_binding",
  description: "Create a property binding between two nodes (data flow wiring).",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    source_node_id: z.string().min(1).describe("Source node GUID."),
    source_property: z.string().min(1).describe("Source property path."),
    target_node_id: z.string().min(1).describe("Target node GUID."),
    target_property: z.string().min(1).describe("Target property path."),
  }),
  annotations: { blockedDuringPie: true },
});

const statetreeBindingRemove = bridgeTool({
  name: "statetree_binding_remove",
  domain: "statetree",
  command: "st_remove_property_binding",
  description: "Remove a property binding by target path.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    target_node_id: z.string().min(1).describe("Target node GUID."),
    target_property: z.string().min(1).describe("Target property path."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const statetreeBindingList = bridgeTool({
  name: "statetree_binding_list",
  domain: "statetree",
  command: "st_list_property_bindings",
  description: "List all property bindings in the tree.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const statetreeBindingListBindable = bridgeTool({
  name: "statetree_binding_list_bindable",
  domain: "statetree",
  command: "st_list_bindable_properties",
  description:
    "Discover what properties are available for binding on a node, and available binding sources.",
  input: z.object({
    asset_path: z.string().min(1).describe("StateTree asset path."),
    node_id: z.string().min(1).describe("Node GUID to inspect."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

export const statetreeTools: ToolDef[] = [
  statetreeCreate,
  statetreeRead,
  statetreeCompile,
  statetreeSave,
  statetreeVerify,
  statetreeListNodeTypes,
  statetreeListSchemas,
  statetreeStateAdd,
  statetreeStateRemove,
  statetreeStateRename,
  statetreeStateMove,
  statetreeStateDuplicate,
  statetreeStateSetProperties,
  statetreeStateList,
  statetreeNodeAdd,
  statetreeNodeRemove,
  statetreeNodeSetProperty,
  statetreeNodeGetProperties,
  statetreeTransitionAdd,
  statetreeTransitionRemove,
  statetreeTransitionSetProperties,
  statetreeBindingAdd,
  statetreeBindingRemove,
  statetreeBindingList,
  statetreeBindingListBindable,
];
