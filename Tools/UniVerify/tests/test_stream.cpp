// test_stream.cpp
// Tier 2: the runtime streaming core (UniStreamer.h) driven by the CPU backend reproduces the
// reference pipeline cell for cell: same objects, same identities, same pool layout rules. This
// verifies the asynchronous probe -> allocate -> place protocol, pool allocation and contributor
// registration from readback, offline. The GPU backend implements the same interface (UE-4).

#include "../src/UniTest.h"
#include "../scenes/UniScenes.h"
#include "UniStreamer.h"

#include <map>

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
