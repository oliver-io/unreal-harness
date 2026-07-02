#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for external mesh/FBX import driven from MCP.
 *
 * The first command, asset_import_mesh, is the headless equivalent of dragging an
 * .fbx (or .obj / .gltf, handled by the same engine code path) into the Content
 * Browser: it runs the engine's FBX import factory through UAssetImportTask +
 * FAssetToolsModule::ImportAssetTasks, producing a UStaticMesh or USkeletalMesh
 * (plus any embedded materials/textures) under /Game/. This is the mesh analogue
 * of FMCPTextureCommands::HandleImportTextures — same UAssetImportTask machinery,
 * different factory/options (UFbxFactory + UFbxImportUI instead of UTextureFactory).
 *
 * Notes for callers:
 *   - FBX files exported from V-Ray / Corona / other DCC renderers usually import
 *     with the engine's DEFAULT materials (the source shaders don't map onto UE's
 *     material model). Expect to rebuild materials afterwards (material_create /
 *     material_apply_to_*), then re-assign via mesh_set_static_mesh_material.
 *   - Skeletal import requires either a `skeleton` (existing USkeleton to bind to)
 *     or the FBX to contain a skeleton/skin (a new USkeleton is then created).
 *
 * This file is the natural home for future mesh-asset import tools
 * (asset_reimport_mesh, LOD import settings, …).
 */
class FMCPMeshImportCommands
{
public:
	FMCPMeshImportCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * Import a single external mesh file as a UStaticMesh or USkeletalMesh asset.
	 *
	 * Required params:
	 *   source_path        (string) — absolute filesystem path to the .fbx/.obj/.gltf
	 *   destination_folder (string) — /Game/... package path the asset lands in
	 *
	 * Optional params:
	 *   name              (string)      — destination asset name; defaults to basename
	 *   import_as_skeletal(bool=false)  — import as USkeletalMesh (else UStaticMesh)
	 *   skeleton          (string)      — /Game/... USkeleton to bind a skeletal import to
	 *   import_materials  (bool=true)   — import embedded materials
	 *   import_textures   (bool=true)   — import embedded textures
	 *   combine_meshes    (bool=false)  — merge all FBX mesh nodes into one (static only)
	 *   force_overwrite   (bool=true)   — replace an existing asset at the destination
	 *
	 * Response on success:
	 *   { count, imported: [ { source, asset_path, class } ],
	 *     created_materials: [ path ], created_textures: [ path ],
	 *     failed: [ { path, reason } ] }
	 */
	TSharedPtr<FJsonObject> HandleImportMesh(const TSharedPtr<FJsonObject>& Params);
};
