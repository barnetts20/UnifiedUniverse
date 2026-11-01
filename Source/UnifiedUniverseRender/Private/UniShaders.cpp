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
IMPLEMENT_GLOBAL_SHADER(FUniProjectCS, "/Plugin/UnifiedUniverse/Private/UniProject.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniSpriteVS, "/Plugin/UnifiedUniverse/Private/UniSprite.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FUniSpritePS, "/Plugin/UnifiedUniverse/Private/UniSprite.usf", "MainPS", SF_Pixel);
