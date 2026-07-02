#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * MCP command handler for StateTree operations.
 * Routes 24 commands across 6 categories: asset lifecycle, state hierarchy,
 * node management, transitions, property bindings, and type introspection.
 * Complex categories delegate to static sub-handler classes in StateTree/.
 */
class UNREALMCP_API FMCPStateTreeCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// Category 1: Asset Lifecycle (inline — simple enough to not need sub-handler)
	TSharedPtr<FJsonObject> HandleCreateStateTree(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReadStateTree(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleCompileStateTree(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSaveStateTree(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleVerifyStateTree(const TSharedPtr<FJsonObject>& Params);

	// Category 6: Type Introspection (delegates to FStateTreeTypeCache)
	TSharedPtr<FJsonObject> HandleListStateTreeNodeTypes(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleListStateTreeSchemas(const TSharedPtr<FJsonObject>& Params);
};
