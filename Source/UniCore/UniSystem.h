#pragma once

// UniSystem.h
// Star systems and planets (B-002, D-142..D-145). Engine-agnostic.
//
//   - A system is generated from its star's record alone (seed, absolute magnitude, radius,
//     colour), so it is the same every time it is activated: deterministic, on rails (owner
//     decision, 2026-11-01). New seed channel UNI_CH_SYSTEM; every rolled quantity keys its
//     stream by name (CLAUDE.md rule 6). Stars' own content is untouched (no reroll).
//   - Orbits are Keplerian, evaluated at the universe clock t (seconds): position(t) is a pure
//     function of the elements and t. Double precision on the CPU.
//   - Planets shine by reflected light: m = M_star - 2.5 log10(A_g Phi(alpha) R^2 (10 pc)^2 /
//     (r^2 Delta^2)), with a Lambert phase function. The GPU sprite path takes an absolute
//     magnitude and applies 5 log10(Delta / 10 pc) itself, so the CPU writes the planet's
//     effective absolute magnitude M_eff = m - 5 log10(Delta / 10 pc) each frame (D-144).
//   - Activation: the project pass appends stars inside Uni_SystemActivationRadius (shared
//     core, UniSystem.ush) for a readback; FSystemManager keeps them active until the camera is
//     beyond 1.25 x the radius (exact distance, CPU), so the edge does not flicker.
//   - Drawing (D-143): each planet becomes one dynamic cell for the existing project pass: the
//     level-34 cell containing it this frame, so its record's offset is exact to 4 cm and the
//     camera-relative vector stays exact (D-104).
//
// Not in v1 (owner): binaries and multiple stars. When they come they belong at the system
// level, so one galaxy-level sprite stands in for the system and resolves into its stars.

#include "UniGenRef.h"
#include "UniScaleLadder.h"
#include "UniSeed.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace UniCore
{
	namespace SysConst
	{
		constexpr double GMSun = 1.32712440018e26;   // cm^3 / s^2
		constexpr double EarthRadius = 6.371e8;      // cm
		constexpr double Pi = 3.14159265358979323846;
		constexpr double TwoPi = 6.28318530717958647692;
		constexpr double TenPc = 10.0 * Units::Pc;
		constexpr u32 MaxPlanets = 10;
		constexpr double OuterLimitAu = 60.0;        // x sqrt(L): no planet beyond
		constexpr double SnowLineAu = 2.7;           // x sqrt(L)
		constexpr double InnerAu = 0.04;             // x sqrt(L): inner edge of the orbit draws, before its roll
		constexpr float MaxEffMag = 60.0f;           // beyond this a planet is hidden (F16 keeps 0.03 mag steps)
	}

	enum class EPlanetType : u32 { Rocky = 0, GasGiant = 1, IceGiant = 2 };

	struct FPlanet
	{
		u32 Seed = 0;
		u32 Index = 0;
		EPlanetType Type = EPlanetType::Rocky;
		double SemiMajorCm = 0.0;
		double Ecc = 0.0;
		double Inc = 0.0, Node = 0.0, Peri = 0.0;   // radians
		double MeanAnomaly0 = 0.0;                   // at t = 0
		double PeriodS = 0.0;
		double RadiusCm = 0.0;
		double Albedo = 0.0;                         // geometric
		float Tint[3] = { 1.0f, 1.0f, 1.0f };
	};

	struct FSystem
	{
		u32 StarSeed = 0;
		float StarAbsMag = 0.0f;
		double StarRadiusCm = 0.0;
		float StarRgb[3] = { 1.0f, 1.0f, 1.0f };
		double LumSun = 1.0, MassSun = 1.0;
		double ActivationCm = 0.0;
		double SpanCm = 0.0;                         // max a (1 + e): the farthest any planet gets
		std::vector<FPlanet> Planets;
	};

	namespace SystemDetail
	{
		inline double Roll(u32 Seed, u32 Channel, double Min, double Max, double Bias = 1.0)
		{
			return double(UniHlsl::Uni_RollRange(Seed, Channel, float(Min), float(Max), float(Bias), 0.0f));
		}
	}

	/** The system of a star, from its record. Pure and deterministic. */
	inline FSystem BuildSystem(const FObjectGPU& Star)
	{
		using namespace SystemDetail;
		namespace HS = UniHlsl;
		FSystem S;
		const HS::FUniObject HO = ToHlsl(Star);
		S.StarSeed = Star.B[0];
		S.StarAbsMag = HS::Uni_ObjectAbsMag(HO);
		S.StarRadiusCm = std::exp2(double(HS::Uni_ObjectRadiusLog2(HO)));
		const HS::float3 C = HS::Uni_UnpackColour(Star.B[3]);
		S.StarRgb[0] = C.x; S.StarRgb[1] = C.y; S.StarRgb[2] = C.z;
		S.LumSun = std::pow(10.0, -0.4 * (double(S.StarAbsMag) - double(HS::UNI_SUN_ABS_MAG_V)));
		S.MassSun = std::clamp(std::pow(S.LumSun, 1.0 / 3.5), 0.08, 60.0);
		S.ActivationCm = double(HS::Uni_SystemActivationRadius(S.StarAbsMag));
		const double SqrtL = std::sqrt(S.LumSun);

		const u32 SysSeed = HS::Uni_SeedMix(S.StarSeed, HS::UNI_CH_SYSTEM);
		const double Mean = S.MassSun < 0.5 ? 2.5 : (S.MassSun < 1.6 ? 4.0 : (S.MassSun < 8.0 ? 3.0 : 1.5));
		const u32 N = std::min<u32>(HS::Uni_PoissonCount(float(Mean), SysSeed), SysConst::MaxPlanets);
		const double GM = SysConst::GMSun * S.MassSun;
		// Orbits: N draws log-uniform over [inner, outer] x sqrt(L), sorted, then pushed apart to
		// a period-ratio-like minimum spacing of 1.3 in a. Each planet keeps the seed of its draw.
		const double Inner = SysConst::InnerAu * Units::Au * SqrtL * Roll(SysSeed, ChannelId("SYSTEM_INNER"), 0.7, 1.6);
		// Inside the activation radius with room for eccentricity and the 1.25x hysteresis: for the
		// brightest stars the radius is capped (1e4 AU) below 60 AU x sqrt(L).
		const double Outer = std::min(SysConst::OuterLimitAu * Units::Au * SqrtL, 0.6 * S.ActivationCm);
		std::vector<std::pair<double, u32>> Draws;
		for (u32 K = 0; K < N; ++K)
		{
			const u32 Ps = HS::Uni_SeedMix(SysSeed, K);
			Draws.push_back({ Inner * std::pow(Outer / Inner, Roll(Ps, ChannelId("PLANET_ORBIT"), 0.0, 1.0)), Ps });
		}
		std::sort(Draws.begin(), Draws.end());
		for (size_t K = 1; K < Draws.size(); ++K) { Draws[K].first = std::max(Draws[K].first, 1.3 * Draws[K - 1].first); }
		const double Snow = SysConst::SnowLineAu * Units::Au * SqrtL;
		u32 Giants = 0;
		for (size_t K = 0; K < Draws.size(); ++K)
		{
			const double A = Draws[K].first;
			const u32 Ps = Draws[K].second;
			if (A > Outer) { break; }
			FPlanet P;
			P.Seed = Ps;
			P.Index = u32(K);
			P.SemiMajorCm = A;
			if (A < Snow) { P.Type = EPlanetType::Rocky; }
			else { P.Type = Giants < 2 ? EPlanetType::GasGiant : EPlanetType::IceGiant; ++Giants; }
			switch (P.Type)
			{
			case EPlanetType::Rocky:
				P.RadiusCm = SysConst::EarthRadius * Roll(Ps, ChannelId("PLANET_RADIUS"), 0.4, 1.8, 1.5);
				P.Albedo = Roll(Ps, ChannelId("PLANET_ALBEDO"), 0.1, 0.45);
				P.Tint[0] = 0.78f; P.Tint[1] = 0.66f; P.Tint[2] = 0.54f;
				break;
			case EPlanetType::GasGiant:
				P.RadiusCm = SysConst::EarthRadius * Roll(Ps, ChannelId("PLANET_RADIUS"), 6.0, 12.0);
				P.Albedo = Roll(Ps, ChannelId("PLANET_ALBEDO"), 0.35, 0.55);
				P.Tint[0] = 0.92f; P.Tint[1] = 0.80f; P.Tint[2] = 0.62f;
				break;
			case EPlanetType::IceGiant:
				P.RadiusCm = SysConst::EarthRadius * Roll(Ps, ChannelId("PLANET_RADIUS"), 3.0, 4.5);
				P.Albedo = Roll(Ps, ChannelId("PLANET_ALBEDO"), 0.4, 0.55);
				P.Tint[0] = 0.62f; P.Tint[1] = 0.82f; P.Tint[2] = 0.95f;
				break;
			}
			P.Ecc = Roll(Ps, ChannelId("PLANET_ECC"), 0.0, 0.25, 2.5);
			P.Inc = Roll(Ps, ChannelId("PLANET_INC"), 0.0, 0.07, 2.0);
			P.Node = Roll(Ps, ChannelId("PLANET_NODE"), 0.0, SysConst::TwoPi);
			P.Peri = Roll(Ps, ChannelId("PLANET_PERI"), 0.0, SysConst::TwoPi);
			P.MeanAnomaly0 = Roll(Ps, ChannelId("PLANET_PHASE"), 0.0, SysConst::TwoPi);
			P.PeriodS = SysConst::TwoPi * std::sqrt(A * A * A / GM);
			S.SpanCm = std::max(S.SpanCm, A * (1.0 + P.Ecc));
			S.Planets.push_back(P);
		}
		return S;
	}

	/** Eccentric anomaly E from mean anomaly M (radians) and e in [0, 1): Newton from a
	 *  standard start, to 1e-15 or 50 iterations. */
	inline double KeplerSolve(double M, double E0)
	{
		M = std::remainder(M, SysConst::TwoPi);
		double E = E0 < 0.8 ? M + E0 * std::sin(M) : (M < 0.0 ? -SysConst::Pi : SysConst::Pi);
		for (int I = 0; I < 50; ++I)
		{
			const double F = E - E0 * std::sin(E) - M;
			const double D = F / (1.0 - E0 * std::cos(E));
			E -= D;
			if (std::abs(D) < 1.0e-15) { break; }
		}
		return E;
	}

	/** Mean anomaly at time t: periodic by construction (t is reduced modulo the period first). */
	inline double MeanAnomalyAt(const FPlanet& P, double TimeS)
	{
		const double Ph = std::fmod(TimeS, P.PeriodS);
		return P.MeanAnomaly0 + SysConst::TwoPi * (Ph < 0.0 ? Ph + P.PeriodS : Ph) / P.PeriodS;
	}

	/** Planet position relative to its star, cm, at time t (orbit on rails). */
	inline FVec3d PlanetOffset(const FPlanet& P, double TimeS)
	{
		const double E = KeplerSolve(MeanAnomalyAt(P, TimeS), P.Ecc);
		const double X = P.SemiMajorCm * (std::cos(E) - P.Ecc);
		const double Y = P.SemiMajorCm * std::sqrt(1.0 - P.Ecc * P.Ecc) * std::sin(E);
		const double Co = std::cos(P.Node), So = std::sin(P.Node);
		const double Cw = std::cos(P.Peri), Sw = std::sin(P.Peri);
		const double Ci = std::cos(P.Inc), Si = std::sin(P.Inc);
		return FVec3d{
			(Co * Cw - So * Sw * Ci) * X + (-Co * Sw - So * Cw * Ci) * Y,
			(So * Cw + Co * Sw * Ci) * X + (-So * Sw + Co * Cw * Ci) * Y,
			(Sw * Si) * X + (Cw * Si) * Y };
	}

	/** Lambert-sphere phase function: 1 at full phase, 0 at new. */
	inline double LambertPhase(double Alpha)
	{
		Alpha = std::clamp(Alpha, 0.0, SysConst::Pi);
		return (std::sin(Alpha) + (SysConst::Pi - Alpha) * std::cos(Alpha)) / SysConst::Pi;
	}

	struct FPlanetPhotometry { double R = 0.0, Delta = 0.0, Alpha = 0.0, Phase = 0.0, AppMag = 0.0, EffAbsMag = 0.0; };

	/** Reflected-light photometry. StarToPlanet = planet - star; PlanetToCamera = camera - planet. */
	inline FPlanetPhotometry PlanetPhotometry(const FPlanet& P, double StarAbsMag, const FVec3d& StarToPlanet, const FVec3d& PlanetToCamera)
	{
		FPlanetPhotometry Ph;
		Ph.R = Length(StarToPlanet);
		Ph.Delta = std::max(Length(PlanetToCamera), 1.0);
		const double Dot = -(StarToPlanet.X * PlanetToCamera.X + StarToPlanet.Y * PlanetToCamera.Y + StarToPlanet.Z * PlanetToCamera.Z);
		Ph.Alpha = std::acos(std::clamp(Dot / (std::max(Ph.R, 1.0) * Ph.Delta), -1.0, 1.0));
		Ph.Phase = LambertPhase(Ph.Alpha);
		const double Ratio = P.Albedo * Ph.Phase * (P.RadiusCm / std::max(Ph.R, 1.0)) * (P.RadiusCm / std::max(Ph.R, 1.0));
		Ph.EffAbsMag = Ratio > 0.0 ? StarAbsMag - 2.5 * std::log10(Ratio) : 1.0e9;
		Ph.AppMag = Ph.EffAbsMag + 5.0 * std::log10(Ph.Delta / SysConst::TenPc);
		return Ph;
	}

	struct FActiveSystem
	{
		FCoord Star;
		FSystem Sys;
		u64 ActivatedFrame = 0;
		FObjectId StarId;           // D-193: the star's identity, when the report named it
		bool bStarIdKnown = false;
		u32 StarArchetype = 0;      // D-193: its archetype id (from the record)
	};

	/** D-193: which planet a dynamic cell of Build's output is (for the API's events and
	 *  lookups: a planet is named by its star and its ordinal). */
	struct FPlanetCellRef
	{
		u32 StarSeed = 0;
		FObjectId StarId;
		bool bStarIdKnown = false;
		int Planet = 0;
		u32 PlanetSeed = 0;
		FCellKey Cell;
	};

	/** D-143 amendment: each planet is drawn from its own small cell, rebuilt every frame, so its
	 *  record's offset quantum is 2^(UNI_PLANET_CELL_LEVEL - 32) = 4 cm wherever it is. (A shared
	 *  per-system cell quantised to ~21 km, measured as a 7e-6 rad disagreement with the exact
	 *  actor at the hand-off, T2.parallax.promotion.) */
	constexpr int UNI_PLANET_CELL_LEVEL = 34;

	/** Active systems: activation from reports (GPU readback, or the CPU reference), exact
	 *  deactivation with hysteresis, and the per-frame dynamic cells and planet records. */
	class FSystemManager
	{
	public:
		size_t MaxActive = 16;
		size_t Activations = 0, Deactivations = 0, Rejected = 0;

		/** A star inside its activation radius, as the project pass saw it. Duplicates are
		 *  ignored (keyed by the star's seed); reports already outside the hysteresis radius
		 *  (a stale readback) are rejected. */
		void Report(const FCellKey& Cell, const FObjectGPU& Star, const FCoord& Camera, u64 Frame, const FObjectId* StarId = nullptr)
		{
			const u32 Seed = Star.B[0];
			if (Active.count(Seed)) { return; }
			if (Active.size() >= MaxActive) { ++Rejected; return; }
			const u32 Off[3] = { Star.A[0], Star.A[1], Star.A[2] };
			FActiveSystem A;
			A.Star = ObjectCoord(Cell, Off);
			A.Sys = BuildSystem(Star);
			const double D = Length(ToDouble(Sub(A.Star, Camera)));
			if (D > double(UniHlsl::UNI_SYSTEM_HYSTERESIS) * A.Sys.ActivationCm) { ++Rejected; return; }
			A.ActivatedFrame = Frame;
			if (StarId) { A.StarId = *StarId; A.bStarIdKnown = true; }
			A.StarArchetype = Star.A[3] & 0xFFFFu;
			Active.emplace(Seed, A);
			++Activations;
		}

		/** Deactivate systems the camera has left (beyond 1.25 x the activation radius). */
		void Update(const FCoord& Camera)
		{
			for (auto It = Active.begin(); It != Active.end();)
			{
				const double D = Length(ToDouble(Sub(It->second.Star, Camera)));
				if (D > double(UniHlsl::UNI_SYSTEM_HYSTERESIS) * It->second.Sys.ActivationCm) { It = Active.erase(It); ++Deactivations; }
				else { ++It; }
			}
		}

		/** One dynamic cell and one record per planet of every active system, for the project
		 *  pass. Records start at pool slot PoolBase; at most Capacity are written. Planets in
		 *  Promoted (D-147) are flagged UNI_OBJ_FLAG_PROMOTED: their actor draws them. */
		void Build(const FCoord& Camera, double TimeS, u32 PoolBase, u32 Capacity, std::vector<FCellGPU>& OutCells, std::vector<FObjectGPU>& OutRecords,
			const std::set<u32>* Promoted = nullptr, std::vector<FPlanetCellRef>* OutRefs = nullptr) const
		{
			for (const auto& KV : Active)
			{
				const FActiveSystem& A = KV.second;
				if (OutRecords.size() + A.Sys.Planets.size() > Capacity) { break; }
				int Ordinal = 0;
				for (const FPlanet& P : A.Sys.Planets)
				{
					FCoord Pc = A.Star;
					FVec3f Frac{};
					AddDisplacement(Pc, Frac, PlanetOffset(P, TimeS));
					const FCellKey K = CellOf(Pc, UNI_PLANET_CELL_LEVEL);
					FCellGPU G{};
					const FDelta D = Sub(CellOrigin(K), Camera);
					for (int Ax = 0; Ax < 3; ++Ax)
					{
						const FU128 T = D.TwosComplement(Ax);
						uint32_t* Dst = Ax == 0 ? G.DeltaX : (Ax == 1 ? G.DeltaY : G.DeltaZ);
						for (int N = 0; N < 4; ++N) { Dst[N] = T.L[N]; }
					}
					G.Level = u32(UNI_PLANET_CELL_LEVEL);
					G.PoolStart = PoolBase + u32(OutRecords.size());
					G.PoolCount = 1u;
					G.Flags = (255u << 8) | (255u << 16);   // fully faded in (planets fade by magnitude, like everything else) and settled (D-187: no arrival)
					OutRecords.push_back(PlanetRecord(A, P, Pc, K, Camera, TimeS));
					if (Promoted && Promoted->count(P.Seed)) { OutRecords.back().A[3] |= (UniHlsl::UNI_OBJ_FLAG_PROMOTED << 16); }
					OutCells.push_back(G);
					if (OutRefs) { OutRefs->push_back(FPlanetCellRef{ A.Sys.StarSeed, A.StarId, A.bStarIdKnown, Ordinal, P.Seed, K }); }
					++Ordinal;
				}
			}
		}

		/** One planet's record: exact offset in its own cell, effective absolute magnitude. */
		static FObjectGPU PlanetRecord(const FActiveSystem& A, const FPlanet& P, const FCoord& Pc, const FCellKey& K, const FCoord& Camera, double TimeS)
		{
			namespace HS = UniHlsl;
			const FVec3d Off = PlanetOffset(P, TimeS);
			const FCoord O = CellOrigin(K);
			u32 Qa[3] = { 0, 0, 0 };
			for (int Ax = 0; Ax < 3; ++Ax)
			{
				// The planet is inside its own cell by construction: the in-cell part is < 2^L.
				const FU128 M = Shr(UniCore::Sub(Pc.Axis(Ax), O.Axis(Ax)), UNI_PLANET_CELL_LEVEL - 32);
				Qa[Ax] = (M.L[0] | M.L[1] | M.L[2]) != 0u ? 0xFFFFFFFFu : M.L[3];
			}
			const HS::uint3 Q(Qa[0], Qa[1], Qa[2]);
			const FVec3d ToCam = ToDouble(Sub(Camera, Pc));
			const FPlanetPhotometry Ph = PlanetPhotometry(P, double(A.Sys.StarAbsMag), Off, ToCam);
			float Rgb[3] = { A.Sys.StarRgb[0] * P.Tint[0], A.Sys.StarRgb[1] * P.Tint[1], A.Sys.StarRgb[2] * P.Tint[2] };
			const float Mx = std::max({ Rgb[0], Rgb[1], Rgb[2], 1.0e-6f });
			for (float& V : Rgb) { V /= Mx; }
			const float RL2 = float(std::log2(P.RadiusCm));
			u32 Flags = HS::UNI_OBJ_FLAG_EXTENDED;
			float M = float(Ph.EffAbsMag);
			if (!(M < SysConst::MaxEffMag)) { M = SysConst::MaxEffMag; Flags |= HS::UNI_OBJ_FLAG_DELTA_HIDDEN; }
			const float SB = HS::Uni_SurfaceBrightness(M, RL2);
			return FromHlsl(HS::Uni_MakeObject(Q, HS::UNI_ARCH_DYNAMIC_PLANET, Flags, P.Seed, RL2, M, SB, HS::float3(Rgb[0], Rgb[1], Rgb[2])));
		}

		const std::map<u32, FActiveSystem>& ActiveSystems() const { return Active; }
		size_t PlanetCount() const { size_t N = 0; for (const auto& KV : Active) { N += KV.second.Sys.Planets.size(); } return N; }
		void Clear() { Active.clear(); }

	private:
		std::map<u32, FActiveSystem> Active;
	};
}
