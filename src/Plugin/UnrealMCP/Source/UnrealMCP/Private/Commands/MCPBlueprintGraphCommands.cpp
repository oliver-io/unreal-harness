#include "Commands/MCPBlueprintGraphCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "Commands/BlueprintGraph/NodeManager.h"
#include "Commands/BlueprintGraph/BPConnector.h"
#include "Commands/BlueprintGraph/BPVariables.h"
#include "Commands/BlueprintGraph/EventManager.h"
#include "Commands/BlueprintGraph/NodeDeleter.h"
#include "Commands/BlueprintGraph/NodePropertyManager.h"
#include "Commands/BlueprintGraph/Function/FunctionManager.h"
#include "Commands/BlueprintGraph/Function/FunctionIO.h"
#include "Commands/BlueprintGraph/DispatcherManager.h"

FMCPBlueprintGraphCommands::FMCPBlueprintGraphCommands()
{
}

FMCPBlueprintGraphCommands::~FMCPBlueprintGraphCommands()
{
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("add_blueprint_node"))
    {
        return HandleAddBlueprintNode(Params);
    }
    else if (CommandType == TEXT("bp_connect_pins"))
    {
        return HandleConnectNodes(Params);
    }
    else if (CommandType == TEXT("bp_create_variable"))
    {
        return HandleCreateVariable(Params);
    }
    else if (CommandType == TEXT("bp_set_variable_properties"))
    {
        return HandleSetVariableProperties(Params);
    }
    else if (CommandType == TEXT("bp_delete_variable"))
    {
        return HandleDeleteVariable(Params);
    }
    else if (CommandType == TEXT("bp_add_event_node"))
    {
        return HandleAddEventNode(Params);
    }
    else if (CommandType == TEXT("bp_add_custom_event"))
    {
        return HandleAddCustomEventNode(Params);
    }
    else if (CommandType == TEXT("bp_delete_node"))
    {
        return HandleDeleteNode(Params);
    }
    else if (CommandType == TEXT("bp_set_node_property"))
    {
        return HandleSetNodeProperty(Params);
    }
    else if (CommandType == TEXT("bp_create_function"))
    {
        return HandleCreateFunction(Params);
    }
    else if (CommandType == TEXT("bp_add_function_input"))
    {
        return HandleAddFunctionInput(Params);
    }
    else if (CommandType == TEXT("bp_add_function_output"))
    {
        return HandleAddFunctionOutput(Params);
    }
    else if (CommandType == TEXT("bp_remove_function_input"))
    {
        return HandleRemoveFunctionInput(Params);
    }
    else if (CommandType == TEXT("bp_remove_function_output"))
    {
        return HandleRemoveFunctionOutput(Params);
    }
    else if (CommandType == TEXT("bp_delete_function"))
    {
        return HandleDeleteFunction(Params);
    }
    else if (CommandType == TEXT("bp_rename_function"))
    {
        return HandleRenameFunction(Params);
    }
    else if (CommandType == TEXT("bp_create_dispatcher"))
    {
        return HandleCreateDispatcher(Params);
    }

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown blueprint graph command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Unrecognized command name. Valid blueprint-graph commands: `add_blueprint_node`, `connect_nodes`, `create_variable`, `set_blueprint_variable_properties`, `delete_variable`, `add_event_node`, `delete_node`, `set_node_property`, `create_function`, `add_function_input`, `add_function_output`, `remove_function_input`, `remove_function_output`, `delete_function`, `rename_function`."));
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleAddBlueprintNode(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString NodeType;
    if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'node_type' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`node_type` is required — the K2Node class or function-call signature to spawn. Examples: `K2Node_VariableGet`, `K2Node_CallFunction`, or a function name like `PrintString`."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleAddBlueprintNode: Adding %s node to blueprint '%s'"), *NodeType, *BlueprintName);

    // Use the NodeManager to add the node
    return FBlueprintNodeManager::AddNode(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleConnectNodes(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString SourceNodeId;
    if (!Params->TryGetStringField(TEXT("source_node_id"), SourceNodeId))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'source_node_id' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`source_node_id` is required — the GUID-suffixed name of the source node whose pin to wire from. Use `read_blueprint_content` to list node ids."));
    }

    FString SourcePinName;
    if (!Params->TryGetStringField(TEXT("source_pin_name"), SourcePinName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'source_pin_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`source_pin_name` is required — the output pin on the source node to wire from. Use `list_node_pins` to enumerate pin names for a node."));
    }

    FString TargetNodeId;
    if (!Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'target_node_id' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`target_node_id` is required — the GUID-suffixed name of the target node whose pin to wire to. Use `read_blueprint_content` to list node ids."));
    }

    FString TargetPinName;
    if (!Params->TryGetStringField(TEXT("target_pin_name"), TargetPinName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'target_pin_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`target_pin_name` is required — the input pin on the target node to wire to. Use `list_node_pins` to enumerate pin names for a node."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleConnectNodes: Connecting %s.%s to %s.%s in blueprint '%s'"),
        *SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName, *BlueprintName);

    // Use the BPConnector to connect the nodes
    return FBPConnector::ConnectNodes(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleCreateVariable(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'variable_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`variable_name` is required — the member variable on the Blueprint to target. Use `read_blueprint_content` to enumerate variable names."));
    }

    FString VariableType;
    if (!Params->TryGetStringField(TEXT("variable_type"), VariableType))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'variable_type' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`variable_type` is required — the variable's C++ type. Built-ins: `bool`, `int`, `int64`, `float`, `double`, `name`, `string`, `text`, `vector`, `rotator`, `transform`, `byte`. For asset/object types pass the full path (e.g. `/Script/Engine.Actor`)."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleCreateVariable: Creating %s variable '%s' in blueprint '%s'"),
        *VariableType, *VariableName, *BlueprintName);

    // Use the BPVariables to create the variable
    return FBPVariables::CreateVariable(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleDeleteVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'variable_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`variable_name` is required — the member variable on the Blueprint to target. Use `read_blueprint_content` to enumerate variable names."));
    }

    UE_LOG(LogUnrealMCP, Display, TEXT("HandleDeleteVariable: Deleting variable '%s' from blueprint '%s'"),
        *VariableName, *BlueprintName);

    return FBPVariables::DeleteVariable(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleSetVariableProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'variable_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`variable_name` is required — the member variable on the Blueprint to target. Use `read_blueprint_content` to enumerate variable names."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleSetVariableProperties: Modifying variable '%s' in blueprint '%s'"),
        *VariableName, *BlueprintName);

    // Use the BPVariables to set the variable properties
    return FBPVariables::SetVariableProperties(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleAddEventNode(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'event_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`event_name` is required — the event node to add (e.g. `BeginPlay`, `Tick`, `ActorBeginOverlap`, or a custom event name). Use `read_blueprint_content` to see existing events."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleAddEventNode: Adding event '%s' to blueprint '%s'"),
        *EventName, *BlueprintName);

    // Use the EventManager to add the event node
    return FEventManager::AddEventNode(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleAddCustomEventNode(const TSharedPtr<FJsonObject>& Params)
{
    // Thin dispatch to FEventManager::AddCustomEventNode (full validation lives there,
    // mirroring HandleAddEventNode). Authors a fresh UK2Node_CustomEvent — GAP-055,
    // the create-side companion to bp_set_event_replication.
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path."));
    }

    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'event_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`event_name` is required — the new custom event's name (becomes UK2Node_CustomEvent CustomFunctionName)."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleAddCustomEventNode: Adding custom event '%s' to blueprint '%s'"),
        *EventName, *BlueprintName);

    return FEventManager::AddCustomEventNode(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleDeleteNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString NodeID;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeID))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'node_id' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`node_id` is required — the GUID-suffixed node name (e.g. `K2Node_VariableGet_0`). Use `read_blueprint_content` or `analyze_blueprint_graph` to list node ids."));
    }

    UE_LOG(LogUnrealMCP, Display,
        TEXT("FMCPBlueprintGraphCommands::HandleDeleteNode: Deleting node '%s' from blueprint '%s'"),
        *NodeID, *BlueprintName);

    return FNodeDeleter::DeleteNode(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleSetNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString NodeID;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeID))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'node_id' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`node_id` is required — the GUID-suffixed node name (e.g. `K2Node_VariableGet_0`). Use `read_blueprint_content` or `analyze_blueprint_graph` to list node ids."));
    }

    // Check if this is semantic mode (action parameter) or legacy mode (property_name)
    bool bHasAction = Params->HasField(TEXT("action"));

    if (bHasAction)
    {
        // Semantic mode - delegate directly to SetNodeProperty
        FString Action;
        Params->TryGetStringField(TEXT("action"), Action);
        UE_LOG(LogUnrealMCP, Display,
            TEXT("FMCPBlueprintGraphCommands::HandleSetNodeProperty: Semantic mode - action '%s' on node '%s' in blueprint '%s'"),
            *Action, *NodeID, *BlueprintName);
    }
    else
    {
        // Legacy mode - require property_name
        FString PropertyName;
        if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'property_name' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Legacy `set_node_property` mode requires `property_name` (the K2Node UPROPERTY to write). Prefer the new semantic `action` mode (pass `action` instead). Use `get_class_properties` on the node's class to list valid property names."));
        }

        UE_LOG(LogUnrealMCP, Display,
            TEXT("FMCPBlueprintGraphCommands::HandleSetNodeProperty: Legacy mode - Setting '%s' on node '%s' in blueprint '%s'"),
            *PropertyName, *NodeID, *BlueprintName);
    }

    return FNodePropertyManager::SetNodeProperty(Params);
}


TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleCreateFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`function_name` is required — the user-function on the Blueprint to target. Use `read_blueprint_content` or `list_blueprint_graphs` to enumerate function names."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleCreateFunction: Creating function '%s' in blueprint '%s'"),
        *FunctionName, *BlueprintName);

    return FFunctionManager::CreateFunction(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleAddFunctionInput(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`function_name` is required — the user-function on the Blueprint to target. Use `read_blueprint_content` or `list_blueprint_graphs` to enumerate function names."));
    }

    FString ParamName;
    if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'param_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`param_name` is required — the input/output parameter on the function to target. Use `get_blueprint_function_details` to enumerate parameter names."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleAddFunctionInput: Adding input '%s' to function '%s' in blueprint '%s'"),
        *ParamName, *FunctionName, *BlueprintName);

    return FFunctionIO::AddFunctionInput(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleAddFunctionOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`function_name` is required — the user-function on the Blueprint to target. Use `read_blueprint_content` or `list_blueprint_graphs` to enumerate function names."));
    }

    FString ParamName;
    if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'param_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`param_name` is required — the input/output parameter on the function to target. Use `get_blueprint_function_details` to enumerate parameter names."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleAddFunctionOutput: Adding output '%s' to function '%s' in blueprint '%s'"),
        *ParamName, *FunctionName, *BlueprintName);

    return FFunctionIO::AddFunctionOutput(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleRemoveFunctionInput(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`function_name` is required — the user-function on the Blueprint to target. Use `read_blueprint_content` or `list_blueprint_graphs` to enumerate function names."));
    }

    FString ParamName;
    if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'param_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`param_name` is required — the input/output parameter on the function to target. Use `get_blueprint_function_details` to enumerate parameter names."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleRemoveFunctionInput: Removing input '%s' from function '%s' in blueprint '%s'"),
        *ParamName, *FunctionName, *BlueprintName);

    return FFunctionIO::RemoveFunctionInput(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleRemoveFunctionOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`function_name` is required — the user-function on the Blueprint to target. Use `read_blueprint_content` or `list_blueprint_graphs` to enumerate function names."));
    }

    FString ParamName;
    if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'param_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`param_name` is required — the input/output parameter on the function to target. Use `get_blueprint_function_details` to enumerate parameter names."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleRemoveFunctionOutput: Removing output '%s' from function '%s' in blueprint '%s'"),
        *ParamName, *FunctionName, *BlueprintName);

    return FFunctionIO::RemoveFunctionOutput(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleDeleteFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`function_name` is required — the user-function on the Blueprint to target. Use `read_blueprint_content` or `list_blueprint_graphs` to enumerate function names."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleDeleteFunction: Deleting function '%s' from blueprint '%s'"),
        *FunctionName, *BlueprintName);

    return FFunctionManager::DeleteFunction(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleRenameFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString OldFunctionName;
    if (!Params->TryGetStringField(TEXT("old_function_name"), OldFunctionName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'old_function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`old_function_name` is required — the existing user-function name to rename. Use `read_blueprint_content` or `list_blueprint_graphs` to enumerate function names."));
    }

    FString NewFunctionName;
    if (!Params->TryGetStringField(TEXT("new_function_name"), NewFunctionName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'new_function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`new_function_name` is required — the target name for the rename. Must be unique within the Blueprint's user-function namespace."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleRenameFunction: Renaming function '%s' to '%s' in blueprint '%s'"),
        *OldFunctionName, *NewFunctionName, *BlueprintName);

    return FFunctionManager::RenameFunction(Params);
}

TSharedPtr<FJsonObject> FMCPBlueprintGraphCommands::HandleCreateDispatcher(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — pass the Blueprint asset's short name (e.g. `BP_Player`) or full asset path. Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString DispatcherName;
    if (!Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'dispatcher_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`dispatcher_name` is required — a new identifier for the event dispatcher to author on the Blueprint."));
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("FMCPBlueprintGraphCommands::HandleCreateDispatcher: Creating dispatcher '%s' in blueprint '%s'"),
        *DispatcherName, *BlueprintName);

    return FDispatcherManager::CreateDispatcher(Params);
}
