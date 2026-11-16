// test_nebsteps.cpp
// D-251 (owner 2026-11-15: "the effect overall is really nice if we could come up with a solid fix
// for the grain noise wed be close to done"; earlier: "my findings support a much finer step
// ratio"): the grain is each step drawing the nebulae's 10-40 pc detail at one jittered point of
// a long step. Over the engine's path (cloud resolve, blue noise, 4 x 4 slicing, 64 still frames,
// the detail to 7 kpc), against a fine reference (an eighth of the steps, unjittered):
//  - 250 pc from the Milky Way's brightest disc nebula: uu.Nebula.StepRatio 0.08 (finer steps through
//    the gas layer within the detail's reach) at most half D-250's error per pixel (measured 0.29x),
//    its frame-to-frame change below D-250's, under 2.5x the steps a ray (measured 1.76x; D-262: 2x before D-256's Milky Way, 2.1-2.3x on it);
//  - along the disc from the solar camera: at most 0.75x D-250's error (measured 0.61x), under
//    2.5x the steps (2x before D-256);
//  - the denoise (uu.VolumeDenoise, one a-trous pass): 0 passes return the resolve bit for bit; one
//    pass on D-250's steps in the nebula at most 0.85x its error (measured 0.75x); its variance 0
//    before a pixel's second sample.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../src/UniBlueNoise.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	double NsLum(const H::float4& V) { return 0.2126 * V.x + 0.7152 * V.y + 0.0722 * V.z; }

	struct FNsRun { std::vector<H::float4> Final; std::vector<H::float4> Vol; std::vector<float> Var; std::vector<H::float2> Dep; float Exp = 1.0f; double Flick = 0.0; double StepsPerRay = 0.0; };
}

UNI_TEST(volume, nebula_steps)
{
	FWebTexture T[3];
	bool bTex = true;
	for (int S = 0; S < 3; ++S) { std::string E; if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bTex = false; } }
	if (!bTex) { Uni::Check(2, "T2.vol.nebula_steps", "D-251 nebula steps").Pass(false, "textures not loaded"); return; }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &T[I]); }
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	const FFieldParams* Mw = nullptr;
	for (const FAnchor& A : D.Anchors) { if (!A.EmitterParams.empty()) { Mw = &A.EmitterParams[0]; } }
	if (!Mw) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } Uni::Check(2, "T2.vol.nebula_steps", "D-251").Pass(false, "no anchor"); return; }
	const H::FUniFieldParams P = Mw->ToHlsl();
	const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
	// The brightest disc nebula (as T2.galaxy.nebula_detail).
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
	FUniverseModel Mo(D);
	FRefGenerator G(Mo);
	FVolumeBuilder VB(Mo, G);
	double Radius = 0.0;
	auto CamAt = [](const FVec3d& Pc) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, Pc); return C; };
	auto Build = [&](const FCamera& Cm, float NebStep)
	{
		FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cm.Coord, FVec3f{});
		FVolumeSettings Sv; Sv.NebulaGain = 1.0f; Sv.NebulaReflect = 6.0f; Sv.NebulaFineReachCm = float(7.0 * Units::Kpc); Sv.NebulaStepRatio = NebStep;
		ApplyNebulaTune(NebulaTune(Sv));
		FVolumeScene Sc = VB.Build(Pp, Cm.Coord, FVec3f{}, Cm.View(), Sv).Subset(false);
		for (const FCarrierGPU& Cg : Sc.Carriers)
		{
			const double Rl = std::sqrt(double(Cg.CamLocal[0]) * Cg.CamLocal[0] + double(Cg.CamLocal[1]) * Cg.CamLocal[1] + double(Cg.CamLocal[2]) * Cg.CamLocal[2]);
			if (Cg.FieldId == H::UNI_FIELD_GALAXY && Rl < 1.0) { Radius = 1.0 / double(Cg.InvRadius); }
		}
		return Sc;
	};
	{ FCamera Cm; Cm.Coord = CamAt({ 8.0 * Units::Kpc, 0.0, 0.0 }); Cm.Width = 4; Cm.Height = 4; Cm.LookAlong({ 0.0, 1.0, 0.0 }); Build(Cm, 0.0f); }
	const int W = 96, Ht = 54;
	auto Reference = [&](const FCamera& Cm)
	{
		FVolumeScene Sf = Build(Cm, 0.0f);
		Sf.View.MarchFlags |= 2u; Sf.View.MaxSteps = 16384u;
		const auto Cb = Sf.CarrierBuf(); const auto Pb = Sf.ParamBuf(); const auto Lb = Sf.LutBuf();
		const auto V = ToHlsl(Sf.View);
		std::vector<H::float4> Ref(size_t(W) * Ht);
		for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
		{
			bool bS = false;
			const H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cm, W, Ht, X, Y, 3.0e38f, bS);
			Ref[size_t(Y) * W + X] = H::float4(R.Radiance.x, R.Radiance.y, R.Radiance.z, 0.0f);
		}
		return Ref;
	};
	UniVerify::FBlueNoiseBind Bn(true);
	auto Run = [&](const FCamera& Cm, float NebStep)
	{
		FVolumeScene Sc = Build(Cm, NebStep);
		Sc.View.MarchFlags |= H::UNI_VOL_JITTER_FLAG;
		FTemporalVolume Tv;
		Tv.MaxSamples = 16.0f; Tv.bCloud = true; Tv.Interleave = 4;
		FNsRun R;
		std::vector<H::float4> Last;
		size_t Nf = 0;
		for (int F = 0; F < 64; ++F)
		{
			Tv.Step(Sc, Cm, FV3{ 0, 0, 0 }, true, true, 2);
			if (F >= 48)
			{
				if (!Last.empty()) { for (size_t I = 0; I < Last.size(); ++I) { R.Flick += std::abs(NsLum(Tv.Vol[I]) - NsLum(Last[I])); } ++Nf; }
				Last = Tv.Vol;
			}
		}
		R.Flick /= double(std::max<size_t>(Nf, 1)) * double(Last.size());
		R.Vol = Tv.Vol; R.Var = Tv.Var; R.Dep = Tv.Depth; R.Exp = Tv.LastExp;
		R.StepsPerRay = double(Tv.Steps) / double(std::max<size_t>(Tv.Marched, 1));
		return R;
	};
	auto Err = [&](const std::vector<H::float4>& A, const std::vector<H::float4>& Ref)
	{
		double M = 0.0; for (const H::float4& V : Ref) { M += NsLum(V) / Ref.size(); }
		double E = 0.0; for (size_t I = 0; I < A.size(); ++I) { E += std::abs(NsLum(A[I]) - NsLum(Ref[I])) / std::max(M, 1e-300) / A.size(); }
		return E;
	};
	// 250 pc from the nebula.
	FCamera Cn; Cn.Coord = CamAt({ Hx * Radius, Hy * Radius - 250.0 * Units::Pc, 5.0 * Units::Pc }); Cn.Width = W; Cn.Height = Ht; Cn.LookAlong({ 0.3, 1.0, 0.05 });
	const std::vector<H::float4> RefN = Reference(Cn);
	const FNsRun N0 = Run(Cn, 0.0f), N1 = Run(Cn, 0.08f);
	const double En0 = Err(N0.Vol, RefN), En1 = Err(N1.Vol, RefN);
	double MeanN = 0.0; for (const H::float4& V : RefN) { MeanN += NsLum(V) / RefN.size(); }
	// The denoise on D-250's steps.
	const std::vector<H::float4> Dn0 = DenoiseVolume(N0.Vol, N0.Var, N0.Dep, W, Ht, N0.Exp, 0);
	size_t Bit = 0;
	for (size_t I = 0; I < Dn0.size(); ++I) { Bit += (Dn0[I].x != N0.Vol[I].x || Dn0[I].y != N0.Vol[I].y || Dn0[I].z != N0.Vol[I].z || Dn0[I].w != N0.Vol[I].w) ? 1 : 0; }
	const double Ed = Err(DenoiseVolume(N0.Vol, N0.Var, N0.Dep, W, Ht, N0.Exp, 1), RefN);
	const bool bVar = H::Uni_DenoiseVariance(H::float2(0.5f, 0.3f), 1.0f, 4.0f) == 0.0f && std::abs(H::Uni_DenoiseVariance(H::float2(0.5f, 0.3f), 2.0f, 4.0f) - 0.0125f) < 1e-6f;
	// Along the disc from the solar camera.
	FCamera Cd; Cd.Coord = CamAt({ -8.0 * Units::Kpc, 0.0, 20.0 * Units::Pc }); Cd.Width = W; Cd.Height = Ht; Cd.LookAlong({ 1.0, 0.25, -0.02 });
	const std::vector<H::float4> RefD = Reference(Cd);
	const FNsRun D0 = Run(Cd, 0.0f), D1 = Run(Cd, 0.08f);
	const double Ed0 = Err(D0.Vol, RefD), Ed1 = Err(D1.Vol, RefD);
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
	ApplyNebulaTune(FNebulaTune{});
	const double SnRatio = N1.StepsPerRay / std::max(N0.StepsPerRay, 1e-9), SdRatio = D1.StepsPerRay / std::max(D0.StepsPerRay, 1e-9);
	const bool bPass = En1 < 0.5 * En0 && N1.Flick < N0.Flick && SnRatio < 2.5 && Ed1 < 0.75 * Ed0 && SdRatio < 2.5 && Bit == 0 && Ed < 0.85 * En0 && bVar;
	Uni::Check(2, "T2.vol.nebula_steps", "D-251 the grain: over the engine's path (cloud resolve, blue noise, 4 x 4, 64 still frames, detail to 7 kpc) against a fine reference, uu.Nebula.StepRatio 0.08 (finer steps through the gas layer within the detail's reach) 250 pc from the Milky Way's brightest nebula at most half D-250's error per pixel with less frame-to-frame change, along the disc at most 0.75x, both under 2.5x the steps a ray (2x before D-256's Milky Way); the denoise's 0 passes bit for bit, one pass on D-250's steps at most 0.85x its error in the nebula, its variance 0 before a pixel's second sample")
		.Metric("nebula_err_d250", En0).Metric("nebula_err", En1).Metric("disc_err_d250", Ed0).Metric("disc_err", Ed1).Metric("nebula_steps_x", SnRatio).Metric("disc_steps_x", SdRatio).Metric("denoise_err", Ed)
		.Pass(bPass, Uni::Fmt("nebula: per pixel %.2f%% -> %.2f%% (x%.2f), frame change %.3f%% -> %.3f%%, steps a ray %.1f -> %.1f (x%.2f); disc: %.2f%% -> %.2f%% (x%.2f), steps %.1f -> %.1f (x%.2f); denoise: 0 passes %zu values differ, one pass on D-250's steps %.2f%% (x%.2f); variance rule %s",
			100.0 * En0, 100.0 * En1, En1 / En0, 100.0 * N0.Flick / std::max(MeanN, 1e-300), 100.0 * N1.Flick / std::max(MeanN, 1e-300), N0.StepsPerRay, N1.StepsPerRay, SnRatio,
			100.0 * Ed0, 100.0 * Ed1, Ed1 / Ed0, D0.StepsPerRay, D1.StepsPerRay, SdRatio, Bit, 100.0 * Ed, Ed / En0, bVar ? "ok" : "OFF"));
}
