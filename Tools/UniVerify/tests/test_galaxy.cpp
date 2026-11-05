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
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>
#include <map>
#include <memory>
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

namespace
{
	/** D-179: the three galaxy textures, loaded once per run (empty, with the reason, if missing). */
	struct FGalTex { FWebTexture T[3]; std::string Err; bool bOk = false; };
	const FGalTex& GalaxyTextures()
	{
		static const FGalTex G = []()
		{
			FGalTex R;
			R.bOk = true;
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), R.T[S], &E)) { R.bOk = false; R.Err += E + "; "; }
			}
			return R;
		}();
		return G;
	}

	struct FGalBind
	{
		explicit FGalBind(const FGalTex& G) { for (int S = 0; S < 3; ++S) { BindGalaxyTexture(S, &G.T[S]); } }
		~FGalBind() { for (int S = 0; S < 3; ++S) { BindGalaxyTexture(S, nullptr); } }
	};

	bool ReadBytes(const std::string& Path, std::vector<uint8_t>& Out)
	{
		FILE* F = std::fopen(Path.c_str(), "rb");
		if (!F) { return false; }
		std::fseek(F, 0, SEEK_END);
		const long N = std::ftell(F);
		std::fseek(F, 0, SEEK_SET);
		Out.resize(size_t(std::max(N, 0L)));
		const bool bOk = N > 0 && std::fread(Out.data(), 1, Out.size(), F) == Out.size();
		std::fclose(F);
		return bOk;
	}
}

UNI_TEST(galaxy, textures)
{
	// D-179: the galaxy textures load (RGBA16F warps, RGBA8 detail as halves of k / 255, full mip
	// chains); a textured galaxy reads NaN with them unbound (fail closed); with them bound the
	// field's light is kept by the prefilter (mips are means) and the warp changes the shape; the
	// CPU cost of a textured galaxy's normalising integral; and a contact sheet.
	const FGalTex& G = GalaxyTextures();
	if (!G.bOk) { Uni::Check(2, "T2.galaxy.textures", "D-179 galaxy textures").Pass(false, "not loaded: " + G.Err); return; }
	// Format: RGBA8 texels are halves of k / 255; RGBA16F texels are the delivered halves.
	size_t Bad = 0, Checked = 0;
	{
		std::vector<uint8_t> Raw;
		if (ReadBytes("Data/Noise/galaxy_detail.bin", Raw) && Raw.size() == size_t(G.T[2].Size) * G.T[2].Size * G.T[2].Size * 4u)
		{
			for (size_t I = 0; I < Raw.size(); I += 4099u)
			{
				++Checked;
				const uint16_t Hh = G.T[2].Mips[0][I];
				if (std::fabs(HalfToFloat(Hh) - float(Raw[I]) / 255.0f) > 2.5e-4f) { ++Bad; }
			}
		}
		else { ++Bad; }
		std::vector<uint8_t> RawW;
		if (ReadBytes("Data/Noise/warp_fine.bin", RawW) && RawW.size() == G.T[0].Mips[0].size() * 2u)
		{
			Checked += 1;
			if (std::memcmp(RawW.data(), G.T[0].Mips[0].data(), RawW.size()) != 0) { ++Bad; }
		}
		else { ++Bad; }
	}
	bool bSizes = G.T[0].Size == G.T[1].Size && G.T[1].Size == G.T[2].Size;
	for (int S = 0; S < 3; ++S) { bSizes = bSizes && G.T[S].NumMips() == 1u + uint32_t(std::log2(double(G.T[S].Size)) + 0.5); }

	FUniverseDef D = UniScenes::Base();
	UniScenes::UseGalaxyTextures(D, G.T[0].Size);
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);

	// Unbound: NaN (fail closed).
	bool bNaN = false;
	{
		const H::FUniFieldParams P = Rolled(E, 0xBEEFu).ToHlsl();
		const float V = H::UniField_GalaxyFiltered(H::float3(0.1f, 0.05f, 0.0f), P, 0.0f);
		bNaN = std::isnan(V);
	}
	FGalBind Bind(G);
	// The prefilter keeps the light, and the warp changes the shape (field correlation between the
	// textured and untextured galaxy in its disc plane).
	double WorstFilter = 0.0, MeanCorr = 0.0, WorstLight = 0.0;
	const int NGal = 6;
	for (u32 Gi = 0; Gi < u32(NGal); ++Gi)
	{
		const FFieldParams Pf = Rolled(E, H::Uni_SeedMix(0x7E47u, Gi));
		FFieldParams Pn = Pf;
		for (int I = 24; I < 31; ++I) { Pn.V[I] = 0.0f; }
		const H::FUniFieldParams P = Pf.ToHlsl(), Pp = Pn.ToHlsl();
		const double I0 = Integral(P, 0.0f, 100 * 100 * 100, 31u + Gi);
		const double I1 = Integral(P, 0.03f, 100 * 100 * 100, 31u + Gi);
		const double In = Integral(Pp, 0.0f, 100 * 100 * 100, 31u + Gi);
		WorstFilter = std::max(WorstFilter, std::abs(I1 / std::max(I0, 1e-300) - 1.0));
		WorstLight = std::max(WorstLight, std::abs(I0 / std::max(In, 1e-300) - 1.0));
		double Sa = 0, Sb = 0, Saa = 0, Sbb = 0, Sab = 0; int N = 0;
		for (int Y = 0; Y < 96; ++Y) for (int X = 0; X < 96; ++X)
		{
			const H::float3 Pt(float(-0.5 + (X + 0.5) / 96.0), float(-0.5 + (Y + 0.5) / 96.0), 0.0f);
			const double A = H::UniField_GalaxyFiltered(Pt, P, 0.0f), B = H::UniField_GalaxyFiltered(Pt, Pp, 0.0f);
			Sa += A; Sb += B; Saa += A * A; Sbb += B * B; Sab += A * B; ++N;
		}
		const double Ca = Saa / N - (Sa / N) * (Sa / N), Cb = Sbb / N - (Sb / N) * (Sb / N);
		MeanCorr += (Sab / N - (Sa / N) * (Sb / N)) / std::sqrt(std::max(Ca * Cb, 1e-300)) / NGal;
	}
	// CPU cost of one textured galaxy's normalising integral (the D-115 estimator, as the
	// streamer's worker jobs compute it).
	double IntegralMs = 0.0;
	{
		FContributor Ct;
		Ct.Emitter = &E;
		Ct.Params = Rolled(E, 0x1234u);
		Ct.InfluenceCm = 1.0;
		const auto T0 = std::chrono::steady_clock::now();
		const double V = FRefGenerator::ComputeIntegral(Ct);
		IntegralMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
		if (!(V > 0.0) || !std::isfinite(V)) { IntegralMs = -1.0; }
	}
	// Stars of the textured Milky Way (galaxy-to-star scene from the solar camera): placed, every
	// one finite, at the untextured scene's count within 10% (the textures move stars, the
	// normalising integral keeps their number).
	size_t StarsTex = 0, StarsPlain = 0, NonFinite = 0;
	for (int Tex = 0; Tex < 2; ++Tex)
	{
		FUniverseDef Dg = UniScenes::GalaxyToStar();
		if (Tex) { UniScenes::UseGalaxyTextures(Dg, G.T[0].Size); }
		FUniverseModel M(Dg);
		FRefGenerator Gn(M);
		FRefPipeline P(Gn);
		P.bVelocityLod = false;
		P.Update(UniScenes::SolarCamera(), FVec3f{});
		for (const auto& KV : P.CacheRef())
		{
			for (const FObjectGPU& O : KV.second.Objects)
			{
				if ((O.A[3] & 0xFFFFu) != UniScenes::kStar) { continue; }
				const float Mg = H::Uni_ObjectAbsMag(ToHlsl(O));
				if (!std::isfinite(Mg)) { ++NonFinite; }
				++(Tex ? StarsTex : StarsPlain);
			}
		}
	}
	const double StarRatio = StarsPlain ? double(StarsTex) / double(StarsPlain) : 0.0;
	// Contact sheet: the nine galaxies of T2.galaxy.shape with the textures on.
	{
		const int Tile = 128, Grid = 3;
		FImage Img(Tile * Grid, Tile * Grid);
		const FVolumeProfile& Prof = D.Arch(UniScenes::kGalaxy).Volume;
		for (int Gi = 0; Gi < Grid * Grid; ++Gi)
		{
			const FFieldParams Pf = Rolled(E, H::Uni_SeedMix(0x9A1Au, u32(Gi)));
			const H::FUniFieldParams P = Pf.ToHlsl();
			const H::FUniFieldParams Pd = DustFrom(Pf, Prof).ToHlsl();
			const double Inc = (20.0 + 7.0 * Gi) * 3.14159265358979 / 180.0, Az = 0.7 * Gi;
			const double Dv[3] = { std::sin(Inc) * std::cos(Az), std::sin(Inc) * std::sin(Az), -std::cos(Inc) };
			const double Rt[3] = { -std::sin(Az), std::cos(Az), 0.0 };
			const double Up[3] = { Dv[1] * Rt[2] - Dv[2] * Rt[1], Dv[2] * Rt[0] - Dv[0] * Rt[2], Dv[0] * Rt[1] - Dv[1] * Rt[0] };
			std::vector<float> L(size_t(Tile) * Tile * 3, 0.0f);
			float Peak = 0.0f;
			const double Half = 0.65, Px = 2.0 * Half / Tile;
			const int Steps = 260;
			const double Dt = 2.0 / Steps;
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
		WriteFrame(Img, "still_galaxy_textured", { "D-179: the nine galaxies of still_galaxy_modes_on with the galaxy textures (warp_fine, warp_large, galaxy_detail)",
			"emission and dust (D-132), each tile normalised to its peak" });
	}
	Uni::Check(2, "T2.galaxy.textures", "D-179 galaxy textures: warp_fine, warp_large (RGBA16F) and galaxy_detail (RGBA8 as halves of k/255) load with full mips; a textured galaxy reads NaN with them unbound (fail closed); bound, the prefiltered field keeps its light (2%), the textures change its total light by under 15% and its shape (disc-plane correlation with the untextured field under 0.97); the normalising integral costs under 3 s on the reference; the textured Milky Way's stars placed, finite, at the untextured count within 10%")
		.Metric("format_bad", double(Bad)).Metric("filter_worst", WorstFilter).Metric("light_change_worst", WorstLight).Metric("shape_corr_mean", MeanCorr).Metric("integral_ms", IntegralMs).Metric("star_ratio", StarRatio)
		.Pass(Bad == 0 && Checked > 100 && bSizes && bNaN && WorstFilter < 0.02 && WorstLight < 0.15 && MeanCorr < 0.97 && IntegralMs > 0.0 && IntegralMs < 3000.0 && StarsTex > 1000 && NonFinite == 0 && std::abs(StarRatio - 1.0) < 0.1,
			Uni::Fmt("%u^3 textures, %u mips each, %zu texels checked against the raw files (%zu wrong); unbound reads %s; 6 rolled galaxies: prefiltered (0.03) light within %.2f%%, light with vs without the textures within %.1f%%, disc-plane correlation with the untextured field %.3f; normalising integral %.0f ms; textured Milky Way from the solar camera: %zu stars (%zu untextured, %.3fx), %zu non-finite",
				G.T[0].Size, G.T[0].NumMips(), Checked, Bad, bNaN ? "NaN" : "a VALUE", 100.0 * WorstFilter, 100.0 * WorstLight, MeanCorr, IntegralMs, StarsTex, StarsPlain, StarRatio, NonFinite));
}

UNI_TEST(galaxy, dust)
{
	// D-180 (render only): a textured galaxy's dust is cloud-like, ridged lanes and superbubble
	// holes from galaxy_detail. Its emission field is untouched (the dust terms live on the dust
	// carrier's parameters only); the dust's face-on column keeps its mean within 25% and gains
	// structure (spread across the disc at least 1.5x the smooth dust's); stills face-on and
	// inclined, with the cloud terms off and on.
	const FGalTex& G = GalaxyTextures();
	if (!G.bOk) { Uni::Check(2, "T2.galaxy.dust", "D-180 cloud dust").Pass(false, "not loaded: " + G.Err); return; }
	FGalBind Bind(G);
	FUniverseDef D = UniScenes::Base();
	UniScenes::UseGalaxyTextures(D, G.T[0].Size);
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
	const FVolumeProfile& Prof = D.Arch(UniScenes::kGalaxy).Volume;
	FVolumeProfile Smooth = Prof; Smooth.DustLanes = 0.0f; Smooth.DustBubbles = 0.0f;
	double MeanRatioWorst = 0.0, SpreadGainMin = 1e9;
	size_t EmissionDiffer = 0, PairChecked = 0, PairDiffer = 0;
	for (u32 Gi = 0; Gi < 4; ++Gi)
	{
		const FFieldParams Pf = Rolled(E, H::Uni_SeedMix(0xD057u, Gi));
		const FFieldParams Pc = DustFrom(Pf, Prof), Ps = DustFrom(Pf, Smooth);
		// The emission parameters carry no dust terms.
		if (Pf.V[14] != 0.0f || Pf.V[15] != 0.0f) { ++EmissionDiffer; }
		const H::FUniFieldParams Hc = Pc.ToHlsl(), Hs = Ps.ToHlsl();
		// D-181: the pair evaluation (the march's) is exactly the two separate ones.
		{
			const H::FUniFieldParams He = Pf.ToHlsl();
			for (u32 K = 0; K < 3000; ++K)
			{
				const u32 Sd = H::Uni_SeedMix(Gi * 131u + 7u, K);
				const H::float3 Pt(float(1.6 * Unit(Sd, 0) - 0.8), float(1.6 * Unit(Sd, 1) - 0.8), float(0.2 * (2.0 * Unit(Sd, 2) - 1.0)));
				const float Foot = (K % 3u) == 0u ? 0.0f : float(0.04 * Unit(Sd, 3));
				float A = 0.0f, B = 0.0f;
				H::UniField_GalaxyPair(Pt, He, Hc, Foot, A, B);
				const float A1 = H::UniField_GalaxyFiltered(Pt, He, Foot), B1 = H::UniField_GalaxyFiltered(Pt, Hc, Foot);
				++PairChecked;
				if (std::memcmp(&A, &A1, 4) != 0 || std::memcmp(&B, &B1, 4) != 0) { ++PairDiffer; }
			}
		}
		double Sc = 0, Sc2 = 0, Ss = 0, Ss2 = 0; int N = 0;
		for (int Y = 0; Y < 64; ++Y) for (int X = 0; X < 64; ++X)
		{
			const double Px = -0.4 + 0.8 * (X + 0.5) / 64.0, Py = -0.4 + 0.8 * (Y + 0.5) / 64.0;
			if (Px * Px + Py * Py > 0.16) { continue; }
			double Cc = 0, Cs = 0;
			for (int Zi = 0; Zi < 120; ++Zi)
			{
				const H::float3 Pt(float(Px), float(Py), float(-0.15 + 0.3 * (Zi + 0.5) / 120.0));
				Cc += H::UniField_GalaxyFiltered(Pt, Hc, 0.0f);
				Cs += H::UniField_GalaxyFiltered(Pt, Hs, 0.0f);
			}
			Sc += Cc; Ss += Cs;
			if (Cs > 0.0) { const double R = Cc / Cs; Sc2 += R; Ss2 += R * R; ++N; }
		}
		// Structure: the spread of the cloud column over the smooth one across the disc (the
		// smooth dust's own radial falloff divides out).
		const double Mr = Sc2 / std::max(N, 1);
		const double Spread = std::sqrt(std::max(Ss2 / std::max(N, 1) - Mr * Mr, 0.0)) / std::max(Mr, 1e-300);
		MeanRatioWorst = std::max(MeanRatioWorst, std::abs(Sc / std::max(Ss, 1e-300) - 1.0));
		SpreadGainMin = std::min(SpreadGainMin, Spread);
	}
	// Stills: two galaxies at 256 px, face-on-ish and inclined, smooth dust then cloud dust.
	{
		const int Tile = 256;
		FImage Img(Tile * 2, Tile * 2);
		for (int Row = 0; Row < 2; ++Row) for (int Col = 0; Col < 2; ++Col)
		{
			const FFieldParams Pf = Rolled(E, H::Uni_SeedMix(0xD057u, 1u));
			const H::FUniFieldParams P = Pf.ToHlsl();
			const H::FUniFieldParams Pd = DustFrom(Pf, Row == 0 ? Smooth : Prof).ToHlsl();
			const double Inc = (Col == 0 ? 25.0 : 68.0) * 3.14159265358979 / 180.0, Az = 0.4;
			const double Dv[3] = { std::sin(Inc) * std::cos(Az), std::sin(Inc) * std::sin(Az), -std::cos(Inc) };
			const double Rt[3] = { -std::sin(Az), std::cos(Az), 0.0 };
			const double Up[3] = { Dv[1] * Rt[2] - Dv[2] * Rt[1], Dv[2] * Rt[0] - Dv[0] * Rt[2], Dv[0] * Rt[1] - Dv[1] * Rt[0] };
			std::vector<float> L(size_t(Tile) * Tile * 3, 0.0f);
			float Peak = 0.0f;
			const double Half = 0.6, Px = 2.0 * Half / Tile;
			const int Steps = 420;
			const double Dt = 2.0 / Steps;
			for (int Y = 0; Y < Tile; ++Y) for (int X = 0; X < Tile; ++X)
			{
				const double U = -Half + (X + 0.5) * Px, V = Half - (Y + 0.5) * Px;
				double O[3];
				for (int Ax = 0; Ax < 3; ++Ax) { O[Ax] = U * Rt[Ax] + V * Up[Ax] - 1.0 * Dv[Ax]; }
				double C3[3] = { 0, 0, 0 }, Tau[3] = { 0, 0, 0 };
				const double Sig[3] = { 0.8, 1.0, 1.3 };
				const float Foot = float(std::max(Px, 0.6 * Dt));
				for (int S = 0; S < Steps; ++S)
				{
					const double T = (S + 0.5) * Dt;
					const H::float3 Pt(float(O[0] + T * Dv[0]), float(O[1] + T * Dv[1]), float(O[2] + T * Dv[2]));
					if (H::dot(Pt, Pt) >= 1.0f) { continue; }
					const double Em = H::UniField_GalaxyFiltered(Pt, P, Foot) * Dt;
					const double Du = Pd.D.y * H::UniField_GalaxyFiltered(Pt, Pd, Foot) * Dt;
					for (int C = 0; C < 3; ++C) { C3[C] += Em * std::exp(-Tau[C]); Tau[C] += Sig[C] * Du; }
				}
				for (int C = 0; C < 3; ++C) { L[(size_t(Y) * Tile + X) * 3 + C] = float(C3[C]); Peak = std::max(Peak, float(C3[C])); }
			}
			const float Gain = Peak > 0.0f ? 6.0f / Peak : 0.0f;
			for (int Y = 0; Y < Tile; ++Y) for (int X = 0; X < Tile; ++X)
			{
				const float* C = &L[(size_t(Y) * Tile + X) * 3];
				Img.Add(Col * Tile + X, Row * Tile + Y, C[0] * Gain * 0.95f, C[1] * Gain * 0.9f, C[2] * Gain * 0.8f);
			}
		}
		WriteFrame(Img, "still_galaxy_dust", { "D-180: top, smooth dust lanes (D-132); bottom, cloud dust from galaxy_detail (ridged lanes, superbubble holes)",
			"one textured galaxy at 25 and 68 deg inclination, each tile normalised to its peak" });
	}
	Uni::Check(2, "T2.galaxy.dust", "D-180 cloud dust (render only), D-181 pair evaluation: emission parameters carry no dust terms; the march's emission + dust pair equals the two separate evaluations bit for bit; a textured galaxy's face-on dust column keeps its mean within 25% of the smooth dust's and the column over the smooth one varies across the disc by at least 25% (rms); stills smooth / cloud")
		.Metric("mean_change_worst", MeanRatioWorst).Metric("cloud_spread_min", SpreadGainMin)
		.Pass(EmissionDiffer == 0 && PairChecked > 0 && PairDiffer == 0 && MeanRatioWorst < 0.25 && SpreadGainMin > 0.25,
			Uni::Fmt("4 rolled galaxies: dust column mean within %.1f%% of the smooth dust's, cloud / smooth column varying by %.0f%% rms at least; %zu emission parameter sets with dust terms; pair vs separate: %zu of %zu samples differ", 100.0 * MeanRatioWorst, 100.0 * SpreadGainMin, EmissionDiffer, PairDiffer, PairChecked));
}

UNI_TEST(galaxy, march_cost)
{
	// Owner, in the engine after D-178/D-179: GPU 63 ms inside a galaxy, the march much dearer.
	// The volume march on the CPU reference (the same core the GPU runs) inside the Milky Way
	// (solar camera, along the disc and out of it) and from 60 kpc, for the field as it was
	// (D-177: no modes, no textures), with the D-178 modes, and with the D-179 textures and
	// D-180 cloud dust: time and steps per ray. Diagnostic: passes when it ran.
	const FGalTex& Gt = GalaxyTextures();
	std::string Detail;
	double Worst = 0.0;
	struct FShot { const char* Name; FVec3d Off; FVec3d Look; };
	const FShot Shots[] = { { "inside, along the disc", { 0, 0, 0 }, { -1, -0.12, -0.02 } }, { "inside, out of plane", { 0, 0, 0 }, { -0.3, 0.1, 1 } }, { "60 kpc", { 0.0, -40.0 * Units::Kpc, 45.0 * Units::Kpc }, { 0.0, 40.0, -45.0 } } };
	for (const FShot& Sh : Shots)
	{
		std::string Parts;
		for (int Cfg = 0; Cfg < 3; ++Cfg)
		{
			if (Cfg == 2 && !Gt.bOk) { Parts += " | textures: not loaded"; continue; }
			FUniverseDef D = UniScenes::GalaxyToStar();
			if (Cfg == 2) { UniScenes::UseGalaxyTextures(D, Gt.T[0].Size); }
			for (FAnchor& A : D.Anchors) { for (FFieldParams& P : A.EmitterParams) { for (int I = Cfg == 0 ? 16 : 24; I < 32 && Cfg < 2; ++I) { P.V[I] = 0.0f; } } }
			std::unique_ptr<FGalBind> Bind;
			if (Cfg == 2) { Bind = std::make_unique<FGalBind>(Gt); }
			FUniverseModel M(D);
			FRefGenerator G(M);
			FVolumeBuilder VB(M, G);
			FRefPipeline P(G);
			P.bVelocityLod = false;
			FCoord Cam = UniScenes::SolarCamera();
			FVec3f Fr{};
			AddDisplacement(Cam, Fr, Sh.Off);
			P.Update(Cam, FVec3f{});
			FCamera Cm; Cm.Coord = Cam; Cm.Width = 242; Cm.Height = 159; Cm.DeltaMag = 0.0;
			Cm.LookAlong({ Sh.Look.X, Sh.Look.Y, Sh.Look.Z });
			const FVolumeScene S = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
			FImage Img(Cm.Width, Cm.Height);
			const auto T0 = std::chrono::steady_clock::now();
			const FVolumeStats St = RenderVolume(S, Cm, Img, nullptr, 1);
			const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
			Worst = std::max(Worst, Ms);
			const double PerRay = St.RaysWithCarrier ? double(St.Steps) / double(St.RaysWithCarrier) : 0.0;
			Parts += Uni::Fmt("%s%s %.0f ms, %.1f steps/ray", Parts.empty() ? "" : " | ", Cfg == 0 ? "D-177" : (Cfg == 1 ? "+modes" : "+textures, cloud dust"), Ms, PerRay);
		}
		Detail += Uni::Fmt("%s[%s] %s", Detail.empty() ? "" : "; ", Sh.Name, Parts.c_str());
	}
	Uni::Check(2, "T2.galaxy.march_cost", "Diagnostic (D-178..D-180): the volume march inside the Milky Way and from 60 kpc on the CPU reference, for the D-177 field, with the disc modes, and with the textures and cloud dust (242 x 159, one thread)")
		.Metric("ms_worst", Worst).Pass(Worst > 0.0, Detail);
}

namespace
{
	/** Hue (degrees) and saturation of a colour. */
	void HueSat(const H::float3& C, double& OutH, double& OutS)
	{
		const double Mx = std::max(C.x, std::max(C.y, C.z)), Mn = std::min(C.x, std::min(C.y, C.z));
		OutS = Mx > 0 ? (Mx - Mn) / Mx : 0.0;
		double Hh = 0.0;
		if (Mx > Mn)
		{
			if (Mx == C.x) { Hh = 60.0 * std::fmod((C.y - C.z) / (Mx - Mn), 6.0); }
			else if (Mx == C.y) { Hh = 60.0 * ((C.z - C.x) / (Mx - Mn) + 2.0); }
			else { Hh = 60.0 * ((C.x - C.y) / (Mx - Mn) + 4.0); }
		}
		OutH = Hh < 0 ? Hh + 360.0 : Hh;
	}
	bool HueIn(double Hh, double Lo, double Hi) { const double A = std::fmod(Hh - Lo + 720.0, 360.0); return A <= std::fmod(Hi - Lo + 720.0, 360.0) + 0.5; }
}

UNI_TEST(galaxy, palette)
{
	// D-182 (owner: every galaxy its own palette, plausible but stylised; its stars take it too).
	// The palette: four colours of luminance 1 in their families, galaxies distinct. Stars of the
	// Milky Way with palettes: every position, seed, magnitude and radius as without, colours
	// tinted. The volume inside it: the same luminance (chroma only), visibly coloured. Stills.
	size_t BadLum = 0, BadFamily = 0, N = 0;
	double MeanDist = 0.0;
	H::float3 Prev(0, 0, 0);
	for (u32 I = 0; I < 2000; ++I)
	{
		const H::FUniGalaxyPalette P = H::Uni_GalaxyPalette(H::Uni_PaletteSeed(H::Uni_SeedMix(0x9A7u, I)));
		const H::float3 Cs[4] = { P.Bulge, P.Disc, P.Arm, P.Knot };
		for (const H::float3& C : Cs) { if (std::abs(H::Uni_PaletteLum(C) - 1.0f) > 1e-4f) { ++BadLum; } }
		double Hh, Ss;
		HueSat(P.Bulge, Hh, Ss); if (!HueIn(Hh, 27.0, 53.0)) { ++BadFamily; }
		HueSat(P.Arm, Hh, Ss); if (!HueIn(Hh, 159.0, 276.0)) { ++BadFamily; }
		HueSat(P.Knot, Hh, Ss); if (!HueIn(Hh, 304.0, 21.0)) { ++BadFamily; }
		HueSat(P.Disc, Hh, Ss); if (!(HueIn(Hh, 184.0, 226.0) || HueIn(Hh, 34.0, 56.0) || HueIn(Hh, 249.0, 281.0)) && Ss > 0.02) { ++BadFamily; }
		const H::float3 F = H::Uni_GalaxyFarTint(P);
		if (I > 0) { MeanDist += std::sqrt(double((F.x - Prev.x) * (F.x - Prev.x) + (F.y - Prev.y) * (F.y - Prev.y) + (F.z - Prev.z) * (F.z - Prev.z))) / 1999.0; }
		Prev = F;
		++N;
	}
	// Stars of the Milky Way with and without palettes.
	size_t Stars = 0, OtherDiffer = 0, Tinted = 0, Missing = 0;
	{
		std::map<std::string, std::vector<FObjectGPU>> Cells[2];
		for (int Pal = 0; Pal < 2; ++Pal)
		{
			FUniverseDef D = UniScenes::GalaxyToStar();
			if (Pal) { UniScenes::UseGalaxyPalettes(D); }
			FUniverseModel M(D);
			FRefGenerator G(M);
			FRefPipeline P(G);
			P.bVelocityLod = false;
			P.Update(UniScenes::SolarCamera(), FVec3f{});
			for (const auto& KV : P.CacheRef())
			{
				const std::string Key = std::to_string(KV.first.Level) + ToHex(KV.first.C[0]) + ToHex(KV.first.C[1]) + ToHex(KV.first.C[2]);
				Cells[Pal][Key] = KV.second.Objects;
			}
		}
		for (const auto& KV : Cells[0])
		{
			auto It = Cells[1].find(KV.first);
			if (It == Cells[1].end() || It->second.size() != KV.second.size()) { ++Missing; continue; }
			for (size_t I = 0; I < KV.second.size(); ++I)
			{
				const FObjectGPU& A = KV.second[I];
				const FObjectGPU& B = It->second[I];
				if ((A.A[3] & 0xFFFFu) != UniScenes::kStar) { continue; }
				++Stars;
				if (A.A[0] != B.A[0] || A.A[1] != B.A[1] || A.A[2] != B.A[2] || A.A[3] != B.A[3] || A.B[0] != B.B[0] || A.B[1] != B.B[1] || A.B[2] != B.B[2]) { ++OtherDiffer; }
				if (A.B[3] != B.B[3]) { ++Tinted; }
			}
		}
	}
	// The volume inside the Milky Way: luminance kept, colour changed.
	double LumRatio = 0.0, ChromaShift = 0.0;
	{
		double Lum[2] = { 0, 0 }; std::vector<float> Img[2];
		for (int Pal = 0; Pal < 2; ++Pal)
		{
			FUniverseDef D = UniScenes::GalaxyToStar();
			if (Pal) { UniScenes::UseGalaxyPalettes(D); }
			FUniverseModel M(D);
			FRefGenerator G(M);
			FVolumeBuilder VB(M, G);
			FRefPipeline P(G);
			P.bVelocityLod = false;
			const FCoord Cam = UniScenes::SolarCamera();
			P.Update(Cam, FVec3f{});
			FCamera Cm; Cm.Coord = Cam; Cm.Width = 160; Cm.Height = 100; Cm.DeltaMag = 0.0;
			Cm.LookAlong({ -1, -0.12, -0.02 });
			const FVolumeScene S = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
			FImage Im(Cm.Width, Cm.Height);
			RenderVolume(S, Cm, Im, nullptr, 2);
			Img[Pal] = Im.Rgb;
			for (size_t I = 0; I < Im.Rgb.size(); I += 3) { Lum[Pal] += 0.2126 * Im.Rgb[I] + 0.7152 * Im.Rgb[I + 1] + 0.0722 * Im.Rgb[I + 2]; }
		}
		LumRatio = Lum[1] / std::max(Lum[0], 1e-300);
		double Sh = 0.0, W = 0.0;
		for (size_t I = 0; I < Img[0].size(); I += 3)
		{
			const double Y0 = 0.2126 * Img[0][I] + 0.7152 * Img[0][I + 1] + 0.0722 * Img[0][I + 2];
			const double Y1 = 0.2126 * Img[1][I] + 0.7152 * Img[1][I + 1] + 0.0722 * Img[1][I + 2];
			if (Y0 <= 0 || Y1 <= 0) { continue; }
			double D2 = 0;
			for (int C = 0; C < 3; ++C) { const double Dd = Img[1][I + C] / Y1 - Img[0][I + C] / Y0; D2 += Dd * Dd; }
			Sh += std::sqrt(D2) * Y0; W += Y0;
		}
		ChromaShift = W > 0 ? Sh / W : 0.0;
	}
	// Stills: nine textured, paletted galaxies (emission x tint, dust), against a 5500 K glow.
	const FGalTex& Gt = GalaxyTextures();
	if (Gt.bOk)
	{
		FGalBind Bind(Gt);
		FUniverseDef D = UniScenes::Base();
		UniScenes::UseGalaxyTextures(D, Gt.T[0].Size);
		UniScenes::UseGalaxyPalettes(D);
		const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
		const FVolumeProfile& Prof = D.Arch(UniScenes::kGalaxy).Volume;
		const H::float3 Base = H::Uni_TemperatureColour(5500.0f);
		const int Tile = 160, Grid = 3;
		FImage Img(Tile * Grid, Tile * Grid);
		for (int Gi = 0; Gi < Grid * Grid; ++Gi)
		{
			FFieldParams Pf = Rolled(E, H::Uni_SeedMix(0x9A1Au, u32(Gi)));
			Pf.V[23] = float(H::Uni_PaletteSeed(H::Uni_SeedMix(0x9A1Au, u32(Gi))));
			const H::FUniFieldParams P = Pf.ToHlsl();
			const H::FUniFieldParams Pd = DustFrom(Pf, Prof).ToHlsl();
			const H::FUniGalaxyPalette Pal = H::Uni_GalaxyPalette(u32(Pf.V[23]));
			const double Inc = (20.0 + 7.0 * Gi) * 3.14159265358979 / 180.0, Az = 0.7 * Gi;
			const double Dv[3] = { std::sin(Inc) * std::cos(Az), std::sin(Inc) * std::sin(Az), -std::cos(Inc) };
			const double Rt[3] = { -std::sin(Az), std::cos(Az), 0.0 };
			const double Up[3] = { Dv[1] * Rt[2] - Dv[2] * Rt[1], Dv[2] * Rt[0] - Dv[0] * Rt[2], Dv[0] * Rt[1] - Dv[1] * Rt[0] };
			std::vector<float> L(size_t(Tile) * Tile * 3, 0.0f);
			float Peak = 0.0f;
			const double Half = 0.65, Px = 2.0 * Half / Tile;
			const int Steps = 280;
			const double Dt = 2.0 / Steps;
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
					float Re = 0.0f, Rd = 0.0f;
					H::float4 Comp(0, 0, 0, 0);
					H::UniField_GalaxyPairC(Pt, P, Pd, Foot, Re, Rd, Comp);
					const H::float3 Lc = H::Uni_TintKeepLuminance(Base, H::Uni_GalaxyTint(Pal, Comp, 0.0f));
					const double Em = double(Re) * Dt, Du = double(Pd.D.y) * double(Rd) * Dt;
					const double Lv[3] = { Lc.x, Lc.y, Lc.z };
					for (int C = 0; C < 3; ++C) { Col[C] += Em * Lv[C] * std::exp(-Tau[C]); Tau[C] += Sig[C] * Du; }
				}
				for (int C = 0; C < 3; ++C) { L[(size_t(Y) * Tile + X) * 3 + C] = float(Col[C]); Peak = std::max(Peak, float(Col[C])); }
			}
			const int Ox = (Gi % Grid) * Tile, Oy = (Gi / Grid) * Tile;
			const float Gain = Peak > 0.0f ? 6.0f / Peak : 0.0f;
			for (int Y = 0; Y < Tile; ++Y) for (int X = 0; X < Tile; ++X)
			{
				const float* C = &L[(size_t(Y) * Tile + X) * 3];
				Img.Add(Ox + X, Oy + Y, C[0] * Gain, C[1] * Gain, C[2] * Gain);
			}
		}
		WriteFrame(Img, "still_galaxy_palettes", { "D-182: nine textured galaxies, each its own palette (bulge, disc, arms, knots)", "emission x palette tint over a 5500 K glow, dust; each tile normalised to its peak" });
	}
	Uni::Check(2, "T2.galaxy.palette", "D-182 galaxy palettes: 2,000 palettes with every colour at luminance 1 and in its family (gold bulges; teal, warm or violet discs; blue-violet or teal arms; pink or red-orange knots), consecutive galaxies distinct; the Milky Way's stars with palettes keep every position, seed, magnitude and radius and are tinted (most of them); the volume inside it keeps its luminance (1%) and changes colour")
		.Metric("bad_luminance", double(BadLum)).Metric("bad_family", double(BadFamily)).Metric("far_tint_distance", MeanDist).Metric("stars", double(Stars)).Metric("tinted", double(Tinted)).Metric("lum_ratio", LumRatio).Metric("chroma_shift", ChromaShift)
		.Pass(BadLum == 0 && BadFamily == 0 && MeanDist > 0.05 && Stars > 1000 && OtherDiffer == 0 && Missing == 0 && Tinted * 2 > Stars && std::abs(LumRatio - 1.0) < 0.01 && ChromaShift > 0.03,
			Uni::Fmt("%zu palettes: %zu colours off luminance 1, %zu out of family; consecutive whole-body tints %.3f apart on average; %zu Milky Way stars, %zu tinted, %zu differ in anything else, %zu cells missing; volume luminance %.4fx, chroma shift %.3f",
				N, BadLum, BadFamily, MeanDist, Stars, Tinted, OtherDiffer, Missing, LumRatio, ChromaShift));
}

namespace
{
	/** Second moments of an image's luminance about its centroid: (xx, xy, yy) and the total. */
	void Moments(const FImage& Im, double& OutSum, double& OutXx, double& OutXy, double& OutYy)
	{
		double S = 0, Sx = 0, Sy = 0;
		for (int Y = 0; Y < Im.H; ++Y) for (int X = 0; X < Im.W; ++X)
		{
			const float* P = &Im.Rgb[(size_t(Y) * Im.W + X) * 3];
			const double L = 0.2126 * P[0] + 0.7152 * P[1] + 0.0722 * P[2];
			S += L; Sx += L * (X + 0.5); Sy += L * (Y + 0.5);
		}
		const double Cx = Sx / S, Cy = Sy / S;
		double Xx = 0, Xy = 0, Yy = 0;
		for (int Y = 0; Y < Im.H; ++Y) for (int X = 0; X < Im.W; ++X)
		{
			const float* P = &Im.Rgb[(size_t(Y) * Im.W + X) * 3];
			const double L = 0.2126 * P[0] + 0.7152 * P[1] + 0.0722 * P[2];
			const double Dx = X + 0.5 - Cx, Dy = Y + 0.5 - Cy;
			Xx += L * Dx * Dx; Xy += L * Dx * Dy; Yy += L * Dy * Dy;
		}
		OutSum = S; OutXx = Xx / S; OutXy = Xy / S; OutYy = Yy / S;
	}
	/** Axis ratio (minor / major sigma) and major-axis angle (rad, mod pi) of a 2 x 2 covariance. */
	void Ellipse(double Xx, double Xy, double Yy, double& OutQ, double& OutAngle)
	{
		const double T = 0.5 * (Xx + Yy), D = std::sqrt(std::max(0.0, 0.25 * (Xx - Yy) * (Xx - Yy) + Xy * Xy));
		OutQ = std::sqrt(std::max(0.0, T - D) / std::max(T + D, 1e-300));
		OutAngle = 0.5 * std::atan2(2.0 * Xy, Xx - Yy);
	}
	double AngleDiffPi(double A, double B) { double D = std::fmod(std::abs(A - B), 3.141592653589793); return std::min(D, 3.141592653589793 - D); }
	FCoord MovedBy(const FCoord& C, const FV3& D) { FCoord R = C; FVec3f F{}; AddDisplacement(R, F, FVec3d{ D.X, D.Y, D.Z }); return R; }
}

UNI_TEST(galaxy, sprites)
{
	// D-183 (plan step B): a disc galaxy's sprite is an ellipse at its disc's inclination. The
	// disc normal the projection derives from the object seed must be the one its contributor
	// marches (RandomRotation's third row; world z for a fixed-orientation anchor). The splatted
	// sprite's second moments must be those of the projected oblate Gaussian of axis ratio q0,
	// sqrt(cos^2 i + q0^2 sin^2 i) along the projected normal (each galaxy looked at head on);
	// its energy unchanged; small sprites round; at the hand-off it matches the field's own projected
	// light (axis ratio, mean error under 0.08 over 24 galaxies); everything but the two new fields bit-identical
	// to the mask off; stars never elliptical.
	FUniverseModel M(UniScenes::WebToGalaxy());
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::VoidCamera();
	P.Update(Cam, FVec3f{});
	u32 DiscMask = 0;
	for (const FArchetype& A : M.Definition().Archetypes)
	{
		for (const FUniEmitter& E : A.Emitters) { if (E.FieldId == H::UNI_FIELD_GALAXY && A.Id < 32u) { DiscMask |= 1u << A.Id; } }
	}
	u32 Fixed[4] = { 0, 0, 0, 0 };
	{
		int N = 0;
		for (const FAnchor& An : M.Definition().Anchors) { if (An.bFixedOrientation && N < 4) { Fixed[N++] = An.Seed; } }
	}
	size_t Galaxies = 0, NormalBad = 0, Projected = 0, Round = 0, OtherDiffer = 0, ShapeN = 0, StarsElliptic = 0, Stars = 0;
	double WorstNormal = 0, WorstQ = 0, WorstAngle = 0, WorstSplatQ = 0, WorstSplatAngle = 0, WorstEnergy = 0, WorstSmallQ = 1.0;
	double FieldErr = 0, FieldWorst = 0;
	int FieldN = 0;
	const int Sheet = 8, Tile = 96;
	FImage Contact(Sheet * Tile, 4 * Tile);
	int Drawn = 0;
	for (const auto& KV : P.Contributors())
	{
		const FContributor& Ct = KV.second;
		if (Ct.Emitter == nullptr || Ct.Emitter->FieldId != H::UNI_FIELD_GALAXY || Galaxies >= 160) { continue; }
		auto It = P.CacheRef().find(Ct.Parent.Cell);
		if (It == P.CacheRef().end() || Ct.Parent.Index >= It->second.Objects.size()) { continue; }
		const FObjectGPU& O = It->second.Objects[Ct.Parent.Index];
		++Galaxies;
		// 1. The normal: the projection's against the contributor's.
		const bool bFixed = O.B[0] == Fixed[0] || O.B[0] == Fixed[1] || O.B[0] == Fixed[2] || O.B[0] == Fixed[3];
		const H::float3 Ng = bFixed ? H::float3(0, 0, 1) : H::Uni_DiscNormal(O.B[0]);
		const double Nd = std::sqrt(double((Ng.x - Ct.Rot[6]) * (Ng.x - Ct.Rot[6]) + (Ng.y - Ct.Rot[7]) * (Ng.y - Ct.Rot[7]) + (Ng.z - Ct.Rot[8]) * (Ng.z - Ct.Rot[8])));
		WorstNormal = std::max(WorstNormal, Nd);
		if (Nd > 1e-4) { ++NormalBad; }
		// 2. Projection looking straight at it from a random direction at 300 radii (resolved,
		//    before the volume takes it over), mask on and off.
		const u32 Rs = H::Uni_SeedMix(0xD15Cu, u32(Galaxies));
		const double Uz = 2.0 * H::Uni_DrawUnit(Rs, 1u, 0u) - 1.0, Ph = 6.283185307179586 * H::Uni_DrawUnit(Rs, 1u, 1u);
		const FV3 Dir{ std::sqrt(1.0 - Uz * Uz) * std::cos(Ph), std::sqrt(1.0 - Uz * Uz) * std::sin(Ph), Uz };
		const double Radius = std::exp2(double(H::Uni_ObjectRadiusLog2(ToHlsl(O))));
		const FCoord CamG = MovedBy(Ct.Centre, Dir * (-300.0 * Radius));
		H::uint4 D[3];
		CellDelta(Ct.Parent.Cell, CamG, D);
		FCamera Cm; Cm.Coord = CamG; Cm.DeltaMag = 8.0; Cm.Width = 512; Cm.Height = 512;
		Cm.DiscArchetypeMask = DiscMask;
		for (int K = 0; K < 4; ++K) { Cm.FixedDiscSeeds[K] = Fixed[K]; }
		Cm.LookAlong(Dir);
		const H::FUniSprite S = H::Uni_ProjectObject(D[0], D[1], D[2], u32(Ct.Parent.Cell.Level), 1.0f, ToHlsl(O), Cm.View(), false);
		FCamera Co = Cm; Co.DiscArchetypeMask = 0;
		H::FUniSprite Sr = H::Uni_ProjectObject(D[0], D[1], D[2], u32(Ct.Parent.Cell.Level), 1.0f, ToHlsl(O), Co.View(), false);
		if (!(S.Flags & H::UNI_SPRITE_VISIBLE) || !(S.Flags & H::UNI_SPRITE_EXTENDED)) { continue; }
		++Projected;
		if (Sr.EllipseAxis != 0.0f) { ++Round; }
		H::FUniSprite Sc = S; Sc.EllipseAngle = 0.0f; Sc.EllipseAxis = 0.0f;
		if (std::memcmp(&Sc, &Sr, sizeof(Sc)) != 0) { ++OtherDiffer; }
		// Expected: the projected covariance I - (1 - q0^2) n n^T onto the quad basis, and in pixels.
		const bool bEarly = ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_EARLY) != 0u;
		const double Q0 = bEarly ? 0.65 : 0.2;
		const FV3 N{ Ct.Rot[6], Ct.Rot[7], Ct.Rot[8] };
		auto CovAB = [&](FV3 A, FV3 B) { return Dot(A, B) - (1.0 - Q0 * Q0) * Dot(A, N) * Dot(B, N); };
		const FV3 Side = Norm(Cross(Dir, Cm.Up + Cm.Right * 1.0e-3)), Upq = Cross(Side, Dir);
		double Qe, Ae;
		Ellipse(CovAB(Side, Side), CovAB(Side, Upq), CovAB(Upq, Upq), Qe, Ae);
		WorstQ = std::max(WorstQ, std::abs(double(S.EllipseAxis) - Qe));
		if (Qe < 0.95) { WorstAngle = std::max(WorstAngle, AngleDiffPi(double(S.EllipseAngle) + 1.5707963267948966, Ae)); }   // the sprite angle is the minor axis
		// 3. Splat it at a 16 px sigma (no halo): energy and pixel moments against the projected disc.
		H::FUniSprite Big = S; Big.HaloRatio = 0.0f; Big.HaloRadius = 0.0f; Big.Flags &= ~H::UNI_SPRITE_BODY;
		Big.AngularRadius = float(16.0 / (Cm.FocalPx() * 0.5));
		Big.NdcX = 0.0f; Big.NdcY = 0.0f;
		FImage Im(Cm.Width, Cm.Height);
		Splat(Im, Cm, Big);
		double Sum, Xx, Xy, Yy;
		Moments(Im, Sum, Xx, Xy, Yy);
		const double Lum = 0.2126 * Big.Radiance.x + 0.7152 * Big.Radiance.y + 0.0722 * Big.Radiance.z;
		WorstEnergy = std::max(WorstEnergy, std::abs(Sum / Lum - 1.0));
		const FV3 Px{ Cm.Right }, Py{ Cm.Up * -1.0 };
		double Qp, Ap, Qm, Am;
		Ellipse(CovAB(Px, Px), CovAB(Px, Py), CovAB(Py, Py), Qp, Ap);
		Ellipse(Xx - 1.0 / 12.0, Xy, Yy - 1.0 / 12.0, Qm, Am);   // less the pixels' own variance
		WorstSplatQ = std::max(WorstSplatQ, std::abs(Qm - Qp));
		if (Qp < 0.9) { WorstSplatAngle = std::max(WorstSplatAngle, AngleDiffPi(Am, Ap)); }
		++ShapeN;
		// Hand-off (diagnostic): the field's own light-weighted projected axis ratio along this view.
		if (FieldN < 24)
		{
			const H::FUniFieldParams Pp = Ct.Params.ToHlsl();
			double Sw = 0, C3[3][3] = {};
			for (u32 K = 0; K < 60000; ++K)
			{
				const u32 Sd = H::Uni_SeedMix(0x0D15Cu, K);
				const H::float3 Pt(float(2 * Unit(Sd, 0) - 1), float(2 * Unit(Sd, 1) - 1), float(2 * Unit(Sd, 2) - 1));
				if (H::dot(Pt, Pt) >= 1.0f) { continue; }
				const double W = H::UniField_GalaxyFiltered(Pt, Pp, 0.0f);
				const double X[3] = { Pt.x, Pt.y, Pt.z };
				Sw += W;
				for (int A = 0; A < 3; ++A) for (int B = 0; B < 3; ++B) { C3[A][B] += W * X[A] * X[B]; }
			}
			// The view in field space: Rot maps world to field.
			const FV3 Df{ Ct.Rot[0] * Dir.X + Ct.Rot[1] * Dir.Y + Ct.Rot[2] * Dir.Z, Ct.Rot[3] * Dir.X + Ct.Rot[4] * Dir.Y + Ct.Rot[5] * Dir.Z, Ct.Rot[6] * Dir.X + Ct.Rot[7] * Dir.Y + Ct.Rot[8] * Dir.Z };
			const FV3 E1 = Norm(Cross(Df, std::fabs(Df.Z) < 0.9 ? FV3{ 0, 0, 1 } : FV3{ 1, 0, 0 })), E2 = Cross(Df, E1);
			auto Cv = [&](FV3 A, FV3 B) { const double Av[3] = { A.X, A.Y, A.Z }, Bv[3] = { B.X, B.Y, B.Z }; double R = 0; for (int I = 0; I < 3; ++I) for (int J = 0; J < 3; ++J) { R += Av[I] * C3[I][J] * Bv[J]; } return R / Sw; };
			double Qf, Af;
			Ellipse(Cv(E1, E1), Cv(E1, E2), Cv(E2, E2), Qf, Af);
			FieldErr += std::abs(Qf - double(S.EllipseAxis)) / 24.0;
			FieldWorst = std::max(FieldWorst, std::abs(Qf - double(S.EllipseAxis)));
			++FieldN;
		}
		// 4. Small: 0.8 px sigma stays near round (minor axis at least ~0.7 px).
		H::FUniSprite Small = Big; Small.AngularRadius = float(0.8 / (Cm.FocalPx() * 0.5));
		FImage Is(64, 64);
		FCamera Cs = Cm; Cs.Width = 64; Cs.Height = 64;
		Small.AngularRadius = float(0.8 / (Cs.FocalPx() * 0.5));
		Splat(Is, Cs, Small);
		double S2, Xx2, Xy2, Yy2, Qs, As;
		Moments(Is, S2, Xx2, Xy2, Yy2);
		Ellipse(Xx2, Xy2, Yy2, Qs, As);
		WorstSmallQ = std::min(WorstSmallQ, Qs);
		WorstEnergy = std::max(WorstEnergy, std::abs(S2 / Lum - 1.0));
		// Contact sheet: the first 32, each at 16 px sigma, normalised to its peak.
		if (Drawn < Sheet * 4)
		{
			float Peak = 0.0f;
			for (float V : Im.Rgb) { Peak = std::max(Peak, V); }
			const int Ox = (Drawn % Sheet) * Tile, Oy = (Drawn / Sheet) * Tile;
			for (int Y = 0; Y < Tile; ++Y) for (int X = 0; X < Tile; ++X)
			{
				const int Sx = X + Im.W / 2 - Tile / 2, Sy = Y + Im.H / 2 - Tile / 2;
				const float* C = &Im.Rgb[(size_t(Sy) * Im.W + Sx) * 3];
				Contact.Add(Ox + X, Oy + Y, C[0] * 3.0f / Peak, C[1] * 3.0f / Peak, C[2] * 3.0f / Peak);
			}
			++Drawn;
		}
	}
	// Stars: never elliptical (their archetype has no galaxy-field emitter).
	{
		FCamera Cm; Cm.Coord = Cam; Cm.DeltaMag = 8.0; Cm.DiscArchetypeMask = DiscMask;
		const H::FUniView V = Cm.View();
		for (const auto& KV : P.CacheRef())
		{
			for (const FObjectGPU& O : KV.second.Objects)
			{
				const u32 Arch = O.A[3] & 0xFFFFu;
				if (Arch < 32u && ((DiscMask >> Arch) & 1u)) { continue; }
				H::uint4 D[3];
				CellDelta(KV.first, Cam, D);
				const H::FUniSprite S = H::Uni_ProjectObject(D[0], D[1], D[2], u32(KV.first.Level), 1.0f, ToHlsl(O), V, false);
				++Stars;
				if (S.EllipseAxis != 0.0f) { ++StarsElliptic; }
			}
		}
	}
	WriteFrame(Contact, "still_galaxy_sprites", { "D-183: 32 field galaxies' sprites at 16 px sigma, each looked at head on", "inclined ellipses from their discs' normals (q0 0.2, early types 0.65); each tile normalised to its peak" });
	Uni::Check(2, "T2.galaxy.sprites", "D-183 inclined galaxy sprites: the projection's disc normal is the contributor's (1e-4); the sprite's axis ratio and angle are the projected oblate disc's (1e-3, 1e-3 rad); splatted at 16 px its second moments match (axis ratio 0.03, angle 0.03 rad) and its energy is unchanged (1e-3); at 0.8 px it stays round (ratio > 0.75); against the field's own projected light the axis ratio is within 0.08 on average; with the mask off every field but the two new ones is bit-identical and the sprite round; no other object is ever elliptical")
		.Metric("galaxies", double(Galaxies)).Metric("projected", double(Projected)).Metric("worst_normal", WorstNormal).Metric("worst_q", WorstQ).Metric("worst_angle", WorstAngle)
		.Metric("worst_splat_q", WorstSplatQ).Metric("worst_splat_angle", WorstSplatAngle).Metric("worst_energy", WorstEnergy).Metric("small_min_q", WorstSmallQ).Metric("stars", double(Stars)).Metric("field_q_mean_err", FieldErr).Metric("field_q_worst_err", FieldWorst)
		.Pass(Galaxies >= 40 && Projected >= 20 && NormalBad == 0 && WorstQ < 1e-3 && WorstAngle < 1e-3 && WorstSplatQ < 0.03 && WorstSplatAngle < 0.03 && WorstEnergy < 1e-3
				&& WorstSmallQ > 0.75 && FieldN == 24 && FieldErr < 0.08 && Round == 0 && OtherDiffer == 0 && StarsElliptic == 0 && Stars > 1000,
			Uni::Fmt("%zu galaxies (%zu projected, %zu splatted): normal worst %.2g (%zu off); axis ratio worst %.2g, angle %.2g rad; splat ratio %.3f, angle %.3f rad, energy %.2g; 0.8 px sprite min ratio %.2f; mask off: %zu not round, %zu differ elsewhere; %zu other objects, %zu elliptical; hand-off: the field's own projected light vs the sprite, axis ratio mean error %.3f, worst %.3f (%d galaxies)",
				Galaxies, Projected, ShapeN, WorstNormal, NormalBad, WorstQ, WorstAngle, WorstSplatQ, WorstSplatAngle, WorstEnergy, WorstSmallQ, Round, OtherDiffer, Stars, StarsElliptic, FieldErr, FieldWorst, FieldN));
}
