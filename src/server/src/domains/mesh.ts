/**
 * Domain: mesh — static/skeletal mesh asset properties, materials, collision,
 * physics-asset binding, and skeletal bend-chain authoring.
 *
 * Port of the `mesh_*` tools in `src/MCP/server.py`.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool, defineTool } from "./_shared.ts";
import { dryRun } from "./_schemas.ts";

const meshSetStaticMeshProperties = bridgeTool({
  name: "mesh_set_static_mesh_properties",
  domain: "mesh",
  description:
    "Set the static mesh on a StaticMeshComponent of a Blueprint. " +
    "static_mesh defaults to the engine Cube. Blocked during PIE.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Target Blueprint name/path."),
    component_name: z.string().min(1).describe("StaticMeshComponent name."),
    static_mesh: z
      .string()
      .default("/Engine/BasicShapes/Cube.Cube")
      .describe("UStaticMesh path to assign."),
  }),
  annotations: { idempotentHint: true, blockedDuringPie: true },
});

const meshSetStaticMeshMaterial = bridgeTool({
  name: "mesh_set_static_mesh_material",
  domain: "mesh",
  description:
    "Set the material at a slot index on a UStaticMesh asset (persistent — mutates " +
    "StaticMaterials[slot].MaterialInterface). Use after duplicate_asset to retarget " +
    "a cloned mesh's slots. For one-off component overrides prefer " +
    "mesh_set_static_mesh_properties. Blocked during PIE.",
  input: z.object({
    mesh_path: z
      .string()
      .min(1)
      .describe("Full package path to the UStaticMesh asset."),
    material_path: z
      .string()
      .min(1)
      .describe("Full package path to the UMaterialInterface to assign."),
    slot_index: z.number().int().default(0).describe("Material slot to overwrite (default 0)."),
  }),
  annotations: { idempotentHint: true, blockedDuringPie: true },
});

const meshGetActorMaterialInfo = bridgeTool({
  name: "mesh_get_actor_material_info",
  domain: "mesh",
  description: "Get information about the materials currently applied to an actor.",
  input: z.object({
    actor_name: z.string().min(1).describe("Name of the actor to inspect."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const meshSetMeshMaterialColor = defineTool({
  name: "mesh_set_mesh_material_color",
  domain: "mesh",
  description:
    "DISABLED (GAP-009): this only ever targets a Blueprint SCS component *template*, and " +
    "baking a runtime (dynamic) material instance into a saved template corrupts level saves " +
    "(\"Illegal reference to private object\"). It now returns a structured error " +
    "(error_code 'feature_disabled') directing you to the saved-asset path instead: " +
    "material_create_instance -> material_instance_set_parameter (vector 'BaseColor'/'Color') " +
    "-> material_apply_to_blueprint. Blocked during PIE.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Target Blueprint name/path."),
    component_name: z.string().min(1).describe("Mesh component name."),
    color: z
      .array(z.number())
      .describe("[R, G, B, A] floats (clamped to 0..1)."),
    material_path: z
      .string()
      .default("/Engine/BasicShapes/BasicShapeMaterial")
      .describe("Material to instance for the color override."),
    parameter_name: z
      .string()
      .default("BaseColor")
      .describe("Legacy arg — ignored; both BaseColor and Color are always set."),
    material_slot: z.number().int().default(0).describe("Material slot to color (default 0)."),
  }),
  annotations: { idempotentHint: true, blockedDuringPie: true },
  handler: async (a, ctx) => {
    // Validate color format (mirrors the Python guard).
    if (!Array.isArray(a.color) || a.color.length !== 4) {
      return {
        success: false,
        message: "Invalid color format. Must be a list of 4 float values [R, G, B, A].",
      };
    }
    // Clamp all color values to [0, 1].
    const color = a.color.map((v) => Math.min(1.0, Math.max(0.0, v)));

    // Set BaseColor parameter first. The bridge handler targets a Blueprint component
    // TEMPLATE and now refuses the dynamic-instance path (GAP-009) — surface its structured
    // error envelope (error_code 'feature_disabled' + the saved-asset redirect hint) directly
    // and short-circuit rather than firing a second doomed call.
    const responseBase = await ctx.conn.sendCommand("mesh_set_mesh_material_color", {
      blueprint_name: a.blueprint_name,
      component_name: a.component_name,
      color,
      material_path: a.material_path,
      parameter_name: "BaseColor",
      material_slot: a.material_slot,
    });
    if (responseBase.status !== "success") {
      return responseBase;
    }

    // Set Color parameter second (for maximum compatibility).
    const responseColor = await ctx.conn.sendCommand("mesh_set_mesh_material_color", {
      blueprint_name: a.blueprint_name,
      component_name: a.component_name,
      color,
      material_path: a.material_path,
      parameter_name: "Color",
      material_slot: a.material_slot,
    });

    // Return success if either parameter setting worked.
    if (responseBase.status === "success" || responseColor.status === "success") {
      return {
        success: true,
        message: `Color applied successfully to slot ${a.material_slot}: ${JSON.stringify(color)}`,
        base_color_result: responseBase,
        color_result: responseColor,
        material_slot: a.material_slot,
      };
    }
    return responseColor;
  },
});

const meshSetCollision = bridgeTool({
  name: "mesh_set_collision",
  domain: "mesh",
  description:
    "Author simple/convex collision onto an EXISTING UStaticMesh asset (headless equivalent " +
    "of the Static Mesh Editor's Collision menu). shape: box|sphere|capsule|kdop10_x/y/z|" +
    "kdop18|kdop26|convex|none. Engine content (/Engine/...) is refused. Blocked during PIE.",
  input: z.object({
    asset_path: z
      .string()
      .min(1)
      .describe("Object/package path to the static mesh. Engine content refused."),
    shape: z
      .string()
      .describe(
        'Collision shape: "box"|"sphere"|"capsule"|"kdop10_x"|"kdop10_y"|"kdop10_z"|' +
          '"kdop18"|"kdop26"|"convex"|"none".',
      ),
    replace_existing: z
      .boolean()
      .default(true)
      .describe("Remove existing collision first so the new shape replaces rather than stacks."),
    hull_count: z.number().int().default(4).describe("convex only — max number of convex pieces."),
    max_hull_verts: z
      .number()
      .int()
      .default(16)
      .describe("convex only — max vertices per hull."),
    hull_precision: z
      .number()
      .int()
      .default(100000)
      .describe("convex only — voxel resolution."),
    collision_trace_flag: z
      .string()
      .default("")
      .describe(
        'Optional body-setup trace flag: "default"|"simple_and_complex"|' +
          '"simple_as_complex"|"complex_as_simple". Empty = leave unchanged.',
      ),
    save: z.boolean().default(true).describe("Persist the package after the change."),
  }),
  annotations: { idempotentHint: true, blockedDuringPie: true },
  // collision_trace_flag is omitted from the wire params when empty (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      shape: a.shape,
      replace_existing: a.replace_existing,
      hull_count: a.hull_count,
      max_hull_verts: a.max_hull_verts,
      hull_precision: a.hull_precision,
      save: a.save,
    };
    if (a.collision_trace_flag) p.collision_trace_flag = a.collision_trace_flag;
    return p;
  },
});

const meshGetCollision = bridgeTool({
  name: "mesh_get_collision",
  domain: "mesh",
  description:
    "Read an existing UStaticMesh's collision setup (non-destructive) — the investigate " +
    "step before mesh_set_collision. Returns has_body_setup, collision_complexity, and " +
    "simple/convex/box/sphere/capsule counts.",
  input: z.object({
    asset_path: z.string().min(1).describe("Object/package path to the static mesh."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const meshGetBounds = bridgeTool({
  name: "mesh_get_bounds",
  domain: "mesh",
  description:
    "Read a UStaticMesh asset's LOCAL-space bounds (read-only) — the asset-space " +
    "counterpart to actor_inspect's per-component world_bounds. Returns local_bounds " +
    "{origin, box_extent, sphere_radius} (extended bounds, from UStaticMesh::GetBounds), " +
    "box_min/box_max (the local bounding box), size (full box dimensions = box_extent*2), " +
    "and positive/negative_bounds_extension. Engine content (/Engine/...) is allowed.",
  input: z.object({
    static_mesh_path: z
      .string()
      .min(1)
      .describe("Object/package path to the UStaticMesh asset (e.g. /Game/Meshes/SM_Bike or /Engine/BasicShapes/Cube)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const meshBuildBendChain = bridgeTool({
  name: "mesh_build_bend_chain",
  domain: "mesh",
  description:
    "(Re)build a Root->tip bone CHAIN up one axis of a skeletal mesh and procedurally re-skin " +
    "every vertex to it by position, with smooth two-bone joint blends, so the mesh can bend. " +
    "Segment lengths decrease toward the tip (stiff butt, whippier tip). Idempotent; saves the " +
    "mesh + bound skeleton. Supports dry_run (returns the bone-station table without mutating). " +
    "Blocked during PIE.",
  input: z.object({
    path: z
      .string()
      .min(1)
      .describe('SkeletalMesh content-browser path, e.g. "/Game/Characters/SKM_Hero".'),
    num_bones: z
      .number()
      .int()
      .default(15)
      .describe("Number of pole bones to add beyond Root (clamped 1..64)."),
    axis: z.string().default("z").describe('The mesh long axis — "x", "y", or "z".'),
    base_fraction: z
      .number()
      .default(0.14)
      .describe("Fraction of the length (from the butt) that stays rigid on Root — the grip."),
    segment_ratio: z
      .number()
      .default(0.85)
      .describe("Each flex segment = ratio x the previous. <1 makes segments shorter toward tip."),
    bone_prefix: z
      .string()
      .default("pole_")
      .describe('Name prefix for the pole bones (pole_01..pole_NN).'),
    root_bone_name: z.string().default("Root").describe("Name of the rigid base/root bone."),
    dry_run: dryRun.describe(
      "When true, returns the computed bone-station table WITHOUT mutating the asset.",
    ),
  }),
  annotations: { idempotentHint: true, blockedDuringPie: true },
});

const meshSetPhysicsAsset = bridgeTool({
  name: "mesh_set_physics_asset",
  domain: "mesh",
  description:
    "Repoint (or clear) a USkeletalMesh's PhysicsAsset. Pass physics_asset=\"\" to CLEAR the " +
    "binding. Returns {path, old_physics_asset, new_physics_asset, saved}. Blocked during PIE.",
  input: z.object({
    path: z.string().min(1).describe('USkeletalMesh path, e.g. "/Game/.../SK_Manny.SK_Manny".'),
    physics_asset: z
      .string()
      .describe('UPhysicsAsset path to bind, or "" (empty) to CLEAR the binding.'),
    save: z.boolean().default(true).describe("Save the mesh after mutation."),
  }),
  annotations: { idempotentHint: true, blockedDuringPie: true },
});

// ── Static-mesh sockets ─────────────────────────────────────────────────────
// Named relative transforms on a UStaticMesh — the SCALE-PROOF storage for the by-eye
// positioning workflow (muzzle / attach / grip points). GetSocketTransform composes the
// socket with a component's live world transform incl. SCALE, so the "hand-tuned offset
// vector x hold-fit scale" class of bug (a held gun's muzzle landing ~20-37 m downrange)
// cannot occur. Author the relative transform here, then drag it by eye in the Static Mesh
// Editor's Socket Manager; read it back with mesh_list_sockets.

const meshListSockets = bridgeTool({
  name: "mesh_list_sockets",
  domain: "mesh",
  description:
    "List the sockets on a UStaticMesh (read). Resolve the mesh either by ASSET path " +
    "(asset_path, or its alias static_mesh_path) OR by a level actor's StaticMeshComponent " +
    "(actor + optional component). Returns sockets[] (socket_name, index, relative " +
    "location{x,y,z}/rotation{pitch,yaw,roll}/scale{x,y,z}, tag) + count; the actor/component " +
    "path ADDITIONALLY returns each socket's resolved world transform. The read step before " +
    "mesh_add/modify/remove_socket and for reading back a by-eye Socket Manager placement. " +
    "Static-mesh sockets have no parent bone — for skeletal sockets use anim_skeleton_list_sockets.",
  input: z.object({
    asset_path: z
      .string()
      .default("")
      .describe("Object/package path to the UStaticMesh. Use this (or static_mesh_path) OR actor."),
    static_mesh_path: z
      .string()
      .default("")
      .describe("Alias for asset_path (UStaticMesh asset path). Use this OR actor."),
    actor: z
      .string()
      .default("")
      .describe("Level actor label/name owning a StaticMeshComponent; the world-transform path. Use this OR asset_path."),
    component: z
      .string()
      .default("")
      .describe("StaticMeshComponent name on the actor; omit to take the first."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.static_mesh_path) p.static_mesh_path = a.static_mesh_path;
    if (a.actor) p.actor = a.actor;
    if (a.component) p.component = a.component;
    return p;
  },
});

const meshAddSocket = bridgeTool({
  name: "mesh_add_socket",
  domain: "mesh",
  description:
    "Create a named socket on a UStaticMesh with a relative location/rotation/scale (the scale-proof " +
    "home for muzzle/attach/grip points — GetSocketTransform composes it with the component's live " +
    "world transform incl. scale). socket_name must be unique on the mesh. Engine content (/Engine/...) " +
    "is refused. Supports dry_run. Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Object/package path to the UStaticMesh. Engine content refused."),
    socket_name: z.string().min(1).describe("Name for the new socket (unique on this mesh)."),
    location_x: z.number().default(0.0).describe("Relative location X (mesh-local units)."),
    location_y: z.number().default(0.0).describe("Relative location Y."),
    location_z: z.number().default(0.0).describe("Relative location Z."),
    rotation_pitch: z.number().default(0.0).describe("Relative rotation pitch (socket forward = +X)."),
    rotation_yaw: z.number().default(0.0).describe("Relative rotation yaw."),
    rotation_roll: z.number().default(0.0).describe("Relative rotation roll."),
    scale_x: z.number().default(1.0).describe("Relative scale X."),
    scale_y: z.number().default(1.0).describe("Relative scale Y."),
    scale_z: z.number().default(1.0).describe("Relative scale Z."),
    dry_run: dryRun.describe("Preview the socket that would be added without mutating."),
  }),
  annotations: { blockedDuringPie: true },
});

const meshModifySocket = bridgeTool({
  name: "mesh_modify_socket",
  domain: "mesh",
  description:
    "Modify an existing socket on a UStaticMesh. Only the provided fields are updated (others kept). " +
    "Use to nudge a muzzle/attach point numerically, or to capture a value read elsewhere. Engine " +
    "content refused. Supports dry_run. Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Object/package path to the UStaticMesh. Engine content refused."),
    socket_name: z.string().min(1).describe("Name of the socket to modify."),
    location_x: z.number().optional().describe("New relative location X."),
    location_y: z.number().optional().describe("New relative location Y."),
    location_z: z.number().optional().describe("New relative location Z."),
    rotation_pitch: z.number().optional().describe("New relative rotation pitch."),
    rotation_yaw: z.number().optional().describe("New relative rotation yaw."),
    rotation_roll: z.number().optional().describe("New relative rotation roll."),
    scale_x: z.number().optional().describe("New relative scale X."),
    scale_y: z.number().optional().describe("New relative scale Y."),
    scale_z: z.number().optional().describe("New relative scale Z."),
    tag: z.string().optional().describe("New socket tag string."),
    dry_run: dryRun.describe("Preview the changed fields without mutating."),
  }),
  annotations: { blockedDuringPie: true },
  // Omit unset optionals so the handler updates only the provided fields.
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      socket_name: a.socket_name,
    };
    if (a.location_x !== undefined) p.location_x = a.location_x;
    if (a.location_y !== undefined) p.location_y = a.location_y;
    if (a.location_z !== undefined) p.location_z = a.location_z;
    if (a.rotation_pitch !== undefined) p.rotation_pitch = a.rotation_pitch;
    if (a.rotation_yaw !== undefined) p.rotation_yaw = a.rotation_yaw;
    if (a.rotation_roll !== undefined) p.rotation_roll = a.rotation_roll;
    if (a.scale_x !== undefined) p.scale_x = a.scale_x;
    if (a.scale_y !== undefined) p.scale_y = a.scale_y;
    if (a.scale_z !== undefined) p.scale_z = a.scale_z;
    if (a.tag !== undefined) p.tag = a.tag;
    if (a.dry_run !== undefined) p.dry_run = a.dry_run;
    return p;
  },
});

const meshRemoveSocket = bridgeTool({
  name: "mesh_remove_socket",
  domain: "mesh",
  description:
    "Remove a socket from a UStaticMesh by name. Returns removed_socket + remaining_sockets count. " +
    "Engine content refused. Supports dry_run. Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Object/package path to the UStaticMesh. Engine content refused."),
    socket_name: z.string().min(1).describe("Name of the socket to remove."),
    dry_run: dryRun.describe("Preview the removal without mutating."),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

export const meshTools: ToolDef[] = [
  meshSetStaticMeshProperties,
  meshSetStaticMeshMaterial,
  meshGetActorMaterialInfo,
  meshSetMeshMaterialColor,
  meshSetCollision,
  meshGetCollision,
  meshGetBounds,
  meshBuildBendChain,
  meshSetPhysicsAsset,
  meshListSockets,
  meshAddSocket,
  meshModifySocket,
  meshRemoveSocket,
];
