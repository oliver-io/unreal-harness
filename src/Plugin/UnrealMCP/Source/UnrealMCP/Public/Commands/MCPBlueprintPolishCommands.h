#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Blueprint authoring polish — small completeness fixes.
 * Mirrors mcp/docs/todo/12_blueprint_polish.md.
 *
 * Surface (this commit ships the entire doc):
 *   - bp_remove_component
 *   - bp_disconnect_pin
 *   - bp_get_parent_class
 *   - bp_list_components
 *
 * The two reads (`bp_get_parent_class`, `bp_list_components`) are
 * registry-first / structural-fallback. The two mutators
 * (`bp_remove_component`, `bp_disconnect_pin`) follow the universal
 * Pre/Post mutation contract and recompile + save.
 */
class FMCPBlueprintPolishCommands
{
public:
    FMCPBlueprintPolishCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDisconnectPin(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetParentClass(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListComponents(const TSharedPtr<FJsonObject>& Params);
};
