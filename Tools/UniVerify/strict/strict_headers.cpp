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
#include "UniSystem.h"
#include "UniParallax.h"
#include "UniPromotion.h"
#include "UniLocalFrame.h"
#include "UniTravel.h"
#include "UniProfile.h"
#include "UniWebTexture.h"
#include "UniNebGrid.h"
#include "UniCompact.h"   // D-272

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
	FSystemManager Sys;
	std::vector<FCellGPU> SysCells;
	std::vector<FObjectGPU> SysRecs;
	FPromotionManager Prom;
	{
		// D-252: the nebula grid's plan instantiated.
		FNebGridConfig Nc; Nc.Cell0 = 1.0e-4; Nc.LayerHalf = 0.02; Nc.KnotTexel = 5.0e-3;
		FNebGridPlanner Np; Np.Configure(Nc, 1u);
		FNebGridPlanner::FFrameOps Ops;
		const double Cam[3] = { -0.33, 0.0, 0.0 };
		Np.Update(Cam, 0.0, 64u, Ops);
		(void)Np.Pending();
	}
	Prom.Update(Sys, UniScenes::SolarCamera(), 0.0, 0);
	const std::set<u32> Culled = Prom.CulledSpriteIds();
	Sys.Build(UniScenes::SolarCamera(), 0.0, 0u, 256u, SysCells, SysRecs, &Culled);
	const FParallaxPlacement Pl = ParallaxPlace(UniScenes::SolarCamera(), 1.0e9, UniScenes::SolarCamera(), FVec3f{}, false);
	return int(S.CellTable().size() > 1000000) + int(SysRecs.size() > 256u) + int(!Pl.bRealSpace) + int(!(Budget < 0.0)) + int(VB.Table(UniScenes::kStar).Rows.empty());
}
