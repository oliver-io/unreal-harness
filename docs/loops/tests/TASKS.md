# Test-coverage tasks

Produced by a full surface-vs-suite analysis (2026-07-02): the canonical Bun registry (292 tools)
diffed against the C++ dispatch surface (288 wire names) and the pytest `@covers` ledger
(248 names), plus a file-by-file depth audit of all 27 pytest modules, their Bun mirrors, and the
top-level Bun unit/protocol tests. Each task names a coverage gap; per the loop doc, the loop agent
still re-derives the observable contract and investigates the implementation before writing
anything — do not take these descriptions at face value.

## A. Harness integrity (do these first — they gate everything below)

(none open — the dead-wire-name report landed in `docs/BUGS.md` § "Dead wire names report
(2026-07-02)"; per-name expose/delete verdicts await the human's decision there.)

## B. Wire ops with NO test at all (from the set diff)

(none open — `pie_capture_from_pose` + `pie_inject_input_action` landed 2026-07-03, gate 7 → 5;
parity twins in `tests/integration/test_pie.py` + `src/server/test/integration/pie.test.ts`,
all green live against the GUI trong editor in attach mode. The inject op's deep consumer-side
positive observation is honestly partial — see #DEFERRED.)

## C. Server-local tools with no test (bun tier)

Enforced (2026-07-02) by the bun coverage gate `src/server/test/coverage.test.ts` — the analog of
pytest's `test_zz_coverage.py`, expected red until this section completes. Rule: registry tools
whose wire name (`ToolDef.command ?? name`) has no C++ dispatch key are server-local (invisible to
the pytest manifest by construction) and must declare bun-side coverage via `covers("tool")` from
`src/server/test/harness/coverage.ts`. Note the original "~33" estimate was the naive
name-vs-manifest diff: 20 of those are legacy `command:`-override bridge tools (`statetree_*`,
`bp_add_node`) that pytest already sees under their wire names — the true server-local set is 13,
of which `catalog_*`/`code_*` (6) are covered in `disclosure.test.ts`. The gate's current missing
list is exactly the items below; landing each test with its `covers(...)` annotation shrinks it.

(none open — the gate is green as of 2026-07-02.)

## D. Hollow tests — op is "covered" but the assertion is the mutator's echo or bare success
(The independent read primitive already exists in nearly every case; fixes are usually 3 lines.
Keep the pytest and bun mirror in lockstep when fixing.)

### Actor / level / physics / kinematics
(all 7 landed 2026-07-03; pytest + bun mirrors in lockstep, green live. Oracle notes for future
auditors: `actor_inspect` exports NO component leaf properties → `actor_set_property` readback is
the dry-run before-value (B4 convention); `level_inspect` surfaces only the WorldSettings
actor's path/class → `level_set_gamemode_override` is observed via the py-hatch DefaultGameMode
readback and now RESTORES the prior override in finally; `anim_physics_inspect` exports no
constraint motion states → swing1 is re-read via a fresh no-op `physics_set_constraint_motion`
call's from-value, then restored; `kinematics_solve` verification "passed" == the after-pose
world location landing on `hand_target_world` (residual < 1; `score` conflates forward-axis
alignment and is 0 even on a perfect positional solve) — the test still skips live on 2-bone
SkeletalCube, but the assertion path was validated by a manual 3-bone mannequin drive.)

### Blueprint / widget
(all 8 landed 2026-07-03; pytest + bun mirrors in lockstep, green live against the GUI trong
editor in attach mode. Oracle notes for future auditors: `bp_compile` is observed via the
py-hatch generated-class UFunction probe (`<pkg>.<Name>_C:<Event>` absent → compile → present;
`bp_add_custom_event` regenerates the SKELETON only, so the before-state is real; class_inspect
can't resolve BP generated classes — ExactClass); `bp_set_event_replication` remains a DEAD WIRE
(docs/BUGS.md — handler exists, MCPBridge.cpp never routes it), so the hardened test xfails on
exactly the "Unknown command" signature and self-unblocks with the full `bp_inspect`
custom-event-decoder readback (`replication == "RunOnServer"`; the :404 function_flags WAS the
mutator's echo; `bp_get_function_details` can't observe it — walks FunctionGraphs only, custom
events live in UbergraphPages) — the missing bun custom-event+replication mirror is ported with
the same guard; `widget_set_property`'s suggested `widget_tree_read` oracle was wrong
(tree read exports only name/class/parent/slot) — observed via the py-hatch WidgetTree-subobject
`is_enabled` readback instead, restore-in-finally, and the property's real FName is `bIsEnabled`
("IsEnabled" → pin_not_found, verified live — the old test could never have run green);
`widget_bind_handler` is observed via `bp_inspect`'s K2Node_ComponentBoundEvent serialization
(delegate_name/component_name).)

### Material / niagara / mesh / asset / editor
(the 10 non-niagara bullets landed 2026-07-03; pytest + bun mirrors in lockstep, green live
against the GUI trong editor in attach mode. Oracle notes for future auditors:
`material_read` DOES expose connections — `expressions[].inputs[].connected_expression` +
top-level `material_inputs.BaseColor.{connected_expression, connected_output_index}` — and
per-type `properties` (Constant3Vector r/g/b); BUT `material_set_expression_property`'s
colour path GATES on the `r` key (TryBuildLinearColorFromJson), so the old test's g-only
payload was a silent success-shaped NO-OP the echo assertion hid — always send `r`.
`material_compile` saves the package (GAP-062), so its independent observable is the
on-disk .uasset write (mtime-keyed — stale files survive ensure_absent against a live
project). `material_apply_to_actor` needs a MESH-BEARING actor (a mesh-less
StaticMeshActor reports zero slots — the old "info non-empty" could never see the path);
spawn actors with UNIQUE per-run names (fixed names hit FName-reuse-until-GC as
name_collision at SpawnActor). `get_blueprint_material_info` still answers on the wire
(dispatched at MCPBridge.cpp:763 despite the dead-wire report). `asset_fixup_redirectors`:
a plain save+rename leaves NO redirector in UE 5.7 (all-loaded referencers are fixed in
memory; the old found-is-not-None assertion passed on found==0), and a read-only referencer
ABORTS the whole rename (CheckOutPackages) — the deterministic manufacture is a temporary
LOCAL collection via the py hatch (`AssetTagsSubsystem`, engine subsystem;
`CollectionShareType.LOCAL`): DetectReferencingCollections forces bCreateRedirector.
`editor_screenshot` needs an explicit `filename` and the bridge confirms the file
(GAP-007) — assert the PNG on disk, then delete it. `editor_read_logs` IS callable from
the pytest tier (the mcp fixture speaks real MCP to the server) but must NOT be named in
pytest `@covers` (server-local — unknown to the bridge manifest, would fail the zz gate);
the `ke * MARKER` log-roundtrip observes `editor_console_exec`. Known flake noted in
passing: bun `reads.test.ts` `editor_live_coding_compile` can exceed its 5s default
timeout against a GUI editor with Live Coding enabled — pre-existing, untouched.)
(the 5 observable niagara bullets landed 2026-07-03; pytest + bun mirrors in lockstep, green live
against the GUI trong editor in attach mode. Oracle notes for future auditors:
`niagara_module_get_inputs` DOES surface per-input values (AppendValueToJson) and a set survives
the setter's recompile re-bake — the shared `_VALUE_KEYS_FOR_TYPE` payloads are now DISTINCTIVE
(7.25 etc.) so readback can never pass on InitializeParticle's 0/1/10-and-ones defaults;
`niagara_system_read` user_parameters carry `value` — set is observed as 12.5 via the reader and
remove as absence; `niagara_scratch_pad_module_add` is invisible to every typed niagara reader
(emitter_read's script list / RI counts unchanged; custom-HLSL pins are not Module.* RI params) —
observed via the py-hatch ObjectIterator pair (scratch UNiagaraScript registered under
`<system>:<emitter>.NiagaraScratchPadContainer_N.<Name>` + the UNiagaraNodeFunctionCall whose
function_script is that script, both path-scoped to the system); `niagara_script_create`
auto-saves — asserted via the attach-safe live-project-root `.uasset` (B6/D3 mtime-keyed
precedent). The three renderer ops moved to the consolidated #DEFERRED niagara entry.)

### Animation / IK / state machines / StateTree
(all 10 landed 2026-07-03; pytest + bun mirrors in lockstep, green live against the GUI trong
editor in attach mode — animation 15 pass/1 honest skip, ik 11/11 pass with the rig-gated tests
now RUNNING positive, statemachine+statetree 23/23. Oracle notes for future auditors:
`anim_list_sequences` is now actually invoked (dedicated test: created namespaced dupe
enumerates, count==len, asset_list registry subset cross-check, bad-skeleton `asset_not_found`
negative) and its five lying `@covers` tags were consolidated; `bp_add_node`'s pytest `@covers`
name is its WIRE name `add_blueprint_node`. The UAnimSequence-gated ops now `asset_duplicate` a
discovered sequence into the namespace and act on the DUPE — real project content is never
mutated and every output lands in-namespace (the old tests mutated + leaked `_MCP*` outputs next
to real trong sequences; those prior-session leaks were LEFT in place — user project content —
and discovery now filters `_MCP`/`__MCPTest__` so they're inert). The output-suffix ops all
SaveAsset their output (MCPIKCommands.cpp) → on-disk `.uasset` assert via the live-project-root
path; `anim_sequence_set_property` is observed via the py-hatch `additive_anim_type` readback
(`AAT_LOCAL_SPACE_BASE`). `anim_node_bind_property`: the PropertyBindings TMap is UNOBSERVABLE —
no typed reader, and the py hatch cannot read `TMap<FName, FAnimGraphNodePropertyBinding>` (the
value struct has no Python glue, verified live) — so the test binds SequencePlayer.PlayRate (a
real pin-optional property; the old Root-node "Sequence" bind was a graceful-answer non-test)
and observes the bind's ReconstructNode side effect: the PlayRate pin ABSENT from
`bp_list_node_pins` before, PRESENT after (note: pin does NOT materialize for non-pin-optional
props like MaxTransitionsPerFrame even though `pin_toggled_visible` echoes true). IK: the
module's `_find_asset` never unwrapped asset_list's `data` envelope, so every rig-gated test
SILENTLY SKIPPED everywhere — even against trong which ships 2 IKRigDefinitions (and the old
`"scale_vertical" in written_fields` assertion could never have passed: entries are
"key=value" strings); fixed, so auto-map is observed via `ik_retarget_read` (identity rig ⇒
every target_chain maps to itself, 50/50 live), pelvis/root-motion settings are re-read via a
SECOND setter call writing an unrelated field (its response reports the STORED op-settings
struct — ik_retarget_read exposes no op settings and the 5.7 FInstancedStruct op stack has no
py glue), and `run_batch` acts on a namespaced dupe found via the typed chain
rig → preview_mesh → skeleton → anim_list_sequences(skeleton_path); its outputs land at the
/GAME CONTENT ROOT and are NOT saved to disk (verified live) → observed via asset_list registry
presence, deleted in finally. State machines: graph-node object paths are deterministic
(`<pkg>.<ABP>:AnimGraph.<MachineNode>[.<Machine>.<Node>]`) so modify_transition's
crossfade_duration/priority_order and set_inner_node_property's Node.MaxTransitionsPerFrame are
py-hatch `find_object` readbacks; set_entry is observed via `bp_list_node_pins` on each state
node scoped by `graph_name=<machine>` — the entry link serializes as an In-pin connection from
`AnimStateEntryNode_*` pin "Entry" (two states, entry on the second, first must have none).
StateTree: `st_set_node_property` re-reads the DISTINCTIVE value (7.25/42) via
`statetree_node_get_properties`, using a scalar-bearing task type (StateTreeDelayTask preferred —
the generic types[0] pick is StateTreeBlueprintTaskWrapper with NO instance properties, which
made the old test silently skip); `statetree_compile`'s suggested oracle was WRONG:
`statetree_verify.ready_to_run` does NOT flip — it stays true even when stale (previously-linked
compiled data remains loaded) — the real observable is `hash_matches`/`status` (stale → ok) plus
`stored_hash == compile.editor_data_hash`. Attach-mode note: the animation/ik disk asserts and
the module fixtures need `UE_MCP_TEST_PROJECT` pointed at the live project
(`projects/trong`) — without it the pre-existing `config.uasset_disk_path` create-tests in these
modules fail against the fixture path, which is how they were latently broken under attach.)

### PIE / bun-editor tier
- [ ] `pie_record_disarm` (`test_pie.py:170-180`) — arm is verified via an independent
  `pie_record_status` read but disarm trusts its own echo; re-read status after disarm.
- [ ] bun `editor.test.ts` hollow cluster (one task): `actor_get_in_level` (:40, bare success — 
  assert the actor list), `actor_spawn` dry-run (:45, echo — assert the diff + `actor_query` shows
  no spawn), `catalog_call` (:54, bare success — assert the forwarded payload), `code_run` (:59,
  trusts the sandbox's self-reported count — cross-check against an outer `actor_get_in_level`).
- [ ] `pie_send_keystrokes`/`pie_send_mouse` (`test_pie.py:143-150`) — echo-only; doctrine permits
  injection-op-only assertions, so investigate a bounded deeper observable (e.g. a `[FEATURE]` log
  marker from an input-bound test primitive) WITHOUT turning the test into play-driving; if none
  is sound, leave as-is and note it.
- [ ] `ai_get_state`/`ai_get_awareness`/`ai_get_perception` (`test_pie.py:88-106`) — guard-only
  (never observes a real AI pawn). A deep version needs a PIE world with a perception-enabled
  AIController fixture — likely GUI-gated; scope it or defer with reason.

## E. Dry-run negative gaps (contract advertises dry_run; only EQS and asset_duplicate test it)
- [ ] One dry-run spec test per family, following the EQS pattern (diff asserted + independent
  read proving zero mutation): `anim_skeleton_add_socket` (param already wired in the helper,
  `test_skeleton.py:70`); `asset_rename`/`asset_move`/`asset_delete`; `tag_add`/`tag_remove`;
  `bp_set_node_property`/`bp_connect_pins`/`bp_disconnect_pin`/`bp_delete_node`/
  `bp_remove_component`/`bp_set_variable_properties`. Split per family when picked up.

## F. Bun-mirror parity drift (silent — the bun side has no coverage gate)
- [ ] `reads.test.ts` omits `input_add_mapping` entirely — the strongest test in the Python module
  (read-back + negative path, `test_reads.py:179-217`). Port it.
- [ ] pytest `test_mesh.py` lacks the bun-only engine-refuse socket test (`mesh.test.ts:229-234`).
  Port to pytest so `@covers` sees it. (`mesh_get_bounds` parity landed 2026-07-02.)
- [ ] `gas_effect_apply` has no positive-path observation anywhere (only the `not_in_pie` guard).
  A PIE-tier test observing an applied effect (attribute readback or log marker) — scope it; if
  it needs primitives that don't exist, add them or defer.

# DEFERRED

- **`pie_inject_input_action` deep consumer-side positive observation** (2026-07-03) — the op
  now has real live coverage (parity twins in `test_pie.py` / `pie.test.ts`, green against the
  GUI trong editor): the no-PIE guard (`invalid_argument`, "PIE is not running" — note the C++
  handler deliberately returns invalid_argument here, NOT not_in_pie), the in-PIE bad-path
  `asset_not_found` guard, and the positive wire path (a fixture `IA_MCPInjectProbe` created via
  `input_create` under `/Game/__MCPTest__/pie`, injected into the live player's
  `UEnhancedInputLocalPlayerSubsystem`). What stays deferred is observing a CONSUMER react:
  `InjectInputForAction` (MCPAutomationCommands.cpp:1275-1344) only evaluates for bindings made
  via `UEnhancedInputComponent::BindAction`, the handler emits no log line of its own, and no
  typed primitive can bind an InputAction to an observable reaction without C++ — a Blueprint
  pawn with an EnhancedInputAction event node would additionally need spawn+POSSESS of the PIE
  player (gameplay-state mutation in the shared editor, and BP event-node support for
  EnhancedInputAction events is unverified). So the positive assertion is the injection ack
  (envelope-level), the same concession the house already grants `pie_send_keystrokes`
  ("doctrine permits injection-op-only assertions"). Unblock after the next legitimate full
  rebuild with a small C++ test-consumer primitive: an `AMCPTestInputConsumer` actor (or a
  handler-side flag) that BindAction()s a given IA path on spawn and flips a UPROPERTY / logs a
  `[MCP:TestInput]` marker on Triggered — then Arrange (spawn consumer) → Act (inject) →
  Observe (`pie_query`/`actor_inspect` the flag or `editor_read_logs` the consumer marker).

- **Landscape value-bearing positive paths** — `landscape_read_heightmap` height stats /
  known-sample assertions and `landscape_list_layers` assigned-layer entries (2026-07-02). The
  three landscape ops LANDED with real coverage (gate 20 → 17; parity twins
  `tests/integration/test_landscape.py` + `src/server/test/integration/landscape.test.ts`, 6/6
  green live in attach mode): `landscape_inspect` has a full positive path (a bare
  `/Script/Landscape.Landscape` arranged via typed `actor_spawn`, observed via
  `landscape_inspect` find-by-name + enumeration, deleted in teardown — live probe confirmed
  zero residue), `landscape_list_layers` its success path (component-less proxy → empty layer
  set) and both ops' `actor_not_found` gates, `landscape_read_heightmap` both documented error
  gates (`actor_not_found`, no-ULandscapeInfo `invalid_argument`).
  **2026-07-03 UPDATE — spawn-based positive paths now fixture-gated:** the bare-Landscape
  spawn+delete arrange arms a delayed `ULandscapeSubsystem::Tick` null-deref that crashed the
  shared editor (UE 5.7 engine bug; the "zero residue" probe above measured the LEVEL — the
  stale registration lives in the subsystem). Full forensics: `docs/BUGS.md` § "Bare
  `ALandscape` spawn+delete". The three spawn-based tests now skip under `--ue-attach` (pytest)
  / require `UE_MCP_FIXTURE_EDITOR=1` (bun); the three never-spawning negative tests still run
  everywhere. First green fixture-mode execution pending, like the level-lifecycle modules.
  Un-gating the positive paths against a shared editor needs either the engine bug fixed
  upstream (null-check in `LandscapeSubsystem.cpp:791-794` or symmetric unregister) or a
  verified in-test defuse — none exists today (no py `unreal.LandscapeSubsystem`; GC only
  detonates sooner). What remains unarrangeable:
  a COMPONENT-BEARING landscape. No typed primitive creates landscape components; the UE 5.7
  Python surface exposes only `unreal.LandscapeProxy.landscape_import_heightmap_from_render_target`
  (requires components to already exist) and no `unreal.LandscapeSubsystem` — verified against
  the live 5.7 editor by introspection; the C-side creation path (`ALandscape::Import` /
  `FLandscapeConfigHelper`) is not reflected. A committed fixture .umap with a tiny landscape
  would also work but could not be authored this task (no fixture editor; shared trong editor
  must not switch levels). Unblock either by (a) generating a minimal landscape fixture map on
  the next fixture-editor (launch-mode) session and committing it under
  `tests/fixtures/TestProject/Content`, or (b) a small C++ arrange primitive
  (`landscape_create_for_test`-style Import wrapper) after the next legitimate full rebuild —
  then assert known raw heights / `height_stats` / an assigned paint layer.

- **Level lifecycle — tests WRITTEN, live execution deferred to the next fixture-editor
  (launch-mode) run** (2026-07-02). `level_new`/`level_load`/`level_save`/`level_save_as` now
  have parity twins (`tests/integration/test_level_lifecycle.py` +
  `src/server/test/integration/level_lifecycle.test.ts`; coverage gate 24 → 20), but they are
  skip-gated against a shared attached editor and have NOT yet executed. Hazard findings
  (`MCPLevelCommands.cpp`): `level_new` passes `bSaveExisting=false` to
  `NewBlankMap`/`NewMapFromTemplate` and `level_load` calls `LoadMap` with no save prompt — both
  DISCARD the open map's unsaved changes (no save-all side effects; `level_save`/`level_save_as`
  are surgical, `SaveMap` on the current map package only). Live probe at task time: the open map
  `/Game/Trong/Maps/L_TestBed` was itself DIRTY plus 16 dirty `__MCPTest__` content packages of
  other agents' WIP, so the "current map clean" exception did not apply and no lifecycle op was
  executed. Gating: pytest skips under `--ue-attach` (runs in default launch mode against
  `tests/fixtures/TestProject`); the bun mirror additionally requires `UE_MCP_FIXTURE_EDITOR=1`
  (the bun tier always attaches, so an explicit fixture-editor attestation is the analog).
  Launch mode could not be run this task: the shared trong editor holds :55557, and a second
  editor on the port is forbidden/broken (duplicate-editor lock). Both modules verified by
  collection + skip-gate run (4 skipped each, editor state unchanged before/after). First green
  execution is expected on the next `tests/run.ps1` fixture run — if it fails there, treat as a
  normal red test, not a coverage regression.

- `editor_build_reflection_captures` (2026-07-02) — no observable beyond the echo that a test
  could both assert AND clean up; deferred as "mutates shared editor level state irrecoverably
  under attach mode; no engine log marker exists". Investigated in full against the live editor
  (trong `L_TestBed`, `save:false`, bounded ~1.2s — runtime is NOT the blocker). Handler
  (`MCPLevelCommands.cpp::HandleBuildReflectionCaptures`) calls
  `GEditor->BuildReflectionCaptures(World)` on the CURRENT level, then (default `save:true`!)
  `FEditorFileUtils::SaveDirtyPackages(bSaveMapPackages=true, bSaveContentPackages=true)`; returns
  only its own `{built, map_name, package_path, saved}` echo. Observation candidates, each
  verified live and dead: (1) **engine log marker** — the bake emitted ZERO engine log lines
  between the `[MCP:Command] Received:` receipt (seq 32213) and the next unrelated line; the
  receipt line is the mutator's own echo, forbidden as an observation. (2) **registry state** —
  with zero captures in the level the bake creates no `<Map>_BuiltData` package at all
  (`ObjectIterator(unreal.Package)` found none post-bake), so a DEEP test must first spawn a
  SphereReflectionCapture; the bake then creates+dirties the `_BuiltData` package, readable via
  `EditorLoadingAndSavingUtils.get_dirty_map_packages()` — but NOTHING can un-dirty it afterward
  (`unreal.Package` exposes no `is_dirty`/`set_dirty_flag`; `unreal.MapBuildDataRegistry` is not
  exposed to Python; adding a C++ observe/restore primitive was hard-blocked this task by the
  plugin-DLL Live Coding gotcha — see the Niagara entry above), leaving an irrecoverable dirty
  build-data package in the shared editor that any later save-all by another agent persists.
  (3) **on-disk `_BuiltData.uasset` via `save:true`** — flatly unsafe attached:
  `SaveDirtyPackages` saves ALL dirty map+content packages; the live editor had the map plus 16
  dirty content packages of other agents' WIP at probe time. (4) **component state** — no
  "built" flag readable via `actor_inspect`; `MapBuildDataId` doesn't change on build. Revisit
  after the next legitimate full rebuild: a `.cpp`-side observe/restore pair (e.g. a build-data
  query + a scoped dirty-flag restore, or running the bake against a test-owned fixture level)
  makes the DEEP design (spawn namespaced capture → bake `save:false` → observe registry →
  restore) straightforward.

- **Niagara renderer readback family (7 ops)** — the setter quartet
  `niagara_emitter_set_local_space`, `niagara_renderer_set_enabled`,
  `niagara_renderer_set_alignment`, `niagara_mesh_renderer_set_mesh` (2026-07-02) plus the
  D-hardening echo trio `niagara_emitter_add_renderer`, `niagara_renderer_set_material`,
  `niagara_renderer_set_material_binding` (consolidated 2026-07-03 — these three HAVE tests in
  `test_niagara.py` / `niagara.test.ts` but their assertions are the mutators' echoes; hardening
  them is the same blocked renderer readback, so they ride the same reader extension rather than
  a separate entry) — needs reader extension + rebuild; blocked by shared editor.
  Investigated in full: none of the four written
  fields (`FVersionedNiagaraEmitterData::bLocalSpace`, renderer `GetIsEnabled`, sprite
  `Alignment`/`FacingMode`, mesh-renderer `Meshes[0].Mesh/Scale` —
  `MCPNiagaraCommands_Create.cpp:654-1001`) is surfaced by ANY read primitive:
  `niagara_emitter_read` (`MCPNiagaraCommands.cpp:391-445`) returns only
  name/id/enabled/sim_target/scripts, `niagara_system_read` only name/id/enabled per emitter, and
  the sole renderer listing on the wire is `niagara_renderer_set_enabled`'s own echo (asserting a
  mutator's echo is forbidden by doctrine). The fix is a `.cpp`-body-only extension of
  `HandleReadNiagaraEmitter` (add `local_space` + `renderers[]` with per-type
  alignment/facing/meshes readback — drafted, compiles clean on UE 5.7 per the Live Coding build
  log), BUT it cannot be activated in a shared-editor session: Live Coding compiled the patch and
  then discarded it — `LogLiveCodingServer: Warning: The module
  '...projects/trong/Binaries/Win64/UnrealEditor-UnrealMCP.dll' has not been loaded by any
  process. Changes will be ignored.` This is the plugin-only DLL gotcha (docs/USAGE.md §3.6): the
  editor loads the plugin DLL from `src/Plugin/UnrealMCP/Binaries/Win64`, Live Coding patches the
  host project's copy, so NO plugin C++ change (even body-only) can hot-load — activation needs
  the full stop → build → copy-DLL → relaunch cycle. Pick this up right after the next legitimate
  full editor rebuild: extend the reader, then all seven tests are straightforward
  (arrange NS + emitter + sprite/mesh renderer under /Game/__MCPTest__/, act the setter,
  observe via the extended `niagara_emitter_read`). Interim-oracle note (2026-07-03, recorded,
  not acted on): the py hatch CAN see renderer state — a live probe found the
  `unreal.NiagaraSpriteRendererProperties` object at `<system>:<emitter>.…Properties_N` via
  ObjectIterator with its `material` editor-property readable — so a py-hatch observation is
  technically available if this deferral needs to be broken before the rebuild; the typed
  reader extension remains the intended fix.

- `editor_build_game_target` positive path (2026-07-02) — the validation-gate slice is covered in
  `src/server/test/editor_build_game_target.test.ts` (real registry handler, real dirs/files, no
  mocks: no-root `invalid_argument`, the Unix-path → env-root fallback, the exactly-one-`.uproject`
  scan, and the engine `Build.bat` probe — the last gate before `spawnSync`, so no UBT process is
  ever spawned). The POSITIVE path (an actual UBT Game-target build) stays untested: it is a
  multi-minute, toolchain-dependent offline build that would contend the shared build lock and
  toolchain other agents use (docs/USAGE.md §3.6 exit-75 semantics), and the only cheap alternative
  would be mocking the spawn — which tests the mock, not UBT. No `coverage.test.ts` EXEMPT entry
  needed: the guard slice satisfies the gate honestly.

- `video_analyze` / `pie_analyze` positive path (2026-07-02) — the guard slice is covered in
  `src/server/test/video_analyze.test.ts` (real registry handlers, no mocks: `feature_disabled`
  on missing key / unknown provider, `invalid_path` on a missing file, `invalid_argument` on an
  over-budget `analysis_fps`, and pie_analyze's earliest headless guard, the record-lease
  `pie_not_holder` refusal). The POSITIVE structured-verdict path remains untested: it requires
  the external Gemini video model (network, key, cost, nondeterministic verdict), and for
  pie_analyze additionally a live GUI editor + PIE recording before its (shared) key check is
  even reached. Faking the provider would test the mock, not the pipeline — deferred per the
  no-mocks doctrine. Note: `config` snapshots env at import, so the tests control
  `config.geminiApiKey`/`videoProvider` directly (with afterEach restore) rather than env vars.
