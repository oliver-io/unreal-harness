# Usage Guide

The MCP server runs on Bun (TypeScript). It lives at `src/server/` and registers tools through a Zod tool registry (`src/server/src/domains/*.ts` wired up by `register.ts`). The C++ plugin (`src/Plugin/UnrealMCP/`) and the TCP/JSON wire protocol it speaks are unchanged.

How to use the MCP — universal contracts that every tool honors, per-domain operational guidance, and the tool-chain patterns we recommend.

For the *why* (design principles, philosophy, boundaries), see [`ARCHITECTURE.md`](ARCHITECTURE.md).

---

## 1. Universal contracts

Every tool in the MCP honors these. New tools must too.

### 1.1. Response envelope

Every tool returns:

```json
{
  "status": "success" | "error",
  "result": { ... },                  // present on success
  "error": "<human-readable message>", // present on error
  "error_code": "<closed-set code>",   // present on error
  "error_hint": "<imperative remediation>" // present on error when actionable
}
```

`status` semantics are stable: `success` means the operation completed; `error` means it didn't. `error_code` and `error_hint` ship as flat top-level fields (not nested into an `error` object) for wire stability with the legacy `error: <string>` contract — net-add.

Server callers: use the envelope helpers in `src/server/src/bridge/envelope.ts` for the raw string, or the typed `ErrorCode` accessor in `src/server/src/bridge/errors.ts`.

### 1.2. Error code taxonomy (closed set)

| Group | Codes |
|---|---|
| **Identity / lookup** | `asset_not_found`, `class_not_loaded`, `node_not_found`, `actor_not_found`, `variable_not_found`, `pin_not_found`, `function_not_found`, `unknown_tag`, `window_not_found` |
| **Input shape** | `invalid_argument`, `invalid_path`, `invalid_pin_type`, `ambiguous_target`, `out_of_range` |
| **Asset state** | `asset_dirty`, `asset_compile_failed`, `asset_locked`, `name_collision` |
| **Capability** | `unsupported_class`, `not_in_pie`, `pie_active`, `editor_not_ready`, `feature_disabled`, `dry_run_unsupported` |
| **Authority / safety** | `would_break_references`, `circular_dependency` |
| **Engine** | `engine_busy`, `live_coding_unavailable`, `compile_in_progress`, `timeout`, `internal` |

`timeout` means an engine-side operation did not complete within its bounded budget (e.g. an editor-viewport screenshot that never rendered a qualifying frame — see GAP-007 in `docs/BUGS.md`); it is environmental, not a caller-input error.

The set is owned by `EMCPErrorCode` in `src/Plugin/UnrealMCP/Source/UnrealMCP/Public/Commands/MCPCommonUtils.h` and mirrored to `src/server/src/bridge/errors.ts` — those two enumerations are the authoritative list (31 codes as of this writing). Adding a code requires updating both ends. One deliberate exception: the PIE lease codes (`pie_busy`, `pie_lease_lost`, `pie_not_holder`, `pie_takeover_failed`) are synthesized server-side by the lease layer and live outside this set — see §2.18.

**Hint quality bar.** An `error_hint` is good if an agent can act on it without further investigation. Tests:
- "did you mean X" hints carry a candidate name list (top three by edit distance where the registry can produce them).
- "must be Y first" hints name the prerequisite tool.
- "valid values are …" hints enumerate the closed set in the message body.

Hints are optional — omit rather than ship filler.

### 1.3. Auto-save

Every C++ mutator persists its change before returning. The mutation contract:

```
PreEditChange → mutate → PostEditChange → MarkPackageDirty → UEditorAssetLibrary::SaveAsset(path, /*bOnlyIfIsDirty=*/false)
```

Consequence: callers do **not** need to call `asset_save` after a mutation. `asset_save` exists for the rare case where a mutator path is bypassed or for force-flushing a manually-dirtied package. There is no `save_all_dirty` — refused by design (auto-save on mutation makes it redundant, and a bulk flush hides which mutation dirtied what).

### 1.4. Dry-run

Most mutators accept `dry_run: true` and return `result.diff` instead of applying. Validation parity is the invariant: a passing dry-run implies a passing commit absent races. Only the apply step is skipped.

**Bridge-level safety net.** Mutators that don't yet support dry-run return `error_code = dry_run_unsupported` when called with `dry_run = true`, instead of silently applying. The blocklist is **~40 tools**, not a one-off — by category: all `ik_retarget_*` mutators, the `gas_*` creators, `level_*` (new/save/save_as/load), asset-factory creators (`enum_create`, `struct_create`, `datatable_create`, `input_create`, `mpc_create`, `material_function_create`, `niagara_script_create`, `physics_material_create`), the `pie_record_*` lifecycle, the `widget_*` mutators, the anim batch fixups (`anim_anchor_feet_to_floor`, `anim_normalize_z_offset`, `anim_smooth_sequence`), `asset_import_mesh`, `input_add_mapping`, `editor_build_reflection_captures`, and `bp_add_node`. Don't enumerate from this doc — the authoritative lists are `FMCPCommonUtils::IsBlockedFromDryRun(CommandType)` (C++, where the bridge enforces the intercept before dispatch) and its mirror `DRY_RUN_UNSUPPORTED` in `src/server/src/bridge/gates.ts` (used for capability disclosure, e.g. `catalog_describe`).

**Diff shapes per subsystem.**

| Subsystem | Shape |
|---|---|
| Asset CRUD | `created[]`, `deleted[]`, `renamed[{from, to}]`, `moved[{from, to}]` |
| BP authoring | `nodes_added[]`, `nodes_removed[]`, `connections_added[]`, `connections_removed[]`, `properties_changed[{node, prop, before, after}]` |
| Material | `expressions_added[]`, `expressions_removed[]`, `connections_changed[]`, `parameters_changed[{name, before, after}]` |
| Animation | `montage_sections[]`, `notifies[]`, `blend_space_samples[]` |
| StateTree / EQS | `states[]`, `transitions[]`, `properties[]` |
| Tag registry | `added[]`, `removed[]`, `moved[]`, `references_affected[]` |
| Scene | `actors_added[]`, `actors_removed[]`, `transforms_changed[]` |
| Other | generic `changes[{path, before, after}]` |

### 1.5. Naming conventions (domain-first canonical)

Every tool follows `{domain}_{verb}(_{modifier})?`. Inspection verbs: `_brief`, `_query`, `_inspect`, `_list`, `_get`, `_read`. Mutation verbs: `_set`, `_add`, `_remove`, `_create`, `_delete`, `_compose`.

Domain prefixes in active use: `actor_`, `bp_`, `material_`, `niagara_`, `anim_`, `widget_`, `statetree_`, `eqs_`, `tag_`, `gas_`, `ik_rig_`, `ik_retarget_`, `asset_`, `editor_`, `pie_`, `class_`, `enum_`, `ai_`, `input_`, `struct_`, `datatable_`, `dataasset_`, `mpc_`, `scene_`, `level_`, `project_`, `pcg_`, `kinematics_`, `landscape_`, `foliage_`.

There is one canonical agent-facing surface: every tool has exactly one name, and for the vast majority of tools the wire name == the tool name == the C++ handler key. The legacy alias map (`ResolveCanonicalCommand`) and the sibling tool registrations were deleted in the naming migration — no open-ended back-compat alias layer remains. Two bounded, test-enforced exceptions to the strict identity exist: an enumerated set of per-tool `command:` overrides where the tool name diverges from the wire/handler key (the 18 `statetree_* → st_*` state/node/transition/binding tools, §2.12, plus `bp_add_node → add_blueprint_node`), and a conservative parameter-alias map (`src/server/src/registry/aliases.ts`) that normalizes a few documented param synonyms (Blueprint identifier, material/asset path — see §2.1) onto the canonical key before dispatch. Both are pinned by `test/gate-error-parity.test.ts` and `test/aliases.test.ts`. Use canonical tool names exclusively.

Lint: `bun run lint:names` (exit code 0 = clean, 1 = violations).

### 1.6. Pagination & cursors

Any tool whose unbounded result could exceed ~1000 entries paginates by default. Pass `cursor` + `limit` (default 200, max 1000). Stable sort by canonical key (path / name) keeps pagination deterministic across calls.

Tail tokens (`since_seq` on `editor_read_logs`, etc.) are opaque cookies — agents must not parse them.

### 1.7. Observability

Single sequenced log stream at `MCP_Unified.log`. GLog + PIE + compile + MCP lifecycle interleaved chronologically. Don't fragment it.

For runtime log read, use `editor_read_logs(category=…, since_seq=…)` — server-side filter, opaque tail-token cursor. The log file is located via the `UNREAL_PROJECT_ROOT` environment variable (the host project root containing `Saved/Logs/`); when unset, `editor_read_logs` returns a structured error naming the variable.

### 1.8. Editor-boot readiness gate

The plugin's TCP listener binds during editor subsystem init — long before the editor is interactive — so "the socket accepts" never means "the editor is ready." Dispatching into that window used to crash editor startup; both layers now gate on a real readiness signal:

- The bridge answers a cheap **`mcp_status`** command synchronously on the network thread (`{ready, phase, pie_active}`) and refuses every other command except `ping` with `error_code: editor_not_ready` until `FEditorDelegates::OnEditorInitialized` fires. The refusal happens before any game-thread dispatch.
- The MCP server polls `mcp_status` before dispatching real commands: a call made while the editor is booting **pends** (up to ~120 s) and then proceeds. Readiness is sticky-cached and re-armed on disconnect, so an editor restart is handled transparently.

Net effect: after an editor relaunch, just issue your command — no manual delay or log-scrape needed. Call `mcp_status` directly to check boot state on demand; it and `ping` are the only calls safe to issue during init.

---

## 2. Per-domain guidance

### 2.1. Blueprint authoring (`bp_*`)

**Authoring sequence:**

```
bp_create_blueprint(parent_class=…)
  → bp_create_variable / bp_add_component        (declare state)
  → bp_create_function                            (declare graphs)
  → bp_add_node / bp_add_event_node              (populate graphs)
  → bp_connect_pins                              (wire)
  → bp_set_node_property / bp_set_default_value  (configure)
  → bp_compile                                    (verify)
  → [auto-saved]
```

**Component management:**

- `bp_add_component`, `bp_list_components`, `bp_remove_component(reparent_children=true)` — invariants: removal with `reparent_children=false` and existing children returns `would_break_references`.
- Physics components: `physics_set_properties`, `physics_set_body_collision`.
- Static-mesh component properties: `mesh_set_static_mesh_properties`.

**Pin operations:**

- `bp_connect_pins(source_node_id, source_pin_name, target_node_id, target_pin_name)`
- `bp_disconnect_pin(node_id, pin_name, target=...)` — no `target` breaks every link on the pin; `target_node_id` + `target_pin_name` severs a single peer.

**CallFunction nodes (`bp_add_node node_type=CallFunction`):** the function is resolved by name across four layers (first hit wins): explicit `target_class`; explicit `target_blueprint` (cross-BP call); the BP's own/inherited functions; then a hardcoded common set — `Actor`/`Pawn`/`Character`/`Controller`/`PlayerController`/`AnimInstance`/`CharacterMovementComponent` and `UKismetSystemLibrary`/`UKismetMathLibrary`/`UGameplayStatics`. Those resolve **without** `target_class`. **Any other library** (a plugin's `U…FunctionLibrary`, `UKismetArrayLibrary`, etc.) **must pass `target_class`** (bare name, UE-prefixed, or `/Script/...` path) — there is no global function-name search (it would risk wrong-overload matches), so omitting it fails node creation. See [`BUGS.md` GAP-022](BUGS.md).

**Event nodes:** `bp_add_event_node(event_name)` — common: `ReceiveBeginPlay`, `ReceiveTick`. Returns `function_not_found` with the common list in `error_hint` if the name isn't a recognized event.

**Pin defaults:** route through `bp_set_node_property` for nested pin trees, or `bp_set_default_value` for top-level inputs.

**Inner-node property writes:** `bp_set_inner_node_property` for reflection-based property writes against nested anim-graph node bindings and similar embedded structs.

**Reparenting:** `bp_reparent(bp_path, new_parent_class)` — auto-compiles and saves.

**Parameter alias:** every BP tool accepts `bp_path` (canonical) or `blueprint_name` / `blueprint_path` (legacy). Net-add.

### 2.2. Blueprint inspection

Multi-resolution rung:

| Resolution | Tool | Returns |
|---|---|---|
| Counts only | `bp_brief` | `parent_class`, `variables_count`, `functions_count`, `components_count`, `graphs[]`, `blueprint_type`, `has_scs` |
| Full structure | `bp_read` | Complete BP dump (variables, functions, components, graphs, nodes, pins) |
| Graph analysis | `bp_inspect` | Per-graph node + connection topology |
| Per-variable | `bp_get_variable_details(var_name)` | Type, default, replication, metadata flags |
| Per-function | `bp_get_function_details(function_name)` | Parameter list, return type, BP-callable flags |
| Parent | `bp_get_parent_class` | First-class parent UClass lookup |
| Components only | `bp_list_components` | Flat list with `parent_component` / `child_count` per entry |
| Cross-references | `bp_function_references(direction=callers|callees)` | Symmetric row shape: `counterpart_function`, `blueprint_path`, `graph_name`, `node_id`, `node_name`. Inbound iterates loaded BPs only (`scanned_loaded_blueprints_only=true` in response) |
| Node pins | `bp_list_node_pins(node_id)` | Pin names + types + directions |
| Graphs | `bp_list_graphs` | Every graph the BP owns |

### 2.3. Reflection (`class_*`, `enum_*`)

Read-only — never dirties packages.

| Tool | Purpose |
|---|---|
| `class_query` | Find UClasses by `name_pattern` + `parent` + `recursive` + `include_hidden`. Paginated. Distinguishes loaded vs. unloaded. |
| `class_inspect(class_name, include=['properties'|'functions'|'hierarchy'])` | Unified per-class introspection. Defaults to `['properties']` for back-compat with legacy `get_class_properties`. Honors `include_inherited`. |
| `enum_inspect` | UEnum members: `name`, `display_name`, `value`, `tooltip`, `is_hidden`. Reports `cpp_form` (enum_class / namespaced / regular) and `is_user_defined`. Excludes `_MAX` sentinel. |
| `reflection_class_properties` | Legacy UPROPERTY reflection — subsumed by `class_inspect`. Kept as alias. |

**Constraints:** hidden / HideDropDown / Deprecated / NewerVersionExists classes filtered from `class_query` unless `include_hidden=true`. Unbounded results paginate (default 200, max 1000).

### 2.4. Scene & Level (`scene_*`, `actor_*`, `level_*`, `project_*`)

**Orient sequence:** `project_context` → `level_inspect` → `scene_brief` → `actor_inspect` (drill-in).

| Tool | Returns |
|---|---|
| `project_context` | `name`, `engine_version`, `plugins[]`, `modules[]`, `default_map`, `settings_paths[]` |
| `level_inspect` | `name`, `path`, `world_type`, `persistent_level{path, actor_count}`, `world_settings`, `sublevels[]` (with `is_loaded`/`should_be_loaded` per entry), `sublevel_count`, `sublevels_loaded`, `sublevels_unloaded` |
| `scene_brief` | `total_actors`, `by_class{class: count}` (sorted descending), `distinct_classes[]`, `skipped_sublevels[]` |
| `actor_inspect` | `name`, `class`, `label`, `folder_path`, `transform`, `tags`, `mobility`, `components[]` (flat with `attach_parent`/`attach_socket`), `key_properties` |
| `actor_get_in_level` | Every actor in the current level — `name`, `label`, `class`, `transform` per entry |

**`key_properties` curation per class family** (documented in `EmitActorKeyProperties`):

- `AStaticMeshActor` → `static_mesh`, `num_materials`
- `ASkeletalMeshActor` → `skeletal_mesh`, `anim_class`
- `APawn` → `controller`, `controller_class`
- `ACharacter` (also APawn keys) → `max_walk_speed`, `jump_z_velocity`, `gravity_scale`
- `ACameraActor` → `fov`, `aspect_ratio`, `constrain_aspect_ratio`
- `ALight` → `light_intensity`, `light_color {r,g,b,a}`, `light_affects_world`

Multiple-family inheritance merges all contributions into one `key_properties` object.

**Querying actors:** `actor_query` — six AND-composed filter axes:

| Filter | Behavior |
|---|---|
| `name_pattern` | Case-insensitive substring on `GetName()` OR `GetActorLabel()` |
| `class` | UClass path (script / asset with `_C` suffix / short name with `A`-prefix fallback). `direct_only=true` skips `IsA` recursion |
| `tag` | String or array; multi-tag matches ALL |
| `label` | Exact case-insensitive match on editor folder path |
| `bbox` | `FBox` containment on actor location (not bounds intersection) |
| `distance_from` | Origin `{x,y,z}` + radius (cm) |

Iterates already-loaded sublevels only — reports `skipped_sublevels[]` so the caller can decide whether to stream.

**Spawning:** `actor_spawn(class, transform, name?, tags?, folder_path?, scale?)`. Validates UClass loadability up front. **Pawns do NOT auto-possess** — wire the Controller separately.

**Other scene tools:**
- `actor_set_transform`, `actor_delete`, `find_actors_by_name` (legacy parallel to `actor_query(name_pattern=…)` — kept as an alias-map exemption since the two have different semantic shape)
- `mesh_get_actor_material_info` — per-actor material assignments
- `actor_set_property(name, property, value, save?)` — reflection write of any edit-exposed UPROPERTY on a placed actor via a dotted path (e.g. `DirectionalLightComponent.Intensity`); visible live but NOT saved to the .umap unless `save=true`. Supports dry_run.
- `actor_spawn_physics` — server-side composite: builds a throwaway Blueprint (StaticMesh + physics + optional `[R,G,B(,A)]` color), compiles it, then spawns it. PIE-blocked like its constituent commands.

**Mesh asset tools (`mesh_*`)** — operate on the mesh *asset*, not a placed actor:

- `mesh_get_bounds(static_mesh_path)` — read-only LOCAL-space bounds of a `UStaticMesh` (`local_bounds{origin, box_extent, sphere_radius}`, `box_min`/`box_max`, `size`) — the asset-space counterpart to `actor_inspect`'s per-component `world_bounds`. Engine content allowed.
- `mesh_get_collision(asset_path)` / `mesh_set_collision(asset_path, shape, …)` — read / author simple collision on a `UStaticMesh` (headless Static Mesh Editor → Collision menu). `shape` ∈ `box|sphere|capsule|kdop10_x/y/z|kdop18|kdop26|convex|none`; convex takes `hull_count`/`max_hull_verts`/`hull_precision`; optional `collision_trace_flag` sets the body-setup complexity; `replace_existing=true` by default. Engine content refused; PIE-blocked.
- **Static-mesh socket CRUD:** `mesh_list_sockets` (resolve by asset path OR by a level actor's StaticMeshComponent — the actor path additionally returns each socket's resolved world transform) → `mesh_add_socket` / `mesh_modify_socket` / `mesh_remove_socket` (dry_run-able; PIE-blocked; engine content refused). Sockets are the scale-proof home for muzzle/attach/grip points — `GetSocketTransform` composes the relative transform with the component's live world transform *including scale*, killing the "hand-tuned offset × hold-fit scale" class of bug. Skeletal-mesh sockets are the `anim_skeleton_*_socket` family (§2.7).
- `mesh_set_physics_asset(path, physics_asset)` — repoint a `USkeletalMesh`'s PhysicsAsset (`""` clears the binding). PIE-blocked.
- `mesh_build_bend_chain(path, num_bones?, axis?, base_fraction?, segment_ratio?, …)` — heavyweight procedural re-skinner: (re)builds a Root→tip bone chain up one axis of a skeletal mesh and re-skins every vertex to it by position (smooth two-bone joint blends, segments shortening toward the tip) so the mesh can bend. Idempotent; saves the mesh **and** its bound skeleton; supports dry_run (returns the bone-station table without mutating); PIE-blocked.

**Level persistence (`level_*` mutators)** — the four world-lifecycle tools are all PIE-blocked and refuse dry_run:

- `level_new(template?)` — new blank (or template-copied) level replacing the current editor world. **The outgoing world is NOT auto-saved** — call `level_save` first to keep it. The new world is a transient `/Temp/` map until `level_save_as`.
- `level_save` — save the current world to its existing on-disk package; errors `invalid_path` on a `/Temp/` untitled level (use `level_save_as` first).
- `level_save_as(package_path)` — headless File → Save Current As; creates the package and makes it the active level.
- `level_load(package_path)` — open an existing level, replacing the current world (no save prompt for the outgoing world).
- `level_set_gamemode_override(level_path, gamemode_class)` — World Settings → GameModeOverride; BP path (auto-`_C`) or native `/Script/...` path; `""` / `"None"` clears. Saves the `.umap`.

### 2.5. Asset references (`asset_references`)

Single direction-aware tool. Inputs:

- `asset_path` (package or object path, suffix stripped)
- `direction` — `"outbound"` (what this uses) | `"inbound"` (what uses this)
- `depth` (1..10)
- `cursor` + `limit` (default 200, max 1000)
- Per-kind toggles: `include_hard`, `include_soft`, `include_searchable_name`

Routes through `IAssetRegistry::GetDependencies` / `GetReferencers`. BFS with visited-set cycle dedupe. 5s wall-clock budget — partial results carry `wall_clock_capped=true`.

`direct=true` only at depth 1; deeper hits are `direct=false`. `Manage` references are skipped — only useful for `UAssetManager`-driven projects with explicit primary-asset rules.

### 2.6. Materials (`material_*`, `mpc_*`)

**Material authoring:**

```
material_create → material_add_expression → material_connect → material_set_expression_property → material_compile → [auto-saved]
```

- `material_delete_expression(material_path, expression_name, dry_run?)` — remove a node from the graph; wires to/from it go null (dry-run enumerates the severed connections).
- `material_set_property(material_path, blend_mode?, two_sided?, shading_model?, material_domain?)` — flip top-level `UMaterial` flags after creation (e.g. Translucent→Opaque); recompiles + saves. `material_create` only sets these at creation time.

**Material instance:**

```
material_create_instance → material_instance_set_parameter → [auto-saved]
material_reparent_instance(new_parent)
```

**Material function:** `material_function_create(path, name, description?, expose_to_library?)` — empty graph; populate via `material_add_expression` (operates on any `UMaterialFunctionInterface`).

**Material Parameter Collection:** `mpc_create(path, name, parameters?)` — `parameters[]` seeds the CDO's `ScalarParameters` / `VectorParameters`. Each entry: `{type: "scalar"|"vector", name, default_value}`. Scalars take a number; vectors take `{r,g,b,a}`.

**Apply:**

- `material_apply_to_actor(actor, material, slot?)`
- `material_apply_to_blueprint(bp, material, slot?)`
- `mesh_set_static_mesh_material(actor, slot_index, material)` — specific slot
- `mesh_set_mesh_material_color(actor, slot_index, r, g, b, a?)` — vector-parameter color shortcut

**Read:** `material_read`, `material_read_function`, `material_read_instance`, `material_get_available`.

### 2.7. Animation (`anim_*`)

**Skeleton inspection chain:**

```
anim_list_skeletons → anim_skeletal_mesh_inspect → anim_skeleton_list_sockets
```

**Socket management:**

- `anim_skeleton_add_socket(skeleton, bone, socket_name, transform)`
- `anim_skeleton_modify_socket`, `anim_skeleton_remove_socket`

**Skeletal mesh:** `anim_skeletal_mesh_set_section_disabled(mesh, lod, section, disabled)` — hide a bodypart at runtime.

**Physics asset:** `anim_physics_inspect`, `physics_set_body_collision`, and `physics_set_constraint_motion(path, joint_name? | bone1+bone2, delete?, swing1/swing2/twist/linear_x/y/z?)` — edit or delete a constraint in a `UPhysicsAsset`'s ConstraintSetup. Key it by `joint_name` OR the `(bone1, bone2)` pair (order-insensitive; preferred); per-axis motions take `"Free"`/`"Limited"`/`"Locked"` (omitted axes unchanged), or `delete=true` removes the constraint. Run `anim_physics_inspect` first. PIE-blocked.

**Anim sequences:** `anim_list_sequences`, `anim_sequence_set_property(asset, property_name, value)`.

**Montages:**

```
anim_montage_create(skeleton, anim_sequence) → anim_montage_add_section → anim_montage_set_section_link → anim_montage_set_blend
```

**Notifies:**

```
anim_notify_add(asset, trigger_time, notify_class) → anim_notify_remove(asset, notify_index)
```

Closed-set property hints on `additive_anim_type` and `base_pose_type` enumerate engine source enum members.

**Clip processing (batch fixups):** all four are PIE-blocked.

- `anim_extract_between_notifies(source_path, dest_name, start_notify?/end_notify?/start_time?/end_time?)` — slice a sub-clip into a **new** asset. Boundaries by notify name or explicit time (notify wins); `start_occurrence`/`end_occurrence` pick among repeated notifies; `dest_path` defaults to the source's folder.
- `anim_smooth_sequence(anim_path, window_size?, filter_type "box"|"gaussian", sigma_frames?, bone_substring_filter?, smooth_positions?)` — sliding-window smoothing of bone tracks (window forced odd; 3 mild, 5 balanced, 9+ aggressive).
- `anim_normalize_z_offset(anim_path, target_z?, bone_substring_filter? default ["root"])` — rebase so frame-0 bone Z lands at `target_z`, subtracting one Z delta from every frame.
- `anim_anchor_feet_to_floor(anim_path, foot_bone_substring?, pelvis_bone_substring?, target_z?, sample_frames?)` — FK-compose foot world Z over the leading frames (median) and shift the pelvis Z curve so feet rest at floor level. The typical post-retarget fixup.

**Data-loss foot-gun:** the three track-mutating fixups (`anim_smooth_sequence`, `anim_normalize_z_offset`, `anim_anchor_feet_to_floor`) have **no dry_run** (`dry_run_unsupported`). Their only safety is `output_suffix`, which defaults to writing a suffixed copy (`_Smoothed` / `_ZNorm` / `_FootAnchored`) — **an empty `output_suffix` mutates the source sequence in place**, with no preview and no undo. Leave the default unless you mean it.

**Blend spaces:**

```
anim_blend_space_create(skeleton, type) → anim_blend_space_add_sample(sample, x, y) → anim_blend_space_remove_sample
```

**AnimBP authoring:**

```
anim_blueprint_create(skeleton) → anim_blueprint_set_skeleton
  → anim_state_machine_create(anim_bp, graph_name)
  → anim_state_machine_state_add → anim_state_machine_set_entry
  → anim_state_machine_transition_add → anim_state_machine_modify_transition
  → anim_node_bind_property(anim_node, property, variable)
```

Inner anim-node property writes route through `bp_set_inner_node_property` — the reflection-based bypass-the-private-header path defends against engine refactoring `UAnimGraphNodeBinding`'s internal layout.

### 2.8. IK retargeting (`ik_retarget_*`, `ik_rig_*`)

Full `UIKRetargeter` authoring — create, wire rigs, map chains, align, tune ops, import retarget poses — plus cross-skeleton batch retargeting. **`UIKRigDefinition` authoring (chains / goals / solvers) is the part that stays unexposed by design** — rigs are read-only via `ik_rig_list_chains`; author them in the editor or use vendor rigs.

**Typical authoring flow:**

```
ik_rig_list_chains (source + target rigs — discover chain names)
  → ik_retarget_create (optionally wiring both rigs) → ik_retarget_set_rigs
  → ik_retarget_auto_map_chains → ik_retarget_set_chain_mapping (manual fixes)
  → ik_retarget_align_bones / ik_retarget_set_pelvis_settings / ik_retarget_set_root_motion_settings
  → ik_retarget_import_pose_from_animation | ik_retarget_import_pose_from_pose_asset
  → ik_retarget_run_batch → ik_retarget_read (verify)
```

| Tool | Behavior |
|---|---|
| `ik_rig_list_chains(ik_rig_path)` | Per chain: name, start/end bones, IK goal; plus pelvis bone + preview mesh. Read-only. |
| `ik_retarget_create(asset_path \| path+name, source_ik_rig_path?, target_ik_rig_path?)` | Factory runs `AddDefaultOps` → standard FK+IK op stack; optionally wires rigs in the same call. |
| `ik_retarget_set_rigs(retargeter_path, source/target_ik_rig_path, rebuild_ops=true)` | At least one rig required. `rebuild_ops=true` (default) wipes + re-adds the op stack so chain mappings re-anchor; `false` preserves a vendor retargeter's baked-in op state. |
| `ik_retarget_auto_map_chains(retargeter_path, match_type="Fuzzy", force_remap=true, align_target_pose=true)` | "Map All" — both rigs must already be set. `match_type` ∈ `Exact` / `Fuzzy` (Levenshtein) / `Clear`. `force_remap=false` fills only unmapped chains. |
| `ik_retarget_set_chain_mapping(retargeter_path, target_chain, source_chain)` | One mapping (manual fix after auto-map); empty `source_chain` clears it. |
| `ik_retarget_align_bones(retargeter_path, source_or_target="target", reset_first=true, excluded_bones[])` | `AutoAlignAllBones` on one side; `excluded_bones` are reset back to bind pose AFTER align (for garbage single-bone chains). |
| `ik_retarget_set_pelvis_settings(retargeter_path, …)` | Tunes the Pelvis Motion Op (UE 5.7 ops architecture); only passed params are written. `scale_vertical=0` locks pelvis Z (kills SMPL-noise knee bounce). Errors if the retargeter has no Pelvis Motion op. |
| `ik_retarget_set_root_motion_settings(retargeter_path, …)` | Tunes the Root Motion Op. `root_height_source="snap_to_ground"` locks root Z to 0 — the fix for body-bob + knee compensation that pelvis settings don't affect. |
| `ik_retarget_import_pose_from_animation(retargeter_path, anim_sequence_path, source_or_target="source", frame_index=0, make_current=true, exclude_bone_substrings?)` | Samples one frame of a `UAnimSequence` as a retarget pose. Source pose at an idle frame ⇒ delta ≈ 0 ⇒ target outputs its bind pose at idle. Pelvis never excluded. |
| `ik_retarget_import_pose_from_pose_asset(retargeter_path, pose_asset_path, source_or_target="source", pose_name="", make_current=true)` | Imports a `UPoseAsset` named pose (vendor `PA_*RetargetPose` assets for novel source skeletons — SMPL, mocap). Empty `pose_name` = first pose. |
| `ik_retarget_run_batch(retargeter_path, source_animations[], name_search/replace/prefix/suffix, include_referenced_assets=true, overwrite_existing=false)` | `UIKRetargetBatchOperation::DuplicateAndRetarget`. Inputs may be `UAnimSequence` / `UBlendSpace` / `UAnimMontage`; source/target meshes are read from the rigs. Response includes `new_assets[]` for follow-up chain operations (rename, move, `asset_references`). |
| `ik_retarget_read(retargeter_path)` | Source/target rig paths + current chain mappings (empty source = unmapped). Read-only. |

**`ik_retarget_run_batch` validation chain** (each step has a structured-error code):
- retargeter not found → `asset_not_found` (wrong class → `unsupported_class`)
- controller null → `engine_busy`
- source / target rig not configured → `asset_locked`
- source / target mesh not assigned on rig → `asset_locked`
- any source animation path fails to load → `asset_not_found` with `result.missing_animations` list
- empty `source_animations` → `invalid_argument`

**Foot-guns:**

- Every `ik_retarget_*` mutator is **PIE-blocked and `dry_run`-unsupported** (both C++ blocklists; mirrored in `src/server/src/bridge/gates.ts`). Each is an atomic asset side effect: it saves the retargeter itself before returning (`SaveAsset`, even if not dirty) — a save failure returns `internal` with a "PIE is likely active or the package is read-only" hint.
- `overwrite_existing=false` (the default, UE's safer behavior) resolves batch-output name collisions with a unique numeric suffix instead of clobbering.

### 2.9. UMG / Widgets (`widget_*`)

**Authoring sequence:**

```
widget_create(parent_class="UserWidget")
  → widget_add_child(child_class, parent_name?, child_name?)
  → widget_set_property(widget_name, property_name, property_value, target="widget"|"slot")
  → bp_compile (required before bind_handler!)
  → widget_bind_handler(widget_name, event_name)
```

**`widget_add_child` modes:**

- `parent_name` empty → seats as root widget. Refuses with `asset_locked` if a root already exists.
- `parent_name` set → DFS find by name, validate parent is a `UPanelWidget`, attach via `ParentPanel->AddChild`. Non-panel parents return `unsupported_class` per "slot type is locked by parent" invariant.

Response includes `slot_class` for follow-up slot-property writes.

**`widget_set_property` targets:**

- `target="widget"` (default) — UPROPERTY on the UWidget instance (IsEnabled, Text, Brush, Color, …)
- `target="slot"` — UPROPERTY on the widget's `UPanelSlot` (CanvasPanelSlot anchors/position/alignment, HorizontalBoxSlot fill, GridSlot row/column, …)

`property_value` accepts any JSON shape — FProperty type-dispatch handles conversion. Common type→shape mapping:

- `FLinearColor` → `{r, g, b, a}`
- `FAnchors` → `{Min: {X, Y}, Max: {X, Y}}`
- `FMargin` → `{Left, Top, Right, Bottom}`

Response captures `before` / `after` via `ExportTextItem_Direct` for confirmable diffs.

**`widget_bind_handler` prerequisites:**

- WBP must have a `GeneratedClass` (compile-first)
- The named widget must have an `FObjectProperty` on the WBP's GeneratedClass — generated by the BP compiler from the SCS

Canonical sequence: `widget_add_child` → `bp_compile` → `widget_bind_handler`. Bind calls `FKismetEditorUtilities::CreateNewBoundEventForComponent` (same path the editor's details-panel "Add Event" button uses). Idempotent — repeat calls don't produce duplicates.

**Inspection:** `widget_tree_read` — flat tree dump (every widget, class, parent, slot config). Read-only.

### 2.10. GAS — Gameplay Ability System (`gas_*`)

Two tiers: **asset authoring** (mutates assets, auto-saves) and **runtime application** (operates on PIE world).

**Asset authoring:**

- `gas_ability_create(path, name, parent_class?, cost_tag?, cooldown_tag?, tags{}?)` — wraps `UBlueprintFactory` with `UGameplayAbility` parent.
- `gas_effect_create(path, name, parent_class?, duration_policy?)` — `duration_policy` ∈ `{"Instant", "Duration", "Infinite"}` (closed set; case-insensitive).
- `gas_attributeset_create(path, name, parent_class?)` — Blueprintable scaffolding tier. Production AttributeSets remain C++. Response carries `is_scaffolding: true` with a migrate-to-C++ hint.
- `gas_ability_set_cost(ability_path, effect_class)` / `gas_ability_set_cooldown(ability_path, effect_class)` — bind the COST / COOLDOWN GameplayEffect class on a `UGameplayAbility` Blueprint (the engine's actual cost/cooldown model — committing the ability applies that GE; a cooldown's duration + tags live on the GE itself). `effect_class` takes a GE Blueprint path (auto-`_C`), native class, or short name; `""` clears the binding. Auto-saves; PIE-blocked; no dry_run.

Tag inputs validate against `UGameplayTagsManager` and return `unknown_tag` with `tag_add` in the hint when missing.

**Runtime application** (requires PIE — returns `not_in_pie` otherwise):

- `gas_effect_apply(target_actor, effect_class, level?, instigator?)` — routes through `UAbilitySystemComponent::ApplyGameplayEffectToTarget`. Validates: PIE active → target in PIE world → target has ASC → effect_class resolves → effect_class is `UGameplayEffect` subclass.

**Hard dependency** on the tag registry (`tag_*`) — see §2.11.

### 2.11. Gameplay Tag Registry (`tag_*`)

| Tool | Behavior |
|---|---|
| `tag_add(tag, comment?, source?)` | `IGameplayTagsEditorModule::AddNewGameplayTagToINI`. `source` targets a specific tag-source list (default `DefaultGameplayTags.ini`). |
| `tag_remove(tag, force?)` | Gathers asset references via `IAssetRegistry::GetReferencers` (tags appear as `SearchableName` dependencies). Returns `would_break_references` with the full referencer list on `result.referencers` unless `force=true`. |
| `tag_list(prefix?, include_dev_comments?)` | Alphabetical sort. Prefix filter case-insensitive. |
| `tag_move(from_tag, to_tag, rename_children?)` | Renames in INI + writes a redirector (always — UE 5.7's API has no opt-out). Reports `redirector_written: true`. |

**Path validation** rejects empty segments, leading/trailing dots, spaces, and filesystem-reserved chars.

### 2.12. StateTree (`statetree_*`) — preferred over Behavior Trees

UE 5.4+ replacement for Behavior Trees + Blackboard. The full surface ships under canonical `statetree_*` tool names, but the wire commands / C++ handler keys for the state / node / transition / binding operations remain `st_*` — each of those 18 tools declares a per-tool `command:` override in `src/server/src/domains/statetree.ts` (e.g. `statetree_state_add` → `st_add_state`), with parity test-enforced (`test/gate-error-parity.test.ts`; see the namespace note in `src/server/src/bridge/gates.ts`). The seven tree-level tools (`statetree_create` / `_read` / `_compile` / `_save` / `_verify` / `_list_node_types` / `_list_schemas`) use their tool name on the wire:

| Tool | Purpose |
|---|---|
| `statetree_create` / `statetree_read` / `statetree_compile` / `statetree_save` / `statetree_verify` | Asset lifecycle |
| `statetree_list_node_types` / `statetree_list_schemas` | Discover available task / condition / consideration types |
| `statetree_state_add` / `_remove` / `_rename` / `_move` / `_duplicate` / `_set_properties` / `_list` | State CRUD |
| `statetree_node_add` / `_remove` / `_set_property` / `_get_properties` | Inline node (task / condition / consideration) |
| `statetree_transition_add` / `_remove` / `_set_properties` | Transition CRUD. Event-driven transitions: `trigger="OnEvent"` + `event_tag=<registered gameplay tag>` writes the transition's `RequiredEvent.Tag` (unregistered tags → `unknown_tag` error; empty string on the setter clears; the read emits `event_tag` when set) |
| `statetree_binding_add` / `_remove` / `_list` / `_list_bindable` | Property bindings — the Blackboard equivalent |

There is no "entry state" tool because StateTree has no entry-state designator: runtime selection starts at the root and tries root-level states **in tree order**, entering the first whose conditions pass (per each state's `selection_behavior`). Control that order with `insert_index` on `statetree_state_add` or with `statetree_state_move`; set `selection_behavior` at add time or via `statetree_state_set_properties`.

**Canonical authoring sequence:**

```
statetree_create
  → statetree_state_add × N (tree order = selection priority; use insert_index / statetree_state_move)
  → statetree_node_add(task | condition | consideration) per state
  → statetree_transition_add × N
  → statetree_binding_add (property → state's input)
  → statetree_compile → statetree_save (auto-saved)
  → statetree_verify (post-rebuild)
```

**Important:** after any C++ rebuild touching task instance data or schema types, call `statetree_save` via MCP — `ValidateStateTreeReference` silently fails otherwise.

### 2.13. EQS — Environment Query System (`eqs_*`)

| Tool | Behavior |
|---|---|
| `eqs_create(asset_path)` | Create empty EQS query asset |
| `eqs_read` | Dump generators / tests / properties |
| `eqs_list_types` | List schema node types available to add |
| `eqs_option_add(generator_class)` / `eqs_option_remove(option_index)` | Generator options |
| `eqs_test_add(option_index, test_class)` / `eqs_test_remove(option_index, test_index)` | Tests per option |
| `eqs_set_property(option_index, test_index?, property_name, value)` | Property writes on generator (no `test_index`) or test |

### 2.14. Niagara (`niagara_*`)

All `niagara_*` mutators are PIE-blocked. Emitter-scoped tools select the emitter via `emitter_name` or `emitter_index`.

**Inspection:**

- `niagara_list_systems` / `niagara_system_read` / `niagara_emitter_read` / `niagara_module_get_inputs`

**System / emitter authoring (create → structure → configure):**

```
niagara_system_create(system_path)                      (empty system — no emitters)
  → niagara_emitter_add(emitter_name, sim_target?)      (blank-but-valid emitter, "cpu"|"gpu")
  → niagara_module_add(target_usage, module_script_path) (insert an engine UNiagaraScript stack module)
  → niagara_module_set_input(module, input, value)       (configure it)
  → niagara_emitter_add_renderer(renderer_type, material_path?)
```

- `niagara_module_add` `target_usage` ∈ `{ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate}`; `target_index=-1` appends.
- `niagara_emitter_add_renderer` `renderer_type` ∈ `{"ribbon"` (default)`, "sprite", "mesh"}`; `material_path` binds ribbon/sprite only.
- `niagara_emitter_set_local_space(local_space)` — simulate particles in the emitter/owner frame instead of world space; essential for effects authored relative to a moving actor.

**Renderer configuration** (`renderer_index` picks the renderer, default 0):

- `niagara_renderer_set_material` — (re)bind the material on a ribbon/sprite renderer.
- `niagara_renderer_set_material_binding(user_param_name)` — bind a ribbon/sprite renderer's material to a `User.*` Material parameter (`MaterialUserParamBinding`) so a runtime `SetVariableMaterial` drives it. Create the Material user param first (`niagara_user_parameter_add … "material"`).
- `niagara_renderer_set_alignment(alignment?, facing?)` — SPRITE renderers only. `alignment="velocity"` orients each sprite's long axis along its velocity → streaks instead of round billboards; `alignment` ∈ `{unaligned, velocity, custom, automatic}`, `facing` ∈ `{camera, camera_plane, custom, camera_position, distance_blend, automatic}`.
- `niagara_mesh_renderer_set_mesh(mesh_path, scale?)` — assign the static mesh (+ optional uniform scale) to a MESH renderer. **A mesh renderer added via `niagara_emitter_add_renderer` starts with NO mesh and draws nothing** — this is the missing half. The per-particle material comes from the mesh's own slot 0.
- `niagara_renderer_set_enabled(enabled)` — silence/re-enable a single renderer (a disabled renderer keeps its config but draws nothing — the reliable way to kill a vestigial sprite renderer that co-draws billboards over a mesh emitter). Result lists ALL renderers (index, type, enabled).

**Other mutation:**

- `niagara_emitter_set_enabled(system, emitter, enabled)`
- `niagara_user_parameter_add` / `_remove` / `_set`
- `niagara_scratch_pad_module_add(system, emitter, module_template)`

**Standalone script creation:**

- `niagara_script_create(usage, path, name)` — `usage` ∈ `{"module", "function", "dynamic_input"}` (closed set; `"dynamicinput"` accepted as typo-robust alias). Creates an asset shell with an empty/seed graph.

The system/emitter **stack** is fully authorable (modules + renderers above), but editing a standalone script asset's HLSL or internal node graph isn't yet exposed — `niagara_script_create` gives you the shell only.

### 2.15. Asset CRUD (`asset_*`)

| Tool | Behavior |
|---|---|
| `asset_list(filter_type?, filter_name?, filter_path?, recursive?)` | Search by type / name / path |
| `asset_rename(path, new_name, dry_run?)` | Rename with redirector |
| `asset_move(from_path, to_path, dry_run?)` | Move with redirector |
| `asset_duplicate(from_path, to_path)` | Duplicate |
| `asset_delete(path, dry_run?)` | Delete |
| `asset_save(path)` | Force flush a manually-dirtied package (rare — mutators auto-save) |
| `asset_open(path)` | Open in the appropriate asset editor |
| `asset_fixup_redirectors(path?)` | Resolve redirectors |
| `asset_textures_import(destination_folder, images[])` | Bulk image → texture import; per-image `settings`: `sRGB`, `compression`, `lod_group` (`"UI"` = never streamed — HUD art without it renders a blurry mip on first open), `mip_gen` (`"NoMipmaps"` for near-1:1 UI), `composite_*` |
| `asset_bake_dynamic_to_static_mesh(actor, destination_path)` | DynamicMesh → StaticMesh bake |
| `asset_datatable_read(path)` | DataTable rows |
| `asset_dataasset_create` / `_read` / `_set_property` | DataAsset CRUD |
| `asset_references(asset_path, direction, ...)` | See §2.5 |

### 2.16. Asset Factory (`enum_*`, `struct_*`, `datatable_*`, `mpc_*`, `material_function_*`, `niagara_script_*`, `input_*`, `physics_material_*`)

Primitive asset creators. All auto-save; all enforce path uniqueness; all use closed-set validation on type-discriminator inputs.

| Tool | Inputs |
|---|---|
| `enum_create` | `path`, `name`, optional `members[]` (each `{name, value, display_name?}`), optional `cpp_form` |
| `struct_create` | `path`, `name`, optional `properties[]` (each `{name, type, default?}`) |
| `datatable_create` | `path`, `name`, `row_struct` (path to a UStruct) |
| `mpc_create` | `path`, `name`, optional `parameters[]` (each `{type: "scalar"|"vector", name, default_value}`) |
| `material_function_create` | `path`, `name`, `description?`, `expose_to_library?` (defaults: empty, `false`) |
| `niagara_script_create` | `usage` (closed: `"module"`/`"function"`/`"dynamic_input"`), `path`, `name` |
| `input_create` | `type` (closed: `"action"`/`"mapping_context"`), `path`, `name`, `value_type?` (for Action: `"boolean"`/`"axis1d"`/`"axis2d"`/`"axis3d"` with friendly aliases) |
| `physics_material_create` | `asset_path`, all optional: `friction`, `static_friction`, `restitution` (validated 0..1), `density`, `friction_combine_mode` / `restitution_combine_mode` (closed: `"Average"`/`"Min"`/`"Multiply"`/`"Max"`) |

**Combine-mode foot-gun:** a `UPhysicalMaterial`'s combine mode only applies when its `bOverride*CombineMode` flag is set — otherwise Project Settings → Physics decides. Passing a combine mode to `physics_material_create` sets the flag for you; omitting it leaves the project default in charge. Bind the result to a body via `bp_set_component_property` on `BodyInstance.PhysMaterialOverride`.

**Note:** Enhanced Input authoring uses direct `NewObject` (UE 5.7 has no dedicated `UFactory` for `UInputAction` / `UInputMappingContext`).

### 2.17. Editor & Diagnostics (`editor_*`)

| Tool | Behavior |
|---|---|
| `editor_console_exec(command)` | Run a console command |
| `editor_read_logs(category?, sources?, since_seq?, limit?)` | Server-side filters; `category` is comma-separated case-insensitive substring on the **inner** category portion of `[SOURCE:Category]` tags; `sources` filters the **outer** SOURCE half (`LOG`, `PIE`, `LIVECODING`, …). Both compose. `since_seq` is an opaque tail-token cursor. |
| `editor_perf_snapshot` | Frame timing (`GAverageFPS`, `GAverageMS`, `GFrameCounter`, `FApp::GetDeltaTime`), memory (`FPlatformMemory::GetStats` in MB), GPU descriptor (`GDynamicRHI->GetName`, `GRHIAdapterName`, `GRHIVendorId`, `GMaxRHIShaderPlatform`). **No GPU per-frame timings** yet. |
| `editor_screenshot` | Viewport screenshot |
| `editor_window_screenshot(tab_name?)` | Specific tab. Empty `tab_name` falls back to active tab. Saves to `Saved/MCPScreenshots/<timestamp>_<tab>.png`. `window_not_found` if tab name doesn't resolve. |
| `editor_viewport_get_camera` | Read the active level-editor PERSPECTIVE viewport camera pose: `{location, rotation, fov, aspect, ortho, viewport_size}`. Prefers the last-focused viewport. The "record my framing" half of the capture rig — feed the pose to `pie_capture_from_pose` (§2.18). If `ortho=true` the fov is meaningless: frame a perspective viewport instead. |
| `editor_content_browser_refresh(path?, force_rescan?)` | `IAssetRegistry::ScanPathsSynchronous`. Default path `/Game`; `force_rescan=true` by default. Logical content paths only (rejects raw filesystem paths). |
| `editor_build_reflection_captures(save?)` | Bake every reflection-capture cubemap into the current level's MapBuildData (headless Build → Build Reflection Captures). Fixes the editor-vs-PIE divergence where a reflective scene looks correct in the viewport (transient live re-capture) but washed-out grey in PIE / cooked builds (which read the serialized build data). `save` defaults true. PIE-blocked; no dry_run. |
| `editor_live_coding_compile` | Ctrl+Alt+F11 equivalent — hot-patches compiled code into the running editor without restart. Use whenever the change is confined to function bodies in existing `.cpp` files. |
| `editor_build_game_target(project_root?, target?)` | Full UBT rebuild of the host project's game target. Project resolution: `project_root` param → `UNREAL_PROJECT_ROOT` env → structured error; the `.uproject` is discovered by glob inside the root and the UBT target name defaults to its filename stem (`target` overrides). The engine is located via `UNREAL_ENGINE_ROOT` (must contain `Engine/Build/BatchFiles/Build.bat`). Unset env vars produce a structured error naming the variable. |

**Build lock (multi-agent coordination).** A C++ build (e.g. `scripts/build-editor.ps1`)
recompiles with the **editor closed**, so during a build every bridge/editor call
fails — which agents easily misread as a crashed session. An in-memory build lock
on the MCP server serializes builds:

- The build scripts **acquire** the lock (over `POST /build/acquire`, sending their
  own PID) before building and **release** it in a `finally` (even on failure). A
  second concurrent build is **refused** — the script prints "Sorry, you can't build
  right now…" and exits 75. The scripts **fail open**: if the MCP server is down,
  the build proceeds without coordination.
- A crashed/aborted build frees the lock almost immediately via a **PID-liveness
  check** (the server checks the build process is still alive); a build that hangs
  while alive is reclaimed after the TTL (`UNREAL_MCP_BUILD_LOCK_TTL_MS`, default
  45 min).
- **`build_status`** (MCP tool) tells an agent whether a build is in progress (holder
  label/pid/target) AND whether the editor is reachable — the one call to run when
  editor calls start failing or a session seems dead. When the editor is unreachable
  *because* of a build, the bridge error says so (`engine_busy`, "EXPECTED during a
  build, NOT a crashed session") instead of a bare connection failure.
- In-memory: a server restart clears the lock. Builds run **exclusively**; everything
  else (file edits, reads) proceeds side-by-side.

### 2.18. PIE — Play In Editor (`pie_*`)

| Tool | Behavior |
|---|---|
| `pie_start(map_path?, requester?)` / `pie_stop(session_id?)` | Lifecycle (lease-serialized — see below) |
| `pie_get_state` | Current PIE state (running, world info) + the full lease/queue readout |
| `pie_query(query?, filter?, limit?)` | Read the **live PIE world** — the editor-world actor tools never see PIE-spawned actors. `query`: `summary` (default), `players`/`player`/`pawn` (piloted pawn + camera POV), `actors` (substring-filtered, capped by `limit`), `all` |
| `pie_send_keystrokes(actions, focus_viewport?)` | Synthetic keyboard events in order (per-event `delay_ms`/modifiers). `focus_viewport:true` only for sustained *polled* (held-key) input — it steals editor focus |
| `pie_send_mouse(x, y, event_type?, button?)` | Synthetic mouse (`move`/`pressed`/`released`) |
| `pie_inject_input_action(action_path, value?)` | Fire an **Enhanced Input** action (one-shot press) on the PIE player — the typed way to trigger event-driven actions (jump, vault, dash) that synthetic keystrokes can't fire; only continuous/axis input survives key simulation |
| `pie_capture_from_pose(location, rotation, fov?, aspect?, restore?, filename?, directory?)` | Screenshot from an **exact camera pose** through the real game render path — the sanctioned capture rig (below) |

For AI-agent runtime introspection during PIE, see §2.19. For console commands during PIE, use `editor_console_exec`.

**Reproducible in-game screenshots (`pie_capture_from_pose`).** This is the
sanctioned "fixed capture rig" required by CLAUDE.md / `docs/TESTING.md` before any
screenshot-based visual verdict — a deterministic pose, zero stateful navigation.
It spawns a transient camera at the pose, swaps the player's view target to it
(instant), waits a few frames to composite, captures the game viewport, then (with
`restore:true`, the default) restores the original view and destroys the temp
camera; `restore:false` parks the view for repeated captures. Feed it
`location`/`rotation`/`fov`/`aspect` from `editor_viewport_get_camera` to reproduce
a human-framed editor shot in-game (the `/capture-pose` skill wraps this pairing).
Returns `result.path` once the bridge confirms the file. Never hand-fly the PIE
camera to a spot and screenshot — that is the verboten play-acting.

**PIE lease (multi-agent coordination).** One editor runs one PIE world, so
`pie_start`/`pie_stop` are serialized by an in-process **lease** keyed by MCP
session (one agent = one session). Behaviour an agent must handle:

- `pie_start` when the lease is **free** → you get it and PIE starts (the normal
  `status:"starting"` result, plus a `result.pie_lease` block with `state:"started"`).
- `pie_start` when **another agent holds it** → `status:"error"`,
  `error_code:"pie_busy"`, with your FIFO `result.pie_lease.position`. **This is not
  a failure to abort on — call `pie_start` again to keep your place.** You are
  promoted when the holder calls `pie_stop` or its lease times out. `pie_start`
  long-polls (~25s) so it often resolves the moment the holder finishes.
- A holder is reaped after a **10-minute** lease TTL (stuck/forgetful agent). On
  changeover the lease **stops the stale PIE before the next agent starts** — a
  promoted agent never inherits a session already in use. The reaped agent's next
  `pie_stop` returns `error_code:"pie_lease_lost"` (its session was reassigned;
  it must not stop the new holder's PIE).
- If that stale PIE **cannot be stopped** within the takeover timeout (30s default,
  `UNREAL_MCP_PIE_TAKEOVER_TIMEOUT_MS`), the promoted agent's `pie_start` returns
  `status:"error"`, `error_code:"pie_takeover_failed"` — a clean session could not
  be started and the editor may need manual attention. The lease is **released**
  (`result.pie_lease.state:"start_failed"`) so the next agent can proceed; inspect
  the error, then retry `pie_start`.
- `pie_stop` only succeeds for the lease holder. If **another agent holds the
  lease**, you get `error_code:"pie_not_holder"` and the stop is **not forwarded** —
  it is their session; queue with `pie_start` instead. When the lease is **free** it
  still forwards (cleans up an untracked/orphaned PIE, e.g. one left running across
  a server restart). `pie_get_state` folds the full lease + queue into
  `result.pie_lease` (holder, queue, `you_hold`, `your_position`).
- A **keep-alive reconciler** polls real editor PIE state while the lease is held,
  so a crashed PIE / crashed editor frees the lease *early* (`pie_ended` /
  `pie_failed_start` / `editor_down`) instead of waiting out the TTL. The 10-min
  TTL is the backstop for a holder that is stuck but whose PIE is still genuinely
  running (and for an agent that hard-crashes without releasing — see next point).
- A holder that **gracefully disconnects** (MCP session terminate) frees the lease
  immediately; a *hard-crashed* agent that leaves PIE running is reclaimed by the
  keep-alive (if PIE also died) or the TTL (if PIE is still up). Reconnect is a new
  session → back of the line.
- State is **in-memory**: a server restart resets the lease and everyone re-queues.
  Tunable via `UNREAL_MCP_PIE_LEASE_TTL_MS` / `_ACQUIRE_CAP_MS` /
  `_TAKEOVER_TIMEOUT_MS` / `_LIVENESS_POLL_MS` / `_STARTUP_GRACE_MS` /
  `_EDITOR_DOWN_GRACE_MS`.

The lease codes — `pie_busy`, `pie_lease_lost`, `pie_not_holder`,
`pie_takeover_failed` — are **deliberate
server-side additions outside the closed C++ taxonomy** (§1.2): coordination signals
synthesized in `src/server/src/domains/pie.ts`, never emitted by the plugin.
`result.pie_lease.state` carries the precise outcome alongside the code.

**Video recording + analysis (`pie_record_*` → `video_analyze`; one-shot `pie_analyze`).**
The recorder captures the live PIE viewport **in-engine** (no external binary):
the 3D scene + UMG/HUD as presented, cropped to the game viewport, downscaled to
FIT `width`×`height` (aspect preserved), H.264 MP4 — plus, by default, the game
audio the player hears (main-submix mix, AAC, sample-clock-synced; `audio:false`
or a missing audio device degrades to video-only, see `result.audio`/`audio_note`).
Output defaults to `<Project>/Saved/MCPRecordings/`. Analysis is **pure
server-side**: the MP4 goes to a video-understanding model; the editor bridge is
never involved and never sees the API key.

| Tool | Behavior |
|---|---|
| `pie_record_start(fps?, width?, height?, bitrate_kbps?, audio?, max_duration_s?, filename?, directory?, wait_for_pie_s?)` | Start recording; returns `recording_id` + output path. `fps:0` = every presented frame (fast/transient behaviour); default 30. `wait_for_pie_s` arms-and-retries until the PIE world is presenting — call `pie_start` then this immediately (no manual sleep) so the opening seconds are captured |
| `pie_record_stop()` | Finalize the MP4: `path`, dimensions, `frames_encoded`/`frames_dropped`, `duration_s`, `bytes`, `stop_reason` |
| `pie_record_status()` | Read-only: active?, `recording_id`, path, `elapsed_s`, frame counters, any encoder error |
| `pie_record_arm(base_name?, …)` / `pie_record_disarm()` | Auto-record **every** PIE session on this editor until disarmed; takes land as `<base_name>_NN.mp4`. Disarm doesn't cut an in-flight take (it still finalizes normally) |
| `video_analyze(path, expected_behavior, criteria?, analysis_fps?, clip_start_s?, clip_end_s?, model?)` | Judge any MP4 (typically `result.path` from `pie_record_stop`) against a stated expectation: structured `verdict` (`matches`/`diverged`/`inconclusive`), summary, timestamped severity-rated divergences, per-criterion results |
| `pie_analyze(expected_behavior, criteria?, duration_s?, capture_fps?, width?, height?, analysis_fps?, model?)` | One-shot composite: record `duration_s` (default 10 s) → stop → `video_analyze`. Drive the scenario yourself (`pie_send_keystrokes` / `pie_inject_input_action`) while it records — the call occupies the session for its whole duration. For anything intricate prefer the explicit `pie_record_*` + `video_analyze` sequence |

Foot-guns:

- **Real RHI + Windows only.** Recording is refused with `feature_disabled` under
  `-nullrhi` (no frames are rendered) and on non-Windows (Media Foundation
  encoder) — known issue, see `docs/BUGS.md` ("PIE video recording requires a
  real RHI and Windows"). Headless CI: launch with a GUI or `-RenderOffscreen`.
- **One recording at a time** (`engine_busy` on a second start). The
  `max_duration_s` **hard watchdog** (1–600 s, default 120) auto-stops and
  finalizes the file so a recording can never leak — it is *not* the intended
  length; call `pie_record_stop` when done. Recordings also auto-finalize when
  PIE ends; a stop after that returns "no recording in progress".
- **Lease-aware.** You may record your own session (you hold the PIE lease) or an
  unleased one (e.g. a human started PIE by hand) — never another agent's:
  `pie_record_*`/`pie_analyze` refuse with `pie_not_holder` when someone else
  holds the lease. `pie_record_arm` applies to **any** PIE session on this editor
  (yours, a human's, another agent's) — arm only with the operator's knowledge.
- **`video_analyze` needs a server-side API key**: `GEMINI_API_KEY` (or
  `GOOGLE_STUDIO_API_KEY`) in the repo-root `.env`; refused with
  `feature_disabled` otherwise. The upload to the provider is transient (deleted
  after analysis); the local file is untouched. Expect tens of seconds of latency
  (upload + inference). Knobs: `UNREAL_MCP_VIDEO_PROVIDER` (`google` is the only
  implementation), `UNREAL_MCP_VIDEO_MODEL` (default `gemini-3.5-flash`),
  `UNREAL_MCP_VIDEO_ANALYSIS_FPS` (default 1), `UNREAL_MCP_VIDEO_MAX_ANALYSIS_FPS`
  (cost guard, default 30 — exceeding it is `invalid_argument`),
  `UNREAL_MCP_VIDEO_UPLOAD_TIMEOUT_MS` (default 120000).
- **Two distinct fps knobs.** The recording's *capture* fps decides which frames
  EXIST; `analysis_fps` decides how densely the model SAMPLES them (~300 tokens
  per sampled second — default 1). Raise it only for fast/transient events, and
  prefer clipping with `clip_start_s`/`clip_end_s` and raising fps over just the
  window that matters.

### 2.19. AI runtime introspection (`ai_*`)

Operate on a **running** PIE agent.

| Tool | Returns |
|---|---|
| `ai_get_state(actor_name)` | AIController + Blackboard + StateTree current values |
| `ai_get_awareness(actor_name)` | What the agent currently sees / hears (perception query results) |
| `ai_get_perception(actor_name)` | Raw perception component dump |

### 2.20. PCG — Procedural Content Generation (`pcg_*`)

Authoring of `UPCGGraph` assets plus driving generation on level actors via `UPCGComponent`. The C++ handler (`FMCPPCGCommands`) drives the **runtime** `UPCGGraph` API directly; the editor's graph-panel mirror rebuilds itself from `NotifyGraphChanged`, so graphs authored here open normally in the PCG editor.

**Canonical authoring sequence:**

```
pcg_list_node_types (discover settings classes — the palette)
  → pcg_graph_create (comes with Input + Output nodes)
  → pcg_node_add × N → pcg_node_connect × N → pcg_node_set_property × N
  → pcg_component_add(actor, graph) → pcg_component_generate(actor)
  → (generation is async — re-query / screenshot a moment later)
```

| Tool | Behavior |
|---|---|
| `pcg_list_graphs(path_filter="/Game")` | List PCG Graph (and Graph Instance) assets: `graphs[]` of `{name, path, class}`. Read-only. |
| `pcg_list_node_types(name_filter?, category_filter?)` | Enumerate library-exposed `UPCGSettings` classes — the palette for `pcg_node_add`. Returns `class_name`, `class_path`, `title`, `type`, pin lists. Read-only. |
| `pcg_graph_read(graph_path)` | Graph structure: `input_node`, `output_node`, `nodes[]` (id, settings_class, title, position, pins), `edges[]`. Node `id` values are the handles for connect / set_property. Read-only. |
| `pcg_graph_create(graph_path)` | New empty graph with Input/Output nodes in place. Auto-saves. |
| `pcg_node_add(graph_path, settings_class, node_title?, pos_x?, pos_y?)` | Add a node by settings class name (from `pcg_list_node_types`) or full class path. `node_title` is also usable as a node handle later. |
| `pcg_node_connect(graph_path, from_node, to_node, from_pin?, to_pin?)` | Wire an output pin to an input pin. `from_node`/`to_node` take node ids, titles, or the tokens `"InputNode"` / `"OutputNode"`; omitted pins default to each node's **first** output/input pin. |
| `pcg_node_set_property(graph_path, node, property_name, property_value)` | Set an EditAnywhere property on a node's settings object (e.g. the mesh on a StaticMeshSpawner, the count on CreatePoints). |
| `pcg_component_add(actor_name, graph_path?)` | Add a `UPCGComponent` to a level actor and optionally assign a graph. Spawn a fresh host with `actor_spawn` first if needed. |
| `pcg_component_generate(actor_name, force=true)` | Trigger generation on the actor's PCG component. **Asynchronous** — scheduled on the PCG subsystem; the result reports `generation_requested` / `is_generating`, not completion. |

**Foot-guns:**

- **The PCG mutators are in *neither* enforcement blocklist** — not `IsBlockedDuringPie` (C++) / `PIE_BLOCKED` (`gates.ts`), and not `IsBlockedFromDryRun` / `DRY_RUN_UNSUPPORTED`. Two consequences: (1) authoring/drive calls **run unguarded while PIE is active** instead of being refused; (2) the handler implements no dry-run and the §1.4 safety net doesn't intercept it, so `dry_run: true` is **silently ignored and the mutation applies for real** — no `result.diff`, no `dry_run_unsupported` refusal. The server module (`domains/pcg.ts`) deliberately carries no `blockedDuringPie`/`dryRunUnsupported` annotations to stay truthful to actual enforcement; adding the gates is a both-sides code change (§3) tracked separately. Until then: don't author PCG during someone's PIE session, and never pass `dry_run` expecting a preview.
- `pcg_component_generate` returning success means generation was *scheduled*. Verify results with a follow-up read (`actor_query`, `scene_brief`, a screenshot) after a moment, not from the call itself.

### 2.21. Kinematics — skeletal transform probes & IK solves (`kinematics_*`)

Spatial ground truth for bones, sockets, and attached meshes — the `/position` skill's verification backend. All three tools reuse the game's `BoneIK` math, so a rotation verified here matches what the shipped IK reproduces. **None mutate assets** — no PIE gate, no dry-run gate, all annotated read-only.

**World resolution:** each call resolves a target world **preferring a running PIE world** (where the possessed player and live animated poses are), falling back to the editor world for posing placed actors; the result's `world_type` (`"pie"` | `"editor"`) reports which was used. In the editor world a skeletal mesh's pose is typically the ref/preview pose — `pose_valid` flags whether the component-space transform array is populated, and `world_type` lets you judge what you measured.

| Tool | Behavior |
|---|---|
| `kinematics_read_transform(actor, queries[], mesh?)` | Read world + component-relative transforms for a batch of `{socket}` / `{bone}` queries, each optionally scoped by `component` (omit/`"body"` = the actor's main skeletal mesh; a component name; or an attached actor's name to reach e.g. a weapon mesh's sockets). Static-root actors fall back to the StaticMeshComponent/scene root (`component_type: 'static'`/`'scene'`, with `world_bounds`) instead of rejecting. An explicit component that matches nothing returns `exists: false` — a same-named body bone can't masquerade as that component's socket. |
| `kinematics_probe(actor, rotations[], probe_points[], mode="dryrun", intent_direction?, forward_axis_local?, mesh?)` | Forward-kinematic probe: apply candidate bone rotation(s) and report each probe point's **end-effector world-space delta** (ΔP + ΔQ), optionally cosine-scored against an intended world direction. The end-effector delta is the only truth. |
| `kinematics_solve(actor, chain{upper,lower,hand}, effector, desired_direction, verify=true, forward_axis_local?, mesh?)` | Inverse solve: find the two-bone-IK rotation that aims a tip along a desired **world** direction via `BoneIK::SolveTwoBoneIK`. Best-effort (two-bone IK aims a hand *position*); `verify: true` (default) re-runs the forward probe on the solved pose and reports the achieved tip direction. |

**`mode` semantics (`kinematics_probe`):**

- `"dryrun"` (default) — computes the post-rotation pose without touching the component. Exact, not approximate: each probe point's transform relative to its *governing bone* is invariant under rotations of that bone or its ancestors, so recomposition gives the true world result.
- `"live"` — atomically applies the rotations to the component and restores it before returning, all within one game-thread call. **It has no numeric advantage**: no animation evaluation runs between apply and restore, so the sampled pose — and every reported number — is identical to dryrun (the result says so in a `note`). Its only intended payoff, a candidate-pose `screenshot`, is currently **deferred** (a synchronous game-thread call can't render a frame mid-call); `screenshot: true` returns a note instead of an image. Use `dryrun`.

**Rotation input (`rotations[]` entries):** `{ "bone", "rotation", "space" }`, where `rotation` is either `{axis: {x,y,z}, angle_deg}` or `{pitch, yaw, roll}`, and `space` is one of three coordinate spaces:

| `space` | Meaning |
|---|---|
| `"component"` | Component space — used as-is (`BoneIK` left-multiplies component-space deltas). |
| `"world"` | World space — conjugated into a component-space delta by the mesh's world rotation. |
| `"bone_local"` | The bone's own local frame — post-multiply equivalence via the bone's component-space rotation. |

`forward_axis_local` (default +X) names which local axis of the probe point / effector counts as its "forward" — e.g. `{"x":0,"y":0,"z":1}` for a blade whose tip is mesh-local +Z.

### 2.22. Landscape & Foliage inspection (`landscape_*`, `foliage_*`) — read-only by design

**Mutation is refused by design.** Sculpting, painting, heightmap import, foliage scatter/removal are brush-driven content authoring and belong to the editor's Landscape and Foliage modes — the MCP only reads existing state. All four tools are read-only and idempotent; none are PIE- or dry-run-gated.

| Tool | Behavior |
|---|---|
| `landscape_inspect(actor_name?)` | Enumerate landscape actors (empty = all): name, class (`Landscape` / `LandscapeStreamingProxy`), guid, location/scale, component/subsection geometry, material, `extent_quads`, `paint_layer_count`. |
| `landscape_list_layers(actor_name?)` | A landscape's paint (target) layers: name, `layer_info_object` asset path (empty if unassigned), `assigned`. Empty actor = first landscape in the world. |
| `landscape_read_heightmap(actor_name?, region?, export_path?, include_samples=false)` | **Bounded** heightmap summary: resolved region, `samples_read`, `height_stats` (min/max/mean raw uint16 + min/max world Z). Never dumps a full grid inline — narrow with `region` (quad-space, clamped), set `include_samples` for a small inline grid (≤256×256 only), or pass `export_path` to write the raw row-major uint16 grid as `.r16` to disk. |
| `foliage_inspect(mode="types"\|"instances", foliage_type?, limit=100, offset=0)` | `mode="types"` (default): per-type summary — placed-instance count, mesh, density, radius, align_to_normal, random_yaw, cull_distance. `mode="instances"`: placed-instance transforms for one `foliage_type` (identity / mesh path / display name from the types listing), paged via `limit` (1..1000) / `offset`, with `total_instances` + `truncated` — never dumps every instance. |

### 2.23. Disclosure meta-tools (`catalog_*`, `result_read`) — server-side plumbing, not Unreal ops

Five server-side tools (no C++ handler, no bridge round-trip) that let an agent work the ~260-tool catalog without paying the full schema tax. They are **advertised in every surface mode** (`UNREAL_MCP_SURFACE=full|compact|code`); in `compact`/`code` they are the *only* path to the domain tools. Discovery flow: `catalog_domains` → `catalog_search` → `catalog_describe` → `catalog_call`.

| Tool | Behavior |
|---|---|
| `catalog_domains()` | List every tool domain + tool count (the shape of the toolset). Hides the meta-domains themselves (`catalog`, `code`). |
| `catalog_search(query, domain?, limit=20)` | Keyword search over tool names + descriptions. Returns compact summaries (name, domain, one-liner) — **not** schemas. Empty query lists all. |
| `catalog_describe(name)` | One tool's full input JSON Schema + annotations, including `blockedDuringPie` and `dryRunUnsupported`. Call before `catalog_call`. |
| `catalog_call(name, params={})` | Invoke **any canonical tool** by name. Identical to a direct call — same Zod validation, same gates, same `{status,result,error}` envelope. Not read-only (it can reach mutators). |
| `result_read(handle, offset=0, length=8000)` | Page a compacted result back by its `_handle` (raw JSON slice + `next_offset`, `null` when done). Only relevant when result compaction is on (`UNREAL_MCP_MAX_RESULT_BYTES > 0`; default 0 = off). Handles are a bounded LRU — an expired handle means re-run the original tool. For large data, prefer filtering in code mode over paging it into context. |

Foot-gun: `catalog_search` results are summaries — never guess parameters from the one-liner; `catalog_describe` first.

Deeper internals — the surface-mode table, compaction thresholds/digests, and code mode (`code_api`/`code_run`) — live in [`src/server/README.md`](../src/server/README.md) ("Token efficiency — progressive disclosure").

---

## 3. C++ author's contract

When adding a new C++ handler, every rule below applies. New tools that violate these don't ship.

### 3.1. Mutation contract

Every handler that touches an asset:

```cpp
Target->PreEditChange(Property /* or nullptr */);
// mutate
Target->PostEditChange();
Target->MarkPackageDirty();
UEditorAssetLibrary::SaveAsset(Target->GetPathName(), /*bOnlyIfIsDirty=*/false);
```

Skipping any step breaks the auto-save invariant. For Blueprint authoring specifically: `MarkBlueprintAsStructurallyModified` + `FKismetEditorUtilities::CompileBlueprint` before save.

### 3.2. Error response construction

Use the structured-error overload exclusively:

```cpp
return FMCPCommonUtils::CreateErrorResponse(Message, EMCPErrorCode::X, Hint);
```

The legacy 1-arg `CreateErrorResponse(FString)` is retained only for the back-compat invariant — new handlers must not use it. Pick the code from the closed taxonomy (§1.2); never invent a code in-handler.

Local per-class `CreateErrorResponse` helpers (a pattern in older code) hide migration debt. Remove them when migrating call sites — every error path should route through the global helper.

### 3.3. Dry-run

If the handler is a mutator, support `dry_run`:

1. Factor into `validate → diff → apply`.
2. On `dry_run = true`: run `validate` + `diff`, skip `apply`, return `result.diff` per the subsystem shape (§1.4).
3. On `dry_run = false`: run all three.

If the handler can't support dry-run (e.g., the apply path is co-mingled with construction), add the command name to **both** blocklists — `FMCPCommonUtils::IsBlockedFromDryRun` (C++, the enforcing side: the bridge returns `dry_run_unsupported` before dispatch) and `DRY_RUN_UNSUPPORTED` in `src/server/src/bridge/gates.ts` (the TS mirror driving capability disclosure). The set is large (~40 tools across `ik_retarget_*`, `gas_*`, `level_*`, factory creators, `pie_record_*`, widget mutators, …; see §1.4) — joining it is normal, but shipping dry-run support later means removing the entry from both sides in the same commit.

### 3.4. File size limit

No `.cpp` or `.h` file exceeds **600 lines**. Split per the `ClassName_Concern.cpp` convention (e.g., `AssetManager_References.cpp` is a satellite of `AssetManager.cpp`). Header navigation comments document the split.

### 3.5. Anonymous-namespace collisions

UE's Unity build aggregates `.cpp` files into one translation unit. Anonymous-namespace symbols can collide across files. Either:

- Use file-specific names (`ResolveIKDestination` vs. `ResolveGASDestination` vs. `ResolveWidgetDestination`)
- Use `static` instead of anonymous namespace for genuinely file-local symbols

### 3.6. Build paths

| Change | Use |
|---|---|
| `.cpp` body change only | `editor_live_coding_compile` (Ctrl+Alt+F11 equivalent) |
| Header change / new file / `UCLASS` / `UFUNCTION` / `.Build.cs` | `editor_build_game_target` or the full rebuild cycle below, after stopping the editor |

Headers can't be hot-patched — Live Coding skips them entirely. The full rebuild also requires the editor to be stopped (Live Coding mutex collision otherwise).

**The full rebuild cycle.** Restarting the editor kills every agent's live session — get
permission for the current task first (the full-rebuild gate in `CLAUDE.md`):

```bash
scripts/stop-editor.ps1     # frees the locked editor/plugin DLLs (leaves the MCP server up)
scripts/build-editor.ps1    # builds <Project>Editor — the game module + the UnrealMCP plugin
scripts/launch-editor.ps1   # GUI; add -Headless for unattended MCP driving (nullrhi)
```

A full rebuild is **non-destructive and shared**: `Build.bat` compiles whatever is on disk,
so it picks up *every* agent's saved edits, not just yours — it never clobbers another
session's in-flight source. Only the live editor session restarts.

**Build coordination endpoints.** `build-editor.ps1` dot-sources `scripts/build-coord.ps1`,
which serializes builds through the always-on MCP server's `/build` REST endpoints
(`http://127.0.0.1:8765`, overridable via `UNREAL_MCP_HOST`/`UNREAL_MCP_PORT`). Lock
semantics — PID-liveness, TTL, fail-open — are §2.17; the wire shapes:

- `POST /build/acquire` `{pid,target,label,host}` — taken at build start. If another build
  holds the lock, the script names the holder (`label` + `pid` + how long held) and
  **exits 75** (`EX_TEMPFAIL`) without building. Set a human label via
  `CLAUDE_BUILD_LABEL` (defaults to `build-editor:<Target>`).
- `POST /build/release` `{build_id,pid}` — released in a `finally`, even on build failure.
- `POST /build/heartbeat` `{build_id|pid, ttl_seconds?}` — holder-only TTL refresh: resets
  the lock's expiry to now + `ttl_seconds` (default TTL if omitted) for a build that
  legitimately outlives the 45-min default. Non-holders get `{ok:false}` — it never steals
  the lock. (Same-PID `acquire` also refreshes, so the scripts today just re-acquire.)
- `GET /build/status` — `{in_progress, holder:{label,pid,target,held_ms,expires_in_ms,
  pid_alive,…}}`. Poll this to wait for a clear window instead of hammering acquire.

On exit 75: **don't kill the other build** (interrupting a running compile can corrupt
artifacts) — poll `GET /build/status` until `in_progress:false`, then
stop-editor → build → launch. Expect contention with concurrent sessions: another agent may
relaunch the editor between your poll and your build, re-locking the plugin DLL and failing
your link step — re-run `stop-editor.ps1` and retry.

**Plugin-only DLL gotcha.** A plugin rebuild can land `UnrealEditor-UnrealMCP.dll` in the
host project's `Binaries/Win64` while the editor loads it from
`src/Plugin/UnrealMCP/Binaries/Win64` — same BuildId, so close the editor and copy the
fresh DLL across before relaunching. The game-module DLL loads from the project dir and is
unaffected. (A real fix is owed here.)

### 3.7. Cook commandlet hazards

`ConstructorHelpers::FObjectFinder<T>` in C++ constructors fails under the cook commandlet — defer project-asset loads to `BeginPlay` / `OnPossess` / `OnRegister`, gated on `!IsRunningCookCommandlet()`.

### 3.8. Bridge registration

Adding a new handler:

1. Header: declare the class in `Public/Commands/`.
2. Source: implement in `Private/Commands/`.
3. Wire in `MCPBridge.h` — declare a `TSharedPtr<FYourHandler>` member.
4. Wire in `MCPBridge.cpp` — construct in `Initialize`, dispatch in `ExecuteCommand`, reset in `Deinitialize`.
5. Server wrapper in `src/server/src/domains/*.ts` — a Zod-typed tool registration (wired through `register.ts`) forwarding to the bridge by command type.

(Wire name == tool name == handler key for all but an enumerated, test-enforced set of per-tool `command:` overrides — the `statetree_* → st_*` family (§2.12) and `bp_add_node → add_blueprint_node` — kept in parity by `test/gate-error-parity.test.ts`. Give a **new** handler one identical name in all three positions; do not add overrides.)

### 3.9. Lint scripts

Informational lint (exit code 0 = clean, 1 = violations):

- `bun run lint:names` — canonical-name compliance (`src/server/scripts/lint-canonical-names.ts`). Run after touching command handlers or tool registrations.

---

## 4. Canonical naming

There is one canonical agent-facing surface — each tool has exactly one name, and all documentation and prompts use canonical names. No open-ended back-compat alias layer exists. For the vast majority of tools the wire name == tool name == C++ handler key; the exceptions are bounded and test-enforced: the per-tool `command:` overrides (`statetree_* → st_*`, §2.12; `bp_add_node → add_blueprint_node`) and the parameter-alias normalizer (`src/server/src/registry/aliases.ts` — the §2.1 Blueprint-identifier synonyms plus material/asset `path`), pinned by `test/gate-error-parity.test.ts` and `test/aliases.test.ts`.

**Deliberate near-duplicates** (different semantic shape, not a 1:1 alias):

- `find_actors_by_name` ↔ `actor_query(name_pattern=…)` — parallel implementations that differ in semantic shape, not just naming. Both stay.
