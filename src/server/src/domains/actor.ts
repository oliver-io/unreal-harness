/**
 * Domain: actor — level actor lifecycle, transforms, inspection, and queries.
 *
 * Port of the `actor_*` tools (plus the lint-exempt `find_actors_by_name`) in
 * `src/MCP/server.py`. `actor_get_in_level` + `actor_spawn` are the P0 seed
 * (kept verbatim as the reference shape); the rest land in P1.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool, defineTool } from "./_shared.ts";
import { Vec3, Rotator, dryRun } from "./_schemas.ts";

const actorGetInLevel = bridgeTool({
  name: "actor_get_in_level",
  domain: "actor",
  description: "List all actors in the current level (name, label, class, transform).",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const actorSpawn = bridgeTool({
  name: "actor_spawn",
  domain: "actor",
  description:
    "Spawn one actor at a transform. class_path: native (/Script/Engine.PointLight), " +
    "asset (/Game/.../BP_Foo — _C appended), or short name. No auto-possess. " +
    "Supports dry_run.",
  input: z.object({
    class_path: z.string().min(1).describe("UClass path — required."),
    location: Vec3.optional().describe('{"x","y","z"} — default origin.'),
    rotation: Rotator.optional().describe('{"pitch","yaw","roll"} — default zero.'),
    scale: Vec3.optional().describe('{"x","y","z"} — default (1,1,1).'),
    name: z.string().optional().describe("Optional FName + label."),
    tags: z.array(z.string()).optional().describe("Optional tag strings."),
    folder_path: z.string().optional().describe("Optional editor folder."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: false },
  // Wire param key is `class`, and unset optionals are omitted (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = { class: a.class_path, dry_run: a.dry_run };
    if (a.location !== undefined) p.location = a.location;
    if (a.rotation !== undefined) p.rotation = a.rotation;
    if (a.scale !== undefined) p.scale = a.scale;
    if (a.name) p.name = a.name;
    if (a.tags !== undefined) p.tags = a.tags;
    if (a.folder_path) p.folder_path = a.folder_path;
    return p;
  },
});

const actorDelete = bridgeTool({
  name: "actor_delete",
  domain: "actor",
  description:
    "Delete an actor by its FName (not the editor label). Supports dry_run " +
    "(diff: {actors_removed: [<actor brief>]}).",
  input: z.object({
    name: z.string().min(1).describe("Actor FName (not the editor label)."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true },
  // 1:1 wire params {name, dry_run}; the Python name-tracking side-effect has no
  // equivalent in the registry port.
});

const actorSetTransform = bridgeTool({
  name: "actor_set_transform",
  domain: "actor",
  description:
    "Set an actor's transform by FName. Only the components you pass (location/" +
    "rotation/scale) are mutated; omitted ones keep their current values. " +
    "Supports dry_run (diff: {transforms_changed: [{name, before, after}]}).",
  input: z.object({
    name: z.string().min(1).describe("Actor FName. Required."),
    location: z
      .array(z.number())
      .optional()
      .describe("[x, y, z] world location — optional override."),
    rotation: z
      .array(z.number())
      .optional()
      .describe("[pitch, yaw, roll] rotation — optional override."),
    scale: z.array(z.number()).optional().describe("[x, y, z] scale — optional override."),
    dry_run: dryRun,
  }),
  // Unset optionals are omitted (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = { name: a.name, dry_run: a.dry_run };
    if (a.location !== undefined) p.location = a.location;
    if (a.rotation !== undefined) p.rotation = a.rotation;
    if (a.scale !== undefined) p.scale = a.scale;
    return p;
  },
});

const actorInspect = bridgeTool({
  name: "actor_inspect",
  domain: "actor",
  description:
    "Dense inspection of one actor by FName — components, transform, tags, mobility, " +
    "plus key_properties curated per class family (StaticMesh/SkeletalMesh/Pawn/" +
    "Character/Camera/Light). Each scene component also reports relative_transform " +
    "and world_transform (engine-resolved ComponentToWorld; absolute flags handled), " +
    "and primitive components add world_bounds (origin + box_extent + sphere_radius). " +
    "Read-only.",
  input: z.object({
    name: z.string().min(1).describe("Actor FName (case-sensitive, not the label)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const actorQuery = bridgeTool({
  name: "actor_query",
  domain: "actor",
  description:
    "Query editor-world actors with composable AND-filters (name/label substring, " +
    "class, tags, folder, spatial AABB, distance-from origin). Read-only; never " +
    "force-loads sublevels (they appear in skipped_sublevels). Paginated.",
  input: z.object({
    name_pattern: z
      .string()
      .default("")
      .describe("Case-insensitive substring vs actor name OR label. Empty disables."),
    class_filter: z
      .string()
      .optional()
      .describe(
        "UClass path: native (/Script/Engine.PointLight), asset (/Game/... — _C " +
          "appended), or short name. Omit to disable.",
      ),
    direct_only: z
      .boolean()
      .default(false)
      .describe("With class_filter, exact class match. Default false (IsA subclass-recursive)."),
    tag: z
      .union([z.string(), z.array(z.string())])
      .optional()
      .describe("Single tag or list of tags; multi-tag matches ALL (AND)."),
    label: z
      .string()
      .default("")
      .describe("Editor folder path filter (exact, case-insensitive)."),
    bbox_min: Vec3.optional().describe('Spatial AABB min {"x","y","z"} — pass with bbox_max.'),
    bbox_max: Vec3.optional().describe('Spatial AABB max {"x","y","z"} — pass with bbox_min.'),
    distance_origin: Vec3.optional().describe('Radius origin {"x","y","z"}.'),
    distance_radius: z
      .number()
      .default(0)
      .describe("Radius (cm); filter active when > 0."),
    cursor: z.number().int().default(0).describe("Pagination offset."),
    limit: z.number().int().default(200).describe("Page size; max 1000."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // Renames class_filter→class; nests bbox/distance_from; omits unset filters.
  params: (a) => {
    const p: Record<string, unknown> = {
      name_pattern: a.name_pattern,
      direct_only: a.direct_only,
      label: a.label,
      cursor: a.cursor,
      limit: a.limit,
    };
    if (a.class_filter !== undefined) p.class = a.class_filter;
    if (a.tag !== undefined) p.tag = a.tag;
    if (a.bbox_min !== undefined && a.bbox_max !== undefined) {
      p.bbox = { min: a.bbox_min, max: a.bbox_max };
    }
    if (a.distance_origin !== undefined && a.distance_radius > 0) {
      p.distance_from = { origin: a.distance_origin, radius: a.distance_radius };
    }
    return p;
  },
});

const actorSetProperty = bridgeTool({
  name: "actor_set_property",
  domain: "actor",
  description:
    "Write any edit-exposed UPROPERTY on a placed level actor by reflection (dotted " +
    "path, e.g. 'DirectionalLightComponent.Intensity' or 'Settings.ColorSaturation'). " +
    "Visible live; NOT saved to the .umap unless save=true. Supports dry_run.",
  input: z.object({
    name: z.string().min(1).describe("Actor FName (GetName(), not the editor label)."),
    property: z
      .string()
      .min(1)
      .describe("Dotted reflection path to the leaf; first segment must be edit-exposed."),
    value: z
      .unknown()
      .describe("JSON value for the resolved leaf (bool/number/enum/struct/object-ref/array)."),
    save: z
      .boolean()
      .default(false)
      .describe("Save the actor's package (.umap) after the write. Default false."),
    dry_run: dryRun,
  }),
  // 1:1 wire params {name, property, value, save, dry_run}.
});

const findActorsByName = bridgeTool({
  name: "find_actors_by_name",
  domain: "actor",
  description: "Find actors by name pattern. Returns matching actors.",
  input: z.object({
    pattern: z.string().describe("Name pattern to match actors against."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

/**
 * Composite: builds a throwaway Blueprint (mesh + physics + optional color),
 * compiles it, then spawns it. Mirrors the Python `actor_spawn_physics` fan-out
 * over its constituent wire commands; returns the final spawn envelope. The
 * constituent commands are all PIE-blocked, so this is too.
 */
const actorSpawnPhysics = defineTool({
  name: "actor_spawn_physics",
  domain: "actor",
  description:
    "Quickly spawn one physics actor: builds a throwaway Blueprint (StaticMesh + " +
    "physics + optional color), compiles, then spawns it. color is [R,G,B] or " +
    "[R,G,B,A] in 0..1 ([R,G,B] gets alpha 1.0). Returns the spawn envelope.",
  input: z.object({
    name: z.string().min(1).describe("Base actor name; the helper Blueprint is <name>_BP."),
    mesh_path: z
      .string()
      .default("/Engine/BasicShapes/Cube.Cube")
      .describe("Static mesh asset path."),
    location: z
      .array(z.number())
      .default([0, 0, 0])
      .describe("[x, y, z] spawn location."),
    mass: z.number().default(1.0).describe("Body mass."),
    simulate_physics: z.boolean().default(true).describe("Enable physics simulation."),
    gravity_enabled: z.boolean().default(true).describe("Enable gravity."),
    color: z
      .array(z.number())
      .optional()
      .describe("Optional [R,G,B] or [R,G,B,A] in 0..1; [R,G,B] gets alpha 1.0."),
    scale: z.array(z.number()).default([1, 1, 1]).describe("[x, y, z] mesh component scale."),
  }),
  annotations: { blockedDuringPie: true },
  handler: async (a, ctx) => {
    const bpName = `${a.name}_BP`;

    await ctx.conn.sendCommand("bp_create_blueprint", {
      name: bpName,
      parent_class: "Actor",
    });
    await ctx.conn.sendCommand("bp_add_component", {
      blueprint_name: bpName,
      component_type: "StaticMeshComponent",
      component_name: "Mesh",
      location: [],
      rotation: [],
      scale: a.scale,
      component_properties: {},
    });
    await ctx.conn.sendCommand("mesh_set_static_mesh_properties", {
      blueprint_name: bpName,
      component_name: "Mesh",
      static_mesh: a.mesh_path,
    });
    await ctx.conn.sendCommand("physics_set_properties", {
      blueprint_name: bpName,
      component_name: "Mesh",
      simulate_physics: a.simulate_physics,
      gravity_enabled: a.gravity_enabled,
      mass: a.mass,
      linear_damping: 0.01,
      angular_damping: 0,
    });

    // Optional color: normalize to RGBA, clamp to 0..1, set BaseColor then Color
    // (mirrors mesh_set_mesh_material_color's two-parameter pass).
    let color = a.color;
    if (color !== undefined) {
      if (color.length === 3) color = [...color, 1.0];
      else if (color.length !== 4) color = undefined;
    }
    if (color !== undefined) {
      const clamped = color.map((v) => Math.min(1.0, Math.max(0.0, v)));
      for (const parameter_name of ["BaseColor", "Color"]) {
        const colorResp = await ctx.conn.sendCommand("mesh_set_mesh_material_color", {
          blueprint_name: bpName,
          component_name: "Mesh",
          color: clamped,
          material_path: "/Engine/BasicShapes/BasicShapeMaterial",
          parameter_name,
          material_slot: 0,
        });
        // mesh_set_mesh_material_color refuses to bake a dynamic material instance into a
        // BP template (it would persist an unsaveable MID — see GAP-009/GAP-009b). Surface
        // that refusal instead of silently spawning an uncolored actor under a false success:
        // the throwaway BP is left uncompiled/unspawned and the caller learns to colour via
        // the saved-asset path (material_create_instance → material_instance_set_parameter →
        // material_apply_to_blueprint) named in the refusal's hint.
        if (colorResp.status !== "success") {
          return colorResp;
        }
      }
    }

    await ctx.conn.sendCommand("bp_compile", { blueprint_name: bpName });

    return ctx.conn.sendCommand("spawn_blueprint_actor", {
      blueprint_name: bpName,
      actor_name: a.name,
      location: a.location,
      rotation: [0, 0, 0],
    });
  },
});

export const actorTools: ToolDef[] = [
  actorGetInLevel,
  actorSpawn,
  actorDelete,
  actorSetTransform,
  actorInspect,
  actorQuery,
  actorSetProperty,
  findActorsByName,
  actorSpawnPhysics,
];
