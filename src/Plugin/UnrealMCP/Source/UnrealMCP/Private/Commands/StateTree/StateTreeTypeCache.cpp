#include "StateTreeTypeCache.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeNodeBase.h"
#include "StateTreeSchema.h"
#include "InstancedStruct.h"
#include "UObject/UObjectIterator.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Commands/MCPCommonUtils.h"

FStateTreeTypeCache& FStateTreeTypeCache::Get()
{
	static FStateTreeTypeCache Instance;
	return Instance;
}

void FStateTreeTypeCache::Invalidate()
{
	bIsBuilt = false;
	AllNodeTypes.Empty();
	TypeCategories.Empty();
}

void FStateTreeTypeCache::EnsureBuilt()
{
	if (bIsBuilt) return;

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (!Struct) continue;

		FString Category;
		if (Struct->IsChildOf(FStateTreeTaskBase::StaticStruct()))
		{
			Category = TEXT("task");
		}
		else if (Struct->IsChildOf(FStateTreeConditionBase::StaticStruct()))
		{
			Category = TEXT("condition");
		}
		else if (Struct->IsChildOf(FStateTreeEvaluatorBase::StaticStruct()))
		{
			Category = TEXT("evaluator");
		}
		// FStateTreeConsiderationBase may not exist in all UE versions
		if (Category.IsEmpty())
		{
			static UScriptStruct* ConsiderationBase = FindObject<UScriptStruct>(nullptr, TEXT("/Script/StateTreeModule.StateTreeConsiderationBase"));
			if (ConsiderationBase && Struct->IsChildOf(ConsiderationBase))
			{
				Category = TEXT("consideration");
			}
		}

		if (!Category.IsEmpty())
		{
			// Skip abstract base structs themselves
			if (Struct == FStateTreeTaskBase::StaticStruct() ||
				Struct == FStateTreeConditionBase::StaticStruct() ||
				Struct == FStateTreeEvaluatorBase::StaticStruct())
			{
				continue;
			}

			FString Name = Struct->GetName();
			AllNodeTypes.Add(Name, Struct);
			TypeCategories.Add(Name, Category);
		}
	}

	bIsBuilt = true;
	UE_LOG(LogUnrealMCP, Display, TEXT("StateTreeTypeCache: Built cache with %d node types"), AllNodeTypes.Num());
}

UScriptStruct* FStateTreeTypeCache::ResolveNodeType(const FString& TypeName)
{
	EnsureBuilt();

	// Try exact match first
	if (UScriptStruct** Found = AllNodeTypes.Find(TypeName))
	{
		return *Found;
	}

	// UScriptStruct::GetName() returns the name with the F prefix stripped, so the
	// cache is keyed by "STTask_AnimalWander" (not "FSTTask_AnimalWander"). Accept
	// both forms — strip an F prefix if the caller passed it (e.g. copied from
	// C++ source or from an earlier version of the schema docstring). Pre-fix,
	// the fallback path tried to *add* F rather than strip it, so every F-prefixed
	// type name produced "Unknown node type" despite being the exact name used
	// in the C++ declaration.
	if (TypeName.StartsWith(TEXT("F")))
	{
		FString WithoutF = TypeName.RightChop(1);
		if (UScriptStruct** Found = AllNodeTypes.Find(WithoutF))
		{
			return *Found;
		}
	}
	else
	{
		// Symmetric fallback for the opposite case (cache happens to store
		// names with F prefix in some UE configurations).
		FString WithF = FString::Printf(TEXT("F%s"), *TypeName);
		if (UScriptStruct** Found = AllNodeTypes.Find(WithF))
		{
			return *Found;
		}
	}

	return nullptr;
}

TSharedPtr<FJsonObject> FStateTreeTypeCache::ListNodeTypes(const FString& BaseClassFilter, const FString& NamePattern)
{
	EnsureBuilt();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> TypesArray;

	for (const auto& Pair : AllNodeTypes)
	{
		const FString& Name = Pair.Key;
		UScriptStruct* Struct = Pair.Value;
		const FString& Category = TypeCategories[Name];

		// Filter by base class
		if (BaseClassFilter != TEXT("all") && Category != BaseClassFilter)
		{
			continue;
		}

		// Filter by name pattern
		if (!NamePattern.IsEmpty() && !Name.Contains(NamePattern))
		{
			continue;
		}

		TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
		TypeObj->SetStringField(TEXT("name"), Name);
		TypeObj->SetStringField(TEXT("category"), Category);

		// Get instance data type if available
		FInstancedStruct TempNode;
		TempNode.InitializeAs(Struct);
		if (const FStateTreeNodeBase* NodeBase = TempNode.GetPtr<FStateTreeNodeBase>())
		{
			if (const UScriptStruct* InstanceType = Cast<UScriptStruct>(NodeBase->GetInstanceDataType()))
			{
				TypeObj->SetStringField(TEXT("instance_data_type"), InstanceType->GetName());

				// List EditAnywhere properties on instance data
				TArray<TSharedPtr<FJsonValue>> PropsArray;
				FInstancedStruct TempInstance;
				TempInstance.InitializeAs(const_cast<UScriptStruct*>(InstanceType));

				for (TFieldIterator<FProperty> PropIt(InstanceType); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

					TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
					PropObj->SetStringField(TEXT("name"), Prop->GetName());
					PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());

					// Get default value
					FString DefaultValue;
					uint8* ValuePtr = TempInstance.GetMutableMemory() + Prop->GetOffset_ForInternal();
					Prop->ExportTextItem_Direct(DefaultValue, ValuePtr, nullptr, nullptr, PPF_None);
					PropObj->SetStringField(TEXT("default_value"), DefaultValue);

					if (!Prop->GetMetaData(TEXT("Category")).IsEmpty())
					{
						PropObj->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));
					}

					PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
				}
				TypeObj->SetField(TEXT("properties"), MakeShared<FJsonValueArray>(PropsArray));
			}
		}

		TypesArray.Add(MakeShared<FJsonValueObject>(TypeObj));
	}

	Result->SetField(TEXT("types"), MakeShared<FJsonValueArray>(TypesArray));
	Result->SetNumberField(TEXT("count"), TypesArray.Num());
	return Result;
}

TSharedPtr<FJsonObject> FStateTreeTypeCache::ListSchemaTypes()
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SchemasArray;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class || Class->HasAnyClassFlags(CLASS_Abstract)) continue;

		static UClass* SchemaBase = UStateTreeSchema::StaticClass();
		if (!Class->IsChildOf(SchemaBase) || Class == SchemaBase) continue;

		TSharedPtr<FJsonObject> SchemaObj = MakeShared<FJsonObject>();
		SchemaObj->SetStringField(TEXT("class_name"), Class->GetName());

		// Get context data descriptors from CDO
		const UStateTreeSchema* SchemaCDO = Class->GetDefaultObject<UStateTreeSchema>();
		if (SchemaCDO)
		{
			TArray<TSharedPtr<FJsonValue>> ContextArray;
			// Schema context actors are accessible through the schema's interface
			SchemaObj->SetStringField(TEXT("outer_class"), SchemaCDO->GetClass()->GetName());
		}

		SchemasArray.Add(MakeShared<FJsonValueObject>(SchemaObj));
	}

	Result->SetField(TEXT("schemas"), MakeShared<FJsonValueArray>(SchemasArray));
	Result->SetNumberField(TEXT("count"), SchemasArray.Num());
	return Result;
}

// ---- Asset loading helpers ----

UStateTree* FStateTreeTypeCache::LoadStateTreeFromParams(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `asset_path` (string) — the /Game/... path of the StateTree asset to load."));
		return nullptr;
	}

	UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *AssetPath);
	if (!StateTree)
	{
		// Try with /Game/ prefix
		FString FullPath = AssetPath;
		if (!FullPath.StartsWith(TEXT("/")))
		{
			FullPath = FString::Printf(TEXT("/Game/%s"), *AssetPath);
		}
		// Try appending the asset name (e.g. "/Game/AI/ST_Combat" -> "/Game/AI/ST_Combat.ST_Combat")
		if (!FullPath.Contains(TEXT(".")))
		{
			FString AssetName = FPaths::GetBaseFilename(FullPath);
			FullPath = FString::Printf(TEXT("%s.%s"), *FullPath, *AssetName);
		}
		StateTree = LoadObject<UStateTree>(nullptr, *FullPath);
	}

	if (!StateTree)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load StateTree asset: %s"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the StateTree asset exists at the supplied /Game/... path. Use `asset_list` with `class_filter=\"StateTree\"` to discover."));
		return nullptr;
	}

	return StateTree;
}

UStateTreeEditorData* FStateTreeTypeCache::GetEditorData(UStateTree* StateTree, TSharedPtr<FJsonObject>& OutError)
{
	if (!StateTree)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			TEXT("StateTree is null"),
			EMCPErrorCode::Internal,
			TEXT("Defensive check — caller passed a null UStateTree pointer. Verify the asset loaded successfully via `LoadStateTreeFromParams`."));
		return nullptr;
	}

	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
	if (!EditorData)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			TEXT("StateTree has no editor data — was it created outside the editor?"),
			EMCPErrorCode::AssetCompileFailed,
			TEXT("StateTree asset has no UStateTreeEditorData — typically means the asset was created without the editor (cooked/runtime-only) or its editor data is missing. Open the asset in the editor and re-save to regenerate."));
		return nullptr;
	}

	return EditorData;
}

// ---- State lookup ----

void FStateTreeTypeCache::FindStateRecursive(UStateTreeState* State, const FString& Identifier, bool bIsGuid, TArray<UStateTreeState*>& OutMatches)
{
	if (!State) return;

	if (bIsGuid)
	{
		if (State->ID.ToString() == Identifier)
		{
			OutMatches.Add(State);
			return; // GUIDs are unique
		}
	}
	else
	{
		if (State->Name.ToString() == Identifier)
		{
			OutMatches.Add(State);
		}
	}

	for (UStateTreeState* Child : State->Children)
	{
		FindStateRecursive(Child, Identifier, bIsGuid, OutMatches);
	}
}

UStateTreeState* FStateTreeTypeCache::FindState(UStateTreeEditorData* EditorData, const FString& StateIdentifier, TSharedPtr<FJsonObject>& OutError)
{
	if (!EditorData)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			TEXT("EditorData is null"),
			EMCPErrorCode::Internal,
			TEXT("Defensive check — caller passed a null UStateTreeEditorData pointer. Verify the asset has editor data via `GetEditorData`."));
		return nullptr;
	}

	// Determine if the identifier is a GUID
	FGuid TestGuid;
	bool bIsGuid = FGuid::Parse(StateIdentifier, TestGuid);

	TArray<UStateTreeState*> Matches;
	for (UStateTreeState* RootState : EditorData->SubTrees)
	{
		FindStateRecursive(RootState, StateIdentifier, bIsGuid, Matches);
	}

	if (Matches.Num() == 0)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("State not found: %s"), *StateIdentifier),
			EMCPErrorCode::NodeNotFound,
			TEXT("State lookup walks the entire tree from SubTrees roots. Use `statetree_state_list` to discover state names + GUIDs. Identifier is matched as GUID first (if it parses) then as name (case-sensitive)."));
		return nullptr;
	}

	if (Matches.Num() > 1)
	{
		// Ambiguous — list all matches with GUIDs and parent paths
		TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetBoolField(TEXT("success"), false);
		ErrorObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Ambiguous state name '%s' — %d matches found. Use GUID to disambiguate."), *StateIdentifier, Matches.Num()));

		TArray<TSharedPtr<FJsonValue>> MatchesArray;
		for (UStateTreeState* Match : Matches)
		{
			TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
			MatchObj->SetStringField(TEXT("id"), Match->ID.ToString());
			MatchObj->SetStringField(TEXT("name"), Match->Name.ToString());
			// Build parent path
			FString Path = Match->Name.ToString();
			UStateTreeState* Parent = Match->Parent;
			while (Parent)
			{
				Path = Parent->Name.ToString() + TEXT(" > ") + Path;
				Parent = Parent->Parent.Get();
			}
			MatchObj->SetStringField(TEXT("path"), Path);
			MatchesArray.Add(MakeShared<FJsonValueObject>(MatchObj));
		}
		ErrorObj->SetField(TEXT("matches"), MakeShared<FJsonValueArray>(MatchesArray));
		OutError = ErrorObj;
		return nullptr;
	}

	return Matches[0];
}

// ---- Node lookup ----

bool FStateTreeTypeCache::FindNodeInState(UStateTreeState* State, const FGuid& NodeGuid, FNodeSearchResult& OutResult)
{
	if (!State) return false;

	// Check tasks
	for (int32 i = 0; i < State->Tasks.Num(); ++i)
	{
		if (State->Tasks[i].ID == NodeGuid)
		{
			OutResult.Node = &State->Tasks[i];
			OutResult.OwningState = State;
			OutResult.SlotType = TEXT("task");
			OutResult.ArrayIndex = i;
			return true;
		}
	}

	// Check enter conditions
	for (int32 i = 0; i < State->EnterConditions.Num(); ++i)
	{
		if (State->EnterConditions[i].ID == NodeGuid)
		{
			OutResult.Node = &State->EnterConditions[i];
			OutResult.OwningState = State;
			OutResult.SlotType = TEXT("enter_condition");
			OutResult.ArrayIndex = i;
			return true;
		}
	}

	// Check utility considerations
	for (int32 i = 0; i < State->Considerations.Num(); ++i)
	{
		if (State->Considerations[i].ID == NodeGuid)
		{
			OutResult.Node = &State->Considerations[i];
			OutResult.OwningState = State;
			OutResult.SlotType = TEXT("consideration");
			OutResult.ArrayIndex = i;
			return true;
		}
	}

	// Check transitions' conditions
	for (int32 t = 0; t < State->Transitions.Num(); ++t)
	{
		for (int32 c = 0; c < State->Transitions[t].Conditions.Num(); ++c)
		{
			if (State->Transitions[t].Conditions[c].ID == NodeGuid)
			{
				OutResult.Node = &State->Transitions[t].Conditions[c];
				OutResult.OwningState = State;
				OutResult.SlotType = FString::Printf(TEXT("transition_%d_condition"), t);
				OutResult.ArrayIndex = c;
				return true;
			}
		}
	}

	// Recurse into children
	for (UStateTreeState* Child : State->Children)
	{
		if (FindNodeInState(Child, NodeGuid, OutResult))
		{
			return true;
		}
	}

	return false;
}

FStateTreeTypeCache::FNodeSearchResult FStateTreeTypeCache::FindNodeByGuid(UStateTreeEditorData* EditorData, const FGuid& NodeGuid)
{
	FNodeSearchResult Result;
	if (!EditorData) return Result;

	// Check global evaluators
	for (int32 i = 0; i < EditorData->Evaluators.Num(); ++i)
	{
		if (EditorData->Evaluators[i].ID == NodeGuid)
		{
			Result.Node = &EditorData->Evaluators[i];
			Result.OwningState = nullptr;
			Result.SlotType = TEXT("evaluator");
			Result.ArrayIndex = i;
			return Result;
		}
	}

	// Check global tasks
	for (int32 i = 0; i < EditorData->GlobalTasks.Num(); ++i)
	{
		if (EditorData->GlobalTasks[i].ID == NodeGuid)
		{
			Result.Node = &EditorData->GlobalTasks[i];
			Result.OwningState = nullptr;
			Result.SlotType = TEXT("global_task");
			Result.ArrayIndex = i;
			return Result;
		}
	}

	// Walk all states
	for (UStateTreeState* RootState : EditorData->SubTrees)
	{
		if (FindNodeInState(RootState, NodeGuid, Result))
		{
			return Result;
		}
	}

	return Result;
}

// ---- Property serialization ----

TSharedPtr<FJsonObject> FStateTreeTypeCache::SerializeInstanceDataProperties(const FInstancedStruct& Instance)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Instance.IsValid()) return Result;

	const UScriptStruct* Struct = Instance.GetScriptStruct();
	const uint8* Memory = Instance.GetMemory();
	if (!Struct || !Memory) return Result;

	Result->SetStringField(TEXT("struct_type"), Struct->GetName());

	TArray<TSharedPtr<FJsonValue>> PropsArray;
	for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());

		FString Value;
		const uint8* ValuePtr = Memory + Prop->GetOffset_ForInternal();
		Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
		PropObj->SetStringField(TEXT("value"), Value);

		if (!Prop->GetMetaData(TEXT("Category")).IsEmpty())
		{
			PropObj->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));
		}

		PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	Result->SetField(TEXT("properties"), MakeShared<FJsonValueArray>(PropsArray));
	return Result;
}

bool FStateTreeTypeCache::ApplyPropertyOverrides(FInstancedStruct& Instance, const TSharedPtr<FJsonObject>& Properties, FString& OutError)
{
	if (!Instance.IsValid())
	{
		OutError = TEXT("Instance data is not valid");
		return false;
	}

	if (!Properties.IsValid()) return true; // No properties to apply — success

	const UScriptStruct* Struct = Instance.GetScriptStruct();
	uint8* Memory = Instance.GetMutableMemory();

	for (const auto& KV : Properties->Values)
	{
		const FString& PropName = KV.Key;
		const TSharedPtr<FJsonValue>& JsonValue = KV.Value;

		// Guard before FName construction — FName(*Str) (default FNAME_Add) hits a
		// fatal checkf in the name pool (UnrealNames.cpp FindOrStoreString) when the
		// string length >= NAME_SIZE (1024), hard-crashing the editor on a
		// caller-controlled JSON property key.
		if (PropName.Len() >= NAME_SIZE)
		{
			OutError = FString::Printf(TEXT("Property name too long (%d chars; max %d)"), PropName.Len(), NAME_SIZE - 1);
			return false;
		}

		FProperty* Prop = Struct->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on struct '%s'"), *PropName, *Struct->GetName());
			return false;
		}

		// Convert JSON value to text format for ImportText
		FString TextValue;
		if (JsonValue->Type == EJson::String)
		{
			TextValue = JsonValue->AsString();
		}
		else if (JsonValue->Type == EJson::Number)
		{
			TextValue = FString::Printf(TEXT("%g"), JsonValue->AsNumber());
		}
		else if (JsonValue->Type == EJson::Boolean)
		{
			TextValue = JsonValue->AsBool() ? TEXT("true") : TEXT("false");
		}
		else
		{
			// For complex types, serialize the JSON value to string
			FString JsonStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
			FJsonSerializer::Serialize(JsonValue.ToSharedRef(), TEXT(""), Writer);
			TextValue = JsonStr;
		}

		uint8* ValuePtr = Memory + Prop->GetOffset_ForInternal();
		const TCHAR* Result = Prop->ImportText_Direct(*TextValue, ValuePtr, nullptr, PPF_None);
		if (!Result)
		{
			OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s' on struct '%s'"), *PropName, *TextValue, *Struct->GetName());
			return false;
		}
	}

	return true;
}
