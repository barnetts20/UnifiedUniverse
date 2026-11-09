// test_cellglow.cpp
// D-203 (owner: transit at any speed without slowdown; no window stretching, loading fast enough
// that a fade implicit in the march covers it). A cell still loading, or fading in, draws only
// part of its sprites; the volume march draws the rest of its children's expected light as glow,
// from the same field and tables (Uni_CellGlow), so pop-in becomes glow condensing into points.
// With that, windows keep their rest size at speed (no D-162 stretch) and the streamer builds at
// most a fixed number of cells a frame (the rest wait as glow), so the work per frame no longer
// grows with speed.
//
//   T2.vol.cell_glow_exact   the march's correction is exact: every cell of a level at fade 0
//                            is the level's fade at 0; per-cell fades add linearly (a set of
//                            cells plus its complement = all plus none); grid entries are
//                            addressed per level (several levels at once)
//   T2.stream.cell_glow      a flight through the Milky Way with GPU-like latency, no stretch and
//                            a build budget: the light (sprites + glow over the whole sphere) stays
//                            the settled reference's while cells load, where without the glow the
//                            loading cells are holes; at most the budget is built a frame; windows
//                            keep their rest size

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniStreamer.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <map>
#include <set>
#include <cstring>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	/** One level's grid over 9^3 cells centred on the camera's cell (wider than any window: the
	 *  drawn light of a level is zero past its visible radius, 1.75 cells), every fade Fn(i). */
	template <typename F>
	FCellGlowLevel GlowGrid(int L, const FCoord& Cam, F&& Fn)
	{
		FCellGlowLevel G;
		G.Level = L;
		G.CellCm = std::ldexp(1.0, L);
		FCellKey K = CellOf(Cam, L);
		for (int A = 0; A < 3; ++A) { K.C[A] = MaskLow(Add(K.C[A], FU128::FromI64(-4)), 128 - L); G.Side[A] = 9; }
		const FVec3d O = ToDouble(Sub(CellOrigin(K), Cam));
		G.OriginCm[0] = O.X; G.OriginCm[1] = O.Y; G.OriginCm[2] = O.Z;
		G.Fades.resize(9u * 9u * 9u);
		for (size_t I = 0; I < G.Fades.size(); ++I) { G.Fades[I] = Fn(I); }
		return G;
	}

	/** The volume's light over six 90-degree faces of Size^2 (luminance per pixel, faces in order). */
	std::vector<double> VolumeSphere(const FVolumeScene& Sc, const FCoord& Cam, int Size)
	{
		const FV3 Looks[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0.001, 0, 1 }, { 0.001, 0, -1 } };
		std::vector<double> Out;
		for (const FV3& Lk : Looks)
		{
			FCamera Cm; Cm.Coord = Cam; Cm.Width = Size; Cm.Height = Size; Cm.LookAlong(Lk);
			FImage Img(Size, Size);
			RenderVolume(Sc, Cm, Img);
			for (int I = 0; I < Size * Size; ++I) { Out.push_back(0.2126 * Img.Rgb[size_t(I) * 3] + 0.7152 * Img.Rgb[size_t(I) * 3 + 1] + 0.0722 * Img.Rgb[size_t(I) * 3 + 2]); }
		}
		return Out;
	}

	double Sum(const std::vector<double>& V) { double S = 0.0; for (double X : V) { S += X; } return S; }

	/** CPU backend with GPU-like latency (as T2.stream.responsive's). */
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

	/** The streamer's frame as the GPU sees it: its cell table (fades as uploaded, bits 8..15) and
	 *  the pool's records, with identities for the carrier handoffs. */
	struct FStreamFrame
	{
		const FUniStreamer& S;
		const FCpuGenBackend& Pool;
		template <typename Fn> void ForEachResident(Fn&& F) const
		{
			std::vector<FCellKey> Keys;
			std::vector<u32> Gens;
			const std::vector<FCellGPU> Cells = S.CellTable(&Keys, &Gens);
			for (size_t I = 0; I < Cells.size(); ++I)
			{
				const float Fade = float((Cells[I].Flags >> 8u) & 255u) / 255.0f;
				for (u32 K = 0; K < Cells[I].PoolCount; ++K)
				{
					FObjectId Id;
					const u64 Hash = S.IdentityAt(Keys[I], Gens[I], K, Id) ? HashObject(Id) : 0u;
					F(Keys[I], Pool.PoolData()[Cells[I].PoolStart + K], Fade, Hash);
				}
			}
		}
	};
}

UNI_TEST(volume, cell_glow_exact)
{
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const FCoord Cam = UniScenes::SolarCamera();
	FRefPipeline P(G);
	P.bVelocityLod = false;
	for (int I = 0; I < 12; ++I) { P.Update(Cam, FVec3f{}); }
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 40; Cm.Height = 40;
	const H::FUniView V = Cm.View();
	FVolumeSettings S;
	S.bCellGlow = true;
	const FSliceTable& ST = M.SlicesOf(UniScenes::kStar);
	std::vector<int> Levels;
	for (int L = ST.LMin; L <= ST.LMax; ++L) { Levels.push_back(L); }
	const int Size = 40;

	// Baseline: no loading cells.
	const std::vector<double> None = VolumeSphere(VB.Build(P, Cam, FVec3f{}, V, S), Cam, Size);
	// Every star level's fade at 0 (all their light is glow) against every cell of every star
	// level at fade 0 through the glow grids.
	for (int L : Levels) { VB.ForceLevelFades[L] = 0.0f; }
	const std::vector<double> Off = VolumeSphere(VB.Build(P, Cam, FVec3f{}, V, S), Cam, Size);
	VB.ForceLevelFades.clear();
	std::vector<FCellGlowLevel> Grids;
	for (int L : Levels) { Grids.push_back(GlowGrid(L, Cam, [](size_t) { return 0.0f; })); }
	VB.GlowOverride = &Grids;
	const std::vector<double> All = VolumeSphere(VB.Build(P, Cam, FVec3f{}, V, S), Cam, Size);
	// Per-cell linearity: a hashed half of the cells at 0 (the rest 1), then its complement.
	auto Half = [](int L, size_t I) { return ((u32(I) * 2654435761u + u32(L) * 40503u) >> 13) & 1u; };
	std::vector<FCellGlowLevel> GA, GB;
	for (int L : Levels)
	{
		GA.push_back(GlowGrid(L, Cam, [&](size_t I) { return Half(L, I) ? 0.0f : 1.0f; }));
		GB.push_back(GlowGrid(L, Cam, [&](size_t I) { return Half(L, I) ? 1.0f : 0.0f; }));
	}
	VB.GlowOverride = &GA;
	const std::vector<double> A = VolumeSphere(VB.Build(P, Cam, FVec3f{}, V, S), Cam, Size);
	VB.GlowOverride = &GB;
	const std::vector<double> B = VolumeSphere(VB.Build(P, Cam, FVec3f{}, V, S), Cam, Size);
	VB.GlowOverride = nullptr;

	const double Tn = Sum(None), To = Sum(Off), Ta = Sum(All);
	double WorstAll = 0.0, WorstLin = 0.0;
	for (size_t I = 0; I < None.size(); ++I)
	{
		const double Ref = std::max(Off[I], 1.0e-3 * To / double(None.size()));
		WorstAll = std::max(WorstAll, std::fabs(All[I] - Off[I]) / Ref);
		WorstLin = std::max(WorstLin, std::fabs((A[I] + B[I]) - (All[I] + None[I])) / std::max(All[I] + None[I], 1.0e-3 * (Ta + Tn) / double(None.size())));
	}
	const double Total = Ta / To - 1.0;
	const double Lin = (Sum(A) + Sum(B)) / (Ta + Tn) - 1.0;
	const double Gain = To / Tn - 1.0;
	Uni::Check(2, "T2.vol.cell_glow_exact", "D-203 cell glow: every cell of every star level at fade 0 equals the levels' fades at 0 (total within 0.05%, every pixel within 0.5%); per-cell fades add linearly (a half plus its complement = all plus none, within 0.05%, every pixel within 0.5%); the levels' drawn light is a real share of the glow (over 5%)")
		.Metric("total", Total).Metric("pixel", WorstAll).Metric("linear", Lin).Metric("linear_pixel", WorstLin).Metric("gain", Gain).Metric("levels", double(Levels.size()))
		.Pass(std::fabs(Total) < 5.0e-4 && WorstAll < 5.0e-3 && std::fabs(Lin) < 5.0e-4 && WorstLin < 5.0e-3 && Gain > 0.05,
			Uni::Fmt("solar camera, %zu star levels L%d..L%d, sphere 6 x %d^2: glow grids at 0 vs fades at 0 %+.4f%% (worst pixel %.3f%%); half + complement vs all + none %+.4f%% (worst pixel %.3f%%); the star levels' drawn light is %.1f%% of the glow",
				Levels.size(), ST.LMin, ST.LMax, Size, 100.0 * Total, 100.0 * WorstAll, 100.0 * Lin, 100.0 * WorstLin, 100.0 * Gain));
}

UNI_TEST(stream, cell_glow)
{
	// Flight along the Milky Way's disc from the solar camera, the engine's D-203 streamer: cell
	// glow, no window stretch, a build budget; 3 frames a phase (GPU-like latency). At sampled
	// frames the light over the whole sphere (sprites as the cell table fades them, plus the
	// volume) against a settled reference pipeline at the same camera, with and without the glow,
	// over the reference's sprite light (the galaxy's glow is ~95% of the sky and never changes
	// with loading). Measured (sprite light, with / without the glow, frames 16 / 32 / 48):
	//   2 pc a frame:   -0.03 / -0.04%, +0.01 / 0.00%, -0.02 / -0.03% (cells load in time)
	//   10 pc a frame:  -1.20 / -2.68%, -0.59 / -1.91%, -0.24 / -1.39%
	//   10, local carriers from 1 pc (UNI_GLOW_START1): -0.58 / -2.55%, -0.09 / -1.77%, +0.20 / -1.26%
	// So most of what the glow leaves is D-195's 10 pc start: a level suspended at speed (or a cell
	// loading) loses its stars within 10 pc, with or without the glow (as before D-203).
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FCpuGenBackend Inner(G, 2097152u);
	FLagBackend Be(Inner, 3);
	FUniStreamer S(G, Be);
	S.bAsyncIntegrals = false;
	S.bCellFade = true;
	S.WindowLeadMax = 0;
	S.BuildBudget = 96;
	if (std::getenv("UNI_GLOW_NOLOD")) { S.bVelocityLod = false; }
	FCoord Cam = UniScenes::SolarCamera();
	FVec3f Frac{};
	// Settle at rest.
	for (int I = 0; I < 200; ++I) { ++Be.Frame; S.Update(Cam); if (S.AllReady() && I > 20) { break; } }
	for (int I = 0; I < 10; ++I) { ++Be.Frame; S.Update(Cam); }
	const char* StepEnv = std::getenv("UNI_GLOW_STEP_PC");   // exploring: other speeds
	const double Step = (StepEnv ? std::atof(StepEnv) : 10.0) * Units::Pc;   // a frame: 600 pc/s at 60 fps
	const int Size = 32;
	FVolumeSettings On; On.bCellGlow = true;
	FVolumeSettings OffS; OffS.bCellGlow = false;
	if (std::getenv("UNI_GLOW_START1")) { On.LocalStartCm = OffS.LocalStartCm = 3.0856775814913673e18; }
	size_t MaxBuilt = 0, MaxCells = 0;
	double WorstOn = 0.0, WorstOff = 0.0, SumOn = 0.0, SumOff = 0.0;
	int Samples = 0;
	double SprShare = 0.0;
	size_t GlowLevelsSeen = 0;
	std::string Detail;
	for (int Fr = 1; Fr <= 48; ++Fr)
	{
		AddDisplacement(Cam, Frac, FVec3d{ 0.0, Step, 0.0 });
		++Be.Frame;
		S.Update(Cam);
		MaxBuilt = std::max<size_t>(MaxBuilt, S.Profile.CellsSubmitted);
		for (const FUniStreamer::FLevelState& L : S.LevelStates()) { MaxCells = std::max(MaxCells, L.Cells); }
		if (Fr % 16 != 0) { continue; }
		FCamera Cm; Cm.Coord = Cam; Cm.Width = Size; Cm.Height = Size;
		const H::FUniView V = Cm.View();
		const FStreamFrame Sf{ S, Inner };
		GlowLevelsSeen = std::max(GlowLevelsSeen, S.CellGlow().size());
		const double LOn = SphereLight(Sf, VB.Build(S, Cam, FVec3f{}, V, On), Cam, Size);
		const double LOff = SphereLight(Sf, VB.Build(S, Cam, FVec3f{}, V, OffS), Cam, Size);
		// The settled reference at this camera (everything generated and faded in).
		FRefPipeline P(G);
		P.bVelocityLod = false;
		for (int I = 0; I < 12; ++I) { P.Update(Cam, FVec3f{}); }
		double SpRef = 0.0;
		const double LRef = SphereLight(P, VB.Build(P, Cam, FVec3f{}, V, OffS), Cam, Size, &SpRef);
		// The difference over the reference's sprite light: what loading could lose (the glow
		// of the galaxy around the camera is most of the sky and never changes with loading).
		const double Spr = std::max(SpRef * LRef, 1.0e-300);
		const double EOn = (LOn - LRef) / Spr, EOff = (LOff - LRef) / Spr;
		SprShare = SpRef;
		WorstOn = std::max(WorstOn, std::fabs(EOn));
		WorstOff = std::max(WorstOff, std::fabs(EOff));
		SumOn += EOn; SumOff += EOff;
		++Samples;
		Detail += Uni::Fmt("%s%d: %+.2f%% / %+.2f%% (sprites %.2f%% of the sky)", Detail.empty() ? "" : "; ", Fr, 100.0 * EOn, 100.0 * EOff, 100.0 * SprShare);
	}
	const bool bBudget = MaxBuilt <= size_t(S.BuildBudget);
	const bool bRest = MaxCells <= 125u;
	Uni::Check(2, "T2.stream.cell_glow", "D-203 flight along the disc at 10 pc a frame (3-frame phases, no window stretch, 96 cells a frame): sprites + glow over the whole sphere within 2% of the settled reference's sprite light at every sample, and on average less than half the loss without the glow; at most the budget built a frame; windows at their rest size (125 cells)")
		.Metric("worst_on", WorstOn).Metric("worst_off", WorstOff).Metric("mean_on", SumOn / std::max(Samples, 1)).Metric("mean_off", SumOff / std::max(Samples, 1))
		.Metric("max_built", double(MaxBuilt)).Metric("max_cells", double(MaxCells)).Metric("glow_levels", double(GlowLevelsSeen))
		.Pass(WorstOn < 0.02 && std::fabs(SumOn) < 0.5 * std::fabs(SumOff) && bBudget && bRest && GlowLevelsSeen > 0,
			Uni::Fmt("light against the settled reference's sprite light, with / without the glow, at frame %s; at most %zu cells built a frame (budget %d); largest window %zu cells; up to %zu levels loading at a sample",
				Detail.c_str(), MaxBuilt, S.BuildBudget, MaxCells, GlowLevelsSeen));
}

namespace
{
	/** As FLagBackend, with one-trip batches (D-207): Lat frames for the whole trip. */
	class FLagFusedBackend : public IGenBackend
	{
	public:
		FLagFusedBackend(FCpuGenBackend& In, int InLat) : Inner(In), Lat(InLat) {}
		void SubmitProbe(std::shared_ptr<FGenBatch> B) override { Inner.SubmitProbe(B); At[B->Ticket] = Frame; }
		bool PollProbe(const FGenBatch& B) override { return Frame - At[B.Ticket] >= Lat; }
		void SubmitPlace(std::shared_ptr<FGenBatch> B) override { Inner.SubmitPlace(B); At[B->Ticket] = Frame; }
		bool PollPlace(const FGenBatch& B) override { return Frame - At[B.Ticket] >= Lat; }
		bool SupportsFused() const override { return Inner.SupportsFused(); }
		void SubmitFused(std::shared_ptr<FGenBatch> B) override { Inner.SubmitFused(B); At[B->Ticket] = Frame; }
		bool PollFused(const FGenBatch& B) override { return Frame - At[B.Ticket] >= Lat; }
		u32 PoolCapacity() const override { return Inner.PoolCapacity(); }
		long Frame = 0;
	private:
		FCpuGenBackend& Inner;
		int Lat;
		std::map<u64, long> At;
	};

	struct FTripRun
	{
		std::map<std::pair<int, u64>, std::vector<FObjectGPU>> Cells;   // Ready cells at the last frame (level, cell hash), records in pool order
		double MeanLatency = 0.0;
		u64 Fused = 0, Overflows = 0;
		size_t PoolUsed = 0, PoolCells = 0;
		bool bIdentities = true;
	};

	FTripRun RunTrip(const FRefGenerator& G, bool bOneTrip, double FirstEstimate)
	{
		FCpuGenBackend Inner(G, 2097152u);
		FLagFusedBackend Be(Inner, 3);
		FUniStreamer S(G, Be);
		S.bAsyncIntegrals = false;
		S.bCellFade = true;
		S.WindowLeadMax = 0;
		S.BuildBudget = 96;
		S.bOneTrip = bOneTrip;
		S.OneTripFirstEstimate = FirstEstimate;
		FCoord Cam = UniScenes::SolarCamera();
		FVec3f Frac{};
		for (int I = 0; I < 200; ++I) { ++Be.Frame; S.Update(Cam); if (S.AllReady() && I > 20) { break; } }
		for (int Fr = 1; Fr <= 60; ++Fr)
		{
			if (Fr <= 40) { AddDisplacement(Cam, Frac, FVec3d{ 0.0, 10.0 * Units::Pc, 0.0 }); }
			++Be.Frame;
			S.Update(Cam);
		}
		FTripRun R;
		std::vector<FCellKey> Keys;
		std::vector<u32> Gens;
		const std::vector<FCellGPU> Cells = S.CellTable(&Keys, &Gens);
		for (size_t I = 0; I < Cells.size(); ++I)
		{
			std::vector<FObjectGPU>& V = R.Cells[{ Keys[I].Level, HashCell(Keys[I]) }];
			for (u32 K = 0; K < Cells[I].PoolCount; ++K)
			{
				V.push_back(Inner.PoolData()[Cells[I].PoolStart + K]);
				FObjectId Id;
				if (K < 4u && !S.IdentityAt(Keys[I], Gens[I], K, Id)) { R.bIdentities = false; }
			}
			R.PoolCells += Cells[I].PoolCount;
		}
		R.Fused = S.FusedCells;
		R.Overflows = S.FusedOverflows;
		R.PoolUsed = S.Stats().PoolUsed;
		R.MeanLatency = S.TripCells ? double(S.TripFramesSum) / double(S.TripCells) : 0.0;
		return R;
	}
}

UNI_TEST(stream, one_trip)
{
	// D-207 (B-020). Leaf cells (stars: their records never come back) and no authored objects
	// are generated in one backend round trip: probe, allocation and place back to back in a pool
	// arena, only the counts read back (FuseAllocate on both sides). The D-203 flight along the
	// Milky Way's disc (10 pc a frame, 3 frames a trip) two-trip, one-trip, and one-trip with a
	// first arena of 1 slot a cell (every cell's first try overflows and goes two-trip): every
	// cell Ready at the end holds the same records in the same order in all three; identities
	// resolve; the pool holds exactly the Ready cells' ranges (no arena leaked).
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	const FTripRun A = RunTrip(G, false, 2048.0);
	const FTripRun B = RunTrip(G, true, 2048.0);
	const FTripRun C = RunTrip(G, true, 1.0);
	auto Compare = [](const FTripRun& X, const FTripRun& Y, size_t& Common, size_t& Differ)
	{
		Common = 0; Differ = 0;
		for (const auto& KV : X.Cells)
		{
			const auto It = Y.Cells.find(KV.first);
			if (It == Y.Cells.end()) { continue; }
			++Common;
			if (KV.second.size() != It->second.size() || (KV.second.size() && std::memcmp(KV.second.data(), It->second.data(), KV.second.size() * sizeof(FObjectGPU)) != 0)) { ++Differ; }
		}
	};
	size_t Cab = 0, Dab = 0, Cac = 0, Dac = 0;
	Compare(A, B, Cab, Dab);
	Compare(A, C, Cac, Dac);
	const bool bPool = A.PoolUsed == A.PoolCells && B.PoolUsed == B.PoolCells && C.PoolUsed == C.PoolCells;
	Uni::Check(2, "T2.stream.one_trip", "D-207 one-trip generation for leaf cells: the same records in every cell Ready in both runs as two-trip generation (also when every first arena overflows and falls back), identities resolve, no pool slot leaked; most leaf cells take one trip, and the mean frames from submission to Ready fall by a third or more")
		.Metric("common_ab", double(Cab)).Metric("differ_ab", double(Dab)).Metric("common_ac", double(Cac)).Metric("differ_ac", double(Dac))
		.Metric("fused_b", double(B.Fused)).Metric("overflow_b", double(B.Overflows)).Metric("fused_c", double(C.Fused)).Metric("overflow_c", double(C.Overflows))
		.Metric("latency_two", A.MeanLatency).Metric("latency_one", B.MeanLatency)
		.Pass(Cab > 1000 && Dab == 0 && Cac > 1000 && Dac == 0 && bPool && A.bIdentities && B.bIdentities && C.bIdentities && B.Fused > 1000 && C.Overflows > 0 && B.MeanLatency < 0.67 * A.MeanLatency,
			Uni::Fmt("two-trip vs one-trip: %zu cells Ready in both, %zu differ; vs one-trip with overflowing first arenas: %zu, %zu differ; one-trip cells %llu (overflowed %llu; forced: %llu, overflowed %llu); pool used = Ready ranges: %s (%zu / %zu / %zu slots); frames from submission to Ready %.2f -> %.2f on average",
				Cab, Dab, Cac, Dac, (unsigned long long)B.Fused, (unsigned long long)B.Overflows, (unsigned long long)C.Fused, (unsigned long long)C.Overflows, bPool ? "yes" : "no", A.PoolUsed, B.PoolUsed, C.PoolUsed, A.MeanLatency, B.MeanLatency));
}

UNI_TEST(volume, bg_lut_layout)
{
	// D-209: the background map is re-marched when the LUT's layout changes. The cell-glow grids
	// (D-203) follow the LUT blocks and change size whenever a level starts or finishes loading;
	// the engine compared the whole buffer's size, so at rest it kept re-marching the background
	// (per pixel under uu.BackgroundMode 2). LutBlocksEnd is the layout: with no grids, one level's
	// and three levels', it is the same and the blocks before it are identical, while the buffer's
	// size differs; and it is the buffer's size without cell glow.
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const FCoord Cam = UniScenes::SolarCamera();
	FRefPipeline P(G);
	P.bVelocityLod = false;
	for (int I = 0; I < 12; ++I) { P.Update(Cam, FVec3f{}); }
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 40; Cm.Height = 40;
	const H::FUniView V = Cm.View();
	FVolumeSettings S;
	S.bCellGlow = true;
	const FSliceTable& ST = M.SlicesOf(UniScenes::kStar);
	std::vector<FCellGlowLevel> None, One, Three;
	One.push_back(GlowGrid(ST.LMin, Cam, [](size_t) { return 0.5f; }));
	for (int L = ST.LMin; L <= std::min(ST.LMin + 2, ST.LMax); ++L) { Three.push_back(GlowGrid(L, Cam, [](size_t) { return 0.25f; })); }
	const std::vector<FCellGlowLevel>* Sets[3] = { &None, &One, &Three };
	std::vector<std::vector<H::float4>> Luts;
	std::vector<u32> Ends, Glows;
	for (const auto* Gs : Sets)
	{
		VB.GlowOverride = Gs;
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, V, S);
		Ends.push_back(LutBlocksEnd(Sc.View.GlowLevels, Sc.View.GlowBase, Sc.Lut.size()));
		Glows.push_back(Sc.View.GlowLevels);
		Luts.push_back(Sc.Lut);
	}
	VB.GlowOverride = nullptr;
	FVolumeSettings Off;
	const FVolumeScene ScOff = VB.Build(P, Cam, FVec3f{}, V, Off);
	const u32 EndOff = LutBlocksEnd(ScOff.View.GlowLevels, ScOff.View.GlowBase, ScOff.Lut.size());

	bool bSameBlocks = true;
	for (size_t K = 1; K < Luts.size(); ++K)
	{
		for (u32 I = 0; I < Ends[0] && bSameBlocks; ++I)
		{
			const H::float4 A = Luts[0][I], B = Luts[K][I];
			bSameBlocks = A.x == B.x && A.y == B.y && A.z == B.z && A.w == B.w;
		}
	}
	const bool bEnds = Ends[0] == Ends[1] && Ends[1] == Ends[2] && Ends[0] == u32(Luts[0].size());
	const bool bSizes = Luts[1].size() > Luts[0].size() && Luts[2].size() > Luts[1].size();
	const bool bOff = ScOff.View.GlowLevels == 0u && EndOff == u32(ScOff.Lut.size());
	Uni::Check(2, "T2.vol.bg_lut_layout", "D-209 the LUT's layout for the background map's re-march test is its blocks' end (LutBlocksEnd): the same with no cell-glow grids, one level's and three levels' while the buffer's size changes, the blocks before it identical, and the buffer's size without cell glow")
		.Metric("blocks_end", double(Ends[0])).Metric("lut_none", double(Luts[0].size())).Metric("lut_one", double(Luts[1].size())).Metric("lut_three", double(Luts[2].size()))
		.Pass(bEnds && bSameBlocks && bSizes && bOff && Glows[0] == 0u && Glows[1] == 1u && Glows[2] >= 2u,
			Uni::Fmt("blocks end %u / %u / %u (glow levels %u / %u / %u); LUT %zu / %zu / %zu entries; blocks identical: %s; without cell glow: end %u of %zu",
				Ends[0], Ends[1], Ends[2], Glows[0], Glows[1], Glows[2], Luts[0].size(), Luts[1].size(), Luts[2].size(), bSameBlocks ? "yes" : "no", EndOff, ScOff.Lut.size()));
}
