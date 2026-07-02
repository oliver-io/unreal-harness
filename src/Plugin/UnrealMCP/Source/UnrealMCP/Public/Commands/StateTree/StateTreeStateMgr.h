#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Static handler for StateTree state hierarchy operations:
 * add, remove, rename, move, duplicate, set properties, list states.
 */
class UNREALMCP_API FStateTreeStateMgr
{
public:
	static TSharedPtr<FJsonObject> AddState(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> RemoveState(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> RenameState(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> MoveState(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> DuplicateState(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> SetStateProperties(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ListStates(const TSharedPtr<FJsonObject>& Params);
};
