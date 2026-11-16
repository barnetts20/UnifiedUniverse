// test_nebdark.cpp
// D-242 (owner 2026-11-14: "nebula currently just have an emissive component ... in reality they
// would have an emissive component and a non emissive component similar to the galaxy dust lanes
// ... another variance handle, basically the balance of luminous dust to non luminous dust"):
// each spiral nebula's dark share (Uni_GalaxyNebulaDarkShare) around the view's mean
// (uu.Nebula.Dark) trades its glow for dust in its structure.
//  - the constants: the shaders' defines equal the shim's knobs, and v's light-weighted means
//    (UNI_NEB_DARK_VPS, VMS) are what they normalise, measured over the Milky Way's gas layer;
//  - at the default share the light keeps D-241's mean (unblended, and blended exactly), the dark
//    clouds add dust where the structure is, blended the dust is D-241's; the shares spread (dark-dominated and glowing nebulae both common) and vary apart
//    from the nebula's brightness and its filaments-or-shells;
//  - the handle: at shares 0.1 and 0.6 the light follows the model's mean, blended and unblended
//    alike, the dark dust the share; off (below 0) is D-241 bit for bit; early types unchanged;
//  - still_nebula_dark: in the Milky Way's mid-plane 400 pc from its brightest nebula, looking
//    along the arm, at shares 0, 0.35, 0.8.

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
	struct FNkTex
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FNkTex()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};

	struct FNkBind
	{
		explicit FNkBind(const FNkTex& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FNkBind() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	struct FNkSample { H::float3 P; H::float4 Neb; };

	FCoord NkCamAt(const FVec3d& P) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, P); return C; }

	std::vector<float> NkMarch(const FVolumeScene& Sc, const FCamera& Cm, u32 Flags, float Shift)
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

	/** D-242's density with the reflection's multiplier optional. */
	float NkDens(const H::float3& InP, const H::float4& InNeb, const H::FUniFieldParams& InPar, float InFoot, float InLod, float InDetail, float InDark, float& OutX, float& OutDust, float* OutReflect = nullptr)
	{
		float R = 1.0f;
		const float V = H::Uni_GalaxyNebulaDensityAt(InP, InNeb, InPar, InFoot, InLod, InDetail, InDark, OutX, OutDust, R);
		if (OutReflect) { *OutReflect = R; }
		return V;
	}

	double NkLum(const std::vector<float>& F, size_t I) { return 0.2126 * F[3 * I] + 0.7152 * F[3 * I + 1] + 0.0722 * F[3 * I + 2]; }

	double NkCorr(const std::vector<double>& A, const std::vector<double>& B, const std::vector<double>& W)
	{
		double Sw = 0, Ma = 0, Mb = 0;
		for (size_t I = 0; I < A.size(); ++I) { Sw += W[I]; Ma += W[I] * A[I]; Mb += W[I] * B[I]; }
		Ma /= Sw; Mb /= Sw;
		double Cab = 0, Caa = 0, Cbb = 0;
		for (size_t I = 0; I < A.size(); ++I) { Cab += W[I] * (A[I] - Ma) * (B[I] - Mb); Caa += W[I] * (A[I] - Ma) * (A[I] - Ma); Cbb += W[I] * (B[I] - Mb) * (B[I] - Mb); }
		return Cab / std::sqrt(std::max(Caa * Cbb, 1e-300));
	}
}

UNI_TEST(galaxy, nebula_dark)
{
	const FNkTex Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.nebula_dark", "D-242 the nebulae's dark share").Pass(false, "textures not loaded: " + Tx.Err); return; }
	const FNkBind Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	const FFieldParams* Mw = nullptr;
	for (const FAnchor& A : D.Anchors) { if (!A.EmitterParams.empty()) { Mw = &A.EmitterParams[0]; } }
	if (!Mw) { Uni::Check(2, "T2.galaxy.nebula_dark", "D-242").Pass(false, "no anchor"); return; }
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
	const FKn Kn[] = { { "UNI_NEB_DARK_MEAN0", H::GKnobNebDarkMean0 }, { "UNI_NEB_DARK_SPREAD", H::GKnobNebDarkSpread }, { "UNI_NEB_DARK_C", H::GKnobNebDarkC },
		{ "UNI_NEB_DARK_DUST", H::GKnobNebDarkDust }, { "UNI_NEB_DARK_VPS", H::GKnobNebDarkVps }, { "UNI_NEB_DARK_VMS", H::GKnobNebDarkVms },
		{ "UNI_NEB_DARK_VPR", H::GKnobNebDarkVpr }, { "UNI_NEB_DARK_VMR", H::GKnobNebDarkVmr }, { "UNI_NEB_DARK_EMIT", H::GKnobNebDarkEmit } };
	size_t ConstBad = 0;
	std::string ConstText;
	for (const FKn& C : Kn)
	{
		const double V = Def(C.Name);
		if (!(std::abs(V - C.Knob) <= 1e-6 * std::max(std::abs(C.Knob), 1e-6))) { ++ConstBad; ConstText += Uni::Fmt(" %s %g vs %g;", C.Name, V, C.Knob); }
	}
	bool bVolDefault = false;
	{
		FVolumeSettings Sv;
		bVolDefault = Sv.NebulaDark == float(UNI_NEB_DARK_MEAN0);
	}

	// The gas layer of an annulus (footprint ~0), with the knots' source.
	std::vector<FNkSample> S;
	const int N = 240000;
	for (int I = 0; I < N; ++I)
	{
		const double R = 0.2 + 0.5 * H::Uni_DrawUnit(0xA12u, 2u, u32(3 * I)), Ph = 6.2831853 * H::Uni_DrawUnit(0xA12u, 2u, u32(3 * I + 1));
		const double Z = (2.0 * H::Uni_DrawUnit(0xA12u, 2u, u32(3 * I + 2)) - 1.0) * 0.5 * double(P.A.y);
		const H::float3 Pt{ float(R * std::cos(Ph)), float(R * std::sin(Ph)), float(Z) };
		H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
		H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-6f, C, Nv);
		if (Nv.x > 0.0f) { S.push_back({ Pt, Nv }); }
	}
	const float FootBlend = 2.0f * UNI_NEBULA_FOOT * std::exp2(UNI_NEBULA_MIP) / (P.G.w * std::max(P.H.w, 1.0f));
	// The normalisation, measured: v's positive and negative means weighted by D-241's light. D-243:
	// all at detail 0 (the even dimming applies where the walls are not read; with them, the
	// fronts: T2.galaxy.nebula_fronts).
	double Ssh = 0, Svp = 0, Svm = 0, Smd = 0, Srp = 0, Srm = 0;
	std::vector<double> Av, Jv, Wv, Wt;
	size_t Off = 0;
	for (const FNkSample& Q : S)
	{
		float X = 0.0f, Md = 1.0f;
		const float Sh = H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, 0.0f, 0.0f, X, Md);
		const double V = H::Uni_GalaxyNebulaDarkV(Q.Neb);
		const double Lw = double(Q.Neb.x) * Sh;   // the light: the source x the shape
		Ssh += Lw; Svp += Lw * std::max(V, 0.0); Svm += Lw * std::max(-V, 0.0);
		Smd += Md; Srp += Md * std::max(V, 0.0); Srm += Md * std::max(-V, 0.0);   // the reflection's: D-241's dust
		if (X > 0.0f) { Av.push_back(H::Uni_GalaxyNebulaDarkShare(Q.Neb, UNI_NEB_DARK_MEAN0)); Jv.push_back(Q.Neb.w); Wv.push_back(Q.Neb.z); Wt.push_back(X); }
		// Off: D-241 bit for bit.
		float X0 = 0, Md0 = 0;
		const float D0 = NkDens(Q.P, Q.Neb, P, 0.0f, 0.0f, 0.0f, -1.0f, X0, Md0);
		Off += (D0 != Q.Neb.x * UNI_NEBULA_SCALE * Sh || Md0 != Md || X0 != X) ? 1 : 0;
	}
	const double Vps = Svp / std::max(Ssh, 1e-300), Vms = Svm / std::max(Ssh, 1e-300), Vpr = Srp / std::max(Smd, 1e-300), Vmr = Srm / std::max(Smd, 1e-300);
	auto Near = [](double A, double B) { return std::abs(A - B) < 0.002 + 0.01 * std::abs(B); };
	const bool bNorm = Near(Vps, UNI_NEB_DARK_VPS) && Near(Vms, UNI_NEB_DARK_VMS) && Near(Vpr, UNI_NEB_DARK_VPR) && Near(Vmr, UNI_NEB_DARK_VMR);
	// The means at three shares, over D-241's: the light (unblended, fully blended, the model's);
	// the dust over the source region and where the structure is (the dark dust added), blended
	// (D-241's exactly: unresolved clouds cover little of a pixel); the reflection's dust.
	struct FAt { double Share = 0, Light = 0, DustS = 0, DustX = 0, LightB = 0, DustB = 0, LightM = 0, Refl = 0; };
	FAt At[3];
	At[0].Share = 0.1; At[1].Share = double(UNI_NEB_DARK_MEAN0); At[2].Share = 0.6;
	for (FAt& T : At)
	{
		double L = 0, L0 = 0, Ds = 0, Ds0 = 0, Dx = 0, Dx0 = 0, Lb = 0, Lb0 = 0, Db = 0, Rf = 0;
		for (const FNkSample& Q : S)
		{
			float X = 0, Md = 1, X0 = 0, Md0 = 1, Mr = 1;
			L += NkDens(Q.P, Q.Neb, P, 0.0f, 0.0f, 0.0f, float(T.Share), X, Md, &Mr);
			Rf += Mr;
			L0 += NkDens(Q.P, Q.Neb, P, 0.0f, 0.0f, 0.0f, -1.0f, X0, Md0);
			Ds += Md; Ds0 += Md0;
			if (X0 > 0.0f) { Dx += Md; Dx0 += Md0; }
			float Xb = 0, Mdb = 1, Xb0 = 0, Mdb0 = 1;
			Lb += NkDens(Q.P, Q.Neb, P, FootBlend, 0.0f, 0.0f, float(T.Share), Xb, Mdb);
			Lb0 += NkDens(Q.P, Q.Neb, P, FootBlend, 0.0f, 0.0f, -1.0f, Xb0, Mdb0);
			Db += Mdb;
		}
		T.Light = L / std::max(L0, 1e-300); T.DustS = Ds / std::max(Ds0, 1e-300); T.DustX = Dx / std::max(Dx0, 1e-300); T.Refl = Rf / std::max(Ds0, 1e-300);
		T.LightB = Lb / std::max(Lb0, 1e-300); T.DustB = Db / std::max<size_t>(S.size(), 1);
		T.LightM = (1.0 - double(UNI_NEB_DARK_EMIT) * double(H::Uni_GalaxyNebulaDarkMean(float(T.Share), UNI_NEB_DARK_VPS, UNI_NEB_DARK_VMS))) / (1.0 - double(UNI_NEB_DARK_EMIT) * double(H::Uni_GalaxyNebulaDarkMean(UNI_NEB_DARK_MEAN0, UNI_NEB_DARK_VPS, UNI_NEB_DARK_VMS)));
	}
	const FAt& Dflt = At[1];
	const bool bDefault = std::abs(Dflt.Light - 1.0) < 0.005 && std::abs(Dflt.LightB - 1.0) < 1e-5 && std::abs(Dflt.Refl - 1.0) < 0.005 && Dflt.DustX > 1.5;
	bool bHandle = At[0].Light > 1.0 && At[2].Light < 1.0 && At[0].DustX < Dflt.DustX && At[2].DustX > Dflt.DustX;
	for (int I : { 0, 1, 2 }) { bHandle = bHandle && std::abs(At[I].Light / At[I].LightM - 1.0) < 0.01 && std::abs(At[I].LightB / At[I].LightM - 1.0) < 1e-4 && std::abs(At[I].DustB - 1.0) < 1e-6; }
	// The spread: dark-dominated (a > 0.6) and glowing (a < 0.15) nebulae, X-weighted; and the share
	// against the brightness (w) and the filaments-or-shells (z).
	double Wsum = 0, Wdark = 0, Wglow = 0;
	for (size_t I = 0; I < Av.size(); ++I) { Wsum += Wt[I]; Wdark += Av[I] > 0.6 ? Wt[I] : 0.0; Wglow += Av[I] < 0.15 ? Wt[I] : 0.0; }
	const double Dark = Wdark / std::max(Wsum, 1e-300), Glow = Wglow / std::max(Wsum, 1e-300);
	const double Rj = NkCorr(Av, Jv, Wt), Rw = NkCorr(Av, Wv, Wt);
	const bool bSpread = Dark > 0.1 && Glow > 0.2 && std::abs(Rj) < 0.2 && std::abs(Rw) < 0.2;
	// Early types: unchanged.
	size_t EarlyBad = 0;
	{
		const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
		for (u32 I = 0; I < 32u; ++I)
		{
			FFieldParams Pf = E.EarlyExemplar;
			for (const FParamRange& R : E.EarlyRanges) { Pf.V[R.Slot] = H::Uni_RollRange(H::Uni_SeedMix(0xF375u, I), ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
			Pf.V[20] = 1.0f;
			const H::FUniFieldParams Pq = Pf.ToHlsl();
			const H::float4 Nv(1.0f, 0.567f * 1.3f, 0.5f, 0.6f);
			float X = 0, Md = 0, X0 = 0, Md0 = 0;
			const float A = NkDens(H::float3{ 0.01f, 0.02f, 0.0f }, Nv, Pq, 0.0f, 0.0f, 1.0f, 0.7f, X, Md);
			const float B = NkDens(H::float3{ 0.01f, 0.02f, 0.0f }, Nv, Pq, 0.0f, 0.0f, 1.0f, -1.0f, X0, Md0);
			EarlyBad += (A != B || Md != Md0) ? 1 : 0;
		}
	}

	// still_nebula_dark: the mean of 8 jittered frames at shares 0, the default and 0.7.
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
	double LightRatio[3] = { 0, 0, 0 };
	{
		FUniverseModel Mo(D);
		FRefGenerator G(Mo);
		FVolumeBuilder VB(Mo, G);
		double Radius = 0.0;
		auto Build = [&](const FCoord& Cam, const FCamera& Cm, float Dark)
		{
			FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cam, FVec3f{});
			FVolumeSettings Sv; Sv.NebulaGain = 1.0f; Sv.NebulaReflect = 6.0f; Sv.NebulaDark = Dark;
			FVolumeScene Sc = VB.Build(Pp, Cam, FVec3f{}, Cm.View(), Sv).Subset(false);
			for (const FCarrierGPU& Cg : Sc.Carriers)
			{
				const double Rl = std::sqrt(double(Cg.CamLocal[0]) * Cg.CamLocal[0] + double(Cg.CamLocal[1]) * Cg.CamLocal[1] + double(Cg.CamLocal[2]) * Cg.CamLocal[2]);
				if (Cg.FieldId == H::UNI_FIELD_GALAXY && Rl < 1.0) { Radius = 1.0 / double(Cg.InvRadius); }
			}
			return Sc;
		};
		{ FCamera Cm; Cm.Coord = NkCamAt({ 8.0 * Units::Kpc, 0.0, 0.0 }); Cm.Width = 4; Cm.Height = 4; Cm.LookAlong({ 0.0, 1.0, 0.0 }); Build(Cm.Coord, Cm, 0.35f); }
		// In the disc's mid-plane 400 pc from the brightest knot, looking along the arm through it:
		// the nebulae against the disc's light behind them (dark clouds show against a background;
		// from above the disc the knots' young stars above the gas layer outshine them).
		const int W = 192, Ht = 108;
		FCamera Cm; Cm.Coord = NkCamAt({ Hx * Radius, Hy * Radius - 400.0 * Units::Pc, 3.0 * Units::Pc }); Cm.Width = W; Cm.Height = Ht; Cm.LookAlong({ 0.15, 1.0, 0.03 });
		const float Shares[3] = { 0.0f, float(UNI_NEB_DARK_MEAN0), 0.8f };
		std::vector<float> Fr[3];
		for (int Q = 0; Q < 3; ++Q)
		{
			const FVolumeScene Sc = Build(Cm.Coord, Cm, Shares[Q]);
			Fr[Q].assign(size_t(W) * Ht * 3, 0.0f);
			for (int Kj = 0; Kj < 8; ++Kj)
			{
				const std::vector<float> J = NkMarch(Sc, Cm, 8u, float(std::fmod(Kj * 0.6180339887, 1.0)));
				for (size_t I = 0; I < J.size(); ++I) { Fr[Q][I] += J[I] / 8.0f; }
			}
		}
		const size_t Np = size_t(W) * Ht;
		std::vector<double> L; for (size_t I = 0; I < Np; ++I) { L.push_back(NkLum(Fr[1], I)); }
		std::sort(L.begin(), L.end());
		const double Top = L[size_t(0.995 * (L.size() - 1))];
		double Sum[3] = { 0, 0, 0 };
		FImage Img(W * 3, Ht);
		for (int Q = 0; Q < 3; ++Q) for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
		{
			const std::vector<float>& F = Fr[Q];
			const size_t I = size_t(Y) * W + X;
			Sum[Q] += NkLum(F, I);
			const double Lr = NkLum(F, I) / std::max(Top, 1e-300);
			const double Sc2 = Lr > 0.0 ? (Lr / (1.0 + 0.25 * Lr)) / Lr / std::max(Top, 1e-300) : 0.0;
			Img.Add(Q * W + X, Y, float(F[3 * I] * Sc2), float(F[3 * I + 1] * Sc2), float(F[3 * I + 2] * Sc2));
		}
		for (int Q = 0; Q < 3; ++Q) { LightRatio[Q] = Sum[Q] / std::max(Sum[1], 1e-300); }
		const std::string Dir = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755); ::mkdir(Dir.c_str(), 0755);
		Img.WritePpm(Dir + "/still_nebula_dark.ppm");
		if (FILE* Fh = std::fopen((Dir + "/still_nebula_dark.hud.json").c_str(), "wb"))
		{
			std::fprintf(Fh, "{\"lines\":[\"D-242: in the Milky Way's mid-plane 400 pc from its brightest nebula, looking along the arm, the mean of 8 jittered frames, dark share 0 (left: all glow), 0.35 (the default), 0.8 (right: mostly dust)\"]}\n");
			std::fclose(Fh);
		}
	}

	const bool bPass = ConstBad == 0 && bVolDefault && bNorm && Off == 0 && bDefault && bHandle && bSpread && EarlyBad == 0 && LightRatio[0] > 1.0 && LightRatio[2] < 1.0;
	Uni::Check(2, "T2.galaxy.nebula_dark", "D-242 each spiral nebula's dark share around uu.Nebula.Dark: the shaders' constants equal the shim's knobs and the volume settings' default is the shaders'; v's light- and dust-weighted means measured over the Milky Way's gas layer (0.002 + 1%); at the default share the light keeps D-241's mean (0.5%), the reflection's dust 0.5%, the dust where the structure is up at least 1.5x (the dark clouds added); at shares 0.1, the default and 0.6 the light is the model's (1%) and blended exactly, blended the dust D-241's exactly, the dark dust moving with the share; over 10% of the nebulae dark-dominated (share > 0.6) and over 20% glowing (< 0.15), the share uncorrelated with the brightness and the filaments-or-shells (|r| < 0.2); off: D-241 bit for bit; early types unchanged; the march brighter at share 0 and darker at 0.8; still_nebula_dark")
		.Metric("vps", Vps).Metric("vms", Vms).Metric("dust_structure", Dflt.DustX).Metric("dark_share", Dark).Metric("glow_share", Glow).Metric("r_brightness", Rj).Metric("r_shells", Rw)
		.Pass(bPass, Uni::Fmt("constants: %zu differ%s, settings default %s; measured vps %.5f (knob %.5f) vms %.5f (%.5f) vpr %.5f (%.5f) vmr %.5f (%.5f); default: light x%.4f, dust x%.4f (structure x%.3f), reflection x%.4f, blended x%.6f / %.6f; share 0.1: light x%.4f (model %.4f, blended %.4f), dust x%.4f (structure x%.3f); 0.6: light x%.4f (model %.4f, blended %.4f), dust x%.4f (structure x%.3f); dark-dominated %.1f%%, glowing %.1f%%, r brightness %+.3f shells %+.3f; off %zu differ; early %zu differ; march light x%.3f at 0, x%.3f at 0.8",
			ConstBad, ConstText.c_str(), bVolDefault ? "ok" : "DIFFERS", Vps, double(UNI_NEB_DARK_VPS), Vms, double(UNI_NEB_DARK_VMS), Vpr, double(UNI_NEB_DARK_VPR), Vmr, double(UNI_NEB_DARK_VMR),
			Dflt.Light, Dflt.DustS, Dflt.DustX, Dflt.Refl, Dflt.LightB, Dflt.DustB, At[0].Light, At[0].LightM, At[0].LightB, At[0].DustS, At[0].DustX, At[2].Light, At[2].LightM, At[2].LightB, At[2].DustS, At[2].DustX,
			100.0 * Dark, 100.0 * Glow, Rj, Rw, Off, EarlyBad, LightRatio[0], LightRatio[2]));
}
