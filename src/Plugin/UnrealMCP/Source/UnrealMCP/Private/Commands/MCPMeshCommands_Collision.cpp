// See MCPMeshCommands.cpp for the full file map.
//
// Collision-authoring commands for existing UStaticMesh assets:
//   - set_static_mesh_collision : add a fitted simple shape (box / sphere /
//     capsule / K-DOP) or run auto-convex decomposition, optionally set the
//     collision-trace flag, and save. Wraps UStaticMeshEditorSubsystem — the
//     same entry points the Static Mesh Editor's Collision menu uses, so the
//     editor close/reopen + PostEditChange rebuild is handled engine-side.
//   - get_static_mesh_collision : non-destructive read of a mesh's collision.

#include "Commands/MCPMeshCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/AggregateGeom.h"

// Static Mesh Editor scripting subsystem + the EScriptCollisionShapeType enum.
#include "StaticMeshEditorSubsystem.h"
#include "StaticMeshEditorSubsystemHelpers.h"

#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "CoreGlobals.h" // GIsPlayInEditorWorld

// ─── helpers ─────────────────────────────────────────────────────────────────

namespace
{
	// NOTE: this mirrors the core MCPMeshCommands.cpp helper (both files map the same
	// small, stable string set). It is NAME-DISTINCT (…Local) because UE unity/jumbo builds merge
	// both .cpp files into one translation unit, where two identically-named anonymous-namespace
	// functions collide (C2084). Keep the two copies in sync — they cover identical flag strings.
	bool ParseCollisionTraceFlagLocal(const FString& In, TEnumAsByte<ECollisionTraceFlag>& Out, FString& OutError)
	{
		const FString Lower = In.ToLower();
		if (Lower == TEXT("default"))               { Out = CTF_UseDefault;          return true; }
		if (Lower == TEXT("simple_and_complex"))    { Out = CTF_UseSimpleAndComplex; return true; }
		if (Lower == TEXT("simple_as_complex"))     { Out = CTF_UseSimpleAsComplex;  return true; }
		if (Lower == TEXT("complex_as_simple"))     { Out = CTF_UseComplexAsSimple;  return true; }
		if (Lower == TEXT("use_complex_collision")) { Out = CTF_UseComplexAsSimple;  return true; }
		OutError = FString::Printf(TEXT("Unknown collision_trace_flag: '%s' (expected default | simple_and_complex | simple_as_complex | complex_as_simple | use_complex_collision)"), *In);
		return false;
	}

	/** Inverse of ParseCollisionTraceFlag — for read/response payloads. */
	FString CollisionTraceFlagToString(TEnumAsByte<ECollisionTraceFlag> Flag)
	{
		switch (Flag)
		{
		case CTF_UseDefault:          return TEXT("default");
		case CTF_UseSimpleAndComplex: return TEXT("simple_and_complex");
		case CTF_UseSimpleAsComplex:  return TEXT("simple_as_complex");
		case CTF_UseComplexAsSimple:  return TEXT("complex_as_simple");
		default:                      return TEXT("unknown");
		}
	}

	/**
	 * Map a string to a simple-collision EScriptCollisionShapeType. Returns true
	 * for the fitted simple shapes; false for the non-simple sentinels "convex" /
	 * "none" (the caller dispatches those to the convex-decomposition / remove
	 * paths) and for genuinely unknown values.
	 */
	bool ParseSimpleShapeType(const FString& In, EScriptCollisionShapeType& Out)
	{
		const FString Lower = In.ToLower();
		if (Lower == TEXT("box"))      { Out = EScriptCollisionShapeType::Box;      return true; }
		if (Lower == TEXT("sphere"))   { Out = EScriptCollisionShapeType::Sphere;   return true; }
		if (Lower == TEXT("capsule"))  { Out = EScriptCollisionShapeType::Capsule;  return true; }
		if (Lower == TEXT("kdop10_x")) { Out = EScriptCollisionShapeType::NDOP10_X; return true; }
		if (Lower == TEXT("kdop10_y")) { Out = EScriptCollisionShapeType::NDOP10_Y; return true; }
		if (Lower == TEXT("kdop10_z")) { Out = EScriptCollisionShapeType::NDOP10_Z; return true; }
		if (Lower == TEXT("kdop18"))   { Out = EScriptCollisionShapeType::NDOP18;   return true; }
		if (Lower == TEXT("kdop26"))   { Out = EScriptCollisionShapeType::NDOP26;   return true; }
		return false;
	}

	/** Resolve a UStaticMesh from a /Game- or plugin-content object/package path. */
	UStaticMesh* ResolveStaticMesh(const FString& AssetPath)
	{
		UObject* Loaded = UEditorAssetLibrary::LoadAsset(AssetPath);
		return Cast<UStaticMesh>(Loaded);
	}

	/** Save the package a static mesh lives in. Returns false (with reason) on failure. */
	bool SaveStaticMeshPackage(UStaticMesh* Mesh, FString& OutError)
	{
		UPackage* Pkg = Mesh ? Mesh->GetPackage() : nullptr;
		if (!Pkg)
		{
			OutError = TEXT("static mesh has no package");
			return false;
		}
		Mesh->MarkPackageDirty();
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags     = SAVE_NoError;
		if (!UPackage::SavePackage(Pkg, Mesh, *PackageFileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("SavePackage failed for %s"), *PackageFileName);
			return false;
		}
		return true;
	}
}

// ─── set_static_mesh_collision ──────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleSetStaticMeshCollision(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This handler requires a params object with at minimum `asset_path` (path to a UStaticMesh) and `shape`."));
	}

	// ── Required params ──
	FString AssetPath, ShapeStr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: asset_path"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required (string) — the object/package path to an existing UStaticMesh, e.g. `/Game/Meshes/SM_Rock_1`. Use `list_assets` with `asset_type='StaticMesh'` to discover."));
	}
	if (!Params->TryGetStringField(TEXT("shape"), ShapeStr) || ShapeStr.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: shape"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`shape` is required (string): box | sphere | capsule | kdop10_x | kdop10_y | kdop10_z | kdop18 | kdop26 | convex | none."));
	}

	// Engine content is read-only-by-policy — refuse to mutate it.
	if (AssetPath.StartsWith(TEXT("/Engine/")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Refusing to author collision on engine content"),
			EMCPErrorCode::InvalidPath,
			TEXT("`asset_path` points at `/Engine/` content. Authoring collision mutates and re-saves the asset; engine content must not be modified. Duplicate the mesh into project (`/Game/...`) or plugin content first."));
	}

	// ── Optional params ──
	const bool  bReplaceExisting = Params->HasField(TEXT("replace_existing")) ? Params->GetBoolField  (TEXT("replace_existing")) : true;
	const int32 HullCount        = Params->HasField(TEXT("hull_count"))       ? Params->GetIntegerField(TEXT("hull_count"))       : 4;
	const int32 MaxHullVerts     = Params->HasField(TEXT("max_hull_verts"))   ? Params->GetIntegerField(TEXT("max_hull_verts"))   : 16;
	const int32 HullPrecision    = Params->HasField(TEXT("hull_precision"))   ? Params->GetIntegerField(TEXT("hull_precision"))   : 100000;
	const bool  bSave            = Params->HasField(TEXT("save"))             ? Params->GetBoolField  (TEXT("save"))             : true;

	const bool bHasTraceFlag = Params->HasField(TEXT("collision_trace_flag"));
	TEnumAsByte<ECollisionTraceFlag> TraceFlag = CTF_UseDefault;
	if (bHasTraceFlag)
	{
		FString TraceErr;
		if (!ParseCollisionTraceFlagLocal(Params->GetStringField(TEXT("collision_trace_flag")), TraceFlag, TraceErr))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TraceErr,
				EMCPErrorCode::InvalidArgument,
				TEXT("`collision_trace_flag` must be: default | simple_and_complex | simple_as_complex | complex_as_simple. `simple_and_complex` (the usual cheap-correct choice) makes simple-shape queries hit the authored hull while only explicit complex/line traces touch the triangle mesh."));
		}
	}

	// ── Resolve subsystem + mesh ──
	UStaticMeshEditorSubsystem* SMSubsystem = GEditor ? GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() : nullptr;
	if (!SMSubsystem)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("UStaticMeshEditorSubsystem unavailable"),
			EMCPErrorCode::Internal,
			TEXT("GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() returned null. The editor is mid-startup/shutdown, or the StaticMeshEditor module failed to load. Retry once the editor is interactive."));
	}

	UStaticMesh* Mesh = ResolveStaticMesh(AssetPath);
	if (!Mesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not load a UStaticMesh at: %s"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`asset_path` did not resolve to a UStaticMesh. Verify the path (object/package path, case-sensitive) and that the asset is a static mesh. Use `list_assets` with `asset_type='StaticMesh'`."));
	}

	// The subsystem's collision ops bail out (return INDEX_NONE / false) when a PIE
	// session is live — surface that up front rather than as an opaque failure.
	if (GEditor->PlayWorld != nullptr || GIsPlayInEditorWorld)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Cannot author collision while a PIE session is active"),
			EMCPErrorCode::InvalidArgument,
			TEXT("UStaticMeshEditorSubsystem collision edits require the editor to not be in Play-In-Editor. Stop PIE (`stop_pie`) and retry."));
	}

	// ── Dispatch by shape ──
	EScriptCollisionShapeType ShapeType;
	const bool bIsSimpleShape = ParseSimpleShapeType(ShapeStr, ShapeType);
	const FString ShapeLower = ShapeStr.ToLower();

	if (bIsSimpleShape)
	{
		// Simple-shape fitting reads the LOD0 mesh description: GenerateBoxAsSimpleCollision
		// dereferences GetMeshDescription(0)->ComputeBoundingBox() and the sphere/capsule/K-DOP
		// variants check(MeshDescription) — both hard-crash when GetMeshDescription(0) returns
		// null (description-less / Nanite-source-stripped meshes). Guard it like the convex path.
		if (!Mesh->IsMeshDescriptionValid(0))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Mesh %s has no valid LOD0 mesh description — simple-shape collision fitting cannot run"), *AssetPath),
				EMCPErrorCode::InvalidArgument,
				TEXT("Fitting a simple collision shape (box/sphere/capsule/kdop*) reads the build-time LOD0 mesh description. Meshes imported without one (or Nanite source-stripped) can't be fitted. Re-import the mesh keeping its source geometry, or use shape `none` to clear collision."));
		}
		// "Add [...] Simplified Collision" stacks by default — remove first so the
		// requested shape replaces rather than accumulates (unless explicitly opted out).
		if (bReplaceExisting)
		{
			SMSubsystem->RemoveCollisions(Mesh);
		}
		const int32 PrimIndex = SMSubsystem->AddSimpleCollisions(Mesh, ShapeType);
		if (PrimIndex < 0)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("AddSimpleCollisions(%s) failed for %s"), *ShapeStr, *AssetPath),
				EMCPErrorCode::Internal,
				TEXT("UStaticMeshEditorSubsystem::AddSimpleCollisions returned a negative index. Check the editor log (LogStaticMeshEditorSubsystem) — the mesh may have no render data or be in an invalid state."));
		}
	}
	else if (ShapeLower == TEXT("convex"))
	{
		// Auto Convex Collision. The subsystem removes existing collision itself and
		// requires a valid LOD0 mesh description (Nanite-only / description-less meshes
		// are silently skipped -> false).
		if (!Mesh->IsMeshDescriptionValid(0))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Mesh %s has no valid LOD0 mesh description — convex decomposition cannot run"), *AssetPath),
				EMCPErrorCode::InvalidArgument,
				TEXT("Auto-convex decomposition needs a build-time mesh description at LOD0. Meshes imported without one (or Nanite source-stripped) can't be decomposed. Use a primitive shape (box/sphere/capsule/kdop*) instead, or re-import the mesh keeping its source geometry."));
		}
		if (HullCount <= 0 || HullPrecision <= 0)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("convex requires hull_count > 0 and hull_precision > 0"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`hull_count` (max convex pieces) and `hull_precision` (voxel resolution) must be positive. Defaults: hull_count=4, max_hull_verts=16, hull_precision=100000."));
		}
		if (!SMSubsystem->SetConvexDecompositionCollisions(Mesh, HullCount, MaxHullVerts, HullPrecision))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("SetConvexDecompositionCollisions failed for %s"), *AssetPath),
				EMCPErrorCode::Internal,
				TEXT("UStaticMeshEditorSubsystem::SetConvexDecompositionCollisions returned false. Check the editor log (LogStaticMeshEditorSubsystem) for the cause — typically degenerate geometry or an invalid mesh description."));
		}
	}
	else if (ShapeLower == TEXT("none"))
	{
		SMSubsystem->RemoveCollisions(Mesh);
	}
	else
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown shape: '%s'"), *ShapeStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("`shape` must be one of: box | sphere | capsule | kdop10_x | kdop10_y | kdop10_z | kdop18 | kdop26 | convex | none."));
	}

	// ── Optional trace-flag write (mirrors the bake handler's body-setup write) ──
	if (bHasTraceFlag)
	{
		UBodySetup* BodySetup = Mesh->GetBodySetup();
		if (!BodySetup)
		{
			Mesh->CreateBodySetup();
			BodySetup = Mesh->GetBodySetup();
		}
		if (BodySetup)
		{
			BodySetup->Modify();
			BodySetup->CollisionTraceFlag = TraceFlag;
			BodySetup->PostEditChange();
		}
	}

	// ── Persist ──
	bool bSaved = false;
	if (bSave)
	{
		FString SaveErr;
		bSaved = SaveStaticMeshPackage(Mesh, SaveErr);
		if (!bSaved)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Collision authored but save failed: %s"), *SaveErr),
				EMCPErrorCode::Internal,
				TEXT("The collision change applied in-memory but the package could not be saved. Common causes: file locked, source-control checkout, or read-only filesystem. Resolve and re-run with the same params (idempotent for a fixed shape + replace_existing=true), or save the asset manually in-editor."));
		}
	}

	// ── Build response ──
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField  (TEXT("success"),                 true);
	Result->SetStringField(TEXT("asset_path"),              Mesh->GetPathName());
	Result->SetStringField(TEXT("shape"),                   ShapeLower);
	Result->SetNumberField(TEXT("simple_collision_count"),  SMSubsystem->GetSimpleCollisionCount(Mesh));
	Result->SetNumberField(TEXT("convex_collision_count"),  SMSubsystem->GetConvexCollisionCount(Mesh));
	Result->SetStringField(TEXT("collision_complexity"),    CollisionTraceFlagToString(SMSubsystem->GetCollisionComplexity(Mesh)));
	Result->SetBoolField  (TEXT("saved"),                   bSaved);
	if (bHasTraceFlag)
	{
		Result->SetStringField(TEXT("collision_trace_flag"), CollisionTraceFlagToString(TraceFlag));
	}
	return Result;
}

// ─── get_static_mesh_collision ──────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleGetStaticMeshCollision(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: asset_path"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required (string) — the object/package path to an existing UStaticMesh, e.g. `/Game/Meshes/SM_Rock_1`."));
	}

	UStaticMeshEditorSubsystem* SMSubsystem = GEditor ? GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() : nullptr;
	if (!SMSubsystem)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("UStaticMeshEditorSubsystem unavailable"),
			EMCPErrorCode::Internal,
			TEXT("GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>() returned null. Retry once the editor is interactive."));
	}

	UStaticMesh* Mesh = ResolveStaticMesh(AssetPath);
	if (!Mesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not load a UStaticMesh at: %s"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`asset_path` did not resolve to a UStaticMesh. Verify the path and that the asset is a static mesh."));
	}

	UBodySetup* BodySetup = Mesh->GetBodySetup();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField  (TEXT("success"),                 true);
	Result->SetStringField(TEXT("asset_path"),              Mesh->GetPathName());
	Result->SetBoolField  (TEXT("has_body_setup"),          BodySetup != nullptr);
	Result->SetStringField(TEXT("collision_complexity"),    CollisionTraceFlagToString(SMSubsystem->GetCollisionComplexity(Mesh)));
	Result->SetNumberField(TEXT("simple_collision_count"),  SMSubsystem->GetSimpleCollisionCount(Mesh));
	Result->SetNumberField(TEXT("convex_collision_count"),  SMSubsystem->GetConvexCollisionCount(Mesh));
	if (BodySetup)
	{
		const FKAggregateGeom& Agg = BodySetup->AggGeom;
		Result->SetNumberField(TEXT("box_count"),     Agg.BoxElems.Num());
		Result->SetNumberField(TEXT("sphere_count"),  Agg.SphereElems.Num());
		Result->SetNumberField(TEXT("capsule_count"), Agg.SphylElems.Num());
		Result->SetNumberField(TEXT("convex_count"),  Agg.ConvexElems.Num());
	}
	else
	{
		Result->SetNumberField(TEXT("box_count"),     0);
		Result->SetNumberField(TEXT("sphere_count"),  0);
		Result->SetNumberField(TEXT("capsule_count"), 0);
		Result->SetNumberField(TEXT("convex_count"),  0);
	}
	return Result;
}
