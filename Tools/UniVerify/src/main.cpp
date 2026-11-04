// univerify: offline verification CLI for the Unified Universe plugin (D-016).
//
//   univerify test [--quick] [--json FILE] [--group NAME]   run registered tests (NAME: a group, or group.test)
//   univerify fuzzdump N FILE                                coordinate cases for the Python oracle
//   univerify seeddump                                       seed values for the Python oracle
//   univerify gen --scene NAME                               run a scene's reference pipeline, print stats
//   univerify list                                           list tests
//
// Every command is run from the plugin root (verify_all does this), so relative paths in
// FOptions resolve against Plugins/UnifiedUniverse.

#include "UniTest.h"
#include "UniVerifyCommon.h"

#include <cstring>
#include <string>

int UniFuzzDump(int N, const char* Path);   // fuzzdump.cpp
int UniSeedDump();                          // seeddump.cpp
int UniGenCommand(int Argc, char** Argv);   // gencmd.cpp

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
	if (Cmd == "test") { return RunTests(Argc, Argv); }
	if (Cmd == "list")
	{
		for (const Uni::FTestEntry& T : Uni::Registry()) { std::printf("%s.%s\n", T.Group.c_str(), T.Name.c_str()); }
		return 0;
	}
	if (Cmd == "fuzzdump" && Argc >= 4) { return UniFuzzDump(std::atoi(Argv[2]), Argv[3]); }
	if (Cmd == "seeddump") { return UniSeedDump(); }
	if (Cmd == "gen") { return UniGenCommand(Argc, Argv); }
	std::fprintf(stderr, "unknown command %s\n", Cmd.c_str());
	return 2;
}
