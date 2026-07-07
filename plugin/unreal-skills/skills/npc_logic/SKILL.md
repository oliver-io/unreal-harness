---
name: npc_logic
description: >-
  Advise on how to architect complicated NPC / creature / enemy AI in Unreal Engine — the layered model (multi-sense perception → pawn-side awareness memory → StateTree decision → action execution → data-asset tuning) and how to turn a behavior spec into that structure. Use when someone wants to "design enemy/creature/companion AI", "structure AI behavior", "how should I use StateTree for AI", "add perception/senses", "build an AI to this spec", "why is my AI flickering between targets", "should this be a StateTree task or a condition", or "how do I keep AI logic out of the tree". Neutral, UE-fact-grounded, design-level advisor — it teaches the architecture and helps you shape it to spec; it is not a creature/game-specific behavior library and not a code generator.
user-invocable: true
allowed-tools: [Read, Write, Edit, Grep, Glob, AskUserQuestion, WebSearch, WebFetch, mcp__unrealMCP__statetree_read, mcp__unrealMCP__statetree_state_list, mcp__unrealMCP__statetree_list_node_types, mcp__unrealMCP__statetree_list_schemas, mcp__unrealMCP__statetree_binding_list_bindable, mcp__unrealMCP__eqs_read, mcp__unrealMCP__eqs_list_types, mcp__unrealMCP__ai_get_state, mcp__unrealMCP__ai_get_perception, mcp__unrealMCP__ai_get_awareness, mcp__unrealMCP__bp_read, mcp__unrealMCP__bp_inspect, mcp__unrealMCP__bp_brief, mcp__unrealMCP__class_inspect, mcp__unrealMCP__class_query, mcp__unrealMCP__reflection_class_properties, mcp__unrealMCP__tag_list, mcp__unrealMCP__catalog_search, mcp__unrealMCP__catalog_describe, mcp__unrealMCP__project_context]
---

# npc_logic

Advise on **how to build complicated NPC AI in Unreal Engine to a spec** — and the architecture that makes "complicated" stay *tractable* instead of collapsing into an unmaintainable mega-tree. This skill teaches one durable shape: a **layered pipeline** with StateTree as the decision framework and the real intelligence in C++ around it. It works whether the spec is a rifle-armed guard, a skittish prey animal, a patrolling sentry, or a companion that follows and assists.

**The job, in one line:** take a behavior description and map it onto the five-layer model below — decide what each layer must hold, where the seams fall, and how the StateTree stays shallow while the behavior gets deep.

> Design-level and neutral. This skill **reasons about and documents architecture** — it helps you choose the layers, the senses, the memory shape, the mode graph, and the data knobs, and it can author/inspect StateTree assets through the MCP when asked. It is **not** a recipe library of pre-built creatures, and it does not hand you a finished behavior — the spec is yours; the structure is what this skill gets right.

---

## The thesis: the tree orchestrates, C++ thinks

The single most important decision in UE AI architecture is **where intelligence lives.** The failure mode that kills AI projects is putting *judgment* — target selection, threat scoring, memory, search logic — inside the decision graph, where it becomes a sprawling, un-debuggable, un-testable tangle of nodes.

Treat Unreal's AI systems as **orchestration and infrastructure, not the home of game logic:**

| Concern | Owner |
|---|---|
| High-level mode flow (which behavior is active, and when it switches) | **StateTree** (asset-backed graph) |
| Sensing the world | **AIPerception** (+ custom senses) on the AIController |
| Memory: what we know, who's a threat, how sure we are | **C++ awareness component** on the pawn |
| Picking a target / scoring a threat / choosing an action | **C++** (functions the tree's tasks invoke) |
| Executing an action (fire, cast, move, flee) | **C++ tasks** → character/component methods or GAS |
| Tunable numbers | **DataAssets / per-task instance data** |

**The guiding rule, bolded because everything else follows from it:**

> **Keep the StateTree shallow and asset-light. Put intelligence into C++ components and the tasks/conditions the tree invokes.** The tree answers "*which* mode am I in?" — never "*how* do I decide who to shoot?"

A good test: if a node would need more than a couple of conditions' worth of branching to express, the logic belongs in C++ behind a single condition or task, not spelled out in the graph.

---

## The five-layer model

Data flows up the stack on sensing and down on action. Each layer has a crisp contract with its neighbours, and **layers never reach past their neighbour** — the decision layer reads *memory*, never raw perception.

```
        ┌─────────────────────────────────────────────┐
  spec  │ 5. TUNING       DataAssets / instance data    │  archetype knobs
   │    ├─────────────────────────────────────────────┤
   │    │ 4. ACTION       C++ tasks → methods / GAS      │  do the thing
   ▼    ├─────────────────────────────────────────────┤
        │ 3. DECISION     StateTree (modes+transitions)  │  which mode?
        ├─────────────────────────────────────────────┤
   ▲    │ 2. AWARENESS    pawn-side memory component      │  what we know
   │    ├─────────────────────────────────────────────┤
 world  │ 1. SENSING      AIPerception + custom senses    │  what we detect
        └─────────────────────────────────────────────┘
```

### Layer 1 — Sensing

`UAIPerceptionComponent` on the **AIController**, configured with one or more sense configs. The richness of an AI's behavior is bounded by how many *distinct* signals it can tell apart, so design senses deliberately:

- **Multiple senses, multiple meanings.** Sight, hearing, and damage are different *facts*, not just different ranges. A spec that says "reacts differently to seeing you vs hearing you vs being shot" is a multi-sense spec — give each its own config and let the awareness layer keep them distinguishable.
- **Tiered sight.** One common, powerful pattern is **two sight configs** on the same listener: a wide, short-range "near sight" that catches anything (even still targets) and a narrow, long-range sight tuned to a different purpose. They coexist because they're configured as separate senses.
- **Custom senses for cheap specialization.** UE lets you subclass a sense (e.g. `UAISense_Sight`) to get a *second independently-tuned channel* of the same modality. The canonical example is **movement-gated far sight**: an otherwise-empty `UAISense_Sight` subclass plus a **producer-side** stimulus-source component that registers an actor as visible-to-far-sight *only while it is moving* (velocity over a threshold, with hysteresis). The expensive LOS/cone work stays in UE's native pipeline; cost scales with the number of *moving* actors, not listeners × actors. Producer-side gating is the general lesson: **filter at the source, not per-listener.**
- **Senses that aren't perception.** Touch/contact is often better as a direct capsule-overlap binding on the pawn that records an instant threat, rather than a perception sense. Don't force everything through `UAIPerceptionComponent`.
- **Teams and attitude.** If you rely on UE's attitude solver (hostile/neutral/friendly), the controller must implement `IGenericTeamAgentInterface` and own a real team ID. **`NoTeam` vs `NoTeam` resolves to *Friendly*, and friendly stimuli get silently dropped** — a classic "my AI ignores the player" bug. State the team model explicitly in any spec with factions.

**Perception fires on the controller.** Its handler filters the stimulus and *writes into the pawn's awareness component*. It may also drive `SetFocus`/`ClearFocus` so control rotation tracks a target. Perception is **event-driven** — handlers run on stimulus, never per-tick polling.

### Layer 2 — Awareness / memory

Pawn-side C++ component(s): **written by the controller, read by StateTree nodes.** This layer is where "complicated" AI actually gets its depth, and it is the layer most often wrongly skipped (people read perception straight from the tree and then wonder why behavior is jittery and unmemorable).

> **StateTree nodes read the pawn's awareness component, never the controller's perception directly.** This indirection is the whole point: memory persists when perception doesn't, and the tree gets a stable, queryable picture instead of a stream of raw events.

Shape the memory to the spec:

- **Single-target memory** (enough for a simple chaser): the tracked actor, last-known location, last-seen/last-lost timestamps, a currently-visible flag. Read API like `HasValidTarget`, `IsTargetCurrentlyVisible`, `HasRecentlyLostTarget`, `GetLastKnownLocation`.
- **Multi-threat memory** (richer reactions): a *set* of perceived threats, each carrying a **per-sense bitmask** of how it's currently sensed. The bitmask matters: it makes classification **additive and commutative**, so an out-of-order event (hearing arriving after sight) can't *downgrade* what you already know. A **primary-threat picker** then scores by sense priority with distance as tie-break — without a stable picker, two equidistant threats cause **ping-pong** (the AI flips targets every frame; a frequent "why is my AI twitching" cause).
- **Graded state, not just boolean.** A 0..100 **wariness/alert meter** that decays on a timer (zero per-frame cost when at rest) lets one component express calm → suspicious → panicked without new modes. Decay belongs on a timer, not a tick.
- **Freshness versioning.** A monotonic `StimulusVersion` lets a long-running task (e.g. an alert "freeze and watch") detect that a *new* stimulus arrived mid-hold and react, without re-entering the state.
- **Server-only.** Awareness/threat memory is gameplay-authoritative state — **never replicated** (see Networking).

### Layer 3 — Decision (StateTree)

`UStateTreeComponent` on the AIController, started in `OnPossess` (`StartLogic`) and stopped in `OnUnPossess`. The asset (state graph + transitions) is authored in the editor / via the MCP and assigned on the controller.

- **A custom schema** subclassing `UStateTreeComponentSchema` sets the **context actor** to your AIController, so `TStateTreeExternalDataHandle<AAIController>` resolves and your custom C++ nodes appear in the editor's node picker. No schema → your nodes don't show up.
- **Conditions gate transitions; tasks own behavior; evaluators compute shared values.** Keep each a thin call into C++: a condition asks the awareness component a yes/no question; a task drives one coherent behavior and reports `Running`/`Succeeded`/`Failed`.
- **Modes stay few and flat.** A capable combat NPC needs only **Idle ↔ Engage ↔ Searching**; a prey animal **Idle/Wander ↔ Alert/Freeze ↔ Flee**. Depth comes from the C++ behind each task, not from twenty nested states.
- **Parallel tasks compose a mode.** "Engage" is often *MoveToTarget ∥ FireAtTarget* running simultaneously — two tasks under one state, not a sub-tree.
- **Instance data is per-node tuning.** Each task/condition carries its own struct (ranges, intervals, radii). This is where small, node-local numbers live.

> **Asset-coupling gotcha — bolded because it silently bites:** after any C++ change to a node's instance-data struct or to the schema types, the StateTree asset must be **re-saved**, or it fails reference validation at load and the tree silently stops working. Re-save (or recompile via the MCP) after structural C++ changes.

### Layer 4 — Action execution

Tasks invoke **action semantics that live in C++**, not in the graph:

- **Direct method calls** for bespoke actions — a `FireAtTarget` task calls the character's `FireWeapon`, which builds the hitscan/projectile params (eye-height origin, control-rotation aim, weapon spread) and executes. Visibility for a "can I shoot?" gate should be **perception-authoritative**, not a hand-rolled `ECC_Visibility` trace (a raw trace passes through non-blocking capsules and lies).
- **GAS** when actions are abilities with costs/cooldowns/tags. Note the seam: you can route *damage/effects* through a Gameplay Effect while still selecting the action with a fixed task — **action execution via GAS does not require action *selection* via GAS.** Escalate to tag-driven ability requests only when the spec has a real menu of interchangeable actions.
- **Direct motion/anim manipulation** (nav movement, anim-rate scaling, IK look-at) for things GAS would only complicate.

**Selection: fixed vs utility.** Start with fixed tasks (one mode → one action). Reach for a **utility/action-selector** (scoring candidate actions by distance, stamina, pressure, vulnerability window…) only when the spec genuinely has *competing* actions whose best choice depends on context. Don't build a utility system for an AI that only ever does one thing per mode.

### Layer 5 — Tuning / data

- **Archetype DataAsset** (`UPrimaryDataAsset`): the single knob-set that makes *one* pawn class behave as many variants by swapping the asset — sense radii/cones, speeds, timing bands, the full alert/flee model, health. Have `GetArchetype()` fall back to the CDO when none is assigned so tasks never nil-check.
- **Per-task instance data** for node-local numbers.
- Keep tunables **out of C++ literals and out of the tree** — they're content, and designers (or you, iterating) change them without recompiling.

---

## Group & spatial intelligence (when the spec needs it)

- **Coordination without O(N²).** For herds/squads, a **world subsystem** (`UTickableWorldSubsystem`, server-only) plus a lightweight per-actor membership component beats every-actor-scans-every-actor. Index members in a **spatial hash** keyed by group tag, re-bucketed at a low rate. Provide group queries (centroid/nearest/count) and **shared per-incident state** — e.g. a single mint-once "flee axis" so a panicking group stampedes *one* direction instead of starbursting. Keep this state world-scoped so PIE restarts and multiple worlds stay clean.
- **EQS for spatial reasoning.** Random nav-mesh points are fine for a basic search/wander. Reach for **EQS** when the spec needs *meaningful* positions — cover, flanking routes, observation points, retreat/guard anchors. EQS is the right tool the moment "where should I stand?" has a quality gradient rather than a yes/no answer. (Inspect/iterate queries with the MCP `eqs_*` tools.)

---

## Networking: server-authoritative, memory private

AI is **server-authoritative**. Perception, StateTree execution, target/threat selection, movement authority, and action resolution all run on the server; a group subsystem should refuse to exist on pure clients. **Awareness/threat memory is server-only and never replicated** — clients receive only the *outcomes*: replicated movement, animation, and combat results. Replicating the AI's private knowledge is both wasteful and a cheat vector. For the replication discipline itself, cross-reference the `networking` skill rather than re-deriving it here.

---

## Performance principles

These are consequences of the architecture, not extra work:

- **Event-driven perception** — handlers fire on stimulus, not per-tick polling.
- **Producer-side gating** — one velocity check per moving actor, not a check per (listener × actor).
- **Timer-driven decay** (alert/wariness meters) — zero cost at rest.
- **Tick throttling during quiescence** — slow the StateTree tick while Idle.
- **Spatial-hash group queries** instead of full-world actor scans.
- **Compact memory** — one tracked target, or a small threat set; don't hoard.

---

## Building to spec — the procedure (when invoked)

1. **Read the spec as layers.** Re-state the behavior, then sort every requirement into the five layers. "Notices you only when you move, freezes to watch, then bolts and warns the herd" decomposes into: movement-gated sense (1) + alert meter & threat set (2) + Idle/Alert/Flee modes (3) + freeze/flee tasks (4) + archetype timing (5) + herd subsystem (group). Surface the layer that's doing the heavy lifting — usually awareness.
2. **Inspect the live AI if one exists.** Ground claims in reality: `bp_inspect`/`bp_read` the controller and pawn, `statetree_read` / `statetree_state_list` the current tree, `statetree_list_schemas` / `statetree_list_node_types` to see available nodes, `ai_get_state` / `ai_get_perception` / `ai_get_awareness` for runtime introspection, `class_inspect` / `reflection_class_properties` for native contracts. Verify; don't assume.
3. **Decide each layer's contract before any nodes:**
   - **Senses:** which signals, what each *means*, team model, what's producer-gated.
   - **Memory:** single-target vs threat-set; what fields; is there a graded meter; what does the tree get to ask.
   - **Modes:** the minimal mode graph and the transitions between them (which condition drives each edge).
   - **Tasks/conditions:** the thin C++ surface each mode invokes, and what `Running/Succeeded/Failed` mean.
   - **Data:** archetype knobs vs per-task instance data.
4. **Shape the StateTree** — shallow, parallel-tasks-per-mode, conditions on edges. When authoring through the harness, the `statetree_*` MCP family creates the asset, adds states/nodes/transitions, binds properties, and **compiles** it; re-save/recompile after any backing C++ struct change (the coupling gotcha above).
5. **Leave the intelligence in C++.** If a piece of judgment crept into the graph, pull it back behind a single condition/task. Re-check against the bolded thesis.
6. **Verify behavior, not just structure.** Drive it in PIE and observe via a *different* read path than the one that controls it (`ai_get_*`, screenshots) — the `automated-tester` skill formalizes this if a standing test is wanted.

Use `AskUserQuestion` only when the spec genuinely forks (e.g. single-target vs multi-threat, fixed vs utility selection, solo vs group) and you can't infer the intent.

---

## Anti-patterns (the things that make AI un-maintainable)

- **Logic in the tree.** Target selection, threat scoring, or search math expressed as graph branching. → Behind one C++ node.
- **Reading perception from the decision layer.** Skips memory; produces jittery, amnesiac AI. → Always go through the awareness component.
- **No primary-target stabilization.** Equidistant threats → frame-by-frame target ping-pong. → Priority + tie-break picker.
- **Downgradable classification.** Overwriting "seen" with "heard" because events arrived out of order. → Additive per-sense bitmask.
- **One mega-mode / deeply nested states.** Depth in the graph instead of in C++. → Few flat modes, parallel tasks, deep C++.
- **`NoTeam` faction bugs.** Friendly-by-default stimuli silently dropped. → Real team IDs + `IGenericTeamAgentInterface`.
- **Hand-rolled visibility traces for combat gates.** `ECC_Visibility` passes through non-blocking capsules. → Trust the perception system's "currently sensed."
- **Per-tick polling and full-world scans.** → Event-driven perception, timers, spatial hashes.
- **Forgetting the asset re-save** after a node struct/schema change → silent load-time validation failure.
- **Replicating AI memory.** Wasteful and exploitable. → Server-only; replicate outcomes.
- **Premature GAS/utility/EQS.** Building the heavy machinery before the spec needs interchangeable actions or graded positions. → Add them against a named requirement, not speculatively.

---

## Boundaries — what this skill does NOT do

- **Not a creature/behavior library.** It teaches the *architecture* and helps shape it to a spec; it does not ship pre-built deer/zombie/guard behaviors. Compose the behavior from the layers.
- **Not a code generator.** It reasons at design level and can author/inspect StateTree *assets* via the MCP when asked, but it does not write your C++ tasks, components, or senses for you — those are implementation. If you want implementation, switch out of this skill explicitly.
- **Not a deep-domain replacement.** For replication depth use `networking`; for standing tests use `automated-tester`; for a written architecture document use `architect`; for EQS/StateTree tool mechanics see the harness tool contracts (`docs/USAGE.md` in the harness repo). Cross-reference, don't re-derive.
- **Not a substitute for the engine docs.** When a claim turns on a specific UE version's StateTree/Perception behavior, verify against current Epic docs for that version rather than asserting from memory.
