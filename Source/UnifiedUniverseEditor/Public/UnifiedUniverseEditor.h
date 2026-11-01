#pragma once

#include "Modules/ModuleManager.h"

/** Editor module shell. Holds the UniVol importer and archetype validation once they land
 *  (D-007; P6 stretch). */
class FUnifiedUniverseEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};
