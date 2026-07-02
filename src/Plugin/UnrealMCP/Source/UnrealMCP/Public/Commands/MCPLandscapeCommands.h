#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Read-only landscape inspection for MCP (GAPS #13). Mutation is refused by
 * design (sculpt/paint/import belong to the editor's Landscape mode) — this
 * domain only reads existing landscape state.
 *
 * Commands:
 *   - landscape_inspect       : enumerate ALandscapeProxy actors in the editor
 *                               world + core properties (component/subsection
 *                               sizing, material, transform, quad extent).
 *   - landscape_list_layers   : list a landscape's paint (target) layers and
 *                               whether each has a LayerInfo object assigned.
 *   - landscape_read_heightmap: bounded summary of a landscape's heightmap
 *                               (region + height statistics), with an optional
 *                               small inline sample grid or a raw .r16 export to
 *                               disk. Never dumps a full grid inline. Editor-only
 *                               (WITH_EDITOR); returns a clean error otherwise.
 */
class FMCPLandscapeCommands
{
public:
	FMCPLandscapeCommands() = default;

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	TSharedPtr<FJsonObject> HandleInspect(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleListLayers(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReadHeightmap(const TSharedPtr<FJsonObject>& Params);
};
