#pragma once

// UniScaleLadder.h
// Human-readable distances from exact cm values, for HUDs, logs and Blueprint readouts.
//
// The reference index names ULSScaleLadder.h as the source, but it is not in the GitHub
// snapshot of UltraLargeScale this session could read (cb2ccc5). This is a fresh
// implementation of the same idea; reconcile its rungs with ULSScaleLadder when porting for
// A/B parity (REVIEW_TONIGHT §6).

#include <cmath>
#include <cstdio>
#include <string>

namespace UniCore
{
	namespace Units
	{
		constexpr double Cm = 1.0;
		constexpr double M = 100.0;
		constexpr double Km = 1.0e5;
		constexpr double Au = 1.495978707e13;
		constexpr double Ly = 9.4607304725808e17;
		constexpr double Pc = 3.0856775814913673e18;
		constexpr double Kpc = 1.0e3 * Pc;
		constexpr double Mpc = 1.0e6 * Pc;
		constexpr double Gpc = 1.0e9 * Pc;
	}

	struct FScaleRung { double Cm; const char* Unit; };

	/** Rungs, smallest first. A distance uses the largest rung it is at least one of. */
	inline const FScaleRung* ScaleLadder(int& OutCount)
	{
		static const FScaleRung Rungs[] = {
			{ Units::Cm, "cm" }, { Units::M, "m" }, { Units::Km, "km" }, { Units::Au, "AU" },
			{ Units::Ly, "ly" }, { Units::Kpc, "kpc" }, { Units::Mpc, "Mpc" }, { Units::Gpc, "Gpc" },
		};
		OutCount = int(sizeof(Rungs) / sizeof(Rungs[0]));
		return Rungs;
	}

	inline std::string FormatDistance(double Cm)
	{
		if (!std::isfinite(Cm)) { return "inf"; }
		const double A = std::fabs(Cm);
		int N;
		const FScaleRung* R = ScaleLadder(N);
		int Pick = 0;
		for (int I = 0; I < N; ++I) { if (A >= R[I].Cm) { Pick = I; } }
		char Buf[64];
		const double V = Cm / R[Pick].Cm;
		if (std::fabs(V) >= 1.0e4) { std::snprintf(Buf, sizeof(Buf), "%.3e %s", V, R[Pick].Unit); }
		else { std::snprintf(Buf, sizeof(Buf), "%.3g %s", V, R[Pick].Unit); }
		return Buf;
	}
}
