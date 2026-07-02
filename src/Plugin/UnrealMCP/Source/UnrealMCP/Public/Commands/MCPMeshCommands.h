#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for static-mesh asset operations driven from MCP.
 *
 * Commands:
 *   - bake_dynamic_mesh_to_static_mesh : headless equivalent of Modeling Mode →
 *     Bake → Static Mesh — snapshots a level actor's UDynamicMeshComponent into
 *     a new UStaticMesh asset on disk (optionally replacing the source actor).
 *   - set_static_mesh_collision : author simple collision (box / sphere /
 *     capsule / K-DOP) or auto-convex decomposition onto an EXISTING UStaticMesh,
 *     optionally set its CollisionTraceFlag, and save. Headless equivalent of the
 *     Static Mesh Editor's Collision menu (wraps UStaticMeshEditorSubsystem).
 *   - get_static_mesh_collision : non-destructive read of an existing mesh's
 *     collision — simple/convex element counts, per-shape breakdown, trace flag.
 *   - mesh_get_bounds : non-destructive read of a UStaticMesh asset's LOCAL-space
 *     bounds (origin / box_extent / sphere_radius + bounding box + bounds
 *     extensions). The asset-space counterpart to actor_inspect's world bounds.
 *
 * Implementation split: this header's handlers live across MCPMeshCommands.cpp
 * (dispatch + bake + sockets), MCPMeshCommands_Collision.cpp (collision), and
 * MCPMeshCommands_Bounds.cpp (bounds). This file is the natural home for future
 * static-mesh-asset tools (recompute_lods, bake_procedural_mesh, …).
 */
class FMCPMeshCommands
{
public:
	FMCPMeshCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * Bake the current contents of a UDynamicMeshComponent into a saved UStaticMesh asset.
	 *
	 * Required params:
	 *   actor_name (string)         — actor label in the loaded level
	 *   target_asset_path (string)  — /Game/... package path for the new asset
	 *
	 * Optional params:
	 *   component_name (string)        — DMC name; auto-pick if singleton, error if ambiguous
	 *   force_overwrite (bool=false)   — replace existing asset at target path
	 *   material_paths (string array)  — per-slot material overrides; empty = source
	 *   collision_trace_flag (string)  — "default" | "simple_as_complex" (default) |
	 *                                    "complex_as_simple" | "use_complex_collision"
	 *   recompute_normals (bool=false)
	 *   recompute_tangents (bool=true)
	 *   enable_nanite (bool=false)
	 *   replace_source_actor (bool=false) — destroy the source AVialActor and spawn an
	 *                                        AStaticMeshActor at its transform
	 *
	 * Response on success:
	 *   { success, created_asset_path, triangle_count, vertex_count,
	 *     material_slot_count, lod_count, package_size_bytes,
	 *     replaced_source_actor }
	 */
	TSharedPtr<FJsonObject> HandleBakeDynamicMeshToStaticMesh(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Author simple/convex collision onto an existing UStaticMesh and (optionally)
	 * set its collision-trace flag, then save. Wraps UStaticMeshEditorSubsystem —
	 * the same calls the Static Mesh Editor's Collision menu makes — so the editor
	 * close/reopen + PostEditChange rebuild is handled engine-side.
	 *
	 * Required params:
	 *   asset_path (string)  — object/package path to the UStaticMesh (e.g.
	 *                          "/Game/Meshes/SM_Rock_1"). Engine content is refused.
	 *   shape (string)       — "box" | "sphere" | "capsule" | "kdop10_x" |
	 *                          "kdop10_y" | "kdop10_z" | "kdop18" | "kdop26" |
	 *                          "convex" | "none". "convex" runs auto-convex
	 *                          decomposition (always replaces); "none" removes all
	 *                          collision.
	 *
	 * Optional params:
	 *   replace_existing (bool=true)   — for the simple shapes, remove existing
	 *                                    collision first so the new shape replaces
	 *                                    rather than stacks. (convex always replaces.)
	 *   hull_count (int=4)             — convex only: max convex pieces.
	 *   max_hull_verts (int=16)        — convex only: max verts per hull.
	 *   hull_precision (int=100000)    — convex only: voxel precision.
	 *   collision_trace_flag (string)  — if present, set the body setup's flag:
	 *                                    "default" | "simple_and_complex" |
	 *                                    "simple_as_complex" | "complex_as_simple".
	 *   save (bool=true)               — persist the package after the change.
	 *
	 * Response on success:
	 *   { success, asset_path, shape, simple_collision_count,
	 *     convex_collision_count, collision_complexity, collision_trace_flag,
	 *     saved }
	 */
	TSharedPtr<FJsonObject> HandleSetStaticMeshCollision(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Non-destructive read of an existing UStaticMesh's collision setup.
	 *
	 * Required params:
	 *   asset_path (string)  — object/package path to the UStaticMesh.
	 *
	 * Response on success:
	 *   { success, asset_path, has_body_setup, collision_complexity,
	 *     simple_collision_count, convex_collision_count, box_count,
	 *     sphere_count, capsule_count, convex_count }
	 */
	TSharedPtr<FJsonObject> HandleGetStaticMeshCollision(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Non-destructive read of a UStaticMesh asset's LOCAL-space bounds (the
	 * asset-space counterpart to actor_inspect's per-component world_bounds).
	 * Sourced from UStaticMesh::GetBounds() (extended bounds; includes the
	 * positive/negative bounds extensions) and UStaticMesh::GetBoundingBox().
	 *
	 * Required params:
	 *   static_mesh_path (string)  — object/package path to the UStaticMesh.
	 *                                Engine content (/Engine/...) is allowed (read).
	 *
	 * Response on success:
	 *   { success, static_mesh_path,
	 *     local_bounds: { origin:{x,y,z}, box_extent:{x,y,z}, sphere_radius },
	 *     box_min:{x,y,z}, box_max:{x,y,z}, size:{x,y,z},
	 *     positive_bounds_extension:{x,y,z}, negative_bounds_extension:{x,y,z} }
	 */
	TSharedPtr<FJsonObject> HandleGetStaticMeshBounds(const TSharedPtr<FJsonObject>& Params);

	// ── Static-mesh sockets (MCPMeshCommands_Sockets.cpp) ──────────────────────
	// Named relative transforms on a UStaticMesh — the scale-proof storage for the by-eye
	// positioning workflow (muzzle/attach/grip points). GetSocketTransform composes them with
	// the component's live world transform incl. scale, so a hand-tuned offset vector's
	// pre-scale class of bug is impossible. The mutators take `asset_path`, refuse /Engine/
	// content, support dry_run, and are blocked during PIE.

	/**
	 * List sockets on a UStaticMesh. Resolves the mesh either directly from
	 * `asset_path` / `static_mesh_path` (the asset), OR from a level `actor`
	 * (+ optional `component`) — the actor path additionally returns each
	 * socket's resolved `world` transform off the live UStaticMeshComponent.
	 * Returns { asset_path, sockets:[{ socket_name, index, location, rotation,
	 * scale, tag, [world] }], count }.
	 */
	TSharedPtr<FJsonObject> HandleListStaticMeshSockets(const TSharedPtr<FJsonObject>& Params);

	/** Add a named socket with a relative transform. Params: asset_path, socket_name, location_/rotation_/scale_*. dry_run. */
	TSharedPtr<FJsonObject> HandleAddStaticMeshSocket(const TSharedPtr<FJsonObject>& Params);

	/** Update only the provided fields of an existing socket. Params: asset_path, socket_name, any location/rotation/scale component or tag. dry_run. */
	TSharedPtr<FJsonObject> HandleModifyStaticMeshSocket(const TSharedPtr<FJsonObject>& Params);

	/** Remove a socket by name. Params: asset_path, socket_name. dry_run. */
	TSharedPtr<FJsonObject> HandleRemoveStaticMeshSocket(const TSharedPtr<FJsonObject>& Params);
};
