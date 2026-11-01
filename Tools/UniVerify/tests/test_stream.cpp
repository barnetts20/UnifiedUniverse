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
