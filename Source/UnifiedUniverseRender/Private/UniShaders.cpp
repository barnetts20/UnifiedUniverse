#include "UniShaders.h"

#include "HAL/IConsoleManager.h"

// Read-only: the global shader map is decided at startup. Set it in DefaultEngine.ini
// ([SystemSettings] uu.Shaders=0) or on the command line (-ini:Engine:[SystemSettings]:uu.Shaders=0).
static TAutoConsoleVariable<int32> CVarUniShaders(TEXT("uu.Shaders"), 1,
	TEXT("0: compile none of UnifiedUniverse's shaders and skip all its passes (escape hatch when a shader breaks editor startup). Read-only."),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

bool UniShadersEnabled()
{
	return CVarUniShaders.GetValueOnAnyThread() != 0;
}

IMPLEMENT_GLOBAL_SHADER(FUniGenProbeCS, "/Plugin/UnifiedUniverse/Private/UniGen.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniGenPlaceCS, "/Plugin/UnifiedUniverse/Private/UniGen.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniScatterCS, "/Plugin/UnifiedUniverse/Private/UniGen.usf", "ScatterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniGenAllocCS, "/Plugin/UnifiedUniverse/Private/UniGen.usf", "AllocCS", SF_Compute);   // D-207
IMPLEMENT_GLOBAL_SHADER(FUniProjectCS, "/Plugin/UnifiedUniverse/Private/UniProject.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniSpriteVS, "/Plugin/UnifiedUniverse/Private/UniSprite.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FUniSpritePS, "/Plugin/UnifiedUniverse/Private/UniSprite.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FUniVolumeMarchCS, "/Plugin/UnifiedUniverse/Private/UniVolume.usf", "MarchCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniGalMapBakeCS, "/Plugin/UnifiedUniverse/Private/UniVolume.usf", "MapBakeCS", SF_Compute);   // D-212
IMPLEMENT_GLOBAL_SHADER(FUniGalMapDownCS, "/Plugin/UnifiedUniverse/Private/UniVolume.usf", "MapDownCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniGalCacheBakeCS, "/Plugin/UnifiedUniverse/Private/UniVolume.usf", "CacheBakeCS", SF_Compute);   // D-211
IMPLEMENT_GLOBAL_SHADER(FUniVolumeBackgroundCS, "/Plugin/UnifiedUniverse/Private/UniVolume.usf", "BackgroundCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniVolumeReconstructCS, "/Plugin/UnifiedUniverse/Private/UniVolume.usf", "ReconstructCS", SF_Compute);   // D-184
IMPLEMENT_GLOBAL_SHADER(FUniVolumeCompositeVS, "/Plugin/UnifiedUniverse/Private/UniVolume.usf", "CompositeVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FUniVolumeCompositePS, "/Plugin/UnifiedUniverse/Private/UniVolume.usf", "CompositePS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FUniWebTexUploadCS, "/Plugin/UnifiedUniverse/Private/UniWebTexUpload.usf", "UploadCS", SF_Compute);   // D-164
IMPLEMENT_GLOBAL_SHADER(FUniPickScoreCS, "/Plugin/UnifiedUniverse/Private/UniQuery.usf", "PickScoreCS", SF_Compute);       // D-193
IMPLEMENT_GLOBAL_SHADER(FUniPickSelectCS, "/Plugin/UnifiedUniverse/Private/UniQuery.usf", "PickSelectCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniPickResolveCS, "/Plugin/UnifiedUniverse/Private/UniQuery.usf", "PickResolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniQueryCS, "/Plugin/UnifiedUniverse/Private/UniQuery.usf", "QueryCS", SF_Compute);
