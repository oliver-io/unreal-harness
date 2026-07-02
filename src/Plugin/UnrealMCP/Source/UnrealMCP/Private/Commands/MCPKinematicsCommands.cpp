// FMCPKinematicsCommands — runtime component-space transform tools.
//
// Implementation split across 3 .cpp:
//   MCPKinematicsCommands.cpp        — shared helpers (world/actor/mesh
//                                                resolution, rotation parse + CS
//                                                conjugation, probe-point capture +
//                                                FK recomposition, per-point report),
//                                                HandleCommand dispatch, and
//                                                kinematic_read_transform.
//   MCPKinematicsCommands_Probe.cpp   — kinematic_probe (dry-run + live).
//   MCPKinematicsCommands_Solve.cpp   — kinematic_solve (inverse, BoneIK).
//
// Shared declarations: MCPKinematicsCommands_Internal.h.
// Design + invariants: docs/mcp/POSITION_PROBE_TOOLS.md.

#include "MCPKinematicsCommands.h"
#include "MCPKinematicsCommands_Internal.h"
#include "MCPCommonUtils.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMeshSocket.h"
#include "Dom/JsonValue.h"

// ─────────────────────────────────────────────────────────────────────────────
// Shared helpers (KinematicsCmd namespace)
// ─────────────────────────────────────────────────────────────────────────────

UWorld* KinematicsCmd::GetTargetWorld(TSharedPtr<FJsonObject>& OutError, FString& OutWorldType)
{
	if (!GEditor)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			TEXT("GEditor not available"),
			EMCPErrorCode::Internal,
			TEXT("GEditor is null — the editor is shutting down or has not finished initializing. Retry once the editor is fully loaded."));
		return nullptr;
	}

	// Prefer a running PIE world — the possessed player and its live animated pose
	// (aim, gripped weapon) exist there, which is what the consultant usually wants
	// to measure. Fall back to the editor world for posing placed actors.
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
			{
				OutWorldType = TEXT("pie");
				return Ctx.World();
			}
		}
	}

	if (UWorld* Editor = GEditor->GetEditorWorldContext().World())
	{
		OutWorldType = TEXT("editor");
		return Editor;
	}

	OutError = FMCPCommonUtils::CreateErrorResponse(
		TEXT("No PIE or editor world available"),
		EMCPErrorCode::Internal,
		TEXT("No running PIE world and GetEditorWorldContext().World() is null. Open a level (and optionally Play-In-Editor) and retry."));
	return nullptr;
}

AActor* KinematicsCmd::FindActorByName(UWorld* World, const FString& Name, TSharedPtr<FJsonObject>& OutError)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel() == Name || It->GetName() == Name || It->GetFName().ToString() == Name)
		{
			return *It;
		}
	}

	OutError = FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Actor not found: '%s'"), *Name),
		EMCPErrorCode::ActorNotFound,
		TEXT("No actor matches the given name (actor-label / short-name / FName; case-sensitive) in the active world. These tools resolve the running PIE world if Play-In-Editor is active, otherwise the editor world. Use get_actors_in_level / find_actors_by_name to discover names, and note PIE actor names differ from editor-world ones."));
	return nullptr;
}

USkeletalMeshComponent* KinematicsCmd::ResolveMesh(AActor* Actor, const FString& Selector)
{
	if (!Actor)
	{
		return nullptr;
	}

	auto FirstSkelMesh = [](AActor* A) -> USkeletalMeshComponent*
	{
		TArray<USkeletalMeshComponent*> Comps;
		A->GetComponents<USkeletalMeshComponent>(Comps);
		return Comps.Num() > 0 ? Comps[0] : nullptr;
	};

	if (Selector.IsEmpty() || Selector.Equals(TEXT("body"), ESearchCase::IgnoreCase))
	{
		if (ACharacter* Char = Cast<ACharacter>(Actor))
		{
			if (USkeletalMeshComponent* M = Char->GetMesh())
			{
				return M;
			}
		}
		return FirstSkelMesh(Actor);
	}

	// Named USkeletalMeshComponent on the actor.
	{
		TArray<USkeletalMeshComponent*> Comps;
		Actor->GetComponents<USkeletalMeshComponent>(Comps);
		for (USkeletalMeshComponent* C : Comps)
		{
			if (C && (C->GetName() == Selector || C->GetFName().ToString() == Selector))
			{
				return C;
			}
		}
	}

	// Attached child actor by name (e.g. an equipped weapon actor).
	{
		TArray<AActor*> Attached;
		Actor->GetAttachedActors(Attached);
		for (AActor* A : Attached)
		{
			if (A && (A->GetActorLabel() == Selector || A->GetName() == Selector || A->GetFName().ToString() == Selector))
			{
				return FirstSkelMesh(A);
			}
		}
	}

	return nullptr;
}

bool KinematicsCmd::IsPoseValid(USkeletalMeshComponent* Mesh)
{
	return Mesh && Mesh->GetSkeletalMeshAsset() && Mesh->GetComponentSpaceTransforms().Num() > 0;
}

bool KinematicsCmd::ParseInputQuat(const TSharedPtr<FJsonObject>& RotObj, FQuat& OutQ, FString& OutErr)
{
	if (!RotObj.IsValid())
	{
		OutErr = TEXT("rotation object missing");
		return false;
	}

	auto Num = [](const TSharedPtr<FJsonObject>& O, const TCHAR* F) -> double
	{
		double V = 0.0;
		O->TryGetNumberField(F, V);
		return V;
	};

	const TSharedPtr<FJsonObject>* AxisObj = nullptr;
	double AngleDeg = 0.0;
	if (RotObj->TryGetObjectField(TEXT("axis"), AxisObj) && AxisObj && RotObj->TryGetNumberField(TEXT("angle_deg"), AngleDeg))
	{
		FVector Axis(Num(*AxisObj, TEXT("x")), Num(*AxisObj, TEXT("y")), Num(*AxisObj, TEXT("z")));
		if (!Axis.Normalize())
		{
			OutErr = TEXT("rotation axis is zero-length");
			return false;
		}
		OutQ = FQuat(Axis, FMath::DegreesToRadians((float)AngleDeg));
		return true;
	}

	double P = 0, Y = 0, R = 0;
	if (RotObj->TryGetNumberField(TEXT("pitch"), P) &&
		RotObj->TryGetNumberField(TEXT("yaw"), Y) &&
		RotObj->TryGetNumberField(TEXT("roll"), R))
	{
		OutQ = FRotator((float)P, (float)Y, (float)R).Quaternion();
		return true;
	}

	OutErr = TEXT("rotation must be { axis:{x,y,z}, angle_deg } or { pitch, yaw, roll }");
	return false;
}

FQuat KinematicsCmd::ToComponentSpaceDelta(const FQuat& QIn, const FString& Space, const FQuat& MeshQuatW, const FQuat& BoneCSRot)
{
	if (Space.Equals(TEXT("world"), ESearchCase::IgnoreCase))
	{
		return MeshQuatW.Inverse() * QIn * MeshQuatW;
	}
	if (Space.Equals(TEXT("bone_local"), ESearchCase::IgnoreCase))
	{
		return BoneCSRot * QIn * BoneCSRot.Inverse();
	}
	return QIn; // "component" (default)
}

KinematicsCmd::FProbeCapture KinematicsCmd::CaptureProbe(
	USkeletalMeshComponent* RotatedMesh, USkeletalMeshComponent* PointMesh,
	const FString& Name, const FString& ComponentLabel)
{
	FProbeCapture Cap;
	Cap.ComponentLabel = ComponentLabel;
	Cap.Name = Name;

	if (!RotatedMesh || !PointMesh)
	{
		Cap.Error = TEXT("mesh not resolved");
		return Cap;
	}

	if (Name.Len() > NAME_SIZE)
	{
		Cap.Error = FString::Printf(TEXT("'%s' name is too long (%d chars) to be an FName"), *Name, Name.Len());
		return Cap;
	}
	const FName NameF(*Name);
	Cap.bExists = PointMesh->DoesSocketExist(NameF) || PointMesh->GetBoneIndex(NameF) != INDEX_NONE;
	if (!Cap.bExists)
	{
		Cap.Error = FString::Printf(TEXT("'%s' is not a socket or bone on the resolved mesh"), *Name);
		return Cap;
	}

	// GetSocketTransform resolves a bone name too if no socket of that name exists.
	Cap.WorldBefore = PointMesh->GetSocketTransform(NameF, RTS_World);
	Cap.RelativeBefore = PointMesh->GetSocketTransform(NameF, RTS_Component);

	// Governing bone ON THE ROTATED MESH — the bone that rigidly carries this point.
	FName GbName = NAME_None;
	if (PointMesh == RotatedMesh)
	{
		if (const USkeletalMeshSocket* S = PointMesh->GetSocketByName(NameF))
		{
			GbName = S->BoneName; // socket → its parent bone
		}
		else
		{
			GbName = NameF;       // the point is itself a bone
		}
	}
	else
	{
		// PointMesh is attached to the rotated mesh — governing bone is the attach point.
		const FName AttachSock = PointMesh->GetAttachSocketName();
		if (const USkeletalMeshSocket* S = RotatedMesh->GetSocketByName(AttachSock))
		{
			GbName = S->BoneName;
		}
		else
		{
			GbName = AttachSock;  // a bone name, or NAME_None
		}
	}

	Cap.GoverningBoneIdx = RotatedMesh->GetBoneIndex(GbName);
	if (Cap.GoverningBoneIdx == INDEX_NONE)
	{
		Cap.GoverningBoneIdx = 0; // root fallback (attached to component root / unresolved socket)
	}

	const FTransform GbWorldBefore = RotatedMesh->GetBoneTransform(Cap.GoverningBoneIdx);
	// Rel such that WorldBefore == Rel * GbWorldBefore (UE relative-transform convention,
	// the same composition SceneComponent uses: ComponentWorld = Relative * ParentWorld).
	Cap.RelToGoverningWorld = Cap.WorldBefore.GetRelativeTransform(GbWorldBefore);
	Cap.bValid = true;
	return Cap;
}

FTransform KinematicsCmd::RecomposeWorld(
	const FProbeCapture& Cap, USkeletalMeshComponent* RotatedMesh, const TArray<FTransform>& ModifiedCS)
{
	if (!RotatedMesh || !ModifiedCS.IsValidIndex(Cap.GoverningBoneIdx))
	{
		return Cap.WorldBefore;
	}
	// Engine convention: BoneWorld = BoneComponentSpace * ComponentToWorld (USkinnedMeshComponent::GetBoneTransform).
	const FTransform GbWorldAfter = ModifiedCS[Cap.GoverningBoneIdx] * RotatedMesh->GetComponentTransform();
	return Cap.RelToGoverningWorld * GbWorldAfter;
}

TSharedPtr<FJsonObject> KinematicsCmd::BuildProbePointReport(
	const FProbeCapture& Cap,
	const FTransform& WorldAfter, const FTransform& RelativeAfter,
	const FVector& IntentDir, const FVector& ForwardLocal,
	double& OutPosScore, double& OutFwdScore)
{
	OutPosScore = 0.0;
	OutFwdScore = 0.0;

	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	{
		TSharedPtr<FJsonObject> Pt = MakeShared<FJsonObject>();
		Pt->SetStringField(TEXT("name"), Cap.Name);
		Pt->SetStringField(TEXT("component"), Cap.ComponentLabel.IsEmpty() ? TEXT("body") : Cap.ComponentLabel);
		O->SetObjectField(TEXT("point"), Pt);
	}

	{
		TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
		Before->SetObjectField(TEXT("world"), XformJson(Cap.WorldBefore));
		Before->SetObjectField(TEXT("relative"), XformJson(Cap.RelativeBefore));
		O->SetObjectField(TEXT("before"), Before);

		TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetObjectField(TEXT("world"), XformJson(WorldAfter));
		After->SetObjectField(TEXT("relative"), XformJson(RelativeAfter));
		O->SetObjectField(TEXT("after"), After);
	}

	const FVector DP = WorldAfter.GetLocation() - Cap.WorldBefore.GetLocation();
	const double Mag = DP.Size();
	const bool bHasIntent = !IntentDir.IsNearlyZero();
	const FVector IntentN = bHasIntent ? IntentDir.GetSafeNormal() : FVector::ZeroVector;

	{
		TSharedPtr<FJsonObject> DPos = MakeShared<FJsonObject>();
		DPos->SetObjectField(TEXT("vector"), Vec3(DP));
		DPos->SetNumberField(TEXT("magnitude"), Mag);
		const double PitchAbove = (Mag > KINDA_SMALL_NUMBER)
			? FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(DP.Z / Mag, -1.0, 1.0)))
			: 0.0;
		DPos->SetNumberField(TEXT("pitch_above_horizontal_deg"), PitchAbove);
		if (bHasIntent && Mag > KINDA_SMALL_NUMBER)
		{
			const double Cos = FVector::DotProduct(DP / Mag, IntentN);
			DPos->SetNumberField(TEXT("angle_off_intent_deg"),
				FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Cos, -1.0, 1.0))));
			OutPosScore = FMath::Clamp(Cos, 0.0, 1.0);
		}
		O->SetObjectField(TEXT("delta_position_world"), DPos);
	}

	{
		const FQuat DQ = WorldAfter.GetRotation() * Cap.WorldBefore.GetRotation().Inverse();
		FVector Axis = FVector::UpVector;
		float Angle = 0.f;
		DQ.ToAxisAndAngle(Axis, Angle);

		TSharedPtr<FJsonObject> DOri = MakeShared<FJsonObject>();
		DOri->SetObjectField(TEXT("axis"), Vec3(Axis));
		DOri->SetNumberField(TEXT("angle_deg"), FMath::RadiansToDegrees(Angle));

		const FVector FwdLocalN = ForwardLocal.GetSafeNormal();
		const FVector NewFwdWorld = WorldAfter.GetRotation().RotateVector(FwdLocalN);
		DOri->SetObjectField(TEXT("new_forward_world"), Vec3(NewFwdWorld));
		DOri->SetObjectField(TEXT("forward_axis_local"), Vec3(FwdLocalN));
		if (bHasIntent)
		{
			const double Cos = FVector::DotProduct(NewFwdWorld, IntentN);
			DOri->SetNumberField(TEXT("forward_angle_off_intent_deg"),
				FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Cos, -1.0, 1.0))));
			OutFwdScore = FMath::Clamp(Cos, 0.0, 1.0);
		}
		O->SetObjectField(TEXT("delta_orientation_world"), DOri);
	}

	return O;
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleCommand dispatch
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPKinematicsCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("kinematics_read_transform")) return HandleReadTransform(Params);
	if (CommandType == TEXT("kinematics_probe"))          return HandleProbe(Params);
	if (CommandType == TEXT("kinematics_solve"))          return HandleSolve(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown kinematics command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: kinematic_read_transform, kinematic_probe, kinematic_solve."));
}

// ─────────────────────────────────────────────────────────────────────────────
// kinematic_read_transform
// ─────────────────────────────────────────────────────────────────────────────

// Resolve a UStaticMeshComponent on the actor by component name (GAPS #19). Used
// by HandleReadTransform so a `component` selector that names a static-mesh
// component (e.g. SM_BIKE_1's HANDLE_L socket) is honored instead of silently
// falling back to the body skeletal mesh.
static UStaticMeshComponent* ResolveStaticMeshComponent(AActor* Actor, const FString& Selector)
{
	if (!Actor || Selector.IsEmpty()) return nullptr;
	TArray<UStaticMeshComponent*> Comps;
	Actor->GetComponents<UStaticMeshComponent>(Comps);
	for (UStaticMeshComponent* C : Comps)
	{
		if (C && (C->GetName() == Selector || C->GetFName().ToString() == Selector))
		{
			return C;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FMCPKinematicsCommands::HandleReadTransform(const TSharedPtr<FJsonObject>& Params)
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
			TEXT("`actor` is required (label / short-name / FName in the active world)."));
	}

	AActor* Actor = FindActorByName(World, ActorName, Error);
	if (!Actor) return Error;

	FString MeshSel;
	Params->TryGetStringField(TEXT("mesh"), MeshSel);
	USkeletalMeshComponent* Mesh = ResolveMesh(Actor, MeshSel);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("world_type"), WorldType);

	if (Mesh)
	{
		// Skeletal path (unchanged): pose validity + skeletal asset + component transform.
		Result->SetStringField(TEXT("component_type"), TEXT("skeletal"));
		Result->SetBoolField(TEXT("pose_valid"), IsPoseValid(Mesh));
		Result->SetStringField(TEXT("mesh"), Mesh->GetSkeletalMeshAsset() ? Mesh->GetSkeletalMeshAsset()->GetPathName() : FString());
		Result->SetObjectField(TEXT("component_to_world"), XformJson(Mesh->GetComponentTransform()));
	}
	else
	{
		// Static/scene fallback (GAP-065b): a static-root actor has no skeletal mesh,
		// but its placed StaticMeshComponent (or scene root) still has a resolvable
		// world transform + bounds. Resolve the most specific scene component and report
		// it rather than rejecting. Skeletal-only fields (pose_valid, mesh) are omitted;
		// component_type tells the caller which kind resolved. World bounds mirror
		// actor_inspect's world_bounds shape (origin / box_extent / sphere_radius).
		USceneComponent* SceneComp = nullptr;
		FString CompType = TEXT("scene");
		if (UStaticMeshComponent* Named = ResolveStaticMeshComponent(Actor, MeshSel))
		{
			SceneComp = Named;
			CompType  = TEXT("static");
		}
		else
		{
			TArray<UStaticMeshComponent*> SMCs;
			Actor->GetComponents<UStaticMeshComponent>(SMCs);
			if (SMCs.Num() > 0)
			{
				SceneComp = SMCs[0];
				CompType  = TEXT("static");
			}
			else
			{
				SceneComp = Actor->GetRootComponent();
				CompType  = TEXT("scene");
			}
		}

		if (!SceneComp)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("No skeletal, static, or scene component resolved for actor '%s' (mesh='%s')"), *ActorName, *MeshSel),
				EMCPErrorCode::UnsupportedClass,
				TEXT("The actor has no USkeletalMeshComponent, no UStaticMeshComponent, and no scene root. `mesh` must be omitted/\"body\", a USkeletalMeshComponent or UStaticMeshComponent name on the actor, or an attached actor's name."));
		}

		Result->SetStringField(TEXT("component_type"), CompType);
		Result->SetStringField(TEXT("component"), SceneComp->GetName());
		Result->SetObjectField(TEXT("component_to_world"), XformJson(SceneComp->GetComponentTransform()));
		if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(SceneComp))
		{
			const FBoxSphereBounds B = Prim->Bounds;
			TSharedPtr<FJsonObject> Bnd = MakeShared<FJsonObject>();
			Bnd->SetObjectField(TEXT("origin"),     Vec3(B.Origin));
			Bnd->SetObjectField(TEXT("box_extent"), Vec3(B.BoxExtent));
			Bnd->SetNumberField(TEXT("sphere_radius"), B.SphereRadius);
			Result->SetObjectField(TEXT("world_bounds"), Bnd);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Out;
	const TArray<TSharedPtr<FJsonValue>>* Queries = nullptr;
	if (Params->TryGetArrayField(TEXT("queries"), Queries))
	{
		for (const TSharedPtr<FJsonValue>& QV : *Queries)
		{
			const TSharedPtr<FJsonObject> Q = QV.IsValid() ? QV->AsObject() : nullptr;
			if (!Q.IsValid()) continue;

			FString CompSel = MeshSel;
			Q->TryGetStringField(TEXT("component"), CompSel);
			// An explicit `component` that differs from the top-level mesh selector is a
			// deliberate target; if it resolves to neither a skeletal nor a static mesh
			// component we must NOT silently fall back to the body skeletal mesh — that
			// produced false-positive matches against same-named body bones (GAPS #19).
			const bool bExplicitComp = !CompSel.IsEmpty() && CompSel != MeshSel;
			USkeletalMeshComponent* QMesh = (CompSel == MeshSel) ? Mesh : ResolveMesh(Actor, CompSel);

			FString Nm;
			if (!Q->TryGetStringField(TEXT("socket"), Nm))
			{
				Q->TryGetStringField(TEXT("bone"), Nm);
			}
			if (Nm.Len() > NAME_SIZE) continue; // name too long for FName() -> would fatal
			const FName NmF(*Nm);

			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("name"), Nm);
			E->SetStringField(TEXT("component"), CompSel.IsEmpty() ? TEXT("body") : CompSel);

			if (QMesh)
			{
				// Skeletal mesh path: sockets or bones.
				E->SetStringField(TEXT("component_type"), TEXT("skeletal"));
				const bool bExists = QMesh->DoesSocketExist(NmF) || QMesh->GetBoneIndex(NmF) != INDEX_NONE;
				E->SetBoolField(TEXT("exists"), bExists);
				if (bExists)
				{
					if (const USkeletalMeshSocket* S = QMesh->GetSocketByName(NmF))
					{
						E->SetStringField(TEXT("parent_bone"), S->BoneName.ToString());
					}
					const FTransform W = QMesh->GetSocketTransform(NmF, RTS_World);
					E->SetObjectField(TEXT("world"), XformJson(W));
					E->SetObjectField(TEXT("relative"), XformJson(QMesh->GetSocketTransform(NmF, RTS_Component)));
					// +X of the world rotation = UE's forward/aim convention (POSITIONING.md §1.1);
					// note crossbow AimAxis is +Z-forward (§1.8) — the caller applies the weapon's convention.
					E->SetObjectField(TEXT("forward_world"), Vec3(W.GetRotation().GetAxisX()));
				}
			}
			else if (bExplicitComp)
			{
				// Static mesh component path (GAPS #19): honor a StaticMeshComponent socket
				// selector. Static-mesh sockets carry no parent bone.
				if (UStaticMeshComponent* SMC = ResolveStaticMeshComponent(Actor, CompSel))
				{
					E->SetStringField(TEXT("component_type"), TEXT("static"));
					const bool bExists = SMC->DoesSocketExist(NmF);
					E->SetBoolField(TEXT("exists"), bExists);
					if (bExists)
					{
						const FTransform W = SMC->GetSocketTransform(NmF, RTS_World);
						E->SetObjectField(TEXT("world"), XformJson(W));
						E->SetObjectField(TEXT("relative"), XformJson(SMC->GetSocketTransform(NmF, RTS_Component)));
						E->SetObjectField(TEXT("forward_world"), Vec3(W.GetRotation().GetAxisX()));
					}
				}
				else
				{
					// Named component matched no skeletal or static mesh component — report
					// not-found rather than masquerading a body bone as this component's socket.
					E->SetBoolField(TEXT("exists"), false);
					E->SetStringField(TEXT("error"),
						FString::Printf(TEXT("component '%s' matched no SkeletalMeshComponent or StaticMeshComponent on actor '%s'"), *CompSel, *ActorName));
				}
			}
			else
			{
				// No resolvable mesh and no explicit component to disambiguate.
				E->SetBoolField(TEXT("exists"), false);
			}
			Out.Add(MakeShared<FJsonValueObject>(E));
		}
	}
	Result->SetArrayField(TEXT("transforms"), Out);
	return Result;
}
