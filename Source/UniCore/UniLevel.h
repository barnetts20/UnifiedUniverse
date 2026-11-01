#pragma once

// UniLevel.h
// Relevance radius and level assignment (design doc, Spec: scale levels and object model).
//
//   Relevance R = max(flux, angular, influence)
//     flux:      R = 10 pc * 10^((m_ref - M) / 5)          point sources
//     angular:   R = R_phys / theta_min                     extended sources
//     influence: archetype-defined (D-101)
//   Level:       L = ceil(log2(R / N_eff)), N_eff = N - 1/K = 1.75 (D-118; the design's N = 2
//                leaves no slack for the one frame a window lags the camera)
//
// Level assignment always uses m_ref and the BASELINE window, never live sensitivity, so a
// scan never changes which objects exist. Computed in double, then corrected with exact
// powers of two, so the coverage guarantee 2^L * N >= R holds even where log2 rounds.

#include "UniShaderCores.h"

#include <algorithm>
#include <cmath>

namespace UniCore
{
	constexpr double kTenPcCm = 3.0856775814913673e19;

	inline double RelevanceFlux(double AbsMag, double MRef = UniHlsl::UNI_M_REF)
	{
		return kTenPcCm * std::pow(10.0, (MRef - AbsMag) / 5.0);
	}

	inline double RelevanceAngular(double PhysRadiusCm, double ThetaMin = UniHlsl::UNI_THETA_MIN)
	{
		return PhysRadiusCm / ThetaMin;
	}

	/** Smallest L in [0, 128] with 2^L * N >= R. N defaults to N_eff (D-118). */
	inline int LevelForRelevance(double RelevanceCm, double N = double(UniHlsl::UNI_WINDOW_N_EFF))
	{
		if (!(RelevanceCm > 0.0)) { return 0; }
		const double Ratio = RelevanceCm / N;
		int L = int(std::ceil(std::log2(Ratio)));
		L = std::clamp(L, 0, 128);
		while (L < 128 && std::ldexp(N, L) < RelevanceCm) { ++L; }
		while (L > 0 && std::ldexp(N, L - 1) >= RelevanceCm) { --L; }
		return L;
	}

	/** Inverse of RelevanceFlux: the absolute magnitude whose relevance is exactly R. */
	inline double AbsMagForRelevance(double RelevanceCm, double MRef = UniHlsl::UNI_M_REF)
	{
		return MRef - 5.0 * std::log10(RelevanceCm / kTenPcCm);
	}

	/** Absolute-magnitude interval [Bright, Faint) whose flux relevance maps to level L.
	 *  Brighter (smaller M) means larger R and coarser L. Level L holds R in
	 *  (N 2^(L-1), N 2^L], i.e. M in [M(N 2^L), M(N 2^(L-1))). */
	inline void FluxSliceForLevel(int L, double& OutBrightM, double& OutFaintM, double N = double(UniHlsl::UNI_WINDOW_N_EFF))
	{
		OutBrightM = AbsMagForRelevance(std::ldexp(double(N), L));
		OutFaintM = (L == 0) ? 1.0e30 : AbsMagForRelevance(std::ldexp(double(N), L - 1));
	}

	inline double CellWidthCm(int L) { return std::ldexp(1.0, L); }
}
