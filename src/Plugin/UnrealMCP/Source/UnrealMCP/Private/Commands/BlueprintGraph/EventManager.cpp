#include "Commands/BlueprintGraph/EventManager.h"
#include "Commands/BlueprintGraph/BPVariables.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorAssetLibrary.h"

TSharedPtr<FJsonObject> FEventManager::AddEventNode(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters. Doc 1 migration: every error path emits structured
	// error_code + actionable error_hint so agents can branch on the failure
	// shape without parsing prose.
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with at minimum `blueprint_name` and `event_name` fields."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint to add the event to."));
	}

	FString EventName;
	if (!Params->TryGetStringField(TEXT("event_name"), EventName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'event_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `event_name` (string) — e.g., \"ReceiveBeginPlay\", \"ReceiveTick\", \"ReceiveDestroyed\"."));
	}

	// An event_name of NAME_SIZE (1024) or more characters fatally asserts when
	// converted to FName (UnrealNames.cpp FindOrStoreString -> checkf(false)).
	if (EventName.Len() >= 1024)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("'event_name' is too long (%d chars); maximum is 1023"), EventName.Len()),
			EMCPErrorCode::InvalidArgument,
			TEXT("Event names map to FName (max 1023 chars). Pass a real event name such as ReceiveBeginPlay/ReceiveTick/ReceiveDestroyed."));
	}

	// Get optional position parameters
	FVector2D Position(0.0f, 0.0f);
	double PosX = 0.0, PosY = 0.0;
	if (Params->TryGetNumberField(TEXT("pos_x"), PosX))
	{
		Position.X = static_cast<float>(PosX);
	}
	if (Params->TryGetNumberField(TEXT("pos_y"), PosY))
	{
		Position.Y = static_cast<float>(PosY);
	}

	// Load the Blueprint
	UBlueprint* Blueprint = LoadBlueprint(BlueprintName);
	if (!Blueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists at the supplied name/path; use `bp_create_blueprint` to create one first."));
	}

	// Get the event graph (auto-creates for normal blueprints)
	UEdGraph* Graph = FMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
	if (!Graph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Blueprint has no event graph (and one could not be created for this blueprint type)"),
			EMCPErrorCode::UnsupportedClass,
			TEXT("Event nodes require a Blueprint with an Event Graph (Actor / Pawn / Character / GameMode etc.). Function-library and pure-data Blueprints have no event graph."));
	}

	// Create the event node
	UK2Node_Event* EventNode = CreateEventNode(Graph, EventName, Position);
	if (!EventNode)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to create event node: %s"), *EventName),
			EMCPErrorCode::FunctionNotFound,
			TEXT("Event lookup uses BlueprintClass->FindFunctionByName — typo'd or unsupported event names fail here. Common: ReceiveBeginPlay, ReceiveTick, ReceiveDestroyed. Use `bp_inspect` to discover valid event names on the Blueprint's parent class."));
	}

	// Notify changes
	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	return CreateSuccessResponse(EventNode);
}

TSharedPtr<FJsonObject> FEventManager::AddCustomEventNode(const TSharedPtr<FJsonObject>& Params)
{
	// Authors a fresh Blueprint custom event (UK2Node_CustomEvent) — the editor's
	// "Add Custom Event…" graph action. The fresh-create path (distinct from
	// CreateFromFunction, which BINDS an existing delegate signature) is grounded in
	// UE5.7 UK2Node_CustomEvent::CreateFromFunction
	// (Engine/Source/Editor/BlueprintGraph/Private/K2Node_CustomEvent.cpp:507):
	//   NewObject → set CustomFunctionName → SetFlags(RF_Transactional) → Graph->Modify
	//   → Graph->AddNode → CreateNewGuid → PostPlacedNewNode → AllocateDefaultPins
	//   → CreateUserDefinedPin(param, EGPD_Output) per signature param (:529).
	// The ctor (same file :167) already sets bIsEditable + bCanRenameNode +
	// FunctionFlags(BlueprintCallable|BlueprintEvent|Public), so no manual flag setup.
	// User parameters become OUTPUT pins on the node — CanCreateUserDefinedPin (:217)
	// REJECTS EGPD_Input ("Cannot add input pins to custom event node!"); the output
	// pins ARE the event's input signature, exactly as CreateFromFunction adds them.
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with at minimum `blueprint_name` and `event_name` fields."));
	}

	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint to add the custom event to."));
	}

	FString EventName;
	if (!Params->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'event_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `event_name` (string) — the new custom event's name (becomes UK2Node_CustomEvent CustomFunctionName)."));
	}

	// event_name → FName; a name of NAME_SIZE (1024)+ chars fatally asserts in FName storage.
	if (EventName.Len() >= NAME_SIZE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("'event_name' is too long (%d chars); maximum is %d"), EventName.Len(), NAME_SIZE - 1),
			EMCPErrorCode::InvalidArgument,
			TEXT("Custom event names map to FName (max 1023 chars). Pass a short identifier-style name."));
	}

	// Optional graph position.
	FVector2D Position(0.0f, 0.0f);
	double PosX = 0.0, PosY = 0.0;
	if (Params->TryGetNumberField(TEXT("pos_x"), PosX)) Position.X = static_cast<float>(PosX);
	if (Params->TryGetNumberField(TEXT("pos_y"), PosY)) Position.Y = static_cast<float>(PosY);

	UBlueprint* Blueprint = LoadBlueprint(BlueprintName);
	if (!Blueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Blueprint exists at the supplied name/path; use `bp_create_blueprint` to create one first."));
	}

	UEdGraph* Graph = FMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
	if (!Graph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Blueprint has no event graph (and one could not be created for this blueprint type)"),
			EMCPErrorCode::UnsupportedClass,
			TEXT("Custom events require a Blueprint with an Event Graph (Actor / Pawn / Character / GameMode etc.). Function-library and pure-data Blueprints have no event graph."));
	}

	// Name-clash: mirror the editor's FKismetNameValidator intent but REJECT (keep a
	// deterministic name) rather than auto-rename. A duplicate custom event, or a
	// same-named engine/override event, in ANY Ubergraph page is a collision.
	const FName WantName(*EventName);
	for (UEdGraph* Page : Blueprint->UbergraphPages)
	{
		if (!Page) continue;
		for (UEdGraphNode* Node : Page->Nodes)
		{
			if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
			{
				if (CE->CustomFunctionName == WantName)
				{
					return FMCPCommonUtils::CreateErrorResponse(
						FString::Printf(TEXT("A custom event named '%s' already exists in this Blueprint"), *EventName),
						EMCPErrorCode::NameCollision,
						TEXT("Pick a different `event_name`, or delete the existing custom event first (`bp_delete_node`)."));
				}
			}
			else if (UK2Node_Event* EV = Cast<UK2Node_Event>(Node))
			{
				if (EV->EventReference.GetMemberName() == WantName)
				{
					return FMCPCommonUtils::CreateErrorResponse(
						FString::Printf(TEXT("An event named '%s' already exists in this Blueprint (engine/override event)"), *EventName),
						EMCPErrorCode::NameCollision,
						TEXT("That name is taken by an override event (e.g. ReceiveBeginPlay). Pick a different `event_name`."));
				}
			}
		}
	}

	// Spawn — replays UK2Node_CustomEvent::CreateFromFunction's sequence for a fresh,
	// signature-less event (K2Node_CustomEvent.cpp:507).
	UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
	EventNode->CustomFunctionName = WantName;
	EventNode->SetFlags(RF_Transactional);
	Graph->Modify();
	Graph->AddNode(EventNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
	EventNode->CreateNewGuid();
	EventNode->PostPlacedNewNode();
	EventNode->AllocateDefaultPins();

	// Optional typed params → OUTPUT pins (the event's INPUT signature). Same
	// `[{name,type}]` shape + type strings as bp_create_dispatcher / bp_create_variable.
	TArray<FString> AddedParams;
	const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("params"), ParamsArray) && ParamsArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ParamsArray)
		{
			const TSharedPtr<FJsonObject>* ParamObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ParamObj) || !ParamObj)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("Each entry of 'params' must be an object"),
					EMCPErrorCode::InvalidArgument,
					TEXT("`params` is an array of objects, each `{ \"name\": string, \"type\": string }`."));
			}
			FString PName, PType;
			if (!(*ParamObj)->TryGetStringField(TEXT("name"), PName) || PName.IsEmpty() ||
				!(*ParamObj)->TryGetStringField(TEXT("type"), PType) || PType.IsEmpty())
			{
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("Each param needs a non-empty 'name' and 'type'"),
					EMCPErrorCode::InvalidArgument,
					TEXT("e.g. { \"name\": \"Damage\", \"type\": \"float\" }. `type` uses the same strings as `bp_create_variable`."));
			}
			if (PName.Len() >= NAME_SIZE)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("param name '%s' is too long (max %d chars)"), *PName, NAME_SIZE - 1),
					EMCPErrorCode::InvalidArgument,
					TEXT("Param names map to FName (max 1023 chars)."));
			}
			FEdGraphPinType PinType = FBPVariables::GetPinTypeFromString(PType);
			UEdGraphPin* NewPin = EventNode->CreateUserDefinedPin(FName(*PName), PinType, EGPD_Output);
			if (!NewPin)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Failed to add parameter pin '%s' (type '%s')"), *PName, *PType),
					EMCPErrorCode::InvalidPinType,
					TEXT("CreateUserDefinedPin returned null — usually a duplicate param name or an unparseable type. Custom-event params must be value (non-exec) pins."));
			}
			AddedParams.Add(PName);
		}
	}

	EventNode->NodePosX = static_cast<int32>(Position.X);
	EventNode->NodePosY = static_cast<int32>(Position.Y);

	// ReconstructNode rebuilds live pins from UserDefinedPins (the editor does this
	// after signature edits); with nothing linked to the delegate-out pin it is a
	// no-op for signature adoption (K2Node_CustomEvent.cpp:427) and preserves our
	// user pins. Then flag the structural change so a recompile regenerates the
	// event's UFunction. The server auto-saves the dirtied package.
	EventNode->ReconstructNode();
	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->MarkPackageDirty();

	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
	Response->SetStringField(TEXT("event_name"), EventNode->CustomFunctionName.ToString());
	Response->SetNumberField(TEXT("pos_x"), EventNode->NodePosX);
	Response->SetNumberField(TEXT("pos_y"), EventNode->NodePosY);
	TArray<TSharedPtr<FJsonValue>> ParamNamesJson;
	for (const FString& P : AddedParams)
	{
		ParamNamesJson.Add(MakeShared<FJsonValueString>(P));
	}
	Response->SetArrayField(TEXT("params_added"), ParamNamesJson);
	Response->SetNumberField(TEXT("num_params"), AddedParams.Num());
	return Response;
}

UK2Node_Event* FEventManager::CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
{
	if (!Graph)
	{
		return nullptr;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!Blueprint)
	{
		return nullptr;
	}

	// Check for existing event node to avoid duplicates
	UK2Node_Event* ExistingNode = FindExistingEventNode(Graph, EventName);
	if (ExistingNode)
	{
		UE_LOG(LogUnrealMCP, Display, TEXT("Using existing event node '%s' (ID: %s)"),
			*EventName, *ExistingNode->NodeGuid.ToString());
		return ExistingNode;
	}

	// Create new event node
	UK2Node_Event* EventNode = nullptr;
	UClass* BlueprintClass = Blueprint->GeneratedClass;

	if (!BlueprintClass)
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("Blueprint has no generated class"));
		return nullptr;
	}

	UFunction* EventFunction = BlueprintClass->FindFunctionByName(FName(*EventName));

	if (EventFunction)
	{
		EventNode = NewObject<UK2Node_Event>(Graph);
		EventNode->EventReference.SetExternalMember(FName(*EventName), BlueprintClass);
		EventNode->NodePosX = static_cast<int32>(Position.X);
		EventNode->NodePosY = static_cast<int32>(Position.Y);
		Graph->AddNode(EventNode, true);
		EventNode->PostPlacedNewNode();
		EventNode->AllocateDefaultPins();

		UE_LOG(LogUnrealMCP, Display, TEXT("Created new event node '%s' (ID: %s)"),
			*EventName, *EventNode->NodeGuid.ToString());
	}
	else
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("Failed to find function for event name: %s"), *EventName);
	}

	return EventNode;
}

UK2Node_Event* FEventManager::FindExistingEventNode(UEdGraph* Graph, const FString& EventName)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
		if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
		{
			return EventNode;
		}
	}

	return nullptr;
}

UBlueprint* FEventManager::LoadBlueprint(const FString& BlueprintName)
{
	return FMCPCommonUtils::FindBlueprintByName(BlueprintName);
}

TSharedPtr<FJsonObject> FEventManager::CreateSuccessResponse(const UK2Node_Event* EventNode)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
	Response->SetStringField(TEXT("event_name"), EventNode->EventReference.GetMemberName().ToString());
	Response->SetNumberField(TEXT("pos_x"), EventNode->NodePosX);
	Response->SetNumberField(TEXT("pos_y"), EventNode->NodePosY);
	return Response;
}

// Local CreateErrorResponse helper removed 2026-05-10 (doc 1 migration). All
// error paths now route through FMCPCommonUtils::CreateErrorResponse
// with explicit (Message, EMCPErrorCode, Hint) so error_code lands on the
// wire envelope. The local helper duplicated the single-arg form and bypassed
// the structured-error path.
