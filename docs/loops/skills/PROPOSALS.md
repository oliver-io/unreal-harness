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
