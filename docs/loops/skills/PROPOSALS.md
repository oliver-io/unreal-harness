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

### /networking — RPC net-type claim is source-confirmed CORRECT, but the authoring primitive it funnels into is UNREACHABLE (TASK-5)

- **Skill**: `.claude/skills/networking/reference/REPLICATION.md:71,108`,
  `reference/AUTHORITY.md:29-33` — a Blueprint RPC is a custom event whose net type is one
  of Run-on-Server / Run-on-Client / Multicast, exactly one at a time, and the type governs
  routing.
- **Test**: `tests/skills/test_networking_rpc_events.py` (currently **xfails** — see the
  bug below; 1 xfailed vs a live editor 2026-07-02, with the baseline half green: a fresh
  `bp_add_custom_event` node reads back via the `bp_inspect` decoder with NO `replication`
  field, i.e. GetNetFlags()==0).
- **Evidence — the claim itself is engine-true** (UE 5.7 source, read this task):
  - `CoreUObject/Public/UObject/Script.h:142` `FUNC_Net = 0x40`, `:143`
    `FUNC_NetReliable = 0x80`, `:150` `FUNC_NetMulticast = 0x4000`, `:157`
    `FUNC_NetServer = 0x200000`, `:160` `FUNC_NetClient = 0x1000000`; `:183`
    `FUNC_NetFuncFlags = FUNC_Net|FUNC_NetReliable|FUNC_NetServer|FUNC_NetClient|FUNC_NetMulticast`.
  - `Editor/BlueprintGraph/Private/K2Node_CustomEvent.cpp:324-345` —
    `GetNetFlags() = FunctionFlags & FUNC_NetFuncFlags` (parent flags win only for
    overrides; a fresh custom event carries exactly what the author set).
  - Harness mutator mirrors the editor faithfully: `MCPBlueprintCommands.cpp:1583-1590`
    dropdown→bit map, `:1645-1654` clear-then-OR (clears all five net bits, ORs
    `FUNC_Net` + the ONE chosen specifier) — so the skill's "exactly one type" claim is
    structurally enforced. Decoder at `:2986-3009` re-derives the type from GetNetFlags(),
    field absent when 0 — a genuinely independent oracle.
- **BUG discovered (for the orchestrator's BUGS.md pass — not fixed here)**: the wire
  command `bp_set_event_replication` is **not routed**. The handler exists and is wired
  inside `FMCPBlueprintCommands::HandleCommand` (`MCPBlueprintCommands.cpp:117` →
  `HandleSetEventReplication` `:1551`), the server registers the tool
  (`domains/bp.ts:429`), and it sits in the PIE blocklist (`MCPCommonUtils.cpp:214`) —
  but `FMCPBridge::ExecuteCommand`'s Blueprint dispatch chain (`MCPBridge.cpp:748-788`)
  omits it (`bp_set_class_replication` is there at `:753`; `bp_set_event_replication`
  appears nowhere in MCPBridge.cpp), so the bridge falls through to
  `Unknown command` (`:1187`). Handler = unreachable dead code; the tool has never worked
  end-to-end. The test xfails on exactly this signature and will run its full six-step
  battery green the moment the dispatch line lands (no test change needed).
- **Spec deviation (task-spec correction, not a skill defect)**: TASKS.md TASK-5 named
  `bp_read include_node_details:true` as the observer, but the replication decoder lives
  in `HandleAnalyzeBlueprintGraph` — the **`bp_inspect`** wire command
  (`MCPBlueprintCommands.cpp:171-174`); `bp_read` (`HandleReadBlueprintContent`,
  `:2549-2561`) emits only name/class/title per event-graph node. The test observes via
  `bp_inspect`.
- **Proposed change**: none to the skill's content — the RPC-type teaching is correct and
  (once the routing bug is fixed) machine-checked. Consider a server-side parity guard
  (every registered bridgeTool command answered by the C++ router) so a dead-dispatch tool
  can't ship silently again.

### /networking — iteration-2 unauthorable ledger (TASK-5, ledger note)

- RepNotify/`ReplicatedUsing` and dormancy guidance: NO authoring primitive exists
  (`bp_set_variable_properties` writes only CPF_Net + ReplicationCondition,
  `BPVariables.cpp:610-631`; `bp_set_class_replication` has no dormancy param) —
  advisory-only, not a skill defect. The `reliable` RPC flag: mutator-echo only, no
  independent read (`bp_inspect`'s decoder omits it, `MCPBlueprintCommands.cpp:2996-3008`)
  — deliberately NOT asserted by `test_networking_rpc_events.py`. Both match the standing
  DEFERRED entries in TASKS.md ("networking (iteration 2 adds)"); recorded here so future
  passes do not re-litigate.

### /npc_logic — Layer-3 placement claims are source-confirmed CORRECT and now machine-checked (TASK-6)

- **Skill**: `.claude/skills/npc_logic/SKILL.md:89-92` — the Layer-3 placement claims:
  the AI component schema is the decision layer's schema (:89); "Conditions gate
  transitions; tasks own behavior" (:90); "Parallel tasks compose a mode … two tasks
  under one state, not a sub-tree" (:92).
- **Test**: `tests/skills/test_npc_logic_statetree_placement.py` — 1/1 green vs a live
  editor (`--ue-attach`, 2026-07-02). Builds the skill's own Engage/Searching example
  under `StateTreeAIComponentSchema`: two task nodes flat on Engage, an OnTick
  Engage→Searching transition with a condition ON the transition; observes via the
  independent `statetree_read` serializer (schema_class round-trip, both task GUIDs in
  Engage's `tasks[]` with `child_count==0`/`depth==0`, condition GUID in the
  transition's `conditions[]` and — negatively — in neither `tasks[]` nor
  `enter_conditions[]`). Deliberately not compiled (compile covered by
  `test_statetree.py::test_compile_verify_and_save`).
- **Evidence — the placement is the engine's own data model** (UE 5.7 source, read
  this task): `StateTree/Source/StateTreeEditorModule/Public/StateTreeState.h:422`
  `UStateTreeState::Tasks`, `:419` `EnterConditions`, `:161`
  `FStateTreeTransition::Conditions` — three separate `TArray<FStateTreeEditorNode>`,
  so a condition that "gates a transition" is a different storage location, not a
  convention. `GameplayStateTree/.../StateTreeAIComponentSchema.h:18-19`
  (`UStateTreeAIComponentSchema : public UStateTreeComponentSchema`), class comment
  `:14-16` ("guarantees access to an AIController…"). Harness mirrors it faithfully:
  `StateTreeNodeMgr.cpp:118-124` slot vocabulary; `:208-217` task→`State->Tasks`,
  enter_condition→`State->EnterConditions`; `:236-273` condition slot requires a
  transition GUID and inserts into `Trans.Conditions` — structurally impossible to
  misplace. Reader: `MCPStateTreeCommands.cpp:261` (schema_class), `:341-357`
  (tasks), `:364-381` (enter_conditions), `:333-334` (depth/child_count),
  `:467-484` (per-transition conditions).
- **Proposed change**: none to the skill's content — the Layer-3 placement teaching is
  engine-true and now harness-guarded. One annotation suggestion: mark SKILL.md:90/92
  as machine-verified by this battery so future audits know these bullets are guarded
  fact. **Observability caveat (cross-reference)**: the :89 schema-context claim
  ("context actor is the AIController") remains engine-true but UNOBSERVABLE — the
  read primitive emits only the schema class name, not its context-data descriptors;
  see the standing "npc_logic (iteration 2 adds)" DEFERRED entry in TASKS.md. If
  `statetree_read` ever emits context-data descriptors, extend this battery rather
  than writing a new one.

### /automated-tester — `uassetDiskPath` wording is ALREADY accurate; no change (TASK-7, verified non-issue)

- **Skill**: `.claude/skills/automated-tester/SKILL.md:74-77`.
- **Test**: none (proposals-only task).
- **Evidence** (orchestrator re-verified 2026-07-02): the iteration-2 analysis flagged that
  the skill presents `uassetDiskPath` as a `test/harness/ops.ts` export, but the current
  SKILL.md text already carries the exact disclaimer: "(`uassetDiskPath` — a file-local
  helper, not part of `test/harness/ops.ts`; copy it from `test/integration/asset.test.ts`)".
  The helper is indeed per-file (grep: `animation.test.ts:30`, `asset.test.ts:32`,
  `blueprint.test.ts:24`, `data.test.ts:26`, `eqs.test.ts:28`, …) and the skill routes the
  reader to copy it — accurate as written.
- **Proposed change**: none. Recorded so future passes don't re-flag it. (A code de-dup —
  promoting the helper into `ops.ts` and updating the skill to match — would be a normal
  refactor deliverable, but it is out of band for this loop and not a skill defect.)

### /capture-pose — rig validation leans entirely on a vision verdict; lead with the mechanical assertions (TASK-7)

- **Skill**: `.claude/skills/capture-pose/SKILL.md` — the "confirm the rig works" step
  validates the capture via a `/visual-critique` framing score (SKILL.md:64-74), a vision
  judgment the repo's own VERBOTEN rule treats as the weakest evidence class.
- **Test**: none — deliberately NOT built. The core "the render reproduces the framing"
  claim is a pixel verdict (VERBOTEN without a rig, flaky even with one — standing
  DEFERRED entry), and capture-file metadata is already covered by
  `tests/integration/test_screenshot.py`.
- **Evidence** (orchestrator re-verified 2026-07-02): `HandleCaptureFromPose` returns only
  `file_path`, `path`, `status:"requested"`, `restored`, `message`
  (`MCPAutomationCommands.cpp:1121-1129`) — the applied `location`/`rotation`/`fov`/
  `aspect` are never echoed back. So there is NO non-pixel oracle that the pose was
  applied; the only validation path the skill can offer today is the vision score.
- **Proposed change (language)**: have the skill assert the rig's *mechanical* success
  deterministically FIRST — call succeeded, output file exists with `bytes > 0` (the
  bridge confirms the file server-side before returning) — and explicitly frame the
  `/visual-critique` framing score as an advisory layer on top, not the proof the rig
  works. **Companion observability gap filed in `docs/BUGS.md`** (orchestrator pass): if
  the handler echoed the applied pose in its result, pose *application* (not framing)
  would become a metadata assertion and capture-pose's core claim would gain a
  deterministic guard. Do not fix inline.

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

### Iteration 3 (2026-07-03) — perceptual verifier plumbing

### /visual-critique + gemini-cred-gate — Gemini key env-var naming is inconsistent across the repo (TASK-8)

- **Skill/machinery**: `.claude/skills/visual-critique/critique.ts` `loadKey()` and
  `.claude/hooks/gemini-cred-gate.py` `key_configured()` accept ONLY
  `GOOGLE_STUDIO_API_KEY`; the Bun server prefers `GEMINI_API_KEY` and merely tolerates
  the studio spelling (`src/server/src/config.ts:119,126`), and server-side user-facing
  messages name only `GEMINI_API_KEY` (`domains/pie.ts:807`, `domains/video.ts:114`;
  `video/analyzer.ts:199` at least names both). The repo `.env` template stores the
  studio spelling. An operator who follows the server's error message and sets
  `GEMINI_API_KEY` still gets denied by the cred gate; one who follows the hook sets a
  key the server docs never mention.
- **Test**: `tests/skills/test_vision_critic_helper.py` (TASK-8 guard suite, 15/15 green
  headless, no editor). The new helper `tests/harness/vision_critic.py` deliberately
  accepts BOTH spellings so tests don't inherit the split.
- **Proposed change (language/process)**: pick `GEMINI_API_KEY` as canonical (matches the
  server) and have `critique.ts` `loadKey()` and the cred-gate `key_configured()` accept
  it alongside the legacy studio name; update the gate's denial message to name both.
  No behavior change beyond acceptance.
- **Second finding (hook parsing footgun)**: `gemini-cred-gate.py:46` matches `.env`
  lines with `startswith("GOOGLE_STUDIO_API_KEY")`, so `GOOGLE_STUDIO_API_KEY_NAME=x`
  (present in the real repo `.env`) satisfies the gate even if the actual key line is
  missing — the gate can wave a call through to a guaranteed runtime failure. The
  helper's guard test `test_gemini_key_exact_name_only` pins the exact-name behavior;
  propose the hook split on `=` and compare the exact variable name.
- **Note for TASK-9/10/11 authors**: the helper pins `gemini-3.5-flash` with NO fallback
  (unlike critique.ts's 404→pro fallback) and temperature 0, and every `ask()` assertion
  must ship a control arm with the opposite expected answer (anti-rubber-stamp rule,
  enforced by convention — see the module docstring).

### /position — directional semantics now perceptually machine-checked; the literal IK arm-raise is a PRIMITIVE GAP (TASK-9, iteration 3, 2026-07-03)

- **Skill**: `.claude/skills/position/SKILL.md` §1.1 — the perceptual consequences of the
  coordinate conventions: with forward=+X and +Z up, world +Y renders screen-RIGHT of a
  +X-facing viewpoint, and +Z renders UP.
- **Test**: `tests/skills/test_position_perceptual_directions.py` — colored-marker
  surrogate (RED/BLUE emissive cubes via `material_create_instance` →
  `material_instance_set_parameter` → `material_apply_to_actor`; the direct
  `mesh_set_mesh_material_color` path is feature_disabled per GAP-009), two fixed-rig
  editor-viewport frames, Gemini critic verdicts with mirrored control questions
  (5 calls/run). Design A pins +Y=screen-right/−Y=left; Design B pins +Z=up. 2/2 green
  vs a live GUI editor (attach) AND via the full `tests/run.ps1 --ue-mode=gui` launch.
- **Rig deviation from the TASK-9 spec (deliberate, evidence-backed)**: the spec named
  `pie_capture_from_pose` as the fixed rig, but live debugging showed its pixels do NOT
  track the requested pose (bug below). The battery instead uses: `level_new` from
  `/Engine/Maps/Templates/Template_Default` (the fixture's default map is an EMPTY unlit
  Untitled world that renders **pure black** — no perceptual question is decidable on it),
  the `py` hatch `UnrealEditorSubsystem().set_level_viewport_camera_info(...)` for an
  explicit computed pose (verified through the independent `editor_viewport_get_camera`
  read), and `editor_screenshot mode=viewport` (FScreenshotRequest through the real render
  pipeline; bridge-confirmed on disk per GAP-007; immune to window occlusion). No PIE.
- **BUG discovered (for the orchestrator's BUGS.md pass — not fixed here)**:
  `pie_capture_from_pose` writes frames that do not reflect the requested pose. Observed
  live (GUI editor, PIE running, 2026-07-03): requesting poses (−450,0,120)/pitch−8,
  (−300,0,150)/pitch−15, (0,0,300)/pitch−90/yaw180, and (−150,150,60)/pitch0 — the last
  one 100 units in front of a 100-unit emissive red cube — all returned the SAME
  fixed-viewpoint frame (origin-ish, rotation-zero; the close-up cube absent; one capture
  showed a TAA smear of that same view). The view-target swap itself works —
  `pie_query` showed `PlayerCameraManager_0` AT the requested pose, and an auto-activated
  `CameraActor` rig reproduced the same wrong pixels — so the defect is in the capture
  path (`MCPCaptureGameViewportToFile`, `MCPAutomationCommands.cpp:658`: PrintWindow of
  `GEngine->GameViewport`'s OS window ~3 ticks after the swap), most likely PrintWindow
  returning stale/non-recomposited content for an occluded PIE window rather than the
  swapped view. Note `tests/integration/test_pie.py`'s capture test only asserts the file
  exists, so this was never caught; TASK-11 (framing differential) will hit this wall as
  specced — it needs this bug fixed first, or the editor-viewport rig.
- **vision_critic hardening (helper change, tests-only)**: live gemini-3.5-flash replies
  sometimes carry a prose preamble + markdown fence even with
  `responseMimeType: application/json`, and thinking tokens can exhaust a small
  `maxOutputTokens`, leaving a truncated non-JSON reply ("Here is the JSON requested:
  ```json" — observed live). `_parse_json_reply` now also extracts the first balanced
  `{...}` block, and per-call output budgets were raised (ask 256→2048, ask_choice
  128→1024). TASK-8's 15 guard tests still pass unchanged.
- **Primitive gap (the reason the literal test wasn't buildable)**: TASKS.md's original
  seed — "IK-raise the RIGHT arm skyward and ask the critic which arm is raised" — cannot
  be arranged with today's primitives. `kinematics_probe`'s live mode APPLIES the pose and
  then RESTORES it within the same call (`MCPKinematicsCommands_Probe.cpp:277`), so no
  posed frame ever survives to be captured; there is no persistent live-pose primitive
  (a "hold this bone transform until released" arrange op), and the fixture project has no
  humanoid/mannequin skeletal content to pose anyway. The colored-marker surrogate
  validates the same left/right/up semantics deterministically. If a persistent-pose
  primitive ever lands (plus a humanoid fixture), upgrade this battery rather than writing
  a new one.
- **Proposed change (skill language)**: none to the convention content — §1.1 is now both
  numerically (TASK-1) and perceptually (this test) machine-checked. Optional annotation:
  mark the §1.1 axis bullets as perceptually verified by this battery. For
  `/capture-pose`: until the PrintWindow bug is fixed, the skill's in-PIE capture step can
  silently produce wrong-view frames while still returning `status:"captured"` — worth a
  warning line once BUGS.md triages the defect.
- **Harness friction noted while building** (not skill defects):
  - `actor_spawn` (MCP tool) cannot attach a static mesh at spawn — the bridge's
    `spawn_actor` accepts `static_mesh`, but the tool surface doesn't; tests must follow
    up with a reflection write to `StaticMeshComponent.StaticMesh` and re-verify via
    `mesh_get_actor_material_info`. A `static_mesh` passthrough on `actor_spawn` would
    remove a two-step footgun.
  - `pie_start`'s success envelope is inconsistent across paths (`"success"` vs
    `"starting"`); lease-acquisition helpers must accept both.
  - The fixture project's intentionally-blank startup map is a trap for any render-based
    test: it renders solid black under GUI. `level_new` from an engine template is the
    one-call fix; worth a note in docs/TESTING.md's render-test section.

### /texture + material authoring — both graph idioms now perceptually machine-checked (TASK-10, iteration 3, 2026-07-03)

- **Skill**: `.claude/skills/texture/SKILL.md` (and the material-authoring primitives its
  workflow rides on: `material_create shading_model:Unlit`, `material_add_expression`
  incl. the Custom-HLSL escape hatch, `material_connect`, `material_apply_to_actor`).
- **Test**: `tests/skills/test_texture_perceptual.py` — 3/3 green on the FIRST full
  `tests/run.ps1 --ue-mode=gui` launch (60.8s), clean teardown (ports 55557/8765 free,
  no editor process, actors/assets/screenshots removed). One cube, one fixed
  editor-viewport rig (the TASK-9 rig: `level_new` from `Template_Default`, py-hatch
  `set_level_viewport_camera_info` verified via `editor_viewport_get_camera`,
  `editor_screenshot mode=viewport`), three unlit materials swapped onto the SAME actor
  between frames, 6 Gemini calls (each frame carries a control question with the
  opposite expected answer):
  - **Custom-HLSL checkerboard** (`fmod(floor(u*6)+floor(v*6),2)` fed by a
    TextureCoordinate through a named `uv` custom input) → critic sees a repeating
    pattern, NOT a solid.
  - **Solid 0.5-gray control** (Constant3Vector → EmissiveColor) → critic sees a solid,
    NOT a pattern (the anti-rubber-stamp arm — same questions, flipped answers).
  - **Pure-node stripe chain** (TextureCoordinate → Multiply(×6) → ComponentMask(r) →
    Frac → If(>0.5 ? white : black)) → critic sees parallel STRIPES and specifically NOT
    a checkerboard — masking r alone is what makes it stripes, so the stripes-not-checker
    pair observes the ComponentMask channel select perceptually.
- **Findings / proposed changes (language only)**:
  1. **Unlit + emissive is the trap-free capture idiom — say so in /texture.** The whole
     battery is decidable regardless of scene lighting because every material is Unlit
     with the pattern driven into EmissiveColor. This is the known editor-viewport-vs-PIE
     brightness trap dodge (see the visual-compare memory); /texture's preview-mesh step
     would benefit from one line recommending an unlit/emissive preview variant when the
     goal is validating the TEXTURE (not the lighting response).
  2. **The Custom-HLSL escape hatch is solid and connectable by input NAME** — the
     `inputs:[{name:"uv"}]` pin is addressable in `material_connect` as
     `target_input:"uv"` just like a built-in pin. Worth stating in the tool description
     / skill so agents don't assume Custom nodes need index-based wiring.
  3. **r-key footgun sidestep**: the battery avoids `material_set_expression_property`
     entirely by passing r/g/b at `material_add_expression` time — a safe authoring
     pattern worth recommending until the g-only silent no-op (test_material.py note) is
     fixed.
  4. **Rig reuse, no new bugs**: per the orchestrator's binding correction the battery
     does NOT use `pie_capture_from_pose` (open BUGS.md defect from TASK-9); the
     editor-viewport rig worked first try. The rig helpers are now duplicated across two
     skill test files (`_set_viewport_camera`/`_capture_viewport`) — a future tests-only
     refactor could promote them into `tests/harness/`, but that is out of band here.

### /capture-pose — framing differential is the anti-rubber-stamp pattern for capture claims; editor-level pose→pixels tracking is now machine-checked, the PIE path xfails on the open bug (TASK-11, iteration 3, 2026-07-03)

- **Skill**: `.claude/skills/capture-pose/SKILL.md` — the core claim that a recorded pose,
  replayed through the capture rig, governs the pixels that come back.
- **Test**: `tests/skills/test_capture_pose_framing.py` — first full
  `tests/run.ps1 --ue-mode=gui` launch: **1 passed, 1 xfailed in 37.3s**, clean teardown
  (ports 55557/8765 free, no editor process, actor/materials/screenshots removed).
- **The design pattern (record it as the standard for any capture claim)**: a
  **framing differential**. One vivid emissive RED cube at a known spot; **pose A**
  300 units in front looking straight at it (critic `ask_choice`: CENTERED); **pose B**
  the SAME camera location yawed 90° away (critic: ABSENT, EDGE tolerated); plus an
  explicit `verdict_a != verdict_b` assertion. Two frames with OPPOSITE expected answers
  means a capture path that ignores the pose — or an always-agreeable critic — fails the
  test instead of passing it. Strictly stronger than any single-frame "does it look
  right?" score, and it costs only ONE critic call per frame (4 Gemini calls for the
  whole file). Any future test claiming "this capture shows X" should be built as a
  differential, never a lone frame.
- **Green half (closes the iteration-2 capture-pose observability gap at the editor
  level)**: the proven editor-viewport rig (`level_new` from `Template_Default`,
  py-hatch `set_level_viewport_camera_info` verified via `editor_viewport_get_camera`,
  `editor_screenshot mode=viewport`) demonstrably tracks the requested pose end to end:
  CENTERED at the cube, ABSENT when yawed away. Camera-pose→pixels is now a
  machine-checked fact for this rig.
- **xfail half (self-unblocking guard on the BUGS.md defect)**: the identical
  differential through `pie_capture_from_pose` (lease-honoring `pie_start` queue loop,
  two pose captures, same critic questions) is `xfail(strict=False)` on docs/BUGS.md
  "`pie_capture_from_pose` pixels do not track the requested pose"; it xfailed as
  expected on this run. The mechanical assertions (status `"captured"`, PNG > 1000
  bytes) sit BEFORE the perceptual ones, so when the capture path is fixed the test
  flips to XPASS with zero edits — an unexpected PASS of this test is the "bug closed"
  signal.
- **Proposed changes (skill language)**:
  1. **SKILL.md's "confirm the rig works" step should be a differential, not a single
     framing score** — capture the recorded pose AND a deliberately-averted control pose
     and require opposite verdicts. A single-frame `/visual-critique` score cannot
     detect the exact failure mode BUGS.md documents (a plausible-looking frame from the
     WRONG pose still scores well). One sentence plus the two-pose recipe suffices.
  2. **Warning line until the bug is fixed**: the in-PIE capture step
     (`pie_capture_from_pose`) can return `status:"captured"` with pixels from a stale
     fixed viewpoint (open BUGS.md defect); until it is fixed, the editor-viewport rig
     (py-hatch `set_level_viewport_camera_info` + `editor_screenshot mode=viewport`) is
     the trustworthy replay path for framing decisions.
  3. Reaffirms the TASK-7 proposal (mechanical assertions first, vision advisory on
     top) — this test is the executable form of that ordering.
