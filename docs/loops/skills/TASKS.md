# Skill-test loop — task list

Compiled 2026-07-02 from per-skill testability analyses (networking, ue-expert, position,
npc_logic — the engine-guidance skills; external-service skills deliberately out of scope).
Rules of engagement: one item per subagent pass; tests land under `tests/skills/` (add
`skills` to `testpaths` in `tests/pytest.ini` on first use — the root `tests/conftest.py`
applies automatically); commits touch ONLY tests and `PROPOSALS.md`; implementation issues
go to `docs/BUGS.md`, never fixed inline. Every test must be strongly verifiable: arrange
via one typed primitive, observe via a DIFFERENT read primitive, headless-safe, bounded,
self-cleaning.

## TODO

### TASK-3 — /npc_logic: StateTree taxonomy guard (effort S, marginal — keep tight)
One headless test `tests/skills/test_npc_logic_statetree_taxonomy.py` asserting the
skill's Layer-3 vocabulary is real in this engine build: `statetree_list_node_types`
returns non-empty task, condition, AND evaluator categories, and `statetree_list_schemas`
exposes `StateTreeAIComponentSchema` distinct from the plain component schema. No PIE.
Proposals to record:
- Runtime AI-behavior claims are unobservable in the fixture: no AI content, and
  behavioral perception tests would be timing-flaky — deliberately not built.
- The skill's team-affiliation gotcha (NoTeam vs NoTeam → Friendly → sight silently
  drops the target) was source-confirmed CORRECT (`AIInterfaces.cpp:29-34`,
  `AIPerceptionTypes.h:218-224`) — credit it in the skill as verified-against-source.
Bug to record in `docs/BUGS.md` (separate commit, no fix): `ai_get_awareness`
(`MCPAIRuntimeCommands.cpp:196-238`) is hardwired to component class names containing
`"CombatAwareness"` — a private-project leak that makes the skill's awareness-layer
advice unobservable for any other project.

### TASK-4 — /ue-expert: proposals only, NO test (effort S)
Recommendation from analysis is NONE for tests (verifiable core already covered by
`test_kinematics.py` and owned by /position; the rest needs C++ fixtures / net PIE /
render verdicts / GC timing — all out of harness reach or inherently flaky).
Proposals to record:
- STALE guidance: skill recommends direct field writes to `NetUpdateFrequency` /
  `MinNetUpdateFrequency`; both are `UE_DEPRECATED(5.5, ...)` with mandated setters
  (`Actor.h:874-881`) — skill should say `SetNetUpdateFrequency()` /
  `SetMinNetUpdateFrequency()`.
- Cross-reference: the coordinate/rotation claims in ue-expert are machine-checked by
  the /position battery (TASK-1) — the skill can point there instead of restating.

## DONE

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
