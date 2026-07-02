#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * UMG / Widget authoring tools. Mirrors mcp/docs/todo/7_widget_umg.md.
 *
 * Surface (rolling):
 *   - widget_create
 *   - widget_tree_read
 *   - widget_add_child
 *   - widget_bind_handler
 *   - widget_set_property (this commit — closes doc 7)
 *
 * widget_tree_read is read-only; the rest mutate and auto-save per the
 * universal Blueprint mutation contract.
 */
class FMCPWidgetCommands
{
public:
    FMCPWidgetCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleWidgetCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleWidgetTreeRead(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleWidgetAddChild(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleWidgetBindHandler(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleWidgetSetProperty(const TSharedPtr<FJsonObject>& Params);
};
