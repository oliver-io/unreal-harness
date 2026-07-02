#include "Commands/BlueprintGraph/NodePropertyManager.h"
#include "Commands/MCPCommonUtils.h"
#include "Commands/BlueprintGraph/Nodes/SwitchEnumEditor.h"
#include "Commands/BlueprintGraph/Nodes/ExecutionSequenceEditor.h"
#include "Commands/BlueprintGraph/Nodes/MakeArrayEditor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MakeArray.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_Select.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ClassDynamicCast.h"
#include "K2Node_CastByteToEnum.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimInstance.h"
#include "BoneContainer.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EditorAssetLibrary.h"
#include "UObject/UObjectIterator.h"
#include "Json.h"

// AnimGraph-node hidden-pin property setter (reflection-based)
// ---------------------------------------------------------------
// AnimGraph wrapper classes (UAnimGraphNode_*) expose an FAnimNode_* struct as
// a member named "Node" by UE convention. Hidden-pin-default UPROPERTYs (those
// with PinHiddenByDefault + EditCondition gates, e.g. FBoneReference chains on
// DragonIK solvers) live on that nested struct. The default pin-default fallback
// in SetGenericNodeProperty can't reach them — the pin isn't in the Pins array
// until it's exposed, and even if it were, the DefaultValue string can't encode
// an FBoneReference. These two helpers walk the reflection graph on the nested
// struct and write the value through the appropriate FProperty subclass.

// Resolve a dotted (or bare) property path against an AnimGraph wrapper node.
// Supported forms:
//   "EndSplineBone"        -> walks all FStructProperty members looking for the child property
//   "Node.EndSplineBone"   -> explicit path on a named struct member
//   "ik_node.EndSplineBone"-> explicit path on a named struct member (DragonIK convention)
//
// For bare names we can't assume the struct member is called "Node" — the UE
// convention is conventional only, not enforced. DragonIK names its member
// `ik_node` (UAnimGraphNode_DragonAimSolver::ik_node), Lyra's ALS uses other
// names, plugin authors vary. So we walk every FStructProperty on the wrapper
// class and take the first one whose nested struct has the requested property.
static bool ResolveAnimGraphProperty(
	UEdGraphNode* InNode,
	const FString& PropertyName,
	void*& OutContainer,
	FProperty*& OutProperty)
{
	OutContainer = nullptr;
	OutProperty = nullptr;
	if (!InNode) return false;

	FString MemberName, ChildName;
	if (!PropertyName.Split(TEXT("."), &MemberName, &ChildName))
	{
		MemberName.Reset();
		ChildName = PropertyName;
	}

	UClass* NodeClass = InNode->GetClass();

	// Explicit dotted path — user gave Member.Property
	if (!MemberName.IsEmpty())
	{
		FProperty* MemberProp = NodeClass->FindPropertyByName(FName(*MemberName));
		FStructProperty* StructProp = CastField<FStructProperty>(MemberProp);
		if (!StructProp || !StructProp->Struct) return false;
		void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(InNode);
		if (FProperty* ChildProp = StructProp->Struct->FindPropertyByName(FName(*ChildName)))
		{
			OutContainer = StructPtr;
			OutProperty = ChildProp;
			return true;
		}
		return false;
	}

	// Bare property — walk every FStructProperty member of the wrapper class.
	// Prioritize "Node" (UE convention) but fall through to the first match.
	const FName ChildFName(*ChildName);
	auto TryMember = [&](FProperty* MemberProp) -> bool
	{
		FStructProperty* StructProp = CastField<FStructProperty>(MemberProp);
		if (!StructProp || !StructProp->Struct) return false;
		if (FProperty* ChildProp = StructProp->Struct->FindPropertyByName(ChildFName))
		{
			OutContainer = StructProp->ContainerPtrToValuePtr<void>(InNode);
			OutProperty = ChildProp;
			return true;
		}
		return false;
	};

	if (FProperty* NodeMember = NodeClass->FindPropertyByName(FName(TEXT("Node"))))
	{
		if (TryMember(NodeMember))
		{
			return true;
		}
	}

	for (TFieldIterator<FProperty> It(NodeClass); It; ++It)
	{
		FProperty* MemberProp = *It;
		if (MemberProp->GetFName() == FName(TEXT("Node"))) continue; // already tried
		if (TryMember(MemberProp))
		{
			return true;
		}
	}

	// Fallback — direct property on the wrapper class
	if (FProperty* DirectProp = NodeClass->FindPropertyByName(ChildFName))
	{
		OutContainer = InNode;
		OutProperty = DirectProp;
		return true;
	}

	return false;
}

// Write a JSON value through an FProperty, handling the types that appear on
// AnimGraph hidden pins: bool, FName, FString, int, float, double, byte/enum,
// FEnumProperty, and FBoneReference (by BoneName).
static bool WriteAnimGraphPropertyValue(
	void* Container,
	FProperty* Property,
	const TSharedPtr<FJsonValue>& Value)
{
	if (!Container || !Property || !Value.IsValid()) return false;

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		bool b;
		if (Value->TryGetBool(b))
		{
			BoolProp->SetPropertyValue_InContainer(Container, b);
			return true;
		}
		FString s;
		if (Value->TryGetString(s))
		{
			BoolProp->SetPropertyValue_InContainer(Container, s.ToBool());
			return true;
		}
		return false;
	}

	if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		FString s;
		if (Value->TryGetString(s))
		{
			NameProp->SetPropertyValue_InContainer(Container, FName(*s));
			return true;
		}
		return false;
	}

	if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		FString s;
		if (Value->TryGetString(s))
		{
			StrProp->SetPropertyValue_InContainer(Container, s);
			return true;
		}
		return false;
	}

	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		double d;
		if (Value->TryGetNumber(d))
		{
			FloatProp->SetPropertyValue_InContainer(Container, static_cast<float>(d));
			return true;
		}
		return false;
	}

	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
	{
		double d;
		if (Value->TryGetNumber(d))
		{
			DoubleProp->SetPropertyValue_InContainer(Container, d);
			return true;
		}
		return false;
	}

	if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		double d;
		if (Value->TryGetNumber(d))
		{
			IntProp->SetPropertyValue_InContainer(Container, static_cast<int32>(d));
			return true;
		}
		return false;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		// FBoneReference — accept string (BoneName) or { "BoneName": "..." } object.
		// Struct name match (not pointer) to avoid hard-linking the engine struct.
		if (StructProp->Struct && StructProp->Struct->GetFName() == FName(TEXT("BoneReference")))
		{
			FBoneReference* BoneRef = StructProp->ContainerPtrToValuePtr<FBoneReference>(Container);
			FString BoneName;
			if (Value->TryGetString(BoneName))
			{
				BoneRef->BoneName = FName(*BoneName);
				return true;
			}
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (Value->TryGetObject(ObjPtr) && ObjPtr && ObjPtr->IsValid())
			{
				FString BoneStr;
				if ((*ObjPtr)->TryGetStringField(TEXT("BoneName"), BoneStr))
				{
					BoneRef->BoneName = FName(*BoneStr);
					return true;
				}
			}
			return false;
		}
		return false;
	}

	if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (ByteProp->Enum)
		{
			FString EnumStr;
			if (Value->TryGetString(EnumStr))
			{
				int64 EnumValue = ByteProp->Enum->GetValueByNameString(EnumStr);
				if (EnumValue == INDEX_NONE)
				{
					EnumValue = ByteProp->Enum->GetValueByNameString(
						FString::Printf(TEXT("%s::%s"), *ByteProp->Enum->GetName(), *EnumStr));
				}
				if (EnumValue != INDEX_NONE)
				{
					ByteProp->SetPropertyValue_InContainer(Container, static_cast<uint8>(EnumValue));
					return true;
				}
			}
		}
		double d;
		if (Value->TryGetNumber(d))
		{
			ByteProp->SetPropertyValue_InContainer(Container, static_cast<uint8>(d));
			return true;
		}
		return false;
	}

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		if (UEnum* Enum = EnumProp->GetEnum())
		{
			FString EnumStr;
			if (Value->TryGetString(EnumStr))
			{
				int64 EnumValue = Enum->GetValueByNameString(EnumStr);
				if (EnumValue == INDEX_NONE)
				{
					EnumValue = Enum->GetValueByNameString(
						FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *EnumStr));
				}
				if (EnumValue != INDEX_NONE)
				{
					EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(
						EnumProp->ContainerPtrToValuePtr<void>(Container), EnumValue);
					return true;
				}
			}
		}
		return false;
	}

	return false;
}

// Entry point — gated on UAnimGraphNode_Base. Skips reserved names handled by
// SetGenericNodeProperty so the dispatch order stays sane.
static bool SetAnimGraphNodeReflectiveProperty(
	UEdGraphNode* Node,
	const FString& PropertyName,
	const TSharedPtr<FJsonValue>& Value)
{
	if (!Cast<UAnimGraphNode_Base>(Node)) return false;

	if (PropertyName.Equals(TEXT("pos_x"), ESearchCase::IgnoreCase) ||
	    PropertyName.Equals(TEXT("pos_y"), ESearchCase::IgnoreCase) ||
	    PropertyName.Equals(TEXT("comment"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	void* Container = nullptr;
	FProperty* Property = nullptr;
	if (!ResolveAnimGraphProperty(Node, PropertyName, Container, Property))
	{
		return false;
	}
	if (!WriteAnimGraphPropertyValue(Container, Property, Value))
	{
		return false;
	}

	// Refresh the node so Details / hidden-pin defaults re-sync from the UPROPERTY.
	if (UK2Node* K2 = Cast<UK2Node>(Node))
	{
		K2->ReconstructNode();
	}
	return true;
}

// Shared helper: resolve a UClass from either a fully-qualified path
// (`/Script/Engine.Pawn`) or a short name (`Pawn`, `APawn`, `AnimInstance`).
// Falls back to TObjectIterator search when the short name is ambiguous about
// package. Used by the Phase 3 `set_function_call` action below to resolve the
// `target_class` param; also mirrored (in spirit) by the resolver that backs
// `add_node` CallFunction / `create_blueprint` parent_class / `add_component_to_blueprint`.
static UClass* ResolveClassByAnyName(const FString& ClassNameOrPath)
{
	if (ClassNameOrPath.IsEmpty()) return nullptr;

	// Fully-qualified path — /Script/Module.Class or /Game/Path.Name_C
	if (ClassNameOrPath.Contains(TEXT("/")) || ClassNameOrPath.Contains(TEXT(".")))
	{
		if (UClass* Loaded = LoadClass<UObject>(nullptr, *ClassNameOrPath))
		{
			return Loaded;
		}
		return nullptr;
	}

	// Short name — UClass::GetName() stores without UE prefix (U/A/F). Match
	// against both the raw string the caller passed and the prefix-stripped form.
	FString Stripped = ClassNameOrPath;
	if (Stripped.Len() > 1 && (Stripped[0] == TEXT('U') || Stripped[0] == TEXT('A')) &&
		FChar::IsUpper(Stripped[1]))
	{
		Stripped = Stripped.RightChop(1);
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		const FString CName = Cls->GetName();
		if (CName == ClassNameOrPath || CName == Stripped)
		{
			return Cls;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FNodePropertyManager::SetNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with `blueprint_name` and `node_id`. Legacy mode requires `property_name` + `property_value`; semantic mode requires `action` + per-action fields."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint owning the target node."));
	}

	FString NodeID;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeID))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_id` (string) — the FGuid of the node to modify. Use `bp_inspect` to discover node IDs."));
	}

	// ===================================================
	// CHECK FOR SEMANTIC ACTION (new mode)
	// ===================================================
	FString Action;
	bool bHasAction = Params->HasField(TEXT("action"));

	if (bHasAction)
	{
		if (Params->TryGetStringField(TEXT("action"), Action))
		{
			if (!Action.IsEmpty())
			{
				// dry_run on semantic mode is not yet supported — semantic actions
				// (reorder pins, swap function, change cast type, etc.) compose multiple
				// graph mutations whose individual diff shapes need per-action handling.
				// Per todo/13 mid-rollout contract: return dry_run_unsupported until
				// per-action diffs ship.
				if (FMCPCommonUtils::ParseDryRun(Params))
				{
					return FMCPCommonUtils::CreateDryRunUnsupportedResponse(
						FString::Printf(TEXT("set_node_property action='%s'"), *Action));
				}
				// Semantic editing mode - delegate to EditNode
				return EditNode(Params);
			}
		}
	}

	// ===================================================
	// LEGACY MODE: Simple property modification
	// ===================================================
	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("SetNodeProperty: Missing 'property_name' parameter"));
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Legacy mode requires `property_name` (string) — the UPROPERTY name on the node class. Use `bp_inspect` to see node properties, or pass `action` instead for semantic-mode editing."));
	}

	if (!Params->HasField(TEXT("property_value")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_value' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Legacy mode requires `property_value` — the value to assign. Type matches the UPROPERTY (string for FString/FName/text, number for ints/floats, bool, JSON object for structs)."));
	}

	TSharedPtr<FJsonValue> PropertyValue = Params->Values.FindRef(TEXT("property_value"));

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
			TEXT("Verify the Blueprint exists; use `asset_list` or `bp_create_blueprint`."));
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
				TEXT("Function-library and pure-data Blueprints have no event graph. Pass `function_name` to target a function graph instead."));
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Function graph not found: %s"), *FunctionName),
				EMCPErrorCode::FunctionNotFound,
				TEXT("Use `bp_list_graphs` to discover graph names. Names are case-sensitive."));
		}
	}

	// Find the node
	UEdGraphNode* Node = FindNodeByID(Graph, NodeID);
	if (!Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeID),
			EMCPErrorCode::NodeNotFound,
			TEXT("Use `bp_inspect` to discover node IDs. Node IDs are FGuids, not display names."));
	}

	// dry_run (legacy mode): structural validation already ran (BP, graph, node).
	// Property-type-specific compatibility checks live inside the SetPrint/Variable/
	// AnimGraph/Generic dispatch below — those CAN'T run in dry-run without their
	// side effects, so we accept partial validation parity here. We DO try to
	// look up the property by name on the node (via reflection) to capture a
	// before-value when possible; agents who commit then get the actual write
	// + the same type-dispatch.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		FString BeforeText;
		if (FProperty* Prop = Node->GetClass()->FindPropertyByName(FName(*PropertyName)))
		{
			Prop->ExportTextItem_Direct(BeforeText,
				Prop->ContainerPtrToValuePtr<void>(Node),
				nullptr, nullptr, PPF_None);
		}

		// Stringify the requested value into a stable text form for the diff.
		FString AfterText;
		if (PropertyValue.IsValid())
		{
			TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&AfterText);
			FJsonSerializer::Serialize(PropertyValue, FString(), JsonWriter);
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node_id"), NodeID);
		Entry->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
		Entry->SetStringField(TEXT("property_name"), PropertyName);
		Entry->SetStringField(TEXT("before"), BeforeText);
		Entry->SetStringField(TEXT("after"), AfterText);

		TArray<TSharedPtr<FJsonValue>> ChangedArr;
		ChangedArr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("properties_changed"), ChangedArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Attempt to set property based on node type
	bool Success = false;

	// Try as Print node (UK2Node_CallFunction)
	UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node);
	if (CallFuncNode)
	{
		Success = SetPrintNodeProperty(CallFuncNode, PropertyName, PropertyValue);
	}

	// Try as Variable node
	if (!Success)
	{
		UK2Node* K2Node = Cast<UK2Node>(Node);
		if (K2Node)
		{
			Success = SetVariableNodeProperty(K2Node, PropertyName, PropertyValue);
		}
	}

	// Try AnimGraph node hidden-pin / nested-struct properties via reflection
	// (handles FBoneReference, FName, bool, enum on the inner `Node` struct).
	// Must run BEFORE SetGenericNodeProperty so hidden-pin UPROPERTYs are set via
	// reflection rather than the pin-default-string fallback (which can't encode structs).
	if (!Success)
	{
		Success = SetAnimGraphNodeReflectiveProperty(Node, PropertyName, PropertyValue);
	}

	// Try generic properties
	if (!Success)
	{
		Success = SetGenericNodeProperty(Node, PropertyName, PropertyValue);
	}

	// Try AnimGraph node properties (anim_asset, slot_name, cache_name)
	if (!Success)
	{
		FString ValueStr;
		if (PropertyValue->TryGetString(ValueStr))
		{
			Success = SetAnimGraphNodeProperty(Node, PropertyName, ValueStr);
		}
	}

	if (!Success)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(
				TEXT("Failed to set property '%s' on node (property not supported or invalid value)"),
				*PropertyName),
			EMCPErrorCode::InvalidArgument,
			TEXT("Property name didn't match any known UPROPERTY on the node, or the value couldn't be coerced to the property's type. Use `bp_inspect` to see node properties + their types; check the editor log for the per-property failure reason."));
	}

	// Notify changes
	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogUnrealMCP, Display,
		TEXT("Successfully set '%s' on node '%s' in %s"),
		*PropertyName, *NodeID, *BlueprintName);

	return CreateSuccessResponse(PropertyName);
}

TSharedPtr<FJsonObject> FNodePropertyManager::EditNode(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a JSON object with `blueprint_name` and `node_id`. Legacy mode requires `property_name` + `property_value`; semantic mode requires `action` + per-action fields."));
	}

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'blueprint_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `blueprint_name` (string) — the name or path of the Blueprint owning the target node."));
	}

	FString NodeID;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeID))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_id` (string) — the FGuid of the node to modify. Use `bp_inspect` to discover node IDs."));
	}

	FString Action;
	if (!Params->TryGetStringField(TEXT("action"), Action))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'action' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Semantic mode requires `action` — one of `set_enum_type`, `add_pin`, `remove_pin`, `set_num_elements`, `set_function_call`, `set_event_type`. Or pass `property_name` + `property_value` for legacy property mode."));
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
			TEXT("Verify the Blueprint exists; use `asset_list` or `bp_create_blueprint`."));
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
				TEXT("Function-library and pure-data Blueprints have no event graph. Pass `function_name` to target a function graph instead."));
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Function graph not found: %s"), *FunctionName),
				EMCPErrorCode::FunctionNotFound,
				TEXT("Use `bp_list_graphs` to discover graph names. Names are case-sensitive."));
		}
	}

	// Find the node
	UEdGraphNode* Node = FindNodeByID(Graph, NodeID);
	if (!Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeID),
			EMCPErrorCode::NodeNotFound,
			TEXT("Use `bp_inspect` to discover node IDs. Node IDs are FGuids, not display names."));
	}

	// Cast to K2Node (edit operations require K2Node)
	UK2Node* K2Node = Cast<UK2Node>(Node);
	if (!K2Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Node is not a K2Node (cannot edit this node type)"),
			EMCPErrorCode::UnsupportedClass,
			TEXT("Semantic-mode actions only operate on UK2Node subclasses (the standard BP graph node type). Comment, group, and other UEdGraphNode subclasses aren't supported."));
	}

	// Dispatch the edit action
	return DispatchEditAction(K2Node, Graph, Action, Params);
}

TSharedPtr<FJsonObject> FNodePropertyManager::DispatchEditAction(
	UK2Node* Node,
	UEdGraph* Graph,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Node || !Graph || !Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid node or graph"),
			EMCPErrorCode::Internal,
			TEXT("Node or owning graph was unexpectedly null after lookup. This is a defensive check; verify `bp_inspect` returns the node before retrying."));
	}

	// === SWITCHENUM: Set enum type and auto-generate pins ===
	if (Action.Equals(TEXT("set_enum_type"), ESearchCase::IgnoreCase))
	{
		FString EnumPath;
		if (!Params->TryGetStringField(TEXT("enum_type"), EnumPath))
		{
			if (!Params->TryGetStringField(TEXT("enum_path"), EnumPath))
			{
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("Missing 'enum_type' or 'enum_path' parameter"),
					EMCPErrorCode::InvalidArgument,
					TEXT("`set_enum_type` action requires either `enum_type` (short name) or `enum_path` (script path) of a UEnum to bind to the SwitchEnum node."));
			}
		}

		bool bSuccess = FSwitchEnumEditor::SetEnumType(Node, Graph, EnumPath);
		if (bSuccess)
		{
			TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
			Response->SetBoolField(TEXT("success"), true);
			Response->SetStringField(TEXT("action"), TEXT("set_enum_type"));
			Response->SetStringField(TEXT("enum_type"), EnumPath);
			return Response;
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Failed to set enum type: %s"), *EnumPath),
				EMCPErrorCode::ClassNotLoaded,
				TEXT("Enum lookup by name/path returned null. Use the full script path (e.g. `/Script/Engine.EAxis`) or ensure the module containing the enum is loaded."));
		}
	}

	// === EXECUTIONSEQUENCE/MAKEARRAY: Add pin ===
	if (Action.Equals(TEXT("add_pin"), ESearchCase::IgnoreCase))
	{
		bool bSuccess = FExecutionSequenceEditor::AddExecutionPin(Node, Graph);

		// If ExecutionSequence failed, try MakeArray
		if (!bSuccess)
		{
			bSuccess = FMakeArrayEditor::AddArrayElementPin(Node, Graph);
		}

		if (bSuccess)
		{
			TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
			Response->SetBoolField(TEXT("success"), true);
			Response->SetStringField(TEXT("action"), TEXT("add_pin"));
			return Response;
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Failed to add pin"),
				EMCPErrorCode::Internal,
				TEXT("`add_pin` action's per-node-type editor (e.g. SwitchEnumEditor, ExecutionSequenceEditor, MakeArrayEditor) returned false. Pin add isn't supported on the node, or the editor's preconditions weren't met. Check the editor log."));
		}
	}

	// === EXECUTIONSEQUENCE/MAKEARRAY: Remove pin ===
	if (Action.Equals(TEXT("remove_pin"), ESearchCase::IgnoreCase))
	{
		FString PinName;
		if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'pin_name' parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`remove_pin` action requires `pin_name` — the name of the pin to sever. Use `bp_list_node_pins` to discover pin names on the node."));
		}

		bool bSuccess = FExecutionSequenceEditor::RemoveExecutionPin(Node, Graph, PinName);
		if (!bSuccess)
		{
			// Try MakeArray if ExecutionSequence failed
			bSuccess = FMakeArrayEditor::RemoveArrayElementPin(Node, Graph, PinName);
		}

		if (bSuccess)
		{
			TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
			Response->SetBoolField(TEXT("success"), true);
			Response->SetStringField(TEXT("action"), TEXT("remove_pin"));
			Response->SetStringField(TEXT("pin_name"), PinName);
			return Response;
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Failed to remove pin: %s"), *PinName),
				EMCPErrorCode::PinNotFound,
				TEXT("Pin name didn't match any removable pin on the node. Use `bp_list_node_pins` to verify pin names; some pins (default execute/then) can't be removed."));
		}
	}

	// === MAKEARRAY: Set number of array elements ===
	if (Action.Equals(TEXT("set_num_elements"), ESearchCase::IgnoreCase))
	{
		double NumElementsDouble = 0.0;
		if (!Params->TryGetNumberField(TEXT("num_elements"), NumElementsDouble))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'num_elements' parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`set_num_elements` action requires `num_elements` (integer >= 0) — the desired element count for the MakeArray node."));
		}
		int32 NumElements = static_cast<int32>(NumElementsDouble);

		bool bSuccess = FMakeArrayEditor::SetNumArrayElements(Node, Graph, NumElements);
		if (bSuccess)
		{
			TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
			Response->SetBoolField(TEXT("success"), true);
			Response->SetStringField(TEXT("action"), TEXT("set_num_elements"));
			Response->SetNumberField(TEXT("num_elements"), NumElements);
			return Response;
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Failed to set array elements to %d"), NumElements),
				EMCPErrorCode::Internal,
				TEXT("MakeArrayEditor::SetElementCount returned false. Common cause: target node isn't a MakeArray node. Verify with `bp_inspect`."));
		}
	}

	// === CALLFUNCTION: Retarget an existing CallFunction node to a different function ===
	// Phase 3 DESTRUCTIVE action: rewrites the node's FunctionReference, severs any
	// pins whose types no longer match, and reconstructs the node. Pre-fix, the
	// schema docstring advertised this action but the dispatcher fell through to
	// "Unknown action" — it was documented-but-not-wired.
	if (Action.Equals(TEXT("set_function_call"), ESearchCase::IgnoreCase))
	{
		UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
		if (!CallNode)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("set_function_call requires a CallFunction node"),
				EMCPErrorCode::UnsupportedClass,
				TEXT("`set_function_call` only operates on UK2Node_CallFunction nodes. Use `bp_inspect` to verify node type, or pass a different node_id."));
		}

		FString TargetFunction;
		if (!Params->TryGetStringField(TEXT("target_function"), TargetFunction))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'target_function' parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`set_function_call` action requires `target_function` (string) — the function name to bind. Optionally pass `target_class` to disambiguate when multiple classes have the same function name."));
		}

		FString TargetClassPath;
		Params->TryGetStringField(TEXT("target_class"), TargetClassPath);

		UClass* TargetClass = nullptr;
		if (!TargetClassPath.IsEmpty())
		{
			TargetClass = ResolveClassByAnyName(TargetClassPath);
			if (!TargetClass)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Could not resolve target_class '%s'"), *TargetClassPath),
					EMCPErrorCode::ClassNotLoaded,
					TEXT("Class lookup by name/path returned null. Use the full script path (e.g. `/Script/Engine.Actor`) or ensure the module containing the class is loaded."));
			}
		}

		UFunction* TargetFunc = nullptr;

		// 1. Explicit target_class
		if (TargetClass)
		{
			TargetFunc = TargetClass->FindFunctionByName(FName(*TargetFunction));
		}

		// 2. Blueprint's own class hierarchy
		if (!TargetFunc)
		{
			UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
			if (Blueprint && Blueprint->GeneratedClass)
			{
				TargetFunc = Blueprint->GeneratedClass->FindFunctionByName(FName(*TargetFunction));
			}
			if (!TargetFunc && Blueprint && Blueprint->ParentClass)
			{
				TargetFunc = Blueprint->ParentClass->FindFunctionByName(FName(*TargetFunction));
			}
		}

		// 3. Common engine classes (instance methods + static libraries)
		if (!TargetFunc)
		{
			static UClass* const CommonClasses[] = {
				AActor::StaticClass(),
				APawn::StaticClass(),
				ACharacter::StaticClass(),
				AController::StaticClass(),
				APlayerController::StaticClass(),
				UAnimInstance::StaticClass(),
				UCharacterMovementComponent::StaticClass(),
				UKismetSystemLibrary::StaticClass(),
				UKismetMathLibrary::StaticClass(),
				UGameplayStatics::StaticClass(),
			};
			for (UClass* Cls : CommonClasses)
			{
				if (Cls)
				{
					TargetFunc = Cls->FindFunctionByName(FName(*TargetFunction));
					if (TargetFunc) break;
				}
			}
		}

		if (!TargetFunc)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(
					TEXT("Could not resolve function '%s'%s"),
					*TargetFunction,
					TargetClass ? *FString::Printf(TEXT(" on class '%s'"), *TargetClass->GetName()) : TEXT("")),
				EMCPErrorCode::FunctionNotFound,
				TEXT("Function lookup walked Blueprint generated/parent class + common engine classes (Actor / Pawn / GameplayStatics etc.) and found no match. Pass `target_class` explicitly to disambiguate, or verify the function name with `bp_get_function_details`."));
		}

		CallNode->SetFromFunction(TargetFunc);
		CallNode->ReconstructNode();
		Graph->NotifyGraphChanged();
		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		if (Blueprint) FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
		Response->SetBoolField(TEXT("success"), true);
		Response->SetStringField(TEXT("action"), TEXT("set_function_call"));
		Response->SetStringField(TEXT("target_function"), TargetFunction);
		if (TargetFunc->GetOuter())
		{
			Response->SetStringField(TEXT("resolved_class"), TargetFunc->GetOuter()->GetName());
		}
		return Response;
	}

	// === EVENT: Retarget an existing Event node to a different event ===
	// Phase 3 DESTRUCTIVE action. Only valid on UK2Node_Event (BeginPlay, Tick,
	// Destroyed, etc.). Accepts the short event name — the function is looked
	// up on the blueprint's parent class where events are declared.
	if (Action.Equals(TEXT("set_event_type"), ESearchCase::IgnoreCase))
	{
		UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
		if (!EventNode)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("set_event_type requires an Event node"),
				EMCPErrorCode::UnsupportedClass,
				TEXT("`set_event_type` only operates on UK2Node_Event nodes. Use `bp_inspect` to verify node type, or use `bp_add_event_node` to create a fresh event with the desired type."));
		}

		FString EventType;
		if (!Params->TryGetStringField(TEXT("event_type"), EventType))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'event_type' parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`set_event_type` action requires `event_type` (string) — e.g. \"ReceiveBeginPlay\", \"ReceiveTick\". Use `bp_inspect` on the parent class to discover available events."));
		}

		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		if (!Blueprint || !Blueprint->ParentClass)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Cannot resolve blueprint parent class for event lookup"),
				EMCPErrorCode::Internal,
				TEXT("Blueprint->ParentClass returned null. The Blueprint may be in a transient state; try saving + reloading."));
		}

		UFunction* EventFunc = Blueprint->ParentClass->FindFunctionByName(FName(*EventType));
		if (!EventFunc)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(
					TEXT("Unknown event '%s' on parent class '%s'"),
					*EventType, *Blueprint->ParentClass->GetName()),
				EMCPErrorCode::FunctionNotFound,
				TEXT("Event lookup uses ParentClass->FindFunctionByName. Common events: ReceiveBeginPlay, ReceiveTick, ReceiveDestroyed. Use `reflection_class_properties` to discover events on the parent class."));
		}

		EventNode->EventReference.SetExternalMember(FName(*EventType), Blueprint->ParentClass);
		EventNode->CustomFunctionName = NAME_None;
		EventNode->ReconstructNode();
		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
		Response->SetBoolField(TEXT("success"), true);
		Response->SetStringField(TEXT("action"), TEXT("set_event_type"));
		Response->SetStringField(TEXT("event_type"), EventType);
		return Response;
	}

	// === DYNAMICCAST: Retarget the cast to a different class ===
	// Resolves `target_type` (native name/path or Blueprint asset name) to a
	// UClass — loading the Blueprint's GeneratedClass when needed — sets it on
	// the cast node's TargetType, then reconstructs so the As<Class> output pin
	// reflects the new type. Works for UK2Node_DynamicCast (object cast) and
	// UK2Node_ClassDynamicCast (class cast).
	if (Action.Equals(TEXT("set_cast_target"), ESearchCase::IgnoreCase))
	{
		UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node);
		if (!CastNode)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("set_cast_target requires a DynamicCast node"),
				EMCPErrorCode::UnsupportedClass,
				TEXT("`set_cast_target` only operates on UK2Node_DynamicCast / UK2Node_ClassDynamicCast nodes. Use `bp_inspect` to verify node type, or create a cast node with `bp_add_node node_type:\"DynamicCast\"`."));
		}

		FString TargetType;
		if (!Params->TryGetStringField(TEXT("target_type"), TargetType) || TargetType.IsEmpty())
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'target_type' parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`set_cast_target` requires `target_type` (string) — a class name/path or Blueprint asset name (e.g. \"Pawn\", \"/Script/Engine.Pawn\", \"BP_Bike\")."));
		}

		UClass* TargetClass = FMCPCommonUtils::ResolveClass(TargetType);
		if (!TargetClass)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Could not resolve cast target '%s'"), *TargetType),
				EMCPErrorCode::ClassNotLoaded,
				TEXT("Class lookup returned null. Pass a full path (\"/Script/Engine.Pawn\" or \"/Game/.../BP_Bike.BP_Bike_C\"), a short native name, or a bare Blueprint asset name."));
		}

		CastNode->TargetType = TargetClass;
		CastNode->ReconstructNode();
		Graph->NotifyGraphChanged();
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node))
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
		Response->SetBoolField(TEXT("success"), true);
		Response->SetStringField(TEXT("action"), TEXT("set_cast_target"));
		Response->SetStringField(TEXT("target_type"), TargetType);
		Response->SetStringField(TEXT("resolved_class"), TargetClass->GetPathName());
		return Response;
	}

	// Unknown action
	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown action: %s"), *Action),
		EMCPErrorCode::InvalidArgument,
		TEXT("Valid `action` values: `set_enum_type`, `add_pin`, `remove_pin`, `set_num_elements`, `set_function_call`, `set_event_type`, `set_cast_target`. Or omit `action` and use `property_name` + `property_value` for legacy property mode."));
}

bool FNodePropertyManager::SetPrintNodeProperty(
	UK2Node_CallFunction* PrintNode,
	const FString& PropertyName,
	const TSharedPtr<FJsonValue>& Value)
{
	if (!PrintNode || !Value.IsValid())
	{
		return false;
	}

	// Handle "message" property
	if (PropertyName.Equals(TEXT("message"), ESearchCase::IgnoreCase))
	{
		FString MessageValue;
		if (Value->TryGetString(MessageValue))
		{
			UEdGraphPin* InStringPin = PrintNode->FindPin(TEXT("InString"));
			if (InStringPin)
			{
				InStringPin->DefaultValue = MessageValue;
				return true;
			}
		}
	}

	// Handle "duration" property
	if (PropertyName.Equals(TEXT("duration"), ESearchCase::IgnoreCase))
	{
		double DurationValue;
		if (Value->TryGetNumber(DurationValue))
		{
			UEdGraphPin* DurationPin = PrintNode->FindPin(TEXT("Duration"));
			if (DurationPin)
			{
				DurationPin->DefaultValue = FString::SanitizeFloat(DurationValue);
				return true;
			}
		}
	}

	return false;
}

bool FNodePropertyManager::SetVariableNodeProperty(
	UK2Node* VarNode,
	const FString& PropertyName,
	const TSharedPtr<FJsonValue>& Value)
{
	if (!VarNode || !Value.IsValid())
	{
		return false;
	}

	// Handle "variable_name" property
	if (PropertyName.Equals(TEXT("variable_name"), ESearchCase::IgnoreCase))
	{
		FString VarName;
		if (Value->TryGetString(VarName))
		{
			UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(VarNode);
			if (VarGet)
			{
				VarGet->VariableReference.SetSelfMember(FName(*VarName));
				VarGet->ReconstructNode();
				return true;
			}

			UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(VarNode);
			if (VarSet)
			{
				VarSet->VariableReference.SetSelfMember(FName(*VarName));
				VarSet->ReconstructNode();
				return true;
			}
		}
	}

	return false;
}

bool FNodePropertyManager::SetGenericNodeProperty(
	UEdGraphNode* Node,
	const FString& PropertyName,
	const TSharedPtr<FJsonValue>& Value)
{
	if (!Node || !Value.IsValid())
	{
		return false;
	}

	// Handle "pos_x" property
	if (PropertyName.Equals(TEXT("pos_x"), ESearchCase::IgnoreCase))
	{
		double PosX;
		if (Value->TryGetNumber(PosX))
		{
			Node->NodePosX = static_cast<int32>(PosX);
			return true;
		}
	}

	// Handle "pos_y" property
	if (PropertyName.Equals(TEXT("pos_y"), ESearchCase::IgnoreCase))
	{
		double PosY;
		if (Value->TryGetNumber(PosY))
		{
			Node->NodePosY = static_cast<int32>(PosY);
			return true;
		}
	}

	// Handle "comment" property
	if (PropertyName.Equals(TEXT("comment"), ESearchCase::IgnoreCase))
	{
		FString Comment;
		if (Value->TryGetString(Comment))
		{
			Node->NodeComment = Comment;
			return true;
		}
	}

	// Fallback: treat PropertyName as a pin name and set its default value
	if (UEdGraphPin* Pin = Node->FindPin(FName(*PropertyName)))
	{
		if (Pin->Direction == EGPD_Input && !Pin->HasAnyConnections())
		{
			const FName PinCat = Pin->PinType.PinCategory;
			const bool bClassLike = PinCat == UEdGraphSchema_K2::PC_Class || PinCat == UEdGraphSchema_K2::PC_SoftClass;
			const bool bObjectLike = PinCat == UEdGraphSchema_K2::PC_Object || PinCat == UEdGraphSchema_K2::PC_SoftObject ||
				PinCat == UEdGraphSchema_K2::PC_Interface;
			const bool bSoft = PinCat == UEdGraphSchema_K2::PC_SoftClass || PinCat == UEdGraphSchema_K2::PC_SoftObject;

			FString StringValue;
			double NumberValue;

			// Class/object literal pins (e.g. SpawnActorFromClass "Class"): resolve
			// the string to a UClass*/UObject* and store it as the pin's default
			// object so the node's type-dependent pins (e.g. Return Value) propagate.
			if ((bClassLike || bObjectLike) && Value->TryGetString(StringValue) && !StringValue.IsEmpty())
			{
				UObject* ResolvedObj = nullptr;
				if (bClassLike)
				{
					// Class pin → the value IS the class.
					ResolvedObj = FMCPCommonUtils::ResolveClass(StringValue);
				}
				else
				{
					// Object pin → typically an asset path; fall back to a class.
					ResolvedObj = StaticLoadObject(UObject::StaticClass(), nullptr, *StringValue);
					if (!ResolvedObj)
					{
						ResolvedObj = FMCPCommonUtils::ResolveClass(StringValue);
					}
				}

				if (ResolvedObj)
				{
					const UEdGraphSchema* Schema = Pin->GetSchema();
					if (bSoft)
					{
						// Soft pins store the object path string, not DefaultObject.
						const FString PathValue = ResolvedObj->GetPathName();
						if (Schema)
						{
							Schema->TrySetDefaultValue(*Pin, PathValue);
						}
						else
						{
							Pin->DefaultValue = PathValue;
							Pin->GetOwningNode()->PinDefaultValueChanged(Pin);
						}
					}
					else if (Schema)
					{
						// TrySetDefaultObject notifies the node so the type propagates.
						Schema->TrySetDefaultObject(*Pin, ResolvedObj);
					}
					else
					{
						Pin->DefaultObject = ResolvedObj;
						Pin->GetOwningNode()->PinDefaultValueChanged(Pin);
					}
					// GAP-039: a CallFunction with meta DeterminesOutputType (e.g.
					// UWidgetBlueprintLibrary::Create) only conforms its output pin to the
					// chosen class on a full reconstruct — PinDefaultValueChanged alone leaves
					// ReturnValue as the generic base type. Reconstruct so the typed output
					// (and any expose-on-spawn pins) propagate.
					if (UEdGraphNode* OwningNode = Pin->GetOwningNode())
					{
						OwningNode->ReconstructNode();
					}
					return true;
				}
				// Unresolved: fall through to the raw-string path below.
			}

			// GAP-015: integer pin literals. SanitizeFloat() writes "1.0" for an int
			// pin, and a quoted "1" string also fails compile ("Expected a valid number
			// for an integer property"). Format int/byte pins as a bare integer.
			const bool bIntLike = PinCat == UEdGraphSchema_K2::PC_Int ||
				PinCat == UEdGraphSchema_K2::PC_Int64 || PinCat == UEdGraphSchema_K2::PC_Byte;

			if (Value->TryGetNumber(NumberValue))
			{
				Pin->DefaultValue = bIntLike
					? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(NumberValue)))
					: FString::SanitizeFloat(NumberValue);
				Pin->GetOwningNode()->PinDefaultValueChanged(Pin);
				return true;
			}
			else if (Value->TryGetString(StringValue))
			{
				// An int pin given a numeric string ("1") must store the bare digits,
				// not a quoted literal — trim and re-emit when it parses as a number.
				if (bIntLike && StringValue.IsNumeric())
				{
					Pin->DefaultValue = FString::Printf(TEXT("%lld"), FCString::Atoi64(*StringValue));
				}
				else
				{
					Pin->DefaultValue = StringValue;
				}
				Pin->GetOwningNode()->PinDefaultValueChanged(Pin);
				return true;
			}
		}
	}

	return false;
}

UEdGraph* FNodePropertyManager::GetGraph(UBlueprint* Blueprint, const FString& FunctionName,
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

	return nullptr;
}

UEdGraphNode* FNodePropertyManager::FindNodeByID(UEdGraph* Graph, const FString& NodeID)
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

UBlueprint* FNodePropertyManager::LoadBlueprint(const FString& BlueprintName)
{
	return FMCPCommonUtils::FindBlueprintByName(BlueprintName);
}

TSharedPtr<FJsonObject> FNodePropertyManager::CreateSuccessResponse(const FString& PropertyName)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject);
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("updated_property"), PropertyName);
	return Response;
}

bool FNodePropertyManager::SetAnimGraphNodeProperty(UEdGraphNode* Node, const FString& PropertyName, const FString& PropertyValue)
{
	// Animation asset (BlendSpace, Sequence, AimOffset, anim_asset — all map to SetAnimationAsset)
	if (PropertyName.Equals(TEXT("anim_asset"), ESearchCase::IgnoreCase) ||
	    PropertyName.Equals(TEXT("BlendSpace"), ESearchCase::IgnoreCase) ||
	    PropertyName.Equals(TEXT("AimOffset"), ESearchCase::IgnoreCase) ||
	    PropertyName.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase) ||
	    PropertyName.Equals(TEXT("AnimSequence"), ESearchCase::IgnoreCase))
	{
		UAnimGraphNode_AssetPlayerBase* AssetNode = Cast<UAnimGraphNode_AssetPlayerBase>(Node);
		if (!AssetNode) return false;

		UAnimationAsset* Asset = LoadObject<UAnimationAsset>(nullptr, *PropertyValue);
		if (!Asset) return false;

		AssetNode->SetAnimationAsset(Asset);
		AssetNode->ReconstructNode();
		return true;
	}

	// Slot name
	if (PropertyName.Equals(TEXT("slot_name"), ESearchCase::IgnoreCase) ||
	    PropertyName.Equals(TEXT("SlotName"), ESearchCase::IgnoreCase))
	{
		UAnimGraphNode_Slot* SlotNode = Cast<UAnimGraphNode_Slot>(Node);
		if (!SlotNode) return false;

		SlotNode->Node.SlotName = FName(*PropertyValue);
		SlotNode->ReconstructNode();
		return true;
	}

	// Cache name (SaveCachedPose)
	if (PropertyName.Equals(TEXT("cache_name"), ESearchCase::IgnoreCase) ||
	    PropertyName.Equals(TEXT("CacheName"), ESearchCase::IgnoreCase))
	{
		UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(Node);
		if (!SaveNode) return false;

		SaveNode->CacheName = PropertyValue;
		SaveNode->ReconstructNode();
		return true;
	}

	return false;
}

// Local CreateErrorResponse helper removed 2026-05-10 (doc 1 migration). All
// error paths now route through FMCPCommonUtils::CreateErrorResponse
// with explicit (Message, EMCPErrorCode, Hint) so error_code lands on the wire.
