#pragma once

// UnifiedUniverseGame (D-293): registers the universe's body factories at startup.

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUnifiedUniverseGame, Log, All);

class FUnifiedUniverseGameModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
