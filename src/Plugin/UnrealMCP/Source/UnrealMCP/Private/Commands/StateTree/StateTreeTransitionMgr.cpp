#include "StateTreeTransitionMgr.h"
#include "StateTreeTypeCache.h"
#include "StateTreeNodeMgr.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "MCPCommonUtils.h"

// Helper: resolve target string to FStateTreeStateLink
static bool ResolveTransitionTarget(UStateTreeEditorData* EditorData, const FString& TargetStr,
	FStateTreeTransition& Trans, FString& OutError)
{
	// Special keyword targets
	if (TargetStr == TEXT("Succeeded"))
	{
		Trans.State.LinkType = EStateTreeTransitionType::Succeeded;
		return true;
	}
	if (TargetStr == TEXT("Failed"))
	{
		Trans.State.LinkType = EStateTreeTransitionType::Failed;
		return true;
	}
	if (TargetStr == TEXT("NextSelectableState") || TargetStr == TEXT("NextState"))
	{
		Trans.State.LinkType = EStateTreeTransitionType::NextState;
		return true;
	}
	if (TargetStr == TEXT("None") || TargetStr == TEXT("NotSet"))
	{
		Trans.State.LinkType = EStateTreeTransitionType::None;
		return true;
	}

	// Resolve as state name or GUID
	TSharedPtr<FJsonObject> Error;
	UStateTreeState* TargetState = FStateTreeTypeCache::FindState(EditorData, TargetStr, Error);
	if (!TargetState)
	{
		OutError = FString::Printf(TEXT("Target state not found: '%s'"), *TargetStr);
		return false;
	}

	Trans.State.LinkType = EStateTreeTransitionType::GotoState;
	Trans.State.ID = TargetState->ID;
	Trans.State.Name = TargetState->Name;
	return true;
}

// Helper: parse trigger string to enum. Returns false on unrecognized input so
// the caller can surface the typo to the user. Pre-bundle, this returned a
// silent default of OnStateCompleted on unknown input — which silently clobbered
// the existing trigger on SetTransitionProperties and silently mis-classified a
// new transition on AddTransition.
static bool TryParseTrigger(const FString& TriggerStr, EStateTreeTransitionTrigger& Out)
{
	if (TriggerStr == TEXT("OnStateCompleted"))   { Out = EStateTreeTransitionTrigger::OnStateCompleted; return true; }
	if (TriggerStr == TEXT("OnStateSucceeded"))   { Out = EStateTreeTransitionTrigger::OnStateSucceeded; return true; }
	if (TriggerStr == TEXT("OnStateFailed"))      { Out = EStateTreeTransitionTrigger::OnStateFailed; return true; }
	if (TriggerStr == TEXT("OnTick"))             { Out = EStateTreeTransitionTrigger::OnTick; return true; }
	if (TriggerStr == TEXT("OnEvent"))            { Out = EStateTreeTransitionTrigger::OnEvent; return true; }
	return false;
}

// Helper: parse priority string. The full engine-defined value set on
// EStateTreeTransitionPriority is Low / Normal / Medium / High / Critical
// (the short canonical names returned by StaticEnum<...>->GetNameStringByValue,
// which is what HandleReadStateTree emits at MCPStateTreeCommands.cpp:333).
// Returns false on unrecognized input. Pre-bundle, this returned a silent
// default of Normal on unknown input, which collided with a *valid* explicit
// Normal in the round-trip (you couldn't tell the difference between
// `priority="Normal"` and `priority="Norml"`); both produced Normal silently.
// The five-branch coverage was added as part of the prior "ParsePriority
// accepts Low/Normal" bundle — the explicit branches stay; only the silent
// fallback is removed.
static bool TryParsePriority(const FString& PriorityStr, EStateTreeTransitionPriority& Out)
{
	if (PriorityStr == TEXT("Low"))      { Out = EStateTreeTransitionPriority::Low; return true; }
	if (PriorityStr == TEXT("Normal"))   { Out = EStateTreeTransitionPriority::Normal; return true; }
	if (PriorityStr == TEXT("Medium"))   { Out = EStateTreeTransitionPriority::Medium; return true; }
	if (PriorityStr == TEXT("High"))     { Out = EStateTreeTransitionPriority::High; return true; }
	if (PriorityStr == TEXT("Critical")) { Out = EStateTreeTransitionPriority::Critical; return true; }
	return false;
}

// Shared error-message bodies so AddTransition and SetTransitionProperties
// stay verbatim-aligned on the user-visible "valid values" list.
#define MCP_INVALID_TRIGGER_MSG TEXT("Invalid 'trigger' value '%s'. Valid: OnStateCompleted, OnStateSucceeded, OnStateFailed, OnTick, OnEvent.")
#define MCP_INVALID_PRIORITY_MSG TEXT("Invalid 'priority' value '%s'. Valid: Low, Normal, Medium, High, Critical.")
// Mirrors the macro of the same name in StateTreeNodeMgr.cpp — kept file-local
// (rather than promoted to MCPCommonUtils.h) per the same scope-discipline
// reasoning as the trigger/priority macros above: at one site in this TU and two
// sites in StateTreeNodeMgr.cpp, file-local-and-duplicated is cheaper than the
// header-coupling cost. See the #RESEARCH note in CHANGELOG.md on shared error
// macros for the promotion threshold.
#define MCP_INVALID_OPERAND_MSG TEXT("Invalid 'operand' value '%s'. Valid: And, Or.")

// ---- AddTransition ----

TSharedPtr<FJsonObject> FStateTreeTransitionMgr::AddTransition(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	// Required: source state
	FString StateId;
	if (!Params->TryGetStringField(TEXT("state"), StateId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'state' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `state` (string) — the name or GUID of the source state to attach the transition to. Use `statetree_state_list` to discover."));
	}

	UStateTreeState* State = FStateTreeTypeCache::FindState(EditorData, StateId, Error);
	if (!State) return Error;

	// Required: trigger
	FString TriggerStr;
	if (!Params->TryGetStringField(TEXT("trigger"), TriggerStr))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'trigger' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `trigger` (string). Valid: \"OnStateCompleted\", \"OnStateSucceeded\", \"OnStateFailed\", \"OnTick\", \"OnEvent\"."));
	}

	// Required: target
	FString TargetStr;
	if (!Params->TryGetStringField(TEXT("target"), TargetStr))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `target` (string). Valid keywords: \"Succeeded\", \"Failed\", \"NextSelectableState\", \"None\". Or a state name/GUID for explicit GotoState target."));
	}

	// Build transition
	FStateTreeTransition NewTrans;
	NewTrans.ID = FGuid::NewGuid();

	// Validate enum-typed fields up-front. NewTrans is a stack value not yet
	// attached to State->Transitions, so an early return on validation failure
	// leaves the asset untouched.
	if (!TryParseTrigger(TriggerStr, NewTrans.Trigger))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(MCP_INVALID_TRIGGER_MSG, *TriggerStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("Closed set: \"OnStateCompleted\", \"OnStateSucceeded\", \"OnStateFailed\", \"OnTick\", \"OnEvent\". Case-sensitive. Use the canonical short names emitted by `statetree_read`."));
	}

	// Optional: priority
	FString PriorityStr;
	bool bHasPriority = false;
	if (Params->TryGetStringField(TEXT("priority"), PriorityStr))
	{
		bHasPriority = true;
		if (!TryParsePriority(PriorityStr, NewTrans.Priority))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(MCP_INVALID_PRIORITY_MSG, *PriorityStr),
				EMCPErrorCode::InvalidArgument,
				TEXT("Closed set: \"Low\", \"Normal\", \"Medium\", \"High\", \"Critical\". Case-sensitive."));
		}
	}

	// Resolve target — done after enum validation so a typo'd `target` doesn't
	// mask a typo'd `trigger`/`priority` (and vice versa). ResolveTransitionTarget
	// writes Trans.State on success and is atomic under failure (#RESEARCH note).
	FString ResolveError;
	if (!ResolveTransitionTarget(EditorData, TargetStr, NewTrans, ResolveError))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			ResolveError,
			EMCPErrorCode::InvalidArgument,
			TEXT("`target` didn't resolve to a valid transition target. Valid keywords: \"Succeeded\", \"Failed\", \"NextSelectableState\" (alias \"NextState\"), \"None\" (alias \"NotSet\"). Otherwise pass a state name or GUID — verify with `statetree_state_list`."));
	}

	// Optional: event_tag → RequiredEvent.Tag. Tag-only FStateTreeEventDesc is
	// engine-blessed: the struct has a FGameplayTag-only ctor, and the deprecated
	// FStateTreeTransition::EventTag_DEPRECATED says "Use RequiredEvent.Tag
	// instead" (StateTreeState.h, UE 5.7). PayloadStruct / bConsumeEventOnSelect
	// keep their CDO defaults — surfacing them is the richer-schema design call
	// still tracked in #DEFERRED. The tag only takes effect when the trigger is
	// OnEvent. NewTrans is stack-local, so the invalid-tag early return leaves
	// the asset untouched.
	FString EventTagStr;
	if (Params->TryGetStringField(TEXT("event_tag"), EventTagStr) && !EventTagStr.IsEmpty())
	{
		const FGameplayTag EventTag = FGameplayTag::RequestGameplayTag(FName(*EventTagStr), /*ErrorIfNotFound*/false);
		if (!EventTag.IsValid())
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Unknown gameplay tag '%s' for 'event_tag'"), *EventTagStr),
				EMCPErrorCode::UnknownTag,
				TEXT("`event_tag` must name a registered gameplay tag (e.g. `Event.Combat.Alert`). Use `tag_list` to discover registered tags or `tag_add` to register a new one, then retry. The tag only takes effect when `trigger` is \"OnEvent\"."));
		}
		NewTrans.RequiredEvent.Tag = EventTag;
	}

	// Optional: delay — gate bDelayTransition on a strictly-positive value so
	// `delay: 0.0` stays a non-delayed transition (matches SetTransitionProperties
	// line ~336 below, and avoids a zero-duration "delayed" transition path).
	double Delay = 0;
	bool bHasDelay = false;
	if (Params->TryGetNumberField(TEXT("delay"), Delay))
	{
		bHasDelay = true;
		NewTrans.bDelayTransition = (Delay > 0);
		NewTrans.DelayDuration = static_cast<float>(Delay);
	}

	double DelayVariance = 0;
	bool bHasDelayVariance = false;
	if (Params->TryGetNumberField(TEXT("delay_variance"), DelayVariance))
	{
		bHasDelayVariance = true;
		NewTrans.DelayRandomVariance = static_cast<float>(DelayVariance);
	}

	// Process inline conditions. Validation is loud-error throughout — every
	// malformed entry fails the whole AddTransition call, matching the atomic
	// contract fixed in AddState (see StateTreeStateMgr.cpp ProcessInlineNodes
	// rollback). NewTrans is stack-local until State->Transitions.Add(NewTrans)
	// below, so any early return leaves the asset untouched. Pre-fix, this loop
	// used `continue` on malformed objects, missing `type`, unknown type, AND
	// silently discarded ApplyPropertyOverrides errors — callers saw
	// transition_id in the response and assumed success, when in reality the
	// conditions had been silently dropped.
	TArray<TSharedPtr<FJsonValue>> ConditionIds;
	const TArray<TSharedPtr<FJsonValue>>* CondsJson = nullptr;
	if (Params->TryGetArrayField(TEXT("conditions"), CondsJson))
	{
		for (int32 CondIdx = 0; CondIdx < CondsJson->Num(); ++CondIdx)
		{
			const TSharedPtr<FJsonValue>& CondVal = (*CondsJson)[CondIdx];
			const TSharedPtr<FJsonObject>* CondObj;
			if (!CondVal->TryGetObject(CondObj))
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("conditions[%d]: entry is not a JSON object"), CondIdx),
					EMCPErrorCode::InvalidArgument,
					TEXT("Each entry in `conditions` must be a JSON object with at minimum a `type` field. Strings, numbers, and arrays are rejected."));
			}

			FString TypeName;
			if (!(*CondObj)->TryGetStringField(TEXT("type"), TypeName))
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("conditions[%d]: missing 'type' field"), CondIdx),
					EMCPErrorCode::InvalidArgument,
					TEXT("Each `conditions` entry needs `type` (string) — a UScriptStruct short name like \"STCond_TargetVisible\". Use `statetree_list_node_types` to discover."));
			}

			UScriptStruct* CondStruct = FStateTreeTypeCache::Get().ResolveNodeType(TypeName);
			if (!CondStruct)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("conditions[%d]: unknown node type '%s'. Use list_state_tree_node_types to discover valid types."), CondIdx, *TypeName),
					EMCPErrorCode::ClassNotLoaded,
					TEXT("Use the canonical short name (no F prefix) from `statetree_list_node_types`, e.g. \"STCond_TargetVisible\"."));
			}

			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			TSharedPtr<FJsonObject> Props;
			if ((*CondObj)->TryGetObjectField(TEXT("properties"), PropsObj))
				Props = *PropsObj;

			FStateTreeEditorNode CondNode;
			CondNode.Node.InitializeAs(CondStruct);
			CondNode.ID = FGuid::NewGuid();

			const FStateTreeNodeBase* NodeBase = CondNode.Node.GetPtr<FStateTreeNodeBase>();
			if (NodeBase)
			{
				const UScriptStruct* InstanceType = Cast<UScriptStruct>(NodeBase->GetInstanceDataType());
				if (InstanceType)
				{
					CondNode.Instance.InitializeAs(const_cast<UScriptStruct*>(InstanceType));
					if (Props.IsValid())
					{
						FString ApplyError;
						if (!FStateTreeTypeCache::ApplyPropertyOverrides(CondNode.Instance, Props, ApplyError))
						{
							return FMCPCommonUtils::CreateErrorResponse(
								FString::Printf(TEXT("conditions[%d]: %s"), CondIdx, *ApplyError),
								EMCPErrorCode::InvalidArgument,
								TEXT("ApplyPropertyOverrides rejected the per-condition `properties` JSON. Common: type mismatch on a struct field, malformed nested JSON, unknown enum value."));
						}
					}
				}
			}

			// Operand is binary-only (And/Or); pre-bundle, an unrecognized string
			// silently collapsed to And. Validate up front so a typo on a single
			// inline condition fails the whole AddTransition call rather than
			// silently committing the wrong operand. NewTrans is stack-local until
			// State->Transitions.Add(NewTrans) below, so an early return here
			// leaves the asset untouched (no atomicity concern).
			FString Operand = TEXT("And");
			(*CondObj)->TryGetStringField(TEXT("operand"), Operand);
			if (Operand != TEXT("And") && Operand != TEXT("Or"))
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(MCP_INVALID_OPERAND_MSG, *Operand),
					EMCPErrorCode::InvalidArgument,
					TEXT("Per-condition `operand` must be exactly \"And\" or \"Or\" (case-sensitive). Selects how this condition combines with sibling conditions on the transition."));
			}
			CondNode.ExpressionOperand = (Operand == TEXT("Or")) ? EStateTreeExpressionOperand::Or : EStateTreeExpressionOperand::And;

			// Symmetric with the AddNode path at StateTreeNodeMgr.cpp:135-139 — inline
			// transition-condition specs accept the same `indent` field that the
			// canonical st_add_node(slot="condition") write path accepts.
			double CondIndent = 0;
			if ((*CondObj)->TryGetNumberField(TEXT("indent"), CondIndent))
			{
				CondNode.ExpressionIndent = static_cast<int32>(CondIndent);
			}

			NewTrans.Conditions.Add(CondNode);
			ConditionIds.Add(MakeShared<FJsonValueString>(CondNode.ID.ToString()));
		}
	}

	// dry_run: every preflight ran (state tree load, editor data, source state
	// lookup, trigger parse, target resolve, priority parse, every inline
	// condition's type lookup + property override). NewTrans is stack-local —
	// nothing has been attached to State->Transitions yet. The remaining work
	// (State->Modify + Transitions.Add) is the side effect we skip. Diff shape
	// per todo/13 phase 5: transitions_added[]. We intentionally do NOT include
	// NewTrans.ID in the diff — a fresh GUID is generated on the actual commit
	// call, so emitting the dry-run GUID would mislead callers into thinking it's
	// stable across the dry-run / commit pair (same reasoning as
	// add_material_expression's pre-create id challenge).
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		auto LinkTypeToString = [](EStateTreeTransitionType T)
		{
			switch (T)
			{
			case EStateTreeTransitionType::None:        return TEXT("None");
			case EStateTreeTransitionType::Succeeded:   return TEXT("Succeeded");
			case EStateTreeTransitionType::Failed:      return TEXT("Failed");
			case EStateTreeTransitionType::GotoState:   return TEXT("GotoState");
			case EStateTreeTransitionType::NextState:   return TEXT("NextState");
			default:                                    return TEXT("Unknown");
			}
		};

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("source_state_id"),
			State->ID.ToString(EGuidFormats::DigitsWithHyphens));
		Entry->SetStringField(TEXT("source_state_name"), State->Name.ToString());
		Entry->SetStringField(TEXT("trigger"), TriggerStr);

		TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("link_type"), LinkTypeToString(NewTrans.State.LinkType));
		if (NewTrans.State.LinkType == EStateTreeTransitionType::GotoState)
		{
			TargetObj->SetStringField(TEXT("id"),
				NewTrans.State.ID.ToString(EGuidFormats::DigitsWithHyphens));
			TargetObj->SetStringField(TEXT("name"), NewTrans.State.Name.ToString());
		}
		Entry->SetObjectField(TEXT("target"), TargetObj);

		if (bHasPriority)
		{
			Entry->SetStringField(TEXT("priority"), PriorityStr);
		}
		if (bHasDelay)
		{
			Entry->SetNumberField(TEXT("delay"), Delay);
		}
		if (bHasDelayVariance)
		{
			Entry->SetNumberField(TEXT("delay_variance"), DelayVariance);
		}

		// Per-condition resolved types — confirms each spec parsed cleanly. We
		// don't emit IDs here for the same fresh-GUID reason as the transition_id
		// itself.
		if (NewTrans.Conditions.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> CondsArr;
			for (const FStateTreeEditorNode& CondNode : NewTrans.Conditions)
			{
				const UScriptStruct* CondScript = CondNode.Node.GetScriptStruct();
				TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("type"),
					CondScript ? CondScript->GetName() : TEXT("Unknown"));
				CondsArr.Add(MakeShared<FJsonValueObject>(C));
			}
			Entry->SetArrayField(TEXT("conditions"), CondsArr);
		}

		TArray<TSharedPtr<FJsonValue>> TransArr;
		TransArr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("transitions_added"), TransArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Add to state — Modify() the state so the package is marked dirty. Pre-fix
	// transition adds left no package-level dirty flag; mirrors FStateTreeViewModel.
	State->Modify();
	State->Transitions.Add(NewTrans);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("transition_id"), NewTrans.ID.ToString());
	if (ConditionIds.Num() > 0)
	{
		Result->SetField(TEXT("condition_ids"), MakeShared<FJsonValueArray>(ConditionIds));
	}
	return Result;
}

// ---- RemoveTransition ----

TSharedPtr<FJsonObject> FStateTreeTransitionMgr::RemoveTransition(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString TransitionId;
	if (!Params->TryGetStringField(TEXT("transition_id"), TransitionId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'transition_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `transition_id` (string GUID) — the FGuid of the transition to remove or modify. Use `statetree_read` (with `include_bindings=true`) to discover transition GUIDs."));
	}

	FGuid TransGuid;
	if (!FGuid::Parse(TransitionId, TransGuid))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid transition_id GUID format"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`transition_id` must be a parseable FGuid string (e.g. \"12345678-1234-1234-1234-123456789012\"). Use the exact value from `statetree_read`."));
	}

	// Refactored to a side-effect-free lookup so dry-run can preview the removal
	// without executing it. The mutation (State->Modify + Transitions.RemoveAt)
	// is deferred until after the dry-run check.
	UStateTreeState* OwnerState = nullptr;
	int32 OwnerIndex = INDEX_NONE;
	TFunction<void(UStateTreeState*)> SearchOnly;
	SearchOnly = [&](UStateTreeState* State) {
		if (OwnerState) return;
		if (!State) return;
		for (int32 i = State->Transitions.Num() - 1; i >= 0; --i)
		{
			if (State->Transitions[i].ID == TransGuid)
			{
				OwnerState = State;
				OwnerIndex = i;
				return;
			}
		}
		for (UStateTreeState* Child : State->Children)
			SearchOnly(Child);
	};

	for (UStateTreeState* Root : EditorData->SubTrees)
		SearchOnly(Root);

	if (!OwnerState)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Transition not found: %s"), *TransitionId),
			EMCPErrorCode::NodeNotFound,
			TEXT("No transition with that GUID exists in the StateTree. Use `statetree_read` (with `include_bindings=true`) to discover transition GUIDs."));
	}

	const FStateTreeTransition& Trans = OwnerState->Transitions[OwnerIndex];

	// dry_run: every preflight ran (state tree load, editor data, GUID parse,
	// transition lookup). The remaining work — State->Modify + Transitions.RemoveAt
	// — is the side effect we skip. Diff shape per todo/13 phase 5:
	// transitions_removed[]. Mirrors transitions_added[] (same field names for
	// trigger/target/priority/delay/conditions) so add/remove diffs round-trip
	// at the schema level. Unlike st_add_transition, we DO emit transition_id
	// here because it's an existing GUID — stable and known.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		auto LinkTypeToString = [](EStateTreeTransitionType T)
		{
			switch (T)
			{
			case EStateTreeTransitionType::None:        return TEXT("None");
			case EStateTreeTransitionType::Succeeded:   return TEXT("Succeeded");
			case EStateTreeTransitionType::Failed:      return TEXT("Failed");
			case EStateTreeTransitionType::GotoState:   return TEXT("GotoState");
			case EStateTreeTransitionType::NextState:   return TEXT("NextState");
			default:                                    return TEXT("Unknown");
			}
		};
		auto TriggerToString = [](EStateTreeTransitionTrigger T)
		{
			switch (T)
			{
			case EStateTreeTransitionTrigger::OnStateCompleted:  return TEXT("OnStateCompleted");
			case EStateTreeTransitionTrigger::OnStateSucceeded:  return TEXT("OnStateSucceeded");
			case EStateTreeTransitionTrigger::OnStateFailed:     return TEXT("OnStateFailed");
			case EStateTreeTransitionTrigger::OnTick:            return TEXT("OnTick");
			case EStateTreeTransitionTrigger::OnEvent:           return TEXT("OnEvent");
			default:                                              return TEXT("Unknown");
			}
		};
		auto PriorityToString = [](EStateTreeTransitionPriority P)
		{
			switch (P)
			{
			case EStateTreeTransitionPriority::Low:       return TEXT("Low");
			case EStateTreeTransitionPriority::Normal:    return TEXT("Normal");
			case EStateTreeTransitionPriority::Medium:    return TEXT("Medium");
			case EStateTreeTransitionPriority::High:      return TEXT("High");
			case EStateTreeTransitionPriority::Critical:  return TEXT("Critical");
			default:                                       return TEXT("Unknown");
			}
		};

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("transition_id"),
			Trans.ID.ToString(EGuidFormats::DigitsWithHyphens));
		Entry->SetStringField(TEXT("from_state_id"),
			OwnerState->ID.ToString(EGuidFormats::DigitsWithHyphens));
		Entry->SetStringField(TEXT("from_state_name"), OwnerState->Name.ToString());
		Entry->SetStringField(TEXT("trigger"), TriggerToString(Trans.Trigger));

		TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("link_type"), LinkTypeToString(Trans.State.LinkType));
		if (Trans.State.LinkType == EStateTreeTransitionType::GotoState)
		{
			TargetObj->SetStringField(TEXT("id"),
				Trans.State.ID.ToString(EGuidFormats::DigitsWithHyphens));
			TargetObj->SetStringField(TEXT("name"), Trans.State.Name.ToString());
		}
		Entry->SetObjectField(TEXT("target"), TargetObj);

		// Priority is always set on a real transition (struct default Normal),
		// so emit it unconditionally — there's no "absent" option to mirror
		// AddTransition's bHasPriority gating, and the user reading a removal
		// preview wants the real value, not a default-by-omission inference.
		Entry->SetStringField(TEXT("priority"), PriorityToString(Trans.Priority));

		// Delay: bDelayTransition is the loud signal a delay is configured
		// (matches the AddTransition gate at line ~176 above). Emit only when
		// truly delayed.
		if (Trans.bDelayTransition)
		{
			Entry->SetNumberField(TEXT("delay"), Trans.DelayDuration);
		}
		if (Trans.DelayRandomVariance != 0.0f)
		{
			Entry->SetNumberField(TEXT("delay_variance"), Trans.DelayRandomVariance);
		}

		if (Trans.Conditions.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> CondsArr;
			for (const FStateTreeEditorNode& CondNode : Trans.Conditions)
			{
				const UScriptStruct* CondScript = CondNode.Node.GetScriptStruct();
				TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("type"),
					CondScript ? CondScript->GetName() : TEXT("Unknown"));
				CondsArr.Add(MakeShared<FJsonValueObject>(C));
			}
			Entry->SetArrayField(TEXT("conditions"), CondsArr);
		}

		TArray<TSharedPtr<FJsonValue>> RemovedArr;
		RemovedArr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("transitions_removed"), RemovedArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Capture the owning-state name for the legacy `removed_from` response field
	// before the mutation runs.
	const FString Location = FString::Printf(
		TEXT("transition on state '%s'"), *OwnerState->Name.ToString());

	OwnerState->Modify();
	OwnerState->Transitions.RemoveAt(OwnerIndex);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("removed_from"), Location);
	return Result;
}

// ---- SetTransitionProperties ----

TSharedPtr<FJsonObject> FStateTreeTransitionMgr::SetTransitionProperties(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString TransitionId;
	if (!Params->TryGetStringField(TEXT("transition_id"), TransitionId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'transition_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `transition_id` (string GUID) — the FGuid of the transition to remove or modify. Use `statetree_read` (with `include_bindings=true`) to discover transition GUIDs."));
	}

	FGuid TransGuid;
	if (!FGuid::Parse(TransitionId, TransGuid))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid transition_id GUID format"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`transition_id` must be a parseable FGuid string (e.g. \"12345678-1234-1234-1234-123456789012\"). Use the exact value from `statetree_read`."));
	}

	// Find the transition AND its owning state — we need the owner to call
	// Modify() on it before mutating the transition's fields. The transition is
	// a struct field on UStateTreeState; the state is the transactional scope.
	FStateTreeTransition* FoundTrans = nullptr;
	UStateTreeState* OwningState = nullptr;
	TFunction<void(UStateTreeState*)> Search;
	Search = [&](UStateTreeState* State) {
		if (FoundTrans) return;
		if (!State) return;
		for (FStateTreeTransition& Trans : State->Transitions)
		{
			if (Trans.ID == TransGuid)
			{
				FoundTrans = &Trans;
				OwningState = State;
				return;
			}
		}
		for (UStateTreeState* Child : State->Children)
			Search(Child);
	};

	for (UStateTreeState* Root : EditorData->SubTrees)
		Search(Root);

	if (!FoundTrans)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Transition not found: %s"), *TransitionId),
			EMCPErrorCode::NodeNotFound,
			TEXT("No transition with that GUID exists in the StateTree. Use `statetree_read` (with `include_bindings=true`) to discover transition GUIDs."));
	}

	// Validate-before-mutate: parse all enum-typed fields into locals first; if
	// any fails, return an error before any field has been written. Pre-bundle,
	// `target` was the only validated field — `trigger` and `priority` silently
	// fell through to default values (OnStateCompleted / Normal) on unrecognized
	// strings, clobbering the existing field. With Try*Parse* gated, all three
	// validations succeed-or-fail together before the first write.
	FString Val;

	bool bWantTrigger = false;
	EStateTreeTransitionTrigger ParsedTrigger = FoundTrans->Trigger;
	if (Params->TryGetStringField(TEXT("trigger"), Val))
	{
		bWantTrigger = true;
		if (!TryParseTrigger(Val, ParsedTrigger))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(MCP_INVALID_TRIGGER_MSG, *Val),
				EMCPErrorCode::InvalidArgument,
				TEXT("Closed set: \"OnStateCompleted\", \"OnStateSucceeded\", \"OnStateFailed\", \"OnTick\", \"OnEvent\". Case-sensitive."));
		}
	}

	bool bWantPriority = false;
	EStateTreeTransitionPriority ParsedPriority = FoundTrans->Priority;
	if (Params->TryGetStringField(TEXT("priority"), Val))
	{
		bWantPriority = true;
		if (!TryParsePriority(Val, ParsedPriority))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(MCP_INVALID_PRIORITY_MSG, *Val),
				EMCPErrorCode::InvalidArgument,
				TEXT("Closed set: \"Low\", \"Normal\", \"Medium\", \"High\", \"Critical\". Case-sensitive."));
		}
	}

	// event_tag → RequiredEvent.Tag (tag-only FStateTreeEventDesc — see the
	// matching AddTransition parse). Validated here, applied below with the
	// other pre-validated writes. An explicit empty string clears the tag.
	bool bWantEventTag = false;
	FGameplayTag ParsedEventTag;
	if (Params->TryGetStringField(TEXT("event_tag"), Val))
	{
		bWantEventTag = true;
		if (!Val.IsEmpty())
		{
			ParsedEventTag = FGameplayTag::RequestGameplayTag(FName(*Val), /*ErrorIfNotFound*/false);
			if (!ParsedEventTag.IsValid())
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Unknown gameplay tag '%s' for 'event_tag'"), *Val),
					EMCPErrorCode::UnknownTag,
					TEXT("`event_tag` must name a registered gameplay tag (use `tag_list` / `tag_add`), or be the empty string to clear the transition's required event tag. The tag only takes effect when `trigger` is \"OnEvent\"."));
			}
		}
		// Val empty: ParsedEventTag stays EmptyTag — the clear case.
	}

	// All validations passed — mark the state dirty BEFORE the writes. Modify()
	// after validate-but-before-mutate keeps the undo buffer clean (validation
	// failure → no Modify call, no stale undo record) and marks the package
	// dirty so save/compile sees the change.
	if (OwningState) OwningState->Modify();

	// `target` resolution writes Trans.State directly. ResolveTransitionTarget
	// is atomic under failure (does not write Trans.State on its single failure
	// path — see #RESEARCH note in CHANGELOG.md), so applying it after the
	// in-local enum validation keeps the whole setter validate-before-mutate.
	if (Params->TryGetStringField(TEXT("target"), Val))
	{
		FString ResolveError;
		if (!ResolveTransitionTarget(EditorData, Val, *FoundTrans, ResolveError))
		{
			return FMCPCommonUtils::CreateErrorResponse(
			ResolveError,
			EMCPErrorCode::InvalidArgument,
			TEXT("`target` didn't resolve to a valid transition target. Valid keywords: \"Succeeded\", \"Failed\", \"NextSelectableState\" (alias \"NextState\"), \"None\" (alias \"NotSet\"). Otherwise pass a state name or GUID — verify with `statetree_state_list`."));
		}
	}

	// All validations passed — apply pre-validated enum writes.
	if (bWantTrigger)  FoundTrans->Trigger = ParsedTrigger;
	if (bWantPriority) FoundTrans->Priority = ParsedPriority;
	if (bWantEventTag) FoundTrans->RequiredEvent.Tag = ParsedEventTag;

	double NumVal;
	if (Params->TryGetNumberField(TEXT("delay"), NumVal))
	{
		FoundTrans->bDelayTransition = (NumVal > 0);
		FoundTrans->DelayDuration = static_cast<float>(NumVal);
	}
	if (Params->TryGetNumberField(TEXT("delay_variance"), NumVal))
	{
		FoundTrans->DelayRandomVariance = static_cast<float>(NumVal);
	}

	bool bEnabled;
	if (Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		FoundTrans->bTransitionEnabled = bEnabled;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("transition_id"), TransitionId);
	return Result;
}
