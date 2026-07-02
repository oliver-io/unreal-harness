#pragma once

// Shared file-local helpers for the Niagara command handlers.
//
// FMCPNiagaraCommands is implemented across two .cpp files (see
// MCPNiagaraCommands.cpp for the file map):
//   - MCPNiagaraCommands.cpp        — read + tweak-existing handlers
//   - MCPNiagaraCommands_Create.cpp — authoring handlers (system /
//                                               emitter / renderer / module create)
//
// Both translation units need to resolve a UNiagaraSystem from the request,
// find an emitter handle, and parse a script-usage string. These three helpers
// were file-local statics in the core .cpp; promoting them to `inline`
// free functions in this internal header lets the satellite .cpp share them
// without duplication (per the project's file-split convention).

#include "CoreMinimal.h"
#include "Json.h"
#include "Commands/MCPCommonUtils.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraCommon.h"

// Resolve the `system_path` request field into a loaded UNiagaraSystem.
// On failure, fills OutError with a structured envelope and returns nullptr.
inline UNiagaraSystem* LoadNiagaraSystemFromParam(
	const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
{
	FString SystemPath;
	if (!Params->TryGetStringField(TEXT("system_path"), SystemPath))
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'system_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `system_path` (string) — the /Game/... path of the Niagara System asset. Use `niagara_list_systems` to discover."));
		return nullptr;
	}
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!System)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not load Niagara system: %s"), *SystemPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("Verify the Niagara System exists at the supplied /Game/... path. Use `niagara_list_systems` to discover."));
		return nullptr;
	}
	return System;
}

// Resolve `emitter_name` (string) or `emitter_index` (int) into an emitter
// handle on the system. On failure, fills OutError and returns nullptr.
inline const FNiagaraEmitterHandle* FindEmitterHandle(
	UNiagaraSystem* System, const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutError, int32& OutIndex)
{
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	FString EmitterName;
	double EmitterIndex;
	if (Params->TryGetStringField(TEXT("emitter_name"), EmitterName))
	{
		for (int32 i = 0; i < Handles.Num(); ++i)
		{
			if (Handles[i].GetName().ToString() == EmitterName)
			{
				OutIndex = i;
				return &Handles[i];
			}
		}
		OutError = FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Emitter '%s' not found"), *EmitterName),
			EMCPErrorCode::NodeNotFound,
			TEXT("No emitter with that name on the system. Use `niagara_system_read` to discover emitter names."));
		return nullptr;
	}
	else if (Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		int32 Idx = (int32)EmitterIndex;
		if (Idx < 0 || Idx >= Handles.Num())
		{
			OutError = FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Emitter index %d out of range (0-%d)"), Idx, Handles.Num() - 1),
				EMCPErrorCode::OutOfRange,
				TEXT("`emitter_index` must be in [0, num_emitters). Use `niagara_system_read` to see emitter list, or pass `emitter_name` instead."));
			return nullptr;
		}
		OutIndex = Idx;
		return &Handles[Idx];
	}
	OutError = FMCPCommonUtils::CreateErrorResponse(
		TEXT("Missing 'emitter_name' or 'emitter_index' parameter"),
		EMCPErrorCode::InvalidArgument,
		TEXT("Pass either `emitter_name` (string) or `emitter_index` (int) to identify the emitter within the system."));
	return nullptr;
}

// Map a human script-usage string ("ParticleUpdate", "EmitterSpawn", …) to the
// ENiagaraScriptUsage enum. Returns false on an unrecognized string.
inline bool ParseScriptUsage(const FString& UsageStr, ENiagaraScriptUsage& OutUsage)
{
	if (UsageStr.Contains(TEXT("ParticleUpdate")))     { OutUsage = ENiagaraScriptUsage::ParticleUpdateScript; return true; }
	if (UsageStr.Contains(TEXT("ParticleSpawn")))      { OutUsage = ENiagaraScriptUsage::ParticleSpawnScript; return true; }
	if (UsageStr.Contains(TEXT("EmitterUpdate")))      { OutUsage = ENiagaraScriptUsage::EmitterUpdateScript; return true; }
	if (UsageStr.Contains(TEXT("EmitterSpawn")))       { OutUsage = ENiagaraScriptUsage::EmitterSpawnScript; return true; }
	if (UsageStr.Contains(TEXT("SystemUpdate")))       { OutUsage = ENiagaraScriptUsage::SystemUpdateScript; return true; }
	if (UsageStr.Contains(TEXT("SystemSpawn")))        { OutUsage = ENiagaraScriptUsage::SystemSpawnScript; return true; }
	if (UsageStr.Contains(TEXT("ParticleSimulation"))) { OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript; return true; }
	if (UsageStr.Contains(TEXT("ParticleEvent")))      { OutUsage = ENiagaraScriptUsage::ParticleEventScript; return true; }
	return false;
}
