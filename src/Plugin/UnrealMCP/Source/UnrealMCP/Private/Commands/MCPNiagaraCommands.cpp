#include "Commands/MCPNiagaraCommands.h"
#include "Commands/MCPNiagaraCommands_Internal.h"
#include "Commands/MCPCommonUtils.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraTypes.h"
#include "NiagaraParameterStore.h"
#include "NiagaraDataInterface.h"
#include "NiagaraScript.h"
#include "NiagaraCommon.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraScratchPadContainer.h"

// NiagaraEditor — scratch pad module creation, graph manipulation
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeCustomHlsl.h"
#include "EdGraphSchema_Niagara.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Materials/MaterialInterface.h" // Material user-parameter type resolution

// ── Helpers ──────────────────────────────────────────────────────────────
//
// LoadNiagaraSystemFromParam, FindEmitterHandle, and ParseScriptUsage now live
// in MCPNiagaraCommands_Internal.h — shared with the authoring split
// (MCPNiagaraCommands_Create.cpp).

static FString NiagaraTypeToString(const FNiagaraTypeDefinition& TypeDef)
{
	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef()) return TEXT("float");
	if (TypeDef == FNiagaraTypeDefinition::GetIntDef()) return TEXT("int32");
	if (TypeDef == FNiagaraTypeDefinition::GetBoolDef()) return TEXT("bool");
	if (TypeDef == FNiagaraTypeDefinition::GetVec2Def()) return TEXT("vector2");
	if (TypeDef == FNiagaraTypeDefinition::GetVec3Def()) return TEXT("vector3");
	if (TypeDef == FNiagaraTypeDefinition::GetVec4Def()) return TEXT("vector4");
	if (TypeDef == FNiagaraTypeDefinition::GetColorDef()) return TEXT("linear_color");
	if (TypeDef == FNiagaraTypeDefinition::GetPositionDef()) return TEXT("position");
	if (TypeDef.IsDataInterface())
	{
		const UClass* DIClass = TypeDef.GetClass();
		return DIClass ? FString::Printf(TEXT("di:%s"), *DIClass->GetName()) : TEXT("data_interface");
	}
	if (TypeDef.IsEnum())
	{
		const UEnum* Enum = TypeDef.GetEnum();
		return Enum ? FString::Printf(TEXT("enum:%s"), *Enum->GetName()) : TEXT("enum");
	}
	// Non-DataInterface UObject params (e.g. a Material user parameter) carry a
	// class, not a script struct — surface the class name instead of "unknown".
	if (TypeDef.IsUObject())
	{
		const UClass* Cls = TypeDef.GetClass();
		if (Cls && Cls->IsChildOf(UMaterialInterface::StaticClass())) return TEXT("material");
		return Cls ? FString::Printf(TEXT("uobject:%s"), *Cls->GetName()) : TEXT("uobject");
	}
	const UScriptStruct* Struct = TypeDef.GetScriptStruct();
	return Struct ? Struct->GetName() : TEXT("unknown");
}

static bool StringToNiagaraType(const FString& TypeName, FNiagaraTypeDefinition& OutType)
{
	if (TypeName.Equals(TEXT("float"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition::GetFloatDef(); return true; }
	if (TypeName.Equals(TEXT("int32"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("int"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition::GetIntDef(); return true; }
	if (TypeName.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition::GetBoolDef(); return true; }
	if (TypeName.Equals(TEXT("vector2"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("vec2"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition::GetVec2Def(); return true; }
	if (TypeName.Equals(TEXT("vector3"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("vec3"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition::GetVec3Def(); return true; }
	if (TypeName.Equals(TEXT("vector4"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("vec4"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition::GetVec4Def(); return true; }
	if (TypeName.Equals(TEXT("linear_color"), ESearchCase::IgnoreCase) || TypeName.Equals(TEXT("color"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition::GetColorDef(); return true; }
	if (TypeName.Equals(TEXT("position"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition::GetPositionDef(); return true; }

	// Material (UObject) user parameter — a User.* Material slot a renderer's
	// MaterialUserParamBinding can point at, or a C++ SetVariableMaterial target.
	// Constructed as a class-typed FNiagaraTypeDefinition (UT_Class), exactly
	// like the renderer constructors do (NiagaraRibbonRendererProperties.cpp).
	if (TypeName.Equals(TEXT("material"), ESearchCase::IgnoreCase) ||
		TypeName.Equals(TEXT("UMaterialInterface"), ESearchCase::IgnoreCase))
	{ OutType = FNiagaraTypeDefinition(UMaterialInterface::StaticClass()); return true; }

	// Data interface class lookup — accept "di:ClassName" or just "ClassName"
	FString DIClassName = TypeName;
	if (DIClassName.StartsWith(TEXT("di:")))
	{
		DIClassName = DIClassName.RightChop(3);
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UNiagaraDataInterface::StaticClass())) continue;
		if (It->HasAnyClassFlags(CLASS_Abstract)) continue;
		if (It->GetName() == DIClassName ||
			It->GetName() == FString::Printf(TEXT("NiagaraDataInterface%s"), *DIClassName))
		{
			OutType = FNiagaraTypeDefinition(*It);
			return true;
		}
	}
	return false;
}

static void AppendValueToJson(TSharedPtr<FJsonObject>& Obj,
	const FNiagaraTypeDefinition& TypeDef, const uint8* Data)
{
	if (!Data) return;

	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
	{
		Obj->SetNumberField(TEXT("value"), *reinterpret_cast<const float*>(Data));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
	{
		Obj->SetNumberField(TEXT("value"), *reinterpret_cast<const int32*>(Data));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
	{
		Obj->SetBoolField(TEXT("value"), (*reinterpret_cast<const int32*>(Data)) != 0);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
	{
		const FVector2f* V = reinterpret_cast<const FVector2f*>(Data);
		Obj->SetNumberField(TEXT("x"), V->X);
		Obj->SetNumberField(TEXT("y"), V->Y);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def() ||
			 TypeDef == FNiagaraTypeDefinition::GetPositionDef())
	{
		const FVector3f* V = reinterpret_cast<const FVector3f*>(Data);
		Obj->SetNumberField(TEXT("x"), V->X);
		Obj->SetNumberField(TEXT("y"), V->Y);
		Obj->SetNumberField(TEXT("z"), V->Z);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
	{
		const FVector4f* V = reinterpret_cast<const FVector4f*>(Data);
		Obj->SetNumberField(TEXT("x"), V->X);
		Obj->SetNumberField(TEXT("y"), V->Y);
		Obj->SetNumberField(TEXT("z"), V->Z);
		Obj->SetNumberField(TEXT("w"), V->W);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
	{
		const FLinearColor* C = reinterpret_cast<const FLinearColor*>(Data);
		Obj->SetNumberField(TEXT("r"), C->R);
		Obj->SetNumberField(TEXT("g"), C->G);
		Obj->SetNumberField(TEXT("b"), C->B);
		Obj->SetNumberField(TEXT("a"), C->A);
	}
}

// Outcome of parsing a JSON payload into a Niagara variable's data block.
// The caller maps non-Ok outcomes onto its own handler-specific error
// message/hint so the two setters' response strings stay exactly as they
// were before the dispatch chains were collapsed into this helper.
enum class ENiagaraValueParse : uint8
{
	Ok,
	MissingNumber,    // float/int type but no numeric `value` field
	MissingBool,      // bool type but no boolean `value` field
	UnsupportedType,  // type outside the supported seven-type set
};

// Write-side mirror of AppendValueToJson — one branch per supported wire
// type (float, int32, bool, vec2/3/4, position [vec3 layout], linear_color).
// Vector/color components zero-fill when omitted (a=1 default for color),
// matching the pre-extraction semantics of both setters. SetVar must be
// constructed with the (TypeDef, Name) pair; AllocateData + SetData happen
// here on success. No partial state on failure: every failing branch
// returns before SetData.
static ENiagaraValueParse ParseNiagaraValueFromJson(
	const FNiagaraTypeDefinition& TypeDef,
	const TSharedPtr<FJsonObject>& Params,
	FNiagaraVariable& SetVar)
{
	SetVar.AllocateData();

	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
	{
		double Val; if (!Params->TryGetNumberField(TEXT("value"), Val))
			return ENiagaraValueParse::MissingNumber;
		float F = (float)Val;
		SetVar.SetData(reinterpret_cast<const uint8*>(&F));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
	{
		double Val; if (!Params->TryGetNumberField(TEXT("value"), Val))
			return ENiagaraValueParse::MissingNumber;
		int32 I = (int32)Val;
		SetVar.SetData(reinterpret_cast<const uint8*>(&I));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
	{
		bool Val; if (!Params->TryGetBoolField(TEXT("value"), Val))
			return ENiagaraValueParse::MissingBool;
		int32 B = Val ? 1 : 0;
		SetVar.SetData(reinterpret_cast<const uint8*>(&B));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
	{
		double X = 0, Y = 0;
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
		FVector2f V((float)X, (float)Y);
		SetVar.SetData(reinterpret_cast<const uint8*>(&V));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def() ||
			 TypeDef == FNiagaraTypeDefinition::GetPositionDef())
	{
		double X = 0, Y = 0, Z = 0;
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
		Params->TryGetNumberField(TEXT("z"), Z);
		FVector3f V((float)X, (float)Y, (float)Z);
		SetVar.SetData(reinterpret_cast<const uint8*>(&V));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
	{
		double X = 0, Y = 0, Z = 0, W = 0;
		Params->TryGetNumberField(TEXT("x"), X);
		Params->TryGetNumberField(TEXT("y"), Y);
		Params->TryGetNumberField(TEXT("z"), Z);
		Params->TryGetNumberField(TEXT("w"), W);
		FVector4f V((float)X, (float)Y, (float)Z, (float)W);
		SetVar.SetData(reinterpret_cast<const uint8*>(&V));
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
	{
		double R = 0, G = 0, B = 0, A = 1;
		Params->TryGetNumberField(TEXT("r"), R);
		Params->TryGetNumberField(TEXT("g"), G);
		Params->TryGetNumberField(TEXT("b"), B);
		Params->TryGetNumberField(TEXT("a"), A);
		FLinearColor C((float)R, (float)G, (float)B, (float)A);
		SetVar.SetData(reinterpret_cast<const uint8*>(&C));
	}
	else
	{
		return ENiagaraValueParse::UnsupportedType;
	}
	return ENiagaraValueParse::Ok;
}

static TSharedPtr<FJsonObject> UserParamToJson(
	const FNiagaraVariableWithOffset& Var, const FNiagaraParameterStore& Store)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Var.GetName().ToString());

	FNiagaraTypeDefinition TypeDef = Var.GetType();
	Obj->SetStringField(TEXT("type"), NiagaraTypeToString(TypeDef));

	if (!TypeDef.IsDataInterface())
	{
		const uint8* Data = Store.GetParameterData(Var);
		AppendValueToJson(Obj, TypeDef, Data);
	}
	else
	{
		Obj->SetStringField(TEXT("value"), TEXT("<data_interface>"));
	}
	return Obj;
}

// FindEmitterHandle moved to MCPNiagaraCommands_Internal.h.

// ── Command routing ──────────────────────────────────────────────────────

FMCPNiagaraCommands::FMCPNiagaraCommands() {}

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("niagara_list_systems"))       return HandleListNiagaraSystems(Params);
	if (CommandType == TEXT("niagara_system_read"))        return HandleReadNiagaraSystem(Params);
	if (CommandType == TEXT("niagara_emitter_read"))       return HandleReadNiagaraEmitter(Params);
	if (CommandType == TEXT("niagara_user_parameter_add")) return HandleAddNiagaraUserParameter(Params);
	if (CommandType == TEXT("niagara_user_parameter_remove")) return HandleRemoveNiagaraUserParameter(Params);
	if (CommandType == TEXT("niagara_user_parameter_set")) return HandleSetNiagaraUserParameterValue(Params);
	if (CommandType == TEXT("niagara_emitter_set_enabled")) return HandleSetNiagaraEmitterEnabled(Params);
	if (CommandType == TEXT("niagara_module_get_inputs"))  return HandleGetNiagaraModuleInputs(Params);
	if (CommandType == TEXT("niagara_module_set_input"))   return HandleSetNiagaraModuleInput(Params);
	if (CommandType == TEXT("niagara_scratch_pad_module_add")) return HandleAddNiagaraScratchPadModule(Params);
	if (CommandType == TEXT("niagara_system_create"))         return HandleNiagaraSystemCreate(Params);
	if (CommandType == TEXT("niagara_emitter_add"))           return HandleNiagaraEmitterAdd(Params);
	if (CommandType == TEXT("niagara_emitter_add_renderer"))  return HandleNiagaraEmitterAddRenderer(Params);
	if (CommandType == TEXT("niagara_renderer_set_material")) return HandleNiagaraRendererSetMaterial(Params);
	if (CommandType == TEXT("niagara_renderer_set_material_binding")) return HandleNiagaraRendererSetMaterialBinding(Params);
	if (CommandType == TEXT("niagara_module_add"))            return HandleNiagaraModuleAdd(Params);
	if (CommandType == TEXT("niagara_emitter_set_local_space")) return HandleNiagaraEmitterSetLocalSpace(Params);
	if (CommandType == TEXT("niagara_renderer_set_alignment")) return HandleNiagaraRendererSetAlignment(Params);
	if (CommandType == TEXT("niagara_mesh_renderer_set_mesh")) return HandleNiagaraMeshRendererSetMesh(Params);
	if (CommandType == TEXT("niagara_renderer_set_enabled")) return HandleNiagaraRendererSetEnabled(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown Niagara command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("Valid Niagara commands: list_niagara_systems, read_niagara_system, read_niagara_emitter, add_niagara_user_parameter, remove_niagara_user_parameter, set_niagara_user_parameter_value, set_niagara_emitter_enabled, get_niagara_module_inputs, set_niagara_module_input, add_niagara_scratch_pad_module, niagara_system_create, niagara_emitter_add, niagara_emitter_add_renderer, niagara_renderer_set_material, niagara_renderer_set_material_binding, niagara_module_add, niagara_emitter_set_local_space, niagara_renderer_set_alignment, niagara_mesh_renderer_set_mesh, niagara_renderer_set_enabled."));
}

// ── list_niagara_systems ─────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleListNiagaraSystems(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter;
	Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByClass(
		FTopLevelAssetPath(TEXT("/Script/Niagara"), TEXT("NiagaraSystem")), Assets);

	TArray<TSharedPtr<FJsonValue>> SystemArray;
	for (const FAssetData& Asset : Assets)
	{
		FString Path = Asset.GetObjectPathString();
		if (!Path.StartsWith(PathFilter)) continue;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Obj->SetStringField(TEXT("path"), Path);
		Obj->SetStringField(TEXT("package"), Asset.PackageName.ToString());
		SystemArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("systems"), SystemArray);
	Result->SetNumberField(TEXT("count"), SystemArray.Num());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── read_niagara_system ──────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleReadNiagaraSystem(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	// Emitters
	TArray<TSharedPtr<FJsonValue>> EmitterArray;
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& Handle = Handles[i];
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), i);
		Obj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		Obj->SetStringField(TEXT("id"), Handle.GetId().ToString());
		Obj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EmitterArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	// User parameters
	const FNiagaraUserRedirectionParameterStore& ParamStore = System->GetExposedParameters();
	TArray<TSharedPtr<FJsonValue>> ParamArray;
	TArrayView<const FNiagaraVariableWithOffset> Variables = ParamStore.ReadParameterVariables();
	for (const FNiagaraVariableWithOffset& Var : Variables)
	{
		ParamArray.Add(MakeShared<FJsonValueObject>(UserParamToJson(Var, ParamStore)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("name"), System->GetName());
	Result->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	Result->SetNumberField(TEXT("emitter_count"), Handles.Num());
	Result->SetArrayField(TEXT("emitters"), EmitterArray);
	Result->SetNumberField(TEXT("user_parameter_count"), Variables.Num());
	Result->SetArrayField(TEXT("user_parameters"), ParamArray);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── read_niagara_emitter ─────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleReadNiagaraEmitter(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FVersionedNiagaraEmitter VerEmitter = Handle->GetInstance();
	FVersionedNiagaraEmitterData* EmitterData = VerEmitter.GetEmitterData();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetNumberField(TEXT("index"), EmitterIdx);
	Result->SetStringField(TEXT("name"), Handle->GetName().ToString());
	Result->SetStringField(TEXT("id"), Handle->GetId().ToString());
	Result->SetBoolField(TEXT("enabled"), Handle->GetIsEnabled());

	if (EmitterData)
	{
		Result->SetStringField(TEXT("sim_target"),
			EmitterData->SimTarget == ENiagaraSimTarget::CPUSim ? TEXT("cpu") : TEXT("gpu"));

		// List scripts present on this emitter
		TArray<UNiagaraScript*> Scripts;
		EmitterData->GetScripts(Scripts, false, false);

		TArray<TSharedPtr<FJsonValue>> ScriptArray;
		for (UNiagaraScript* Script : Scripts)
		{
			if (!Script) continue;
			TSharedPtr<FJsonObject> ScriptObj = MakeShared<FJsonObject>();
			ScriptObj->SetStringField(TEXT("name"), Script->GetName());
			ScriptObj->SetStringField(TEXT("usage"),
				StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(
					static_cast<int64>(Script->GetUsage())));
			ScriptObj->SetStringField(TEXT("usage_id"), Script->GetUsageId().ToString());

			// Count rapid iteration parameters
			const FNiagaraParameterStore& RIStore = Script->RapidIterationParameters;
			ScriptObj->SetNumberField(TEXT("rapid_iteration_param_count"),
				RIStore.ReadParameterVariables().Num());

			ScriptArray.Add(MakeShared<FJsonValueObject>(ScriptObj));
		}
		Result->SetArrayField(TEXT("scripts"), ScriptArray);
		Result->SetNumberField(TEXT("script_count"), ScriptArray.Num());
	}

	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── add_niagara_user_parameter ───────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleAddNiagaraUserParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	FString ParamName;
	if (!Params->TryGetStringField(TEXT("parameter_name"), ParamName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'parameter_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `parameter_name` (string) — the user parameter name on the Niagara System (without `User.` prefix). Use `niagara_system_read` to discover."));
	}

	FString TypeName;
	if (!Params->TryGetStringField(TEXT("type_name"), TypeName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'type_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `type_name` (string). Valid: \"float\", \"int32\", \"bool\", \"vector2\", \"vector3\", \"vector4\", \"linear_color\", \"position\", or a data interface class name (e.g. \"NiagaraDataInterfaceWater\" or \"di:Water\")."));
	}

	FNiagaraTypeDefinition TypeDef;
	if (!StringToNiagaraType(TypeName, TypeDef))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown Niagara type: '%s'. Use float, int32, bool, vector2, "
				"vector3, vector4, linear_color, position, or a data interface class name "
				"(e.g. 'NiagaraDataInterfaceWater' or 'di:Water')"), *TypeName),
			EMCPErrorCode::ClassNotLoaded,
			TEXT("`type_name` didn't resolve to a built-in Niagara type or a UNiagaraDataInterface subclass. Verify the spelling, or use the `di:` prefix for data interfaces."));
	}

	// Prepend "User." namespace if not already present
	FName FullName = ParamName.StartsWith(TEXT("User."))
		? FName(*ParamName)
		: FName(*FString::Printf(TEXT("User.%s"), *ParamName));

	// Check for duplicate
	FNiagaraUserRedirectionParameterStore& ParamStore = System->GetExposedParameters();
	TArrayView<const FNiagaraVariableWithOffset> Existing = ParamStore.ReadParameterVariables();
	for (const FNiagaraVariableWithOffset& V : Existing)
	{
		if (V.GetName() == FullName)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("User parameter '%s' already exists"), *FullName.ToString()),
				EMCPErrorCode::NameCollision,
				TEXT("Pick a different `parameter_name`, or use `niagara_user_parameter_remove` to delete the existing one first."));
		}
	}

	FNiagaraVariable NewVar(TypeDef, FullName);
	// Both DataInterface and plain-UObject (e.g. Material) params carry no inline
	// byte payload — GetSizeInBytes() is 0 and the store keeps them in its
	// DataInterfaces / UObjects arrays, not ParameterData. Only POD types need a
	// zeroed data buffer.
	if (!TypeDef.IsDataInterface() && !TypeDef.IsUObject())
	{
		NewVar.AllocateData();
		FMemory::Memzero(NewVar.GetData(), NewVar.GetSizeInBytes());
	}

	System->PreEditChange(nullptr);
	bool bAdded = ParamStore.AddParameter(NewVar, true);
	if (!bAdded)
	{
		System->PostEditChange();
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to add parameter to store"),
			EMCPErrorCode::Internal,
			TEXT("FNiagaraParameterStore::AddParameter returned false. Check the editor log for the specific cause."));
	}

	System->PostEditChange();
	System->MarkPackageDirty();

	// Structural changes to the ExposedParameters store (add/remove, not value
	// writes) change the layout of what compiled scripts bind against. Scripts
	// reference parameters by name lookup against the parameter store — until
	// a recompile runs, a newly-added User.Foo is not surfaced to any module
	// that tries to read it, and removed parameters remain bound but point to
	// freed storage. RequestCompile(false) triggers the system's standard
	// async compile; PollForCompilationComplete(true) drains any in-flight
	// compiles so the save writes the up-to-date bytecode.
	System->RequestCompile(false);
	System->PollForCompilationComplete(true);

	if (!UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("User parameter added in-memory but failed to persist to disk: %s"), *System->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("parameter_name"), FullName.ToString());
	Result->SetStringField(TEXT("type"), NiagaraTypeToString(TypeDef));
	Result->SetBoolField(TEXT("recompiled"), true);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── remove_niagara_user_parameter ────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleRemoveNiagaraUserParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	FString ParamName;
	if (!Params->TryGetStringField(TEXT("parameter_name"), ParamName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'parameter_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `parameter_name` (string) — the user parameter name on the Niagara System (without `User.` prefix). Use `niagara_system_read` to discover."));
	}

	FName FullName = ParamName.StartsWith(TEXT("User."))
		? FName(*ParamName)
		: FName(*FString::Printf(TEXT("User.%s"), *ParamName));

	FNiagaraUserRedirectionParameterStore& ParamStore = System->GetExposedParameters();

	// Find the variable to get its type for removal
	TArrayView<const FNiagaraVariableWithOffset> Variables = ParamStore.ReadParameterVariables();
	const FNiagaraVariableWithOffset* Found = nullptr;
	for (const FNiagaraVariableWithOffset& V : Variables)
	{
		if (V.GetName() == FullName)
		{
			Found = &V;
			break;
		}
	}
	if (!Found)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("User parameter '%s' not found"), *FullName.ToString()),
			EMCPErrorCode::NodeNotFound,
			TEXT("No user parameter with that name on the system. Use `niagara_system_read` to discover. Note: the wire form is the bare name; `User.` prefix is added automatically."));
	}

	FNiagaraVariable RemoveVar(Found->GetType(), FullName);
	System->PreEditChange(nullptr);
	bool bRemoved = ParamStore.RemoveParameter(RemoveVar);
	if (!bRemoved)
	{
		System->PostEditChange();
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to remove parameter"),
			EMCPErrorCode::Internal,
			TEXT("FNiagaraParameterStore::RemoveParameter returned false despite finding the parameter. Engine state may be inconsistent — try saving and reloading the asset."));
	}

	System->PostEditChange();
	System->MarkPackageDirty();

	// Same rebind concern as HandleAddNiagaraUserParameter — removing a parameter
	// leaves compiled scripts with a dangling bind until recompile.
	System->RequestCompile(false);
	System->PollForCompilationComplete(true);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("removed_parameter"), FullName.ToString());
	Result->SetNumberField(TEXT("remaining_parameters"), ParamStore.ReadParameterVariables().Num());
	Result->SetBoolField(TEXT("recompiled"), true);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── set_niagara_user_parameter_value ─────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleSetNiagaraUserParameterValue(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	FString ParamName;
	if (!Params->TryGetStringField(TEXT("parameter_name"), ParamName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'parameter_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `parameter_name` (string) — the user parameter name on the Niagara System (without `User.` prefix). Use `niagara_system_read` to discover."));
	}

	FName FullName = ParamName.StartsWith(TEXT("User."))
		? FName(*ParamName)
		: FName(*FString::Printf(TEXT("User.%s"), *ParamName));

	FNiagaraUserRedirectionParameterStore& ParamStore = System->GetExposedParameters();

	// Find the variable
	TArrayView<const FNiagaraVariableWithOffset> Variables = ParamStore.ReadParameterVariables();
	const FNiagaraVariableWithOffset* Found = nullptr;
	for (const FNiagaraVariableWithOffset& V : Variables)
	{
		if (V.GetName() == FullName) { Found = &V; break; }
	}
	if (!Found)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("User parameter '%s' not found"), *FullName.ToString()),
			EMCPErrorCode::NodeNotFound,
			TEXT("No user parameter with that name on the system. Use `niagara_system_read` to discover. Note: the wire form is the bare name; `User.` prefix is added automatically."));
	}

	FNiagaraTypeDefinition TypeDef = Found->GetType();
	if (TypeDef.IsDataInterface())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Cannot set default value for data interface parameters via MCP. "
				 "Bind them at runtime from C++ using SetNiagaraVariableObject."),
			EMCPErrorCode::FeatureDisabled,
			TEXT("Data interface (di:*) parameters require runtime binding via SetNiagaraVariableObject in C++. The MCP doesn't expose that path because it's runtime-only."));
	}

	FNiagaraVariable SetVar(TypeDef, FullName);

	// Shared seven-type dispatch (see ParseNiagaraValueFromJson). The error
	// rewrap preserves this handler's pre-extraction message strings exactly.
	switch (ParseNiagaraValueFromJson(TypeDef, Params, SetVar))
	{
	case ENiagaraValueParse::Ok:
		break;
	case ENiagaraValueParse::MissingNumber:
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'value' (number)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This parameter type requires a numeric `value` (float or int)."));
	case ENiagaraValueParse::MissingBool:
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'value' (bool)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This parameter type requires a boolean `value` (true/false)."));
	case ENiagaraValueParse::UnsupportedType:
	default:
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Setting values for type '%s' not yet supported"),
				*NiagaraTypeToString(TypeDef)),
			EMCPErrorCode::FeatureDisabled,
			TEXT("MCP supports value-set for: float, int32, bool, vector2/3/4, linear_color, position. Other Niagara types (custom structs, advanced) need bespoke wire-format support."));
	}

	System->PreEditChange(nullptr);
	ParamStore.SetParameterData(SetVar.GetData(), SetVar, false);
	System->PostEditChange();
	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("parameter_name"), FullName.ToString());
	Result->SetStringField(TEXT("type"), NiagaraTypeToString(TypeDef));
	// Re-read and include value in response
	const uint8* NewData = ParamStore.GetParameterData(*Found);
	AppendValueToJson(Result, TypeDef, NewData);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── set_niagara_emitter_enabled ──────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleSetNiagaraEmitterEnabled(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	bool bEnabled;
	if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'enabled' (bool) parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `enabled` (boolean) — true to activate the emitter, false to disable."));
	}

	// GetEmitterHandles returns const — we need mutable access
	TArray<FNiagaraEmitterHandle>& MutableHandles =
		const_cast<TArray<FNiagaraEmitterHandle>&>(System->GetEmitterHandles());
	System->PreEditChange(nullptr);
	MutableHandles[EmitterIdx].SetIsEnabled(bEnabled, *System, false);
	System->PostEditChange();
	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetBoolField(TEXT("enabled"), bEnabled);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── get_niagara_module_inputs (rapid iteration parameters) ───────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleGetNiagaraModuleInputs(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FVersionedNiagaraEmitter VerEmitter = Handle->GetInstance();
	FVersionedNiagaraEmitterData* EmitterData = VerEmitter.GetEmitterData();
	if (!EmitterData)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"),
			EMCPErrorCode::Internal,
			TEXT("UNiagaraEmitter::GetLatestEmitterData returned null. The emitter handle is valid but its UNiagaraEmitterData is missing — possibly an asset corruption. Try opening the asset in the editor and re-saving."));
	}

	// Gather scripts from this emitter
	TArray<UNiagaraScript*> Scripts;
	EmitterData->GetScripts(Scripts, false, false);

	// Optional filter by script usage name
	FString UsageFilter;
	Params->TryGetStringField(TEXT("script_usage"), UsageFilter);

	TArray<TSharedPtr<FJsonValue>> AllParams;
	for (UNiagaraScript* Script : Scripts)
	{
		if (!Script) continue;

		FString UsageName = StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(
			static_cast<int64>(Script->GetUsage()));

		if (!UsageFilter.IsEmpty() && !UsageName.Contains(UsageFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		const FNiagaraParameterStore& RIStore = Script->RapidIterationParameters;
		TArrayView<const FNiagaraVariableWithOffset> RIVars = RIStore.ReadParameterVariables();

		for (const FNiagaraVariableWithOffset& Var : RIVars)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("script_usage"), UsageName);
			Obj->SetStringField(TEXT("parameter_name"), Var.GetName().ToString());

			FNiagaraTypeDefinition TypeDef = Var.GetType();
			Obj->SetStringField(TEXT("type"), NiagaraTypeToString(TypeDef));

			if (!TypeDef.IsDataInterface())
			{
				const uint8* Data = RIStore.GetParameterData(Var);
				AppendValueToJson(Obj, TypeDef, Data);
			}
			AllParams.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetArrayField(TEXT("module_inputs"), AllParams);
	Result->SetNumberField(TEXT("count"), AllParams.Num());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── set_niagara_module_input ─────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleSetNiagaraModuleInput(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FString ParamNameStr;
	if (!Params->TryGetStringField(TEXT("parameter_name"), ParamNameStr))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'parameter_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `parameter_name` (string) — the rapid-iteration parameter name on the emitter's script."));
	}
	FName ParamName(*ParamNameStr);

	FVersionedNiagaraEmitter VerEmitter = Handle->GetInstance();
	FVersionedNiagaraEmitterData* EmitterData = VerEmitter.GetEmitterData();
	if (!EmitterData)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"),
			EMCPErrorCode::Internal,
			TEXT("UNiagaraEmitter::GetLatestEmitterData returned null. The emitter handle is valid but its UNiagaraEmitterData is missing — possibly an asset corruption. Try opening the asset in the editor and re-saving."));
	}

	// Search across all scripts for the matching rapid iteration parameter
	TArray<UNiagaraScript*> Scripts;
	EmitterData->GetScripts(Scripts, false, false);

	for (UNiagaraScript* Script : Scripts)
	{
		if (!Script) continue;

		FNiagaraParameterStore& RIStore = Script->RapidIterationParameters;
		TArrayView<const FNiagaraVariableWithOffset> RIVars = RIStore.ReadParameterVariables();

		for (const FNiagaraVariableWithOffset& Var : RIVars)
		{
			if (Var.GetName() != ParamName) continue;

			FNiagaraTypeDefinition TypeDef = Var.GetType();
			if (TypeDef.IsDataInterface())
			{
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("Cannot set data interface module inputs via MCP"),
					EMCPErrorCode::FeatureDisabled,
					TEXT("Data interface module inputs need runtime C++ binding (SetNiagaraVariableObject); not exposed via MCP."));
			}

			FNiagaraVariable SetVar(TypeDef, ParamName);

			// Shared seven-type dispatch (see ParseNiagaraValueFromJson) —
			// same logic as the user-parameter setter. The error rewrap
			// preserves this handler's pre-extraction message strings exactly
			// (missing number and missing bool share one message here).
			switch (ParseNiagaraValueFromJson(TypeDef, Params, SetVar))
			{
			case ENiagaraValueParse::Ok:
				break;
			case ENiagaraValueParse::MissingNumber:
			case ENiagaraValueParse::MissingBool:
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("Missing 'value'"),
					EMCPErrorCode::InvalidArgument,
					TEXT("Module input requires `value` matching the parameter's type — number for float/int, bool for bool, vector components (x/y/z/w or r/g/b/a) for FVector / FLinearColor."));
			case ENiagaraValueParse::UnsupportedType:
			default:
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Setting type '%s' not yet supported"),
						*NiagaraTypeToString(TypeDef)),
					EMCPErrorCode::FeatureDisabled,
					TEXT("MCP supports module-input set for: float, int32, bool, vector2/3/4, linear_color, position. Other Niagara types need bespoke wire-format support."));
			}

			System->PreEditChange(nullptr);
			RIStore.SetParameterData(SetVar.GetData(), SetVar, false);
			System->PostEditChange();
			System->MarkPackageDirty();

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("system"), System->GetPathName());
			Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
			Result->SetStringField(TEXT("parameter_name"), ParamNameStr);
			Result->SetStringField(TEXT("type"), NiagaraTypeToString(TypeDef));
			// Read the just-written value back into the response BEFORE recompiling.
			// RequestCompile re-bakes the rapid-iteration store (empties + repopulates
			// it from the compiled BakedRapidIterationParameters), which invalidates
			// `Var`'s offset into RIStore — so capture the value into JSON first.
			const uint8* NewData = RIStore.GetParameterData(Var);
			AppendValueToJson(Result, TypeDef, NewData);

			// A rapid-iteration write only updates the asset's parameter store; the
			// compiled simulation keeps the value baked at the last compile (the RI
			// params are baked into each script's ScriptExecutionParamStore at compile
			// time). Without a recompile, a freshly-instantiated component (e.g. a new
			// PIE run) keeps using the stale baked value — the change silently no-ops
			// (GAP-066). Emitter-property setters (e.g. niagara_emitter_set_local_space)
			// mask this because they force a recompile. Mirror that: re-bake the current
			// RI params into the compiled scripts, drain the async compile, then persist —
			// the same recompile-then-save the user-parameter setter above uses.
			System->RequestCompile(false);
			System->PollForCompilationComplete(true);
			if (!UEditorAssetLibrary::SaveAsset(System->GetPathName(), /*bOnlyIfIsDirty=*/false))
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Module input set in-memory but failed to persist to disk: %s"), *System->GetPathName()),
					EMCPErrorCode::Internal,
					TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));
			}

			Result->SetBoolField(TEXT("recompiled"), true);
			Result->SetBoolField(TEXT("success"), true);
			return Result;
		}
	}

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Rapid iteration parameter '%s' not found on emitter '%s'"),
			*ParamNameStr, *Handle->GetName().ToString()),
		EMCPErrorCode::NodeNotFound,
		TEXT("Rapid-iteration parameters are emitter-script-scoped; use `niagara_module_get_inputs` to discover. Set `script_usage` to narrow the search if multiple scripts on the same emitter share a name."));
}

// ParseScriptUsage moved to MCPNiagaraCommands_Internal.h.

// ── add_niagara_scratch_pad_module ───────────────────────────────────────

TSharedPtr<FJsonObject> FMCPNiagaraCommands::HandleAddNiagaraScratchPadModule(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UNiagaraSystem* System = LoadNiagaraSystemFromParam(Params, Error);
	if (!System) return Error;

	int32 EmitterIdx;
	const FNiagaraEmitterHandle* Handle = FindEmitterHandle(System, Params, Error, EmitterIdx);
	if (!Handle) return Error;

	FString ModuleName;
	if (!Params->TryGetStringField(TEXT("module_name"), ModuleName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'module_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `module_name` (string) — the name for the new scratch-pad module."));
	}

	FString TargetUsageStr;
	if (!Params->TryGetStringField(TEXT("target_usage"), TargetUsageStr))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target_usage' (e.g. 'ParticleUpdate', 'EmitterSpawn')"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `target_usage` (string). Valid: \"ParticleSpawn\", \"ParticleUpdate\", \"EmitterSpawn\", \"EmitterUpdate\", \"SystemSpawn\", \"SystemUpdate\"."));
	}
	ENiagaraScriptUsage TargetUsage;
	if (!ParseScriptUsage(TargetUsageStr, TargetUsage))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown script usage: '%s'"), *TargetUsageStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("Closed set: \"ParticleSpawn\", \"ParticleUpdate\", \"EmitterSpawn\", \"EmitterUpdate\", \"SystemSpawn\", \"SystemUpdate\". Case-sensitive."));
	}

	FString HlslBody;
	if (!Params->TryGetStringField(TEXT("hlsl"), HlslBody))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'hlsl' body"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `hlsl` (string) — the body of the scratch-pad module's HLSL function. Inputs/outputs are declared via the `inputs` / `outputs` arrays."));
	}

	// Parse inputs and outputs arrays
	const TArray<TSharedPtr<FJsonValue>>* InputsJson = nullptr;
	Params->TryGetArrayField(TEXT("inputs"), InputsJson);
	const TArray<TSharedPtr<FJsonValue>>* OutputsJson = nullptr;
	Params->TryGetArrayField(TEXT("outputs"), OutputsJson);

	double TargetIndexDbl = -1;
	Params->TryGetNumberField(TEXT("target_index"), TargetIndexDbl);
	int32 TargetIndex = (int32)TargetIndexDbl;

	// ── Get emitter data and target script graph ─────────────────────────
	FVersionedNiagaraEmitter VerEmitter = Handle->GetInstance();
	FVersionedNiagaraEmitterData* EmitterData = VerEmitter.GetEmitterData();
	if (!EmitterData)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Emitter data is null"),
			EMCPErrorCode::Internal,
			TEXT("UNiagaraEmitter::GetLatestEmitterData returned null. The emitter handle is valid but its UNiagaraEmitterData is missing — possibly an asset corruption. Try opening the asset in the editor and re-saving."));
	}

	UNiagaraScript* TargetScript = EmitterData->GetScript(TargetUsage, FGuid());
	if (!TargetScript)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("No script for usage '%s' on this emitter"), *TargetUsageStr),
			EMCPErrorCode::NodeNotFound,
			TEXT("Emitter doesn't have a script for that usage. Use `niagara_emitter_read` to see which scripts exist on the emitter."));
	}

	UNiagaraScriptSource* TargetSource = Cast<UNiagaraScriptSource>(TargetScript->GetLatestSource());
	if (!TargetSource || !TargetSource->NodeGraph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Could not access target script graph"),
			EMCPErrorCode::Internal,
			TEXT("Niagara script's source graph couldn't be loaded. The asset may be missing editor-only data; open in the editor and re-save."));
	}
	UNiagaraGraph* TargetGraph = TargetSource->NodeGraph;

	// Find the output node for the target usage
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
			TEXT("Could not find output node in target graph"),
			EMCPErrorCode::Internal,
			TEXT("Target script graph has no UNiagaraNodeOutput. The script may be malformed; verify it compiles in the editor first."));
	}

	// ── Create the scratch pad module script ─────────────────────────────
	UNiagaraScratchPadContainer* Container = EmitterData->ScratchPads;
	if (!Container)
	{
		Container = NewObject<UNiagaraScratchPadContainer>(VerEmitter.Emitter, NAME_None, RF_Transactional);
		EmitterData->ScratchPads = Container;
	}

	if (StaticFindObjectFast(UNiagaraScript::StaticClass(), Container, FName(*ModuleName)) != nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("A scratch-pad module named '%s' already exists on this emitter"), *ModuleName),
			EMCPErrorCode::NameCollision,
			TEXT("Pick a different `module_name`. Re-adding with an existing name would reconstruct the prior scratch-pad script in place and leave the stack graph that still references it pointing at a half-initialized script."));
	}

	if (StaticFindObjectFast(UNiagaraScript::StaticClass(), Container, FName(*ModuleName)) != nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("A scratch-pad module named '%s' already exists on this emitter"), *ModuleName),
			EMCPErrorCode::NameCollision,
			TEXT("Pick a different `module_name`. Re-adding with an existing name would reconstruct the prior scratch-pad script in place and leave the stack graph that still references it pointing at a half-initialized script."));
	}

	UNiagaraScript* ModuleScript = NewObject<UNiagaraScript>(
		Container, FName(*ModuleName), RF_Transactional);
	ModuleScript->SetUsage(ENiagaraScriptUsage::Module);

	// Manually initialize graph (InitializeScript is not exported)
	UNiagaraScriptSource* ModuleSource = NewObject<UNiagaraScriptSource>(
		ModuleScript, TEXT("Source"), RF_Transactional);
	ModuleScript->SetLatestSource(ModuleSource);
	UNiagaraGraph* ModuleGraph = NewObject<UNiagaraGraph>(
		ModuleSource, TEXT("NiagaraGraph"), RF_Transactional);
	ModuleSource->NodeGraph = ModuleGraph;

	// Create the module graph's output node
	FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*ModuleGraph);
	UNiagaraNodeOutput* ModuleOutputNode = OutputCreator.CreateNode();
	ModuleOutputNode->SetUsage(ENiagaraScriptUsage::Module);
	ModuleOutputNode->NodePosX = 400;
	OutputCreator.Finalize();

	// Create Custom HLSL node
	FGraphNodeCreator<UNiagaraNodeCustomHlsl> HlslCreator(*ModuleGraph);
	UNiagaraNodeCustomHlsl* HlslNode = HlslCreator.CreateNode();
	HlslNode->NodePosX = 0;
	HlslNode->NodePosY = 0;
	HlslCreator.Finalize();

	// Set HLSL body via reflection (CustomHlsl is private UPROPERTY)
	{
		FStrProperty* HlslProp = CastField<FStrProperty>(
			UNiagaraNodeCustomHlsl::StaticClass()->FindPropertyByName(TEXT("CustomHlsl")));
		if (HlslProp)
		{
			FString* HlslPtr = HlslProp->ContainerPtrToValuePtr<FString>(HlslNode);
			*HlslPtr = HlslBody;
		}
	}

	// Add typed input pins via CreatePin (RequestNewTypedPin is not exported)
	if (InputsJson)
	{
		for (const TSharedPtr<FJsonValue>& InputVal : *InputsJson)
		{
			const TSharedPtr<FJsonObject>& InputObj = InputVal->AsObject();
			if (!InputObj) continue;
			FString PinName, PinTypeName;
			InputObj->TryGetStringField(TEXT("name"), PinName);
			InputObj->TryGetStringField(TEXT("type"), PinTypeName);
			FNiagaraTypeDefinition PinType;
			if (!PinName.IsEmpty() && StringToNiagaraType(PinTypeName, PinType))
			{
				FEdGraphPinType EdPinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(PinType);
				HlslNode->CreatePin(EGPD_Input, EdPinType, *PinName);
			}
		}
	}

	// Add typed output pins
	if (OutputsJson)
	{
		for (const TSharedPtr<FJsonValue>& OutputVal : *OutputsJson)
		{
			const TSharedPtr<FJsonObject>& OutputObj = OutputVal->AsObject();
			if (!OutputObj) continue;
			FString PinName, PinTypeName;
			OutputObj->TryGetStringField(TEXT("name"), PinName);
			OutputObj->TryGetStringField(TEXT("type"), PinTypeName);
			FNiagaraTypeDefinition PinType;
			if (!PinName.IsEmpty() && StringToNiagaraType(PinTypeName, PinType))
			{
				FEdGraphPinType EdPinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(PinType);
				HlslNode->CreatePin(EGPD_Output, EdPinType, *PinName);
			}
		}
	}

	// Connect parameter map pins: HLSL out → Output node in
	// Parameter map pins use the NiagaraParameterMap pin category
	{
		UEdGraphPin* HlslParamOut = nullptr;
		UEdGraphPin* OutputParamIn = nullptr;
		for (UEdGraphPin* Pin : HlslNode->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc)
			{
				HlslParamOut = Pin;
				break;
			}
		}
		for (UEdGraphPin* Pin : ModuleOutputNode->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc)
			{
				OutputParamIn = Pin;
				break;
			}
		}
		if (HlslParamOut && OutputParamIn)
		{
			HlslParamOut->MakeLinkTo(OutputParamIn);
		}
	}

	// Register in scratch pad container
	Container->Scripts.Add(ModuleScript);

	// ── Add the module to the emitter stack ──────────────────────────────
	System->PreEditChange(nullptr);
	UNiagaraNodeFunctionCall* ModuleNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *TargetOutputNode, TargetIndex, ModuleName);

	System->PostEditChange();
	System->MarkPackageDirty();

	// A scratch-pad module is a new UNiagaraScript grafted into the emitter stack;
	// the emitter's compiled VM bytecode was generated before this module existed
	// and doesn't reference it. Saving without a recompile writes the graph edit
	// but not the compiled representation that the runtime actually executes, so
	// the new HLSL body never runs until the user opens the emitter and triggers
	// an editor-side recompile. Matches the BlendSpace/StateTree derived-data
	// bug class — author-time graph change visible, runtime reads stale compile.
	System->RequestCompile(false);
	System->PollForCompilationComplete(true);

	// ── Build result ─────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), System->GetPathName());
	Result->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Result->SetStringField(TEXT("module_name"), ModuleName);
	Result->SetStringField(TEXT("target_usage"), TargetUsageStr);
	Result->SetBoolField(TEXT("module_node_created"), ModuleNode != nullptr);
	Result->SetBoolField(TEXT("recompiled"), true);
	if (InputsJson)  Result->SetNumberField(TEXT("input_count"), InputsJson->Num());
	if (OutputsJson) Result->SetNumberField(TEXT("output_count"), OutputsJson->Num());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}
