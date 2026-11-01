// gencmd.cpp
// `univerify gen --scene NAME`: run a scene's reference pipeline at its camera and print the
// slice tables and per-level residency, gather and generation statistics.

#include "UniTest.h"
#include "UniVerifyCommon.h"
#include "../scenes/UniScenes.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace UniCore;

int UniGenCommand(int Argc, char** Argv)
{
	std::string Scene = "galaxy_to_star";
	for (int I = 2; I < Argc; ++I) { if (!std::strcmp(Argv[I], "--scene") && I + 1 < Argc) { Scene = Argv[++I]; } }
	FUniverseDef D = Scene == "web_to_galaxy" ? UniScenes::WebToGalaxy() : (Scene == "flythrough" ? UniScenes::Flythrough() : UniScenes::GalaxyToStar());
	const FCoord Cam = Scene == "web_to_galaxy" ? UniScenes::VoidCamera() : UniScenes::SolarCamera();
	FUniverseModel M(D);
	for (const std::string& P : M.Validate()) { std::printf("VALIDATE: %s\n", P.c_str()); }
	for (u32 A : { UniScenes::kStar, UniScenes::kGalaxy })
	{
		const FSliceTable& T = M.SlicesOf(A);
		std::printf("%s slices: L%d..L%d, sum P = %.9f, monotonic %d\n", M.Definition().Arch(A).Name.c_str(), T.LMin, T.LMax, T.PSum, int(T.bMonotonic));
		for (const FSlice& S : T.Slices) { std::printf("   L%3d  P=%.3e  cdf [%.6f, %.6f)  cell %s\n", S.Level, S.P, S.CdfLo, S.CdfHi, FormatDistance(CellWidthCm(S.Level)).c_str()); }
	}
	FRefGenerator G(M);
	FRefPipeline P(G);
	Uni::Timer T;
	P.Update(Cam, FVec3f{});
	std::printf("pipeline: %zu levels, %.2f s, %zu resident, %zu contributors registered\n", P.LevelList().size(), T.Seconds(), P.TotalResident(), P.ContributorCount());
	for (const auto& KV : P.Stats())
	{
		if (KV.second.Generated == 0) { continue; }
		std::printf("   L%3d  resident %8zu  gen %5zu  maxContrib %3zu  truncated %zu\n", KV.first, KV.second.Resident, KV.second.Generated, KV.second.MaxContributors, KV.second.Truncated);
	}
	return 0;
}
