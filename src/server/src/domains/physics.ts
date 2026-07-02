/**
 * Domain: physics — physics-body, constraint, and component physics authoring.
 *
 * Port of the `physics_*` tools in `src/MCP/server.py`. physics_set_properties
 * mutates a Blueprint component's body settings (routed through the Blueprint
 * command handler); physics_set_body_collision / physics_set_constraint_motion
 * edit a UPhysicsAsset's bodies/constraints (routed through the skeletal-mesh
 * handler); physics_material_create makes a UPhysicalMaterial asset (routed
 * through the asset-factory handler). Wire command == tool name in every case.
 * All are refused while a PIE session is active. None support dry_run.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const COMBINE_MODES = ["Average", "Min", "Multiply", "Max"] as const;

const physicsMaterialCreate = bridgeTool({
  name: "physics_material_create",
  domain: "physics",
  description:
    "Create a UPhysicalMaterial asset (friction / restitution / density). Passing a combine mode " +
    "also sets that mode's per-material override flag — without it the Project Settings physics " +
    "default applies and the material's own mode is ignored. Bouncy surfaces: restitution near 1 " +
    "plus restitution_combine_mode 'Max'. Auto-saves to disk; refuses to overwrite an existing " +
    "asset (name_collision). Assign to a body via bp_set_component_property " +
    "'BodyInstance.PhysMaterialOverride'.",
  input: z.object({
    asset_path: z
      .string()
      .min(1)
      .describe('Destination asset path, e.g. "/Game/Physics/PM_Bouncy". Folders auto-created.'),
    friction: z.number().optional().describe("Kinetic friction coefficient (engine default 0.7)."),
    static_friction: z.number().optional().describe("Static friction coefficient (engine default 0)."),
    restitution: z
      .number()
      .min(0)
      .max(1)
      .optional()
      .describe("Bounciness 0..1 (engine default 0.3)."),
    density: z.number().optional().describe("Density in g/cm³ (engine default 1.0)."),
    friction_combine_mode: z
      .enum(COMBINE_MODES)
      .optional()
      .describe("How friction combines across the contact pair; setting it enables the per-material override."),
    restitution_combine_mode: z
      .enum(COMBINE_MODES)
      .optional()
      .describe("How restitution combines across the contact pair; setting it enables the per-material override."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  // Optionals omitted when unset so the C++ handler keeps factory defaults.
  params: (a) => {
    const p: Record<string, unknown> = { asset_path: a.asset_path };
    if (a.friction !== undefined) p.friction = a.friction;
    if (a.static_friction !== undefined) p.static_friction = a.static_friction;
    if (a.restitution !== undefined) p.restitution = a.restitution;
    if (a.density !== undefined) p.density = a.density;
    if (a.friction_combine_mode !== undefined) p.friction_combine_mode = a.friction_combine_mode;
    if (a.restitution_combine_mode !== undefined) p.restitution_combine_mode = a.restitution_combine_mode;
    return p;
  },
});

const physicsSetProperties = bridgeTool({
  name: "physics_set_properties",
  domain: "physics",
  description:
    "Set physics properties on a Blueprint component (simulate, gravity, mass, damping). " +
    "Requires blueprint_name + component_name.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Blueprint asset name/path owning the component."),
    component_name: z.string().min(1).describe("Component on the Blueprint to configure."),
    simulate_physics: z.boolean().default(true).describe("Enable rigid-body simulation on the component."),
    gravity_enabled: z.boolean().default(true).describe("Apply gravity while simulating."),
    mass: z.number().default(1).describe("Body mass (kg)."),
    linear_damping: z.number().default(0.01).describe("Linear velocity damping."),
    angular_damping: z.number().default(0).describe("Angular velocity damping."),
  }),
  annotations: { blockedDuringPie: true },
});

const physicsSetBodyCollision = bridgeTool({
  name: "physics_set_body_collision",
  domain: "physics",
  description:
    "Set CollisionEnabled on bodies of a UPhysicsAsset. Omit/empty body_names to apply to all " +
    "bodies. Persists to disk by default. Common use: enable query collision on per-bone capsules " +
    "so SweepComponent / LineTraceComponent against the skeletal mesh registers hits.",
  input: z.object({
    path: z
      .string()
      .min(1)
      .describe('UPhysicsAsset path, e.g. "/Game/.../HorsePhysAsset.HorsePhysAsset".'),
    collision_enabled: z
      .string()
      .default("QueryAndPhysics")
      .describe(
        'One of "NoCollision", "QueryOnly", "PhysicsOnly", "QueryAndPhysics" (default), "ProbeOnly", "QueryAndProbe".',
      ),
    body_names: z
      .array(z.string())
      .optional()
      .describe('Optional bone names (e.g. ["Head_M", "Neck1_M"]) to target. Omit for all bodies.'),
    save: z.boolean().default(true).describe("True (default) to save the asset after mutation."),
  }),
  annotations: { blockedDuringPie: true },
  // body_names omitted when empty/unset (matches Python `if body_names`).
  params: (a) => {
    const p: Record<string, unknown> = {
      path: a.path,
      collision_enabled: a.collision_enabled,
      save: a.save,
    };
    if (a.body_names && a.body_names.length > 0) p.body_names = a.body_names;
    return p;
  },
});

const physicsSetConstraintMotion = bridgeTool({
  name: "physics_set_constraint_motion",
  domain: "physics",
  description:
    "Edit or delete a constraint in a UPhysicsAsset's ConstraintSetup. Key it by joint_name OR by " +
    "the (bone1, bone2) pair (order-insensitive; preferred). Either delete=true to remove it, or " +
    "relax/lock per-axis degrees of freedom (swing1/swing2/twist, linear_x/y/z). Use anim_physics_inspect first.",
  input: z.object({
    path: z
      .string()
      .min(1)
      .describe('UPhysicsAsset path, e.g. "/Game/.../PHYS_Mannequin.PHYS_Mannequin".'),
    joint_name: z.string().optional().describe("Constraint joint name (alternative to bone1+bone2)."),
    bone1: z.string().optional().describe("First constrained bone (order-insensitive with bone2)."),
    bone2: z.string().optional().describe("Second constrained bone (order-insensitive with bone1)."),
    delete: z.boolean().default(false).describe("True to remove the constraint entirely. Ignores the motion args."),
    swing1: z.string().optional().describe('Angular swing1 motion — "Free", "Limited", or "Locked". Omit to leave unchanged.'),
    swing2: z.string().optional().describe('Angular swing2 motion — "Free", "Limited", or "Locked". Omit to leave unchanged.'),
    twist: z.string().optional().describe('Angular twist motion — "Free", "Limited", or "Locked". Omit to leave unchanged.'),
    linear_x: z.string().optional().describe('Linear X motion — "Free", "Limited", or "Locked". Omit to leave unchanged.'),
    linear_y: z.string().optional().describe('Linear Y motion — "Free", "Limited", or "Locked". Omit to leave unchanged.'),
    linear_z: z.string().optional().describe('Linear Z motion — "Free", "Limited", or "Locked". Omit to leave unchanged.'),
    save: z.boolean().default(true).describe("True (default) to save the asset after mutation."),
  }),
  annotations: { blockedDuringPie: true },
  // Optional keys omitted when None (matches Python's `if val is not None`).
  params: (a) => {
    const p: Record<string, unknown> = { path: a.path, delete: a.delete, save: a.save };
    if (a.joint_name !== undefined) p.joint_name = a.joint_name;
    if (a.bone1 !== undefined) p.bone1 = a.bone1;
    if (a.bone2 !== undefined) p.bone2 = a.bone2;
    if (a.swing1 !== undefined) p.swing1 = a.swing1;
    if (a.swing2 !== undefined) p.swing2 = a.swing2;
    if (a.twist !== undefined) p.twist = a.twist;
    if (a.linear_x !== undefined) p.linear_x = a.linear_x;
    if (a.linear_y !== undefined) p.linear_y = a.linear_y;
    if (a.linear_z !== undefined) p.linear_z = a.linear_z;
    return p;
  },
});

export const physicsTools: ToolDef[] = [
  physicsMaterialCreate,
  physicsSetProperties,
  physicsSetBodyCollision,
  physicsSetConstraintMotion,
];
