// UniGpuParityTest.cpp
// UnifiedUniverse.Render.GpuParity (UE-4): GPU generation against the CPU reference, cell by
// cell. Two streamers run side by side on the same camera, one with the GPU backend (UniGen.usf)
// and one with the CPU backend (the shared cores through UniHlslShim.h, verified offline by
// Tier 2). Once both have settled, the GPU pool is read back and every ready cell is compared
// object by object.
//
// What must match (D-105):
//   - integer parts (offsets, archetype and flags, object seed) bit for bit, wherever the
//     cell's count matches;
//   - counts in at least 99% of cells. The probe sum is a tree reduction on the GPU and a
//     sequential sum on the CPU, so a Poisson draw can land on the other side of a boundary in
//     rare cells;
//   - float parts (radius, magnitudes, colour) are reported, not required to be bit-identical
//     (GPU transcendentals differ by a few ULP).
//
// Needs a real GPU: skipped under -nullrhi. Run from the editor's Session Frontend, or:
//   UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests UnifiedUniverse.Render;Quit" -unattended -log
// (no -nullrhi). It blocks the game thread once, for the readback (test only).

#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Misc/App.h"
#include "RHI.h"

#include "UniDemoScenes.h"
#include "UniRenderer.h"
#include "UniStreamer.h"
#include "UniSubsystem.h"

#include <map>
#include <tuple>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FUniGpuParityState
	{
		TUniquePtr<UniCore::FUniverseModel> Model;
		TUniquePtr<UniCore::FRefGenerator> Gen;
		TUniquePtr<FUniGpuGenBackend> GpuBackend;
		TUniquePtr<UniCore::FCpuGenBackend> CpuBackend;
		TUniquePtr<UniCore::FUniStreamer> Gpu;
		TUniquePtr<UniCore::FUniStreamer> Cpu;
		UniCore::FCoord Camera;
		int32 Frames = 0;
		int32 SettledFrames = 0;
		FAutomationTestBase* Test = nullptr;
	};

	using FKey = std::tuple<int, uint64, uint64, uint64>;
	FKey KeyOf(const UniCore::FCellKey& K)
	{
		// Low 64 bits of each axis are enough to tell the window's cells apart.
		auto Low = [](const UniCore::FU128& V) { return (uint64(V.L[2]) << 32) | uint64(V.L[3]); };
		return FKey(K.Level, Low(K.C[0]), Low(K.C[1]), Low(K.C[2]));
	}

	void Compare(FUniGpuParityState& S)
	{
		FAutomationTestBase& T = *S.Test;
		std::map<FKey, std::pair<uint32, uint32>> CpuCells, GpuCells;
		uint32 MaxEnd = 0;
		S.Cpu->ForEachReady([&](const UniCore::FCellKey& K, uint32 Start, uint32 Count, float) { CpuCells[KeyOf(K)] = { Start, Count }; });
		S.Gpu->ForEachReady([&](const UniCore::FCellKey& K, uint32 Start, uint32 Count, float) { GpuCells[KeyOf(K)] = { Start, Count }; MaxEnd = FMath::Max(MaxEnd, Start + Count); });

		TArray<UniCore::FObjectGPU> GpuPool;
		if (!FUniRenderer::Get().ReadbackPoolBlocking_GameThread(GpuPool, MaxEnd))
		{
			T.AddError(TEXT("GpuParity: pool readback timed out."));
			return;
		}
		const std::vector<UniCore::FObjectGPU>& CpuPool = S.CpuBackend->PoolData();

		int64 Cells = 0, CountMatch = 0, Missing = 0, Objects = 0, IntIdentical = 0, AllIdentical = 0, FloatOnly = 0;
		for (const auto& KV : GpuCells)
		{
			auto It = CpuCells.find(KV.first);
			if (It == CpuCells.end()) { ++Missing; continue; }
			++Cells;
			if (It->second.second != KV.second.second) { continue; }
			++CountMatch;
			for (uint32 I = 0; I < KV.second.second; ++I)
			{
				const UniCore::FObjectGPU& G = GpuPool[int32(KV.second.first + I)];
				const UniCore::FObjectGPU& C = CpuPool[size_t(It->second.first + I)];
				++Objects;
				// A: offsets x/y/z, archetype | flags. B.x: object seed. B.y..w: radius, magnitudes, colour.
				const bool bInt = G.A[0] == C.A[0] && G.A[1] == C.A[1] && G.A[2] == C.A[2] && G.A[3] == C.A[3] && G.B[0] == C.B[0];
				const bool bAll = bInt && G.B[1] == C.B[1] && G.B[2] == C.B[2] && G.B[3] == C.B[3];
				IntIdentical += bInt ? 1 : 0;
				AllIdentical += bAll ? 1 : 0;
				FloatOnly += (bInt && !bAll) ? 1 : 0;
			}
		}
		const double CountRate = Cells ? double(CountMatch) / double(Cells) : 0.0;
		const double IntRate = Objects ? double(IntIdentical) / double(Objects) : 0.0;
		T.AddInfo(FString::Printf(TEXT("GpuParity: %lld cells compared (%lld GPU cells missing on the CPU side), counts equal in %.3f%%; %lld objects in equal-count cells: integer parts identical %.4f%%, fully identical %.4f%%, float-only differences %lld. Settled after %d frames."),
			Cells, Missing, 100.0 * CountRate, Objects, 100.0 * IntRate, Objects ? 100.0 * double(AllIdentical) / double(Objects) : 0.0, FloatOnly, S.Frames));
		T.TestTrue(TEXT("GpuParity: cells were compared"), Cells > 100);
		T.TestTrue(TEXT("GpuParity: counts equal in >= 99% of cells (D-105)"), CountRate >= 0.99);
		T.TestTrue(TEXT("GpuParity: integer parts identical in >= 99% of objects in equal-count cells"), IntRate >= 0.99);
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FUniGpuParityLatent, TSharedPtr<FUniGpuParityState>, State);

bool FUniGpuParityLatent::Update()
{
	FUniGpuParityState& S = *State;
	S.Gpu->Update(S.Camera);
	S.Cpu->Update(S.Camera);
	++S.Frames;
	S.SettledFrames = (S.Gpu->AllReady() && S.Cpu->AllReady()) ? S.SettledFrames + 1 : 0;
	if (S.SettledFrames >= 3)
	{
		Compare(S);
		S.Gpu.Reset(); S.Cpu.Reset(); S.GpuBackend.Reset(); S.CpuBackend.Reset();
		return true;
	}
	if (S.Frames > 900)
	{
		S.Test->AddError(FString::Printf(TEXT("GpuParity: the GPU streamer did not settle in %d frames (batches failing? see uu.Stats and the log)."), S.Frames));
		S.Gpu.Reset(); S.Cpu.Reset(); S.GpuBackend.Reset(); S.CpuBackend.Reset();
		return true;
	}
	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUniGpuParityTest, "UnifiedUniverse.Render.GpuParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUniGpuParityTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender() || !GDynamicRHI || FCString::Stristr(FCommandLine::Get(), TEXT("nullrhi")))
	{
		AddWarning(TEXT("GpuParity needs a GPU; skipped (running with -nullrhi or without rendering)."));
		return true;
	}
	if (!FUniRenderer::ShadersEnabled())
	{
		AddError(TEXT("GpuParity: uu.Shaders=0, so the generation shaders were not compiled."));
		return false;
	}
	TSharedPtr<FUniGpuParityState> S = MakeShared<FUniGpuParityState>();
	S->Test = this;
	S->Model = MakeUnique<UniCore::FUniverseModel>(UniScenes::GalaxyToStar());
	S->Gen = MakeUnique<UniCore::FRefGenerator>(*S->Model);
	S->GpuBackend = MakeUnique<FUniGpuGenBackend>();
	S->CpuBackend = MakeUnique<UniCore::FCpuGenBackend>(*S->Gen, S->GpuBackend->PoolCapacity());
	S->Gpu = MakeUnique<UniCore::FUniStreamer>(*S->Gen, *S->GpuBackend);
	S->Cpu = MakeUnique<UniCore::FUniStreamer>(*S->Gen, *S->CpuBackend);
	S->Gpu->bVelocityLod = false;
	S->Cpu->bVelocityLod = false;
	S->Camera = UniScenes::SolarCamera();

	// The GPU pool and slice tables are shared with any running universe: refuse to run then.
	if (UUniSubsystem* Running = UUniSubsystem::FindForConsole(); Running && Running->IsTickable())
	{
		AddError(TEXT("GpuParity: a universe is enabled (uu.Enable); run uu.Disable first, the test shares the GPU pool."));
		return false;
	}
	TArray<UniCore::FSliceGPU> Slices;
	Slices.Append(S->Model->GpuSlices().data(), int32(S->Model->GpuSlices().size()));
	TArray<float> Lut;
	Lut.Append(S->Model->GpuLut().data(), int32(S->Model->GpuLut().size()));
	FUniRenderer::Get().SetTables_GameThread(MoveTemp(Slices), MoveTemp(Lut));

	ADD_LATENT_AUTOMATION_COMMAND(FUniGpuParityLatent(S));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
