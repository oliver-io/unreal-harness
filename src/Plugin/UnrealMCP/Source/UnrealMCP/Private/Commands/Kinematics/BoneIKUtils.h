// VENDORED COPY — when a consuming game project ships this same IK math
// (this plugin was extracted alongside one; its copy lives in the game
// module's Core/ folder), the game's copy is canonical. Vendored verbatim
// (plus this note) so the plugin builds standalone with no host-project
// reach-in. Re-vendor by re-copying the canonical header over this one and
// restoring this comment block.
//
// The plugin uses ONLY the inline functions (RotateSubChain in
// MCPKinematicsCommands_Probe.cpp, SolveTwoBoneIK in
// MCPKinematicsCommands_Solve.cpp). Two declarations have their
// definitions in the game's BoneIKUtils.cpp and are NOT linkable from this
// plugin — do not call FArmChainIndices::ResolveOnce or ApplyForwardSpineLean
// here.
//
// Shared bone-chain IK utilities — pure math, no actor/component dependencies.
//
// FArmChainIndices: resolved-once cache of the six L+R arm-chain bone indices
//   (upperarm/lowerarm/hand) every held-object IK consumer needs.
// RotateSubChain: rotate a bone and all its skeleton descendants around a pivot.
// DistributeRotationAcrossChain: spread one rotation evenly over a bone sub-chain.
// SolveTwoBoneIK: law-of-cosines IK for a shoulder→elbow→hand chain (auto-pole or
//   explicit pole *direction*).
// SolveTwoBoneIKToPole: same solve, but the elbow bends toward an explicit pole
//   *position* (a world point) — used by the bow draw, sword slash, fishing rod.
//
// Used by: SpearActor_TickCombat.cpp, RidingComponent_IK.cpp, HandGripComponent.cpp,
//   BowActor_ArmIK.cpp, SlashMeleeCombat_Tick.cpp, FishingRodActor_IK.cpp

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "ReferenceSkeleton.h"

class USkeletalMeshComponent;

namespace BoneIK
{

/**
 * Resolved-once bone indices for the two two-bone arm chains
 * (shoulder→elbow→hand, left + right) that every held-object IK consumer
 * needs. Embed as a member and call ResolveOnce(Mesh) at the top of each
 * solve — it binds on the first call and is a no-op afterwards.
 */
struct FArmChainIndices
{
	int32 UpperArmL = INDEX_NONE;
	int32 LowerArmL = INDEX_NONE;
	int32 HandL     = INDEX_NONE;
	int32 UpperArmR = INDEX_NONE;
	int32 LowerArmR = INDEX_NONE;
	int32 HandR     = INDEX_NONE;
	bool  bResolved = false;

	void ResolveOnce(const USkeletalMeshComponent* Mesh);

	bool HasLeftChain() const
	{
		return UpperArmL != INDEX_NONE && LowerArmL != INDEX_NONE && HandL != INDEX_NONE;
	}
	bool HasRightChain() const
	{
		return UpperArmR != INDEX_NONE && LowerArmR != INDEX_NONE && HandR != INDEX_NONE;
	}
};

/**
 * Rotate a bone and all its descendants in component-space transforms around a pivot.
 * Modifies CSTransforms in-place.
 */
inline void RotateSubChain(
	TArray<FTransform>& CSTransforms,
	const FQuat& Rotation,
	const FVector& Pivot,
	int32 RootBoneIdx,
	const FReferenceSkeleton& RefSkel)
{
	TArray<int32> Stack;
	Stack.Push(RootBoneIdx);
	while (Stack.Num() > 0)
	{
		const int32 Idx = Stack.Pop(EAllowShrinking::No);
		FTransform& T = CSTransforms[Idx];
		T.SetLocation(Pivot + Rotation.RotateVector(T.GetLocation() - Pivot));
		T.SetRotation(Rotation * T.GetRotation());

		TArray<int32> Children;
		RefSkel.GetDirectChildBones(Idx, Children);
		Stack.Append(Children);
	}
}

/**
 * Distribute a single component-space rotation evenly across a bone sub-chain.
 *
 * Each resolvable bone in ChainIndices receives the fractional share
 * Slerp(Identity, TotalRotationCS, 1/N) applied via RotateSubChain, re-reading its
 * pivot each iteration (an earlier bone's rotation moves the ones below it on a
 * nested chain). Because (Q^(1/N))^N == Q, the full rotation still lands on the
 * chain tip regardless of N — spread smoothly across the joints instead of kinking
 * one. INDEX_NONE and out-of-range entries are skipped (and excluded from N), so
 * callers may pass a fixed-size index buffer with unresolved bones left as
 * INDEX_NONE. Modifies CSTransforms in-place.
 *
 * @return the number of bones actually rotated (N).
 */
inline int32 DistributeRotationAcrossChain(
	TArray<FTransform>& CSTransforms,
	const FReferenceSkeleton& RefSkel,
	const FQuat& TotalRotationCS,
	TConstArrayView<int32> ChainIndices)
{
	int32 N = 0;
	for (const int32 Idx : ChainIndices)
		if (Idx != INDEX_NONE && CSTransforms.IsValidIndex(Idx)) ++N;
	if (N == 0) return 0;

	const FQuat Q_Share = FQuat::Slerp(FQuat::Identity, TotalRotationCS,
		1.0f / static_cast<float>(N));
	for (const int32 Idx : ChainIndices)
	{
		if (Idx == INDEX_NONE || !CSTransforms.IsValidIndex(Idx)) continue;
		const FVector PivotCS = CSTransforms[Idx].GetLocation();  // re-read: prior bone moved this one
		RotateSubChain(CSTransforms, Q_Share, PivotCS, Idx, RefSkel);
	}
	return N;
}

/**
 * Translate a bone and all its descendants in component-space transforms.
 * Modifies CSTransforms in-place.  Used for hit rebound displacement.
 */
inline void TranslateSubChain(
	TArray<FTransform>& CSTransforms,
	const FVector& Offset,
	int32 RootBoneIdx,
	const FReferenceSkeleton& RefSkel)
{
	TArray<int32> Stack;
	Stack.Push(RootBoneIdx);
	while (Stack.Num() > 0)
	{
		const int32 Idx = Stack.Pop(EAllowShrinking::No);
		CSTransforms[Idx].AddToTranslation(Offset);

		TArray<int32> Children;
		RefSkel.GetDirectChildBones(Idx, Children);
		Stack.Append(Children);
	}
}

/**
 * Two-bone IK solver (law of cosines).
 *
 * Solves for the elbow position given shoulder, target, and bone lengths,
 * then applies rotations to the upper and lower arm chains via RotateSubChain.
 * All positions are in component space; TargetWorldPos is transformed internally.
 *
 * @param CSTransforms     Editable component-space bone transforms
 * @param RefSkel          Reference skeleton for child-bone traversal
 * @param MeshWorldTransform  Component-to-world transform for the skeletal mesh
 * @param UpperIdx         Bone index of the upper arm (shoulder)
 * @param LowerIdx         Bone index of the lower arm (elbow)
 * @param HandIdx          Bone index of the hand (end effector)
 * @param TargetWorldPos   Desired hand position in world space
 */
inline void SolveTwoBoneIK(
	TArray<FTransform>& CSTransforms,
	const FReferenceSkeleton& RefSkel,
	const FTransform& MeshWorldTransform,
	int32 UpperIdx, int32 LowerIdx, int32 HandIdx,
	const FVector& TargetWorldPos,
	// Optional explicit bend direction (WORLD). When non-zero, the elbow bends toward
	// this instead of being inferred from the current elbow pose — use it when the
	// caller knows the bend plane in a stable frame (e.g. object-relative) so the
	// solve doesn't leak the animated/world pose into the bend. Zero = infer (default).
	const FVector& ExplicitPoleDirWorld = FVector::ZeroVector)
{
	if (UpperIdx == INDEX_NONE || LowerIdx == INDEX_NONE || HandIdx == INDEX_NONE) return;
	if (CSTransforms.Num() == 0) return;

	const FVector TargetCS = MeshWorldTransform.InverseTransformPosition(TargetWorldPos);

	const FVector ShoulderCS = CSTransforms[UpperIdx].GetLocation();
	const FVector ElbowCS    = CSTransforms[LowerIdx].GetLocation();
	const FVector HandCS     = CSTransforms[HandIdx].GetLocation();

	const float UpperLen  = FVector::Dist(ShoulderCS, ElbowCS);
	const float LowerLen  = FVector::Dist(ElbowCS, HandCS);
	const float MaxReach  = UpperLen + LowerLen - 0.1f;
	float TargetDist      = FVector::Dist(ShoulderCS, TargetCS);

	FVector ClampedTarget = TargetCS;
	if (TargetDist > MaxReach)
	{
		ClampedTarget = ShoulderCS + (TargetCS - ShoulderCS).GetSafeNormal() * MaxReach;
		TargetDist = MaxReach;
	}

	const FVector ToTarget = (ClampedTarget - ShoulderCS).GetSafeNormal();

	// Angle at the shoulder: cos(A) = (u² + d² - l²) / (2·u·d)
	const float CosAngle = FMath::Clamp(
		(UpperLen * UpperLen + TargetDist * TargetDist - LowerLen * LowerLen)
		/ (2.0f * UpperLen * FMath::Max(TargetDist, 0.01f)),
		-1.0f, 1.0f);
	const float SinAngle = FMath::Sqrt(1.0f - CosAngle * CosAngle);

	// Pole vector (bend direction): either the caller-supplied explicit direction
	// (transformed to component space) or — inferred — the current elbow's offset
	// perpendicular to the shoulder→target line.
	FVector PoleDir;
	if (!ExplicitPoleDirWorld.IsNearlyZero())
	{
		PoleDir = MeshWorldTransform.InverseTransformVectorNoScale(ExplicitPoleDirWorld);
		PoleDir -= ToTarget * FVector::DotProduct(PoleDir, ToTarget);   // keep ⊥ to the reach
	}
	else
	{
		PoleDir = ElbowCS - ShoulderCS
			- ToTarget * FVector::DotProduct(ElbowCS - ShoulderCS, ToTarget);
	}
	if (PoleDir.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		PoleDir = FVector::CrossProduct(ToTarget, FVector::UpVector);
		if (PoleDir.SizeSquared() < KINDA_SMALL_NUMBER)
			PoleDir = FVector::CrossProduct(ToTarget, FVector::RightVector);
	}
	PoleDir.Normalize();

	const FVector NewElbowCS = ShoulderCS
		+ (ToTarget * CosAngle + PoleDir * SinAngle) * UpperLen;

	// Step 1: rotate upper arm so elbow reaches its new position.
	const FVector OldElbowDir = (ElbowCS - ShoulderCS).GetSafeNormal();
	const FVector NewElbowDir = (NewElbowCS - ShoulderCS).GetSafeNormal();

	if (!OldElbowDir.IsNearlyZero() && !NewElbowDir.IsNearlyZero())
	{
		const FQuat ShoulderRot = FQuat::FindBetweenNormals(OldElbowDir, NewElbowDir);
		RotateSubChain(CSTransforms, ShoulderRot, ShoulderCS, UpperIdx, RefSkel);
	}

	// Step 2: rotate lower arm so hand reaches the target.
	const FVector ElbowCS2   = CSTransforms[LowerIdx].GetLocation();
	const FVector HandCS2    = CSTransforms[HandIdx].GetLocation();
	const FVector OldLowerDir = (HandCS2 - ElbowCS2).GetSafeNormal();
	const FVector NewLowerDir = (ClampedTarget - ElbowCS2).GetSafeNormal();

	if (!OldLowerDir.IsNearlyZero() && !NewLowerDir.IsNearlyZero())
	{
		const FQuat ElbowRot = FQuat::FindBetweenNormals(OldLowerDir, NewLowerDir);
		RotateSubChain(CSTransforms, ElbowRot, ElbowCS2, LowerIdx, RefSkel);
	}
}

/**
 * Two-bone IK (law of cosines) where the elbow bends toward an explicit pole
 * *position* — a world-space point the elbow should point at — rather than
 * inheriting the animated elbow direction (SolveTwoBoneIK's auto-pole) or a pole
 * *direction* (its ExplicitPoleDirWorld overload). Pins the bend plane with the
 * component of (PoleWorldPos − shoulder) perpendicular to the reach, so the arm
 * can't fold the wrong way; use it when the caller has a stable world anchor the
 * elbow should track (bow draw, sword slash, fishing-rod hold). Degenerate (pole
 * colinear with the reach) → fall back to the animated elbow offset, then to an
 * arbitrary perpendicular. All positions component space; world inputs transformed
 * internally. Modifies CSTransforms in place.
 */
inline void SolveTwoBoneIKToPole(
	TArray<FTransform>& CSTransforms,
	const FReferenceSkeleton& RefSkel,
	const FTransform& MeshWorldTransform,
	int32 UpperIdx, int32 LowerIdx, int32 HandIdx,
	const FVector& TargetWorldPos, const FVector& PoleWorldPos)
{
	if (UpperIdx == INDEX_NONE || LowerIdx == INDEX_NONE || HandIdx == INDEX_NONE) return;
	if (CSTransforms.Num() == 0) return;

	const FVector TargetCS = MeshWorldTransform.InverseTransformPosition(TargetWorldPos);
	const FVector PoleCS   = MeshWorldTransform.InverseTransformPosition(PoleWorldPos);

	const FVector ShoulderCS = CSTransforms[UpperIdx].GetLocation();
	const FVector ElbowCS    = CSTransforms[LowerIdx].GetLocation();
	const FVector HandCS     = CSTransforms[HandIdx].GetLocation();

	const float UpperLen = FVector::Dist(ShoulderCS, ElbowCS);
	const float LowerLen = FVector::Dist(ElbowCS, HandCS);
	const float MaxReach = UpperLen + LowerLen - 0.1f;
	float TargetDist     = FVector::Dist(ShoulderCS, TargetCS);

	FVector ClampedTarget = TargetCS;
	if (TargetDist > MaxReach)
	{
		ClampedTarget = ShoulderCS + (TargetCS - ShoulderCS).GetSafeNormal() * MaxReach;
		TargetDist = MaxReach;
	}

	const FVector ToTarget = (ClampedTarget - ShoulderCS).GetSafeNormal();

	const float CosAngle = FMath::Clamp(
		(UpperLen * UpperLen + TargetDist * TargetDist - LowerLen * LowerLen)
		/ (2.0f * UpperLen * FMath::Max(TargetDist, 0.01f)), -1.0f, 1.0f);
	const float SinAngle = FMath::Sqrt(1.0f - CosAngle * CosAngle);

	// Bend direction = the part of (Pole - Shoulder) perpendicular to the
	// shoulder→target line. Degenerate (pole colinear) → fall back to the
	// animated elbow offset, then to an arbitrary perpendicular.
	FVector PoleDir = (PoleCS - ShoulderCS)
		- ToTarget * FVector::DotProduct(PoleCS - ShoulderCS, ToTarget);
	if (PoleDir.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		PoleDir = (ElbowCS - ShoulderCS)
			- ToTarget * FVector::DotProduct(ElbowCS - ShoulderCS, ToTarget);
		if (PoleDir.SizeSquared() < KINDA_SMALL_NUMBER)
			PoleDir = FVector::CrossProduct(ToTarget, FVector::UpVector);
	}
	PoleDir.Normalize();

	const FVector NewElbowCS = ShoulderCS
		+ (ToTarget * CosAngle + PoleDir * SinAngle) * UpperLen;

	// Step 1: rotate upper arm so the elbow reaches its new position.
	const FVector OldElbowDir = (ElbowCS - ShoulderCS).GetSafeNormal();
	const FVector NewElbowDir = (NewElbowCS - ShoulderCS).GetSafeNormal();
	if (!OldElbowDir.IsNearlyZero() && !NewElbowDir.IsNearlyZero())
	{
		const FQuat ShoulderRot = FQuat::FindBetweenNormals(OldElbowDir, NewElbowDir);
		RotateSubChain(CSTransforms, ShoulderRot, ShoulderCS, UpperIdx, RefSkel);
	}

	// Step 2: rotate lower arm so the hand reaches the target.
	const FVector ElbowCS2    = CSTransforms[LowerIdx].GetLocation();
	const FVector HandCS2     = CSTransforms[HandIdx].GetLocation();
	const FVector OldLowerDir = (HandCS2 - ElbowCS2).GetSafeNormal();
	const FVector NewLowerDir = (ClampedTarget - ElbowCS2).GetSafeNormal();
	if (!OldLowerDir.IsNearlyZero() && !NewLowerDir.IsNearlyZero())
	{
		const FQuat ElbowRot = FQuat::FindBetweenNormals(OldLowerDir, NewLowerDir);
		RotateSubChain(CSTransforms, ElbowRot, ElbowCS2, LowerIdx, RefSkel);
	}
}

/**
 * Forward-lean the upper spine when the view pitches steeply downward — the 1P
 * "look down" posture, so the body follows the look (and the camera can later be
 * pitched forward to see past it / off a ledge). Distributes the lean across
 * spine_01..spine_05 in mesh component space via a smoothstep ramp tuned by the
 * aim.LookLean* cvars. LookPitchDeg is the signed view pitch (UE convention:
 * negative = looking down). Returns the total applied lean (deg); 0 below the onset
 * angle or if no spine bones resolve. Defined (non-inline) in BoneIKUtils.cpp — it
 * owns the shared cvars so bow + crossbow lean from one tuning surface.
 */
float ApplyForwardSpineLean(
	TArray<FTransform>& CSTransforms,
	const FReferenceSkeleton& RefSkel,
	USkeletalMeshComponent* Mesh,
	float LookPitchDeg);

} // namespace BoneIK
