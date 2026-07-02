#pragma once

// Shared helpers for the Kinematics command handler, used across the three
// implementation .cpp files (core / _Probe / _Solve). See
// MCPKinematicsCommands.cpp for the file map and
// docs/mcp/POSITION_PROBE_TOOLS.md for the design.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ReferenceSkeleton.h"

class UWorld;
class AActor;
class USkeletalMeshComponent;

namespace KinematicsCmd
{
	// ── JSON serialization (inline — the project convention's {x,y,z}/{pitch,yaw,roll}) ──
	inline TSharedPtr<FJsonObject> Vec3(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	inline TSharedPtr<FJsonObject> RotJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("pitch"), R.Pitch);
		O->SetNumberField(TEXT("yaw"), R.Yaw);
		O->SetNumberField(TEXT("roll"), R.Roll);
		return O;
	}

	// { location, rotation } — the world or component-relative transform shape.
	inline TSharedPtr<FJsonObject> XformJson(const FTransform& T)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("location"), Vec3(T.GetLocation()));
		O->SetObjectField(TEXT("rotation"), RotJson(T.Rotator()));
		return O;
	}

	// Read an { x, y, z } object field; returns Def if absent.
	inline FVector ReadVec3Field(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, const FVector& Def)
	{
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if (O.IsValid() && O->TryGetObjectField(Field, Sub) && Sub)
		{
			double X = 0, Y = 0, Z = 0;
			(*Sub)->TryGetNumberField(TEXT("x"), X);
			(*Sub)->TryGetNumberField(TEXT("y"), Y);
			(*Sub)->TryGetNumberField(TEXT("z"), Z);
			return FVector(X, Y, Z);
		}
		return Def;
	}

	// ── World / actor / mesh resolution (defined in the core .cpp) ──
	// Prefer a running PIE world (the player + live animated poses live there);
	// fall back to the editor world for posing placed actors. OutWorldType is set
	// to "pie" or "editor" so callers report which world they resolved.
	UWorld* GetTargetWorld(TSharedPtr<FJsonObject>& OutError, FString& OutWorldType);
	AActor* FindActorByName(UWorld* World, const FString& Name, TSharedPtr<FJsonObject>& OutError);

	// Resolve a skeletal mesh from a selector relative to Actor:
	//   "" / "body"           -> the actor's first USkeletalMeshComponent
	//   "<ComponentName>"     -> a named USkeletalMeshComponent on Actor
	//   "<attached actor name>" -> a child actor's first USkeletalMeshComponent
	USkeletalMeshComponent* ResolveMesh(AActor* Actor, const FString& Selector);

	// True when the mesh has an asset + a populated component-space transform
	// array. In the editor world this is typically the ref/preview pose — the
	// caller is told the world type so it can judge.
	bool IsPoseValid(USkeletalMeshComponent* Mesh);

	// ── Rotation input parsing / conjugation ──
	// Accept { "axis": {x,y,z}, "angle_deg": N } OR { "pitch", "yaw", "roll" }.
	bool ParseInputQuat(const TSharedPtr<FJsonObject>& RotObj, FQuat& OutQ, FString& OutErr);

	// Conjugate an input quat (expressed in `Space`) into a component-space delta
	// suitable for BoneIK::RotateSubChain (which left-multiplies in CS):
	//   "component"  -> used as-is
	//   "world"      -> MeshQuatW^-1 * Q * MeshQuatW          (POSITIONING.md §1.6)
	//   "bone_local" -> BoneCSRot * Q * BoneCSRot^-1          (post-multiply equivalence)
	FQuat ToComponentSpaceDelta(const FQuat& QIn, const FString& Space, const FQuat& MeshQuatW, const FQuat& BoneCSRot);

	// ── Probe-point capture + FK recomposition ──
	// A probe point is a socket OR bone (GetSocketTransform resolves both) on
	// PointMesh, which may be the rotated mesh itself or an attached weapon mesh.
	struct FProbeCapture
	{
		bool      bValid = false;
		FString   Error;
		FString   ComponentLabel;   // echoed back
		FString   Name;             // socket/bone name
		bool      bExists = false;
		FTransform WorldBefore;     // world space
		FTransform RelativeBefore;  // RTS_Component on its own mesh
		// The bone ON THE ROTATED MESH that rigidly governs this point's motion
		// (the point's parent bone if on the rotated mesh; the attach bone if on
		// an attached mesh). RelToGoverningWorld is invariant under any rotation
		// of that bone or its ancestors, which is what makes the dry-run exact.
		int32      GoverningBoneIdx = INDEX_NONE;
		FTransform RelToGoverningWorld; // GbWorldBefore^-1 * WorldBefore
	};

	FProbeCapture CaptureProbe(USkeletalMeshComponent* RotatedMesh, USkeletalMeshComponent* PointMesh,
		const FString& Name, const FString& ComponentLabel);

	// World transform of the captured point after RotatedMesh's CS array changed.
	FTransform RecomposeWorld(const FProbeCapture& Cap, USkeletalMeshComponent* RotatedMesh,
		const TArray<FTransform>& ModifiedCS);

	// ── Per-probe-point report block (world ΔP/ΔQ + intent scoring) ──
	// Builds the JSON for one probe point given its before/after world+relative
	// transforms. IntentDir may be zero (no scoring). ForwardLocal is the local
	// axis treated as the point's "forward" (default +X; e.g. +Z for a blade).
	// OutPosScore/OutFwdScore are cosine-similarity terms in [0,1] (0 if no intent).
	TSharedPtr<FJsonObject> BuildProbePointReport(
		const FProbeCapture& Cap,
		const FTransform& WorldAfter, const FTransform& RelativeAfter,
		const FVector& IntentDir, const FVector& ForwardLocal,
		double& OutPosScore, double& OutFwdScore);
}
