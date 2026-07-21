/**
 * Advisory mirrors of the C++ gate sets in `MCPCommonUtils.cpp`
 * (`IsBlockedDuringPie` / `IsBlockedFromDryRun`).
 *
 * IMPORTANT: the C++ bridge ENFORCES these gates — the server does not. They are
 * advisory here: surfaced as tool annotations (see `disclosure/metatools.ts`,
 * which keys them by `def.name`) so a client knows up front that a call will be
 * refused during PIE or that `dry_run` is unsupported, instead of discovering it
 * from an error.
 *
 * Namespace note: these sets are keyed on **canonical tool names** (what the
 * client sees), while the C++ blocklists are keyed on **wire command names**.
 * They differ only where a tool declares a `command:` override (the whole
 * `statetree_* → st_*` family, plus `bp_add_node → add_blueprint_node`). The C++
 * side additionally carries a few wire-only commands with no tool surface
 * (`add_conduit`, `merge_bones_*`) — reachable via raw TCP but not as tools, so
 * they are intentionally absent here.
 *
 * `test/gate-error-parity.test.ts` enforces this mapping: it maps every tool's
 * wire command and asserts these sets equal the C++ literals. Editing a blocklist
 * on one side without the other fails that test.
 */

/** Commands refused while a PIE session is active (canonical tool names). */
export const PIE_BLOCKED: ReadonlySet<string> = new Set([
  "anim_anchor_feet_to_floor", "anim_blend_space_add_sample",
  "anim_blend_space_create", "anim_blend_space_remove_sample",
  "anim_blueprint_create", "anim_blueprint_set_skeleton",
  "anim_extract_between_notifies", "anim_montage_add_section",
  "anim_montage_create", "anim_montage_set_blend",
  "anim_montage_set_section_link", "anim_node_bind_property",
  "anim_normalize_z_offset", "anim_notify_add", "anim_notify_remove",
  "anim_physics_inspect", "anim_sequence_set_property",
  "anim_skeletal_mesh_inspect", "anim_skeletal_mesh_set_section_disabled",
  "anim_skeleton_add_socket", "anim_skeleton_modify_socket",
  "anim_skeleton_remove_socket", "anim_smooth_sequence",
  "anim_state_machine_create", "anim_state_machine_modify_transition",
  "anim_state_machine_set_entry", "anim_state_machine_state_add",
  "anim_state_machine_state_remove", "anim_state_machine_transition_add",
  "anim_state_machine_transition_remove", "asset_bake_dynamic_to_static_mesh",
  "asset_dataasset_create", "asset_dataasset_set_property", "asset_delete",
  "asset_duplicate", "asset_fixup_redirectors", "asset_import_mesh",
  "asset_move", "asset_open",
  "asset_rename", "asset_save", "asset_textures_import", "bp_add_component",
  "bp_set_component_property", "bp_set_component_transform", "bp_set_class_replication",
  "bp_set_event_replication",
  "bp_add_event_node", "bp_add_function_input", "bp_add_function_output",
  "bp_add_node", "bp_compile", "bp_connect_pins", "bp_create_blueprint",
  "bp_create_dispatcher",
  "bp_create_function", "bp_create_variable", "bp_delete_function",
  "bp_delete_node", "bp_delete_variable", "bp_disconnect_pin",
  "bp_remove_component", "bp_remove_function_input", "bp_remove_function_output",
  "bp_rename_function", "bp_reparent", "bp_set_default_value",
  "bp_set_inner_node_property", "bp_set_node_property",
  "bp_set_variable_properties", "datatable_create", "enum_create", "eqs_create",
  "eqs_option_add", "eqs_option_remove", "eqs_set_property", "eqs_test_add",
  "eqs_test_remove", "gas_ability_create", "gas_ability_set_cooldown",
  "gas_ability_set_cost", "gas_attributeset_create",
  "gas_effect_apply", "gas_effect_create", "ik_retarget_align_bones",
  "ik_retarget_auto_map_chains", "ik_retarget_create",
  "ik_retarget_import_pose_from_animation",
  "ik_retarget_import_pose_from_pose_asset", "ik_retarget_run_batch",
  "ik_retarget_set_chain_mapping", "ik_retarget_set_pelvis_settings",
  "ik_retarget_set_rigs", "ik_retarget_set_root_motion_settings", "input_create",
  "input_add_mapping",
  "level_new", "level_save", "level_save_as", "level_load",
  "editor_build_reflection_captures",
  "material_add_expression", "material_apply_to_actor",
  "material_apply_to_blueprint", "material_compile", "material_connect",
  "material_create", "material_create_instance", "material_delete_expression",
  "material_function_create", "material_instance_set_parameter",
  "material_reparent_instance", "material_set_expression_property",
  "material_set_property", "mesh_add_socket", "mesh_build_bend_chain",
  "mesh_modify_socket", "mesh_remove_socket", "mesh_set_collision",
  "mesh_set_mesh_material_color", "mesh_set_physics_asset",
  "mesh_set_static_mesh_material", "mesh_set_static_mesh_properties",
  "mpc_create", "niagara_emitter_add", "niagara_emitter_add_renderer",
  "niagara_emitter_set_enabled", "niagara_emitter_set_local_space",
  "niagara_mesh_renderer_set_mesh", "niagara_module_add",
  "niagara_module_set_input", "niagara_renderer_set_alignment",
  "niagara_renderer_set_enabled", "niagara_renderer_set_material",
  "niagara_renderer_set_material_binding", "niagara_scratch_pad_module_add",
  "niagara_script_create", "niagara_system_create", "niagara_user_parameter_add",
  "niagara_user_parameter_remove", "niagara_user_parameter_set",
  "physics_material_create", "physics_set_body_collision",
  "physics_set_constraint_motion",
  "physics_set_properties", "statetree_binding_add", "statetree_binding_remove",
  "statetree_compile", "statetree_create", "statetree_node_add",
  "statetree_node_remove", "statetree_node_set_property", "statetree_save",
  "statetree_state_add", "statetree_state_duplicate", "statetree_state_move",
  "statetree_state_remove", "statetree_state_rename",
  "statetree_state_set_properties", "statetree_transition_add",
  "statetree_transition_remove", "statetree_transition_set_properties",
  "struct_create", "tag_add", "tag_move", "tag_remove", "widget_add_child",
  "widget_bind_handler", "widget_create", "widget_set_property",
]);

/** Commands that refuse `dry_run:true` with `dry_run_unsupported` (tool names). */
export const DRY_RUN_UNSUPPORTED: ReadonlySet<string> = new Set([
  "anim_anchor_feet_to_floor", "anim_normalize_z_offset",
  "asset_import_mesh",
  "anim_smooth_sequence", "bp_add_node", "datatable_create", "enum_create",
  "gas_ability_create", "gas_ability_set_cooldown", "gas_ability_set_cost",
  "gas_attributeset_create", "gas_effect_create",
  "ik_retarget_align_bones", "ik_retarget_auto_map_chains", "ik_retarget_create",
  "ik_retarget_import_pose_from_animation",
  "ik_retarget_import_pose_from_pose_asset", "ik_retarget_run_batch",
  "ik_retarget_set_chain_mapping", "ik_retarget_set_pelvis_settings",
  "ik_retarget_set_rigs", "ik_retarget_set_root_motion_settings", "input_create",
  "input_add_mapping",
  "level_new", "level_save", "level_save_as", "level_load",
  "editor_build_reflection_captures",
  "material_function_create", "mpc_create", "niagara_script_create",
  "physics_material_create",
  "pie_record_start", "pie_record_stop", "pie_record_arm", "pie_record_disarm",
  "stream_start", "stream_stop",
  "struct_create", "widget_add_child", "widget_bind_handler", "widget_create",
  "widget_set_property",
]);

export const isPieBlocked = (command: string): boolean => PIE_BLOCKED.has(command);
export const isDryRunUnsupported = (command: string): boolean =>
  DRY_RUN_UNSUPPORTED.has(command);
