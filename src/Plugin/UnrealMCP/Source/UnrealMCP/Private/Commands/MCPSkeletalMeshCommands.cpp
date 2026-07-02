#include "Commands/MCPSkeletalMeshCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "ClothingAssetBase.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "EditorAssetLibrary.h"
#include "UObject/SavePackage.h"
#include "Materials/MaterialInterface.h"

FMCPSkeletalMeshCommands::FMCPSkeletalMeshCommands()
{
}

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("anim_skeletal_mesh_inspect"))
	{
		return HandleInspectSkeletalMesh(Params);
	}
	if (CommandType == TEXT("anim_skeletal_mesh_set_section_disabled"))
	{
		return HandleSetSkeletalMeshSectionDisabled(Params);
	}
	if (CommandType == TEXT("anim_physics_inspect"))
	{
		return HandleInspectPhysicsAsset(Params);
	}
	if (CommandType == TEXT("physics_set_body_collision"))
	{
		return HandleSetPhysicsBodyCollision(Params);
	}
	if (CommandType == TEXT("physics_set_constraint_motion"))
	{
		return HandleSetPhysicsConstraintMotion(Params);
	}
	if (CommandType == TEXT("mesh_set_physics_asset"))
	{
		return HandleSetSkeletalMeshPhysicsAsset(Params);
	}
	if (CommandType == TEXT("merge_bones_into_skeleton"))
	{
		return HandleMergeBonesIntoSkeleton(Params);
	}
	if (CommandType == TEXT("merge_bones_into_skeletal_mesh"))
	{
		return HandleMergeBonesIntoSkeletalMesh(Params);
	}
	if (CommandType == TEXT("mesh_build_bend_chain"))
	{
		return HandleBuildBendChain(Params);
	}
	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown skeletal-mesh command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: inspect_skeletal_mesh, set_skeletal_mesh_section_disabled, inspect_physics_asset, set_physics_body_collision, set_physics_constraint_motion, set_skeletal_mesh_physics_asset, merge_bones_into_skeleton, merge_bones_into_skeletal_mesh, skeletal_mesh_build_bend_chain."));
}

// ─── helpers ─────────────────────────────────────────────────────────────────

static USkeletalMesh* LoadSkeletalMeshFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutPath, FString& OutError, EMCPErrorCode& OutCode, FString& OutHint)
{
	if (!Params.IsValid() || !Params->HasField(TEXT("path")))
	{
		OutError = TEXT("Missing required param: path");
		OutCode = EMCPErrorCode::InvalidArgument;
		OutHint = TEXT("`path` is required and must be the full asset path to a USkeletalMesh, e.g. `/Game/Characters/MyChar_SK`. Use `list_assets` with `asset_type='SkeletalMesh'` to discover.");
		return nullptr;
	}
	OutPath = Params->GetStringField(TEXT("path"));
	UObject* Loaded = UEditorAssetLibrary::LoadAsset(OutPath);
	if (!Loaded)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at: %s"), *OutPath);
		OutCode = EMCPErrorCode::AssetNotFound;
		OutHint = TEXT("Verify the asset exists at the given path. Paths are case-sensitive and must include `/Game/` prefix. Use `list_assets` to discover.");
		return nullptr;
	}
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(Loaded);
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("Asset is not a USkeletalMesh: %s"), *OutPath);
		OutCode = EMCPErrorCode::UnsupportedClass;
		OutHint = TEXT("Expected a USkeletalMesh. Use `list_assets` with `asset_type='SkeletalMesh'` to discover skeletal-mesh assets, or `get_class_properties` on the actual class for context.");
		return nullptr;
	}
	return Mesh;
}

// ─── inspect_skeletal_mesh ───────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleInspectSkeletalMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error, Hint;
	EMCPErrorCode Code = EMCPErrorCode::InvalidArgument;
	USkeletalMesh* Mesh = LoadSkeletalMeshFromParams(Params, Path, Error, Code, Hint);
	if (!Mesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error, Code, Hint);
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("path"), Path);

	// ── Skeleton bones ──
	const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
	const int32 NumBones = RefSkel.GetNum();
	{
		// Compute component-space bone transforms by walking the hierarchy and
		// concatenating local transforms. The ref skeleton stores LOCAL bind
		// poses; component-space tells us where each bone lives in mesh space.
		const TArray<FTransform>& LocalPose = RefSkel.GetRefBonePose();
		TArray<FTransform> ComponentSpace;
		ComponentSpace.SetNum(NumBones);
		for (int32 i = 0; i < NumBones; ++i)
		{
			const int32 Parent = RefSkel.GetParentIndex(i);
			ComponentSpace[i] = (Parent == INDEX_NONE)
				? LocalPose[i]
				: LocalPose[i] * ComponentSpace[Parent];
		}

		TArray<TSharedPtr<FJsonValue>> BonesJson;
		for (int32 i = 0; i < NumBones; ++i)
		{
			TSharedPtr<FJsonObject> BoneObj = MakeShareable(new FJsonObject);
			BoneObj->SetNumberField(TEXT("index"), i);
			BoneObj->SetStringField(TEXT("name"), RefSkel.GetBoneName(i).ToString());
			BoneObj->SetNumberField(TEXT("parent_index"), RefSkel.GetParentIndex(i));
			const FVector LocalT = LocalPose[i].GetTranslation();
			const FVector LocalS = LocalPose[i].GetScale3D();
			const FVector CompT  = ComponentSpace[i].GetTranslation();
			const FVector CompS  = ComponentSpace[i].GetScale3D();
			auto MakeVec = [](const FVector& V) {
				TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
				O->SetNumberField(TEXT("x"), V.X);
				O->SetNumberField(TEXT("y"), V.Y);
				O->SetNumberField(TEXT("z"), V.Z);
				return O;
			};
			BoneObj->SetObjectField(TEXT("local_pos"),     MakeVec(LocalT));
			BoneObj->SetObjectField(TEXT("local_scale"),   MakeVec(LocalS));
			BoneObj->SetObjectField(TEXT("component_pos"), MakeVec(CompT));
			BoneObj->SetObjectField(TEXT("component_scale"), MakeVec(CompS));
			BonesJson.Add(MakeShareable(new FJsonValueObject(BoneObj)));
		}
		Result->SetNumberField(TEXT("num_bones"), NumBones);
		Result->SetArrayField(TEXT("bones"), BonesJson);
	}

	// ── Material slots ──
	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	{
		TArray<TSharedPtr<FJsonValue>> MatsJson;
		for (int32 i = 0; i < Materials.Num(); ++i)
		{
			const FSkeletalMaterial& M = Materials[i];
			TSharedPtr<FJsonObject> MatObj = MakeShareable(new FJsonObject);
			MatObj->SetNumberField(TEXT("index"), i);
			MatObj->SetStringField(TEXT("slot_name"), M.MaterialSlotName.ToString());
			MatObj->SetStringField(TEXT("imported_slot_name"), M.ImportedMaterialSlotName.ToString());
			MatObj->SetStringField(TEXT("material"),
				M.MaterialInterface ? M.MaterialInterface->GetPathName() : TEXT(""));
			MatsJson.Add(MakeShareable(new FJsonValueObject(MatObj)));
		}
		Result->SetNumberField(TEXT("num_material_slots"), Materials.Num());
		Result->SetArrayField(TEXT("material_slots"), MatsJson);
	}

	// ── LOD render sections ──
	{
		TArray<TSharedPtr<FJsonValue>> LodsJson;
		if (FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering())
		{
			for (int32 LodIdx = 0; LodIdx < RenderData->LODRenderData.Num(); ++LodIdx)
			{
				const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LodIdx];
				TSharedPtr<FJsonObject> LodObj = MakeShareable(new FJsonObject);
				LodObj->SetNumberField(TEXT("lod_index"), LodIdx);

				TArray<TSharedPtr<FJsonValue>> SectionsJson;
				for (int32 s = 0; s < LODData.RenderSections.Num(); ++s)
				{
					const FSkelMeshRenderSection& Sec = LODData.RenderSections[s];
					TSharedPtr<FJsonObject> SecObj = MakeShareable(new FJsonObject);
					SecObj->SetNumberField(TEXT("section_index"), s);
					SecObj->SetNumberField(TEXT("material_index"), Sec.MaterialIndex);
					SecObj->SetNumberField(TEXT("num_vertices"), Sec.NumVertices);
					SecObj->SetNumberField(TEXT("num_triangles"), Sec.NumTriangles);
					SecObj->SetNumberField(TEXT("base_vertex_index"), Sec.BaseVertexIndex);
					SecObj->SetBoolField(TEXT("disabled"), Sec.bDisabled);
					SecObj->SetBoolField(TEXT("has_clothing_data"), Sec.HasClothingData());
					// Bone map: which ref-skeleton bones this section's vertices are skinned to.
					// A size-1 bone map means the section is rigidly bound to a single bone (no
					// bending); a chain-bent rod section spans many bones. Exposes the skin
					// binding so re-skinning (skeletal_mesh_build_bend_chain) can be verified.
					SecObj->SetNumberField(TEXT("bone_map_count"), Sec.BoneMap.Num());
					SecObj->SetNumberField(TEXT("max_bone_influences"), Sec.MaxBoneInfluences);
					{
						TArray<TSharedPtr<FJsonValue>> BoneMapJson;
						for (const FBoneIndexType BoneIdx : Sec.BoneMap)
						{
							BoneMapJson.Add(MakeShareable(new FJsonValueNumber(BoneIdx)));
						}
						SecObj->SetArrayField(TEXT("bone_map"), BoneMapJson);
					}
					SectionsJson.Add(MakeShareable(new FJsonValueObject(SecObj)));
				}
				LodObj->SetArrayField(TEXT("sections"), SectionsJson);
				LodsJson.Add(MakeShareable(new FJsonValueObject(LodObj)));
			}
		}
		Result->SetArrayField(TEXT("lods"), LodsJson);
	}

	// ── Clothing assets ──
	{
		const TArray<UClothingAssetBase*>& ClothAssets = Mesh->GetMeshClothingAssets();
		TArray<TSharedPtr<FJsonValue>> ClothJson;
		for (int32 i = 0; i < ClothAssets.Num(); ++i)
		{
			UClothingAssetBase* CA = ClothAssets[i];
			TSharedPtr<FJsonObject> CObj = MakeShareable(new FJsonObject);
			CObj->SetNumberField(TEXT("index"), i);
			CObj->SetStringField(TEXT("name"), CA ? CA->GetName() : TEXT("<null>"));
			CObj->SetStringField(TEXT("path"), CA ? CA->GetPathName() : TEXT(""));
			CObj->SetStringField(TEXT("class"), CA ? CA->GetClass()->GetName() : TEXT(""));
			ClothJson.Add(MakeShareable(new FJsonValueObject(CObj)));
		}
		Result->SetNumberField(TEXT("num_clothing_assets"), ClothAssets.Num());
		Result->SetArrayField(TEXT("clothing_assets"), ClothJson);
	}

	// ── Physics asset ──
	{
		UPhysicsAsset* PA = Mesh->GetPhysicsAsset();
		Result->SetStringField(TEXT("physics_asset"), PA ? PA->GetPathName() : TEXT(""));
	}

	// ── Skeleton ──
	{
		USkeleton* Skel = Mesh->GetSkeleton();
		Result->SetStringField(TEXT("skeleton"), Skel ? Skel->GetPathName() : TEXT(""));
	}

	return FMCPCommonUtils::CreateSuccessResponse(Result);
}

// ─── set_skeletal_mesh_section_disabled ─────────────────────────────────────

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleSetSkeletalMeshSectionDisabled(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error, Hint;
	EMCPErrorCode Code = EMCPErrorCode::InvalidArgument;
	USkeletalMesh* Mesh = LoadSkeletalMeshFromParams(Params, Path, Error, Code, Hint);
	if (!Mesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error, Code, Hint);
	}

	int32 LodIndex = 0, SectionIndex = 0;
	bool bDisabled = true;
	Params->TryGetNumberField(TEXT("lod_index"), LodIndex);
	if (!Params->TryGetNumberField(TEXT("section_index"), SectionIndex))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: section_index"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`section_index` is required (integer). Use `inspect_skeletal_mesh` to list LODs and their sections — `lods[i].sections[j].section_index` is the value to pass."));
	}
	Params->TryGetBoolField(TEXT("disabled"), bDisabled);

	// The persisted bDisabled flag lives on the imported model (LODModel) per
	// section, NOT on the runtime LODRenderData (which is rebuilt). Editing
	// here flows through to the cooked render data on next build.
	FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	if (!ImportedModel || !ImportedModel->LODModels.IsValidIndex(LodIndex))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid LOD index: %d"), LodIndex),
			EMCPErrorCode::OutOfRange,
			TEXT("`lod_index` must be < the mesh's LOD count. Use `inspect_skeletal_mesh` to see the available LOD indices in `lods[i].lod_index`."));
	}

	FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LodIndex];
	if (!LODModel.Sections.IsValidIndex(SectionIndex))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid section index: %d (LOD %d has %d sections)"),
				SectionIndex, LodIndex, LODModel.Sections.Num()),
			EMCPErrorCode::OutOfRange,
			TEXT("`section_index` must be < the LOD's section count. Use `inspect_skeletal_mesh` to list valid section indices for this LOD."));
	}

	Mesh->Modify();

	// The PERSISTED disabled flag lives in LODModel.UserSectionsData (a TMap
	// keyed by Section.OriginalDataSectionIndex). FSkelMeshSection::bDisabled
	// is a *derived cache* that gets rebuilt from UserSectionsData; modifying
	// it alone doesn't survive editor reload. Set both, then sync.
	FSkelMeshSection& Section = LODModel.Sections[SectionIndex];
	FSkelMeshSourceSectionUserData& UserData =
		FSkelMeshSourceSectionUserData::GetSourceSectionUserData(LODModel.UserSectionsData, Section);
	UserData.bDisabled = bDisabled;
	Section.bDisabled = bDisabled;

	// Sync the user-section data back into all FSkelMeshSection entries so the
	// derived cache matches what we just persisted.
	LODModel.SyncronizeUserSectionsDataArray();

	// Also flip the live RenderSection flag so the change is visible without
	// a full rebuild (which would also reset other in-memory edits).
	if (FSkeletalMeshRenderData* RD = Mesh->GetResourceForRendering())
	{
		if (RD->LODRenderData.IsValidIndex(LodIndex) &&
		    RD->LODRenderData[LodIndex].RenderSections.IsValidIndex(SectionIndex))
		{
			RD->LODRenderData[LodIndex].RenderSections[SectionIndex].bDisabled = bDisabled;
		}
	}

	Mesh->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveLoadedAsset(Mesh, /*bOnlyIfIsDirty=*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Section disabled flag was set in memory but the package failed to save to disk: %s"), *Path),
			EMCPErrorCode::Internal,
			TEXT("SaveLoadedAsset returned false — the edit will not survive an editor restart. Saving no-ops while PIE is running or when the package is read-only; stop PIE and ensure the asset is writable, then retry."));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("lod_index"), LodIndex);
	Result->SetNumberField(TEXT("section_index"), SectionIndex);
	Result->SetBoolField(TEXT("disabled"), bDisabled);
	return FMCPCommonUtils::CreateSuccessResponse(Result);
}

// ─── inspect_physics_asset ──────────────────────────────────────────────────

#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "Engine/EngineTypes.h"

namespace
{
	// ECollisionEnabled <-> string conversion shared by inspect + set tools.
	const TCHAR* CollisionEnabledToString(ECollisionEnabled::Type Value)
	{
		switch (Value)
		{
		case ECollisionEnabled::NoCollision:       return TEXT("NoCollision");
		case ECollisionEnabled::QueryOnly:         return TEXT("QueryOnly");
		case ECollisionEnabled::PhysicsOnly:       return TEXT("PhysicsOnly");
		case ECollisionEnabled::QueryAndPhysics:   return TEXT("QueryAndPhysics");
		case ECollisionEnabled::ProbeOnly:         return TEXT("ProbeOnly");
		case ECollisionEnabled::QueryAndProbe:     return TEXT("QueryAndProbe");
		default:                                   return TEXT("Unknown");
		}
	}

	// Returns true on success.  Caller passes the rejected string back to the
	// client when this returns false.
	bool StringToCollisionEnabled(const FString& In, ECollisionEnabled::Type& Out)
	{
		if (In.Equals(TEXT("NoCollision"),       ESearchCase::IgnoreCase)) { Out = ECollisionEnabled::NoCollision;     return true; }
		if (In.Equals(TEXT("QueryOnly"),         ESearchCase::IgnoreCase)) { Out = ECollisionEnabled::QueryOnly;       return true; }
		if (In.Equals(TEXT("PhysicsOnly"),       ESearchCase::IgnoreCase)) { Out = ECollisionEnabled::PhysicsOnly;     return true; }
		if (In.Equals(TEXT("QueryAndPhysics"),   ESearchCase::IgnoreCase)) { Out = ECollisionEnabled::QueryAndPhysics; return true; }
		if (In.Equals(TEXT("ProbeOnly"),         ESearchCase::IgnoreCase)) { Out = ECollisionEnabled::ProbeOnly;       return true; }
		if (In.Equals(TEXT("QueryAndProbe"),     ESearchCase::IgnoreCase)) { Out = ECollisionEnabled::QueryAndProbe;   return true; }
		return false;
	}

	TSharedPtr<FJsonObject> VectorToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	TSharedPtr<FJsonObject> RotatorToJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetNumberField(TEXT("pitch"), R.Pitch);
		O->SetNumberField(TEXT("yaw"),   R.Yaw);
		O->SetNumberField(TEXT("roll"),  R.Roll);
		return O;
	}

	// Constraint-motion <-> string conversion (Free | Limited | Locked), shared
	// by set_physics_constraint_motion. Angular and linear use distinct enums.
	const TCHAR* AngularMotionToString(EAngularConstraintMotion V)
	{
		switch (V)
		{
		case ACM_Free:    return TEXT("Free");
		case ACM_Limited: return TEXT("Limited");
		case ACM_Locked:  return TEXT("Locked");
		default:          return TEXT("Unknown");
		}
	}
	const TCHAR* LinearMotionToString(ELinearConstraintMotion V)
	{
		switch (V)
		{
		case LCM_Free:    return TEXT("Free");
		case LCM_Limited: return TEXT("Limited");
		case LCM_Locked:  return TEXT("Locked");
		default:          return TEXT("Unknown");
		}
	}
	bool StringToAngularMotion(const FString& In, EAngularConstraintMotion& Out)
	{
		if (In.Equals(TEXT("Free"),    ESearchCase::IgnoreCase)) { Out = ACM_Free;    return true; }
		if (In.Equals(TEXT("Limited"), ESearchCase::IgnoreCase)) { Out = ACM_Limited; return true; }
		if (In.Equals(TEXT("Locked"),  ESearchCase::IgnoreCase)) { Out = ACM_Locked;  return true; }
		return false;
	}
	bool StringToLinearMotion(const FString& In, ELinearConstraintMotion& Out)
	{
		if (In.Equals(TEXT("Free"),    ESearchCase::IgnoreCase)) { Out = LCM_Free;    return true; }
		if (In.Equals(TEXT("Limited"), ESearchCase::IgnoreCase)) { Out = LCM_Limited; return true; }
		if (In.Equals(TEXT("Locked"),  ESearchCase::IgnoreCase)) { Out = LCM_Locked;  return true; }
		return false;
	}
}

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleInspectPhysicsAsset(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid() || !Params->HasField(TEXT("path")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: path"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`path` is required and must be the full asset path to a UPhysicsAsset, e.g. `/Game/Characters/MyChar_PhysicsAsset`. Use `list_assets` with `asset_type='PhysicsAsset'` to discover."));
	}
	const FString Path = Params->GetStringField(TEXT("path"));
	UObject* Loaded = UEditorAssetLibrary::LoadAsset(Path);
	if (!Loaded)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load asset at: %s"), *Path),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the asset exists at the given path. Paths are case-sensitive and must include `/Game/` prefix. Use `list_assets` to discover."));
	}
	UPhysicsAsset* PA = Cast<UPhysicsAsset>(Loaded);
	if (!PA)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset is not a UPhysicsAsset: %s"), *Path),
			EMCPErrorCode::UnsupportedClass,
			TEXT("Expected a UPhysicsAsset. Use `list_assets` with `asset_type='PhysicsAsset'` to discover physics assets, or `inspect_skeletal_mesh` to find the bound physics_asset on a skeletal mesh."));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("path"), Path);

	// ── Bodies ──
	TArray<TSharedPtr<FJsonValue>> BodiesJson;
	for (int32 i = 0; i < PA->SkeletalBodySetups.Num(); ++i)
	{
		USkeletalBodySetup* BS = PA->SkeletalBodySetups[i];
		if (!BS) continue;
		TSharedPtr<FJsonObject> BObj = MakeShareable(new FJsonObject);
		BObj->SetNumberField(TEXT("index"), i);
		BObj->SetStringField(TEXT("bone_name"), BS->BoneName.ToString());
		BObj->SetNumberField(TEXT("num_box"),     BS->AggGeom.BoxElems.Num());
		BObj->SetNumberField(TEXT("num_sphere"),  BS->AggGeom.SphereElems.Num());
		BObj->SetNumberField(TEXT("num_capsule"), BS->AggGeom.SphylElems.Num());
		BObj->SetNumberField(TEXT("num_convex"),  BS->AggGeom.ConvexElems.Num());
		// PhysicsType: simulated / kinematic / default
		const TCHAR* PhysType =
			BS->PhysicsType == EPhysicsType::PhysType_Simulated ? TEXT("Simulated") :
			BS->PhysicsType == EPhysicsType::PhysType_Kinematic ? TEXT("Kinematic") :
			TEXT("Default");
		BObj->SetStringField(TEXT("physics_type"), PhysType);

		// Collision: the field that decides whether SweepComponent /
		// LineTraceComponent against the skeletal mesh registers hits on
		// this body.  NoCollision/PhysicsOnly = invisible to query traces;
		// QueryOnly/QueryAndPhysics/QueryAndProbe = sweep-visible.
		BObj->SetStringField(TEXT("collision_enabled"),
			CollisionEnabledToString(BS->DefaultInstance.GetCollisionEnabled()));
		BObj->SetStringField(TEXT("collision_profile_name"),
			BS->DefaultInstance.GetCollisionProfileName().ToString());

		// Per-shape geometry — capsule / sphere / box dimensions + bone-local
		// transforms.  Convex hulls are reported by count only (their vertex
		// arrays are large and not usually needed for collision tuning).
		TArray<TSharedPtr<FJsonValue>> CapsulesJson;
		for (const FKSphylElem& E : BS->AggGeom.SphylElems)
		{
			TSharedPtr<FJsonObject> S = MakeShareable(new FJsonObject);
			S->SetStringField(TEXT("name"),    E.GetName().ToString());
			S->SetNumberField(TEXT("radius"),  E.Radius);
			S->SetNumberField(TEXT("length"),  E.Length);
			S->SetObjectField(TEXT("center"),  VectorToJson(E.Center));
			S->SetObjectField(TEXT("rotation"), RotatorToJson(E.Rotation));
			CapsulesJson.Add(MakeShareable(new FJsonValueObject(S)));
		}
		BObj->SetArrayField(TEXT("capsules"), CapsulesJson);

		TArray<TSharedPtr<FJsonValue>> SpheresJson;
		for (const FKSphereElem& E : BS->AggGeom.SphereElems)
		{
			TSharedPtr<FJsonObject> S = MakeShareable(new FJsonObject);
			S->SetStringField(TEXT("name"),   E.GetName().ToString());
			S->SetNumberField(TEXT("radius"), E.Radius);
			S->SetObjectField(TEXT("center"), VectorToJson(E.Center));
			SpheresJson.Add(MakeShareable(new FJsonValueObject(S)));
		}
		BObj->SetArrayField(TEXT("spheres"), SpheresJson);

		TArray<TSharedPtr<FJsonValue>> BoxesJson;
		for (const FKBoxElem& E : BS->AggGeom.BoxElems)
		{
			TSharedPtr<FJsonObject> S = MakeShareable(new FJsonObject);
			S->SetStringField(TEXT("name"),     E.GetName().ToString());
			S->SetNumberField(TEXT("x"),        E.X);
			S->SetNumberField(TEXT("y"),        E.Y);
			S->SetNumberField(TEXT("z"),        E.Z);
			S->SetObjectField(TEXT("center"),   VectorToJson(E.Center));
			S->SetObjectField(TEXT("rotation"), RotatorToJson(E.Rotation));
			BoxesJson.Add(MakeShareable(new FJsonValueObject(S)));
		}
		BObj->SetArrayField(TEXT("boxes"), BoxesJson);

		BodiesJson.Add(MakeShareable(new FJsonValueObject(BObj)));
	}
	Result->SetNumberField(TEXT("num_bodies"), PA->SkeletalBodySetups.Num());
	Result->SetArrayField(TEXT("bodies"), BodiesJson);

	// ── Constraints ──
	TArray<TSharedPtr<FJsonValue>> ConstraintsJson;
	for (int32 i = 0; i < PA->ConstraintSetup.Num(); ++i)
	{
		UPhysicsConstraintTemplate* CT = PA->ConstraintSetup[i];
		if (!CT) continue;
		TSharedPtr<FJsonObject> CObj = MakeShareable(new FJsonObject);
		CObj->SetNumberField(TEXT("index"), i);
		CObj->SetStringField(TEXT("bone1"), CT->DefaultInstance.ConstraintBone1.ToString());
		CObj->SetStringField(TEXT("bone2"), CT->DefaultInstance.ConstraintBone2.ToString());
		CObj->SetStringField(TEXT("joint_name"), CT->DefaultInstance.JointName.ToString());
		ConstraintsJson.Add(MakeShareable(new FJsonValueObject(CObj)));
	}
	Result->SetNumberField(TEXT("num_constraints"), PA->ConstraintSetup.Num());
	Result->SetArrayField(TEXT("constraints"), ConstraintsJson);

	// ── Bound skeleton + preview mesh ──
	if (PA->GetPreviewMesh())
	{
		Result->SetStringField(TEXT("preview_mesh"), PA->GetPreviewMesh()->GetPathName());
	}

	return FMCPCommonUtils::CreateSuccessResponse(Result);
}

// ─── set_physics_body_collision ─────────────────────────────────────────────
// Mutate per-body CollisionEnabled.  The most common use is enabling query
// collision on per-bone capsules so SweepComponent / LineTraceComponent
// against the skeletal mesh registers hits — required for things like the
// joust head-exclusion sweep against the horse.

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleSetPhysicsBodyCollision(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid() || !Params->HasField(TEXT("path")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: path"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`path` is required and must be the full asset path to a UPhysicsAsset, e.g. `/Game/Characters/MyChar_PhysicsAsset`. Use `list_assets` with `asset_type='PhysicsAsset'` to discover."));
	}
	const FString Path = Params->GetStringField(TEXT("path"));
	UObject* Loaded = UEditorAssetLibrary::LoadAsset(Path);
	if (!Loaded)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load asset at: %s"), *Path),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the asset exists at the given path. Paths are case-sensitive and must include `/Game/` prefix. Use `list_assets` to discover."));
	}
	UPhysicsAsset* PA = Cast<UPhysicsAsset>(Loaded);
	if (!PA)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset is not a UPhysicsAsset: %s"), *Path),
			EMCPErrorCode::UnsupportedClass,
			TEXT("Expected a UPhysicsAsset. Use `list_assets` with `asset_type='PhysicsAsset'` to discover physics assets, or `inspect_skeletal_mesh` to find the bound physics_asset on a skeletal mesh."));
	}

	FString CollisionEnabledStr;
	if (!Params->TryGetStringField(TEXT("collision_enabled"), CollisionEnabledStr))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: collision_enabled (NoCollision | QueryOnly | "
			     "PhysicsOnly | QueryAndPhysics | ProbeOnly | QueryAndProbe)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`collision_enabled` is required. Valid values: NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics, ProbeOnly, QueryAndProbe. For sweep-visible bodies (e.g. capsules answering trace queries) use QueryOnly or QueryAndPhysics."));
	}
	ECollisionEnabled::Type NewValue = ECollisionEnabled::NoCollision;
	if (!StringToCollisionEnabled(CollisionEnabledStr, NewValue))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown collision_enabled value: %s"), *CollisionEnabledStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("`collision_enabled` must be exactly one of: NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics, ProbeOnly, QueryAndProbe (case-insensitive). Reject is on string mismatch."));
	}

	// Optional body filter — names array.  When absent or empty, apply to all
	// bodies in the asset (the typical "fix the whole horse" case).
	TSet<FName> WantedBones;
	const TArray<TSharedPtr<FJsonValue>>* BodyNamesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("body_names"), BodyNamesArr) && BodyNamesArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *BodyNamesArr)
		{
			if (V.IsValid() && V->Type == EJson::String)
			{
				WantedBones.Add(*V->AsString());
			}
		}
	}
	const bool bFilterByName = WantedBones.Num() > 0;

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	// Apply.  Wrap in a transaction so the editor's undo stack records this
	// as one logical operation.
	PA->Modify();

	TArray<TSharedPtr<FJsonValue>> ChangedJson;
	for (int32 i = 0; i < PA->SkeletalBodySetups.Num(); ++i)
	{
		USkeletalBodySetup* BS = PA->SkeletalBodySetups[i];
		if (!BS) continue;
		if (bFilterByName && !WantedBones.Contains(BS->BoneName)) continue;

		const ECollisionEnabled::Type Old = BS->DefaultInstance.GetCollisionEnabled();
		if (Old == NewValue) continue;

		BS->Modify();
		BS->DefaultInstance.SetCollisionEnabled(NewValue);

		TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
		Entry->SetNumberField(TEXT("index"), i);
		Entry->SetStringField(TEXT("bone_name"), BS->BoneName.ToString());
		Entry->SetStringField(TEXT("from"), CollisionEnabledToString(Old));
		Entry->SetStringField(TEXT("to"),   CollisionEnabledToString(NewValue));
		ChangedJson.Add(MakeShareable(new FJsonValueObject(Entry)));
	}

	bool bSaved = false;
	if (ChangedJson.Num() > 0)
	{
		PA->MarkPackageDirty();
		if (bSave)
		{
			bSaved = UEditorAssetLibrary::SaveLoadedAsset(PA, /*bOnlyIfIsDirty=*/false);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("collision_enabled"), CollisionEnabledToString(NewValue));
	Result->SetNumberField(TEXT("num_changed"), ChangedJson.Num());
	Result->SetArrayField(TEXT("changed"), ChangedJson);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMCPCommonUtils::CreateSuccessResponse(Result);
}

// ─── set_physics_constraint_motion ───────────────────────────────────────────
// Edit (or delete) a constraint in a UPhysicsAsset's ConstraintSetup, keyed by
// joint_name OR a (bone1, bone2) pair. The motivating case: an over-constrained
// ragdoll (redundant closed-loop constraints the solver can't satisfy) that
// jitters and never settles — delete the offending joint, or relax its motions
// to Free, without opening the PhysicsAsset editor by hand.

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleSetPhysicsConstraintMotion(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid() || !Params->HasField(TEXT("path")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: path"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`path` is required and must be the full asset path to a UPhysicsAsset. Use `inspect_physics_asset` to list its constraints (index, bone1, bone2, joint_name)."));
	}
	const FString Path = Params->GetStringField(TEXT("path"));
	UObject* Loaded = UEditorAssetLibrary::LoadAsset(Path);
	if (!Loaded)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load asset at: %s"), *Path),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the asset exists at the given path. If a PIE session is running, asset loads fail — stop PIE and retry."));
	}
	UPhysicsAsset* PA = Cast<UPhysicsAsset>(Loaded);
	if (!PA)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset is not a UPhysicsAsset: %s"), *Path),
			EMCPErrorCode::UnsupportedClass,
			TEXT("Expected a UPhysicsAsset. Use `list_assets` with `asset_type='PhysicsAsset'` to discover."));
	}

	// Locate the constraint: prefer the (bone1, bone2) pair (order-insensitive),
	// fall back to joint_name. JointName is frequently None on auto-generated
	// assets, so the bone pair is the reliable key.
	FString JointName, Bone1, Bone2;
	Params->TryGetStringField(TEXT("joint_name"), JointName);
	Params->TryGetStringField(TEXT("bone1"), Bone1);
	Params->TryGetStringField(TEXT("bone2"), Bone2);
	if (JointName.IsEmpty() && (Bone1.IsEmpty() || Bone2.IsEmpty()))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing constraint key: provide joint_name, or both bone1 and bone2"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Key the constraint by `joint_name`, or by the `bone1`+`bone2` pair (order-insensitive). Use `inspect_physics_asset` to read the available constraints."));
	}

	int32 FoundIdx = INDEX_NONE;
	for (int32 i = 0; i < PA->ConstraintSetup.Num(); ++i)
	{
		UPhysicsConstraintTemplate* CT = PA->ConstraintSetup[i];
		if (!CT) continue;
		const FName CB1 = CT->DefaultInstance.ConstraintBone1;
		const FName CB2 = CT->DefaultInstance.ConstraintBone2;
		const FName CJ  = CT->DefaultInstance.JointName;
		const bool bMatchPair = !Bone1.IsEmpty() && !Bone2.IsEmpty() &&
			((CB1 == FName(*Bone1) && CB2 == FName(*Bone2)) ||
			 (CB1 == FName(*Bone2) && CB2 == FName(*Bone1)));
		const bool bMatchJoint = !JointName.IsEmpty() && CJ == FName(*JointName);
		if (bMatchPair || bMatchJoint) { FoundIdx = i; break; }
	}
	if (FoundIdx == INDEX_NONE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No matching constraint found"),
			EMCPErrorCode::NodeNotFound,
			TEXT("No constraint matched the given joint_name / bone pair. Use `inspect_physics_asset` to read the exact bone1/bone2/joint_name values (names are case-sensitive)."));
	}

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bDelete = false;
	Params->TryGetBoolField(TEXT("delete"), bDelete);

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("constraint_index"), FoundIdx);

	if (bDelete)
	{
		UPhysicsConstraintTemplate* CT = PA->ConstraintSetup[FoundIdx];
		Result->SetStringField(TEXT("bone1"), CT->DefaultInstance.ConstraintBone1.ToString());
		Result->SetStringField(TEXT("bone2"), CT->DefaultInstance.ConstraintBone2.ToString());
		Result->SetStringField(TEXT("joint_name"), CT->DefaultInstance.JointName.ToString());
		// Mirror the editor's delete path (PhysicsAsset->Modify(); RemoveAt) — the
		// engine util FPhysicsAssetUtils::DestroyConstraint is just RemoveAt(idx)
		// on the public ConstraintSetup array, so do it inline (no extra module dep).
		PA->Modify();
		PA->ConstraintSetup.RemoveAt(FoundIdx);
		PA->MarkPackageDirty();
		PA->RefreshPhysicsAssetChange();
		bool bSaved = false;
		if (bSave) { bSaved = UEditorAssetLibrary::SaveLoadedAsset(PA, /*bOnlyIfIsDirty=*/false); }
		Result->SetStringField(TEXT("action"), TEXT("deleted"));
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPCommonUtils::CreateSuccessResponse(Result);
	}

	// Motion edit: any of swing1/swing2/twist (angular) and linear_x/y/z (linear).
	UPhysicsConstraintTemplate* CT = PA->ConstraintSetup[FoundIdx];
	FConstraintInstance& CI = CT->DefaultInstance;

	TSharedPtr<FJsonObject> Changes = MakeShareable(new FJsonObject);
	int32 NumChanged = 0;

	auto ApplyAngular = [&](const TCHAR* Key, EAngularConstraintMotion Cur, TFunctionRef<void(EAngularConstraintMotion)> Setter)
	{
		FString S;
		if (!Params->TryGetStringField(Key, S)) return true; // not provided — skip
		EAngularConstraintMotion M;
		if (!StringToAngularMotion(S, M)) return false;       // invalid value
		if (M != Cur)
		{
			Setter(M);
			++NumChanged;
		}
		TSharedPtr<FJsonObject> Pair = MakeShareable(new FJsonObject);
		Pair->SetStringField(TEXT("from"), AngularMotionToString(Cur));
		Pair->SetStringField(TEXT("to"),   AngularMotionToString(M));
		Changes->SetObjectField(Key, Pair);
		return true;
	};
	auto ApplyLinear = [&](const TCHAR* Key, ELinearConstraintMotion Cur, TFunctionRef<void(ELinearConstraintMotion)> Setter)
	{
		FString S;
		if (!Params->TryGetStringField(Key, S)) return true;
		ELinearConstraintMotion M;
		if (!StringToLinearMotion(S, M)) return false;
		if (M != Cur)
		{
			Setter(M);
			++NumChanged;
		}
		TSharedPtr<FJsonObject> Pair = MakeShareable(new FJsonObject);
		Pair->SetStringField(TEXT("from"), LinearMotionToString(Cur));
		Pair->SetStringField(TEXT("to"),   LinearMotionToString(M));
		Changes->SetObjectField(Key, Pair);
		return true;
	};

	PA->Modify();
	bool bOk = true;
	// Evaluate every axis (no short-circuit) so all provided edits apply and all
	// from/to pairs report, even if an earlier axis had an invalid value.
	bOk = ApplyAngular(TEXT("swing1"), CI.GetAngularSwing1Motion(), [&](EAngularConstraintMotion M){ CI.SetAngularSwing1Motion(M); }) && bOk;
	bOk = ApplyAngular(TEXT("swing2"), CI.GetAngularSwing2Motion(), [&](EAngularConstraintMotion M){ CI.SetAngularSwing2Motion(M); }) && bOk;
	bOk = ApplyAngular(TEXT("twist"),  CI.GetAngularTwistMotion(),  [&](EAngularConstraintMotion M){ CI.SetAngularTwistMotion(M); }) && bOk;
	bOk = ApplyLinear(TEXT("linear_x"), CI.GetLinearXMotion(), [&](ELinearConstraintMotion M){ CI.SetLinearXMotion(M); }) && bOk;
	bOk = ApplyLinear(TEXT("linear_y"), CI.GetLinearYMotion(), [&](ELinearConstraintMotion M){ CI.SetLinearYMotion(M); }) && bOk;
	bOk = ApplyLinear(TEXT("linear_z"), CI.GetLinearZMotion(), [&](ELinearConstraintMotion M){ CI.SetLinearZMotion(M); }) && bOk;

	if (!bOk)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid motion value (expected Free | Limited | Locked)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Each of `swing1`/`swing2`/`twist`/`linear_x`/`linear_y`/`linear_z` must be exactly one of: Free, Limited, Locked (case-insensitive)."));
	}

	bool bSaved = false;
	if (NumChanged > 0)
	{
		// Push DefaultInstance into the active constraint profile (the editor does
		// this after SetAngular*/SetLinear* edits), then refresh any live state.
		CT->UpdateProfileInstance();
		PA->MarkPackageDirty();
		PA->RefreshPhysicsAssetChange();
		if (bSave) { bSaved = UEditorAssetLibrary::SaveLoadedAsset(PA, /*bOnlyIfIsDirty=*/false); }
	}

	Result->SetStringField(TEXT("action"), TEXT("set_motion"));
	Result->SetStringField(TEXT("bone1"), CI.ConstraintBone1.ToString());
	Result->SetStringField(TEXT("bone2"), CI.ConstraintBone2.ToString());
	Result->SetStringField(TEXT("joint_name"), CI.JointName.ToString());
	Result->SetNumberField(TEXT("num_changed"), NumChanged);
	Result->SetObjectField(TEXT("changes"), Changes);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMCPCommonUtils::CreateSuccessResponse(Result);
}

// ─── set_skeletal_mesh_physics_asset ─────────────────────────────────────────
// Repoint a USkeletalMesh's PhysicsAsset (e.g. swap a third-party-pack copy for
// a project-local one). Pass physics_asset="" to clear the binding.

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleSetSkeletalMeshPhysicsAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath, Error, Hint;
	EMCPErrorCode Code = EMCPErrorCode::Internal;
	USkeletalMesh* Mesh = LoadSkeletalMeshFromParams(Params, MeshPath, Error, Code, Hint);
	if (!Mesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error, Code, Hint);
	}

	// physics_asset must be present; empty string explicitly clears the binding.
	if (!Params->HasField(TEXT("physics_asset")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: physics_asset"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`physics_asset` is required — the full /Game/... path to a UPhysicsAsset to bind, or \"\" (empty string) to clear the mesh's physics asset."));
	}
	const FString PAPath = Params->GetStringField(TEXT("physics_asset"));

	UPhysicsAsset* NewPA = nullptr;
	if (!PAPath.IsEmpty())
	{
		UObject* Loaded = UEditorAssetLibrary::LoadAsset(PAPath);
		NewPA = Cast<UPhysicsAsset>(Loaded);
		if (!NewPA)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("physics_asset failed to load as a UPhysicsAsset: %s"), *PAPath),
				EMCPErrorCode::AssetNotFound,
				TEXT("Verify the path resolves to a UPhysicsAsset. Use `list_assets` with `asset_type='PhysicsAsset'`. If PIE is running, asset loads fail — stop PIE and retry."));
		}
	}

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	const UPhysicsAsset* OldPA = Mesh->GetPhysicsAsset();
	Mesh->Modify();
	Mesh->SetPhysicsAsset(NewPA);
	Mesh->MarkPackageDirty();
	bool bSaved = false;
	if (bSave) { bSaved = UEditorAssetLibrary::SaveLoadedAsset(Mesh, /*bOnlyIfIsDirty=*/false); }

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("path"), MeshPath);
	Result->SetStringField(TEXT("old_physics_asset"), OldPA ? OldPA->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("new_physics_asset"), NewPA ? NewPA->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMCPCommonUtils::CreateSuccessResponse(Result);
}

// ─── merge_bones_into_skeleton ───────────────────────────────────────────────

#include "Animation/Skeleton.h"

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleMergeBonesIntoSkeleton(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This handler requires a params object. Pass a JSON object with at minimum the documented required fields for this tool."));
	}

	FString SourcePath, TargetPath, TargetParentBone;
	if (!Params->TryGetStringField(TEXT("source_skeleton"), SourcePath) ||
	    !Params->TryGetStringField(TEXT("target_skeleton"), TargetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing source_skeleton or target_skeleton"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Both `source_skeleton` and `target_skeleton` are required and must be full asset paths to USkeleton assets (e.g. `/Game/Characters/MyChar_Skeleton`). Use `list_skeletons` to discover."));
	}
	Params->TryGetStringField(TEXT("target_parent_bone"), TargetParentBone);

	const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("bones"), BonesArr) || !BonesArr || BonesArr->Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing or empty bones array"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`bones` is required and must be a non-empty JSON array of bone-name strings, e.g. `[\"spine_01\",\"clavicle_l\"]`. Use `inspect_skeletal_mesh` on the source skeleton to list its bones."));
	}
	TArray<FName> BonesToCopy;
	for (const TSharedPtr<FJsonValue>& V : *BonesArr)
	{
		if (V.IsValid() && V->Type == EJson::String)
		{
			BonesToCopy.Add(*V->AsString());
		}
	}

	USkeleton* SrcSkel = Cast<USkeleton>(UEditorAssetLibrary::LoadAsset(SourcePath));
	USkeleton* DstSkel = Cast<USkeleton>(UEditorAssetLibrary::LoadAsset(TargetPath));
	if (!SrcSkel)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Source skeleton not found: %s"), *SourcePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`source_skeleton` must be a full asset path to a USkeleton, e.g. `/Game/Characters/MyChar_Skeleton`. Use `list_skeletons` to discover."));
	}
	if (!DstSkel)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Target skeleton not found: %s"), *TargetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`target_skeleton` must be a full asset path to a USkeleton, e.g. `/Game/Characters/MyChar_Skeleton`. Use `list_skeletons` to discover."));
	}

	const FReferenceSkeleton& SrcRefSkel = SrcSkel->GetReferenceSkeleton();

	// Validate target_parent_bone exists in target before opening the modifier.
	int32 TargetParentIdx = 0; // default to root
	const FReferenceSkeleton& TargetRefRO = DstSkel->GetReferenceSkeleton();
	if (!TargetParentBone.IsEmpty())
	{
		TargetParentIdx = TargetRefRO.FindBoneIndex(*TargetParentBone);
		if (TargetParentIdx == INDEX_NONE)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("target_parent_bone '%s' not found in target skeleton"), *TargetParentBone),
				EMCPErrorCode::InvalidArgument,
				TEXT("`target_parent_bone` must name a bone that already exists in the target skeleton's reference skeleton. Bone names are case-sensitive. Omit `target_parent_bone` to attach merged bones at the root."));
		}
	}

	// Validate source bones up-front (idempotent skip if already in target).
	const TArray<FTransform>& SrcLocalTransforms = SrcRefSkel.GetRefBonePose();

	DstSkel->Modify();

	int32 BonesAdded = 0;
	TArray<FString> AddedNames;
	{
		// USkeleton overload writes through to the skeleton's internal ref skel.
		FReferenceSkeletonModifier Modifier(DstSkel);

		for (const FName& BoneName : BonesToCopy)
		{
			const int32 SrcIdx = SrcRefSkel.FindBoneIndex(BoneName);
			if (SrcIdx == INDEX_NONE)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Bone '%s' not found in source skeleton"), *BoneName.ToString()),
					EMCPErrorCode::InvalidArgument,
					TEXT("Every entry in `bones` must exist in the source skeleton's reference skeleton. Use `inspect_skeletal_mesh` (or list_skeleton_sockets-style introspection) on the source to verify bone names — they are case-sensitive."));
			}

			// Idempotent skip: bone already exists in target.
			const FReferenceSkeleton& LiveTarget = Modifier.GetReferenceSkeleton();
			if (LiveTarget.FindBoneIndex(BoneName) != INDEX_NONE)
			{
				continue;
			}

			// Parent in target: if source's parent is already in target, use that;
			// otherwise fall back to TargetParentIdx.
			const int32 SrcParentIdx = SrcRefSkel.GetParentIndex(SrcIdx);
			const FName SrcParentName = (SrcParentIdx != INDEX_NONE)
				? SrcRefSkel.GetBoneName(SrcParentIdx) : NAME_None;
			const int32 ExistingInTarget = (SrcParentName != NAME_None)
				? LiveTarget.FindBoneIndex(SrcParentName) : INDEX_NONE;
			const int32 TgtParentForThis =
				(ExistingInTarget != INDEX_NONE) ? ExistingInTarget : TargetParentIdx;

			FMeshBoneInfo Info;
			Info.Name = BoneName;
			Info.ExportName = BoneName.ToString();
			Info.ParentIndex = TgtParentForThis;
			const FTransform& LocalXform = SrcLocalTransforms[SrcIdx];
			Modifier.Add(Info, LocalXform);
			AddedNames.Add(BoneName.ToString());
			++BonesAdded;
		}
	} // ~Modifier finalizes & writes through to USkeleton

	// Refresh internal caches (clears bone-tree caches; HandleSkeletonHierarchyChange
	// is protected, but ClearCacheData is public and handles the same invalidations).
	DstSkel->ClearCacheData();
	DstSkel->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveAsset(TargetPath, /*bOnlyIfIsDirty=*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Merged %d bone(s) into the target skeleton in memory but the package failed to save to disk: %s"), BonesAdded, *TargetPath),
			EMCPErrorCode::Internal,
			TEXT("SaveAsset returned false — the merged bones will not survive an editor restart. Saving no-ops while PIE is running or when the package is read-only; stop PIE and ensure the asset is writable, then retry."));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("source_skeleton"), SourcePath);
	Result->SetStringField(TEXT("target_skeleton"), TargetPath);
	Result->SetNumberField(TEXT("bones_added"), BonesAdded);
	TArray<TSharedPtr<FJsonValue>> AddedJson;
	for (const FString& N : AddedNames) AddedJson.Add(MakeShareable(new FJsonValueString(N)));
	Result->SetArrayField(TEXT("added_bones"), AddedJson);
	return FMCPCommonUtils::CreateSuccessResponse(Result);
}

// ─── merge_bones_into_skeletal_mesh ─────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleMergeBonesIntoSkeletalMesh(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This handler requires a params object. Pass a JSON object with at minimum the documented required fields for this tool."));
	}

	FString SourcePath, TargetMeshPath, TargetParentBone;
	if (!Params->TryGetStringField(TEXT("source_skeleton"), SourcePath) ||
	    !Params->TryGetStringField(TEXT("target_mesh"), TargetMeshPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing source_skeleton or target_mesh"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`source_skeleton` (USkeleton asset path) and `target_mesh` (USkeletalMesh asset path) are both required. Use `list_skeletons` and `list_assets` with `asset_type='SkeletalMesh'` to discover."));
	}
	Params->TryGetStringField(TEXT("target_parent_bone"), TargetParentBone);

	const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("bones"), BonesArr) || !BonesArr || BonesArr->Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing or empty bones array"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`bones` is required and must be a non-empty JSON array of bone-name strings, e.g. `[\"spine_01\",\"clavicle_l\"]`. Use `inspect_skeletal_mesh` on the source skeleton to list its bones."));
	}
	TArray<FName> BonesToCopy;
	for (const TSharedPtr<FJsonValue>& V : *BonesArr)
	{
		if (V.IsValid() && V->Type == EJson::String) BonesToCopy.Add(*V->AsString());
	}

	USkeleton* SrcSkel = Cast<USkeleton>(UEditorAssetLibrary::LoadAsset(SourcePath));
	USkeletalMesh* DstMesh = Cast<USkeletalMesh>(UEditorAssetLibrary::LoadAsset(TargetMeshPath));
	if (!SrcSkel)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Source skeleton not found: %s"), *SourcePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`source_skeleton` must be a full asset path to a USkeleton, e.g. `/Game/Characters/MyChar_Skeleton`. Use `list_skeletons` to discover."));
	}
	if (!DstMesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Target skeletal mesh not found: %s"), *TargetMeshPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`target_mesh` must be a full asset path to a USkeletalMesh, e.g. `/Game/Characters/MyChar_SK`. Use `list_assets` with `asset_type='SkeletalMesh'` to discover."));
	}

	const FReferenceSkeleton& SrcRefSkel = SrcSkel->GetReferenceSkeleton();
	const TArray<FTransform>& SrcLocalTransforms = SrcRefSkel.GetRefBonePose();

	int32 TargetParentIdx = 0;
	if (!TargetParentBone.IsEmpty())
	{
		TargetParentIdx = DstMesh->GetRefSkeleton().FindBoneIndex(*TargetParentBone);
		if (TargetParentIdx == INDEX_NONE)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("target_parent_bone '%s' not found in target mesh ref skeleton"), *TargetParentBone),
				EMCPErrorCode::InvalidArgument,
				TEXT("`target_parent_bone` must name a bone that already exists in the target mesh's reference skeleton (not the bound USkeleton). Use `inspect_skeletal_mesh` and look at `bones[*].name` to verify. Omit `target_parent_bone` to attach merged bones at the root."));
		}
	}

	DstMesh->Modify();

	int32 BonesAdded = 0;
	TArray<FString> AddedNames;
	{
		// Modify the MESH'S ref skeleton (the one that governs skinning).
		FReferenceSkeleton& MeshRefSkel = DstMesh->GetRefSkeleton();
		FReferenceSkeletonModifier Modifier(MeshRefSkel, DstMesh->GetSkeleton());

		for (const FName& BoneName : BonesToCopy)
		{
			const int32 SrcIdx = SrcRefSkel.FindBoneIndex(BoneName);
			if (SrcIdx == INDEX_NONE)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Bone '%s' not found in source skeleton"), *BoneName.ToString()),
					EMCPErrorCode::InvalidArgument,
					TEXT("Every entry in `bones` must exist in the source skeleton's reference skeleton. Use `inspect_skeletal_mesh` (or list_skeleton_sockets-style introspection) on the source to verify bone names — they are case-sensitive."));
			}

			const FReferenceSkeleton& LiveTarget = Modifier.GetReferenceSkeleton();
			if (LiveTarget.FindBoneIndex(BoneName) != INDEX_NONE) continue;

			const int32 SrcParentIdx = SrcRefSkel.GetParentIndex(SrcIdx);
			const FName SrcParentName = (SrcParentIdx != INDEX_NONE)
				? SrcRefSkel.GetBoneName(SrcParentIdx) : NAME_None;
			const int32 ExistingInTarget = (SrcParentName != NAME_None)
				? LiveTarget.FindBoneIndex(SrcParentName) : INDEX_NONE;
			const int32 TgtParentForThis =
				(ExistingInTarget != INDEX_NONE) ? ExistingInTarget : TargetParentIdx;

			FMeshBoneInfo Info;
			Info.Name = BoneName;
			Info.ExportName = BoneName.ToString();
			Info.ParentIndex = TgtParentForThis;
			Modifier.Add(Info, SrcLocalTransforms[SrcIdx]);
			AddedNames.Add(BoneName.ToString());
			++BonesAdded;
		}
	}

	// Sync new bones into the bound skeleton so it stays compatible.
	if (USkeleton* BoundSkel = DstMesh->GetSkeleton())
	{
		BoundSkel->Modify();
		BoundSkel->MergeAllBonesToBoneTree(DstMesh);
		if (!UEditorAssetLibrary::SaveAsset(BoundSkel->GetPathName(), false))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Merged bones into the target mesh, but the bound skeleton package failed to save: %s"), *BoundSkel->GetPathName()),
				EMCPErrorCode::Internal,
				TEXT("SaveAsset returned false for the bound skeleton — saving no-ops while PIE is running or when the package is read-only; stop PIE and ensure the asset is writable, then retry."));
		}
	}

	DstMesh->PostEditChange();
	DstMesh->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveAsset(TargetMeshPath, false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Merged %d bone(s) into the target mesh in memory but the package failed to save to disk: %s"), BonesAdded, *TargetMeshPath),
			EMCPErrorCode::Internal,
			TEXT("SaveAsset returned false — the merged bones will not survive an editor restart. Saving no-ops while PIE is running or when the package is read-only; stop PIE and ensure the asset is writable, then retry."));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("source_skeleton"), SourcePath);
	Result->SetStringField(TEXT("target_mesh"), TargetMeshPath);
	Result->SetNumberField(TEXT("bones_added"), BonesAdded);
	TArray<TSharedPtr<FJsonValue>> AddedJson;
	for (const FString& N : AddedNames) AddedJson.Add(MakeShareable(new FJsonValueString(N)));
	Result->SetArrayField(TEXT("added_bones"), AddedJson);
	return FMCPCommonUtils::CreateSuccessResponse(Result);
}

// ─── skeletal_mesh_build_bend_chain ──────────────────────────────────────────
// (Re)builds a Root→tip bone chain up one axis of a procedurally-authored skeletal
// mesh and PROCEDURALLY skins every vertex to that chain by position along the axis,
// with smooth two-bone blends across each joint — so the mesh can bend. Built for the
// fishing rod: the tapered pole was baked static→skeletal with only a Root bone, so it
// is rigid; this inserts N pole bones whose SEGMENT LENGTHS DECREASE toward the tip
// (stiff butt, finer/whippier tip — the "decreasingly flexible" feel) and re-skins.
//
// The whole build reuses the engine's own static→skeletal path:
// FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions, which
// sets the ref skeleton, computes inverse-ref matrices, runs IMeshUtilities::
// BuildSkeletalMesh, and rebuilds render data inside an FScopedSkeletalMeshPostEditChange.
// We clone LOD0's mesh description (geometry survives), rewrite ONLY its skin weights by
// axis position, hand it back with a freshly-built reference skeleton, then sync the new
// bones into the bound USkeleton (MergeAllBonesToBoneTree — additive, preserves sockets).
//
// Idempotent: it rebuilds the ref skeleton + skin from the mesh geometry each call, so
// re-running with a different num_bones just reskins from scratch. (Re-running with FEWER
// bones leaves the higher-index bones on the bound skeleton, since the skeleton sync is
// additive; harmless — they carry no skin weight.)

#include "StaticToSkeletalMeshConverter.h"
#include "SkeletalMeshAttributes.h"
#include "SkinWeightsAttributesRef.h"
#include "BoneWeights.h"
#include "MeshDescription.h"

TSharedPtr<FJsonObject> FMCPSkeletalMeshCommands::HandleBuildBendChain(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error, Hint;
	EMCPErrorCode Code = EMCPErrorCode::InvalidArgument;
	USkeletalMesh* Mesh = LoadSkeletalMeshFromParams(Params, Path, Error, Code, Hint);
	if (!Mesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error, Code, Hint);
	}

	// ── Params ──
	int32 NumBones = 15;
	Params->TryGetNumberField(TEXT("num_bones"), NumBones);
	NumBones = FMath::Clamp(NumBones, 1, 64);

	double BaseFraction = 0.14;   // fraction of the length that stays rigid on Root (the grip)
	Params->TryGetNumberField(TEXT("base_fraction"), BaseFraction);
	BaseFraction = FMath::Clamp(BaseFraction, 0.0, 0.9);

	double SegmentRatio = 0.85;   // each flex segment = ratio × the previous (<1 → shorter toward the tip)
	Params->TryGetNumberField(TEXT("segment_ratio"), SegmentRatio);
	SegmentRatio = FMath::Clamp(SegmentRatio, 0.2, 2.0);

	FString Axis = TEXT("z");
	Params->TryGetStringField(TEXT("axis"), Axis);
	Axis = Axis.ToLower();
	const int32 AxisIdx = (Axis == TEXT("x")) ? 0 : (Axis == TEXT("y")) ? 1 : 2;

	FString BonePrefix = TEXT("pole_");
	Params->TryGetStringField(TEXT("bone_prefix"), BonePrefix);
	FString RootBoneName = TEXT("Root");
	Params->TryGetStringField(TEXT("root_bone_name"), RootBoneName);

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	const int32 LODIndex = 0;
	if (!Mesh->HasMeshDescription(LODIndex))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Skeletal mesh '%s' has no source mesh description on LOD0"), *Path),
			EMCPErrorCode::UnsupportedClass,
			TEXT("This tool rebuilds the mesh from its LOD0 source mesh description (the geometry must be present as editable source). Meshes with only cooked render data and no source can't be re-skinned this way. The fishing rod baked via static→skeletal conversion has a source description."));
	}

	// Clone LOD0 geometry (positions/UVs/normals survive; we only rewrite skin weights).
	FMeshDescription MeshDesc;
	if (!Mesh->CloneMeshDescription(LODIndex, MeshDesc))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to clone LOD0 mesh description for '%s'"), *Path),
			EMCPErrorCode::Internal,
			TEXT("CloneMeshDescription returned false — the LOD may be auto-generated rather than source-imported."));
	}

	FSkeletalMeshAttributes Attributes(MeshDesc);
	Attributes.Register(/*bKeepExistingAttribute=*/true);

	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();

	// ── Bounds along the chosen axis ──
	double AxisMin = TNumericLimits<double>::Max();
	double AxisMax = TNumericLimits<double>::Lowest();
	int32 VertexCount = 0;
	for (const FVertexID VertexID : MeshDesc.Vertices().GetElementIDs())
	{
		const double C = static_cast<double>(Positions.Get(VertexID)[AxisIdx]);
		AxisMin = FMath::Min(AxisMin, C);
		AxisMax = FMath::Max(AxisMax, C);
		++VertexCount;
	}
	const double Length = AxisMax - AxisMin;
	if (Length <= KINDA_SMALL_NUMBER || VertexCount == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Degenerate extent along axis '%s' (length %.4f, %d verts)"), *Axis, Length, VertexCount),
			EMCPErrorCode::InvalidArgument,
			TEXT("The mesh has no measurable length along the requested axis. Verify `axis` (x/y/z) matches the rod's long axis — the fishing rod runs along +Z."));
	}

	// ── Station positions along the axis (component space) ──
	// Station[0] = AxisMin (Root, the butt). The flex region [GripTop, AxisMax] is split into
	// NumBones segments whose lengths form a geometric series with ratio SegmentRatio, so each
	// segment toward the tip is shorter → finer subdivision (whippier tip).
	const double GripTop = AxisMin + BaseFraction * Length;
	const double FlexLen = AxisMax - GripTop;
	const double R = SegmentRatio;

	double BaseSeg;
	if (FMath::IsNearlyEqual(R, 1.0))
	{
		BaseSeg = FlexLen / static_cast<double>(NumBones);
	}
	else
	{
		BaseSeg = FlexLen * (1.0 - R) / (1.0 - FMath::Pow(R, static_cast<double>(NumBones)));
	}

	TArray<double> StationPos;   // StationPos[0..NumBones]
	StationPos.SetNum(NumBones + 1);
	StationPos[0] = AxisMin;
	{
		double Cumulative = 0.0;
		for (int32 k = 1; k <= NumBones; ++k)
		{
			Cumulative += BaseSeg * FMath::Pow(R, static_cast<double>(k - 1));
			StationPos[k] = GripTop + Cumulative;
		}
		StationPos[NumBones] = AxisMax;   // pin the tip bone exactly at the end
	}

	// Bone names: Root + pole_01..pole_NN.
	TArray<FString> BoneNames;
	BoneNames.Reserve(NumBones + 1);
	BoneNames.Add(RootBoneName);
	for (int32 k = 1; k <= NumBones; ++k)
	{
		BoneNames.Add(FString::Printf(TEXT("%s%02d"), *BonePrefix, k));
	}

	// Bone names must be unique — FReferenceSkeleton::Add() asserts (hard editor crash)
	// on a duplicate name. A caller-supplied root_bone_name can collide with a generated
	// `<bone_prefix>NN` pole name (e.g. root_bone_name="pole_01"). Reject before building.
	{
		TSet<FString> SeenBoneNames;
		for (const FString& BN : BoneNames)
		{
			bool bAlreadyPresent = false;
			SeenBoneNames.Add(BN, &bAlreadyPresent);
			if (bAlreadyPresent)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Duplicate bone name '%s' (root_bone_name collides with a generated pole name)"), *BN),
					EMCPErrorCode::InvalidArgument,
					TEXT("`root_bone_name` must not collide with the generated `<bone_prefix>NN` pole names. Choose a distinct `root_bone_name` or `bone_prefix`."));
			}
		}
	}

	auto AxisVector = [AxisIdx](double V) -> FVector
	{
		FVector Out = FVector::ZeroVector;
		Out[AxisIdx] = V;
		return Out;
	};

	// ── Build the station-table JSON (shared by dry-run + the success payload) ──
	auto BuildStationJson = [&]() -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (int32 k = 0; k <= NumBones; ++k)
		{
			TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
			O->SetNumberField(TEXT("index"), k);
			O->SetStringField(TEXT("name"), BoneNames[k]);
			O->SetNumberField(TEXT("parent_index"), k == 0 ? -1 : k - 1);
			O->SetNumberField(TEXT("axis_pos"), StationPos[k]);
			O->SetNumberField(TEXT("segment_length"), k == 0 ? 0.0 : (StationPos[k] - StationPos[k - 1]));
			Arr.Add(MakeShareable(new FJsonValueObject(O)));
		}
		return Arr;
	};

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
		Result->SetStringField(TEXT("path"), Path);
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetStringField(TEXT("axis"), Axis);
		Result->SetNumberField(TEXT("axis_min"), AxisMin);
		Result->SetNumberField(TEXT("axis_max"), AxisMax);
		Result->SetNumberField(TEXT("grip_top"), GripTop);
		Result->SetNumberField(TEXT("num_pole_bones"), NumBones);
		Result->SetNumberField(TEXT("vertex_count"), VertexCount);
		Result->SetArrayField(TEXT("bones"), BuildStationJson());
		return FMCPCommonUtils::CreateSuccessResponse(Result);
	}

	// ── Build the new reference skeleton (Root at origin, poles up the axis) ──
	// Root stays at component origin (identity) to preserve the existing socket frame
	// (GripAttach_L/R live on Root in the bound skeleton). Each pole's LOCAL translation is
	// its segment length along the axis, so component-space pole k sits at StationPos[k].
	USkeleton* BoundSkel = Mesh->GetSkeleton();
	FReferenceSkeleton NewRefSkel;
	{
		FReferenceSkeletonModifier Modifier(NewRefSkel, BoundSkel);
		Modifier.Add(FMeshBoneInfo(*RootBoneName, RootBoneName, INDEX_NONE), FTransform::Identity);
		for (int32 k = 1; k <= NumBones; ++k)
		{
			const FVector LocalT = AxisVector(StationPos[k] - StationPos[k - 1]);
			Modifier.Add(FMeshBoneInfo(*BoneNames[k], BoneNames[k], k - 1), FTransform(LocalT));
		}
	}

	// ── Re-skin: weight each vertex by its axis position, blended across the bracketing
	// stations (smooth two-bone joints). Below Station[0] → all Root; above Station[N] → all tip. ──
	{
		using namespace UE::AnimationCore;
		FSkinWeightsVertexAttributesRef SkinWeights = Attributes.GetVertexSkinWeights();
		for (const FVertexID VertexID : MeshDesc.Vertices().GetElementIDs())
		{
			const double Z = static_cast<double>(Positions.Get(VertexID)[AxisIdx]);

			TArray<FBoneWeight, TInlineAllocator<2>> Influences;
			if (Z <= StationPos[0])
			{
				Influences.Add(FBoneWeight(0, 1.0f));
			}
			else if (Z >= StationPos[NumBones])
			{
				Influences.Add(FBoneWeight(static_cast<FBoneIndexType>(NumBones), 1.0f));
			}
			else
			{
				int32 k = 0;
				while (k < NumBones && Z > StationPos[k + 1])
				{
					++k;
				}
				const double Span = StationPos[k + 1] - StationPos[k];
				const float T = (Span > KINDA_SMALL_NUMBER)
					? static_cast<float>((Z - StationPos[k]) / Span) : 0.f;
				Influences.Add(FBoneWeight(static_cast<FBoneIndexType>(k), 1.0f - T));
				Influences.Add(FBoneWeight(static_cast<FBoneIndexType>(k + 1), T));
			}
			// FBoneWeightsSettings default normalizes (and drops zero influences).
			SkinWeights.Set(VertexID, Influences);
		}
	}

	// ── Rebuild the mesh from the modified description + new ref skeleton ──
	Mesh->Modify();
	Mesh->GetImportedModel()->LODModels.Empty();   // InitializeSkeletalMeshFromMeshDescriptions requires an empty mesh
	Mesh->ResetLODInfo();

	// Build a materials list with ONE distinctly-named slot per polygon group, and stamp
	// those names onto the polygon groups themselves. The converter maps polygon groups to
	// material slots by name; identical or None names (this mesh was baked/converted with 0
	// real material slots — the runtime assigns materials on the component, not the asset)
	// make it index an empty array (crash) or merge the rod's two sections (blank + grip)
	// into one. Distinct names preserve the section split, and writing them back onto the
	// groups makes this idempotent regardless of what slot state a prior run left behind.
	// Existing material interfaces (if any) are carried over by group index.
	const TArray<FSkeletalMaterial> ExistingMaterials = Mesh->GetMaterials();
	TArray<FSkeletalMaterial> Materials;
	{
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		int32 GroupIndex = 0;
		TSet<FName> UsedNames;
		for (const FPolygonGroupID PolygonGroupID : MeshDesc.PolygonGroups().GetElementIDs())
		{
			FName SlotName = SlotNames.IsValid() ? SlotNames.Get(PolygonGroupID) : NAME_None;
			if (SlotName.IsNone() || UsedNames.Contains(SlotName))
			{
				SlotName = FName(*FString::Printf(TEXT("Material_%d"), GroupIndex));
			}
			UsedNames.Add(SlotName);
			if (SlotNames.IsValid())
			{
				SlotNames.Set(PolygonGroupID, SlotName);   // valid + distinct → converter skips its merge-prone fixup
			}
			UMaterialInterface* MatIface = ExistingMaterials.IsValidIndex(GroupIndex)
				? ExistingMaterials[GroupIndex].MaterialInterface : nullptr;
			Materials.Add(FSkeletalMaterial(MatIface, SlotName, SlotName));
			++GroupIndex;
		}
		if (Materials.Num() == 0)
		{
			const FName Fallback(TEXT("Material_0"));
			Materials.Add(FSkeletalMaterial(nullptr, Fallback, Fallback));
		}
	}

	TArray<const FMeshDescription*> Descriptions;
	Descriptions.Add(&MeshDesc);

	const bool bBuilt = FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions(
		Mesh, Descriptions, Materials, NewRefSkel,
		/*bInRecomputeNormals=*/false, /*bInRecomputeTangents=*/true, /*bCacheOptimize=*/true);
	if (!bBuilt)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Skeletal mesh rebuild failed for '%s' (see MCP_Unified.log for build warnings)"), *Path),
			EMCPErrorCode::Internal,
			TEXT("InitializeSkeletalMeshFromMeshDescriptions returned false. Common causes: a skin weight referencing a non-existent bone, or non-manifold geometry. Check the unified log for LogStaticToSkeletalMeshConverter warnings."));
	}

	// ── Sync the new bones into the bound USkeleton (additive; preserves existing sockets) ──
	if (BoundSkel)
	{
		BoundSkel->Modify();
		BoundSkel->MergeAllBonesToBoneTree(Mesh);
	}

	Mesh->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveLoadedAsset(Mesh, /*bOnlyIfIsDirty=*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Rebuilt the bend chain in memory but the skeletal mesh package failed to save to disk: %s"), *Path),
			EMCPErrorCode::Internal,
			TEXT("SaveLoadedAsset returned false — the re-skinned mesh will not survive an editor restart. Saving no-ops while PIE is running or when the package is read-only; stop PIE and ensure the asset is writable, then retry."));
	}
	if (BoundSkel && !UEditorAssetLibrary::SaveLoadedAsset(BoundSkel, /*bOnlyIfIsDirty=*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Re-skinned mesh saved, but the bound skeleton package failed to save: %s"), *BoundSkel->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("SaveLoadedAsset returned false for the bound skeleton — the new bones may not survive a restart. Stop PIE and ensure the skeleton asset is writable, then retry."));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("skeleton"), BoundSkel ? BoundSkel->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("axis"), Axis);
	Result->SetNumberField(TEXT("axis_min"), AxisMin);
	Result->SetNumberField(TEXT("axis_max"), AxisMax);
	Result->SetNumberField(TEXT("grip_top"), GripTop);
	Result->SetNumberField(TEXT("num_pole_bones"), NumBones);
	Result->SetNumberField(TEXT("total_bones"), NumBones + 1);
	Result->SetNumberField(TEXT("vertex_count"), VertexCount);
	Result->SetArrayField(TEXT("bones"), BuildStationJson());
	return FMCPCommonUtils::CreateSuccessResponse(Result);
}



