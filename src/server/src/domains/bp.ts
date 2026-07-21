/**
 * Domain: bp — Blueprint asset lifecycle, graph authoring, variables, functions.
 *
 * Port of the `bp_*` tools in `src/MCP/server.py`. Many authoring tools route
 * through `src/MCP/helpers/blueprint_graph/*.py` in the Python server; the wire
 * command + exact param dict each helper sends is reproduced here.
 *
 * PIE gating: every `bp_*` mutator is in `IsBlockedDuringPie` (MCPCommonUtils.cpp);
 * read/list/inspect tools are not. `bp_add_node` is additionally in
 * `IsBlockedFromDryRun` (wire `add_blueprint_node`) and so refuses `dry_run`.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";
import { dryRun } from "./_schemas.ts";

// ── Asset lifecycle ─────────────────────────────────────────────────────────

const bpCreateBlueprint = bridgeTool({
  name: "bp_create_blueprint",
  domain: "bp",
  description:
    "Create a Blueprint / AnimBlueprint / AnimLayerInterface / BlueprintInterface asset. " +
    "Factory is chosen from the resolved parent_class. target_skeleton is required when " +
    "parent derives from UAnimInstance or is UAnimLayerInterface (ignored otherwise).",
  input: z.object({
    name: z
      .string()
      .min(1)
      .describe('Bare name ("MyBP" → /Game/Blueprints/MyBP) or full /Game/... path. Folders auto-created.'),
    parent_class: z
      .string()
      .min(1)
      .describe('Short name ("Pawn"), UE-prefixed ("APawn"), or path ("/Script/MyGame.MyAnimInstance").'),
    target_skeleton: z
      .string()
      .default("")
      .describe("USkeleton /Game/... path — required for AnimInstance/AnimLayerInterface parents."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = { name: a.name, parent_class: a.parent_class };
    if (a.target_skeleton) p.target_skeleton = a.target_skeleton;
    return p;
  },
});

const bpAddComponent = bridgeTool({
  name: "bp_add_component",
  domain: "bp",
  description:
    "Add a component to a Blueprint. component_type is a component class name; component_name is the " +
    "new SCS variable name. location/rotation/scale are optional [x,y,z]/[pitch,yaw,roll] arrays. " +
    "component_properties is a map of property overrides applied to the new component template — " +
    "keys may be dotted paths (e.g. {\"BodyInstance.bNotifyRigidBodyCollision\": true, \"BoxExtent\": {\"X\":50,\"Y\":50,\"Z\":50}}). " +
    "parent_component nests the new component under an existing one instead of the root. " +
    "Re-calling with an EXISTING component_name of the SAME class UPDATES that component in place " +
    "(applies component_properties / transform / parent_component) instead of duplicating — the response " +
    "carries updated:true. A same name with a DIFFERENT class is refused (name_collision); remove it first. " +
    "For a SkeletalMeshComponent, set its mesh via the dedicated skeletal_mesh param (NOT component_properties): " +
    "the canonical SkeletalMeshAsset property is Transient and only the engine Setter wires it into the render " +
    "path, so a raw property write registers a null mesh (renders nothing if the mesh is first assigned at " +
    "runtime — GAP-053); skeletal_mesh routes through SetSkeletalMeshAsset so the template registers WITH a mesh. " +
    "Returns the ACTUAL component_name plus any property_errors.",
  input: z.object({
    blueprint_name: z.string().min(1),
    component_type: z.string().min(1).describe("Component class (e.g. StaticMeshComponent)."),
    component_name: z.string().min(1).describe("New SCS variable name."),
    location: z.array(z.number()).default([]).describe("[x,y,z] relative location."),
    rotation: z.array(z.number()).default([]).describe("[pitch,yaw,roll] relative rotation."),
    scale: z.array(z.number()).default([]).describe("[x,y,z] relative scale."),
    component_properties: z
      .record(z.unknown())
      .default({})
      .describe("Optional map of property overrides for the component template; keys may be dotted paths into structs/sub-objects."),
    skeletal_mesh: z
      .string()
      .optional()
      .describe(
        "Optional /Game/... path to a USkeletalMesh, set on a SkeletalMeshComponent template via the engine Setter " +
        "(SetSkeletalMeshAsset). Use this instead of component_properties for skeletal meshes so the component " +
        "registers with a non-null mesh and renders at frame 0 (GAP-053).",
      ),
    parent_component: z
      .string()
      .optional()
      .describe("Optional existing component to nest the new one under (defaults to the root)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      component_type: a.component_type,
      component_name: a.component_name,
      location: a.location,
      rotation: a.rotation,
      scale: a.scale,
      component_properties: a.component_properties,
    };
    if (a.skeletal_mesh) p.skeletal_mesh = a.skeletal_mesh;
    if (a.parent_component) p.parent_component = a.parent_component;
    return p;
  },
});

const bpCompile = bridgeTool({
  name: "bp_compile",
  domain: "bp",
  description:
    "Compile a Blueprint. blueprint_name = asset name or path. " +
    "Set save:true to also persist the asset to disk after a successful compile " +
    "(saves a round-trip vs a follow-up asset_save); a failed compile is never saved.",
  input: z.object({
    blueprint_name: z.string().min(1),
    save: z
      .boolean()
      .optional()
      .describe(
        "If true, save the Blueprint package to disk after a successful compile. Default false (compile only).",
      ),
  }),
  annotations: { blockedDuringPie: true },
});

const bpReparent = bridgeTool({
  name: "bp_reparent",
  domain: "bp",
  description:
    "Change a Blueprint's parent class (regular / Anim / Widget BPs). Recompiles after reparenting. " +
    "Returns old_parent_class + new_parent_class.",
  input: z.object({
    blueprint_path: z.string().min(1).describe("Full path to the Blueprint asset."),
    new_parent_class: z.string().min(1).describe('Name or path of the new parent ("AnimInstance", "/Script/Engine.AnimInstance").'),
  }),
  annotations: { blockedDuringPie: true },
});

const bpSetDefaultValue = bridgeTool({
  name: "bp_set_default_value",
  domain: "bp",
  description:
    "Write a default-value override on a Blueprint class CDO for any edit-exposed UPROPERTY, " +
    "including inherited C++ properties (unlike bp_set_variable_properties). Auto-recompiles + saves. " +
    "For TSoftObjectPtr<X> pass the asset path string.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Full /Game/... path or short name of the Blueprint."),
    property: z.string().min(1).describe("UPROPERTY name (case-sensitive)."),
    value: z.unknown().describe("JSON-compatible value matching the property type."),
  }),
  annotations: { blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpGetParentClass = bridgeTool({
  name: "bp_get_parent_class",
  domain: "bp",
  description:
    "Lookup a Blueprint's parent UClass. Cheaper than bp_read. Returns parent_class (path), " +
    "parent_class_name, generated_class.",
  input: z.object({ bp_path: z.string().min(1).describe("Blueprint asset path or short name.") }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

// ── Reads / inspection ──────────────────────────────────────────────────────

const bpBrief = bridgeTool({
  name: "bp_brief",
  domain: "bp",
  description:
    "Compact summary of a Blueprint — counts of variables/functions/components, parent class, " +
    "blueprint_type, has_scs, and the graph list (name, schema, node_count).",
  input: z.object({ bp_path: z.string().min(1).describe("Blueprint asset path or short name.") }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const bpRead = bridgeTool({
  name: "bp_read",
  domain: "bp",
  description:
    "Read full Blueprint content — event graph, functions, variables, components, interfaces. " +
    "include_* flags toggle sections; include_variable_defaults resolves CDO values, " +
    "include_component_properties emits archetype-diff overrides, include_inherited_components walks the parent chain.",
  input: z.object({
    blueprint_path: z.string().min(1),
    include_event_graph: z.boolean().default(true),
    include_functions: z.boolean().default(true),
    include_variables: z.boolean().default(true),
    include_components: z.boolean().default(true),
    include_interfaces: z.boolean().default(true),
    include_variable_defaults: z
      .boolean()
      .default(false)
      .describe("Resolve each variable's CDO value (adds cdo_value, cpp_type)."),
    include_component_properties: z
      .boolean()
      .default(false)
      .describe("Add property_overrides (values differing from the component archetype)."),
    include_inherited_components: z
      .boolean()
      .default(false)
      .describe("Also emit parent-BP and native parent-class components (inherited: true)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const bpInspect = bridgeTool({
  name: "bp_inspect",
  domain: "bp",
  description:
    "Analyze a specific graph within a Blueprint (EventGraph, a function, etc.) — nodes, connections, " +
    "execution flow. from_state/to_state target a specific transition rule graph. " +
    "Dense graphs can be large: use detail:\"summary\" (names/classes/titles only) or max_nodes+offset " +
    "to page (the result carries total_nodes / returned_nodes / next_offset).",
  input: z.object({
    blueprint_path: z.string().min(1),
    graph_name: z.string().default("EventGraph").describe('Graph to analyze ("EventGraph", function name, etc.).'),
    detail: z.enum(["full", "summary"]).default("full")
      .describe('"summary" returns only name/class/title per node (no pins/details) — use for dense graphs that overflow the token cap.'),
    max_nodes: z.number().int().positive().optional()
      .describe("Cap the number of nodes returned this call (pagination); pair with offset."),
    offset: z.number().int().nonnegative().default(0)
      .describe("Skip the first N nodes (pagination cursor — use next_offset from a prior call)."),
    include_node_details: z.boolean().default(true),
    include_pin_connections: z.boolean().default(true),
    trace_execution_flow: z.boolean().default(true),
    from_state: z.string().optional().describe("Source state name for a transition rule graph."),
    to_state: z.string().optional().describe("Target state name for a transition rule graph."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_path: a.blueprint_path,
      graph_name: a.graph_name,
      detail: a.detail,
      offset: a.offset,
      include_node_details: a.include_node_details,
      include_pin_connections: a.include_pin_connections,
      trace_execution_flow: a.trace_execution_flow,
    };
    if (a.max_nodes !== undefined) p.max_nodes = a.max_nodes;
    if (a.from_state) p.from_state = a.from_state;
    if (a.to_state) p.to_state = a.to_state;
    return p;
  },
});

const bpGetVariableDetails = bridgeTool({
  name: "bp_get_variable_details",
  domain: "bp",
  description:
    "Get Blueprint variable details (type, defaults, metadata) — round-trip parity with " +
    "bp_set_variable_properties. Omit variable_name to return all variables.",
  input: z.object({
    blueprint_path: z.string().min(1),
    variable_name: z.string().optional().describe("Specific variable; omit for all."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // Python always sends variable_name (null when unset).
  params: (a) => ({ blueprint_path: a.blueprint_path, variable_name: a.variable_name ?? null }),
});

const bpGetFunctionDetails = bridgeTool({
  name: "bp_get_function_details",
  domain: "bp",
  description:
    "Get Blueprint function details — parameters, return values, local variables, and (when " +
    "include_graph) the function graph. Omit function_name to return all functions.",
  input: z.object({
    blueprint_path: z.string().min(1),
    function_name: z.string().optional().describe("Specific function; omit for all."),
    include_graph: z.boolean().default(true).describe("Include the function's graph nodes and connections."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // Python always sends function_name (null when unset).
  params: (a) => ({
    blueprint_path: a.blueprint_path,
    function_name: a.function_name ?? null,
    include_graph: a.include_graph,
  }),
});

const bpFunctionReferences = bridgeTool({
  name: "bp_function_references",
  domain: "bp",
  description:
    "Direction-aware lookup of Blueprint function call references. direction: 'callees'/'outbound' " +
    "(what this function calls) or 'callers'/'inbound' (who calls it — loaded BPs only).",
  input: z.object({
    bp_path: z.string().min(1).describe("Asset path of the Blueprint owning the function."),
    function_name: z.string().min(1).describe("Function short name (FName), case-sensitive."),
    direction: z.string().min(1).describe("'callees'/'outbound' or 'callers'/'inbound'."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const bpListComponents = bridgeTool({
  name: "bp_list_components",
  domain: "bp",
  description:
    "List a Blueprint's SimpleConstructionScript components (name, class, parent_component, child_count). " +
    "has_scs is false for BP kinds without an SCS.",
  input: z.object({ bp_path: z.string().min(1).describe("Blueprint asset path or short name.") }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const bpListGraphs = bridgeTool({
  name: "bp_list_graphs",
  domain: "bp",
  description:
    "List all graphs in a Blueprint — EventGraph, function graphs, AnimGraph, state machine sub-graphs, " +
    "per-state inner graphs, transition rule graphs. Essential for AnimBPs where graph names aren't predictable.",
  input: z.object({ blueprint_path: z.string().min(1).describe("Full path to the Blueprint asset.") }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const bpListNodePins = bridgeTool({
  name: "bp_list_node_pins",
  domain: "bp",
  description:
    "List all pins on a node (name, direction, type, connections). Use after bp_add_node to discover " +
    "pin names before bp_connect_pins. graph_name (alias: function_name) narrows the search; " +
    "from_state/to_state target a transition graph. Scope it whenever the node lives in a function graph — " +
    "node ids are per-graph and reused across graphs, so an unscoped search can match a same-id node elsewhere.",
  input: z.object({
    blueprint_name: z.string().min(1),
    node_id: z.string().min(1).describe("The node's `node_id` from a read tool (its GetName() id) — or its `node_guid`, or a unique display `title`."),
    graph_name: z.string().optional().describe("Optional graph name; searches all if omitted."),
    function_name: z.string().optional().describe("Alias for graph_name (function graph to scope to)."),
    from_state: z.string().optional(),
    to_state: z.string().optional(),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = { blueprint_name: a.blueprint_name, node_id: a.node_id };
    if (a.graph_name) p.graph_name = a.graph_name;
    if (a.function_name) p.function_name = a.function_name;
    if (a.from_state) p.from_state = a.from_state;
    if (a.to_state) p.to_state = a.to_state;
    return p;
  },
});

// ── Component / pin mutators ────────────────────────────────────────────────

const bpSetComponentProperty = bridgeTool({
  name: "bp_set_component_property",
  domain: "bp",
  description:
    "Set any edit-exposed UPROPERTY on a Blueprint component template by reflection — the design-time " +
    "equivalent of actor_set_property for a BP's SCS component (e.g. FloatingPawnMovement MaxSpeed, " +
    "SpringArm TargetArmLength/SocketOffset, BoxComponent BoxExtent). `property` may be a dotted path " +
    "into structs/sub-objects (e.g. \"BodyInstance.bNotifyRigidBodyCollision\"). Unlike bp_set_default_value " +
    "(top-level CDO props only), this reaches component-template sub-objects. Object/class references " +
    "(StaticMesh, material, mesh, UClass) are settable by asset/class path string, and cleared with null " +
    "or \"None\". Recompiles on next save.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Short name or full asset path of the Blueprint."),
    component_name: z.string().min(1).describe("SCS variable name of the component (e.g. SpringArm)."),
    property: z.string().min(1).describe("UPROPERTY name, or a dotted path into a struct/sub-object."),
    value: z
      .unknown()
      .describe(
        "JSON value matching the property type (number/bool/string/array/object). For an object/class " +
          "reference pass the asset/class path string (e.g. \"/Game/Meshes/SM_Cube.SM_Cube\"); pass null or " +
          "\"None\" to clear it.",
      ),
  }),
  annotations: { blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpSetComponentTransform = bridgeTool({
  name: "bp_set_component_transform",
  domain: "bp",
  description:
    "Set the relative transform of an existing Blueprint component template (location/rotation/scale). " +
    "Provide at least one of location [x,y,z], rotation [pitch,yaw,roll], scale [x,y,z]. Recompiles on next save.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Short name or full asset path of the Blueprint."),
    component_name: z.string().min(1).describe("SCS variable name of the component."),
    location: z.array(z.number()).optional().describe("[x,y,z] relative location."),
    rotation: z.array(z.number()).optional().describe("[pitch,yaw,roll] relative rotation."),
    scale: z.array(z.number()).optional().describe("[x,y,z] relative scale."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      component_name: a.component_name,
    };
    if (a.location) p.location = a.location;
    if (a.rotation) p.rotation = a.rotation;
    if (a.scale) p.scale = a.scale;
    return p;
  },
});

const bpSetClassReplication = bridgeTool({
  name: "bp_set_class_replication",
  domain: "bp",
  description:
    "Set actor-class networking on an Actor Blueprint's CDO: replicates (bReplicates), " +
    "replicate_movement, always_relevant, net_cull_distance_squared. These are inherited native AActor " +
    "flags that bp_set_default_value can't reach. Pass at least one. Recompiles + saves. Actor BPs only.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Short name or full asset path of an Actor Blueprint."),
    replicates: z.boolean().optional().describe("AActor bReplicates — make the actor replicate."),
    replicate_movement: z.boolean().optional().describe("AActor bReplicateMovement."),
    always_relevant: z.boolean().optional().describe("AActor bAlwaysRelevant."),
    net_cull_distance_squared: z
      .number()
      .optional()
      .describe("AActor NetCullDistanceSquared (relevancy cull distance, squared)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = { blueprint_name: a.blueprint_name };
    if (a.replicates !== undefined) p.replicates = a.replicates;
    if (a.replicate_movement !== undefined) p.replicate_movement = a.replicate_movement;
    if (a.always_relevant !== undefined) p.always_relevant = a.always_relevant;
    if (a.net_cull_distance_squared !== undefined)
      p.net_cull_distance_squared = a.net_cull_distance_squared;
    return p;
  },
});

const bpSetEventReplication = bridgeTool({
  name: "bp_set_event_replication",
  domain: "bp",
  description:
    "Set a Blueprint CUSTOM EVENT's net specifiers — the editor Details-panel 'Replicates' dropdown + " +
    "'Reliable' checkbox. replication: none (clear net flags) | multicast (Server→all clients) | " +
    "server (client→server RPC) | client (server→owning client). reliable ORs FUNC_NetReliable (ignored when " +
    "replication=none; unset = unreliable). Maps to the event's FunctionFlags (FUNC_Net + one of " +
    "FUNC_NetMulticast/FUNC_NetServer/FUNC_NetClient). Only works on a custom event (UK2Node_CustomEvent); " +
    "engine override events (BeginPlay/Tick) inherit flags from C++ and are rejected. Marks the Blueprint " +
    "structurally modified + saves — run bp_compile before use. Blocked during PIE.",
  input: z.object({
    blueprint_name: z
      .string()
      .min(1)
      .describe("Short name or full asset path of the Blueprint that owns the custom event."),
    event_name: z
      .string()
      .min(1)
      .describe("The custom event's name (UK2Node_CustomEvent CustomFunctionName), case-insensitive."),
    replication: z
      .enum(["none", "multicast", "server", "client"])
      .describe(
        "none = Not Replicated; multicast = Multicast; server = Run on Server; client = Run on owning Client.",
      ),
    reliable: z
      .boolean()
      .optional()
      .describe("Reliable delivery (FUNC_NetReliable). Ignored when replication=none. Default false (unreliable)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      event_name: a.event_name,
      replication: a.replication,
    };
    if (a.reliable !== undefined) p.reliable = a.reliable;
    return p;
  },
});

const bpRemoveComponent = bridgeTool({
  name: "bp_remove_component",
  domain: "bp",
  description:
    "Remove a component from a Blueprint's SCS (inverse of bp_add_component). Recompiles + saves. " +
    "Refuses (would_break_references) when the component has children and reparent_children=false. dry_run supported.",
  input: z.object({
    bp_path: z.string().min(1).describe("Blueprint asset path or short name."),
    component_name: z.string().min(1).describe("SCS variable name of the component to remove."),
    reparent_children: z
      .boolean()
      .default(true)
      .describe("Promote children to the removed node's parent; refuse if false and children exist."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  // 1:1 identical key names — all always sent.
});

const bpDisconnectPin = bridgeTool({
  name: "bp_disconnect_pin",
  domain: "bp",
  description:
    "Sever a pin's wire(s) (counterpart to bp_connect_pins). With no target_*, breaks every link on the " +
    "pin; with both target_node_id + target_pin_name, severs only that one peer link. Recompiles + saves. dry_run supported. " +
    "Pass function_name (or from_state/to_state) to scope the node lookup to one graph — node ids are per-graph and " +
    "reused across graphs, so an unscoped search can match a same-id node in the wrong graph.",
  input: z.object({
    bp_path: z.string().min(1),
    node_id: z.string().min(1).describe("Source node's `node_id` from a read tool (its GetName() id) — or its `node_guid`, or a unique display `title`."),
    pin_name: z.string().min(1).describe("Pin to disconnect."),
    target_node_id: z.string().default("").describe("Optional peer node to sever just that one link."),
    target_pin_name: z.string().default("").describe("Optional peer pin to sever just that one link."),
    function_name: z.string().optional().describe("Optional function/graph to scope the node lookup to."),
    from_state: z.string().optional().describe("Optional transition source state (with to_state) to scope to a transition graph."),
    to_state: z.string().optional().describe("Optional transition target state (with from_state)."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      bp_path: a.bp_path,
      node_id: a.node_id,
      pin_name: a.pin_name,
      dry_run: a.dry_run,
    };
    if (a.target_node_id) p.target_node_id = a.target_node_id;
    if (a.target_pin_name) p.target_pin_name = a.target_pin_name;
    if (a.function_name) p.function_name = a.function_name;
    if (a.from_state) p.from_state = a.from_state;
    if (a.to_state) p.to_state = a.to_state;
    return p;
  },
});

// ── Graph node authoring ────────────────────────────────────────────────────

const bpAddNode = bridgeTool({
  name: "bp_add_node",
  domain: "bp",
  // Routes through node_manager.add_node → wire command "add_blueprint_node",
  // packing the per-node options under a nested node_params object.
  command: "add_blueprint_node",
  description:
    "Add a node to a Blueprint graph. Creates K2Nodes (logic) or AnimGraph nodes (target via " +
    "function_name='AnimGraph'). node_type selects the node (Branch, CallFunction, VariableGet, Print, " +
    "Event, SequencePlayer, TwoBoneIK, …). Use bp_list_node_pins afterward to discover pins. dry_run unsupported.",
  input: z.object({
    blueprint_name: z.string().min(1),
    node_type: z.string().min(1).describe("Node type to create (see tool docs for the full set)."),
    pos_x: z.number().default(0).describe("X position in graph."),
    pos_y: z.number().default(0).describe("Y position in graph."),
    message: z.string().default("").describe("For Print nodes: text to print."),
    event_type: z.string().default("BeginPlay").describe("For Event nodes: event name (BeginPlay, Tick, …)."),
    variable_name: z.string().default("").describe("For Variable nodes: the variable name."),
    target_function: z.string().default("").describe("For CallFunction nodes: function to call."),
    target_class: z.string().optional().describe(
      "For CallFunction: class owning the function. Resolves WITHOUT this for the BP's own/inherited " +
      "functions, common engine classes (Actor/Pawn/Character/Controller/PlayerController/AnimInstance/" +
      "CharacterMovementComponent), and UKismetSystem/Math/GameplayStatics. REQUIRED for any other library " +
      "(e.g. a plugin's U…FunctionLibrary, UKismetArrayLibrary) — pass the bare name, UE-prefixed name, or " +
      "/Script/... path. No global name search; omitting it for an arbitrary library fails node creation.",
    ),
    target_blueprint: z.string().optional().describe(
      "For CallFunction: target Blueprint path/name for a cross-BP call (resolves its generated class).",
    ),
    function_name: z.string().optional().describe("Target graph name; 'AnimGraph' for animation nodes."),
    anim_asset: z.string().optional().describe("AnimSequence/BlendSpace/AimOffset path for player nodes."),
    slot_name: z.string().optional().describe("Slot name for AnimSlot/Slot nodes."),
    cache_name: z.string().optional().describe("Cache id for SaveCachedPose/UseCachedPose nodes."),
    target_bone: z.string().optional().describe("Bone name for skeletal control / IK nodes."),
    linked_class: z.string().optional().describe("AnimInstance class path for LinkedAnimGraph nodes."),
    layer_interface: z.string().optional().describe("AnimLayerInterface class path for LinkedAnimLayer nodes."),
    from_state: z.string().optional().describe("Source state for a transition rule graph."),
    to_state: z.string().optional().describe("Target state for a transition rule graph."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const np: Record<string, unknown> = { pos_x: a.pos_x, pos_y: a.pos_y };
    if (a.message) np.message = a.message;
    if (a.event_type) np.event_type = a.event_type;
    if (a.variable_name) np.variable_name = a.variable_name;
    if (a.target_function) np.target_function = a.target_function;
    if (a.target_class) np.target_class = a.target_class;
    if (a.target_blueprint) np.target_blueprint = a.target_blueprint;
    if (a.function_name) np.function_name = a.function_name;
    if (a.anim_asset) np.anim_asset = a.anim_asset;
    if (a.slot_name) np.slot_name = a.slot_name;
    if (a.cache_name) np.cache_name = a.cache_name;
    if (a.target_bone) np.target_bone = a.target_bone;
    if (a.linked_class) np.linked_class = a.linked_class;
    if (a.layer_interface) np.layer_interface = a.layer_interface;
    if (a.from_state) np.from_state = a.from_state;
    if (a.to_state) np.to_state = a.to_state;
    return { blueprint_name: a.blueprint_name, node_type: a.node_type, node_params: np };
  },
});

const bpAddEventNode = bridgeTool({
  name: "bp_add_event_node",
  domain: "bp",
  description:
    "Add a dedicated event node (ReceiveBeginPlay, ReceiveTick, ReceiveDestroyed, …) to a Blueprint's " +
    "event graph at a position.",
  input: z.object({
    blueprint_name: z.string().min(1),
    event_name: z.string().min(1).describe('Event name (e.g. "ReceiveBeginPlay", "ReceiveTick").'),
    pos_x: z.number().default(0),
    pos_y: z.number().default(0),
  }),
  annotations: { blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpAddCustomEvent = bridgeTool({
  name: "bp_add_custom_event",
  domain: "bp",
  command: "bp_add_custom_event",
  description:
    "Author a fresh CUSTOM EVENT (UK2Node_CustomEvent) in a Blueprint's event graph — the editor's " +
    "'Add Custom Event…' action. Distinct from bp_add_event_node, which only binds an existing engine " +
    "override (ReceiveBeginPlay/Tick/…); this creates a brand-new named event you can call, bind, or " +
    "turn into a client→server / server→client RPC via bp_set_event_replication. Optional params[] become " +
    "the event's typed INPUT signature (output pins on the node). Rejects (name_collision) when an event of " +
    "that name already exists. Marks the Blueprint structurally modified + auto-saves — run bp_compile " +
    "before use. Blocked during PIE.",
  input: z.object({
    blueprint_name: z
      .string()
      .min(1)
      .describe("Short name or full asset path of the Blueprint to add the custom event to."),
    event_name: z
      .string()
      .min(1)
      .describe("The new custom event's name (becomes UK2Node_CustomEvent CustomFunctionName)."),
    params: z
      .array(
        z.object({
          name: z.string().min(1).describe("Parameter (pin) name — C++ identifier style."),
          type: z
            .string()
            .min(1)
            .describe(
              'Type string — same vocabulary as bp_create_variable: "bool", "int", "float", "string", ' +
                '"vector", "rotator", "transform", "byte", "int64", "name", "text", or "class:/struct:/enum:Name".',
            ),
        }),
      )
      .optional()
      .describe("Optional typed input parameters → the event's signature (each an output pin on the node)."),
    pos_x: z.number().default(0),
    pos_y: z.number().default(0),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      event_name: a.event_name,
      pos_x: a.pos_x,
      pos_y: a.pos_y,
    };
    if (a.params !== undefined) p.params = a.params;
    return p;
  },
});

const bpDeleteNode = bridgeTool({
  name: "bp_delete_node",
  domain: "bp",
  description:
    "Delete a node and all its connections from a Blueprint graph. function_name targets a function " +
    "graph (defaults to EventGraph); from_state/to_state target a transition rule graph. dry_run supported.",
  input: z.object({
    blueprint_name: z.string().min(1),
    node_id: z.string().min(1).describe("The node's `node_id` from a read tool (its GetName() id) — or its `node_guid`, or a unique display `title`."),
    function_name: z.string().optional().describe("Function graph name; defaults to EventGraph."),
    from_state: z.string().optional(),
    to_state: z.string().optional(),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      node_id: a.node_id,
      dry_run: a.dry_run,
    };
    if (a.function_name !== undefined) p.function_name = a.function_name;
    if (a.from_state) p.from_state = a.from_state;
    if (a.to_state) p.to_state = a.to_state;
    return p;
  },
});

const bpConnectPins = bridgeTool({
  name: "bp_connect_pins",
  domain: "bp",
  description:
    "Connect a source output pin to a target input pin between two existing nodes. function_name selects " +
    "a function graph (else EventGraph); from_state/to_state target a transition rule graph. dry_run supported. " +
    "source_node_id/target_node_id: pass the `node_id` a read tool (bp_inspect/bp_read/bp_list_node_pins) reports for the node — it round-trips verbatim (the `node_guid` or a unique `title` are also accepted).",
  input: z.object({
    blueprint_name: z.string().min(1),
    source_node_id: z.string().min(1).describe("Source node id — the `node_id` field from a read tool (or its `node_guid`, or a unique `title`)."),
    source_pin_name: z.string().min(1).describe("Output pin on the source node."),
    target_node_id: z.string().min(1).describe("Target node id — the `node_id` field from a read tool (or its `node_guid`, or a unique `title`)."),
    target_pin_name: z.string().min(1).describe("Input pin on the target node."),
    function_name: z.string().optional().describe("Function graph name; defaults to EventGraph."),
    from_state: z.string().optional(),
    to_state: z.string().optional(),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      source_node_id: a.source_node_id,
      source_pin_name: a.source_pin_name,
      target_node_id: a.target_node_id,
      target_pin_name: a.target_pin_name,
      dry_run: a.dry_run,
    };
    if (a.function_name) p.function_name = a.function_name;
    if (a.from_state) p.from_state = a.from_state;
    if (a.to_state) p.to_state = a.to_state;
    return p;
  },
});

const bpSetNodeProperty = bridgeTool({
  name: "bp_set_node_property",
  domain: "bp",
  description:
    "Set a property on a Blueprint node (legacy mode: property_name + property_value) OR perform a " +
    "semantic action (add_pin, remove_pin, set_enum_type, set_pin_type, set_value_type, set_cast_target, " +
    "set_function_call, set_event_type — supply `action` plus its parameters). Phase-3 actions are destructive. dry_run supported.",
  input: z.object({
    blueprint_name: z.string().min(1),
    node_id: z.string().min(1).describe("The `node_id` a read tool (bp_inspect/bp_read) reports for the node — round-trips verbatim (its `node_guid` or a unique `title` also work)."),
    property_name: z.string().default("").describe("Legacy mode: property to set (used when action is unset)."),
    property_value: z.unknown().optional().describe("Legacy mode: value to set."),
    function_name: z.string().optional().describe("Function graph name; defaults to EventGraph."),
    from_state: z.string().optional(),
    to_state: z.string().optional(),
    action: z
      .string()
      .optional()
      .describe("Semantic action: add_pin | remove_pin | set_enum_type | set_pin_type | set_value_type | set_cast_target | set_function_call | set_event_type."),
    pin_type: z.string().optional().describe("add_pin: SwitchCase | ExecutionOutput | ArrayElement | EnumValue."),
    pin_name: z.string().optional().describe("remove_pin / set_pin_type: target pin name."),
    enum_type: z.string().optional().describe("set_enum_type: full enum path."),
    new_type: z.string().optional().describe("set_pin_type / set_value_type: new type."),
    target_type: z.string().optional().describe("set_cast_target: target class path."),
    target_function: z.string().optional().describe("set_function_call: function to call."),
    target_class: z.string().optional().describe("set_function_call: optional class containing the function."),
    event_type: z.string().optional().describe("set_event_type: event name."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      node_id: a.node_id,
      dry_run: a.dry_run,
    };
    if (a.action !== undefined) {
      p.action = a.action;
      if (a.pin_type !== undefined) p.pin_type = a.pin_type;
      if (a.pin_name !== undefined) p.pin_name = a.pin_name;
      if (a.enum_type !== undefined) p.enum_type = a.enum_type;
      if (a.new_type !== undefined) p.new_type = a.new_type;
      if (a.target_type !== undefined) p.target_type = a.target_type;
      if (a.target_function !== undefined) p.target_function = a.target_function;
      if (a.target_class !== undefined) p.target_class = a.target_class;
      if (a.event_type !== undefined) p.event_type = a.event_type;
    } else {
      p.property_name = a.property_name;
      p.property_value = a.property_value ?? null;
    }
    if (a.function_name !== undefined) p.function_name = a.function_name;
    if (a.from_state !== undefined) p.from_state = a.from_state;
    if (a.to_state !== undefined) p.to_state = a.to_state;
    return p;
  },
});

const bpSetInnerNodeProperty = bridgeTool({
  name: "bp_set_inner_node_property",
  domain: "bp",
  description:
    "Set a property on the inner FAnimNode struct of an AnimGraph node (e.g. FAnimNode_ModifyBone, " +
    "FAnimNode_TwoBoneIK) via reflection — bone references, spaces, modes, alpha. property_value is sent " +
    "as a string (FProperty::ImportText). graph_name searches all graphs if omitted.",
  input: z.object({
    blueprint_name: z.string().min(1),
    node_id: z.string().min(1),
    property_name: z.string().min(1).describe("Property on the inner FAnimNode struct."),
    property_value: z.unknown().describe("Value (sent as a string; e.g. bone name or enum value name)."),
    graph_name: z.string().optional().describe("Optional graph name; searches all if omitted."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      node_id: a.node_id,
      property_name: a.property_name,
      property_value: String(a.property_value),
    };
    if (a.graph_name) p.graph_name = a.graph_name;
    return p;
  },
});

// ── Variables ───────────────────────────────────────────────────────────────

const bpCreateVariable = bridgeTool({
  name: "bp_create_variable",
  domain: "bp",
  description:
    "Create a Blueprint variable with a type and optional default/visibility/category. " +
    "variable_type: bool, int, float, string, vector, rotator, etc. dry_run previews without creating.",
  input: z.object({
    blueprint_name: z.string().min(1),
    variable_name: z.string().min(1),
    variable_type: z.string().min(1).describe('Type ("bool", "int", "float", "string", "vector", "rotator", …).'),
    default_value: z.unknown().optional().describe("Optional default value."),
    is_public: z.boolean().default(false).describe("Public/editable."),
    tooltip: z.string().default("").describe("Optional tooltip."),
    category: z.string().default("Default").describe("Category for organizing variables."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      variable_name: a.variable_name,
      variable_type: a.variable_type,
      dry_run: a.dry_run,
    };
    if (a.default_value !== undefined && a.default_value !== null) p.default_value = a.default_value;
    if (a.is_public) p.is_public = a.is_public;
    if (a.tooltip) p.tooltip = a.tooltip;
    if (a.category !== "Default") p.category = a.category;
    return p;
  },
});

const bpDeleteVariable = bridgeTool({
  name: "bp_delete_variable",
  domain: "bp",
  description:
    "Delete a Blueprint-authored variable (NewVariables only, not inherited C++ members). Cleans up " +
    "referencing nodes and recompiles.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Name or path of the Blueprint."),
    variable_name: z.string().min(1),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpSetVariableProperties = bridgeTool({
  name: "bp_set_variable_properties",
  domain: "bp",
  description:
    "Modify an existing Blueprint variable's properties without deleting it (preserves connected Get/Set " +
    "nodes). Any provided field is applied: rename (var_name), retype (var_type), visibility, replication, " +
    "metadata (tooltip, category, ranges, units, bitmask, expose flags). dry_run supported.",
  input: z.object({
    blueprint_name: z.string().min(1),
    variable_name: z.string().min(1),
    var_name: z.string().optional().describe("Rename the variable."),
    var_type: z.string().optional().describe("Change variable type."),
    is_blueprint_readable: z.boolean().optional(),
    is_blueprint_writable: z.boolean().optional(),
    is_public: z.boolean().optional(),
    is_editable_in_instance: z.boolean().optional(),
    tooltip: z.string().optional(),
    category: z.string().optional(),
    default_value: z.unknown().optional(),
    expose_on_spawn: z.boolean().optional(),
    expose_to_cinematics: z.boolean().optional(),
    slider_range_min: z.string().optional(),
    slider_range_max: z.string().optional(),
    value_range_min: z.string().optional(),
    value_range_max: z.string().optional(),
    units: z.string().optional().describe('Display units; use long form ("Centimeters", "Meters").'),
    bitmask: z.boolean().optional(),
    bitmask_enum: z.string().optional().describe('Bitmask enum — REQUIRES full path "/Script/Module.EnumName".'),
    replication_enabled: z.boolean().optional(),
    replication_condition: z
      .number()
      .optional()
      .describe("ELifetimeCondition 0-7 (0=None, 1=InitialOnly, 2=OwnerOnly, …)."),
    is_private: z.boolean().optional(),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      variable_name: a.variable_name,
      dry_run: a.dry_run,
    };
    if (a.var_name !== undefined) p.var_name = a.var_name;
    if (a.var_type !== undefined) p.var_type = a.var_type;
    if (a.is_blueprint_readable !== undefined) p.is_blueprint_readable = a.is_blueprint_readable;
    if (a.is_blueprint_writable !== undefined) p.is_blueprint_writable = a.is_blueprint_writable;
    if (a.is_public !== undefined) p.is_public = a.is_public;
    if (a.is_editable_in_instance !== undefined) p.is_editable_in_instance = a.is_editable_in_instance;
    if (a.tooltip !== undefined) p.tooltip = a.tooltip;
    if (a.category !== undefined) p.category = a.category;
    if (a.default_value !== undefined && a.default_value !== null) p.default_value = a.default_value;
    if (a.expose_on_spawn !== undefined) p.expose_on_spawn = a.expose_on_spawn;
    if (a.expose_to_cinematics !== undefined) p.expose_to_cinematics = a.expose_to_cinematics;
    if (a.slider_range_min !== undefined) p.slider_range_min = a.slider_range_min;
    if (a.slider_range_max !== undefined) p.slider_range_max = a.slider_range_max;
    if (a.value_range_min !== undefined) p.value_range_min = a.value_range_min;
    if (a.value_range_max !== undefined) p.value_range_max = a.value_range_max;
    if (a.units !== undefined) p.units = a.units;
    if (a.bitmask !== undefined) p.bitmask = a.bitmask;
    if (a.bitmask_enum !== undefined) p.bitmask_enum = a.bitmask_enum;
    if (a.replication_enabled !== undefined) p.replication_enabled = a.replication_enabled;
    if (a.replication_condition !== undefined) p.replication_condition = a.replication_condition;
    if (a.is_private !== undefined) p.is_private = a.is_private;
    return p;
  },
});

// ── Functions ───────────────────────────────────────────────────────────────

const bpCreateFunction = bridgeTool({
  name: "bp_create_function",
  domain: "bp",
  description:
    "Create a new function in a Blueprint. return_type defaults to 'void'; a non-void type creates a " +
    "FunctionResult node with a 'ReturnValue' output pin of that type (no separate bp_add_function_output needed). " +
    "Returns function_name + graph_id.",
  input: z.object({
    blueprint_name: z.string().min(1),
    function_name: z.string().min(1),
    return_type: z.string().default("void"),
  }),
  annotations: { blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpCreateDispatcher = bridgeTool({
  name: "bp_create_dispatcher",
  domain: "bp",
  description:
    "Author a real Blueprint event dispatcher (multicast delegate) — a PC_MCDelegate member plus a registered " +
    "delegate SIGNATURE GRAPH, exactly as the editor's 'Add Event Dispatcher' does (NOT what " +
    "bp_create_variable variable_type='MulticastDelegate' makes, which has no signature graph and can't be bound/broadcast). " +
    "Optional params[] become the dispatcher's broadcast arguments (typed input pins on the signature). " +
    "Recompiles the Blueprint. dry_run previews without creating. Bind/call with bp_add_node once authored.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Short name or full asset path of the Blueprint."),
    dispatcher_name: z.string().min(1).describe("New identifier for the dispatcher (PascalCase by convention)."),
    params: z
      .array(
        z.object({
          name: z.string().min(1).describe("Parameter (broadcast argument) name."),
          type: z
            .string()
            .min(1)
            .describe('Type — same strings as bp_create_variable ("bool", "int", "float", "vector", "class:/struct:/enum:Name", or a /Script or /Game path).'),
          is_array: z.boolean().default(false).describe("Make this argument an array."),
        }),
      )
      .optional()
      .describe("Typed broadcast arguments, in order. Omit for a parameterless dispatcher."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      dispatcher_name: a.dispatcher_name,
      dry_run: a.dry_run,
    };
    if (a.params && a.params.length > 0) p.params = a.params;
    return p;
  },
});

const bpAddFunctionInput = bridgeTool({
  name: "bp_add_function_input",
  domain: "bp",
  description:
    "Add an input parameter to a Blueprint function. param_type: bool, int, float, string, vector, etc. " +
    "is_array makes it an array parameter.",
  input: z.object({
    blueprint_name: z.string().min(1),
    function_name: z.string().min(1),
    param_name: z.string().min(1),
    param_type: z.string().min(1),
    is_array: z.boolean().default(false),
  }),
  annotations: { blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpAddFunctionOutput = bridgeTool({
  name: "bp_add_function_output",
  domain: "bp",
  description:
    "Add an output parameter to a Blueprint function. param_type: bool, int, float, string, vector, etc. " +
    "is_array makes it an array parameter.",
  input: z.object({
    blueprint_name: z.string().min(1),
    function_name: z.string().min(1),
    param_name: z.string().min(1),
    param_type: z.string().min(1),
    is_array: z.boolean().default(false),
  }),
  annotations: { blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpRemoveFunctionInput = bridgeTool({
  name: "bp_remove_function_input",
  domain: "bp",
  description:
    "Remove an input parameter from a Blueprint function (mirrors the editor 'Remove Parameter' action). " +
    "Connections to the removed pin are dropped on next graph refresh.",
  input: z.object({
    blueprint_name: z.string().min(1),
    function_name: z.string().min(1),
    param_name: z.string().min(1).describe("Input parameter to remove."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpRemoveFunctionOutput = bridgeTool({
  name: "bp_remove_function_output",
  domain: "bp",
  description:
    "Remove an output parameter from a Blueprint function. Removed from every FunctionResult node so the " +
    "signature stays consistent across all return points.",
  input: z.object({
    blueprint_name: z.string().min(1),
    function_name: z.string().min(1),
    param_name: z.string().min(1).describe("Output parameter to remove."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpDeleteFunction = bridgeTool({
  name: "bp_delete_function",
  domain: "bp",
  description: "Delete a function from a Blueprint.",
  input: z.object({
    blueprint_name: z.string().min(1),
    function_name: z.string().min(1).describe("Function to delete."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  // 1:1 identical key names.
});

const bpRenameFunction = bridgeTool({
  name: "bp_rename_function",
  domain: "bp",
  description: "Rename a function in a Blueprint.",
  input: z.object({
    blueprint_name: z.string().min(1),
    old_function_name: z.string().min(1),
    new_function_name: z.string().min(1),
  }),
  annotations: { blockedDuringPie: true },
  // 1:1 identical key names.
});

export const bpTools: ToolDef[] = [
  bpCreateBlueprint,
  bpAddComponent,
  bpCompile,
  bpReparent,
  bpSetDefaultValue,
  bpGetParentClass,
  bpBrief,
  bpRead,
  bpInspect,
  bpGetVariableDetails,
  bpGetFunctionDetails,
  bpFunctionReferences,
  bpListComponents,
  bpListGraphs,
  bpListNodePins,
  bpSetComponentProperty,
  bpSetComponentTransform,
  bpSetClassReplication,
  bpSetEventReplication,
  bpRemoveComponent,
  bpDisconnectPin,
  bpAddNode,
  bpAddEventNode,
  bpAddCustomEvent,
  bpDeleteNode,
  bpConnectPins,
  bpSetNodeProperty,
  bpSetInnerNodeProperty,
  bpCreateVariable,
  bpDeleteVariable,
  bpSetVariableProperties,
  bpCreateFunction,
  bpCreateDispatcher,
  bpAddFunctionInput,
  bpAddFunctionOutput,
  bpRemoveFunctionInput,
  bpRemoveFunctionOutput,
  bpDeleteFunction,
  bpRenameFunction,
];
