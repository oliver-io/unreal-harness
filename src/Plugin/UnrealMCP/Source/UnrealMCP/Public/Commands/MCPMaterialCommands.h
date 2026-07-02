#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UMaterial;

/**
 * Handler class for Material graph MCP commands.
 * Supports creating materials, adding/connecting/deleting expressions,
 * reading material graphs, and triggering recompilation.
 */
class FMCPMaterialCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

	/** Load a UMaterial by path or short name. */
	static UMaterial* FindMaterialByPath(const FString& MaterialPath);

private:
	// Asset-level
	TSharedPtr<FJsonObject> HandleCreateMaterial(const TSharedPtr<FJsonObject>& Params);
	/** Change top-level flags (blend_mode/two_sided/shading_model/material_domain) on an
	 *  existing UMaterial. Params: { "material_path": "/Game/...", "blend_mode": "Opaque", ... } */
	TSharedPtr<FJsonObject> HandleSetMaterialProperty(const TSharedPtr<FJsonObject>& Params);
	/** Create a UMaterialInstanceConstant whose Parent is set to a given UMaterial(Interface).
	 *  Caller wires textures into it via set_material_instance_parameter afterwards.
	 *  Params: { "asset_path": "/Game/Generated/red_clay/MI_RedClay",
	 *            "parent_material": "/Game/Generated/_M_PBR_Master" } */
	TSharedPtr<FJsonObject> HandleCreateMaterialInstance(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReadMaterialGraph(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReadMaterialFunction(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReadMaterialInstance(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Params);

	/** Enumerate every parameter exposed by a UMaterial or UMaterialInterface,
	 *  using UE reflection (GetAllVectorParameterInfo etc.). Returns names so
	 *  callers can drive set_material_instance_parameter without guessing.
	 *  Param: { "material_path": "/Game/.../X.X" }. */
	TSharedPtr<FJsonObject> HandleListMaterialParameters(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReparentMaterialInstance(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleCompileMaterial(const TSharedPtr<FJsonObject>& Params);

	// Expression CRUD
	TSharedPtr<FJsonObject> HandleAddMaterialExpression(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleDeleteMaterialExpression(const TSharedPtr<FJsonObject>& Params);

	// Wiring
	TSharedPtr<FJsonObject> HandleConnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params);
};
