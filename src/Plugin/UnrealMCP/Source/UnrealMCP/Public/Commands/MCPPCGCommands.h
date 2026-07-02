#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for PCG (Procedural Content Generation) MCP commands.
 *
 * Covers the first vertical slice (see docs/proposals/pcg-mcp.md):
 *   - Discovery : pcg_list_graphs, pcg_list_node_types
 *   - Read      : pcg_graph_read
 *   - Authoring : pcg_graph_create, pcg_node_add, pcg_node_connect,
 *                 pcg_node_set_property
 *   - Drive     : pcg_component_add, pcg_component_generate
 *
 * Drives the runtime UPCGGraph API directly (the editor UEdGraph mirror
 * reconstructs itself from NotifyGraphChanged), which is the single source of
 * truth and works regardless of whether a graph editor is open.
 */
class FMCPPCGCommands
{
public:
	FMCPPCGCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// Discovery / read
	TSharedPtr<FJsonObject> HandleListGraphs(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleListNodeTypes(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleGraphRead(const TSharedPtr<FJsonObject>& Params);

	// Authoring
	TSharedPtr<FJsonObject> HandleGraphCreate(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNodeAdd(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNodeConnect(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNodeSetProperty(const TSharedPtr<FJsonObject>& Params);

	// Drive in level
	TSharedPtr<FJsonObject> HandleComponentAdd(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleComponentGenerate(const TSharedPtr<FJsonObject>& Params);
};
