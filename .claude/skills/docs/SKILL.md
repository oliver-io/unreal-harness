---
name: docs
description: Build and maintain a game project's architecture-documentation system — a bidirectionally-navigable, code-synced knowledge base genericized from a production UE reference project's two-taxonomy model. One namespace, three modes. `/docs taxonomy` builds the FORWARD taxonomy (the file-by-file source atlas TAXONOMY.md + doc router INDEX.md, for top-down navigation). `/docs reverse` builds the REVERSE taxonomy (backward-reference CLAUDE.md files that point a leaf folder up to its owning doc + an ANNOTATE.md backlog of undocumented systems). `/docs audit` is the SYNC loop (verify each doc against the code, one per pass, with a resumable ledger). Use for "build a taxonomy / index the codebase / source atlas", "add folder pointers / annotate the source tree / stop agents drifting off-architecture", "audit the docs / check docs against code / find stale docs". Markdown-only — no source edits, no editor mutation. Generic across any UE project (C++-first, Blueprint-first, or with a TS backend); derives source roots from the project, hardcodes none. The doc *bodies* themselves are written by the sibling `/architect` skill.
user-invocable: true
argument-hint: "[taxonomy|reverse|audit]"
allowed-tools: [Read, Write, Edit, Grep, Glob, Bash, Skill, AskUserQuestion, WebSearch, WebFetch, mcp__unrealMCP__asset_list, mcp__unrealMCP__asset_references, mcp__unrealMCP__project_context, mcp__unrealMCP__class_query, mcp__unrealMCP__class_inspect, mcp__unrealMCP__reflection_class_properties, mcp__unrealMCP__catalog_search, mcp__unrealMCP__bp_brief, mcp__unrealMCP__bp_inspect, mcp__unrealMCP__bp_read, mcp__unrealMCP__bp_list_graphs, mcp__unrealMCP__bp_list_components, mcp__unrealMCP__bp_get_variable_details, mcp__unrealMCP__bp_get_function_details, mcp__unrealMCP__bp_get_parent_class, mcp__unrealMCP__actor_get_in_level, mcp__unrealMCP__find_actors_by_name, mcp__unrealMCP__material_read, mcp__unrealMCP__statetree_read, mcp__unrealMCP__niagara_system_read]
---

# docs — the architecture-documentation system

One skill, one namespace, for standing up and maintaining a game project's documentation as a **bidirectionally-navigable, code-synced knowledge base**. Genericized from a production UE reference project's proven model so any game gets the same workbench.

## The model you are maintaining

A documentation system is **five layers**, with **code as ground truth** at the bottom:

| Layer | Artifact | Navigation |
|---|---|---|
| 1 | Project root `CLAUDE.md` — constitution + router | — |
| 2 | `docs/architecture/INDEX.md` — doc router (1 line/doc, **no status tags**) | — |
| 3 | `docs/architecture/**/*.md` — the architecture docs (written by **`/architect`**) | — |
| 4 | **`TAXONOMY.md`** — file-by-file source atlas | **forward**: architecture → files (top-down) |
| 5 | source-tree folder **`CLAUDE.md`** files | **reverse**: leaf folder → owning doc (bottom-up) |
| — | code | ground truth |

The two taxonomies are **duals.** The forward atlas (layer 4) lets an agent explore *from the top*; the reverse pointers (layer 5) stop an agent that lands deep in a folder — chasing a symbol, wandering leaf-to-sibling-leaf — from **drifting off the architectural principles** the docs encode. Three concerns keep all of it honest against the code: the forward index (`/docs taxonomy`), the reverse pointers (`/docs reverse`), and the doc-body verification loop (`/docs audit`). The **doc bodies themselves** (layer 3) are authored by **`/architect`** — this skill indexes, points at, and verifies them; it never writes them.

## Modes — dispatch on the argument

`$ARGUMENTS` selects the mode:

- **`taxonomy`** (aliases: `forward`, `map`, `atlas`, `index`) → build/maintain the forward taxonomy.
- **`reverse`** (aliases: `backward`, `link`, `pointers`, `annotate`) → build/maintain the reverse taxonomy.
- **`audit`** (aliases: `sync`, `verify`, `stale`, `check`) → verify the docs against the code.

If no mode is given, infer it from the request ("the docs are stale" → audit; "index the codebase" → taxonomy; "point folders at their docs" → reverse). If genuinely ambiguous, ask with `AskUserQuestion`. To write or update a *doc body*, this is the wrong skill — use **`/architect`**; say so and stop.

## What every mode shares

- **Operates on a game project, never the harness.** Games live under `projects/<Name>/` (self-managing, gitignored) — or you may be pointed at an adopted external project. The docs tree is `<project>/docs/architecture/`. Never touch the harness's own `src/`/`docs/` unless explicitly asked. If no project is named and several exist under `projects/`, ask which with `AskUserQuestion`.
- **Derive source roots; hardcode none.** A project has one or more of: **C++** (`<project>/Source/<Module>/` — read `<Name>.uproject` for module names), **Blueprint/content** (`<project>/Content/**`, indexed by asset via `asset_list`/`bp_brief`), **backend/non-engine** (`<project>/src/`, `service/`, `web/`, `tools/` — TS on Bun by convention). Index/annotate/audit whichever exist. A pure-Blueprint game's atlas is an asset map, not a `.cpp` map.
- **Code is ground truth; the docs are the map.** Where they disagree, the doc is wrong — fix it against the code. **Never** write a row, pointer, or claim from memory; read the source, open the asset, or query the live editor first.
- **Markdown-only.** No source edits, no builds, no editor mutation. Every `Write`/`Edit` targets a `.md`. `Bash` is for `ls`/`git log`/`git diff` — reading, never mutation.
- **Inherit the `/architect` standard:** no line numbers, no copy-pasted code bodies; names exactly as they appear on disk; slim and architectural; status vocabulary **Implemented / Partially implemented / Scaffolding / Planned / Design-only**; denormalized living docs.
- **The modes cross-feed.** A fix in one layer often implies a fix in another — an atlas correction may need its INDEX row or reverse pointer updated; an audit finding hands you TAXONOMY corrections; a reverse-pass discovers undocumented systems that become `/architect`'s work queue. Note the hand-offs in your report.
- **Use `/neo4j`** when the project maintains a graph projection and you need cross-cutting structure (interface implementers, execution chains, hardcoded constants) to group, route, or verify correctly.

---

## Mode: `taxonomy` — the forward source atlas

Build the document an agent reads to navigate the game **from the top down**: which systems exist, which files implement them, where each doc lives. Two artifacts under `<project>/docs/architecture/`:

### TAXONOMY.md — the file-by-file atlas

An **atlas, not an encyclopedia.** Every entry is a *pointer with a one-line responsibility*, never an explanation — if a row needs a paragraph, that detail belongs in an architecture doc, and the row should point at it. Recommended shape (a small project needs only the per-folder sections):

1. **Header** — what paths are relative to; conventions (e.g. "headers listed once per class; split `.cpp` satellites noted inline with `→`").
2. **Project Overview** — 2–3 sentences: what the game is, the dominant tech split (all-C++? Blueprint-first? C++ core + BP content? what backend?).
3. **System Map** — a semantic table: Domain → Directories → file count → one-line summary. The 30-second orientation. Derive domains from the real folder structure, not invented categories.
4. **Class Hierarchy** (C++ projects) — the actor/object inheritance tree for gameplay-relevant classes, as an indented code block; name the base every pawn/actor extends and the notable subclasses. For Blueprint-first projects, show BP parent chains or skip.
5. **Component / subsystem inventory** (optional) — the reusable `UActorComponent`/`USubsystem` set and who owns each.
6. **Per-folder sections** — one `## Folder/ — Short Title` per meaningful source folder, each a `| File | What lives here |` table. This is the bulk and the part agents grep.

Rules:
- **Responsibility-focused rows.** A simple file gets one line; a complex or multi-satellite file earns a denser row that captures its *salient specifics* — the key constants/formulas, what governs it, how it's split — so the row orients without the reader opening the file. Dense is fine; **line numbers and copy-pasted code are not** (they go stale instantly). The atlas leans terse, but accuracy and orientation beat a rigid one-liner.
- **The `→` satellite convention.** When one class's `.cpp` is split across satellite files that share a header, list each satellite as its **own** `→`-prefixed row beneath the header's row, and have the header row note the split and name the satellites (`… split across 6 .cpp — _Input / _TickCombat / …`).
- **Every source file appears once** — coverage is the contract; a file absent from the atlas is invisible to top-down navigation.
- **Names exactly as on disk** — it's a lookup table; keys must match for grep.
- **Cross-feed the doc layer** — where a folder is owned by an architecture doc, name it in the relevant rows (`See systems/OBJECT_SYSTEM.md`), making the atlas a launch point into the specs.

### INDEX.md — the doc router

One line per architecture document, grouped by domain subfolder (`gameplay/`, `systems/`, `networking/`, `presentation/`, `services/`, `process/`, …). A **router, not a summary**, carrying **no status tags** — each doc's own status header is authoritative, so the router never drifts when a system ships. Include the maintenance note: "when a new architecture doc is created (`/architect`), add a row here." If the project has no `docs/architecture/` tree yet, create the directory and a minimal `INDEX.md` + `TAXONOMY.md` skeleton; the doc bodies come from `/architect`.

### Research & cadence

Build from ground truth: `Glob`/`ls` the source roots for the real tree (it *is* the System Map skeleton); read `.h` headers for an accurate one-liner (rarely need the `.cpp` body for an atlas row); `class_query`/`class_inspect` or grep `: public A...` for the inheritance tree; counts must match `ls | wc -l`. When **maintaining**, work in small verified passes: pick a section (or the churned files since last pass via `git diff --name-only`), ask *"what would I want to know from a top level of this section?"*, reconcile against disk (no dangling rows, no missing files), keep it slim, stop after one solid improvement.

---

## Mode: `reverse` — backward references from leaves to docs

Place (and keep current) small backward-reference `CLAUDE.md` files at sensible folder levels of the source tree, each pointing *up* to the architecture doc owning that area — the reverse of the atlas. You need the architecture docs already in place to point *to*; if none exist, run `/docs taxonomy` + `/architect` first, or record the gap and stop.

### What a backward-reference CLAUDE.md contains

Keep it **brief** — a pointer, not an essay. The proven shape (match any existing folder-`CLAUDE.md` voice in the project):

1. **Title** — `# Folder/ — Short Descriptive Title`.
2. **Opening paragraph** — one to three sentences naming the logical grouping and what happens here ("the equip/unequip *lifecycle* and everything worn: the equipment manager, the definition + runtime instance objects, …; the *data + ownership* of items lives next door in `Inventory/`").
3. **`**Read first:**` line** — the single primary parent doc by path, with a clause on what it owns: "**Read first:** `docs/architecture/systems/OBJECT_SYSTEM.md` — the parent doc (equipment as part of the unified object system: definitions, instances, slots, transfers)." This is the highest-value line in the file.
4. **`## Backward references`** — a three-column table `| Sub-group | Files | Architecture doc |` when the folder spans several concerns: each row maps a sub-grouping and its files to the doc(s) that govern it. A row may point at more than one doc with a clause ("worn-equipment visual state is **server-published, not client-re-derived** → `networking/REPLICATION.md`").
5. **`## The invariants that govern this folder`** (≤3) — the load-bearing rules an agent must not violate while editing here, **bolded**, at the architectural level, pulled from the owning doc (don't invent them).
6. **"Where the rest lives"** (optional) — a short paragraph if related code sits in sibling folders (the presentation widgets live in `UI/`, the world-map cue in `WorldMap/`…), so the agent doesn't rediscover the spread.

**Purely architectural and structural** — no line references, no code, no implementation steps. Use file/class names exactly as on disk.

### Don't over-document — the placement judgment

Restraint is the skill. A `CLAUDE.md` in *every* folder is noise. Annotate a folder **only if all three are yes**: *Can I describe the logical grouping here? Can I point to a correct, existing doc? Will this genuinely help an agent that lands here?* Prefer one pointer at a grouping's root over many in its subfolders; a folder whose parent already carries a clear pointer usually needs none.

### When no doc owns the folder → the ANNOTATE backlog

A real, substantial system often has **no doc at all** — you can't point at a spec that doesn't exist. Two moves:

- **Write a stopgap `CLAUDE.md`** — title + opening paragraph as usual, then state plainly **"No owning architecture doc yet"** and that the folder is on the `process/ANNOTATE.md` backlog, then route to what *does* exist: the folder's `TAXONOMY.md § <Folder>/` section for the file map, any design notes (e.g. under `docs/todo/…`), and the sibling systems it plugs into with *their* docs ("the strike routes through `Weapons/MeleeSweep` → `systems/COLLISION.md`; minted ore is per-instance-quality inventory data → `systems/OBJECT_SYSTEM.md`"). No `## Backward references` table — there's nothing yet to point a column at.
- **Record the gap** in `docs/architecture/process/ANNOTATE.md` (create if absent) — proposed doc path, code location, current (partial/none) coverage, and a "must answer" list of the questions the real doc needs to address.

This backlog is the reverse pass's most valuable byproduct and **`/architect`'s work queue**: walking from the leaves is how you *discover* what's undocumented.

### Cadence (one folder per pass)

Read `TAXONOMY.md`/`INDEX.md` first (know what docs exist) → pick one unannotated (or stale) meaningful grouping → verify it against code (read enough headers to describe it honestly and confirm which doc owns it) → write the pointer (or stopgap + ANNOTATE row) → fix any staleness you pass (a pointer naming a deleted file / moved doc) → stop and report. **Done** when every substantial grouping has either a pointer or an ANNOTATE row.

---

## Mode: `audit` — verify the docs against the code

The verification loop that finds and kills drift. **A stale document is worse than none.** You need an audit ledger — `docs/architecture/process/STALE_DOCS_LOOP.md` (create if absent): a purpose blurb, a checklist of every reviewable doc (`[ ]` unreviewed / `[x]` verified this cycle / `[~]` reviewed, follow-up deferred), and an **Audit Trail** (newest first). The ledger makes the sweep resumable.

### Process (one document per pass)

1. **Pick an unreviewed doc** from the ledger (or the most likely-stale — `git log`/`git diff` over the code it covers reveals churn). Read it fully; understand its claims.
2. **Open the implementation.** For **every concrete claim** — struct fields, state-machine phases, authority model, file map, default value, integration seam — find the code that proves it true or false. Headers for contracts; `.cpp`/`.ts` for behavior; MCP read tools for visual logic / live composition.
3. **Correct** false claims against code. **Add** anything highly relevant that's missing. **Remove** cruft and instant-falsity (line numbers, pasted code bodies).
4. **Double-check** — re-read the corrected sections against code once more.
5. **Record** — tick the box and append a dated Audit Trail entry with *specifics*: not "updated INVENTORY.md" but *what claim changed and why* ("BoltSpeed 3500→4400, now a fallback; damage is table-authoritative via `ProjectileLaunch::ResolveLaunch`, not a fixed `BaseDamage=65`").
6. **Cross-feed** — if the audit reveals the doc's `INDEX.md` row, its `TAXONOMY.md` rows, or a reverse `CLAUDE.md` are also stale, fix them (or hand to the `taxonomy`/`reverse` modes).

A doc is **reviewed** once checked against code in a pass that ticked its box — not permanent; re-audit when the system changes. A cycle closes when every reviewable doc is audited, then re-opens against what's since changed.

### What counts as verifying a claim

Reading the proof, not recognizing the claim as plausible. A struct claim → open the definition, confirm the fields exist. A behavior claim → find the code path (or prove it doesn't and correct the doc). A file map → reconcile against disk both ways. A **"Design-only/Planned"** claim → check whether it has since *shipped* (the most common drift — promote to Implemented and document the as-built reality). A status word → use the project's vocabulary precisely. A version-dependent UE fact → verify against current Epic docs for the project's exact engine version (`WebSearch`/`WebFetch`). Process docs (`philosophy/`, `process/`) describe intent/workflow — audit for currency of *process*, not against `.cpp`.

---

## Boundaries (all modes)

- **Markdown-only**, on the **target game project**. No source edits, no builds, no editor mutation. If a fix seems to require changing code, that's out of scope — note it and stop.
- **Does not write doc bodies** — that's **`/architect`** (fed by the `reverse` mode's ANNOTATE backlog). This skill indexes them (`taxonomy`), points at them (`reverse`), and verifies them (`audit`).
- **Discipline:** taxonomy/reverse work in one-section/one-folder passes; audit works one-doc-per-pass. Restraint in `reverse` (when in doubt, leave a folder un-annotated) is itself the skill.

## Final report

State which project, which mode, and what changed: the section/folder/doc touched and the verification you did (files read, counts reconciled, claims proven); any ledger/index rows updated; any cross-feed hand-offs owed to the other modes; and any undocumented systems for `/architect` (with the ANNOTATE rows you added).
