#include "Commands/BlueprintGraph/NodeDeleter.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorAssetLibrary.h"

TSharedPtr<FJsonObject> FNodeDeleter::DeleteNode(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters. Doc 1 migration: structured error_code + actionable hint
	// on every failure path.
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with at minimum `blueprint_name` and `node_id` fields. Optional `function_name` / `from_state` / `to_state` target a specific graph."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint owning the node."));
	}

	FString NodeID;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeID))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_id` (string) — the FGuid of the node to delete. Use `bp_inspect` to discover node IDs."));
	}

	// Get optional function name and transition state filters
	FString FunctionName;
	Params->TryGetStringField(TEXT("function_name"), FunctionName);

	FString TransFromState, TransToState;
	Params->TryGetStringField(TEXT("from_state"), TransFromState);
	Params->TryGetStringField(TEXT("to_state"), TransToState);

	// Load the Blueprint
	UBlueprint* Blueprint = LoadBlueprint(BlueprintName);
	if (!Blueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists at the supplied name/path; use `asset_list` or `bp_create_blueprint` first."));
	}

	// Get the appropriate graph
	UEdGraph* Graph = GetGraph(Blueprint, FunctionName, TransFromState, TransToState);
	if (!Graph)
	{
		if (FunctionName.IsEmpty())
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Blueprint has no event graph"),
				EMCPErrorCode::UnsupportedClass,
				TEXT("Blueprints without an event graph (function-library, pure-data) can't host event-graph nodes. Pass `function_name` to target a function graph instead, or pick a different BP."));
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Function graph not found: %s"), *FunctionName),
				EMCPErrorCode::FunctionNotFound,
				TEXT("Use `bp_list_graphs` to discover the BP's graph names — function graphs, AnimGraph sub-graphs, state machines, transition rule graphs are all listed there. Names are case-sensitive."));
		}
	}

	// Find the node
	UEdGraphNode* Node = FindNodeByID(Graph, NodeID);
	if (!Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeID),
			EMCPErrorCode::NodeNotFound,
			TEXT("Use `bp_inspect` to discover node IDs in the target graph. Note: node IDs are FGuids, not display names."));
	}

	// Store node ID before deletion
	FString DeletedID = Node->NodeGuid.ToString();

	// dry_run: validation already ran (BP, graph, node lookups). Diff shape per
	// todo/13: {nodes_removed: [{node_id, type, position, connections_severed_count}]}.
	// We capture the connection count from the live node since BreakAllNodeLinks
	// hasn't run yet — gives the agent visibility into how much wiring this
	// removal would tear down.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		int32 ConnectionsCount = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin) ConnectionsCount += Pin->LinkedTo.Num();
		}

		TSharedPtr<FJsonObject> NodeEntry = MakeShared<FJsonObject>();
		NodeEntry->SetStringField(TEXT("node_id"), DeletedID);
		NodeEntry->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
		NodeEntry->SetStringField(TEXT("node_name"),  Node->GetName());

		TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), Node->NodePosX);
		Position->SetNumberField(TEXT("y"), Node->NodePosY);
		NodeEntry->SetObjectField(TEXT("position"), Position);

		NodeEntry->SetNumberField(TEXT("connections_severed"), ConnectionsCount);

		TArray<TSharedPtr<FJsonValue>> RemovedArr;
		RemovedArr.Add(MakeShared<FJsonValueObject>(NodeEntry));

		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("nodes_removed"), RemovedArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Remove the node
	if (!RemoveNode(Graph, Node))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to remove node: %s"), *NodeID),
			EMCPErrorCode::Internal,
			TEXT("Engine RemoveNode call returned false despite the node being found. Possible cause: node has hardcoded 'cannot delete' flag (e.g., a function entry/result node). Check the editor log for specifics."));
	}

	// Notify changes
	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogUnrealMCP, Display, TEXT("Successfully deleted node '%s' from %s"), *DeletedID, *BlueprintName);

	return CreateSuccessResponse(DeletedID);
}

UEdGraph* FNodeDeleter::GetGraph(UBlueprint* Blueprint, const FString& FunctionName,
	const FString& TransFromState, const FString& TransToState)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// If no function name but from/to state provided, resolve transition rule graph
	if (FunctionName.IsEmpty() && !TransFromState.IsEmpty() && !TransToState.IsEmpty())
	{
		return FMCPCommonUtils::FindGraphByName(Blueprint, FunctionName, TransFromState, TransToState);
	}

	// If no function name at all, use event graph
	if (FunctionName.IsEmpty())
	{
		return FMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
	}

	// Full search including state machine sub-graphs
	UEdGraph* Found = FMCPCommonUtils::FindGraphByName(Blueprint, FunctionName, TransFromState, TransToState);
	if (Found) return Found;

	// Fallback: case-insensitive match in function graphs
	for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
	{
		if (FuncGraph && FuncGraph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			return FuncGraph;
		}
	}

	return nullptr;
}

UEdGraphNode* FNodeDeleter::FindNodeByID(UEdGraph* Graph, const FString& NodeID)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// Try matching by NodeGuid
		if (Node->NodeGuid.ToString().Equals(NodeID, ESearchCase::IgnoreCase))
		{
			return Node;
		}

		// Try matching by GetName()
		if (Node->GetName().Equals(NodeID, ESearchCase::IgnoreCase))
		{
			return Node;
		}
	}

	return nullptr;
}

bool FNodeDeleter::RemoveNode(UEdGraph* Graph, UEdGraphNode* Node)
{
	if (!Graph || !Node)
	{
		return false;
	}

	// Break all connections first
	Node->BreakAllNodeLinks();

	// Remove from graph
	Graph->RemoveNode(Node);

	return true;
}

UBlueprint* FNodeDeleter::LoadBlueprint(const FString& BlueprintName)
{
	return FMCPCommonUtils::FindBlueprintByName(BlueprintName);
}

TSharedPtr<FJsonObject> FNodeDeleter::CreateSuccessResponse(const FString& DeletedNodeID)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("deleted_node_id"), DeletedNodeID);
	return Response;
}

// Local CreateErrorResponse helper removed 2026-05-10 (doc 1 migration). All
// error paths now route through FMCPCommonUtils::CreateErrorResponse
// with explicit (Message, EMCPErrorCode, Hint) so error_code lands on the wire.
