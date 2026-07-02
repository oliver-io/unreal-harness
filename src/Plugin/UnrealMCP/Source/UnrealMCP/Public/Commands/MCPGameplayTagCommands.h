#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Gameplay tag registry CRUD. Mirrors mcp/docs/todo/9_gameplay_tag_registry.md.
 *
 * Surface (this commit ships the entire doc):
 *   - tag_add
 *   - tag_remove
 *   - tag_list
 *   - tag_move
 *
 * Routes through IGameplayTagsEditorModule (INI writeback) and
 * UGameplayTagsManager (runtime tree refresh). Hard prerequisite for the
 * GAS authoring surface (todo/10_gas_authoring.md).
 */
class FMCPGameplayTagCommands
{
public:
    FMCPGameplayTagCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleTagAdd(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleTagRemove(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleTagList(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleTagMove(const TSharedPtr<FJsonObject>& Params);
};
