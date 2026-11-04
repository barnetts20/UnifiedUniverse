// test_galaxy.cpp
// D-178: galaxy disc-level modes and per-arm asymmetry (SPRITE_PASS_PLAN §3 A1). The galaxy
// field with every new parameter at zero is the D-172 field exactly; each mode does what it
// claims (the warp moves the disc's mid-plane by its formula, lopsidedness and flare keep the
// face-on light, the asymmetric arms reduce to the symmetric wave as asymmetry goes to zero);
// the prefiltered field keeps the light of rolled galaxies; and contact sheets of rolled
// galaxies with the modes off and on.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	void WriteFrame(const FImage& Img, const std::string& Name, const std::vector<std::string>& Hud)
	{
		const std::string Base = RenderDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb");
		if (!F) { return; }
		std::fprintf(F, "{\"lines\":[");
		for (size_t I = 0; I < Hud.size(); ++I) { std::fprintf(F, "%s\"%s\"", I ? "," : "", Hud[I].c_str()); }
		std::fprintf(F, "]}\n");
		std::fclose(F);
	}

	/** The galaxy field as it was before D-178 (D-172), copied verbatim: the reference for "every
	 *  new parameter at zero changes nothing". */
	float LegacyGalaxy(const H::float3& InP, const H::FUniFieldParams& InPar, float InFoot)
	{
		using namespace UniHlsl;
		const float Cut = InPar.D.x;
		const float R2 = dot(InP, InP);
		if (R2 >= Cut * Cut) { return 0.0f; }
		const float Rcyl = sqrt(InP.x * InP.x + InP.y * InP.y);
		const float Z = abs(InP.z);
		float Rho = 0.0f;
		const float DiscW = InPar.A.z;
		if (DiscW > 0.0f)
		{
			float Disc;
			if (InFoot > 0.0f)
			{
				const float Hh = max(InPar.A.y, 1.0e-6f);
				const float He = sqrt(Hh * Hh + 0.25f * InFoot * InFoot);
				Disc = DiscW * exp(-Rcyl / max(InPar.A.x, 1.0e-5f)) * exp(-Z / He) * (Hh / He);
			}
			else
			{
				Disc = DiscW * exp(-Rcyl / max(InPar.A.x, 1.0e-5f)) * exp(-Z / max(InPar.A.y, 1.0e-6f));
			}
			const float ArmAmp = InPar.C.x;
			if (ArmAmp > 0.0f && InPar.B.z > 0.0f)
			{
				float Blend = 0.0f;
				if (InFoot > 0.0f)
				{
					const float TanP = max(InPar.B.w, 1.0e-3f);
					const float Grad = InPar.B.z / max(Rcyl, 0.5f * max(InPar.A.x, 1.0e-4f)) * sqrt(1.0f + 1.0f / (TanP * TanP));
					Blend = saturate((InFoot * Grad - 1.0f) * (1.0f / 3.0f));
				}
				float Wave = 0.0f;
				if (Blend < 1.0f)
				{
					const float Theta = atan2(InP.y, InP.x);
					const float Phi = InPar.B.z * (Theta - log(max(Rcyl, 1.0e-4f)) / max(InPar.B.w, 1.0e-3f) + InPar.C.z);
					Wave = pow(0.5f + 0.5f * cos(Phi), max(InPar.C.y, 0.1f));
				}
				if (Blend > 0.0f) { Wave = Wave + (Uni_GalaxyArmMean(InPar.C.y) - Wave) * Blend; }
				Disc = Disc * ((1.0f - ArmAmp) + ArmAmp * Wave * (1.0f + InPar.C.y));
			}
			Rho += Disc;
		}
		const float BulgeW = InPar.B.y;
		if (BulgeW > 0.0f)
		{
			const float Q = max(InPar.B.x, 0.05f);
			float Re = max(InPar.A.w, 1.0e-5f);
			float Amp = BulgeW;
			if (InFoot > 0.0f)
			{
				const float Rf = sqrt(Re * Re + InFoot * InFoot);
				const float S = Re / Rf;
				Amp = Amp * (S * S * S) * (Uni_BulgeInCut(Re, Cut) / max(Uni_BulgeInCut(Rf, Cut), 1.0e-6f));
				Re = Rf;
			}
			const float R3 = sqrt(Rcyl * Rcyl + (Z / Q) * (Z / Q));
			Rho += Amp * exp(-3.67f * (sqrt(R3 / Re) - 1.0f));
		}
		const float R = sqrt(R2);
		return Rho * saturate((Cut - R) / (0.05f * Cut));
	}

	/** A galaxy's parameters rolled as the generator rolls them (exemplar, then each range by
	 *  name from the object seed). */
	FFieldParams Rolled(const FUniEmitter& E, u32 ObjSeed)
	{
		FFieldParams P = E.Exemplar;
		for (const FParamRange& R : E.Ranges) { P.V[R.Slot] = H::Uni_RollRange(ObjSeed, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
		return P;
	}

	FFieldParams ModesOff(FFieldParams P)
	{
		for (int I = 16; I < 32; ++I) { P.V[I] = 0.0f; }
		return P;
	}

	double Unit(u32 S, u32 K) { return double(H::Uni_DrawUnit(S, 0x51u, K)); }

	/** Monte Carlo integral of the field (unit-cube stratified, inside the cut sphere). */
	double Integral(const H::FUniFieldParams& P, float Foot, int N, u32 Seed)
	{
		double Sum = 0.0;
		const int G = int(std::cbrt(double(N)));
		for (int X = 0; X < G; ++X) for (int Y = 0; Y < G; ++Y) for (int Z = 0; Z < G; ++Z)
		{
			const u32 K = u32((X * G + Y) * G + Z);
			// Concentrated in z: the disc is thin, so sample z on [-0.25, 0.25] (the bulge's light
			// beyond it is the same with and without modes) and x, y on [-1, 1].
			const H::float3 Pt(float(-1.0 + 2.0 * (X + Unit(Seed, 3 * K)) / G), float(-1.0 + 2.0 * (Y + Unit(Seed, 3 * K + 1)) / G), float(-0.25 + 0.5 * (Z + Unit(Seed, 3 * K + 2)) / G));
			Sum += double(H::UniField_GalaxyFiltered(Pt, P, Foot));
		}
		return Sum * (4.0 * 0.5) / double(G * G * G);
	}
}

UNI_TEST(galaxy, shape)
{
	const FUniverseDef D = UniScenes::Base();
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
	const FVolumeProfile& Prof = D.Arch(UniScenes::kGalaxy).Volume;

	// 1. Every new parameter at zero: the D-172 field bit for bit (rolled galaxies, filtered and not).
	size_t Points = 0, Differ = 0;
	for (u32 G = 0; G < 24; ++G)
	{
		const H::FUniFieldParams P = ModesOff(Rolled(E, H::Uni_SeedMix(0xC0FFEEu, G))).ToHlsl();
		for (u32 K = 0; K < 2000; ++K)
		{
			const u32 S = H::Uni_SeedMix(G, K);
			const H::float3 Pt(float(2.0 * Unit(S, 0) - 1.0), float(2.0 * Unit(S, 1) - 1.0), float(0.3 * (2.0 * Unit(S, 2) - 1.0)));
			const float Foot = (K & 1u) ? float(0.05 * Unit(S, 3)) : 0.0f;
			const float A = H::UniField_GalaxyFiltered(Pt, P, Foot), B = LegacyGalaxy(Pt, P, Foot);
			++Points;
			if (std::memcmp(&A, &B, sizeof(float)) != 0) { ++Differ; }
		}
	}

	// 2. Asymmetric arms at vanishing asymmetry (and no tightening) = the symmetric wave.
	double WorstArm = 0.0;
	for (u32 G = 0; G < 12; ++G)
	{
		FFieldParams Pf = ModesOff(Rolled(E, H::Uni_SeedMix(0xA11u, G)));
		const H::FUniFieldParams Sym = Pf.ToHlsl();
		Pf.V[20] = 1.0e-7f; Pf.V[22] = float(G * 977u);
		const H::FUniFieldParams Asy = Pf.ToHlsl();
		for (u32 K = 0; K < 4000; ++K)
		{
			const u32 S = H::Uni_SeedMix(G ^ 0x77u, K);
			const H::float3 Pt(float(1.2 * Unit(S, 0) - 0.6), float(1.2 * Unit(S, 1) - 0.6), float(0.02 * (2.0 * Unit(S, 2) - 1.0)));
			const double A = H::UniField_GalaxyFiltered(Pt, Sym, 0.0f), B = H::UniField_GalaxyFiltered(Pt, Asy, 0.0f);
			WorstArm = std::max(WorstArm, std::abs(A - B) / std::max(A, 1e-6));
		}
	}

	// 3. The warp moves the disc's mid-plane by A rn^2 sin(theta - phase - twist rn): the
	//    density-weighted mean z on rings, against the formula (phase recovered from the fit).
	double WarpCorr = 1.0, WarpGain = 1.0;
	{
		FFieldParams Pf = ModesOff(Rolled(E, 0x5EEDu));
		Pf.V[8] = 0.0f;             // no arms: a clean disc
		Pf.V[5] = 0.0f;             // no bulge
		Pf.V[16] = 0.12f; Pf.V[17] = 0.8f; Pf.V[22] = 12345.0f;
		const H::FUniFieldParams P = Pf.ToHlsl();
		double Sxy = 0, Sxx = 0, Syy = 0;
		for (int Ring = 0; Ring < 4; ++Ring)
		{
			const double Rn = 0.3 + 0.1 * Ring;
			for (int A = 0; A < 64; ++A)
			{
				const double Th = 2.0 * 3.14159265358979 * (A + 0.5) / 64.0;
				double W = 0, Wz = 0;
				for (int Zi = 0; Zi < 400; ++Zi)
				{
					const double Zz = -0.1 + 0.2 * (Zi + 0.5) / 400.0;
					const double Rho = H::UniField_GalaxyFiltered(H::float3(float(Rn * std::cos(Th)), float(Rn * std::sin(Th)), float(Zz)), P, 0.0f);
					W += Rho; Wz += Rho * Zz;
				}
				const double Got = W > 0 ? Wz / W : 0.0;
				// The formula with its seeded phase (the field's own hash).
				const u32 Hp = H::Uni_SeedMix(H::Uni_SeedMix(12345u, H::UNI_CH_DISCMODE), 0u);
				const double PhW = 6.2831853 * double(Hp & 0xFFFFu) / 65536.0;
				const double Want = 0.12 * Rn * Rn * std::sin(Th - PhW - 0.8 * Rn);
				Sxy += Got * Want; Sxx += Want * Want; Syy += Got * Got;
			}
		}
		WarpCorr = Sxy / std::sqrt(std::max(Sxx * Syy, 1e-300));
		WarpGain = Sxy / std::max(Sxx, 1e-300);
	}

	// 4. Lopsidedness and flare keep the disc's light (azimuthal mean 1; surface density kept).
	double LopRatio = 0.0, FlareRatio = 0.0;
	{
		FFieldParams Pf = ModesOff(Rolled(E, 0xD15Cu));
		Pf.V[5] = 0.0f;
		const double Base = Integral(Pf.ToHlsl(), 0.0f, 120 * 120 * 120, 7u);
		FFieldParams Pl = Pf; Pl.V[18] = 0.35f; Pl.V[22] = 4242.0f;
		FFieldParams Pfl = Pf; Pfl.V[19] = 1.2f;
		LopRatio = Integral(Pl.ToHlsl(), 0.0f, 120 * 120 * 120, 7u) / Base;
		FlareRatio = Integral(Pfl.ToHlsl(), 0.0f, 120 * 120 * 120, 7u) / Base;
	}

	// 5. The prefiltered field keeps rolled galaxies' light (all modes on) at a volume footprint.
	//    The same galaxies with the modes off give the prefilter's own error, for comparison.
	double WorstFilter = 0.0, WorstFilterOff = 0.0;
	for (u32 G = 0; G < 6; ++G)
	{
		for (int Off = 0; Off < 2; ++Off)
		{
			FFieldParams Pf = Rolled(E, H::Uni_SeedMix(0xF00Du, G));
			if (Off) { Pf = ModesOff(Pf); }
			const H::FUniFieldParams P = Pf.ToHlsl();
			const double I0 = Integral(P, 0.0f, 110 * 110 * 110, 9u + G);
			const double I1 = Integral(P, 0.03f, 110 * 110 * 110, 9u + G);
			double& W = Off ? WorstFilterOff : WorstFilter;
			W = std::max(W, std::abs(I1 / std::max(I0, 1e-300) - 1.0));
		}
	}

	// 6. Contact sheets: 3 x 3 rolled galaxies at various inclinations, modes off and on (emission
	//    with the dust lanes, D-132's dust field), each tile normalised to its own peak.
	const int Tile = 128, Grid = 3;
	for (int Set = 0; Set < 2; ++Set)
	{
		FImage Img(Tile * Grid, Tile * Grid);
		for (int Gi = 0; Gi < Grid * Grid; ++Gi)
		{
			FFieldParams Pf = Rolled(E, H::Uni_SeedMix(0x9A1Au, u32(Gi)));
			if (Set == 0) { Pf = ModesOff(Pf); }
			const H::FUniFieldParams P = Pf.ToHlsl();
			const H::FUniFieldParams Pd = DustFrom(Pf, Prof).ToHlsl();
			const double Inc = (20.0 + 7.0 * Gi) * 3.14159265358979 / 180.0;
			const double Az = 0.7 * Gi;
			// View direction (towards the galaxy), right and up.
			const double Dv[3] = { std::sin(Inc) * std::cos(Az), std::sin(Inc) * std::sin(Az), -std::cos(Inc) };
			double Rt[3] = { -std::sin(Az), std::cos(Az), 0.0 };
			const double Up[3] = { Dv[1] * Rt[2] - Dv[2] * Rt[1], Dv[2] * Rt[0] - Dv[0] * Rt[2], Dv[0] * Rt[1] - Dv[1] * Rt[0] };
			std::vector<float> L(size_t(Tile) * Tile * 3, 0.0f);
			float Peak = 0.0f;
			const double Half = 0.65, Px = 2.0 * Half / Tile;
			const int Steps = 260;
			const double Len = 2.0, Dt = Len / Steps;
			for (int Y = 0; Y < Tile; ++Y) for (int X = 0; X < Tile; ++X)
			{
				const double U = -Half + (X + 0.5) * Px, V = Half - (Y + 0.5) * Px;
				double O[3];
				for (int Ax = 0; Ax < 3; ++Ax) { O[Ax] = U * Rt[Ax] + V * Up[Ax] - 1.0 * Dv[Ax]; }
				double Col[3] = { 0, 0, 0 }, Tau[3] = { 0, 0, 0 };
				const double Sig[3] = { 0.8, 1.0, 1.3 };
				const float Foot = float(std::max(Px, 0.6 * Dt));
				for (int S = 0; S < Steps; ++S)
				{
					const double T = (S + 0.5) * Dt;
					const H::float3 Pt(float(O[0] + T * Dv[0]), float(O[1] + T * Dv[1]), float(O[2] + T * Dv[2]));
					if (H::dot(Pt, Pt) >= 1.0f) { continue; }
					const double Em = H::UniField_GalaxyFiltered(Pt, P, Foot) * Dt;
					const double Du = Pd.D.y * H::UniField_GalaxyFiltered(Pt, Pd, Foot) * Dt;
					for (int C = 0; C < 3; ++C) { Col[C] += Em * std::exp(-Tau[C]); Tau[C] += Sig[C] * Du; }
				}
				for (int C = 0; C < 3; ++C) { L[(size_t(Y) * Tile + X) * 3 + C] = float(Col[C]); Peak = std::max(Peak, float(Col[C])); }
			}
			const int Ox = (Gi % Grid) * Tile, Oy = (Gi / Grid) * Tile;
			const float Gain = Peak > 0.0f ? 6.0f / Peak : 0.0f;
			for (int Y = 0; Y < Tile; ++Y) for (int X = 0; X < Tile; ++X)
			{
				const float* C = &L[(size_t(Y) * Tile + X) * 3];
				Img.Add(Ox + X, Oy + Y, C[0] * Gain * 0.95f, C[1] * Gain * 0.9f, C[2] * Gain * 0.8f);
			}
		}
		WriteFrame(Img, Set ? "still_galaxy_modes_on" : "still_galaxy_modes_off",
			{ Set ? "D-178: nine rolled galaxies with disc warp, lopsidedness, flare, uneven arms, tightening" : "The same nine galaxies with the D-178 modes off (the D-172 field)",
			  "emission and dust (D-132), 20-76 deg inclination, each tile normalised to its peak" });
	}

	Uni::Check(2, "T2.galaxy.shape", "D-178 galaxy disc modes: with every new parameter at zero the field is the D-172 field bit for bit; asymmetric arms at vanishing asymmetry = the symmetric wave (1e-3); the warp moves the mid-plane by its formula (correlation > 0.98, gain 0.9-1.1); lopsidedness and flare keep the disc's light (1%); the prefiltered field keeps rolled galaxies' light (2%); contact sheets off / on")
		.Metric("points_differ", double(Differ)).Metric("arm_worst_rel", WorstArm).Metric("warp_corr", WarpCorr).Metric("warp_gain", WarpGain)
		.Metric("lopsided_light", LopRatio).Metric("flare_light", FlareRatio).Metric("filter_worst", WorstFilter).Metric("filter_worst_modes_off", WorstFilterOff)
		.Pass(Differ == 0 && Points > 0 && WorstArm < 1e-3 && WarpCorr > 0.98 && WarpGain > 0.9 && WarpGain < 1.1 && std::abs(LopRatio - 1.0) < 0.01 && std::abs(FlareRatio - 1.0) < 0.01 && WorstFilter < 0.02,
			Uni::Fmt("modes off: %zu of %zu points differ from the D-172 field; arms at asymmetry 1e-7 within %.2g of the symmetric wave; warp mid-plane vs formula: correlation %.4f, gain %.3f; light with lopsidedness 0.35: %.4f, flare 1.2: %.4f of the plain disc; prefiltered (footprint 0.03) light within %.2f%% for 6 rolled galaxies (%.2f%% with their modes off)",
				Differ, Points, WorstArm, WarpCorr, WarpGain, LopRatio, FlareRatio, 100.0 * WorstFilter, 100.0 * WorstFilterOff));
}
