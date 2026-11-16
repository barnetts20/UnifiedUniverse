// test_nebfront.cpp
// D-243 (owner 2026-11-14, B-034: dark clouds offset from the H II regions, ionisation fronts at
// cloud edges): where the walls are read, each spiral nebula is split into glowing gas and dark
// dust by the walls read's fBm against its dark share, with a bright front between them.
//  - the constants: the shaders' defines equal the shim's knobs; the normalisations (LSPLIT,
//    RSPLIT) are the means they normalise, measured over the Milky Way's gas layer;
//  - the light at the default share keeps D-241's mean at full detail (0.5%) and half faded (3%:
//    1.7% on the Milky Way before D-256, 2.2% after); the reflection's dust its mean (0.5%; RSPLIT
//    measured where the split applies, the structure: elsewhere VPR / VMR's even dimming);
//  - the split: the structure's dark part (glow below a half) is the nebulae's mean share (within
//    0.05), its dust over three times the glowing part's, fronts present (rim over a half);
//  - still_nebula_fronts: a slice through the brightest knot (glow pink, dark dust brown, fronts
//    white) and the march in the mid-plane 250 pc back (the mean of 8 jittered frames).

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FNfTex
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FNfTex()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};

	struct FNfBind
	{
		explicit FNfBind(const FNfTex& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FNfBind() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	struct FNfSample { H::float3 P; H::float4 Neb; };

	FCoord NfCamAt(const FVec3d& P) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, P); return C; }

	std::vector<float> NfMarch(const FVolumeScene& Sc, const FCamera& Cm, u32 Flags, float Shift)
	{
		auto V = ToHlsl(Sc.View);
		V.MarchFlags |= Flags; V.JitterShift = Shift;
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		std::vector<float> Rgb(size_t(Cm.Width) * Cm.Height * 3, 0.0f);
		std::atomic<int> Next{ 0 };
		auto Work = [&]()
		{
			for (int Y = Next++; Y < Cm.Height; Y = Next++) for (int X = 0; X < Cm.Width; ++X)
			{
				bool bS = false;
				const H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cm, Cm.Width, Cm.Height, X, Y, 3.0e38f, bS);
				const size_t I = (size_t(Y) * Cm.Width + X) * 3;
				Rgb[I] = R.Radiance.x; Rgb[I + 1] = R.Radiance.y; Rgb[I + 2] = R.Radiance.z;
			}
		};
		std::vector<std::thread> Ts; for (int T = 1; T < 2; ++T) { Ts.emplace_back(Work); }
		Work(); for (std::thread& T : Ts) { T.join(); }
		return Rgb;
	}

	double NfLum(const std::vector<float>& F, size_t I) { return 0.2126 * F[3 * I] + 0.7152 * F[3 * I + 1] + 0.0722 * F[3 * I + 2]; }

	float NfDens(const H::float3& InP, const H::float4& InNeb, const H::FUniFieldParams& InPar, float InFoot, float InDetail, float InDark, float& OutX, float& OutDust, float& OutReflect)
	{
		return H::Uni_GalaxyNebulaDensityAt(InP, InNeb, InPar, InFoot, 0.0f, InDetail, InDark, OutX, OutDust, OutReflect);
	}
}

UNI_TEST(galaxy, nebula_fronts)
{
	const FNfTex Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.nebula_fronts", "D-243 ionisation fronts").Pass(false, "textures not loaded: " + Tx.Err); return; }
	const FNfBind Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	const FFieldParams* Mw = nullptr;
	for (const FAnchor& A : D.Anchors) { if (!A.EmitterParams.empty()) { Mw = &A.EmitterParams[0]; } }
	if (!Mw) { Uni::Check(2, "T2.galaxy.nebula_fronts", "D-243").Pass(false, "no anchor"); return; }
	const H::FUniFieldParams P = Mw->ToHlsl();
	const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);

	// The constants.
	std::string Src;
	{
		std::ifstream F("Shaders/Private/Core/UniFields.ush");
		std::stringstream B; B << F.rdbuf(); Src = B.str();
	}
	auto Def = [&](const char* Name) { const std::regex Re(std::string("#define\\s+") + Name + "\\s+([-0-9.eE]+)f?"); std::smatch M; return std::regex_search(Src, M, Re) ? std::stod(M[1].str()) : std::nan(""); };
	struct FKn { const char* Name; double Knob; };
	const FKn Kn[] = { { "UNI_NEB_FRONT_W", H::GKnobNebFrontW }, { "UNI_NEB_RIM_W", H::GKnobNebRimW }, { "UNI_NEB_RIM", H::GKnobNebRim },
		{ "UNI_NEB_LSPLIT", H::GKnobNebLsplit }, { "UNI_NEB_RSPLIT", H::GKnobNebRsplit } };
	size_t ConstBad = 0;
	std::string ConstText;
	for (const FKn& C : Kn)
	{
		const double V = Def(C.Name);
		if (!(std::abs(V - C.Knob) <= 1e-6 * std::max(std::abs(C.Knob), 1e-6))) { ++ConstBad; ConstText += Uni::Fmt(" %s %g vs %g;", C.Name, V, C.Knob); }
	}

	// The gas layer of an annulus, with the knots' source.
	std::vector<FNfSample> S;
	const int N = 240000;
	for (int I = 0; I < N; ++I)
	{
		const double R = 0.2 + 0.5 * H::Uni_DrawUnit(0xA13u, 2u, u32(3 * I)), Ph = 6.2831853 * H::Uni_DrawUnit(0xA13u, 2u, u32(3 * I + 1));
		const double Z = (2.0 * H::Uni_DrawUnit(0xA13u, 2u, u32(3 * I + 2)) - 1.0) * 0.5 * double(P.A.y);
		const H::float3 Pt{ float(R * std::cos(Ph)), float(R * std::sin(Ph)), float(Z) };
		H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
		H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-6f, C, Nv);
		if (Nv.x > 0.0f) { S.push_back({ Pt, Nv }); }
	}
	const float M0 = UNI_NEB_DARK_MEAN0;
	// The normalisations: the glow-and-rim factor's light-weighted (source x shape) and dust-weighted
	// means at the default share, full detail.
	double Lw = 0, Lg = 0, Dw = 0, Dg = 0;
	double Xw = 0, XDark = 0, XShare = 0, XRim = 0, DustDark = 0, DustGlow = 0, NDark = 0, NGlow = 0;
	for (const FNfSample& Q : S)
	{
		float X = 0, Md = 1, Sp = 0.5f, Sw = 0;
		const float Sh = H::Uni_GalaxyNebulaShapeSplitAt(Q.P, Q.Neb, P, 0.0f, 1.0f, X, Md, Sp, Sw);
		const float A = H::Uni_GalaxyNebulaDarkShare(Q.Neb, M0);
		float Rim = 0;
		const float G = H::Uni_GalaxyNebulaFront(Sp, A, Rim);
		const double F = double(G) + double(UNI_NEB_RIM) * Rim;
		Lw += double(Q.Neb.x) * Sh; Lg += double(Q.Neb.x) * Sh * F;
		if (Sw > 0.0f) { Dw += Md; Dg += Md * F; }   // where the split applies (elsewhere the even dimming, VPR / VMR's)
		if (X > 0.0f)
		{
			Xw += X; XShare += X * A; XDark += G < 0.5f ? X : 0.0; XRim += Rim > 0.5f ? X : 0.0;
			float Xd = 0, Mdd = 1, Mr = 1;
			NfDens(Q.P, Q.Neb, P, 0.0f, 1.0f, M0, Xd, Mdd, Mr);
			if (G < 0.5f) { DustDark += Mdd; ++NDark; } else { DustGlow += Mdd; ++NGlow; }
		}
	}
	const double Lsplit = Lg / std::max(Lw, 1e-300), Rsplit = Dg / std::max(Dw, 1e-300);
	const bool bNorm = std::abs(Lsplit / UNI_NEB_LSPLIT - 1.0) < 0.005 && std::abs(Rsplit / UNI_NEB_RSPLIT - 1.0) < 0.005;
	const double DarkPart = XDark / std::max(Xw, 1e-300), ShareMean = XShare / std::max(Xw, 1e-300), RimPart = XRim / std::max(Xw, 1e-300);
	const double DustRatio = (DustDark / std::max(NDark, 1.0)) / std::max(DustGlow / std::max(NGlow, 1.0), 1e-300);
	// The light (over D-241's) at full detail and half faded, and the reflection's dust.
	double Lf = 0, Lh = 0, L0 = 0, Rf = 0, R0 = 0;
	for (const FNfSample& Q : S)
	{
		float X = 0, Md = 1, Mr = 1;
		Lf += NfDens(Q.P, Q.Neb, P, 0.0f, 1.0f, M0, X, Md, Mr); Rf += Mr;
		Lh += NfDens(Q.P, Q.Neb, P, 0.0f, 0.5f, M0, X, Md, Mr);
		float X0 = 0, Md0 = 1, Mr0 = 1;
		L0 += NfDens(Q.P, Q.Neb, P, 0.0f, 1.0f, -1.0f, X0, Md0, Mr0); R0 += Mr0;
	}
	const double LightFull = Lf / std::max(L0, 1e-300), LightHalf = Lh / std::max(L0, 1e-300), Refl = Rf / std::max(R0, 1e-300);
	// (Half faded the base shape differs from full detail's: compared against D-241's at detail 0.5.)
	double L05 = 0;
	for (const FNfSample& Q : S) { float X = 0, Md = 1, Mr = 1; L05 += NfDens(Q.P, Q.Neb, P, 0.0f, 0.5f, -1.0f, X, Md, Mr); }
	const double LightHalfRel = Lh / std::max(L05, 1e-300);

	// still_nebula_fronts: a slice through the brightest knot and the march from 250 pc back.
	double Hx = 0.0, Hy = 0.0, Hb = -1.0;
	for (int I = 0; I < 60000; ++I)
	{
		const double R = 0.3 + 0.4 * H::Uni_DrawUnit(0xF374u, 2u, u32(2 * I)), Ph = 6.2831853 * H::Uni_DrawUnit(0xF374u, 2u, u32(2 * I + 1));
		const H::float3 Pt{ float(R * std::cos(Ph)), float(R * std::sin(Ph)), 0.0f };
		H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
		const float Rho = H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-5f, C, Nv);
		const double Sc = double(H::Uni_GalaxyNebulaX(Nv, P)) * double(H::Uni_GalaxyNebulaDensity(Nv, P, 1e-5f)) / std::max(double(Rho), 1e-30);
		if (Sc > Hb) { Hb = Sc; Hx = Pt.x; Hy = Pt.y; }
	}
	const std::string Dir = UniVerify::Options().OutDir + "/render";
	::mkdir(UniVerify::Options().OutDir.c_str(), 0755); ::mkdir(Dir.c_str(), 0755);
	const int Ws = 256;
	FImage Img(Ws * 3, Ws);
	{
		// 800 pc a side at shares 0, the default and 0.8: glow pink, dark dust brown (its added
		// dust), fronts white.
		const double Span = 800.0 / 24000.0;
		const float Foot = float(Span / Ws);
		const float Shares[3] = { 0.0f, M0, 0.8f };
		std::vector<float> Vv(size_t(3) * Ws * Ws, 0.0f), Dv(size_t(3) * Ws * Ws, 1.0f);
		for (int Q = 0; Q < 3; ++Q) for (int Y = 0; Y < Ws; ++Y) for (int X = 0; X < Ws; ++X)
		{
			const H::float3 Pt{ float(Hx + ((X + 0.5) / Ws - 0.5) * Span), float(Hy + (0.5 - (Y + 0.5) / Ws) * Span), 0.0f };
			H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
			H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-6f, C, Nv);
			float Xs = 0, Md = 1, Mr = 1;
			const float Dn = NfDens(Pt, Nv, P, Foot, 1.0f, Shares[Q], Xs, Md, Mr);
			const size_t I = (size_t(Q) * Ws + Y) * Ws + X;
			Vv[I] = Nv.x > 0.0f ? Dn / (Nv.x * UNI_NEBULA_SCALE * H::UNI_NEBULA_SHAPE_MEAN) : 0.0f;
			Dv[I] = Md;
		}
		std::vector<float> Vs(Vv.begin() + size_t(Ws) * Ws, Vv.begin() + size_t(2) * Ws * Ws);
		std::sort(Vs.begin(), Vs.end());
		const float Top = std::max(Vs[size_t(0.99 * (Vs.size() - 1))], 1e-6f);   // the default panel's 99th percentile
		for (int Q = 0; Q < 3; ++Q) for (int Y = 0; Y < Ws; ++Y) for (int X = 0; X < Ws; ++X)
		{
			const size_t I = (size_t(Q) * Ws + Y) * Ws + X;
			const float V = Vv[I], Md = Dv[I];
			const float Gl = std::sqrt(std::min(V / Top, 1.0f));
			const float Dk = std::min(std::max(Md - 1.0f, 0.0f) / 30.0f, 1.0f);
			Img.Add(Q * Ws + X, Y, Gl * (1.0f - 0.6f * Dk) + 0.35f * Dk, 0.55f * Gl * (1.0f - 0.6f * Dk) + 0.22f * Dk, 0.65f * Gl * (1.0f - 0.6f * Dk) + 0.12f * Dk);
		}
		Img.WritePpm(Dir + "/still_nebula_fronts.ppm");
		if (FILE* Fh = std::fopen((Dir + "/still_nebula_fronts.hud.json").c_str(), "wb"))
		{
			std::fprintf(Fh, "{\"lines\":[\"D-243: an 800 pc slice through the Milky Way's brightest disc nebula, dark share 0, 0.35 (the default) and 0.8: glow pink (brighter at the fronts), the dark dust brown\"]}\n");
			std::fclose(Fh);
		}
	}
	double MarchRatio = 0.0, RefineRatio = 0.0;
	{
		FUniverseModel Mo(D);
		FRefGenerator G(Mo);
		FVolumeBuilder VB(Mo, G);
		double Radius = 0.0;
		auto Build = [&](const FCoord& Cam, const FCamera& Cm, float Dark, u32 Refine = 1u)
		{
			FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cam, FVec3f{});
			FVolumeSettings Sv; Sv.NebulaGain = 1.0f; Sv.NebulaReflect = 6.0f; Sv.NebulaDark = Dark; Sv.NebulaRefine = Refine;
			FVolumeScene Sc = VB.Build(Pp, Cam, FVec3f{}, Cm.View(), Sv).Subset(false);
			for (const FCarrierGPU& Cg : Sc.Carriers)
			{
				const double Rl = std::sqrt(double(Cg.CamLocal[0]) * Cg.CamLocal[0] + double(Cg.CamLocal[1]) * Cg.CamLocal[1] + double(Cg.CamLocal[2]) * Cg.CamLocal[2]);
				if (Cg.FieldId == H::UNI_FIELD_GALAXY && Rl < 1.0) { Radius = 1.0 / double(Cg.InvRadius); }
			}
			return Sc;
		};
		{ FCamera Cm; Cm.Coord = NfCamAt({ 8.0 * Units::Kpc, 0.0, 0.0 }); Cm.Width = 4; Cm.Height = 4; Cm.LookAlong({ 0.0, 1.0, 0.0 }); Build(Cm.Coord, Cm, M0); }
		const int W = 192, Ht = 108;
		FCamera Cm; Cm.Coord = NfCamAt({ Hx * Radius, Hy * Radius - 250.0 * Units::Pc, 3.0 * Units::Pc }); Cm.Width = W; Cm.Height = Ht; Cm.LookAlong({ 0.3, 1.0, 0.03 });
		std::vector<float> Fr[3];
		const float Shares[3] = { -1.0f, M0, M0 };
		for (int Q = 0; Q < 3; ++Q)
		{
			// D-244: the third with uu.Nebula.Refine 4 (the march's sub-samples; its mean the same light).
			const FVolumeScene Sc = Build(Cm.Coord, Cm, Shares[Q], Q == 2 ? 4u : 1u);
			Fr[Q].assign(size_t(W) * Ht * 3, 0.0f);
			for (int Kj = 0; Kj < 8; ++Kj)
			{
				const std::vector<float> J = NfMarch(Sc, Cm, 8u, float(std::fmod(Kj * 0.6180339887, 1.0)));
				for (size_t I = 0; I < J.size(); ++I) { Fr[Q][I] += J[I] / 8.0f; }
			}
		}
		const size_t Np = size_t(W) * Ht;
		std::vector<double> L; for (size_t I = 0; I < Np; ++I) { L.push_back(NfLum(Fr[0], I)); }
		std::sort(L.begin(), L.end());
		const double Top = L[size_t(0.995 * (L.size() - 1))];
		double Sum[3] = { 0, 0, 0 };
		for (size_t I = 0; I < Np; ++I) { Sum[2] += NfLum(Fr[2], I); }
		FImage Im2(W * 2, Ht);
		for (int Q = 0; Q < 2; ++Q) for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
		{
			const std::vector<float>& F = Fr[Q];
			const size_t I = size_t(Y) * W + X;
			Sum[Q] += NfLum(F, I);
			const double Lr = NfLum(F, I) / std::max(Top, 1e-300);
			const double Sc2 = Lr > 0.0 ? (Lr / (1.0 + 0.25 * Lr)) / Lr / std::max(Top, 1e-300) : 0.0;
			Im2.Add(Q * W + X, Y, float(F[3 * I] * Sc2), float(F[3 * I + 1] * Sc2), float(F[3 * I + 2] * Sc2));
		}
		MarchRatio = Sum[1] / std::max(Sum[0], 1e-300);
		RefineRatio = Sum[2] / std::max(Sum[1], 1e-300);
		Im2.WritePpm(Dir + "/still_nebula_fronts_march.ppm");
		if (FILE* Fh = std::fopen((Dir + "/still_nebula_fronts_march.hud.json").c_str(), "wb"))
		{
			std::fprintf(Fh, "{\"lines\":[\"D-243: 250 pc from the Milky Way's brightest disc nebula in the mid-plane, the mean of 8 jittered frames: D-241 (left), with the dark shares and fronts (right)\"]}\n");
			std::fclose(Fh);
		}
	}

	const bool bPass = ConstBad == 0 && bNorm && std::abs(LightFull - 1.0) < 0.005 && std::abs(LightHalfRel - 1.0) < 0.03 && std::abs(Refl - 1.0) < 0.005
		&& std::abs(DarkPart - ShareMean) < 0.05 && DustRatio > 3.0 && RimPart > 0.02 && MarchRatio > 0.0 && std::abs(RefineRatio - 1.0) < 0.03;
	Uni::Check(2, "T2.galaxy.nebula_fronts", "D-243 each spiral nebula split into glowing gas and dark dust by the walls read's fBm against its dark share, a bright front between: the shaders' constants equal the shim's knobs, the normalisations the means they normalise (0.5%); at the default share the light D-241's at full detail (0.5%) and half faded (3%: the even dimming and the split are each normalised over their own detail's shapes; 1.7% before D-256, 2.2% after), the reflection's dust its mean (0.5%; RSPLIT over the samples where the split applies); the structure's dark part the nebulae's mean share (0.05), its dust over 3x the glowing part's, fronts over 2% of the structure; D-244: the march 250 pc back with uu.Nebula.Refine 4 the same light within 3% (the mean of 8 jittered frames); still_nebula_fronts, still_nebula_fronts_march")
		.Metric("lsplit", Lsplit).Metric("rsplit", Rsplit).Metric("dark_part", DarkPart).Metric("share_mean", ShareMean).Metric("dust_ratio", DustRatio).Metric("rim_part", RimPart).Metric("march_ratio", MarchRatio).Metric("refine_ratio", RefineRatio)
		.Pass(bPass, Uni::Fmt("constants: %zu differ%s; measured lsplit %.5f (knob %.5f), rsplit %.5f (%.5f); light x%.4f full, x%.4f half faded (x%.4f of D-241's there), reflection x%.4f; dark part %.3f of the structure (mean share %.3f), dust dark/glowing x%.2f, fronts %.1f%%; the march 250 pc back x%.3f of D-241's, refined x4 x%.4f",
			ConstBad, ConstText.c_str(), Lsplit, double(UNI_NEB_LSPLIT), Rsplit, double(UNI_NEB_RSPLIT), LightFull, LightHalf, LightHalfRel, Refl, DarkPart, ShareMean, DustRatio, 100.0 * RimPart, MarchRatio, RefineRatio));
}
