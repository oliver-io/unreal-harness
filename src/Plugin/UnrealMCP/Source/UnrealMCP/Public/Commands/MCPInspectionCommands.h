#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * "Orient-rung" inspection tools — compact, read-only summaries of project /
 * level / scene / actor / blueprint scopes. Mirrors mcp/docs/todo/3_multi_res_inspection.md.
 *
 * Surface (rolling):
 *   - project_context
 *   - scene_brief
 *   - level_inspect
 *   - bp_brief
 *   - actor_inspect    (this commit — closes doc 3)
 *
 * Every handler is read-only: no PreEditChange, no MarkPackageDirty, no asset save.
 * Briefs avoid force-loading uncached assets — registry tags first, asset only as fallback.
 */
class FMCPInspectionCommands
{
public:
    FMCPInspectionCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleProjectContext(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSceneBrief(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLevelInspect(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleBpBrief(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleActorInspect(const TSharedPtr<FJsonObject>& Params);
};
