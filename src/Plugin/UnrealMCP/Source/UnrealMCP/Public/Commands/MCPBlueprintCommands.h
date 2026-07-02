#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for Blueprint-related MCP commands
 */
class FMCPBlueprintCommands
{
public:
    	FMCPBlueprintCommands();

    // Handle blueprint commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Specific blueprint command handlers (only used functions)
    TSharedPtr<FJsonObject> HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddComponentToBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetComponentTransform(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetClassReplication(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetEventReplication(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetPhysicsProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetStaticMeshProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMeshMaterialColor(const TSharedPtr<FJsonObject>& Params);
    
    // Material management functions
    TSharedPtr<FJsonObject> HandleGetAvailableMaterials(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleApplyMaterialToActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleApplyMaterialToBlueprint(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetStaticMeshMaterial(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetActorMaterialInfo(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetBlueprintMaterialInfo(const TSharedPtr<FJsonObject>& Params);

    // Blueprint analysis functions
    TSharedPtr<FJsonObject> HandleReadBlueprintContent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAnalyzeBlueprintGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetBlueprintVariableDetails(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetBlueprintFunctionDetails(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetClassProperties(const TSharedPtr<FJsonObject>& Params);

    // Mesh query
    TSharedPtr<FJsonObject> HandleGetMeshBounds(const TSharedPtr<FJsonObject>& Params);

    // AnimBP graph enumeration and reparent
    TSharedPtr<FJsonObject> HandleListBlueprintGraphs(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleReparentBlueprint(const TSharedPtr<FJsonObject>& Params);

    // Animation asset commands
    TSharedPtr<FJsonObject> HandleListSkeletons(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListAnimSequences(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateBlendSpace(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddBlendSpaceSample(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveBlendSpaceSample(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetAnimSequenceProperty(const TSharedPtr<FJsonObject>& Params);

    // Skeleton socket commands
    TSharedPtr<FJsonObject> HandleListSkeletonSockets(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddSkeletonSocket(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleModifySkeletonSocket(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRemoveSkeletonSocket(const TSharedPtr<FJsonObject>& Params);

    // Pin introspection
    TSharedPtr<FJsonObject> HandleListNodePins(const TSharedPtr<FJsonObject>& Params);

    // BP CDO default-value writer for inherited C++ UPROPERTYs.
    // set_blueprint_variable_properties only handles BP-NEW variables —
    // inherited properties from a native parent class are written via
    // the BP class's GeneratedClass->ClassDefaultObject CDO. This command
    // covers that gap (e.g. binding a TSoftObjectPtr<UDataAsset>
    // declared in C++ on AGameModeBase descendants).
    TSharedPtr<FJsonObject> HandleSetBlueprintDefaultValue(const TSharedPtr<FJsonObject>& Params);

    // Animation asset discovery
    TSharedPtr<FJsonObject> HandleListBlendSpaces(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleReadBlendSpace(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListAnimBlueprints(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListAnimMontages(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListAnimLayerInterfaces(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleReadAnimMontage(const TSharedPtr<FJsonObject>& Params);
};