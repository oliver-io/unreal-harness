// kinematic_solve — inverse: solve a two-bone-IK rotation that aims a tip along a
// desired world direction, then optionally verify with the forward probe.
// See MCPKinematicsCommands.cpp for the file map.

#include "MCPKinematicsCommands.h"
#include "MCPKinematicsCommands_Internal.h"
#include "MCPCommonUtils.h"
#include "Kinematics/BoneIKUtils.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonValue.h"

namespace
{
	TSharedPtr<FJsonObject> AxisAngleJson(const FQuat& Q)
	{
		FVector Axis = FVector::UpVector;
		float Angle = 0.f;
		Q.ToAxisAndAngle(Axis, Angle);
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("axis"), KinematicsCmd::Vec3(Axis));
		O->SetNumberField(TEXT("angle_deg"), FMath::RadiansToDegrees(Angle));
		return O;
	}
}

TSharedPtr<FJsonObject> FMCPKinematicsCommands::HandleSolve(const TSharedPtr<FJsonObject>& Params)
{
	using namespace KinematicsCmd;

	TSharedPtr<FJsonObject> Error;
	FString WorldType;
	UWorld* World = GetTargetWorld(Error, WorldType);
	if (!World) return Error;

	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor"), ActorName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'actor'"), EMCPErrorCode::InvalidArgument,
			TEXT("`actor` is required (active-world label / short-name / FName)."));
	}

	AActor* Actor = FindActorByName(World, ActorName, Error);
	if (!Actor) return Error;

	FString MeshSel;
	Params->TryGetStringField(TEXT("mesh"), MeshSel);
	USkeletalMeshComponent* Mesh = ResolveMesh(Actor, MeshSel);
	if (!Mesh || !Mesh->GetSkeletalMeshAsset())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("No skeletal mesh (with asset) resolved for actor '%s' (mesh='%s')"), *ActorName, *MeshSel),
			EMCPErrorCode::UnsupportedClass,
			TEXT("`mesh` must resolve to a USkeletalMeshComponent that has a skeletal mesh asset."));
	}
	const FReferenceSkeleton& RefSkel = Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();

	// ── Chain ──
	const TSharedPtr<FJsonObject>* ChainObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("chain"), ChainObj) || !ChainObj)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'chain'"), EMCPErrorCode::InvalidArgument,
			TEXT("`chain` = { upper, lower, hand } bone names for the two-bone IK (e.g. upperarm_r / lowerarm_r / hand_r)."));
	}
	FString UpperName, LowerName, HandName;
	(*ChainObj)->TryGetStringField(TEXT("upper"), UpperName);
	(*ChainObj)->TryGetStringField(TEXT("lower"), LowerName);
	(*ChainObj)->TryGetStringField(TEXT("hand"), HandName);
	if (UpperName.Len() >= NAME_SIZE || LowerName.Len() >= NAME_SIZE || HandName.Len() >= NAME_SIZE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Chain bone name too long"), EMCPErrorCode::InvalidArgument,
			TEXT("`chain.upper/lower/hand` must each be shorter than 1024 characters (FName limit)."));
	}
	const int32 UpperIdx = Mesh->GetBoneIndex(FName(*UpperName));
	const int32 LowerIdx = Mesh->GetBoneIndex(FName(*LowerName));
	const int32 HandIdx = Mesh->GetBoneIndex(FName(*HandName));
	if (UpperIdx == INDEX_NONE || LowerIdx == INDEX_NONE || HandIdx == INDEX_NONE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("One or more chain bones not found"), EMCPErrorCode::InvalidArgument,
			TEXT("`chain.upper/lower/hand` must all be bones on the resolved mesh. Use kinematic_read_transform / inspect_skeletal_mesh to discover names."));
	}

	// ── Effector tip ──
	const TSharedPtr<FJsonObject>* EffObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("effector"), EffObj) || !EffObj)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'effector'"), EMCPErrorCode::InvalidArgument,
			TEXT("`effector` = { component?, socket|bone } — the tip to aim (e.g. a sword blade-tip socket)."));
	}
	FString EffComp = MeshSel;
	(*EffObj)->TryGetStringField(TEXT("component"), EffComp);
	USkeletalMeshComponent* EffMesh = (EffComp == MeshSel) ? Mesh : ResolveMesh(Actor, EffComp);
	if (!EffMesh) EffMesh = Mesh;
	FString EffName;
	if (!(*EffObj)->TryGetStringField(TEXT("socket"), EffName))
	{
		(*EffObj)->TryGetStringField(TEXT("bone"), EffName);
	}

	FProbeCapture Cap = CaptureProbe(Mesh, EffMesh, EffName, EffComp.IsEmpty() ? TEXT("body") : EffComp);
	if (!Cap.bValid)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Effector resolve failed: %s"), *Cap.Error),
			EMCPErrorCode::InvalidArgument,
			TEXT("`effector.socket`/`bone` must exist on the resolved component."));
	}

	// ── Desired direction (world) ──
	const FVector DesiredDir = ReadVec3Field(Params, TEXT("desired_direction"), FVector::ZeroVector);
	if (DesiredDir.IsNearlyZero())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing/zero 'desired_direction'"), EMCPErrorCode::InvalidArgument,
			TEXT("`desired_direction` is a non-zero world vector (e.g. {x:0,y:0,z:1} for straight up). Only world space is supported in the first cut."));
	}
	const FVector DesiredN = DesiredDir.GetSafeNormal();

	bool bVerify = false;
	Params->TryGetBoolField(TEXT("verify"), bVerify);

	// ── Build the hand world-target ──
	// SolveTwoBoneIK aims at a hand POSITION. Place the tip at (shoulder + dir·dist)
	// keeping the current hand→tip offset, and back out the hand position. This is a
	// best-effort producer — the forward verify reports how close it actually got.
	const FTransform ShoulderWorld = Mesh->GetBoneTransform(UpperIdx);
	const FTransform HandWorld = Mesh->GetBoneTransform(HandIdx);
	const FVector TipWorld = Cap.WorldBefore.GetLocation();

	const double TipDist = FVector::Dist(TipWorld, ShoulderWorld.GetLocation());
	const FVector DesiredTipPos = ShoulderWorld.GetLocation() + DesiredN * TipDist;
	const FVector HandFromTip = HandWorld.GetLocation() - TipWorld;  // rigid offset (approx — rotates with hand)
	const FVector HandTarget = DesiredTipPos + HandFromTip;

	// ── Solve on a copy (dry-run; no live mutation in the first cut) ──
	TArray<FTransform> CopyCS = Mesh->GetComponentSpaceTransforms();
	const FQuat PreUpper = CopyCS.IsValidIndex(UpperIdx) ? CopyCS[UpperIdx].GetRotation() : FQuat::Identity;
	const FQuat PreLower = CopyCS.IsValidIndex(LowerIdx) ? CopyCS[LowerIdx].GetRotation() : FQuat::Identity;

	// Reachability (BoneIKUtils clamps to UpperLen+LowerLen-0.1; surface it).
	bool bReachable = true;
	if (CopyCS.IsValidIndex(UpperIdx) && CopyCS.IsValidIndex(LowerIdx) && CopyCS.IsValidIndex(HandIdx))
	{
		const double UpperLen = FVector::Dist(CopyCS[UpperIdx].GetLocation(), CopyCS[LowerIdx].GetLocation());
		const double LowerLen = FVector::Dist(CopyCS[LowerIdx].GetLocation(), CopyCS[HandIdx].GetLocation());
		const double MaxReach = UpperLen + LowerLen - 0.1;
		const double TargetDist = FVector::Dist(HandTarget, ShoulderWorld.GetLocation());
		bReachable = TargetDist <= MaxReach;
	}

	BoneIK::SolveTwoBoneIK(CopyCS, RefSkel, Mesh->GetComponentTransform(), UpperIdx, LowerIdx, HandIdx, HandTarget);

	const FQuat PostUpper = CopyCS.IsValidIndex(UpperIdx) ? CopyCS[UpperIdx].GetRotation() : FQuat::Identity;
	const FQuat PostLower = CopyCS.IsValidIndex(LowerIdx) ? CopyCS[LowerIdx].GetRotation() : FQuat::Identity;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("world_type"), WorldType);
	Result->SetBoolField(TEXT("pose_valid"), IsPoseValid(Mesh));
	Result->SetBoolField(TEXT("solved"), true);
	Result->SetBoolField(TEXT("reachable"), bReachable);
	Result->SetObjectField(TEXT("hand_target_world"), Vec3(HandTarget));

	{
		TSharedPtr<FJsonObject> ChainOut = MakeShared<FJsonObject>();
		ChainOut->SetStringField(TEXT("upper"), UpperName);
		ChainOut->SetStringField(TEXT("lower"), LowerName);
		ChainOut->SetStringField(TEXT("hand"), HandName);
		Result->SetObjectField(TEXT("chain"), ChainOut);
	}

	{
		TArray<TSharedPtr<FJsonValue>> Rots;
		{
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("bone"), UpperName);
			R->SetObjectField(TEXT("rotation_component"), AxisAngleJson(PostUpper * PreUpper.Inverse()));
			Rots.Add(MakeShared<FJsonValueObject>(R));
		}
		{
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("bone"), LowerName);
			R->SetObjectField(TEXT("rotation_component"), AxisAngleJson(PostLower * PreLower.Inverse()));
			Rots.Add(MakeShared<FJsonValueObject>(R));
		}
		Result->SetArrayField(TEXT("resulting_rotations"), Rots);
	}

	if (bVerify)
	{
		const FVector FwdLocal = ReadVec3Field(Params, TEXT("forward_axis_local"), FVector(1.f, 0.f, 0.f));
		const FTransform WorldAfter = RecomposeWorld(Cap, Mesh, CopyCS);
		const FTransform RelAfter = (EffMesh == Mesh)
			? WorldAfter.GetRelativeTransform(Mesh->GetComponentTransform())
			: Cap.RelativeBefore;
		double Pos = 0.0, Fwd = 0.0;
		TSharedPtr<FJsonObject> Verify = BuildProbePointReport(Cap, WorldAfter, RelAfter, DesiredDir, FwdLocal, Pos, Fwd);
		Verify->SetNumberField(TEXT("score"), Pos * Fwd);
		Result->SetObjectField(TEXT("verification"), Verify);
	}

	Result->SetStringField(TEXT("note"),
		TEXT("Two-bone IK aims a hand POSITION, not a tip direction directly, so this is a best-effort producer — pass verify:true (or run kinematic_probe with the resulting_rotations) to confirm the achieved tip direction. resulting_rotations are component-space deltas. No live mutation in the first cut."));

	return Result;
}
