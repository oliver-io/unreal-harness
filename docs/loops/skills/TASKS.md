# Skill-test loop — task list

Compiled 2026-07-02 from per-skill testability analyses (networking, ue-expert, position,
npc_logic — the engine-guidance skills; external-service skills deliberately out of scope).
Rules of engagement: one item per subagent pass; tests land under `tests/skills/` (add
`skills` to `testpaths` in `tests/pytest.ini` on first use — the root `tests/conftest.py`
applies automatically); commits touch ONLY tests and `PROPOSALS.md`; implementation issues
go to `docs/BUGS.md`, never fixed inline. Every test must be strongly verifiable: arrange
via one typed primitive, observe via a DIFFERENT read primitive, headless-safe, bounded,
self-cleaning.

## TODO — iteration 2 (compiled 2026-07-02 from second-pass analyses: networking,
npc_logic authoring flows, capture-pose, automated-tester)

- **TASK-7 — proposals only, NO test (capture-pose + automated-tester).** Record in
  PROPOSALS.md: (P1) automated-tester SKILL.md:74-76 presents `uassetDiskPath` as an
  `ops.ts` harness export but it is a per-file local helper
  (`animation.test.ts:30`, `asset.test.ts:32`) — an agent following the skill imports
  it and fails; (P2) capture-pose's only rig-validation path is a /visual-critique
  vision verdict because `HandleCaptureFromPose` never echoes the applied
  location/rotation/fov (`MCPAutomationCommands.cpp:1121-1129`) — propose the skill
  lead with the mechanical assertions (status==captured, file bytes>0) and mark the
  framing score advisory; companion observability-gap entry for the orchestrator's
  BUGS.md pass (echo the applied pose in the result). Also ledger the networking
  unauthorable items (below).

## DONE

- **TASK-6 — /npc_logic StateTree Layer-3 placement guard** (2026-07-02):
  `tests/skills/test_npc_logic_statetree_placement.py`, 1/1 green first run vs a live
  editor (`--ue-attach`). Builds the skill's Engage/Searching example under
  `StateTreeAIComponentSchema`; all three placement claims confirmed engine-true and
  harness-faithful (`StateTreeState.h:161,419,422` are three separate arrays;
  `StateTreeNodeMgr.cpp:208-217,236-273` mirrors them; reader oracle
  `MCPStateTreeCommands.cpp:261,333-334,341-357,364-381,467-484` — spec's cited
  anchors re-verified, no deviations). Proposal + schema-context observability
  cross-reference recorded in PROPOSALS.md. No implementation bugs found.

- **TASK-5 — /networking RPC net-type round-trip on a custom event** (2026-07-02):
  `tests/skills/test_networking_rpc_events.py` written; runs **xfailed** vs a live editor
  because the test found a routing bug — `bp_set_event_replication` is missing from
  `FMCPBridge::ExecuteCommand`'s dispatch chain (`MCPBridge.cpp:748-788`; handler wired
  only at `MCPBlueprintCommands.cpp:117` → unreachable, bridge answers "Unknown command"
  `:1187`), for the orchestrator's BUGS.md pass. The baseline half is green (fresh custom
  event reads back with NO `replication` field); the full six-step battery self-unblocks
  when the dispatch line lands. Engine oracles re-verified (`Script.h:142-160,183`;
  `K2Node_CustomEvent.cpp:324-345`). Spec correction: the decoder observer is
  **bp_inspect** (HandleAnalyzeBlueprintGraph, `:2986-3009`), not bp_read. Proposals +
  iteration-2 unauthorable ledger recorded in PROPOSALS.md.

- **TASK-4 — /ue-expert proposals only, NO test** (2026-07-02): analysis recommendation
  NONE for tests confirmed (verifiable core owned by /position + `test_kinematics.py`;
  the rest needs C++ fixtures / net PIE / render verdicts / GC timing). Both proposals
  recorded in PROPOSALS.md: stale direct-field guidance at SKILL.md:126 (deprecation
  re-verified at `Actor.h:874-881`, setters at `:4622-4640`; NetPriority/dormancy remain
  direct), and the /position cross-reference for the coordinate/rotation bullets.

- **TASK-3 — /npc_logic StateTree taxonomy guard** (2026-07-02):
  `tests/skills/test_npc_logic_statetree_taxonomy.py`, 3/3 green vs a live editor
  (`--ue-attach`); task/condition/evaluator families + base_class filter +
  `StateTreeAIComponentSchema` distinct from `StateTreeComponentSchema` all confirmed
  (engine class at `GameplayStateTree/.../StateTreeAIComponentSchema.h:18`; class_name
  strings match the spec exactly). Both proposals recorded in PROPOSALS.md (runtime-AI
  untestability ledger; team-affiliation gotcha source-confirmed — note the cited
  headers live under `AIModule/Classes/`, `NoTeamId = 255` at
  `GenericTeamAgentInterface.h:32`). `ai_get_awareness` CombatAwareness bug left for
  the orchestrator's BUGS.md pass.

- **TASK-2 — /networking bp_set_class_replication parity** (2026-07-02):
  `tests/skills/test_networking_authoring.py`, 2/2 green vs a live editor (`--ue-attach`);
  round-trip observed on a spawned instance via dry-run reflective reads — NOT via
  `reflection_class_properties`/`class_inspect` as specced, because their ResolveClass uses
  `FindFirstObject<UClass>(…, ExactClass)` which cannot resolve a UBlueprintGeneratedClass
  (real observability gap, documented in the loop report). Engine defaults re-verified
  (Actor.cpp:287/:310 — bReplicateMovement defaults False, not True). Both proposals
  recorded in PROPOSALS.md.

- **TASK-1 — /position coordinate-convention battery** (2026-07-02):
  `tests/skills/test_position_conventions.py`, 3/3 green vs a live editor (`--ue-attach`);
  engine oracles re-verified (`Character.cpp:77` InitCapsuleSize(34, 88); Mesh block
  `:118-133` sets no relative offset); proposal recorded in PROPOSALS.md. Fixture caveat
  discovered: SkeletalCube's root bone is authored pitched ~+90°, so the battery cancels it
  with actor pitch −90 before pinning yaw handedness (the spec's "forward ≈ (1,0,0) at
  yaw 0" assumption was wrong for this mesh).

## DEFERRED / NOT TESTABLE (by design — do not revisit without new harness capability)

- **networking (iteration 2 adds)**: RepNotify/`ReplicatedUsing` and dormancy guidance —
  NO authoring primitive exists (`bp_set_variable_properties` writes only CPF_Net +
  ReplicationCondition, `BPVariables.cpp:610-631`; `bp_set_class_replication` has no
  dormancy param) — advisory-only, unauthorable, not a skill defect. The `reliable`
  RPC flag — mutator-echo only, no independent read (`bp_read` decoder omits it,
  `MCPBlueprintCommands.cpp:2996-3008`; compiled-UFunction path BPGC-blocked).
  Variable replication flags via `bp_get_variable_details` — rejected as a weak
  mirror oracle (reads the same struct fields the setter wrote,
  `MCPBlueprintCommands.cpp:3381-3382`).
- **capture-pose**: core "render reproduces the framing" claim is a pixel verdict
  (VERBOTEN); capture-file metadata already covered by
  `tests/integration/test_screenshot.py`; the applied pose is not echoed back by the
  handler (`MCPAutomationCommands.cpp:1121-1129`) so pose application has no non-pixel
  oracle (observability gap noted in PROPOSALS/BUGS); `editor_viewport_get_camera` has
  no typed set-camera arrange counterpart and is GUI-gated.
- **automated-tester**: no engine-behavior surface (claims are repo conventions +
  already-documented GAP-030/031); a static skill-anchor drift guard was considered
  and REJECTED as over-engineering (green-today, tautological, lockstep-maintenance) —
  all cited anchors verified resolving 2026-07-02.
- **npc_logic (iteration 2 adds)**: SKILL.md:89 schema-context claim ("context actor is
  the AIController") is engine-true (`StateTreeAIComponentSchema.h:15,28`) but has NO
  read primitive surfacing schema context/external data — unobservable until
  `statetree_read` (or similar) emits context-data descriptors.

- **networking**: all runtime multiplayer topology claims — no multi-connection PIE.
- **ue-expert**: CDO/constructor traps (no per-test C++ compile), replication/OnRep/CMC
  prediction (no net), ragdoll/GC/PSO/cook (non-deterministic or out of editor scope),
  SceneCapture/render claims (screenshot verdicts require a capture rig; still flaky),
  `FRotator` positional-constructor order (never observable via the named-field MCP path),
  `ECC_Visibility` Pawn=Ignore (no per-channel collision read primitive — building one
  just to check a static engine fact is over-engineering).
- **npc_logic**: the "tree orchestrates, C++ thinks" thesis, awareness/memory layer,
  runtime StateTree semantics, perception timing — need game C++ and/or are timing-flaky.
- **position**: FRotator constructor ordering (source-authoring fact), procedural-mesh
  winding (render-only), mannequin bone claims (fixture has no mannequin content).
