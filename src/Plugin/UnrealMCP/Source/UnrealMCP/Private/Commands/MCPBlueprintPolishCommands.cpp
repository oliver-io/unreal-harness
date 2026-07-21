#include "Commands/MCPBlueprintPolishCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"

namespace
{
    UBlueprint* LoadBlueprintFlexible(const FString& BlueprintRef)
    {
        // Accept asset path ("/Game/Foo/BP_Bar") or short name ("BP_Bar").
        if (UBlueprint* BP = FMCPCommonUtils::FindBlueprint(BlueprintRef))
        {
            return BP;
        }
        if (UObject* Loaded = UEditorAssetLibrary::LoadAsset(BlueprintRef))
        {
            return Cast<UBlueprint>(Loaded);
        }
        return nullptr;
    }

    UEdGraphNode* FindGraphNodeById(UEdGraph* Graph, const FString& NodeId)
    {
        if (!Graph) return nullptr;
        for (UEdGraphNode* N : Graph->Nodes)
        {
            if (!N) continue;
            if (N->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase)) return N;
            if (N->GetName().Equals(NodeId, ESearchCase::IgnoreCase))           return N;
        }
        // GAP-066 fallback: unique node TITLE (refuse on ambiguity).
        UEdGraphNode* TitleMatch = nullptr;
        int32 TitleMatchCount = 0;
        for (UEdGraphNode* N : Graph->Nodes)
        {
            if (!N) continue;
            const FString Title = N->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
            const FString ListTitle = N->GetNodeTitle(ENodeTitleType::ListView).ToString();
            if (Title.Equals(NodeId, ESearchCase::IgnoreCase) || ListTitle.Equals(NodeId, ESearchCase::IgnoreCase))
            {
                ++TitleMatchCount;
                TitleMatch = N;
            }
        }
        if (TitleMatchCount == 1) return TitleMatch;
        return nullptr;
    }

    // Returns whether the on-disk save actually persisted. SaveAsset no-ops
    // (returns false) under PIE / -unattended / a read-only package; the callers
    // surface that instead of reporting a save that never happened as success.
    bool CompileAndSaveBlueprint(UBlueprint* BP)
    {
        if (!BP) return false;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        FKismetEditorUtilities::CompileBlueprint(BP);
        BP->MarkPackageDirty();
        return UEditorAssetLibrary::SaveAsset(BP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    }
}

TSharedPtr<FJsonObject> FMCPBlueprintPolishCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_remove_component"))   return HandleRemoveComponent(Params);
    if (CommandType == TEXT("bp_disconnect_pin"))     return HandleDisconnectPin(Params);
    if (CommandType == TEXT("bp_get_parent_class"))   return HandleGetParentClass(Params);
    if (CommandType == TEXT("bp_list_components"))    return HandleListComponents(Params);

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown blueprint polish command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported in this build: bp_remove_component, bp_disconnect_pin, bp_get_parent_class, bp_list_components."));
}

TSharedPtr<FJsonObject> FMCPBlueprintPolishCommands::HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintRef;
    if (!Params->TryGetStringField(TEXT("bp_path"), BlueprintRef) &&
        !Params->TryGetStringField(TEXT("blueprint_name"), BlueprintRef))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'bp_path' (or legacy 'blueprint_name') parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the Blueprint asset path or short name."));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'component_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the SCS variable name of the component to remove. bp_list_components surfaces them."));
    }

    if (ComponentName.Len() >= NAME_SIZE)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'component_name' is too long (%d chars; FName max is %d)"), ComponentName.Len(), NAME_SIZE - 1),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the SCS variable name of the component; an over-length name cannot be a valid FName."));
    }

    bool bReparentChildren = true;
    Params->TryGetBoolField(TEXT("reparent_children"), bReparentChildren);

    UBlueprint* BP = LoadBlueprintFlexible(BlueprintRef);
    if (!BP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintRef),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the path. fixup_redirectors may help if the asset was renamed."));
    }
    if (!BP->SimpleConstructionScript)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint has no SimpleConstructionScript: %s"), *BlueprintRef),
            EMCPErrorCode::UnsupportedClass,
            TEXT("This Blueprint kind does not support components (e.g. UAnimBlueprint, UWidgetBlueprint without a UUserWidget parent)."));
    }

    USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*ComponentName));
    if (!Node)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component not found on Blueprint: %s"), *ComponentName),
            EMCPErrorCode::NodeNotFound,
            TEXT("bp_list_components shows the current SCS variable names."));
    }

    const TArray<USCS_Node*> Children = Node->GetChildNodes();
    if (Children.Num() > 0 && !bReparentChildren)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component '%s' has %d child(ren); refused with reparent_children=false"), *ComponentName, Children.Num()),
            EMCPErrorCode::WouldBreakReferences,
            TEXT("Either pass reparent_children=true (default) or remove the children first to avoid orphans."));
    }

    // dry_run: validation already ran (BP, SCS, node lookup, would-orphan check).
    // Diff shape per todo/13 phase 2: components_removed entries — name, class,
    // child_count, child_names (so the agent sees what would be promoted), and
    // would_promote_children flag (true when children exist + reparent_children).
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), ComponentName);
        if (Node->ComponentClass)
        {
            Entry->SetStringField(TEXT("class"), Node->ComponentClass->GetPathName());
        }
        Entry->SetNumberField(TEXT("child_count"), Children.Num());

        TArray<TSharedPtr<FJsonValue>> ChildNamesArr;
        for (USCS_Node* Child : Children)
        {
            if (Child)
            {
                ChildNamesArr.Add(MakeShared<FJsonValueString>(Child->GetVariableName().ToString()));
            }
        }
        Entry->SetArrayField(TEXT("child_names"), ChildNamesArr);
        Entry->SetBoolField(TEXT("would_promote_children"), bReparentChildren && Children.Num() > 0);

        if (Node->ParentComponentOrVariableName != NAME_None)
        {
            Entry->SetStringField(TEXT("parent_component"), Node->ParentComponentOrVariableName.ToString());
        }

        TArray<TSharedPtr<FJsonValue>> RemovedArr;
        RemovedArr.Add(MakeShared<FJsonValueObject>(Entry));

        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("components_removed"), RemovedArr);
        TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
        Wrapped->SetBoolField(TEXT("reparent_children"), bReparentChildren);
        Wrapped->SetNumberField(TEXT("children_to_promote"), bReparentChildren ? Children.Num() : 0);
        return Wrapped;
    }

    if (bReparentChildren)
    {
        BP->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);
    }
    else
    {
        BP->SimpleConstructionScript->RemoveNode(Node);
    }

    if (!CompileAndSaveBlueprint(BP))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Component was removed and the Blueprint recompiled in-memory, but the asset save did not persist to disk"),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — typically PIE is active, the editor was launched -unattended, or the package file is read-only. Stop PIE / relaunch a normal editor / clear the read-only flag, then retry."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("bp_path"), BP->GetPathName());
    ResultObj->SetStringField(TEXT("removed_component"), ComponentName);
    ResultObj->SetBoolField(TEXT("reparent_children"), bReparentChildren);
    ResultObj->SetNumberField(TEXT("children_promoted"), bReparentChildren ? Children.Num() : 0);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintPolishCommands::HandleDisconnectPin(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintRef;
    if (!Params->TryGetStringField(TEXT("bp_path"), BlueprintRef) &&
        !Params->TryGetStringField(TEXT("blueprint_name"), BlueprintRef))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'bp_path' (or legacy 'blueprint_name') parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the Blueprint asset path or short name."));
    }

    FString NodeId, PinName;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'node_id' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the node GUID (or its name); analyze_blueprint_graph surfaces the IDs."));
    }
    if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'pin_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pin names visible via list_node_pins."));
    }

    FString TargetNodeId, TargetPinName;
    Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId);
    Params->TryGetStringField(TEXT("target_pin_name"), TargetPinName);
    const bool bTargetedDisconnect = !TargetNodeId.IsEmpty() && !TargetPinName.IsEmpty();

    UBlueprint* BP = LoadBlueprintFlexible(BlueprintRef);
    if (!BP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintRef),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the path."));
    }

    // GAP-035: honor graph scope like bp_connect_pins. Node ids are per-graph and
    // reused across graphs (an EventGraph and a function can both own a
    // K2Node_CallFunction_1), so an unscoped all-graph search matched the wrong node.
    // When `function_name` (or a from/to transition pair) is provided, search ONLY that
    // graph; otherwise fall back to the historical all-graph scan for compatibility.
    FString FunctionName, FromState, ToState;
    Params->TryGetStringField(TEXT("function_name"), FunctionName);
    Params->TryGetStringField(TEXT("from_state"), FromState);
    Params->TryGetStringField(TEXT("to_state"), ToState);

    TArray<UEdGraph*> AllGraphs;
    if (!FunctionName.IsEmpty() || (!FromState.IsEmpty() && !ToState.IsEmpty()))
    {
        UEdGraph* Scoped = FMCPCommonUtils::FindGraphByName(BP, FunctionName, FromState, ToState);
        if (!Scoped)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Graph not found for scope: %s"), *FunctionName),
                EMCPErrorCode::FunctionNotFound,
                TEXT("`function_name` (or from_state/to_state) did not resolve to a graph on this Blueprint. Use bp_list_graphs to enumerate, or omit the scope to search all graphs."));
        }
        AllGraphs.Add(Scoped);
    }
    else
    {
        BP->GetAllGraphs(AllGraphs);
    }

    UEdGraphNode* SourceNode = nullptr;
    for (UEdGraph* G : AllGraphs)
    {
        if (UEdGraphNode* Found = FindGraphNodeById(G, NodeId))
        {
            SourceNode = Found;
            break;
        }
    }
    if (!SourceNode)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Node not found in any graph: %s"), *NodeId),
            EMCPErrorCode::NodeNotFound,
            TEXT("Pass the node GUID or its display name. analyze_blueprint_graph lists available node IDs."));
    }

    UEdGraphPin* SourcePin = FMCPCommonUtils::FindPin(SourceNode, PinName);
    if (!SourcePin)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Pin not found on node: %s.%s"), *NodeId, *PinName),
            EMCPErrorCode::PinNotFound,
            TEXT("list_node_pins shows the available pins on a node."));
    }

    int32 SeveredCount = 0;
    if (bTargetedDisconnect)
    {
        // Find peer node + pin and break only that one link.
        UEdGraphNode* TargetNode = nullptr;
        for (UEdGraph* G : AllGraphs)
        {
            if (UEdGraphNode* Found = FindGraphNodeById(G, TargetNodeId))
            {
                TargetNode = Found;
                break;
            }
        }
        if (!TargetNode)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId),
                EMCPErrorCode::NodeNotFound,
                TEXT("Verify the target node id."));
        }
        UEdGraphPin* TargetPin = FMCPCommonUtils::FindPin(TargetNode, TargetPinName);
        if (!TargetPin)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Target pin not found: %s.%s"), *TargetNodeId, *TargetPinName),
                EMCPErrorCode::PinNotFound,
                TEXT("Verify the target pin name."));
        }
        const bool bWouldSever = SourcePin->LinkedTo.Contains(TargetPin);
        if (FMCPCommonUtils::ParseDryRun(Params))
        {
            // Targeted dry-run: emit single-link diff if it exists, empty if not (still successful, just a no-op).
            TArray<TSharedPtr<FJsonValue>> RemovedArr;
            if (bWouldSever)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("source_node"), NodeId);
                Entry->SetStringField(TEXT("source_pin"),  PinName);
                Entry->SetStringField(TEXT("target_node"), TargetNodeId);
                Entry->SetStringField(TEXT("target_pin"),  TargetPinName);
                Entry->SetStringField(TEXT("connection_type"), SourcePin->PinType.PinCategory.ToString());
                RemovedArr.Add(MakeShared<FJsonValueObject>(Entry));
            }
            TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
            Diff->SetArrayField(TEXT("connections_removed"), RemovedArr);
            TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
            Wrapped->SetBoolField(TEXT("targeted"), true);
            Wrapped->SetNumberField(TEXT("links_severed"), bWouldSever ? 1 : 0);
            return Wrapped;
        }
        if (bWouldSever)
        {
            SourcePin->BreakLinkTo(TargetPin);
            SeveredCount = 1;
        }
        // else: nothing to do — count stays 0; not an error per the doc spec
        // (a "no-op disconnect" is informational, not a failure).
    }
    else
    {
        if (FMCPCommonUtils::ParseDryRun(Params))
        {
            // Broad dry-run: enumerate every link the BreakAllPinLinks call
            // would sever. Each row carries the peer node id + pin name so
            // the agent can reconstruct the wiring being removed.
            TArray<TSharedPtr<FJsonValue>> RemovedArr;
            for (UEdGraphPin* PeerPin : SourcePin->LinkedTo)
            {
                if (!PeerPin) continue;
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("source_node"), NodeId);
                Entry->SetStringField(TEXT("source_pin"),  PinName);
                if (PeerPin->GetOwningNode())
                {
                    Entry->SetStringField(TEXT("target_node"), PeerPin->GetOwningNode()->NodeGuid.ToString());
                }
                Entry->SetStringField(TEXT("target_pin"), PeerPin->PinName.ToString());
                Entry->SetStringField(TEXT("connection_type"), SourcePin->PinType.PinCategory.ToString());
                RemovedArr.Add(MakeShared<FJsonValueObject>(Entry));
            }
            TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
            Diff->SetArrayField(TEXT("connections_removed"), RemovedArr);
            TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
            Wrapped->SetBoolField(TEXT("targeted"), false);
            Wrapped->SetNumberField(TEXT("links_severed"), SourcePin->LinkedTo.Num());
            return Wrapped;
        }
        SeveredCount = SourcePin->LinkedTo.Num();
        SourcePin->BreakAllPinLinks();
    }

    if (!CompileAndSaveBlueprint(BP))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Pin(s) were disconnected and the Blueprint recompiled in-memory, but the asset save did not persist to disk"),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — typically PIE is active, the editor was launched -unattended, or the package file is read-only. Stop PIE / relaunch a normal editor / clear the read-only flag, then retry."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("bp_path"), BP->GetPathName());
    ResultObj->SetStringField(TEXT("node_id"), NodeId);
    ResultObj->SetStringField(TEXT("pin_name"), PinName);
    ResultObj->SetBoolField(TEXT("targeted"), bTargetedDisconnect);
    if (bTargetedDisconnect)
    {
        ResultObj->SetStringField(TEXT("target_node_id"), TargetNodeId);
        ResultObj->SetStringField(TEXT("target_pin_name"), TargetPinName);
    }
    ResultObj->SetNumberField(TEXT("links_severed"), SeveredCount);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintPolishCommands::HandleGetParentClass(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintRef;
    if (!Params->TryGetStringField(TEXT("bp_path"), BlueprintRef) &&
        !Params->TryGetStringField(TEXT("blueprint_name"), BlueprintRef))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'bp_path' (or legacy 'blueprint_name') parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the Blueprint asset path or short name."));
    }

    UBlueprint* BP = LoadBlueprintFlexible(BlueprintRef);
    if (!BP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintRef),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the path."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("bp_path"), BP->GetPathName());
    if (UClass* Parent = BP->ParentClass)
    {
        ResultObj->SetStringField(TEXT("parent_class"), Parent->GetPathName());
        ResultObj->SetStringField(TEXT("parent_class_name"), Parent->GetName());
    }
    if (UClass* Generated = BP->GeneratedClass)
    {
        ResultObj->SetStringField(TEXT("generated_class"), Generated->GetPathName());
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintPolishCommands::HandleListComponents(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintRef;
    if (!Params->TryGetStringField(TEXT("bp_path"), BlueprintRef) &&
        !Params->TryGetStringField(TEXT("blueprint_name"), BlueprintRef))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'bp_path' (or legacy 'blueprint_name') parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the Blueprint asset path or short name."));
    }

    UBlueprint* BP = LoadBlueprintFlexible(BlueprintRef);
    if (!BP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintRef),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the path."));
    }
    if (!BP->SimpleConstructionScript)
    {
        TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
        Empty->SetStringField(TEXT("bp_path"), BP->GetPathName());
        Empty->SetArrayField(TEXT("components"), TArray<TSharedPtr<FJsonValue>>());
        Empty->SetNumberField(TEXT("count"), 0);
        Empty->SetBoolField(TEXT("has_scs"), false);
        Empty->SetBoolField(TEXT("success"), true);
        return Empty;
    }

    TArray<TSharedPtr<FJsonValue>> ComponentArr;
    for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
    {
        if (!Node) continue;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
        if (Node->ComponentClass)
        {
            Entry->SetStringField(TEXT("class"), Node->ComponentClass->GetPathName());
        }
        if (Node->ParentComponentOrVariableName != NAME_None)
        {
            Entry->SetStringField(TEXT("parent_component"), Node->ParentComponentOrVariableName.ToString());
        }
        Entry->SetNumberField(TEXT("child_count"), Node->GetChildNodes().Num());
        ComponentArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("bp_path"), BP->GetPathName());
    ResultObj->SetArrayField(TEXT("components"), ComponentArr);
    ResultObj->SetNumberField(TEXT("count"), ComponentArr.Num());
    ResultObj->SetBoolField(TEXT("has_scs"), true);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
