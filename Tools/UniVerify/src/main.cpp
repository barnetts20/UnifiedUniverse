// univerify: offline verification CLI for the Unified Universe plugin (D-016).
//
//   univerify test [--quick] [--json FILE] [--group NAME]   run registered tests (NAME: a group, or group.test)
//   univerify fuzzdump N FILE                                coordinate cases for the Python oracle
//   univerify seeddump                                       seed values for the Python oracle
//   univerify gen --scene NAME                               run a scene's reference pipeline, print stats
//   univerify galslices [--out DIR] [--quick]                the sliced galaxy-map prototype's measurements
//   univerify galwarp [--out DIR] [--quick] [--only N]       D-220: the disc warp's aliasing and look, by variant
//   univerify list                                           list tests
//
// Every command is run from the plugin root (verify_all does this), so relative paths in
// FOptions resolve against Plugins/UnifiedUniverse.

#include "UniTest.h"
#include "UniVerifyCommon.h"
#include "UniWebTexture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int UniFuzzDump(int N, const char* Path);   // fuzzdump.cpp
int UniSeedDump();                          // seeddump.cpp
int UniGenCommand(int Argc, char** Argv);   // gencmd.cpp
int UniGalSlicesCommand(int Argc, char** Argv);   // galslices.cpp
int UniGalWarpCommand(int Argc, char** Argv);     // galwarp.cpp

namespace
{
	int RunTests(int Argc, char** Argv)
	{
		std::string Json;
		std::string Group;
		for (int I = 2; I < Argc; ++I)
		{
			if (!std::strcmp(Argv[I], "--quick")) { UniVerify::Options().FuzzCases = 100000; UniVerify::Options().FullScale = 0; }
			else if (!std::strcmp(Argv[I], "--json") && I + 1 < Argc) { Json = Argv[++I]; }
			else if (!std::strcmp(Argv[I], "--group") && I + 1 < Argc) { Group = Argv[++I]; }
		}

		for (const Uni::FTestEntry& T : Uni::Registry())
		{
			if (!Group.empty() && T.Group != Group && T.Group + "." + T.Name != Group) { continue; }   // a group, or one test as group.name
			std::printf("[%s.%s]\n", T.Group.c_str(), T.Name.c_str());
			std::fflush(stdout);
			const int FailedBefore = Uni::FExpect::Get().Failed;
			T.Fn();
			const int NewFails = Uni::FExpect::Get().Failed - FailedBefore;
			if (NewFails > 0)
			{
				Uni::Check(1, "unit." + T.Group + "." + T.Name, "Unit expectations in " + T.Group + "." + T.Name)
					.Pass(false, Uni::Fmt("%d expectation(s) failed", NewFails));
			}
		}

		const Uni::FExpect& E = Uni::FExpect::Get();
		if (E.Total > 0)
		{
			Uni::Check(1, "unit.expectations", "All plain unit expectations across tests")
				.Metric("total", E.Total).Metric("failed", E.Failed)
				.Pass(E.Failed == 0, Uni::Fmt("%d expectations, %d failed", E.Total, E.Failed));
		}

		if (!Json.empty() && !Uni::Report::Get().WriteJsonl(Json))
		{
			std::fprintf(stderr, "could not write %s\n", Json.c_str());
			return 2;
		}
		const int Failed = Uni::Report::Get().NumFailed();
		std::printf("\n%zu checks, %d failed\n", Uni::Report::Get().Num(), Failed);
		return Failed == 0 ? 0 : 1;
	}
}

int main(int Argc, char** Argv)
{
	if (Argc < 2)
	{
		std::printf("usage: univerify test|fuzzdump|list|gen|render ...\n");
		return 2;
	}
	const std::string Cmd = Argv[1];
	// D-259 (explorations only; the gate never sets it): UNI_GALSHAPE="knots dust light freq" sets
	// UNI_GAL_SHAPE and galaxy_shape's powers (dust and light 0 / 1 switch the structure at the
	// bake's powers 4 and 3; knots 0 the linear knots), to tell a check's change by its term.
	if (const char* E = std::getenv("UNI_GALSHAPE"))
	{
		float A = 1, B = 1, C = 1, D = 0.25f;
		if (std::sscanf(E, "%f %f %f %f", &A, &B, &C, &D) == 4)
		{
			UniHlsl::GGalShape = UniHlsl::float4(A, B, C, D);
			std::printf("UNI_GALSHAPE: knots %g, dust %g, light %g, structure frequency %g\n", double(A), double(B), double(C), double(D));
		}
	}
	if (Cmd == "test") { return RunTests(Argc, Argv); }
	if (Cmd == "list")
	{
		for (const Uni::FTestEntry& T : Uni::Registry()) { std::printf("%s.%s\n", T.Group.c_str(), T.Name.c_str()); }
		return 0;
	}
	if (Cmd == "fuzzdump" && Argc >= 4) { return UniFuzzDump(std::atoi(Argv[2]), Argv[3]); }
	if (Cmd == "seeddump") { return UniSeedDump(); }
	if (Cmd == "gen") { return UniGenCommand(Argc, Argv); }
	if (Cmd == "galslices") { return UniGalSlicesCommand(Argc, Argv); }
	if (Cmd == "galwarp") { return UniGalWarpCommand(Argc, Argv); }
	std::fprintf(stderr, "unknown command %s\n", Cmd.c_str());
	return 2;
}
