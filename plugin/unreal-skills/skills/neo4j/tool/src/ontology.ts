/**
 * Ontological mapping — turns raw read-tool JSON into a SEMANTIC graph: typed
 * nodes, resolved cross-reference edges (a transition's target becomes an edge to
 * the real state; an exec wire becomes an EXEC edge between the two nodes), and
 * normalized properties. This is what makes the graph useful to query rather than
 * a faithful-but-inert dump of the JSON shape.
 *
 * Generic across projects: the rules map UE *concepts* (StateTree states, Blueprint
 * graph wires, material expressions), never any game's specific assets. Types we
 * don't have a typed shaper for fall back to the schema-agnostic walk in shape.ts.
 *
 * A "handler" pairs a (possibly multi-call) READ with a SHAPE. Add a row to
 * `HANDLERS` to give a new asset type first-class treatment.
 */

import type { Mcp } from "./mcp.ts";
import type { Asset } from "./select.ts";
import { type GNode, type GRel, shapeAsset } from "./shape.ts";
import { READERS, readerFor } from "./readers.ts";

// ── shared normalization ─────────────────────────────────────────────────────
export const sanitizeLabel = (raw: string): string =>
  (raw || "").replace(/[^A-Za-z0-9_]/g, "_").replace(/^(\d)/, "_$1") || "Unknown";

export const sanitizeRelType = (raw: string): string =>
  (raw || "unknown").toUpperCase().replace(/[^A-Z0-9_]/g, "_");

/** Strip UE noise from a type/class string: enum scopes, /Script/ paths, object suffix. */
export function cleanType(s: unknown): string {
  let x = String(s ?? "");
  if (x.includes("::")) x = x.split("::").pop()!; // EStateTreeStateType::State -> State
  if (x.includes("/")) x = x.split("/").pop()!; //   /Script/Engine.Actor -> Engine.Actor
  if (x.includes(".")) x = x.split(".").pop()!; //   Engine.Actor -> Actor
  return x;
}

const isScalar = (v: unknown): boolean =>
  v === null || ["string", "number", "boolean"].includes(typeof v);

/** Flatten a node's instance_data ({properties:[{name,value}]} or a map) to prop_* keys. */
function instanceProps(node: any): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  const id = node?.instance_data ?? node?.instanceData;
  const props = id?.properties ?? id;
  if (Array.isArray(props)) {
    for (const p of props) if (p?.name != null && isScalar(p.value)) out[`prop_${sanitizeLabel(p.name)}`] = p.value;
  } else if (props && typeof props === "object") {
    for (const [k, v] of Object.entries(props)) if (isScalar(v)) out[`prop_${sanitizeLabel(k)}`] = v as any;
  }
  return out;
}

/** Copy primitive fields (skipping the given structural keys) onto a props bag. */
function scalarsExcept(obj: Record<string, unknown>, skip: Set<string>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(obj)) {
    if (skip.has(k)) continue;
    if (isScalar(v) && v !== null) out[sanitizeLabel(k)] = v;
    else if (Array.isArray(v) && v.every(isScalar) && v.length) out[sanitizeLabel(k)] = v;
  }
  return out;
}

// ── handler type ──────────────────────────────────────────────────────────────
export interface Handler {
  read: (mcp: Mcp, asset: Asset) => Promise<any>;
  shape: (asset: Asset, doc: any) => { nodes: GNode[]; rels: GRel[] };
}

/** Default read = single tool call to the class's reader. */
function singleRead(mcp: Mcp, asset: Asset, opts: { dataAssets?: boolean }): Promise<any> {
  const r = readerFor(asset.class, opts)!;
  return mcp.call(r.tool, { [r.pathParam]: asset.path, ...(r.args ?? {}) });
}

// ── StateTree shaper ───────────────────────────────────────────────────────────
function shapeStateTree(asset: Asset, doc: any): { nodes: GNode[]; rels: GRel[] } {
  const nodes: GNode[] = [];
  const rels: GRel[] = [];
  const stId = asset.path;
  const states: any[] = doc?.states ?? [];

  nodes.push({
    id: stId,
    label: "StateTree",
    props: { path: asset.path, name: asset.name ?? cleanType(asset.path), schema_class: cleanType(doc?.schema_class) },
  });

  const sid = (id: string) => `${stId}::state::${id}`;

  for (const st of states) {
    const stateId = sid(st.id);
    nodes.push({
      id: stateId,
      label: "STState",
      props: {
        st_id: st.id,
        name: st.name,
        type: cleanType(st.type),
        selection_behavior: cleanType(st.selection_behavior),
        depth: st.depth,
        child_count: st.child_count,
        enabled: st.enabled,
        weight: st.weight,
      },
    });
    rels.push({ from: stId, to: stateId, type: "HAS_STATE", props: {} });
    if (st.parent_id) rels.push({ from: stateId, to: sid(st.parent_id), type: "PARENT_STATE", props: {} });

    (st.tasks ?? []).forEach((task: any, i: number) => {
      const taskId = `${stateId}::task::${i}`;
      nodes.push({ id: taskId, label: "STTask", props: { type: cleanType(task.type), ...instanceProps(task) } });
      rels.push({ from: stateId, to: taskId, type: "HAS_TASK", props: {} });
    });

    (st.transitions ?? []).forEach((tr: any, i: number) => {
      const transId = `${stateId}::trans::${i}`;
      nodes.push({ id: transId, label: "STTransition", props: { trigger: cleanType(tr.trigger), target: tr.target, priority: cleanType(tr.priority) } });
      rels.push({ from: stateId, to: transId, type: "HAS_TRANSITION", props: {} });

      // Resolve the target — harness references states by GUID; tolerate name too.
      const target = states.find((s) => s.id === tr.target) ?? states.find((s) => s.name === tr.target);
      if (target) {
        rels.push({ from: transId, to: sid(target.id), type: "TARGETS_STATE", props: {} });
        rels.push({ from: stateId, to: sid(target.id), type: "TRANSITIONS_TO", props: { trigger: cleanType(tr.trigger) } });
      }

      (tr.conditions ?? []).forEach((cond: any, ci: number) => {
        const condId = `${transId}::cond::${ci}`;
        nodes.push({ id: condId, label: "STCondition", props: { type: cleanType(cond.type), ...instanceProps(cond) } });
        rels.push({ from: transId, to: condId, type: "GUARDED_BY", props: {} });
      });
    });
  }
  return { nodes, rels };
}

// ── Blueprint shaper ────────────────────────────────────────────────────────────
const deriveRelType = (fromPinType?: string, toPinType?: string): string => {
  const t = fromPinType ?? toPinType ?? "unknown";
  return t === "exec" ? "EXEC" : `DATA_${sanitizeRelType(t)}`;
};

function shapeBlueprint(asset: Asset, doc: any): { nodes: GNode[]; rels: GRel[] } {
  const nodes: GNode[] = [];
  const rels: GRel[] = [];
  const bp = doc?.bp ?? doc; // tolerate a bare bp_read result
  const graphs: Record<string, any> = doc?.graphs ?? {};
  const bpId = asset.path;

  nodes.push({
    id: bpId,
    label: "Blueprint",
    props: { name: bp.blueprint_name ?? asset.name, path: bpId, parent_class: cleanType(bp.parent_class) },
  });

  if (bp.parent_class && bp.parent_class !== "None") {
    const classId = `class::${cleanType(bp.parent_class)}`;
    nodes.push({ id: classId, label: "Class", props: { name: cleanType(bp.parent_class) } });
    rels.push({ from: bpId, to: classId, type: "INHERITS_FROM", props: {} });
  }

  for (const v of bp.variables ?? []) {
    const vid = `${bpId}::var::${v.name}`;
    nodes.push({ id: vid, label: "Variable", props: { name: v.name, type: cleanType(v.type), default_value: v.default_value, is_editable: v.is_editable } });
    rels.push({ from: bpId, to: vid, type: "HAS_VARIABLE", props: { var_type: cleanType(v.type) } });
  }

  for (const c of bp.components ?? []) {
    const cid = `${bpId}::comp::${c.name}`;
    nodes.push({ id: cid, label: "Component", props: { name: c.name, class: cleanType(c.class), is_root: c.is_root, attach_socket: c.attach_socket } });
    rels.push({ from: bpId, to: cid, type: "HAS_COMPONENT", props: {} });
    if (c.parent_component) rels.push({ from: cid, to: `${bpId}::comp::${c.parent_component}`, type: "CHILD_OF", props: {} });
  }

  for (const f of bp.functions ?? []) {
    const fid = `${bpId}::func::${f.name}`;
    nodes.push({ id: fid, label: "Function", props: { name: f.name, graph_type: cleanType(f.graph_type), node_count: f.node_count } });
    rels.push({ from: bpId, to: fid, type: "HAS_FUNCTION", props: {} });
  }

  for (const i of bp.interfaces ?? []) {
    const name = typeof i === "string" ? i : i?.name;
    if (!name) continue;
    const iid = `interface::${cleanType(name)}`;
    nodes.push({ id: iid, label: "Interface", props: { name: cleanType(name) } });
    rels.push({ from: bpId, to: iid, type: "IMPLEMENTS_INTERFACE", props: {} });
  }

  // Per-graph nodes + typed wires (the execution/data-flow value).
  for (const [graphName, gd] of Object.entries(graphs)) {
    if (!gd) continue;
    const gid = `${bpId}::graph::${graphName}`;
    nodes.push({ id: gid, label: "Graph", props: { name: graphName, type: cleanType((gd as any).graph_type) } });
    rels.push({ from: bpId, to: gid, type: "HAS_GRAPH", props: {} });
    // Tie a function graph to its Function node when names line up.
    if ((bp.functions ?? []).some((f: any) => f.name === graphName)) {
      rels.push({ from: `${bpId}::func::${graphName}`, to: gid, type: "FUNCTION_GRAPH", props: {} });
    }

    const gnodes: any[] = (gd as any).nodes ?? [];
    const byName = new Map<string, any>(gnodes.map((n) => [n.name, n]));
    const nodeId = (name: string) => `${bpId}::${graphName}::${name}`;

    for (const n of gnodes) {
      const skip = new Set(["pins", "connections"]);
      nodes.push({
        id: nodeId(n.name),
        label: "GraphNode",
        props: { node_class: cleanType(n.class), ...scalarsExcept(n, skip) },
      });
      rels.push({ from: gid, to: nodeId(n.name), type: "CONTAINS_NODE", props: {} });
    }

    const pinType = (nodeName: string, pin: string, dir: "Output" | "Input"): string | undefined =>
      byName.get(nodeName)?.pins?.find((p: any) => p.name === pin && p.direction === dir)?.type;

    // Dedupe (canonical pair) + orient (Output -> Input) + type the edge.
    const seen = new Set<string>();
    for (const c of (gd as any).connections ?? []) {
      const key = [`${c.from_node}.${c.from_pin}`, `${c.to_node}.${c.to_pin}`].sort().join("|");
      if (seen.has(key)) continue;
      seen.add(key);

      let conn = c;
      const fwd = pinType(c.from_node, c.from_pin, "Output") && pinType(c.to_node, c.to_pin, "Input");
      const rev = pinType(c.to_node, c.to_pin, "Output") && pinType(c.from_node, c.from_pin, "Input");
      if (!fwd && rev) conn = { from_node: c.to_node, from_pin: c.to_pin, to_node: c.from_node, to_pin: c.from_pin };

      if (!byName.has(conn.from_node) || !byName.has(conn.to_node)) continue;
      const pt = pinType(conn.from_node, conn.from_pin, "Output") ?? pinType(conn.to_node, conn.to_pin, "Input");
      rels.push({
        from: nodeId(conn.from_node),
        to: nodeId(conn.to_node),
        type: deriveRelType(pt),
        props: { from_pin: conn.from_pin, to_pin: conn.to_pin, pin_type: pt ?? "unknown" },
      });
    }
  }
  return { nodes, rels };
}

async function readBlueprint(mcp: Mcp, asset: Asset): Promise<any> {
  const bp = await mcp.call("bp_read", { blueprint_path: asset.path });
  const graphNames = ["EventGraph", ...(bp.functions ?? []).map((f: any) => f.name).filter(Boolean)];
  const graphs: Record<string, any> = {};
  for (const gn of graphNames) {
    try {
      const g = await mcp.call("bp_inspect", { blueprint_path: asset.path, graph_name: gn });
      const gd = g?.graph_data ?? g;
      if (gd?.nodes?.length) graphs[gn] = gd;
    } catch {
      /* a graph that can't be inspected is skipped */
    }
  }
  return { bp, graphs };
}

// ── Material shaper ──────────────────────────────────────────────────────────────
function exprTitle(e: any): string {
  const p = e.properties ?? {};
  switch (e.type) {
    case "TextureSample":
    case "TextureSampleParameter2D": {
      const tex = typeof p.texture === "string" && p.texture !== "None" ? cleanType(p.texture) : "";
      return `${e.type}: ${tex}${p.parameter_name ? ` (${p.parameter_name})` : ""}`;
    }
    case "Constant": return `Constant: ${p.value ?? 0}`;
    case "Constant3Vector": return `Constant3: (${p.r ?? 0}, ${p.g ?? 0}, ${p.b ?? 0})`;
    case "ScalarParameter": return `ScalarParam: ${p.parameter_name ?? "?"} = ${p.default_value ?? 0}`;
    case "VectorParameter": return `VectorParam: ${p.parameter_name ?? "?"}`;
    case "LinearInterpolate": return "Lerp";
    default: return e.type;
  }
}

function textureRole(file: string): string {
  const s = file.toLowerCase();
  if (/_n\b/.test(s) || s.endsWith("_normal")) return "Normal";
  if (/_orm\b/.test(s)) return "ORM";
  if (/_r\b/.test(s) || s.endsWith("_roughness")) return "Roughness";
  if (/_m\b/.test(s) || s.endsWith("_metallic")) return "Metallic";
  if (/_b\b/.test(s) || s.endsWith("_basecolor") || s.endsWith("_diffuse")) return "BaseColor";
  if (/_ao\b/.test(s)) return "AO";
  return "Unknown";
}

function shapeMaterial(asset: Asset, doc: any): { nodes: GNode[]; rels: GRel[] } {
  const nodes: GNode[] = [];
  const rels: GRel[] = [];
  const matId = asset.path;
  const exprs: any[] = doc?.expressions ?? [];
  const inputs: Record<string, any> = doc?.material_inputs ?? {};

  nodes.push({
    id: matId,
    label: "Material",
    props: {
      name: doc?.material_name ?? asset.name,
      path: matId,
      blend_mode: cleanType(doc?.blend_mode),
      shading_model: cleanType(doc?.shading_model),
      material_domain: cleanType(doc?.material_domain),
      two_sided: doc?.two_sided,
    },
  });

  const byName = new Map<string, any>(exprs.map((e) => [e.name, e]));
  const exprId = (name: string) => `${matId}::expr::${name}`;

  // Reachability: which expressions feed a material output?
  const connected = new Set<string>();
  const walk = (name: string) => {
    if (!name || connected.has(name)) return;
    connected.add(name);
    for (const inp of byName.get(name)?.inputs ?? []) if (inp.connected_expression) walk(inp.connected_expression);
  };
  for (const conn of Object.values(inputs)) walk((conn as any)?.connected_expression);

  for (const e of exprs) {
    const flat: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(e.properties ?? {})) if (isScalar(v)) flat[`prop_${sanitizeLabel(k)}`] = v as any;
    nodes.push({
      id: exprId(e.name),
      label: "MaterialExpression",
      props: { name: e.name, type: cleanType(e.type), title: exprTitle(e), pos_x: e.pos_x, pos_y: e.pos_y, is_connected: connected.has(e.name), ...flat },
    });

    for (const inp of e.inputs ?? []) {
      if (!inp.connected_expression) continue;
      rels.push({
        from: exprId(inp.connected_expression),
        to: exprId(e.name),
        type: "MATERIAL_WIRE",
        props: { target_input: inp.name, source_output_index: inp.connected_output_index ?? 0 },
      });
    }

    const tex = e.properties?.texture;
    if (typeof tex === "string" && tex !== "None") {
      const texId = `texture::${tex}`;
      const role = textureRole(cleanType(tex));
      nodes.push({ id: texId, label: "Texture", props: { path: tex, name: cleanType(tex), role } });
      rels.push({ from: exprId(e.name), to: texId, type: "REFERENCES_TEXTURE", props: { texture_role: role } });
    }
  }

  for (const [inName, conn] of Object.entries(inputs)) {
    const inId = `${matId}::matinput::${inName}`;
    nodes.push({ id: inId, label: "MaterialInput", props: { name: inName } });
    rels.push({ from: matId, to: inId, type: "HAS_MATERIAL_INPUT", props: {} });
    const src = (conn as any)?.connected_expression;
    if (src) rels.push({ from: exprId(src), to: inId, type: "FEEDS_MATERIAL_INPUT", props: { output_index: (conn as any).connected_output_index ?? 0 } });
  }
  return { nodes, rels };
}

// ── registry ────────────────────────────────────────────────────────────────────
const TYPED: Record<string, Handler> = {
  StateTree: { read: (m, a) => singleRead(m, a, {}), shape: shapeStateTree },
  Blueprint: { read: readBlueprint, shape: shapeBlueprint },
  Material: { read: (m, a) => singleRead(m, a, {}), shape: shapeMaterial },
};

/**
 * Resolve a handler for an asset class. Typed shaper if we have one; otherwise the
 * class's single-call reader paired with the schema-agnostic generic shaper; else
 * null (no reader — caller skips it).
 */
export function handlerFor(cls: string, opts: { dataAssets?: boolean } = {}): Handler | null {
  if (TYPED[cls]) return TYPED[cls];
  if (READERS[cls] || (opts.dataAssets && /DataAsset$/.test(cls))) {
    return { read: (m, a) => singleRead(m, a, opts), shape: (a, doc) => shapeAsset(a, doc) };
  }
  return null;
}

/** Labels a typed shaper can emit — for docs/help. */
export const TYPED_CLASSES = Object.keys(TYPED);
