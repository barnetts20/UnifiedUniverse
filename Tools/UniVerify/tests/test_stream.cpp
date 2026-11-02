// test_stream.cpp
// Tier 2: the runtime streaming core (UniStreamer.h) driven by the CPU backend reproduces the
// reference pipeline cell for cell: same objects, same identities, same pool layout rules. This
// verifies the asynchronous probe -> allocate -> place protocol, pool allocation and contributor
// registration from readback, offline. The GPU backend implements the same interface (UE-4).

#include "../src/UniTest.h"
#include "../scenes/UniScenes.h"
#include "UniStreamer.h"

#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>

using namespace UniCore;

UNI_TEST(stream, parity)
{
	long long Cells = 0, Mismatch = 0, Objects = 0;
	size_t MaxLatency = 0, Batches = 0, Failures = 0, PoolUsed = 0;
	std::string Detail;
	for (int Sc = 0; Sc < 2; ++Sc)
	{
		FUniverseModel M(Sc == 0 ? UniScenes::GalaxyToStar() : UniScenes::Flythrough());
		FRefGenerator G(M);
		const FCoord Start = UniScenes::SolarCamera();
		std::vector<FCoord> Path;
		FCoord C = Start;
		for (int I = 0; I < 12; ++I)
		{
			Path.push_back(C);
			FVec3f F;
			AddDisplacement(C, F, FVec3d{ 0.7 * CellWidthCm(64), 0.4 * CellWidthCm(64), 0.0 });
		}
		// Reference.
		FRefPipeline P(G);
		P.bVelocityLod = false;
		for (const FCoord& X : Path) { P.Update(X, FVec3f{}); }
		std::map<std::string, std::vector<std::pair<u32, FObjectGPU>>> Ref;
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU&, float, u64) { (void)K; });
		for (const auto& KV : P.CacheRef())
		{
			const std::string Key = std::to_string(KV.first.Level) + ToHex(KV.first.C[0]) + ToHex(KV.first.C[1]) + ToHex(KV.first.C[2]);
			for (size_t I = 0; I < KV.second.Objects.size(); ++I) { Ref[Key].push_back({ KV.second.Indices[I], KV.second.Objects[I] }); }
		}
		// Streamer with the CPU backend; extra frames at the end let every batch finish.
		FCpuGenBackend Be(G, 2097152u);
		FUniStreamer S(G, Be);
		S.bVelocityLod = false;
		S.bAsyncIntegrals = false;   // frame-counted check: integrals inline (D-140)
		for (const FCoord& X : Path) { S.Update(X); }
		for (int I = 0; I < 40 && !S.AllReady(); ++I) { S.Update(Path.back()); }
		S.ForEachReady([&](const FCellKey& K, u32 Start, u32 Count, float)
		{
			const std::string Key = std::to_string(K.Level) + ToHex(K.C[0]) + ToHex(K.C[1]) + ToHex(K.C[2]);
			std::vector<FObjectGPU> Got;
			for (u32 I = 0; I < Count; ++I)
			{
				const FObjectGPU& O = Be.PoolData()[Start + I];
				if (!((O.A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN)) { Got.push_back(O); }
			}
			auto It = Ref.find(Key);
			const size_t Want = It == Ref.end() ? 0 : It->second.size();
			++Cells;
			Objects += static_cast<long long>(Got.size());
			bool Same = Got.size() == Want;
			for (size_t I = 0; Same && I < Want; ++I) { Same = std::memcmp(&Got[I], &It->second[I].second, sizeof(FObjectGPU)) == 0; }
			if (!Same) { ++Mismatch; }
		});
		const auto St = S.Stats();
		MaxLatency = std::max(MaxLatency, St.MaxLatencyFrames);
		Batches += St.Batches; Failures += St.AllocFailures; PoolUsed = std::max(PoolUsed, St.PoolUsed);
		Detail += Uni::Fmt("%s%s: %zu ready cells", Detail.empty() ? "" : "; ", Sc == 0 ? "galaxy_to_star" : "flythrough", St.CellsReady);
	}
	Uni::Check(2, "T2.stream.parity", "Runtime streamer (probe -> allocate -> place, readback contributors) with the CPU backend reproduces the reference pipeline cell for cell")
		.Metric("cells", double(Cells)).Metric("mismatch", double(Mismatch)).Metric("objects", double(Objects)).Metric("max_latency_frames", double(MaxLatency))
		.Metric("alloc_failures", double(Failures)).Metric("pool_used", double(PoolUsed))
		.Pass(Mismatch == 0 && Cells > 1000 && Failures == 0,
			Uni::Fmt("%lld cells / %lld objects compared, %lld differ; %s; %zu batches, worst entering-to-ready latency %zu frames, pool peak %zu, 0 alloc failures required (%zu)",
				Cells, Objects, Mismatch, Detail.c_str(), Batches, MaxLatency, PoolUsed, Failures));
}

UNI_TEST(stream, sensitivity_resize)
{
	// A sensitivity change resizes windows. The streamer must keep the cells still inside
	// (no regeneration of the interior), and after it settles it must match the reference
	// pipeline at the new sensitivity.
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FCpuGenBackend Be(G, 2097152u);
	FUniStreamer S(G, Be);
	S.bVelocityLod = false;
	S.bAsyncIntegrals = false;   // frame-counted check: integrals inline (D-140)
	const FCoord C = UniScenes::SolarCamera();
	S.Update(C);   // AllReady() is trivially true before the first update
	for (int I = 0; I < 60 && !S.AllReady(); ++I) { S.Update(C); }
	const size_t Batches0 = S.Stats().Batches;
	size_t ReadyBefore = S.Stats().CellsReady;
	S.DeltaMag = 1.0;     // N 2 -> 4
	S.Update(C);
	const size_t ReadyRightAfter = S.Stats().CellsReady;
	for (int I = 0; I < 80 && !S.AllReady(); ++I) { S.Update(C); }
	size_t StreamObjects = 0;
	S.ForEachReady([&](const FCellKey&, u32 Start, u32 Count, float) { for (u32 I = 0; I < Count; ++I) { StreamObjects += ((Be.PoolData()[Start + I].A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN) ? 0 : 1; } });
	FRefPipeline P(G);
	P.bVelocityLod = false;
	P.DeltaMag = 1.0;
	P.Update(C, FVec3f{});
	const bool bMatch = StreamObjects == P.TotalResident();
	Uni::Check(2, "T2.stream.sensitivity_resize", "Sensitivity resize keeps resident cells (no interior regeneration) and settles to the reference at the new dm")
		.Metric("ready_before", double(ReadyBefore)).Metric("ready_right_after", double(ReadyRightAfter)).Metric("objects", double(StreamObjects)).Metric("reference", double(P.TotalResident()))
		.Pass(ReadyBefore > 1000 && ReadyRightAfter >= ReadyBefore && bMatch,
			Uni::Fmt("ready cells %zu before, %zu on the resize frame (kept), settled objects %zu vs reference %zu; batches %zu -> %zu",
				ReadyBefore, ReadyRightAfter, StreamObjects, P.TotalResident(), Batches0, S.Stats().Batches));
}

namespace
{
	/** Wraps the CPU backend and fails every Nth batch in the place phase, as a GPU timeout would. */
	class FFlakyBackend : public IGenBackend
	{
	public:
		FFlakyBackend(FCpuGenBackend& In, u64 InEvery) : Inner(In), Every(InEvery) {}
		void SubmitProbe(std::shared_ptr<FGenBatch> B) override { Inner.SubmitProbe(B); }
		bool PollProbe(const FGenBatch& B) override { return Inner.PollProbe(B); }
		void SubmitPlace(std::shared_ptr<FGenBatch> B) override
		{
			if (B->Ticket % Every == 0) { B->bFailed = true; B->Accepted.assign(B->Tasks.size(), 0u); B->ReadObjects.clear(); ++Failed; return; }
			Inner.SubmitPlace(B);
		}
		bool PollPlace(const FGenBatch& B) override { return Inner.PollPlace(B); }
		u32 PoolCapacity() const override { return Inner.PoolCapacity(); }
		int Failed = 0;
	private:
		FCpuGenBackend& Inner;
		u64 Every;
	};
}

UNI_TEST(stream, fail_closed)
{
	// Backend timeouts: the affected cells fail closed (not drawn, ranges released), the
	// streamer neither crashes nor reads past the readback, and the rest still matches.
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FCpuGenBackend Cpu(G, 2097152u);
	FFlakyBackend Be(Cpu, 3);
	FUniStreamer S(G, Be);
	S.bVelocityLod = false;
	S.bAsyncIntegrals = false;   // frame-counted check: integrals inline (D-140)
	const FCoord C = UniScenes::SolarCamera();
	S.Update(C);
	for (int I = 0; I < 80 && !S.AllReady(); ++I) { S.Update(C); }
	size_t DrawnHidden = 0, Drawn = 0;
	for (const FCellGPU& Cell : S.CellTable())
	{
		for (u32 I = 0; I < Cell.PoolCount; ++I)
		{
			const FObjectGPU& O = Cpu.PoolData()[Cell.PoolStart + I];
			Drawn++;
			// A slot drawn from a failed batch would still hold a stale or zero record; a zero record has archetype 0.
			if ((O.A[3] & 0xFFFFu) == 0u && !((O.A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN)) { ++DrawnHidden; }
		}
	}
	const auto St = S.Stats();
	Uni::Check(2, "T2.stream.fail_closed", "Backend timeouts fail closed: affected cells are not drawn and release their ranges; no unwritten slot is ever drawn")
		.Metric("failed_batches", Be.Failed).Metric("failed_cells", double(St.AllocFailures)).Metric("unwritten_drawn", double(DrawnHidden))
		.Pass(Be.Failed > 0 && St.AllocFailures > 0 && DrawnHidden == 0 && Drawn > 0,
			Uni::Fmt("%d batches failed on purpose, %zu cells failed closed, %zu slots drawn, %zu of them unwritten", Be.Failed, St.AllocFailures, Drawn, DrawnHidden));
}

#include "UniVolume.h"
#include "../src/UniRenderRef.h"

UNI_TEST(stream, volume_parity)
{
	// The volume scene the engine builds from the streamer equals the one univerify builds from
	// the reference pipeline: same carriers (bytes), parameters and LUTs, once the streamer has
	// settled. While a level fills, its fade may not rise (D-128): sprites of a filling level are
	// not drawn, so the volume carries their light; counted over the whole fill.
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const FCoord Cam = UniScenes::SolarCamera();
	FRefPipeline P(G);
	P.bVelocityLod = false;
	P.Update(Cam, FVec3f{});
	FCpuGenBackend Be(G, 2097152u);
	FUniStreamer S(G, Be);
	S.bVelocityLod = false;
	S.bAsyncIntegrals = false;   // frame-counted check: integrals inline (D-140)
	long long HoldViolations = 0;
	int Frames = 0;
	for (; Frames < 80; ++Frames)
	{
		S.Update(Cam);
		// During the first fill a level with any non-Ready cell must have fade 0 (it starts at 0
		// and cannot rise until its window is complete).
		for (const FUniStreamer::FLevelState& L : S.LevelStates()) { if (!L.bReady && L.Fade > 0.0f) { ++HoldViolations; if (std::getenv("UNI_DEBUG_HOLD")) { std::printf("frame %d level %d fade %.3f\n", Frames, L.Level, double(L.Fade)); } } }
		if (S.AllReady() && Frames > 12) { break; }
	}
	for (int I = 0; I < 10; ++I) { S.Update(Cam); }
	UniRender::FCamera Cm; Cm.Coord = Cam;
	const H::FUniView V = Cm.View();
	const FVolumeScene A = VB.Build(P, Cam, FVec3f{}, V);
	const FVolumeScene B = VB.Build(S, Cam, FVec3f{}, V);
	const bool bCarriers = A.Carriers.size() == B.Carriers.size() && (A.Carriers.empty() || std::memcmp(A.Carriers.data(), B.Carriers.data(), A.Carriers.size() * sizeof(FCarrierGPU)) == 0);
	const bool bParams = A.Params.size() == B.Params.size() && (A.Params.empty() || std::memcmp(A.Params.data(), B.Params.data(), A.Params.size() * sizeof(FFieldParamsGPU)) == 0);
	size_t LutDiff = 0;
	for (size_t I = 0; I < std::min(A.Lut.size(), B.Lut.size()); ++I) { if (A.Lut[I].x != B.Lut[I].x || A.Lut[I].y != B.Lut[I].y || A.Lut[I].z != B.Lut[I].z) { ++LutDiff; } }
	const bool bLut = A.Lut.size() == B.Lut.size() && LutDiff == 0;
	Uni::Check(2, "T2.stream.volume_parity", "The volume scene built from the streamer (CPU backend, settled) equals the reference pipeline's: carriers and parameters byte-identical, LUTs identical; no filling level has a fade above 0 (D-128)")
		.Metric("carriers", double(A.Carriers.size())).Metric("params", double(A.Params.size())).Metric("lut_diff", double(LutDiff)).Metric("frames", Frames).Metric("hold_violations", double(HoldViolations))
		.Pass(bCarriers && bParams && bLut && !A.Carriers.empty() && HoldViolations == 0,
			Uni::Fmt("%zu carriers (%s), %zu parameter blocks (%s), %zu LUT entries, %zu differ; streamer settled in %d frames; %lld level-frames filling with fade > 0",
				A.Carriers.size(), bCarriers ? "identical" : "DIFFER", A.Params.size(), bParams ? "identical" : "DIFFER", A.Lut.size(), LutDiff, Frames, HoldViolations));
}

namespace
{
	/** CPU backend with GPU-like latency: each phase reports done LatFrames frames after it was
	 *  submitted (submit, render thread, GPU, readback, copy). The test advances Frame. */
	class FLagBackend : public IGenBackend
	{
	public:
		FLagBackend(FCpuGenBackend& In, int InLat) : Inner(In), Lat(InLat) {}
		void SubmitProbe(std::shared_ptr<FGenBatch> B) override { Inner.SubmitProbe(B); ProbeAt[B->Ticket] = Frame; }
		bool PollProbe(const FGenBatch& B) override { return Frame - ProbeAt[B.Ticket] >= Lat; }
		void SubmitPlace(std::shared_ptr<FGenBatch> B) override { Inner.SubmitPlace(B); PlaceAt[B->Ticket] = Frame; }
		bool PollPlace(const FGenBatch& B) override { return Frame - PlaceAt[B.Ticket] >= Lat; }
		u32 PoolCapacity() const override { return Inner.PoolCapacity(); }
		long Frame = 0;
	private:
		FCpuGenBackend& Inner;
		int Lat;
		std::map<u64, long> ProbeAt, PlaceAt;
	};
}

UNI_TEST(stream, responsive)
{
	// D-137/D-138/D-139: in flight, with a backend that takes 3 frames per phase (the GPU's order):
	//   - every cell is Ready within 2 x 3 frames of entering. Before D-137 a level waited for
	//     every coarser level: 36-132 frames;
	//   - in steady flight, every cell inside the visible radius N_eff of an active (not
	//     suspended) level is Ready on every frame. Before D-139 the window assumed one frame of
	//     lag, so cells were still generating when they came inside N_eff: pop-in. The first
	//     AccelFrames after going from rest to full speed in one frame are reported separately:
	//     the widened shell takes one fill to arrive (the case B-001's warp streaks are for);
	//   - the per-frame time of Update is reported (it includes the CPU backend's generation, so
	//     it is an upper bound on bookkeeping; before D-138 the web scene spent ~10 ms in Gather).
	constexpr int Lat = 3;
	struct FRun { const char* Name; int Scene; double CmPerFrame; };
	const FRun Runs[] = {
		{ "galaxy at 1 kpc/s", 0, 1.0 * Units::Kpc / 60.0 },
		{ "galaxy at 10 kpc/s", 0, 10.0 * Units::Kpc / 60.0 },
		{ "web at 10 Mpc/s", 1, 10.0 * Units::Mpc / 60.0 },
		{ "web at 30 Mpc/s", 1, 30.0 * Units::Mpc / 60.0 },
		{ "flythrough at 10 kpc/s", 2, 10.0 * Units::Kpc / 60.0 },
	};
	const double NEff = double(UniHlsl::UNI_WINDOW_N_EFF);
	size_t WorstLatency = 0;
	double WorstVisible = 1.0, WorstMs = 0.0;
	long long Holes = 0, AccelHoles = 0;
	size_t WorstPool = 0, Failures = 0;
	constexpr int AccelFrames = 2 * Lat + 2;
	std::string Detail;
	for (const FRun& R : Runs)
	{
		FUniverseModel M(R.Scene == 0 ? UniScenes::GalaxyToStar() : (R.Scene == 1 ? UniScenes::WebToGalaxy() : UniScenes::Flythrough()));
		FRefGenerator G(M);
		FCpuGenBackend Cpu(G, 8388608u);
		FLagBackend Be(Cpu, Lat);
		FUniStreamer S(G, Be);
		S.bAsyncIntegrals = false;   // frame-counted check: integrals inline (D-140)
		FCoord C = R.Scene == 1 ? UniScenes::VoidCamera() : UniScenes::SolarCamera();
		FVec3f F{};
		S.Update(C); ++Be.Frame;
		for (int I = 0; I < 300 && !S.AllReady(); ++I) { S.Update(C); ++Be.Frame; }
		const int Frames = 240;
		int VisibleOk = 0, MaxN = 0, Suspended = 0;
		size_t PoolPeak = 0;
		double SumMs = 0.0;
		for (int Fr = 0; Fr < Frames; ++Fr)
		{
			AddDisplacement(C, F, FVec3d{ R.CmPerFrame, 0.31 * R.CmPerFrame, 0.07 * R.CmPerFrame });
			const auto T0 = std::chrono::steady_clock::now();
			S.Update(C);
			SumMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
			++Be.Frame;
			PoolPeak = std::max(PoolPeak, S.Stats().PoolUsed);
			// Ready cells inside the visible radius, per level: Chebyshev distance from the camera
			// to the cell's box < N_eff, in the level's cells.
			auto Frac = [&](int L, int A)
			{
				const FCellKey Cc = CellOf(S.CameraCoord(), L);
				const FVec3d D = ToDouble(Sub(S.CameraCoord(), CellOrigin(Cc)));
				return (A == 0 ? D.X : (A == 1 ? D.Y : D.Z)) / CellWidthCm(L);
			};
			auto Visible = [&](const int O[3], const double Fc[3])
			{
				double Cheb = 0.0;
				for (int A = 0; A < 3; ++A) { Cheb = std::max(Cheb, std::max({ 0.0, double(O[A]) - Fc[A], Fc[A] - double(O[A]) - 1.0 })); }
				return Cheb < NEff;
			};
			std::map<int, int> ReadyVisible;
			S.ForEachReady([&](const FCellKey& K, u32, u32, float)
			{
				const FCellKey Cc = CellOf(S.CameraCoord(), K.Level);
				const double Fc[3] = { Frac(K.Level, 0), Frac(K.Level, 1), Frac(K.Level, 2) };
				int O[3];
				for (int A = 0; A < 3; ++A) { O[A] = int32_t(K.C[A].L[3] - Cc.C[A].L[3]); }
				if (Visible(O, Fc)) { ++ReadyVisible[K.Level]; }
			});
			bool bOk = true;
			for (const auto& L : S.LevelStates())
			{
				MaxN = std::max(MaxN, L.HalfWidth);
				if (L.bSuspended) { ++Suspended; continue; }
				if (L.Cells == 0) { continue; }
				const double Fc[3] = { Frac(L.Level, 0), Frac(L.Level, 1), Frac(L.Level, 2) };
				int Want = 0;
				for (int X = -6; X <= 6; ++X) for (int Y = -6; Y <= 6; ++Y) for (int Z = -6; Z <= 6; ++Z)
				{
					const int O[3] = { X, Y, Z };
					Want += Visible(O, Fc) ? 1 : 0;
				}
				if (ReadyVisible[L.Level] < Want && Fr < AccelFrames) { AccelHoles += Want - ReadyVisible[L.Level]; continue; }
				if (ReadyVisible[L.Level] < Want) { bOk = false; Holes += Want - ReadyVisible[L.Level]; if (std::getenv("UNI_DEBUG_HOLES")) { std::printf("    %s frame %d L%d fade %.2f N %d: %d of %d visible cells Ready\n", R.Name, Fr, L.Level, L.Fade, L.HalfWidth, ReadyVisible[L.Level], Want); } }
			}
			VisibleOk += bOk ? 1 : 0;
		}
		const double Vis = double(VisibleOk) / Frames;   // acceleration frames count as OK here; their holes are in AccelHoles
		const double Ms = SumMs / Frames;
		WorstLatency = std::max(WorstLatency, S.Stats().MaxLatencyFrames);
		WorstVisible = std::min(WorstVisible, Vis);
		WorstMs = std::max(WorstMs, Ms);
		Detail += Uni::Fmt("%s%s: latency <= %zu frames (measured %.1f), visible cells all Ready on %.1f%% of frames, widest window N = %d, %.1f levels suspended per frame, Update %.2f ms, pool peak %zu, alloc failures %zu",
			Detail.empty() ? "" : "; ", R.Name, S.Stats().MaxLatencyFrames, S.LatencyFrames(), 100.0 * Vis, MaxN, double(Suspended) / Frames, Ms, PoolPeak, S.Stats().AllocFailures);
		WorstPool = std::max(WorstPool, PoolPeak);
		Failures += S.Stats().AllocFailures;
	}
	Detail += Uni::Fmt("; steady-flight holes %lld; holes in the %d frames after a standing start %lld (reported, not required)", Holes, AccelFrames, AccelHoles);
	Uni::Check(2, "T2.stream.responsive", "In flight with 3-frame backend phases: every cell Ready within 6 frames of entering; in steady flight every cell inside N_eff of an active level is Ready on every frame")
		.Metric("worst_latency_frames", double(WorstLatency)).Metric("worst_visible_ready", WorstVisible).Metric("visible_holes", double(Holes)).Metric("accel_holes", double(AccelHoles)).Metric("update_ms_upper_bound", WorstMs).Metric("pool_peak", double(WorstPool))
		.Pass(WorstLatency <= size_t(2 * Lat) && Holes == 0 && Failures == 0, Detail);
}

UNI_TEST(stream, async_integrals)
{
	// D-140: with integrals on worker threads, no normalising integral is computed on the
	// calling (game) thread, cells that need one wait for it, and the settled content is
	// identical to the reference pipeline's (same values, computed elsewhere).
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FCpuGenBackend Be(G, 4194304u);
	FUniStreamer S(G, Be);
	S.bVelocityLod = false;
	const FCoord C = UniScenes::SolarCamera();
	S.Update(C);
	int Frames = 1;
	size_t MaxJobs = 0;
	const auto T0 = std::chrono::steady_clock::now();
	while (!S.AllReady() && std::chrono::steady_clock::now() - T0 < std::chrono::seconds(60))
	{
		S.Update(C);
		++Frames;
		MaxJobs = std::max(MaxJobs, S.PendingIntegrals());
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const size_t Inline = G.InlineIntegrals;
	FRefGenerator G2(M);
	FRefPipeline P(G2);
	P.bVelocityLod = false;
	P.Update(C, FVec3f{});
	std::map<std::string, std::vector<FObjectGPU>> Ref;
	for (const auto& KV : P.CacheRef())
	{
		const std::string Key = std::to_string(KV.first.Level) + ToHex(KV.first.C[0]) + ToHex(KV.first.C[1]) + ToHex(KV.first.C[2]);
		Ref[Key] = KV.second.Objects;
	}
	long long Cells = 0, Mismatch = 0;
	S.ForEachReady([&](const FCellKey& K, u32 Start, u32 Count, float)
	{
		const std::string Key = std::to_string(K.Level) + ToHex(K.C[0]) + ToHex(K.C[1]) + ToHex(K.C[2]);
		std::vector<FObjectGPU> Got;
		for (u32 I = 0; I < Count; ++I)
		{
			const FObjectGPU& O = Be.PoolData()[Start + I];
			if (!((O.A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN)) { Got.push_back(O); }
		}
		auto It = Ref.find(Key);
		const size_t Want = It == Ref.end() ? 0 : It->second.size();
		bool Same = Got.size() == Want;
		for (size_t I = 0; Same && I < Want; ++I) { Same = std::memcmp(&Got[I], &It->second[I], sizeof(FObjectGPU)) == 0; }
		++Cells;
		Mismatch += Same ? 0 : 1;
	});
	Uni::Check(2, "T2.stream.async_integrals", "Normalising integrals on worker threads: none computed on the calling thread, settled content identical to the reference")
		.Metric("inline_integrals", double(Inline)).Metric("max_jobs", double(MaxJobs)).Metric("cells", double(Cells)).Metric("mismatch", double(Mismatch)).Metric("frames", Frames)
		.Pass(Inline == 0 && MaxJobs > 0 && Cells > 1000 && Mismatch == 0 && S.AllReady(),
			Uni::Fmt("%zu integrals computed inline, up to %zu worker jobs at once; settled after %d frames; %lld cells compared with the reference, %lld differ", Inline, MaxJobs, Frames, Cells, Mismatch));
}
