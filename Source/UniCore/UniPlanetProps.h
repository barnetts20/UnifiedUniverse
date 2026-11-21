#pragma once

// UniPlanetProps.h
// D-293 (SHIPS_PLANETS.md §4): what a planet of a star system is, as a world to stand on: the seed
// its surface grows from, whether it has air (and what kind), whether it has seas, how strongly
// it pulls, and the radius within which the world is its frame. Everything follows from the
// planet's own seed and its system, so the same planet is the same world every time (one source).
// Engine-agnostic; the game module hands these to the planet actor (VoxelPlanets' APlanetActor).

#include "UniSystem.h"

#include <algorithm>
#include <cmath>

namespace UniCore
{
	/** The atmosphere a planet has: none, air alone (no clouds or weather), a terrestrial slab
	 *  (clouds and weather), or a gas giant's (the planet actor's EPlanetAtmosphereType, with None). */
	enum class EPlanetAtmo : u32 { None = 0, AirOnly = 1, Terrestrial = 2, GasGiant = 3 };

	namespace PlanetPropsConst
	{
		constexpr double G = 6.67430e-8;                // cm^3 g^-1 s^-2
		constexpr double EarthG = 980.665;              // cm/s^2
		constexpr double RockyDensity = 5.5;            // g/cm^3 (an Earth-like rock and iron mix)
		constexpr double GasGiantDensity = 1.3;         // Jupiter's
		constexpr double IceGiantDensity = 1.6;         // Uranus's and Neptune's
		constexpr double AirMinEarthRadii = 0.5;        // smaller rocky planets keep no air
		constexpr double WeatherTeqMinK = 200.0;        // a terrestrial slab (clouds, weather) between these equilibrium temperatures ..
		constexpr double WeatherTeqMaxK = 350.0;        // .. else air alone
		constexpr double OceanTeqMinK = 235.0;          // seas between these (a terrestrial slab's greenhouse puts the surface ~30 K above) ..
		constexpr double OceanTeqMaxK = 320.0;          // .. else frozen or dry
		constexpr double TeqEarthK = 278.6;             // a black body at 1 AU from the Sun: T = 278.6 K L^(1/4) a_AU^(-1/2) (1 - A)^(1/4)
	}

	/** Play's limits on a surface's pull, in g (physics' own range is wider; tunable). */
	struct FPlanetGravityLimits
	{
		double MinG = 0.3;
		double MaxG = 2.0;
	};

	struct FPlanetProps
	{
		u32 SurfaceSeed = 0;            // the surface generator's seed (new channel PLANET_SURFACE)
		EPlanetAtmo Atmosphere = EPlanetAtmo::None;
		bool bOcean = false;
		double TeqK = 0.0;              // equilibrium temperature (no greenhouse)
		double InsolationEarth = 0.0;   // the star's light there over Earth's
		double MassG = 0.0;             // grams, from radius and the type's density
		double SurfaceGravity = 0.0;    // cm/s^2, physical, then within the play limits
		double FrameRadiusCm = 0.0;     // the planet's frame holds inside this (SHIPS_PLANETS.md §5)
	};

	/** A type's bulk density, g/cm^3. */
	inline double PlanetDensity(EPlanetType T)
	{
		using namespace PlanetPropsConst;
		return T == EPlanetType::Rocky ? RockyDensity : (T == EPlanetType::GasGiant ? GasGiantDensity : IceGiantDensity);
	}

	/** Equilibrium temperature (K) of a planet at semi-major axis A (cm) about a star of
	 *  luminosity L (solar), with albedo Alb. */
	inline double PlanetEquilibriumK(double LumSun, double SemiMajorCm, double Albedo)
	{
		const double Au = SemiMajorCm / Units::Au;
		return PlanetPropsConst::TeqEarthK * std::pow(std::max(LumSun, 0.0), 0.25) / std::sqrt(std::max(Au, 1.0e-6)) * std::pow(std::clamp(1.0 - Albedo, 0.0, 1.0), 0.25);
	}

	/** The world a planet is (D-293). FrameRadii: the frame's radius in planet radii (default 3). */
	inline FPlanetProps PlanetPropsOf(const FSystem& S, const FPlanet& P, double FrameRadii = 3.0, const FPlanetGravityLimits& Limits = FPlanetGravityLimits())
	{
		using namespace PlanetPropsConst;
		FPlanetProps Out;
		Out.SurfaceSeed = UniHlsl::Uni_SeedMix(P.Seed, ChannelId("PLANET_SURFACE"));
		const double Au = P.SemiMajorCm / Units::Au;
		Out.InsolationEarth = S.LumSun / std::max(Au * Au, 1.0e-12);
		Out.TeqK = PlanetEquilibriumK(S.LumSun, P.SemiMajorCm, P.Albedo);
		const double R = P.RadiusCm;
		Out.MassG = 4.0 / 3.0 * SysConst::Pi * R * R * R * PlanetDensity(P.Type);
		const double Gphys = G * Out.MassG / std::max(R * R, 1.0);
		Out.SurfaceGravity = std::clamp(Gphys, Limits.MinG * EarthG, std::max(Limits.MinG, Limits.MaxG) * EarthG);
		Out.FrameRadiusCm = std::max(FrameRadii, 1.05) * R;
		switch (P.Type)
		{
		case EPlanetType::GasGiant:
			Out.Atmosphere = EPlanetAtmo::GasGiant;
			break;
		case EPlanetType::IceGiant:
			Out.Atmosphere = EPlanetAtmo::GasGiant;   // the nearest model the planet actor has (ice giants keep the placeholder: D-290)
			break;
		case EPlanetType::Rocky:
			if (R < AirMinEarthRadii * SysConst::EarthRadius) { Out.Atmosphere = EPlanetAtmo::None; }
			else if (Out.TeqK >= WeatherTeqMinK && Out.TeqK <= WeatherTeqMaxK) { Out.Atmosphere = EPlanetAtmo::Terrestrial; }
			else { Out.Atmosphere = EPlanetAtmo::AirOnly; }
			Out.bOcean = Out.Atmosphere == EPlanetAtmo::Terrestrial && Out.TeqK >= OceanTeqMinK && Out.TeqK <= OceanTeqMaxK;
			break;
		}
		return Out;
	}

	/** A planet's centre at universe time t: exact whole centimetres plus the sub-centimetre part
	 *  (the frame's origin on it, D-291: nothing about it is rounded away). */
	inline void PlanetCentreAt(const FCoord& Star, const FPlanet& P, double TimeS, FCoord& OutCentre, FVec3f& OutFraction)
	{
		OutCentre = Star;
		OutFraction = FVec3f{};
		AddDisplacement(OutCentre, OutFraction, PlanetOffset(P, TimeS));
	}

	/** The world shift when the world's origin moves from (OldA, OldF) to (NewA, NewF): every
	 *  world position p becomes p + Shift and stays the same universal point. */
	inline FVec3d FrameShift(const FCoord& OldA, const FVec3f& OldF, const FCoord& NewA, const FVec3f& NewF)
	{
		FVec3d S = ToDouble(Sub(OldA, NewA));
		S.X += double(OldF.X) - double(NewF.X);
		S.Y += double(OldF.Y) - double(NewF.Y);
		S.Z += double(OldF.Z) - double(NewF.Z);
		return S;
	}
}
