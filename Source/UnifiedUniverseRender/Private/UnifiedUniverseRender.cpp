#include "UnifiedUniverseRender.h"

#include "UniRenderer.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "RenderingThread.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogUniRender);

#define LOCTEXT_NAMESPACE "FUnifiedUniverseRenderModule"

void FUnifiedUniverseRenderModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnifiedUniverse"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogUniRender, Error, TEXT("UnifiedUniverse plugin not found; shader directory not mapped."));
		return;
	}

	const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));

	// Lesson #33: registering the same virtual path twice asserts, and live coding re-runs
	// StartupModule without unregistering. Guard it.
	if (!AllShaderSourceDirectoryMappings().Contains(ShaderVirtualPath))
	{
		AddShaderSourceDirectoryMapping(ShaderVirtualPath, ShaderDir);
	}

	// R4: the renderer is a function-local static, destroyed after the RHI is gone. Drop its
	// pooled buffers and readbacks while the render thread still runs.
	PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([]()
	{
		ENQUEUE_RENDER_COMMAND(UniReleaseRenderer)([](FRHICommandListImmediate&)
		{
			FUniRenderer::Get().Release_RenderThread();
		});
		FlushRenderingCommands();
	});
}

void FUnifiedUniverseRenderModule::ShutdownModule()
{
	FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
	// The engine owns the mapping table and clears it at shutdown. Removing the entry here
	// would break a live-coding reload, which re-runs StartupModule against the same table.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnifiedUniverseRenderModule, UnifiedUniverseRender)
