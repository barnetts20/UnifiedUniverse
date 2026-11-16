// test_diffusedust.cpp
// D-250 (owner 2026-11-15: "some degree of background dust in the galaxy that makes it less likely
// for you to be able to view out to the distance of the entire galactic span from within it - not
// a heavily dense cloud like in the dust lanes or nebula, but a low grade background of dust";
// "it should vary galaxy by galaxy ... randomize it from .75 kpc to 10 kpc"): a dusty galaxy's
// diffuse dust (Uni_GalaxyDiffuseDust), render only.
//  - the reach: log-uniform in [lo, hi] by the galaxy's seed (4000 seeds: inside the range, the
//    geometric middle splits them within 3%, each quarter of the log range within 3% of a quarter);
//  - the profile: 1 at the reference radius in the mid-plane, e^-1 one scale height above, held to
//    UNI_DIFFUSE_DUST_CORE at the centre;
//  - the march: off (gain 0) is D-249's bit for bit; inside the Milky Way (the solar camera, a 2 kpc
//    reach), the march's added optical depth on rays along and near the disc within 4% of the
//    profile integrated finely along the same ray (rays under tau 6: deeper, the march stops once
//    the light is gone; measured 0.1%), and rays out of the plane dimmed less than along it.

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
#include <string>
#include <thread>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	std::vector<H::FUniVolumeResult> DdMarch(const FVolumeScene& Sc, const FCamera& Cm)
	{
		const auto V = ToHlsl(Sc.View);
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		std::vector<H::FUniVolumeResult> Out(size_t(Cm.Width) * Cm.Height);
		std::atomic<int> Next{ 0 };
		auto Work = [&]()
		{
			for (int Y = Next++; Y < Cm.Height; Y = Next++) for (int X = 0; X < Cm.Width; ++X)
			{
				bool bS = false;
				Out[size_t(Y) * Cm.Width + X] = MarchPixel(Cb, Pb, Lb, V, Cm, Cm.Width, Cm.Height, X, Y, 3.0e38f, bS);
			}
		};
		std::vector<std::thread> Ts; for (int T = 1; T < 2; ++T) { Ts.emplace_back(Work); }
		Work(); for (std::thread& T : Ts) { T.join(); }
		return Out;
	}
}

UNI_TEST(volume, diffuse_dust)
{
	// The reach.
	const float Lo = 2.3142581861185255e21f, Hi = 3.0856775814913673e22f;
	size_t Out = 0, Below = 0;
	size_t Quart[4] = {};
	const int Ns = 4000;
	for (int I = 0; I < Ns; ++I)
	{
		H::FUniFieldParams P{};
		P.F.z = float(1000 + 7919 * I % 16000000);
		const float R = H::Uni_GalaxyDiffuseDustReach(P, Lo, Hi);
		if (!(R >= Lo * 0.9999f && R <= Hi * 1.0001f)) { ++Out; }
		const double U = std::log(double(R) / Lo) / std::log(double(Hi) / Lo);
		Below += U < 0.5 ? 1 : 0;
		Quart[std::min(int(U * 4.0), 3)]++;
	}
	double QuartWorst = 0.0;
	for (size_t Q : Quart) { QuartWorst = std::max(QuartWorst, std::abs(double(Q) / Ns - 0.25) / 0.25); }
	const double Split = double(Below) / Ns;

	// The profile.
	H::FUniFieldParams Pe{}, Pd{};
	Pe.A.x = 0.12f; Pe.A.y = 0.01f; Pd.A.y = 0.004f;
	const float Hd = 0.004f * 1.5f;
	const float P0 = H::Uni_GalaxyDiffuseDust(H::float3{ 2.5f * 0.12f, 0.0f, 0.0f }, Pe, Pd, 0u, 1.5f);
	const float P1 = H::Uni_GalaxyDiffuseDust(H::float3{ 0.0f, 2.5f * 0.12f, Hd }, Pe, Pd, 0u, 1.5f);
	const float Pc = H::Uni_GalaxyDiffuseDust(H::float3{ 0.0f, 0.0f, 0.0f }, Pe, Pd, 0u, 1.5f);
	const bool bProfile = std::abs(P0 - 1.0f) < 1e-5f && std::abs(P1 - std::exp(-1.0f)) < 1e-4f && std::abs(Pc - UNI_DIFFUSE_DUST_CORE) < 1e-5f;

	// The march.
	FWebTexture T[3];
	bool bTex = true;
	for (int S = 0; S < 3; ++S) { std::string E; if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bTex = false; } }
	if (!bTex) { Uni::Check(2, "T2.vol.diffuse_dust", "D-250 diffuse dust").Pass(false, "textures not loaded"); return; }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &T[I]); }
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	FUniverseModel Mo(D);
	FRefGenerator G(Mo);
	FVolumeBuilder VB(Mo, G);
	const int W = 48, Ht = 32;
	FCamera Cm;
	{ FCoord C = FCoord::Origin(); FVec3f Fr; AddDisplacement(C, Fr, FVec3d{ -8.0 * Units::Kpc, 0.0, 20.0 * Units::Pc }); Cm.Coord = C; }
	Cm.Width = W; Cm.Height = Ht; Cm.LookAlong({ 0.3, 1.0, 0.0 });
	FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cm.Coord, FVec3f{});
	FVolumeSettings S0;
	const FVolumeScene Sc0 = VB.Build(Pp, Cm.Coord, FVec3f{}, Cm.View(), S0).Subset(false);
	FVolumeSettings S1 = S0;
	S1.DiffuseDustGain = 1.0f; S1.DiffuseDustReachLoCm = S1.DiffuseDustReachHiCm = float(2.0 * Units::Kpc);
	const FVolumeScene Sc1 = VB.Build(Pp, Cm.Coord, FVec3f{}, Cm.View(), S1).Subset(false);
	FVolumeSettings Sz = S0;
	Sz.DiffuseDustGain = 0.0f; Sz.DiffuseDustReachLoCm = Sz.DiffuseDustReachHiCm = float(2.0 * Units::Kpc);
	const FVolumeScene ScZ = VB.Build(Pp, Cm.Coord, FVec3f{}, Cm.View(), Sz).Subset(false);
	const std::vector<H::FUniVolumeResult> R0 = DdMarch(Sc0, Cm), R1 = DdMarch(Sc1, Cm), Rz = DdMarch(ScZ, Cm);
	size_t OffDiff = 0;
	for (size_t I = 0; I < R0.size(); ++I)
	{
		OffDiff += (R0[I].Radiance.x != Rz[I].Radiance.x || R0[I].Radiance.y != Rz[I].Radiance.y || R0[I].Transmittance.y != Rz[I].Transmittance.y) ? 1 : 0;
	}
	// The dusty galaxy's carrier, and the direct integral along the centre row's rays.
	const auto Cb = Sc1.CarrierBuf();
	const auto Pb = Sc1.ParamBuf();
	int Gi = -1;
	for (u32 C = 0; C < Sc1.View.NumCarriers; ++C) { if (Cb[C].FieldId == H::UNI_FIELD_GALAXY && (Cb[C].Flags & H::UNI_CARRIER_DUST) != 0u && Cb[C].NebulaHide < 1.0f) { Gi = int(C); break; } }
	double WorstTau = 0.0, TauPlane = 0.0, TauUp = 0.0;
	if (Gi >= 0)
	{
		const H::FUniCarrier Ca = Cb[u32(Gi)];
		const H::FUniFieldParams Par = Pb[Ca.ParamIndex], DPar = Pb[Ca.DustParamIndex];
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(Par);
		const float ReachF = H::Uni_GalaxyDiffuseDustReach(Par, S1.DiffuseDustReachLoCm, S1.DiffuseDustReachHiCm) * Ca.InvRadius;
		const float Kd0 = (1.0f - Ca.NebulaHide) / ReachF;
		for (int Y = Ht / 2 - 6; Y <= Ht / 2; Y += 3) for (int X = 4; X < W; X += 8)
		{
			const FV3 Dd = PixelDir(Cm, W, Ht, X + 0.5, Y + 0.5);
			const H::float3 Ld = H::Uni_CarrierLocalDir(Ca, H::float3(float(Dd.X), float(Dd.Y), float(Dd.Z)));
			float T0 = 0.0f, T1 = 0.0f;
			if (!H::Uni_CarrierInterval(Ca, Ld, 3.0e38f, T0, T1)) { continue; }
			double Tau = 0.0;
			const int Nst = 20000;
			for (int K2 = 0; K2 < Nst; ++K2)
			{
				const double Tm = T0 + (double(T1) - T0) * (K2 + 0.5) / Nst;
				const H::float3 Pt = H::Uni_CarrierPoint(Ca, Ld, float(Tm));
				Tau += double(Kd0) * H::Uni_GalaxyDiffuseDust(Pt, Par, DPar, K.Hp, S1.DiffuseDustHeight) * ((double(T1) - T0) / Nst) * Ca.InvRadius;
			}
			const size_t I = size_t(Y) * W + X;
			const double Sg = double(Sc1.View.DustSigmaRgb[1]);
			const double TauMarch = -std::log(std::max(double(R1[I].Transmittance.y), 1e-30) / std::max(double(R0[I].Transmittance.y), 1e-30)) / std::max(Sg, 1e-12);
			// Deeper than tau 6 the march stops once the light is gone (measured: 9.37 -> 7.0).
			if (Tau < 6.0) { WorstTau = std::max(WorstTau, std::abs(TauMarch / std::max(Tau, 1e-12) - 1.0)); }
			TauPlane = std::max(TauPlane, Tau);
		}
		// Up and out of the plane: the top row's centre.
		{
			const FV3 Dd = PixelDir(Cm, W, Ht, W / 2 + 0.5, 0.5);
			const size_t I = size_t(W / 2);
			(void)Dd;
			TauUp = -std::log(std::max(double(R1[I].Transmittance.y), 1e-30) / std::max(double(R0[I].Transmittance.y), 1e-30)) / std::max(double(Sc1.View.DustSigmaRgb[1]), 1e-12);
		}
	}
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
	const bool bPass = Out == 0 && std::abs(Split - 0.5) < 0.03 && QuartWorst < 0.03 * 4.0 && bProfile && OffDiff == 0 && Gi >= 0 && WorstTau < 0.04 && TauUp < TauPlane;
	Uni::Check(2, "T2.vol.diffuse_dust", "D-250 a dusty galaxy's diffuse dust: its reach log-uniform in [lo, hi] by seed (4000 seeds in range, the geometric middle splits them within 3%, each log quarter within 3% of a quarter of them); the profile 1 at the reference radius in the mid-plane, e^-1 one scale height up, the centre held to the cap; off is D-249's march bit for bit; from the solar camera with a 2 kpc reach the march's added depth along and near the disc within 4% of the profile integrated finely (rays under tau 6: deeper the march stops once the light is gone), and rays out of the plane dimmed less")
		.Metric("split", Split).Metric("quarter_worst", QuartWorst).Metric("tau_worst", WorstTau).Metric("tau_plane", TauPlane).Metric("tau_up", TauUp)
		.Pass(bPass, Uni::Fmt("reach: %zu out of range, %.3f below the geometric middle, log quarters within %.1f%%; profile %s (%.5f, %.5f, %.3f); off: %zu pixels differ; the march vs the integral along the disc within %.2f%% (deepest tau %.2f: the far side gone), up out of the plane tau %.3f",
			Out, Split, 100.0 * QuartWorst / 4.0, bProfile ? "ok" : "OFF", P0, P1, Pc, OffDiff, 100.0 * WorstTau, TauPlane, TauUp));
}
