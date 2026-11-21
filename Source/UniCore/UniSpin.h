// UniSpin.h
// D-299 (SHIPS_PLANETS.md §6, first cut): planets turn about the universe's +Z axis, and the world
// turns with the planet whose frame it is in, so that planet stands still in the world while its sun
// and sky go round.
//
// THE WORLD'S TURN. The UE world and the universe share their axes until a planet's frame is
// entered; from then on they differ by a turn about +Z, the world's yaw:
//     universal displacement = RotateYaw(world displacement, Yaw).
// In a planet's frame the yaw follows the planet's spin (Yaw = spin + a constant fixed on entry), so
// the planet's turn in the world (spin - Yaw) is constant: the ground, the pawn on it and every
// physics body stay put. Out of a frame the yaw holds where it was: nothing in the world turns when
// the frame is left or entered, and the sky does not jump. Every mapping between the world and the
// universe (the camera, placement, the light's direction, the renderer's view basis) goes through it.
//
// THE DAY. A planet's spin period is set from the day wanted at its surface: the sun returns once a
// solar day, so the planet turns once in 1 / (1 / day + 1 / orbit): the sun's longitude about +Z
// goes once round a year at any inclination below 90 degrees (the orbits are prograde about +Z;
// a retrograde one takes the year off). The axis is +Z for every
// planet for now: the obliquity is the orbit's inclination (0-4 degrees), so the seasons are faint.
// Seeded periods, phases and tilts (§6) come later on their own seed channels; a tilt makes the
// world's turn a full rotation, not a yaw.
//
// Engine-agnostic (no UE includes). Checked by T1.frame.spin (tests/test_spin.cpp).

#pragma once

#include "UniCoord.h"
#include "UniSystem.h"

#include <cmath>

namespace UniCore
{
	namespace SpinConst
	{
		constexpr double TwoPi = 6.283185307179586476925286766559;
		constexpr double EarthDayS = 86400.0;
	}

	/** An angle into [0, 2 pi). */
	inline double WrapAngle(double A)
	{
		A = std::fmod(A, SpinConst::TwoPi);
		return A < 0.0 ? A + SpinConst::TwoPi : A;
	}

	/** A vector turned about +Z by A radians (X towards Y: UE's yaw). */
	inline FVec3d RotateYaw(const FVec3d& V, double A)
	{
		const double C = std::cos(A), S = std::sin(A);
		return FVec3d{ V.X * C - V.Y * S, V.X * S + V.Y * C, V.Z };
	}

	/** The turn period about +Z that gives a mean solar day of SolarDayS on an orbit of
	 *  OrbitPeriodS, prograde about +Z or not (the sun's longitude goes once round the year, either
	 *  way). The solar day itself where the orbit is unknown, or where a retrograde orbit outruns
	 *  the day. */
	inline double SiderealPeriodS(double SolarDayS, double OrbitPeriodS, bool bPrograde)
	{
		if (SolarDayS <= 0.0) { return 0.0; }
		const double Rate = 1.0 / SolarDayS + (OrbitPeriodS > 0.0 ? (bPrograde ? 1.0 : -1.0) / OrbitPeriodS : 0.0);
		return Rate > 0.0 ? 1.0 / Rate : SolarDayS;
	}

	/** A planet's turn period for a mean solar day of SolarDayS. */
	inline double PlanetSpinPeriodS(const FPlanet& P, double SolarDayS)
	{
		return SiderealPeriodS(SolarDayS, P.PeriodS, std::cos(P.Inc) >= 0.0);
	}

	/** A planet's turn about +Z at time TimeS (s; UniverseTime): Phase0 at t = 0, one turn a
	 *  PeriodS. The period's whole turns are taken out in double first (fmod is exact), so the angle
	 *  keeps full precision at any time. A period of 0 or less: no spin. */
	inline double SpinAngle(double TimeS, double PeriodS, double Phase0 = 0.0)
	{
		if (!(PeriodS > 0.0)) { return WrapAngle(Phase0); }
		const double InTurn = std::fmod(TimeS, PeriodS);
		return WrapAngle(Phase0 + SpinConst::TwoPi * (InTurn / PeriodS));
	}

	/** The world's turn against the universe's axes (see the header). */
	struct FWorldTurn
	{
		double Yaw = 0.0;          // universal = RotateYaw(world, Yaw)
		bool bFollowing = false;   // in a planet's frame: Yaw follows its spin
		u32 Body = 0;              // the planet followed
		double Offset = 0.0;       // Yaw - its spin while following

		/** In the frame of planet BodyId, whose spin is now Spin: the yaw follows it. The first call
		 *  for a planet fixes the offset at the current yaw, so the yaw does not jump. */
		void Follow(u32 BodyId, double Spin)
		{
			if (!bFollowing || Body != BodyId)
			{
				Offset = WrapAngle(Yaw - Spin);
				bFollowing = true;
				Body = BodyId;
			}
			Yaw = WrapAngle(Spin + Offset);
		}

		/** Out of a frame: the yaw holds. */
		void Hold() { bFollowing = false; Body = 0; }

		/** Back to the universe's axes (a new start: nothing in the world to keep). */
		void Reset() { *this = FWorldTurn(); }

		FVec3d ToUniversal(const FVec3d& World) const { return Yaw == 0.0 ? World : RotateYaw(World, Yaw); }
		FVec3d ToWorld(const FVec3d& Universal) const { return Yaw == 0.0 ? Universal : RotateYaw(Universal, -Yaw); }

		/** Planet BodyId's turn in the world (its actor's yaw) at its spin Spin: exactly constant for
		 *  the planet followed (from the offset, not from two angles that round apart). */
		double BodyYaw(u32 BodyId, double Spin) const { return (bFollowing && BodyId == Body) ? WrapAngle(-Offset) : WrapAngle(Spin - Yaw); }
	};
}
