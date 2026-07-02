#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMCPModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static inline FMCPModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FMCPModule>("UnrealMCP");
	}

	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("UnrealMCP");
	}
}; 