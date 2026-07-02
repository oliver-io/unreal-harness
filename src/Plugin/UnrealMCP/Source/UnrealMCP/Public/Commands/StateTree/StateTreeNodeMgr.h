#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FStateTreeEditorNode;
class UStateTreeState;

/**
 * Static handler for StateTree node operations:
 * add, remove, set properties, get properties.
 * Contains the critical template-bypass logic for constructing FStateTreeEditorNode
 * from runtime type names (since AddTask<T>() is template-only).
 */
class UNREALMCP_API FStateTreeNodeMgr
{
public:
	static TSharedPtr<FJsonObject> AddNode(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> RemoveNode(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> SetNodeProperty(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> GetNodeProperties(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Inline node construction used by AddState for inline tasks/conditions.
	 * @param State - target state to add the node to
	 * @param SlotType - "task" or "enter_condition"
	 * @param NodeSpec - JSON spec with "type", optional "properties", "operand"
	 * @param OutNodeId - receives the new node's GUID string
	 * @param OutError - error message if construction fails
	 * @return true on success
	 */
	static bool AddNodeInline(UStateTreeState* State, const FString& SlotType,
		const TSharedPtr<FJsonObject>& NodeSpec, FString& OutNodeId, FString& OutError);

private:
	/**
	 * Construct an FStateTreeEditorNode from a runtime UScriptStruct* (template bypass).
	 * 1. Node.InitializeAs(ResolvedStruct)
	 * 2. Instance.InitializeAs(GetInstanceDataType())
	 * 3. ID = FGuid::NewGuid()
	 */
	static bool ConstructEditorNode(FStateTreeEditorNode& OutNode, UScriptStruct* NodeStruct,
		const TSharedPtr<FJsonObject>& Properties, FString& OutError);
};
