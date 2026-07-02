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

### 1.2. Error code taxonomy (closed set, 30 codes)

| Group | Codes |
|---|---|
| **Identity / lookup** | `asset_not_found`, `class_not_loaded`, `node_not_found`, `actor_not_found`, `variable_not_found`, `pin_not_found`, `function_not_found`, `unknown_tag`, `window_not_found` |
| **Input shape** | `invalid_argument`, `invalid_path`, `invalid_pin_type`, `ambiguous_target`, `out_of_range` |
| **Asset state** | `asset_dirty`, `asset_compile_failed`, `asset_locked`, `name_collision` |
| **Capability** | `unsupported_class`, `not_in_pie`, `pie_active`, `editor_not_ready`, `feature_disabled`, `dry_run_unsupported` |
| **Authority / safety** | `would_break_references`, `circular_dependency` |
| **Engine** | `engine_busy`, `live_coding_unavailable`, `compile_in_progress`, `internal` |

The set is owned by `EMCPErrorCode` in `Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/MCPCommonUtils.h` and mirrored to `src/server/src/bridge/errors.ts`. Adding a code requires updating both ends.

**Hint quality bar.** An `error_hint` is good if an agent can act on it without further investigation. Tests:
- "did you mean X" hints carry a candidate name list (top three by edit distance where the registry can produce them).
- "must be Y first" hints name the prerequisite tool.
- "valid values are …" hints enumerate the closed set in the message body.

Hints are optional — omit rather than ship filler.

**Recovery helper.** `isRecoverable(code)` in `src/server/src/bridge/errors.ts` returns `true` for identity / input-shape / asset-state / `not_in_pie` / `engine_busy` / `compile_in_progress`. Returns `false` for `internal`, `unsupported_class`, `asset_compile_failed`, `would_break_references`, `circular_dependency`, `live_coding_unavailable`, `feature_disabled`, `dry_run_unsupported`.

### 1.3. Auto-save

Every C++ mutator persists its change before returning. The mutation contract:

```
PreEditChange → mutate → PostEditChange → MarkPackageDirty → UEditorAssetLibrary::SaveAsset(path, /*bOnlyIfIsDirty=*/false)
```

Consequence: callers do **not** need to call `asset_save` after a mutation. `asset_save` exists for the rare case where a mutator path is bypassed or for force-flushing a manually-dirtied package. There is no `save_all_dirty` — refused by design (auto-save on mutation makes it redundant, and a bulk flush hides which mutation dirtied what).

### 1.4. Dry-run

Most mutators accept `dry_run: true` and return `result.diff` instead of applying. Validation parity is the invariant: a passing dry-run implies a passing commit absent races. Only the apply step is skipped.

**Bridge-level safety net.** Mutators that don't yet support dry-run return `error_code = dry_run_unsupported` when called with `dry_run = true`. The block list lives in `FMCPCommonUtils::IsBlockedFromDryRun(CommandType)`. Initial registry: `add_node` / `bp_add_node` (still blocked).

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

Domain prefixes in active use: `actor_`, `bp_`, `material_`, `niagara_`, `anim_`, `widget_`, `statetree_`, `eqs_`, `tag_`, `gas_`, `ik_rig_`, `ik_retarget_`, `asset_`, `editor_`, `pie_`, `class_`, `enum_`, `ai_`, `input_`, `struct_`, `datatable_`, `dataasset_`, `mpc_`, `scene_`, `level_`, `project_`.

There is one canonical surface: the wire name == the tool name == the C++ handler key. The legacy alias map (`ResolveCanonicalCommand`) and the sibling tool registrations were deleted in the naming migration — no back-compat aliases remain. Use canonical names exclusively.

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
| Graph analysis | `bp_inspect` / `analyze_blueprint_graph` | Per-graph node + connection topology |
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
- `physics_spawn_blueprint_actor` — physics-BP-specific spawn variant

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

**Physics asset:** `anim_physics_inspect`, `physics_set_body_collision`.

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

### 2.8. Animation retargeting (`ik_retarget_run_batch`, planned `anim_auto_retarget`)

The MCP exposes one low-level primitive today; the high-level path is planned. **IK Rig + Retargeter asset authoring is intentionally not exposed** — for compatible skeletons the editor's right-click → "Retarget Animations" flow auto-creates rigs / retargeter / chains in one click, and that's the operation we want to surface as a single MCP call. Manual rig authoring is editor UI territory.

**Low-level primitive (shipped) — `ik_retarget_run_batch`:**

Invokes `UIKRetargetBatchOperation::DuplicateAndRetarget` against a pre-built `UIKRetargeter` asset. Use only when you've hand-crafted a retargeter in the editor and want to execute it programmatically.

```
ik_retarget_run_batch(
    retargeter,                  # path to a pre-built UIKRetargeter asset
    source_animations,           # list of AnimSequence paths
    name_search="",              # rename rule
    name_replace="",
    name_prefix="",
    name_suffix="",
    include_referenced_assets=true,  # also retarget referenced sub-blendspaces / sequences
    overwrite_existing=false,    # unique-numeric-suffix on collision (UE's safer default)
)
```

Validation chain (each step has a structured-error code):
- retargeter not found → `asset_not_found`
- controller null → `engine_busy`
- source / target rig not configured → `asset_locked`
- source / target mesh not assigned on rig → `asset_locked`
- any source animation path fails to load → `asset_not_found` with `result.missing_animations` list
- empty `source_animations` → `invalid_argument`

Response includes `new_assets[]` for follow-up chain operations (rename, move, `asset_references`).

**Recommended high-level path (planned) — `anim_auto_retarget`:**

```
anim_auto_retarget(
    source_animations,           # list of AnimSequence paths
    target_skeleton,             # the project's standard skeleton path
    persist_retargeter=false,    # true → save the auto-built retargeter asset
)
```

Mirrors the editor's right-click flow: derives the source skeleton from each animation, auto-discovers or auto-creates IK Rigs for source + target, auto-maps chains, builds a transient (or persistent) retargeter, runs `DuplicateAndRetarget`. Fails closed with a structured error if the skeletons can't be auto-paired (hint: "open the IK Rig editor and configure manually").

### 2.9. UMG / Widgets (`widget_*`)

**Authoring sequence:**

```
widget_create(parent_class="UserWidget")
  → widget_add_child(child_class, parent_name?, child_name?)
  → widget_set_property(widget_name, property_name, property_value, target="widget"|"slot")
  → compile_blueprint (required before bind_handler!)
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

Canonical sequence: `widget_add_child` → `compile_blueprint` → `widget_bind_handler`. Bind calls `FKismetEditorUtilities::CreateNewBoundEventForComponent` (same path the editor's details-panel "Add Event" button uses). Idempotent — repeat calls don't produce duplicates.

**Inspection:** `widget_tree_read` — flat tree dump (every widget, class, parent, slot config). Read-only.

### 2.10. GAS — Gameplay Ability System (`gas_*`)

Two tiers: **asset authoring** (mutates assets, auto-saves) and **runtime application** (operates on PIE world).

**Asset authoring:**

- `gas_ability_create(path, name, parent_class?, cost_tag?, cooldown_tag?, tags{}?)` — wraps `UBlueprintFactory` with `UGameplayAbility` parent.
- `gas_effect_create(path, name, parent_class?, duration_policy?)` — `duration_policy` ∈ `{"Instant", "Duration", "Infinite"}` (closed set; case-insensitive).
- `gas_attributeset_create(path, name, parent_class?)` — Blueprintable scaffolding tier. Production AttributeSets remain C++. Response carries `is_scaffolding: true` with a migrate-to-C++ hint.

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

UE 5.4+ replacement for Behavior Trees + Blackboard. The full surface ships under canonical `statetree_*` names (the abbreviated `st_*` aliases were retired in the naming migration):

| Tool | Purpose |
|---|---|
| `statetree_create` / `statetree_read` / `statetree_compile` / `statetree_save` / `statetree_verify` | Asset lifecycle |
| `statetree_list_node_types` / `statetree_list_schemas` | Discover available task / condition / consideration types |
| `statetree_state_add` / `_remove` / `_rename` / `_move` / `_duplicate` / `_set_properties` / `_list` | State CRUD |
| `statetree_node_add` / `_remove` / `_set_property` / `_get_properties` | Inline node (task / condition / consideration) |
| `statetree_transition_add` / `_remove` / `_set_properties` | Transition CRUD. Event-driven transitions: `trigger="OnEvent"` + `event_tag=<registered gameplay tag>` writes the transition's `RequiredEvent.Tag` (unregistered tags → `unknown_tag` error; empty string on the setter clears; the read emits `event_tag` when set) |
| `statetree_binding_add` / `_remove` / `_list` / `_list_bindable` | Property bindings — the Blackboard equivalent |
| `st_set_entry_state` | Designate the entry state |

**Canonical authoring sequence:**

```
statetree_create
  → statetree_state_add × N → set_entry_state
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

**Inspection:**

- `niagara_list_systems` / `niagara_system_read` / `niagara_emitter_read` / `niagara_module_get_inputs`

**System / emitter mutation:**

- `niagara_emitter_set_enabled(system, emitter, enabled)`
- `niagara_user_parameter_add` / `_remove` / `_set`
- `niagara_module_set_input(system, emitter, module, input, value)`
- `niagara_scratch_pad_module_add(system, emitter, module_template)`

**Standalone script creation:**

- `niagara_script_create(usage, path, name)` — `usage` ∈ `{"module", "function", "dynamic_input"}` (closed set; `"dynamicinput"` accepted as typo-robust alias). Empty/seed-from-defaults graph — populate via existing Niagara node tools.

Editing an existing standalone script's HLSL or visual graph isn't yet exposed.

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
| `content_browser_refresh(path?, force_rescan?)` | `IAssetRegistry::ScanPathsSynchronous`. Default path `/Game`; `force_rescan=true` by default. Logical content paths only (rejects raw filesystem paths). |
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
| `pie_start` / `pie_stop` | Lifecycle |
| `pie_get_state` | Current PIE state (running, paused, world type) |
| `pie_send_keystrokes(keys, target?)` | Synthetic input |
| `pie_send_mouse(x, y, button?, action?)` | Synthetic mouse |

For runtime queries during PIE, see §2.19. For console commands during PIE, use `editor_console_exec`.

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
- `pie_stop` only succeeds for the lease holder; when the lease is free it still
  forwards (cleans up an untracked/orphaned PIE). `pie_get_state` folds the full
  lease + queue into `result.pie_lease` (holder, queue, `you_hold`, `your_position`).
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

### 2.19. AI runtime introspection (`ai_*`)

Operate on a **running** PIE agent.

| Tool | Returns |
|---|---|
| `ai_get_state(actor_name)` | AIController + Blackboard + StateTree current values |
| `ai_get_awareness(actor_name)` | What the agent currently sees / hears (perception query results) |
| `ai_get_perception(actor_name)` | Raw perception component dump |

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

If the handler can't support dry-run (e.g., the apply path is co-mingled with construction), add the command name to `FMCPCommonUtils::IsBlockedFromDryRun` so the bridge returns `dry_run_unsupported` on attempted dry-run calls.

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

(Wire name == tool name == handler key — there is no alias or translation step.)

### 3.9. Lint scripts

Informational lint (exit code 0 = clean, 1 = violations):

- `bun run lint:names` — canonical-name compliance (`src/server/scripts/lint-canonical-names.ts`). Run after touching command handlers or tool registrations.

---

## 4. Canonical naming

There is one canonical surface — wire name == tool name == C++ handler key. No back-compat aliases exist; all documentation and prompts use canonical names.

**Deliberate near-duplicates** (different semantic shape, not a 1:1 alias):

- `find_actors_by_name` ↔ `actor_query(name_pattern=…)` — parallel implementations that differ in semantic shape, not just naming. Both stay.
