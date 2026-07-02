#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for Animation Blueprint MCP commands.
 * Covers state machine authoring, anim notify management,
 * montage creation, ABP setup, and skeletal control properties.
 */
class FMCPAnimationCommands
{
public:
	FMCPAnimationCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// ── State machine authoring (Phase 4) ─────────────────────────────
	TSharedPtr<FJsonObject> HandleCreateStateMachine(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleAddState(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleAddConduit(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleAddTransition(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetEntryState(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleModifyTransition(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRemoveState(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRemoveTransition(const TSharedPtr<FJsonObject>& Params);

	// ── Skeletal control properties (Phase 6) ─────────────────────────
	TSharedPtr<FJsonObject> HandleSetInnerNodeProperty(const TSharedPtr<FJsonObject>& Params);

	// ── AnimGraph property binding ────────────────────────────────────
	// Bind an AnimGraph node's input property (e.g. SequencePlayer's
	// `Sequence`) to a Blueprint variable.  Equivalent to clicking the
	// binding chip on the input pin in the Anim BP editor and selecting
	// a variable from the dropdown.  Writes the binding into the node's
	// UAnimGraphNodeBinding sub-object (FAnimGraphNodePropertyBinding map),
	// toggles the optional pin visible, and reconstructs the node so
	// the binding pill renders.  Required to make a SequencePlayer's
	// Sequence input track an AnimInstance variable like ArmedIdleAnim3P.
	TSharedPtr<FJsonObject> HandleBindAnimNodeProperty(const TSharedPtr<FJsonObject>& Params);

	// ── Anim notify management (Phase 7) ──────────────────────────────
	TSharedPtr<FJsonObject> HandleListAnimNotifies(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleAddAnimNotify(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRemoveAnimNotify(const TSharedPtr<FJsonObject>& Params);

	// ── Slice an animation between two notifies (or two times) ────────
	// Duplicates the source UAnimSequence, then crops the copy to the
	// [start, end] window using the engine's own crop primitive
	// (UE::Anim::AnimationData::Trim — the same path as Persona's
	// right-click Crop).  Trim preserves both the raw bone tracks AND the
	// additive transform curves, so any gizmo + SetKey ("S") edits baked
	// into the source ride along into the slice.  The two boundary tags are
	// stripped from the output; other in-window notifies are rebased to the
	// new t=0.
	TSharedPtr<FJsonObject> HandleExtractAnimBetweenNotifies(const TSharedPtr<FJsonObject>& Params);

	// ── Montage creation (Phase 8) ────────────────────────────────────
	TSharedPtr<FJsonObject> HandleCreateAnimMontage(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleAddMontageSection(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetMontageSectionLink(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetMontageBlend(const TSharedPtr<FJsonObject>& Params);

	// ── ABP creation (Phase 9) ────────────────────────────────────────
	TSharedPtr<FJsonObject> HandleCreateAnimBlueprint(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetAnimBlueprintSkeleton(const TSharedPtr<FJsonObject>& Params);
};
