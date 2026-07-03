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

- [ ] `pie_capture_from_pose` — GUI-gated: capture from a saved pose, observe the PNG on disk
  (exists, non-zero, expected dimensions). The pose IS the fixed rig — doctrine-compliant.
- [ ] `pie_inject_input_action` — design to stay non-VERBOTEN: bind a test input action to a
  deterministic observable (C++ test actor flips a UPROPERTY / emits a `[FEATURE]` log marker),
  inject once, observe via `pie_query`/`editor_read_logs`. The injection is the Act; the
  observation is independent state — no play-acting, no navigation.

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
- [ ] `actor_set_property` (`tests/integration/test_actor.py:126`) — echo before/after; re-read via
  `actor_inspect` (BoundsScale == 2.0).
- [ ] `actor_set_transform` dry-run (`test_actor.py:113`) — asserts `dry_run` flag only; follow
  with `actor_inspect` proving the transform unchanged.
- [ ] `level_set_gamemode_override` (`test_actor.py:199`) — echo; observe via `level_inspect`
  world-settings readback.
- [ ] `physics_set_properties` (`test_physics.py:75`) — `success is not False` only; read the
  flags back via `bp_inspect`/`reflection_class_properties`.
- [ ] `physics_set_constraint_motion` (`test_physics.py:237`) — echo; read back via
  `anim_physics_inspect` (constraint swing1 == Free).
- [ ] `mesh_set_physics_asset` (`test_physics.py:260`) — echo; read back via
  `anim_skeletal_mesh_inspect` (physics_asset path).
- [ ] `kinematics_solve` (`test_kinematics.py:134`) — asserts the `verification` key EXISTS, not
  that it passed; assert residual / `reached == true`.

### Blueprint / widget
- [ ] `bp_compile` (`test_blueprint.py:47`) — bare success (the textbook hollow case); add
  `bp_inspect` proving the generated class recompiled.
- [ ] `bp_add_node` print-string (`test_blueprint_graph.py:88`) — node_id echo; read the `InString`
  default back via `bp_list_node_pins`.
- [ ] `bp_delete_node` (`test_blueprint_graph.py:188`) — echo; assert the node is gone via
  `bp_list_node_pins`/`bp_inspect`.
- [ ] `bp_set_event_replication` (`test_blueprint_graph.py:395`) — auditors disagreed whether the
  function_flags assertion at :404 is an independent readback or the mutator's echo; verify, and
  if echo, prove via `bp_get_function_details` on the resolved function. ALSO port the missing bun
  mirror (custom-event + replication test absent from `blueprint_graph.test.ts`).
- [ ] `bp_create_variable` dry-run (`test_blueprint.py:103`) — echo `dry_run:true`; assert the
  variable is ABSENT via `bp_read`.
- [ ] Shape-only reads (one task): `bp_brief`, `bp_read`, `bp_list_graphs`,
  `bp_function_references` assert "non-empty dict"; assert one concrete known field each.
- [ ] `widget_set_property` (`test_widget.py:78`) — setter's own before/after; re-read via
  `widget_tree_read` (IsEnabled == false on the Button).
- [ ] `widget_bind_handler` (`test_widget.py:94`) — echo event_name; prove the OnClicked binding
  exists via `widget_tree_read`/`bp_read` of the WBP.

### Material / niagara / mesh / asset / editor
- [ ] `material_connect` (`test_material.py:108`) — connection never read back; assert via
  `material_read` that Multiply.A sources `c3` and BaseColor sources `mul`.
- [ ] `material_set_expression_property` (`test_material.py:124`) — echo; assert the constant
  value via `material_read`.
- [ ] `material_instance_set_parameter` (`test_material.py:209`) — asserts the param NAME (also
  present on the parent — proves nothing); assert the VALUE via `material_read_instance`.
- [ ] `material_apply_to_actor` (`test_material.py:301`) + `material_apply_to_blueprint` (:321) —
  "info non-empty" only; assert slot-0 material_path via `mesh_get_actor_material_info` /
  `get_blueprint_material_info`.
- [ ] `material_compile` (`test_material.py:63`) — echo errors field; at minimum assert
  `material_read` reflects a valid compiled graph.
- [ ] **Niagara ECHO cluster (one task, same pattern):** `niagara_emitter_add_renderer` (:143),
  `niagara_renderer_set_material` (:157), `niagara_renderer_set_material_binding` (:175),
  `niagara_module_set_input` (:229), `niagara_scratch_pad_module_add` (:239),
  `niagara_user_parameter_set` (:273), `niagara_user_parameter_remove` (:280) — the oracle is
  already imported in each case (`niagara_emitter_read` / `niagara_module_get_inputs` /
  `niagara_system_read`); read the written value / absence back.
- [ ] `niagara_script_create` (`test_niagara.py:289`) — echoed path only; assert the `.uasset` on
  disk like `niagara_system_create` does.
- [ ] `mesh_set_static_mesh_properties` (`test_mesh.py:212`) — echo (the comment admits it); read
  the component back via `bp_list_components` asserting the assigned StaticMesh.
- [ ] `asset_fixup_redirectors` (`test_asset.py:176`) — asserts `found is not None`; the test
  already manufactures a known redirector — assert THAT redirector is in the found set.
- [ ] `editor_screenshot` (`test_screenshot.py:33`) — string-match on ".png"; poll for the file on
  disk like `editor_window_screenshot` does.
- [ ] `input_create` (`test_reads.py:163`) — echo; confirm via `asset_list` with
  `class_filter: InputAction` (house pattern from `physics_material_create`).

### Animation / IK / state machines / StateTree
- [ ] `anim_list_sequences` — tagged in FIVE `@covers` decorators (`test_animation.py:363-422`) but
  NEVER invoked (discovery uses `asset_list`) — the ledger is lying. Actually call and assert it.
- [ ] **Anim output-suffix ops (one task):** `anim_extract_between_notifies` (:389),
  `anim_smooth_sequence` (:404), `anim_normalize_z_offset` (:418), `anim_anchor_feet_to_floor`
  (:435) — all trust the echoed output_path; assert the new `.uasset` exists on disk.
- [ ] `anim_sequence_set_property` (`test_animation.py:372`) — echo; NO read primitive exists for
  sequence properties — add an `anim_sequence_read` (or extend an existing read), then assert.
- [ ] `anim_node_bind_property` (`test_animation.py:181`) — currently passes on success OR error;
  prove the binding via a node-pin/variable readback.
- [ ] **IK echo cluster (one task):** `ik_retarget_auto_map_chains` (:120 — assert mappings
  populated via `ik_retarget_read`), `ik_retarget_set_pelvis_settings` (:163) /
  `set_root_motion_settings` (:174) — read written values back; `ik_retarget_run_batch` (:221) —
  assert new_assets on disk. Note: these skip in the stock fixture (no IKRigDefinition ships);
  arranging a minimal IK rig fixture is part of the task — if infeasible, #DEFERRED with reason.
- [ ] `anim_state_machine_modify_transition` (`test_statemachine.py:213`) — sets blend 0.35 /
  priority 2 but only re-checks the edge exists; read the values back.
- [ ] `anim_state_machine_set_entry` (`test_statemachine.py:268`) — echo; read the entry target back.
- [ ] `bp_set_inner_node_property` (`test_statemachine.py:283`) — echo value=="5"; re-read the
  inner-node property (add the read counterpart if missing).
- [ ] `st_set_node_property` (`test_statetree.py:333`) — echo; re-read via
  `statetree_node_get_properties` (pattern already used two tests up).
- [ ] `statetree_compile` (`test_statetree.py:386`) — own compilation_status; assert
  `statetree_verify.ready_to_run` flips true after compile.

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

- **Niagara setter quartet** — `niagara_emitter_set_local_space`, `niagara_renderer_set_enabled`,
  `niagara_renderer_set_alignment`, `niagara_mesh_renderer_set_mesh` (2026-07-02) — needs reader
  extension + rebuild; blocked by shared editor. Investigated in full: none of the four written
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
  full editor rebuild: extend the reader, then the four tests are straightforward
  (arrange NS + emitter + sprite/mesh renderer under /Game/__MCPTest__/, act the setter,
  observe via the extended `niagara_emitter_read`).

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
