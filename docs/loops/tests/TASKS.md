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

- [ ] **PCG domain (9 ops, one task):** `pcg_graph_create`, `pcg_graph_read`, `pcg_list_graphs`,
  `pcg_list_node_types`, `pcg_node_add`, `pcg_node_connect`, `pcg_node_set_property`,
  `pcg_component_add`, `pcg_component_generate` — zero coverage for the whole domain. Arrange:
  `tests/fixtures/TestProject/TestProject.uproject` does NOT enable the PCG plugin — enabling it
  (built-in engine plugin) is part of the task. Observe: topology via `pcg_graph_read` after
  node_add/connect; `pcg_component_generate` via `actor_inspect` on the host actor or a log marker.
- [ ] **Level lifecycle:** `level_new`, `level_load`, `level_save`, `level_save_as` — untested.
  Observe via `level_inspect` (level name / actor count) + on-disk `.umap`. CAUTION: mutates the
  shared editor's open level — MUST restore the baseline map in `finally` and respect the
  multi-agent shared-editor rules.
- [ ] `asset_import_mesh` — generate a minimal OBJ/FBX in the test, import, observe via
  `asset_list` + `mesh_get_bounds` (known extents prove real geometry, not just a package).
- [ ] `asset_import_audio` — generate a WAV with known duration, observe via `asset_list` + a
  SoundWave read (duration field).
- [ ] `asset_import_font` — needs a TTF source (find one in repo/engine or generate; else defer),
  observe via `asset_list` + font asset read.
- [ ] **Landscape (3 ops, one task):** `landscape_inspect`, `landscape_list_layers`,
  `landscape_read_heightmap`. Arrange: a fixture level containing a Landscape (via
  `editor_console_exec` py or a minimal committed fixture map). Deep = assert known component
  counts / heightmap values, not just shape.
- [ ] `foliage_inspect` — needs fixture foliage (an InstancedFoliageActor with one type). If
  arranging needs a missing primitive, add it or defer with reason.
- [ ] `bp_set_component_property` — observe via `bp_list_components`/`bp_inspect` value readback.
- [ ] `bp_set_component_transform` — observe via `bp_list_components`/`bp_inspect` transform readback.
- [ ] `bp_set_class_replication` — observe via `bp_inspect`/`reflection_class_properties`
  (bReplicates on the generated class).
- [ ] `gas_ability_set_cooldown` + `gas_ability_set_cost` — observe via an independent read of the
  generated GA (Cooldown/Cost GE class wired). If no read primitive can see it, add one or defer.
- [ ] **Niagara setters (one task):** `niagara_emitter_set_local_space`,
  `niagara_renderer_set_enabled`, `niagara_renderer_set_alignment`,
  `niagara_mesh_renderer_set_mesh` — each observed via `niagara_emitter_read`.
- [ ] `material_set_property` — observe via `material_read` (e.g. two-sided / blend-mode flip).
- [ ] `mesh_get_bounds` — bun `mesh.test.ts:100-123` tests it DEEP; pytest has nothing. Port the
  parity test (known engine-cube extents). Note the legacy `get_mesh_bounds` wire duplicate for
  the manifest task.
- [ ] `editor_viewport_get_camera` — arrange a known camera pose (console exec / a set-camera
  primitive; add one if missing), read back and assert the pose.
- [ ] `editor_build_reflection_captures` — investigate a log-marker observation via
  `editor_read_logs`; if nothing observable beyond the echo, #DEFERRED with that reason.
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

- [ ] `actor_spawn_physics` — composite never driven (only its sub-op is, raw). Static tier:
  ensureAbsent → act → observe via `actor_inspect`/`actor_query` that the actor exists AND
  simulate-physics landed true (`src/server/src/domains/actor.ts:214`).
- [ ] `result_read` — the tool wrapper is never invoked (only the underlying store is, in
  `compaction.test.ts`). Arrange a compacted payload (low maxBytes), act `result_read`, observe
  chunk === original slice and `next_offset` walks to null.
- [ ] `build_status` — lock logic is tested but the tool never called. Acquire a lock via the
  `__test` hook with a live pid, act `build_status`, observe `in_progress == true`.
- [ ] `editor_read_logs` — marker roundtrip: emit a known marker via `editor_console_exec`, act
  `editor_read_logs` with grep, observe the line. (This also deepens `editor_console_exec`.)
- [ ] `video_analyze` / `pie_analyze` — headless slice only: assert the `feature_disabled` guard
  when no model key is configured (`src/server/src/domains/video.ts:56` path). The full
  structured-verdict run needs the external model → note as partial, defer the rest.
- [ ] `editor_build_game_target` — full offline UBT build (minutes, toolchain-dependent); expected
  outcome: #DEFERRED with that reason unless a cheap observable exists.

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
- [ ] pytest `test_mesh.py` lacks the bun-only `mesh_get_bounds` + engine-refuse socket tests
  (`mesh.test.ts:100-123`, `:229-234`). Port to pytest so `@covers` sees them.
- [ ] `gas_effect_apply` has no positive-path observation anywhere (only the `not_in_pie` guard).
  A PIE-tier test observing an applied effect (attribute readback or log marker) — scope it; if
  it needs primitives that don't exist, add them or defer.

# DEFERRED
(nothing yet — the loop moves items here with reasons)
