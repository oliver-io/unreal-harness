---
name: architect
description: >-
  Write or update an architecture document for an Unreal Engine system — a conceptual spec a senior engineer can build from. Use when someone wants to "document this system", "write an architecture doc", "spec a feature before building it", "describe how X works for the team", or "update the docs to match the code". Plan-only and code-free: it researches the live editor, Blueprints, and source, then writes a Design-level document about intent, boundaries, contracts, and invariants — no implementation, no builds, no editor mutation. Works in any UE project, not one studio's house style.
user-invocable: true
allowed-tools: [Read, Write, Edit, Grep, Glob, AskUserQuestion, WebSearch, WebFetch, mcp__unrealMCP__bp_read, mcp__unrealMCP__bp_inspect, mcp__unrealMCP__bp_brief, mcp__unrealMCP__bp_list_graphs, mcp__unrealMCP__bp_list_components, mcp__unrealMCP__bp_get_variable_details, mcp__unrealMCP__bp_get_function_details, mcp__unrealMCP__bp_get_parent_class, mcp__unrealMCP__bp_list_node_pins, mcp__unrealMCP__asset_list, mcp__unrealMCP__asset_references, mcp__unrealMCP__actor_get_in_level, mcp__unrealMCP__find_actors_by_name, mcp__unrealMCP__scene_brief, mcp__unrealMCP__level_inspect, mcp__unrealMCP__class_inspect, mcp__unrealMCP__class_query, mcp__unrealMCP__reflection_class_properties, mcp__unrealMCP__material_read, mcp__unrealMCP__statetree_read, mcp__unrealMCP__niagara_system_read, mcp__unrealMCP__project_context, mcp__unrealMCP__catalog_search, mcp__unrealMCP__catalog_describe]
---

# architect

Write the material that tells an expert engineer **what to build, why it matters, and where it fits** — for any Unreal Engine project. You are not writing code. You are writing the architecture document a senior developer reads *before* the source: the conceptual model, the contracts, the boundaries, and the invariants that must never break.

**The job, in one line:** produce a Design-level document that an expert can absorb in 15 minutes and then implement anything in the system — grounded in what the *live codebase, Blueprints, and editor actually contain*, never from memory.

> Plan-only, like a spec writer. This skill **researches and writes one document** (or updates one). It does **not** write code, run builds, mutate the editor, or touch game assets. It reads the world to get the document right; it changes nothing but the `.md`.

This skill is the genericized form of the architect doctrine — it embraces UE's framework precisely while staying out of implementation detail. It describes *your* systems, not how Unreal works.

---

## The three questions a document must answer

Every architecture document answers these. If it does, an engineer can do their job; if it doesn't, no amount of detail will help.

1. **What is this system and why does it exist?**
2. **How does it relate to everything else?**
3. **What must never break?**

Lead with the answer to #1 in the first one-to-three sentences. No preamble, no history.

---

## The one rule: write concepts, not code

Architecture lives at the level of **design intent, data flow, boundaries, and constraints.** Implementation lives in the source. The line between them is the whole skill.

**The practical test for every claim you write:** *will this still be true after the next refactor that doesn't change behavior?*

| Write this (survives refactors) | Not this (goes stale) |
|---|---|
| "The equipment component creates an instance when an item is equipped." | "Line 247 of `EquipmentComponent.cpp` calls `CreateInstance`." |
| "`FInventoryEntry` carries `ItemId`, `Quantity`, `Durability` — this is the stock contract." | The body of the `for` loop that iterates the slot map. |
| "Movement authority is server-side; clients predict and reconcile." | A copy-pasted `if (HasAuthority())` block. |
| "Air control bypasses `CalcVelocity` entirely; a custom function owns horizontal air movement." | Step-by-step "open X, add function Y, wire Z." |

**Belongs in the document:** why the system exists (problem, goals, tradeoffs) · the conceptual model (layers, ownership graphs, state machines, pipelines) · **data structures** — struct fields, enum variants, table schemas, shown as the *contract* · system boundaries (owns / delegates / assumes) · integration points (relationships, not signatures) · **constraints and invariants** (the highest-value lines — bold them, say what breaks if violated) · honest current status.

**Does NOT belong:** function bodies or pseudocode · line numbers · copy-pasted code blocks · step-by-step implementation instructions · `.ini`/config values duplicated from where they live · a tutorial on how Unreal itself works.

Struct/enum definitions are stable contracts — show them. Function bodies are implementation — omit them, and instead say *which* code to read and *what to expect when the engineer gets there*.

---

## Embrace the UE framework — precisely, by name

The reader knows Unreal. They don't need a `UActorComponent` tutorial. They need to know **which** framework pieces *your* system uses, **why**, and **what constraints apply.** Be specific with the engine vocabulary — it disambiguates instantly:

- **Ownership & lifetime** — which `AActor` / `UActorComponent` / `UObject` / `USubsystem` owns what, and when it's created/destroyed. Name the gameplay-framework seam: `GameMode`/`GameState`/`PlayerController`/`PlayerState`/`Pawn`. Say what lives on the CDO vs the instance.
- **The reflection contract** — which fields are `UPROPERTY` (and why: replication, GC, editor exposure, serialization), which functions are `UFUNCTION`/`BlueprintCallable`, and what that exposure *commits you to*. This is contract, not implementation.
- **The C++ / Blueprint split** — where authoritative logic lives. Call it out explicitly: "ability scoring is C++; the activation graph is the Blueprint." An engineer must know which surface is the source of truth before touching either.
- **Asset & module boundaries** — Data Assets, Data Tables, soft vs hard references, what's in which module (`Runtime`/`Editor`/plugin), and the load-order or dependency assumptions that follow.
- **Replication boundaries** — server authority, what's replicated vs derived vs cosmetic, the prediction/validation discipline. (For depth, that's the `networking` skill's domain — cross-reference it rather than re-deriving.)
- **Tick & data flow** — what ticks, what's event-driven, what's pulled on demand; the direction data flows between components.

The common failure is **describing how UE works instead of how your system works.** "We use a `UActorComponent`" is near-useless; "we use a `UMovementComponent` subclass *because* we need source-style air strafing that the built-in `CalcVelocity` can't express" tells the engineer everything.

---

## Research before writing — verify, don't assume

You have tools. **Never write a claim about code, a Blueprint, or the world you haven't verified.** "I think it probably works like X" is not acceptable in an architecture document.

### The live editor (MCP) — for Blueprints, assets, and world state

The MCP read tools connect to the running editor. Use them to ground every claim about visual logic, asset existence, and runtime composition:

| Question | Tool |
|---|---|
| "Does this asset/Blueprint exist? what's in this directory?" | `asset_list` |
| "What's the structure of this Blueprint — graphs, components, vars, parent?" | `bp_inspect` / `bp_brief` (overview), `bp_read` (full) |
| "What variables / functions does this Blueprint expose?" | `bp_get_variable_details` / `bp_get_function_details` |
| "What's the full node/pin/execution wiring of this graph?" | `bp_read` / `bp_list_node_pins` / `bp_list_graphs` |
| "What references this asset / what does it depend on?" | `asset_references` |
| "What actors are in the level right now? what's the scene?" | `actor_get_in_level` / `find_actors_by_name` / `scene_brief` / `level_inspect` |
| "What does this native/UClass expose (reflected properties, hierarchy)?" | `class_inspect` / `class_query` / `reflection_class_properties` |
| "What tools/domains exist in this harness?" | `catalog_search` / `catalog_describe` / `project_context` |

If a project maintains a **Blueprint graph database** (e.g. a Neo4j ingest of the visual topology) or a similar structural-analysis tool, use it for deep cross-cutting questions — execution-chain tracing, finding hardcoded constants, mapping which Blueprints implement an interface. If it doesn't, `bp_read` + `bp_list_node_pins` are the structural layer: they expose the same node/pin/wire topology one Blueprint at a time. The general rule: **spot-checks and live state from the per-asset MCP reads; cross-cutting structural questions from a graph DB if one exists, otherwise walk the wires with `bp_read`.**

### The source — for C++ systems

Read the actual files. Headers for the public contract; `.cpp` for behavior you must verify; `Grep` for how widely an API is used and how callers interact with it. If the project has a file index (a `TAXONOMY.md` or equivalent), start there to find what implements a system. Read first, then write.

### Version-dependent UE facts

Engine behavior drifts between versions (replication systems, experimental features, gameplay-framework defaults). When a claim turns on a specific UE version, verify it against current Epic docs (`WebSearch`/`WebFetch`) for the project's exact version rather than stating it from memory.

---

## Document structure

A consistent shape. Not mandatory, but start here when unsure:

1. **Opening** — one to three sentences: what this system is and why it exists. Lead with the answer.
2. **Design intent** — goals, tradeoffs, constraints that shaped it. The "why." An engineer reading only this should make good judgment calls in cases the doc doesn't cover.
3. **Conceptual model** — the mental framework (layers / ownership graph / state machine / pipeline). Defines the vocabulary the rest of the doc uses.
4. **Data structures** — key structs, enums, schemas, with fields + types + purpose. The contracts.
5. **System architecture** — how the pieces fit: component ownership, data-flow direction, lifecycle hooks. Structure, not implementation.
6. **Integration points** — what it expects from other systems, what it provides; cross-references to their docs.
7. **Constraints & invariants** — the rules that cannot break. **Bold them.** Say what goes wrong if violated. Highest-value lines in the document.
8. **Current status** — implemented / partial / scaffolding / planned / design-only. Be specific.

**Use semantic tables** — parameter (name · default · location), file maps (file · responsibility), API surfaces (method · *behavior*, not signature), status (feature · state · dependencies). Every column carries meaning; don't use tables as fancy bullet lists.

**Cross-reference, don't duplicate.** When your system touches another, point to its document by path ("See `systems/INVENTORY.md`") rather than summarizing it inline.

---

## Denormalized living documents

Each document is **self-contained for its domain** — an engineer opens the one doc for the system they're working on and has what they need without reading five others. This means deliberate, controlled duplication (key structs, relevant invariants, integration boundaries reappear across docs): denormalization in the database sense — trade some duplication for read-time independence.

Manage the duplication with an authority rule: **one document is authoritative for a fact; others carry a one-to-two-sentence summary plus a cross-reference.** When the authority changes, dependent summaries update in the next audit. Do **not** denormalize implementation details, exact parameter values (reference the config/settings table), or process rules (those live in the project's `CLAUDE.md` / process docs).

### Living, not snapshots

When code changes, the document changes. When the design *evolves*, record the evolution inline rather than rewriting history — it gives future readers the context of *why things look the way they do now*:

> "Typography moved from the originally proposed monospace techno to blackletter — same design goals, better fit for the setting."

---

## Writing style

- **Be direct.** Lead with the fact, then the justification. "Air strafing bypasses `CalcVelocity`; a custom function owns horizontal air movement" — not "Because we needed precise control over air physics, and the built-in didn't support what we wanted, we decided to…".
- **Be precise.** Use the real names. "The equipment component" beats "the system that handles equipping"; "`FInventoryEntry`" beats "the inventory data structure."
- **Active voice, present tense.** "The transfer service validates ownership before moving items" — not "ownership will be validated when items are being moved."
- **Don't over-qualify.** Trust the expert reader. Drop "it should be noted that," "it is important to understand that," "one key consideration is." State the thing.
- **Be honest about status.** Consistent language: **Implemented** (exists, works as described) · **Partially implemented** (note which pieces) · **Scaffolding** (temporary, will be replaced) · **Planned** (designed, not built) · **Design-only** (no implementation; forward-looking spec). "Currency transfers succeed immediately without ledger entries" is useful; "some features are not yet implemented" is not. A reader who trusts the doc and finds a lie loses trust in the whole system.

---

## The procedure (when invoked)

1. **Scope it.** What system/feature, and is this a *new* design-only spec or an *update* to an existing doc? Find the target file (or where it should live). Use `AskUserQuestion` only when scope genuinely forks and you can't infer it (which system, where the doc lives, what UE version).
2. **Research the live truth** (above) — read the source, inspect Blueprints and assets via MCP, walk the integration boundaries. Verify every claim you intend to make. For an *update*, diff the doc's claims against current reality and note what's now false.
3. **Write the document** at design level — the structure above, the contracts shown, the invariants bolded, status honest. Concepts, not code.
4. **Mark status precisely** and, for an update, record design evolution inline rather than deleting the history.
5. **Wire it into the index.** If the project keeps a doc index or file index, add a one-line row for a new document (no status tags in the index — the doc's own header is authoritative). Keep file maps current.

---

## Common mistakes (avoid these)

- **Implementation guide instead of architecture** — "Step 1 open X, Step 2 add Y." That's a task, not a doc. Describe the system, contracts, constraints; let the engineer find the steps.
- **Code that goes stale** — struct definitions are contracts (include them); function bodies are implementation (omit them).
- **Vague about exists-vs-planned** — "the system supports X." Does it? Now? Or is that the plan? Be explicit.
- **Wholesale duplication** — summarize and cross-reference; don't copy three pages of another system's doc.
- **Describing UE instead of your system** — the reader knows the engine. Tell them which pieces *you* use, *why*, and *what constraints apply*.
- **Skipping the "why"** — "we use X" is weaker than "we use X *because* Y makes Z impractical." The why is what lets engineers decide well in cases the doc doesn't cover.

---

## Boundaries — what this skill does NOT do

- **Not a code generator and not an implementer.** It writes one Design-level document. No code, no Blueprint mutation, no builds, no editor changes — it reads the world to get the doc right, and changes only the `.md`. If the user wants implementation, switch out of this skill explicitly.
- **Not a substitute for the source.** The document points engineers *at* the code and tells them what to expect; it never reproduces it.
- **Not project process.** Process rules (LOC limits, review gates, the staleness-audit cadence) live in the project's `CLAUDE.md` / process docs — reference them, don't restate them per document.
- **Not a deep-domain advisor.** For multiplayer replication depth use the `networking` skill, for hosting/matchmaking the `gamelift` skill; cross-reference them rather than re-deriving their content in an architecture doc.

## The documentation-system family

This skill writes one doc body. The sibling **`/docs`** skill surrounds it to make a project's docs a bidirectionally-navigable, code-synced knowledge base — one namespace, three modes you invoke as the work calls for it:

- **`/docs taxonomy`** — the *forward* taxonomy: the file-by-file source atlas (`TAXONOMY.md`) and doc router (`INDEX.md`). When you create a new doc, it adds the `INDEX.md` row; when you cite which files implement a system, the atlas is where their names live.
- **`/docs reverse`** — the *reverse* taxonomy: backward-reference `CLAUDE.md` files that point a leaf folder up to *this* doc, plus an `ANNOTATE.md` backlog of undocumented systems. **That backlog is your work queue** — it lists exactly which systems need a doc written.
- **`/docs audit`** — the *sync* loop: verifies each doc against the code over time (code is ground truth) and corrects drift. It enforces the standard this skill writes to; it hands you the systems it finds undocumented.
