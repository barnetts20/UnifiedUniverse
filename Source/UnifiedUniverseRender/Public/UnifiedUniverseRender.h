#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUniRender, Log, All);

/** Render module: owns the /Plugin/UnifiedUniverse shader directory mapping. Loaded at
 *  PostConfigInit so the mapping exists before global shaders compile. */
class FUnifiedUniverseRenderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Virtual shader path, D-001. Shaders live at /Plugin/UnifiedUniverse/Private/... */
	static constexpr const TCHAR* ShaderVirtualPath = TEXT("/Plugin/UnifiedUniverse");
};
