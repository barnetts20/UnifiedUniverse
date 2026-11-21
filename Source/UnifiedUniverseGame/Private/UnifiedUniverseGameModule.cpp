// UnifiedUniverseGameModule.cpp
// D-293: the voxel planet factory is registered with the universe for as long as this module is
// loaded (the universe makes one per enabled universe from the registered classes).

#include "UnifiedUniverseGameModule.h"

#include "UniSubsystem.h"
#include "UniVoxelPlanetFactory.h"
#include "UniRenderer.h"
#include "AtmosphereViewExtension.h"   // CloudAtmosphere_GetViewOutput (stage RS2)
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogUnifiedUniverseGame);

void FUnifiedUniverseGameModule::StartupModule()
{
	UUniSubsystem::RegisterBodyFactoryClass(UUniVoxelPlanetFactory::StaticClass());
	// D-296: the universe's sprites read what the planet's atmosphere left over the scene.
	FUniRenderer::SetAtmosphereQuery([](const FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTextureRef& OutColor, FRDGTextureRef& OutTint, FIntRect& OutRect)
		{
			return CloudAtmosphere_GetViewOutput(GraphBuilder, View, OutColor, OutTint, OutRect);
		});
}

void FUnifiedUniverseGameModule::ShutdownModule()
{
	FUniRenderer::SetAtmosphereQuery(FUniRenderer::FAtmosphereQuery());
	// (Engine exit: the UObject system may already be gone, and the registry with it.)
	if (UObjectInitialized()) { UUniSubsystem::UnregisterBodyFactoryClass(UUniVoxelPlanetFactory::StaticClass()); }
}

IMPLEMENT_MODULE(FUnifiedUniverseGameModule, UnifiedUniverseGame)
