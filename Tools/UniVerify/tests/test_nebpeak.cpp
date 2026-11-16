// test_nebpeak.cpp
// D-253 (owner 2026-11-15: "our peak nebula brightness is probably too high"): uu.Nebula.Peak, the
// nebulae's shape held softly under k x its mean (UNI_NEB_TUNE3.y; Uni_GalaxyNebulaDensityAtF:
// s / sqrt(1 + (s / k)^2)). Over the Milky Way's gas layer about the solar circle (400k points):
// off (0) bit for bit; at 200 every density exactly the rule's, none above 200 x the mean, the
// nebulae's median within 1%, their 99th percentile at least halved; the light lost reported.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace UniCore;
namespace H = UniHlsl;

UNI_TEST(galaxy, nebula_peak)
{
	FWebTexture T[3];
	bool bTex = true;
	for (int S = 0; S < 3; ++S) { std::string E; if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bTex = false; } }
	if (!bTex) { Uni::Check(2, "T2.galaxy.nebula_peak", "D-253").Pass(false, "textures not loaded"); return; }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &T[I]); }
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, T[0].Size);
	const FFieldParams* Mw = nullptr;
	for (const FAnchor& A : D.Anchors) { if (!A.EmitterParams.empty()) { Mw = &A.EmitterParams[0]; } }
	const H::FUniFieldParams P = Mw->ToHlsl();
	const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
	const float Kp = 200.0f;
	auto Density = [&](const H::float3& Pt, const H::float4& Nv, float Peak, float& Md)
	{
		FVolumeSettings Sv; Sv.NebulaPeak = Peak;
		ApplyNebulaTune(NebulaTune(Sv));
		float X = 0.0f, Mr = 1.0f;
		Md = 1.0f;
		return H::Uni_GalaxyNebulaDensityAtF(Pt, Nv, P, 1e-5f, 1e-5f, 0.0f, 1.0f, 0.35f, X, Md, Mr);
	};
	size_t N = 0, BitOff = 0, RuleBad = 0, Over = 0;
	std::vector<double> S0, S1;
	double L0 = 0.0, L1 = 0.0;
	ApplyNebulaTune(FNebulaTune{});
	const double Mean = double(H::GNebTune.z);
	for (int I = 0; I < 400000; ++I)
	{
		const double R = 0.25 + 0.5 * H::Uni_DrawUnit(0xBEEFu, 1u, u32(3 * I)), Ph = 6.2831853 * H::Uni_DrawUnit(0xBEEFu, 1u, u32(3 * I + 1));
		const double Z = (H::Uni_DrawUnit(0xBEEFu, 1u, u32(3 * I + 2)) - 0.5) * 0.02;
		const H::float3 Pt(float(R * std::cos(Ph)), float(R * std::sin(Ph)), float(Z));
		H::float4 C(0, 0, 0, 0), Nv(0, -1, 0.5f, 0);
		ApplyNebulaTune(FNebulaTune{});
		H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-5f, C, Nv);
		if (!(Nv.x > 0.0f)) { continue; }
		float Md0 = 1.0f, Mdb = 1.0f, Md1 = 1.0f;
		ApplyNebulaTune(FNebulaTune{});
		float X = 0.0f, Mr = 1.0f;
		const float Base = H::Uni_GalaxyNebulaDensityAtF(Pt, Nv, P, 1e-5f, 1e-5f, 0.0f, 1.0f, 0.35f, X, Mdb, Mr);   // D-252's
		const float D0 = Density(Pt, Nv, 0.0f, Md0);
		const float D1 = Density(Pt, Nv, Kp, Md1);
		++N;
		BitOff += (D0 != Base || Md0 != Mdb) ? 1 : 0;
		const double Sc = double(Nv.x) * double(UNI_NEBULA_SCALE);
		const double Sa = double(D0) / Sc, Sb = double(D1) / Sc;
		const double Want = Sa / std::sqrt(1.0 + (Sa / (double(Kp) * Mean)) * (Sa / (double(Kp) * Mean)));
		RuleBad += std::abs(Sb - Want) > 1e-5 * std::max(Want, 1e-6) + 1e-9 ? 1 : 0;
		Over += Sb > double(Kp) * Mean * 1.0001 ? 1 : 0;
		L0 += D0; L1 += D1;
		if (Sa > 0.0) { S0.push_back(Sa / Mean); S1.push_back(Sb / Mean); }
	}
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
	ApplyNebulaTune(FNebulaTune{});
	auto Pct = [](std::vector<double> V, double Q) { std::sort(V.begin(), V.end()); return V.empty() ? 0.0 : V[size_t(Q * double(V.size() - 1))]; };
	const double M0 = Pct(S0, 0.5), M1 = Pct(S1, 0.5), P0 = Pct(S0, 0.99), P1 = Pct(S1, 0.99);
	const bool bPass = BitOff == 0 && RuleBad == 0 && Over == 0 && std::abs(M1 / M0 - 1.0) <= 0.01 && P1 <= 0.5 * P0 && !S0.empty();
	Uni::Check(2, "T2.galaxy.nebula_peak", "D-253 uu.Nebula.Peak: over the Milky Way's gas layer about the solar circle, off bit for bit (D-252's); at 200 every density the rule's s / sqrt(1 + (s / k)^2), none above 200 x the mean, the nebulae's median within 1%, their 99th percentile at least halved")
		.Metric("median_x", M1 / M0).Metric("p99_x", P1 / P0).Metric("light_x", L1 / std::max(L0, 1e-300))
		.Pass(bPass, Uni::Fmt("%zu points in the galaxy (%zu in nebulae): off %zu differ; at 200: %zu off the rule, %zu above the cap; median %.1f -> %.1f x the mean (x%.3f), 99th percentile %.0f -> %.0f (x%.2f); the nebulae's light x%.3f",
			N, S0.size(), BitOff, RuleBad, Over, M0, M1, M1 / M0, P0, P1, P1 / P0, L1 / std::max(L0, 1e-300)));
}
