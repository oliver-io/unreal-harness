#include "StateTreeStateMgr.h"
#include "StateTreeTypeCache.h"
#include "StateTreeNodeMgr.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "MCPCommonUtils.h"

// Forward-declare inline node construction helper used by AddState for inline tasks/conditions
static bool ProcessInlineNodes(UStateTreeState* State, const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& Result, FString& OutError);

// ---- AddState ----

TSharedPtr<FJsonObject> FStateTreeStateMgr::AddState(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString StateName;
	if (!Params->TryGetStringField(TEXT("name"), StateName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `name` (string) — the new state's name (PascalCase convention)."));
	}

	FString ParentId;
	Params->TryGetStringField(TEXT("parent"), ParentId);

	// State type / selection behavior — validate the strings against the canonical
	// short names emitted by HandleReadStateTree / ListStates (via StaticEnum<...>
	// ->GetNameStringByValue). Pre-bundle, unrecognized values silently fell
	// through to the UE struct default with no error returned. Validate up front
	// so typos surface as loud errors before any UObject is created.
	EStateTreeStateType StateType = EStateTreeStateType::State;
	{
		FString TypeStr;
		if (Params->TryGetStringField(TEXT("type"), TypeStr))
		{
			if (TypeStr == TEXT("State"))         StateType = EStateTreeStateType::State;
			else if (TypeStr == TEXT("Group"))    StateType = EStateTreeStateType::Group;
			else if (TypeStr == TEXT("Subtree"))  StateType = EStateTreeStateType::Subtree;
			else return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Invalid 'type' value '%s'. Valid: State, Group, Subtree."), *TypeStr),
				EMCPErrorCode::InvalidArgument,
				TEXT("Closed set: \"State\", \"Group\", \"Subtree\". Case-sensitive."));
		}
	}

	bool bSetSelectionBehavior = false;
	EStateTreeStateSelectionBehavior SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
	{
		FString SelectionStr;
		if (Params->TryGetStringField(TEXT("selection_behavior"), SelectionStr))
		{
			bSetSelectionBehavior = true;
			if (SelectionStr == TEXT("TryEnterState"))                  SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
			else if (SelectionStr == TEXT("TrySelectChildrenInOrder"))  SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
			else if (SelectionStr == TEXT("TryFollowTransitions"))      SelectionBehavior = EStateTreeStateSelectionBehavior::TryFollowTransitions;
			else return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Invalid 'selection_behavior' value '%s'. Valid: TryEnterState, TrySelectChildrenInOrder, TryFollowTransitions."), *SelectionStr),
				EMCPErrorCode::InvalidArgument,
				TEXT("Closed set: \"TryEnterState\", \"TrySelectChildrenInOrder\", \"TryFollowTransitions\". Case-sensitive."));
		}
	}

	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	// Insert index
	int32 InsertIndex = INDEX_NONE;
	double InsertIdxD;
	if (Params->TryGetNumberField(TEXT("insert_index"), InsertIdxD))
	{
		InsertIndex = static_cast<int32>(InsertIdxD);
	}

	UStateTreeState* NewState = nullptr;

	// Pre-resolve the parent (if specified) so dry-run can validate it without
	// invoking Modify() / NewObject. The else branch below also calls FindState
	// — the resolution is idempotent so duplicating it on dry-run is safe.
	UStateTreeState* PreResolvedParent = nullptr;
	if (!ParentId.IsEmpty())
	{
		TSharedPtr<FJsonObject> LookupError;
		PreResolvedParent = FStateTreeTypeCache::FindState(EditorData, ParentId, LookupError);
		if (!PreResolvedParent) return LookupError;
	}

	// dry_run: every preflight ran (state tree load, editor data, name,
	// parent lookup, type/selection_behavior validation). NewObject + Modify
	// is the side effect we skip. Diff shape per todo/13 phase 5: states[]
	// per the doc table — extended with the per-state metadata that's
	// already validated.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		auto StateTypeToString = [](EStateTreeStateType T)
		{
			switch (T)
			{
			case EStateTreeStateType::State:    return TEXT("State");
			case EStateTreeStateType::Group:    return TEXT("Group");
			case EStateTreeStateType::Subtree:  return TEXT("Subtree");
			default:                            return TEXT("Unknown");
			}
		};
		auto SelectionBehaviorToString = [](EStateTreeStateSelectionBehavior B)
		{
			switch (B)
			{
			case EStateTreeStateSelectionBehavior::TryEnterState:             return TEXT("TryEnterState");
			case EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder:  return TEXT("TrySelectChildrenInOrder");
			case EStateTreeStateSelectionBehavior::TryFollowTransitions:      return TEXT("TryFollowTransitions");
			default:                                                           return TEXT("Unknown");
			}
		};

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), StateName);
		Entry->SetStringField(TEXT("type"), StateTypeToString(StateType));
		Entry->SetBoolField(TEXT("enabled"), bEnabled);
		if (bSetSelectionBehavior)
		{
			Entry->SetStringField(TEXT("selection_behavior"), SelectionBehaviorToString(SelectionBehavior));
		}
		if (PreResolvedParent)
		{
			Entry->SetStringField(TEXT("parent_id"), ParentId);
			Entry->SetStringField(TEXT("parent_name"), PreResolvedParent->Name.ToString());
			const int32 SiblingCount = PreResolvedParent->Children.Num();
			Entry->SetNumberField(TEXT("would_be_index"),
				(InsertIndex >= 0 && InsertIndex < SiblingCount) ? InsertIndex : SiblingCount);
		}
		else
		{
			Entry->SetBoolField(TEXT("is_root"), true);
			const int32 SubtreeCount = EditorData->SubTrees.Num();
			Entry->SetNumberField(TEXT("would_be_index"),
				(InsertIndex >= 0 && InsertIndex < SubtreeCount) ? InsertIndex : SubtreeCount);
		}

		TArray<TSharedPtr<FJsonValue>> StatesArr;
		StatesArr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("states"), StatesArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Modify() + RF_Transactional mirrors FStateTreeViewModel::AddState — marks the
	// package dirty (so the editor/Content-Browser reflect unsaved changes and a
	// subsequent save actually writes), and tags the new state as undo-tracked so
	// it plays nicely with the transaction system. Pre-fix neither was done, so
	// MCP mutations left the asset technically-dirty-at-the-field-level but with
	// no dirty flag on the package — explicit save via save_state_tree still worked
	// (we MarkPackageDirty there), but if the user opened the asset in the editor
	// mid-session the ViewModel had no idea state had been added behind its back.
	if (ParentId.IsEmpty())
	{
		// Add as root-level subtree
		EditorData->Modify();
		NewState = NewObject<UStateTreeState>(EditorData, FName(*StateName), RF_Transactional);
		NewState->Name = FName(*StateName);
		NewState->Type = StateType;
		NewState->ID = FGuid::NewGuid();
		NewState->bEnabled = bEnabled;
		NewState->Parent = nullptr; // Root-level states have no parent state

		if (InsertIndex >= 0 && InsertIndex < EditorData->SubTrees.Num())
		{
			EditorData->SubTrees.Insert(NewState, InsertIndex);
		}
		else
		{
			EditorData->SubTrees.Add(NewState);
		}
	}
	else
	{
		// Find parent state
		UStateTreeState* ParentState = FStateTreeTypeCache::FindState(EditorData, ParentId, Error);
		if (!ParentState) return Error;

		ParentState->Modify();
		NewState = NewObject<UStateTreeState>(ParentState, FName(*StateName), RF_Transactional);
		NewState->Name = FName(*StateName);
		NewState->Type = StateType;
		NewState->ID = FGuid::NewGuid();
		NewState->bEnabled = bEnabled;
		NewState->Parent = ParentState;

		if (InsertIndex >= 0 && InsertIndex < ParentState->Children.Num())
		{
			ParentState->Children.Insert(NewState, InsertIndex);
		}
		else
		{
			ParentState->Children.Add(NewState);
		}
	}

	// Apply pre-validated selection_behavior (omitted → leave UE default).
	if (bSetSelectionBehavior)
	{
		NewState->SelectionBehavior = SelectionBehavior;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Process inline tasks/conditions BEFORE finalizing success. AddState must be
	// atomic: if any inline node fails to construct, the newly-created state is
	// removed from its parent (or EditorData->SubTrees for root states) and a
	// hard error is returned. Pre-fix, success=true was set first and any inline
	// failure produced only an `inline_warning` — callers (LLM agents) treated
	// that as failure and retried, ending up with two states sharing the same
	// `Name` FProperty (NewObject auto-renames the UObject name to avoid the
	// collision, but the `Name` field stays identical on both). FindState then
	// returns "Ambiguous state name" on lookup, and the partial state can only
	// be removed by editing the asset in the editor UI.
	FString InlineError;
	ProcessInlineNodes(NewState, Params, Result, InlineError);
	if (!InlineError.IsEmpty())
	{
		// Rollback: detach NewState from wherever AddState placed it. The branch
		// mirrors the parent-vs-root logic above — Parent is nullptr for root
		// subtrees (set explicitly at line 87) and a real pointer otherwise.
		if (UStateTreeState* ParentState = NewState->Parent.Get())
		{
			ParentState->Children.Remove(NewState);
		}
		else
		{
			EditorData->SubTrees.Remove(NewState);
		}

		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Inline node creation failed; state '%s' was rolled back: %s"),
				*StateName, *InlineError),
			EMCPErrorCode::InvalidArgument,
			TEXT("One or more inline tasks/enter_conditions in the state's payload failed validation (type lookup, ApplyPropertyOverrides, operand parse). The new state was rolled back atomically — see the message for the specific per-condition failure."));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("state_id"), NewState->ID.ToString());
	Result->SetStringField(TEXT("name"), StateName);
	return Result;
}

// Process inline task/condition/transition specs on an AddState call
static bool ProcessInlineNodes(UStateTreeState* State, const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& Result, FString& OutError)
{
	// Inline tasks
	const TArray<TSharedPtr<FJsonValue>>* TasksJson = nullptr;
	if (Params->TryGetArrayField(TEXT("tasks"), TasksJson))
	{
		TArray<TSharedPtr<FJsonValue>> TaskIds;
		for (const auto& TaskVal : *TasksJson)
		{
			const TSharedPtr<FJsonObject>* TaskObj;
			if (!TaskVal->TryGetObject(TaskObj)) continue;

			FString NodeId;
			FString Err;
			if (FStateTreeNodeMgr::AddNodeInline(State, TEXT("task"), *TaskObj, NodeId, Err))
			{
				TaskIds.Add(MakeShared<FJsonValueString>(NodeId));
			}
			else
			{
				OutError += Err + TEXT("; ");
			}
		}
		Result->SetField(TEXT("task_ids"), MakeShared<FJsonValueArray>(TaskIds));
	}

	// Inline enter conditions
	const TArray<TSharedPtr<FJsonValue>>* CondsJson = nullptr;
	if (Params->TryGetArrayField(TEXT("enter_conditions"), CondsJson))
	{
		TArray<TSharedPtr<FJsonValue>> CondIds;
		for (const auto& CondVal : *CondsJson)
		{
			const TSharedPtr<FJsonObject>* CondObj;
			if (!CondVal->TryGetObject(CondObj)) continue;

			FString NodeId;
			FString Err;
			if (FStateTreeNodeMgr::AddNodeInline(State, TEXT("enter_condition"), *CondObj, NodeId, Err))
			{
				CondIds.Add(MakeShared<FJsonValueString>(NodeId));
			}
			else
			{
				OutError += Err + TEXT("; ");
			}
		}
		Result->SetField(TEXT("condition_ids"), MakeShared<FJsonValueArray>(CondIds));
	}

	return OutError.IsEmpty();
}

// ---- RemoveState ----

TSharedPtr<FJsonObject> FStateTreeStateMgr::RemoveState(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString StateId;
	if (!Params->TryGetStringField(TEXT("state"), StateId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `state` (string) — the name or GUID of the state to operate on. Use `statetree_state_list` to discover."));
	}

	UStateTreeState* State = FStateTreeTypeCache::FindState(EditorData, StateId, Error);
	if (!State) return Error;

	auto StateTypeToString = [](EStateTreeStateType T)
	{
		switch (T)
		{
		case EStateTreeStateType::State:    return TEXT("State");
		case EStateTreeStateType::Group:    return TEXT("Group");
		case EStateTreeStateType::Subtree:  return TEXT("Subtree");
		default:                            return TEXT("Unknown");
		}
	};

	// Walk state + descendants once. Used by both dry-run (diff) and the apply
	// branch (count). Each entry mirrors what AddState dry-run emits per state,
	// extended with the GUID of the state itself so callers can map the diff
	// onto their previous read_state_tree output.
	struct FRemovedEntry
	{
		FGuid Id;
		FName Name;
		EStateTreeStateType Type;
		FGuid ParentId;
		FName ParentName;
		bool bIsRoot;
	};
	TArray<FRemovedEntry> RemovedEntries;
	TSet<FGuid> RemovedIdSet;
	TFunction<void(UStateTreeState*)> CollectStates;
	CollectStates = [&](UStateTreeState* S) {
		if (!S) return;
		FRemovedEntry E;
		E.Id = S->ID;
		E.Name = S->Name;
		E.Type = S->Type;
		if (UStateTreeState* P = S->Parent.Get())
		{
			E.bIsRoot = false;
			E.ParentId = P->ID;
			E.ParentName = P->Name;
		}
		else
		{
			E.bIsRoot = true;
		}
		RemovedEntries.Add(E);
		RemovedIdSet.Add(S->ID);
		for (UStateTreeState* Child : S->Children) CollectStates(Child);
	};
	CollectStates(State);

	// dry_run: every preflight ran (state tree load, editor data, state lookup).
	// The remaining work — Modify() and container removal — is the side effect we
	// skip. Diff shape per todo/13 phase 5: states_removed[] + transitions_orphaned[].
	// Orphans are computed by walking every transition on every OTHER state in the
	// asset and finding any whose target.ID is in RemovedIdSet. (Transitions on
	// removed states themselves vanish with their owner — they're not "orphaned",
	// just gone.)
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		auto BuildEntry = [&](const FRemovedEntry& E) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("id"), E.Id.ToString(EGuidFormats::DigitsWithHyphens));
			Obj->SetStringField(TEXT("name"), E.Name.ToString());
			Obj->SetStringField(TEXT("type"), StateTypeToString(E.Type));
			if (E.bIsRoot)
			{
				Obj->SetBoolField(TEXT("is_root"), true);
			}
			else
			{
				Obj->SetStringField(TEXT("parent_id"), E.ParentId.ToString(EGuidFormats::DigitsWithHyphens));
				Obj->SetStringField(TEXT("parent_name"), E.ParentName.ToString());
			}
			return Obj;
		};

		TArray<TSharedPtr<FJsonValue>> RemovedArr;
		for (const FRemovedEntry& E : RemovedEntries)
		{
			RemovedArr.Add(MakeShared<FJsonValueObject>(BuildEntry(E)));
		}

		// Orphan scan: walk the full tree and visit transitions on every state NOT
		// in RemovedIdSet. A transition is orphaned iff its LinkType resolves to a
		// concrete target (GotoState) and that target's ID is in RemovedIdSet.
		TArray<TSharedPtr<FJsonValue>> OrphanArr;
		TFunction<void(UStateTreeState*)> ScanForOrphans;
		ScanForOrphans = [&](UStateTreeState* S) {
			if (!S) return;
			if (!RemovedIdSet.Contains(S->ID))
			{
				for (const FStateTreeTransition& Trans : S->Transitions)
				{
					if (Trans.State.LinkType == EStateTreeTransitionType::GotoState
						&& RemovedIdSet.Contains(Trans.State.ID))
					{
						TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
						O->SetStringField(TEXT("transition_id"), Trans.ID.ToString(EGuidFormats::DigitsWithHyphens));
						O->SetStringField(TEXT("from_state_id"), S->ID.ToString(EGuidFormats::DigitsWithHyphens));
						O->SetStringField(TEXT("from_state_name"), S->Name.ToString());
						O->SetStringField(TEXT("target_id_was"), Trans.State.ID.ToString(EGuidFormats::DigitsWithHyphens));
						O->SetStringField(TEXT("target_name_was"), Trans.State.Name.ToString());
						OrphanArr.Add(MakeShared<FJsonValueObject>(O));
					}
				}
			}
			for (UStateTreeState* Child : S->Children) ScanForOrphans(Child);
		};
		for (UStateTreeState* RootState : EditorData->SubTrees)
		{
			ScanForOrphans(RootState);
		}

		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("states_removed"), RemovedArr);
		Diff->SetArrayField(TEXT("transitions_orphaned"), OrphanArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Modify() the parent (or root EditorData) BEFORE the container mutation —
	// matches FStateTreeViewModel::RemoveSelectedStates. Marks package dirty.
	if (UStateTreeState* ParentState = State->Parent.Get())
	{
		ParentState->Modify();
		ParentState->Children.Remove(State);
	}
	else
	{
		// Root-level state
		EditorData->Modify();
		EditorData->SubTrees.Remove(State);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("removed_count"), RemovedEntries.Num());
	return Result;
}

// ---- RenameState ----

TSharedPtr<FJsonObject> FStateTreeStateMgr::RenameState(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString StateId;
	if (!Params->TryGetStringField(TEXT("state"), StateId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `state` (string) — the name or GUID of the state to operate on. Use `statetree_state_list` to discover."));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'new_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `new_name` (string) — the desired new state name. Must be unique within the same parent."));
	}

	UStateTreeState* State = FStateTreeTypeCache::FindState(EditorData, StateId, Error);
	if (!State) return Error;

	State->Modify();
	FString OldName = State->Name.ToString();
	State->Name = FName(*NewName);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("old_name"), OldName);
	Result->SetStringField(TEXT("new_name"), NewName);
	return Result;
}

// ---- MoveState ----

TSharedPtr<FJsonObject> FStateTreeStateMgr::MoveState(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString StateId;
	if (!Params->TryGetStringField(TEXT("state"), StateId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `state` (string) — the name or GUID of the state to operate on. Use `statetree_state_list` to discover."));
	}

	UStateTreeState* State = FStateTreeTypeCache::FindState(EditorData, StateId, Error);
	if (!State) return Error;

	FString NewParentId;
	Params->TryGetStringField(TEXT("new_parent"), NewParentId);

	int32 InsertIndex = INDEX_NONE;
	double InsertIdxD;
	if (Params->TryGetNumberField(TEXT("insert_index"), InsertIdxD))
	{
		InsertIndex = static_cast<int32>(InsertIdxD);
	}

	// Resolve and validate the target parent BEFORE any mutation. Detaching
	// first and failing on FindState/cycle afterwards would leave the state
	// orphaned (removed from its old parent but attached to nothing).
	// NewParent == nullptr after this block means "move to root".
	UStateTreeState* NewParent = nullptr;
	if (!NewParentId.IsEmpty())
	{
		NewParent = FStateTreeTypeCache::FindState(EditorData, NewParentId, Error);
		if (!NewParent) return Error;

		// Reject cycles: NewParent must not be State nor any descendant of
		// State. Walking NewParent->Parent up to null reaches State iff
		// NewParent is inside State's subtree.
		UStateTreeState* Ancestor = NewParent;
		while (Ancestor)
		{
			if (Ancestor == State)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("Cannot move a state to be its own descendant"),
					EMCPErrorCode::CircularDependency,
					TEXT("StateTree hierarchy must remain a DAG — a state can't be reparented under itself or one of its own descendants. Pick a different `new_parent`."));
			}
			Ancestor = Ancestor->Parent.Get();
		}
	}

	// Modify() both endpoints + the state itself (parent pointer is about to change).
	State->Modify();

	// Detach from current parent. All failure paths above this line are
	// now safe (the state is still attached to its original parent).
	if (UStateTreeState* ParentState = State->Parent.Get())
	{
		ParentState->Modify();
		ParentState->Children.Remove(State);
	}
	else
	{
		EditorData->Modify();
		EditorData->SubTrees.Remove(State);
	}

	// Attach to resolved parent (or root if NewParent is null).
	if (NewParent == nullptr)
	{
		EditorData->Modify();
		State->Parent = nullptr;
		if (InsertIndex >= 0 && InsertIndex < EditorData->SubTrees.Num())
			EditorData->SubTrees.Insert(State, InsertIndex);
		else
			EditorData->SubTrees.Add(State);
	}
	else
	{
		NewParent->Modify();
		State->Parent = NewParent;
		if (InsertIndex >= 0 && InsertIndex < NewParent->Children.Num())
			NewParent->Children.Insert(State, InsertIndex);
		else
			NewParent->Children.Add(State);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("state_id"), State->ID.ToString());
	return Result;
}

// ---- DuplicateState ----

TSharedPtr<FJsonObject> FStateTreeStateMgr::DuplicateState(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString StateId;
	if (!Params->TryGetStringField(TEXT("state"), StateId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `state` (string) — the name or GUID of the state to operate on. Use `statetree_state_list` to discover."));
	}

	UStateTreeState* SourceState = FStateTreeTypeCache::FindState(EditorData, StateId, Error);
	if (!SourceState) return Error;

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		NewName = SourceState->Name.ToString() + TEXT("_Copy");
	}

	// Deep copy using DuplicateObject
	UObject* ParentObj = SourceState->Parent ? static_cast<UObject*>(SourceState->Parent.Get()) : static_cast<UObject*>(EditorData);
	UStateTreeState* NewState = DuplicateObject<UStateTreeState>(SourceState, ParentObj, FName(*NewName));
	NewState->Name = FName(*NewName);

	// Assign fresh GUIDs recursively
	TMap<FGuid, FGuid> GuidMap; // old -> new
	TFunction<void(UStateTreeState*)> ReassignGuids;
	ReassignGuids = [&](UStateTreeState* S) {
		if (!S) return;
		FGuid OldId = S->ID;
		S->ID = FGuid::NewGuid();
		GuidMap.Add(OldId, S->ID);

		for (FStateTreeEditorNode& Node : S->Tasks)
		{
			FGuid OldNodeId = Node.ID;
			Node.ID = FGuid::NewGuid();
			GuidMap.Add(OldNodeId, Node.ID);
		}
		for (FStateTreeEditorNode& Node : S->EnterConditions)
		{
			FGuid OldNodeId = Node.ID;
			Node.ID = FGuid::NewGuid();
			GuidMap.Add(OldNodeId, Node.ID);
		}
		for (FStateTreeEditorNode& Node : S->Considerations)
		{
			FGuid OldNodeId = Node.ID;
			Node.ID = FGuid::NewGuid();
			GuidMap.Add(OldNodeId, Node.ID);
		}
		for (FStateTreeTransition& Trans : S->Transitions)
		{
			FGuid OldTransId = Trans.ID;
			Trans.ID = FGuid::NewGuid();
			GuidMap.Add(OldTransId, Trans.ID);

			for (FStateTreeEditorNode& Cond : Trans.Conditions)
			{
				FGuid OldCondId = Cond.ID;
				Cond.ID = FGuid::NewGuid();
				GuidMap.Add(OldCondId, Cond.ID);
			}
		}
		for (UStateTreeState* Child : S->Children) ReassignGuids(Child);
	};
	ReassignGuids(NewState);

	// Add to parent's children — Modify() the insertion target before mutating its array.
	if (UStateTreeState* ParentState = SourceState->Parent.Get())
	{
		ParentState->Modify();
		NewState->Parent = ParentState;
		ParentState->Children.Add(NewState);
	}
	else
	{
		EditorData->Modify();
		NewState->Parent = nullptr;
		EditorData->SubTrees.Add(NewState);
	}

	// Build guid mapping response
	TSharedPtr<FJsonObject> MappingObj = MakeShared<FJsonObject>();
	for (const auto& Pair : GuidMap)
	{
		MappingObj->SetStringField(Pair.Key.ToString(), Pair.Value.ToString());
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("state_id"), NewState->ID.ToString());
	Result->SetStringField(TEXT("name"), NewName);
	Result->SetObjectField(TEXT("guid_mapping"), MappingObj);
	return Result;
}

// ---- SetStateProperties ----

TSharedPtr<FJsonObject> FStateTreeStateMgr::SetStateProperties(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString StateId;
	if (!Params->TryGetStringField(TEXT("state"), StateId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `state` (string) — the name or GUID of the state to operate on. Use `statetree_state_list` to discover."));
	}

	UStateTreeState* State = FStateTreeTypeCache::FindState(EditorData, StateId, Error);
	if (!State) return Error;

	// Validate-before-mutate: parse enum-typed fields into locals first; if any
	// fails, return an error before any field has been written. Pre-bundle, each
	// inline if/else chain silently no-op'd on unrecognized strings (typo →
	// existing field unchanged, success response). Same atomicity pattern as
	// the MoveState fix — closes a validate-after-mutate hazard on this setter.
	FString Val;

	bool bWantType = false;
	EStateTreeStateType ParsedType = State->Type;
	if (Params->TryGetStringField(TEXT("type"), Val))
	{
		bWantType = true;
		if (Val == TEXT("State"))         ParsedType = EStateTreeStateType::State;
		else if (Val == TEXT("Group"))    ParsedType = EStateTreeStateType::Group;
		else if (Val == TEXT("Subtree"))  ParsedType = EStateTreeStateType::Subtree;
		else return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid 'type' value '%s'. Valid: State, Group, Subtree."), *Val),
			EMCPErrorCode::InvalidArgument,
			TEXT("Closed set: \"State\", \"Group\", \"Subtree\". Case-sensitive."));
	}

	bool bWantSelection = false;
	EStateTreeStateSelectionBehavior ParsedSelection = State->SelectionBehavior;
	if (Params->TryGetStringField(TEXT("selection_behavior"), Val))
	{
		bWantSelection = true;
		if (Val == TEXT("TryEnterState"))                  ParsedSelection = EStateTreeStateSelectionBehavior::TryEnterState;
		else if (Val == TEXT("TrySelectChildrenInOrder"))  ParsedSelection = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
		else if (Val == TEXT("TryFollowTransitions"))      ParsedSelection = EStateTreeStateSelectionBehavior::TryFollowTransitions;
		else return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid 'selection_behavior' value '%s'. Valid: TryEnterState, TrySelectChildrenInOrder, TryFollowTransitions."), *Val),
			EMCPErrorCode::InvalidArgument,
			TEXT("Closed set: \"TryEnterState\", \"TrySelectChildrenInOrder\", \"TryFollowTransitions\". Case-sensitive."));
	}

	// All enum validations passed — apply writes (numeric/string/bool fields
	// have no parse-failure path). Modify() AFTER validation so a validation
	// error doesn't leave the undo buffer with a spurious no-op record.
	State->Modify();
	if (Params->TryGetStringField(TEXT("name"), Val))
		State->Name = FName(*Val);

	if (bWantType)
		State->Type = ParsedType;

	if (bWantSelection)
		State->SelectionBehavior = ParsedSelection;

	bool bVal;
	if (Params->TryGetBoolField(TEXT("enabled"), bVal))
		State->bEnabled = bVal;

	double Weight;
	if (Params->TryGetNumberField(TEXT("weight"), Weight))
		State->Weight = static_cast<float>(Weight);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("state_id"), State->ID.ToString());
	return Result;
}

// ---- ListStates ----

TSharedPtr<FJsonObject> FStateTreeStateMgr::ListStates(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	TArray<TSharedPtr<FJsonValue>> StatesArray;

	TFunction<void(UStateTreeState*, int32)> WalkState;
	WalkState = [&](UStateTreeState* State, int32 Depth) {
		if (!State) return;

		TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
		StateObj->SetStringField(TEXT("id"), State->ID.ToString());
		StateObj->SetStringField(TEXT("name"), State->Name.ToString());
		StateObj->SetStringField(TEXT("type"), StaticEnum<EStateTreeStateType>()->GetNameStringByValue(static_cast<int64>(State->Type)));
		// `selection_behavior` / `weight` mirror the fields SetStateProperties
		// writes (see StateTreeStateMgr.cpp:474-490). Kept in sync with the
		// sibling serializer in MCPStateTreeCommands.cpp::HandleReadStateTree.
		StateObj->SetStringField(TEXT("selection_behavior"), StaticEnum<EStateTreeStateSelectionBehavior>()->GetNameStringByValue(static_cast<int64>(State->SelectionBehavior)));
		StateObj->SetNumberField(TEXT("depth"), Depth);
		StateObj->SetBoolField(TEXT("enabled"), State->bEnabled);
		StateObj->SetNumberField(TEXT("weight"), State->Weight);
		StateObj->SetNumberField(TEXT("child_count"), State->Children.Num());
		StateObj->SetNumberField(TEXT("task_count"), State->Tasks.Num());
		StateObj->SetNumberField(TEXT("transition_count"), State->Transitions.Num());

		if (UStateTreeState* ParentState = State->Parent.Get())
		{
			StateObj->SetStringField(TEXT("parent_id"), ParentState->ID.ToString());
		}

		StatesArray.Add(MakeShared<FJsonValueObject>(StateObj));

		for (UStateTreeState* Child : State->Children)
		{
			WalkState(Child, Depth + 1);
		}
	};

	for (UStateTreeState* Root : EditorData->SubTrees)
	{
		WalkState(Root, 0);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetField(TEXT("states"), MakeShared<FJsonValueArray>(StatesArray));
	Result->SetNumberField(TEXT("count"), StatesArray.Num());
	return Result;
}
