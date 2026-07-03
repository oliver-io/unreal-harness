# BUGS.md — known issues, workarounds, and the GAP registry

Open behavioral issues in the harness and the `GAP-###` entries other docs cite
(`docs/TESTING.md`, `docs/USAGE.md`, skills, C++/TS comments). Every entry below is
**verified against the current code** — cited `file:line` is ground truth.

Provenance note: the original GAP ledger (GAP-001 … GAP-066) predates this repo's
open-source snapshot and was not carried over; most GAP ids now live only as
*resolved-gap markers* in code comments (e.g. GAP-003 retry rule in
`src/server/src/bridge/connection.ts`, GAP-060 non-blocking Live Coding in
`MCPBridge.cpp`). This file reconstructs the entries that docs still point at. Where
an original entry's full meaning could not be re-established, the entry says so
rather than inventing detail.

---

## GAP-007 — `editor_screenshot` (editor viewport) can silently produce no file; now bounded by `timeout`

- **Symptom:** the editor-viewport path of `editor_screenshot` historically returned
  `"requested"` but no PNG ever appeared on disk.
- **Root cause:** that path is async — `FScreenshotRequest` is serviced at
  end-of-frame inside `FViewport::Draw`. A non-realtime, backgrounded, or minimized
  editor viewport schedules no frame, so the request is never serviced and no file
  is written
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPAutomationCommands.cpp:948-979`).
- **Status: mitigated, not fully closed.** The handler forces a burst of realtime
  frames + invalidates the level viewport(s) (`RequestRealTimeFrames(120)`,
  non-persistent), and the bridge polls for the output file on the server thread
  (`MCPBridge.cpp:124,1292`), returning the `timeout` error code instead of a false
  "requested" — `EMCPErrorCode::Timeout` was added to the closed taxonomy for this
  gap (`src/Plugin/UnrealMCP/Source/UnrealMCP/Public/Commands/MCPCommonUtils.h:72-77`).
  A fully occluded/minimized window may still composite no frame.
- **Workaround:** keep the editor window foregrounded with realtime rendering on;
  treat `timeout` as recoverable by foregrounding.
- **Bites:** `docs/TESTING.md` §3 Tier-2b (pixel evidence); USAGE §1.2 taxonomy.

## GAP-009 — `mesh_set_mesh_material_color` would bake a runtime MID into a Blueprint template (DISABLED by refusal)

- **Symptom:** level saves fail with *"Illegal reference to private object"* for any
  map placing the affected Blueprint.
- **Root cause:** the tool only ever targets a Blueprint SCS component **template**;
  persisting a `UMaterialInstanceDynamic` (runtime-only) into the saved asset
  corrupts `SaveMap`.
- **Status: closed by refusal.** Both sides refuse the dynamic-instance path with
  `feature_disabled` and redirect to the saved-asset route:
  `material_create_instance` → `material_instance_set_parameter` →
  `material_apply_to_blueprint`
  (`src/server/src/domains/mesh.ts:62-71`,
  `src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPBlueprintCommands.cpp:1746-1761`).
  `actor_spawn_physics` sidesteps the same trap on BP templates
  (`src/server/src/domains/actor.ts:295`).

## GAP-022 — `bp_add_node` CallFunction: no global function-name search; `target_class` required for uncommon libraries

- **Symptom:** creating a `CallFunction` node for a function on a library outside
  the hardcoded common set fails unless `target_class` is passed.
- **Root cause (by design):** function resolution walks a **bounded** chain — explicit
  `target_class` → explicit `target_blueprint` → the BP's own `GeneratedClass` /
  `ParentClass` → a fixed common set (Actor/Pawn/Character/Controller/
  PlayerController, AnimInstance, CharacterMovementComponent,
  KismetSystem/KismetMath/GameplayStatics)
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/BlueprintGraph/Nodes/UtilityNodes.cpp:150-229`).
  There is deliberately no global search — it would risk wrong-overload matches.
- **Workaround:** pass `target_class` (bare name, UE-prefixed, or `/Script/...`
  path) for anything else. Contract: `docs/USAGE.md` §2.5.
- Original ledger entry lost; reconstructed from USAGE + the resolver code.

## GAP-030 — synthetic PIE input doesn't reach polled input / doesn't reliably actuate Slate UI

- **Symptom:** `pie_send_keystrokes` reports success but `IsInputKeyDown`-style
  polled input never sees the key; synthetic clicks are similarly unreliable at
  actuating Slate/UMG widgets.
- **Root cause:** injected events don't hold sustained Slate keyboard focus on the
  PIE viewport; on viewport focus loss the engine flushes pressed keys
  (`FlushPressedKeys`) before the pawn polls
  (`src/server/src/domains/pie.ts:398-409` documents the mechanics).
- **Workaround:** prefer **self-driving subjects** — a bot, auto-tick Blueprint, or
  placed `BP_*Director` — observed via `pie_query` + log markers; use injection only
  to assert the injection op itself (`docs/TESTING.md` §5). For sustained polled
  input specifically, `pie_send_keystrokes` has an opt-in `focus_viewport: true`
  (steals keyboard focus from the editor window — leave off otherwise).
- **Bites:** TESTING §5, `/automated-tester` §2 (Tier-2 specifics).

## GAP-031 — GameMode `BeginPlay` does not reliably fire in PIE

- **Symptom:** scenario orchestration hung off the GameMode's `BeginPlay` never runs
  during a PIE session.
- **Workaround:** orchestrate from a **placed** `BP_*Director` actor in the level
  instead of the GameMode (`docs/TESTING.md` §5,
  `.claude/skills/automated-tester/SKILL.md` Tier-2 specifics).
- **Status: open.** The original ledger entry is lost and the engine-level root
  cause has not been re-established from code — this entry is reconstructed from
  the citing docs only. Treat the workaround as the contract.

## GAP-056 — use-after-free building the response after `PostEditChangeProperty` (FIXED; standing C++-author warning)

- **Symptom:** `EXCEPTION_ACCESS_VIOLATION` in `FName::ToString` while building the
  `owner` response field of an actor property write.
- **Root cause:** on a Blueprint-instanced actor, `PostEditChangeProperty` (and a
  subsequent `SaveAsset`) can trigger `RerunConstructionScripts`, which **destroys**
  the SCS component (renamed `TRASH_*`, freed); dereferencing the component or its
  value memory afterwards is a use-after-free.
- **Status: fixed** — the handler snapshots everything needed for the
  response/log *before* `PostEditChangeProperty`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPEditorCommands.cpp:697-716`).
  Kept here as a pattern warning: any handler mutating a component on a
  BP-instanced actor must not dereference the component after `PostEditChange`.

---

## Known issues (unnumbered)

The lost ledger's numbering can't be recovered, so new entries are unnumbered to
avoid colliding with lost GAP ids.

### PIE video recording requires a real RHI and Windows

`pie_record_start` (and armed auto-record) is refused with `feature_disabled` under
`-nullrhi` — there are no frames to record
(`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPVideoRecorder.cpp:723-728`:
`GUsingNullRHI || !FApp::CanEverRender()`). The encoder is Media Foundation, so
non-Windows builds compile a refusing stub
(`MCPRecorderCommands.cpp:39,184-190`, `#if PLATFORM_WINDOWS`). Bites the Tier-2b
video-evidence channel in headless CI: launch with a GUI (or `-RenderOffscreen`),
not `-nullrhi`.

### Duplicate editor on :55557 fails its bind silently

A second editor instance can't bind the bridge port; the failure is only an error
line in *its* log ("Failed to bind listener socket",
`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/MCPBridge.cpp:416-420`) — the editor
otherwise runs normally, bridge-less. Symptom: MCP commands land on the *other*
(first) editor, and the duplicate can hold `.uasset` file locks that break saves.
Fix: find the non-bridge instance (the process **not** owning port 55557) and close
it; grep the editor log for the bind error.

### A failed Live Coding patch can crash the editor

`editor_live_coding_compile` drives a non-blocking compile (GAP-060), but a failed
patch can take the whole editor down; the server detects this when `mcp_status`
stops answering mid-compile and surfaces `engine_busy` with an "editor became
unreachable … may have crashed" message
(`src/server/src/bridge/connection.ts:240-254`). Mitigation: reserve Live Coding
for `.cpp`-body edits; reflection-affecting changes (headers, `UPROPERTY`/
`UFUNCTION`, vtable) need the full stop → build → launch cycle
(`docs/USAGE.md` §3.6). On crash, check the editor process and `MCP_Unified.log`
(`LIVECODING`).

### `ai_get_awareness` is hardwired to component names containing "CombatAwareness"

The handler only recognizes an awareness component whose class name
`Contains(TEXT("CombatAwareness"))`
(`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPAIRuntimeCommands.cpp:185,202`) —
a leak from a specific private game project. Any project whose pawn-side awareness
component is named anything else gets `has_awareness:false` unconditionally, which
makes the `/npc_logic` skill's central awareness-layer advice unobservable through
this tool. Surfaced by the skill-test loop 2026-07-02
(`docs/loops/skills/TASKS.md` TASK-3). Candidate fixes (not applied): accept a
`component_class` parameter, or match any component implementing a marker
interface/tag instead of a name substring.

### `class_inspect` / `reflection_class_properties` cannot resolve Blueprint-generated classes

Both resolvers walk `FindFirstObject<UClass>(…, EFindFirstObjectOptions::ExactClass)`
(`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPBlueprintCommands.cpp:5551-5563`,
`MCPReflectionCommands.cpp:72-87`), and `ExactClass` matches only objects whose class
is exactly `UClass` — excluding every `UBlueprintGeneratedClass` instance. The load
branch only handles `/Script/` paths, so a `BP_…_C` name returns `class_not_loaded`
even though `class_inspect`'s error hint implies broader acceptance
(`MCPBlueprintCommands.cpp:5571`). Live-verified during the skill-test loop 2026-07-02
(TASK-2): CDO flags set by `bp_set_class_replication` could not be observed through
either tool; the test observes via `actor_spawn` + dry-run reflective read instead
(`tests/skills/test_networking_authoring.py` documents the workaround). Candidate fix
(not applied): fall back to `FindFirstObject<UClass>` without `ExactClass` (or resolve
via the Blueprint asset's `GeneratedClass`) for `_C` names.

---

## Dead wire names report (2026-07-02)

Eight C++ dispatch keys are live in the plugin but unreachable from any canonical
server tool (found by the test-coverage loop's surface diff; verified against current
code below). This is a **report for a human decision** — no C++ was changed. Three
verdict classes: **DELETE** (dead duplicate), **EXPOSE + TEST** (unique capability
worth a canonical tool), **KEEP-INTERNAL** (deliberately used raw by the harness).
Note: `add_conduit` and `merge_bones_*` appear on the C++ PIE/dry-run blocklists
(`MCPCommonUtils.cpp:290,304`) and are documented as intentionally absent from the
TS mirror (`src/server/src/bridge/gates.ts:14-17`) — any delete/expose must touch
those lists on both sides.

### `add_conduit` — EXPOSE + TEST

- **Handler:** `FMCPAnimationCommands::HandleAddConduit`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPAnimationCommands.cpp:267`,
  dispatched at `:47`).
- **Does:** adds a conduit node to a named state-machine graph inside an Anim
  Blueprint (`blueprint_name`, `state_machine_graph`, optional `conduit_name`).
- **Canonical coverage:** none — the `anim_state_machine_*` family has states,
  transitions, entry, and inner-node property tools but no conduit op. Unique.
- **Callers:** none anywhere (tests exclude it; `tests/tools/regen_operations.py:35`
  lists it as deliberately excluded from the manifest).
- **Verdict:** EXPOSE + TEST as `anim_state_machine_conduit_add`, completing the
  family — or DELETE if conduits are declared out of scope. Exposing is the cheap,
  consistent option.

### `editor_focus_actor` — EXPOSE + TEST

- **Handler:** `FMCPAutomationCommands::HandleFocusActor`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPAutomationCommands.cpp:108`,
  dispatched at `:84`).
- **Does:** points the level-editor perspective viewport at an actor (colliding-only
  bounds first, optional `focus_location`/distance overrides) so
  `editor_screenshot` (editor mode) frames it — the handler's own comment calls it
  "the controllable-camera primitive".
- **Canonical coverage:** none — `editor_viewport_get_camera` is read-only; there is
  no viewport set-camera tool.
- **Callers:** none anywhere.
- **Verdict:** EXPOSE + TEST (e.g. `editor_viewport_focus_actor`). It is exactly the
  "set-camera primitive; add one if missing" that the `editor_viewport_get_camera`
  test task in `docs/loops/tests/TASKS.md` §B calls for.

### `get_blueprint_material_info` — EXPOSE + TEST (currently KEEP-INTERNAL de facto)

- **Handler:** `FMCPBlueprintCommands::HandleGetBlueprintMaterialInfo`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPBlueprintCommands.cpp:2300`,
  dispatched at `:162`).
- **Does:** reads material-slot info for a named mesh component on a Blueprint (the
  SCS template) — the Blueprint-side sibling of the canonical
  `mesh_get_actor_material_info` (`src/server/src/domains/mesh.ts:53`), which only
  covers placed actors.
- **Callers:** both parity suites call it raw and annotate "bridge-internal command
  (no standalone MCP tool)" — `tests/integration/test_material.py:355-357`,
  `src/server/test/integration/material.test.ts:323-325`. The
  `material_apply_to_blueprint` slot_index error hint **directs users to it**
  (`MCPBlueprintCommands.cpp:2188`), and the hollow-test task at
  `docs/loops/tests/TASKS.md` §D wants it as the oracle for
  `material_apply_to_blueprint`.
- **Verdict:** EXPOSE + TEST (e.g. `mesh_get_blueprint_material_info`) — the surface
  already advertises it in an error hint, and tests already depend on it; a
  canonical tool removes the raw-bridge exception documented in
  `tests/README.md:30-31`.

### `get_mesh_bounds` — DELETE (dead duplicate)

- **Handler:** `FMCPBlueprintCommands::HandleGetMeshBounds`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPBlueprintCommands.cpp:3632`,
  dispatched at `:187`) — a **separate implementation**, not an alias, of the same
  capability as canonical `mesh_get_bounds`
  (`FMCPMeshCommands`, `Commands/MCPMeshCommands.cpp:58`).
- **Does:** mode 1 (`mesh_path`) returns a UStaticMesh asset's local bounds —
  duplicating canonical `mesh_get_bounds`; mode 2 (`actor_name`) returns a placed
  actor's world-space AABB including scale.
- **Canonical coverage:** asset mode fully duplicated; actor mode substantially
  covered by `actor_inspect`, which returns per-component `world_bounds`
  (`Commands/MCPInspectionCommands.cpp:646-655`) — the only delta is the aggregated
  whole-actor box.
- **Callers:** none anywhere.
- **Verdict:** DELETE. If the aggregated actor AABB is wanted, add an `actor_name`
  mode to the canonical `mesh_get_bounds` (with a test) as part of the same change.

### `list_material_parameters` — EXPOSE + TEST

- **Handler:** `FMCPMaterialCommands::HandleListMaterialParameters`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPMaterialCommands.cpp:679`,
  dispatched at `:77`).
- **Does:** enumerates ALL parameters (scalar/vector/texture …) of any
  UMaterialInterface via reflection — including parameters inherited from a parent
  material, on either a Material or a Material Instance.
- **Canonical coverage:** partial only — `material_read` reads a base material's
  expression graph and `material_read_instance` reads an MI's *overridden* params;
  neither enumerates the full inherited parameter set on an arbitrary interface.
- **Callers:** none anywhere (it is even listed in its own domain's dispatch error
  hint, `MCPMaterialCommands.cpp:85`, so the error surface advertises a command no
  tool can reach).
- **Verdict:** EXPOSE + TEST as `material_list_parameters` — the natural discovery
  primitive before `material_instance_set_parameter`.

### `merge_bones_into_skeleton` / `merge_bones_into_skeletal_mesh` — DELETE (superseded), pending confirmation

- **Handlers:** `FMCPSkeletalMeshCommands::HandleMergeBonesIntoSkeleton`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPSkeletalMeshCommands.cpp:921`)
  and `…IntoSkeletalMesh` (`:1069`); contracts in
  `Public/Commands/MCPSkeletalMeshCommands.h:65-77`.
- **Do:** copy named bones from a source USkeleton into a target USkeleton
  preserving hierarchy/bind pose; the `_mesh` variant also writes the mesh's ref
  skeleton (the one governing skinning) and syncs the bound USkeleton.
- **Canonical coverage:** no tool merges bones. However the header comments show
  these were built for the same static-baked-rod problem that the canonical
  `mesh_build_bend_chain` (`MCPSkeletalMeshCommands.h:79-89`) now solves end-to-end
  (inserts a bone chain AND re-skins); the merge ops add bones without re-skinning.
- **Callers:** none anywhere (excluded in `regen_operations.py:35`; noted as
  wire-only in `gates.ts:16`).
- **Verdict:** DELETE as superseded by `mesh_build_bend_chain` — unless arbitrary
  cross-skeleton bone merge (e.g. attaching equipment bones) is a workflow someone
  wants, in which case EXPOSE + TEST the pair. Human call.

### `spawn_actor` — KEEP-INTERNAL today; DELETE after test migration

- **Handler:** `FMCPEditorCommands::HandleSpawnActor`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/MCPEditorCommands.cpp:136`,
  dispatched at `:49`).
- **Does:** legacy spawn of a hardcoded native set (StaticMeshActor, Point/Spot/
  DirectionalLight, CameraActor) by required unique `name`; refuses name collisions.
- **Canonical coverage:** `actor_spawn` — a separate, richer handler
  (`FMCPSceneCommands::HandleActorSpawn`, `Commands/MCPSceneCommands.cpp:118`) with
  arbitrary class support and dry-run. Full duplicate in capability.
- **Callers:** the parity suites use it raw and on purpose —
  `tests/integration/test_actor.py:45` (plus `:5,54,68,145`),
  `tests/integration/test_mesh.py:185,311`,
  `src/server/test/integration/actor.test.ts:45` (plus mirrors),
  `src/server/test/integration/mesh.test.ts:142,308`; the exception is documented in
  `tests/README.md:30-31` and it is manifest-excluded in `regen_operations.py:35`.
- **Verdict:** KEEP-INTERNAL as-is only while the tests depend on it; recommended
  end state is DELETE — port the four test call sites to `actor_spawn`, drop the
  README exception, then remove the handler.

---

# DEFERRED

Issues understood but parked (the bugfix loop `docs/loops/mcp/mcp-bugfix-loop.md`
consumes this list).

- *(empty)*
