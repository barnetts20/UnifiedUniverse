#pragma once

// UniHome.h
// D-293 (SHIPS_PLANETS.md §2, §7; D-290, D-291): the home system and the start on its planet.
//
// The home star is an authored anchor (as the home galaxy is): a Sun-like star at the solar
// camera, its seed pinned offline (HomeStarSeed) so the system its seed rolls has an Earth-like
// planet in the habitable zone. Being authored, it exists on the GPU and the CPU alike by
// construction. The "home" scene is the flythrough universe with this one anchor added, so every
// other scene is unchanged. Engine-agnostic.

#include "UniDemoScenes.h"
#include "UniPlanetProps.h"
#include "UniSystem.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace UniScenes
{
	using namespace UniCore;

	namespace HomeConst
	{
		constexpr float SunAbsMag = 4.83f;                 // V, a G2 dwarf
		constexpr double SunRadiusCm = 6.957e10;
		constexpr double MinEarthRadii = 0.8;              // D-291: the home planet near the size proven at real scale (~5,000 km)
		constexpr double MaxEarthRadii = 1.0;
		constexpr double MinInsolation = 0.7;              // the habitable zone, in Earth's insolation
		constexpr double MaxInsolation = 1.4;
		constexpr double MaxEcc = 0.1;                     // a mild orbit: seasons from tilt, not distance
		/** Pinned offline by FindHomeStarSeed (T1.home.system re-finds it): the first seed from 1
		 *  whose system qualifies. Its system: six planets; the home planet is the second (rocky,
		 *  0.81 Earth radii = 5,142 km, 0.913 AU, e 0.05, 1.2x Earth's light, 0.81 g, a 319-day
		 *  year), with three more rocky worlds and gas giants at 5.2 and 50 AU. Changing it changes
		 *  the home system (a reroll of the start). */
		constexpr u32 HomeStarSeed = 35u;
	}

	/** The home star's anchor at seed Seed: Sun-like, at the solar camera's point (8 kpc out in the
	 *  home galaxy's disc). */
	inline FAnchor HomeStarAnchor(u32 Seed)
	{
		FAnchor A;
		A.Coord = SolarCamera();
		A.ArchetypeId = kStar;
		A.AbsMag = HomeConst::SunAbsMag;
		A.RadiusLog2 = float(std::log2(HomeConst::SunRadiusCm));
		A.bFixedOrientation = false;
		A.Seed = Seed;
		return A;
	}

	/** The home planet of a system: the first rocky planet that qualifies (-1: none). */
	inline int HomePlanetOf(const FSystem& S)
	{
		using namespace HomeConst;
		if (S.Bh.bValid) { return -1; }   // no black hole companion at home
		for (size_t I = 0; I < S.Planets.size(); ++I)
		{
			const FPlanet& P = S.Planets[I];
			if (P.Type != EPlanetType::Rocky) { continue; }
			const double Re = P.RadiusCm / SysConst::EarthRadius;
			const FPlanetProps Pr = PlanetPropsOf(S, P);
			if (Re < MinEarthRadii || Re > MaxEarthRadii || P.Ecc > MaxEcc) { continue; }
			if (Pr.InsolationEarth < MinInsolation || Pr.InsolationEarth > MaxInsolation) { continue; }
			if (Pr.Atmosphere != EPlanetAtmo::Terrestrial || !Pr.bOcean) { continue; }
			return int(I);
		}
		return -1;
	}

	/** The home star's record and its cell, as generation places it (AnchorRecord). */
	inline FObjectGPU HomeStarRecord(const FUniverseModel& Model, const FAnchor& A, FCellKey& OutCell)
	{
		OutCell = CellOf(A.Coord, Model.LevelOf(A.ArchetypeId, A.AbsMag));
		return AnchorRecord(Model.Definition(), A, OutCell);
	}

	/** The first seed from First whose home star's system qualifies (0: none in Count tries). */
	inline u32 FindHomeStarSeed(const FUniverseModel& Model, u32 First = 1u, u32 Count = 200000u)
	{
		for (u32 Seed = First; Seed < First + Count; ++Seed)
		{
			FCellKey K;
			const FObjectGPU R = HomeStarRecord(Model, HomeStarAnchor(Seed), K);
			if (HomePlanetOf(BuildSystem(R, FStellarBhConfig{})) >= 0) { return Seed; }
		}
		return 0u;
	}

	/** The home scene: the flythrough universe plus the home star (D-293). */
	inline FUniverseDef Home(const FClusterTuning& T = FClusterTuning(), u32 Seed = HomeConst::HomeStarSeed)
	{
		FUniverseDef D = Flythrough(T);
		D.Anchors.push_back(HomeStarAnchor(Seed));
		return D;
	}

	/** Candidate spawn directions on the home planet (planet frame: its poles on +-Z, D-291),
	 *  deterministic from its seed: on the day side (the star at least 20 degrees up) and within
	 *  40 degrees of the equator. Up to N, in order. */
	inline std::vector<FVec3d> HomeSpawnCandidates(u32 PlanetSeed, const FVec3d& ToStar, int N = 48)
	{
		std::vector<FVec3d> Out;
		const double Ls = Length(ToStar);
		const FVec3d Sun = Ls > 0.0 ? FVec3d{ ToStar.X / Ls, ToStar.Y / Ls, ToStar.Z / Ls } : FVec3d{ 1.0, 0.0, 0.0 };
		const double MaxZ = std::sin(40.0 * SysConst::Pi / 180.0);
		const double MinSun = std::sin(20.0 * SysConst::Pi / 180.0);
		const u32 Base = UniHlsl::Uni_SeedMix(PlanetSeed, ChannelId("HOME_SPAWN"));
		for (u32 K = 0; K < 4096u && int(Out.size()) < N; ++K)
		{
			const u32 S = UniHlsl::Uni_SeedMix(Base, K);
			const double Z = 2.0 * double(UniHlsl::Uni_DrawUnit(S, 0u, 0u)) - 1.0;
			const double Phi = SysConst::TwoPi * double(UniHlsl::Uni_DrawUnit(S, 0u, 1u));
			const double Rxy = std::sqrt(std::max(0.0, 1.0 - Z * Z));
			const FVec3d D{ Rxy * std::cos(Phi), Rxy * std::sin(Phi), Z };
			if (std::fabs(D.Z) > MaxZ) { continue; }
			if (D.X * Sun.X + D.Y * Sun.Y + D.Z * Sun.Z < MinSun) { continue; }
			Out.push_back(D);
		}
		return Out;
	}

	/** What the planet says about the ground at a direction: dry land or not, and its height (m
	 *  above sea level). */
	struct FGroundSample
	{
		bool bValid = false;
		bool bDry = false;
		double HeightM = 0.0;
	};

	/** The spawn point among Candidates: the first whose ground is dry, 2-2,500 m up, and level (its
	 *  four neighbours ProbeM away within MaxStepM of it). Falls back to the levellest dry one, then
	 *  candidate 0. Ground(dir) samples the planet at a unit direction. */
	inline int PickHomeSpawn(const std::vector<FVec3d>& Candidates, double RadiusCm, const std::function<FGroundSample(const FVec3d&)>& Ground,
		double ProbeM = 100.0, double MaxStepM = 15.0)
	{
		int Fallback = -1;
		double FallbackStep = 1.0e30;
		for (size_t I = 0; I < Candidates.size(); ++I)
		{
			const FVec3d D = Candidates[I];
			const FGroundSample C = Ground(D);
			if (!C.bValid || !C.bDry || C.HeightM < 2.0 || C.HeightM > 2500.0) { continue; }
			// Two tangent axes at D.
			const FVec3d Ref = std::fabs(D.Z) < 0.9 ? FVec3d{ 0.0, 0.0, 1.0 } : FVec3d{ 1.0, 0.0, 0.0 };
			FVec3d T1{ Ref.Y * D.Z - Ref.Z * D.Y, Ref.Z * D.X - Ref.X * D.Z, Ref.X * D.Y - Ref.Y * D.X };
			const double L1 = Length(T1);
			T1 = FVec3d{ T1.X / L1, T1.Y / L1, T1.Z / L1 };
			const FVec3d T2{ D.Y * T1.Z - D.Z * T1.Y, D.Z * T1.X - D.X * T1.Z, D.X * T1.Y - D.Y * T1.X };
			const double A = ProbeM * 100.0 / std::max(RadiusCm, 1.0);   // the angle ProbeM subtends
			double Step = 0.0;
			bool bDryAll = true;
			const FVec3d Dirs[4] = { T1, FVec3d{ -T1.X, -T1.Y, -T1.Z }, T2, FVec3d{ -T2.X, -T2.Y, -T2.Z } };
			for (const FVec3d& T : Dirs)
			{
				FVec3d N{ D.X + A * T.X, D.Y + A * T.Y, D.Z + A * T.Z };
				const double Ln = Length(N);
				N = FVec3d{ N.X / Ln, N.Y / Ln, N.Z / Ln };
				const FGroundSample G = Ground(N);
				if (!G.bValid || !G.bDry) { bDryAll = false; break; }
				Step = std::max(Step, std::fabs(G.HeightM - C.HeightM));
			}
			if (!bDryAll) { continue; }
			if (Step <= MaxStepM) { return int(I); }
			if (Step < FallbackStep) { FallbackStep = Step; Fallback = int(I); }
		}
		return Fallback >= 0 ? Fallback : (Candidates.empty() ? -1 : 0);
	}
}
