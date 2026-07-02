#include "Commands/MCPMaterialCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "Commands/MaterialGraph/MaterialExpressionManager.h"
#include "Commands/MaterialGraph/MaterialConnector.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionComment.h"
#include "MaterialShared.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "FileHelpers.h"
#include "UObject/SavePackage.h"

// ---------------------------------------------------------------------------
// Command routing
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("material_create"))
	{
		return HandleCreateMaterial(Params);
	}
	else if (CommandType == TEXT("material_set_property"))
	{
		return HandleSetMaterialProperty(Params);
	}
	else if (CommandType == TEXT("material_create_instance"))
	{
		return HandleCreateMaterialInstance(Params);
	}
	else if (CommandType == TEXT("material_read"))
	{
		return HandleReadMaterialGraph(Params);
	}
	else if (CommandType == TEXT("material_read_function"))
	{
		return HandleReadMaterialFunction(Params);
	}
	else if (CommandType == TEXT("material_read_instance"))
	{
		return HandleReadMaterialInstance(Params);
	}
	else if (CommandType == TEXT("material_instance_set_parameter"))
	{
		return HandleSetMaterialInstanceParameter(Params);
	}
	else if (CommandType == TEXT("material_reparent_instance"))
	{
		return HandleReparentMaterialInstance(Params);
	}
	else if (CommandType == TEXT("material_compile"))
	{
		return HandleCompileMaterial(Params);
	}
	else if (CommandType == TEXT("material_add_expression"))
	{
		return HandleAddMaterialExpression(Params);
	}
	else if (CommandType == TEXT("material_set_expression_property"))
	{
		return HandleSetMaterialExpressionProperty(Params);
	}
	else if (CommandType == TEXT("material_delete_expression"))
	{
		return HandleDeleteMaterialExpression(Params);
	}
	else if (CommandType == TEXT("material_connect"))
	{
		return HandleConnectMaterialExpressions(Params);
	}
	else if (CommandType == TEXT("list_material_parameters"))
	{
		return HandleListMaterialParameters(Params);
	}

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown material command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("Valid material commands: create_material, create_material_instance, compile_material, read_material_graph, read_material_function, read_material_instance, add_material_expression, delete_material_expression, set_material_expression_property, set_material_instance_parameter, reparent_material_instance, connect_material_expressions, list_material_parameters."));
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

UMaterial* FMCPMaterialCommands::FindMaterialByPath(const FString& MaterialPath)
{
	// Full path — load directly
	if (MaterialPath.StartsWith(TEXT("/")))
	{
		UObject* Obj = UEditorAssetLibrary::LoadAsset(MaterialPath);
		if (UMaterial* Mat = Cast<UMaterial>(Obj))
		{
			return Mat;
		}
		// Try appending .AssetName
		FString AssetName = FPaths::GetBaseFilename(MaterialPath);
		FString FullPath = FString::Printf(TEXT("%s.%s"), *MaterialPath, *AssetName);
		return LoadObject<UMaterial>(nullptr, *FullPath);
	}

	// Short name — try /Game/Materials/<Name> first
	FString DefaultPath = FString::Printf(TEXT("/Game/Materials/%s"), *MaterialPath);
	UObject* Obj = UEditorAssetLibrary::LoadAsset(DefaultPath);
	if (UMaterial* Mat = Cast<UMaterial>(Obj))
	{
		return Mat;
	}

	// Fallback: scan asset registry
	FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> Found;
	AR.Get().GetAssetsByClass(UMaterial::StaticClass()->GetClassPathName(), Found);
	for (const FAssetData& Asset : Found)
	{
		if (Asset.AssetName.ToString() == MaterialPath)
		{
			return Cast<UMaterial>(Asset.GetAsset());
		}
	}

	UE_LOG(LogUnrealMCP, Error, TEXT("FindMaterialByPath: Failed to find material: %s"), *MaterialPath);
	return nullptr;
}

// ---------------------------------------------------------------------------
// create_material
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleCreateMaterial(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialName;
	if (!Params->TryGetStringField(TEXT("material_name"), MaterialName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `material_name` (string) — the name of the new UMaterial asset (without path). Optionally pass `package_path` to override the default `/Game/Materials` location."));
	}

	FString PackagePath = TEXT("/Game/Materials");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);

	FString FullPath = PackagePath / MaterialName;
	FString PackageName = FullPath;

	// An existing object of a *different* class at this exact path would make the
	// factory's NewObject<UMaterial>(Package, Class, FName(*MaterialName)) hit
	// StaticAllocateObject's "Cannot replace existing object of a different class"
	// Fatal log (hard editor crash). Re-creating over an existing UMaterial is a
	// valid in-place overwrite, so only the incompatible case is rejected here.
	if (UObject* ExistingAtPath = UEditorAssetLibrary::LoadAsset(FullPath))
	{
		if (!ExistingAtPath->IsA<UMaterial>())
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("An asset of a different class already exists at %s"), *FullPath),
				EMCPErrorCode::NameCollision,
				TEXT("A non-Material asset already occupies this path; creating a UMaterial there would replace an object of a different class and crash the editor. Pick a different `material_name`/`package_path`, or delete the existing asset first."));
		}
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to create package: %s"), *PackageName),
			EMCPErrorCode::Internal,
			TEXT("CreatePackage returned null — the path may collide with an existing package or contain invalid characters. Verify the path and content-browser permissions."));
	}

	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(Factory->FactoryCreateNew(
		UMaterial::StaticClass(), Package, FName(*MaterialName),
		RF_Public | RF_Standalone, nullptr, GWarn));

	if (!Material)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create material"),
			EMCPErrorCode::Internal,
			TEXT("UMaterialFactoryNew::FactoryCreateNew returned null. The package was created but the factory rejected the asset. Check the editor log for the factory's failure reason."));
	}

	// Optional top-level material properties. Applied before PostEditChange so
	// the shader compile picks up the chosen domain. All three are optional —
	// omitting falls through to the engine factory defaults (Opaque, single-sided,
	// DefaultLit).
	FString BlendModeStr;
	if (Params->TryGetStringField(TEXT("blend_mode"), BlendModeStr))
	{
		if      (BlendModeStr.Equals(TEXT("Opaque"),         ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Opaque;
		else if (BlendModeStr.Equals(TEXT("Masked"),         ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Masked;
		else if (BlendModeStr.Equals(TEXT("Translucent"),    ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Translucent;
		else if (BlendModeStr.Equals(TEXT("Additive"),       ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Additive;
		else if (BlendModeStr.Equals(TEXT("Modulate"),       ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Modulate;
		else if (BlendModeStr.Equals(TEXT("AlphaComposite"), ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_AlphaComposite;
		else if (BlendModeStr.Equals(TEXT("AlphaHoldout"),   ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_AlphaHoldout;
		else UE_LOG(LogUnrealMCP, Warning, TEXT("Unknown blend_mode '%s' — leaving at factory default"), *BlendModeStr);
	}

	bool bTwoSided = false;
	if (Params->TryGetBoolField(TEXT("two_sided"), bTwoSided))
	{
		Material->TwoSided = bTwoSided;
	}

	FString ShadingModelStr;
	if (Params->TryGetStringField(TEXT("shading_model"), ShadingModelStr))
	{
		if      (ShadingModelStr.Equals(TEXT("DefaultLit"),         ESearchCase::IgnoreCase) ||
		         ShadingModelStr.Equals(TEXT("Lit"),                ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_DefaultLit);
		else if (ShadingModelStr.Equals(TEXT("Unlit"),              ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Unlit);
		else if (ShadingModelStr.Equals(TEXT("Subsurface"),         ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Subsurface);
		else if (ShadingModelStr.Equals(TEXT("PreintegratedSkin"),  ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_PreintegratedSkin);
		else if (ShadingModelStr.Equals(TEXT("ClearCoat"),          ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_ClearCoat);
		else if (ShadingModelStr.Equals(TEXT("SubsurfaceProfile"),  ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_SubsurfaceProfile);
		else if (ShadingModelStr.Equals(TEXT("TwoSidedFoliage"),    ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_TwoSidedFoliage);
		else if (ShadingModelStr.Equals(TEXT("Hair"),               ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Hair);
		else if (ShadingModelStr.Equals(TEXT("Cloth"),              ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Cloth);
		else if (ShadingModelStr.Equals(TEXT("Eye"),                ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Eye);
		else if (ShadingModelStr.Equals(TEXT("SingleLayerWater"),   ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_SingleLayerWater);
		else if (ShadingModelStr.Equals(TEXT("ThinTranslucent"),    ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_ThinTranslucent);
		else UE_LOG(LogUnrealMCP, Warning, TEXT("Unknown shading_model '%s' — leaving at factory default"), *ShadingModelStr);
	}

	// Material domain. UMG/Slate widget materials MUST be MD_UI or they will not
	// render through the Slate brush pipeline. Set before PostEditChange so the
	// initial shader compile targets the right domain.
	FString DomainStr;
	if (Params->TryGetStringField(TEXT("material_domain"), DomainStr))
	{
		if      (DomainStr.Equals(TEXT("Surface"),          ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_Surface;
		else if (DomainStr.Equals(TEXT("UI"),               ESearchCase::IgnoreCase) ||
		         DomainStr.Equals(TEXT("UserInterface"),    ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_UI;
		else if (DomainStr.Equals(TEXT("PostProcess"),      ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_PostProcess;
		else if (DomainStr.Equals(TEXT("DeferredDecal"),    ESearchCase::IgnoreCase) ||
		         DomainStr.Equals(TEXT("Decal"),            ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_DeferredDecal;
		else if (DomainStr.Equals(TEXT("LightFunction"),    ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_LightFunction;
		else if (DomainStr.Equals(TEXT("Volume"),           ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_Volume;
		else UE_LOG(LogUnrealMCP, Warning, TEXT("Unknown material_domain '%s' — leaving at factory default (Surface)"), *DomainStr);
	}

	FAssetRegistryModule::AssetCreated(Material);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveAsset(FullPath, /*bOnlyIfIsDirty=*/ false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Created material %s in memory but failed to save it to disk"), *FullPath),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not persisted (commonly because a PIE session is active, which blocks asset saves). Stop PIE and retry; the in-memory material will otherwise be lost on editor restart."));
	}

	UE_LOG(LogUnrealMCP, Display, TEXT("Created material: %s"), *FullPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_name"), MaterialName);
	Data->SetStringField(TEXT("material_path"), FullPath);
	Data->SetBoolField(TEXT("success"), true);
	return Data;
}

// ---------------------------------------------------------------------------
// material_set_property — GAP-051. Change top-level flags (blend_mode, two_sided,
// shading_model, material_domain) on an EXISTING UMaterial. material_create accepted
// these at creation but there was no way to flip them afterward (e.g. Translucent→Opaque
// to kill ground-plane sort shimmer) without recreating the whole graph.
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleSetMaterialProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`material_path` is required — the full asset path to a UMaterial, e.g. `/Game/Materials/M_Foo`."));

	UMaterial* Material = FindMaterialByPath(MaterialPath);
	if (!Material)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`material_path` did not resolve to a UMaterial (note: this sets flags on a base UMaterial, not a Material Instance). Use list_assets asset_type='Material'."));

	Material->PreEditChange(nullptr);

	bool bChanged = false;
	TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();

	FString BlendModeStr;
	if (Params->TryGetStringField(TEXT("blend_mode"), BlendModeStr))
	{
		bool bOk = true;
		if      (BlendModeStr.Equals(TEXT("Opaque"),         ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Opaque;
		else if (BlendModeStr.Equals(TEXT("Masked"),         ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Masked;
		else if (BlendModeStr.Equals(TEXT("Translucent"),    ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Translucent;
		else if (BlendModeStr.Equals(TEXT("Additive"),       ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Additive;
		else if (BlendModeStr.Equals(TEXT("Modulate"),       ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_Modulate;
		else if (BlendModeStr.Equals(TEXT("AlphaComposite"), ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_AlphaComposite;
		else if (BlendModeStr.Equals(TEXT("AlphaHoldout"),   ESearchCase::IgnoreCase)) Material->BlendMode = BLEND_AlphaHoldout;
		else bOk = false;
		if (bOk) { bChanged = true; Applied->SetStringField(TEXT("blend_mode"), BlendModeStr); }
	}

	bool bTwoSided = false;
	if (Params->TryGetBoolField(TEXT("two_sided"), bTwoSided))
	{
		Material->TwoSided = bTwoSided;
		bChanged = true;
		Applied->SetBoolField(TEXT("two_sided"), bTwoSided);
	}

	FString ShadingModelStr;
	if (Params->TryGetStringField(TEXT("shading_model"), ShadingModelStr))
	{
		bool bOk = true;
		if      (ShadingModelStr.Equals(TEXT("DefaultLit"),         ESearchCase::IgnoreCase) ||
		         ShadingModelStr.Equals(TEXT("Lit"),                ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_DefaultLit);
		else if (ShadingModelStr.Equals(TEXT("Unlit"),              ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Unlit);
		else if (ShadingModelStr.Equals(TEXT("Subsurface"),         ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Subsurface);
		else if (ShadingModelStr.Equals(TEXT("PreintegratedSkin"),  ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_PreintegratedSkin);
		else if (ShadingModelStr.Equals(TEXT("ClearCoat"),          ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_ClearCoat);
		else if (ShadingModelStr.Equals(TEXT("SubsurfaceProfile"),  ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_SubsurfaceProfile);
		else if (ShadingModelStr.Equals(TEXT("TwoSidedFoliage"),    ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_TwoSidedFoliage);
		else if (ShadingModelStr.Equals(TEXT("Hair"),               ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Hair);
		else if (ShadingModelStr.Equals(TEXT("Cloth"),              ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Cloth);
		else if (ShadingModelStr.Equals(TEXT("Eye"),                ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_Eye);
		else if (ShadingModelStr.Equals(TEXT("SingleLayerWater"),   ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_SingleLayerWater);
		else if (ShadingModelStr.Equals(TEXT("ThinTranslucent"),    ESearchCase::IgnoreCase)) Material->SetShadingModel(MSM_ThinTranslucent);
		else bOk = false;
		if (bOk) { bChanged = true; Applied->SetStringField(TEXT("shading_model"), ShadingModelStr); }
	}

	FString DomainStr;
	if (Params->TryGetStringField(TEXT("material_domain"), DomainStr))
	{
		bool bOk = true;
		if      (DomainStr.Equals(TEXT("Surface"),          ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_Surface;
		else if (DomainStr.Equals(TEXT("UI"),               ESearchCase::IgnoreCase) ||
		         DomainStr.Equals(TEXT("UserInterface"),    ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_UI;
		else if (DomainStr.Equals(TEXT("PostProcess"),      ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_PostProcess;
		else if (DomainStr.Equals(TEXT("DeferredDecal"),    ESearchCase::IgnoreCase) ||
		         DomainStr.Equals(TEXT("Decal"),            ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_DeferredDecal;
		else if (DomainStr.Equals(TEXT("LightFunction"),    ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_LightFunction;
		else if (DomainStr.Equals(TEXT("Volume"),           ESearchCase::IgnoreCase)) Material->MaterialDomain = MD_Volume;
		else bOk = false;
		if (bOk) { bChanged = true; Applied->SetStringField(TEXT("material_domain"), DomainStr); }
	}

	if (!bChanged)
	{
		Material->PostEditChange();   // close the envelope we opened
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No recognized material properties provided"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Provide at least one of `blend_mode`, `two_sided`, `shading_model`, `material_domain` with a valid value."));
	}

	// Recompile the material's shaders so the flag change takes effect AND propagate to
	// dependent material instances (a blend/shading-model change can require child MICs to
	// pick up shaders the new settings demand — see RecompileMaterialWithDependents).
	FMCPCommonUtils::RecompileMaterialWithDependents(Material);
	if (!UEditorAssetLibrary::SaveAsset(Material->GetPathName(), /*bOnlyIfIsDirty=*/ false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material flags changed in memory but failed to save: %s"), *Material->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false (commonly blocked during PIE). Stop PIE and retry."));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), Material->GetPathName());
	Data->SetObjectField(TEXT("applied"), Applied);
	Data->SetBoolField(TEXT("success"), true);
	return Data;
}

// ---------------------------------------------------------------------------
// create_material_instance
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleCreateMaterialInstance(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `asset_path` (string) — the full /Game/... path where the new Material Instance Constant should be created."));
	}
	if (!AssetPath.StartsWith(TEXT("/Game/")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("asset_path must start with /Game/ (got %s)"), *AssetPath),
			EMCPErrorCode::InvalidPath,
			TEXT("Asset paths use the /Game/... virtual root (mapped to your project's Content folder). System paths like /Engine/ or absolute disk paths aren't valid here."));
	}

	FString ParentPath;
	if (!Params->TryGetStringField(TEXT("parent_material"), ParentPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'parent_material' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `parent_material` (string) — the /Game/... path of the UMaterial or UMaterialInterface to inherit from. The instance's parameters mirror the parent's exposed parameters."));
	}

	// LoadAsset's path normalisation is finicky with leading-underscore asset
	// names (e.g. `_M_PBR_Master`). Mirror FindMaterialByPath's two-step
	// strategy: try LoadAsset first, then fall back to LoadObject with the
	// `.AssetName` object suffix appended.
	UMaterialInterface* Parent = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(ParentPath));
	if (Parent == nullptr)
	{
		const FString AssetName = FPaths::GetBaseFilename(ParentPath);
		const FString FullPath = ParentPath.Contains(TEXT("."))
			? ParentPath
			: FString::Printf(TEXT("%s.%s"), *ParentPath, *AssetName);
		Parent = LoadObject<UMaterialInterface>(nullptr, *FullPath);
	}
	if (Parent == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("parent_material '%s' did not resolve to a UMaterialInterface"), *ParentPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("The path didn't load to a UMaterial or UMaterialInterface subclass. Verify the asset exists, or use `material_get_available` to discover candidates."));
	}

	const FString PackageFolder = FPaths::GetPath(AssetPath);
	const FString AssetName     = FPaths::GetCleanFilename(AssetPath);
	const FString PackageName   = AssetPath;

	// Caller may have asked to overwrite — delete any existing asset at the path
	// so the factory can land cleanly. We do this only when the caller passed
	// force_overwrite=true (matches import_textures semantics).
	bool bForceOverwrite = false;
	Params->TryGetBoolField(TEXT("force_overwrite"), bForceOverwrite);
	if (bForceOverwrite && UEditorAssetLibrary::DoesAssetExist(AssetPath))
	{
		UEditorAssetLibrary::DeleteAsset(AssetPath);
	}
	else if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset already exists at %s — pass force_overwrite=true to replace it"), *AssetPath),
			EMCPErrorCode::NameCollision,
			TEXT("Pass `force_overwrite=true` to delete the existing asset before creating, or pick a different `asset_path`. `asset_list` can confirm which paths are taken."));
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (Package == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to create package: %s"), *PackageName),
			EMCPErrorCode::Internal,
			TEXT("CreatePackage returned null — the path may collide with an existing package or contain invalid characters. Verify the path and content-browser permissions."));
	}

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Parent;

	UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(
		Factory->FactoryCreateNew(
			UMaterialInstanceConstant::StaticClass(),
			Package,
			FName(*AssetName),
			RF_Public | RF_Standalone,
			nullptr,
			GWarn));

	if (MIC == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Factory failed to produce a UMaterialInstanceConstant"),
			EMCPErrorCode::Internal,
			TEXT("UMaterialInstanceConstantFactoryNew::FactoryCreateNew returned null. Parent material was loaded but the factory rejected. Check the editor log for specifics."));
	}

	FAssetRegistryModule::AssetCreated(MIC);
	MIC->PostEditChange();
	MIC->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveAsset(AssetPath, /*bOnlyIfIsDirty=*/ false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Created Material Instance %s in memory but failed to save it to disk"), *AssetPath),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not persisted (commonly because a PIE session is active, which blocks asset saves). Stop PIE and retry; the in-memory instance will otherwise be lost on editor restart."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("parent_material"), Parent->GetPathName());
	Data->SetBoolField(TEXT("success"), true);
	return Data;
}

// ---------------------------------------------------------------------------
// compile_material
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleCompileMaterial(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `material_path` (string) — the /Game/... path of the UMaterial to compile. Use `material_get_available` to discover."));
	}

	UMaterial* Material = FindMaterialByPath(MaterialPath);
	if (!Material)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Material exists at the supplied /Game/... path; use `material_get_available` to discover candidates."));
	}

	// Recompile the material AND propagate the change to every dependent material
	// INSTANCE — the part a bare Material->PostEditChange() skips. A mesh slot almost
	// always renders a UMaterialInstance (MIC), not the base UMaterial; when the parent
	// graph gains/changes TextureSample nodes, each child instance must rebuild its own
	// uniform-expression set (texture bindings) or it keeps a stale, textureless render
	// proxy and draws as the DEFAULT grey-checkerboard material — a "false-compiled" state
	// that otherwise only cleared by opening the asset in the Material Editor (which runs
	// UMaterialEditingLibrary::RecompileMaterial). The FMaterialUpdateContext destructor is
	// what performs that propagation: it walks every UMaterialInstance whose base material
	// was updated and calls UpdateCachedData() + RecacheUniformExpressions(true) +
	// InitStaticPermutation() on it (see Engine FMaterialUpdateContext::~FMaterialUpdateContext).
	// This mirrors UMaterialEditingLibrary::RecompileMaterialInternal without taking a
	// MaterialEditor-module dependency (FMaterialUpdateContext + AddMaterial are ENGINE_API).
	{
		FMaterialUpdateContext UpdateContext;
		UpdateContext.AddMaterial(Material);

		Material->PreEditChange(nullptr);
		Material->PostEditChange();
		Material->MarkPackageDirty();

		// Force synchronous compilation of the BASE material first, so its referenced-texture
		// table + shader map are final before the UpdateContext destructor (scope exit below)
		// recompiles the dependent instances against it. Also makes compile errors available
		// immediately for the response.
		Material->ForceRecompileForRendering();
		if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			Resource->FinishCompilation();
		}
	} // <- FMaterialUpdateContext destructor recompiles/recaches dependent instances + components

	// GAP-062: the recompile above MarkPackageDirty's the material but historically
	// never wrote it back, so a graph rebuild that ended in material_compile was
	// silently reverted on the next editor reload. Persist by default, mirroring the
	// material-graph mutators. Saved regardless of shader-compile errors below — the
	// asset is valid on disk; the errors[] array reports shader state, not a save
	// failure. A genuine save failure (PIE active / read-only package) is surfaced.
	if (TSharedPtr<FJsonObject> SaveErr = FMCPCommonUtils::SaveMaterialOrError(Material))
		return SaveErr;

	// Collect compile errors/warnings from the current platform's material resource.
	TArray<FString> AllErrors;
	if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		for (const FString& Err : Resource->GetCompileErrors())
		{
			AllErrors.AddUnique(Err);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), MaterialPath);
	Data->SetBoolField(TEXT("success"), AllErrors.Num() == 0);

	TArray<TSharedPtr<FJsonValue>> ErrorArray;
	for (const FString& Err : AllErrors)
	{
		ErrorArray.Add(MakeShared<FJsonValueString>(Err));
		UE_LOG(LogUnrealMCP, Warning, TEXT("Material compile error: %s"), *Err);
	}
	Data->SetArrayField(TEXT("errors"), ErrorArray);

	// Set "error" summary string for the bridge's error path.
	if (AllErrors.Num() > 0)
	{
		Data->SetStringField(TEXT("error"),
			FString::Printf(TEXT("Material compilation failed with %d error(s)"), AllErrors.Num()));
	}

	return Data;
}

// ---------------------------------------------------------------------------
// read_material_graph  (delegates to MaterialExpressionManager)
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleReadMaterialGraph(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialExpressionManager::ReadMaterialGraph(Params);
}

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleReadMaterialFunction(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialExpressionManager::ReadMaterialFunction(Params);
}

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleReadMaterialInstance(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialExpressionManager::ReadMaterialInstance(Params);
}

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleSetMaterialInstanceParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialExpressionManager::SetMaterialInstanceParameter(Params);
}

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleReparentMaterialInstance(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialExpressionManager::ReparentMaterialInstance(Params);
}

// ---------------------------------------------------------------------------
// Expression CRUD  (delegates to MaterialExpressionManager)
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleAddMaterialExpression(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialExpressionManager::AddExpression(Params);
}

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleSetMaterialExpressionProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialExpressionManager::SetExpressionProperty(Params);
}

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleDeleteMaterialExpression(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialExpressionManager::DeleteExpression(Params);
}

// ---------------------------------------------------------------------------
// Wiring  (delegates to MaterialConnector)
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleConnectMaterialExpressions(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMaterialConnector::ConnectExpressions(Params);
}

// ---------------------------------------------------------------------------
// list_material_parameters — enumerate all parameters via reflection
// ---------------------------------------------------------------------------

#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialTypes.h"
#include "EditorAssetLibrary.h"

TSharedPtr<FJsonObject> FMCPMaterialCommands::HandleListMaterialParameters(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid() || !Params->HasField(TEXT("material_path")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing required param: material_path"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `material_path` (string) — the /Game/... path of the Material or Material Instance to enumerate parameters from."));
	}
	const FString Path = Params->GetStringField(TEXT("material_path"));
	UObject* Loaded = UEditorAssetLibrary::LoadAsset(Path);
	if (!Loaded)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset not found: %s"), *Path),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the asset exists at the supplied /Game/... path; use `asset_list` to discover."));
	}
	UMaterialInterface* MatIface = Cast<UMaterialInterface>(Loaded);
	if (!MatIface)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset is not a Material/MaterialInstance: %s"), *Path),
			EMCPErrorCode::UnsupportedClass,
			TEXT("`list_material_parameters` only operates on UMaterial / UMaterialInterface assets. The path resolved to a different UObject subclass."));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("material_path"), Path);

	auto SerializeParams = [](const TArray<FMaterialParameterInfo>& Infos,
	                          const TArray<FGuid>& Guids,
	                          const TCHAR* Type) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (int32 i = 0; i < Infos.Num(); ++i)
		{
			TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
			O->SetStringField(TEXT("name"), Infos[i].Name.ToString());
			O->SetStringField(TEXT("type"), Type);
			Arr.Add(MakeShareable(new FJsonValueObject(O)));
		}
		return Arr;
	};

	TArray<FMaterialParameterInfo> ScalarInfos, VectorInfos, TextureInfos, StaticSwitchInfos;
	TArray<FGuid> Guids;

	MatIface->GetAllScalarParameterInfo(ScalarInfos, Guids);
	MatIface->GetAllVectorParameterInfo(VectorInfos, Guids);
	MatIface->GetAllTextureParameterInfo(TextureInfos, Guids);
	MatIface->GetAllStaticSwitchParameterInfo(StaticSwitchInfos, Guids);

	Result->SetArrayField(TEXT("scalars"),  SerializeParams(ScalarInfos,  Guids, TEXT("Scalar")));
	Result->SetArrayField(TEXT("vectors"),  SerializeParams(VectorInfos,  Guids, TEXT("Vector")));
	Result->SetArrayField(TEXT("textures"), SerializeParams(TextureInfos, Guids, TEXT("Texture")));
	Result->SetArrayField(TEXT("static_switches"), SerializeParams(StaticSwitchInfos, Guids, TEXT("StaticSwitch")));

	Result->SetNumberField(TEXT("num_scalars"),  ScalarInfos.Num());
	Result->SetNumberField(TEXT("num_vectors"),  VectorInfos.Num());
	Result->SetNumberField(TEXT("num_textures"), TextureInfos.Num());
	Result->SetNumberField(TEXT("num_static_switches"), StaticSwitchInfos.Num());

	return FMCPCommonUtils::CreateSuccessResponse(Result);
}
