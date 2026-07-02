#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for SkeletalMesh + cloth + physics-asset MCP commands.
 *
 * Phase 1 (read-only): inspect_skeletal_mesh dumps the asset's structure —
 * bones, render sections, material slots, clothing assets, physics asset.
 *
 * Phase 2+ (mutating): set_skeletal_mesh_section_disabled and cloth-replicate
 * tools land here as they're added.
 */
class FMCPSkeletalMeshCommands
{
public:
	FMCPSkeletalMeshCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/** Read-only structural dump. Param: { "path": "/Game/.../SK.SK" }. */
	TSharedPtr<FJsonObject> HandleInspectSkeletalMesh(const TSharedPtr<FJsonObject>& Params);

	/** Toggle a render section's bDisabled flag at the asset level (persisted).
	 *  Params: { "path": "...", "lod_index": 0, "section_index": 0, "disabled": true }. */
	TSharedPtr<FJsonObject> HandleSetSkeletalMeshSectionDisabled(const TSharedPtr<FJsonObject>& Params);

	/** Dump UPhysicsAsset structure: body→bone mapping, per-body collision
	 *  settings (CollisionEnabled, profile name), per-shape geometry
	 *  (capsule/sphere/box dimensions + transforms), and constraint
	 *  bone-pairs. Param: { "path": "/Game/.../PA.PA" }. */
	TSharedPtr<FJsonObject> HandleInspectPhysicsAsset(const TSharedPtr<FJsonObject>& Params);

	/** Mutate per-body CollisionEnabled on a UPhysicsAsset. Pass body_names
	 *  (array of bone names) to target specific bodies, or omit to target all.
	 *  Saves the asset by default.
	 *  Params: { "path": "/Game/.../PA.PA",
	 *            "body_names": ["Head_M", ...]   // optional; all bodies if absent
	 *            "collision_enabled": "QueryAndPhysics" | "QueryOnly" |
	 *                                 "PhysicsOnly" | "NoCollision" |
	 *                                 "ProbeOnly" | "QueryAndProbe",
	 *            "save": true }. */
	TSharedPtr<FJsonObject> HandleSetPhysicsBodyCollision(const TSharedPtr<FJsonObject>& Params);

	/** Edit or delete a constraint in a UPhysicsAsset's ConstraintSetup, keyed by
	 *  `joint_name` OR a (`bone1`,`bone2`) pair (order-insensitive). Either delete
	 *  it (`delete: true`) or relax/lock its motions (Free | Limited | Locked).
	 *  Params: { "path": "/Game/.../PA.PA",
	 *            "joint_name": "..."         // OR
	 *            "bone1": "calf_r", "bone2": "thigh_r",
	 *            "delete": false,
	 *            "swing1"/"swing2"/"twist": "Free"|"Limited"|"Locked",   // angular, optional
	 *            "linear_x"/"linear_y"/"linear_z": "Free"|"Limited"|"Locked", // linear, optional
	 *            "save": true }. */
	TSharedPtr<FJsonObject> HandleSetPhysicsConstraintMotion(const TSharedPtr<FJsonObject>& Params);

	/** Repoint (or clear) a USkeletalMesh's PhysicsAsset via USkeletalMesh::SetPhysicsAsset.
	 *  Params: { "path": "/Game/.../SK.SK",
	 *            "physics_asset": "/Game/.../PA.PA" | "",  // "" clears the binding
	 *            "save": true }. */
	TSharedPtr<FJsonObject> HandleSetSkeletalMeshPhysicsAsset(const TSharedPtr<FJsonObject>& Params);

	/** Copy named bones from a source USkeleton into a target USkeleton, preserving
	 *  parent relationships and bind-pose transforms. The first bone in the list
	 *  is parented to `target_parent_bone` (default: target root). Subsequent bones
	 *  must reference parents already in the source list (added in order).
	 *  Params: { "source_skeleton": "...", "target_skeleton": "...",
	 *            "bones": ["Bone","Bone_001",...], "target_parent_bone": "Root" }. */
	TSharedPtr<FJsonObject> HandleMergeBonesIntoSkeleton(const TSharedPtr<FJsonObject>& Params);

	/** Same as MergeBonesIntoSkeleton but operates on a USkeletalMesh's ref skeleton
	 *  (the one that governs vertex skinning) AND syncs the bones into the bound
	 *  USkeleton. This is what's needed to make new bones available for skinning new
	 *  geometry. Params: same as merge_bones_into_skeleton plus "target_mesh": SK path. */
	TSharedPtr<FJsonObject> HandleMergeBonesIntoSkeletalMesh(const TSharedPtr<FJsonObject>& Params);

	/** (Re)build a Root→tip bone CHAIN up one axis of a skeletal mesh and procedurally
	 *  RE-SKIN every vertex to it by position along that axis, with smooth two-bone
	 *  blends across each joint — so the mesh can actually bend. Built for the fishing
	 *  rod: the pole was baked static→skeletal with a single Root bone, so it's rigid;
	 *  this inserts N pole bones in decreasing segment lengths (stiff butt, finer/whippy
	 *  tip) and re-skins. Rebuilds the ref skeleton + skin from the mesh geometry each
	 *  call (idempotent), then syncs the bones into the bound USkeleton. Pass
	 *  `dry_run: true` to preview the bone-station table without mutating.
	 *  Params: { "path": SKM, "num_bones": 15, "axis": "z", "base_fraction": 0.14,
	 *            "segment_ratio": 0.85, "bone_prefix": "pole_", "root_bone_name": "Root",
	 *            "dry_run": false }. */
	TSharedPtr<FJsonObject> HandleBuildBendChain(const TSharedPtr<FJsonObject>& Params);
};
