#include "Commands/BlueprintGraph/Function/FunctionManager.h"
#include "Commands/BlueprintGraph/Function/FunctionIO.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"

TSharedPtr<FJsonObject> FFunctionManager::CreateFunction(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with `blueprint_name` and the operation-specific function-name field (`function_name` for create/delete, `old_function_name` + `new_function_name` for rename)."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint."));
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'function_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `function_name` (string). For create: a new identifier following C++ naming rules. For delete: an existing function name discoverable via `bp_get_function_details`."));
	}

	// Get optional return type (default: void)
	FString ReturnType = TEXT("void");
	Params->TryGetStringField(TEXT("return_type"), ReturnType);

	// Validate function name
	if (!ValidateFunctionName(FunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid function name: contains spaces or special characters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Function names follow C++ identifier rules — letters, digits, underscores; no spaces, hyphens, or punctuation. Convention: PascalCase."));
	}

	// Load the Blueprint
	UBlueprint* Blueprint = LoadBlueprint(BlueprintName);
	if (!Blueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists; use `asset_list` or `bp_create_blueprint` first."));
	}

	// Check if function already exists
	if (FunctionExists(Blueprint, FunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Function already exists: %s"), *FunctionName),
			EMCPErrorCode::NameCollision,
			TEXT("Pick a different `function_name`, or use `bp_delete_function` to remove the existing function first."));
	}

	// Store current graph count to track which graph was created
	int32 GraphCountBefore = Blueprint->FunctionGraphs.Num();

	// Create the function using FBlueprintEditorUtils
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!NewGraph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create function graph"),
			EMCPErrorCode::Internal,
			TEXT("FBlueprintEditorUtils::CreateNewGraph returned null. Common cause: function_name passed validation but the engine rejected it (reserved name, K2 schema rule). Check the editor log."));
	}

	// Add the function to the Blueprint (bIsUserCreated = true)
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, true, nullptr);

	// Verify that FunctionEntry and FunctionResult nodes were created correctly
	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;

	// Find nodes directly in the graph (more reliable than TObjectIterator)
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		if (Node)
		{
			if (Node->IsA<UK2Node_FunctionEntry>())
			{
				EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			}
			else if (Node->IsA<UK2Node_FunctionResult>())
			{
				ResultNode = Cast<UK2Node_FunctionResult>(Node);
			}
		}
	}

	// A missing FunctionResult node is the normal state for a freshly created
	// void function — no engine hook creates one in response to entry-pin
	// mutations (K2Node_FunctionEntry::CreateUserDefinedPin / RemoveUserDefinedPin
	// / PostReconstructNode / NotifyGraphChanged spawn no sibling nodes). The
	// node is created on demand by FFunctionIO::AddFunctionParameter's output
	// branch (FunctionIO.cpp), which builds it manually via
	// NewObject<UK2Node_FunctionResult> when the first output parameter lands.
	// (A transient add-then-remove "__DummyOutput" pin dance lived here until
	// 2026-06-11 under the false belief it forced FunctionResult creation; it
	// was a no-op for that purpose and is removed.)
	(void)EntryNode;
	(void)ResultNode;

	// GAP-033: honor return_type. A freshly created function has only a
	// FunctionEntry node; the return_type arg was previously parsed but ignored, so
	// the caller had to follow up with bp_add_function_output. Reuse the public
	// AddFunctionIO path (direction="output") — it builds the FunctionResult node on
	// demand — to add a "ReturnValue" output pin of the requested type when non-void.
	if (!ReturnType.IsEmpty() && !ReturnType.Equals(TEXT("void"), ESearchCase::IgnoreCase))
	{
		TSharedPtr<FJsonObject> OutParams = MakeShared<FJsonObject>();
		OutParams->SetStringField(TEXT("blueprint_name"), BlueprintName);
		OutParams->SetStringField(TEXT("function_name"), FunctionName);
		OutParams->SetStringField(TEXT("param_name"), TEXT("ReturnValue"));
		OutParams->SetStringField(TEXT("param_type"), ReturnType);
		OutParams->SetStringField(TEXT("direction"), TEXT("output"));
		TSharedPtr<FJsonObject> AddResult = FFunctionIO::AddFunctionIO(OutParams);
		const bool bAdded = AddResult.IsValid() && AddResult->HasField(TEXT("param_name"));
		if (!bAdded)
		{
			UE_LOG(LogUnrealMCP, Warning,
				TEXT("CreateFunction: return_type '%s' could not be applied to '%s' (unparseable type?); function created without a return value."),
				*ReturnType, *FunctionName);
		}
	}

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Compile the Blueprint AFTER verifying nodes (like GenBlueprintUtils does)
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// Get the actual graph name that was created
	FString ActualGraphName = NewGraph->GetFName().ToString();
	if (Blueprint->FunctionGraphs.Num() > GraphCountBefore)
	{
		// Use the newly added graph's name
		UEdGraph* CreatedGraph = Blueprint->FunctionGraphs[Blueprint->FunctionGraphs.Num() - 1];
		if (CreatedGraph)
		{
			ActualGraphName = CreatedGraph->GetFName().ToString();
		}
	}

	UE_LOG(LogUnrealMCP, Display, TEXT("Successfully created function '%s' with internal name '%s' in %s"), *FunctionName, *ActualGraphName, *BlueprintName);

	return CreateSuccessResponse(FunctionName, ActualGraphName);
}

TSharedPtr<FJsonObject> FFunctionManager::DeleteFunction(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with `blueprint_name` and the operation-specific function-name field (`function_name` for create/delete, `old_function_name` + `new_function_name` for rename)."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint."));
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'function_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `function_name` (string). For create: a new identifier following C++ naming rules. For delete: an existing function name discoverable via `bp_get_function_details`."));
	}

	// Load the Blueprint
	UBlueprint* Blueprint = LoadBlueprint(BlueprintName);
	if (!Blueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists; use `asset_list` or `bp_create_blueprint` first."));
	}

	// Resolve the function graph first so the system-function guard below
	// can inspect the RESOLVED graph's internal name rather than the caller-
	// supplied string. FindFunctionGraph has a substring-match strategy
	// (Strategy 3, Contains), so any substring of a system graph's name
	// ("ConstructionScript", "User", "Anim", etc.) could otherwise resolve
	// to a system graph while bypassing a string-literal guard on the
	// input. Checking the resolved graph's FName is robust to all lookup
	// strategies. See IsReservedSystemGraph() below for the engine
	// constants and rationale.
	UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, FunctionName);
	if (!FunctionGraph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Function not found: %s"), *FunctionName),
			EMCPErrorCode::FunctionNotFound,
			TEXT("Verify the function exists. Use `bp_get_function_details` (or `bp_read` with `include_functions=true`) to discover function names."));
	}

	// Prevent deletion of system functions
	const FString ResolvedName = FunctionGraph->GetFName().ToString();
	if (IsReservedSystemGraph(ResolvedName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Cannot delete system function: %s"), *ResolvedName),
			EMCPErrorCode::FeatureDisabled,
			TEXT("System / inherited functions (UserConstructionScript, BeginPlay overrides, etc.) cannot be deleted from a Blueprint — they belong to the parent class. Only BP-authored functions are deletable."));
	}

	FBlueprintEditorUtils::RemoveGraph(Blueprint, FunctionGraph);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogUnrealMCP, Display, TEXT("Successfully deleted function '%s' from %s"), *FunctionName, *BlueprintName);

	return CreateSuccessResponse(FunctionName);
}

TSharedPtr<FJsonObject> FFunctionManager::RenameFunction(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with `blueprint_name` and the operation-specific function-name field (`function_name` for create/delete, `old_function_name` + `new_function_name` for rename)."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint."));
	}

	FString OldFunctionName;
	if (!Params->TryGetStringField(TEXT("old_function_name"), OldFunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'old_function_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `old_function_name` (string) — the existing function name to rename. Use `bp_get_function_details` to discover."));
	}

	FString NewFunctionName;
	if (!Params->TryGetStringField(TEXT("new_function_name"), NewFunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'new_function_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `new_function_name` (string) — the new identifier following C++ naming rules."));
	}

	// Validate new function name
	if (!ValidateFunctionName(NewFunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid function name: contains spaces or special characters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Function names follow C++ identifier rules — letters, digits, underscores; no spaces, hyphens, or punctuation. Convention: PascalCase."));
	}

	// Load the Blueprint
	UBlueprint* Blueprint = LoadBlueprint(BlueprintName);
	if (!Blueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists; use `asset_list` or `bp_create_blueprint` first."));
	}

	// Resolve the source graph first so the system-function guard below
	// can inspect the RESOLVED name (parallel to DeleteFunction). The
	// previous `FunctionExists` lookup is a thin wrapper over the same
	// FindFunctionGraph call, so collapsing them avoids a duplicate
	// search and lets the guard use the resolved FName directly.
	UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, OldFunctionName);
	if (!FunctionGraph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Function not found: %s"), *OldFunctionName),
			EMCPErrorCode::FunctionNotFound,
			TEXT("Verify `old_function_name` exists on the BP. Use `bp_get_function_details` to discover names."));
	}

	// Prevent renaming of system functions. Engine-side
	// FBlueprintEditorUtils::RenameGraph
	// (BlueprintEditorUtils.cpp:2582-2738 in UE 5.7) has no reserved-name
	// safety net — it unconditionally renames the graph and rewires every
	// FunctionEntry/FunctionResult/CallFunction reference to the new name
	// (lines 2606-2731). Renaming the construction script or AnimGraph
	// silently corrupts the blueprint: the engine looks up these graphs
	// by their canonical FName, so post-rename the construction-script
	// hook stops firing and AnimBP compilation can't locate its root
	// pose graph. The editor UI normally filters this via the per-graph
	// `bAllowRenaming = false` flag (set on AnimGraph at Kismet2.cpp:503
	// and on the construction script at BlueprintEditor.cpp:1961-area
	// adjacent calls), but RenameGraph itself ignores the flag — only
	// the right-click menu in BlueprintEditorUtils.cpp:9446 reads it.
	// MCP has no menu, so the guard lives here.
	const FString ResolvedOldName = FunctionGraph->GetFName().ToString();
	if (IsReservedSystemGraph(ResolvedOldName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Cannot rename system function: %s"), *ResolvedOldName),
			EMCPErrorCode::FeatureDisabled,
			TEXT("System / inherited functions cannot be renamed — they belong to the parent class. Only BP-authored functions are renamable."));
	}

	// Check if new name already exists. NOTE: this uses FunctionExists,
	// which goes through FindFunctionGraph — its Strategy 3 substring
	// match means a NewFunctionName that is a substring of any existing
	// graph name will report a false collision. Acceptable for now: the
	// substring strategy is documented in the #RESEARCH section as a
	// pre-existing foot-gun; tightening it is its own bundle.
	if (FunctionExists(Blueprint, NewFunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Function already exists: %s"), *NewFunctionName),
			EMCPErrorCode::NameCollision,
			TEXT("Pick a different `new_function_name`, or use `bp_delete_function` to remove the existing function before renaming."));
	}

	// Also reject renaming TO a reserved system-graph name. Engine-side
	// RenameGraph would happily overwrite the FName slot with no check;
	// the resulting blueprint would have two graphs claiming the same
	// canonical name on next reload. Symmetric with the "rename FROM"
	// guard above.
	if (IsReservedSystemGraph(NewFunctionName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Cannot rename to reserved system function name: %s"), *NewFunctionName),
			EMCPErrorCode::NameCollision,
			TEXT("`new_function_name` collides with a reserved system function (UserConstructionScript, BeginPlay, etc.). Pick a non-reserved identifier."));
	}

	// Rename using FBlueprintEditorUtils
	FBlueprintEditorUtils::RenameGraph(FunctionGraph, NewFunctionName);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogUnrealMCP, Display, TEXT("Successfully renamed function '%s' to '%s' in %s"), *OldFunctionName, *NewFunctionName, *BlueprintName);

	return CreateSuccessResponse(NewFunctionName);
}

UBlueprint* FFunctionManager::LoadBlueprint(const FString& BlueprintName)
{
	// Try direct load with _C suffix first (most reliable for Blueprint assets)
	FString ClassPath = BlueprintName + TEXT("_C");
	UClass* BlueprintClass = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, *ClassPath));
	if (BlueprintClass)
	{
		// Get the Blueprint asset from the class
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			if (It->GetPathName().Contains(BlueprintName))
			{
				return *It;
			}
		}
	}

	// Try EditorAssetLibrary as fallback
	if (UEditorAssetLibrary::DoesAssetExist(BlueprintName))
	{
		UObject* Asset = UEditorAssetLibrary::LoadAsset(BlueprintName);
		return Cast<UBlueprint>(Asset);
	}

	return nullptr;
}

bool FFunctionManager::ValidateFunctionName(const FString& FunctionName)
{
	if (FunctionName.IsEmpty())
	{
		return false;
	}

	// Check for spaces or invalid characters
	for (TCHAR Char : FunctionName)
	{
		if (FChar::IsWhitespace(Char) || (!FChar::IsAlnum(Char) && Char != TEXT('_')))
		{
			return false;
		}
	}

	// Must start with letter or underscore
	if (!FChar::IsAlpha(FunctionName[0]) && FunctionName[0] != TEXT('_'))
	{
		return false;
	}

	return true;
}

bool FFunctionManager::FunctionExists(UBlueprint* Blueprint, const FString& FunctionName)
{
	if (!Blueprint)
	{
		return false;
	}

	return FindFunctionGraph(Blueprint, FunctionName) != nullptr;
}

bool FFunctionManager::IsReservedSystemGraph(const FString& GraphName)
{
	// Engine constants from EdGraphSchema_K2 are the source of truth for
	// the canonical system-graph FNames (UE 5.7,
	// EdGraphSchema_K2.cpp:636-639):
	//   FN_UserConstructionScript → "UserConstructionScript"
	//     (lives in Blueprint->FunctionGraphs; resolvable by
	//     FindFunctionGraph). Pre-2026-04-22-bugfix the literal
	//     "Construction Script" with a space was the UI display label,
	//     not the internal graph name, so the guard never matched.
	//   GN_EventGraph             → "EventGraph"
	//     (lives in Blueprint->UbergraphPages, NOT searched by the
	//     current FindFunctionGraph). The arm is defensive against any
	//     future lookup-strategy change that would let FindFunctionGraph
	//     reach into UbergraphPages.
	//   GN_AnimGraph              → "AnimGraph"
	//     (added to Blueprint->FunctionGraphs by FBlueprintEditorUtils::
	//     AddDomainSpecificGraph at BlueprintEditorUtils.cpp:2418-2430,
	//     called from CreateBlueprint at Kismet2.cpp:500-504 with
	//     bAllowDeletion = false). AnimBPs depend on this graph existing
	//     under its canonical name — both AnimBlueprintCompiler.cpp:1654
	//     and AnimGraphNode_LinkedInputPose.cpp:689 look it up via
	//     `Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph`. Delete
	//     or rename it and the AnimBP fails to compile / the linked
	//     input pose stops resolving. The editor UI normally filters
	//     this via the per-graph bAllowDeletion/bAllowRenaming flags
	//     (BlueprintEditorUtils.cpp:9446 reads them from the right-click
	//     handler), but FBlueprintEditorUtils::RemoveGraph and RenameGraph
	//     themselves ignore both flags — only the menu honors them. MCP
	//     has no menu; the guard lives here.
	return GraphName == UEdGraphSchema_K2::FN_UserConstructionScript.ToString()
		|| GraphName == UEdGraphSchema_K2::GN_EventGraph.ToString()
		|| GraphName == UEdGraphSchema_K2::GN_AnimGraph.ToString();
}

UEdGraph* FFunctionManager::FindFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName)
{
	if (!Blueprint || FunctionName.IsEmpty())
	{
		return nullptr;
	}

	// Strategy 1: Exact name match
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName().ToString() == FunctionName)
		{
			return Graph;
		}
	}

	// Strategy 2: Search by outer object name
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetOuter() && Graph->GetOuter()->GetFName().ToString() == FunctionName)
		{
			return Graph;
		}
	}

	// Strategy 3: Partial match (for auto-generated names like EdGraph_N)
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName().ToString().Contains(FunctionName))
		{
			return Graph;
		}
	}

	return nullptr;
}

TSharedPtr<FJsonObject> FFunctionManager::CreateSuccessResponse(const FString& FunctionName, const FString& GraphID)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("function_name"), FunctionName);
	if (!GraphID.IsEmpty())
	{
		Response->SetStringField(TEXT("graph_id"), GraphID);
	}
	return Response;
}

// Local CreateErrorResponse helper removed 2026-05-10 (doc 1 migration). All
// error paths now route through FMCPCommonUtils::CreateErrorResponse
// with explicit (Message, EMCPErrorCode, Hint) so error_code lands on the wire.
