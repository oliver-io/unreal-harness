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

---

# DEFERRED

Issues understood but parked (the bugfix loop `docs/loops/mcp/mcp-bugfix-loop.md`
consumes this list).

- *(empty)*
