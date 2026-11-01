#include "UniShaders.h"

IMPLEMENT_GLOBAL_SHADER(FUniGenProbeCS, "/Plugin/UnifiedUniverse/Private/UniGen.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniGenPlaceCS, "/Plugin/UnifiedUniverse/Private/UniGen.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniScatterCS, "/Plugin/UnifiedUniverse/Private/UniGen.usf", "ScatterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniProjectCS, "/Plugin/UnifiedUniverse/Private/UniProject.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUniSpriteVS, "/Plugin/UnifiedUniverse/Private/UniSprite.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FUniSpritePS, "/Plugin/UnifiedUniverse/Private/UniSprite.usf", "MainPS", SF_Pixel);
