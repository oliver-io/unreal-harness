#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for Niagara system MCP commands.
 * Supports reading/modifying Niagara systems, user parameters,
 * emitter properties, and module inputs.
 */
class FMCPNiagaraCommands
{
public:
	FMCPNiagaraCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// System-level
	TSharedPtr<FJsonObject> HandleListNiagaraSystems(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReadNiagaraSystem(const TSharedPtr<FJsonObject>& Params);

	// User parameters
	TSharedPtr<FJsonObject> HandleAddNiagaraUserParameter(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRemoveNiagaraUserParameter(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetNiagaraUserParameterValue(const TSharedPtr<FJsonObject>& Params);

	// Emitter properties
	TSharedPtr<FJsonObject> HandleSetNiagaraEmitterEnabled(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReadNiagaraEmitter(const TSharedPtr<FJsonObject>& Params);

	// Module inputs (via rapid iteration parameters)
	TSharedPtr<FJsonObject> HandleGetNiagaraModuleInputs(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetNiagaraModuleInput(const TSharedPtr<FJsonObject>& Params);

	// Scratch pad module creation
	TSharedPtr<FJsonObject> HandleAddNiagaraScratchPadModule(const TSharedPtr<FJsonObject>& Params);

	// Authoring (create/structural) — implemented in MCPNiagaraCommands_Create.cpp
	TSharedPtr<FJsonObject> HandleNiagaraSystemCreate(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraEmitterAdd(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraEmitterAddRenderer(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraRendererSetMaterial(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraRendererSetMaterialBinding(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraModuleAdd(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraEmitterSetLocalSpace(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraRendererSetAlignment(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraMeshRendererSetMesh(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleNiagaraRendererSetEnabled(const TSharedPtr<FJsonObject>& Params);
};
