# ue-neo4j — generic Unreal → Neo4j ingester

Project a **live** Unreal project's assets into a Neo4j graph so their topology becomes
queryable with Cypher. Reads through this harness's canonical MCP read tools, derives
the asset set from the running editor, and loads it idempotently. **Nothing here is
game-specific** — no hardcoded asset paths, no fixed schema, no embedded credentials.

This is the runnable companion to the `/neo4j` skill (it is bundled at the skill's
`tool/` subdirectory), which
is the doctrine; this is the tool.

## How it works

```
asset_list (live editor)  ─▶  per-asset MCP read  ─▶  generic JSON→graph  ─▶  MERGE + index
  discover what exists         bp_read / statetree_read    nodes + relationships     idempotent load
                               material_read / …
```

- **Discovery is live.** `asset_list` enumerates the running project; you scope it with
  flags. No asset list is baked into the tool.
- **Typed ontological mapping (the useful part).** `src/ontology.ts` turns raw read-tool
  JSON into a *semantic* graph: typed nodes and **resolved cross-reference edges** — a
  StateTree transition's target becomes a `TARGETS_STATE`/`TRANSITIONS_TO` edge to the
  real state; a Blueprint exec wire becomes an `EXEC` edge between the two nodes (data
  wires → `DATA_<type>`); a component's parent becomes `CHILD_OF`; a material expression
  feeding an output becomes `FEEDS_MATERIAL_INPUT`. Type strings are normalized
  (`EStateTreeStateType::State` → `State`, `/Script/Engine.Actor` → `Actor`), instance
  data is flattened to `prop_*`. This is what makes the graph queryable instead of an
  inert dump. Typed shapers: **StateTree, Blueprint, Material**.
- **Generic fallback for everything else.** `src/shape.ts` turns *any* other read-tool
  JSON into a graph schema-agnostically: nested objects → child nodes linked by
  `HAS_<KEY>`, scalars → properties, labels from the containing key. Less curated, but
  works on a type before anyone writes a typed shaper for it.
- **Loading is idempotent.** Node ids are deterministic; nodes MERGE on `id`,
  relationships MERGE on `(from,to,type)`, per-label `id` indexes created first. Run it
  twice → converges, no duplicates.

## Prerequisites

1. **Editor running + harness MCP server up** (`scripts/launch-editor`, `scripts/run-server`),
   editor booted (the readers reflect live state).
2. **Neo4j reachable.** Any instance — start a throwaway one:
   ```bash
   docker run -d --name ue-neo4j -p 7474:7474 -p 7687:7687 \
     -e NEO4J_AUTH="neo4j/${NEO4J_PASS:?set NEO4J_PASS}" neo4j:5
   ```
3. `bun install` (in this directory).

## Configuration (env)

| Var | Default | Notes |
|---|---|---|
| `MCP_URL` | `http://127.0.0.1:8765` | harness MCP server (`/mcp` appended if absent) |
| `NEO4J_URI` | `bolt://localhost:7687` | Bolt endpoint |
| `NEO4J_USER` | `neo4j` | |
| `NEO4J_PASS` | *(required for graph ops)* | never hardcode/commit it |
| `UE_NEO4J_DATA_DIR` | `tmp/data` | where pulled JSON is cached |

> Run the harness server with result compaction **off** (`UNREAL_MCP_MAX_RESULT_BYTES=0`,
> the default) so reads return whole — a compacted result truncates the graph.

## Commands

```bash
bun run src/cli.ts <command> [flags]      # or: bun run ingest -- …  (package.json scripts)
```

| Command | Does |
|---|---|
| `discover` | list assets matching the selector + reader coverage (no reads, no writes) |
| `pull` | read selected assets via MCP, write their JSON to the data dir |
| `ingest` | pull (unless `--cached`) → shape → MERGE + index + verify |
| `query` | run a Cypher statement (positional) or `--file q.cypher` |
| `verify` | print node-label counts and index states |
| `clear` | `DETACH DELETE` the whole graph (requires `--yes`) |

**Selector flags** (`discover`/`pull`/`ingest`):

| Flag | Meaning |
|---|---|
| `--dir <path>` | `asset_list` root (default `/Game/`) |
| `--class <Class>` | server-side class filter (`Blueprint`, `StateTree`, `Material`, …) |
| `--filter <substr>` | client-side name/path substring (case-insensitive) |
| `--paths a,b,c` | client-side whitelist of exact asset paths |
| `--no-recursive` | don't recurse under `--dir` |
| `--data-assets` | also read `*DataAsset` classes via reflection |

**`ingest` extras:** `--cached` (shape JSON already on disk, no editor reads),
`--with-refs` (add `REFERENCES` edges between selected assets via `asset_references`),
`--clear` (wipe first — full rebuild; otherwise MERGE is additive).

## Examples

```bash
# What's in the project, and what can be read?
bun run src/cli.ts discover

# All StateTrees → graph (additive), then confirm
bun run src/cli.ts ingest --class StateTree
bun run src/cli.ts verify

# A subtree, Blueprints + Materials + their cross-references, fresh rebuild
bun run src/cli.ts ingest --dir /Game/MyFeature/ --with-refs --clear

# Just the assets whose name contains "Enemy"
bun run src/cli.ts ingest --filter Enemy

# Query: which materials does each Blueprint reference?
bun run src/cli.ts query \
  "MATCH (b:Blueprint)-[:REFERENCES]->(m:Material) RETURN b.name, collect(m.name)"
```

## What the graph looks like (ontology)

Typed shapers emit these labels and resolved edges (generic UE concepts, not
game-specific):

| Type | Node labels | Key relationships |
|---|---|---|
| **StateTree** | `StateTree`, `STState`, `STTask`, `STTransition`, `STCondition` | `HAS_STATE`, `PARENT_STATE`, `HAS_TASK`, `HAS_TRANSITION`, `TARGETS_STATE`, `TRANSITIONS_TO`, `GUARDED_BY` |
| **Blueprint** | `Blueprint`, `Class`, `Variable`, `Component`, `Function`, `Graph`, `GraphNode` | `INHERITS_FROM`, `HAS_VARIABLE`, `HAS_COMPONENT`, `CHILD_OF`, `HAS_FUNCTION`, `IMPLEMENTS_INTERFACE`, `HAS_GRAPH`, `CONTAINS_NODE`, `EXEC`, `DATA_<type>` |
| **Material** | `Material`, `MaterialExpression`, `MaterialInput`, `Texture` | `MATERIAL_WIRE`, `HAS_MATERIAL_INPUT`, `FEEDS_MATERIAL_INPUT`, `REFERENCES_TEXTURE` |

Example queries those edges unlock:

```cypher
// Execution chain out of an event
MATCH p=(:GraphNode {title:'Event Tick'})-[:EXEC*1..20]->(n) RETURN p;
// Which states can a StateTree reach, and under what trigger?
MATCH (:StateTree {name:$t})-[:HAS_STATE]->(s)-[r:TRANSITIONS_TO]->(d) RETURN s.name, r.trigger, d.name;
// What feeds a material's EmissiveColor?
MATCH (e)-[:FEEDS_MATERIAL_INPUT]->(:MaterialInput {name:'EmissiveColor'}) RETURN e.title;
```

Everything without a typed shaper falls back to the generic walk (`HAS_<KEY>` edges,
key-derived labels) — still queryable, just uncurated.

## Supported asset types

- **Typed ontology:** `Blueprint`, `StateTree`, `Material`.
- **Generic shaper:** `MaterialInstanceConstant`, `NiagaraSystem`, `DataTable`
  (+ `*DataAsset` with `--data-assets`).
- Everything else lists as *(no reader)* in `discover` until added.

## Extending

- **New type via generic shaper:** add `{ Class: { tool, pathParam } }` to
  `src/readers.ts` — one row, done.
- **New type with a curated ontology (resolved edges, clean labels):** add a `Handler`
  (`read` + `shape`) to `TYPED` in `src/ontology.ts`. `read` may make several MCP calls
  (the Blueprint handler calls `bp_read` then `bp_inspect` per graph); `shape` maps the
  doc to nodes/rels, resolving references into edges. Reuse the normalizers
  (`cleanType`, `sanitizeLabel`, `instanceProps`); keep `id`-keyed MERGE + per-label
  indexes as the invariant.

## Notes / limits

- A read that fails (asset missing, tool error) is logged and skipped — the run continues.
- `--with-refs` only links assets **both** present in the selection (no dangling stubs);
  widen the selection (e.g. ingest a whole subtree) to capture more edges.
- The graph is a **read-only projection**. Never wire it back into a mutation path —
  changes go through the typed MCP tools and the graph is re-derived.
