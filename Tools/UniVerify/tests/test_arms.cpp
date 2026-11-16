// test_arms.cpp
// D-256 (owner 2026-11-15: GAMER's Fig. 7, "spiral math/barred spiral logic, and spiral
// parameterization to get that table of morphology variance"): the arms' centre line from
// Ringermacher & Mead (Uni_GalaxyWindingU). T2.galaxy.arms_rm:
//  - the openness 0 is the log spiral u / tan(p) bit for bit, its rate 1 / tan(p);
//  - with the openness: the winding 0 far out (as 1 / r: at 1000 disc scale lengths under 0.5% of the total),
//    the total winding at the centre (within 1%), monotonic inward; its rate the numerical
//    derivative's (central differences in ln r: 1e-3 of the largest rate);
//  - three or more arms take at least the floors (openness 0.5, winding 3.5);
//  - over 64 rolled galaxies the disc's light within 1% of the same arms on the old log spiral
//    (a winding shifts each radius's lobes in azimuth: their mean is kept).

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace UniCore;
namespace H = UniHlsl;

UNI_TEST(galaxy, arms_rm)
{
	const FUniverseDef D = UniScenes::Base();
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
	auto Rolled = [&](u32 Seed)
	{
		FFieldParams P = E.Exemplar;
		for (const FParamRange& R : E.Ranges) { P.V[R.Slot] = H::Uni_RollRange(Seed, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
		return P;
	};
	// 1. Openness 0: the log spiral.
	size_t LogBad = 0, LogN = 0;
	for (int I = 0; I < 2000; ++I)
	{
		FFieldParams P = E.Exemplar;
		P.V[7] = 0.1f + 0.5f * float(H::Uni_DrawUnit(0xA1u, 1u, u32(I)));
		P.V[21] = 0.0f;
		const H::FUniFieldParams Hp = P.ToHlsl();
		const float R = 1.0e-3f + float(H::Uni_DrawUnit(0xA2u, 1u, u32(I)));
		const float U = -std::log(std::max(R, 1.0e-4f));
		float Rate = 0.0f;
		const float W = H::Uni_GalaxyWindingU(R, U, Hp, Rate);
		const float Want = U / std::max(Hp.B.w, 1.0e-3f), WantR = 1.0f / std::max(Hp.B.w, 1.0e-3f);
		++LogN;
		LogBad += (W != Want || Rate != WantR) ? 1 : 0;
	}
	// 2. With the openness: limits, monotonic, the rate.
	size_t Gal = 0, FarBad = 0, CentreBad = 0, MonoBad = 0, FloorBad = 0;
	double WorstRate = 0.0;
	for (u32 G = 0; G < 64; ++G)
	{
		FFieldParams P = Rolled(H::Uni_SeedMix(0xA3A5u, G));
		const H::FUniFieldParams Hp = P.ToHlsl();
		const float Hx = Hp.A.x;
		const float Wt = Hp.B.z > 2.5f ? std::max(Hp.B.w, 3.5f) : Hp.B.w;
		float Rate = 0.0f;
		const float Far = H::Uni_GalaxyWindingU(1000.0f * Hx, -std::log(1000.0f * Hx), Hp, Rate);
		const float Centre = H::Uni_GalaxyWindingU(1.0e-5f, -std::log(1.0e-5f), Hp, Rate);
		++Gal;
		FarBad += std::abs(Far) > 0.005f * Wt ? 1 : 0;
		CentreBad += std::abs(Centre / Wt - 1.0f) > 0.01f ? 1 : 0;
		float Prev = -1.0e30f, MaxRate = 0.0f;
		std::vector<std::pair<double, double>> Rt;
		for (int K = 0; K <= 400; ++K)
		{
			const double Lr = std::log(20.0 * Hx) - (std::log(20.0 * Hx) - std::log(0.01 * Hx)) * K / 400.0;   // inward
			const float R = float(std::exp(Lr));
			float Rk = 0.0f;
			const float W = H::Uni_GalaxyWindingU(R, -std::log(R), Hp, Rk);
			MonoBad += W < Prev - 1.0e-5f ? 1 : 0;
			Prev = W;
			MaxRate = std::max(MaxRate, Rk);
			const double E1 = 1.0e-3;
			float Ra = 0.0f, Rb = 0.0f;
			const double Wa = H::Uni_GalaxyWindingU(float(std::exp(Lr - E1)), float(-(Lr - E1)), Hp, Ra);
			const double Wb = H::Uni_GalaxyWindingU(float(std::exp(Lr + E1)), float(-(Lr + E1)), Hp, Rb);
			Rt.push_back({ double(Rk), (Wa - Wb) / (2.0 * E1) });   // d theta / du, u = -ln r
		}
		for (const auto& Q : Rt) { WorstRate = std::max(WorstRate, std::abs(Q.first - Q.second) / std::max(double(MaxRate), 1e-9)); }
		// The floors for three or more arms.
		if (Hp.B.z > 2.5f)
		{
			FFieldParams Pl = P; Pl.V[7] = 1.0f; Pl.V[21] = 0.05f;
			FFieldParams Pf = P; Pf.V[7] = 3.5f; Pf.V[21] = 0.5f;
			for (int K = 0; K < 8; ++K)
			{
				const float R = Hx * (0.1f + 0.5f * float(K));
				float A = 0.0f, B = 0.0f;
				FloorBad += H::Uni_GalaxyWindingU(R, -std::log(R), Pl.ToHlsl(), A) != H::Uni_GalaxyWindingU(R, -std::log(R), Pf.ToHlsl(), B) ? 1 : 0;
			}
		}
	}
	// 3. The winding keeps the disc's light: the mid-plane over the disc against the log spiral's.
	double WorstLight = 0.0;
	for (u32 G = 0; G < 64; ++G)
	{
		FFieldParams P = Rolled(H::Uni_SeedMix(0xA3A6u, G));
		P.V[5] = 0.0f;   // no bulge: the disc's own light
		for (int I = 16; I < 20; ++I) { P.V[I] = 0.0f; }   // no disc modes (the lopsidedness's phase against the arms' moves light by up to ~10%)
		FFieldParams Pn = P; Pn.V[7] = 0.25f; Pn.V[21] = 0.0f;   // the same arms on the old log spiral
		const H::FUniFieldParams A = P.ToHlsl(), B = Pn.ToHlsl();
		double Sa = 0.0, Sb = 0.0;
		const int Nr = 60, Nt = 96;
		for (int Ri = 0; Ri < Nr; ++Ri)
		{
			const double R = 0.6 * (Ri + 0.5) / Nr;
			for (int Ti = 0; Ti < Nt; ++Ti)
			{
				const double Th = 6.283185307 * (Ti + 0.5) / Nt;
				const H::float3 Pt(float(R * std::cos(Th)), float(R * std::sin(Th)), 0.0f);
				Sa += R * H::UniField_GalaxyFiltered(Pt, A, 0.0f);
				Sb += R * H::UniField_GalaxyFiltered(Pt, B, 0.0f);
			}
		}
		WorstLight = std::max(WorstLight, std::abs(Sa / std::max(Sb, 1e-300) - 1.0));
		if (std::getenv("UNI_ARMS_DEBUG")) { std::printf("    g%u ratio %.4f arms %.0f amp %.2f sharp %.2f asym %.2f W %.2f B %.3f G.x %.3f H.x %.3f H.y %.3f H.z %.3f inner %.2f\n", G, Sa / Sb, A.B.z, A.C.x, A.C.y, A.F.x, A.B.w, A.F.y, A.G.x, A.H.x, A.H.y, A.H.z, A.C.w); }
	}
	const bool bPass = LogBad == 0 && FarBad == 0 && CentreBad == 0 && MonoBad == 0 && FloorBad == 0 && WorstRate < 1.0e-3 && WorstLight < 0.01;
	Uni::Check(2, "T2.galaxy.arms_rm", "D-256 Ringermacher & Mead arms (Uni_GalaxyWindingU): openness 0 the log spiral bit for bit; over 64 rolled galaxies the winding 0 far out (0.5% of the total at 1000 scale lengths), the total at the centre (1%), monotonic inward, its rate the numerical derivative's (1e-3); three or more arms at the floors; the disc's light (modes off) within 1% of the same arms on the log spiral")
		.Metric("rate_err", WorstRate).Metric("light", WorstLight)
		.Pass(bPass, Uni::Fmt("log spiral: %zu of %zu differ; %zu galaxies: far %zu, centre %zu, not monotonic %zu, rate within %.2g of the largest; floors %zu differ; the disc's light against the log spiral's within %.3f%%",
			LogBad, LogN, Gal, FarBad, CentreBad, MonoBad, WorstRate, FloorBad, 100.0 * WorstLight));
}
