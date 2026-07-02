#include "StateTreeNodeMgr.h"
#include "StateTreeTypeCache.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeNodeBase.h"
#include "StateTreeTaskBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEvaluatorBase.h"
#include "InstancedStruct.h"
#include "MCPCommonUtils.h"

// Shared error-message body so both AddNode and AddNodeInline (in this TU) and
// AddTransition's inline-conditions loop (in StateTreeTransitionMgr.cpp) emit a
// byte-identical "Valid: ..." enumeration string. Pre-bundle, all three sites
// silently collapsed unrecognized `operand` strings to `EStateTreeExpressionOperand::And`
// — the binary-only sibling of the trigger/priority silent-default class closed
// in the prior 2026-04-22 enum-string validation bundle.
#define MCP_INVALID_OPERAND_MSG TEXT("Invalid 'operand' value '%s'. Valid: And, Or.")

// ---- Core: Template-bypass node construction ----

bool FStateTreeNodeMgr::ConstructEditorNode(FStateTreeEditorNode& OutNode, UScriptStruct* NodeStruct,
	const TSharedPtr<FJsonObject>& Properties, FString& OutError)
{
	if (!NodeStruct)
	{
		OutError = TEXT("NodeStruct is null");
		return false;
	}

	// Step 1: Initialize the node struct
	OutNode.Node.InitializeAs(NodeStruct);
	OutNode.ID = FGuid::NewGuid();

	// Step 2: Get instance data type from the node base
	const FStateTreeNodeBase* NodeBase = OutNode.Node.GetPtr<FStateTreeNodeBase>();
	if (!NodeBase)
	{
		OutError = FString::Printf(TEXT("Struct '%s' does not derive from FStateTreeNodeBase"), *NodeStruct->GetName());
		return false;
	}

	const UScriptStruct* InstanceType = Cast<UScriptStruct>(NodeBase->GetInstanceDataType());
	if (InstanceType)
	{
		// Step 3: Initialize instance data
		OutNode.Instance.InitializeAs(const_cast<UScriptStruct*>(InstanceType));
	}

	// Step 4: Apply property overrides to instance data
	if (Properties.IsValid() && Properties->Values.Num() > 0)
	{
		if (OutNode.Instance.IsValid())
		{
			if (!FStateTreeTypeCache::ApplyPropertyOverrides(OutNode.Instance, Properties, OutError))
			{
				return false;
			}
		}
		else
		{
			// Try applying to the node struct itself (some nodes have properties on the node, not instance data)
			// Create a temporary wrapper
			FInstancedStruct NodeWrapper;
			NodeWrapper.InitializeAs(NodeStruct, OutNode.Node.GetMemory());
			if (!FStateTreeTypeCache::ApplyPropertyOverrides(NodeWrapper, Properties, OutError))
			{
				// Not fatal — just warn
				OutError = FString::Printf(TEXT("Warning: could not apply properties (no instance data type on '%s')"), *NodeStruct->GetName());
			}
		}
	}

	return true;
}

// ---- AddNode (full MCP tool version) ----

TSharedPtr<FJsonObject> FStateTreeNodeMgr::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	// Required: node_type
	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_type' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_type` (string) — the canonical short name of the node (e.g. \"STTask_AnimalWander\", \"STCond_TargetVisible\"). Use `statetree_list_node_types` to discover."));
	}

	// Resolve type
	UScriptStruct* NodeStruct = FStateTreeTypeCache::Get().ResolveNodeType(NodeType);
	if (!NodeStruct)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown node type: '%s'. Use list_state_tree_node_types to discover available types."), *NodeType),
			EMCPErrorCode::ClassNotLoaded,
			TEXT("Node type didn't resolve to a UScriptStruct. Use the canonical short name (e.g. \"STTask_AnimalWander\" — no F prefix) emitted by `statetree_list_node_types`."));
	}

	// Required: target (where to attach)
	const TSharedPtr<FJsonObject>* TargetObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("target"), TargetObj))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target' object parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `target` (JSON object) with `slot` plus the slot-specific identifier — `state` for task/enter_condition, `transition` for condition. Example: `{\"slot\": \"task\", \"state\": \"Engage\"}`."));
	}

	FString Slot;
	if (!(*TargetObj)->TryGetStringField(TEXT("slot"), Slot))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'slot' in target"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target.slot` (string) selects which collection the node attaches to. Valid: \"task\", \"enter_condition\", \"consideration\", \"condition\", \"evaluator\", \"global_task\"."));
	}

	// Optional properties
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	TSharedPtr<FJsonObject> Properties;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj))
	{
		Properties = *PropsObj;
	}

	// Construct the node
	FStateTreeEditorNode NewNode;
	FString ConstructError;
	if (!ConstructEditorNode(NewNode, NodeStruct, Properties, ConstructError))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to construct node: %s"), *ConstructError),
			EMCPErrorCode::Internal,
			TEXT("Node type was recognized but instance construction failed. Check the message above for the specific failure (typically a property override didn't apply)."));
	}

	// Set condition-specific properties. Validate `operand` against the And/Or
	// pair before writing — pre-bundle, an unrecognized string silently collapsed
	// to And, indistinguishable from a correct call. NewNode is stack-local (not
	// yet inserted into any TargetArray below), so the early return on validation
	// failure leaves the asset untouched.
	// Considerations take the same operand/indent expression fields: the engine
	// compiler consumes ConsiderationNode.ExpressionOperand / ExpressionIndent
	// exactly like conditions (StateTreeCompiler.cpp:2184-2228 mirrors the
	// condition loop at :800-817).
	if (Slot.Contains(TEXT("condition")) || Slot == TEXT("consideration"))
	{
		FString Operand = TEXT("And");
		Params->TryGetStringField(TEXT("operand"), Operand);
		if (Operand != TEXT("And") && Operand != TEXT("Or"))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(MCP_INVALID_OPERAND_MSG, *Operand),
				EMCPErrorCode::InvalidArgument,
				TEXT("`operand` must be exactly \"And\" or \"Or\" (case-sensitive). Selects how this condition combines with siblings."));
		}
		NewNode.ExpressionOperand = (Operand == TEXT("Or"))
			? EStateTreeExpressionOperand::Or
			: EStateTreeExpressionOperand::And;

		double Indent = 0;
		if (Params->TryGetNumberField(TEXT("indent"), Indent))
		{
			NewNode.ExpressionIndent = static_cast<int32>(Indent);
		}
	}

	// Insert into the appropriate array
	int32 InsertIndex = INDEX_NONE;
	double InsertIdxD;
	if (Params->TryGetNumberField(TEXT("insert_index"), InsertIdxD))
	{
		InsertIndex = static_cast<int32>(InsertIdxD);
	}

	FString Location;

	// Per-state slots. `consideration` targets UStateTreeState::Considerations
	// (verified UE 5.7: StateTreeState.h:424-426, TArray<FStateTreeEditorNode>
	// with BaseStruct StateTreeConsiderationBase — exactly parallel to
	// EnterConditions/Tasks). Note the engine compiler warns that compiled
	// considerations have no effect unless the PARENT state's selection
	// behavior is a utility mode (StateTreeCompiler.cpp:2172-2182).
	if (Slot == TEXT("task") || Slot == TEXT("enter_condition") || Slot == TEXT("consideration"))
	{
		FString StateId;
		if (!(*TargetObj)->TryGetStringField(TEXT("state"), StateId))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'state' in target for task/enter_condition/consideration slot"),
				EMCPErrorCode::InvalidArgument,
				TEXT("Task / enter_condition / consideration nodes attach to a specific state. Pass `target.state` (string) with the state name or GUID. Use `statetree_state_list` to discover."));
		}

		UStateTreeState* State = FStateTreeTypeCache::FindState(EditorData, StateId, Error);
		if (!State) return Error;

		TArray<FStateTreeEditorNode>* TargetArray = nullptr;
		if (Slot == TEXT("task"))
		{
			TargetArray = &State->Tasks;
			Location = FString::Printf(TEXT("task on state '%s'"), *State->Name.ToString());
		}
		else if (Slot == TEXT("enter_condition"))
		{
			TargetArray = &State->EnterConditions;
			Location = FString::Printf(TEXT("enter condition on state '%s'"), *State->Name.ToString());
		}
		else if (Slot == TEXT("consideration"))
		{
			TargetArray = &State->Considerations;
			Location = FString::Printf(TEXT("utility consideration on state '%s'"), *State->Name.ToString());
		}

		if (TargetArray)
		{
			// Modify() the state containing the array — marks package dirty so the
			// subsequent save/compile sees a changed asset. Pre-fix node adds left
			// no package-level dirty flag.
			State->Modify();
			if (InsertIndex >= 0 && InsertIndex < TargetArray->Num())
				TargetArray->Insert(NewNode, InsertIndex);
			else
				TargetArray->Add(NewNode);
		}
	}
	else if (Slot == TEXT("condition"))
	{
		// Condition on a transition
		FString TransitionId;
		if (!(*TargetObj)->TryGetStringField(TEXT("transition"), TransitionId))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'transition' in target for condition slot"),
				EMCPErrorCode::InvalidArgument,
				TEXT("Condition nodes attach to a specific transition. Pass `target.transition` (string GUID) of an existing transition (use `statetree_read` with `include_bindings=true` to discover)."));
		}

		FGuid TransGuid;
		if (!FGuid::Parse(TransitionId, TransGuid))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Invalid transition GUID"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`target.transition` must be a parseable FGuid string. Use the exact value emitted by `statetree_read`'s transition entries."));
		}

		// Find the transition
		bool bFound = false;
		TFunction<void(UStateTreeState*)> SearchTransitions;
		SearchTransitions = [&](UStateTreeState* State) {
			if (bFound) return;
			if (!State) return;
			for (FStateTreeTransition& Trans : State->Transitions)
			{
				if (Trans.ID == TransGuid)
				{
					// Modify() the state that owns the transition — the transition
					// array lives on the state, so that's the transactional scope.
					State->Modify();
					if (InsertIndex >= 0 && InsertIndex < Trans.Conditions.Num())
						Trans.Conditions.Insert(NewNode, InsertIndex);
					else
						Trans.Conditions.Add(NewNode);
					Location = FString::Printf(TEXT("condition on transition in state '%s'"), *State->Name.ToString());
					bFound = true;
					return;
				}
			}
			for (UStateTreeState* Child : State->Children)
				SearchTransitions(Child);
		};

		for (UStateTreeState* Root : EditorData->SubTrees)
			SearchTransitions(Root);

		if (!bFound)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Transition not found: %s"), *TransitionId),
				EMCPErrorCode::NodeNotFound,
				TEXT("No transition with that GUID exists in the StateTree. Use `statetree_read` with `include_bindings=true` to discover transition GUIDs."));
		}
	}
	else if (Slot == TEXT("evaluator"))
	{
		// EditorData owns the global evaluator array — Modify() it directly.
		EditorData->Modify();
		if (InsertIndex >= 0 && InsertIndex < EditorData->Evaluators.Num())
			EditorData->Evaluators.Insert(NewNode, InsertIndex);
		else
			EditorData->Evaluators.Add(NewNode);
		Location = TEXT("global evaluator");
	}
	else if (Slot == TEXT("global_task"))
	{
		EditorData->Modify();
		if (InsertIndex >= 0 && InsertIndex < EditorData->GlobalTasks.Num())
			EditorData->GlobalTasks.Insert(NewNode, InsertIndex);
		else
			EditorData->GlobalTasks.Add(NewNode);
		Location = TEXT("global task");
	}
	else
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown slot type: '%s'. Valid slots: task, enter_condition, consideration, condition, evaluator, global_task."), *Slot),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target.slot` must be one of: \"task\", \"enter_condition\", \"consideration\" (per-state slots requiring `target.state`; considerations only take effect when the parent state's selection behavior is a utility mode), \"condition\" (per-transition, requires `target.transition`), \"evaluator\", \"global_task\" (StateTree-wide)."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), NewNode.ID.ToString());
	Result->SetStringField(TEXT("node_type"), NodeStruct->GetName());
	Result->SetStringField(TEXT("location"), Location);
	return Result;
}

// ---- AddNodeInline (used by AddState for inline specs) ----

bool FStateTreeNodeMgr::AddNodeInline(UStateTreeState* State, const FString& SlotType,
	const TSharedPtr<FJsonObject>& NodeSpec, FString& OutNodeId, FString& OutError)
{
	FString TypeName;
	if (!NodeSpec->TryGetStringField(TEXT("type"), TypeName))
	{
		OutError = TEXT("Inline node spec missing 'type' field");
		return false;
	}

	UScriptStruct* NodeStruct = FStateTreeTypeCache::Get().ResolveNodeType(TypeName);
	if (!NodeStruct)
	{
		OutError = FString::Printf(TEXT("Unknown node type: '%s'"), *TypeName);
		return false;
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	TSharedPtr<FJsonObject> Properties;
	if (NodeSpec->TryGetObjectField(TEXT("properties"), PropsObj))
	{
		Properties = *PropsObj;
	}

	FStateTreeEditorNode NewNode;
	if (!ConstructEditorNode(NewNode, NodeStruct, Properties, OutError))
	{
		return false;
	}

	// Set condition-specific properties (symmetric with the AddNode path above —
	// inline specs accept the same `operand` and `indent` fields that the canonical
	// st_add_node write path accepts). Validate-or-error matches AddNode; the
	// caller (ProcessInlineNodes in StateTreeStateMgr.cpp) accumulates the error
	// string into the `inline_warning` response field, so an invalid operand on
	// one inline condition causes that condition to be dropped (not added) with
	// an explicit warning rather than silently collapsing to And.
	if (SlotType.Contains(TEXT("condition")))
	{
		FString Operand = TEXT("And");
		NodeSpec->TryGetStringField(TEXT("operand"), Operand);
		if (Operand != TEXT("And") && Operand != TEXT("Or"))
		{
			OutError = FString::Printf(MCP_INVALID_OPERAND_MSG, *Operand);
			return false;
		}
		NewNode.ExpressionOperand = (Operand == TEXT("Or"))
			? EStateTreeExpressionOperand::Or
			: EStateTreeExpressionOperand::And;

		double Indent = 0;
		if (NodeSpec->TryGetNumberField(TEXT("indent"), Indent))
		{
			NewNode.ExpressionIndent = static_cast<int32>(Indent);
		}
	}

	// Append to the appropriate array — Modify() the state so the package is
	// marked dirty. This runs from AddState's inline-nodes loop; AddState already
	// Modified() the parent/EditorData before inserting State itself, but this
	// State object is brand-new and hasn't had Modify() called on it yet.
	if (SlotType == TEXT("task"))
	{
		State->Modify();
		State->Tasks.Add(NewNode);
	}
	else if (SlotType == TEXT("enter_condition"))
	{
		State->Modify();
		State->EnterConditions.Add(NewNode);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown inline slot type: '%s'"), *SlotType);
		return false;
	}

	OutNodeId = NewNode.ID.ToString();
	return true;
}

// ---- RemoveNode ----

TSharedPtr<FJsonObject> FStateTreeNodeMgr::RemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_id` (string GUID) — discoverable via `statetree_state_list` or `statetree_read` (each task/condition/evaluator has its own GUID)."));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeId, NodeGuid))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid node_id GUID format"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`node_id` must be a parseable FGuid string (e.g. \"12345678-1234-1234-1234-123456789012\"). Use the exact value returned by `statetree_state_list` / `statetree_read`."));
	}

	auto SearchResult = FStateTreeTypeCache::FindNodeByGuid(EditorData, NodeGuid);
	if (!SearchResult.Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeId),
			EMCPErrorCode::NodeNotFound,
			TEXT("Node GUID didn't match any node in the StateTree (states, transitions, evaluators, global_tasks all searched). Use `statetree_state_list` / `statetree_read` to discover node GUIDs."));
	}

	FString Location = SearchResult.SlotType;
	if (SearchResult.OwningState)
	{
		Location += FString::Printf(TEXT(" on state '%s'"), *SearchResult.OwningState->Name.ToString());
	}

	// Remove from the appropriate array — Modify() the owner of the array first
	// so the package is marked dirty.
	if (SearchResult.SlotType == TEXT("task") && SearchResult.OwningState)
	{
		SearchResult.OwningState->Modify();
		SearchResult.OwningState->Tasks.RemoveAt(SearchResult.ArrayIndex);
	}
	else if (SearchResult.SlotType == TEXT("enter_condition") && SearchResult.OwningState)
	{
		SearchResult.OwningState->Modify();
		SearchResult.OwningState->EnterConditions.RemoveAt(SearchResult.ArrayIndex);
	}
	else if (SearchResult.SlotType == TEXT("consideration") && SearchResult.OwningState)
	{
		SearchResult.OwningState->Modify();
		SearchResult.OwningState->Considerations.RemoveAt(SearchResult.ArrayIndex);
	}
	else if (SearchResult.SlotType == TEXT("evaluator"))
	{
		EditorData->Modify();
		EditorData->Evaluators.RemoveAt(SearchResult.ArrayIndex);
	}
	else if (SearchResult.SlotType == TEXT("global_task"))
	{
		EditorData->Modify();
		EditorData->GlobalTasks.RemoveAt(SearchResult.ArrayIndex);
	}
	else if (SearchResult.SlotType.Contains(TEXT("transition")) && SearchResult.OwningState)
	{
		// Parse transition index from slot type (e.g. "transition_0_condition")
		int32 TransIdx = INDEX_NONE;
		FString SlotStr = SearchResult.SlotType;
		SlotStr.RemoveFromStart(TEXT("transition_"));
		int32 UnderscorePos;
		if (SlotStr.FindChar('_', UnderscorePos))
		{
			TransIdx = FCString::Atoi(*SlotStr.Left(UnderscorePos));
		}
		if (TransIdx != INDEX_NONE && TransIdx < SearchResult.OwningState->Transitions.Num())
		{
			SearchResult.OwningState->Modify();
			SearchResult.OwningState->Transitions[TransIdx].Conditions.RemoveAt(SearchResult.ArrayIndex);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("removed_from"), Location);
	return Result;
}

// ---- SetNodeProperty ----

TSharedPtr<FJsonObject> FStateTreeNodeMgr::SetNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_id` (string GUID) — discoverable via `statetree_state_list` or `statetree_read` (each task/condition/evaluator has its own GUID)."));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `property_name` (string) — the UPROPERTY name on the node's instance struct. Use `statetree_node_get_properties` to discover."));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeId, NodeGuid))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid node_id GUID format"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`node_id` must be a parseable FGuid string (e.g. \"12345678-1234-1234-1234-123456789012\"). Use the exact value returned by `statetree_state_list` / `statetree_read`."));
	}

	auto SearchResult = FStateTreeTypeCache::FindNodeByGuid(EditorData, NodeGuid);
	if (!SearchResult.Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeId),
			EMCPErrorCode::NodeNotFound,
			TEXT("Node GUID didn't match any node in the StateTree (states, transitions, evaluators, global_tasks all searched). Use `statetree_state_list` / `statetree_read` to discover node GUIDs."));
	}

	// Get the property value from params
	TSharedPtr<FJsonValue> PropertyValue = Params->TryGetField(TEXT("property_value"));
	if (!PropertyValue.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_value' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `property_value` — the value to assign. Type matches the UPROPERTY (string for FString/FName, number for ints/floats, bool, JSON object for FInstancedStruct/structs)."));
	}

	// Build a single-property JSON object and apply.
	// Modify() the state that owns the node (or EditorData for global nodes) BEFORE
	// the instance-data write — pre-fix this mutator never marked the package dirty,
	// so st_set_node_property edits could be silently lost if the caller didn't
	// follow up with another mutator that happened to trigger a dirty flag.
	if (SearchResult.OwningState)
	{
		SearchResult.OwningState->Modify();
	}
	else
	{
		EditorData->Modify();
	}

	TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
	PropsObj->SetField(PropertyName, PropertyValue);

	FString ApplyError;
	if (!FStateTreeTypeCache::ApplyPropertyOverrides(SearchResult.Node->Instance, PropsObj, ApplyError))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			ApplyError,
			EMCPErrorCode::InvalidArgument,
			TEXT("ApplyPropertyOverrides rejected the value. Common causes: type mismatch, malformed struct JSON, unknown enum value. Check the message above for the specific property."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("property_name"), PropertyName);
	return Result;
}

// ---- GetNodeProperties ----

TSharedPtr<FJsonObject> FStateTreeNodeMgr::GetNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_id` (string GUID) — discoverable via `statetree_state_list` or `statetree_read` (each task/condition/evaluator has its own GUID)."));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeId, NodeGuid))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid node_id GUID format"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`node_id` must be a parseable FGuid string (e.g. \"12345678-1234-1234-1234-123456789012\"). Use the exact value returned by `statetree_state_list` / `statetree_read`."));
	}

	auto SearchResult = FStateTreeTypeCache::FindNodeByGuid(EditorData, NodeGuid);
	if (!SearchResult.Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeId),
			EMCPErrorCode::NodeNotFound,
			TEXT("Node GUID didn't match any node in the StateTree (states, transitions, evaluators, global_tasks all searched). Use `statetree_state_list` / `statetree_read` to discover node GUIDs."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("location"), SearchResult.SlotType);

	if (SearchResult.OwningState)
	{
		Result->SetStringField(TEXT("state"), SearchResult.OwningState->Name.ToString());
	}

	if (SearchResult.Node->Node.IsValid())
	{
		Result->SetStringField(TEXT("node_type"), SearchResult.Node->Node.GetScriptStruct()->GetName());
	}

	Result->SetObjectField(TEXT("instance_data"), FStateTreeTypeCache::SerializeInstanceDataProperties(SearchResult.Node->Instance));

	return Result;
}
