// strict_headers.cpp
// Compiled by verify_all with -std=c++20 -Wall -Wextra -Wshadow -Wconversion-free strictness,
// as the Unreal modules compile UniCore (C++20, MSVC treats shadowing harshly). Includes every
// UniCore header and instantiates the main paths so templates and inline code are checked.

#include "UniCoord.h"
#include "UniIdentity.h"
#include "UniScaleLadder.h"
#include "UniShaderCores.h"
#include "UniSeed.h"
#include "UniLevel.h"
#include "UniWindow.h"
#include "UniArchetype.h"
#include "UniVol.h"
#include "UniGenRef.h"
#include "UniStreamer.h"
#include "UniDemoScenes.h"
#include "UniVolume.h"

int main()
{
	using namespace UniCore;
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FCpuGenBackend B(G, 1024u * 1024u);
	FUniStreamer S(G, B);
	S.Update(UniScenes::SolarCamera());
	FVolumeBuilder VB(M, G);
	const double Budget = LuminosityBudget(M, UniScenes::kGalaxy, -21.0);
	return int(S.CellTable().size() > 1000000) + int(!(Budget < 0.0)) + int(VB.Table(UniScenes::kStar).Rows.empty());
}
