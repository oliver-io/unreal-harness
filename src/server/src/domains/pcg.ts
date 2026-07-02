/**
 * Domain: pcg — Procedural Content Generation (UPCGGraph / nodes / components).
 *
 * Port of the `pcg_*` family in `src/MCP/server.py` (see docs/proposals/pcg-mcp.md).
 * Wire command == tool name. Thin forwards to the C++ FMCPPCGCommands handler,
 * which drives the runtime UPCGGraph API directly (the editor UEdGraph mirror
 * rebuilds itself from NotifyGraphChanged).
 *
 * First vertical slice:
 *   Discovery : pcg_list_graphs, pcg_list_node_types
 *   Read      : pcg_graph_read
 *   Authoring : pcg_graph_create, pcg_node_add, pcg_node_connect, pcg_node_set_property
 *   Drive     : pcg_component_add, pcg_component_generate
 *
 * Note: the authoring/drive mutators are not in the C++ PIE/dry-run blocklists
 * (MCPCommonUtils), so they carry no blockedDuringPie/dryRunUnsupported hints —
 * kept truthful to actual enforcement. See docs/proposals/pcg-mcp.md.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const pcgListGraphs = bridgeTool({
  name: "pcg_list_graphs",
  domain: "pcg",
  description:
    "List PCG Graph (and Graph Instance) assets in the project. Returns a `graphs` " +
    "array of {name, path, class}.",
  input: z.object({
    path_filter: z
      .string()
      .default("/Game")
      .describe('Only return graphs whose path starts with this prefix (default "/Game").'),
  }),
  annotations: { readOnlyHint: true },
});

const pcgListNodeTypes = bridgeTool({
  name: "pcg_list_node_types",
  domain: "pcg",
  description:
    "Enumerate available PCG node types (library-exposed UPCGSettings classes) — the " +
    "palette for pcg_node_add. Returns a `node_types` array of {class_name (pass to " +
    "pcg_node_add), class_path, title, type, preconfigured_count, input_pins, output_pins}.",
  input: z.object({
    name_filter: z
      .string()
      .default("")
      .describe("Optional case-sensitive substring matched against class name or title."),
    category_filter: z
      .string()
      .default("")
      .describe(
        'Optional substring matched against the node type/category ' +
          '(e.g. "Sampler", "Spawner", "PointOps", "Filter", "Spatial").',
      ),
  }),
  annotations: { readOnlyHint: true },
});

const pcgGraphRead = bridgeTool({
  name: "pcg_graph_read",
  domain: "pcg",
  description:
    "Read a PCG Graph's structure. Returns input_node, output_node, a `nodes` array " +
    "(id, settings_class, title, pos_x/pos_y, input_pins, output_pins) and an `edges` " +
    "array (from_node, from_pin, to_node, to_pin). Node `id` values are the handles used " +
    "by pcg_node_connect / pcg_node_set_property.",
  input: z.object({
    graph_path: z
      .string()
      .describe('Full path to the PCG Graph asset (e.g. "/Game/PCG/PCG_Scatter").'),
  }),
  annotations: { readOnlyHint: true },
});

const pcgGraphCreate = bridgeTool({
  name: "pcg_graph_create",
  domain: "pcg",
  description:
    "Create a new (empty) PCG Graph asset. It comes with its Input and Output nodes " +
    "in place; add interior nodes with pcg_node_add and wire them with pcg_node_connect. " +
    "Auto-saves. Returns graph, asset_path, input_node, output_node.",
  input: z.object({
    graph_path: z
      .string()
      .describe('Full /Game/... destination, e.g. "/Game/PCG/PCG_Scatter".'),
  }),
});

const pcgNodeAdd = bridgeTool({
  name: "pcg_node_add",
  domain: "pcg",
  description:
    "Add a node of a given settings type to a PCG Graph. Returns the created `node` " +
    "(id, settings_class, title, pins).",
  input: z.object({
    graph_path: z.string().describe("Full path to the PCG Graph asset."),
    settings_class: z
      .string()
      .describe(
        'A PCG settings class name (e.g. "PCGCreatePointsSettings", ' +
          '"PCGStaticMeshSpawnerSettings") or its full class_path. Discover valid values ' +
          "with pcg_list_node_types.",
      ),
    node_title: z
      .string()
      .default("")
      .describe("Optional human-friendly title (also usable as a node handle later)."),
    pos_x: z.number().int().optional().describe("Optional editor canvas X position."),
    pos_y: z.number().int().optional().describe("Optional editor canvas Y position."),
  }),
  // Omit empty/unset optionals (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {
      graph_path: a.graph_path,
      settings_class: a.settings_class,
    };
    if (a.node_title) p.node_title = a.node_title;
    if (a.pos_x !== undefined) p.pos_x = a.pos_x;
    if (a.pos_y !== undefined) p.pos_y = a.pos_y;
    return p;
  },
});

const pcgNodeConnect = bridgeTool({
  name: "pcg_node_connect",
  domain: "pcg",
  description:
    "Connect an output pin of one node to an input pin of another. Returns confirmation " +
    "of from_node/from_pin -> to_node/to_pin.",
  input: z.object({
    graph_path: z.string().describe("Full path to the PCG Graph asset."),
    from_node: z
      .string()
      .describe('Source node id (from pcg_graph_read) or the token "InputNode".'),
    to_node: z
      .string()
      .describe('Target node id or the token "OutputNode".'),
    from_pin: z
      .string()
      .default("")
      .describe("Output pin label. Defaults to the source node's first output pin."),
    to_pin: z
      .string()
      .default("")
      .describe("Input pin label. Defaults to the target node's first input pin."),
  }),
  params: (a) => {
    const p: Record<string, unknown> = {
      graph_path: a.graph_path,
      from_node: a.from_node,
      to_node: a.to_node,
    };
    if (a.from_pin) p.from_pin = a.from_pin;
    if (a.to_pin) p.to_pin = a.to_pin;
    return p;
  },
});

const pcgNodeSetProperty = bridgeTool({
  name: "pcg_node_set_property",
  domain: "pcg",
  description:
    "Set an EditAnywhere property on a node's settings object (e.g. the mesh on a " +
    "StaticMeshSpawner, a count on CreatePoints). Returns node, property_name, settings_class.",
  input: z.object({
    graph_path: z.string().describe("Full path to the PCG Graph asset."),
    node: z.string().describe("Node id (from pcg_graph_read) or its title."),
    property_name: z.string().describe("The settings property to set."),
    property_value: z
      .unknown()
      .describe("The value (string/number/bool/object, matching the property)."),
  }),
});

const pcgComponentAdd = bridgeTool({
  name: "pcg_component_add",
  domain: "pcg",
  description:
    "Add a UPCGComponent to an actor in the level and (optionally) assign a graph. " +
    "Spawn an actor first with actor_spawn if you need a fresh host. Returns actor, " +
    "component, graph.",
  input: z.object({
    actor_name: z.string().describe("Label or name of an actor in the editor world."),
    graph_path: z
      .string()
      .default("")
      .describe("Optional PCG Graph asset to assign to the component."),
  }),
  params: (a) => {
    const p: Record<string, unknown> = { actor_name: a.actor_name };
    if (a.graph_path) p.graph_path = a.graph_path;
    return p;
  },
});

const pcgComponentGenerate = bridgeTool({
  name: "pcg_component_generate",
  domain: "pcg",
  description:
    "Trigger generation on the PCG component attached to an actor. Generation is " +
    "asynchronous (scheduled on the PCG subsystem) — screenshot or re-query the level a " +
    "moment later to see results. Returns generation_requested, is_generating, and a note.",
  input: z.object({
    actor_name: z.string().describe("Label or name of the actor carrying the PCG component."),
    force: z
      .boolean()
      .default(true)
      .describe("Force regeneration even if inputs look unchanged (default true)."),
  }),
});

export const pcgTools: ToolDef[] = [
  pcgListGraphs,
  pcgListNodeTypes,
  pcgGraphRead,
  pcgGraphCreate,
  pcgNodeAdd,
  pcgNodeConnect,
  pcgNodeSetProperty,
  pcgComponentAdd,
  pcgComponentGenerate,
];
