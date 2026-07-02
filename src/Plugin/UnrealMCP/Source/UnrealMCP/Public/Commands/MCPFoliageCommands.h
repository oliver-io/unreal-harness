#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Read-only foliage inspection for MCP (GAPS #14). Mutation (add type / scatter /
 * remove) is refused by design — that is procedural, brush-driven content and
 * belongs to the editor's Foliage mode. This domain only reads existing foliage.
 *
 * Command:
 *   - foliage_inspect : mode='types' (default) lists each foliage type in the
 *     loaded world with its placed-instance count + key properties; mode='instances'
 *     lists placed instance transforms for one foliage type, bounded by limit/offset
 *     (never dumps every instance by default).
 */
class FMCPFoliageCommands
{
public:
	FMCPFoliageCommands() = default;

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	TSharedPtr<FJsonObject> HandleInspect(const TSharedPtr<FJsonObject>& Params);
};
