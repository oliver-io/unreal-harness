# Skill-test loop — task list

Compiled 2026-07-02 from per-skill testability analyses (networking, ue-expert, position,
npc_logic — the engine-guidance skills; external-service skills deliberately out of scope).
Rules of engagement: one item per subagent pass; tests land under `tests/skills/` (add
`skills` to `testpaths` in `tests/pytest.ini` on first use — the root `tests/conftest.py`
applies automatically); commits touch ONLY tests and `PROPOSALS.md`; implementation issues
go to `docs/BUGS.md`, never fixed inline. Every test must be strongly verifiable: arrange
via one typed primitive, observe via a DIFFERENT read primitive, headless-safe, bounded,
self-cleaning.

## TODO — iteration 3 (opened 2026-07-03): PERCEPTUAL VALIDATION category

Iteration 2 verified complete & non-stale 2026-07-03 (bp_set_event_replication still
undispatched in MCPBridge.cpp → TASK-5 xfail valid; pose still un-echoed in
HandleCaptureFromPose → capture-pose deferral valid). All 5 iteration-1/2 test files
present in tests/skills/.

New test category — **perceptual integration tests**: arrange deterministically via typed
primitives, capture via a fixed rig (zero stateful navigation, per VERBOTEN rule 3), then
validate a *semantic claim* by asking the Gemini critic (/visual-critique machinery) a
narrow, binary-decidable question — optionally cross-checked by a second look (Claude
reading the same image). This does NOT overturn prior pixel-verdict deferrals; it adds the
missing verifier. Candidate seeds (scoped by the 2026-07-03 analysis pass): directional
semantics (IK/pose: raise the RIGHT arm skyward → critic states which arm is raised),
texture qualities (author a procedural wood-grain texture → render on a lit mesh → critic
confirms grain).

Category plumbing facts (from the 2026-07-03 scoping pass — reuse, don't rediscover):
- Critic recipe: POST `generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash:generateContent`,
  header `x-goog-api-key`, image as `inline_data` base64 PNG, `responseMimeType:"application/json"`,
  temperature 0, PIN the model (no fallback in tests). Copy mechanics from
  `.claude/skills/visual-critique/critique.ts`; key resolution accepts BOTH
  `GEMINI_API_KEY` and `GOOGLE_STUDIO_API_KEY` (env or repo `.env`, cf.
  `.claude/hooks/gemini-cred-gate.py` `key_configured()`).
- Gating: `@pytest.mark.render` (auto-skipped unless `--ue-mode=gui`, tests/conftest.py)
  PLUS a new key-skipif. Keep calls ≤~3/test; majority vote only if a question proves flaky.
- Capture rig: `pie_capture_from_pose` with explicit computed location/rotation/fov is the
  sanctioned fixed rig (real game render path; editor viewport has NO typed camera-set —
  `editor_focus_actor` exists in C++ `MCPAutomationCommands.cpp:84` but is not surfaced).
- Anti-rubber-stamp rule: every perceptual assertion ships a differential/control arm
  (a frame where the expected answer is the OPPOSITE) so an always-agreeable critic fails.
- `mesh_set_mesh_material_color` is DISABLED (GAP-009); color actors via
  `material_create_instance` → `material_instance_set_parameter` → `material_apply_to_actor`.
- PIE is leased: treat `pie_busy`+queue as retry, `finally: pie_stop`, bounded polls.

(iteration 3 complete 2026-07-03 — all four tasks landed; outcomes appended per entry)

- [x] **TASK-8 — vision-critic test helper (prerequisite).** Add
  `tests/harness/vision_critic.py`: `ask(image_path, question) -> bool` forcing a JSON
  `{answer: true|false, confidence}` reply per the recipe above, plus `requires_gemini`
  skipif + a `perceptual` marker registered in `pytest_configure`, plus a no-network
  guard test (missing key → clean skip/False path) modeled on
  `src/server/test/video_analyze.test.ts`. No engine needed. Commit: tests only.
  **DONE 2026-07-03** — commit `c7faf74`; 15/15 passed incl. one live Gemini smoke
  (red/blue square, control arm). Later hardened in `a404265` (balanced-brace JSON
  extraction, bigger output budget) after live Gemini returned prose-wrapped JSON.
  Proposals: env-var naming split (GOOGLE_STUDIO_API_KEY vs GEMINI_API_KEY);
  gemini-cred-gate.py `startswith` matches `GOOGLE_STUDIO_API_KEY_NAME=` (footgun).

- [x] **TASK-9 — /position directional semantics (perceptual).**
  `tests/skills/test_position_perceptual_directions.py`. Design A: reference actor at
  origin facing +X; RED cube at (0,+150,50), BLUE at (0,−150,50) (material-instance
  coloring); `pie_start` → `pie_capture_from_pose(location=(−450,0,120), pitch −8, yaw 0,
  fov 90)`; critic: "Is the RED cube on the RIGHT side of the image?" YES + blue/left YES
  (sign flip fails both). Design B (same file): red cube high (200,0,250) vs blue low
  (200,0,20), level camera, "Is the red cube ABOVE the blue?" Validates SKILL.md §1.1
  +Y=right / +Z=up. NOTE: the literal "IK raise the right arm" is NOT buildable — no
  persistent live-pose primitive (`kinematics_probe` live mode applies-and-restores,
  `MCPKinematicsCommands_Probe.cpp:277`) and no mannequin in the fixture; record that
  primitive gap in PROPOSALS.md. Cleanup: delete actors + `/Game/__MCPTest__` materials.
  **DONE 2026-07-03** — commit `a404265`; 2/2 passed vs a live GUI editor. RIG
  DEVIATION: `pie_capture_from_pose` found broken (pixels don't track the pose —
  filed in docs/BUGS.md); replaced with the editor-viewport rig (`level_new` from
  Template_Default + py-hatch `set_level_viewport_camera_info` verified via
  `editor_viewport_get_camera` + `editor_screenshot mode=viewport`), no PIE at all.
  Also found: fixture startup map renders black under GUI (use an engine template).

- [x] **TASK-10 — /texture + material authoring perceptual quality.**
  `tests/skills/test_texture_perceptual.py`. UNLIT checkerboard material (Custom-HLSL
  frac parity node, or pure-node stripe chain TextureCoordinate→Multiply→ComponentMask→
  Frac→If) vs an unlit solid-gray control, applied to cubes, same fixed rig; critic pair:
  "checkerboard/striped pattern?" YES on the patterned actor, "single solid color, no
  pattern?" YES on the control. Optional secondary: stripe orientation H vs V (exercises
  ComponentMask U/V). Unlit sidesteps the editor-vs-PIE brightness trap. Wood-grain
  realism REJECTED (aesthetic). Watch the `material_set_expression_property` r-key
  footgun (g-only payload = silent no-op). Cleanup: actors + material assets.
  **DONE 2026-07-03** — commit `9e280fd`; 3/3 green on the first full run (unlit
  Custom-HLSL checkerboard / solid-gray control / pure-node stripe chain, 6 Gemini
  calls, editor-viewport rig, no PIE). Proposals: unlit/emissive preview variant for
  /texture; Custom-HLSL named inputs wireable in material_connect; creation-time
  r/g/b as the safe color pattern; rig helpers now duplicated → tests/harness
  promotion candidate.

- [x] **TASK-11 — /capture-pose framing differential (closes iteration-2 gap).**
  `tests/skills/test_capture_pose_framing.py`. One vivid solid primitive at a known
  location; `pie_capture_from_pose` twice — pose A aimed at it (critic: CENTERED),
  pose B same location yawed ~90° away (critic: ABSENT/EDGE). Proves the pose parameter
  is honored — the non-pixel oracle iteration 2 said didn't exist (`HandleCaptureFromPose`
  echoes no pose). Does not require the BUGS.md pose-echo fix. Cleanup as above.
  **DONE 2026-07-03 (re-specced)** — commit `cfdc2a8`; `1 passed, 1 xfailed`. Green
  half proves pose→pixels tracking via the editor-viewport rig (CENTERED vs
  ABSENT/EDGE differential + verdicts-must-differ). PIE half runs the same
  differential through `pie_capture_from_pose` and xfails (strict=False) on the
  open BUGS.md capture defect — XPASS is the closure signal; flip to strict then.

## DONE

- **TASK-7 — proposals only, NO test (capture-pose + automated-tester)** (2026-07-02):
  both claims re-verified by the orchestrator before recording. (P1) was a FALSE
  POSITIVE — automated-tester SKILL.md:74-77 already disclaims `uassetDiskPath` as a
  file-local helper to copy from `asset.test.ts`; recorded in PROPOSALS.md as a
  verified non-issue so it isn't re-flagged. (P2) confirmed — `HandleCaptureFromPose`
  echoes no pose (`MCPAutomationCommands.cpp:1121-1129`, and note `status` is
  `"requested"`, not `"captured"` as the analysis assumed); mechanical-first wording
  proposal recorded, observability gap filed in docs/BUGS.md by the orchestrator.

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

- **iteration 3 adds (2026-07-03)**: literal "IK-pose a character's right arm skyward" —
  no persistent live-pose primitive (kinematics probe restores within the call) and no
  humanoid in the fixture; colored-marker surrogate (TASK-9) validates the same
  left/right/up semantics. Wood-grain / realism judgments — aesthetic, high-variance
  critic verdicts. /icon (external GPT image gen), /gimp-import (needs GIMP + authored
  .xcf), /progress-video (corpus mining, not rig-able), /key-indicator-helper (fixture
  lacks keycap content + C++ widget fixture), /see (self-verifying pure-CPU, no rig
  applies), /visual-critique itself (it IS the oracle machinery).

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
