// Niagara *authoring* command handlers — the create/structural half of
// FMCPNiagaraCommands. The read + tweak-existing handlers (list, read,
// user-params, module-input, scratch-pad) live in the core
// MCPNiagaraCommands.cpp; the shared statics they both use live in
// MCPNiagaraCommands_Internal.h.
//
// Handlers here:
//   niagara_system_create          — new empty UNiagaraSystem on disk
//   niagara_emitter_add            — add a blank CPU emitter (output nodes, no renderer)
//   niagara_emitter_add_renderer   — add a Ribbon/Sprite/Mesh renderer (+ optional material)
//   niagara_renderer_set_material  — (re)bind a renderer's material
//   niagara_module_add             — insert a stack module from an engine UNiagaraScript asset
//
// All five use only NIAGARAEDITOR_API / NIAGARA_API-exported entry points
// (InitializeSystem / InitializeEmitter / AddEmitterToSystem / AddRenderer /
// AddScriptModuleToStack(UNiagaraScript*)), so they link from this external
// plugin module. We deliberately avoid the FNiagaraSystemViewModel path (heavier,
// requires Cleanup()) and the unexported ResetGraphForOutput/RelayoutGraph — the
// emitter factory builds the output nodes, and we find them by walking the graph
// exactly as HandleAddNiagaraScratchPadModule does.

#include "Commands/MCPNiagaraCommands.h"
#include "Commands/MCPNiagaraCommands_Internal.h"
#include "Commands/MCPCommonUtils.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraParameterStore.h" // GetExposedParameters().ReadParameterVariables() in the material-binding handler
#include "NiagaraTypes.h"          // FNiagaraTypeDefinition::IsUObject()/GetClass()
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEditorUtilities.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h" // niagara_mesh_renderer_set_mesh assigns Meshes[0].Mesh
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"

// ── Local helpers ─────────────────────────────────────────────────────────

// Persist a system's package to disk (mirrors the factory handlers' save idiom).
static bool SaveNiagaraSystemPackage(UNiagaraSystem* System)
{
	if (System && System->GetPackage())
	{
		return UEditorAssetLibrary::SaveAsset(System->GetPackage()->GetName(), /*bOnlyIfIsDirty*/false);
	}
	return false;
}

// Standard recompile-and-drain after a structural edit (matches the core
// handlers): kick the async compile, then block so the save writes fresh bytecode.
static void RecompileSystem(UNiagaraSystem* System)
{
	System->RequestCompile(false);
	System->PollForCompilationComplete(true);
}

// ── niagara_system_create ─────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraSystemCreate(
	const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath;
	if (!Params->TryGetStringField(TEXT("system_path"), SystemPath) || SystemPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'system_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `system_path` (string) — the full /Game/... destination for the new Niagara System, e.g. /Game/FX/NS_Sparks."));
	}

	// Split /Game/Dir/Name → package dir + asset name; tolerate a trailing
	// .Object suffix if the caller passed an object path.
	int32 LastSlash = INDEX_NONE;
	if (!SystemPath.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Malformed system_path: %s"), *SystemPath),
			EMCPErrorCode::InvalidArgument,
			TEXT("`system_path` must be a full content path like /Game/FX/NS_Sparks."));
	}
	const FString PackagePath = SystemPath.Left(LastSlash);
	FString AssetName = SystemPath.Mid(LastSlash + 1);
	{
		FString NameOnly;
		if (AssetName.Split(TEXT("."), &NameOnly, nullptr)) { AssetName = NameOnly; }
	}
	const FString FullPackageName = PackagePath / AssetName;

	if (UEditorAssetLibrary::DoesAssetExist(FullPackageName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset already exists: %s"), *FullPackageName),
			EMCPErrorCode::NameCollision,
			TEXT("No silent overwrite. Pick a different system_path or delete the existing asset first."));
	}

	UPackage* Package = CreatePackage(*FullPackageName);
	if (!Package)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not create package: %s"), *FullPackageName),
			EMCPErrorCode::Internal,
			TEXT("CreatePackage returned null — verify the /Game/... path is writable."));
	}

	// Construct + initialize directly. We do NOT route through the factory's
	// IAssetTools::CreateAsset because UNiagaraSystemFactoryNew::ConfigureProperties
	// opens a modal Slate window that would hang headless MCP execution.
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Package, UNiagaraSystem::StaticClass(), FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!System)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("NewObject<UNiagaraSystem> returned null"),
			EMCPErrorCode::Internal,
			TEXT("Engine refused to construct the system; check the editor log."));
	}
	UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes*/true);

	FAssetRegistryModule::AssetCreated(System);
	System->MarkPackageDirty();
	RecompileSystem(System);
	if (!UEditorAssetLibrary::SaveAsset(FullPackageName, /*bOnlyIfIsDirty*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("System created in memory but failed to save to disk: %s"), *FullPackageName),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false, so nothing was written to disk — UEditorAssetSubsystem::SaveAsset bails (returns false) while a PIE session is active (GEditor->PlayWorld set). Stop PIE and retry."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("asset_path"), FullPackageName);
	Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_emitter_add ───────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraEmitterAdd(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	FString EmitterName;
	if (!Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'emitter_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `emitter_name` (string) — the name for the new emitter on the system."));
	}

	// Build a blank-but-valid emitter (output nodes present, no default renderer).
	// AddEmitterToSystem derives the handle name from the emitter's FName, so we
	// name the transient emitter here rather than via the unexported setter.
	UNiagaraEmitter* NewEmitter = NewObject<UNiagaraEmitter>(
		GetTransientPackage(), FName(*EmitterName), RF_Transactional);
	if (!NewEmitter)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("NewObject<UNiagaraEmitter> returned null"),
			EMCPErrorCode::Internal,
			TEXT("Engine refused to construct the emitter; check the editor log."));
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(NewEmitter, /*bAddDefaultModulesAndRenderers*/false);

	// Sim target — CPU by default (the InitializeEmitter default); GPU only on request.
	FString SimTargetStr;
	if (Params->TryGetStringField(TEXT("sim_target"), SimTargetStr)
		&& SimTargetStr.Equals(TEXT("gpu"), ESearchCase::IgnoreCase))
	{
		if (FVersionedNiagaraEmitterData* Data = NewEmitter->GetLatestEmitterData())
		{
			Data->SimTarget = ENiagaraSimTarget::GPUComputeSim;
		}
	}

	System->PreEditChange(nullptr);
	const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(
		*System, *NewEmitter, FGuid(), /*bCreateCopy*/true);
	System->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	// Resolve the resulting handle (the name may have been uniquified) for the reply.
	FString ResultName = EmitterName;
	int32 ResultIndex = INDEX_NONE;
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		if (Handles[i].GetId() == HandleId)
		{
			ResultName = Handles[i].GetName().ToString();
			ResultIndex = i;
			break;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter_name"), ResultName);
	Result->SetNumberField(TEXT("emitter_index"), ResultIndex);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_emitter_add_renderer ──────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraEmitterAddRenderer(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FString RendererType = TEXT("ribbon");
	Params->TryGetStringField(TEXT("renderer_type"), RendererType);
	RendererType = RendererType.ToLower();

	FVersionedNiagaraEmitter VerEmitter = Handle->GetInstance();
	UNiagaraEmitter* Emitter = VerEmitter.Emitter;
	if (!Emitter)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter instance is null"),
			EMCPErrorCode::Internal,
			TEXT("The emitter handle resolved but its UNiagaraEmitter is null — possible asset corruption."));
	}

	// Optional material (ribbon / sprite only).
	UMaterialInterface* Material = nullptr;
	FString MaterialPath;
	if (Params->TryGetStringField(TEXT("material_path"), MaterialPath) && !MaterialPath.IsEmpty())
	{
		Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Material)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Could not load material: %s"), *MaterialPath),
				EMCPErrorCode::AssetNotFound,
				TEXT("Verify the /Game/... material path exists."));
		}
	}

	UNiagaraRendererProperties* Props = nullptr;
	if (RendererType == TEXT("ribbon"))
	{
		UNiagaraRibbonRendererProperties* R =
			NewObject<UNiagaraRibbonRendererProperties>(Emitter, NAME_None, RF_Transactional);
		if (Material) { R->Material = Material; }
		Props = R;
	}
	else if (RendererType == TEXT("sprite"))
	{
		UNiagaraSpriteRendererProperties* R =
			NewObject<UNiagaraSpriteRendererProperties>(Emitter, NAME_None, RF_Transactional);
		if (Material) { R->Material = Material; }
		Props = R;
	}
	else if (RendererType == TEXT("mesh"))
	{
		// Mesh renderers take materials from the mesh / OverrideMaterials, not a
		// single Material field — material_path is ignored for this type.
		Props = NewObject<UNiagaraMeshRendererProperties>(Emitter, NAME_None, RF_Transactional);
	}
	else
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown renderer_type: '%s'"), *RendererType),
			EMCPErrorCode::InvalidArgument,
			TEXT("Supported: \"ribbon\" (default), \"sprite\", \"mesh\"."));
	}

	System->PreEditChange(nullptr);
	Emitter->AddRenderer(Props, VerEmitter.Version);
	System->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	int32 RendererIndex = INDEX_NONE;
	if (FVersionedNiagaraEmitterData* Data = VerEmitter.GetEmitterData())
	{
		RendererIndex = Data->GetRenderers().Num() - 1;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetStringField(TEXT("renderer_type"), RendererType);
	Result->SetNumberField(TEXT("renderer_index"), RendererIndex);
	if (Material) { Result->SetStringField(TEXT("material"), Material->GetPathName()); }
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_renderer_set_material ─────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraRendererSetMaterial(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `material_path` (string) — the /Game/... material to bind to the renderer."));
	}
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not load material: %s"), *MaterialPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the /Game/... material path exists."));
	}

	FVersionedNiagaraEmitterData* Data = Handle->GetInstance().GetEmitterData();
	if (!Data)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"), EMCPErrorCode::Internal,
			TEXT("Emitter handle valid but emitter data missing — open the asset and re-save."));
	}
	const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
	if (Renderers.Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter has no renderers"), EMCPErrorCode::NodeNotFound,
			TEXT("Add a renderer first with niagara_emitter_add_renderer."));
	}

	int32 RendererIndex = 0;
	double RendererIndexDbl;
	if (Params->TryGetNumberField(TEXT("renderer_index"), RendererIndexDbl))
	{
		RendererIndex = (int32)RendererIndexDbl;
	}
	if (RendererIndex < 0 || RendererIndex >= Renderers.Num())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("renderer_index %d out of range (0-%d)"), RendererIndex, Renderers.Num() - 1),
			EMCPErrorCode::OutOfRange,
			TEXT("`renderer_index` must be in [0, num_renderers). Use niagara_emitter_read or omit for index 0."));
	}

	UNiagaraRendererProperties* R = Renderers[RendererIndex];
	UNiagaraRibbonRendererProperties* Ribbon = Cast<UNiagaraRibbonRendererProperties>(R);
	UNiagaraSpriteRendererProperties* Sprite = Ribbon ? nullptr : Cast<UNiagaraSpriteRendererProperties>(R);
	if (!Ribbon && !Sprite)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Renderer type has no single Material property"),
			EMCPErrorCode::FeatureDisabled,
			TEXT("Only ribbon and sprite renderers expose a direct Material. Mesh renderers take materials from the mesh / override-materials array."));
	}

	System->PreEditChange(nullptr);
	if (Ribbon) { Ribbon->Material = Material; }
	else        { Sprite->Material = Material; }
	System->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetNumberField(TEXT("renderer_index"), RendererIndex);
	Result->SetStringField(TEXT("material"), Material->GetPathName());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_renderer_set_material_binding ─────────────────────────────────
//
// Bind a ribbon/sprite renderer's material to a User.* Material parameter
// (the renderer's MaterialUserParamBinding). This is the half that MCP could
// not previously reach: a C++ UNiagaraComponent::SetVariableMaterial("User.X",
// MID) only takes effect if the renderer's MaterialUserParamBinding points at
// "User.X". We set ONLY the binding's name — never its type. The renderer
// constructor pre-types MaterialUserParamBinding.Parameter to UMaterialInterface
// and the engine's own customization explicitly refuses to re-set the type
// (NiagaraTypeCustomizations.cpp). Mesh renderers have no such binding.

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraRendererSetMaterialBinding(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FString UserParamName;
	if (!Params->TryGetStringField(TEXT("user_param_name"), UserParamName) || UserParamName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'user_param_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `user_param_name` — the Material user parameter to bind, with or without the `User.` prefix (e.g. `RibbonMaterial` or `User.RibbonMaterial`). Create it first with add_niagara_user_parameter(type_name=\"material\")."));
	}
	// Renderer bindings resolve against the exposed store, whose names are
	// User.-namespaced — match the prefix add_niagara_user_parameter stores.
	const FName FullName = UserParamName.StartsWith(TEXT("User."))
		? FName(*UserParamName)
		: FName(*FString::Printf(TEXT("User.%s"), *UserParamName));

	// Validate the parameter exists and is a Material — a name that doesn't
	// resolve silently falls back to the renderer's plain Material at runtime,
	// so turn that into a loud error here.
	{
		bool bFound = false, bIsMaterial = false;
		for (const FNiagaraVariableWithOffset& V : System->GetExposedParameters().ReadParameterVariables())
		{
			if (V.GetName() == FullName)
			{
				bFound = true;
				const UClass* Cls = V.GetType().IsUObject() ? V.GetType().GetClass() : nullptr;
				bIsMaterial = Cls && Cls->IsChildOf(UMaterialInterface::StaticClass());
				break;
			}
		}
		if (!bFound)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("User parameter '%s' does not exist on this system"), *FullName.ToString()),
				EMCPErrorCode::NodeNotFound,
				TEXT("Create the Material user parameter first: add_niagara_user_parameter(parameter_name=..., type_name=\"material\"). Use read_niagara_system to list existing user parameters."));
		}
		if (!bIsMaterial)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("User parameter '%s' is not a Material"), *FullName.ToString()),
				EMCPErrorCode::InvalidArgument,
				TEXT("MaterialUserParamBinding can only bind a UMaterialInterface user parameter. Recreate it with type_name=\"material\"."));
		}
	}

	FVersionedNiagaraEmitterData* Data = Handle->GetInstance().GetEmitterData();
	if (!Data)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"), EMCPErrorCode::Internal,
			TEXT("Emitter handle valid but emitter data missing — open the asset and re-save."));
	}
	const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
	if (Renderers.Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter has no renderers"), EMCPErrorCode::NodeNotFound,
			TEXT("Add a renderer first with niagara_emitter_add_renderer."));
	}

	int32 RendererIndex = 0;
	double RendererIndexDbl;
	if (Params->TryGetNumberField(TEXT("renderer_index"), RendererIndexDbl))
	{
		RendererIndex = (int32)RendererIndexDbl;
	}
	if (RendererIndex < 0 || RendererIndex >= Renderers.Num())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("renderer_index %d out of range (0-%d)"), RendererIndex, Renderers.Num() - 1),
			EMCPErrorCode::OutOfRange,
			TEXT("`renderer_index` must be in [0, num_renderers). Use niagara_emitter_read or omit for index 0."));
	}

	UNiagaraRendererProperties* R = Renderers[RendererIndex];
	UNiagaraRibbonRendererProperties* Ribbon = Cast<UNiagaraRibbonRendererProperties>(R);
	UNiagaraSpriteRendererProperties* Sprite = Ribbon ? nullptr : Cast<UNiagaraSpriteRendererProperties>(R);
	if (!Ribbon && !Sprite)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Renderer type has no MaterialUserParamBinding"),
			EMCPErrorCode::FeatureDisabled,
			TEXT("Only ribbon and sprite renderers expose MaterialUserParamBinding. Mesh renderers take materials from the mesh / override-materials array."));
	}

	System->PreEditChange(nullptr);
	R->Modify();
	// Set ONLY the name — the type is pre-bound to UMaterialInterface by the
	// renderer ctor and must not be overwritten.
	if (Ribbon) { Ribbon->MaterialUserParamBinding.Parameter.SetName(FullName); }
	else        { Sprite->MaterialUserParamBinding.Parameter.SetName(FullName); }
	System->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetNumberField(TEXT("renderer_index"), RendererIndex);
	Result->SetStringField(TEXT("user_param_name"), FullName.ToString());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_module_add ────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraModuleAdd(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FString TargetUsageStr;
	if (!Params->TryGetStringField(TEXT("target_usage"), TargetUsageStr))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target_usage'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `target_usage`. Valid: \"ParticleSpawn\", \"ParticleUpdate\", \"EmitterSpawn\", \"EmitterUpdate\", \"SystemSpawn\", \"SystemUpdate\"."));
	}
	ENiagaraScriptUsage TargetUsage;
	if (!ParseScriptUsage(TargetUsageStr, TargetUsage))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown script usage: '%s'"), *TargetUsageStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("Closed set: \"ParticleSpawn\", \"ParticleUpdate\", \"EmitterSpawn\", \"EmitterUpdate\", \"SystemSpawn\", \"SystemUpdate\"."));
	}

	FString ModuleScriptPath;
	if (!Params->TryGetStringField(TEXT("module_script_path"), ModuleScriptPath) || ModuleScriptPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'module_script_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `module_script_path` — the UNiagaraScript module asset, e.g. /Niagara/Modules/Emitter/SpawnPerUnit.SpawnPerUnit."));
	}
	UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *ModuleScriptPath);
	if (!ModuleScript)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not load module script: %s"), *ModuleScriptPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the module script path. Standard modules live under /Niagara/Modules/..."));
	}

	double TargetIndexDbl = -1;
	Params->TryGetNumberField(TEXT("target_index"), TargetIndexDbl);
	const int32 TargetIndex = (int32)TargetIndexDbl;

	FVersionedNiagaraEmitterData* EmitterData = Handle->GetInstance().GetEmitterData();
	if (!EmitterData)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"), EMCPErrorCode::Internal,
			TEXT("Emitter handle valid but emitter data missing — open the asset and re-save."));
	}

	UNiagaraScript* TargetScript = EmitterData->GetScript(TargetUsage, FGuid());
	if (!TargetScript)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("No script for usage '%s' on this emitter"), *TargetUsageStr),
			EMCPErrorCode::NodeNotFound,
			TEXT("Emitter has no script for that usage. Use niagara_emitter_read to see scripts."));
	}
	UNiagaraScriptSource* TargetSource = Cast<UNiagaraScriptSource>(TargetScript->GetLatestSource());
	if (!TargetSource || !TargetSource->NodeGraph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Could not access target script graph"), EMCPErrorCode::Internal,
			TEXT("Niagara script source graph missing editor data; open + re-save the asset."));
	}
	UNiagaraGraph* TargetGraph = TargetSource->NodeGraph;

	// Find the output node for the target usage — same graph walk as the
	// scratch-pad handler (avoids the unexported ResetGraphForOutput).
	TArray<UNiagaraNodeOutput*> OutputNodes;
	TargetGraph->GetNodesOfClass(OutputNodes);
	UNiagaraNodeOutput* TargetOutputNode = nullptr;
	for (UNiagaraNodeOutput* OutNode : OutputNodes)
	{
		if (OutNode->GetUsage() == TargetUsage)
		{
			TargetOutputNode = OutNode;
			break;
		}
	}
	if (!TargetOutputNode)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Could not find output node for target usage"), EMCPErrorCode::Internal,
			TEXT("Target script graph has no UNiagaraNodeOutput for that usage; verify it compiles in the editor."));
	}

	System->PreEditChange(nullptr);
	UNiagaraNodeFunctionCall* ModuleNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *TargetOutputNode, TargetIndex);
	System->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetStringField(TEXT("target_usage"), TargetUsageStr);
	Result->SetStringField(TEXT("module_script"), ModuleScriptPath);
	Result->SetBoolField(TEXT("module_node_created"), ModuleNode != nullptr);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_emitter_set_local_space ───────────────────────────────────────
//
// Set an emitter's Local Space flag. Local space = particles simulate in the
// emitter/owner frame (they move + rotate WITH the spawning component) instead
// of world space. Essential for effects authored relative to a moving actor —
// e.g. exhaust / speed streaks that shoot BACKWARD off a vehicle in its own
// frame, correct regardless of the actor's world heading.
TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraEmitterSetLocalSpace(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	bool bLocalSpace = false;
	if (!Params->TryGetBoolField(TEXT("local_space"), bLocalSpace))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'local_space'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `local_space` (bool) — true = simulate in the emitter/owner frame, false = world space."));
	}

	FVersionedNiagaraEmitter VerEmitter = Handle->GetInstance();
	UNiagaraEmitter* Emitter = VerEmitter.Emitter;
	FVersionedNiagaraEmitterData* Data = VerEmitter.GetEmitterData();
	if (!Emitter || !Data)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter instance/data is null"), EMCPErrorCode::Internal,
			TEXT("The emitter handle resolved but its data is missing — open the asset and re-save."));
	}

	Emitter->PreEditChange(nullptr);
	Data->bLocalSpace = bLocalSpace;
	Emitter->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetBoolField(TEXT("local_space"), bLocalSpace);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_renderer_set_alignment ────────────────────────────────────────
//
// Set a SPRITE renderer's Alignment + FacingMode. VelocityAligned orients each
// sprite's long axis along its velocity → camera-facing STREAKS (speed lines)
// instead of round billboards. Only sprite renderers expose these.
//   alignment ∈ {unaligned, velocity, custom, automatic}
//   facing    ∈ {camera, camera_plane, custom, camera_position, distance_blend, automatic}
TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraRendererSetAlignment(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FVersionedNiagaraEmitterData* Data = Handle->GetInstance().GetEmitterData();
	if (!Data)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"), EMCPErrorCode::Internal,
			TEXT("Emitter handle valid but data missing — open the asset and re-save."));
	}
	const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
	if (Renderers.Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter has no renderers"), EMCPErrorCode::NodeNotFound,
			TEXT("Add a sprite renderer first with niagara_emitter_add_renderer (renderer_type=\"sprite\")."));
	}

	int32 RendererIndex = 0;
	double RendererIndexDbl;
	if (Params->TryGetNumberField(TEXT("renderer_index"), RendererIndexDbl)) RendererIndex = (int32)RendererIndexDbl;
	if (RendererIndex < 0 || RendererIndex >= Renderers.Num())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("renderer_index %d out of range (0-%d)"), RendererIndex, Renderers.Num() - 1),
			EMCPErrorCode::OutOfRange,
			TEXT("`renderer_index` must be in [0, num_renderers). Use niagara_emitter_read or omit for index 0."));
	}

	UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderers[RendererIndex]);
	if (!Sprite)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Renderer is not a sprite renderer"),
			EMCPErrorCode::FeatureDisabled,
			TEXT("Alignment/FacingMode are sprite-renderer properties; this renderer index is a ribbon or mesh renderer."));
	}

	// Parse + validate BEFORE mutating (so an error leaves the asset untouched).
	ENiagaraSpriteAlignment NewAlign = ENiagaraSpriteAlignment::Unaligned; bool bSetAlign = false;
	FString AlignStr;
	if (Params->TryGetStringField(TEXT("alignment"), AlignStr))
	{
		const FString A = AlignStr.ToLower();
		if      (A == TEXT("unaligned")) NewAlign = ENiagaraSpriteAlignment::Unaligned;
		else if (A == TEXT("velocity"))  NewAlign = ENiagaraSpriteAlignment::VelocityAligned;
		else if (A == TEXT("custom"))    NewAlign = ENiagaraSpriteAlignment::CustomAlignment;
		else if (A == TEXT("automatic")) NewAlign = ENiagaraSpriteAlignment::Automatic;
		else return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown alignment: '%s'"), *AlignStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("alignment must be one of: unaligned, velocity, custom, automatic."));
		bSetAlign = true;
	}
	ENiagaraSpriteFacingMode NewFace = ENiagaraSpriteFacingMode::FaceCamera; bool bSetFace = false;
	FString FaceStr;
	if (Params->TryGetStringField(TEXT("facing"), FaceStr))
	{
		const FString F = FaceStr.ToLower();
		if      (F == TEXT("camera"))          NewFace = ENiagaraSpriteFacingMode::FaceCamera;
		else if (F == TEXT("camera_plane"))    NewFace = ENiagaraSpriteFacingMode::FaceCameraPlane;
		else if (F == TEXT("custom"))          NewFace = ENiagaraSpriteFacingMode::CustomFacingVector;
		else if (F == TEXT("camera_position")) NewFace = ENiagaraSpriteFacingMode::FaceCameraPosition;
		else if (F == TEXT("distance_blend"))  NewFace = ENiagaraSpriteFacingMode::FaceCameraDistanceBlend;
		else if (F == TEXT("automatic"))       NewFace = ENiagaraSpriteFacingMode::Automatic;
		else return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown facing: '%s'"), *FaceStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("facing must be one of: camera, camera_plane, custom, camera_position, distance_blend, automatic."));
		bSetFace = true;
	}
	if (!bSetAlign && !bSetFace)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Nothing to set"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass at least one of `alignment` or `facing`."));
	}

	System->PreEditChange(nullptr);
	if (bSetAlign) { Sprite->Alignment = NewAlign; }
	if (bSetFace)  { Sprite->FacingMode = NewFace; }
	System->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetNumberField(TEXT("renderer_index"), RendererIndex);
	if (bSetAlign) { Result->SetStringField(TEXT("alignment"), AlignStr.ToLower()); }
	if (bSetFace)  { Result->SetStringField(TEXT("facing"), FaceStr.ToLower()); }
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_mesh_renderer_set_mesh ────────────────────────────────────────
//
// Assign a UStaticMesh (and optional uniform scale) to a MESH renderer's
// Meshes[0] entry. A mesh renderer added via niagara_emitter_add_renderer starts
// with an EMPTY Meshes array (no MCP path set one) and therefore renders NOTHING
// — this is the missing half. Per-particle materials come from the assigned
// mesh's own material slots (bake the look into the mesh's slot 0 with
// mesh_set_static_mesh_material), and per-particle tint/relative-time flow to the
// material via the standard Particle Color / Particle Relative Time nodes.
TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraMeshRendererSetMesh(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FString MeshPath;
	if (!Params->TryGetStringField(TEXT("mesh_path"), MeshPath) || MeshPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'mesh_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `mesh_path` (string) — the /Game/... UStaticMesh to render per particle."));
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not load static mesh: %s"), *MeshPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the /Game/... static mesh path exists."));
	}

	FVersionedNiagaraEmitterData* Data = Handle->GetInstance().GetEmitterData();
	if (!Data)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"), EMCPErrorCode::Internal,
			TEXT("Emitter handle valid but data missing — open the asset and re-save."));
	}
	const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
	if (Renderers.Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter has no renderers"), EMCPErrorCode::NodeNotFound,
			TEXT("Add a mesh renderer first with niagara_emitter_add_renderer (renderer_type=\"mesh\")."));
	}

	int32 RendererIndex = 0;
	double RendererIndexDbl;
	if (Params->TryGetNumberField(TEXT("renderer_index"), RendererIndexDbl)) RendererIndex = (int32)RendererIndexDbl;
	if (RendererIndex < 0 || RendererIndex >= Renderers.Num())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("renderer_index %d out of range (0-%d)"), RendererIndex, Renderers.Num() - 1),
			EMCPErrorCode::OutOfRange,
			TEXT("`renderer_index` must be in [0, num_renderers). Use niagara_emitter_read or omit for index 0."));
	}

	UNiagaraMeshRendererProperties* MeshProps = Cast<UNiagaraMeshRendererProperties>(Renderers[RendererIndex]);
	if (!MeshProps)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Renderer is not a mesh renderer"),
			EMCPErrorCode::FeatureDisabled,
			TEXT("This renderer index is a sprite or ribbon renderer; add a mesh renderer with renderer_type=\"mesh\"."));
	}

	// Optional uniform scale — the engine BasicShapes/Cube is 100cm, so small
	// particles need scaling down (e.g. 0.25 → 25cm cubes).
	bool bHasScale = false;
	double ScaleVal = 1.0;
	if (Params->TryGetNumberField(TEXT("scale"), ScaleVal)) bHasScale = true;

	System->PreEditChange(nullptr);
	if (MeshProps->Meshes.Num() == 0)
	{
		MeshProps->Meshes.AddDefaulted();
	}
	FNiagaraMeshRendererMeshProperties& Entry = MeshProps->Meshes[0];
	Entry.Mesh = Mesh;
	if (bHasScale) { Entry.Scale = FVector(ScaleVal); }
	MeshProps->PostEditChange();
	System->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetNumberField(TEXT("renderer_index"), RendererIndex);
	Result->SetStringField(TEXT("mesh"), Mesh->GetPathName());
	if (bHasScale) { Result->SetNumberField(TEXT("scale"), ScaleVal); }
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── niagara_renderer_set_enabled ──────────────────────────────────────────
//
// Enable/disable a single renderer on an emitter (UNiagaraRendererProperties::
// SetIsEnabled). A disabled renderer keeps its config but draws nothing — the
// non-destructive way to silence a vestigial renderer (e.g. the leftover sprite
// renderer that a mesh-particle emitter inherits when duplicated from a sprite
// system, which otherwise co-draws round DefaultSpriteMaterial billboards over
// the mesh). There is no MCP "remove renderer", and zeroing a particle attribute
// (Sprite Size) does NOT suppress a renderer whose size isn't bound to it — so
// disabling the renderer is the reliable, cook-safe fix. The result always lists
// ALL renderers (index, type, enabled) so callers can confirm which is which
// without a separate read.
TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleNiagaraRendererSetEnabled(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	bool bEnabled = false;
	if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'enabled'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `enabled` (bool) — true to show the renderer, false to silence it."));
	}

	FVersionedNiagaraEmitterData* Data = Handle->GetInstance().GetEmitterData();
	if (!Data)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"), EMCPErrorCode::Internal,
			TEXT("Emitter handle valid but data missing — open the asset and re-save."));
	}
	const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
	if (Renderers.Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter has no renderers"), EMCPErrorCode::NodeNotFound,
			TEXT("Add a renderer first with niagara_emitter_add_renderer."));
	}

	int32 RendererIndex = 0;
	double RendererIndexDbl;
	if (Params->TryGetNumberField(TEXT("renderer_index"), RendererIndexDbl)) RendererIndex = (int32)RendererIndexDbl;
	if (RendererIndex < 0 || RendererIndex >= Renderers.Num())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("renderer_index %d out of range (0-%d)"), RendererIndex, Renderers.Num() - 1),
			EMCPErrorCode::OutOfRange,
			TEXT("`renderer_index` must be in [0, num_renderers). The result lists every renderer's index/type."));
	}

	UNiagaraRendererProperties* R = Renderers[RendererIndex];
	System->PreEditChange(nullptr);
	R->SetIsEnabled(bEnabled);
	R->PostEditChange();
	System->PostEditChange();
	System->MarkPackageDirty();
	RecompileSystem(System);
	SaveNiagaraSystemPackage(System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetNumberField(TEXT("renderer_index"), RendererIndex);
	Result->SetStringField(TEXT("renderer_type"), R->GetClass()->GetName());
	Result->SetBoolField(TEXT("enabled"), bEnabled);

	// Echo every renderer so the caller can see the full layout (which index is the
	// sprite vs the mesh) without a separate read.
	TArray<TSharedPtr<FJsonValue>> RendererArray;
	for (int32 i = 0; i < Renderers.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), i);
		Obj->SetStringField(TEXT("type"), Renderers[i] ? Renderers[i]->GetClass()->GetName() : TEXT("null"));
		Obj->SetBoolField(TEXT("enabled"), Renderers[i] ? Renderers[i]->GetIsEnabled() : false);
		RendererArray.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("renderers"), RendererArray);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}
