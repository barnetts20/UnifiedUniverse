// test_genmaps.cpp
// D-214: generation load. Galaxy stars placed from the galaxy's map (the light the volume draws),
// envelopes per octant for galaxy tasks, the GPU's split place pass (EVAL / SCAN / WRITE)
// emulated against the serial reference, the frame's candidate cap, and the build order's view
// priority.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"

#include "UniGalaxyMap.h"
#include "UniStreamer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <random>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	constexpr int kStarLevelMax = 76;   // galaxy_to_star: stars live at L58..L75

	struct FStarRun
	{
		u64 Tasks = 0, Candidates = 0, Accepted = 0, Clipped = 0;
		size_t Objects = 0;
		u64 Hash = 0;
		bool bHaveGalaxy = false;
		FContributor Galaxy;
	};

	FStarRun RunStars(const FUniverseModel& M, FGalaxyMapCache* Maps, const FCoord& Cam)
	{
		FRefGenerator G(M);
		G.Maps = Maps;
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FStarRun R;
		std::map<std::pair<int, u64>, u64> Hashes;   // ordered: the run's hash does not depend on map order
		for (const auto& KV : P.CacheRef())
		{
			if (KV.first.Level > kStarLevelMax) { continue; }
			for (const FTaskStats& T : KV.second.Tasks) { ++R.Tasks; R.Candidates += T.Candidates; R.Accepted += T.Accepted; R.Clipped += T.Clipped; }
			R.Objects += KV.second.Objects.size();
			Hashes[{ KV.first.Level, HashCell(KV.first) }] = KV.second.Hash;
		}
		for (const auto& KV : Hashes) { R.Hash = Mix64(R.Hash ^ KV.second ^ KV.first.second); }
		for (const auto& KV : P.Contributors())
		{
			if (KV.second.Emitter && KV.second.Emitter->FieldId == H::UNI_FIELD_GALAXY && KV.second.InfluenceCm > 1.0e21) { R.Galaxy = KV.second; R.bHaveGalaxy = true; break; }
		}
		return R;
	}

	H::FUniFieldParams HlslParams(const FFieldParamsGPU& P)
	{
		H::FUniFieldParams R;
		std::memcpy(static_cast<void*>(&R), &P, sizeof(R));
		return R;
	}
}

UNI_TEST(gen, map_placement)
{
	// D-214: a galaxy's stars are placed from its map (Uni_GalaxyMapLight: the light the volume
	// marches) instead of its analytic field. Against the field's placement from the solar camera:
	// the star count within 3% (counts follow the mean, the map is within its bake's error of the
	// field), the map's light within a few % of the field's pointwise over the disc, the same
	// content after the maps are baked again (a map is a function of its parameters), and a cell
	// waits (ResolveMap false, the bake requested) until its galaxy's map is baked. Eviction never
	// takes a slot asked for within EvictTicks.
	FUniverseModel M(UniScenes::GalaxyToStar());
	const FCoord Cam = UniScenes::SolarCamera();
	const FStarRun Field = RunStars(M, nullptr, Cam);
	FStarRun Mapped, Again;
	FGalaxyMapCache Cache;
	double LightErr = 0.0, LightP95 = 0.0;
	int SlotUsed = -1;
	{
		FCpuGalaxyMaps Host(Cache, 4u, 1024u, 4);
		Mapped = RunStars(M, &Cache, Cam);
		// The map's light against the field over the Milky Way's disc (field units).
		if (Mapped.bHaveGalaxy)
		{
			const FFieldParamsGPU Pg = Mapped.Galaxy.Params.Pack();
			SlotUsed = Cache.SlotOf(GalaxyMapKey(Pg));
			const H::FUniFieldParams Par = HlslParams(Pg);
			const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(Par);
			std::mt19937 Rng(214u);
			std::uniform_real_distribution<float> U(-0.8f, 0.8f);
			std::normal_distribution<float> Z(0.0f, std::max(Par.A.y, 1.0e-4f));
			double SumF = 0.0, SumD = 0.0;
			std::vector<double> Rel;
			for (int I = 0; I < 40000 && SlotUsed >= 0; ++I)
			{
				const H::float3 P(U(Rng), U(Rng), Z(Rng));
				const float F = H::UniField_Galaxy(P, Par);
				H::float4 Comp(0.0f, 0.0f, 0.0f, 0.0f);
				const float Mv = H::Uni_GalaxyMapLight(P, Par, K, u32(SlotUsed), Comp);
				if (!(F > 0.0f)) { continue; }
				SumF += F;
				SumD += std::fabs(double(Mv) - double(F));
				Rel.push_back(std::fabs(double(Mv) - double(F)) / double(F));
			}
			LightErr = SumF > 0.0 ? SumD / SumF : 1.0;
			if (!Rel.empty()) { std::sort(Rel.begin(), Rel.end()); LightP95 = Rel[size_t(0.95 * double(Rel.size() - 1))]; }
		}
	}
	{
		FGalaxyMapCache Cache2;
		FCpuGalaxyMaps Host(Cache2, 4u, 1024u, 4);
		Again = RunStars(M, &Cache2, Cam);
	}
	// Waiting: a cache with no host does not have the map: the task waits, the bake is pending;
	// once the host reports it baked, the task names the slot.
	bool bWait = false;
	{
		FGalaxyMapCache W;
		W.Configure(2u, 256u);
		FRefGenerator G(M);
		G.Maps = &W;
		FGenTaskGPU T{};
		const bool bFirst = Mapped.bHaveGalaxy ? G.ResolveMap(T, Mapped.Galaxy) : true;
		const std::vector<u32> Pend = W.PendingBakes();
		bool bAfter = false;
		u32 SlotNamed = 0u;
		if (!Pend.empty())
		{
			W.MarkBaked(Pend[0], W.Entry(Pend[0]).Epoch);
			bAfter = G.ResolveMap(T, Mapped.Galaxy);
			SlotNamed = T.BoxSpan[3];
		}
		bWait = Mapped.bHaveGalaxy && !bFirst && Pend.size() == 1u && bAfter && SlotNamed == Pend[0] + 1u;
	}
	// Eviction: two slots asked for this tick, a third galaxy waits; 64 ticks later it gets one.
	bool bEvict = false;
	{
		FGalaxyMapCache E;
		E.Configure(2u, 256u);
		E.SyncBake = [](uint32_t, const FGalaxyMapCache::FEntry&) {};
		FGalaxyMapRequest A, B, C;
		A.Par.V[0] = 1.0f; B.Par.V[0] = 2.0f; C.Par.V[0] = 3.0f;
		const int Sa = E.AcquireLight(A), Sb = E.AcquireLight(B), Sc0 = E.AcquireLight(C);
		for (int I = 0; I < 64; ++I) { E.Tick(); }
		const int Sc1 = E.AcquireLight(C);
		bEvict = Sa >= 0 && Sb >= 0 && Sa != Sb && Sc0 < 0 && Sc1 >= 0 && E.Evictions == 1u;
	}
	const double Ratio = Field.Objects ? double(Mapped.Objects) / double(Field.Objects) : 0.0;
	const double CandField = double(Field.Candidates) / double(std::max<u64>(Field.Accepted, 1));
	const double CandMap = double(Mapped.Candidates) / double(std::max<u64>(Mapped.Accepted, 1));
	const double ClipMap = double(Mapped.Clipped) / double(std::max<u64>(Mapped.Candidates, 1));
	Uni::Check(2, "T2.gen.map_placement", "D-214 galaxy stars placed from the galaxy's map: star count within 3% of the field's placement, map light within 3% of the field (light-weighted), the same content after a rebake, cells wait for an unbaked map, no slot evicted while in use")
		.Metric("stars_field", double(Field.Objects)).Metric("stars_map", double(Mapped.Objects)).Metric("ratio", Ratio)
		.Metric("light_err", LightErr).Metric("light_p95", LightP95).Metric("cand_per_star_field", CandField).Metric("cand_per_star_map", CandMap).Metric("clipped_map", ClipMap)
		.Pass(Mapped.bHaveGalaxy && SlotUsed >= 0 && Ratio > 0.97 && Ratio < 1.03 && LightErr < 0.03 && Mapped.Hash == Again.Hash && Mapped.Objects == Again.Objects && bWait && bEvict,
			Uni::Fmt("solar camera, star levels: %zu stars from the field, %zu from the map (%.4fx), %zu after a rebake (%s); map light vs field over the disc: %.2f%% light-weighted, p95 %.1f%%; candidates a star %.2f (field) / %.2f (map), %.3f%% over their envelope; wait for the bake: %s; eviction only after %llu idle ticks: %s",
				Field.Objects, Mapped.Objects, Ratio, Again.Objects, Mapped.Hash == Again.Hash ? "identical" : "DIFFERENT", 100.0 * LightErr, 100.0 * LightP95, CandField, CandMap, 100.0 * ClipMap,
				bWait ? "yes" : "no", (unsigned long long)64, bEvict ? "yes" : "no"));
}

namespace
{
	/** D-214: UniGen.usf's split place pass (EVAL, SCAN, WRITE with its serial tail), emulated with
	 *  the same core functions. PlanCap (0: none) caps the plan to force the tail. Chunks are
	 *  evaluated last to first: order must not matter. */
	void SplitPlace(const FRefGenerator& G, const FGenTaskGPU& T, const FTaskCountGPU& C, u32 PlanCap, std::vector<FObjectGPU>& Out, bool& bTail)
	{
		const H::FUniGenTask HT = FRefGenerator::ToHlsl(T);
		const H::FUniTaskEval Ev = H::Uni_TaskEvalOf(HT);
		const H::FUniTaskCount HC = FRefGenerator::ToHlsl(C);
		const H::FUniSlice S = G.SliceHlsl(T.SliceIndex);
		const std::vector<float>& L = G.ModelRef().GpuLut();
		const H::StructuredBuffer<float> Lut(L.data(), u32(L.size()));
		const u32 Cap = T.PoolCapacity;
		u32 Plan = H::Uni_PlanCandidates(Cap, HC);
		if (PlanCap) { Plan = std::min(Plan, PlanCap); }
		const u32 Chunks = H::Uni_PlanChunks(Plan);
		std::vector<std::array<u32, 8>> Mask(Chunks);
		std::vector<u32> Count(Chunks, 0u), Prefix(Chunks, 0u);
		for (u32 Kc = Chunks; Kc-- > 0u;)   // EVAL
		{
			Mask[Kc].fill(0u);
			for (u32 B = 0; B < H::UNI_GEN_CHUNK; ++B)
			{
				const u32 J = Kc * H::UNI_GEN_CHUNK + B;
				if (J >= Plan) { continue; }
				H::uint3 Off; float D, E;
				if (H::Uni_TaskCandidate(HT, Ev, HC, J, Off, D, E)) { Mask[Kc][B >> 5] |= 1u << (B & 31u); ++Count[Kc]; }
			}
		}
		u32 Sum = 0;
		for (u32 Kc = 0; Kc < Chunks; ++Kc) { Prefix[Kc] = Sum; Sum += Count[Kc]; }   // SCAN
		Out.assign(Cap, FromHlsl(H::Uni_BlankObject()));
		auto Pop = [](u32 V) { u32 N = 0; while (V) { V &= V - 1u; ++N; } return N; };
		for (u32 Kc = 0; Kc < Chunks; ++Kc)   // WRITE
		{
			if (Prefix[Kc] >= Cap) { continue; }
			for (u32 B = 0; B < H::UNI_GEN_CHUNK; ++B)
			{
				if (!(Mask[Kc][B >> 5] & (1u << (B & 31u)))) { continue; }
				u32 Below = 0;
				for (u32 W = 0; W < (B >> 5); ++W) { Below += Pop(Mask[Kc][W]); }
				Below += Pop(Mask[Kc][B >> 5] & ((1u << (B & 31u)) - 1u));
				const u32 Rank = Prefix[Kc] + Below;
				if (Rank >= Cap) { continue; }
				float E;
				const H::uint3 Off = H::Uni_TaskCandidateOffset(HT, Ev, HC, Kc * H::UNI_GEN_CHUNK + B, E);
				if (!H::Uni_TaskSuppressed(HT, Off)) { Out[Rank] = FromHlsl(H::Uni_TaskMakeObject(HT, Ev, S, Lut, Rank, Off)); }
			}
		}
		u32 Filled = std::min(Sum, Cap);
		bTail = false;
		if (Filled < Cap && Plan < C.CandidateBudget)   // the last chunk's serial tail
		{
			bTail = true;
			for (u32 J = Plan; Filled < Cap && J < C.CandidateBudget; ++J)
			{
				H::uint3 Off; float D, E;
				if (!H::Uni_TaskCandidate(HT, Ev, HC, J, Off, D, E)) { continue; }
				const u32 Rank = Filled++;
				if (!H::Uni_TaskSuppressed(HT, Off)) { Out[Rank] = FromHlsl(H::Uni_TaskMakeObject(HT, Ev, S, Lut, Rank, Off)); }
			}
		}
	}
}

UNI_TEST(gen, split_place)
{
	// D-214: the GPU's split place pass gives exactly the serial loop's records (RunPlace, the
	// reference): galaxy star tasks placed from their map and web / cluster tasks, with the plan as
	// UniGen.usf computes it and with the plan cut short so the serial tail does the rest. And the
	// plan is rarely short in practice: the tail is the exception.
	u64 Tasks = 0, Differ = 0, TailsNatural = 0, TailsForced = 0, PlannedSum = 0, UsedSum = 0;
	for (int Scn = 0; Scn < 2; ++Scn)
	{
		FUniverseModel M(Scn == 0 ? UniScenes::GalaxyToStar() : UniScenes::WebToGalaxy());
		const FCoord Cam = Scn == 0 ? UniScenes::SolarCamera() : UniScenes::VoidCamera();
		FGalaxyMapCache Cache;
		FCpuGalaxyMaps Host(Cache, 4u, 256u, 4);
		FRefGenerator G(M);
		G.Maps = &Cache;
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		u64 Here = 0;
		for (const auto& KV : P.CacheRef())
		{
			if (Here >= 500u) { break; }
			for (const auto& Ck : P.Contributors())
			{
				if (Here >= 500u) { break; }
				const FContributor& Ct = Ck.second;
				if (!G.Overlaps(Ct, KV.first)) { continue; }
				FGenTaskGPU T;
				if (!G.BuildTask(Ct, KV.first, T) || !G.ResolveMap(T, Ct)) { continue; }
				const FTaskCountGPU C = G.RunProbe(T);
				T.PoolCapacity = std::min(C.Count, G.MaxPerCell);
				if (T.PoolCapacity == 0u) { continue; }
				std::vector<FObjectGPU> Ref, A, B;
				FTaskStats St;
				G.RunPlace(T, C, Ref, St);
				bool bTailA = false, bTailB = false;
				SplitPlace(G, T, C, 0u, A, bTailA);
				SplitPlace(G, T, C, std::max(T.PoolCapacity / 2u, 1u), B, bTailB);
				const bool bSame = Ref.size() == A.size() && Ref.size() == B.size()
					&& std::memcmp(Ref.data(), A.data(), Ref.size() * sizeof(FObjectGPU)) == 0
					&& std::memcmp(Ref.data(), B.data(), Ref.size() * sizeof(FObjectGPU)) == 0;
				++Tasks; ++Here;
				if (!bSame) { ++Differ; }
				TailsNatural += bTailA ? 1u : 0u;
				TailsForced += bTailB ? 1u : 0u;
				PlannedSum += H::Uni_PlanCandidates(T.PoolCapacity, FRefGenerator::ToHlsl(C));
				UsedSum += St.Candidates;
			}
		}
	}
	Uni::Check(2, "T2.gen.split_place", "D-214 the GPU's split place pass (EVAL / SCAN / WRITE, chunks in any order, the serial tail when the plan falls short) gives exactly the serial loop's records; the plan rarely falls short")
		.Metric("tasks", double(Tasks)).Metric("differ", double(Differ)).Metric("tails_natural", double(TailsNatural)).Metric("tails_forced", double(TailsForced))
		.Metric("plan_over_used", double(PlannedSum) / double(std::max<u64>(UsedSum, 1)))
		.Pass(Tasks > 300 && Differ == 0 && TailsForced > Tasks / 2 && TailsNatural * 50u <= Tasks,
			Uni::Fmt("%llu tasks (galaxy stars from their maps, web and cluster fields): %llu differ from the serial loop; the plan fell short in %llu (its tail), forced short in %llu; candidates planned %.2fx those the serial loop used",
				(unsigned long long)Tasks, (unsigned long long)Differ, (unsigned long long)TailsNatural, (unsigned long long)TailsForced, double(PlannedSum) / double(std::max<u64>(UsedSum, 1))));
}

namespace
{
	/** One-trip batches lagged Lat frames; measures each batch's planned candidates (FuseAllocate). */
	class FCapBackend : public IGenBackend
	{
	public:
		FCapBackend(FCpuGenBackend& In, int InLat) : Inner(In), Lat(InLat) {}
		void SubmitProbe(std::shared_ptr<FGenBatch> B) override { Inner.SubmitProbe(B); At[B->Ticket] = Frame; }
		bool PollProbe(const FGenBatch& B) override { return Frame - At[B.Ticket] >= Lat; }
		void SubmitPlace(std::shared_ptr<FGenBatch> B) override { Inner.SubmitPlace(B); At[B->Ticket] = Frame; }
		bool PollPlace(const FGenBatch& B) override { return Frame - At[B.Ticket] >= Lat; }
		bool SupportsFused() const override { return Inner.SupportsFused(); }
		void SubmitFused(std::shared_ptr<FGenBatch> B) override
		{
			Inner.SubmitFused(B);
			At[B->Ticket] = Frame;
			std::vector<FFusedSlot> Sl;
			u64 Planned = 0;
			FuseAllocate(B->Tasks, B->Counts, B->ArenaBase, B->ArenaCap, B->MaxPerCell, B->CandidateCap, B->ChunkCap, Sl, &Planned);
			std::map<u32, bool> Packed;
			for (size_t I = 0; I < Sl.size(); ++I) { if (Sl[I].bFits && !Sl[I].bDeferred) { Packed[B->Tasks[I].BoxLo[3]] = true; } }
			MaxPlanned = std::max(MaxPlanned, Planned);
			if (B->CandidateCap > 0u && Planned > B->CandidateCap && Packed.size() > 1u) { ++Violations; }
			++Batches;
		}
		bool PollFused(const FGenBatch& B) override { return Frame - At[B.Ticket] >= Lat; }
		u32 PoolCapacity() const override { return Inner.PoolCapacity(); }
		long Frame = 0;
		u64 MaxPlanned = 0, Violations = 0, Batches = 0;
	private:
		FCpuGenBackend& Inner;
		int Lat;
		std::map<u64, long> At;
	};

	struct FCapRun
	{
		std::map<std::pair<int, u64>, std::vector<FObjectGPU>> Cells;
		u64 MaxPlanned = 0, Violations = 0, Deferred = 0, FusedBatches = 0, Frames = 0;
		size_t PoolUsed = 0, PoolCells = 0;
		bool bAllReady = false;
	};

	/** D-203's flight along the disc (10 pc a frame), stars placed from their maps, with a frame
	 *  cap; then frames at rest until everything is Ready. */
	FCapRun RunCapped(const FUniverseModel& M, u64 Cap)
	{
		FGalaxyMapCache Cache;
		FCpuGalaxyMaps Host(Cache, 4u, 256u, 4);
		FRefGenerator G(M);
		G.Maps = &Cache;
		FCpuGenBackend Inner(G, 2097152u);
		FCapBackend Be(Inner, 3);
		FUniStreamer S(G, Be);
		S.bAsyncIntegrals = false;
		S.bCellFade = true;
		S.BuildBudget = 96;
		S.bOneTrip = true;
		S.bTickMaps = true;
		S.FrameCandidateCap = Cap;
		FCoord Cam = UniScenes::SolarCamera();
		FVec3f Frac{};
		FCapRun R;
		for (int I = 0; I < 200; ++I) { ++Be.Frame; S.Update(Cam); if (S.AllReady() && I > 20) { break; } }
		for (int Fr = 1; Fr <= 40; ++Fr) { AddDisplacement(Cam, Frac, FVec3d{ 0.0, 10.0 * Units::Pc, 0.0 }); ++Be.Frame; S.Update(Cam); }
		for (int I = 0; I < 400 && !S.AllReady(); ++I) { ++Be.Frame; S.Update(Cam); ++R.Frames; }
		R.bAllReady = S.AllReady();
		std::vector<FCellKey> Keys;
		const std::vector<FCellGPU> Cells = S.CellTable(&Keys);
		for (size_t I = 0; I < Cells.size(); ++I)
		{
			std::vector<FObjectGPU>& V = R.Cells[{ Keys[I].Level, HashCell(Keys[I]) }];
			for (u32 K = 0; K < Cells[I].PoolCount; ++K) { V.push_back(Inner.PoolData()[Cells[I].PoolStart + K]); }
			R.PoolCells += Cells[I].PoolCount;
		}
		R.MaxPlanned = Be.MaxPlanned;
		R.Violations = Be.Violations;
		R.Deferred = S.FusedDeferred;
		R.FusedBatches = S.FusedBatches;
		R.PoolUsed = S.Stats().PoolUsed;
		return R;
	}
}

UNI_TEST(stream, frame_cap)
{
	// D-214: the frame's one-trip batch evaluates at most uu.GenFrameCandidates candidates up front
	// (FuseAllocate on both sides); cells past it wait a frame (glow). Flying along the Milky Way's
	// disc with stars from their maps, uncapped and with a cap of a tenth of the uncapped peak: no
	// capped batch over the cap with more than one cell, cells deferred, every cell Ready once at
	// rest, the same records in every cell (when a cell is built never changes it), no pool slot
	// leaked.
	FUniverseModel M(UniScenes::Flythrough());
	const FCapRun A = RunCapped(M, 0u);
	const u64 Cap = std::max<u64>(A.MaxPlanned / 10u, 1000u);
	const FCapRun B = RunCapped(M, Cap);
	size_t Common = 0, Differ = 0;
	for (const auto& KV : A.Cells)
	{
		const auto It = B.Cells.find(KV.first);
		if (It == B.Cells.end()) { continue; }
		++Common;
		if (KV.second.size() != It->second.size() || (KV.second.size() && std::memcmp(KV.second.data(), It->second.data(), KV.second.size() * sizeof(FObjectGPU)) != 0)) { ++Differ; }
	}
	const bool bPool = A.PoolUsed == A.PoolCells && B.PoolUsed == B.PoolCells;
	Uni::Check(2, "T2.stream.frame_cap", "D-214 the frame's candidate cap: no batch past it with more than one cell, cells deferred and all Ready at rest, the same records as uncapped in every cell, no pool slot leaked")
		.Metric("peak_uncapped", double(A.MaxPlanned)).Metric("cap", double(Cap)).Metric("peak_capped", double(B.MaxPlanned))
		.Metric("violations", double(B.Violations)).Metric("deferred", double(B.Deferred)).Metric("common", double(Common)).Metric("differ", double(Differ))
		.Pass(A.bAllReady && B.bAllReady && B.Violations == 0u && B.Deferred > 0u && Common > 1000u && Differ == 0u && bPool,
			Uni::Fmt("peak candidates a batch %llu uncapped; cap %llu: peak %llu, %llu batches over it with more than one cell, %llu cells deferred, all Ready %s (%llu frames at rest); %zu cells in both runs, %zu differ; pool = Ready ranges: %s",
				(unsigned long long)A.MaxPlanned, (unsigned long long)Cap, (unsigned long long)B.MaxPlanned, (unsigned long long)B.Violations, (unsigned long long)B.Deferred,
				B.bAllReady ? "yes" : "no", (unsigned long long)B.Frames, Common, Differ, bPool ? "yes" : "no"));
}

UNI_TEST(stream, view_priority)
{
	// D-214: with few cells built a frame, cells in view are built before those out of view. From
	// a cold start at the solar camera, 24 cells a frame, a 90 degree view along +x: the star
	// cells in view become Ready earlier on average than with the priority off, and earlier than
	// those out of view.
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	const FCoord Cam = UniScenes::SolarCamera();
	const FVec3d Dir{ 1.0, 0.0, 0.0 };
	const double ViewCos = std::cos(0.25 * 3.14159265358979);
	auto InView = [&](const FCellKey& K)
	{
		const double W = CellWidthCm(K.Level);
		const FVec3d O = ToDouble(Sub(CellOrigin(K), Cam));
		const FVec3d C{ O.X + 0.5 * W, O.Y + 0.5 * W, O.Z + 0.5 * W };
		const double D = Length(C), Rad = 0.8660254037844386 * W;
		if (D <= Rad) { return true; }
		const double Cs = (C.X * Dir.X + C.Y * Dir.Y + C.Z * Dir.Z) / D;
		const double SinR = Rad / D, CosR = std::sqrt(std::max(0.0, 1.0 - SinR * SinR)), SinA = std::sqrt(std::max(0.0, 1.0 - Cs * Cs));
		return Cs * CosR + SinA * SinR >= ViewCos;
	};
	double MeanIn[2] = { 0, 0 }, MeanOut[2] = { 0, 0 };
	size_t NIn[2] = { 0, 0 }, NOut[2] = { 0, 0 };
	for (int Run = 0; Run < 2; ++Run)
	{
		FCpuGenBackend Be(G, 2097152u);
		FUniStreamer S(G, Be);
		S.bAsyncIntegrals = false;
		S.bVelocityLod = false;
		S.BuildBudget = 24;
		S.bViewPriority = Run == 1;
		S.ViewDir = Dir;
		S.ViewCos = ViewCos;
		std::map<std::pair<int, u64>, int> First;
		std::map<std::pair<int, u64>, bool> View;
		for (int F = 0; F < 400; ++F)
		{
			S.Update(Cam);
			S.ForEachReady([&](const FCellKey& K, u32, u32, float)
			{
				if (K.Level > kStarLevelMax) { return; }
				const auto Key = std::make_pair(K.Level, HashCell(K));
				if (First.emplace(Key, F).second) { View[Key] = InView(K); }
			});
			if (S.AllReady() && F > 10) { break; }
		}
		for (const auto& KV : First)
		{
			if (View[KV.first]) { MeanIn[Run] += KV.second; ++NIn[Run]; } else { MeanOut[Run] += KV.second; ++NOut[Run]; }
		}
		MeanIn[Run] /= double(std::max<size_t>(NIn[Run], 1));
		MeanOut[Run] /= double(std::max<size_t>(NOut[Run], 1));
	}
	Uni::Check(2, "T2.stream.view_priority", "D-214 cells in view are built first: with 24 cells a frame from a cold start, star cells in view Ready earlier than with the priority off and earlier than those out of view")
		.Metric("in_off", MeanIn[0]).Metric("out_off", MeanOut[0]).Metric("in_on", MeanIn[1]).Metric("out_on", MeanOut[1])
		.Pass(NIn[1] > 50 && NOut[1] > 50 && MeanIn[1] < 0.8 * MeanIn[0] && MeanIn[1] < MeanOut[1],
			Uni::Fmt("mean frame a star cell is first Ready, in view / out of view: priority off %.1f / %.1f, on %.1f / %.1f (%zu / %zu cells)",
				MeanIn[0], MeanOut[0], MeanIn[1], MeanOut[1], NIn[1], NOut[1]));
}
