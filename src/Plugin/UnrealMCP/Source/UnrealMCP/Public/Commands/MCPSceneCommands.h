#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Scene query / compose primitives. Mirrors mcp/docs/todo/5_scene_query_compose.md.
 *
 * Surface:
 *   - actor_query   (six filter axes, AND-composed)
 *   - actor_spawn   (UClass + transform + optional name / tags / folder)
 *
 * Doc invariants enforced here:
 *   - actor_query never force-loads streamed sublevels; reports skipped ones
 *     in the response (parallel to scene_brief).
 *   - actor_spawn never auto-possesses Pawns. Class loadability is validated
 *     up front; failures flow through the structured-error envelope.
 *   - Both tools accept and return world coordinates (no local-space filters).
 */
class FMCPSceneCommands
{
public:
    FMCPSceneCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleActorQuery(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleActorSpawn(const TSharedPtr<FJsonObject>& Params);
};
