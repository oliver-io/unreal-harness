// kinematic_probe — forward FK probe (dry-run + live-apply-and-restore).
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
	struct FRotationReq
	{
		int32   BoneIdx = INDEX_NONE;
		FString BoneName;
		FQuat   QIn = FQuat::Identity;
		FString Space = TEXT("component");
	};

	struct FProbePoint
	{
		KinematicsCmd::FProbeCapture Cap;
		USkeletalMeshComponent*      PointMesh = nullptr;
	};

	// One probe point's report block plus running best-score bookkeeping.
	void EmitPoint(
		const FProbePoint& Pt, const FTransform& WorldAfter, const FTransform& RelAfter,
		const FVector& Intent, const FVector& FwdLocal, bool bHasIntent,
		TArray<TSharedPtr<FJsonValue>>& Out, double& BestScore, FString& BestName)
	{
		using namespace KinematicsCmd;
		if (!Pt.Cap.bValid)
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Pt.Cap.Name);
			P->SetStringField(TEXT("component"), Pt.Cap.ComponentLabel.IsEmpty() ? TEXT("body") : Pt.Cap.ComponentLabel);
			E->SetObjectField(TEXT("point"), P);
			E->SetBoolField(TEXT("exists"), false);
			E->SetStringField(TEXT("error"), Pt.Cap.Error);
			Out.Add(MakeShared<FJsonValueObject>(E));
			return;
		}

		double Pos = 0.0, Fwd = 0.0;
		TSharedPtr<FJsonObject> O = BuildProbePointReport(Pt.Cap, WorldAfter, RelAfter, Intent, FwdLocal, Pos, Fwd);
		if (bHasIntent)
		{
			const double Combined = Pos * Fwd;
			O->SetNumberField(TEXT("score"), Combined);
			if (Combined > BestScore)
			{
				BestScore = Combined;
				BestName = Pt.Cap.Name;
			}
		}
		Out.Add(MakeShared<FJsonValueObject>(O));
	}
}

TSharedPtr<FJsonObject> FMCPKinematicsCommands::HandleProbe(const TSharedPtr<FJsonObject>& Params)
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

	// ── Parse rotations ──
	TArray<FRotationReq> Rots;
	const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("rotations"), RotArr) || RotArr->Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing/empty 'rotations'"), EMCPErrorCode::InvalidArgument,
			TEXT("`rotations` is a non-empty array of { bone, rotation, space? } applied in order. rotation = { axis:{x,y,z}, angle_deg } or { pitch, yaw, roll }; space = component (default) | world | bone_local."));
	}
	for (const TSharedPtr<FJsonValue>& RV : *RotArr)
	{
		const TSharedPtr<FJsonObject> R = RV.IsValid() ? RV->AsObject() : nullptr;
		if (!R.IsValid()) continue;

		FString BoneName;
		if (!R->TryGetStringField(TEXT("bone"), BoneName))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("rotation entry missing 'bone'"), EMCPErrorCode::InvalidArgument,
				TEXT("Each rotation needs a `bone` name on the resolved mesh."));
		}
		if (BoneName.Len() >= NAME_SIZE)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Bone name too long (%d chars; FName max is %d)"), BoneName.Len(), NAME_SIZE - 1),
				EMCPErrorCode::InvalidArgument);
		}
		const int32 Idx = Mesh->GetBoneIndex(FName(*BoneName));
		if (Idx == INDEX_NONE)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Bone '%s' not found on the resolved mesh"), *BoneName),
				EMCPErrorCode::InvalidArgument,
				TEXT("Use inspect_skeletal_mesh or kinematic_read_transform to discover bone names."));
		}

		const TSharedPtr<FJsonObject>* RotObjPtr = nullptr;
		R->TryGetObjectField(TEXT("rotation"), RotObjPtr);
		FQuat Q = FQuat::Identity;
		FString PErr;
		if (!ParseInputQuat(RotObjPtr ? *RotObjPtr : nullptr, Q, PErr))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("rotation parse failed for bone '%s': %s"), *BoneName, *PErr),
				EMCPErrorCode::InvalidArgument);
		}

		FRotationReq Req;
		Req.BoneIdx = Idx;
		Req.BoneName = BoneName;
		Req.QIn = Q;
		Req.Space = TEXT("component");
		R->TryGetStringField(TEXT("space"), Req.Space);
		Rots.Add(Req);
	}

	// ── Parse probe points (capture BEFORE state) ──
	TArray<FProbePoint> Points;
	const TArray<TSharedPtr<FJsonValue>>* PtArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("probe_points"), PtArr) || PtArr->Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing/empty 'probe_points'"), EMCPErrorCode::InvalidArgument,
			TEXT("`probe_points` is a non-empty array of { component?, socket|bone } — the end-effectors to measure (e.g. a sword's blade-tip socket)."));
	}
	for (const TSharedPtr<FJsonValue>& PV : *PtArr)
	{
		const TSharedPtr<FJsonObject> P = PV.IsValid() ? PV->AsObject() : nullptr;
		if (!P.IsValid()) continue;

		FString CompSel = MeshSel;
		P->TryGetStringField(TEXT("component"), CompSel);
		USkeletalMeshComponent* PMesh = (CompSel == MeshSel) ? Mesh : ResolveMesh(Actor, CompSel);
		if (!PMesh) PMesh = Mesh;

		FString Nm;
		if (!P->TryGetStringField(TEXT("socket"), Nm))
		{
			P->TryGetStringField(TEXT("bone"), Nm);
		}

		FProbePoint Pt;
		Pt.PointMesh = PMesh;
		Pt.Cap = CaptureProbe(Mesh, PMesh, Nm, CompSel.IsEmpty() ? TEXT("body") : CompSel);
		Points.Add(Pt);
	}

	// ── Intent / forward / mode ──
	const FVector Intent = ReadVec3Field(Params, TEXT("intent_direction"), FVector::ZeroVector);
	const FVector FwdLocal = ReadVec3Field(Params, TEXT("forward_axis_local"), FVector(1.f, 0.f, 0.f));
	const bool bHasIntent = !Intent.IsNearlyZero();

	FString Mode = TEXT("dryrun");
	Params->TryGetStringField(TEXT("mode"), Mode);
	const bool bLive = Mode.Equals(TEXT("live"), ESearchCase::IgnoreCase);
	bool bScreenshot = false;
	Params->TryGetBoolField(TEXT("screenshot"), bScreenshot);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("world_type"), WorldType);
	Result->SetBoolField(TEXT("pose_valid"), IsPoseValid(Mesh));
	Result->SetStringField(TEXT("mode"), bLive ? TEXT("live") : TEXT("dryrun"));

	TArray<TSharedPtr<FJsonValue>> PointOut;
	double BestScore = -1.0;
	FString BestName;
	const FQuat MeshQuatW = Mesh->GetComponentQuat();

	if (!bLive)
	{
		// ── DRY-RUN: rotate a copy, recompose probe points from invariant relatives ──
		TArray<FTransform> CopyCS = Mesh->GetComponentSpaceTransforms();
		for (const FRotationReq& Rq : Rots)
		{
			if (!CopyCS.IsValidIndex(Rq.BoneIdx)) continue;
			const FVector Pivot = CopyCS[Rq.BoneIdx].GetLocation();
			const FQuat BoneCSRot = CopyCS[Rq.BoneIdx].GetRotation();
			const FQuat Qcs = ToComponentSpaceDelta(Rq.QIn, Rq.Space, MeshQuatW, BoneCSRot);
			BoneIK::RotateSubChain(CopyCS, Qcs, Pivot, Rq.BoneIdx, RefSkel);
		}
		for (const FProbePoint& Pt : Points)
		{
			FTransform WorldAfter = Pt.Cap.WorldBefore;
			FTransform RelAfter = Pt.Cap.RelativeBefore;
			if (Pt.Cap.bValid)
			{
				WorldAfter = RecomposeWorld(Pt.Cap, Mesh, CopyCS);
				RelAfter = (Pt.PointMesh == Mesh)
					? WorldAfter.GetRelativeTransform(Mesh->GetComponentTransform()) // RTS_Component on the body mesh
					: Pt.Cap.RelativeBefore;                                          // invariant on the weapon's own mesh
			}
			EmitPoint(Pt, WorldAfter, RelAfter, Intent, FwdLocal, bHasIntent, PointOut, BestScore, BestName);
		}
	}
	else
	{
		// ── LIVE-APPLY-AND-RESTORE: mutate the real mesh atomically (one game-thread call) ──
		TArray<FTransform> SavedCS = Mesh->GetComponentSpaceTransforms();    // read buffer
		TArray<FTransform>& EditBuf = Mesh->GetEditableComponentSpaceTransforms();
		TArray<FTransform> SavedEdit = EditBuf;                              // double-buffer hazard — save both

		TSet<USkeletalMeshComponent*> AttachedToRefresh;
		for (const FProbePoint& Pt : Points)
		{
			if (Pt.PointMesh && Pt.PointMesh != Mesh) AttachedToRefresh.Add(Pt.PointMesh);
		}

		TArray<FTransform>& LiveCS = const_cast<TArray<FTransform>&>(Mesh->GetComponentSpaceTransforms());
		for (const FRotationReq& Rq : Rots)
		{
			if (!LiveCS.IsValidIndex(Rq.BoneIdx)) continue;
			const FVector Pivot = LiveCS[Rq.BoneIdx].GetLocation();
			const FQuat BoneCSRot = LiveCS[Rq.BoneIdx].GetRotation();
			const FQuat Qcs = ToComponentSpaceDelta(Rq.QIn, Rq.Space, MeshQuatW, BoneCSRot);
			BoneIK::RotateSubChain(LiveCS, Qcs, Pivot, Rq.BoneIdx, RefSkel);
		}
		// Shipped commit pattern (CrossbowActor_IK): mirror to edit buffer, mark dirty,
		// refresh the component (and attached weapon) so socket world queries reflect it.
		// NEVER RefreshBoneTransforms — it re-runs the anim graph and erases the override.
		if (EditBuf.GetData() != LiveCS.GetData()) EditBuf = LiveCS;
		Mesh->MarkRenderDynamicDataDirty();
		Mesh->UpdateComponentToWorld();
		for (USkeletalMeshComponent* W : AttachedToRefresh) { if (W) W->UpdateComponentToWorld(); }

		for (const FProbePoint& Pt : Points)
		{
			FTransform WorldAfter = Pt.Cap.WorldBefore;
			FTransform RelAfter = Pt.Cap.RelativeBefore;
			if (Pt.Cap.bValid && Pt.PointMesh)
			{
				const FName NmF(*Pt.Cap.Name);
				WorldAfter = Pt.PointMesh->GetSocketTransform(NmF, RTS_World);
				RelAfter = Pt.PointMesh->GetSocketTransform(NmF, RTS_Component);
			}
			EmitPoint(Pt, WorldAfter, RelAfter, Intent, FwdLocal, bHasIntent, PointOut, BestScore, BestName);
		}

		// Restore both buffers, refresh again — nothing survives the call.
		LiveCS = SavedCS;
		EditBuf = SavedEdit;
		Mesh->MarkRenderDynamicDataDirty();
		Mesh->UpdateComponentToWorld();
		for (USkeletalMeshComponent* W : AttachedToRefresh) { if (W) W->UpdateComponentToWorld(); }

		TSharedPtr<FJsonObject> Shot = MakeShared<FJsonObject>();
		Shot->SetBoolField(TEXT("captured"), false);
		if (bScreenshot)
		{
			Shot->SetStringField(TEXT("note"),
				TEXT("Candidate-pose screenshot is deferred: a synchronous game-thread call cannot render a frame between apply and restore. The live numbers equal dry-run (no anim eval runs mid-call), so use dry-run for the verdict. An async screenshot variant is a planned extension."));
		}
		Result->SetObjectField(TEXT("screenshot"), Shot);
	}

	Result->SetArrayField(TEXT("probe_points"), PointOut);

	if (bHasIntent)
	{
		TSharedPtr<FJsonObject> V = MakeShared<FJsonObject>();
		V->SetNumberField(TEXT("score"), BestScore < 0.0 ? 0.0 : BestScore);
		V->SetStringField(TEXT("best_point"), BestName);
		V->SetStringField(TEXT("note"),
			TEXT("score = max over probe points of (position-alignment × forward-alignment), each a cosine clamped to [0,1]. The raw angles per point are the source of truth; the score is a convenience summary."));
		Result->SetObjectField(TEXT("verdict"), V);
	}

	if (bLive && !bScreenshot)
	{
		Result->SetStringField(TEXT("hint"),
			TEXT("mode=live has no numeric advantage over dryrun (no anim eval runs between apply and restore within the single game-thread call, so the sampled pose is identical). Use dryrun unless you need the (currently deferred) candidate-pose screenshot."));
	}

	return Result;
}
