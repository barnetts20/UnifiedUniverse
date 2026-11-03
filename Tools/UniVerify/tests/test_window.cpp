// test_window.cpp
// Tier 1 window and level checks: toroidal slot bijection, slot stability while sliding,
// entering/exiting diff, seam handling, velocity LOD, and level-assignment coverage.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "UniLevel.h"
#include "UniWindow.h"

#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

using namespace UniCore;

namespace
{
	std::string KeyStr(const FCellKey& K)
	{
		return std::to_string(K.Level) + ":" + ToHex(K.C[0]) + ToHex(K.C[1]) + ToHex(K.C[2]);
	}

	FCellKey RandomCentre(Uni::FTestRng& R, int L)
	{
		FCoord P;
		const unsigned Mode = R.U32() % 4u;
		for (int I = 0; I < 3; ++I)
		{
			FU128 V = FU128::Make(R.U32(), R.U32(), R.U32(), R.U32());
			if (Mode == 0u) { V = FCoord::Origin().Axis(I); }             // nominal origin
			if (Mode == 1u) { V = FU128::Make(0u, 0u, 0u, R.U32() % 4u); } // just past the seam
			P.SetAxis(I, V);
		}
		FCellKey K = CellOf(P, L);
		if (Mode == 1u) { K = Neighbour(K, -int(R.U32() % 3u), 0, 0); }   // straddle the seam
		return K;
	}

	/** Chebyshev window membership by toroidal cell distance, without enumerating cells. */
	bool InWindow(const FWindowGeometry& G, const FCellKey& K)
	{
		if (G.AxisCovered()) { return true; }
		const int Bits = G.AxisBits();
		for (int I = 0; I < 3; ++I)
		{
			const FU128 D = MaskLow(Sub(K.C[I], G.Centre.C[I]), Bits);
			const FU128 NegD = MaskLow(Sub(G.Centre.C[I], K.C[I]), Bits);
			const FU128 NB = FU128::FromU64(u64(G.N));
			if (FU128::Compare(D, NB) > 0 && FU128::Compare(NegD, NB) > 0) { return false; }
		}
		return true;
	}
}

UNI_TEST(window, bijection)
{
	// Exhaustive over N = 1..6 and a spread of levels including every coarse level (124..128)
	// where axes are covered, and random centres including the seam and the origin.
	Uni::FTestRng R(0xB1Eull);
	long long Windows = 0, BadBijection = 0, BadStable = 0, Slides = 0, BadDiff = 0, Rebuilds = 0;
	const int Levels[] = { 0, 1, 20, 31, 32, 33, 58, 64, 75, 96, 100, 120, 123, 124, 125, 126, 127, 128 };
	for (int N = 1; N <= 6; ++N)
	{
		for (int L : Levels)
		{
			for (int Trial = 0; Trial < 6; ++Trial)
			{
				FLevelWindow W(L, N);
				FCellKey C = RandomCentre(R, L);
				FWindowDiff D0 = W.Recentre(C);
				++Windows;

				auto CheckBijection = [&](const FLevelWindow& Win)
				{
					std::set<int> Slots;
					std::set<std::string> Keys;
					const std::vector<FWindowCell> Cells = Win.Geometry().Cells();
					for (const FWindowCell& Cell : Cells)
					{
						Slots.insert(Cell.Slot);
						Keys.insert(KeyStr(Cell.Key));
					}
					// Distinct cells map to distinct slots, Span^3 of them (Side^3, or the whole
					// axis cubed on covered levels).
					const size_t Expect = size_t(Win.Geometry().NumSlots());
					return Slots.size() == Keys.size() && Keys.size() == Cells.size() && Cells.size() == Expect;
				};
				if (!CheckBijection(W)) { ++BadBijection; }
				if (D0.Entering.size() != W.Geometry().Cells().size()) { ++BadDiff; }

				// Slide by random small steps; residents keep their slot unless a seam
				// transition forced a rebuild.
				for (int S = 0; S < 8; ++S)
				{
					std::map<std::string, int> Before;
					for (size_t I = 0; I < W.SlotTable().size(); ++I)
					{
						if (W.SlotTable()[I].bOccupied) { Before[KeyStr(W.SlotTable()[I].Key)] = int(I); }
					}
					const int Dx = int(R.U32() % 5u) - 2, Dy = int(R.U32() % 3u) - 1, Dz = (S % 3 == 0) ? int(R.U32() % 11u) - 5 : 0;
					C = Neighbour(C, Dx, Dy, Dz);
					const FWindowDiff D = W.Recentre(C);
					++Slides;
					if (!CheckBijection(W)) { ++BadBijection; }
					if (D.bRebuilt) { ++Rebuilds; continue; }
					for (size_t I = 0; I < W.SlotTable().size(); ++I)
					{
						const FSlot& Sl = W.SlotTable()[I];
						if (!Sl.bOccupied) { continue; }
						auto It = Before.find(KeyStr(Sl.Key));
						if (It != Before.end() && It->second != int(I)) { ++BadStable; }
					}
					// Diff consistency: entering + staying = window; exiting were resident.
					std::set<std::string> Now;
					for (const FWindowCell& Cell : W.Geometry().Cells()) { Now.insert(KeyStr(Cell.Key)); }
					size_t Staying = 0;
					for (const auto& P : Before) { Staying += Now.count(P.first); }
					if (Staying + D.Entering.size() != W.Geometry().Cells().size() || D.Exiting.size() != Before.size() - Staying) { ++BadDiff; }
				}
			}
		}
	}

	// A unit slide on a plain level enters and exits exactly one face.
	FLevelWindow W(60, 2);
	const FCellKey C0 = CellOf(FCoord::Origin(), 60);
	W.Recentre(C0);
	const FWindowDiff D1 = W.Recentre(Neighbour(C0, 1, 0, 0));
	const bool Face = D1.Entering.size() == 25 && D1.Exiting.size() == 25 && !D1.bRebuilt;

	Uni::Check(1, "T1.window.bijection", "Toroidal slot bijection; residents keep their slot while sliding; correct entering/exiting diff (N = 1..6)")
		.Metric("windows", double(Windows)).Metric("slides", double(Slides)).Metric("bad_bijection", double(BadBijection))
		.Metric("bad_stable", double(BadStable)).Metric("bad_diff", double(BadDiff)).Metric("seam_rebuilds", double(Rebuilds))
		.Pass(BadBijection == 0 && BadStable == 0 && BadDiff == 0 && Face,
			Uni::Fmt("%lld windows, %lld slides (%lld seam rebuilds), %lld bijection / %lld stability / %lld diff violations; unit slide 25 in / 25 out: %s",
				Windows, Slides, Rebuilds, BadBijection, BadStable, BadDiff, Face ? "yes" : "NO"));
}

UNI_TEST(window, boxes)
{
	// D-162: even and stretched boxes (4^3, 4 x 6 x 4, 5 x 4 x 7 ...), over plain, seam and
	// covered levels: slots are a bijection, residents keep their slot as the box slides (centre
	// and placement both move, shape fixed), and the diff is consistent. Plus the placement rule:
	// the box always covers [Fr - Back, Fr + Fwd] and only moves when it must.
	Uni::FTestRng R(0xB0E5ull);
	struct FShape { int Lo[3], Hi[3]; };
	const FShape Shapes[] = { { { -2, -2, -2 }, { 1, 1, 1 } }, { { -1, -2, -1 }, { 2, 1, 2 } }, { { -2, -1, -2 }, { 1, 4, 1 } }, { { -4, -2, -1 }, { 0, 1, 5 } }, { { 0, 0, 0 }, { 3, 3, 3 } } };
	const int Levels[] = { 0, 20, 64, 100, 124, 125, 126, 127, 128 };
	long long Windows = 0, BadBijection = 0, BadStable = 0, BadDiff = 0, Slides = 0;
	for (const FShape& Sh : Shapes) for (int L : Levels) for (int Trial = 0; Trial < 6; ++Trial)
	{
		FWindowGeometry G;
		G.Level = L;
		for (int A = 0; A < 3; ++A) { G.Lo[A] = Sh.Lo[A]; G.Hi[A] = Sh.Hi[A]; }
		FLevelWindow W(G);
		G.Centre = RandomCentre(R, L);
		const FWindowDiff D0 = W.Recentre(G);
		++Windows;
		auto Bij = [&]()
		{
			std::set<int> Slots; std::set<std::string> Keys;
			const std::vector<FWindowCell> Cells = W.Geometry().Cells();
			for (const FWindowCell& C : Cells) { Slots.insert(C.Slot); Keys.insert(KeyStr(C.Key)); }
			return Slots.size() == Keys.size() && Keys.size() == Cells.size() && Cells.size() == size_t(W.Geometry().NumSlots());
		};
		if (!Bij() || D0.Entering.size() != W.Geometry().Cells().size()) { ++BadBijection; }
		for (int S = 0; S < 8; ++S)
		{
			std::map<std::string, int> Before;
			for (size_t I = 0; I < W.SlotTable().size(); ++I) { if (W.SlotTable()[I].bOccupied) { Before[KeyStr(W.SlotTable()[I].Key)] = int(I); } }
			FWindowGeometry Nx = W.Geometry();
			Nx.Centre = Neighbour(Nx.Centre, int(R.U32() % 3u) - 1, int(R.U32() % 3u) - 1, int(R.U32() % 5u) - 2);
			const int Shift = int(R.U32() % 3u) - 1;   // placement moves too, shape fixed
			Nx.Lo[1] += Shift; Nx.Hi[1] += Shift;
			const FWindowDiff D = W.Recentre(Nx);
			++Slides;
			if (!Bij()) { ++BadBijection; }
			if (D.bRebuilt) { continue; }
			for (size_t I = 0; I < W.SlotTable().size(); ++I)
			{
				const FSlot& Sl = W.SlotTable()[I];
				if (!Sl.bOccupied) { continue; }
				auto It = Before.find(KeyStr(Sl.Key));
				if (It != Before.end() && It->second != int(I)) { ++BadStable; }
			}
			std::set<std::string> Now;
			for (const FWindowCell& C : W.Geometry().Cells()) { Now.insert(KeyStr(C.Key)); }
			size_t Staying = 0;
			for (const auto& P : Before) { Staying += Now.count(P.first); }
			if (Staying + D.Entering.size() != W.Geometry().Cells().size() || D.Exiting.size() != Before.size() - Staying) { ++BadDiff; }
		}
	}
	// Placement: random positions and reaches; coverage always, and a still camera never moves it.
	long long BadCover = 0, Moves = 0, Placements = 0;
	for (int I = 0; I < 20000; ++I)
	{
		const double Back = 0.5 + 3.0 * R.Unit(), Fwd = 0.5 + 3.0 * R.Unit(), Fr = R.Unit();
		const int Side = WindowSide(Back, Fwd);
		const int Lo0 = PlaceWindowAxis(Fr, Back, Fwd, Side, false, 0);
		const int Lo1 = PlaceWindowAxis(Fr, Back, Fwd, Side, true, Lo0);
		const double Fr2 = std::min(0.999999, std::max(0.0, Fr + 0.2 * (R.Unit() - 0.5)));
		const int Lo2 = PlaceWindowAxis(Fr2, Back, Fwd, Side, true, Lo0);
		for (int Lo : { Lo0, Lo2 })
		{
			const double F = Lo == Lo0 ? Fr : Fr2;
			if (!(double(Lo) <= std::floor(F - Back) && double(Lo + Side - 1) >= std::floor(F + Fwd))) { ++BadCover; }
		}
		Moves += Lo1 != Lo0 ? 1 : 0;
		++Placements;
	}
	// 4^3 at rest: reach 1.5 each way needs exactly 4 cells.
	const bool Four = WindowSide(1.5, 1.5) == 4;
	Uni::Check(1, "T1.window.boxes", "D-162 window boxes (even, asymmetric, stretched): slot bijection, residents keep slots, consistent diffs; placement always covers the reach and does not move without cause; a 1.5-cell reach is a 4^3 window")
		.Metric("windows", double(Windows)).Metric("slides", double(Slides)).Metric("bad_bijection", double(BadBijection)).Metric("bad_stable", double(BadStable)).Metric("bad_diff", double(BadDiff))
		.Metric("bad_cover", double(BadCover)).Metric("spurious_moves", double(Moves))
		.Pass(BadBijection == 0 && BadStable == 0 && BadDiff == 0 && BadCover == 0 && Moves == 0 && Four,
			Uni::Fmt("%lld windows, %lld slides: %lld bijection / %lld stability / %lld diff violations; %lld placements: %lld uncovered, %lld moved without cause; 1.5-cell reach -> %d cells",
				Windows, Slides, BadBijection, BadStable, BadDiff, Placements, BadCover, Moves, WindowSide(1.5, 1.5)));
}

UNI_TEST(window, velocity_lod)
{
	FVelocityLod V;
	const int K = UniHlsl::UNI_VELOCITY_LOD_K;
	bool Ok = true;
	Ok &= !V.Update(0.2, K);           // 1 cell per 5 frames: active
	Ok &= V.Update(0.26, K);           // faster than 1 per 4: suspended
	Ok &= V.Update(0.22, K);           // inside hysteresis: still suspended
	Ok &= !V.Update(0.19, K);          // below 0.8/K: resumes
	Uni::Check(1, "T1.window.velocity_lod", "Velocity LOD suspends above 1 cell per K = 4 frames, resumes below 0.8 of that")
		.Pass(Ok, Ok ? "transitions as specified" : "wrong transition");
}

UNI_TEST(level, coverage)
{
	// For random objects with relevance R and a camera anywhere in the centre cell, every
	// object with distance <= R lies inside its level's window (N = 2 baseline, and N = 1..6).
	Uni::FTestRng R(0xC0FEull);
	long long Trials = 0, Violations = 0;
	double MinSlack = 1e300;
	for (int N = 1; N <= 6; ++N)
	{
		for (int T = 0; T < 40000; ++T)
		{
			const double LogR = 2.0 + R.Unit() * 120.0;            // R from 4 cm to 2^122 cm
			const double Rel = std::ldexp(1.0, int(LogR)) * (1.0 + R.Unit());
			const int L = LevelForRelevance(Rel, double(N) - 1.0 / UniHlsl::UNI_VELOCITY_LOD_K);
			// Camera: random point; object: random direction at distance in [0, R].
			FCoord Cam = FCoord::FromOriginOffset(i64(R.U32()) << 20, -(i64(R.U32()) << 10), i64(R.U32()));
			FVec3f Fr;
			// Random sub-cell placement of the camera at level L.
			AddDisplacement(Cam, Fr, FVec3d{ R.Unit() * std::ldexp(1.0, L), R.Unit() * std::ldexp(1.0, L), R.Unit() * std::ldexp(1.0, L) });
			double Dir[3] = { R.Unit() * 2 - 1, R.Unit() * 2 - 1, R.Unit() * 2 - 1 };
			const double Len = std::sqrt(Dir[0] * Dir[0] + Dir[1] * Dir[1] + Dir[2] * Dir[2]) + 1e-12;
			const double Dist = Rel * std::sqrt(R.Unit());
			FCoord Obj = Cam;
			FVec3f Fo = Fr;
			AddDisplacement(Obj, Fo, FVec3d{ Dir[0] / Len * Dist, Dir[1] / Len * Dist, Dir[2] / Len * Dist });

			FWindowGeometry G;
			G.Level = L; G.N = N; G.Centre = CellOf(Cam, L);
			const bool Inside = InWindow(G, CellOf(Obj, L));
			++Trials;
			if (!Inside) { ++Violations; }
			MinSlack = std::min(MinSlack, std::ldexp(double(N) - 1.0 / UniHlsl::UNI_VELOCITY_LOD_K, L) / Rel);
		}
	}
	Uni::Check(1, "T1.level.coverage", "L = ceil(log2(R/N_eff)), N_eff = N - 1/K: for random camera offsets in the centre cell, every object with distance <= R is inside its level's window")
		.Metric("trials", double(Trials)).Metric("violations", double(Violations)).Metric("min_coverage_over_R", MinSlack)
		.Pass(Violations == 0 && MinSlack >= 1.0,
			Uni::Fmt("%lld trials over N = 1..6 and R = 4 cm..2^123 cm, %lld violations; min N_eff*2^L / R = %.6f", Trials, Violations, MinSlack));
}
