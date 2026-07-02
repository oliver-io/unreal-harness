#include "Commands/BlueprintGraph/BPConnector.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_PromotableOperator.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorAssetLibrary.h"

namespace
{
	// Mirror of the editor's drag-and-drop behavior for promotable operator nodes:
	// when a wildcard A/B/ReturnValue pin on a UK2Node_PromotableOperator is
	// targeted for connection with a typed pin, promote the operator's signature
	// to match the typed pin before CanCreateConnection validates the link.
	// Without this, float->wildcard connections fail with:
	//   "No matching 'None' function for 'Float (single-precision)'"
	// because the wildcard signature slot is still 'None'.
	void PromoteWildcardPinsIfNeeded(UEdGraphPin* SourcePin, UEdGraphPin* TargetPin)
	{
		const FName PC_Wildcard = UEdGraphSchema_K2::PC_Wildcard;
		auto IsWildcard = [&](const UEdGraphPin* Pin) -> bool
		{
			return Pin && Pin->PinType.PinCategory == PC_Wildcard;
		};

		// Target side: wildcard on PromotableOperator, promote to source pin's type.
		if (TargetPin && IsWildcard(TargetPin) && !IsWildcard(SourcePin))
		{
			if (UK2Node_PromotableOperator* PromoNode = Cast<UK2Node_PromotableOperator>(TargetPin->GetOwningNode()))
			{
				if (PromoNode->CanConvertPinType(TargetPin))
				{
					PromoNode->ConvertPinType(TargetPin, SourcePin->PinType);
				}
			}
		}

		// Source side: wildcard on PromotableOperator, promote to target pin's type.
		if (SourcePin && IsWildcard(SourcePin) && !IsWildcard(TargetPin))
		{
			if (UK2Node_PromotableOperator* PromoNode = Cast<UK2Node_PromotableOperator>(SourcePin->GetOwningNode()))
			{
				if (PromoNode->CanConvertPinType(SourcePin))
				{
					PromoNode->ConvertPinType(SourcePin, TargetPin->PinType);
				}
			}
		}
	}
}

TSharedPtr<FJsonObject> FBPConnector::ConnectNodes(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    // Extraire paramètres
    FString BlueprintName = Params->GetStringField(TEXT("blueprint_name"));
    FString SourceNodeId = Params->GetStringField(TEXT("source_node_id"));
    FString SourcePinName = Params->GetStringField(TEXT("source_pin_name"));
    FString TargetNodeId = Params->GetStringField(TEXT("target_node_id"));
    FString TargetPinName = Params->GetStringField(TEXT("target_pin_name"));

    FString FunctionName;
    Params->TryGetStringField(TEXT("function_name"), FunctionName);

    FString TransFromState, TransToState;
    Params->TryGetStringField(TEXT("from_state"), TransFromState);
    Params->TryGetStringField(TEXT("to_state"), TransToState);

    // Load Blueprint using unified lookup (supports short names at any path)
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprintByName(BlueprintName);

    if (!Blueprint)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", "Blueprint not found");
        return Result;
    }

    // Get graph — full search including state machine sub-graphs
    UEdGraph* Graph = nullptr;

    if (!FunctionName.IsEmpty() || (!TransFromState.IsEmpty() && !TransToState.IsEmpty()))
    {
        Graph = FMCPCommonUtils::FindGraphByName(Blueprint, FunctionName, TransFromState, TransToState);
        if (!Graph)
        {
            Result->SetBoolField("success", false);
            Result->SetStringField("error", FString::Printf(TEXT("Graph not found: %s (from_state=%s, to_state=%s)"), *FunctionName, *TransFromState, *TransToState));
            return Result;
        }
    }
    else
    {
        Graph = FMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
    }

    if (!Graph)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", "Graph not found");
        return Result;
    }

    // Find nodes
    UK2Node* SourceNode = FindNodeById(Graph, SourceNodeId);
    UK2Node* TargetNode = FindNodeById(Graph, TargetNodeId);

    if (!SourceNode || !TargetNode)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", "Node not found");
        return Result;
    }

    // Trouver pins
    UEdGraphPin* SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_Output);
    UEdGraphPin* TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_Input);

    if (!SourcePin || !TargetPin)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", "Pin not found");
        return Result;
    }

    // Promote PromotableOperator wildcard pins to match the typed side of the
    // connection. Mirrors the editor's drag-and-drop behavior — MCP callers don't
    // get the wildcard-resolution step the Details UI performs automatically.
    PromoteWildcardPinsIfNeeded(SourcePin, TargetPin);

    // Use the graph's schema for connection — handles pose pins, breaks existing links, notifies graph
    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (Schema)
    {
        FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
        if (Response.Response == CONNECT_RESPONSE_DISALLOW)
        {
            Result->SetBoolField("success", false);
            Result->SetStringField("error", FString::Printf(TEXT("Connection not allowed: %s"), *Response.Message.ToString()));
            return Result;
        }

        // dry_run: schema validation already ran (CanCreateConnection above);
        // emit the diff and bail before TryCreateConnection's actual link.
        // Per todo/13 phase 2 diff shape: {connections_added: [{source_node,
        // source_pin, target_node, target_pin, connection_type}]}.
        // We also surface any link-breaks the schema would have triggered —
        // CONNECT_RESPONSE_BREAK_OTHERS_A/_B/_AB indicate the schema would
        // sever existing wires to make room. The agent sees this in the diff.
        if (FMCPCommonUtils::ParseDryRun(Params))
        {
            TSharedPtr<FJsonObject> AddEntry = MakeShared<FJsonObject>();
            AddEntry->SetStringField(TEXT("source_node"), SourceNodeId);
            AddEntry->SetStringField(TEXT("source_pin"),  SourcePinName);
            AddEntry->SetStringField(TEXT("target_node"), TargetNodeId);
            AddEntry->SetStringField(TEXT("target_pin"),  TargetPinName);
            AddEntry->SetStringField(TEXT("connection_type"), SourcePin->PinType.PinCategory.ToString());

            const bool bWouldBreakA = (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A
                                    || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB);
            const bool bWouldBreakB = (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B
                                    || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB);
            int32 ConnectionsBroken = 0;
            if (bWouldBreakA) ConnectionsBroken += SourcePin->LinkedTo.Num();
            if (bWouldBreakB) ConnectionsBroken += TargetPin->LinkedTo.Num();
            AddEntry->SetNumberField(TEXT("would_break_existing_links"), ConnectionsBroken);

            TArray<TSharedPtr<FJsonValue>> AddedArr;
            AddedArr.Add(MakeShared<FJsonValueObject>(AddEntry));
            TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
            Diff->SetArrayField(TEXT("connections_added"), AddedArr);
            return FMCPCommonUtils::CreateDryRunResponse(Diff);
        }

        if (!Schema->TryCreateConnection(SourcePin, TargetPin))
        {
            Result->SetBoolField("success", false);
            Result->SetStringField("error", TEXT("Schema->TryCreateConnection failed"));
            return Result;
        }
    }
    else
    {
        // Fallback — raw link (no schema available). Same dry-run discipline:
        // emit the diff and skip MakeLinkTo. No schema means we can't surface
        // would_break info, so it's omitted on this branch.
        if (FMCPCommonUtils::ParseDryRun(Params))
        {
            TSharedPtr<FJsonObject> AddEntry = MakeShared<FJsonObject>();
            AddEntry->SetStringField(TEXT("source_node"), SourceNodeId);
            AddEntry->SetStringField(TEXT("source_pin"),  SourcePinName);
            AddEntry->SetStringField(TEXT("target_node"), TargetNodeId);
            AddEntry->SetStringField(TEXT("target_pin"),  TargetPinName);
            AddEntry->SetStringField(TEXT("connection_type"), SourcePin->PinType.PinCategory.ToString());

            TArray<TSharedPtr<FJsonValue>> AddedArr;
            AddedArr.Add(MakeShared<FJsonValueObject>(AddEntry));
            TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
            Diff->SetArrayField(TEXT("connections_added"), AddedArr);
            return FMCPCommonUtils::CreateDryRunResponse(Diff);
        }

        SourcePin->MakeLinkTo(TargetPin);
    }

    // Mark modified and recompile
    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    // Return
    Result->SetBoolField("success", true);

    TSharedPtr<FJsonObject> ConnectionInfo = MakeShared<FJsonObject>();
    ConnectionInfo->SetStringField("source_node", SourceNodeId);
    ConnectionInfo->SetStringField("source_pin", SourcePinName);
    ConnectionInfo->SetStringField("target_node", TargetNodeId);
    ConnectionInfo->SetStringField("target_pin", TargetPinName);
    ConnectionInfo->SetStringField("connection_type", SourcePin->PinType.PinCategory.ToString());

    Result->SetObjectField("connection", ConnectionInfo);

    return Result;
}

UK2Node* FBPConnector::FindNodeById(UEdGraph* Graph, const FString& NodeId)
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

        // Try matching by NodeGuid first
        if (Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
        {
            UK2Node* K2Node = Cast<UK2Node>(Node);
            return K2Node;  // Return even if nullptr (caller will handle)
        }

        // Try matching by GetName()
        if (Node->GetName().Equals(NodeId, ESearchCase::IgnoreCase))
        {
            UK2Node* K2Node = Cast<UK2Node>(Node);
            return K2Node;  // Return even if nullptr (caller will handle)
        }
    }

    return nullptr;
}

UEdGraphPin* FBPConnector::FindPinByName(UK2Node* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString() == PinName && Pin->Direction == Direction)
        {
            return Pin;
        }
    }
    return nullptr;
}

bool FBPConnector::ArePinsCompatible(UEdGraphPin* SourcePin, UEdGraphPin* TargetPin)
{
    if (SourcePin->Direction != EGPD_Output || TargetPin->Direction != EGPD_Input)
    {
        return false;
    }

    return SourcePin->PinType.PinCategory == TargetPin->PinType.PinCategory;
}