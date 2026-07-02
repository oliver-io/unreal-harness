#include "MCPStateTreeCommands.h"
#include "StateTree/StateTreeTypeCache.h"
#include "StateTree/StateTreeStateMgr.h"
#include "StateTree/StateTreeNodeMgr.h"
#include "StateTree/StateTreeTransitionMgr.h"
#include "StateTree/StateTreeBindingMgr.h"
#include "MCPCommonUtils.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeSchema.h"
#include "StateTreeCompiler.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeCompilerManager.h"
#include "StateTreeEditingSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/Factory.h"
#include "StateTreeFactory.h"
#include "UObject/SavePackage.h"
#include "FileHelpers.h"
#include "Logging/TokenizedMessage.h"

namespace
{
	// FStateTreeCompilerLog keeps its Messages array `protected`, so we can't read
	// it directly. ToTokenizedMessages() is public, though, and returns one
	// FTokenizedMessage per entry with severity + formatted text — enough to
	// surface compile errors in MCP responses without log-scraping.
	//
	// This is what makes InstanceData-struct-layout errors visible to MCP callers:
	//   "State 'Flee': Task 'Animal Flee': The instance data type does not match."
	// Pre-fix the response was `{ success:false, messages:[] }` — totally opaque
	// unless you tailed the log.
	TArray<TSharedPtr<FJsonValue>> CompilerLogToJsonArray(const FStateTreeCompilerLog& Log)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		TArray<TSharedRef<FTokenizedMessage>> Tokens = Log.ToTokenizedMessages();
		Array.Reserve(Tokens.Num());
		for (const TSharedRef<FTokenizedMessage>& Msg : Tokens)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			FString SevStr;
			switch (Msg->GetSeverity())
			{
				case EMessageSeverity::Error:              SevStr = TEXT("error");               break;
				case EMessageSeverity::PerformanceWarning: SevStr = TEXT("performance_warning"); break;
				case EMessageSeverity::Warning:            SevStr = TEXT("warning");             break;
				case EMessageSeverity::Info:               SevStr = TEXT("info");                break;
				default:                                    SevStr = TEXT("unknown");             break;
			}
			Entry->SetStringField(TEXT("severity"), SevStr);
			Entry->SetStringField(TEXT("message"), Msg->ToText().ToString());
			Array.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Array;
	}
}

// ---- HandleCommand dispatcher ----

TSharedPtr<FJsonObject> FMCPStateTreeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	// Category 1: Asset Lifecycle
	if (CommandType == TEXT("statetree_create"))        return HandleCreateStateTree(Params);
	if (CommandType == TEXT("statetree_read"))          return HandleReadStateTree(Params);
	if (CommandType == TEXT("statetree_compile"))       return HandleCompileStateTree(Params);
	if (CommandType == TEXT("statetree_save"))          return HandleSaveStateTree(Params);
	if (CommandType == TEXT("statetree_verify"))        return HandleVerifyStateTree(Params);

	// Category 2: State Hierarchy (delegate to FStateTreeStateMgr)
	if (CommandType == TEXT("st_add_state"))             return FStateTreeStateMgr::AddState(Params);
	if (CommandType == TEXT("st_remove_state"))          return FStateTreeStateMgr::RemoveState(Params);
	if (CommandType == TEXT("st_rename_state"))          return FStateTreeStateMgr::RenameState(Params);
	if (CommandType == TEXT("st_move_state"))            return FStateTreeStateMgr::MoveState(Params);
	if (CommandType == TEXT("st_duplicate_state"))       return FStateTreeStateMgr::DuplicateState(Params);
	if (CommandType == TEXT("st_set_state_properties"))  return FStateTreeStateMgr::SetStateProperties(Params);
	if (CommandType == TEXT("st_list_states"))           return FStateTreeStateMgr::ListStates(Params);

	// Category 3: Node Management (delegate to FStateTreeNodeMgr)
	if (CommandType == TEXT("st_add_node"))              return FStateTreeNodeMgr::AddNode(Params);
	if (CommandType == TEXT("st_remove_node"))           return FStateTreeNodeMgr::RemoveNode(Params);
	if (CommandType == TEXT("st_set_node_property"))     return FStateTreeNodeMgr::SetNodeProperty(Params);
	if (CommandType == TEXT("st_get_node_properties"))   return FStateTreeNodeMgr::GetNodeProperties(Params);

	// Category 4: Transitions (delegate to FStateTreeTransitionMgr)
	if (CommandType == TEXT("st_add_transition"))        return FStateTreeTransitionMgr::AddTransition(Params);
	if (CommandType == TEXT("st_remove_transition"))     return FStateTreeTransitionMgr::RemoveTransition(Params);
	if (CommandType == TEXT("st_set_transition_properties")) return FStateTreeTransitionMgr::SetTransitionProperties(Params);

	// Category 5: Property Bindings (delegate to FStateTreeBindingMgr)
	if (CommandType == TEXT("st_add_property_binding"))  return FStateTreeBindingMgr::AddPropertyBinding(Params);
	if (CommandType == TEXT("st_remove_property_binding")) return FStateTreeBindingMgr::RemovePropertyBinding(Params);
	if (CommandType == TEXT("st_list_property_bindings")) return FStateTreeBindingMgr::ListPropertyBindings(Params);
	if (CommandType == TEXT("st_list_bindable_properties")) return FStateTreeBindingMgr::ListBindableProperties(Params);

	// Category 6: Type Introspection
	if (CommandType == TEXT("statetree_list_node_types")) return HandleListStateTreeNodeTypes(Params);
	if (CommandType == TEXT("statetree_list_schemas"))  return HandleListStateTreeSchemas(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown StateTree command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: create_state_tree, read_state_tree, compile_state_tree, save_state_tree, verify_state_tree, st_add_state, st_remove_state, st_rename_state, st_move_state, st_duplicate_state, st_set_state_properties, st_list_states, st_add_node, st_remove_node, st_set_node_property, st_get_node_properties, st_add_transition, st_remove_transition, st_set_transition_properties, st_add_property_binding, st_remove_property_binding, st_list_property_bindings, st_list_bindable_properties, list_state_tree_node_types, list_state_tree_schemas."));
}

// ---- Category 1: Asset Lifecycle ----

TSharedPtr<FJsonObject> FMCPStateTreeCommands::HandleCreateStateTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required and must be the full target asset path including the asset name, e.g. `/Game/AI/Enemy_StateTree`. The folder will be created if it doesn't exist."));
	}

	// Schema is required: UStateTreeFactory::FactoryCreateNew early-returns
	// nullptr when StateTreeSchemaClass is unset (see engine source at
	// Engine/Plugins/Runtime/StateTree/Source/StateTreeEditorModule/Private/
	// StateTreeFactory.cpp:115-118). Pre-fix, this param was optional and the
	// schema was applied AFTER CreateAsset — but CreateAsset had already
	// returned null, so the schema-assignment block was dead code and the
	// whole call produced a cryptic "Failed to create StateTree asset" error
	// with no diagnostic. Mandate and validate up front.
	FString SchemaClassName;
	if (!Params->TryGetStringField(TEXT("schema_class"), SchemaClassName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'schema_class' parameter. Use list_state_tree_schemas to discover valid schema class names."),
			EMCPErrorCode::InvalidArgument,
			TEXT("`schema_class` is required (UStateTreeFactory::FactoryCreateNew returns nullptr if the schema is not set BEFORE asset creation). Use `list_state_tree_schemas` to discover valid schema class names — e.g. `StateTreeAIComponentSchema`, `StateTreeComponentSchema`."));
	}

	UClass* SchemaClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UStateTreeSchema::StaticClass())) continue;
		if (It->HasAnyClassFlags(CLASS_Abstract)) continue;
		if (It->GetName() == SchemaClassName ||
			It->GetName() == FString::Printf(TEXT("U%s"), *SchemaClassName))
		{
			SchemaClass = *It;
			break;
		}
	}
	if (!SchemaClass)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown schema class: '%s'. Use list_state_tree_schemas to discover valid schema class names."), *SchemaClassName),
			EMCPErrorCode::ClassNotLoaded,
			TEXT("`schema_class` must name a non-abstract subclass of UStateTreeSchema. Pass the class name with or without the `U` prefix (e.g. `StateTreeAIComponentSchema` or `UStateTreeAIComponentSchema`). Use `list_state_tree_schemas` to discover the full set of available schema classes."));
	}

	// Parse package path and asset name
	FString PackagePath = FPaths::GetPath(AssetPath);
	FString AssetName = FPaths::GetBaseFilename(AssetPath);
	if (AssetName.IsEmpty())
	{
		AssetName = FPaths::GetBaseFilename(PackagePath);
		PackagePath = FPaths::GetPath(PackagePath);
	}

	// Use AssetTools to create the asset via the StateTree factory.
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Instantiate the factory directly (UStateTreeFactory is visible via the
	// StateTreeEditorModule public dependency). The pre-fix code used TObjectIterator
	// to scan every UFactory subclass — functionally equivalent but slower and
	// ambiguous if another factory ever claims UStateTree::SupportedClass.
	UStateTreeFactory* Factory = NewObject<UStateTreeFactory>(GetTransientPackage());
	if (!Factory)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to instantiate UStateTreeFactory — is StateTreeEditorModule loaded?"),
			EMCPErrorCode::Internal,
			TEXT("UStateTreeFactory could not be allocated via NewObject. This typically indicates StateTreeEditorModule is not loaded — check that the plugin is enabled and that the editor finished initializing. If reproducible, restart the editor."));
	}

	// Set schema BEFORE CreateAsset. UStateTreeFactory::FactoryCreateNew reads
	// StateTreeSchemaClass and bails with nullptr if unset — this is the single
	// line that was missing from the pre-fix implementation.
	Factory->SetSchemaClass(SchemaClass);

	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UStateTree::StaticClass(), Factory);
	if (!NewAsset)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to create StateTree asset at %s/%s with schema '%s'"), *PackagePath, *AssetName, *SchemaClassName),
			EMCPErrorCode::Internal,
			TEXT("AssetTools.CreateAsset returned nullptr despite a validated schema_class. Common causes: (1) destination path is read-only or under source control without checkout; (2) an asset already exists at the path and the factory refused overwrite; (3) the AssetRegistry is mid-scan. Pick a clean destination path, ensure the folder is writable, and retry."));
	}

	UStateTree* NewStateTree = Cast<UStateTree>(NewAsset);

	// Save the package
	UPackage* Package = NewStateTree->GetOutermost();
	Package->MarkPackageDirty();
	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	const bool bSaved = UPackage::SavePackage(Package, NewStateTree, *PackageFileName, SaveArgs);
	if (!bSaved)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("StateTree asset was created in memory but failed to write to disk at %s"), *PackageFileName),
			EMCPErrorCode::Internal,
			TEXT("UPackage::SavePackage returned false — the package exists and is dirty in memory but the file was not written. Common causes: destination path is read-only, the target file is checked out/locked by source control, or the disk is full. Resolve the write barrier, then re-save via save_state_tree."));
	}

	// Build response
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), NewStateTree->GetPathName());
	Result->SetStringField(TEXT("asset_name"), AssetName);

	// Report root state ID if available
	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(NewStateTree->EditorData);
	if (EditorData && EditorData->SubTrees.Num() > 0)
	{
		Result->SetStringField(TEXT("root_state_id"), EditorData->SubTrees[0]->ID.ToString());
	}

	return Result;
}

TSharedPtr<FJsonObject> FMCPStateTreeCommands::HandleReadStateTree(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* StateTree = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!StateTree) return Error;

	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(StateTree, Error);
	if (!EditorData) return Error;

	// Read filter options
	bool bIncludeNodeProperties = true;
	bool bIncludeBindings = true;
	int32 MaxDepth = -1; // unlimited
	Params->TryGetBoolField(TEXT("include_node_properties"), bIncludeNodeProperties);
	Params->TryGetBoolField(TEXT("include_bindings"), bIncludeBindings);

	double MaxDepthD;
	if (Params->TryGetNumberField(TEXT("max_depth"), MaxDepthD))
	{
		MaxDepth = static_cast<int32>(MaxDepthD);
	}

	// Optional: read only a specific subtree
	FString StateIdFilter;
	Params->TryGetStringField(TEXT("state_id"), StateIdFilter);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), StateTree->GetPathName());

	// Schema info
	if (EditorData->Schema)
	{
		Result->SetStringField(TEXT("schema_class"), EditorData->Schema->GetClass()->GetName());
	}

	// Global evaluators
	TArray<TSharedPtr<FJsonValue>> EvalArray;
	for (const FStateTreeEditorNode& EvalNode : EditorData->Evaluators)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("id"), EvalNode.ID.ToString());
		if (EvalNode.Node.IsValid())
		{
			NodeObj->SetStringField(TEXT("type"), EvalNode.Node.GetScriptStruct()->GetName());
		}
		if (bIncludeNodeProperties)
		{
			NodeObj->SetObjectField(TEXT("instance_data"), FStateTreeTypeCache::SerializeInstanceDataProperties(EvalNode.Instance));
		}
		EvalArray.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	Result->SetField(TEXT("evaluators"), MakeShared<FJsonValueArray>(EvalArray));

	// Global tasks
	TArray<TSharedPtr<FJsonValue>> GlobalTaskArray;
	for (const FStateTreeEditorNode& TaskNode : EditorData->GlobalTasks)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("id"), TaskNode.ID.ToString());
		if (TaskNode.Node.IsValid())
		{
			NodeObj->SetStringField(TEXT("type"), TaskNode.Node.GetScriptStruct()->GetName());
		}
		if (bIncludeNodeProperties)
		{
			NodeObj->SetObjectField(TEXT("instance_data"), FStateTreeTypeCache::SerializeInstanceDataProperties(TaskNode.Instance));
		}
		GlobalTaskArray.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	Result->SetField(TEXT("global_tasks"), MakeShared<FJsonValueArray>(GlobalTaskArray));

	// States — use FStateTreeStateMgr's serialization
	TArray<TSharedPtr<FJsonValue>> StatesArray;

	TArray<UStateTreeState*> RootStates;
	if (!StateIdFilter.IsEmpty())
	{
		UStateTreeState* FilterState = FStateTreeTypeCache::FindState(EditorData, StateIdFilter, Error);
		if (!FilterState) return Error;
		RootStates.Add(FilterState);
	}
	else
	{
		RootStates = EditorData->SubTrees;
	}

	// Use a lambda to recursively serialize states
	TFunction<void(UStateTreeState*, int32)> SerializeState;
	SerializeState = [&](UStateTreeState* State, int32 Depth)
	{
		if (!State) return;
		if (MaxDepth >= 0 && Depth > MaxDepth) return;

		TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
		StateObj->SetStringField(TEXT("id"), State->ID.ToString());
		StateObj->SetStringField(TEXT("name"), State->Name.ToString());
		StateObj->SetStringField(TEXT("type"), StaticEnum<EStateTreeStateType>()->GetNameStringByValue(static_cast<int64>(State->Type)));
		// `selection_behavior` / `weight` mirror the fields FStateTreeStateMgr::SetStateProperties
		// writes from the matching JSON params (StateTreeStateMgr.cpp:474-490). Short enum
		// name is canonical for UENUM enum class (e.g. "TryEnterState"), round-trips
		// through the SetStateProperties parser unchanged.
		StateObj->SetStringField(TEXT("selection_behavior"), StaticEnum<EStateTreeStateSelectionBehavior>()->GetNameStringByValue(static_cast<int64>(State->SelectionBehavior)));
		StateObj->SetBoolField(TEXT("enabled"), State->bEnabled);
		StateObj->SetNumberField(TEXT("weight"), State->Weight);
		StateObj->SetNumberField(TEXT("depth"), Depth);
		StateObj->SetNumberField(TEXT("child_count"), State->Children.Num());

		if (UStateTreeState* ParentState = State->Parent.Get())
		{
			StateObj->SetStringField(TEXT("parent_id"), ParentState->ID.ToString());
		}

		// Tasks
		TArray<TSharedPtr<FJsonValue>> TasksArr;
		for (const FStateTreeEditorNode& TaskNode : State->Tasks)
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("id"), TaskNode.ID.ToString());
			if (TaskNode.Node.IsValid())
			{
				NodeObj->SetStringField(TEXT("type"), TaskNode.Node.GetScriptStruct()->GetName());
			}
			if (bIncludeNodeProperties)
			{
				NodeObj->SetObjectField(TEXT("instance_data"), FStateTreeTypeCache::SerializeInstanceDataProperties(TaskNode.Instance));
			}
			TasksArr.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		StateObj->SetField(TEXT("tasks"), MakeShared<FJsonValueArray>(TasksArr));

		// Enter conditions
		// `operand` / `indent` mirror the fields FStateTreeNodeMgr::AddNode writes from the
		// matching JSON params (StateTreeNodeMgr.cpp:128-139) for any slot whose name
		// contains "condition". Emitted together so read→add→read round-trips preserve
		// both the boolean-operator grouping and the visual indent of condition expressions.
		TArray<TSharedPtr<FJsonValue>> CondArr;
		for (const FStateTreeEditorNode& CondNode : State->EnterConditions)
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("id"), CondNode.ID.ToString());
			if (CondNode.Node.IsValid())
			{
				NodeObj->SetStringField(TEXT("type"), CondNode.Node.GetScriptStruct()->GetName());
			}
			NodeObj->SetStringField(TEXT("operand"), StaticEnum<EStateTreeExpressionOperand>()->GetNameStringByValue(static_cast<int64>(CondNode.ExpressionOperand)));
			NodeObj->SetNumberField(TEXT("indent"), CondNode.ExpressionIndent);
			if (bIncludeNodeProperties)
			{
				NodeObj->SetObjectField(TEXT("instance_data"), FStateTreeTypeCache::SerializeInstanceDataProperties(CondNode.Instance));
			}
			CondArr.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		StateObj->SetField(TEXT("enter_conditions"), MakeShared<FJsonValueArray>(CondArr));

		// Utility considerations — same node shape as enter_conditions (id, type,
		// operand, indent, instance_data): the engine compiler consumes
		// ExpressionOperand/ExpressionIndent for considerations exactly like
		// conditions (StateTreeCompiler.cpp:2184-2228). Mirrors the consideration
		// slot FStateTreeNodeMgr::AddNode writes.
		TArray<TSharedPtr<FJsonValue>> ConsArr;
		for (const FStateTreeEditorNode& ConsNode : State->Considerations)
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("id"), ConsNode.ID.ToString());
			if (ConsNode.Node.IsValid())
			{
				NodeObj->SetStringField(TEXT("type"), ConsNode.Node.GetScriptStruct()->GetName());
			}
			NodeObj->SetStringField(TEXT("operand"), StaticEnum<EStateTreeExpressionOperand>()->GetNameStringByValue(static_cast<int64>(ConsNode.ExpressionOperand)));
			NodeObj->SetNumberField(TEXT("indent"), ConsNode.ExpressionIndent);
			if (bIncludeNodeProperties)
			{
				NodeObj->SetObjectField(TEXT("instance_data"), FStateTreeTypeCache::SerializeInstanceDataProperties(ConsNode.Instance));
			}
			ConsArr.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		StateObj->SetField(TEXT("considerations"), MakeShared<FJsonValueArray>(ConsArr));

		// Transitions
		TArray<TSharedPtr<FJsonValue>> TransArr;
		for (const FStateTreeTransition& Trans : State->Transitions)
		{
			TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();
			TransObj->SetStringField(TEXT("id"), Trans.ID.ToString());
			TransObj->SetStringField(TEXT("trigger"), StaticEnum<EStateTreeTransitionTrigger>()->GetNameStringByValue(static_cast<int64>(Trans.Trigger)));
			// `enabled` mirrors FStateTreeTransition::bTransitionEnabled — the same field
			// FStateTreeTransitionMgr::SetTransitionProperties writes from the `enabled`
			// JSON param. The sibling field `bDelayTransition` controls the timing path
			// (delayed vs immediate); it does NOT toggle the transition on/off.
			TransObj->SetBoolField(TEXT("enabled"), Trans.bTransitionEnabled);
			TransObj->SetStringField(TEXT("priority"), StaticEnum<EStateTreeTransitionPriority>()->GetNameStringByValue(static_cast<int64>(Trans.Priority)));
			// `delay` / `delay_variance` mirror DelayDuration / DelayRandomVariance.
			// Reported unconditionally so the read response round-trips with
			// SetTransitionProperties (which derives bDelayTransition from delay > 0).
			TransObj->SetNumberField(TEXT("delay"), Trans.DelayDuration);
			TransObj->SetNumberField(TEXT("delay_variance"), Trans.DelayRandomVariance);
			// `event_tag` mirrors RequiredEvent.Tag — the field st_add_transition /
			// st_set_transition_properties write from the `event_tag` JSON param.
			// Emitted only when set, so read→edit→write round-trips preserve the
			// required-event tag of OnEvent transitions. (RequiredEvent's
			// PayloadStruct / bConsumeEventOnSelect are not MCP-surfaced — see the
			// richer-schema #DEFERRED entry.)
			if (Trans.RequiredEvent.Tag.IsValid())
			{
				TransObj->SetStringField(TEXT("event_tag"), Trans.RequiredEvent.Tag.ToString());
			}

			// Target — every emitted string is a value FStateTreeTransitionMgr::ResolveTransitionTarget
			// (StateTreeTransitionMgr.cpp:11-49) accepts on input, so a read→edit→write round-trip
			// preserves LinkType for every value of EStateTreeTransitionType the engine enum
			// surfaces today. The "Unknown" fallback is reserved for forward-compat (a future
			// engine value not in the switch) and would loud-error on the parser side via
			// "Target state not found: 'Unknown'", which is the right signal — silent acceptance
			// would mask a missing reader branch.
			FString TargetStr;
			switch (Trans.State.LinkType)
			{
			case EStateTreeTransitionType::None: TargetStr = TEXT("None"); break;
			case EStateTreeTransitionType::Succeeded: TargetStr = TEXT("Succeeded"); break;
			case EStateTreeTransitionType::Failed: TargetStr = TEXT("Failed"); break;
			case EStateTreeTransitionType::NextState: TargetStr = TEXT("NextSelectableState"); break;
			case EStateTreeTransitionType::GotoState: TargetStr = Trans.State.Name.ToString(); break;
			default: TargetStr = TEXT("Unknown"); break;
			}
			TransObj->SetStringField(TEXT("target"), TargetStr);
			if (Trans.State.LinkType == EStateTreeTransitionType::GotoState)
			{
				TransObj->SetStringField(TEXT("target_id"), Trans.State.ID.ToString());
			}

			// Transition conditions
			// `operand` / `indent` mirror the fields FStateTreeNodeMgr::AddNode writes for
			// the "condition" slot on a transition (StateTreeNodeMgr.cpp:128-139) and the
			// inline-conditions loop in FStateTreeTransitionMgr::AddTransition
			// (StateTreeTransitionMgr.cpp inline-conditions block). Same round-trip
			// rationale as the enter_conditions serializer above — emit both fields so
			// read→add→read preserves grouping and indentation of transition-condition
			// expressions.
			TArray<TSharedPtr<FJsonValue>> TCondArr;
			for (const FStateTreeEditorNode& TCond : Trans.Conditions)
			{
				TSharedPtr<FJsonObject> TCondObj = MakeShared<FJsonObject>();
				TCondObj->SetStringField(TEXT("id"), TCond.ID.ToString());
				if (TCond.Node.IsValid())
				{
					TCondObj->SetStringField(TEXT("type"), TCond.Node.GetScriptStruct()->GetName());
				}
				TCondObj->SetStringField(TEXT("operand"), StaticEnum<EStateTreeExpressionOperand>()->GetNameStringByValue(static_cast<int64>(TCond.ExpressionOperand)));
				TCondObj->SetNumberField(TEXT("indent"), TCond.ExpressionIndent);
				if (bIncludeNodeProperties)
				{
					TCondObj->SetObjectField(TEXT("instance_data"), FStateTreeTypeCache::SerializeInstanceDataProperties(TCond.Instance));
				}
				TCondArr.Add(MakeShared<FJsonValueObject>(TCondObj));
			}
			TransObj->SetField(TEXT("conditions"), MakeShared<FJsonValueArray>(TCondArr));

			TransArr.Add(MakeShared<FJsonValueObject>(TransObj));
		}
		StateObj->SetField(TEXT("transitions"), MakeShared<FJsonValueArray>(TransArr));

		// Children
		TArray<TSharedPtr<FJsonValue>> ChildArr;
		for (UStateTreeState* Child : State->Children)
		{
			SerializeState(Child, Depth + 1);
		}

		StatesArray.Add(MakeShared<FJsonValueObject>(StateObj));
	};

	for (UStateTreeState* RootState : RootStates)
	{
		SerializeState(RootState, 0);
	}

	Result->SetField(TEXT("states"), MakeShared<FJsonValueArray>(StatesArray));
	return Result;
}

TSharedPtr<FJsonObject> FMCPStateTreeCommands::HandleCompileStateTree(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* StateTree = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!StateTree) return Error;

	bool bAutoSave = false;
	Params->TryGetBoolField(TEXT("auto_save"), bAutoSave);

	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(StateTree, Error);
	if (!EditorData) return Error;

	// Compile via the engine's CompilerManager — NOT via FStateTreeCompiler directly.
	// Pre-fix this called `FStateTreeCompiler::Compile(*StateTree)` and stopped there,
	// skipping three things the proper entry point
	// (`UE::StateTree::Compiler::FCompilerManager::CompileSynchronously`) does:
	//
	//   1. `UStateTreeEditingSubsystem::ValidateStateTree(StateTree)` runs first —
	//      it fixes up renamed state-link references, refreshes linked-state parameter
	//      overrides, and updates transactional flags on editor objects. Without it,
	//      compiles against freshly-mutated editor data can bake stale `FStateTreeStateLink::Name`
	//      values or miss a linked-state parameter that was added since the last compile.
	//
	//   2. On success it writes `StateTree->LastCompiledEditorDataHash = CalculateStateTreeHash(StateTree)`.
	//      This hash is a UPROPERTY and is serialized with the asset. `IsDataValid`
	//      (StateTree.cpp:337) compares a freshly-computed hash against this stamped
	//      value and emits "X is not compiled. Please recompile the State Tree." when
	//      they differ. Pre-fix every MCP-compiled asset failed this check on load
	//      because the stamp was never written. The editor's `StateTreeEditorModeToolkit`
	//      also uses this comparison to decide whether to flash the "needs recompile"
	//      warning — so opening the asset post-MCP always looked dirty.
	//
	//   3. On failure it calls `StateTree->ResetCompiled()` AND zeroes the hash, so the
	//      asset doesn't end up with partial/stale compiled bytecode. Pre-fix a failed
	//      compile left whatever was compiled before still in `Frames`/`States`/`Transitions`
	//      — the caller thought the compile failed but the runtime kept reading the
	//      previous successful compile.
	//
	//   4. Broadcasts `UE::StateTree::Delegates::OnPostCompile` — editor ViewModels
	//      subscribe to this to refresh their UI and to GatherDependencies() for
	//      cross-asset recompile propagation. Pre-fix none of that fired on MCP compile.
	//
	// `FCompilerManager::CompileSynchronously` is public and exported by
	// `STATETREEEDITORMODULE_API` (which our plugin already depends on via
	// `StateTreeEditorModule` in Build.cs).
	FStateTreeCompilerLog CompilerLog;
	const bool bSuccess = UE::StateTree::Compiler::FCompilerManager::CompileSynchronously(StateTree, CompilerLog);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(TEXT("compilation_status"), bSuccess ? TEXT("success") : TEXT("failed"));
	Result->SetNumberField(TEXT("editor_data_hash"), StateTree->LastCompiledEditorDataHash);

	// Also dump to UE log so failures show up in Saved/Logs for users running with -log.
	if (!bSuccess)
	{
		CompilerLog.DumpToLog(LogUnrealMCP);
	}

	// Surface compile-log entries in the response via ToTokenizedMessages() —
	// this is what makes InstanceData layout mismatches and other compile errors
	// visible without tailing the editor log.
	Result->SetField(TEXT("messages"), MakeShared<FJsonValueArray>(CompilerLogToJsonArray(CompilerLog)));

	if (bSuccess && bAutoSave)
	{
		UPackage* Package = StateTree->GetOutermost();
		Package->MarkPackageDirty();
		FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		const bool bSaved = UPackage::SavePackage(Package, StateTree, *PackageFileName, SaveArgs);
		Result->SetBoolField(TEXT("saved"), bSaved);
		if (!bSaved)
		{
			Result->SetStringField(TEXT("save_error"),
				FString::Printf(TEXT("Compiled successfully but UPackage::SavePackage failed to write the package to disk at %s (read-only path, source-control lock, or full disk)."), *PackageFileName));
		}
	}

	return Result;
}

TSharedPtr<FJsonObject> FMCPStateTreeCommands::HandleSaveStateTree(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* StateTree = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!StateTree) return Error;

	// Auto-compile before save unless the caller explicitly opts out.
	//
	// The UStateTree asset stores TWO copies of state-tree data as UPROPERTYs:
	//   * Author-time: `UStateTreeEditorData` (states, nodes, transitions, bindings)
	//   * Compiled: `Frames`, `States`, `Transitions`, `Nodes`, `IDToStateMappings`,
	//     `PropertyBindings` — the flat runtime representation the game reads.
	//
	// Pre-fix this handler just saved the current in-memory state. Every MCP flow
	// that called st_add_state/st_add_node/... and then save_state_tree without an
	// intermediate compile_state_tree wrote **new editor data + stale compiled data**
	// to disk. The stale compiled data kept being read at runtime, so the AI would
	// silently use the old state graph despite the asset LOOKING correct in the
	// editor. This matches the reported "blendspace reverts on editor close" pattern
	// — same class of bug, different asset type.
	//
	// Default: auto-compile. Opt-out: pass `compile_first: false` when you truly want
	// a raw save (e.g. to preserve a known-bad state for debugging, or when the
	// caller has already compiled via compile_state_tree and wants to skip the
	// redundant compile).
	bool bCompileFirst = true;
	Params->TryGetBoolField(TEXT("compile_first"), bCompileFirst);

	TSharedPtr<FJsonObject> CompileInfo;
	if (bCompileFirst)
	{
		FStateTreeCompilerLog CompilerLog;
		const bool bCompiled = UE::StateTree::Compiler::FCompilerManager::CompileSynchronously(StateTree, CompilerLog);
		CompileInfo = MakeShared<FJsonObject>();
		CompileInfo->SetBoolField(TEXT("compiled"), bCompiled);
		CompileInfo->SetNumberField(TEXT("editor_data_hash"), StateTree->LastCompiledEditorDataHash);

		if (!bCompiled)
		{
			CompilerLog.DumpToLog(LogUnrealMCP);
			// Don't proceed to save a broken asset. Return the compile failure up
			// with the full compiler log so the caller can act on it without
			// tailing the editor log.
			CompileInfo->SetField(TEXT("messages"), MakeShared<FJsonValueArray>(CompilerLogToJsonArray(CompilerLog)));
			TSharedPtr<FJsonObject> FailResult = MakeShared<FJsonObject>();
			FailResult->SetBoolField(TEXT("success"), false);
			FailResult->SetStringField(TEXT("error"),
				FString::Printf(TEXT("Pre-save compile failed for %s; asset NOT saved. See compile.messages for details. "
					"Pass compile_first=false to save anyway (will write stale compiled data)."),
					*StateTree->GetPathName()));
			FailResult->SetObjectField(TEXT("compile"), CompileInfo);
			return FailResult;
		}
		// Compile succeeded — include messages (likely empty but may contain warnings)
		CompileInfo->SetField(TEXT("messages"), MakeShared<FJsonValueArray>(CompilerLogToJsonArray(CompilerLog)));
	}

	UPackage* Package = StateTree->GetOutermost();
	Package->MarkPackageDirty();
	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	bool bSaved = UPackage::SavePackage(Package, StateTree, *PackageFileName, SaveArgs);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSaved);
	if (CompileInfo.IsValid())
	{
		Result->SetObjectField(TEXT("compile"), CompileInfo);
	}
	if (bSaved)
	{
		Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Saved %s"), *StateTree->GetPathName()));
	}
	else
	{
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to save %s"), *StateTree->GetPathName()));
	}
	return Result;
}

TSharedPtr<FJsonObject> FMCPStateTreeCommands::HandleVerifyStateTree(const TSharedPtr<FJsonObject>& Params)
{
	// Non-mutating diagnostic tool. Answers two orthogonal questions that together
	// describe every "StateTree looks fine but AI isn't running" situation:
	//
	//   1. Does the compiled data on disk match the author-time data?
	//      Compare `LastCompiledEditorDataHash` (UPROPERTY stamped by the compiler)
	//      against a freshly-computed hash from `UStateTreeEditingSubsystem::CalculateStateTreeHash`.
	//      Mismatch -> author-time edits were never compiled+saved (the "post-rebuild
	//      save_state_tree" workflow gotcha) or the compiled blob got stamped wrong.
	//
	//   2. Did the runtime successfully link the compiled data?
	//      `IsReadyToRun()` returns true only after UStateTree::Link() succeeds —
	//      which is exactly where `UStateTreeComponent::ValidateStateTreeReference`
	//      fails with "The State Tree schema is not compatible" when a task's
	//      InstanceData struct layout changed since last compile (the InstanceData-
	//      struct-mutation engine-level limitation).
	//
	// Returns status = "ok" | "stale" | "not_ready" with an actionable hint.
	// Does NOT mutate the asset — safe to call as often as needed.

	TSharedPtr<FJsonObject> Error;
	UStateTree* StateTree = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!StateTree) return Error;

	const uint32 StoredHash = StateTree->LastCompiledEditorDataHash;
	const uint32 FreshHash = UStateTreeEditingSubsystem::CalculateStateTreeHash(StateTree);
	const bool bHashStamped = (StoredHash != 0);
	const bool bHashMatches = bHashStamped && (StoredHash == FreshHash);
	const bool bReadyToRun = StateTree->IsReadyToRun();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), StateTree->GetPathName());
	Result->SetNumberField(TEXT("stored_hash"), StoredHash);
	Result->SetNumberField(TEXT("fresh_hash"), FreshHash);
	Result->SetBoolField(TEXT("hash_stamped"), bHashStamped);
	Result->SetBoolField(TEXT("hash_matches"), bHashMatches);
	Result->SetBoolField(TEXT("ready_to_run"), bReadyToRun);

	FString Status;
	FString Hint;
	if (bHashMatches && bReadyToRun)
	{
		Status = TEXT("ok");
	}
	else if (!bHashStamped)
	{
		Status = TEXT("never_compiled");
		Hint = TEXT("StateTree has no compiled-data hash stamp — it has never been compiled via the editor. "
		            "Call compile_state_tree then save_state_tree to generate compiled runtime data.");
	}
	else if (!bHashMatches)
	{
		Status = TEXT("stale");
		Hint = TEXT("Author-time data has diverged from the compiled hash — asset was mutated since last compile. "
		            "Call compile_state_tree then save_state_tree.");
	}
	else
	{
		// Hash stamped and matches, but runtime Link() failed. Almost always a
		// task-InstanceData or schema-type struct layout mismatch after a C++ rebuild.
		Status = TEXT("not_ready");
		Hint = TEXT("Compile hash matches but runtime Link() failed — typically a task InstanceData "
		            "or schema struct layout change after a C++ rebuild. Call compile_state_tree first; "
		            "if it reports 'instance data type does not match', st_remove_node the affected task "
		            "and st_add_node a fresh one of the same type, then compile_state_tree + save_state_tree.");
	}
	Result->SetStringField(TEXT("status"), Status);
	if (!Hint.IsEmpty())
	{
		Result->SetStringField(TEXT("hint"), Hint);
	}

	return Result;
}

// ---- Category 6: Type Introspection ----

TSharedPtr<FJsonObject> FMCPStateTreeCommands::HandleListStateTreeNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	FString BaseClassFilter = TEXT("all");
	FString NamePattern;
	Params->TryGetStringField(TEXT("base_class"), BaseClassFilter);
	Params->TryGetStringField(TEXT("name_pattern"), NamePattern);

	TSharedPtr<FJsonObject> Result = FStateTreeTypeCache::Get().ListNodeTypes(BaseClassFilter, NamePattern);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

TSharedPtr<FJsonObject> FMCPStateTreeCommands::HandleListStateTreeSchemas(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = FStateTreeTypeCache::Get().ListSchemaTypes();
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}
