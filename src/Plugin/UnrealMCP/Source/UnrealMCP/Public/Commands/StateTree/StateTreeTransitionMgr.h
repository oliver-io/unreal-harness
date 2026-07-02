#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Static handler for StateTree transition operations:
 * add, remove, set properties. Handles FStateTreeStateLink resolution.
 */
class UNREALMCP_API FStateTreeTransitionMgr
{
public:
	static TSharedPtr<FJsonObject> AddTransition(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> RemoveTransition(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> SetTransitionProperties(const TSharedPtr<FJsonObject>& Params);
};
