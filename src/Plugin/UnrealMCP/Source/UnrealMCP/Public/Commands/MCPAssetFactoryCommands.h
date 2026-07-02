#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Primitive asset factory tools — UEnum / UStruct / UDataTable / UInputAction /
 * MaterialFunction / MaterialParameterCollection / NiagaraScript shells.
 * Mirrors mcp/docs/todo/6_asset_factory.md.
 *
 * Surface (rolling):
 *   - enum_create
 *   - struct_create
 *   - datatable_create
 *   - mpc_create
 *   - material_function_create
 *   - niagara_script_create
 *   - input_create            (this commit) — last tool in doc 6
 *   - physics_material_create (post-doc-6 addition — UPhysicalMaterial shell)
 *
 * UDataAsset creation is already covered by the existing `create_data_asset`
 * tool (FMCPDataAssetCommands); doc 14 will alias `dataasset_create`
 * onto it rather than re-implementing.
 *
 * Doc 6 invariants enforced:
 *   - Auto-save on every successful creation (philosophy #5).
 *   - Path uniqueness — name_collision on existing path; no silent overwrite.
 *   - Validate before factory invocation; structured-error envelope on failure.
 */
class FMCPAssetFactoryCommands
{
public:
    FMCPAssetFactoryCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleEnumCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleStructCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDatatableCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleMpcCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleMaterialFunctionCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleNiagaraScriptCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleInputCreate(const TSharedPtr<FJsonObject>& Params);
    // GAP-002: add key→action mapping rows (FEnhancedActionKeyMapping) to an IMC.
    TSharedPtr<FJsonObject> HandleInputAddMapping(const TSharedPtr<FJsonObject>& Params);
    // UPhysicalMaterial shell (friction/restitution/density + combine modes).
    TSharedPtr<FJsonObject> HandlePhysicsMaterialCreate(const TSharedPtr<FJsonObject>& Params);
};
