/**
 * Domain: anim — skeletons, sequences, blend spaces, montages, anim blueprints,
 * state machines, notifies, sockets, and skeletal-mesh / physics-asset inspection.
 *
 * Port of the `anim_*` tools in `src/MCP/server.py`. Wire command == tool name
 * for every entry (verified against each `send_command(...)` call).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";
import { dryRun } from "./_schemas.ts";

// ── Discovery / read tools ──────────────────────────────────────────────────

const animListSkeletons = bridgeTool({
  name: "anim_list_skeletons",
  domain: "anim",
  description:
    "List all Skeleton assets in the project. Returns skeletons[] (name, path, package_path).",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const animListSequences = bridgeTool({
  name: "anim_list_sequences",
  domain: "anim",
  description:
    "List AnimSequence assets, optionally filtered by skeleton compatibility. " +
    "Returns anim_sequences[] (name, path, duration).",
  input: z.object({
    skeleton_path: z
      .string()
      .default("")
      .describe("Optional Skeleton asset path to filter by compatibility."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.skeleton_path) p.skeleton_path = a.skeleton_path;
    return p;
  },
});

const animSkeletonListSockets = bridgeTool({
  name: "anim_skeleton_list_sockets",
  domain: "anim",
  description:
    "List all sockets on a Skeleton asset. Returns sockets[] (socket_name, bone_name, " +
    "location/rotation/scale components, force_always_animated).",
  input: z.object({
    skeleton_path: z.string().describe("Full path to the Skeleton asset."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const animListBlendSpaces = bridgeTool({
  name: "anim_list_blend_spaces",
  domain: "anim",
  description:
    "List all BlendSpace assets (1D, 2D, AimOffset). Returns blend_spaces[] (name, path, type).",
  input: z.object({
    skeleton_path: z.string().default("").describe("Optional skeleton path filter."),
    package_path: z
      .string()
      .default("")
      .describe('Optional content path filter (e.g. "/Game/Animation").'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.skeleton_path) p.skeleton_path = a.skeleton_path;
    if (a.package_path) p.package_path = a.package_path;
    return p;
  },
});

const animBlendSpaceRead = bridgeTool({
  name: "anim_blend_space_read",
  domain: "anim",
  description:
    "Read a BlendSpace's full structure: axes, samples, settings. " +
    "Returns path, type, x_axis/y_axis config, samples[] with positions and animations.",
  input: z.object({
    blend_space_path: z.string().describe("Full asset path to the BlendSpace."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const animListBlueprints = bridgeTool({
  name: "anim_list_blueprints",
  domain: "anim",
  description:
    "List all Animation Blueprint assets with parent class and target skeleton.",
  input: z.object({
    skeleton_path: z.string().default("").describe("Optional skeleton path filter."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.skeleton_path) p.skeleton_path = a.skeleton_path;
    return p;
  },
});

const animListMontages = bridgeTool({
  name: "anim_list_montages",
  domain: "anim",
  description: "List all AnimMontage assets with skeleton, slot, and duration.",
  input: z.object({
    skeleton_path: z.string().default("").describe("Optional skeleton path filter."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.skeleton_path) p.skeleton_path = a.skeleton_path;
    return p;
  },
});

const animListLayerInterfaces = bridgeTool({
  name: "anim_list_layer_interfaces",
  domain: "anim",
  description:
    "List all AnimLayerInterface assets. Essential for the LinkedAnimLayer workflow.",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const animMontageRead = bridgeTool({
  name: "anim_montage_read",
  domain: "anim",
  description:
    "Read a montage's full structure: sections (with links), slot tracks, notifies, blend settings.",
  input: z.object({
    montage_path: z.string().describe("Full asset path to the AnimMontage."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const animListNotifies = bridgeTool({
  name: "anim_list_notifies",
  domain: "anim",
  description:
    "List all notifies on an AnimSequence or AnimMontage. Returns notifies[] " +
    "(index, trigger_time, duration, class, track_index, is_state).",
  input: z.object({
    anim_path: z.string().describe("Full asset path to the animation asset."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const animSkeletalMeshInspect = bridgeTool({
  name: "anim_skeletal_mesh_inspect",
  domain: "anim",
  description:
    "Dump a SkeletalMesh's structure: bones, render sections, material slots, " +
    "clothing assets, physics asset, skeleton. Blocked during PIE (asset load).",
  input: z.object({
    path: z.string().describe("Content-browser path to the SkeletalMesh asset."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true, blockedDuringPie: true },
});

const animPhysicsInspect = bridgeTool({
  name: "anim_physics_inspect",
  domain: "anim",
  description:
    "Dump a UPhysicsAsset: per-body bone mapping, collision settings, per-shape geometry " +
    "(capsule/sphere/box), and constraint bone-pairs. Blocked during PIE (asset load).",
  input: z.object({
    path: z.string().describe("Content-browser path to the physics asset."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true, blockedDuringPie: true },
});

// ── BlendSpace mutators ─────────────────────────────────────────────────────

const animBlendSpaceCreate = bridgeTool({
  name: "anim_blend_space_create",
  domain: "anim",
  description:
    "Create a BlendSpace (1D or 2D, optionally AimOffset) with axis config and optional " +
    "initial samples. Required: name, skeleton_path. y_axis only sent when is_2d.",
  input: z.object({
    name: z.string().describe('Asset name (e.g. "BS1D_Locomotion").'),
    skeleton_path: z.string().describe("Full path to the target Skeleton asset."),
    is_2d: z.boolean().default(false).describe("Create a 2D BlendSpace (default 1D)."),
    is_aim_offset: z
      .boolean()
      .default(false)
      .describe("Create an AimOffset (additive blend space) instead of a regular BlendSpace."),
    package_path: z
      .string()
      .default("/Game/BlendSpaces")
      .describe("Content Browser folder."),
    x_axis_name: z.string().default("Speed").describe("Display name for the X axis."),
    x_axis_min: z.number().default(0.0).describe("Minimum value for X axis."),
    x_axis_max: z.number().default(100.0).describe("Maximum value for X axis."),
    x_axis_grid_divisions: z
      .number()
      .default(4)
      .describe("Number of grid divisions on X axis."),
    y_axis_name: z.string().default("Direction").describe("Display name for Y axis (2D only)."),
    y_axis_min: z.number().default(-180.0).describe("Minimum value for Y axis (2D only)."),
    y_axis_max: z.number().default(180.0).describe("Maximum value for Y axis (2D only)."),
    y_axis_grid_divisions: z
      .number()
      .default(4)
      .describe("Grid divisions on Y axis (2D only)."),
    samples: z
      .array(
        z.object({
          anim_sequence: z.string().describe("Full path to an AnimSequence asset."),
          x: z.number().describe("X coordinate in blend space."),
          y: z.number().optional().describe("Y coordinate (2D only, default 0)."),
        }),
      )
      .optional()
      .describe("Optional list of initial samples."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      name: a.name,
      skeleton_path: a.skeleton_path,
      is_2d: a.is_2d,
      is_aim_offset: a.is_aim_offset,
      package_path: a.package_path,
      x_axis: {
        name: a.x_axis_name,
        min: a.x_axis_min,
        max: a.x_axis_max,
        grid_divisions: a.x_axis_grid_divisions,
      },
    };
    if (a.is_2d) {
      p.y_axis = {
        name: a.y_axis_name,
        min: a.y_axis_min,
        max: a.y_axis_max,
        grid_divisions: a.y_axis_grid_divisions,
      };
    }
    if (a.samples && a.samples.length > 0) p.samples = a.samples;
    return p;
  },
});

const animBlendSpaceAddSample = bridgeTool({
  name: "anim_blend_space_add_sample",
  domain: "anim",
  description:
    "Add a sample (animation at a coordinate) to an existing BlendSpace. Supports dry_run.",
  input: z.object({
    blend_space_path: z.string().describe("Full path to the BlendSpace asset."),
    anim_sequence: z.string().describe("Full path to the AnimSequence to add as a sample."),
    x: z.number().default(0.0).describe("X coordinate in the blend space."),
    y: z.number().default(0.0).describe("Y coordinate (for 2D blend spaces)."),
    dry_run: dryRun.describe("Preview without adding."),
  }),
  annotations: { blockedDuringPie: true },
});

const animBlendSpaceRemoveSample = bridgeTool({
  name: "anim_blend_space_remove_sample",
  domain: "anim",
  description:
    "Remove a sample from a BlendSpace by 0-based index. Supports dry_run.",
  input: z.object({
    blend_space_path: z.string().describe("Full path to the BlendSpace asset."),
    sample_index: z.number().describe("Index of the sample to remove (0-based)."),
    dry_run: dryRun.describe("Preview without removing."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

// ── AnimSequence property + cleanup mutators ────────────────────────────────

const animSequenceSetProperty = bridgeTool({
  name: "anim_sequence_set_property",
  domain: "anim",
  description:
    "Set properties on an AnimSequence — primarily additive settings for aim offsets. " +
    "Only provided fields are applied; invalid enum strings error loudly.",
  input: z.object({
    anim_path: z.string().describe("Full path to the AnimSequence asset."),
    additive_anim_type: z
      .string()
      .optional()
      .describe('None/NoAdditive, LocalSpace, or MeshSpace (engine AAT_* names also accepted).'),
    base_pose_type: z
      .string()
      .optional()
      .describe("None, AnimFrame, AnimScale, SelectedAnimationFrame, or LocalAnimFrame."),
    base_pose_anim: z
      .string()
      .optional()
      .describe("Full path to the base pose AnimSequence (for AnimScaled/AnimFrame types)."),
    ref_frame_index: z
      .number()
      .optional()
      .describe("Reference frame index (for AnimFrame / LocalAnimFrame types)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = { anim_path: a.anim_path };
    if (a.additive_anim_type !== undefined) p.additive_anim_type = a.additive_anim_type;
    if (a.base_pose_type !== undefined) p.base_pose_type = a.base_pose_type;
    if (a.base_pose_anim !== undefined) p.base_pose_anim = a.base_pose_anim;
    if (a.ref_frame_index !== undefined) p.ref_frame_index = a.ref_frame_index;
    return p;
  },
});

const animSmoothSequence = bridgeTool({
  name: "anim_smooth_sequence",
  domain: "anim",
  description:
    "Apply a sliding-window smoothing filter (box or gaussian) to a UAnimSequence's bone tracks. " +
    "Outputs a duplicated _Smoothed copy by default (empty output_suffix = in place). No dry_run.",
  input: z.object({
    anim_path: z.string().describe("Asset path of the UAnimSequence to smooth."),
    window_size: z
      .number()
      .default(5)
      .describe("Odd window size (forced odd). 3 mild, 5 balanced, 9+ aggressive."),
    filter_type: z.string().default("box").describe("'box' (default) or 'gaussian'."),
    sigma_frames: z
      .number()
      .optional()
      .describe("Gaussian only: overrides default sigma = window/6."),
    output_suffix: z
      .string()
      .default("_Smoothed")
      .describe("Non-empty duplicates+smooths the copy; empty smooths in place."),
    bone_substring_filter: z
      .array(z.string())
      .optional()
      .describe("Case-insensitive substrings; only matching bones are smoothed (empty = all)."),
    smooth_positions: z
      .boolean()
      .default(false)
      .describe("Also smooth positional keys (most bones don't animate position)."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      anim_path: a.anim_path,
      window_size: a.window_size,
      filter_type: a.filter_type,
      output_suffix: a.output_suffix,
      smooth_positions: a.smooth_positions,
    };
    if (a.sigma_frames !== undefined) p.sigma_frames = a.sigma_frames;
    if (a.bone_substring_filter !== undefined) p.bone_substring_filter = a.bone_substring_filter;
    return p;
  },
});

const animNormalizeZOffset = bridgeTool({
  name: "anim_normalize_z_offset",
  domain: "anim",
  description:
    "Rebase a UAnimSequence so its frame-0 bone Z lands at target_z, subtracting one Z delta " +
    "from every frame. Outputs a _ZNorm copy by default (empty output_suffix = in place). No dry_run.",
  input: z.object({
    anim_path: z.string().describe("Asset path of the UAnimSequence to rebase."),
    target_z: z.number().default(0.0).describe("Z value frame 0 of each matched bone should land at."),
    bone_substring_filter: z
      .array(z.string())
      .optional()
      .describe("Case-insensitive substrings; only matching bones normalized (default ['root'])."),
    output_suffix: z
      .string()
      .default("_ZNorm")
      .describe("Non-empty duplicates+rebases the copy; empty rebases in place."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      anim_path: a.anim_path,
      target_z: a.target_z,
      output_suffix: a.output_suffix,
    };
    if (a.bone_substring_filter !== undefined) p.bone_substring_filter = a.bone_substring_filter;
    return p;
  },
});

const animAnchorFeetToFloor = bridgeTool({
  name: "anim_anchor_feet_to_floor",
  domain: "anim",
  description:
    "Anchor a UAnimSequence so feet rest at floor level by FK-composing foot world Z over the " +
    "first N frames (median) and shifting the pelvis Z curve. Outputs a _FootAnchored copy by " +
    "default (empty output_suffix = in place). No dry_run.",
  input: z.object({
    anim_path: z.string().describe("UAnimSequence to anchor (typically retargeted output)."),
    foot_bone_substring: z.string().default("foot_l").describe("Substring matching a foot bone."),
    pelvis_bone_substring: z
      .string()
      .default("pelvis")
      .describe("Substring matching the bone whose Z carries the offset."),
    target_z: z.number().default(0.0).describe("Where the median foot Z should land (floor)."),
    sample_frames: z
      .number()
      .default(10)
      .describe("Leading frames sampled for the median (1 = frame-0-only)."),
    output_suffix: z
      .string()
      .default("_FootAnchored")
      .describe("Non-empty duplicates+anchors the copy; empty anchors in place."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
});

// ── Skeleton socket mutators ────────────────────────────────────────────────

const animSkeletonAddSocket = bridgeTool({
  name: "anim_skeleton_add_socket",
  domain: "anim",
  description:
    "Create a socket on a Skeleton attached to a bone, with relative location/rotation/scale. " +
    "Supports dry_run. socket_name must be unique on the skeleton.",
  input: z.object({
    skeleton_path: z.string().describe("Full path to the Skeleton asset."),
    socket_name: z.string().describe("Name for the new socket (unique on this skeleton)."),
    bone_name: z.string().describe("Name of the bone to attach the socket to."),
    location_x: z.number().default(0.0).describe("Relative location offset X."),
    location_y: z.number().default(0.0).describe("Relative location offset Y."),
    location_z: z.number().default(0.0).describe("Relative location offset Z."),
    rotation_pitch: z.number().default(0.0).describe("Relative rotation pitch."),
    rotation_yaw: z.number().default(0.0).describe("Relative rotation yaw."),
    rotation_roll: z.number().default(0.0).describe("Relative rotation roll."),
    scale_x: z.number().default(1.0).describe("Relative scale X."),
    scale_y: z.number().default(1.0).describe("Relative scale Y."),
    scale_z: z.number().default(1.0).describe("Relative scale Z."),
    dry_run: dryRun.describe("Preview without creating."),
  }),
  annotations: { blockedDuringPie: true },
});

const animSkeletonModifySocket = bridgeTool({
  name: "anim_skeleton_modify_socket",
  domain: "anim",
  description:
    "Modify an existing socket on a Skeleton. Only provided fields are updated.",
  input: z.object({
    skeleton_path: z.string().describe("Full path to the Skeleton asset."),
    socket_name: z.string().describe("Name of the socket to modify."),
    bone_name: z.string().optional().describe("New parent bone."),
    location_x: z.number().optional().describe("New relative location X."),
    location_y: z.number().optional().describe("New relative location Y."),
    location_z: z.number().optional().describe("New relative location Z."),
    rotation_pitch: z.number().optional().describe("New relative rotation pitch."),
    rotation_yaw: z.number().optional().describe("New relative rotation yaw."),
    rotation_roll: z.number().optional().describe("New relative rotation roll."),
    scale_x: z.number().optional().describe("New relative scale X."),
    scale_y: z.number().optional().describe("New relative scale Y."),
    scale_z: z.number().optional().describe("New relative scale Z."),
    force_always_animated: z
      .boolean()
      .optional()
      .describe("Force bone hierarchy evaluation even at low LOD."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      skeleton_path: a.skeleton_path,
      socket_name: a.socket_name,
    };
    if (a.bone_name !== undefined) p.bone_name = a.bone_name;
    if (a.location_x !== undefined) p.location_x = a.location_x;
    if (a.location_y !== undefined) p.location_y = a.location_y;
    if (a.location_z !== undefined) p.location_z = a.location_z;
    if (a.rotation_pitch !== undefined) p.rotation_pitch = a.rotation_pitch;
    if (a.rotation_yaw !== undefined) p.rotation_yaw = a.rotation_yaw;
    if (a.rotation_roll !== undefined) p.rotation_roll = a.rotation_roll;
    if (a.scale_x !== undefined) p.scale_x = a.scale_x;
    if (a.scale_y !== undefined) p.scale_y = a.scale_y;
    if (a.scale_z !== undefined) p.scale_z = a.scale_z;
    if (a.force_always_animated !== undefined)
      p.force_always_animated = a.force_always_animated;
    return p;
  },
});

const animSkeletonRemoveSocket = bridgeTool({
  name: "anim_skeleton_remove_socket",
  domain: "anim",
  description:
    "Remove a socket from a Skeleton by name. Returns removed_socket and remaining_sockets count.",
  input: z.object({
    skeleton_path: z.string().describe("Full path to the Skeleton asset."),
    socket_name: z.string().describe("Name of the socket to remove."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

// ── State machine mutators ──────────────────────────────────────────────────

const animStateMachineCreate = bridgeTool({
  name: "anim_state_machine_create",
  domain: "anim",
  description:
    "Create a state machine node in an AnimGraph. Returns node_id and machine_name " +
    "(the state machine graph name for use in state_add).",
  input: z.object({
    blueprint_name: z.string().describe("Name of the Animation Blueprint."),
    machine_name: z.string().default("StateMachine").describe("Name for the state machine."),
    pos_x: z.number().default(0).describe("X position in the AnimGraph."),
    pos_y: z.number().default(0).describe("Y position in the AnimGraph."),
    graph_name: z.string().default("AnimGraph").describe("Target graph."),
  }),
  annotations: { blockedDuringPie: true },
});

const animStateMachineStateAdd = bridgeTool({
  name: "anim_state_machine_state_add",
  domain: "anim",
  description:
    "Add a state to a state machine. Returns node_id, state_name, inner_graph " +
    "(the state's inner AnimGraph name).",
  input: z.object({
    blueprint_name: z.string().describe("Name of the Animation Blueprint."),
    state_machine_graph: z.string().describe("State machine graph name (from state_machine_create)."),
    state_name: z.string().describe("Name for the new state."),
    pos_x: z.number().default(0).describe("X position in the state machine graph."),
    pos_y: z.number().default(0).describe("Y position in the state machine graph."),
  }),
  annotations: { blockedDuringPie: true },
});

const animStateMachineTransitionAdd = bridgeTool({
  name: "anim_state_machine_transition_add",
  domain: "anim",
  description:
    "Add a transition between two states. Returns node_id, from_state, to_state, rule_graph " +
    "(for adding the transition condition nodes).",
  input: z.object({
    blueprint_name: z.string().describe("Name of the Animation Blueprint."),
    state_machine_graph: z.string().describe("State machine graph name."),
    from_state: z.string().describe("Source state name."),
    to_state: z.string().describe("Target state name."),
    blend_duration: z.number().optional().describe("Crossfade time in seconds."),
    priority_order: z
      .number()
      .optional()
      .describe("Priority when multiple transitions are valid (lower = higher)."),
    bidirectional: z.boolean().optional().describe("Create reverse transition automatically."),
    automatic_rule_based_on_sequence_player: z
      .boolean()
      .optional()
      .describe("Auto-transition when the animation ends."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      state_machine_graph: a.state_machine_graph,
      from_state: a.from_state,
      to_state: a.to_state,
    };
    if (a.blend_duration !== undefined) p.blend_duration = a.blend_duration;
    if (a.priority_order !== undefined) p.priority_order = a.priority_order;
    if (a.bidirectional !== undefined) p.bidirectional = a.bidirectional;
    if (a.automatic_rule_based_on_sequence_player !== undefined)
      p.automatic_rule_based_on_sequence_player = a.automatic_rule_based_on_sequence_player;
    return p;
  },
});

const animStateMachineSetEntry = bridgeTool({
  name: "anim_state_machine_set_entry",
  domain: "anim",
  description: "Set which state the state machine enters first.",
  input: z.object({
    blueprint_name: z.string().describe("Name of the Animation Blueprint."),
    state_machine_graph: z.string().describe("State machine graph name."),
    state_name: z.string().describe("Name of the state to set as entry."),
  }),
  annotations: { blockedDuringPie: true },
});

const animStateMachineModifyTransition = bridgeTool({
  name: "anim_state_machine_modify_transition",
  domain: "anim",
  description:
    "Modify an existing transition's properties. Identify by transition_id OR from_state + to_state.",
  input: z.object({
    blueprint_name: z.string().describe("Name of the Animation Blueprint."),
    state_machine_graph: z.string().describe("State machine graph name."),
    transition_id: z.string().optional().describe("Node ID of the transition."),
    from_state: z.string().optional().describe("Source state name (alternative to transition_id)."),
    to_state: z.string().optional().describe("Target state name (alternative to transition_id)."),
    blend_duration: z.number().optional().describe("Crossfade time in seconds."),
    priority_order: z.number().optional().describe("Priority when multiple transitions are valid."),
    bidirectional: z.boolean().optional().describe("Whether transition works in both directions."),
    automatic_rule_based_on_sequence_player: z
      .boolean()
      .optional()
      .describe("Auto-transition when the animation ends."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      state_machine_graph: a.state_machine_graph,
    };
    if (a.transition_id) p.transition_id = a.transition_id;
    if (a.from_state) p.from_state = a.from_state;
    if (a.to_state) p.to_state = a.to_state;
    if (a.blend_duration !== undefined) p.blend_duration = a.blend_duration;
    if (a.priority_order !== undefined) p.priority_order = a.priority_order;
    if (a.bidirectional !== undefined) p.bidirectional = a.bidirectional;
    if (a.automatic_rule_based_on_sequence_player !== undefined)
      p.automatic_rule_based_on_sequence_player = a.automatic_rule_based_on_sequence_player;
    return p;
  },
});

const animStateMachineStateRemove = bridgeTool({
  name: "anim_state_machine_state_remove",
  domain: "anim",
  description: "Remove a state and all its transitions from a state machine.",
  input: z.object({
    blueprint_name: z.string().describe("Name of the Animation Blueprint."),
    state_machine_graph: z.string().describe("State machine graph name."),
    state_name: z.string().describe("Name of the state to remove."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const animStateMachineTransitionRemove = bridgeTool({
  name: "anim_state_machine_transition_remove",
  domain: "anim",
  description: "Remove a transition. Identify by transition_id OR from_state + to_state.",
  input: z.object({
    blueprint_name: z.string().describe("Name of the Animation Blueprint."),
    state_machine_graph: z.string().describe("State machine graph name."),
    transition_id: z.string().optional().describe("Node ID of the transition."),
    from_state: z.string().optional().describe("Source state name (alternative to transition_id)."),
    to_state: z.string().optional().describe("Target state name (alternative to transition_id)."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      state_machine_graph: a.state_machine_graph,
    };
    if (a.transition_id) p.transition_id = a.transition_id;
    if (a.from_state) p.from_state = a.from_state;
    if (a.to_state) p.to_state = a.to_state;
    return p;
  },
});

// ── AnimGraph node binding ──────────────────────────────────────────────────

const animNodeBindProperty = bridgeTool({
  name: "anim_node_bind_property",
  domain: "anim",
  description:
    "Bind an AnimGraph node's input property to a Blueprint/AnimInstance variable (the binding " +
    "chip on a pin). Returns success, node_id, property_name, variable_name, pin_toggled_visible.",
  input: z.object({
    blueprint_name: z.string().describe("Name (or /Game/... path) of the Anim Blueprint."),
    node_id: z.string().describe("Node name or NodeGuid of the target UAnimGraphNode_Base."),
    property_name: z.string().describe("Target binding name on the node's FAnimNode struct."),
    variable_name: z.string().describe("AnimInstance/Blueprint variable to bind to."),
    graph_name: z.string().optional().describe("Optional graph name; searches all if omitted."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      blueprint_name: a.blueprint_name,
      node_id: a.node_id,
      property_name: a.property_name,
      variable_name: a.variable_name,
    };
    if (a.graph_name) p.graph_name = a.graph_name;
    return p;
  },
});

// ── Notify mutators ─────────────────────────────────────────────────────────

const animNotifyAdd = bridgeTool({
  name: "anim_notify_add",
  domain: "anim",
  description:
    "Add a notify event to an AnimSequence or AnimMontage at trigger_time. Supports dry_run. " +
    "duration sets a UAnimNotifyState window.",
  input: z.object({
    anim_path: z.string().describe("Full asset path to the AnimSequence or AnimMontage."),
    notify_class: z
      .string()
      .describe('Notify class name (e.g. "AnimNotify_PlaySound").'),
    trigger_time: z.number().describe("Time in seconds to trigger the notify."),
    track_index: z.number().default(0).describe("Notify track index."),
    duration: z
      .number()
      .optional()
      .describe("For UAnimNotifyState (stateful notifies), the duration in seconds."),
    dry_run: dryRun.describe("Validate without committing."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      anim_path: a.anim_path,
      notify_class: a.notify_class,
      trigger_time: a.trigger_time,
      track_index: a.track_index,
      dry_run: a.dry_run,
    };
    if (a.duration !== undefined) p.duration = a.duration;
    return p;
  },
});

const animNotifyRemove = bridgeTool({
  name: "anim_notify_remove",
  domain: "anim",
  description: "Remove a notify by index from an animation asset. Supports dry_run.",
  input: z.object({
    anim_path: z.string().describe("Full asset path to the animation asset."),
    notify_index: z.number().describe("Index of the notify to remove (from anim_list_notifies)."),
    dry_run: dryRun.describe("Preview without removing."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const animExtractBetweenNotifies = bridgeTool({
  name: "anim_extract_between_notifies",
  domain: "anim",
  description:
    "Slice a sub-clip from a UAnimSequence into a new asset, cropping between a start and end " +
    "boundary (by notify name or explicit time; notify wins). Required: source_path, dest_name.",
  input: z.object({
    source_path: z.string().describe("Full /Game/... path to the source UAnimSequence."),
    dest_name: z.string().describe("Short asset name for the new clip (no path/extension)."),
    start_notify: z.string().optional().describe("Name of the notify marking the window start."),
    end_notify: z.string().optional().describe("Name of the notify marking the window end."),
    start_time: z.number().optional().describe("Window start in seconds (if start_notify omitted)."),
    end_time: z.number().optional().describe("Window end in seconds (if end_notify omitted)."),
    start_occurrence: z
      .number()
      .default(0)
      .describe("Which match of start_notify to use (0-based)."),
    end_occurrence: z.number().default(0).describe("Which match of end_notify to use (0-based)."),
    dest_path: z.string().optional().describe("Destination folder (default: source's own folder)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      source_path: a.source_path,
      dest_name: a.dest_name,
      start_occurrence: a.start_occurrence,
      end_occurrence: a.end_occurrence,
    };
    if (a.start_notify !== undefined) p.start_notify = a.start_notify;
    if (a.end_notify !== undefined) p.end_notify = a.end_notify;
    if (a.start_time !== undefined) p.start_time = a.start_time;
    if (a.end_time !== undefined) p.end_time = a.end_time;
    if (a.dest_path !== undefined) p.dest_path = a.dest_path;
    return p;
  },
});

// ── Montage mutators ────────────────────────────────────────────────────────

const animMontageCreate = bridgeTool({
  name: "anim_montage_create",
  domain: "anim",
  description:
    "Create a new AnimMontage asset. Required: name, skeleton_path. Returns path, name, duration.",
  input: z.object({
    name: z.string().describe("Name for the new montage."),
    skeleton_path: z.string().describe("Full path to the target skeleton."),
    source_sequence: z.string().optional().describe("Optional AnimSequence to build from."),
    package_path: z.string().default("/Game/Montages").describe("Content directory for the asset."),
    slot_name: z.string().default("DefaultSlot").describe("Slot name for montage playback."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      name: a.name,
      skeleton_path: a.skeleton_path,
      package_path: a.package_path,
      slot_name: a.slot_name,
    };
    if (a.source_sequence) p.source_sequence = a.source_sequence;
    return p;
  },
});

const animMontageAddSection = bridgeTool({
  name: "anim_montage_add_section",
  domain: "anim",
  description: "Add a named section to a montage at a specific time.",
  input: z.object({
    montage_path: z.string().describe("Full asset path to the AnimMontage."),
    section_name: z.string().describe("Name for the new section."),
    start_time: z.number().default(0).describe("Start time in seconds."),
  }),
  annotations: { blockedDuringPie: true },
});

const animMontageSetSectionLink = bridgeTool({
  name: "anim_montage_set_section_link",
  domain: "anim",
  description: "Link montage sections for sequential playback (section A -> section B).",
  input: z.object({
    montage_path: z.string().describe("Full asset path to the AnimMontage."),
    section_name: z.string().describe("Source section name."),
    next_section_name: z.string().describe("Section to play next."),
  }),
  annotations: { blockedDuringPie: true },
});

const animMontageSetBlend = bridgeTool({
  name: "anim_montage_set_blend",
  domain: "anim",
  description: "Configure montage blend-in and blend-out settings. Only provided fields are applied.",
  input: z.object({
    montage_path: z.string().describe("Full asset path to the AnimMontage."),
    blend_in_time: z.number().optional().describe("Blend-in duration in seconds."),
    blend_out_time: z.number().optional().describe("Blend-out duration in seconds."),
    blend_out_trigger_time: z
      .number()
      .optional()
      .describe("Time before end to start blend-out (negative = from end)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = { montage_path: a.montage_path };
    if (a.blend_in_time !== undefined) p.blend_in_time = a.blend_in_time;
    if (a.blend_out_time !== undefined) p.blend_out_time = a.blend_out_time;
    if (a.blend_out_trigger_time !== undefined)
      p.blend_out_trigger_time = a.blend_out_trigger_time;
    return p;
  },
});

// ── Animation Blueprint mutators ────────────────────────────────────────────

const animBlueprintCreate = bridgeTool({
  name: "anim_blueprint_create",
  domain: "anim",
  description:
    "Create an Animation Blueprint with proper skeleton + AnimGraph (output pose node) setup. " +
    "Preferred over bp_create + reparent. Returns path, name, anim_graph, output_pose_node_id, skeleton.",
  input: z.object({
    name: z.string().describe("Name for the new AnimBP."),
    skeleton_path: z.string().describe("Full path to the target skeleton."),
    parent_class: z
      .string()
      .optional()
      .describe('Optional parent class path (defaults to UAnimInstance).'),
    package_path: z.string().default("/Game/Animation").describe("Content directory."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      name: a.name,
      skeleton_path: a.skeleton_path,
      package_path: a.package_path,
    };
    if (a.parent_class) p.parent_class = a.parent_class;
    return p;
  },
});

const animBlueprintSetSkeleton = bridgeTool({
  name: "anim_blueprint_set_skeleton",
  domain: "anim",
  description:
    "Change the target skeleton of an existing AnimBP. Triggers recompilation.",
  input: z.object({
    blueprint_path: z.string().describe("Path or name of the Animation Blueprint."),
    skeleton_path: z.string().describe("Full path to the new target skeleton."),
  }),
  annotations: { blockedDuringPie: true },
});

// ── Skeletal mesh section mutator ───────────────────────────────────────────

const animSkeletalMeshSetSectionDisabled = bridgeTool({
  name: "anim_skeletal_mesh_set_section_disabled",
  domain: "anim",
  description:
    "Toggle the bDisabled flag on a SkeletalMesh render section (persisted to disk). " +
    "Disabled sections don't render.",
  input: z.object({
    path: z.string().describe("SkeletalMesh content-browser path."),
    section_index: z.number().describe("Index into the LOD's RenderSections array."),
    disabled: z.boolean().default(true).describe("True to hide the section, False to show."),
    lod_index: z.number().default(0).describe("LOD level."),
  }),
  annotations: { blockedDuringPie: true },
});

export const animTools: ToolDef[] = [
  animListSkeletons,
  animListSequences,
  animSkeletonListSockets,
  animListBlendSpaces,
  animBlendSpaceRead,
  animListBlueprints,
  animListMontages,
  animListLayerInterfaces,
  animMontageRead,
  animListNotifies,
  animSkeletalMeshInspect,
  animPhysicsInspect,
  animBlendSpaceCreate,
  animBlendSpaceAddSample,
  animBlendSpaceRemoveSample,
  animSequenceSetProperty,
  animSmoothSequence,
  animNormalizeZOffset,
  animAnchorFeetToFloor,
  animSkeletonAddSocket,
  animSkeletonModifySocket,
  animSkeletonRemoveSocket,
  animStateMachineCreate,
  animStateMachineStateAdd,
  animStateMachineTransitionAdd,
  animStateMachineSetEntry,
  animStateMachineModifyTransition,
  animStateMachineStateRemove,
  animStateMachineTransitionRemove,
  animNodeBindProperty,
  animNotifyAdd,
  animNotifyRemove,
  animExtractBetweenNotifies,
  animMontageCreate,
  animMontageAddSection,
  animMontageSetSectionLink,
  animMontageSetBlend,
  animBlueprintCreate,
  animBlueprintSetSkeleton,
  animSkeletalMeshSetSectionDisabled,
];
