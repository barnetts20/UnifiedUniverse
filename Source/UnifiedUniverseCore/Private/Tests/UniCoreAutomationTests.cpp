// UniCoreAutomationTests.cpp
// UE Automation tests for the engine-agnostic core, runnable headless:
//   UnrealEditor-Cmd <proj> -ExecCmds="Automation RunTests UnifiedUniverse;Quit" -unattended -nullrhi -nosplash
//
// They re-run the core checks under the ENGINE's compiler (MSVC) and, most importantly, the
// engine-parity checks the offline harness cannot do (UE-2): UniSeed's HashCombine and
// FRandomStream re-implementations against the engine's own (D-107), and ChannelId over FName.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Math/RandomStream.h"
#include "Templates/TypeHash.h"

#include "UniCoordTypes.h"
#include "UniDemoScenes.h"
#include "UniSeed.h"
#include "UniStreamer.h"
#include "UniWindow.h"

#if WITH_DEV_AUTOMATION_TESTS

// A macro rather than a typed constant: EAutomationTestFlags is an enum class from UE 5.5 and a
// namespaced enum before it, and the expression spells the same in both.
#define UNI_AUTOMATION_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUniCoordTest, "UnifiedUniverse.Core.Coord", UNI_AUTOMATION_FLAGS)
bool FUniCoordTest::RunTest(const FString&)
{
	using namespace UniCore;
	const FU128 Max = FU128::Make(~0u, ~0u, ~0u, ~0u);
	TestTrue(TEXT("root wrap: max + 1 == 0"), Add(Max, FU128::FromU64(1)).IsZero());
	TestTrue(TEXT("borrow chain: 2^96 - 1"), Sub(FU128::Pow2(96), FU128::FromU64(1)) == FU128::Make(0u, ~0u, ~0u, ~0u));
	const FDelta D = Sub(FCoord::Origin(), FCoord());
	TestTrue(TEXT("antipode delta is -2^127"), D.Neg[0] && D.Mag[0] == FU128::Pow2(127));
	TestEqual(TEXT("ToDouble antipode"), ToDouble(D).X, -FMath::Pow(2.0, 127.0));

	// The shader core (via the shim) agrees with UniCoord.h under this compiler.
	FRandomStream R(1234);
	int32 Bad = 0;
	for (int32 I = 0; I < 20000; ++I)
	{
		FCoord A, B;
		for (int32 Ax = 0; Ax < 3; ++Ax)
		{
			A.SetAxis(Ax, FU128::Make(R.GetUnsignedInt(), R.GetUnsignedInt(), R.GetUnsignedInt(), R.GetUnsignedInt()));
			B.SetAxis(Ax, FU128::Make(R.GetUnsignedInt(), R.GetUnsignedInt(), R.GetUnsignedInt(), R.GetUnsignedInt()));
		}
		const FDelta Dd = Sub(A, B);
		const FHiLo HL = ToHiLo(Dd);
		float Hi, Lo;
		UniHlsl::Uni_S128ToHiLo(ToUint4(Dd.TwosComplement(0)), Hi, Lo);
		Bad += (Hi != HL.Hi.X || Lo != HL.Lo.X) ? 1 : 0;
		Bad += (FromUint4(UniHlsl::Uni_U128Sub(ToUint4(A.Axis(1)), ToUint4(B.Axis(1)))) != Dd.TwosComplement(1)) ? 1 : 0;
	}
	TestEqual(TEXT("UniCoord.ush vs UniCoord.h mismatches (MSVC)"), Bad, 0);

	// Blueprint wrapper and hex round trip.
	const FUniCoord U = FUniCoord::Make(FCoord::FromOriginOffset(12345, -678, 9));
	UniCore::FCoord Parsed;
	TestTrue(TEXT("hex round trip"), FromHexString(std::string(TCHAR_TO_UTF8(*U.ToHexString())), Parsed) && FUniCoord::Make(Parsed) == U);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUniSeedEngineParityTest, "UnifiedUniverse.Core.SeedEngineParity", UNI_AUTOMATION_FLAGS)
bool FUniSeedEngineParityTest::RunTest(const FString&)
{
	using namespace UniCore;
	// D-107: which HashCombine does this engine have? Compare both re-implementations.
	auto Jenkins = [](uint32 A, uint32 C)
	{
		uint32 B = 0x9E3779B9u; A += B;
		A -= B; A -= C; A ^= (C >> 13); B -= C; B -= A; B ^= (A << 8); C -= A; C -= B; C ^= (B >> 13);
		A -= B; A -= C; A ^= (C >> 12); B -= C; B -= A; B ^= (A << 16); C -= A; C -= B; C ^= (B >> 5);
		A -= B; A -= C; A ^= (C >> 3); B -= C; B -= A; B ^= (A << 10); C -= A; C -= B; C ^= (B >> 15);
		return C;
	};
	auto Fast = [](uint32 A, uint32 C) { return C ^ (A + 0x9E3779B9u + (C << 6) + (C >> 2)); };
	int32 MatchJenkins = 0, MatchFast = 0, MatchUni = 0;
	FRandomStream R(42);
	const int32 N = 1000;
	for (int32 I = 0; I < N; ++I)
	{
		const uint32 A = R.GetUnsignedInt(), C = R.GetUnsignedInt();
		const uint32 Engine = ::HashCombine(A, C);
		MatchJenkins += Engine == Jenkins(A, C);
		MatchFast += Engine == Fast(A, C);
		MatchUni += Engine == UniCore::HashCombine(A, C);
	}
	AddInfo(FString::Printf(TEXT("Engine HashCombine matches: Jenkins %d/%d, fast form %d/%d, UniSeed (UNI_SEED_HASHCOMBINE_FAST=%d) %d/%d"),
		MatchJenkins, N, MatchFast, N, UNI_SEED_HASHCOMBINE_FAST, MatchUni, N));
	if (MatchUni != N)
	{
		AddError(FString::Printf(TEXT("UniSeed's HashCombine differs from the engine's. Set UNI_SEED_HASHCOMBINE_FAST to %d in UniSeed.ush (D-107). Rolled archetype parameters change; identity seeds do not (D-109)."),
			MatchJenkins == N ? 0 : 1));
	}

	// FRandomStream: FRand, GetUnsignedInt, RandRange.
	int32 BadStream = 0;
	for (int32 Seed = -50; Seed < 50; ++Seed)
	{
		FRandomStream E(Seed);
		FRandomStreamLite L(Seed);
		for (int32 K = 0; K < 16; ++K)
		{
			BadStream += (E.FRand() != L.FRand()) ? 1 : 0;
			BadStream += (E.GetUnsignedInt() != L.GetUnsignedInt()) ? 1 : 0;
			BadStream += (E.RandRange(2, 9) != L.RandRange(2, 9)) ? 1 : 0;
		}
	}
	TestEqual(TEXT("FRandomStreamLite vs FRandomStream mismatches"), BadStream, 0);

	// ChannelId over FName text: uppercased, character-hashed (ProcSeed::ChannelId(FName)).
	const FName Names[] = { FName(TEXT("ArmRadius")), FName(TEXT("Disc.ScaleLength")), FName(TEXT("arms.count")) };
	for (const FName& Nm : Names)
	{
		const FString Up = Nm.ToString().ToUpper();
		uint32 H = 2166136261u;
		for (const TCHAR Ch : Up) { H ^= uint32(Ch); H *= 16777619u; }
		TestEqual(*FString::Printf(TEXT("ChannelId(%s)"), *Nm.ToString()), int64(ChannelIdUpper(std::string(TCHAR_TO_UTF8(*Nm.ToString())))), int64(H));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUniWindowTest, "UnifiedUniverse.Core.Window", UNI_AUTOMATION_FLAGS)
bool FUniWindowTest::RunTest(const FString&)
{
	using namespace UniCore;
	int32 Bad = 0;
	for (int32 L : { 0, 60, 124, 127, 128 })
	{
		FLevelWindow W(L, 2);
		W.Recentre(CellOf(FCoord::Origin(), L));
		TSet<int32> Slots;
		for (const FWindowCell& C : W.Geometry().Cells()) { bool bDup = false; Slots.Add(C.Slot, &bDup); Bad += bDup ? 1 : 0; }
	}
	TestEqual(TEXT("slot collisions"), Bad, 0);
	TestEqual(TEXT("window half-width at dm = 0"), UniHlsl::Uni_WindowHalfWidth(0.0f), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUniReferenceParityTest, "UnifiedUniverse.Core.StreamerParity", UNI_AUTOMATION_FLAGS)
bool FUniReferenceParityTest::RunTest(const FString&)
{
	// The runtime streamer with the CPU backend reproduces the reference pipeline under MSVC,
	// the same check as T2.stream.parity offline.
	using namespace UniCore;
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	P.Update(UniScenes::SolarCamera(), FVec3f{});
	FCpuGenBackend Be(G, 2097152u);
	FUniStreamer S(G, Be);
	S.bVelocityLod = false;
	S.bAsyncIntegrals = false;   // frame-counted: integrals inline (D-140)
	S.Update(UniScenes::SolarCamera());   // AllReady() is trivially true before the first update
	for (int32 I = 0; I < 60 && !S.AllReady(); ++I) { S.Update(UniScenes::SolarCamera()); }
	size_t StreamObjects = 0;
	S.ForEachReady([&](const FCellKey&, uint32 Start, uint32 Count, float)
	{
		for (uint32 I = 0; I < Count; ++I) { StreamObjects += ((Be.PoolData()[Start + I].A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN) ? 0 : 1; }
	});
	TestEqual(TEXT("resident objects, streamer vs reference"), int64(StreamObjects), int64(P.TotalResident()));
	TestTrue(TEXT("reference pipeline generated stars"), P.TotalResident() > 1000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUniLayoutTest, "UnifiedUniverse.Core.Layouts", UNI_AUTOMATION_FLAGS)
bool FUniLayoutTest::RunTest(const FString&)
{
	TestEqual(TEXT("FObjectGPU"), int32(sizeof(UniCore::FObjectGPU)), 32);
	TestEqual(TEXT("FCellGPU"), int32(sizeof(UniCore::FCellGPU)), 64);
	TestEqual(TEXT("FGenTaskGPU"), int32(sizeof(UniCore::FGenTaskGPU)), 272);   // D-178
	TestEqual(TEXT("FSliceGPU"), int32(sizeof(UniCore::FSliceGPU)), 64);
	TestEqual(TEXT("FTaskCountGPU"), int32(sizeof(UniCore::FTaskCountGPU)), 16);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
