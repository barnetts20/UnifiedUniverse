// test_home.cpp
// D-293 (SHIPS_PLANETS.md; D-290, D-291, D-292): the home system, planets as worlds, the planet's
// frame, and the spawn point (UniHome.h, UniPlanetProps.h, UniGenRef.h's AnchorRecord).
//   system      the pinned seed (35) is the first that qualifies from 1 (FindHomeStarSeed); its
//               record from AnchorRecord is the generator's own (the anchor's cell, bit for bit);
//               the system it rolls has its home planet (0.8-1.0 Earth radii, 0.7-1.4 of Earth's
//               light, e < 0.1, weather and seas, no black hole), the same every build.
//   properties  over 20,000 systems: deterministic; rocky planets under 0.5 Earth radii airless,
//               the rest air alone or a terrestrial slab by their equilibrium temperature, seas only
//               under a slab within the band; giants the gas giant model; gravity g = 4/3 pi G rho R
//               within the play limits; Earth at 1 AU with albedo 0.3 at 255 K (1 K); the frame's
//               radius 3 R; surface seeds differ from the planet's own.
//   moving_anchor  over one orbit of the home planet at 1,000 times: with the world's origin at
//               the planet's exact centre, the camera (origin + view) is the planet's centre plus
//               the view to 1e-3 cm; the planet's placement (ParallaxPlace + view) within 1 cm of the
//               origin; a frame change (FrameShift) leaves every universal point where it was
//               (1e-3 cm) for shifts up to 1e9 cm, and its inverse returns to the start.
//   spawn       the candidates deterministic, on the day side (the star >= 20 degrees up) and within
//               40 degrees of the equator; the picker on a stand-in ground (seas, slopes, heights):
//               the first passing candidate chosen, the fallbacks in order.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "UniHome.h"
#include "UniPlanetProps.h"
#include "UniParallax.h"
#include "UniLocalFrame.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	const FUniverseModel& HomeModel()
	{
		static const FUniverseModel M(UniScenes::Home());
		return M;
	}

	bool SameRecord(const FObjectGPU& A, const FObjectGPU& B)
	{
		for (int I = 0; I < 4; ++I) { if (A.A[I] != B.A[I] || A.B[I] != B.B[I]) { return false; } }
		return true;
	}
}

UNI_TEST(home, system)
{
	std::string Txt;
	bool bOk = true;
	const FUniverseModel& M = HomeModel();
	const u32 Found = UniScenes::FindHomeStarSeed(M, 1u, 5000u);
	bOk = bOk && Found == UniScenes::HomeConst::HomeStarSeed;
	Txt += Uni::Fmt("first qualifying seed %u (pinned %u)", Found, UniScenes::HomeConst::HomeStarSeed);

	// The record AnchorRecord makes is the one the generator puts in the anchor's cell.
	const FAnchor A = UniScenes::HomeStarAnchor(UniScenes::HomeConst::HomeStarSeed);
	FCellKey K;
	const FObjectGPU R = UniScenes::HomeStarRecord(M, A, K);
	FRefGenerator Gen(M);
	FCellContent C;
	Gen.GenerateCell(K, {}, C);   // no contributors: the anchors alone
	bool bInCell = false;
	for (size_t I = 0; I < C.Objects.size(); ++I)
	{
		if (C.Indices[I] >= kFirstAnchorIndex && SameRecord(C.Objects[I], R)) { bInCell = true; }
	}
	bOk = bOk && bInCell;
	Txt += Uni::Fmt("; record in the generated cell (L%d) %s", K.Level, bInCell ? "identical" : "MISSING");

	const FSystem S1 = BuildSystem(R, FStellarBhConfig{});
	const FSystem S2 = BuildSystem(R, FStellarBhConfig{});
	const int P = UniScenes::HomePlanetOf(S1);
	bool bSame = S1.Planets.size() == S2.Planets.size();
	for (size_t I = 0; bSame && I < S1.Planets.size(); ++I) { bSame = S1.Planets[I].Seed == S2.Planets[I].Seed && S1.Planets[I].RadiusCm == S2.Planets[I].RadiusCm; }
	bOk = bOk && P >= 0 && bSame && !S1.Bh.bValid;
	if (P >= 0)
	{
		const FPlanet& Pl = S1.Planets[size_t(P)];
		const FPlanetProps Pr = PlanetPropsOf(S1, Pl);
		const double Re = Pl.RadiusCm / SysConst::EarthRadius;
		const bool bQual = Re >= 0.8 && Re <= 1.0 && Pr.InsolationEarth >= 0.7 && Pr.InsolationEarth <= 1.4 && Pl.Ecc < 0.1
			&& Pr.Atmosphere == EPlanetAtmo::Terrestrial && Pr.bOcean;
		bOk = bOk && bQual;
		Txt += Uni::Fmt("; %zu planets, home #%d: %.3f Earth radii (%.0f km), %.3f AU, e %.3f, %.2f x Earth's light, Teq %.0f K, %.2f g, a %.0f-day year",
			S1.Planets.size(), P, Re, Pl.RadiusCm * 1.0e-5, Pl.SemiMajorCm / Units::Au, Pl.Ecc, Pr.InsolationEarth, Pr.TeqK, Pr.SurfaceGravity / PlanetPropsConst::EarthG, Pl.PeriodS / 86400.0);
	}
	Txt += bSame ? "; the same system every build" : "; DIFFERENT SYSTEMS";
	Uni::Check(1, "T1.home.system", "D-293 the home system: the pinned seed the first qualifying from 1; its record from AnchorRecord the generator's own in its cell; its home planet 0.8-1.0 Earth radii, 0.7-1.4 of Earth's light, e < 0.1, weather and seas, no black hole; the same every build")
		.Pass(bOk, Txt);
}

UNI_TEST(planet, properties)
{
	std::string Txt;
	bool bOk = true;
	size_t Planets = 0, Airless = 0, Air = 0, Weather = 0, Seas = 0, Giants = 0, Bad = 0, Nondet = 0, GBad = 0, SeedSame = 0;
	const FPlanetGravityLimits Lim;
	for (u32 I = 0; I < 20000u; ++I)
	{
		const u32 Seed = H::Uni_SeedMix(0x5EEDu, I);
		const float AbsMag = float(-1.0 + 11.0 * double(H::Uni_DrawUnit(Seed, 7u, 0u)));
		const FObjectGPU R = FromHlsl(H::Uni_MakeObject(H::uint3(0u, 0u, 0u), UniScenes::kStar, 0u, Seed, float(std::log2(6.957e10)), AbsMag, 0.0f, H::float3(1.0f, 1.0f, 1.0f)));
		const FSystem S = BuildSystem(R, FStellarBhConfig{});
		for (const FPlanet& P : S.Planets)
		{
			++Planets;
			const FPlanetProps A = PlanetPropsOf(S, P);
			const FPlanetProps B = PlanetPropsOf(S, P);
			if (A.SurfaceSeed != B.SurfaceSeed || A.Atmosphere != B.Atmosphere || A.bOcean != B.bOcean || A.SurfaceGravity != B.SurfaceGravity) { ++Nondet; }
			if (A.SurfaceSeed == P.Seed) { ++SeedSame; }
			const double Gphys = PlanetPropsConst::G * (4.0 / 3.0 * SysConst::Pi * P.RadiusCm * P.RadiusCm * P.RadiusCm * PlanetDensity(P.Type)) / (P.RadiusCm * P.RadiusCm);
			const double Gexp = std::clamp(Gphys, Lim.MinG * PlanetPropsConst::EarthG, Lim.MaxG * PlanetPropsConst::EarthG);
			if (std::fabs(A.SurfaceGravity - Gexp) > 1.0e-9 * Gexp) { ++GBad; }
			if (std::fabs(A.FrameRadiusCm - 3.0 * P.RadiusCm) > 1.0e-6 * P.RadiusCm) { ++Bad; }
			if (P.Type != EPlanetType::Rocky)
			{
				++Giants;
				if (A.Atmosphere != EPlanetAtmo::GasGiant || A.bOcean) { ++Bad; }
				continue;
			}
			const bool bSmall = P.RadiusCm < PlanetPropsConst::AirMinEarthRadii * SysConst::EarthRadius;
			const bool bWeatherT = A.TeqK >= PlanetPropsConst::WeatherTeqMinK && A.TeqK <= PlanetPropsConst::WeatherTeqMaxK;
			const EPlanetAtmo Want = bSmall ? EPlanetAtmo::None : (bWeatherT ? EPlanetAtmo::Terrestrial : EPlanetAtmo::AirOnly);
			if (A.Atmosphere != Want) { ++Bad; }
			const bool bSeaT = A.TeqK >= PlanetPropsConst::OceanTeqMinK && A.TeqK <= PlanetPropsConst::OceanTeqMaxK;
			if (A.bOcean != (Want == EPlanetAtmo::Terrestrial && bSeaT)) { ++Bad; }
			Airless += A.Atmosphere == EPlanetAtmo::None;
			Air += A.Atmosphere == EPlanetAtmo::AirOnly;
			Weather += A.Atmosphere == EPlanetAtmo::Terrestrial;
			Seas += A.bOcean;
		}
	}
	const double TeqEarth = PlanetEquilibriumK(1.0, Units::Au, 0.3);
	// Earth's own: 0.0 of the planet table, 1 g at 1 Earth radius with rock's density (physical 0.9997 g).
	const double Gearth = PlanetPropsConst::G * (4.0 / 3.0 * SysConst::Pi * std::pow(SysConst::EarthRadius, 3.0) * PlanetPropsConst::RockyDensity) / (SysConst::EarthRadius * SysConst::EarthRadius) / PlanetPropsConst::EarthG;
	bOk = Nondet == 0 && Bad == 0 && GBad == 0 && SeedSame == 0 && std::fabs(TeqEarth - 255.0) < 1.0 && std::fabs(Gearth - 1.0) < 0.01 && Weather > 0 && Seas > 0 && Airless > 0 && Air > 0;
	Txt = Uni::Fmt("%zu planets (%zu giants): rocky airless %zu, air alone %zu, weather %zu (seas %zu); rule mismatches %zu, gravity %zu, nondeterministic %zu, surface seed = planet seed %zu; Earth at 1 AU, albedo 0.3: Teq %.1f K; rock at Earth's radius %.4f g",
		Planets, Giants, Airless, Air, Weather, Seas, Bad, GBad, Nondet, SeedSame, TeqEarth, Gearth);
	Uni::Check(1, "T1.planet.properties", "D-293 planets as worlds: deterministic; rocky under 0.5 Earth radii airless, else air alone or weather by equilibrium temperature (200-350 K), seas only with weather at 235-320 K; giants the gas giant model; gravity 4/3 pi G rho R within 0.3-2 g; Earth at 1 AU and albedo 0.3 255 K (1 K); the frame 3 radii; surface seeds their own")
		.Pass(bOk, Txt);
}

UNI_TEST(frame, moving_anchor)
{
	std::string Txt;
	bool bOk = true;
	const FUniverseModel& M = HomeModel();
	FCellKey K;
	const FObjectGPU R = UniScenes::HomeStarRecord(M, UniScenes::HomeStarAnchor(UniScenes::HomeConst::HomeStarSeed), K);
	const FSystem S = BuildSystem(R, FStellarBhConfig{});
	const int Pi = UniScenes::HomePlanetOf(S);
	const u32 Off[3] = { R.A[0], R.A[1], R.A[2] };
	const FCoord Star = ObjectCoord(K, Off);
	const FPlanet& P = S.Planets[size_t(std::max(Pi, 0))];
	double WorstCam = 0.0, WorstPlace = 0.0, WorstShift = 0.0, WorstBack = 0.0;
	const FVec3d View{ 3.1e8, -4.2e8, 2.9e8 };   // a point on and above a ~5,000 km planet's surface
	for (int I = 0; I < 1000; ++I)
	{
		const double T = P.PeriodS * double(I) / 1000.0 + 0.37;
		FCoord O; FVec3f Of;
		PlanetCentreAt(Star, P, T, O, Of);
		// The camera: the origin plus the view (the subsystem's LocalToUniversal with the fraction).
		FVec3f CamF{};
		const FCoord Cam = LocalToUniversal(O, FVec3d{ View.X + double(Of.X), View.Y + double(Of.Y), View.Z + double(Of.Z) }, CamF);
		// Exact: the planet's centre (star + its offset, in double) plus the view.
		const FVec3d Off2 = PlanetOffset(P, T);
		const FVec3d D = ToDouble(Sub(Cam, Star));
		const FVec3d E{ D.X + double(CamF.X) - (Off2.X + View.X), D.Y + double(CamF.Y) - (Off2.Y + View.Y), D.Z + double(CamF.Z) - (Off2.Z + View.Z) };
		WorstCam = std::max(WorstCam, Length(E));
		// The planet placed by parallax (its coordinate's whole centimetres): at the origin.
		const FParallaxPlacement Pl = ParallaxPlace(O, P.RadiusCm, Cam, CamF, true);
		const FVec3d World{ View.X + Pl.Offset.X, View.Y + Pl.Offset.Y, View.Z + Pl.Offset.Z };
		WorstPlace = std::max(WorstPlace, Length(World));
		// A frame change to an origin up to 1e9 cm away keeps every universal point.
		const double Sc = 1.0e9 * double(H::Uni_DrawUnit(u32(I) * 2654435761u, 3u, 0u));
		FCoord O2 = O; FVec3f F2 = Of;
		AddDisplacement(O2, F2, FVec3d{ Sc * 0.6, -Sc * 0.48, Sc * 0.64 + 0.3125 });
		const FVec3d Sh = FrameShift(O, Of, O2, F2);
		const FVec3d View2{ View.X + Sh.X, View.Y + Sh.Y, View.Z + Sh.Z };
		FVec3f CamF2{};
		const FCoord Cam2 = LocalToUniversal(O2, FVec3d{ View2.X + double(F2.X), View2.Y + double(F2.Y), View2.Z + double(F2.Z) }, CamF2);
		const FVec3d Dd = ToDouble(Sub(Cam2, Cam));
		WorstShift = std::max(WorstShift, Length(FVec3d{ Dd.X + double(CamF2.X) - double(CamF.X), Dd.Y + double(CamF2.Y) - double(CamF.Y), Dd.Z + double(CamF2.Z) - double(CamF.Z) }));
		const FVec3d Back = FrameShift(O2, F2, O, Of);
		WorstBack = std::max(WorstBack, Length(FVec3d{ View2.X + Back.X - View.X, View2.Y + Back.Y - View.Y, View2.Z + Back.Z - View.Z }));
	}
	// The placement reads the centre's whole centimetres (a body's coordinate), so it sits within
	// sqrt(3) cm of the exact origin; the voxel planet itself is pinned at the origin, not placed.
	bOk = Pi >= 0 && WorstCam < 1.0e-3 && WorstPlace < 1.7321 && WorstShift < 1.0e-3 && WorstBack < 1.0e-3;
	Txt = Uni::Fmt("one orbit (%.0f days) at 1,000 times: camera vs exact %.2e cm; the planet placed %.3f cm from the origin; a frame change %.2e cm, and back %.2e cm",
		P.PeriodS / 86400.0, WorstCam, WorstPlace, WorstShift, WorstBack);
	Uni::Check(1, "T1.frame.moving_anchor", "D-293 the planet's frame (D-291): the world's origin at the planet's exact centre over an orbit: camera = centre + view (1e-3 cm), the planet's placeholder placed within its coordinate's rounding of the origin (sqrt 3 cm); a frame change keeps every universal point (1e-3 cm) and its inverse returns")
		.Pass(bOk, Txt);
}

UNI_TEST(home, spawn)
{
	std::string Txt;
	bool bOk = true;
	const FVec3d ToStar{ 0.3, -0.9, 0.2 };
	const std::vector<FVec3d> A = UniScenes::HomeSpawnCandidates(12345u, ToStar);
	const std::vector<FVec3d> B = UniScenes::HomeSpawnCandidates(12345u, ToStar);
	bool bSame = A.size() == B.size();
	for (size_t I = 0; bSame && I < A.size(); ++I) { bSame = A[I].X == B[I].X && A[I].Y == B[I].Y && A[I].Z == B[I].Z; }
	const double Ls = Length(ToStar);
	size_t OutOfBand = 0, Night = 0;
	for (const FVec3d& D : A)
	{
		if (std::fabs(D.Z) > std::sin(40.0 * SysConst::Pi / 180.0) + 1e-12) { ++OutOfBand; }
		if ((D.X * ToStar.X + D.Y * ToStar.Y + D.Z * ToStar.Z) / Ls < std::sin(20.0 * SysConst::Pi / 180.0) - 1e-12) { ++Night; }
	}
	bOk = bOk && bSame && A.size() == 48u && OutOfBand == 0 && Night == 0;
	// A stand-in ground: sea where x is at or above Thresh (just under candidate 0's, so it is sea);
	// elsewhere 100-900 m in waves along y (gentle, k 1,000: at most ~8 m per 100 m; steep, k 10^5:
	// up to ~780 m).
	const double Rad = 5.142e8;
	const double Thresh = A.empty() ? 0.0 : A[0].X - 0.05;
	auto Ground = [&](const FVec3d& D, double SlopeScale)
	{
		UniScenes::FGroundSample G;
		G.bValid = true;
		G.bDry = D.X < Thresh;
		G.HeightM = 500.0 + 400.0 * std::sin(SlopeScale * D.Y);   // 100-900 m
		return G;
	};
	const int Gentle = UniScenes::PickHomeSpawn(A, Rad, [&](const FVec3d& D) { return Ground(D, 1000.0); });
	int FirstDry = -1;
	for (size_t I = 0; I < A.size(); ++I)
	{
		if (A[I].X < Thresh - 1.0e-4) { FirstDry = int(I); break; }   // its four neighbours (2e-5 rad away) dry too
	}
	// Steep everywhere: none passes; the levellest dry one, which on this ground is the dry candidate
	// with the smallest |dh| between its neighbours (the slope's tangent component, the same for all).
	const int SteepPick = UniScenes::PickHomeSpawn(A, Rad, [&](const FVec3d& D) { return Ground(D, 1.0e5); });
	const bool bSteepDry = SteepPick >= 0 && A[size_t(SteepPick)].X < Thresh;
	const int None = UniScenes::PickHomeSpawn(A, Rad, [&](const FVec3d&) { UniScenes::FGroundSample G; G.bValid = true; G.bDry = false; return G; });
	bOk = bOk && FirstDry > 0 && Gentle == FirstDry && bSteepDry && None == 0;
	Txt = Uni::Fmt("%zu candidates, the same twice: %s; outside +-40 degrees %zu, sun below 20 degrees %zu; candidate 0 at sea, gentle ground: candidate %d (first dry %d); steep ground: a dry fallback %d; all sea: candidate %d",
		A.size(), bSame ? "yes" : "NO", OutOfBand, Night, Gentle, FirstDry, SteepPick, None);
	Uni::Check(1, "T1.home.spawn", "D-293 the spawn point: candidates deterministic, on the day side (star >= 20 degrees up), within 40 degrees of the equator; the picker takes the first dry, level candidate, else the levellest dry, else the first")
		.Pass(bOk, Txt);
}
