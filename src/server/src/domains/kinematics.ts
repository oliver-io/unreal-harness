/**
 * Domain: kinematics — editor-world skeletal-mesh transform probes and IK solves.
 *
 * Port of the `kinematics_*` tools in `src/MCP/server.py`. All three operate on
 * the EDITOR world (not PIE) and reuse the game's BoneIK math, so a verified
 * rotation matches what the shipped IK reproduces. None mutate assets, so none
 * are PIE-blocked or dry-run-blocked. See docs/mcp/POSITION_PROBE_TOOLS.md.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const kinematicsReadTransform = bridgeTool({
  name: "kinematics_read_transform",
  domain: "kinematics",
  description:
    "Read world + component-relative transforms of bones/sockets on a skeletal mesh in " +
    "the EDITOR world (not PIE). Returns world_type, top-level component_type " +
    "('skeletal'|'static'|'scene'), component_to_world, and transforms[] (world/relative/" +
    "forward_world + component_type per query). For a skeletal actor it also returns " +
    "pose_valid + the skeletal mesh path; for a STATIC-ROOT actor (no skeletal mesh) it falls " +
    "back to the actor's StaticMeshComponent (or scene root) — reporting component_type='static'" +
    "/'scene', the component name, component_to_world, and world_bounds (origin/box_extent/" +
    "sphere_radius) instead of rejecting. A query's \"component\" may also name a " +
    "StaticMeshComponent — its sockets resolve directly (component_type='static', no " +
    "parent_bone). An explicit component that matches no skeletal or static mesh component " +
    "returns exists:false (no body-mesh fallback), so a same-named body bone can no longer " +
    "masquerade as that component's socket.",
  input: z.object({
    actor: z.string().min(1).describe("Editor-world actor label / short-name / FName."),
    queries: z
      .array(z.record(z.string(), z.unknown()))
      .describe(
        'List of { "socket": name } or { "bone": name }, each optionally with ' +
          '"component" (omit/"body" for the actor\'s main skeletal mesh, a SkeletalMeshComponent ' +
          "or StaticMeshComponent name, or an attached actor's name to reach a weapon mesh's sockets).",
      ),
    mesh: z
      .string()
      .default("")
      .describe('Default mesh selector for queries that omit "component".'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // Omit `mesh` when empty (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = { actor: a.actor, queries: a.queries };
    if (a.mesh) p.mesh = a.mesh;
    return p;
  },
});

const kinematicsProbe = bridgeTool({
  name: "kinematics_probe",
  domain: "kinematics",
  description:
    "Forward kinematic probe: apply candidate bone rotation(s) and report the END-EFFECTOR " +
    "world-space delta (ΔP + ΔQ), scored against an intended world direction. EDITOR world " +
    "(not PIE). mode='dryrun' (default, non-mutating) or 'live' (atomic apply-and-restore).",
  input: z.object({
    actor: z.string().min(1).describe("Editor-world actor name."),
    rotations: z
      .array(z.record(z.string(), z.unknown()))
      .describe(
        'Ordered list of { "bone": name, "rotation": {axis:{x,y,z},angle_deg} OR ' +
          '{pitch,yaw,roll}, "space": "component"|"world"|"bone_local" }.',
      ),
    probe_points: z
      .array(z.record(z.string(), z.unknown()))
      .describe(
        'List of { "component"?, "socket"|"bone" } — the tips to measure (e.g. a ' +
          "sword blade-tip socket). The end-effector world delta is the only truth.",
      ),
    mesh: z
      .string()
      .default("")
      .describe("Mesh selector for the rotated bones (default the actor's body mesh)."),
    intent_direction: z
      .record(z.string(), z.number())
      .optional()
      .describe(
        'Optional world {x,y,z} you want the tip to move/point along (e.g. ' +
          '{"x":0,"y":0,"z":1} = straight up); enables scoring.',
      ),
    forward_axis_local: z
      .record(z.string(), z.number())
      .optional()
      .describe(
        'The probe point\'s local "forward" axis (default +X; use {"x":0,"y":0,"z":1} ' +
          "for a blade whose tip is mesh-local +Z).",
      ),
    mode: z
      .string()
      .default("dryrun")
      .describe("'dryrun' (default, non-mutating) or 'live' (atomic apply-and-restore)."),
    screenshot: z
      .boolean()
      .default(false)
      .describe("Only meaningful in mode=live (candidate-pose capture is deferred)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // mesh/intent_direction/forward_axis_local omitted when empty/unset (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {
      actor: a.actor,
      rotations: a.rotations,
      probe_points: a.probe_points,
      mode: a.mode,
      screenshot: a.screenshot,
    };
    if (a.mesh) p.mesh = a.mesh;
    if (a.intent_direction !== undefined) p.intent_direction = a.intent_direction;
    if (a.forward_axis_local !== undefined) p.forward_axis_local = a.forward_axis_local;
    return p;
  },
});

const kinematicsSolve = bridgeTool({
  name: "kinematics_solve",
  domain: "kinematics",
  description:
    "Inverse solve: find the two-bone-IK rotation that aims a tip along a desired WORLD " +
    "direction, via the game's BoneIK::SolveTwoBoneIK. EDITOR world (not PIE). Best-effort " +
    "(two-bone IK aims a hand POSITION); verify:true reports the achieved tip direction.",
  input: z.object({
    actor: z.string().min(1).describe("Editor-world actor name."),
    chain: z
      .record(z.string(), z.string())
      .describe(
        '{ "upper": bone, "lower": bone, "hand": bone } (e.g. upperarm_r / lowerarm_r / hand_r).',
      ),
    effector: z
      .record(z.string(), z.unknown())
      .describe('{ "component"?, "socket"|"bone" } — the tip to aim.'),
    desired_direction: z
      .record(z.string(), z.number())
      .describe('World {x,y,z} the tip should point along (e.g. {"x":0,"y":0,"z":1}).'),
    mesh: z
      .string()
      .default("")
      .describe("Mesh selector for the chain bones (default body mesh)."),
    forward_axis_local: z
      .record(z.string(), z.number())
      .optional()
      .describe('The effector\'s local "forward" axis (default +X) for verify.'),
    verify: z
      .boolean()
      .default(true)
      .describe(
        "Re-run the forward probe on the solved pose and include its delta (default True).",
      ),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // mesh/forward_axis_local omitted when empty/unset (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {
      actor: a.actor,
      chain: a.chain,
      effector: a.effector,
      desired_direction: a.desired_direction,
      verify: a.verify,
    };
    if (a.mesh) p.mesh = a.mesh;
    if (a.forward_axis_local !== undefined) p.forward_axis_local = a.forward_axis_local;
    return p;
  },
});

export const kinematicsTools: ToolDef[] = [
  kinematicsReadTransform,
  kinematicsProbe,
  kinematicsSolve,
];
