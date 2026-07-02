#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * MCP command handler for AI runtime inspection during PIE.
 * 3 tools: get AI state (StateTree), awareness (combat component), perception.
 */
class UNREALMCP_API FMCPAIRuntimeCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	TSharedPtr<FJsonObject> HandleGetAIState(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleGetAIAwareness(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleGetAIPerception(const TSharedPtr<FJsonObject>& Params);
};
