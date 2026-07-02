#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Read-only reflection tools — class registry navigation, per-class/enum inspection,
 * Blueprint function cross-references. Mirrors mcp/docs/todo/2_reflection_expansion.md.
 *
 * Surface (rolling):
 *   - enum_inspect
 *   - class_query
 *   - bp_function_references
 *   - class_inspect          (this commit — supersedes get_class_properties; closes doc 2)
 *
 * Every handler is read-only: no PreEditChange, no MarkPackageDirty, no asset save.
 */
class FMCPReflectionCommands
{
public:
    FMCPReflectionCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleEnumInspect(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleClassQuery(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleBpFunctionReferences(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleClassInspect(const TSharedPtr<FJsonObject>& Params);
};
