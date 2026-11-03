// test_photometry.cpp
// Tier 1: f(r) (monotonic, C1 at r0, invertible over 0..2^128 cm) and photometry
// (apparent magnitude, fade band, sensitivity scaling) against closed forms in double.

#include "../src/UniTest.h"
#include "UniLevel.h"

#include <cmath>

using namespace UniCore;
namespace H = UniHlsl;

UNI_TEST(photometry, compression)
{
	const double R0 = H::UNI_R0_CM;
	int NonMonotonic = 0;
	double WorstInverse = 0.0;      // relative error of f^-1(f(r)) against r
	double WorstInverseRatio = 0.0; // that error over its float bound
	float Prev = -1.0f;
	int Samples = 0;
	// Log steps from 1 cm to 2^128 cm (float max is 2^128, so stop just below).
	for (double L2 = 0.0; L2 < 127.999; L2 += 0.01)
	{
		const float R = float(std::ldexp(1.0, 0) * std::exp2(L2));
		const float F = H::Uni_Compress(R);
		if (!(F >= Prev)) { ++NonMonotonic; }
		Prev = F;
		const float Back = H::Uni_CompressInverse(F);
		const double Rel = std::fabs(double(Back) - double(R)) / double(R);
		WorstInverse = std::max(WorstInverse, Rel);
		// Float bound: f(r) carries about 2^-24 relative error, and exp(s/r0) turns an absolute
		// error in s/r0 into a relative error in r. So |dr/r| <= 4 * 2^-24 * (f(r)/r0 + 1).
		const double Bound = 4.0 * std::ldexp(1.0, -24) * (double(F) / R0 + 1.0);
		WorstInverseRatio = std::max(WorstInverseRatio, Rel / Bound);
		++Samples;
	}
	// C1 at r0: value and slope match from both sides.
	const double Eps = 1e-3;
	const double Left = H::Uni_Compress(float(R0 * (1 - Eps))), Right = H::Uni_Compress(float(R0 * (1 + Eps)));
	const double SlopeL = H::Uni_CompressSlope(float(R0 * (1 - 1e-6))), SlopeR = H::Uni_CompressSlope(float(R0 * (1 + 1e-6)));
	const bool C0 = std::fabs(double(H::Uni_Compress(float(R0))) - R0) / R0 < 1e-6 && std::fabs((Right - Left) / (2 * Eps * R0) - 1.0) < 1e-3;
	const bool C1 = std::fabs(SlopeL - 1.0) < 1e-6 && std::fabs(SlopeR - 1.0) < 1e-5;
	const double Span = H::Uni_Compress(float(std::ldexp(1.0, 127) * 1.9999)) / R0;

	// The inverse is judged against its float conditioning (see Bound above), not a flat
	// threshold: at 2^128 cm, f(r)/r0 is about 69 and float exp/log alone give ~1e-5 (D-110).
	Uni::Check(1, "T1.fr", "f(r) monotonic over 1..2^128 cm in log steps, C1 at r0, inverse exists (within float conditioning)")
		.Metric("samples", Samples).Metric("non_monotonic", NonMonotonic).Metric("worst_inverse_rel", WorstInverse)
		.Metric("worst_inverse_over_bound", WorstInverseRatio).Metric("span_in_r0", Span)
		.Pass(NonMonotonic == 0 && C0 && C1 && WorstInverseRatio <= 1.0,
			Uni::Fmt("%d samples, %d non-monotonic; continuous %s, C1 %s; inverse worst rel error %.2e = %.2f of its float bound; 2^128 cm maps to %.1f r0",
				Samples, NonMonotonic, C0 ? "yes" : "NO", C1 ? "yes" : "NO", WorstInverse, WorstInverseRatio, Span));
}

UNI_TEST(photometry, magnitudes)
{
	double WorstM = 0.0, WorstK = 0.0, WorstI = 0.0;   // M: error / (|M| + |5 log10(r/10pc)|)
	int FadeBad = 0;
	Uni::FTestRng R(0xF07Dull);
	for (int I = 0; I < 200000; ++I)
	{
		const double M = -25.0 + R.Unit() * 45.0;
		const double Dist = std::exp2(1.0 + R.Unit() * 125.0);
		const double Term = 5.0 * std::log10(Dist / kTenPcCm);
		const double Ref = M + Term;
		const double Got = H::Uni_ApparentMag(float(M), float(Dist));
		// Relative to the terms, not the sum: M + 5 log10 cancels, and a float sum of two
		// terms of size T is only good to ~T * 2^-24 however small the result (D-110).
		WorstM = std::max(WorstM, std::fabs(Got - Ref) / (std::fabs(M) + std::fabs(Term)));

		const double Dm = -5.0 + R.Unit() * 10.0;
		const double KRef = std::pow(10.0, Dm / 5.0);
		WorstK = std::max(WorstK, std::fabs(double(H::Uni_SensitivityScale(float(Dm))) - KRef) / KRef);

		const double MLim = 6.5 + Dm;
		// Exposure tracks the scene: intensities are judged within 30 mag of the exposure
		// point, the range a frame can show (float 10^x overflows past ~38 mag regardless).
		const double MExp = Ref - 30.0 + R.Unit() * 60.0;
		const double FadeRef = std::min(1.0, std::max(0.0, (MLim - Ref) / 0.5));
		const double IRef = std::pow(10.0, -0.4 * (Ref - MExp)) * FadeRef;
		const double IGot = H::Uni_SpriteIntensity(float(Ref), float(MExp), float(MLim));
		// 10^(-0.4 m) amplifies an absolute magnitude error dm into 0.92 dm relative; the
		// float magnitude itself is good to ~1e-6 of 30, so 1e-4 relative is the bound.
		if (IRef > 1e-30) { WorstI = std::max(WorstI, std::fabs(IGot - IRef) / IRef); }
		if (Ref >= MLim && IGot != 0.0) { ++FadeBad; }                  // invisible at and past m_lim
		if (Ref <= MLim - 0.5 && H::Uni_MagFade(float(Ref), float(MLim)) != 1.0f) { ++FadeBad; }
	}
	// Window growth (D-162): reach = min(N_eff k + 1/K, N_max) cells, 2 at dm = 0 (a 5^3 window);
	// bounding half-width ceil(reach): 2 at dm 0, 4 at +1, N_max (6) at +5, 1 at -5.
	const bool Win = std::fabs(H::Uni_WindowReach(0.0f) - 2.0f) < 1e-5f && H::Uni_WindowHalfWidth(0.0f) == 2 && H::Uni_WindowHalfWidth(5.0f) == 6
		&& H::Uni_WindowHalfWidth(1.0f) == 4 && H::Uni_WindowHalfWidth(-5.0f) == 1;
	// Relevance and level inverse: AbsMagForRelevance(RelevanceFlux(M)) == M.
	double WorstRel = 0.0;
	for (double M = -30; M < 30; M += 0.37) { WorstRel = std::max(WorstRel, std::fabs(AbsMagForRelevance(RelevanceFlux(M)) - M)); }

	// Tooling bound 1e-6, applied relative to the terms for m (cancellation) and to k directly;
	// intensity inherits ln(10) * 0.4 * |m - m_exp| amplification, bound 1e-4 (D-110).
	Uni::Check(1, "T1.photometry", "Apparent magnitude, fade band and sensitivity k = 10^(dm/5) match closed form (1e-6, condition-aware)")
		.Metric("worst_mag_rel_terms", WorstM).Metric("worst_k_rel", WorstK).Metric("worst_intensity_rel", WorstI).Metric("fade_violations", FadeBad)
		.Pass(WorstM < 1e-6 && WorstK < 1e-6 && WorstI < 1e-4 && FadeBad == 0 && Win && WorstRel < 1e-9,
			Uni::Fmt("200k samples: worst m err %.2e of term size, k rel err %.2e, intensity rel err %.2e, fade violations %d; window N(dm) %s; relevance inverse err %.1e",
				WorstM, WorstK, WorstI, FadeBad, Win ? "ok" : "WRONG", WorstRel));
}
