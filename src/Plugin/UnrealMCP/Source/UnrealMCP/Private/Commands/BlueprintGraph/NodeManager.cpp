#include "Commands/BlueprintGraph/NodeManager.h"
#include "Commands/MCPCommonUtils.h"
#include "Commands/BlueprintGraph/Nodes/ControlFlowNodes.h"
#include "Commands/BlueprintGraph/Nodes/DataNodes.h"
#include "Commands/BlueprintGraph/Nodes/UtilityNodes.h"
#include "Commands/BlueprintGraph/Nodes/CastingNodes.h"
#include "Commands/BlueprintGraph/Nodes/AnimationNodes.h"
#include "Commands/BlueprintGraph/Nodes/SpecializedNodes.h"
#include "Engine/Blueprint.h"
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_RotationOffsetBlendSpace.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_ApplyAdditive.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "AnimGraphNode_LocalToComponentSpace.h"
// Phase 3 Tier A — project-critical node types
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Inertialization.h"
#include "AnimGraphNode_BlendListByEnum.h"
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_CopyPoseFromMesh.h"
// Phase 3 Tier B — common animation nodes
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimGraphNode_BlendSpaceEvaluator.h"
#include "AnimGraphNode_ApplyMeshSpaceAdditive.h"
#include "AnimGraphNode_MakeDynamicAdditive.h"
#include "AnimGraphNode_LocalRefPose.h"
#include "AnimGraphNode_MeshRefPose.h"
#include "AnimGraphNode_ModifyCurve.h"
#include "AnimGraphNode_RandomPlayer.h"
#include "AnimGraphNode_DeadBlending.h"
// Phase 3 Tier C — IK and skeletal control
#include "AnimGraphNode_CCDIK.h"
#include "AnimGraphNode_Fabrik.h"
#include "AnimGraphNode_LookAt.h"
#include "AnimGraphNode_SpringBone.h"
#include "AnimGraphNode_CopyBone.h"
#include "AnimGraphNode_BoneDrivenController.h"
#include "AnimGraphNode_AnimDynamics.h"
#include "AnimGraphNode_LegIK.h"
#include "AnimGraphNode_SplineIK.h"
#include "AnimGraphNode_HandIKRetargeting.h"
// Phase 3 Tier D — advanced/niche
#include "AnimGraphNode_Mirror.h"
#include "AnimGraphNode_RotateRootBone.h"
#include "AnimGraphNode_CurveSource.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionPoseEvaluator.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimLayerInterface.h"
#include "Animation/BlendSpace.h"
#include "Animation/AimOffsetBlendSpace.h"
// Generic AnimGraph fallback resolver (see CreateAnimGraphNodeByClass below)
#include "AnimGraphNode_Base.h"
#include "UObject/UObjectIterator.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "EditorAssetLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"

// ── AnimGraph node creation helper ─────────────────────────────────────
// Creates any UAnimGraphNode_* subclass, sets position.
template<typename T>
T* FBlueprintNodeManager::CreateAnimGraphNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid()) return nullptr;

	// AnimGraph nodes assert in PostPlacedNewNode (CastChecked<UAnimBlueprint> via
	// RequestExtensionsForNode -> GetAnimBlueprint) when their owning Blueprint is not
	// an AnimBlueprint. Refuse instead of hard-crashing the editor.
	if (!Cast<UAnimBlueprint>(FBlueprintEditorUtils::FindBlueprintForGraph(Graph))) return nullptr;

	T* Node = NewObject<T>(Graph);
	if (!Node) return nullptr;

	double PosX = 0, PosY = 0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);
	Node->NodePosX = (int32)PosX;
	Node->NodePosY = (int32)PosY;

	Graph->AddNode(Node, true, false);
	Node->AllocateDefaultPins();
	Node->PostPlacedNewNode();

	return Node;
}

// Set animation asset on nodes that support it (SequencePlayer, BlendSpacePlayer, AimOffset)
static void SetAnimAssetFromParams(UAnimGraphNode_AssetPlayerBase* Node, const TSharedPtr<FJsonObject>& Params)
{
	FString AnimAssetPath;
	if (Params->TryGetStringField(TEXT("anim_asset"), AnimAssetPath))
	{
		UAnimationAsset* Asset = LoadObject<UAnimationAsset>(nullptr, *AnimAssetPath);
		if (Asset)
		{
			Node->SetAnimationAsset(Asset);
			Node->ReconstructNode();
		}
	}
}

// Generic AnimGraph node class resolver — handles any UAnimGraphNode_Base subclass by name.
// Catches plugin AnimGraph nodes (DragonIK, ALSv4, etc.) that aren't in the explicit registry.
// Accepts three input forms:
//   1. Fully-qualified path: "/Script/DragonIKPluginEditor.AnimGraphNode_DragonFeetSolver"
//   2. Full class name:      "AnimGraphNode_DragonFeetSolver"
//   3. Short name:           "DragonFeetSolver"  (prefix auto-added)
static UClass* ResolveAnimGraphNodeClass(const FString& NodeType)
{
	if (NodeType.IsEmpty()) return nullptr;

	// Form 1: /Script/Module.ClassName — direct LoadClass
	if (NodeType.StartsWith(TEXT("/Script/")))
	{
		if (UClass* Cls = LoadClass<UAnimGraphNode_Base>(nullptr, *NodeType))
		{
			return Cls;
		}
		// Try plain LoadObject<UClass> as well (handles non-native cases)
		if (UClass* Cls = LoadObject<UClass>(nullptr, *NodeType))
		{
			if (Cls->IsChildOf(UAnimGraphNode_Base::StaticClass()))
			{
				return Cls;
			}
		}
		return nullptr;
	}

	// Friendly aliases used by the add_node dispatch whose advertised name differs from
	// the underlying UAnimGraphNode_* class basename. Map each to its canonical class name
	// so this resolver (and therefore the AddNode auto-route predicate that calls it) treats
	// them as AnimGraph node types. WITHOUT this, an anim node added by one of these aliases
	// with no explicit function_name was misrouted into the EventGraph — see docs/bugs/mcp.md
	// "bp_add_node silently places AnimGraph node classes in the EventGraph".
	FString CanonicalName = NodeType;
	if      (NodeType.Equals(TEXT("AimOffset"), ESearchCase::IgnoreCase))           CanonicalName = TEXT("AnimGraphNode_RotationOffsetBlendSpace");
	else if (NodeType.Equals(TEXT("AnimSlot"), ESearchCase::IgnoreCase))            CanonicalName = TEXT("AnimGraphNode_Slot");
	else if (NodeType.Equals(TEXT("AnimBlendListByBool"), ESearchCase::IgnoreCase)) CanonicalName = TEXT("AnimGraphNode_BlendListByBool");
	else if (NodeType.Equals(TEXT("ComponentToLocal"), ESearchCase::IgnoreCase))    CanonicalName = TEXT("AnimGraphNode_ComponentToLocalSpace");
	else if (NodeType.Equals(TEXT("LocalToComponent"), ESearchCase::IgnoreCase))    CanonicalName = TEXT("AnimGraphNode_LocalToComponentSpace");

	// Form 2/3: iterate all registered UClass and match on GetName()
	// Try both the raw name and the AnimGraphNode_-prefixed variant.
	const FString WithPrefix = CanonicalName.StartsWith(TEXT("AnimGraphNode_"))
		? CanonicalName
		: (FString(TEXT("AnimGraphNode_")) + CanonicalName);

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (!Cls || !Cls->IsChildOf(UAnimGraphNode_Base::StaticClass()))
		{
			continue;
		}
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}
		const FString ClassName = Cls->GetName();
		if (ClassName.Equals(CanonicalName, ESearchCase::IgnoreCase) ||
		    ClassName.Equals(WithPrefix, ESearchCase::IgnoreCase))
		{
			return Cls;
		}
	}

	return nullptr;
}

// Creates any UAnimGraphNode_Base subclass at the given position in the graph.
// Runtime analogue of the CreateAnimGraphNode<T> template — used by the generic fallback path.
static UAnimGraphNode_Base* CreateAnimGraphNodeByClass(UEdGraph* Graph, UClass* NodeClass, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !NodeClass) return nullptr;
	if (!NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass())) return nullptr;
	if (NodeClass->HasAnyClassFlags(CLASS_Abstract)) return nullptr;
	// AnimGraph nodes assert in PostPlacedNewNode (CastChecked<UAnimBlueprint>) when their
	// owning Blueprint is not an AnimBlueprint -- refuse rather than crash the editor.
	if (!Cast<UAnimBlueprint>(FBlueprintEditorUtils::FindBlueprintForGraph(Graph))) return nullptr;

	UAnimGraphNode_Base* Node = NewObject<UAnimGraphNode_Base>(Graph, NodeClass);
	if (!Node) return nullptr;

	double PosX = 0, PosY = 0;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("pos_x"), PosX);
		Params->TryGetNumberField(TEXT("pos_y"), PosY);
	}
	Node->NodePosX = (int32)PosX;
	Node->NodePosY = (int32)PosY;

	Graph->AddNode(Node, true, false);
	Node->AllocateDefaultPins();
	Node->PostPlacedNewNode();

	return Node;
}

TSharedPtr<FJsonObject> FBlueprintNodeManager::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters. Doc 1 migration: every error path emits structured
	// error_code + actionable error_hint so agents can branch on the failure
	// shape without parsing prose.
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with at minimum `blueprint_name` and `node_type` fields. Optional `node_params` carries node-type-specific options (target_function, variable_name, anim_asset, etc.)."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint whose graph should receive the new node."));
	}

	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_type' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_type` (string) — e.g. \"Branch\", \"CallFunction\", \"VariableGet\", \"SequencePlayer\". Use bp_inspect on a similar BP for examples."));
	}

	// Get optional node parameters
	const TSharedPtr<FJsonObject>* NodeParamsPtr;
	TSharedPtr<FJsonObject> NodeParams;
	if (Params->TryGetObjectField(TEXT("node_params"), NodeParamsPtr))
	{
		NodeParams = *NodeParamsPtr;
	}
	else
	{
		NodeParams = MakeShareable(new FJsonObject);
	}

	// Load the Blueprint
	UBlueprint* BP = LoadBlueprint(BlueprintName);
	if (!BP)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists at the supplied name/path; use `bp_create_blueprint` to create one first or `asset_list` to discover existing BPs."));
	}

	// Get the target graph — supports function_name for function/AnimGraph/state machine sub-graphs
	// For transition rule graphs, use from_state + to_state to disambiguate (they all share the name "Transition")
	FString FunctionName;
	FString TransFromState, TransToState;
	NodeParams->TryGetStringField(TEXT("from_state"), TransFromState);
	NodeParams->TryGetStringField(TEXT("to_state"), TransToState);
	UEdGraph* Graph = nullptr;

	// Auto-route UAnimGraphNode_* classes to the AnimBP's AnimGraph when the caller
	// did not pass an explicit function_name. The EventGraph never accepts AnimGraph
	// nodes, so silently landing them there is always wrong — the linker then can't
	// find them when connecting against AnimGraph nodes (cross-graph link refused).
	// See docs/bugs/mcp.md "bp_add_node silently places AnimGraph node classes in the
	// EventGraph unless function_name is set".
	// `bIsAnimNodeType` is the authoritative "this add will create an AnimGraph node"
	// predicate. After the friendly-alias fix in ResolveAnimGraphNodeClass, it resolves
	// every node_type the dispatch below creates as an AnimGraph node — both the explicit
	// cases (SequencePlayer, AimOffset, AnimSlot, ComponentToLocal, …) and the generic
	// UAnimGraphNode_* fallback. It is gated on the owning Blueprint actually being an
	// AnimBlueprint so a regular Actor/Pawn BP never pays the UClass-iteration cost and
	// never triggers anim routing. Computed once and reused for both the auto-route and
	// the EventGraph safety net below.
	const bool bIsAnimBlueprint = Cast<UAnimBlueprint>(BP) != nullptr;
	const bool bIsAnimGraphShortName =
		NodeType.StartsWith(TEXT("AnimGraphNode_"), ESearchCase::IgnoreCase) ||
		(NodeType.StartsWith(TEXT("/Script/")) && NodeType.Contains(TEXT("AnimGraphNode_")));
	const bool bIsAnimNodeType = bIsAnimBlueprint &&
		(bIsAnimGraphShortName || ResolveAnimGraphNodeClass(NodeType) != nullptr);

	const bool bExplicitFunctionName = NodeParams->HasField(TEXT("function_name"));
	if (!bExplicitFunctionName && bIsAnimNodeType)
	{
		FunctionName = TEXT("AnimGraph");
		NodeParams->SetStringField(TEXT("function_name"), FunctionName);
	}

	if (NodeParams->TryGetStringField(TEXT("function_name"), FunctionName) && !FunctionName.IsEmpty())
	{
		// Full search: FunctionGraphs, UbergraphPages, state machine sub-graphs
		Graph = FMCPCommonUtils::FindGraphByName(BP, FunctionName, TransFromState, TransToState);

		if (!Graph)
		{
			// Fallback: partial match for auto-generated names in FunctionGraphs
			for (UEdGraph* FuncGraph : BP->FunctionGraphs)
			{
				if (FuncGraph && FuncGraph->GetFName().ToString().Contains(FunctionName))
				{
					Graph = FuncGraph;
					break;
				}
			}
		}

		if (!Graph)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Graph not found: %s"), *FunctionName),
				EMCPErrorCode::FunctionNotFound,
				TEXT("Use `bp_list_graphs` to discover the BP's graph names — function graphs, AnimGraph sub-graphs, state machines, transition rule graphs are all listed there. Names are case-sensitive."));
		}
	}
	else if (!TransFromState.IsEmpty() && !TransToState.IsEmpty())
	{
		// Transition rule graph targeted by state names — FindGraphByName handles
		// disambiguation across all state machines using from/to state matching.
		Graph = FMCPCommonUtils::FindGraphByName(BP, FunctionName, TransFromState, TransToState);
		if (!Graph)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Transition graph not found: %s → %s"), *TransFromState, *TransToState),
				EMCPErrorCode::FunctionNotFound,
				TEXT("Verify both `from_state` and `to_state` exist in the same state machine, and a transition between them was created. Use `bp_inspect` on the AnimBP to see the state machine's transitions."));
		}
	}
	else
	{
		// Use event graph if no function specified (auto-creates for normal blueprints)
		Graph = FMCPCommonUtils::FindOrCreateEventGraph(BP);
		if (!Graph)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Blueprint has no event graph (and one could not be created for this blueprint type)"),
				EMCPErrorCode::UnsupportedClass,
				TEXT("Event-graph nodes require a Blueprint with an Event Graph (Actor / Pawn / Character / GameMode etc.). Function-library and pure-data Blueprints have no event graph; use the `function_name` parameter to target a function graph instead, or pick a different Blueprint type."));
		}
	}

	// Safety net: an AnimGraph node must NEVER be authored into an AnimBlueprint's
	// EventGraph (a Ubergraph). The auto-route above sends anim node types to the
	// AnimGraph, but an explicit function_name pointing at the EventGraph (or any future
	// routing drift) could still land us there — historically this misrouted the node
	// silently into the EventGraph (docs/bugs/mcp.md). The AnimGraph and per-state inner
	// pose graphs live in FunctionGraphs / state-machine sub-graphs, never in
	// UbergraphPages, so this check only rejects the EventGraph and never a legitimate
	// anim target. Refuse with a structured error instead of misrouting.
	if (bIsAnimNodeType && Graph && BP->UbergraphPages.Contains(Graph))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("AnimGraph node '%s' cannot be placed in the EventGraph '%s'"), *NodeType, *Graph->GetName()),
			EMCPErrorCode::InvalidArgument,
			TEXT("AnimGraph nodes belong in the AnimGraph (or a state's inner pose graph), not the EventGraph. Omit `function_name` to auto-route to the AnimGraph, or pass `function_name=\"AnimGraph\"` (or a state inner-graph name from `bp_list_graphs`)."));
	}

	// Create node based on type - routed to specialized node creators
	UK2Node* NewNode = nullptr;

	// Control Flow Nodes
	if (NodeType.Equals(TEXT("Branch"), ESearchCase::IgnoreCase))
	{
		NewNode = FControlFlowNodeCreator::CreateBranchNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("Comparison"), ESearchCase::IgnoreCase))
	{
		NewNode = FControlFlowNodeCreator::CreateComparisonNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("Switch"), ESearchCase::IgnoreCase))
	{
		NewNode = FControlFlowNodeCreator::CreateSwitchNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("SwitchEnum"), ESearchCase::IgnoreCase))
	{
		NewNode = FControlFlowNodeCreator::CreateSwitchEnumNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("SwitchInteger"), ESearchCase::IgnoreCase))
	{
		NewNode = FControlFlowNodeCreator::CreateSwitchIntegerNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("ExecutionSequence"), ESearchCase::IgnoreCase))
	{
		NewNode = FControlFlowNodeCreator::CreateExecutionSequenceNode(Graph, NodeParams);
	}
	// Data Nodes
	else if (NodeType.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase))
	{
		NewNode = FDataNodeCreator::CreateVariableGetNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase))
	{
		NewNode = FDataNodeCreator::CreateVariableSetNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("MakeArray"), ESearchCase::IgnoreCase))
	{
		NewNode = FDataNodeCreator::CreateMakeArrayNode(Graph, NodeParams);
	}
	// Utility Nodes
	else if (NodeType.Equals(TEXT("Print"), ESearchCase::IgnoreCase))
	{
		NewNode = FUtilityNodeCreator::CreatePrintNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
	{
		NewNode = FUtilityNodeCreator::CreateCallFunctionNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("Select"), ESearchCase::IgnoreCase))
	{
		NewNode = FUtilityNodeCreator::CreateSelectNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("SpawnActor"), ESearchCase::IgnoreCase))
	{
		NewNode = FUtilityNodeCreator::CreateSpawnActorNode(Graph, NodeParams);
	}
	// Casting Nodes
	else if (NodeType.Equals(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
	{
		NewNode = FCastingNodeCreator::CreateDynamicCastNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("ClassDynamicCast"), ESearchCase::IgnoreCase))
	{
		NewNode = FCastingNodeCreator::CreateClassDynamicCastNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("CastByteToEnum"), ESearchCase::IgnoreCase))
	{
		NewNode = FCastingNodeCreator::CreateCastByteToEnumNode(Graph, NodeParams);
	}
	// Animation Nodes
	else if (NodeType.Equals(TEXT("Timeline"), ESearchCase::IgnoreCase))
	{
		NewNode = FAnimationNodeCreator::CreateTimelineNode(Graph, NodeParams);
	}
	// Specialized Nodes
	else if (NodeType.Equals(TEXT("GetDataTableRow"), ESearchCase::IgnoreCase))
	{
		NewNode = FSpecializedNodeCreator::CreateGetDataTableRowNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("AddComponentByClass"), ESearchCase::IgnoreCase))
	{
		NewNode = FSpecializedNodeCreator::CreateAddComponentByClassNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
	{
		NewNode = FSpecializedNodeCreator::CreateSelfNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("ConstructObject"), ESearchCase::IgnoreCase))
	{
		NewNode = FSpecializedNodeCreator::CreateConstructObjectNode(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("Knot"), ESearchCase::IgnoreCase))
	{
		NewNode = FSpecializedNodeCreator::CreateKnotNode(Graph, NodeParams);
	}
	// Event nodes (kept for backward compatibility - should use add_event_node)
	else if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateEventNode(Graph, NodeParams);
	}
	// ── AnimGraph nodes ────────────────────────────────────────────────
	else if (NodeType.Equals(TEXT("SequencePlayer"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_SequencePlayer>(Graph, NodeParams);
		if (N) SetAnimAssetFromParams(N, NodeParams);
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("BlendSpacePlayer"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_BlendSpacePlayer>(Graph, NodeParams);
		if (N) SetAnimAssetFromParams(N, NodeParams);
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("AimOffset"), ESearchCase::IgnoreCase) ||
	         NodeType.Equals(TEXT("RotationOffsetBlendSpace"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_RotationOffsetBlendSpace>(Graph, NodeParams);
		if (N) SetAnimAssetFromParams(N, NodeParams);
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("AnimSlot"), ESearchCase::IgnoreCase) ||
	         NodeType.Equals(TEXT("Slot"), ESearchCase::IgnoreCase))
	{
		auto* SlotNode = CreateAnimGraphNode<UAnimGraphNode_Slot>(Graph, NodeParams);
		if (SlotNode)
		{
			FString SlotName;
			if (NodeParams->TryGetStringField(TEXT("slot_name"), SlotName))
			{
				SlotNode->Node.SlotName = FName(*SlotName);
			}
		}
		NewNode = SlotNode;
	}
	else if (NodeType.Equals(TEXT("LayeredBoneBlend"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_LayeredBoneBlend>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("TwoWayBlend"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_TwoWayBlend>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("ApplyAdditive"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_ApplyAdditive>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("SaveCachedPose"), ESearchCase::IgnoreCase))
	{
		auto* SaveNode = CreateAnimGraphNode<UAnimGraphNode_SaveCachedPose>(Graph, NodeParams);
		if (SaveNode)
		{
			FString CacheName;
			if (NodeParams->TryGetStringField(TEXT("cache_name"), CacheName))
			{
				SaveNode->CacheName = CacheName;
			}
		}
		NewNode = SaveNode;
	}
	else if (NodeType.Equals(TEXT("UseCachedPose"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_UseCachedPose>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("ComponentToLocal"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_ComponentToLocalSpace>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("LocalToComponent"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_LocalToComponentSpace>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("AnimBlendListByBool"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_BlendListByBool>(Graph, NodeParams);
	}
	// ── Phase 2: ModifyBone and TwoBoneIK (already #include-d) ────────
	else if (NodeType.Equals(TEXT("ModifyBone"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_ModifyBone>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.BoneToModify.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("TwoBoneIK"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_TwoBoneIK>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.IKBone.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	// ── Phase 3 Tier A: Project-critical node types ───────────────────
	else if (NodeType.Equals(TEXT("LinkedAnimLayer"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_LinkedAnimLayer>(Graph, NodeParams);
		if (N)
		{
			FString InterfacePath;
			if (NodeParams->TryGetStringField(TEXT("layer_interface"), InterfacePath))
			{
				UClass* InterfaceClass = LoadObject<UClass>(nullptr, *InterfacePath);
				if (InterfaceClass && InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()))
				{
					N->Node.Interface = TSubclassOf<UAnimLayerInterface>(InterfaceClass);
					N->ReconstructNode();
				}
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("LinkedAnimGraph"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_LinkedAnimGraph>(Graph, NodeParams);
		if (N)
		{
			FString ClassPath;
			if (NodeParams->TryGetStringField(TEXT("linked_class"), ClassPath))
			{
				UClass* AnimClass = LoadObject<UClass>(nullptr, *ClassPath);
				if (AnimClass)
				{
					N->Node.InstanceClass = TSubclassOf<UAnimInstance>(AnimClass);
					N->ReconstructNode();
				}
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("LinkedInputPose"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_LinkedInputPose>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("Inertialization"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_Inertialization>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("BlendListByEnum"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_BlendListByEnum>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("BlendListByInt"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_BlendListByInt>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("CopyPoseFromMesh"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_CopyPoseFromMesh>(Graph, NodeParams);
	}
	// ── Phase 3 Tier B: Common animation nodes ────────────────────────
	else if (NodeType.Equals(TEXT("SequenceEvaluator"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_SequenceEvaluator>(Graph, NodeParams);
		if (N) SetAnimAssetFromParams(N, NodeParams);
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("BlendSpaceEvaluator"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_BlendSpaceEvaluator>(Graph, NodeParams);
		if (N) SetAnimAssetFromParams(N, NodeParams);
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("ApplyMeshSpaceAdditive"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_ApplyMeshSpaceAdditive>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("MakeDynamicAdditive"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_MakeDynamicAdditive>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("LocalRefPose"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_LocalRefPose>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("MeshRefPose"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_MeshRefPose>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("ModifyCurve"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_ModifyCurve>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("RandomPlayer"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_RandomPlayer>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("DeadBlending"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_DeadBlending>(Graph, NodeParams);
	}
	// ── Phase 3 Tier C: IK and skeletal control ───────────────────────
	else if (NodeType.Equals(TEXT("CCDIK"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_CCDIK>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.TipBone.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("Fabrik"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_Fabrik>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.TipBone.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("LookAt"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_LookAt>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.BoneToModify.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("SpringBone"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_SpringBone>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.SpringBone.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("CopyBone"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_CopyBone>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.TargetBone.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("BoneDrivenController"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_BoneDrivenController>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.SourceBone.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("AnimDynamics"), ESearchCase::IgnoreCase))
	{
		auto* N = CreateAnimGraphNode<UAnimGraphNode_AnimDynamics>(Graph, NodeParams);
		if (N)
		{
			FString BoneName;
			if (NodeParams->TryGetStringField(TEXT("target_bone"), BoneName))
			{
				N->Node.BoundBone.BoneName = FName(*BoneName);
				N->ReconstructNode();
			}
		}
		NewNode = N;
	}
	else if (NodeType.Equals(TEXT("LegIK"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_LegIK>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("SplineIK"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_SplineIK>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("HandIKRetargeting"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_HandIKRetargeting>(Graph, NodeParams);
	}
	// ── Phase 3 Tier D: Advanced/niche ────────────────────────────────
	else if (NodeType.Equals(TEXT("Mirror"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_Mirror>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("RotateRootBone"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_RotateRootBone>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("CurveSource"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_CurveSource>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("StateResult"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_StateResult>(Graph, NodeParams);
	}
	else if (NodeType.Equals(TEXT("TransitionPoseEvaluator"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateAnimGraphNode<UAnimGraphNode_TransitionPoseEvaluator>(Graph, NodeParams);
	}
	else
	{
		// Generic fallback — any UAnimGraphNode_Base subclass registered in the UClass iterator.
		// Resolves plugin AnimGraph nodes (DragonIK, ALSv4, etc.) that aren't in the explicit
		// registry above. Accepts short names, full class names, and /Script/Module.Class paths.
		if (UClass* AnimNodeClass = ResolveAnimGraphNodeClass(NodeType))
		{
			NewNode = CreateAnimGraphNodeByClass(Graph, AnimNodeClass, NodeParams);
			if (!NewNode)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(
						TEXT("Resolved '%s' to class '%s' (UAnimGraphNode_Base subclass) but failed to create node instance"),
						*NodeType, *AnimNodeClass->GetName()),
					EMCPErrorCode::Internal,
					TEXT("AnimGraph fallback resolved the type to a real UClass but NewObject<>() returned null. Common cause: the target graph isn't an AnimGraph (AnimGraph nodes only attach to AnimBP graphs). Confirm the target Blueprint is an AnimInstance subclass."));
			}
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Unknown node type: %s"), *NodeType),
				EMCPErrorCode::InvalidArgument,
				TEXT("Valid node_type values include Branch, Comparison, Switch, SwitchEnum, SwitchInteger, ExecutionSequence, VariableGet, VariableSet, MakeArray, Print, CallFunction, Select, SpawnActor, DynamicCast, ClassDynamicCast, CastByteToEnum, Timeline, GetDataTableRow, AddComponentByClass, Self, ConstructObject, Knot, Event, plus all UAnimGraphNode_* subclasses (SequencePlayer, BlendSpacePlayer, ModifyBone, TwoBoneIK, etc.). The AnimGraph fallback also resolves any UAnimGraphNode_Base subclass by short name."));
		}
	}

	if (!NewNode)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to create %s node"), *NodeType),
			EMCPErrorCode::Internal,
			TEXT("Type was recognized but the per-type creator returned null. Cause is type-specific (e.g., target_function not found for CallFunction, anim_asset path invalid for SequencePlayer). Check the editor log for the specific creator's error message."));
	}

	// Notify changes
	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	// Ensure node has a valid GUID
	if (NewNode->NodeGuid.IsValid() == false || NewNode->NodeGuid == FGuid())
	{
		NewNode->CreateNewGuid();
	}

	return CreateSuccessResponse(NewNode, NodeType);
}

UK2Node* FBlueprintNodeManager::CreatePrintNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph)
	{
		return nullptr;
	}

	UK2Node_CallFunction* PrintNode = NewObject<UK2Node_CallFunction>(Graph);
	if (!PrintNode)
	{
		return nullptr;
	}

	UFunction* PrintFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)
	);

	if (!PrintFunc)
	{
		return nullptr;
	}

	PrintNode->SetFromFunction(PrintFunc);

	// Set position
	double PosX = 0.0;
	double PosY = 0.0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	PrintNode->NodePosX = static_cast<int32>(PosX);
	PrintNode->NodePosY = static_cast<int32>(PosY);

	PrintNode->AllocateDefaultPins();

	// Set message if provided
	FString Message;
	if (Params->TryGetStringField(TEXT("message"), Message))
	{
		UEdGraphPin* InStringPin = PrintNode->FindPin(TEXT("InString"));
		if (InStringPin)
		{
			InStringPin->DefaultValue = Message;
		}
	}

	Graph->AddNode(PrintNode, true, false);
	return PrintNode;
}

UK2Node_Event* FBlueprintNodeManager::CreateEventNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph)
	{
		return nullptr;
	}

	FString EventType;
	if (!Params->TryGetStringField(TEXT("event_type"), EventType))
	{
		EventType = TEXT("BeginPlay");
	}

	UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
	if (!EventNode)
	{
		return nullptr;
	}

	if (EventType.Equals(TEXT("BeginPlay"), ESearchCase::IgnoreCase))
	{
		// Use direct function name - ReceiveBeginPlay is protected
		EventNode->EventReference.SetExternalDelegateMember(FName(TEXT("ReceiveBeginPlay")));
		EventNode->bOverrideFunction = true;
	}
	else if (EventType.Equals(TEXT("Tick"), ESearchCase::IgnoreCase))
	{
		// Use direct function name - ReceiveTick is protected
		EventNode->EventReference.SetExternalDelegateMember(FName(TEXT("ReceiveTick")));
		EventNode->bOverrideFunction = true;
	}
	else
	{
		EventNode->CustomFunctionName = FName(*EventType);
	}

	double PosX = 0.0;
	double PosY = 0.0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	EventNode->NodePosX = static_cast<int32>(PosX);
	EventNode->NodePosY = static_cast<int32>(PosY);

	EventNode->AllocateDefaultPins();
	Graph->AddNode(EventNode, true, false);

	return EventNode;
}

UK2Node* FBlueprintNodeManager::CreateVariableGetNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph)
	{
		return nullptr;
	}

	FString VariableName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return nullptr;
	}

	UK2Node_VariableGet* VarGetNode = NewObject<UK2Node_VariableGet>(Graph);
	if (!VarGetNode)
	{
		return nullptr;
	}

	VarGetNode->VariableReference.SetSelfMember(FName(*VariableName));

	double PosX = 0.0;
	double PosY = 0.0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	VarGetNode->NodePosX = static_cast<int32>(PosX);
	VarGetNode->NodePosY = static_cast<int32>(PosY);

	VarGetNode->AllocateDefaultPins();
	Graph->AddNode(VarGetNode, true, false);

	return VarGetNode;
}

UK2Node* FBlueprintNodeManager::CreateVariableSetNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph)
	{
		return nullptr;
	}

	FString VariableName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		return nullptr;
	}

	UK2Node_VariableSet* VarSetNode = NewObject<UK2Node_VariableSet>(Graph);
	if (!VarSetNode)
	{
		return nullptr;
	}

	VarSetNode->VariableReference.SetSelfMember(FName(*VariableName));

	double PosX = 0.0;
	double PosY = 0.0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	VarSetNode->NodePosX = static_cast<int32>(PosX);
	VarSetNode->NodePosY = static_cast<int32>(PosY);

	VarSetNode->AllocateDefaultPins();
	Graph->AddNode(VarSetNode, true, false);

	return VarSetNode;
}

UBlueprint* FBlueprintNodeManager::LoadBlueprint(const FString& BlueprintName)
{
	return FMCPCommonUtils::FindBlueprintByName(BlueprintName);
}

UK2Node* FBlueprintNodeManager::CreateCallFunctionNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph)
	{
		return nullptr;
	}

	// Get target function name
	FString TargetFunction;
	if (!Params->TryGetStringField(TEXT("target_function"), TargetFunction))
	{
		return nullptr;
	}

	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	if (!CallNode)
	{
		return nullptr;
	}

	// Set position
	double PosX = 0.0;
	double PosY = 0.0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	CallNode->NodePosX = static_cast<int32>(PosX);
	CallNode->NodePosY = static_cast<int32>(PosY);

	// Create GUID for the node
	CallNode->CreateNewGuid();

	// Set the function reference. If a `target_class` is supplied, author an
	// EXTERNAL member call (a function living on another class — e.g. a
	// component's function), not a self call. Without this branch every call
	// resolved to self (SetSelfMember unconditionally), so cross-object calls
	// silently failed to compile. See docs/bugs/mcp.md.
	FString TargetClassName;
	if (Params->TryGetStringField(TEXT("target_class"), TargetClassName) && !TargetClassName.IsEmpty())
	{
		UClass* TargetClass = LoadObject<UClass>(nullptr, *TargetClassName);
		if (!TargetClass)
		{
			TargetClass = FindFirstObject<UClass>(*TargetClassName, EFindFirstObjectOptions::NativeFirst);
		}
		if (!TargetClass)
		{
			// Surface the failure instead of silently degrading to a (wrong)
			// self-call. The dispatcher reports the null node as a clean error.
			UE_LOG(LogUnrealMCP, Warning,
				TEXT("[MCP] CreateCallFunctionNode: target_class '%s' did not resolve to a UClass — refusing to author a self-call for external function '%s'."),
				*TargetClassName, *TargetFunction);
			return nullptr;
		}
		CallNode->FunctionReference.SetExternalMember(FName(*TargetFunction), TargetClass);
	}
	else
	{
		CallNode->FunctionReference.SetSelfMember(FName(*TargetFunction));
	}

	// Add node to graph with proper initialization
	Graph->AddNode(CallNode, true, false);

	// Post-place initialization
	CallNode->PostPlacedNewNode();

	// Allocate pins after all setup
	CallNode->AllocateDefaultPins();

	return CallNode;
}

UK2Node* FBlueprintNodeManager::CreateComparisonNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph)
	{
		return nullptr;
	}

	// Create a Promotable Operator node for comparisons
	// Note: UK2Node_PromotableOperator in UE5.5 doesn't expose SetOperator() method
	// The node is created with default operator (Equal) and Unreal handles initialization
	UK2Node_PromotableOperator* ComparisonNode = NewObject<UK2Node_PromotableOperator>(Graph);
	if (!ComparisonNode)
	{
		return nullptr;
	}

	// Set position
	double PosX = 0.0;
	double PosY = 0.0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	ComparisonNode->NodePosX = static_cast<int32>(PosX);
	ComparisonNode->NodePosY = static_cast<int32>(PosY);

	// Add to graph and initialize pins
	Graph->AddNode(ComparisonNode, false, false);
	ComparisonNode->CreateNewGuid();
	ComparisonNode->PostPlacedNewNode();
	ComparisonNode->AllocateDefaultPins();

	// Set pin type if specified (int, float, string, bool, etc.)
	FString PinType;
	if (Params->TryGetStringField(TEXT("pin_type"), PinType))
	{
		// Find and update the A and B pins to the specified type
		UEdGraphPin* PinA = ComparisonNode->FindPin(TEXT("A"));
		UEdGraphPin* PinB = ComparisonNode->FindPin(TEXT("B"));

		if (PinA && PinB)
		{
			// Create a proper FEdGraphPinType structure
			FEdGraphPinType NewPinType;

			if (PinType.Equals(TEXT("int"), ESearchCase::IgnoreCase))
			{
				NewPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			}
			else if (PinType.Equals(TEXT("float"), ESearchCase::IgnoreCase) || PinType.Equals(TEXT("double"), ESearchCase::IgnoreCase))
			{
				NewPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			}
			else if (PinType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
			{
				NewPinType.PinCategory = UEdGraphSchema_K2::PC_String;
			}
			else if (PinType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
			{
				NewPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			}
			else if (PinType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				NewPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				NewPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			}
			else if (PinType.Equals(TEXT("name"), ESearchCase::IgnoreCase))
			{
				NewPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			}
			else if (PinType.Equals(TEXT("text"), ESearchCase::IgnoreCase))
			{
				NewPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			}

			// Apply the entire pin type structure
			PinA->PinType = NewPinType;
			PinB->PinType = NewPinType;

			// Notify schema that pins have changed
			ComparisonNode->ReconstructNode();
		}
	}

	return ComparisonNode;
}

UK2Node* FBlueprintNodeManager::CreateBranchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph)
	{
		return nullptr;
	}

	// Create a Branch node using K2Node_IfThenElse
	UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Graph);
	if (!BranchNode)
	{
		return nullptr;
	}

	// Set position
	double PosX = 0.0;
	double PosY = 0.0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	BranchNode->NodePosX = static_cast<int32>(PosX);
	BranchNode->NodePosY = static_cast<int32>(PosY);

	// Add to graph and initialize pins
	Graph->AddNode(BranchNode, false, false);
	BranchNode->CreateNewGuid();
	BranchNode->PostPlacedNewNode();
	BranchNode->AllocateDefaultPins();
	return BranchNode;
}

TSharedPtr<FJsonObject> FBlueprintNodeManager::CreateSuccessResponse(const UK2Node* Node, const FString& NodeType)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("node_id"), Node->GetName());
	Response->SetStringField(TEXT("node_type"), NodeType);
	Response->SetNumberField(TEXT("pos_x"), Node->NodePosX);
	Response->SetNumberField(TEXT("pos_y"), Node->NodePosY);
	return Response;
}

// Local CreateErrorResponse helper removed 2026-05-10 (doc 1 migration). All
// AddNode error paths now route through FMCPCommonUtils::CreateErrorResponse
// with explicit (Message, EMCPErrorCode, Hint) so error_code lands on the
// wire envelope. The local helper duplicated the single-arg form and bypassed
// the structured-error path.
