/**
 * Canonical-naming lint — Bun port of `src/MCP/lint_canonical_names.py`.
 *
 * Operating over the registry (not source AST) makes this trivial: every tool
 * name must be `<domain>_<verb>(_<modifier>)*` with a registered domain prefix,
 * and the tool's declared `domain` must match that prefix. A small exempt set
 * covers protocol primitives.
 *
 *   bun run lint:names      # exit 1 on any violation
 */

import { buildRegistry } from "../src/register.ts";

// Longest-match wins (multi-word domains first). NOTE: the abbreviated `st`
// domain was retired — `statetree_*` is the single canonical StateTree family
// (full word, consistent with material/niagara/physics; a strict superset of
// the old st_*). `st_*` names are now violations by design.
const DOMAIN_PREFIXES = [
  "ik_rig", "ik_retarget", "actor", "bp", "build", "material", "niagara", "anim",
  "widget", "statetree", "eqs", "tag", "gas", "asset", "editor", "pie",
  "class", "enum", "ai", "reflection", "mesh", "physics", "kinematics",
  "input", "struct", "datatable", "dataasset", "mpc", "scene", "level",
  "landscape", "foliage", "project", "catalog", "code", "result", "pcg",
  "video",
].sort((a, b) => b.length - a.length);

// Protocol primitives intentionally un-prefixed (mirror the Python exempt set).
const EXEMPT = new Set(["mcp_status", "find_actors_by_name"]);

const CANONICAL_RE = /^[a-z][a-z0-9]*(_[a-z][a-z0-9]*)+$/;

function domainPrefixOf(name: string): string | undefined {
  for (const p of DOMAIN_PREFIXES) {
    if (name !== p && name.startsWith(p + "_")) return p;
  }
  return undefined;
}

const registry = buildRegistry();
const violations: string[] = [];
let exempt = 0;

for (const def of registry.all()) {
  if (EXEMPT.has(def.name)) {
    exempt++;
    continue;
  }
  const prefix = domainPrefixOf(def.name);
  if (!CANONICAL_RE.test(def.name) || !prefix) {
    violations.push(`${def.name}: not <domain>_<verb> (unknown or missing domain prefix)`);
    continue;
  }
  if (def.domain !== prefix) {
    violations.push(`${def.name}: declared domain "${def.domain}" != name prefix "${prefix}"`);
  }
}

console.log(
  `lint: ${registry.size()} tools, ${violations.length} violations, ${exempt} exempt`,
);
for (const v of violations) console.log(`  ✗ ${v}`);
process.exit(violations.length > 0 ? 1 : 0);
