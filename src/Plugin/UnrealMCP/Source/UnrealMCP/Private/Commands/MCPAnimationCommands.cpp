#include "Commands/MCPAnimationCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
// Animation graph
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNodeBinding.h"  // UAnimGraphNodeBinding (Internal/) — needed so the forward-declared pointer in AnimGraphNode_Base.h converts to UObject* for reflection
#include "AnimGraphNode_StateResult.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
// Animation assets
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceHelpers.h"          // UE::Anim::AnimationData::Trim (engine crop primitive)
#include "Animation/AnimData/IAnimationDataModel.h"  // GetNumberOfFrames/GetNumberOfKeys/GetFrameRate
#include "Misc/PackageName.h"                         // FPackageName::GetLongPackagePath (default dest folder)
#include "Misc/FrameRate.h"                           // FFrameRate / FFrameNumber for time→frame conversion
#include "Animation/Skeleton.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/AnimMontageFactory.h"

FMCPAnimationCommands::FMCPAnimationCommands()
{
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	// State machine
	if (CommandType == TEXT("anim_state_machine_create"))       return HandleCreateStateMachine(Params);
	if (CommandType == TEXT("anim_state_machine_state_add"))                   return HandleAddState(Params);
	if (CommandType == TEXT("add_conduit"))                 return HandleAddConduit(Params);
	if (CommandType == TEXT("anim_state_machine_transition_add"))              return HandleAddTransition(Params);
	if (CommandType == TEXT("anim_state_machine_set_entry"))             return HandleSetEntryState(Params);
	if (CommandType == TEXT("anim_state_machine_modify_transition"))           return HandleModifyTransition(Params);
	if (CommandType == TEXT("anim_state_machine_state_remove"))                return HandleRemoveState(Params);
	if (CommandType == TEXT("anim_state_machine_transition_remove"))           return HandleRemoveTransition(Params);
	// Skeletal control
	if (CommandType == TEXT("bp_set_inner_node_property"))     return HandleSetInnerNodeProperty(Params);
	// AnimGraph property binding (variable → node input pin)
	if (CommandType == TEXT("anim_node_bind_property"))     return HandleBindAnimNodeProperty(Params);
	// Notifies
	if (CommandType == TEXT("anim_list_notifies"))          return HandleListAnimNotifies(Params);
	if (CommandType == TEXT("anim_notify_add"))             return HandleAddAnimNotify(Params);
	if (CommandType == TEXT("anim_notify_remove"))          return HandleRemoveAnimNotify(Params);
	if (CommandType == TEXT("anim_extract_between_notifies")) return HandleExtractAnimBetweenNotifies(Params);
	// Montage
	if (CommandType == TEXT("anim_montage_create"))         return HandleCreateAnimMontage(Params);
	if (CommandType == TEXT("anim_montage_add_section"))         return HandleAddMontageSection(Params);
	if (CommandType == TEXT("anim_montage_set_section_link"))    return HandleSetMontageSectionLink(Params);
	if (CommandType == TEXT("anim_montage_set_blend"))           return HandleSetMontageBlend(Params);
	// ABP creation
	if (CommandType == TEXT("anim_blueprint_create"))       return HandleCreateAnimBlueprint(Params);
	if (CommandType == TEXT("anim_blueprint_set_skeleton")) return HandleSetAnimBlueprintSkeleton(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown animation command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: create_state_machine, add_state, add_conduit, add_transition, set_entry_state, modify_transition, remove_state, remove_transition, set_inner_node_property, bind_anim_node_property, list_anim_notifies, add_anim_notify, remove_anim_notify, extract_anim_between_notifies, create_anim_montage, add_montage_section, set_montage_section_link, set_montage_blend, create_anim_blueprint, set_anim_blueprint_skeleton."));
}

// ═══════════════════════════════════════════════════════════════════════
// Phase 4: State Machine Authoring
// ═══════════════════════════════════════════════════════════════════════

// Helper: find AnimBP + target graph
static UAnimGraphNode_StateMachine* FindStateMachineByGraph(UBlueprint* BP, const FString& GraphName)
{
	auto Search = [&](const TArray<UEdGraph*>& Graphs) -> UAnimGraphNode_StateMachine*
	{
		for (UEdGraph* G : Graphs)
		{
			for (UEdGraphNode* Node : G->Nodes)
			{
				UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node);
				if (SM && SM->EditorStateMachineGraph &&
					SM->EditorStateMachineGraph->GetName() == GraphName)
				{
					return SM;
				}
			}
		}
		return nullptr;
	};
	if (auto* Found = Search(BP->UbergraphPages)) return Found;
	return Search(BP->FunctionGraphs);
}

static UAnimStateNode* FindStateByName(UAnimationStateMachineGraph* SMGraph, const FString& StateName)
{
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
		if (StateNode && StateNode->GetStateName() == StateName)
		{
			return StateNode;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleCreateStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString MachineName;
	if (!Params->TryGetStringField(TEXT("machine_name"), MachineName))
		MachineName = TEXT("StateMachine");

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	// Find the AnimGraph
	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
		GraphName = TEXT("AnimGraph");

	UEdGraph* AnimGraph = FMCPCommonUtils::FindGraphByName(BP, GraphName);
	if (!AnimGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Graph not found: %s"), *GraphName),
			EMCPErrorCode::NodeNotFound,
			TEXT("`graph_name` did not match any UEdGraph on the Anim Blueprint. Defaults to `AnimGraph` if omitted. Use `list_blueprint_graphs` to enumerate. Names are case-sensitive."));

	// Create the state machine node
	UAnimGraphNode_StateMachine* SMNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
	if (!SMNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create StateMachine node"),
			EMCPErrorCode::Internal,
			TEXT("NewObject<UAnimGraphNode_StateMachine> returned nullptr. The AnimGraph module may not be loaded, or the parent graph is invalid. Verify the AnimGraph plugin is enabled and the editor is fully loaded. Retry once."));

	double PosX = 0, PosY = 0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);
	SMNode->NodePosX = (int32)PosX;
	SMNode->NodePosY = (int32)PosY;

	AnimGraph->AddNode(SMNode, true, false);
	SMNode->AllocateDefaultPins();
	SMNode->PostPlacedNewNode();

	// Name the machine
	if (SMNode->EditorStateMachineGraph)
	{
		SMNode->EditorStateMachineGraph->Rename(*MachineName);
	}

	AnimGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), SMNode->GetName());
	Result->SetStringField(TEXT("machine_name"), SMNode->EditorStateMachineGraph ? SMNode->EditorStateMachineGraph->GetName() : TEXT(""));
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleAddState(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString SMGraphName;
	if (!Params->TryGetStringField(TEXT("state_machine_graph"), SMGraphName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_machine_graph'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_machine_graph` is required (string, the name of an AnimationStateMachineGraph inside the Anim Blueprint). Use `list_blueprint_graphs` to enumerate — state machine graphs appear as sub-graphs of `UAnimGraphNode_StateMachine` nodes."));

	FString StateName;
	if (!Params->TryGetStringField(TEXT("state_name"), StateName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_name` is required (string, the UAnimStateNode's state name). State names are case-sensitive and match the visible state label. Use a read of the state machine graph to enumerate."));

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	UEdGraph* SMGraph = FMCPCommonUtils::FindGraphByName(BP, SMGraphName);
	if (!SMGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("State machine graph not found: %s"), *SMGraphName),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_machine_graph` did not match any UEdGraph on the Anim Blueprint. Names are case-sensitive. Use `list_blueprint_graphs` to enumerate; state machine graphs appear as sub-graphs of `UAnimGraphNode_StateMachine` nodes."));

	UAnimationStateMachineGraph* AnimSMGraph = Cast<UAnimationStateMachineGraph>(SMGraph);
	if (!AnimSMGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Graph is not a state machine graph"),
			EMCPErrorCode::UnsupportedClass,
			TEXT("The resolved graph is not a UAnimationStateMachineGraph — it's some other UEdGraph type (AnimGraph, EventGraph, function graph, etc.). This operation requires a state-machine sub-graph specifically. Use `list_blueprint_graphs` and pick a graph whose owning node is `UAnimGraphNode_StateMachine`."));

	// Create state node
	UAnimStateNode* StateNode = NewObject<UAnimStateNode>(AnimSMGraph);
	if (!StateNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create state node"),
			EMCPErrorCode::Internal,
			TEXT("NewObject<UAnimStateNode> returned nullptr. The AnimGraph plugin may not be loaded, or the parent UAnimationStateMachineGraph is invalid. Verify the plugin is enabled and retry."));

	double PosX = 0, PosY = 0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);
	StateNode->NodePosX = (int32)PosX;
	StateNode->NodePosY = (int32)PosY;

	AnimSMGraph->AddNode(StateNode, true, false);
	// Stamp a NodeGuid — raw NewObject leaves it invalid (all-zero), which makes
	// cooks non-deterministic ("missing NodeGuid ... will not cook deterministically
	// until resaved"). The editor's own state-node spawn path does the same call
	// here (FEdGraphSchemaAction_NewStateNode::PerformAction, after AddNode).
	StateNode->CreateNewGuid();
	StateNode->AllocateDefaultPins();
	StateNode->PostPlacedNewNode();

	// Set state name via the bound graph
	if (StateNode->BoundGraph)
	{
		StateNode->BoundGraph->Rename(*StateName);
	}

	AnimSMGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), StateNode->GetName());
	Result->SetStringField(TEXT("state_name"), StateNode->GetStateName());
	Result->SetStringField(TEXT("inner_graph"), StateNode->BoundGraph ? StateNode->BoundGraph->GetName() : TEXT(""));
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleAddConduit(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString SMGraphName;
	if (!Params->TryGetStringField(TEXT("state_machine_graph"), SMGraphName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_machine_graph'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_machine_graph` is required (string, the name of an AnimationStateMachineGraph inside the Anim Blueprint). Use `list_blueprint_graphs` to enumerate — state machine graphs appear as sub-graphs of `UAnimGraphNode_StateMachine` nodes."));

	FString ConduitName;
	if (!Params->TryGetStringField(TEXT("conduit_name"), ConduitName))
		ConduitName = TEXT("Conduit");

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	UEdGraph* SMGraph = FMCPCommonUtils::FindGraphByName(BP, SMGraphName);
	UAnimationStateMachineGraph* AnimSMGraph = Cast<UAnimationStateMachineGraph>(SMGraph);
	if (!AnimSMGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("State machine graph not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_machine_graph` did not match any state-machine sub-graph on the Anim Blueprint. Names are case-sensitive and match the UAnimationStateMachineGraph's name. Use `list_blueprint_graphs` to enumerate."));

	UAnimStateConduitNode* ConduitNode = NewObject<UAnimStateConduitNode>(AnimSMGraph);
	if (!ConduitNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create conduit node"),
			EMCPErrorCode::Internal,
			TEXT("NewObject<UAnimStateConduitNode> returned nullptr. The AnimGraph plugin may not be loaded, or the parent state machine graph is invalid. Verify the plugin is enabled and retry."));

	double PosX = 0, PosY = 0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);
	ConduitNode->NodePosX = (int32)PosX;
	ConduitNode->NodePosY = (int32)PosY;

	AnimSMGraph->AddNode(ConduitNode, true, false);
	// Stamp a NodeGuid (see HandleAddState) — deterministic-cook requirement.
	ConduitNode->CreateNewGuid();
	ConduitNode->AllocateDefaultPins();
	ConduitNode->PostPlacedNewNode();

	if (ConduitNode->BoundGraph)
	{
		ConduitNode->BoundGraph->Rename(*ConduitName);
	}

	AnimSMGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), ConduitNode->GetName());
	Result->SetStringField(TEXT("rule_graph"), ConduitNode->BoundGraph ? ConduitNode->BoundGraph->GetName() : TEXT(""));
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleAddTransition(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString SMGraphName;
	if (!Params->TryGetStringField(TEXT("state_machine_graph"), SMGraphName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_machine_graph'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_machine_graph` is required (string, the name of an AnimationStateMachineGraph inside the Anim Blueprint). Use `list_blueprint_graphs` to enumerate — state machine graphs appear as sub-graphs of `UAnimGraphNode_StateMachine` nodes."));

	FString FromState, ToState;
	if (!Params->TryGetStringField(TEXT("from_state"), FromState))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'from_state'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`from_state` is required (string, name of an existing UAnimStateNode in the state machine graph). Source state for a state-machine transition. Names are case-sensitive."));
	if (!Params->TryGetStringField(TEXT("to_state"), ToState))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'to_state'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`to_state` is required (string, name of an existing UAnimStateNode in the state machine graph). Target state for a state-machine transition. Names are case-sensitive."));

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	UEdGraph* SMGraph = FMCPCommonUtils::FindGraphByName(BP, SMGraphName);
	UAnimationStateMachineGraph* AnimSMGraph = Cast<UAnimationStateMachineGraph>(SMGraph);
	if (!AnimSMGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("State machine graph not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_machine_graph` did not match any state-machine sub-graph on the Anim Blueprint. Names are case-sensitive and match the UAnimationStateMachineGraph's name. Use `list_blueprint_graphs` to enumerate."));

	// Find source and target states
	UAnimStateNode* SourceState = FindStateByName(AnimSMGraph, FromState);
	UAnimStateNode* TargetState = FindStateByName(AnimSMGraph, ToState);

	if (!SourceState)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Source state not found: %s"), *FromState),
			EMCPErrorCode::NodeNotFound,
			TEXT("`from_state` did not match any UAnimStateNode in the state machine graph. State names are case-sensitive and match the visible state label. Use `analyze_blueprint_graph` on the state machine graph to enumerate states."));
	if (!TargetState)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Target state not found: %s"), *ToState),
			EMCPErrorCode::NodeNotFound,
			TEXT("`to_state` did not match any UAnimStateNode in the state machine graph. State names are case-sensitive. Use `analyze_blueprint_graph` on the state machine graph to enumerate."));

	// Create transition node
	UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(AnimSMGraph);
	if (!TransNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create transition node"),
			EMCPErrorCode::Internal,
			TEXT("NewObject<UAnimStateTransitionNode> returned nullptr. The AnimGraph plugin may not be loaded, or the parent state machine graph is invalid. Verify the plugin is enabled and retry."));

	// Position between states
	TransNode->NodePosX = (SourceState->NodePosX + TargetState->NodePosX) / 2;
	TransNode->NodePosY = (SourceState->NodePosY + TargetState->NodePosY) / 2;

	AnimSMGraph->AddNode(TransNode, true, false);
	// Stamp a NodeGuid (see HandleAddState) — deterministic-cook requirement.
	TransNode->CreateNewGuid();
	TransNode->AllocateDefaultPins();
	TransNode->PostPlacedNewNode();

	// Connect: source state output → transition input, transition output → target state input
	UEdGraphPin* SourceOut = SourceState->GetOutputPin();
	UEdGraphPin* TransIn = TransNode->GetInputPin();
	UEdGraphPin* TransOut = TransNode->GetOutputPin();
	UEdGraphPin* TargetIn = TargetState->GetInputPin();

	if (SourceOut && TransIn)
	{
		const UEdGraphSchema* Schema = AnimSMGraph->GetSchema();
		if (Schema)
		{
			Schema->TryCreateConnection(SourceOut, TransIn);
		}
	}
	if (TransOut && TargetIn)
	{
		const UEdGraphSchema* Schema = AnimSMGraph->GetSchema();
		if (Schema)
		{
			Schema->TryCreateConnection(TransOut, TargetIn);
		}
	}

	// Apply optional settings
	double BlendDuration = 0.0;
	if (Params->TryGetNumberField(TEXT("blend_duration"), BlendDuration))
	{
		TransNode->CrossfadeDuration = (float)BlendDuration;
	}

	double PriorityOrder = 0;
	if (Params->TryGetNumberField(TEXT("priority_order"), PriorityOrder))
	{
		TransNode->PriorityOrder = (int32)PriorityOrder;
	}

	bool bBidirectional = false;
	if (Params->TryGetBoolField(TEXT("bidirectional"), bBidirectional))
	{
		TransNode->Bidirectional = bBidirectional;
	}

	bool bAutoRule = false;
	if (Params->TryGetBoolField(TEXT("automatic_rule_based_on_sequence_player"), bAutoRule))
	{
		TransNode->bAutomaticRuleBasedOnSequencePlayerInState = bAutoRule;
	}

	AnimSMGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), TransNode->GetName());
	Result->SetStringField(TEXT("from_state"), FromState);
	Result->SetStringField(TEXT("to_state"), ToState);
	Result->SetStringField(TEXT("rule_graph"), TransNode->BoundGraph ? TransNode->BoundGraph->GetName() : TEXT(""));
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleSetEntryState(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString SMGraphName;
	if (!Params->TryGetStringField(TEXT("state_machine_graph"), SMGraphName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_machine_graph'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_machine_graph` is required (string, the name of an AnimationStateMachineGraph inside the Anim Blueprint). Use `list_blueprint_graphs` to enumerate — state machine graphs appear as sub-graphs of `UAnimGraphNode_StateMachine` nodes."));

	FString StateName;
	if (!Params->TryGetStringField(TEXT("state_name"), StateName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_name` is required (string, the UAnimStateNode's state name). State names are case-sensitive and match the visible state label. Use a read of the state machine graph to enumerate."));

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	UEdGraph* SMGraph = FMCPCommonUtils::FindGraphByName(BP, SMGraphName);
	UAnimationStateMachineGraph* AnimSMGraph = Cast<UAnimationStateMachineGraph>(SMGraph);
	if (!AnimSMGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("State machine graph not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_machine_graph` did not match any state-machine sub-graph on the Anim Blueprint. Names are case-sensitive and match the UAnimationStateMachineGraph's name. Use `list_blueprint_graphs` to enumerate."));

	UAnimStateNode* TargetState = FindStateByName(AnimSMGraph, StateName);
	if (!TargetState)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("State not found: %s"), *StateName),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_name` did not match any UAnimStateNode in the target state machine graph. State names are case-sensitive and match the visible state label. Use `analyze_blueprint_graph` on the state machine graph to enumerate states."));

	// Find entry node
	UAnimStateEntryNode* EntryNode = AnimSMGraph->EntryNode;
	if (!EntryNode)
	{
		for (UEdGraphNode* Node : AnimSMGraph->Nodes)
		{
			EntryNode = Cast<UAnimStateEntryNode>(Node);
			if (EntryNode) break;
		}
	}
	if (!EntryNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Entry node not found in state machine"),
			EMCPErrorCode::NodeNotFound,
			TEXT("Could not locate the UAnimStateEntryNode in the state machine graph. Every UAnimationStateMachineGraph should have one entry node by construction. This indicates the graph is malformed — re-create the state machine via `create_state_machine`."));

	// Break existing connections from entry (entry node has one output pin)
	UEdGraphPin* EntryOut = nullptr;
	for (UEdGraphPin* Pin : EntryNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output)
		{
			EntryOut = Pin;
			break;
		}
	}
	if (EntryOut)
	{
		EntryOut->BreakAllPinLinks();
	}

	// Connect entry to target state
	UEdGraphPin* StateIn = TargetState->GetInputPin();
	if (EntryOut && StateIn)
	{
		const UEdGraphSchema* Schema = AnimSMGraph->GetSchema();
		if (Schema)
		{
			Schema->TryCreateConnection(EntryOut, StateIn);
		}
	}

	AnimSMGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("entry_state"), StateName);
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleModifyTransition(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString SMGraphName;
	if (!Params->TryGetStringField(TEXT("state_machine_graph"), SMGraphName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_machine_graph'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_machine_graph` is required (string, the name of an AnimationStateMachineGraph inside the Anim Blueprint). Use `list_blueprint_graphs` to enumerate — state machine graphs appear as sub-graphs of `UAnimGraphNode_StateMachine` nodes."));

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	UEdGraph* SMGraph = FMCPCommonUtils::FindGraphByName(BP, SMGraphName);
	UAnimationStateMachineGraph* AnimSMGraph = Cast<UAnimationStateMachineGraph>(SMGraph);
	if (!AnimSMGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("State machine graph not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_machine_graph` did not match any state-machine sub-graph on the Anim Blueprint. Names are case-sensitive and match the UAnimationStateMachineGraph's name. Use `list_blueprint_graphs` to enumerate."));

	// Find transition by ID or from_state+to_state
	UAnimStateTransitionNode* TransNode = nullptr;
	FString TransId;
	if (Params->TryGetStringField(TEXT("transition_id"), TransId))
	{
		for (UEdGraphNode* Node : AnimSMGraph->Nodes)
		{
			if (Node->GetName() == TransId || Node->NodeGuid.ToString() == TransId)
			{
				TransNode = Cast<UAnimStateTransitionNode>(Node);
				break;
			}
		}
	}
	else
	{
		FString FromState, ToState;
		Params->TryGetStringField(TEXT("from_state"), FromState);
		Params->TryGetStringField(TEXT("to_state"), ToState);
		for (UEdGraphNode* Node : AnimSMGraph->Nodes)
		{
			UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node);
			if (T && T->GetPreviousState() && T->GetNextState() &&
				T->GetPreviousState()->GetStateName() == FromState &&
				T->GetNextState()->GetStateName() == ToState)
			{
				TransNode = T;
				break;
			}
		}
	}

	if (!TransNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Transition not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("Could not locate the UAnimStateTransitionNode connecting the named states. Verify both `from_state` and `to_state` exist (case-sensitive) and that an existing transition links them. A transition must exist from-state → to-state for the targeted state machine."));

	// Apply modifications
	double BlendDuration;
	if (Params->TryGetNumberField(TEXT("blend_duration"), BlendDuration))
	{
		TransNode->CrossfadeDuration = (float)BlendDuration;
	}

	double PriorityOrder;
	if (Params->TryGetNumberField(TEXT("priority_order"), PriorityOrder))
	{
		TransNode->PriorityOrder = (int32)PriorityOrder;
	}

	bool bBidirectional;
	if (Params->TryGetBoolField(TEXT("bidirectional"), bBidirectional))
	{
		TransNode->Bidirectional = bBidirectional;
	}

	bool bAutoRule;
	if (Params->TryGetBoolField(TEXT("automatic_rule_based_on_sequence_player"), bAutoRule))
	{
		TransNode->bAutomaticRuleBasedOnSequencePlayerInState = bAutoRule;
	}

	AnimSMGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), TransNode->GetName());
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleRemoveState(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString SMGraphName;
	if (!Params->TryGetStringField(TEXT("state_machine_graph"), SMGraphName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_machine_graph'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_machine_graph` is required (string, the name of an AnimationStateMachineGraph inside the Anim Blueprint). Use `list_blueprint_graphs` to enumerate — state machine graphs appear as sub-graphs of `UAnimGraphNode_StateMachine` nodes."));

	FString StateName;
	if (!Params->TryGetStringField(TEXT("state_name"), StateName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_name` is required (string, the UAnimStateNode's state name). State names are case-sensitive and match the visible state label. Use a read of the state machine graph to enumerate."));

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	UEdGraph* SMGraph = FMCPCommonUtils::FindGraphByName(BP, SMGraphName);
	UAnimationStateMachineGraph* AnimSMGraph = Cast<UAnimationStateMachineGraph>(SMGraph);
	if (!AnimSMGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("State machine graph not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_machine_graph` did not match any state-machine sub-graph on the Anim Blueprint. Names are case-sensitive and match the UAnimationStateMachineGraph's name. Use `list_blueprint_graphs` to enumerate."));

	UAnimStateNode* StateNode = FindStateByName(AnimSMGraph, StateName);
	if (!StateNode)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("State not found: %s"), *StateName),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_name` did not match any UAnimStateNode in the target state machine graph. State names are case-sensitive and match the visible state label. Use `analyze_blueprint_graph` on the state machine graph to enumerate states."));

	// Break all connections
	StateNode->BreakAllNodeLinks();
	AnimSMGraph->RemoveNode(StateNode);

	AnimSMGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("removed_state"), StateName);
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleRemoveTransition(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString SMGraphName;
	if (!Params->TryGetStringField(TEXT("state_machine_graph"), SMGraphName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state_machine_graph'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`state_machine_graph` is required (string, the name of an AnimationStateMachineGraph inside the Anim Blueprint). Use `list_blueprint_graphs` to enumerate — state machine graphs appear as sub-graphs of `UAnimGraphNode_StateMachine` nodes."));

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	UEdGraph* SMGraph = FMCPCommonUtils::FindGraphByName(BP, SMGraphName);
	UAnimationStateMachineGraph* AnimSMGraph = Cast<UAnimationStateMachineGraph>(SMGraph);
	if (!AnimSMGraph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("State machine graph not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("`state_machine_graph` did not match any state-machine sub-graph on the Anim Blueprint. Names are case-sensitive and match the UAnimationStateMachineGraph's name. Use `list_blueprint_graphs` to enumerate."));

	FString TransId;
	UAnimStateTransitionNode* TransNode = nullptr;

	if (Params->TryGetStringField(TEXT("transition_id"), TransId))
	{
		for (UEdGraphNode* Node : AnimSMGraph->Nodes)
		{
			if (Node->GetName() == TransId || Node->NodeGuid.ToString() == TransId)
			{
				TransNode = Cast<UAnimStateTransitionNode>(Node);
				break;
			}
		}
	}
	else
	{
		FString FromState, ToState;
		Params->TryGetStringField(TEXT("from_state"), FromState);
		Params->TryGetStringField(TEXT("to_state"), ToState);
		for (UEdGraphNode* Node : AnimSMGraph->Nodes)
		{
			UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node);
			if (T && T->GetPreviousState() && T->GetNextState() &&
				T->GetPreviousState()->GetStateName() == FromState &&
				T->GetNextState()->GetStateName() == ToState)
			{
				TransNode = T;
				break;
			}
		}
	}

	if (!TransNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Transition not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("Could not locate the UAnimStateTransitionNode connecting the named states. Verify both `from_state` and `to_state` exist (case-sensitive) and that an existing transition links them. A transition must exist from-state → to-state for the targeted state machine."));

	TransNode->BreakAllNodeLinks();
	AnimSMGraph->RemoveNode(TransNode);

	AnimSMGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ═══════════════════════════════════════════════════════════════════════
// Phase 6: Skeletal Control Inner Property Setter
// ═══════════════════════════════════════════════════════════════════════

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleSetInnerNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`node_id` is required (string, the UEdGraphNode's NodeGuid or short identifier). Use `analyze_blueprint_graph` to enumerate node IDs in the target graph."));

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property_name` is required (string, the FProperty name on the target AnimGraph node's class). Names are case-sensitive UPROPERTY identifiers. Use `get_class_properties` on the node's class to enumerate."));

	FString PropertyValue;
	if (!Params->TryGetStringField(TEXT("property_value"), PropertyValue))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_value'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property_value` is required (string, the text-form value passed to FProperty::ImportText_Direct). For structs use T3D-style literals (`(X=1,Y=2,Z=3)`); for enums pass the literal name; for object refs pass the asset path."));

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	// Find graph (search all)
	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UEdGraph* Graph = nullptr;
	if (!GraphName.IsEmpty())
	{
		Graph = FMCPCommonUtils::FindGraphByName(BP, GraphName);
	}
	else
	{
		// Search all graphs for the node
		auto SearchGraphs = [&](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* G : Graphs)
			{
				if (!G) continue;
				for (UEdGraphNode* N : G->Nodes)
				{
					if (N->GetName() == NodeId || N->NodeGuid.ToString() == NodeId)
						return G;
				}
			}
			return nullptr;
		};
		Graph = SearchGraphs(BP->UbergraphPages);
		if (!Graph) Graph = SearchGraphs(BP->FunctionGraphs);
	}
	if (!Graph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Graph not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("Could not locate the target UEdGraph on the Anim Blueprint. Use `list_blueprint_graphs` to enumerate available graphs (EventGraph, AnimGraph, function graphs)."));

	// Find node
	UEdGraphNode* TargetNode = nullptr;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (N->GetName() == NodeId || N->NodeGuid.ToString() == NodeId)
		{
			TargetNode = N;
			break;
		}
	}
	if (!TargetNode)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeId),
			EMCPErrorCode::NodeNotFound,
			TEXT("`node_id` did not match any UEdGraphNode in the Anim Blueprint's graphs. Use `analyze_blueprint_graph` to enumerate node IDs (NodeGuid) for a given graph."));

	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(TargetNode);
	if (!AnimNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Node is not an AnimGraph node"),
			EMCPErrorCode::UnsupportedClass,
			TEXT("The resolved node is not a UAnimGraphNode_Base subclass. This operation requires an animation graph node (UAnimGraphNode_StateMachine, UAnimGraphNode_BlendListByEnum, etc.). Use `analyze_blueprint_graph` to inspect node types."));

	// Access the inner FAnimNode struct via the "Node" FStructProperty
	FStructProperty* NodeProp = AnimNode->GetFNodeProperty();
	if (!NodeProp)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Cannot get inner FAnimNode property"),
			EMCPErrorCode::Internal,
			TEXT("The UAnimGraphNode_Base subclass does not expose a `Node` FStructProperty. This is unexpected — every UAnimGraphNode_* class should have a `Node` field of its corresponding FAnimNode_* struct type. Report as a bug; the node class is in the response context."));

	UScriptStruct* NodeStruct = NodeProp->Struct;
	void* NodeStructPtr = NodeProp->ContainerPtrToValuePtr<void>(AnimNode);
	if (!NodeStruct || !NodeStructPtr)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Cannot get inner FAnimNode instance"),
			EMCPErrorCode::Internal,
			TEXT("ContainerPtrToValuePtr returned null on the `Node` property — the FAnimNode_* struct instance is unreachable. This indicates the node is in a corrupted state. Re-add the node via the appropriate handler and retry."));

	// Find property on the struct
	FProperty* Prop = NodeStruct->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		// Try searching recursively in parent structs
		for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
		{
			if (It->GetName() == PropertyName)
			{
				Prop = *It;
				break;
			}
		}
	}

	if (!Prop)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Property '%s' not found on inner FAnimNode"), *PropertyName),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property_name` did not match any FProperty on the inner FAnimNode_* struct. Property names are case-sensitive UPROPERTY identifiers. Use `get_class_properties` on the FAnimNode struct (visible as the `Node` field's struct) to enumerate."));

	// Reject deprecated inner-struct UPROPERTYs. UE rebinds renamed properties via
	// CoreRedirects, so bare names like "Tag" can silently resolve to
	// `Tag_DEPRECATED` on FAnimNode_LinkedAnimGraph — writes succeed but the
	// runtime LinkAnimGraphByTag path reads the live UAnimGraphNode_Base::Tag
	// field instead, and the animation layer never links. Surface the redirect
	// to the outer-node property (settable via `set_node_property`).
	const bool bDeprecatedFlag = Prop->HasAnyPropertyFlags(CPF_Deprecated);
	const bool bDeprecatedName = Prop->GetName().EndsWith(TEXT("_DEPRECATED"));
	if (bDeprecatedFlag || bDeprecatedName)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Property '%s' resolved to deprecated inner field '%s' — refusing write"),
				*PropertyName, *Prop->GetName()),
			EMCPErrorCode::InvalidArgument,
			TEXT("The requested inner FAnimNode UPROPERTY is deprecated. Setting it succeeds but the runtime reads the live counterpart on the outer UAnimGraphNode_Base wrapper instead. Use `set_node_property` (outer node) with the same `property_name` (e.g. `Tag` on a LinkedAnimLayer node)."));
	}

	// Set via ImportText
	void* PropAddr = Prop->ContainerPtrToValuePtr<void>(NodeStructPtr);
	if (!Prop->ImportText_Direct(*PropertyValue, PropAddr, nullptr, PPF_None))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to set property '%s' to '%s'"), *PropertyName, *PropertyValue),
			EMCPErrorCode::InvalidArgument,
			TEXT("FProperty::ImportText_Direct returned nullptr for the supplied value text. For structs/objects pass the engine T3D-style literal (e.g. `(X=1,Y=2,Z=3)` for FVector); for enums pass the literal name; for object refs pass the asset path. Check the property's CPP type via `get_class_properties`."));
	}

	// Reconstruct to update pins
	AnimNode->ReconstructNode();

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("value"), PropertyValue);
	return Result;
}

// ═══════════════════════════════════════════════════════════════════════
// AnimGraph property binding (variable → node input pin)
// ═══════════════════════════════════════════════════════════════════════
//
// Binds an AnimGraph node's input property (e.g. FAnimNode_SequencePlayer's
// `Sequence`) to a Blueprint variable.  Mirrors the editor's "click the
// binding chip on the pin → pick variable" workflow, but headlessly.
//
// Why this exists:  property bindings live in a UAnimGraphNodeBinding
// sub-object on the AnimGraphNode (the concrete subclass UAnimGraphNodeBinding_Base
// stores them in a TMap<FName, FAnimGraphNodePropertyBinding>).  That sub-object's
// TMap can't be reached through `set_node_property` (outer-node UPROPERTY
// reflection doesn't see inside sub-objects) and the deprecated inner Tag /
// Sequence path doesn't establish a runtime binding — it just sets a literal
// value.  Without a real binding, the SequencePlayer can't track a property
// like ArmedIdleAnim3P that's written per-tick by the AnimInstance / equipment
// manager.
//
// Required params:
//   blueprint_name   — owning AnimBP path or name
//   node_id          — UAnimGraphNode_Base node name or NodeGuid string
//   property_name    — the target binding name on the node's FAnimNode struct
//                      (e.g. "Sequence" on FAnimNode_SequencePlayer)
//   variable_name    — the source AnimInstance variable to bind to
//
// Optional:
//   graph_name       — narrows the node search to one graph
TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleBindAnimNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_name` is required (string). Accepts a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). For Anim Blueprints, use `list_anim_blueprints` to discover."));

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`node_id` is required (string, the UEdGraphNode's NodeGuid or short identifier). Use `analyze_blueprint_graph` to enumerate node IDs in the target graph."));

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property_name` is required (string, the FProperty name on the target AnimGraph node's class). Names are case-sensitive UPROPERTY identifiers. Use `get_class_properties` on the node's class to enumerate."));

	FString VariableName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'variable_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`variable_name` is required (string, the name of an existing BP variable in the Anim Blueprint). Variable names are case-sensitive. Use `read_blueprint_content` to enumerate the Blueprint's variables."));

	UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
	if (!BP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_name` did not resolve to a Blueprint via FindBlueprintByName. Use `list_anim_blueprints` to discover. Names are case-sensitive."));

	// Find graph (optional graph_name; otherwise search all)
	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UEdGraph* Graph = nullptr;
	if (!GraphName.IsEmpty())
	{
		Graph = FMCPCommonUtils::FindGraphByName(BP, GraphName);
	}
	else
	{
		auto SearchGraphs = [&](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* G : Graphs)
			{
				if (!G) continue;
				for (UEdGraphNode* N : G->Nodes)
				{
					if (N->GetName() == NodeId || N->NodeGuid.ToString() == NodeId)
						return G;
				}
			}
			return nullptr;
		};
		Graph = SearchGraphs(BP->UbergraphPages);
		if (!Graph) Graph = SearchGraphs(BP->FunctionGraphs);
	}
	if (!Graph)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Graph not found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("Could not locate the target UEdGraph on the Anim Blueprint. Use `list_blueprint_graphs` to enumerate available graphs (EventGraph, AnimGraph, function graphs)."));

	UEdGraphNode* TargetNode = nullptr;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (N->GetName() == NodeId || N->NodeGuid.ToString() == NodeId)
		{
			TargetNode = N;
			break;
		}
	}
	if (!TargetNode)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeId),
			EMCPErrorCode::NodeNotFound,
			TEXT("`node_id` did not match any UEdGraphNode in the Anim Blueprint's graphs. Use `analyze_blueprint_graph` to enumerate node IDs (NodeGuid) for a given graph."));

	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(TargetNode);
	if (!AnimNode)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Node is not an AnimGraph node"),
			EMCPErrorCode::UnsupportedClass,
			TEXT("The resolved node is not a UAnimGraphNode_Base subclass. This operation requires an animation graph node (UAnimGraphNode_StateMachine, UAnimGraphNode_BlendListByEnum, etc.). Use `analyze_blueprint_graph` to inspect node types."));

	// The binding sub-object is created in PostPlacedNewNode for nodes added
	// via add_node, and re-ensured during PostLoad serialization migration for
	// older nodes.  EnsureBindingsArePresent is protected on UAnimGraphNode_Base,
	// so we can't call it from outside; rely on the standard lifecycle to have
	// already populated Binding.  If it's null here, the node hasn't been
	// fully initialized through any normal codepath — surface that as an error.
	UAnimGraphNodeBinding* BindingObj = AnimNode->GetMutableBinding();
	if (!BindingObj)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Node has no UAnimGraphNodeBinding sub-object — was it properly initialized via add_node / PostPlacedNewNode?"),
			EMCPErrorCode::Internal,
			TEXT("UAnimGraphNode_Base::GetMutableBinding returned null — the binding sub-object is created in PostPlacedNewNode for newly-added nodes and re-ensured during PostLoad for older nodes. Re-add the node via `add_node` to trigger the lifecycle, or open and re-save the Anim Blueprint in the editor to force a PostLoad pass."));

	// Locate the PropertyBindings TMap on the binding sub-object via reflection.
	// This sidesteps the Private/ header on UAnimGraphNodeBinding_Base — we
	// only need the field name, which is part of the UPROPERTY reflection data.
	FMapProperty* MapProp = FindFProperty<FMapProperty>(BindingObj->GetClass(), TEXT("PropertyBindings"));
	if (!MapProp)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Binding sub-object class '%s' has no 'PropertyBindings' TMap (unknown UAnimGraphNodeBinding subclass?)"), *BindingObj->GetClass()->GetName()),
			EMCPErrorCode::Internal,
			TEXT("The resolved UAnimGraphNodeBinding subclass doesn't expose the expected `PropertyBindings` FMapProperty via reflection. This bypass-the-private-header path requires UAnimGraphNodeBinding_Base — your engine version may have refactored the binding class. Report as a bug; the actual class name is in the response context."));

	FNameProperty* KeyProp = CastField<FNameProperty>(MapProp->KeyProp);
	FStructProperty* ValueProp = CastField<FStructProperty>(MapProp->ValueProp);
	if (!KeyProp || !ValueProp ||
		ValueProp->Struct->GetFName() != FName(TEXT("AnimGraphNodePropertyBinding")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("PropertyBindings TMap has unexpected key/value types — refusing to write blind."),
			EMCPErrorCode::Internal,
			TEXT("The `PropertyBindings` TMap's key/value FProperty types don't match the expected `TMap<FName, FAnimGraphNodePropertyBinding>` shape. Your engine version may have refactored the binding type. Refusing to write blind to avoid asset corruption — report as a bug."));
	}

	const FName BindingKey(*PropertyName);

	// Build the FAnimGraphNodePropertyBinding value.  Mirrors what the editor's
	// "Bind variable to pin" workflow constructs in
	// UAnimGraphNodeBinding_Base::MakePropertyBindingWidget (~line 486 in 5.7).
	FAnimGraphNodePropertyBinding NewBinding;
	NewBinding.PropertyName = BindingKey;
	NewBinding.PropertyPath.Add(VariableName);
	NewBinding.PathAsText  = FText::FromString(VariableName);
	NewBinding.Type        = EAnimGraphNodePropertyBindingType::Property;
	NewBinding.bIsBound    = true;

	// PinType / PromotedPinType get filled by UAnimGraphNodeBinding_Base::OnReconstructNode,
	// which iterates PropertyBindings and calls RecalculateBindingType for each entry.
	// ReconstructNode below triggers OnReconstructNode, so we don't need to call
	// the protected RecalculateBindingType ourselves — UE will recompute the
	// binding type once the entry exists in the map.

	// Insert (or replace) the entry in the TMap.
	BindingObj->Modify();
	void* MapAddr = MapProp->ContainerPtrToValuePtr<void>(BindingObj);
	FScriptMapHelper MapHelper(MapProp, MapAddr);

	const int32 ExistingIdx = MapHelper.FindMapIndexWithKey(&BindingKey);
	if (ExistingIdx != INDEX_NONE)
	{
		MapHelper.RemoveAt(ExistingIdx);
	}
	const int32 NewIdx = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
	*reinterpret_cast<FName*>(MapHelper.GetKeyPtr(NewIdx)) = BindingKey;
	*reinterpret_cast<FAnimGraphNodePropertyBinding*>(MapHelper.GetValuePtr(NewIdx)) = NewBinding;
	MapHelper.Rehash();

	// Toggle the optional pin to "shown" so the binding chip renders on the
	// pin itself.  ShowPinForProperties is auto-populated by the node from
	// its FAnimNode struct's UPROPERTYs at construction time, so the entry
	// for `property_name` should already exist on a freshly-added node.
	bool bToggledPin = false;
	for (int32 i = 0; i < AnimNode->ShowPinForProperties.Num(); ++i)
	{
		if (AnimNode->ShowPinForProperties[i].PropertyName == BindingKey)
		{
			AnimNode->ShowPinForProperties[i].bShowPin = true;
			bToggledPin = true;
			break;
		}
	}

	// ReconstructNode rebuilds the pin set + invokes UAnimGraphNodeBinding::OnReconstructNode,
	// which re-runs RecalculateBindingType for every binding (handles the case where
	// the source variable's type changed after we set the binding).
	AnimNode->ReconstructNode();

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("property_name"), PropertyName);
	Result->SetStringField(TEXT("variable_name"), VariableName);
	Result->SetBoolField(TEXT("pin_toggled_visible"), bToggledPin);
	return Result;
}

// ═══════════════════════════════════════════════════════════════════════
// Phase 7: Anim Notify Management
// ═══════════════════════════════════════════════════════════════════════

static UAnimSequenceBase* LoadAnimAsset(const FString& Path)
{
	UAnimSequenceBase* Asset = LoadObject<UAnimSequenceBase>(nullptr, *Path);
	return Asset;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleListAnimNotifies(const TSharedPtr<FJsonObject>& Params)
{
	FString AnimPath;
	if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'anim_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`anim_path` is required (string, full `/Game/...` asset path to a UAnimSequence). Use `list_anim_sequences` to discover."));

	UAnimSequenceBase* Anim = LoadAnimAsset(AnimPath);
	if (!Anim)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`anim_path` did not resolve to a UAnimSequenceBase via LoadObject. Verify with `list_anim_sequences` or `list_anim_montages`. Paths are case-sensitive."));

	TArray<TSharedPtr<FJsonValue>> NotifyArray;
	for (int32 i = 0; i < Anim->Notifies.Num(); ++i)
	{
		const FAnimNotifyEvent& Notify = Anim->Notifies[i];
		TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
		NObj->SetNumberField(TEXT("index"), i);
		NObj->SetNumberField(TEXT("trigger_time"), Notify.GetTriggerTime());
		NObj->SetNumberField(TEXT("duration"), Notify.GetDuration());
		NObj->SetStringField(TEXT("notify_name"), Notify.NotifyName.ToString());
		NObj->SetNumberField(TEXT("track_index"), Notify.TrackIndex);

		if (Notify.Notify)
		{
			NObj->SetStringField(TEXT("class"), Notify.Notify->GetClass()->GetName());
			NObj->SetBoolField(TEXT("is_state"), false);
		}
		else if (Notify.NotifyStateClass)
		{
			NObj->SetStringField(TEXT("class"), Notify.NotifyStateClass->GetClass()->GetName());
			NObj->SetBoolField(TEXT("is_state"), true);
		}

		NotifyArray.Add(MakeShared<FJsonValueObject>(NObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("anim_path"), AnimPath);
	Result->SetArrayField(TEXT("notifies"), NotifyArray);
	Result->SetNumberField(TEXT("count"), NotifyArray.Num());
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleAddAnimNotify(const TSharedPtr<FJsonObject>& Params)
{
	FString AnimPath;
	if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'anim_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`anim_path` is required (string, full `/Game/...` asset path to a UAnimSequence). Use `list_anim_sequences` to discover."));

	FString NotifyClassName;
	if (!Params->TryGetStringField(TEXT("notify_class"), NotifyClassName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'notify_class'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`notify_class` is required (string, full class path or short name of a UAnimNotify or UAnimNotifyState subclass). Accepts FQN path (`/Script/Engine.AnimNotify_PlaySound`), or BP path with `_C` suffix. Use `list_anim_notifies` to see currently-used notify types."));

	double TriggerTime = 0;
	if (!Params->TryGetNumberField(TEXT("trigger_time"), TriggerTime))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'trigger_time'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`trigger_time` is required (number, seconds from the start of the animation). Must be in [0, animation duration). Use `read_anim_montage` or anim sequence introspection to check duration."));

	UAnimSequenceBase* Anim = LoadAnimAsset(AnimPath);
	if (!Anim)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`anim_path` did not resolve to a UAnimSequenceBase via LoadObject. Verify with `list_anim_sequences` or `list_anim_montages`. Paths are case-sensitive."));

	// Resolve notify class
	UClass* NotifyClass = FindObject<UClass>(nullptr, *NotifyClassName);
	if (!NotifyClass)
	{
		// Try with various prefixes
		NotifyClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *NotifyClassName));
	}
	if (!NotifyClass)
	{
		NotifyClass = FindFirstObject<UClass>(*NotifyClassName, EFindFirstObjectOptions::None);
	}

	double TrackIndex = 0;
	Params->TryGetNumberField(TEXT("track_index"), TrackIndex);

	double Duration = 0;
	bool bHasDuration = Params->TryGetNumberField(TEXT("duration"), Duration);

	// dry_run: all validation runs above (anim load, notify class lookup,
	// trigger_time / track_index / duration parse). NewObject<UAnimNotify>
	// below would create a UObject inside the anim's package — that's the
	// side effect we skip. Diff shape per todo/13 phase 4: {notifies_added:
	// [{anim_path, notify_class, trigger_time, track_index, duration,
	// is_notify_state}]}.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		const bool bIsNotifyState = (NotifyClass && NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()));

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("anim_path"), AnimPath);
		Entry->SetStringField(TEXT("notify_class"), NotifyClassName);
		if (NotifyClass)
		{
			Entry->SetStringField(TEXT("resolved_class"), NotifyClass->GetPathName());
		}
		Entry->SetNumberField(TEXT("trigger_time"), TriggerTime);
		Entry->SetNumberField(TEXT("track_index"), TrackIndex);
		Entry->SetBoolField(TEXT("is_notify_state"), bIsNotifyState);
		if (bIsNotifyState && bHasDuration)
		{
			Entry->SetNumberField(TEXT("duration"), Duration);
		}
		Entry->SetNumberField(TEXT("would_be_index"), Anim->Notifies.Num());

		TArray<TSharedPtr<FJsonValue>> AddedArr;
		AddedArr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("notifies_added"), AddedArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Create notify event
	FAnimNotifyEvent NewNotify;
	NewNotify.NotifyName = FName(*NotifyClassName);
	NewNotify.SetTime((float)TriggerTime);
	NewNotify.TrackIndex = (int32)TrackIndex;

	if (NotifyClass)
	{
		if (NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()))
		{
			UAnimNotifyState* NotifyState = NewObject<UAnimNotifyState>(Anim, NotifyClass);
			NewNotify.NotifyStateClass = NotifyState;
			if (bHasDuration)
			{
				NewNotify.SetDuration((float)Duration);
			}
		}
		else if (NotifyClass->IsChildOf(UAnimNotify::StaticClass()))
		{
			UAnimNotify* Notify = NewObject<UAnimNotify>(Anim, NotifyClass);
			NewNotify.Notify = Notify;
		}
	}

	Anim->PreEditChange(nullptr);
	int32 NewIndex = Anim->Notifies.Add(NewNotify);
	Anim->PostEditChange();
	Anim->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("notify_index"), NewIndex);
	Result->SetStringField(TEXT("notify_class"), NotifyClassName);
	Result->SetNumberField(TEXT("trigger_time"), TriggerTime);
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleRemoveAnimNotify(const TSharedPtr<FJsonObject>& Params)
{
	FString AnimPath;
	if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'anim_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`anim_path` is required (string, full `/Game/...` asset path to a UAnimSequence). Use `list_anim_sequences` to discover."));

	double NotifyIndex = 0;
	if (!Params->TryGetNumberField(TEXT("notify_index"), NotifyIndex))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'notify_index'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`notify_index` is required (integer, zero-based within the animation's Notifies array). Use `list_anim_notifies` to enumerate notifies on the target animation — each entry's `index` is the value to pass."));

	UAnimSequenceBase* Anim = LoadAnimAsset(AnimPath);
	if (!Anim)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`anim_path` did not resolve to a UAnimSequenceBase via LoadObject. Verify with `list_anim_sequences` or `list_anim_montages`. Paths are case-sensitive."));

	int32 Idx = (int32)NotifyIndex;
	if (!Anim->Notifies.IsValidIndex(Idx))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid notify index: %d"), Idx),
			EMCPErrorCode::OutOfRange,
			TEXT("`notify_index` must be in [0, total_notifies). Use `list_anim_notifies` to see the current count. The bound shifts after each add/remove — re-read if you're chaining mutations."));

	const FAnimNotifyEvent& NotifyToRemove = Anim->Notifies[Idx];

	// dry_run: every preflight ran (asset load, index range check). Skip the
	// PreEditChange + RemoveAt + PostEditChange + MarkPackageDirty. Diff shape
	// per todo/13 phase 4: notifies_removed[] mirroring the per-notify entry
	// shape that list_anim_notifies emits (index, trigger_time, duration,
	// notify_name, track_index, class, is_state) so an
	// list_anim_notifies → remove_anim_notify dry-run round-trips at the
	// schema level.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("anim_path"), Anim->GetPathName());
		Entry->SetNumberField(TEXT("index"), Idx);
		Entry->SetNumberField(TEXT("trigger_time"), NotifyToRemove.GetTriggerTime());
		Entry->SetNumberField(TEXT("duration"), NotifyToRemove.GetDuration());
		Entry->SetStringField(TEXT("notify_name"), NotifyToRemove.NotifyName.ToString());
		Entry->SetNumberField(TEXT("track_index"), NotifyToRemove.TrackIndex);

		// Class disambiguation mirrors HandleListAnimNotifies (:989-998):
		// notify_class is a UAnimNotify-derived instance (point-in-time event);
		// NotifyStateClass is a UAnimNotifyState-derived instance (range event
		// with duration). Exactly one is non-null on a real notify entry.
		if (NotifyToRemove.Notify)
		{
			Entry->SetStringField(TEXT("class"), NotifyToRemove.Notify->GetClass()->GetName());
			Entry->SetBoolField(TEXT("is_state"), false);
		}
		else if (NotifyToRemove.NotifyStateClass)
		{
			Entry->SetStringField(TEXT("class"), NotifyToRemove.NotifyStateClass->GetClass()->GetName());
			Entry->SetBoolField(TEXT("is_state"), true);
		}

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("notifies_removed"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	Anim->PreEditChange(nullptr);
	Anim->Notifies.RemoveAt(Idx);
	Anim->PostEditChange();
	Anim->MarkPackageDirty();

	if (!UEditorAssetLibrary::SaveAsset(Anim->GetPathName(), /*bOnlyIfIsDirty=*/false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Notify removed in-memory but failed to persist to disk: %s"), *Anim->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("removed_index"), Idx);
	return Result;
}

// ═══════════════════════════════════════════════════════════════════════
// Slice an animation between two notifies (or two explicit times)
// ═══════════════════════════════════════════════════════════════════════
//
// Strategy: duplicate the source UAnimSequence, then crop the copy down to
// the [start, end] window using UE::Anim::AnimationData::Trim — the exact
// helper behind Persona's right-click → Crop (SAnimTimeline::OnCropAnimSequence).
// Trim rebuilds the bone tracks via SetBoneTrackKeys AND calls ResizeInFrames,
// which cascades to ResizeCurves — so the additive *transform curves* (the
// edits you bake with the viewport gizmo + SetKey/"S") are trimmed in range
// and preserved.  Two trims: tail first (so head indices stay valid), then
// head.  Boundary tags are stripped; other in-window notifies are rebased to
// the new t=0.
TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleExtractAnimBetweenNotifies(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'source_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`source_path` is required (string, full `/Game/...` path to the source UAnimSequence to slice). Use `list_anim_sequences` to discover."));

	FString DestName;
	if (!Params->TryGetStringField(TEXT("dest_name"), DestName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'dest_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`dest_name` is required (string, short asset name for the new clip — no path, no extension)."));

	UAnimSequence* Source = Cast<UAnimSequence>(LoadAnimAsset(SourcePath));
	if (!Source)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Source is not a UAnimSequence: %s"), *SourcePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`source_path` must resolve to a UAnimSequence via LoadObject. Montages/composites are not supported — only sequences carry the raw bone tracks + transform curves this slices. Verify with `list_anim_sequences`."));

	const float PlayLength = Source->GetPlayLength();

	// ── Resolve the [StartTime, EndTime] window ───────────────────────
	// Two ways in per boundary: a notify name (with optional `*_occurrence`
	// to disambiguate duplicate names) or an explicit time in seconds.  A
	// notify boundary also yields the array index of the tag, which we strip
	// from the output.
	float StartTime = 0.f, EndTime = PlayLength;
	int32 StartBoundaryIndex = INDEX_NONE, EndBoundaryIndex = INDEX_NONE;

	auto ResolveNotify = [&](const FString& NotifyName, int32 Occurrence, float& OutTime, int32& OutIndex) -> bool
	{
		int32 Seen = 0;
		for (int32 i = 0; i < Source->Notifies.Num(); ++i)
		{
			if (Source->Notifies[i].NotifyName.ToString() == NotifyName)
			{
				if (Seen == Occurrence)
				{
					OutTime = Source->Notifies[i].GetTriggerTime();
					OutIndex = i;
					return true;
				}
				++Seen;
			}
		}
		return false;
	};

	FString StartNotify, EndNotify;
	double StartOccurrenceD = 0, EndOccurrenceD = 0;
	Params->TryGetNumberField(TEXT("start_occurrence"), StartOccurrenceD);
	Params->TryGetNumberField(TEXT("end_occurrence"), EndOccurrenceD);

	if (Params->TryGetStringField(TEXT("start_notify"), StartNotify))
	{
		if (!ResolveNotify(StartNotify, (int32)StartOccurrenceD, StartTime, StartBoundaryIndex))
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Start notify not found: '%s' (occurrence %d)"), *StartNotify, (int32)StartOccurrenceD),
				EMCPErrorCode::InvalidArgument,
				TEXT("`start_notify` did not match a notify on the source (or `start_occurrence` exceeded the match count). Use `list_anim_notifies` for exact names and counts."));
	}
	else
	{
		double T = 0;
		if (!Params->TryGetNumberField(TEXT("start_time"), T))
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing start boundary"),
				EMCPErrorCode::InvalidArgument,
				TEXT("Provide either `start_notify` (notify name) or `start_time` (seconds) to mark the start of the window."));
		StartTime = (float)T;
	}

	if (Params->TryGetStringField(TEXT("end_notify"), EndNotify))
	{
		if (!ResolveNotify(EndNotify, (int32)EndOccurrenceD, EndTime, EndBoundaryIndex))
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("End notify not found: '%s' (occurrence %d)"), *EndNotify, (int32)EndOccurrenceD),
				EMCPErrorCode::InvalidArgument,
				TEXT("`end_notify` did not match a notify on the source (or `end_occurrence` exceeded the match count). Use `list_anim_notifies` for exact names."));
	}
	else
	{
		double T = 0;
		if (!Params->TryGetNumberField(TEXT("end_time"), T))
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing end boundary"),
				EMCPErrorCode::InvalidArgument,
				TEXT("Provide either `end_notify` (notify name) or `end_time` (seconds) to mark the end of the window."));
		EndTime = (float)T;
	}

	if (!(StartTime < EndTime))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Empty/inverted window: start=%.4f end=%.4f"), StartTime, EndTime),
			EMCPErrorCode::InvalidArgument,
			TEXT("Resolved start must be strictly less than end. If you passed notify names, the start tag fires at/after the end tag — swap them, or check trigger times via `list_anim_notifies`."));

	StartTime = FMath::Clamp(StartTime, 0.f, PlayLength);
	EndTime = FMath::Clamp(EndTime, 0.f, PlayLength);

	// ── Duplicate the source, then crop the copy ──────────────────────
	FString DestPath;
	if (!Params->TryGetStringField(TEXT("dest_path"), DestPath))
	{
		DestPath = FPackageName::GetLongPackagePath(Source->GetPackage()->GetName());
	}
	const FString DestObjectPath = DestPath / DestName;

	UAnimSequence* NewSeq = Cast<UAnimSequence>(UEditorAssetLibrary::DuplicateAsset(SourcePath, DestObjectPath));
	if (!NewSeq)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to duplicate to: %s"), *DestObjectPath),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::DuplicateAsset returned null. Common causes: destination already occupied, target folder read-only, or AssetRegistry mid-scan. Pick a clean `dest_path`/`dest_name` and retry."));

	// Snapshot the surviving notifies (rebased to the new t=0) BEFORE the
	// crop: Trim's length change auto-clamps the existing Notifies to the new
	// length without rebasing, so we capture now and overwrite the array
	// after.  Strip the two boundary tags; keep + shift the rest in-window.
	// Shift by GetTime() (not GetTriggerTime()) so any TriggerTimeOffset is
	// preserved exactly.
	TArray<FAnimNotifyEvent> RebuiltNotifies;
	for (int32 i = 0; i < NewSeq->Notifies.Num(); ++i)
	{
		if (i == StartBoundaryIndex || i == EndBoundaryIndex)
			continue; // strip the boundary tags
		const FAnimNotifyEvent& N = NewSeq->Notifies[i];
		const float Trigger = N.GetTriggerTime();
		if (Trigger >= StartTime && Trigger <= EndTime)
		{
			FAnimNotifyEvent Copy = N;
			Copy.SetTime(N.GetTime() - StartTime);
			RebuiltNotifies.Add(Copy);
		}
	}

	// Convert the time window to data-model frame indices (the space Trim
	// operates in).  Capture counts before trimming.
	const IAnimationDataModel* Model = NewSeq->GetDataModel();
	if (!Model)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Duplicated sequence has no animation data model: %s"), *NewSeq->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UAnimSequence::GetDataModel returned null on the freshly-duplicated clip — the bone-track/curve model is unreachable, so the crop window cannot be computed. The source sequence may be a legacy/corrupt asset; re-import or re-save it in the editor and retry."));
	const FFrameRate FrameRate = Model->GetFrameRate();
	const int32 NumFrames = Model->GetNumberOfFrames();
	const int32 NumKeys = Model->GetNumberOfKeys(); // == NumFrames + 1

	const int32 FrameAVal = FMath::Clamp(FrameRate.AsFrameTime(StartTime).RoundToFrame().Value, 0, NumFrames);
	const int32 FrameBVal = FMath::Clamp(FrameRate.AsFrameTime(EndTime).RoundToFrame().Value, FrameAVal, NumFrames);

	// Tail: remove (FrameB+1) .. last sampled key.
	if (FrameBVal + 1 < NumKeys)
	{
		UE::Anim::AnimationData::Trim(NewSeq,
			TRange<FFrameNumber>(
				TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(FrameBVal + 1)),
				TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(NumKeys))));
	}

	// Head: remove 0 .. (FrameA-1).  FrameA is unaffected by the tail removal.
	if (FrameAVal > 0)
	{
		UE::Anim::AnimationData::Trim(NewSeq,
			TRange<FFrameNumber>(
				TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(0)),
				TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(FrameAVal))));
	}

	// Commit the rebased / boundary-stripped notify set.
	NewSeq->PreEditChange(nullptr);
	NewSeq->Notifies = RebuiltNotifies;
	NewSeq->PostEditChange();
	NewSeq->MarkPackageDirty();

	// Persist (auto-save universal contract, mirrors HandleCreateAnimMontage).
	if (!UEditorAssetLibrary::SaveAsset(NewSeq->GetPathName(), /*bOnlyIfIsDirty=*/false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Sliced sequence was created in-memory but failed to save to disk: %s"), *NewSeq->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset returns false while a PIE session is active (saves are blocked during play) or when the destination package is read-only / checked out by another writer. Stop PIE, ensure the target folder is writable, and retry; the asset exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), NewSeq->GetPathName());
	Result->SetStringField(TEXT("name"), NewSeq->GetName());
	Result->SetNumberField(TEXT("source_start_time"), StartTime);
	Result->SetNumberField(TEXT("source_end_time"), EndTime);
	Result->SetNumberField(TEXT("duration"), NewSeq->GetPlayLength());
	Result->SetNumberField(TEXT("num_frames"), NewSeq->GetDataModel()->GetNumberOfFrames());
	Result->SetNumberField(TEXT("notifies_kept"), RebuiltNotifies.Num());
	return Result;
}

// ═══════════════════════════════════════════════════════════════════════
// Phase 8: AnimMontage Creation
// ═══════════════════════════════════════════════════════════════════════

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleCreateAnimMontage(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`name` is required (string). The semantics depend on the handler — typically the new asset's short name or the section/notify FName being added."));

	FString SkeletonPath;
	if (!Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'skeleton_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`skeleton_path` is required (string, full `/Game/...` asset path to a USkeleton). Use `list_skeletons` to discover."));

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`skeleton_path` did not resolve to a USkeleton via LoadObject. Verify with `list_skeletons`. Paths are case-sensitive."));

	FString PackagePath = TEXT("/Game/Montages");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);

	FString SlotName = TEXT("DefaultSlot");
	Params->TryGetStringField(TEXT("slot_name"), SlotName);

	// Create montage via factory
	UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
	Factory->TargetSkeleton = Skeleton;

	// Load source sequence if provided
	FString SourceSeqPath;
	if (Params->TryGetStringField(TEXT("source_sequence"), SourceSeqPath))
	{
		UAnimSequence* SourceSeq = LoadObject<UAnimSequence>(nullptr, *SourceSeqPath);
		if (SourceSeq)
		{
			Factory->SourceAnimation = SourceSeq;
		}
	}

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, PackagePath, UAnimMontage::StaticClass(), Factory);
	UAnimMontage* Montage = Cast<UAnimMontage>(NewAsset);
	if (!Montage)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create AnimMontage"),
			EMCPErrorCode::Internal,
			TEXT("AssetTools.CreateAsset returned nullptr for UAnimMontage creation via UAnimMontageFactory. Common causes: target path is read-only or already occupied; AssetRegistry mid-scan; the source AnimSequence's skeleton isn't compatible. Pick a clean destination path and retry."));

	// Set slot
	if (Montage->SlotAnimTracks.Num() > 0)
	{
		Montage->SlotAnimTracks[0].SlotName = FName(*SlotName);
	}

	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	// Auto-save per IMPROVEMENT_PHILOSOPHY "auto-save on every mutator" universal
	// contract. Matches the AssetFactoryCommands precedent — every asset-creation
	// path follows MarkPackageDirty + SaveAsset(bOnlyIfIsDirty=false). Pre-fix,
	// create_anim_montage left the asset in-memory only; agent had to call
	// save_asset explicitly, breaking the universal-contract claim cited in
	// 0_DEFERRED.md (the reason `save_all_dirty` isn't shipped).
	if (!UEditorAssetLibrary::SaveAsset(Montage->GetPathName(), /*bOnlyIfIsDirty=*/false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Montage was created in-memory but failed to save to disk: %s"), *Montage->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset returns false while a PIE session is active or when the destination package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the montage exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), Montage->GetPathName());
	Result->SetStringField(TEXT("name"), Montage->GetName());
	Result->SetNumberField(TEXT("duration"), Montage->GetPlayLength());
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleAddMontageSection(const TSharedPtr<FJsonObject>& Params)
{
	FString MontagePath;
	if (!Params->TryGetStringField(TEXT("montage_path"), MontagePath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'montage_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`montage_path` is required (string, full `/Game/...` asset path to a UAnimMontage). Use `list_anim_montages` to discover."));

	FString SectionName;
	if (!Params->TryGetStringField(TEXT("section_name"), SectionName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'section_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`section_name` is required (string, the FName of a section on the UAnimMontage). Section names are case-sensitive. Use `read_anim_montage` to enumerate the montage's sections."));

	double StartTime = 0;
	Params->TryGetNumberField(TEXT("start_time"), StartTime);

	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *MontagePath);
	if (!Montage)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Montage not found: %s"), *MontagePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`montage_path` did not resolve to a UAnimMontage via LoadObject. Verify with `list_anim_montages`. Paths are case-sensitive."));

	// Add composite section
	Montage->PreEditChange(nullptr);
	int32 NewIdx = Montage->AddAnimCompositeSection(FName(*SectionName), (float)StartTime);
	Montage->PostEditChange();
	Montage->MarkPackageDirty();

	if (!UEditorAssetLibrary::SaveAsset(Montage->GetPathName(), /*bOnlyIfIsDirty=*/false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Montage section mutated in-memory but failed to persist to disk: %s"), *Montage->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("section_index"), NewIdx);
	Result->SetStringField(TEXT("section_name"), SectionName);
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleSetMontageSectionLink(const TSharedPtr<FJsonObject>& Params)
{
	FString MontagePath;
	if (!Params->TryGetStringField(TEXT("montage_path"), MontagePath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'montage_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`montage_path` is required (string, full `/Game/...` asset path to a UAnimMontage). Use `list_anim_montages` to discover."));

	FString SectionName;
	if (!Params->TryGetStringField(TEXT("section_name"), SectionName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'section_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`section_name` is required (string, the FName of a section on the UAnimMontage). Section names are case-sensitive. Use `read_anim_montage` to enumerate the montage's sections."));

	FString NextSectionName;
	if (!Params->TryGetStringField(TEXT("next_section_name"), NextSectionName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'next_section_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`next_section_name` is required (string, name of an existing section to link to). Empty string is NOT accepted here — pass the empty target via a sentinel like `\"\"` is not supported; to clear a section link, use a dedicated `clear_montage_section_link` (if available)."));

	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *MontagePath);
	if (!Montage)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Montage not found: %s"), *MontagePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`montage_path` did not resolve to a UAnimMontage via LoadObject. Verify with `list_anim_montages`. Paths are case-sensitive."));

	// Find section index
	int32 SectionIdx = Montage->GetSectionIndex(FName(*SectionName));
	int32 NextIdx = Montage->GetSectionIndex(FName(*NextSectionName));

	if (SectionIdx == INDEX_NONE)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Section not found: %s"), *SectionName),
			EMCPErrorCode::NodeNotFound,
			TEXT("`section_name` did not match any section on the UAnimMontage. Section names are case-sensitive. Use `read_anim_montage` to enumerate the montage's sections."));
	if (NextIdx == INDEX_NONE)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Next section not found: %s"), *NextSectionName),
			EMCPErrorCode::NodeNotFound,
			TEXT("`next_section_name` did not match any section on the UAnimMontage. Section names are case-sensitive. Use `read_anim_montage` to enumerate."));

	Montage->PreEditChange(nullptr);
	Montage->CompositeSections[SectionIdx].NextSectionName = FName(*NextSectionName);
	Montage->PostEditChange();
	Montage->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("section"), SectionName);
	Result->SetStringField(TEXT("next_section"), NextSectionName);
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleSetMontageBlend(const TSharedPtr<FJsonObject>& Params)
{
	FString MontagePath;
	if (!Params->TryGetStringField(TEXT("montage_path"), MontagePath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'montage_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`montage_path` is required (string, full `/Game/...` asset path to a UAnimMontage). Use `list_anim_montages` to discover."));

	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *MontagePath);
	if (!Montage)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Montage not found: %s"), *MontagePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`montage_path` did not resolve to a UAnimMontage via LoadObject. Verify with `list_anim_montages`. Paths are case-sensitive."));

	Montage->PreEditChange(nullptr);

	double BlendInTime;
	if (Params->TryGetNumberField(TEXT("blend_in_time"), BlendInTime))
	{
		Montage->BlendIn.SetBlendTime((float)BlendInTime);
	}

	double BlendOutTime;
	if (Params->TryGetNumberField(TEXT("blend_out_time"), BlendOutTime))
	{
		Montage->BlendOut.SetBlendTime((float)BlendOutTime);
	}

	double BlendOutTriggerTime;
	if (Params->TryGetNumberField(TEXT("blend_out_trigger_time"), BlendOutTriggerTime))
	{
		Montage->BlendOutTriggerTime = (float)BlendOutTriggerTime;
	}

	Montage->PostEditChange();
	Montage->MarkPackageDirty();

	if (!UEditorAssetLibrary::SaveAsset(Montage->GetPathName(), /*bOnlyIfIsDirty=*/false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Montage blend mutated in-memory but failed to persist to disk: %s"), *Montage->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), MontagePath);
	return Result;
}

// ═══════════════════════════════════════════════════════════════════════
// Phase 9: AnimBP Creation
// ═══════════════════════════════════════════════════════════════════════

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleCreateAnimBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`name` is required (string). The semantics depend on the handler — typically the new asset's short name or the section/notify FName being added."));

	FString SkeletonPath;
	if (!Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'skeleton_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`skeleton_path` is required (string, full `/Game/...` asset path to a USkeleton). Use `list_skeletons` to discover."));

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`skeleton_path` did not resolve to a USkeleton via LoadObject. Verify with `list_skeletons`. Paths are case-sensitive."));

	FString PackagePath = TEXT("/Game/Animation");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);

	FString ParentClassPath;
	UClass* ParentClass = UAnimInstance::StaticClass();
	if (Params->TryGetStringField(TEXT("parent_class"), ParentClassPath))
	{
		UClass* CustomParent = LoadObject<UClass>(nullptr, *ParentClassPath);
		if (CustomParent && CustomParent->IsChildOf(UAnimInstance::StaticClass()))
		{
			ParentClass = CustomParent;
		}
	}

	// Create AnimBlueprint via factory
	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->TargetSkeleton = Skeleton;
	Factory->ParentClass = ParentClass;

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, PackagePath, UAnimBlueprint::StaticClass(), Factory);
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(NewAsset);
	if (!AnimBP)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create AnimBlueprint"),
			EMCPErrorCode::Internal,
			TEXT("AssetTools.CreateAsset returned nullptr for UAnimBlueprint creation via UAnimBlueprintFactory. Common causes: target path is read-only or already occupied; AssetRegistry mid-scan; the target USkeleton is incompatible. Pick a clean destination path and retry."));

	// Find the AnimGraph and output pose node
	FString AnimGraphName;
	FString OutputNodeId;
	for (UEdGraph* Graph : AnimBP->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Contains(TEXT("AnimGraph")))
		{
			AnimGraphName = Graph->GetName();
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node->GetClass()->GetName().Contains(TEXT("Root")))
				{
					OutputNodeId = Node->GetName();
					break;
				}
			}
			break;
		}
	}

	// Auto-save per IMPROVEMENT_PHILOSOPHY "auto-save on every mutator" universal
	// contract. Matches the AssetFactoryCommands precedent. UAnimBlueprintFactory
	// runs initial compile inside FactoryCreateNew, so the BP is in a saveable
	// state immediately after AssetTools.CreateAsset returns. Pre-fix,
	// create_anim_blueprint left the BP in-memory only — agent had to call
	// save_asset explicitly, breaking the universal-contract claim.
	AnimBP->PostEditChange();
	AnimBP->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveAsset(AnimBP->GetPathName(), /*bOnlyIfIsDirty=*/false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Anim Blueprint was created in-memory but failed to save to disk: %s"), *AnimBP->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset returns false while a PIE session is active or when the destination package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the Blueprint exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), AnimBP->GetPathName());
	Result->SetStringField(TEXT("name"), AnimBP->GetName());
	Result->SetStringField(TEXT("anim_graph"), AnimGraphName);
	Result->SetStringField(TEXT("output_pose_node_id"), OutputNodeId);
	Result->SetStringField(TEXT("skeleton"), SkeletonPath);
	return Result;
}

TSharedPtr<FJsonObject> FMCPAnimationCommands::HandleSetAnimBlueprintSkeleton(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintPath;
	if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`blueprint_path` is required (string, full `/Game/...` asset path to an Anim Blueprint). Use `list_anim_blueprints` to discover."));

	FString SkeletonPath;
	if (!Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'skeleton_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`skeleton_path` is required (string, full `/Game/...` asset path to a USkeleton). Use `list_skeletons` to discover."));

	UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *BlueprintPath);
	if (!AnimBP)
	{
		// Try finding by name
		UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintPath);
		AnimBP = Cast<UAnimBlueprint>(BP);
	}
	if (!AnimBP)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("AnimBlueprint not found: %s"), *BlueprintPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`blueprint_path` did not resolve to a UAnimBlueprint via LoadObject. Verify with `list_anim_blueprints`. Paths are case-sensitive; the asset must be a UAnimBlueprint (not a regular UBlueprint)."));

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`skeleton_path` did not resolve to a USkeleton via LoadObject. Verify with `list_skeletons`. Paths are case-sensitive."));

	AnimBP->TargetSkeleton = Skeleton;
	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	FKismetEditorUtilities::CompileBlueprint(AnimBP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintPath);
	Result->SetStringField(TEXT("skeleton"), SkeletonPath);
	return Result;
}
