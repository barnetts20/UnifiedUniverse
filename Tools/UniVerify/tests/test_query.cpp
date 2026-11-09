// test_query.cpp
// D-193: the runtime API's core (UniQuery.h, Core/UniQuery.ush), offline. A streamer driven by
// the CPU backend stands in for the engine's GPU pool: the checks name objects the way the
// engine's readbacks do (cell index in a frame's table, slot in the cell's range) and run the
// pick and query rules through the same shader core as the GPU passes.
//
//  - T2.query.identity: every slot of every Ready cell names the object the reference
//    pipeline gives that identity (record for record); a stale generation names nothing; a
//    planet's dynamic cell names its star and ordinal.
//  - T2.query.pick: a cursor on an isolated sprite picks it; the brightest-and-nearest rule.
//  - T2.query.resident: sphere and nearest queries against exact 128-bit distances.
//  - T2.query.events: resident / evicted events over a flight track the CPU-known objects.
//  - T2.query.watch: visible / hidden events for watched galaxies while the camera turns.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../scenes/UniScenes.h"
#include "UniQuery.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	std::string KeyStr(const FCellKey& K) { return std::to_string(K.Level) + ToHex(K.C[0]) + ToHex(K.C[1]) + ToHex(K.C[2]); }

	bool Hidden(const FObjectGPU& O) { return ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) != 0u; }

	/** A camera track: Steps moves of (Dx, Dy, 0) x the level-64 cell width. */
	std::vector<FCoord> Track(const FCoord& Start, int Steps, double Dx, double Dy)
	{
		std::vector<FCoord> Path;
		FCoord C = Start;
		for (int I = 0; I < Steps; ++I)
		{
			Path.push_back(C);
			FVec3f F;
			AddDisplacement(C, F, FVec3d{ Dx * CellWidthCm(64), Dy * CellWidthCm(64), 0.0 });
		}
		return Path;
	}

	void Settle(FUniStreamer& S, const FCoord& At)
	{
		for (int I = 0; I < 60 && !S.AllReady(); ++I) { S.Update(At); }
	}

	/** The project pass on the CPU: every resident object through Uni_ProjectObject, keeping
	 *  the visible sprites and their (cell index, slot). */
	void ProjectAll(const std::vector<FCellGPU>& Cells, const std::vector<FObjectGPU>& Pool, const FUniStreamer& S, const std::vector<FCellKey>& Keys,
		const H::FUniView& V, std::vector<H::FUniSprite>& OutSprites, std::vector<std::pair<u32, u32>>& OutSource)
	{
		for (size_t Ci = 0; Ci < Cells.size(); ++Ci)
		{
			const FCellGPU& C = Cells[Ci];
			const H::uint4 Dx(C.DeltaX[0], C.DeltaX[1], C.DeltaX[2], C.DeltaX[3]), Dy(C.DeltaY[0], C.DeltaY[1], C.DeltaY[2], C.DeltaY[3]), Dz(C.DeltaZ[0], C.DeltaZ[1], C.DeltaZ[2], C.DeltaZ[3]);
			const float Fade = Ci < Keys.size() ? S.LevelFade(Keys[Ci].Level) : 1.0f;
			for (u32 I = 0; I < C.PoolCount; ++I)
			{
				const H::FUniSprite Sp = H::Uni_ProjectObject(Dx, Dy, Dz, C.Level, Fade, ToHlsl(Pool[C.PoolStart + I]), V, false);
				if ((Sp.Flags & H::UNI_SPRITE_VISIBLE) == 0u) { continue; }
				OutSprites.push_back(Sp);
				OutSource.push_back({ u32(Ci), I });
			}
		}
	}

	struct FWorld
	{
		FUniverseModel M;
		FRefGenerator G;
		FCpuGenBackend Be;
		FUniStreamer S;
		explicit FWorld(const FUniverseDef& D) : M(D), G(M), Be(G, 2097152u), S(G, Be)
		{
			S.bVelocityLod = false;
			S.bAsyncIntegrals = false;
		}
	};
}

UNI_TEST(query, identity)
{
	// The flythrough scene from the solar camera, after a short track: every non-blank slot of
	// every Ready cell, named through the frame's table, is the reference pipeline's object of
	// that identity, bit for bit; generation + 1 names nothing; planets name their star.
	FWorld W(UniScenes::Flythrough());
	const std::vector<FCoord> Path = Track(UniScenes::SolarCamera(), 6, 0.6, 0.3);
	FRefPipeline P(W.G);
	P.bVelocityLod = false;
	for (const FCoord& X : Path) { P.Update(X, FVec3f{}); W.S.Update(X); }
	Settle(W.S, Path.back());
	std::map<std::string, std::map<u32, FObjectGPU>> Ref;
	for (const auto& KV : P.CacheRef()) { for (size_t I = 0; I < KV.second.Objects.size(); ++I) { Ref[KeyStr(KV.first)][KV.second.Indices[I]] = KV.second.Objects[I]; } }
	FFrameCells F;
	const std::vector<FCellGPU> Cells = W.S.CellTable(&F.Keys, &F.Generations);
	const std::vector<FObjectGPU>& Pool = W.Be.PoolData();
	size_t Named = 0, Wrong = 0, Unnamed = 0, StaleNamed = 0, Anchors = 0;
	for (size_t Ci = 0; Ci < Cells.size(); ++Ci)
	{
		const auto Rt = Ref.find(KeyStr(F.Keys[Ci]));
		for (u32 I = 0; I < Cells[Ci].PoolCount; ++I)
		{
			const FObjectGPU& O = Pool[Cells[Ci].PoolStart + I];
			if (Hidden(O)) { continue; }
			FObjectId Id; int Pl = -1; FCellKey Rc;
			if (!NameSlot(F, W.S, u32(Ci), I, Id, Pl, Rc)) { ++Unnamed; continue; }
			++Named;
			Anchors += Id.IsAnchor() ? 1 : 0;
			const bool bOk = Rt != Ref.end() && Rt->second.count(Id.Index) && std::memcmp(&Rt->second.at(Id.Index), &O, sizeof(FObjectGPU)) == 0 && Id.Cell == F.Keys[Ci] && Pl == -1;
			Wrong += bOk ? 0 : 1;
			if (I % 97u == 0u)
			{
				FObjectId Ix;
				StaleNamed += W.S.IdentityAt(F.Keys[Ci], F.Generations[Ci] + 1u, I, Ix) ? 1 : 0;
			}
		}
	}
	// Planets: activate the system of the nearest star (the camera moved to it), then name
	// each planet's dynamic cell.
	size_t PlanetsNamed = 0, PlanetsWrong = 0;
	double WorstPlanetCm = 0.0;
	{
		const FCoord Cam = Path.back();
		int BestCi = -1; u32 BestSlot = 0; double BestD = 1e300;
		for (size_t Ci = 0; Ci < Cells.size(); ++Ci)
		{
			for (u32 I = 0; I < Cells[Ci].PoolCount; ++I)
			{
				const FObjectGPU& O = Pool[Cells[Ci].PoolStart + I];
				if (Hidden(O) || (O.A[3] & 0xFFFFu) != UniScenes::kStar) { continue; }
				const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
				const double D = Length(ToDouble(Sub(ObjectCoord(F.Keys[Ci], Off), Cam)));
				if (D < BestD) { BestD = D; BestCi = int(Ci); BestSlot = I; }
			}
		}
		if (BestCi >= 0)
		{
			const FObjectGPU& Star = Pool[Cells[size_t(BestCi)].PoolStart + BestSlot];
			FObjectId StarId; int Pl = -1; FCellKey Rc;
			NameSlot(F, W.S, u32(BestCi), BestSlot, StarId, Pl, Rc);
			const u32 Off[3] = { Star.A[0], Star.A[1], Star.A[2] };
			const FCoord StarC = ObjectCoord(F.Keys[size_t(BestCi)], Off);
			FSystemManager Sys;
			Sys.Report(F.Keys[size_t(BestCi)], Star, StarC, 1, &StarId);
			std::vector<FCellGPU> DynCells;
			std::vector<FObjectGPU> DynRecords;
			Sys.Build(StarC, 1000.0, 0u, 64u, DynCells, DynRecords, nullptr, &F.Bodies);
			const auto& Act = Sys.ActiveSystems();
			for (size_t B = 0; B < DynCells.size(); ++B)
			{
				FObjectId Id; FCellKey Cell;
				if (!NameSlot(F, W.S, u32(F.Keys.size() + B), 0u, Id, Pl, Cell)) { ++PlanetsWrong; continue; }
				++PlanetsNamed;
				const FObjectInfo Info = MakeObjectInfo(Id, Cell, DynRecords[B], StarC, Pl);
				const FActiveSystem& A = Act.begin()->second;
				FCoord Want = A.Star;
				FVec3f Fr{};
				AddDisplacement(Want, Fr, PlanetOffset(A.Sys.Planets[size_t(Pl)], 1000.0));
				WorstPlanetCm = std::max(WorstPlanetCm, Length(ToDouble(Sub(Info.Coord, Want))));
				PlanetsWrong += (Id != StarId || Pl != int(B)) ? 1 : 0;
			}
		}
	}
	Uni::Check(2, "T2.query.identity", "D-193 naming objects as the GPU readbacks do (cell index in the frame's table, slot): every non-blank slot of every Ready cell is the reference pipeline's object of that identity, bit for bit; a newer generation names nothing (stale); a planet's dynamic cell names its star and ordinal, its coordinate within 8 cm of its orbit position")
		.Metric("named", double(Named)).Metric("wrong", double(Wrong)).Metric("unnamed", double(Unnamed)).Metric("stale_named", double(StaleNamed)).Metric("anchors", double(Anchors)).Metric("planets", double(PlanetsNamed)).Metric("worst_planet_cm", WorstPlanetCm)
		.Pass(Named > 10000 && Wrong == 0 && Unnamed == 0 && StaleNamed == 0 && PlanetsNamed >= 1 && PlanetsWrong == 0 && WorstPlanetCm <= 8.0,
			Uni::Fmt("%zu objects in %zu cells named (%zu anchors), %zu wrong, %zu unnamed; %zu stale lookups named something; %zu planets named, %zu wrong, worst %.1f cm from their orbit position",
				Named, Cells.size(), Anchors, Wrong, Unnamed, StaleNamed, PlanetsNamed, PlanetsWrong, WorstPlanetCm));
}

UNI_TEST(query, pick)
{
	// The galaxy-to-star scene from the solar camera, 960 x 540, looking along the disc: the
	// frame's visible sprites as the project pass makes them. A cursor on a sprite no other
	// sprite reaches picks that sprite, and the pick names its object; the rule itself on
	// constructed pairs (a bright star 3 px away beats a faint one under the cursor unless it is
	// out of reach).
	FWorld W(UniScenes::GalaxyToStar());
	const FCoord Cam = UniScenes::SolarCamera();
	W.S.Update(Cam);
	Settle(W.S, Cam);
	FFrameCells F;
	const std::vector<FCellGPU> Cells = W.S.CellTable(&F.Keys, &F.Generations);
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 960; Cm.Height = 540; Cm.LookAlong({ -1.0, 0.2, 0.05 });
	const H::FUniView V = Cm.View();
	std::vector<H::FUniSprite> Sprites;
	std::vector<std::pair<u32, u32>> Src;
	ProjectAll(Cells, W.Be.PoolData(), W.S, F.Keys, V, Sprites, Src);
	const float HalfW = 480.0f, HalfH = 270.0f, Ppr = float(Cm.FocalPx()), MinPx = 4.0f;
	size_t Isolated = 0, IsolatedRight = 0, Crowded = 0, NamedRight = 0, Tried = 0, OutOfReach = 0;
	for (size_t I = 0; I < Sprites.size(); I += std::max<size_t>(1, Sprites.size() / 3000))
	{
		++Tried;
		const float Cx = Sprites[I].NdcX, Cy = Sprites[I].NdcY;
		size_t Reaching = 0;
		for (const H::FUniSprite& O : Sprites) { Reaching += H::Uni_PickScore(O, H::float2(Cx, Cy), H::float2(HalfW, HalfH), Ppr, MinPx) > 0.0f ? 1 : 0; }
		const int Got = PickSprites(Sprites, Cx, Cy, HalfW, HalfH, Ppr, MinPx);
		if (Got < 0) { continue; }
		// The winner reaches the cursor (independently: its centre's pixel distance).
		const H::FUniSprite& Gs = Sprites[size_t(Got)];
		const double Dpx = std::hypot((double(Gs.NdcX) - Cx) * HalfW, (double(Gs.NdcY) - Cy) * HalfH);
		const double Reach = std::max(double(MinPx), std::max(2.0 * Gs.AngularRadius, double(Gs.PhysAngularRadius)) * Ppr);
		OutOfReach += Dpx > Reach * (1.0 + 1e-5) ? 1 : 0;
		if (Reaching == 1) { ++Isolated; IsolatedRight += Got == int(I) ? 1 : 0; } else { ++Crowded; }
		FObjectId Id; int Pl; FCellKey Rc;
		if (NameSlot(F, W.S, Src[size_t(Got)].first, Src[size_t(Got)].second, Id, Pl, Rc))
		{
			const FObjectGPU& Rec = W.Be.PoolData()[Cells[Src[size_t(Got)].first].PoolStart + Src[size_t(Got)].second];
			const FObjectInfo Info = MakeObjectInfo(Id, Rc, Rec, Cam);
			NamedRight += (Info.Seed == Rec.B[0] && Info.Archetype == (Rec.A[3] & 0xFFFFu)) ? 1 : 0;
		}
	}
	// The rule on constructed pairs.
	auto Make = [](float Nx, float L, float Ang) { H::FUniSprite S{}; S.NdcX = Nx; S.NdcY = 0.0f; S.Radiance = H::float3(L, L, L); S.AngularRadius = Ang; S.PhysAngularRadius = 0.0f; S.Flags = H::UNI_SPRITE_VISIBLE; return S; };
	const float Px = 1.0f / HalfW;
	const float CoreAng = 1.0f / Ppr;   // a 1 px core sigma: reach max(4, 2) = 4 px
	const std::vector<H::FUniSprite> NearBright = { Make(0.0f, 1.0f, CoreAng), Make(3.0f * Px, 3.0f, CoreAng) };
	const std::vector<H::FUniSprite> FarBright = { Make(0.0f, 1.0f, CoreAng), Make(6.0f * Px, 30.0f, CoreAng) };
	const std::vector<H::FUniSprite> CloseCall = { Make(0.0f, 1.0f, CoreAng), Make(3.0f * Px, 1.5f, CoreAng) };   // 1.5 / (1 + 0.5625) < 1
	const bool bRule = PickSprites(NearBright, 0, 0, HalfW, HalfH, Ppr, MinPx) == 1 && PickSprites(FarBright, 0, 0, HalfW, HalfH, Ppr, MinPx) == 0
		&& PickSprites(CloseCall, 0, 0, HalfW, HalfH, Ppr, MinPx) == 0 && PickSprites(NearBright, 0.5f, 0.5f, HalfW, HalfH, Ppr, MinPx) == -1;
	const double IsoFrac = Isolated ? double(IsolatedRight) / double(Isolated) : 0.0;
	Uni::Check(2, "T2.query.pick", "D-193 pick (Uni_PickScore: brightest under the cursor, weighted by nearness): over up to 3,000 of the frame's visible sprites, a cursor on a sprite nothing else reaches picks it (every time); every winner reaches the cursor and names its own object; on constructed pairs a 3x brighter star 3 px away wins, a bright one out of reach does not, a 1.5x brighter one 3 px away does not, empty sky picks nothing")
		.Metric("sprites", double(Sprites.size())).Metric("tried", double(Tried)).Metric("isolated", double(Isolated)).Metric("isolated_right", IsoFrac).Metric("crowded", double(Crowded)).Metric("out_of_reach", double(OutOfReach)).Metric("named_right", double(NamedRight))
		.Pass(Sprites.size() > 500 && Isolated > 100 && IsolatedRight == Isolated && OutOfReach == 0 && NamedRight == Isolated + Crowded && bRule,
			Uni::Fmt("%zu visible sprites; %zu cursors: %zu on isolated sprites (%zu picked them), %zu crowded; %zu winners out of reach; %zu named their own object; rule on constructed pairs %s",
				Sprites.size(), Tried, Isolated, IsolatedRight, Crowded, OutOfReach, NamedRight, bRule ? "right" : "WRONG"));
}

UNI_TEST(query, resident)
{
	// Sphere and nearest queries (Uni_QueryRelative, Uni_QueryCellMayHit, Uni_QueryMatch) over
	// the flythrough scene's resident set, against exact 128-bit distances: centres at the
	// camera, at a resident star 0.4 pc away and at a point 3 kpc away; radii from 1 pc to 30
	// kpc; filters by archetype and magnitude. A hit set may differ from the exact one only
	// within 1e-5 of the radius; distances within 1e-5 (relative) of exact; nearest is exact.
	FWorld W(UniScenes::Flythrough());
	const FCoord Cam = UniScenes::SolarCamera();
	W.S.Update(Cam);
	Settle(W.S, Cam);
	std::vector<FCellKey> Keys;
	const std::vector<FCellGPU> Cells = W.S.CellTable(&Keys);
	const std::vector<FObjectGPU>& Pool = W.Be.PoolData();
	const double Pc = 3.0856775814913673e18;
	struct FCase { FCoord Centre; double R; u32 Mask; float MaxMag; const char* Name; };
	std::vector<FCase> Cases;
	Cases.push_back({ Cam, 10.0 * Pc, 0u, 99.0f, "camera 10 pc" });
	Cases.push_back({ Cam, 30.0 * Pc, 1u << UniScenes::kStar, 5.0f, "camera 30 pc, stars brighter than M 5" });
	{
		// A resident star about 0.4 pc away.
		FCoord Best = Cam; double Bd = 1e300;
		for (size_t Ci = 0; Ci < Cells.size(); ++Ci) for (u32 I = 0; I < Cells[Ci].PoolCount; ++I)
		{
			const FObjectGPU& O = Pool[Cells[Ci].PoolStart + I];
			if (Hidden(O) || (O.A[3] & 0xFFFFu) != UniScenes::kStar) { continue; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const FCoord X = ObjectCoord(Keys[Ci], Off);
			const double D = std::abs(Length(ToDouble(Sub(X, Cam))) - 0.4 * Pc);
			if (D < Bd) { Bd = D; Best = X; }
		}
		Cases.push_back({ Best, 1.0 * Pc, 0u, 99.0f, "a star 0.4 pc away, 1 pc" });
	}
	{
		FCoord Far = Cam; FVec3f Fr{};
		AddDisplacement(Far, Fr, FVec3d{ 3000.0 * Pc, -1200.0 * Pc, 400.0 * Pc });
		Cases.push_back({ Far, 30000.0 * Pc, 0u, 99.0f, "3 kpc away, 30 kpc" });
	}
	size_t TotalHits = 0, Mismatch = 0, NearestWrong = 0;
	double WorstRel = 0.0;
	std::string Detail;
	for (const FCase& Cs : Cases)
	{
		H::uint4 Qd[3];
		QueryDeltaUint4(Cs.Centre, Cam, Qd);
		const std::vector<FQueryHit> Hits = QueryPool(Cells, Pool, Qd, float(Cs.R), Cs.Mask, Cs.MaxMag, H::UNI_QUERY_MODE_SPHERE);
		std::set<std::pair<u32, u32>> Got;
		for (const FQueryHit& Ht : Hits) { Got.insert({ Ht.CellIndex, Ht.Slot }); }
		size_t Exact = 0;
		double Nearest = 1e300;
		for (size_t Ci = 0; Ci < Cells.size(); ++Ci) for (u32 I = 0; I < Cells[Ci].PoolCount; ++I)
		{
			const FObjectGPU& O = Pool[Cells[Ci].PoolStart + I];
			if (!H::Uni_QueryMatch(ToHlsl(O), Cs.Mask, Cs.MaxMag)) { continue; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const double D = Length(ToDouble(Sub(ObjectCoord(Keys[Ci], Off), Cs.Centre)));
			Nearest = std::min(Nearest, D);
			const bool bIn = D <= Cs.R, bGot = Got.count({ u32(Ci), I }) != 0;
			Exact += bIn ? 1 : 0;
			if (bIn != bGot && std::abs(D - Cs.R) > 1e-5 * Cs.R) { ++Mismatch; }
		}
		for (const FQueryHit& Ht : Hits)
		{
			const FObjectGPU& O = Ht.Record;
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const double D = Length(ToDouble(Sub(ObjectCoord(Keys[Ht.CellIndex], Off), Cs.Centre)));
			WorstRel = std::max(WorstRel, std::abs(double(Ht.DistanceCm) - D) / std::max(D, 1.0));
		}
		const std::vector<FQueryHit> Nh = QueryPool(Cells, Pool, Qd, 0.0f, Cs.Mask, Cs.MaxMag, H::UNI_QUERY_MODE_NEAREST);
		if (Nh.empty() || std::abs(double(Nh[0].DistanceCm) - Nearest) > 1e-5 * std::max(Nearest, 1.0)) { ++NearestWrong; }
		TotalHits += Hits.size();
		Detail += Uni::Fmt("%s%s: %zu hits (exact %zu)", Detail.empty() ? "" : "; ", Cs.Name, Hits.size(), Exact);
	}
	Uni::Check(2, "T2.query.resident", "D-193 resident queries through the shared core against exact 128-bit distances: hit sets equal to the exact ones except within 1e-5 of the radius; hit distances within 1e-5 relative; nearest exact; four centres and radii from 1 pc to 30 kpc, with archetype and magnitude filters")
		.Metric("hits", double(TotalHits)).Metric("mismatch", double(Mismatch)).Metric("worst_rel_distance", WorstRel).Metric("nearest_wrong", double(NearestWrong))
		.Pass(TotalHits > 100 && Mismatch == 0 && WorstRel < 1e-5 && NearestWrong == 0,
			Uni::Fmt("%s; %zu mismatches outside the radius tolerance, distances within %.1e, %zu nearest wrong", Detail.c_str(), Mismatch, WorstRel, NearestWrong));
}

UNI_TEST(query, events)
{
	// The flythrough scene over a 12-step track: after every frame, the objects reported
	// resident (minus those reported evicted) are exactly the CPU-known objects of the Ready
	// cells whose archetype is wanted; no object is reported twice or evicted unreported. Half
	// way the wanted archetypes change (galaxies, then galaxies and stars-with-systems' parent
	// archetypes off: none), then back.
	FWorld W(UniScenes::Flythrough());
	const std::vector<FCoord> Path = Track(UniScenes::SolarCamera(), 12, 0.7, 0.4);
	FResidentEvents Ev;
	std::set<std::string> Have;
	size_t Doubles = 0, Ghosts = 0, Disagree = 0, Resident = 0, Evicted = 0, Frames = 0, MaxHave = 0;
	auto Name = [](const FObjectId& Id) { return KeyStr(Id.Cell) + ":" + std::to_string(Id.Index); };
	auto Apply = [&](const std::vector<FObjectInfo>& R, const std::vector<FObjectId>& E)
	{
		for (const FObjectId& Id : E) { Ghosts += Have.erase(Name(Id)) ? 0 : 1; ++Evicted; }
		for (const FObjectInfo& I : R) { Doubles += Have.insert(Name(I.Id)).second ? 0 : 1; ++Resident; }
	};
	auto Expected = [&](u32 Mask)
	{
		std::set<std::string> X;
		W.S.ForEachReady([&](const FCellKey& K, u32, u32, float)
		{
			const FCellContent* C = W.S.KnownObjects(K);
			if (!C) { return; }
			for (size_t I = 0; I < C->Objects.size(); ++I)
			{
				const u32 A = C->Objects[I].A[3] & 0xFFFFu;
				if (Mask != 0u && A < 32u && (Mask & (1u << A)) && !Hidden(C->Objects[I])) { X.insert(Name(FObjectId{ K, C->Indices[I] })); }
			}
		});
		return X;
	};
	u32 Mask = 1u << UniScenes::kGalaxy;
	{
		std::vector<FObjectInfo> R; std::vector<FObjectId> E;
		Ev.SetArchMask(Mask, W.S, Path[0], R, E);
		Apply(R, E);
	}
	for (size_t Fr = 0; Fr < Path.size() + 30; ++Fr)
	{
		const FCoord& X = Path[std::min(Fr, Path.size() - 1)];
		W.S.Update(X);
		std::vector<FObjectInfo> R; std::vector<FObjectId> E;
		Ev.Update(W.S, X, R, E);
		Apply(R, E);
		if (Fr == 8 || Fr == 14)
		{
			R.clear(); E.clear();
			Mask = Fr == 8 ? 0u : (1u << UniScenes::kGalaxy) | (1u << UniScenes::kCluster);
			Ev.SetArchMask(Mask, W.S, X, R, E);
			Apply(R, E);
		}
		++Frames;
		MaxHave = std::max(MaxHave, Have.size());
		if (Have != Expected(Mask)) { ++Disagree; }
	}
	Uni::Check(2, "T2.query.events", "D-193 resident / evicted events for CPU-known objects over a 12-step flight (+30 settling frames), the wanted archetypes changed twice (galaxies; none; galaxies and clusters): after every frame the reported set is exactly the Ready cells' wanted objects; nothing reported resident twice or evicted unreported")
		.Metric("frames", double(Frames)).Metric("resident_events", double(Resident)).Metric("evicted_events", double(Evicted)).Metric("max_reported", double(MaxHave)).Metric("disagree_frames", double(Disagree)).Metric("doubles", double(Doubles)).Metric("ghosts", double(Ghosts))
		.Pass(Resident > 100 && Evicted > 50 && Disagree == 0 && Doubles == 0 && Ghosts == 0,
			Uni::Fmt("%zu frames: %zu resident and %zu evicted events, up to %zu reported at once; %zu frames disagreed, %zu doubles, %zu ghost evictions",
				Frames, Resident, Evicted, MaxHave, Disagree, Doubles, Ghosts));
}

UNI_TEST(query, watch)
{
	// The flythrough scene at the solar camera: the 64 apparently brightest resident galaxies
	// (the CPU knows every galaxy: they emit stars) watched while the
	// camera turns through 360 degrees in 10 degree steps (90 degree view). Every frame the
	// watch's seen set follows its events, and agrees with an independent test (exact direction
	// in double inside the frustum widened by the galaxy's angular radius, and the shared
	// photometry's visibility) except within 0.1 degree of the frustum's edge.
	FWorld W(UniScenes::Flythrough());
	const FCoord Cam = UniScenes::SolarCamera();
	W.S.Update(Cam);
	Settle(W.S, Cam);
	for (int I = 0; I < 30; ++I) { W.S.Update(Cam); }   // level fades complete
	FVisibilityWatch Wt;
	std::vector<std::pair<FObjectId, FObjectGPU>> Gal;
	std::vector<std::pair<float, std::pair<FObjectId, FObjectGPU>>> All;
	W.S.ForEachReady([&](const FCellKey& K, u32, u32, float)
	{
		const FCellContent* C = W.S.KnownObjects(K);
		if (!C) { return; }
		for (size_t I = 0; I < C->Objects.size(); ++I)
		{
			const FObjectGPU& O = C->Objects[I];
			if ((O.A[3] & 0xFFFFu) != UniScenes::kGalaxy || Hidden(O)) { continue; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const double R = Length(ToDouble(Sub(ObjectCoord(K, Off), Cam)));
			All.push_back({ H::Uni_ApparentMag(H::Uni_ObjectAbsMag(ToHlsl(O)), float(std::max(R, 1.0))), { FObjectId{ K, C->Indices[I] }, O } });
		}
	});
	std::sort(All.begin(), All.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
	for (size_t I = 0; I < All.size() && I < 64; ++I) { Gal.push_back(All[I].second); }
	for (const auto& G : Gal) { Wt.Watch(G.first); }
	std::set<std::string> Seen;
	auto Name = [](const FObjectId& Id) { return KeyStr(Id.Cell) + ":" + std::to_string(Id.Index); };
	size_t Events = 0, Disagree = 0, EdgeSkips = 0, BadEvents = 0, SeenMax = 0;
	for (int Step = 0; Step <= 36; ++Step)
	{
		const double A = Step * 10.0 * 3.141592653589793 / 180.0;
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 960; Cm.Height = 540; Cm.LookAlong({ std::cos(A), std::sin(A), 0.15 });
		const H::FUniView V = Cm.View();
		std::vector<FObjectInfo> Vis; std::vector<FObjectId> Hid;
		Wt.Update(W.S, Cam, V, Vis, Hid);
		for (const FObjectInfo& I : Vis) { BadEvents += Seen.insert(Name(I.Id)).second ? 0 : 1; ++Events; }
		for (const FObjectId& Id : Hid) { BadEvents += Seen.erase(Name(Id)) ? 0 : 1; ++Events; }
		SeenMax = std::max(SeenMax, Seen.size());
		for (const auto& G : Gal)
		{
			const u32 Off[3] = { G.second.A[0], G.second.A[1], G.second.A[2] };
			const FVec3d Rel = ToDouble(Sub(ObjectCoord(G.first.Cell, Off), Cam));
			const double R = Length(Rel);
			const double Rad = std::exp2(double(H::Uni_ObjectRadiusLog2(ToHlsl(G.second))));
			const double Ang = std::asin(std::min(1.0, Rad / std::max(R, 1.0)));
			const double Fz = (Rel.X * Cm.Forward.X + Rel.Y * Cm.Forward.Y + Rel.Z * Cm.Forward.Z) / R;
			const double Nx = (Rel.X * Cm.Right.X + Rel.Y * Cm.Right.Y + Rel.Z * Cm.Right.Z) / R;
			const double Ny = (Rel.X * Cm.Up.X + Rel.Y * Cm.Up.Y + Rel.Z * Cm.Up.Z) / R;
			// Angles off the frustum's side planes (positive: inside).
			const double Hx = std::atan(Cm.TanX()), Hy = std::atan(Cm.TanY());
			const double Ax = Hx - std::abs(std::atan2(Nx, Fz)), Ay = Hy - std::abs(std::atan2(Ny, Fz));
			const double Inside = std::min(Ax, Ay) + Ang;
			if (std::abs(Inside) < 0.1 * 3.141592653589793 / 180.0 + Ang) { ++EdgeSkips; continue; }
			const float Vis2 = H::Uni_ObjectVisibility(ToHlsl(G.second), float(R), float(Rad / R), V) * W.S.LevelFade(G.first.Cell.Level);
			const bool bWant = Inside > 0.0 && Fz > -std::sin(Ang) && Vis2 > 0.0f;
			if (bWant != (Seen.count(Name(G.first)) != 0)) { ++Disagree; }
		}
	}
	Uni::Check(2, "T2.query.watch", "D-193 visibility watch (Uni_ObjectSeen) on up to 64 galaxies while the camera turns 360 degrees in 10 degree steps: the seen set follows its events (no double or ghost event; at least 20 events, 3 seen at once) and agrees with an independent double-precision frustum and visibility test except within 0.1 degree (+ the galaxy's radius) of the frustum's edge")
		.Metric("watched", double(Gal.size())).Metric("events", double(Events)).Metric("bad_events", double(BadEvents)).Metric("disagree", double(Disagree)).Metric("edge_skips", double(EdgeSkips)).Metric("seen_max", double(SeenMax))
		.Pass(Gal.size() >= 8 && Events >= 20 && SeenMax >= 3 && BadEvents == 0 && Disagree == 0,
			Uni::Fmt("%zu galaxies watched over 37 views: %zu events, %zu bad; %zu disagreements with the independent test (%zu near the edge skipped); up to %zu seen at once",
				Gal.size(), Events, BadEvents, Disagree, EdgeSkips, SeenMax));
}
