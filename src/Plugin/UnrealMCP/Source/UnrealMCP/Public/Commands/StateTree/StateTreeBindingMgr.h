#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Static handler for StateTree property binding operations:
 * add, remove, list bindings, list bindable properties.
 */
class UNREALMCP_API FStateTreeBindingMgr
{
public:
	static TSharedPtr<FJsonObject> AddPropertyBinding(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> RemovePropertyBinding(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ListPropertyBindings(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ListBindableProperties(const TSharedPtr<FJsonObject>& Params);
};
