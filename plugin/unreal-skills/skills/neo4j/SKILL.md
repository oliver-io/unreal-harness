---
name: neo4j
description: Project a live Unreal project's assets — Blueprints, StateTrees, materials, Niagara, data tables — into a Neo4j graph database so their topology becomes queryable with Cypher. Use when someone wants to "dump/ingest assets into Neo4j", "graph the Blueprints", "build a Blueprint/StateTree graph", "query the project as a graph", "trace an execution/data-flow chain", "find all callers of X across Blueprints", "set up Neo4j for this project", or "index the AI StateTrees". Generic across ANY project driven by this harness — it reads through the harness's canonical MCP read tools and derives the asset set from the live project; it hardcodes no game-specific assets, paths, or schema. A read-only analysis projection, never a mutation path.
user-invocable: true
allowed-tools: [Read, Grep, Glob, Bash, PowerShell, AskUserQuestion]
---

# neo4j

Make an Unreal project **queryable as a graph**. Blueprints, StateTrees, materials,
Niagara systems — they're all visual programs: nodes wired to nodes. Reading them one
asset at a time (open the editor, trace the wires by eye) doesn't scale and doesn't
answer cross-cutting questions. Loading their topology into **Neo4j** turns "trace this
execution chain", "who calls this function", "what feeds this pin", "which trees share
this task" into one-line Cypher queries.

This skill is the doctrine for doing that **in this harness, generically**. It reads
through the harness's own canonical MCP read tools and derives what to ingest from the
*live project* — it bakes in no specific game's assets, paths, password, or schema.

**A runnable, generic ingester ships with the harness: `${CLAUDE_SKILL_DIR}/tool/`** (wrapped by
`${CLAUDE_SKILL_DIR}/neo4j.{ps1,sh}`). It implements everything below — discover from the live
project, read via the canonical MCP tools, shape any asset's JSON into a graph, and
load idempotently with indexes. Prefer it over hand-rolling an ingest; reach for the
doctrine when extending it (new asset type, curated schema) or when a project already
has its own pipeline. Quick start:

```bash
${CLAUDE_SKILL_DIR}/neo4j.ps1 discover                       # what exists + reader coverage
${CLAUDE_SKILL_DIR}/neo4j.ps1 ingest --class StateTree       # graph all StateTrees (additive)
${CLAUDE_SKILL_DIR}/neo4j.ps1 ingest --dir /Game/Foo/ --with-refs   # a subtree + cross-asset edges
${CLAUDE_SKILL_DIR}/neo4j.ps1 query "MATCH (n) RETURN labels(n)[0], count(*)"
```

> Follows the architecture-document doctrine (concepts, contracts, constraints — lead
> with the answer, be honest about status). Cypher templates and the schema below are
> *contracts* (stable shapes), so they belong here; anything project-specific is
> discovered at run time.

---

## The one mental model

Neo4j is a **read-only analysis projection of the live editor.** The pipeline is a
classic ETL with three stages, and the harness already owns the hard middle:

```
  READ                         TRANSFORM                  LOAD
  MCP read tools          ▸    asset JSON → nodes    ▸    MERGE into Neo4j
  (statetree_read,             + relationships            + CREATE INDEX
   bp_read, material_read…)    (UE concept → label)       (idempotent)
```

**The graph is a projection, never a source of truth.** You never mutate the project
*through* Neo4j — mutation always goes back through the typed MCP tools. The graph is
for *questions*, the MCP is for *changes*. This keeps the harness's "no escape hatches"
contract intact: Neo4j is inspection, layered on top of the inspection tools.

Two consequences that drive every decision below:

- **The editor must be running** with the MCP bridge up before you can read anything —
  the read tools talk to live editor state.
- **Re-ingest is idempotent.** Every write is a `MERGE` keyed on a stable `id`, so
  running the pipeline twice converges instead of duplicating. That's what makes
  incremental, per-asset ingestion safe.

---

## Standing up Neo4j (project-agnostic)

Neo4j runs as a container; nothing about it is project-specific. Credentials and URI
come from **environment variables** so no secret is ever hardcoded in a skill or repo.

```bash
# one-off container (data persists in a named volume)
docker run -d --name ue-neo4j \
  -p 7474:7474 -p 7687:7687 \
  -v ue-neo4j-data:/data \
  -e NEO4J_AUTH="neo4j/${NEO4J_PASS:?set NEO4J_PASS}" \
  neo4j:5
```

- **7474** — browser console (`http://localhost:7474`)
- **7687** — Bolt (driver/`cypher-shell` connection)

Connection settings every consumer should read from env (with sane local defaults):

| Var | Default | Meaning |
|---|---|---|
| `NEO4J_URI` | `bolt://localhost:7687` | Bolt endpoint |
| `NEO4J_USER` | `neo4j` | username |
| `NEO4J_PASS` | *(required — no default)* | password; never commit it |

Run Cypher from the shell without a driver:

```bash
docker exec ue-neo4j cypher-shell -u "$NEO4J_USER" -p "$NEO4J_PASS" --format plain "RETURN 1;"
```

If a project already runs its own Neo4j (its own container/compose, its own
credentials), **use that** — point `NEO4J_URI`/`NEO4J_PASS` at it rather than starting a
second instance on the same ports. Discover it (`docker ps`, the project's
`docker-compose*.yml`) before standing up your own.

---

## The data model (a recommended generic schema)

These labels describe **Unreal concepts**, not any one game — they apply to every UE
project. Use `id` as the stable MERGE key on every node and put a range index on it.

**Blueprints / graphs**

| Node label | Represents | Key non-id props |
|---|---|---|
| `Blueprint` | a Blueprint asset | `path`, `name`, `parent_class` |
| `Graph` | an event graph / function graph | `name`, `type` |
| `GraphNode` | a node in a graph | `node_type`, `title` |
| `Variable` | a Blueprint variable | `name`, `type` |
| `Component` | a component on the BP | `name`, `class` |
| `Function` | a function/event | `name` |

Relationships: `INHERITS_FROM` (Blueprint→Class/Blueprint), `HAS_VARIABLE`,
`HAS_COMPONENT`, `HAS_FUNCTION`, `HAS_GRAPH`, `CONTAINS_NODE` (Graph→GraphNode),
`EXEC` (node→node, execution wire), `DATA_FLOW` (node→node, typed data wire).

**StateTrees**

| Node label | Represents |
|---|---|
| `StateTree` | the tree asset |
| `STState` | a state (`name`, `type`, `selection_behavior`, `depth`) |
| `STTask` | a task on a state (`type`, plus `prop_*`) |
| `STTransition` | a transition (`trigger`, `target`) |
| `STCondition` | a guard condition (`type`, plus `prop_*`) |

Relationships: `HAS_STATE`, `PARENT_STATE` (hierarchy), `HAS_TASK`, `HAS_TRANSITION`,
`TARGETS_STATE`, `TRANSITIONS_TO` (convenience state→state), `GUARDED_BY`.

Materials, Niagara, data tables, anim assets extend the same pattern (one label per
concept, `id`-keyed, `prop_*` for leaf properties). Add labels as you ingest new asset
types — the schema is open, not fixed.

---

## How to ingest (the procedure)

1. **Preflight.** Editor running + MCP bridge up (the harness server connected). If not,
   start them (`scripts/launch-editor`, `scripts/run-server`) and wait for boot. Neo4j
   reachable (`cypher-shell … "RETURN 1"`). Fail loudly if either is down — the read
   tools and the loader both need their endpoint.

2. **Discover the asset set from the LIVE project — don't hardcode it.** This is the
   crux of staying generic. Use the harness's catalog/list tools to enumerate what
   actually exists:
   - `asset_list` (filter by path/class) to find Blueprints, StateTrees, materials, etc.
   - `catalog_search` / `catalog_domains` to find the right read tool per asset type.
   Let the user scope it ("all AI StateTrees", "the player Blueprint and its
   dependencies", "everything under /Game/Foo"). Confirm the resolved list before a
   large pull.

3. **Read each asset via its canonical MCP read tool.** These return structured JSON —
   the transform input. The relevant readers in this harness:
   - StateTrees: `statetree_read` (+ `statetree_state_list`, `statetree_node_get_properties`)
   - Blueprints: `bp_read`, `bp_inspect`, `bp_list_graphs`, `bp_list_node_pins`,
     `bp_get_variable_details`, `bp_get_function_details`
   - Materials: `material_read`, `material_read_instance`, `material_read_function`
   - Others: `niagara_system_read`, `asset_datatable_read`, `class_inspect`,
     `reflection_class_properties`, the `anim_*` read tools
   (Names are this harness's canonical surface. A project running a *different* MCP
   server may expose different names — read its tool list; never assume.)

4. **Transform → nodes + relationships** per the schema above. One node per concept,
   `id` a deterministic key (e.g. `<asset_path>#<local_id>`), leaf values as `prop_*`.

5. **Load with idempotent MERGE + ensure indexes.** Always `MERGE`, never `CREATE`
   blindly. Create the per-label `id` index once (cheap, `IF NOT EXISTS`):

   ```cypher
   CREATE INDEX IF NOT EXISTS FOR (n:StateTree)   ON (n.id);
   CREATE INDEX IF NOT EXISTS FOR (n:STState)     ON (n.id);
   // … one per label you ingest

   UNWIND $nodes AS n
     MERGE (x { id: n.id })
     SET x += n.props
     WITH x, n CALL apoc.create.addLabels(x, [n.label]) YIELD node RETURN count(*);
   // (or generate one MERGE per label if APOC isn't installed)

   UNWIND $rels AS r
     MATCH (a { id: r.from }), (b { id: r.to })
     MERGE (a)-[e:`%TYPE%` ]->(b);   // %TYPE% templated per relationship type
   ```

6. **Verify, don't trust the exit code.** Count what you loaded and confirm indexes are
   ONLINE:

   ```cypher
   MATCH (n) RETURN labels(n)[0] AS label, count(*) ORDER BY label;
   SHOW INDEXES YIELD labelsOrTypes, properties, state;
   ```

---

## Useful query patterns (project-agnostic)

```cypher
// Trace an execution chain out of an event node
MATCH p = (start:GraphNode {title:$event})-[:EXEC*1..20]->(end) RETURN p;

// Who reads a given variable (data-flow back to sources)
MATCH (v:Variable {name:$var})<-[:DATA_FLOW*1..10]-(consumer) RETURN DISTINCT consumer;

// Every state a StateTree can reach from its root
MATCH (t:StateTree {name:$tree})-[:HAS_STATE]->(:STState)-[:TRANSITIONS_TO*0..]->(s)
RETURN DISTINCT s.name;

// Tasks shared across more than one tree (refactor candidates)
MATCH (t:StateTree)-[:HAS_STATE]->(:STState)-[:HAS_TASK]->(k:STTask)
WITH k.type AS task, collect(DISTINCT t.name) AS trees
WHERE size(trees) > 1 RETURN task, trees;

// Blueprints inheriting from a base class
MATCH (b:Blueprint)-[:INHERITS_FROM*]->(c {name:$base}) RETURN b.name;
```

---

## Constraints and invariants

- **Editor + bridge must be live before any read.** The readers reflect live editor
  state; a stale or absent editor yields nothing or errors. Start and wait for boot.
- **Never `CREATE` for re-ingestible data — always `MERGE` on `id`.** Non-idempotent
  loads duplicate nodes on the second run and silently corrupt counts.
- **Index every label's `id` before bulk MERGE.** Without the index, MERGE does a full
  scan per row and a large ingest crawls.
- **Don't `CLEAR`/`DETACH DELETE` the whole DB to add a few assets.** Scope the
  re-ingest to the assets you're refreshing (MERGE handles the overwrite). A full clear
  is only for a deliberate from-scratch rebuild — confirm with the user first; it's
  destructive to everything else already graphed.
- **Derive the asset set from the project, not from this skill.** The moment you paste a
  fixed list of asset paths into the pipeline, it stops being generic. Enumerate via
  `asset_list`/`catalog_search` and let the user scope.
- **Read-only projection.** Never wire Neo4j back into a mutation path. Changes go
  through typed MCP tools; the graph is re-derived from the result.
- **Credentials from env only.** `NEO4J_PASS` is never written into a skill, script, or
  committed file. A project that ships its own Neo4j owns its own credentials — read
  them, don't duplicate them.
- **One Neo4j per port pair.** 7474/7687 bind once. If a project already runs Neo4j,
  target it rather than starting a competing instance.

---

## Status / honesty

- The harness **ships a working ingester** at `${CLAUDE_SKILL_DIR}/tool/` (Bun/TS, wrapped by
  `${CLAUDE_SKILL_DIR}/neo4j.{ps1,sh}`) implementing the read→shape→load pipeline above. Validated
  end-to-end against a live editor (resolved `EXEC`/`DATA_*` wires, `CHILD_OF` component
  hierarchy, `FEEDS_MATERIAL_INPUT`, cross-asset `REFERENCES`).
- **Typed ontological shapers** (`${CLAUDE_SKILL_DIR}/tool/src/ontology.ts`) produce the curated schema
  above for `Blueprint`, `StateTree`, `Material` — resolving cross-references into edges
  and normalizing types (`EStateTreeStateType::State` → `State`), not just walking JSON.
  `MaterialInstanceConstant`, `NiagaraSystem`, `DataTable` (+ `*DataAsset`) use the
  generic walk (`${CLAUDE_SKILL_DIR}/tool/src/shape.ts`); other classes list as *(no reader)*.
- To give a new type first-class treatment, add a `Handler` (multi-call `read` + typed
  `shape`) to `TYPED` in `ontology.ts`; for a quick generic mapping, add a row to
  `readers.ts`. Keep `id`-keyed MERGE + per-label indexes as the invariant.
- If a project already has its own ingest pipeline, **use it** — just be aware it may
  target a different MCP server with different tool names; confirm the names against that
  server's tool list before assuming `statetree_read` vs `read_state_tree` etc.
- The schema here is a recommended baseline, not a fixed contract. Extend labels/rels as
  you ingest more asset types; keep `id`-keyed MERGE and per-label indexes as the
  invariant.
