// test_shape.cpp
// D-258 (owner 2026-11-15, on GAMER: "worth lifting a lot of the shaping and noise paradigms"):
// galaxy_detail's channels shaped nonlinearly (UNI_GAL_SHAPE: the knots to the 8th; the structure
// read at a quarter of the detail's frequency, its ridged channel to the 4th for dust, its fBm
// cubed for light). D-259: the structure read from galaxy_shape (shaped before its mips were
// taken), the knots' amplitude keeping the young share. T2.galaxy.noise_shape:
//  - the constants: UniFields.ush's UNI_GAL_SHAPE the shim's, the bake's powers D-258's (4, 3, 8);
//  - galaxy_shape: each channel's mean 1 at every mip (1e-3: half floats) and over 200k random
//    trilinear reads at mips 0-6 (2%);
//  - the knots (galaxy_shape's G^8 at the detail's point, its alpha by mip): their mean the linear
//    term's (0.005) at mips 0-6 and between, their young share E[max(Knot, 0)] the linear term's
//    within 5% at mips 0-4;
//  - the Milky Way's and 16 rolled galaxies' disc light and dust (face-on columns over the disc):
//    the shaping's factor (against the structure read off) at footprints of 3e-3 and 3e-2 against
//    its own at 1e-4: over the population unbiased (1.5%; galaxy_shape's reads are exact means, so
//    at 3e-3 the dust's within 0.2% galaxy by galaxy), each galaxy within 6% (light) / 20% (dust)
//    (the structure is read at the arms' twisted point: at 3e-2 the arms and lanes blend to their
//    means and take their correlation with it, +-1.4% / +-4.4% rms); against the structure
//    read off the population's mean light within 5%, dust within 10% (one galaxy's own pattern
//    moves its total, by +-25%: a few structure cells hold most of a disc's light).

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace UniCore;
namespace H = UniHlsl;

UNI_TEST(galaxy, noise_shape)
{
	FWebTexture T[3];
	for (int S = 0; S < 3; ++S) { std::string E; if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { Uni::Check(2, "T2.galaxy.noise_shape", "D-258").Pass(false, "textures not loaded"); return; } }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &T[I]); }
	const H::float4 Sh = H::GGalShape;
	// 0. The constants.
	std::string ConstText;
	{
		std::ifstream F("Shaders/Private/Core/UniFields.ush");
		std::stringstream B; B << F.rdbuf();
		const std::string Src = B.str();
		const std::regex Re("#define\\s+UNI_GAL_SHAPE\\s+float4\\(([-0-9.eE]+)f?,\\s*([-0-9.eE]+)f?,\\s*([-0-9.eE]+)f?,\\s*([-0-9.eE]+)f?\\)");
		std::smatch M;
		if (!std::regex_search(Src, M, Re)) { ConstText = " UNI_GAL_SHAPE not found;"; }
		else
		{
			const float V[4] = { std::stof(M[1].str()), std::stof(M[2].str()), std::stof(M[3].str()), std::stof(M[4].str()) };
			if (V[0] != Sh.x || V[1] != Sh.y || V[2] != Sh.z || V[3] != Sh.w) { ConstText += Uni::Fmt(" HLSL (%g %g %g %g) vs shim;", V[0], V[1], V[2], V[3]); }
		}
		const FGalaxyShapePowers Pw = GalaxyShapePowers();
		if (Pw.Dust != 4.0f || Pw.Light != 3.0f || Pw.Knot != 8.0f) { ConstText += " bake powers not D-258's (4, 3, 8);"; }
	}
	// 1. galaxy_shape's means: every mip's texels, and random trilinear reads.
	const FWebTexture* Gs4 = BoundGalaxyShapeTexture();
	double WorstTexel = 1.0, WorstRead = 0.0;
	if (Gs4)
	{
		WorstTexel = 0.0;
		for (uint32_t Mi = 0; Mi < Gs4->NumMips(); ++Mi)
		{
			double A[3] = { 0, 0, 0 };
			const size_t Nt = Gs4->TexelCount(Mi);
			for (size_t I = 0; I < Nt; ++I) { const H::float4 V = Gs4->TexelAt(Mi, I); A[0] += V.x; A[1] += V.y; A[2] += V.z; }
			for (int C = 0; C < 3; ++C) { WorstTexel = std::max(WorstTexel, std::abs(A[C] / double(Nt) - 1.0)); }
		}
		for (int Mi = 0; Mi <= 6; ++Mi)
		{
			double A[3] = { 0, 0, 0 };
			const int N = 200000;
			for (int I = 0; I < N; ++I)
			{
				const H::float3 P(float(H::Uni_DrawUnit(0x5A3u, u32(Mi), u32(3 * I))) * 64.0f, float(H::Uni_DrawUnit(0x5A3u, u32(Mi), u32(3 * I + 1))) * 64.0f, float(H::Uni_DrawUnit(0x5A3u, u32(Mi), u32(3 * I + 2))) * 64.0f);
				const H::float4 V = UNI_GAL_SHAPED(P, float(Mi));
				A[0] += V.x; A[1] += V.y; A[2] += V.z;
			}
			for (int C = 0; C < 3; ++C) { WorstRead = std::max(WorstRead, std::abs(A[C] / N - 1.0)); }
		}
	}
	// 2. The knots: their young share (E[max(Knot, 0)], the shaped read at the detail's point) the
	// linear term's at mips 0-4, their mean the linear term's (0.0457) at every mip.
	double WorstMean = 0.0, WorstYoung = 0.0;
	std::string MeanDetail;
	for (int Mi = 0; Mi <= 12; ++Mi)
	{
		const float Mip = 0.5f * float(Mi);
		double Km = 0.0, Ys = 0.0, Yl = 0.0;
		const int N = 200000;
		for (int I = 0; I < N; ++I)
		{
			const H::float3 P(float(H::Uni_DrawUnit(0x5A1u, u32(Mi), u32(3 * I))) * 64.0f, float(H::Uni_DrawUnit(0x5A1u, u32(Mi), u32(3 * I + 1))) * 64.0f, float(H::Uni_DrawUnit(0x5A1u, u32(Mi), u32(3 * I + 2))) * 64.0f);
			const H::float4 V = H::UNI_GAL_DETAIL(P, Mip);
			const H::float4 Ks = UNI_GAL_SHAPED(P, Mip);
			float Nf = 0.0f, Kn = 0.0f, Cl = 1.0f;
			H::FUniFieldParams Pz{};
			H::Uni_GalaxyDetailTerms(V, Sh.x * Ks.w * (Ks.z - 1.0f), Pz, Nf, Kn, Cl);
			Km += Kn; Ys += std::max(double(Kn), 0.0); Yl += std::max(2.0 * double(V.y) - 1.0, 0.0);
		}
		WorstMean = std::max(WorstMean, std::abs(Km / N - kGalKnotMean));
		if (Mip <= 4.0f) { WorstYoung = std::max(WorstYoung, std::abs(Ys / std::max(Yl, 1e-300) - 1.0)); }
		if (Mi % 4 == 0) { MeanDetail += Uni::Fmt("%smip %.0f mean %+.4f young x%.3f", MeanDetail.empty() ? "" : ", ", double(Mip), Km / N - kGalKnotMean, Ys / std::max(Yl, 1e-300)); }
	}
	// 3. Light and dust kept (face-on columns through the disc).
	FUniverseDef D = UniScenes::Base();
	UniScenes::UseGalaxyTextures(D, T[0].Size);
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
	const FVolumeProfile& Prof = D.Arch(UniScenes::kGalaxy).Volume;
	std::vector<FFieldParams> Gs;
	{
		FUniverseDef F = UniScenes::Flythrough();
		UniScenes::UseGalaxyTextures(F, T[0].Size);
		for (const FAnchor& A : F.Anchors) { if (!A.EmitterParams.empty()) { Gs.push_back(A.EmitterParams[0]); } }
	}
	for (u32 G = 0; G < 16; ++G)
	{
		FFieldParams P = E.Exemplar;
		const u32 Seed = H::Uni_SeedMix(0x5A2u, G);
		for (const FParamRange& R : E.Ranges) { P.V[R.Slot] = H::Uni_RollRange(Seed, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
		Gs.push_back(P);
	}
	double WorstL = 0.0, WorstD = 0.0, SumL = 0.0, SumD = 0.0, BiasL[2] = { 0, 0 }, BiasD[2] = { 0, 0 };
	int NL = 0;
	const float Foots[3] = { 1.0e-4f, 3.0e-3f, 3.0e-2f };
	for (const FFieldParams& Pf : Gs)
	{
		const H::FUniFieldParams P = Pf.ToHlsl(), Pd = DustFrom(Pf, Prof).ToHlsl();
		double L0 = 0.0, D0 = 0.0;
		for (float Foot : Foots)
		{
			double L[2] = { 0, 0 }, Du[2] = { 0, 0 };
			for (int K = 0; K < 2; ++K)
			{
				H::GGalShape = K == 0 ? Sh : H::float4(Sh.x, Sh.y, Sh.z, 0.0f);
				const int Nr = 40, Nt = 64, Nz = 12;
				for (int Ri = 0; Ri < Nr; ++Ri) for (int Ti = 0; Ti < Nt; ++Ti) for (int Zi = 0; Zi < Nz; ++Zi)
				{
					const double R = 0.5 * (Ri + 0.5) / Nr, Th = 6.283185307 * (Ti + 0.37 * Zi) / Nt, Z = 0.03 * ((Zi + 0.5) / Nz - 0.5);
					const H::float3 Pt(float(R * std::cos(Th)), float(R * std::sin(Th)), float(Z));
					float Re = 0.0f, Rd = 0.0f;
					H::float4 Comp(0, 0, 0, 0);
					H::UniField_GalaxyPairC(Pt, P, Pd, Foot, Re, Rd, Comp);
					L[K] += R * Re; Du[K] += R * Rd;
				}
			}
			H::GGalShape = Sh;
			if (std::getenv("UNI_SHAPE_DEBUG")) { std::printf("    foot %.0e light x%.3f dust x%.3f\n", double(Foot), L[0] / L[1], Du[0] / Du[1]); }
			const double Rl = L[0] / std::max(L[1], 1e-300), Rdu = Du[0] / std::max(Du[1], 1e-300);   // the shaping's own factor
			if (Foot == Foots[0]) { L0 = Rl; D0 = Rdu; SumL += Rl; SumD += Rdu; ++NL; }
			else
			{
				WorstL = std::max(WorstL, std::abs(Rl / std::max(L0, 1e-300) - 1.0));
				WorstD = std::max(WorstD, std::abs(Rdu / std::max(D0, 1e-300) - 1.0));
				const int Fi = Foot == Foots[1] ? 0 : 1;
				BiasL[Fi] += Rl / std::max(L0, 1e-300) - 1.0; BiasD[Fi] += Rdu / std::max(D0, 1e-300) - 1.0;
			}
		}
	}
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
	const double MeanL = SumL / std::max(NL, 1), MeanD = SumD / std::max(NL, 1);
	double WorstBias = 0.0;
	for (int Fi = 0; Fi < 2; ++Fi) { BiasL[Fi] /= std::max(NL, 1); BiasD[Fi] /= std::max(NL, 1); WorstBias = std::max({ WorstBias, std::abs(BiasL[Fi]), std::abs(BiasD[Fi]) }); }
	const bool bPass = ConstText.empty() && Gs4 != nullptr && WorstTexel <= 1e-3 && WorstRead <= 0.02 && WorstMean <= 0.005 && WorstYoung <= 0.05
		&& WorstBias <= 0.015 && WorstL <= 0.06 && WorstD <= 0.20 && std::abs(MeanL - 1.0) <= 0.05 && std::abs(MeanD - 1.0) <= 0.10;
	Uni::Check(2, "T2.galaxy.noise_shape", "D-258 / D-259 GAMER-style noise shaping: UNI_GAL_SHAPE the shim's, its y and z the bake's powers; galaxy_shape's channels mean 1 at every mip (1e-3) and over random trilinear reads at mips 0-6 (2%); the knots' mean the linear term's (0.005) at mips 0-6 and halves, their young share the linear term's within 5% at mips 0-4; 17 galaxies' disc light and dust: the shaping's factor at footprints 3e-3 and 3e-2 against its own at 1e-4 unbiased over the population (1.5%), each galaxy within 6% (light) / 20% (dust) (the structure's correlation with the arms and lanes, which blend to their means); against the structure read off, the mean light within 5%, dust 10%")
		.Metric("mean_err", WorstMean).Metric("young_err", WorstYoung).Metric("shape_read_err", WorstRead).Metric("lod_light", WorstL).Metric("lod_dust", WorstD).Metric("pop_light", MeanL).Metric("pop_dust", MeanD)
		.Pass(bPass, Uni::Fmt("constants%s; galaxy_shape %s: texel means within %.1e, reads %.2f%%; knots: means within %.4f (%s); %zu galaxies: across footprints mean light %+.2f%% / %+.2f%%, dust %+.2f%% / %+.2f%%, each within light %.2f%%, dust %.2f%%; against the read off: mean light x%.3f, dust x%.3f",
			ConstText.empty() ? " ok" : ConstText.c_str(), Gs4 ? "bound" : "MISSING", WorstTexel, 100.0 * WorstRead, WorstMean, MeanDetail.c_str(), Gs.size(), 100.0 * BiasL[0], 100.0 * BiasL[1], 100.0 * BiasD[0], 100.0 * BiasD[1], 100.0 * WorstL, 100.0 * WorstD, MeanL, MeanD));
}
