#include "MCPModule.h"
#include "Commands/MCPCommonUtils.h"
#include "MCPBridge.h"
#include "Modules/ModuleManager.h"
#include "EditorSubsystem.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "FMCPModule"

void FMCPModule::StartupModule()
{
	UE_LOG(LogUnrealMCP, Display, TEXT("Epic Unreal MCP Module has started"));
}

void FMCPModule::ShutdownModule()
{
	UE_LOG(LogUnrealMCP, Display, TEXT("Epic Unreal MCP Module has shut down"));
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FMCPModule, UnrealMCP) 
