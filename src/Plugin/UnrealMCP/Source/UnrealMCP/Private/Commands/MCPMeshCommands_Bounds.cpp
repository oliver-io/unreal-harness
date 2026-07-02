// FMCPMeshCommands::HandleGetStaticMeshBounds — non-destructive read of a
// UStaticMesh asset's LOCAL-space bounds. The asset-space counterpart to
// actor_inspect's per-component world_bounds (origin / box_extent /
// sphere_radius), sourced from:
//   UStaticMesh::GetBounds()      -> FBoxSphereBounds (extended bounds; includes
//                                    the positive/negative bounds extensions)
//                                    [StaticMesh.cpp:5003 -> GetExtendedBounds()]
//   UStaticMesh::GetBoundingBox() -> FBox of the same extended bounds
//                                    [StaticMesh.cpp:5008]
// Bounds extensions applied per UStaticMesh::CalculateExtendedBounds
// (StaticMesh.cpp:7154-7167).

#include "Commands/MCPMeshCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Engine/StaticMesh.h"
#include "EditorAssetLibrary.h"

namespace
{
	/** {x,y,z} JSON object — matches the project's vector convention. */
	TSharedPtr<FJsonObject> MeshBoundsVec3Json(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}
}

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleGetStaticMeshBounds(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("static_mesh_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: static_mesh_path"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`static_mesh_path` is required (string, non-empty) — an object/package path to a UStaticMesh asset (e.g. /Game/Meshes/SM_Bike or /Engine/BasicShapes/Cube). Use asset_list with asset_type='StaticMesh' to discover meshes."));
	}

	UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
	if (!Mesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("No UStaticMesh at '%s'"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`static_mesh_path` must resolve to a UStaticMesh. Use asset_list with asset_type='StaticMesh' to discover meshes. Engine content paths are allowed for reads."));
	}

	// Extended bounds: render-data bounds plus the asset's positive/negative bounds
	// extensions (UStaticMesh::GetBounds == GetExtendedBounds; StaticMesh.cpp:5003).
	const FBoxSphereBounds B = Mesh->GetBounds();
	const FBox Box = Mesh->GetBoundingBox(); // same extended bounds, as a FBox

	TSharedPtr<FJsonObject> Local = MakeShared<FJsonObject>();
	Local->SetObjectField(TEXT("origin"),     MeshBoundsVec3Json(B.Origin));
	Local->SetObjectField(TEXT("box_extent"), MeshBoundsVec3Json(B.BoxExtent));
	Local->SetNumberField(TEXT("sphere_radius"), B.SphereRadius);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("static_mesh_path"), Mesh->GetPathName());
	Result->SetObjectField(TEXT("local_bounds"), Local);
	Result->SetObjectField(TEXT("box_min"), MeshBoundsVec3Json(Box.Min));
	Result->SetObjectField(TEXT("box_max"), MeshBoundsVec3Json(Box.Max));
	Result->SetObjectField(TEXT("size"),    MeshBoundsVec3Json(B.BoxExtent * 2.0)); // full box size
	Result->SetObjectField(TEXT("positive_bounds_extension"), MeshBoundsVec3Json(Mesh->GetPositiveBoundsExtension()));
	Result->SetObjectField(TEXT("negative_bounds_extension"), MeshBoundsVec3Json(Mesh->GetNegativeBoundsExtension()));
	return Result;
}
