/**
 * Generic JSON → graph transformer. Works on ANY read-tool result without knowing
 * its schema: the asset becomes a root node; nested objects become child nodes
 * linked by HAS_<KEY> relationships; scalar fields become node properties.
 *
 * Labels are derived from the containing key (singularized + PascalCased), so the
 * shape is stable and queryable — e.g. result.states[] → :State, result.nodes[] →
 * :Node, result.functions[] → :Function. A discriminator field (type/node_type/…)
 * is preserved as a property, not exploded into labels.
 *
 * Node ids are deterministic JSON-pointer-ish paths (assetPath.states[0].tasks[1]),
 * so re-ingesting MERGEs onto the same nodes — idempotent by construction.
 */

export interface GNode {
  id: string;
  label: string;
  props: Record<string, unknown>;
}
export interface GRel {
  from: string;
  to: string;
  type: string;
  props: Record<string, unknown>;
}

const RESERVED = new Set(["id", "label"]);

const isScalar = (v: unknown): boolean =>
  v === null || ["string", "number", "boolean"].includes(typeof v);

const sanitizeKey = (k: string): string => k.replace(/[^A-Za-z0-9_]/g, "_");

const pascal = (k: string): string =>
  k
    .replace(/(^|[_\s-])([a-zA-Z0-9])/g, (_m, _s, c: string) => c.toUpperCase())
    .replace(/[^A-Za-z0-9]/g, "");

function singular(k: string): string {
  if (k.endsWith("ies")) return k.slice(0, -3) + "y";
  if (k.endsWith("ses") || k.endsWith("xes")) return k.slice(0, -2);
  if (k.endsWith("s") && !k.endsWith("ss")) return k.slice(0, -1);
  return k;
}

const labelFromKey = (k: string): string => pascal(singular(sanitizeKey(k))) || "Node";
const relFromKey = (k: string): string => "HAS_" + (singular(sanitizeKey(k)).toUpperCase() || "CHILD");

/** Neo4j only stores homogeneous primitive arrays; coerce anything else to JSON text. */
function coerceArray(arr: unknown[]): unknown {
  if (arr.length === 0) return undefined;
  const t = typeof arr[0];
  const homogeneous = arr.every((x) => typeof x === t && isScalar(x));
  return homogeneous ? arr : JSON.stringify(arr);
}

function scalarProps(obj: Record<string, unknown>): Record<string, unknown> {
  const p: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(obj)) {
    const key = sanitizeKey(k);
    if (RESERVED.has(key)) continue;
    if (isScalar(v)) {
      if (v !== null) p[key] = v;
    } else if (Array.isArray(v) && v.every(isScalar)) {
      const c = coerceArray(v);
      if (c !== undefined) p[key] = c;
    }
  }
  return p;
}

function nameOf(obj: Record<string, unknown>): string | undefined {
  for (const k of ["name", "title", "id", "node_name", "state_name", "display_name", "label"]) {
    const v = obj[k];
    if (typeof v === "string" && v) return v;
  }
  return undefined;
}

export function shapeAsset(
  asset: { path: string; class: string; name?: string },
  result: unknown,
): { nodes: GNode[]; rels: GRel[] } {
  const nodes: GNode[] = [];
  const rels: GRel[] = [];

  const rootId = asset.path;
  const rootProps: Record<string, unknown> = {
    path: asset.path,
    class: asset.class,
    ...(asset.name ? { name: asset.name } : {}),
  };
  const isObj = result && typeof result === "object" && !Array.isArray(result);
  if (isObj) Object.assign(rootProps, scalarProps(result as Record<string, unknown>));
  nodes.push({ id: rootId, label: pascal(asset.class) || "Asset", props: rootProps });

  const emitChild = (key: string, childId: string, val: Record<string, unknown>, parentId: string) => {
    const props = scalarProps(val);
    const nm = nameOf(val);
    if (nm && props.name === undefined) props.name = nm;
    nodes.push({ id: childId, label: labelFromKey(key), props });
    rels.push({ from: parentId, to: childId, type: relFromKey(key), props: {} });
    walk(val, childId);
  };

  function walk(obj: Record<string, unknown>, nodeId: string): void {
    for (const [k, v] of Object.entries(obj)) {
      if (isScalar(v) || (Array.isArray(v) && v.every(isScalar))) continue; // already props
      if (Array.isArray(v)) {
        v.forEach((item, i) => {
          if (item && typeof item === "object" && !Array.isArray(item)) {
            emitChild(k, `${nodeId}.${sanitizeKey(k)}[${i}]`, item as Record<string, unknown>, nodeId);
          }
        });
      } else if (v && typeof v === "object") {
        emitChild(k, `${nodeId}.${sanitizeKey(k)}`, v as Record<string, unknown>, nodeId);
      }
    }
  }

  if (isObj) walk(result as Record<string, unknown>, rootId);

  // De-dup nodes by id (last write wins) — defensive; ids are already unique per asset.
  const seen = new Map<string, GNode>();
  for (const n of nodes) seen.set(n.id, n);
  return { nodes: [...seen.values()], rels };
}
