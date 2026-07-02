// Static-mesh asset commands for the MCP. Implementation split across .cpp files:
//   - MCPMeshCommands.cpp            (this file): command dispatch +
//                                              bake_dynamic_mesh_to_static_mesh
//                                              (Modeling Mode → Bake → Static Mesh).
//   - MCPMeshCommands_Collision.cpp: set_static_mesh_collision /
//                                              get_static_mesh_collision (author +
//                                              read simple/convex collision).

#include "Commands/MCPMeshCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Components/DynamicMeshComponent.h"
#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/GeometryScriptTypes.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMeshSocket.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"

#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

FMCPMeshCommands::FMCPMeshCommands()
{
}

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("asset_bake_dynamic_to_static_mesh"))
	{
		return HandleBakeDynamicMeshToStaticMesh(Params);
	}
	if (CommandType == TEXT("mesh_set_collision"))
	{
		return HandleSetStaticMeshCollision(Params);
	}
	if (CommandType == TEXT("mesh_get_collision"))
	{
		return HandleGetStaticMeshCollision(Params);
	}
	if (CommandType == TEXT("mesh_list_sockets"))
	{
		return HandleListStaticMeshSockets(Params);
	}
	if (CommandType == TEXT("mesh_get_bounds"))
	{
		return HandleGetStaticMeshBounds(Params);
	}
	if (CommandType == TEXT("mesh_add_socket"))
	{
		return HandleAddStaticMeshSocket(Params);
	}
	if (CommandType == TEXT("mesh_modify_socket"))
	{
		return HandleModifyStaticMeshSocket(Params);
	}
	if (CommandType == TEXT("mesh_remove_socket"))
	{
		return HandleRemoveStaticMeshSocket(Params);
	}
	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown mesh command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: asset_bake_dynamic_to_static_mesh, mesh_set_collision, mesh_get_collision, mesh_list_sockets, mesh_get_bounds, mesh_add_socket, mesh_modify_socket, mesh_remove_socket."));
}

// ─── helpers ─────────────────────────────────────────────────────────────────

namespace
{
	AActor* FindActorByLabelOrName(UWorld* World, const FString& Name)
	{
		if (!World) return nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A) continue;
			if (A->GetActorLabel() == Name) return A;
			if (A->GetName() == Name)       return A;
		}
		return nullptr;
	}

	/** Collect every UDynamicMeshComponent on the actor. */
	void GatherDynamicMeshComponents(AActor* Actor, TArray<UDynamicMeshComponent*>& Out)
	{
		Out.Reset();
		if (!Actor) return;
		Actor->GetComponents<UDynamicMeshComponent>(Out);
	}

	/** Map a string param to a TEnumAsByte<ECollisionTraceFlag>. */
	bool ParseCollisionTraceFlag(const FString& In, TEnumAsByte<ECollisionTraceFlag>& Out, FString& OutError)
	{
		const FString Lower = In.ToLower();
		if (Lower == TEXT("default"))                  { Out = CTF_UseDefault;            return true; }
		if (Lower == TEXT("simple_and_complex"))       { Out = CTF_UseSimpleAndComplex;   return true; }
		if (Lower == TEXT("simple_as_complex"))        { Out = CTF_UseSimpleAsComplex;    return true; }
		if (Lower == TEXT("complex_as_simple"))        { Out = CTF_UseComplexAsSimple;    return true; }
		if (Lower == TEXT("use_complex_collision"))    { Out = CTF_UseComplexAsSimple;    return true; }
		OutError = FString::Printf(TEXT("Unknown collision_trace_flag: '%s' (expected default | simple_and_complex | simple_as_complex | complex_as_simple | use_complex_collision)"), *In);
		return false;
	}

}

// NOTE: mesh_list_sockets is implemented (unified asset-path + actor/component path) in
// MCPMeshCommands_Sockets.cpp alongside the socket-authoring family.

// ─── bake_dynamic_mesh_to_static_mesh ───────────────────────────────────────

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleBakeDynamicMeshToStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This handler requires a params object with at minimum `actor_name` (string) and `target_asset_path` (`/Game/...` path)."));
	}

	// ── Required params ──
	FString ActorName, TargetAssetPath;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: actor_name"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`actor_name` is required (string, non-empty). Pass the actor's label (display name) or short name. The source actor must own at least one UDynamicMeshComponent. Use `get_actors_in_level` to discover."));
	}
	if (!Params->TryGetStringField(TEXT("target_asset_path"), TargetAssetPath) || TargetAssetPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: target_asset_path"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target_asset_path` is required (string, non-empty) and must be a `/Game/...` content-root path including the asset name, e.g. `/Game/Meshes/SM_BakedFloor`. The folder is created if missing. Pass `force_overwrite: true` to replace an existing asset."));
	}
	if (!TargetAssetPath.StartsWith(TEXT("/Game/")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("target_asset_path must start with /Game/"),
			EMCPErrorCode::InvalidPath,
			TEXT("`target_asset_path` must be a `/Game/...` content-root path. Filesystem paths (`C:/...`), engine content (`/Engine/`), or plugin content (`/<PluginName>/`) are not accepted — the bake target must live under the project's content folder."));
	}
	if (TargetAssetPath.Len() > 1024)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("target_asset_path is too long"),
			EMCPErrorCode::InvalidPath,
			TEXT("`target_asset_path` exceeds the engine FName limit (NAME_SIZE = 1024). CreatePackage and NewObject build an FName from this path, and FName fatally asserts above 1024 characters. Use a shorter `/Game/...` path."));
	}

	// ── Optional params ──
	const FString ComponentName        = Params->HasField(TEXT("component_name"))        ? Params->GetStringField(TEXT("component_name"))        : FString();
	const bool    bForceOverwrite      = Params->HasField(TEXT("force_overwrite"))       ? Params->GetBoolField  (TEXT("force_overwrite"))       : false;
	const FString CollisionFlagStr     = Params->HasField(TEXT("collision_trace_flag"))  ? Params->GetStringField(TEXT("collision_trace_flag"))  : TEXT("simple_as_complex");
	const bool    bRecomputeNormals    = Params->HasField(TEXT("recompute_normals"))     ? Params->GetBoolField  (TEXT("recompute_normals"))     : false;
	const bool    bRecomputeTangents   = Params->HasField(TEXT("recompute_tangents"))    ? Params->GetBoolField  (TEXT("recompute_tangents"))    : true;
	const bool    bEnableNanite        = Params->HasField(TEXT("enable_nanite"))         ? Params->GetBoolField  (TEXT("enable_nanite"))         : false;
	const bool    bReplaceSourceActor  = Params->HasField(TEXT("replace_source_actor"))  ? Params->GetBoolField  (TEXT("replace_source_actor"))  : false;

	TArray<FString> MaterialPaths;
	if (Params->HasField(TEXT("material_paths")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(TEXT("material_paths"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				if (V.IsValid() && V->Type == EJson::String)
				{
					MaterialPaths.Add(V->AsString());
				}
			}
		}
	}

	TEnumAsByte<ECollisionTraceFlag> CollisionTraceFlag = CTF_UseSimpleAsComplex;
	{
		FString CollisionErr;
		if (!ParseCollisionTraceFlag(CollisionFlagStr, CollisionTraceFlag, CollisionErr))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				CollisionErr,
				EMCPErrorCode::InvalidArgument,
				TEXT("`collision_trace_flag` must be one of: `default` (CTF_UseDefault), `simple_as_complex` (CTF_UseSimpleAsComplex — the default if omitted), `complex_as_simple` / `use_complex_collision` (CTF_UseComplexAsSimple). Match is case-insensitive."));
		}
	}

	// ── Resolve the editor world + actor + DMC ──
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No editor world available"),
			EMCPErrorCode::Internal,
			TEXT("Could not resolve the editor world (GEditor null or its world context's World() returned null). The editor is mid-startup or shutdown. Retry once the editor is fully loaded."));
	}

	AActor* SourceActor = FindActorByLabelOrName(World, ActorName);
	if (!SourceActor)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor not found: %s"), *ActorName),
			EMCPErrorCode::ActorNotFound,
			TEXT("No actor in the editor world matches `actor_name` by ActorLabel or UObject name. Names are case-sensitive. Use `get_actors_in_level` or `find_actors_by_name` to discover."));
	}

	TArray<UDynamicMeshComponent*> DMCs;
	GatherDynamicMeshComponents(SourceActor, DMCs);
	if (DMCs.Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor %s has no UDynamicMeshComponent"), *ActorName),
			EMCPErrorCode::InvalidArgument,
			TEXT("The resolved actor has no UDynamicMeshComponent — bake_dynamic_mesh_to_static_mesh requires a Modeling Mode dynamic-mesh source. The source actor is typically a Geometry Script generated actor or a Modeling Mode primitive — UStaticMeshComponent / USkeletalMeshComponent actors are NOT supported."));
	}

	UDynamicMeshComponent* SourceDMC = nullptr;
	if (!ComponentName.IsEmpty())
	{
		for (UDynamicMeshComponent* C : DMCs)
		{
			if (C && C->GetName() == ComponentName)
			{
				SourceDMC = C;
				break;
			}
		}
		if (!SourceDMC)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Component '%s' not found on actor '%s' (or is not a UDynamicMeshComponent)"), *ComponentName, *ActorName),
				EMCPErrorCode::InvalidArgument,
				TEXT("`component_name` did not match any UDynamicMeshComponent on the resolved actor. Component names match `UObject::GetName()` and are case-sensitive. Omit `component_name` if the actor has a single dynamic-mesh component to use it automatically."));
		}
	}
	else if (DMCs.Num() == 1)
	{
		SourceDMC = DMCs[0];
	}
	else
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor %s has %d UDynamicMeshComponents — specify component_name to disambiguate"), *ActorName, DMCs.Num()),
			EMCPErrorCode::AmbiguousTarget,
			TEXT("The actor owns multiple UDynamicMeshComponents and `component_name` is required to disambiguate. Pass the desired component's `GetName()` value — typically the variable name from the BP or a default like `DynamicMesh_0`. Use `get_actors_in_level` with component introspection to enumerate."));
	}

	UDynamicMesh* SourceMesh = SourceDMC->GetDynamicMesh();
	if (!SourceMesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("DynamicMeshComponent on %s has no UDynamicMesh"), *ActorName),
			EMCPErrorCode::Internal,
			TEXT("UDynamicMeshComponent::GetDynamicMesh() returned null — every dynamic-mesh component should have a UDynamicMesh by construction. This indicates the component is in an invalid state, typically from a partial CDO or deserialization failure. Re-spawn the actor or open it in the editor and recompile."));
	}

	// Sanity check: bail if the live mesh has no triangles. ProcessMesh runs a
	// reader lambda against the underlying FDynamicMesh3 — read-only, no copy.
	int32 SourceTriangleCount = 0;
	int32 SourceVertexCount   = 0;
	SourceMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& M)
	{
		SourceTriangleCount = M.TriangleCount();
		SourceVertexCount   = M.VertexCount();
	});
	if (SourceTriangleCount == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("DynamicMesh has no geometry — nothing to bake"),
			EMCPErrorCode::InvalidArgument,
			TEXT("The source UDynamicMesh has zero triangles. Bake requires actual geometry. Verify the source actor's mesh has been built (Geometry Script populates triangles via `Append*` / `Generate*` calls; Modeling Mode generates from interactive operations). Take a screenshot of the editor viewport to confirm visible geometry before baking."));
	}

	// ── Existing-asset handling ──
	if (UEditorAssetLibrary::DoesAssetExist(TargetAssetPath))
	{
		if (!bForceOverwrite)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Asset already exists at %s — pass force_overwrite=true to replace"), *TargetAssetPath),
				EMCPErrorCode::NameCollision,
				TEXT("An asset already exists at the target path. To replace it, pass `force_overwrite: true` (the existing asset is deleted first). To keep it, pick a different `target_asset_path`. Use `read_static_mesh` or `list_assets` to inspect the existing asset before overwriting."));
		}
		if (!UEditorAssetLibrary::DeleteAsset(TargetAssetPath))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("force_overwrite was true but DeleteAsset(%s) failed"), *TargetAssetPath),
				EMCPErrorCode::Internal,
				TEXT("UEditorAssetLibrary::DeleteAsset returned false. Common causes: (1) the existing asset is referenced by other assets — use `fixup_redirectors` or check references first; (2) source control has the file checked out by someone else; (3) the asset file is locked by another process. Resolve the dependency or lock, then retry."));
		}
	}

	// Ensure the parent folder exists. MakeDirectory is a no-op if already present.
	const FString ParentFolder = FPaths::GetPath(TargetAssetPath);
	if (!ParentFolder.IsEmpty() && !UEditorAssetLibrary::DoesDirectoryExist(ParentFolder))
	{
		UEditorAssetLibrary::MakeDirectory(ParentFolder);
	}

	// ── Resolve material overrides (if any) ──
	TArray<TObjectPtr<UMaterialInterface>> NewMaterials;
	if (MaterialPaths.Num() > 0)
	{
		NewMaterials.Reserve(MaterialPaths.Num());
		for (int32 i = 0; i < MaterialPaths.Num(); ++i)
		{
			const FString& MatPath = MaterialPaths[i];
			if (MatPath.IsEmpty())
			{
				NewMaterials.Add(nullptr);
				continue;
			}
			UObject* Loaded = UEditorAssetLibrary::LoadAsset(MatPath);
			UMaterialInterface* Mat = Cast<UMaterialInterface>(Loaded);
			if (!Mat)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("material_paths[%d] failed to load as UMaterialInterface: %s"), i, *MatPath),
					EMCPErrorCode::AssetNotFound,
					TEXT("An entry in `material_paths` did not resolve to a UMaterialInterface. Each entry must be a `/Game/...` path to a UMaterial or UMaterialInstance. Use `list_assets` with `asset_type='Material'` or `'MaterialInstanceConstant'` to discover. Pass an empty string at index `i` to leave that material slot unset."));
			}
			NewMaterials.Add(Mat);
		}
	}

	// ── Create package + UStaticMesh ──
	UPackage* Pkg = CreatePackage(*TargetAssetPath);
	if (!Pkg)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to create package at %s"), *TargetAssetPath),
			EMCPErrorCode::Internal,
			TEXT("CreatePackage returned nullptr for the target path. Verify the path is a valid `/Game/...` content-root path with no invalid filename characters, and that the destination folder isn't under a read-only source-control region. Retry with a known-good path."));
	}
	Pkg->FullyLoad();

	const FString AssetName = FPaths::GetBaseFilename(TargetAssetPath);
	UStaticMesh* NewSM = NewObject<UStaticMesh>(
		Pkg, *AssetName,
		RF_Public | RF_Standalone | RF_Transactional);
	if (!NewSM)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to construct UStaticMesh at %s"), *TargetAssetPath),
			EMCPErrorCode::Internal,
			TEXT("NewObject<UStaticMesh> returned nullptr despite a valid package. This indicates a constructor failure or naming collision within the package. Verify the package is freshly created (no existing UStaticMesh with the same name) and retry."));
	}

	// ── Configure bake options ──
	FGeometryScriptCopyMeshToAssetOptions Options;
	Options.bEnableRecomputeNormals  = bRecomputeNormals;
	Options.bEnableRecomputeTangents = bRecomputeTangents;
	Options.bUseBuildScale           = true;
	Options.bReplaceMaterials        = (NewMaterials.Num() > 0);
	Options.NewMaterials             = NewMaterials;
	Options.bApplyNaniteSettings     = bEnableNanite;
	Options.NewNaniteSettings.bEnabled = bEnableNanite;
	Options.bEmitTransaction         = false; // headless — skip undo recording
	Options.bDeferMeshPostEditChange = false;

	FGeometryScriptMeshWriteLOD TargetLOD;
	TargetLOD.LODIndex        = 0;
	TargetLOD.bWriteHiResSource = false;

	// ── Run the bake ──
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
		SourceMesh,
		NewSM,
		Options,
		TargetLOD,
		Outcome,
		/*bUseSectionMaterials=*/true,
		/*Debug=*/nullptr);

	if (Outcome != EGeometryScriptOutcomePins::Success)
	{
		// CopyMeshToStaticMesh leaves the constructed UStaticMesh in a bad state
		// on failure. Mark it for GC so the orphan doesn't leak.
		NewSM->ClearFlags(RF_Public | RF_Standalone);
		NewSM->MarkAsGarbage();
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("CopyMeshToStaticMesh failed — see editor log for the geometry-scripting error"),
			EMCPErrorCode::Internal,
			TEXT("UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh returned EGeometryScriptOutcomePins::Failure. The orphan UStaticMesh has been marked for GC. Common causes: source mesh has degenerate triangles, invalid UVs, or non-manifold geometry. Inspect the editor log for the specific `LogGeometry` error, fix the source mesh in Modeling Mode, and retry."));
	}

	// ── Apply collision trace flag on the body setup ──
	{
		UBodySetup* BodySetup = NewSM->GetBodySetup();
		if (!BodySetup)
		{
			NewSM->CreateBodySetup();
			BodySetup = NewSM->GetBodySetup();
		}
		if (BodySetup)
		{
			BodySetup->Modify();
			BodySetup->CollisionTraceFlag = CollisionTraceFlag;
			BodySetup->PostEditChange();
		}
	}

	// ── Persist ──
	FAssetRegistryModule::AssetCreated(NewSM);
	NewSM->MarkPackageDirty();
	Pkg->MarkPackageDirty();

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Pkg->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags     = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Pkg, NewSM, *PackageFileName, SaveArgs);
	if (!bSaved)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to save package %s to %s"), *TargetAssetPath, *PackageFileName),
			EMCPErrorCode::Internal,
			TEXT("UPackage::SavePackage returned false despite a successful in-memory bake. Common causes: (1) destination file is locked by another process; (2) source control has the file checked out; (3) destination filesystem is full or read-only. Check the editor log for the SavePackage error detail, resolve the lock/permission issue, and retry."));
	}

	// ── Optional: replace the source actor with a static-mesh actor ──
	bool bReplacedSourceActor = false;
	if (bReplaceSourceActor)
	{
		const FTransform Xform = SourceActor->GetActorTransform();
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.OverrideLevel = SourceActor->GetLevel();
		AStaticMeshActor* NewActor = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), Xform, SpawnParams);
		if (NewActor)
		{
			NewActor->SetActorLabel(SourceActor->GetActorLabel() + TEXT("_Baked"));
			NewActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			NewActor->GetStaticMeshComponent()->SetStaticMesh(NewSM);
			NewActor->SetActorTransform(Xform);
			World->DestroyActor(SourceActor);
			bReplacedSourceActor = true;
		}
	}

	// ── Build response ──
	int32 BakedTriangleCount = 0;
	int32 BakedVertexCount   = 0;
	if (FStaticMeshRenderData* RD = NewSM->GetRenderData())
	{
		if (RD->LODResources.Num() > 0)
		{
			BakedTriangleCount = RD->LODResources[0].GetNumTriangles();
			BakedVertexCount   = RD->LODResources[0].GetNumVertices();
		}
	}
	const int64 PackageSizeBytes = IFileManager::Get().FileSize(*PackageFileName);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField  (TEXT("success"),                 true);
	Result->SetStringField(TEXT("created_asset_path"),      NewSM->GetPathName());
	Result->SetNumberField(TEXT("triangle_count"),          BakedTriangleCount);
	Result->SetNumberField(TEXT("vertex_count"),            BakedVertexCount);
	Result->SetNumberField(TEXT("source_triangle_count"),   SourceTriangleCount);
	Result->SetNumberField(TEXT("source_vertex_count"),     SourceVertexCount);
	Result->SetNumberField(TEXT("material_slot_count"),     NewSM->GetStaticMaterials().Num());
	Result->SetNumberField(TEXT("lod_count"),               NewSM->GetNumLODs());
	Result->SetNumberField(TEXT("package_size_bytes"),      static_cast<double>(PackageSizeBytes));
	Result->SetBoolField  (TEXT("replaced_source_actor"),   bReplacedSourceActor);
	return Result;
}
