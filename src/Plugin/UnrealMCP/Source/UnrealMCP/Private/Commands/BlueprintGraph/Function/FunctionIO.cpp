#include "Commands/BlueprintGraph/Function/FunctionIO.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CallFunction.h"
#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraphNode.h"

TSharedPtr<FJsonObject> FFunctionIO::AddFunctionIO(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with `blueprint_name`, `function_name`, `param_name`, `param_type` (and `direction` for the unified add+remove form)."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint owning the target function."));
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'function_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `function_name` (string) — the name of an existing function on the Blueprint. Use `bp_get_function_details` to discover function names."));
	}

	FString ParamName;
	if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'param_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `param_name` (string) — the name of the input/output parameter to add or remove."));
	}

	FString ParamType;
	if (!Params->TryGetStringField(TEXT("param_type"), ParamType))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'param_type' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `param_type` (string) — e.g. \"bool\", \"int\", \"float\", \"string\", \"vector\", \"rotator\", or a class path for object types. Same format `bp_create_variable` uses."));
	}

	// Get direction parameter (input or output)
	FString Direction = TEXT("input");
	Params->TryGetStringField(TEXT("direction"), Direction);

	// Validate direction
	bool bIsInput;
	if (Direction == TEXT("input"))
	{
		bIsInput = true;
	}
	else if (Direction == TEXT("output"))
	{
		bIsInput = false;
	}
	else
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid direction: must be 'input' or 'output'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `direction` (string) as exactly \"input\" or \"output\" (case-sensitive). Selects which parameter list to mutate."));
	}

	// Get optional is_array parameter
	bool bIsArray = false;
	Params->TryGetBoolField(TEXT("is_array"), bIsArray);

	// Validate parameter name
	if (!ValidateParameterName(ParamName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameter name: contains spaces or special characters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Function parameter names follow the same identifier rules as C++ variables — letters, digits, underscores; no spaces, hyphens, or punctuation. Convention: PascalCase for the public API."));
	}

	// Load the Blueprint
	UBlueprint* Blueprint = LoadBlueprint(BlueprintName);
	if (!Blueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists at the supplied name/path; use `asset_list` or `bp_create_blueprint` first."));
	}

	// Find the function graph
	UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, FunctionName);
	if (!FunctionGraph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Function not found: %s"), *FunctionName),
			EMCPErrorCode::FunctionNotFound,
			TEXT("Verify the function exists on the Blueprint. Use `bp_get_function_details` (or `bp_read` with `include_functions=true`) to discover function names."));
	}

	// Add the parameter
	if (!AddFunctionParameter(Blueprint, FunctionName, ParamName, ParamType, bIsInput, bIsArray))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to add %s parameter: %s"),
				bIsInput ? TEXT("input") : TEXT("output"), *ParamName),
			EMCPErrorCode::Internal,
			TEXT("AddFunctionParameter returned false. Common causes: param_name already exists on the function, or the param_type string couldn't be parsed into an FEdGraphPinType. Check the editor log for details."));
	}

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FunctionGraph->NotifyGraphChanged();

	return CreateSuccessResponse(ParamName, ParamType, bIsInput);
}

TSharedPtr<FJsonObject> FFunctionIO::AddFunctionInput(const TSharedPtr<FJsonObject>& Params)
{
	// Create a copy of params and add direction="input"
	TSharedPtr<FJsonObject> InputParams = MakeShareable(new FJsonObject);

	// Copy all fields from original params
	for (const auto& Field : Params->Values)
	{
		InputParams->SetField(Field.Key, Field.Value);
	}

	// Set direction to input
	InputParams->SetStringField(TEXT("direction"), TEXT("input"));

	// Call the unified function
	return AddFunctionIO(InputParams);
}

TSharedPtr<FJsonObject> FFunctionIO::AddFunctionOutput(const TSharedPtr<FJsonObject>& Params)
{
	// Create a copy of params and add direction="output"
	TSharedPtr<FJsonObject> OutputParams = MakeShareable(new FJsonObject);

	// Copy all fields from original params
	for (const auto& Field : Params->Values)
	{
		OutputParams->SetField(Field.Key, Field.Value);
	}

	// Set direction to output
	OutputParams->SetStringField(TEXT("direction"), TEXT("output"));

	// Call the unified function
	return AddFunctionIO(OutputParams);
}

TSharedPtr<FJsonObject> FFunctionIO::RemoveFunctionIO(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with `blueprint_name`, `function_name`, `param_name`, `param_type` (and `direction` for the unified add+remove form)."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint owning the target function."));
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'function_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `function_name` (string) — the name of an existing function on the Blueprint. Use `bp_get_function_details` to discover function names."));
	}

	FString ParamName;
	if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'param_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `param_name` (string) — the name of the input/output parameter to add or remove."));
	}

	// Direction is required for the symmetric remove path. AddFunctionIO defaults
	// to "input" for backward compatibility with older callers; remove is a new
	// surface so it requires the field explicitly to avoid silent mis-targeting
	// (an output remove that walks FunctionEntry would miss the pin entirely).
	FString Direction;
	if (!Params->TryGetStringField(TEXT("direction"), Direction))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'direction' parameter (must be 'input' or 'output')"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `direction` (string): \"input\" to remove from the input list, \"output\" to remove from the output list. Case-sensitive."));
	}

	bool bIsInput;
	if (Direction == TEXT("input"))
	{
		bIsInput = true;
	}
	else if (Direction == TEXT("output"))
	{
		bIsInput = false;
	}
	else
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid direction: must be 'input' or 'output'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `direction` (string) as exactly \"input\" or \"output\" (case-sensitive). Selects which parameter list to mutate."));
	}

	if (!ValidateParameterName(ParamName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameter name: contains spaces or special characters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Function parameter names follow the same identifier rules as C++ variables — letters, digits, underscores; no spaces, hyphens, or punctuation. Convention: PascalCase for the public API."));
	}

	UBlueprint* Blueprint = LoadBlueprint(BlueprintName);
	if (!Blueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists at the supplied name/path; use `asset_list` or `bp_create_blueprint` first."));
	}

	UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, FunctionName);
	if (!FunctionGraph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Function not found: %s"), *FunctionName),
			EMCPErrorCode::FunctionNotFound,
			TEXT("Verify the function exists on the Blueprint. Use `bp_get_function_details` (or `bp_read` with `include_functions=true`) to discover function names."));
	}

	FString ErrorMessage;
	if (!RemoveFunctionParameter(Blueprint, FunctionName, ParamName, bIsInput, ErrorMessage))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			ErrorMessage,
			EMCPErrorCode::Internal,
			TEXT("RemoveFunctionParameter returned false with the message above. Common causes: param_name not present on the function, or the function entry/result node couldn't be reconstructed."));
	}

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FunctionGraph->NotifyGraphChanged();

	return CreateRemoveSuccessResponse(ParamName, bIsInput);
}

TSharedPtr<FJsonObject> FFunctionIO::RemoveFunctionInput(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> InputParams = MakeShareable(new FJsonObject);
	for (const auto& Field : Params->Values)
	{
		InputParams->SetField(Field.Key, Field.Value);
	}
	InputParams->SetStringField(TEXT("direction"), TEXT("input"));
	return RemoveFunctionIO(InputParams);
}

TSharedPtr<FJsonObject> FFunctionIO::RemoveFunctionOutput(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> OutputParams = MakeShareable(new FJsonObject);
	for (const auto& Field : Params->Values)
	{
		OutputParams->SetField(Field.Key, Field.Value);
	}
	OutputParams->SetStringField(TEXT("direction"), TEXT("output"));
	return RemoveFunctionIO(OutputParams);
}

bool FFunctionIO::AddFunctionParameter(
	UBlueprint* Blueprint,
	const FString& FunctionName,
	const FString& ParamName,
	const FString& ParamType,
	bool bIsInput,
	bool bIsArray)
{
	if (!Blueprint)
	{
		return false;
	}

	// Find the function graph
	UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, FunctionName);
	if (!FunctionGraph)
	{
		return false;
	}

	// Create the property type
	FEdGraphPinType PropertyType = GetPropertyTypeFromString(ParamType);

	UEdGraphPin* NewPin = nullptr;

	if (bIsInput)
	{
		// For INPUT parameters: Find FunctionEntry and create OUTPUT pin
		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_FunctionEntry>())
			{
				EntryNode = Cast<UK2Node_FunctionEntry>(Node);
				break;
			}
		}

		if (!EntryNode)
		{
			UE_LOG(LogUnrealMCP, Warning, TEXT("Could not find FunctionEntry node in function graph '%s'"), *FunctionName);
			return false;
		}

		// Create OUTPUT pin on FunctionEntry for input parameter
		NewPin = EntryNode->CreateUserDefinedPin(*ParamName, PropertyType, EGPD_Output);
	}
	else
	{
		// For OUTPUT parameters: Find or create FunctionResult and create OUTPUT pin
		UK2Node_FunctionResult* ResultNode = nullptr;

		// First try to find existing FunctionResult node
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_FunctionResult>())
			{
				ResultNode = Cast<UK2Node_FunctionResult>(Node);
				break;
			}
		}

		if (ResultNode)
		{
			// FunctionResult already exists, add pin to it.
			// Function-output parameters live as EGPD_Input pins on the result node:
			// data flows INTO the result node from the function body. Engine convention,
			// see K2Node_FunctionResult::CanCreateUserDefinedPin (rejects EGPD_Output)
			// and CreateUserDefinedPinsForFunctionEntryExit (entry=Output, exit=Input).
			// Storing EGPD_Output here would persist a wrong DesiredPinDirection on
			// the FUserPinInfo and cause the pin to be dropped on AllocateDefaultPins
			// (called on blueprint reload / recompile), losing the output parameter.
			NewPin = ResultNode->CreateUserDefinedPin(*ParamName, PropertyType, EGPD_Input);
		}
		else
		{
			// No result node exists yet (function was previously void), construct one
			// manually. Do NOT call ResultNode->AllocateDefaultPins() — that helper
			// creates its own execute pin (see /ue5-source K2Node_FunctionResult.cpp:167-179),
			// and we create the execute pin explicitly below. Calling both would yield
			// two input-execute pins and a miscompiled graph. The execute pin must also
			// be created before PostPlacedNewNode() so downstream initialization sees it.
			ResultNode = NewObject<UK2Node_FunctionResult>(FunctionGraph);
			if (!ResultNode)
			{
				UE_LOG(LogUnrealMCP, Error, TEXT("Failed to create FunctionResult node for function '%s'"), *FunctionName);
				return false;
			}

			// Initialize the node
			ResultNode->NodePosX = 400;
			ResultNode->NodePosY = 0;
			ResultNode->CreateNewGuid();
			FunctionGraph->AddNode(ResultNode, false, false);

			// Create the execute input pin manually BEFORE PostPlacedNewNode
			// Use "execute" as the pin name (this is the standard for FunctionResult execute pin)
			UEdGraphPin* ExecutePin = ResultNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, FName(TEXT("execute")));
			if (!ExecutePin)
			{
				UE_LOG(LogUnrealMCP, Error, TEXT("Failed to create execute pin for FunctionResult in function '%s'"), *FunctionName);
				return false;
			}

			// Now call PostPlacedNewNode AFTER manually creating the execute pin
			ResultNode->PostPlacedNewNode();

			// Now add the OUTPUT pin to the FunctionResult (this will be a user-defined pin)
			NewPin = ResultNode->CreateUserDefinedPin(*ParamName, PropertyType, EGPD_Input);
		}
	}

	// Both branches above always attempt CreateUserDefinedPin and a nullptr return
	// means the live pin failed to materialize (e.g. CreatePin failed for the
	// requested type — see K2Node_FunctionResult::CreatePinFromUserDefinition at
	// /ue5-source/Editor/BlueprintGraph/Private/K2Node_FunctionResult.cpp:195-200
	// and K2Node_FunctionEntry::CreatePinFromUserDefinition at
	// /ue5-source/Editor/BlueprintGraph/Private/K2Node_FunctionEntry.cpp:461-473).
	// The legacy guard was input-only because the OUTPUT branch could previously
	// fall through without creating a result node — that invariant changed in the
	// 2026-04-22 add-function-output fix; both branches now construct (or find) a
	// ResultNode and the OUTPUT path also requires NewPin to be non-null. Treat
	// either direction's nullptr as a real failure and report it.
	if (!NewPin)
	{
		return false;
	}

	// Update the function signature
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FunctionGraph->NotifyGraphChanged();

	return true;
}

bool FFunctionIO::RemoveFunctionParameter(
	UBlueprint* Blueprint,
	const FString& FunctionName,
	const FString& ParamName,
	bool bIsInput,
	FString& OutErrorMessage)
{
	if (!Blueprint)
	{
		OutErrorMessage = TEXT("Invalid Blueprint");
		return false;
	}

	UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, FunctionName);
	if (!FunctionGraph)
	{
		OutErrorMessage = FString::Printf(TEXT("Function not found: %s"), *FunctionName);
		return false;
	}

	// Mirrors the engine's user-facing "Remove Parameter" path in
	// Editor/Kismet/Private/BlueprintDetailsCustomization.cpp around line 3849:
	//   FScopedTransaction; Node->Modify(); Node->RemoveUserDefinedPinByName(Name).
	// For OUTPUTs we walk every FunctionResult in the graph because a function
	// may have multiple return points (UK2Node_FunctionResult::GetAllResultNodes,
	// see GatherAllResultNodes at BlueprintDetailsCustomization.cpp:3376) and the
	// signature must stay consistent across all of them — otherwise the function
	// fails to compile with mismatched return-pin sets.
	const FName ParamFName(*ParamName);
	bool bRemovedAny = false;

	if (bIsInput)
	{
		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_FunctionEntry>())
			{
				EntryNode = Cast<UK2Node_FunctionEntry>(Node);
				break;
			}
		}

		if (!EntryNode)
		{
			OutErrorMessage = FString::Printf(TEXT("FunctionEntry node not found in '%s'"), *FunctionName);
			return false;
		}

		// Manual check — UK2Node_EditablePinBase::UserDefinedPinExists is not
		// DLL-exported in all engine builds, so we walk UserDefinedPins directly.
		bool bFoundOnEntry = false;
		for (const TSharedPtr<FUserPinInfo>& PinInfo : EntryNode->UserDefinedPins)
		{
			if (PinInfo.IsValid() && PinInfo->PinName == ParamFName)
			{
				bFoundOnEntry = true;
				break;
			}
		}
		if (!bFoundOnEntry)
		{
			OutErrorMessage = FString::Printf(TEXT("Input parameter '%s' not found on function '%s'"), *ParamName, *FunctionName);
			return false;
		}

		EntryNode->Modify();
		EntryNode->RemoveUserDefinedPinByName(ParamFName);
		bRemovedAny = true;
	}
	else
	{
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
			{
				bool bHasPin = false;
				for (const TSharedPtr<FUserPinInfo>& PinInfo : ResultNode->UserDefinedPins)
				{
					if (PinInfo.IsValid() && PinInfo->PinName == ParamFName)
					{
						bHasPin = true;
						break;
					}
				}
				if (bHasPin)
				{
					ResultNode->Modify();
					ResultNode->RemoveUserDefinedPinByName(ParamFName);
					bRemovedAny = true;
				}
			}
		}

		if (!bRemovedAny)
		{
			OutErrorMessage = FString::Printf(TEXT("Output parameter '%s' not found on function '%s'"), *ParamName, *FunctionName);
			return false;
		}
	}

	return bRemovedAny;
}

UBlueprint* FFunctionIO::LoadBlueprint(const FString& BlueprintName)
{
	// Try direct load
	UBlueprint* Blueprint = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *BlueprintName));
	if (Blueprint)
	{
		return Blueprint;
	}

	// Try EditorAssetLibrary
	if (UEditorAssetLibrary::DoesAssetExist(BlueprintName))
	{
		return Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(BlueprintName));
	}

	return nullptr;
}

FEdGraphPinType FFunctionIO::GetPropertyTypeFromString(const FString& TypeName)
{
	FEdGraphPinType PinType;

	if (TypeName == TEXT("bool"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (TypeName == TEXT("int") || TypeName == TEXT("int32"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (TypeName == TEXT("float"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	}
	else if (TypeName == TEXT("string"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (TypeName == TEXT("vector") || TypeName == TEXT("FVector"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (TypeName == TEXT("rotator") || TypeName == TEXT("FRotator"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (TypeName == TEXT("object"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
	}
	else
	{
		// Default to object for unknown types
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
	}

	return PinType;
}

UEdGraph* FFunctionIO::FindFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName)
{
	if (!Blueprint || FunctionName.IsEmpty())
	{
		return nullptr;
	}

	// Search in all function graphs
	// Try exact name match first
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			FString GraphName = Graph->GetFName().ToString();
			// Try exact match
			if (GraphName == FunctionName)
			{
				return Graph;
			}
			// Try matching the outer name (function name)
			if (Graph->GetOuter() && Graph->GetOuter()->GetFName().ToString() == FunctionName)
			{
				return Graph;
			}
		}
	}

	// If not found, try to find by searching all graphs more broadly
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName().ToString().Contains(FunctionName))
		{
			return Graph;
		}
	}

	return nullptr;
}

bool FFunctionIO::ValidateParameterName(const FString& ParamName)
{
	if (ParamName.IsEmpty())
	{
		return false;
	}

	// Names longer than the FName cap (NAME_SIZE = 1024) fatal in HashLowerCase
	// (UnrealNames.cpp:1064, UE_CLOG(Len > NAME_SIZE, Fatal)) when the pin FName is
	// built downstream (CreateUserDefinedPin / FName(*ParamName)). Reject first.
	if (ParamName.Len() > NAME_SIZE)
	{
		return false;
	}

	// Check for spaces or invalid characters
	for (TCHAR Char : ParamName)
	{
		if (FChar::IsWhitespace(Char) || (!FChar::IsAlnum(Char) && Char != TEXT('_')))
		{
			return false;
		}
	}

	// Must start with letter or underscore
	if (!FChar::IsAlpha(ParamName[0]) && ParamName[0] != TEXT('_'))
	{
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> FFunctionIO::CreateSuccessResponse(const FString& ParamName, const FString& ParamType, bool bIsInput)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("param_name"), ParamName);
	Response->SetStringField(TEXT("param_type"), ParamType);
	Response->SetStringField(TEXT("direction"), bIsInput ? TEXT("input") : TEXT("output"));
	return Response;
}

TSharedPtr<FJsonObject> FFunctionIO::CreateRemoveSuccessResponse(const FString& ParamName, bool bIsInput)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("param_name"), ParamName);
	Response->SetStringField(TEXT("direction"), bIsInput ? TEXT("input") : TEXT("output"));
	Response->SetBoolField(TEXT("removed"), true);
	return Response;
}

// Local CreateErrorResponse helper removed 2026-05-10 (doc 1 migration). All
// error paths now route through FMCPCommonUtils::CreateErrorResponse
// with explicit (Message, EMCPErrorCode, Hint) so error_code lands on the wire.
