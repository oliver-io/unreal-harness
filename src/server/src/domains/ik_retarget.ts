/**
 * Domain: ik_retarget — UIKRetargeter authoring (rigs, chain mappings, retarget
 * poses, op settings) and cross-skeleton batch retargeting.
 *
 * Port of the `ik_retarget_*` tools in `src/MCP/server.py`. Every mutator here is
 * dry-run-unsupported (atomic asset side effect, saves before returning) and
 * blocked during PIE — both per the C++ blocklists in `MCPCommonUtils.cpp`.
 * Wire command names equal the tool names post naming-migration.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

// ── read ────────────────────────────────────────────────────────────────────

const ikRetargetRead = bridgeTool({
  name: "ik_retarget_read",
  domain: "ik_retarget",
  description:
    "Inspect a UIKRetargeter: source/target rig paths and current chain mappings " +
    "(target_chain → source_chain, empty when unmapped). Read counterpart to " +
    "ik_retarget_set_chain_mapping / ik_retarget_auto_map_chains.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of a UIKRetargeter."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

// ── create / wire rigs ──────────────────────────────────────────────────────

const ikRetargetCreate = bridgeTool({
  name: "ik_retarget_create",
  domain: "ik_retarget",
  description:
    "Create a new UIKRetargeter asset (factory runs AddDefaultOps → standard FK+IK " +
    "op stack), optionally wiring source/target rigs in the same call. Provide " +
    "asset_path, OR path (package dir) + name. dry_run unsupported.",
  input: z.object({
    asset_path: z
      .string()
      .default("")
      .describe('Full asset path, e.g. "/Game/Anim/Retargeters/RTG_Foo". Alternative: path + name.'),
    path: z.string().default("").describe("Package dir (used with name when asset_path omitted)."),
    name: z.string().default("").describe("Asset name (used with path when asset_path omitted)."),
    source_ik_rig_path: z.string().default("").describe("Optional source UIKRigDefinition asset path."),
    target_ik_rig_path: z.string().default("").describe("Optional target UIKRigDefinition asset path."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.source_ik_rig_path) p.source_ik_rig_path = a.source_ik_rig_path;
    if (a.target_ik_rig_path) p.target_ik_rig_path = a.target_ik_rig_path;
    return p;
  },
});

const ikRetargetSetRigs = bridgeTool({
  name: "ik_retarget_set_rigs",
  domain: "ik_retarget",
  description:
    "Set source and/or target IK rigs on an existing UIKRetargeter (at least one " +
    "required). rebuild_ops=true (default) wipes + re-adds the op stack so chain " +
    "mappings re-anchor; false preserves a vendor retargeter's baked-in op state. " +
    "dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of the UIKRetargeter to mutate."),
    source_ik_rig_path: z.string().default("").describe("Optional new source UIKRigDefinition asset path."),
    target_ik_rig_path: z.string().default("").describe("Optional new target UIKRigDefinition asset path."),
    rebuild_ops: z
      .boolean()
      .default(true)
      .describe("Rebuild default op stack after rig swap (default true); false preserves existing ops."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      retargeter_path: a.retargeter_path,
      rebuild_ops: a.rebuild_ops,
    };
    if (a.source_ik_rig_path) p.source_ik_rig_path = a.source_ik_rig_path;
    if (a.target_ik_rig_path) p.target_ik_rig_path = a.target_ik_rig_path;
    return p;
  },
});

// ── chain mapping ───────────────────────────────────────────────────────────

const ikRetargetAutoMapChains = bridgeTool({
  name: "ik_retarget_auto_map_chains",
  domain: "ik_retarget",
  description:
    'Bulk-wire target chains to source chains by name ("Map All"). Both rigs must ' +
    'already be set. match_type: "Exact" | "Fuzzy" (default, Levenshtein) | "Clear". ' +
    "force_remap overwrites existing mappings; align_target_pose resets target pose + " +
    "AutoAlignAllBones to match source. dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of the UIKRetargeter."),
    match_type: z
      .string()
      .default("Fuzzy")
      .describe('"Exact" (identical names), "Fuzzy" (default, closest by Levenshtein), or "Clear".'),
    force_remap: z
      .boolean()
      .default(true)
      .describe("Overwrite existing mappings (default true); false only fills unmapped chains."),
    align_target_pose: z
      .boolean()
      .default(true)
      .describe("Reset target pose + AutoAlignAllBones(ChainToChain) after mapping (default true)."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
});

const ikRetargetSetChainMapping = bridgeTool({
  name: "ik_retarget_set_chain_mapping",
  domain: "ik_retarget",
  description:
    "Wire one source chain to one target chain on the retargeter (manual fix after " +
    "auto-map). Empty source_chain clears the mapping (sets None). dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of the UIKRetargeter."),
    target_chain: z.string().min(1).describe("Name of a target-IK-rig chain."),
    source_chain: z
      .string()
      .default("")
      .describe("Name of a source-IK-rig chain, or empty to clear the mapping."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
});

const ikRetargetAlignBones = bridgeTool({
  name: "ik_retarget_align_bones",
  domain: "ik_retarget",
  description:
    "Reset (optional) and run AutoAlignAllBones on one side of a retargeter so its " +
    "chain directions match the other side's current retarget pose. excluded_bones " +
    "are reset back to bind pose AFTER align (for garbage single-bone chains). " +
    "dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of the UIKRetargeter."),
    source_or_target: z
      .string()
      .default("target")
      .describe('"source" or "target" (default "target") — which side to align.'),
    reset_first: z
      .boolean()
      .default(true)
      .describe("Reset the side's retarget pose before aligning (default true)."),
    excluded_bones: z
      .array(z.string())
      .default([])
      .describe("Bone names to reset back to bind pose AFTER auto-align runs."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
});

// ── op settings ─────────────────────────────────────────────────────────────

const ikRetargetSetPelvisSettings = bridgeTool({
  name: "ik_retarget_set_pelvis_settings",
  domain: "ik_retarget",
  description:
    "Tune the Pelvis Motion Op (UE 5.7 ops architecture) on a UIKRetargeter. Only " +
    "the parameters you pass are written; rest left as-is. E.g. scale_vertical=0.0 " +
    "locks pelvis Z to kill SMPL-noise knee bounce. Errors if no Pelvis Motion op. " +
    "dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of the UIKRetargeter to mutate."),
    translation_alpha: z.number().optional().describe("0..1 overall pelvis translation blend (0 freezes)."),
    scale_horizontal: z.number().optional().describe("Per-axis scale for pelvis X/Y translation (default 1.0; 0 freezes)."),
    scale_vertical: z.number().optional().describe("Per-axis scale for pelvis Z translation (default 1.0; 0 freezes)."),
    blend_to_source_translation: z.number().optional().describe("0..1 blend toward source's exact pelvis location (default 0)."),
    blend_to_source_x: z.number().optional().describe("Per-axis weight for blend-to-source X (default 1)."),
    blend_to_source_y: z.number().optional().describe("Per-axis weight for blend-to-source Y (default 1)."),
    blend_to_source_z: z.number().optional().describe("Per-axis weight for blend-to-source Z (default 1)."),
    rotation_alpha: z.number().optional().describe("0..1 pelvis rotation blend (0 freezes)."),
    affect_ik_horizontal: z.number().optional().describe("0..1 how pelvis horizontal motion affects IK goals (default 1)."),
    affect_ik_vertical: z.number().optional().describe("0..1 how pelvis vertical motion affects IK goals (default 0)."),
    floor_constraint_weight: z.number().optional().describe("0..1 enable the asymmetric floor-clamping constraint."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = { retargeter_path: a.retargeter_path };
    if (a.translation_alpha !== undefined) p.translation_alpha = a.translation_alpha;
    if (a.scale_horizontal !== undefined) p.scale_horizontal = a.scale_horizontal;
    if (a.scale_vertical !== undefined) p.scale_vertical = a.scale_vertical;
    if (a.blend_to_source_translation !== undefined) p.blend_to_source_translation = a.blend_to_source_translation;
    if (a.blend_to_source_x !== undefined) p.blend_to_source_x = a.blend_to_source_x;
    if (a.blend_to_source_y !== undefined) p.blend_to_source_y = a.blend_to_source_y;
    if (a.blend_to_source_z !== undefined) p.blend_to_source_z = a.blend_to_source_z;
    if (a.rotation_alpha !== undefined) p.rotation_alpha = a.rotation_alpha;
    if (a.affect_ik_horizontal !== undefined) p.affect_ik_horizontal = a.affect_ik_horizontal;
    if (a.affect_ik_vertical !== undefined) p.affect_ik_vertical = a.affect_ik_vertical;
    if (a.floor_constraint_weight !== undefined) p.floor_constraint_weight = a.floor_constraint_weight;
    return p;
  },
});

const ikRetargetSetRootMotionSettings = bridgeTool({
  name: "ik_retarget_set_root_motion_settings",
  domain: "ik_retarget",
  description:
    "Tune the Root Motion Op on a UIKRetargeter. Only passed params are written. " +
    'root_height_source="snap_to_ground" locks root Z to 0 — the fix for SMPL ' +
    "body-bob + knee-compensation that pelvis settings don't affect. dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of the UIKRetargeter to mutate."),
    root_motion_source: z
      .string()
      .optional()
      .describe('"copy_from_source_root" or "generate_from_target_pelvis".'),
    root_height_source: z
      .string()
      .optional()
      .describe('"copy_height_from_source" or "snap_to_ground" (lock root Z to 0).'),
    rotate_with_pelvis: z.boolean().optional().describe("Applied offset rotates with the pelvis (generate_from_target_pelvis)."),
    maintain_offset_from_pelvis: z.boolean().optional().describe("Preserve retarget-pose root↔pelvis offset."),
    propagate_to_non_retargeted_children: z.boolean().optional().describe("Propagate root transform to non-retargeted children."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = { retargeter_path: a.retargeter_path };
    if (a.root_motion_source !== undefined) p.root_motion_source = a.root_motion_source;
    if (a.root_height_source !== undefined) p.root_height_source = a.root_height_source;
    if (a.rotate_with_pelvis !== undefined) p.rotate_with_pelvis = a.rotate_with_pelvis;
    if (a.maintain_offset_from_pelvis !== undefined) p.maintain_offset_from_pelvis = a.maintain_offset_from_pelvis;
    if (a.propagate_to_non_retargeted_children !== undefined)
      p.propagate_to_non_retargeted_children = a.propagate_to_non_retargeted_children;
    return p;
  },
});

// ── retarget pose import ────────────────────────────────────────────────────

const ikRetargetImportPoseFromAnimation = bridgeTool({
  name: "ik_retarget_import_pose_from_animation",
  domain: "ik_retarget",
  description:
    'Sample a frame of a UAnimSequence and store it as a retarget pose ("Import ' +
    'Retarget Pose from Sequence"). Setting the source pose to an idle frame makes ' +
    "its delta ≈ 0 so the target outputs its bind pose at idle. dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of the UIKRetargeter to mutate."),
    anim_sequence_path: z.string().min(1).describe("Asset path of the UAnimSequence to sample."),
    source_or_target: z.string().default("source").describe('"source" (default) or "target".'),
    frame_index: z.number().int().default(0).describe("Sampled-key index to evaluate (default 0); clamped to range."),
    make_current: z.boolean().default(true).describe("Make the imported pose the active retarget pose (default true)."),
    exclude_bone_substrings: z
      .array(z.string())
      .optional()
      .describe("Case-insensitive substrings; matching bones are skipped (no delta). Pelvis never excluded."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      retargeter_path: a.retargeter_path,
      anim_sequence_path: a.anim_sequence_path,
      source_or_target: a.source_or_target,
      frame_index: a.frame_index,
      make_current: a.make_current,
    };
    if (a.exclude_bone_substrings !== undefined) p.exclude_bone_substrings = a.exclude_bone_substrings;
    return p;
  },
});

const ikRetargetImportPoseFromPoseAsset = bridgeTool({
  name: "ik_retarget_import_pose_from_pose_asset",
  domain: "ik_retarget",
  description:
    'Import a UPoseAsset\'s named pose as a retarget pose ("Import retarget pose ' +
    'from PoseAsset"). Use for vendor PA_*RetargetPose assets on novel source ' +
    "skeletons (SMPL, mocap). Empty pose_name = first pose. dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of the UIKRetargeter to mutate."),
    pose_asset_path: z.string().min(1).describe("Asset path of the UPoseAsset to import from."),
    source_or_target: z.string().default("source").describe('"source" (default) or "target".'),
    pose_name: z.string().default("").describe("Name of the pose inside the pose asset. Empty = first pose."),
    make_current: z.boolean().default(true).describe("Make the imported pose the active retarget pose (default true)."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      retargeter_path: a.retargeter_path,
      pose_asset_path: a.pose_asset_path,
      source_or_target: a.source_or_target,
      make_current: a.make_current,
    };
    if (a.pose_name) p.pose_name = a.pose_name;
    return p;
  },
});

// ── batch ───────────────────────────────────────────────────────────────────

const ikRetargetRunBatch = bridgeTool({
  name: "ik_retarget_run_batch",
  domain: "ik_retarget",
  description:
    "Cross-skeleton anim retargeting — duplicate-and-retarget a list of animation " +
    "assets through the retargeter (UIKRetargetBatchOperation::DuplicateAndRetarget). " +
    "Source/target meshes read from the rigs. Unresolvable paths fail upfront " +
    "(asset_not_found). dry_run unsupported.",
  input: z.object({
    retargeter_path: z.string().min(1).describe("Asset path of a UIKRetargeter."),
    source_animations: z
      .array(z.string())
      .describe("Asset paths — UAnimSequence, UBlendSpace, or UAnimMontage. Each must resolve."),
    name_search: z.string().default("").describe("Search string applied to new file names."),
    name_replace: z.string().default("").describe("Replacement for name_search in new file names."),
    name_prefix: z.string().default("").describe("Prepended to new file names."),
    name_suffix: z.string().default("").describe("Appended to new file names."),
    include_referenced_assets: z
      .boolean()
      .default(true)
      .describe("Retarget assets referenced by the inputs too (default true)."),
    overwrite_existing: z
      .boolean()
      .default(false)
      .describe("Overwrite existing files at new paths (default false → unique numeric suffix)."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      retargeter_path: a.retargeter_path,
      source_animations: a.source_animations,
      include_referenced_assets: a.include_referenced_assets,
      overwrite_existing: a.overwrite_existing,
    };
    if (a.name_search) p.name_search = a.name_search;
    if (a.name_replace) p.name_replace = a.name_replace;
    if (a.name_prefix) p.name_prefix = a.name_prefix;
    if (a.name_suffix) p.name_suffix = a.name_suffix;
    return p;
  },
});

export const ik_retargetTools: ToolDef[] = [
  ikRetargetRead,
  ikRetargetCreate,
  ikRetargetSetRigs,
  ikRetargetAutoMapChains,
  ikRetargetSetChainMapping,
  ikRetargetAlignBones,
  ikRetargetSetPelvisSettings,
  ikRetargetSetRootMotionSettings,
  ikRetargetImportPoseFromAnimation,
  ikRetargetImportPoseFromPoseAsset,
  ikRetargetRunBatch,
];
