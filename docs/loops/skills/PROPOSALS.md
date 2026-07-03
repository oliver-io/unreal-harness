# Skill proposals

Improvements to skill language/process discovered by the skill-test loop
(`skill-test-loop.md`). We do not edit skill prompts directly from the loop — each entry
here is a documented, evidence-backed proposal for a later deliberate edit. Each entry
names the skill, the associated test (if any), the evidence, and the proposed change.

(Note: this file previously held a misplaced copy of the bug fix-loop doc; replaced
2026-07-02 when the skill-test loop first ran.)

## Proposals

(populated by the loop — one section per finding)

### /position — ThirdPerson mesh offset is BP-template-authored, not an ACharacter C++ default (TASK-1)

- **Skill**: `.claude/skills/position/SKILL.md`, §"The default ThirdPerson mesh offset (a UE
  template convention)".
- **Test**: `tests/skills/test_position_conventions.py` (the coordinate-convention battery;
  3/3 green against a live editor 2026-07-02).
- **Evidence** (UE 5.7 source, read this task): the `ACharacter` constructor sets
  `CapsuleComponent->InitCapsuleSize(34.0f, 88.0f)`
  (`Engine/Source/Runtime/Engine/Private/Character.cpp:77`), but the Mesh subobject block
  (`Character.cpp:118-133`) only creates the component, attaches it to the capsule, and sets
  collision/tick flags — it assigns **no** `RelativeLocation`/`RelativeRotation`. The
  `(0,0,-90)` / `Yaw -90` offset the skill describes exists only where a Blueprint template
  (e.g. ThirdPerson `BP_ThirdPersonCharacter`) authors it; `CacheInitialMeshOffset`
  (`Character.cpp:149,191-194`) merely caches whatever the template authored. Confirmed live:
  a bare spawned `/Script/Engine.Character` reads back capsule 34/88, with no mesh offset in
  play (test 2 of the battery).
- **Proposed change**: the skill already labels the offset "a UE template convention", which
  is correct — sharpen it one notch so an agent never expects the offset on a bare
  `ACharacter`: e.g. "This offset is authored in the ThirdPerson **Blueprint template**, NOT
  in `ACharacter`'s C++ constructor (`Character.cpp:118-133` sets no relative offset) — a
  bare spawned Character has mesh offset (0,0,0)/yaw 0."
- **Bonus fixture note for future test authors** (not a skill defect): the engine test mesh
  `/Engine/EngineMeshes/SkeletalCube` has its root bone authored pitched ~+90° (bone-local +X
  points at component +Z), so a root-bone `forward_world` at actor rotation zero is ~(0,0,1),
  not (1,0,0). TASK-1's spec assumed identity authoring; the battery cancels the authored
  pitch with actor pitch −90 before testing yaw handedness. Any future test that treats a
  SkeletalCube bone's forward as the actor forward must do the same.

### /networking — runtime multiplayer claims are STRUCTURALLY untestable in this harness (TASK-2, ledger note)

- **Skill**: `.claude/skills/networking/SKILL.md` + `reference/REPLICATION.md` /
  `reference/AUTHORITY.md` — the core runtime claims (Iris vs legacy behaviour, the CMC
  ServerMove seam, COND_* condition overrides, RPC drop/ordering rules, distance relevancy
  culling, late-join state delivery, prediction/reconciliation).
- **Test**: none — deliberately NOT built. The authoring-side primitive the skill's advice
  funnels into IS now covered (`tests/skills/test_networking_authoring.py`, 2/2 green vs a
  live editor 2026-07-02: `bp_set_class_replication` round-trips
  bReplicates/bAlwaysRelevant/bReplicateMovement/NetCullDistanceSquared onto the class
  defaults and survives recompile).
- **Evidence** (read this task): the harness's only PIE entry point starts a default
  single-process, single-player session — `MCPAutomationCommands.cpp:374-375`:
  `FRequestPlaySessionParams SessionParams; GEditor->RequestPlaySession(SessionParams);`
  (no net mode, no client count, no dedicated-server flag). Every replication claim above
  requires at least two connections (server + remote client) to observe; with one local
  player there is no NetDriver traffic to assert on. Verifying them would mean building
  multi-process net-PIE plumbing plus cross-process observation primitives — out of scope
  for the skill-test loop (and arguably for the harness; see ARCHITECTURE §5 before anyone
  builds it).
- **Proposed change**: none to the skill's content — this is a testability ledger entry so
  future loop passes do not re-litigate it. If the harness ever grows a net-PIE primitive,
  start from the relevancy claim (`bAlwaysRelevant` vs distance culling) since its authoring
  side is already machine-checked.

### /npc_logic — runtime AI-behavior claims deliberately NOT tested (TASK-3, ledger note)

- **Skill**: `.claude/skills/npc_logic/SKILL.md` — the runtime layers: perception event
  timing, pawn-side awareness memory, target stabilization/decay, StateTree runtime
  semantics ("tree orchestrates, C++ thinks").
- **Test**: only the static Layer-3 vocabulary is guarded
  (`tests/skills/test_npc_logic_statetree_taxonomy.py`, 3/3 green vs a live editor
  2026-07-02: task/condition/evaluator node families all discoverable via
  `statetree_list_node_types`, and `statetree_list_schemas` exposes
  `StateTreeAIComponentSchema` distinct from `StateTreeComponentSchema` — engine class
  confirmed at `GameplayStateTree/.../StateTreeAIComponentSchema.h:18`, plugin
  force-enabled by `UnrealMCP.uplugin`).
- **Evidence for stopping there** (read this task): the fixture project has no AI
  content — no AIController subclass, no awareness component, no StateTree assets with
  behavior — so every runtime claim would need game C++ built per-test; and behavioral
  perception assertions (sight gain/loss, stimulus aging, `SetPeripheralVisionAngle`
  effects) are wall-clock/tick-timing dependent, exactly the flakiness class
  `docs/TESTING.md` refuses. Matches the standing DEFERRED entry in TASKS.md.
- **Proposed change**: none to the skill's content — testability ledger entry so future
  loop passes do not re-litigate. If a project ever contributes a real AI fixture (a
  compiled controller + awareness component), start from the awareness-memory claims,
  which are deterministic once perception events are injected rather than sensed.

### /npc_logic — team-affiliation gotcha is source-confirmed CORRECT; credit it (TASK-3)

- **Skill**: `.claude/skills/npc_logic/SKILL.md:67` ("**`NoTeam` vs `NoTeam` resolves to
  *Friendly*, and friendly stimuli get silently dropped** — a classic 'my AI ignores the
  player' bug") and the pitfall recap at `SKILL.md:166`.
- **Test**: n/a (engine-source fact; no fixture AI to observe it on — see the ledger
  entry above).
- **Evidence** (UE 5.7 source, re-verified this task):
  - `Engine/Source/Runtime/AIModule/Private/AIInterfaces.cpp:28-34` —
    `DefaultTeamAttitudeSolver(A, B)` returns `A != B ? Hostile : Friendly`, so two
    agents both at NoTeam (equal IDs) resolve **Friendly**.
  - `Engine/Source/Runtime/AIModule/Classes/GenericTeamAgentInterface.h:32` —
    `NoTeamId = 255` (and `:40`: it is the default-constructed team), plus
    `AIInterfaces.cpp:14-23`: any actor NOT implementing `IGenericTeamAgentInterface`
    reports NoTeam — so "no team model at all" lands every pair on 255 vs 255.
  - `Engine/Source/Runtime/AIModule/Classes/Perception/AIPerceptionTypes.h:217-224` —
    `FAISenseAffiliationFilter` detect flags; `:231-234` `ShouldSenseTeam` gates
    sensing on `1 << GetAttitude(...)` vs the flags, so a Friendly attitude with
    `bDetectFriendlies` unset (the common enemies-only sight config) silently drops
    the target. The skill's causal chain is correct end to end.
  - (Note: the TASK spec cited `AIInterfaces.cpp:29-34` / `AIPerceptionTypes.h:218-224`
    and a NoTeam id "in GenericTeamAgentInterface.h" — all confirmed, with the path
    nuance that both headers live under `AIModule/Classes/`, and
    `GenericTeamAgentInterface.h` is in the AIModule, not Engine/Classes.)
- **Proposed change**: annotate the SKILL.md:67 bullet as verified-against-source, e.g.
  append "(source-confirmed UE 5.7: `AIInterfaces.cpp:30-33` default attitude solver;
  `AIPerceptionTypes.h:231-234` `ShouldSenseTeam`)" — so future audits know this claim
  is load-bearing fact, not folklore, and don't soften it.

### /networking — AUTHORITY.md calls the plugin "Mover (2.0)"; the 5.7 descriptor says VersionName 1.0 (TASK-2)

- **Skill**: `.claude/skills/networking/reference/AUTHORITY.md:103` — "Epic's **Mover (2.0)**
  plugin is the intended successor to the `CharacterMovementComponent`…".
- **Test**: n/a (naming nit; no behaviour to test).
- **Evidence** (read this task):
  `C:\UE5\UnrealEngine-5.7-source\Engine\Plugins\Experimental\Mover\Mover.uplugin` —
  `"VersionName": "1.0"`, `"IsExperimentalVersion": true`, FriendlyName "Mover". "Mover 2.0"
  was Epic's marketing/roadmap name for the successor-to-CMC effort; the shipped 5.7 plugin
  is simply "Mover" at version 1.0, Experimental. An agent grepping the engine for a
  "Mover 2.0" plugin will not find one.
- **Proposed change**: soften the reference to "the **Mover** plugin (Experimental in 5.7)" —
  drop the "(2.0)". The sentence's substance (experimental, long-runway, CMC is the shipping
  path) is correct and stays.

### /ue-expert — STALE: direct field writes to NetUpdateFrequency/MinNetUpdateFrequency are deprecated since 5.5 (TASK-4)

- **Skill**: `.claude/skills/ue-expert/SKILL.md:126` — "**Set replication tuning at
  constructor time (zero per-frame cost):** bucket actors by category and set
  `NetUpdateFrequency`/`MinNetUpdateFrequency`/`NetPriority`/dormancy once."
- **Test**: n/a (C++ authoring guidance; deprecation is a compile-time fact, no runtime
  oracle in this harness).
- **Evidence** (UE 5.7 source, re-verified this task):
  `Engine/Source/Runtime/Engine/Classes/GameFramework/Actor.h:874` —
  `UE_DEPRECATED(5.5, "Public access to NetUpdateFrequency has been deprecated. Use
  SetNetUpdateFrequency() and GetNetUpdateFrequency() instead.")`; same for
  `MinNetUpdateFrequency` at `:879-881`. Setters/getters declared at `:4622-4640`.
  An agent following the skill's wording verbatim writes `NetUpdateFrequency = 2.f;` and
  gets a deprecation warning (or an error under warnings-as-errors). `NetPriority` and
  dormancy remain directly settable — only the two frequency fields moved to setters.
- **Proposed change**: reword SKILL.md:126 to "…set the tuning once:
  `SetNetUpdateFrequency()`/`SetMinNetUpdateFrequency()` (direct field access is
  `UE_DEPRECATED(5.5)`), plus `NetPriority`/dormancy." The advice itself (constructor-time
  bucketing, zero per-frame cost) is correct and stays. The rate-concept mentions of
  `NetUpdateFrequency` at SKILL.md:125/149 are fine as-is — they name the throttle, not an
  access pattern.

### /ue-expert — coordinate/rotation claims are now machine-checked by the /position battery (TASK-4, cross-reference)

- **Skill**: `.claude/skills/ue-expert/SKILL.md` — the coordinate/transform convention
  bullets (forward axis, yaw handedness, transform composition order) that overlap
  `/position`'s jurisdiction.
- **Test**: `tests/skills/test_position_conventions.py` (TASK-1, 3/3 green) pins forward=+X,
  +yaw→+Y handedness, `InitCapsuleSize(34, 88)`, and the LHS-first composition identity
  against a live editor; `tests/integration/test_kinematics.py` covers component-space
  probe/solve math.
- **Proposed change**: where ue-expert restates coordinate/rotation conventions, add a
  pointer that these specific facts are machine-verified by the /position test battery —
  so future audits know which bullets are harness-guarded fact vs advisory prose, and the
  two skills don't drift apart. No content change to the claims themselves (all
  spot-checked correct).
