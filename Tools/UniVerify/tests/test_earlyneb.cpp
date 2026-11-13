// test_earlyneb.cpp
// D-236 (owner, 2026-11-12: "more variance in possible nebula colors and a pass to make the
// globular galaxies more interesting - they could also get nebulas, potentially different
// colors, etc"):
//  - early types' nebula filaments: only textured early types roll them (spirals, dwarfs, the home
//    galaxy and untextured scenes are unchanged); the shape's mean constant is the sampled one;
//    their light is a few to tens of percent of the stars' and holds as the pattern blends to its
//    mean with distance; the march's sphere cap samples them (no slide of their own as the steps
//    move, as the gas layer's for spirals);
//  - tidal shells: only early types roll them; the stars' slide is the shell-less spheroid's;
//  - nebula colours: three a galaxy at luminance 1, from wider families (spiral and early sets);
//  - stills: nine ellipticals with filaments and shells (whole and the cores), and the colours.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FEnTex
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FEnTex()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};

	struct FEnBind
	{
		explicit FEnBind(const FEnTex& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FEnBind() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	std::string EnRenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	void EnWriteFrame(const FImage& Img, const std::string& Name, const std::vector<std::string>& Hud)
	{
		const std::string Base = EnRenderDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb");
		if (!F) { return; }
		std::fprintf(F, "{\"lines\":[");
		for (size_t I = 0; I < Hud.size(); ++I) { std::fprintf(F, "%s\"%s\"", I ? "," : "", Hud[I].c_str()); }
		std::fprintf(F, "]}\n");
		std::fclose(F);
	}

	FFieldParams EnRoll(const FUniEmitter& E, u32 S, bool bEarly)
	{
		FFieldParams P = bEarly ? E.EarlyExemplar : E.Exemplar;
		for (const FParamRange& R : bEarly ? E.EarlyRanges : E.Ranges) { P.V[R.Slot] = H::Uni_RollRange(S, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
		return P;
	}

	double EnUnit(u32 S, u32 K) { return double(H::Uni_DrawUnit(S, 0xE36u, K)); }

	struct FEnView
	{
		double Dv[3], Rt[3], Up[3];
		FEnView(double Inc, double Az)
		{
			Dv[0] = std::sin(Inc) * std::cos(Az); Dv[1] = std::sin(Inc) * std::sin(Az); Dv[2] = -std::cos(Inc);
			Rt[0] = -std::sin(Az); Rt[1] = std::cos(Az); Rt[2] = 0.0;
			Up[0] = Dv[1] * Rt[2] - Dv[2] * Rt[1]; Up[1] = Dv[2] * Rt[0] - Dv[0] * Rt[2]; Up[2] = Dv[0] * Rt[1] - Dv[1] * Rt[0];
		}
	};

	struct FEnRay { double Stars = 0.0, Neb = 0.0, NebM = 0.0, Src = 0.0; double Col[3] = { 0.0, 0.0, 0.0 }; int Steps = 0; };

	/** One ray marched as the engine steps a galaxy (log steps x StepRatio, the galaxy's hint, the
	 *  nebulae's cap at the screen footprint; the first step's length x (0.25 + Phase)), from the
	 *  ball's entry. Stars at a 4800 K white, nebulae at the stars' luminance per unit density in
	 *  their colour. bCap false: without the nebulae's cap (the march with uu.Nebula 0). */
	FEnRay EnMarch(const H::FUniFieldParams& P, const H::FUniGalaxyConst& K, const double O[3], const double Dir[3], double ThetaPx, double Phase, bool bCap = true)
	{
		FEnRay R;
		const H::float3 D{ float(Dir[0]), float(Dir[1]), float(Dir[2]) };
		const double Bo = O[0] * Dir[0] + O[1] * Dir[1] + O[2] * Dir[2], Co = O[0] * O[0] + O[1] * O[1] + O[2] * O[2] - 1.0;
		const double Disc = Bo * Bo - Co;
		if (Disc <= 0.0) { return R; }
		const double Tx = -Bo + std::sqrt(Disc);
		double T = std::max(-Bo - std::sqrt(Disc), 1.0e-4);
		bool bFirst = true;
		const H::float3 Base = H::Uni_TemperatureColour(4800.0f);
		for (int N = 0; N < 6000 && T < Tx; ++N)
		{
			const H::float3 Pt{ float(O[0] + T * Dir[0]), float(O[1] + T * Dir[1]), float(O[2] + T * Dir[2]) };
			const float Foot = float(ThetaPx * T);
			double S = std::min(0.32 * T, double(H::Uni_GalaxyStepHintFilteredK(Pt, D, P, Foot, K.Hp)));
			if (bCap) { S = std::min(S, double(H::Uni_GalaxyNebulaStepCap(Pt, D, P, K.Hp, Foot))); }
			S = std::max(S, 1.0e-6);
			if (bFirst) { S *= (0.25 + Phase); bFirst = false; }
			S = std::min(S, Tx - T + 1.0e-7);
			const double Tm = T + 0.5 * S;
			const H::float3 Pm{ float(O[0] + Tm * Dir[0]), float(O[1] + Tm * Dir[1]), float(O[2] + Tm * Dir[2]) };
			H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
			const float Fm = float(ThetaPx * Tm);
			const float Rho = H::UniField_GalaxyFilteredCKN(Pm, P, K, Fm, C, Nv);
			const float Nd = H::Uni_GalaxyNebulaDensity(Nv, P, Fm, float(S));
			const H::float3 Cn = H::Uni_GalaxyNebulaColour(P, K.Hs, Nv.z, H::Uni_GalaxyNebulaBlend(P, Fm, float(S)));
			R.Stars += double(Rho) * S;
			R.Neb += double(Nd) * S;
			R.NebM += double(H::Uni_GalaxyNebulaDensity(Nv, P, 1.0f, float(S))) * S;
			R.Src += double(Nv.x) * S;
			R.Col[0] += (double(Rho) * Base.x + double(Nd * Cn.x)) * S;
			R.Col[1] += (double(Rho) * Base.y + double(Nd * Cn.y)) * S;
			R.Col[2] += (double(Rho) * Base.z + double(Nd * Cn.z)) * S;
			++R.Steps;
			T += S;
		}
		return R;
	}

	/** A camera Dcam out along the view, a square of Half field units across at the centre. */
	void EnPixelRay(const FEnView& V, double Dcam, double U, double W, double O[3], double Dir[3])
	{
		double L = 0.0;
		for (int A = 0; A < 3; ++A) { O[A] = -Dcam * V.Dv[A]; Dir[A] = U * V.Rt[A] + W * V.Up[A] + Dcam * V.Dv[A]; L += Dir[A] * Dir[A]; }
		L = std::sqrt(L);
		for (int A = 0; A < 3; ++A) { Dir[A] /= L; }
	}

	/** A tile of the march (sqrt stretch of the luminance to 3 x the tile's peak). */
	void EnTile(FImage& Img, int Ox, int Oy, int T, const H::FUniFieldParams& P, const FEnView& V, double Half)
	{
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
		const double Dcam = 3.0, Px = 2.0 * Half / T;
		std::vector<float> L(size_t(T) * T * 3, 0.0f);
		float Peak = 0.0f;
		for (int Y = 0; Y < T; ++Y) for (int X = 0; X < T; ++X)
		{
			double O[3], Dir[3];
			EnPixelRay(V, Dcam, -Half + (X + 0.5) * Px, Half - (Y + 0.5) * Px, O, Dir);
			const FEnRay R = EnMarch(P, K, O, Dir, Px / Dcam, 0.0);
			for (int C = 0; C < 3; ++C) { L[(size_t(Y) * T + X) * 3 + C] = float(R.Col[C]); }
			Peak = std::max(Peak, float(0.2126 * R.Col[0] + 0.7152 * R.Col[1] + 0.0722 * R.Col[2]));
		}
		const float G = 3.0f / std::max(Peak, 1.0e-30f);
		for (int Y = 0; Y < T; ++Y) for (int X = 0; X < T; ++X)
		{
			const float* C = &L[(size_t(Y) * T + X) * 3];
			const float Lm = std::max((0.2126f * C[0] + 0.7152f * C[1] + 0.0722f * C[2]) * G, 1.0e-30f);
			const float S = std::sqrt(Lm) / Lm * G;
			Img.Add(Ox + X, Oy + Y, C[0] * S, C[1] * S, C[2] * S);
		}
	}

	double EnShaderConstant(const std::string& Text, const std::string& Name)
	{
		const std::regex Re("#define\\s+" + Name + "\\s+([-0-9.eE]+)f?");
		std::smatch M;
		if (std::regex_search(Text, M, Re)) { return std::stod(M[1].str()); }
		const std::regex Rs("static const float\\s+" + Name + "\\s*=\\s*([-0-9.eE]+)f?");
		return std::regex_search(Text, M, Rs) ? std::stod(M[1].str()) : std::nan("");
	}
}

UNI_TEST(galaxy, early_nebulae)
{
	const FEnTex Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.early_nebulae", "D-236 early types' nebulae and shells").Pass(false, "textures not loaded: " + Tx.Err); return; }
	const FEnBind Bind(Tx);
	FUniverseDef D = UniScenes::Base();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
	const FUniverseDef D0 = UniScenes::Base();
	const FUniEmitter& E0 = D0.Arch(UniScenes::kGalaxy).Emitters.at(0);

	// The shape's mean constant against the sampled one (the read's mip, trilinear).
	std::string Src;
	{
		std::ifstream F("Shaders/Private/Core/UniFields.ush");
		std::stringstream B; B << F.rdbuf(); Src = B.str();
	}
	const double T0 = EnShaderConstant(Src, "UNI_EARLY_NEB_T0"), T1 = EnShaderConstant(Src, "UNI_EARLY_NEB_T1"), Cmean = EnShaderConstant(Src, "UNI_EARLY_NEB_SHAPE_MEAN");
	double Smean = 0.0;
	{
		const int N = 200000;
		for (int I = 0; I < N; ++I)
		{
			const H::float3 Pt{ H::Uni_DrawUnit(0xE361u, 2u, u32(3 * I)), H::Uni_DrawUnit(0xE361u, 2u, u32(3 * I + 1)), H::Uni_DrawUnit(0xE361u, 2u, u32(3 * I + 2)) };
			const double R = double(UNI_GAL_DETAIL(Pt, UNI_EARLY_NEB_MIP).x) / 0.567;
			const double X = std::min(std::max((R - T0) / (T1 - T0), 0.0), 1.0);
			Smean += X * X / N;
		}
	}
	const bool bMean = std::abs(Smean / Cmean - 1.0) < 0.03;

	// Gating: 256 rolls. Early types roll filaments and shells (some); spirals, untextured early
	// types, dust sets and the home galaxy have neither.
	size_t NebN = 0, ShellN = 0, LateBad = 0, PlainBad = 0, OtherBad = 0;
	for (u32 I = 0; I < 256u; ++I)
	{
		const u32 S = H::Uni_SeedMix(0xE362u, I);
		const H::FUniFieldParams Pe = EnRoll(E, S, true).ToHlsl(), Pl = EnRoll(E, S, false).ToHlsl(), P0 = EnRoll(E0, S, true).ToHlsl();
		NebN += H::Uni_GalaxyEarlyNebula(Pe) > 0.0f ? 1 : 0;
		ShellN += (H::Uni_GalaxySpheroidNoise(Pe) > 0.0f && Pe.C.y > 0.0f) ? 1 : 0;
		LateBad += (H::Uni_GalaxyEarlyNebula(Pl) > 0.0f || H::Uni_GalaxySpheroidNoise(Pl) > 0.0f) ? 1 : 0;
		PlainBad += (H::Uni_GalaxyEarlyNebula(P0) > 0.0f || H::Uni_GalaxySpheroidNoise(P0) > 0.0f) ? 1 : 0;
	}
	for (const FArchetype& A : D.Archetypes)
	{
		for (const FUniEmitter& Em : A.Emitters)
		{
			if (Em.FieldId != H::UNI_FIELD_GALAXY) { continue; }
			for (u32 I = 0; I < 64u; ++I) { OtherBad += H::Uni_GalaxyEarlyNebula(EnRoll(Em, H::Uni_SeedMix(0xE367u, I), false).ToHlsl()) > 0.0f ? 1 : 0; }
			// A dust set (DustFrom: D.y its sigma, no bulge) never has filaments.
			for (u32 I = 0; I < 16u; ++I) { OtherBad += H::Uni_GalaxyEarlyNebula(DustFrom(EnRoll(Em, H::Uni_SeedMix(0xE363u, I), true), A.Volume).ToHlsl()) > 0.0f ? 1 : 0; }
		}
	}
	for (const FAnchor& A : D.Anchors) { for (const FFieldParams& P : A.EmitterParams) { OtherBad += H::Uni_GalaxyEarlyNebula(P.ToHlsl()) > 0.0f ? 1 : 0; } }

	// Light, blend and slide on eight early types with the filaments at amount 1.
	const int NGal = 8;
	double ShareMin = 1e300, ShareMax = 0.0, ShareMean = 0.0, BlendWorst = 0.0, LogBlend = 0.0;
	double SlideNeb = 0.0, SlideSrc = 0.0, SlideStarsOn = 0.0, SlideStarsOff = 0.0, StepCost = 0.0;
	double SlideNebIn = 0.0, SlideSrcIn = 0.0, SlideStarsOnIn = 0.0, SlideStarsOffIn = 0.0;
	for (int Gi = 0; Gi < NGal; ++Gi)
	{
		FFieldParams Pf = EnRoll(E, H::Uni_SeedMix(0xE364u, u32(Gi)), true);
		Pf.V[20] = 1.0f;
		Pf.V[9] = Gi % 2 ? 0.5f : 0.0f;
		const H::FUniFieldParams P = Pf.ToHlsl();
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
		const FEnView V(0.3 + 0.15 * Gi, 0.8 * Gi);
		const double Re = Pf.V[3];
		// The share, from 3 radii, over 3 r_e squared (40 x 40 rays): resolved (a pixel of 0.1
		// texel) and far (the footprint past the blend: the mean).
		auto Light = [&](double ThetaScale, double& OutStars, double& OutPatternOverMean)
		{
			const int G = 60; double Sn = 0.0, Ss = 0.0, SnM = 0.0;
			const double Half = 5.0 * Re, Px = 2.0 * Half / G;
			for (int Y = 0; Y < G; ++Y) for (int X = 0; X < G; ++X)
			{
				double O[3], Dir[3];
				EnPixelRay(V, 3.0, -Half + (X + 0.5) * Px, Half - (Y + 0.5) * Px, O, Dir);
				const FEnRay R = EnMarch(P, K, O, Dir, ThetaScale, 0.0);
				Sn += R.Neb; Ss += R.Stars; SnM += R.NebM;
			}
			OutStars = Ss;
			OutPatternOverMean = Sn / std::max(SnM, 1e-300);
			return Sn;
		};
		const double Te = double(H::Uni_GalaxyEarlyNebulaTexel(P));
		double Ss = 0.0, Pm = 1.0;
		const double Near = Light(0.1 * Te / 3.0, Ss, Pm);
		const double Share = Near / std::max(Ss, 1e-300);
		ShareMin = std::min(ShareMin, Share); ShareMax = std::max(ShareMax, Share); ShareMean += Share / NGal;
		LogBlend += std::log(Pm) / NGal;
		BlendWorst = std::max(BlendWorst, std::abs(std::log(Pm)));
		// Slide: the first step's phase moved (0, 1/4, 1/2, 3/4: the shells move with the camera),
		// the spread over the mean, light-weighted: the nebulae's, their source's (no pattern), and
		// the stars' with shells and without. From outside (a close view of the core, a pixel 0.1
		// texel, 16 x 16 rays: the steps start at the sphere's surface, so none) and from inside
		// (a camera 0.4 of the sphere's radius out, 64 rays in all directions).
		if (Gi < 4)
		{
			FFieldParams Pn = Pf; Pn.V[9] = 0.0f;
			const H::FUniFieldParams Pq = Pn.ToHlsl();
			const H::FUniGalaxyConst Kq = H::Uni_GalaxyConstOf(Pq);
			FFieldParams Ps = Pf; Ps.V[9] = 0.5f;
			const H::FUniFieldParams Pw = Ps.ToHlsl();
			const H::FUniGalaxyConst Kw = H::Uni_GalaxyConstOf(Pw);
			for (int Side = 0; Side < 2; ++Side)
			{
				double Wn = 0, Dn = 0, Wsrc = 0, Dsrc = 0, Won = 0, Don = 0, Woff = 0, Doff = 0, Ncap = 0, Nfree = 0;
				const int NR = Side == 0 ? 256 : 64;
				const double Half = 2.0 * Re, Px = 2.0 * Half / 16, Th = Side == 0 ? 0.1 * Te / 3.0 : 0.01;
				for (int Ri = 0; Ri < NR; ++Ri)
				{
					double O[3], Dir[3];
					if (Side == 0) { EnPixelRay(V, 3.0, -Half + (Ri % 16 + 0.5) * Px, Half - (Ri / 16 + 0.5) * Px, O, Dir); }
					else
					{
						const double Rs = 0.4 * 3.0 * Re;
						const double Cz = 2.0 * EnUnit(0xE3B0u + Gi, 4 * Ri) - 1.0, Ph = 6.2831853 * EnUnit(0xE3B0u + Gi, 4 * Ri + 1), Sz = std::sqrt(1.0 - Cz * Cz);
						const double Cd = 2.0 * EnUnit(0xE3B0u + Gi, 4 * Ri + 2) - 1.0, Pd = 6.2831853 * EnUnit(0xE3B0u + Gi, 4 * Ri + 3), Sd = std::sqrt(1.0 - Cd * Cd);
						O[0] = Rs * Sz * std::cos(Ph); O[1] = Rs * Sz * std::sin(Ph); O[2] = Rs * Cz;
						Dir[0] = Sd * std::cos(Pd); Dir[1] = Sd * std::sin(Pd); Dir[2] = Cd;
					}
					double Lo[4] = { 1e300, 1e300, 1e300, 1e300 }, Hi[4] = { -1e300, -1e300, -1e300, -1e300 }, Mn[4] = { 0, 0, 0, 0 };
					for (int Ph = 0; Ph < 4; ++Ph)
					{
						const FEnRay A = EnMarch(Pq, Kq, O, Dir, Th, 0.25 * Ph);
						const FEnRay B = EnMarch(Pw, Kw, O, Dir, Th, 0.25 * Ph);
						const double Vv[4] = { A.Neb, A.Src, B.Stars, A.Stars };
						for (int I = 0; I < 4; ++I) { Lo[I] = std::min(Lo[I], Vv[I]); Hi[I] = std::max(Hi[I], Vv[I]); Mn[I] += 0.25 * Vv[I]; }
						if (Ph == 0 && Side == 0) { Ncap += A.Steps; Nfree += EnMarch(Pq, Kq, O, Dir, Th, 0.0, false).Steps; }
					}
					Wn += Mn[0]; Dn += Hi[0] - Lo[0];
					Wsrc += Mn[1]; Dsrc += Hi[1] - Lo[1];
					Won += Mn[2]; Don += Hi[2] - Lo[2];
					Woff += Mn[3]; Doff += Hi[3] - Lo[3];
				}
				(Side == 0 ? SlideNeb : SlideNebIn) += Dn / std::max(Wn, 1e-300) / 4.0;
				(Side == 0 ? SlideSrc : SlideSrcIn) += Dsrc / std::max(Wsrc, 1e-300) / 4.0;
				(Side == 0 ? SlideStarsOn : SlideStarsOnIn) += Don / std::max(Won, 1e-300) / 4.0;
				(Side == 0 ? SlideStarsOff : SlideStarsOffIn) += Doff / std::max(Woff, 1e-300) / 4.0;
				if (Side == 0) { StepCost += Ncap / std::max(Nfree, 1.0) / 4.0; }
			}
		}
	}

	// Colours: three a galaxy at luminance 1; hue families covered (spirals and early types).
	size_t LumBad = 0, Flat = 0;
	std::set<int> HueBinsLate, HueBinsEarly;
	for (u32 I = 0; I < 128u; ++I)
	{
		const bool bEarly = I >= 64u;
		FFieldParams Pf = EnRoll(E, H::Uni_SeedMix(0xE365u, I), bEarly);
		if (bEarly) { Pf.V[20] = 0.5f; } else { Pf.V[29] = std::max(Pf.V[29], 0.5f); }   // H.y: knots on
		const H::FUniFieldParams P = Pf.ToHlsl();
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
		H::float3 C[3];
		for (int M = 0; M < 3; ++M)
		{
			C[M] = H::Uni_GalaxyNebulaColour(P, K.Hs, 0.526f + 0.26f * (0.5f * M - 0.5f), 0.0f);
			LumBad += std::abs(H::Uni_PaletteLum(C[M]) - 1.0f) > 1e-3f ? 1 : 0;
			const double Mx = std::max({ C[M].x, C[M].y, C[M].z }), Mi = std::min({ C[M].x, C[M].y, C[M].z });
			if (Mx - Mi < 1e-6) { continue; }
			double Hh;
			if (Mx == C[M].x) { Hh = 60.0 * std::fmod((C[M].y - C[M].z) / (Mx - Mi) + 6.0, 6.0); }
			else if (Mx == C[M].y) { Hh = 60.0 * ((C[M].z - C[M].x) / (Mx - Mi) + 2.0); }
			else { Hh = 60.0 * ((C[M].x - C[M].y) / (Mx - Mi) + 4.0); }
			(bEarly ? HueBinsEarly : HueBinsLate).insert(int(Hh / 30.0) % 12);
		}
		double Dmax = 0.0;
		for (int A = 0; A < 3; ++A) for (int B = A + 1; B < 3; ++B) { Dmax = std::max(Dmax, double(std::abs(C[A].x - C[B].x) + std::abs(C[A].y - C[B].y) + std::abs(C[A].z - C[B].z))); }
		Flat += Dmax < 0.15 ? 1 : 0;
	}

	// Stills: nine rolled early types with filaments (forced on in the right half) and shells,
	// whole and the cores; the nebula colours of 24 galaxies (spirals, then early types).
	{
		const int T = 150, Gn = 3;
		FImage Img(T * Gn * 2, T * Gn * 2);
		for (int Gi = 0; Gi < Gn * Gn; ++Gi)
		{
			FFieldParams Pf = EnRoll(E, H::Uni_SeedMix(0x9A1Au, u32(Gi + 40)), true);
			FFieldParams Pr = Pf; Pr.V[20] = std::max(Pr.V[20], 0.6f); if (Gi % 2) { Pr.V[9] = 0.5f; }
			const FEnView V((20.0 + 7.0 * Gi) * 3.14159265358979 / 180.0, 0.7 * Gi);
			const int Ox = (Gi % Gn) * T, Oy = (Gi / Gn) * T;
			EnTile(Img, Ox, Oy, T, Pf.ToHlsl(), V, 0.5);
			EnTile(Img, Ox + T * Gn, Oy, T, Pr.ToHlsl(), V, 0.5);
			EnTile(Img, Ox, Oy + T * Gn, T, Pf.ToHlsl(), V, 0.2);
			EnTile(Img, Ox + T * Gn, Oy + T * Gn, T, Pr.ToHlsl(), V, 0.2);
		}
		EnWriteFrame(Img, "still_galaxy_early_nebulae", { "D-236: nine rolled early types as rolled (left) and with filaments >= 0.6 and shells 0.5 on odd ones (right); top whole (1 field unit), bottom the cores (0.4)",
			"the march as the engine steps it, sqrt stretch to 3 x each tile's peak" });
	}
	{
		const int W = 24, Sw = 40;
		FImage Img(W * Sw, 2 * 3 * Sw);
		for (int Row = 0; Row < 2; ++Row) for (int I = 0; I < W; ++I)
		{
			FFieldParams Pf = EnRoll(E, H::Uni_SeedMix(0xE366u, u32(I + 100 * Row)), Row == 1);
			if (Row == 1) { Pf.V[20] = 0.5f; } else { Pf.V[29] = std::max(Pf.V[29], 0.5f); }
			const H::FUniFieldParams P = Pf.ToHlsl();
			const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
			for (int Y = 0; Y < 3 * Sw; ++Y)
			{
				const float M = float(Y) / float(3 * Sw - 1);
				const H::float3 C = H::Uni_GalaxyNebulaColour(P, K.Hs, 0.526f + 0.26f * (M - 0.5f), 0.0f);
				for (int X = 0; X < Sw - 2; ++X) { Img.Add(I * Sw + X, Row * 3 * Sw + Y, 0.35f * C.x, 0.35f * C.y, 0.35f * C.z); }
			}
		}
		EnWriteFrame(Img, "still_nebula_colours", { "D-236: nebula colours, 24 spirals (top) and 24 early types (bottom): each column a galaxy's three colours along the mix",
			"luminance 1 x 0.35" });
	}

	const bool bPass = bMean && NebN > 80 && NebN < 220 && ShellN > 30 && ShellN < 150 && LateBad == 0 && PlainBad == 0 && OtherBad == 0
		&& ShareMin > 0.02 && ShareMax < 0.4 && std::abs(LogBlend) < std::log(1.35) && BlendWorst < std::log(2.5) && SlideNeb < SlideSrc + 0.02 && SlideStarsOn < SlideStarsOff + 0.005 && SlideNebIn < SlideSrcIn + 0.02 && SlideStarsOnIn < SlideStarsOffIn + 0.005
		&& LumBad == 0 && Flat < 13 && HueBinsLate.size() >= 9 && HueBinsEarly.size() >= 7;
	Uni::Check(2, "T2.galaxy.early_nebulae", "D-236 early types' nebula filaments and shells, and the nebulae's colours: the filaments' shape mean is the sampled one (3%); only textured early types roll filaments and shells (spirals, untextured early types, dust sets, the home galaxy: none); at amount 1 the filaments' light is 2-40% of the stars' and its blended mean is the pattern's (on the same samples) within x 1.35 over eight galaxies (one galaxy's core holds only a few of the texture's features: x 2.5 the worst); marched as the engine steps, from outside (steps from the sphere's surface) and inside (a camera in the core), their slide across step phases is their source's within 2% and the shells add none to the stars' (0.5%); three colours a galaxy at luminance 1, under 10% nearly flat, spirals over 9 and early types over 7 of 12 hue bins")
		.Metric("shape_mean", Smean).Metric("share_min", ShareMin).Metric("share_mean", ShareMean).Metric("share_max", ShareMax).Metric("blend_mean", std::exp(LogBlend)).Metric("blend_worst", std::exp(BlendWorst))
		.Metric("slide_neb", SlideNeb).Metric("slide_src", SlideSrc).Metric("slide_stars_shells", SlideStarsOn).Metric("slide_stars", SlideStarsOff).Metric("step_cost", StepCost).Metric("slide_neb_inside", SlideNebIn).Metric("slide_src_inside", SlideSrcIn).Metric("slide_stars_shells_inside", SlideStarsOnIn).Metric("slide_stars_inside", SlideStarsOffIn)
		.Pass(bPass, Uni::Fmt("shape mean %.5f sampled vs %.5f; 256 rolls: %zu filaments, %zu shells; spirals %zu / untextured %zu / others %zu bad; light %.1f-%.1f%% (mean %.1f%%) of the stars', the pattern over its blended mean on the same samples x %.2f (geometric mean; x %.2f the worst); slide outside %.2f%% (source %.2f%%), stars %.3f%% with shells vs %.3f%%; inside %.2f%% (source %.2f%%), stars %.3f%% vs %.3f%%; cap steps x %.2f in the cores; colours: %zu off luminance, %zu flat, hue bins %zu spiral / %zu early",
			Smean, Cmean, NebN, ShellN, LateBad, PlainBad, OtherBad, 100.0 * ShareMin, 100.0 * ShareMax, 100.0 * ShareMean, std::exp(LogBlend), std::exp(BlendWorst),
			100.0 * SlideNeb, 100.0 * SlideSrc, 100.0 * SlideStarsOn, 100.0 * SlideStarsOff, 100.0 * SlideNebIn, 100.0 * SlideSrcIn, 100.0 * SlideStarsOnIn, 100.0 * SlideStarsOffIn, StepCost, LumBad, Flat, HueBinsLate.size(), HueBinsEarly.size()));
}
