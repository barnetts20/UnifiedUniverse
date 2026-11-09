// test_volperf.cpp
// D-195 (owner's capture: inside a galaxy, Uni.Volume is 20-31 ms of a 24-36 ms frame, 85-90%
// of the GPU). The volume march from inside the Milky Way: steps per pixel, active carriers,
// supersampled pixels and time for the D-129..D-194 settings (step ratio 0.08, hints x 1, local
// carriers from 1 pc) and the current defaults (0.32 since D-199, hints x 3 in carriers around the camera since D-198, from 10 pc), each against a fine
// reference (0.02, hints x 1, from 1 pc, 20000 steps).
//
// The sweep that chose the D-195 defaults (same views; steps/px toward the centre, along the
// disc, out of the plane; p95 per-pixel error against the reference):
//   0.08 (old)           198.5 / 158.4 / 134.3
//   0.24                 136.9 /  87.4 /  67.7   p95 0.1%
//   0.24, hints x 2       85.5 /  63.2 /  51.8   p95 0.2-0.3%
//   0.24, hints x 4       61.8 /  53.9 /  46.9   p95 0.4-0.5%
//   0.24, no hints        48.5 /  51.6 /  46.7   p95 6.4% toward the centre (the hints matter there)
//   0.24, from 10 pc     126.2 /  75.7 /  57.0   p95 0.1%
//   0.24, from 100 pc    115.5 /  64.0 /  46.3   p95 2.7-3.0% (near dust and stars lost)
// D-195 shipped hints x 2: 74.8 / 52.2 / 41.1 steps/px, p95 0.21-0.33%. D-198, hints x 3:
// 58.6 / 45.8 / 37.4, p95 0.27-0.41% (3.4-3.6x fewer than 0.08); T3.vol.march 0.67% worst.
// D-199, ratio 0.32 (hints x 3): 52.8 / 38.9 / 30.7, p95 0.40-0.61%; T3.vol.march 0.72%. Rejected:
// 0.40 x 3 (49.5 / 34.9 / 26.9, p95 0.78%); 0.32 x 4 (44.8 / 35.8 / 28.9, but T3.vol.march 2.2%
// per ray toward the centre).
// The looser hints apply only to carriers the camera is inside: from outside, a ray crosses a
// disc steeply and x 2 costs 5% per ray (T3.vol.march, face-on at 60 kpc, unprefiltered).

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_set>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FMarchRun
	{
		std::vector<double> Lum;   // per pixel radiance x solid angle (luminance)
		double Steps = 0.0;        // mean steps per pixel (all sub-rays)
		double Active = 0.0;       // mean active carriers per pixel
		double Super = 0.0;        // share of pixels supersampled
		double Ms = 0.0;
		u32 MaxSteps = 0;          // most steps in a pixel (all its sub-rays)
		u64 Truncated = 0;         // rays that ran out of steps
	};

	FMarchRun MarchAll(const FVolumeScene& Sc, const FCamera& Cam, const H::FUniVolumeView& V, int Threads = 2)
	{
		FMarchRun R;
		const int W = Cam.Width, Ht = Cam.Height;
		R.Lum.assign(size_t(W) * Ht, 0.0);
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		std::vector<double> Steps(size_t(Threads), 0.0), Act(size_t(Threads), 0.0), Sup(size_t(Threads), 0.0);
		std::vector<u32> Mx(size_t(Threads), 0u);
		std::vector<u64> Tr(size_t(Threads), 0u);
		std::atomic<int> Next{ 0 };
		const auto T0 = std::chrono::steady_clock::now();
		auto Work = [&](int Tid)
		{
			for (int Y = Next++; Y < Ht; Y = Next++)
			{
				for (int X = 0; X < W; ++X)
				{
					bool bSuper = false;
					const H::FUniVolumeResult Rs = MarchPixel(Cb, Pb, Lb, V, Cam, W, Ht, X, Y, 3.0e38f, bSuper);
					R.Lum[size_t(Y) * W + X] = H::Uni_Luminance(Rs.Radiance) * PixelSolidAngle(Cam, W, Ht, X + 0.5, Y + 0.5);
					Steps[size_t(Tid)] += Rs.Steps;
					Act[size_t(Tid)] += Rs.Active;
					Sup[size_t(Tid)] += bSuper ? 1.0 : 0.0;
					Mx[size_t(Tid)] = std::max(Mx[size_t(Tid)], Rs.Steps);
					Tr[size_t(Tid)] += Rs.Truncated;
				}
			}
		};
		std::vector<std::thread> Ts;
		for (int T = 1; T < Threads; ++T) { Ts.emplace_back(Work, T); }
		Work(0);
		for (auto& T : Ts) { T.join(); }
		R.Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
		const double N = double(W) * Ht;
		for (int T = 0; T < Threads; ++T) { R.Steps += Steps[size_t(T)] / N; R.Active += Act[size_t(T)] / N; R.Super += Sup[size_t(T)] / N; R.MaxSteps = std::max(R.MaxSteps, Mx[size_t(T)]); R.Truncated += Tr[size_t(T)]; }
		return R;
	}

	/** Total light error and the 95th percentile of per-pixel error over lit pixels (above 1%
	 *  of the mean), against a reference. */
	void Compare(const FMarchRun& A, const FMarchRun& Ref, double& OutLight, double& OutP95)
	{
		double Sa = 0.0, Sr = 0.0;
		for (size_t I = 0; I < Ref.Lum.size(); ++I) { Sa += A.Lum[I]; Sr += Ref.Lum[I]; }
		OutLight = std::abs(Sa / std::max(Sr, 1e-300) - 1.0);
		const double Mean = Sr / double(Ref.Lum.size());
		std::vector<double> E;
		for (size_t I = 0; I < Ref.Lum.size(); ++I) { if (Ref.Lum[I] > 0.01 * Mean) { E.push_back(std::abs(A.Lum[I] - Ref.Lum[I]) / Ref.Lum[I]); } }
		std::sort(E.begin(), E.end());
		OutP95 = E.empty() ? 0.0 : E[size_t(0.95 * double(E.size() - 1))];
	}
}

UNI_TEST(volume, perf_inside)
{
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::SolarCamera();
	P.Update(Cam, FVec3f{});
	struct FView { const char* Name; FV3 Look; };
	const FView Views[] = { { "toward the centre", { -1.0, 0.05, 0.02 } }, { "along the disc", { 0.05, 1.0, 0.02 } }, { "out of the plane", { 0.3, 0.2, 1.0 } } };

	FVolumeSettings RefS;
	RefS.StepRatio = 0.02f; RefS.HintScale = 1.0f; RefS.LocalStartCm = 3.0856775814913673e18; RefS.MaxSteps = 20000u;
	FVolumeSettings OldS;
	OldS.StepRatio = 0.08f; OldS.HintScale = 1.0f; OldS.LocalStartCm = 3.0856775814913673e18;
	const FVolumeSettings NewS;

	std::string Detail;
	double OldSum = 0.0, NewSum = 0.0, OldMs = 0.0, NewMs = 0.0, WorstLight = 0.0, WorstP95 = 0.0, WorstGain = 1e30;
	u64 NewTrunc = 0;
	for (const FView& Vw : Views)
	{
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 240; Cm.Height = 135; Cm.DeltaMag = 0.0;
		Cm.LookAlong(Vw.Look);
		auto Run = [&](const FVolumeSettings& S, FVolumeScene& OutSc)
		{
			OutSc = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(false);
			return MarchAll(OutSc, Cm, ToHlsl(OutSc.View));
		};
		FVolumeScene ScR, ScO, ScN;
		const FMarchRun Ref = Run(RefS, ScR);
		const FMarchRun Old = Run(OldS, ScO);
		const FMarchRun New = Run(NewS, ScN);
		double LeO = 0.0, P95O = 0.0, LeN = 0.0, P95N = 0.0;
		Compare(Old, Ref, LeO, P95O);
		Compare(New, Ref, LeN, P95N);
		OldSum += Old.Steps; NewSum += New.Steps; OldMs += Old.Ms; NewMs += New.Ms;
		WorstLight = std::max(WorstLight, LeN); WorstP95 = std::max(WorstP95, P95N);
		WorstGain = std::min(WorstGain, Old.Steps / std::max(New.Steps, 1e-9));
		NewTrunc += New.Truncated;
		Detail += Uni::Fmt("%s[%s: %u local carriers; ref %.1f steps/px; old %.1f steps/px (max %u), %.2f active, %.1f%% supersampled, light %.2f%%, p95 %.2f%%; new %.1f steps/px (max %u), %.2f active, %.1f%% supersampled, light %.2f%%, p95 %.2f%%; %.2fx fewer]",
			Detail.empty() ? "" : "; ", Vw.Name, ToHlsl(ScN.View).NumCarriers, Ref.Steps,
			Old.Steps, Old.MaxSteps, Old.Active, 100.0 * Old.Super, 100.0 * LeO, 100.0 * P95O,
			New.Steps, New.MaxSteps, New.Active, 100.0 * New.Super, 100.0 * LeN, 100.0 * P95N, Old.Steps / std::max(New.Steps, 1e-9));
	}
	Detail += Uni::Fmt("; CPU march time %.0f -> %.0f ms, %llu truncated", OldMs, NewMs, (unsigned long long)NewTrunc);
	Uni::Check(2, "T2.vol.perf_inside", "D-195 the volume march from inside the Milky Way (flythrough scene, solar camera, 240 x 135, three views): the defaults (step ratio 0.24, hints x 3 since D-198, local carriers from 10 pc) take at least 2x fewer steps per pixel than the old (0.08, x 1, 1 pc) in every view, with total light within 0.5% and the 95th percentile per-pixel error under 1% of a fine reference, and no ray running out of steps")
		.Metric("old_steps_per_px", OldSum / 3.0).Metric("new_steps_per_px", NewSum / 3.0).Metric("worst_gain", WorstGain)
		.Metric("worst_light", WorstLight).Metric("worst_p95", WorstP95).Metric("truncated", double(NewTrunc))
		.Pass(WorstGain >= 2.0 && WorstLight < 0.005 && WorstP95 < 0.01 && NewTrunc == 0, Detail);
}

namespace
{
	/** The march image bilinearly upsampled to an output size (the composite's rule on open
	 *  sky: no scene depth here), luminance per output pixel (radiance x its solid angle). */
	std::vector<double> Upsampled(const FMarchRun& R, int Mw, int Mh, const FCamera& Out)
	{
		std::vector<double> L(size_t(Out.Width) * Out.Height, 0.0);
		for (int Y = 0; Y < Out.Height; ++Y) for (int X = 0; X < Out.Width; ++X)
		{
			const double Mx = (X + 0.5) * double(Mw) / Out.Width - 0.5, My = (Y + 0.5) * double(Mh) / Out.Height - 0.5;
			const int X0 = int(std::floor(Mx)), Y0 = int(std::floor(My));
			const double Fx = Mx - X0, Fy = My - Y0;
			double Acc = 0.0;
			for (int J = 0; J < 2; ++J) for (int I = 0; I < 2; ++I)
			{
				const int Qx = std::clamp(X0 + I, 0, Mw - 1), Qy = std::clamp(Y0 + J, 0, Mh - 1);
				// FMarchRun stores radiance x the march texel's solid angle: back to radiance.
				const double Om = PixelSolidAngle(Out, Mw, Mh, Qx + 0.5, Qy + 0.5);
				Acc += (I ? Fx : 1.0 - Fx) * (J ? Fy : 1.0 - Fy) * R.Lum[size_t(Qy) * Mw + Qx] / Om;
			}
			L[size_t(Y) * Out.Width + X] = Acc * PixelSolidAngle(Out, Out.Width, Out.Height, X + 0.5, Y + 0.5);
		}
		return L;
	}
}

UNI_TEST(volume, march_budget)
{
	// D-196: the march's pixel budget (uu.VolumeMaxMarchPixels). On a 1880 x 1120 viewport, half
	// resolution is 940 x 560 (0.53 Mpx); the budget (0.26 Mpx) marches 663 x 395, half the rays.
	// Here at a third of that scale (output 627 x 373): half resolution 313 x 186 against the
	// budget's 221 x 131, both upsampled to the output, against a full-resolution march.
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::SolarCamera();
	P.Update(Cam, FVec3f{});
	struct FView { const char* Name; FV3 Look; };
	const FView Views[] = { { "toward the centre", { -1.0, 0.05, 0.02 } }, { "along the disc", { 0.05, 1.0, 0.02 } } };
	const int Ow = 627, Oh = 373;
	int Hw = 0, Hh = 0, Bw = 0, Bh = 0;
	VolumeMarchSize(Ow, Oh, 2, 0.0, Hw, Hh);
	VolumeMarchSize(Ow, Oh, 2, 262144.0 / 9.0, Bw, Bh);
	std::string Detail;
	double WorstLight = 0.0, WorstP95Budget = 0.0, WorstP95Half = 0.0, RayRatio = 0.0;
	for (const FView& Vw : Views)
	{
		FCamera Co; Co.Coord = Cam; Co.Width = Ow; Co.Height = Oh; Co.DeltaMag = 0.0; Co.LookAlong(Vw.Look);
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Co.View()).Subset(false);
		auto MarchAt = [&](int W, int Ht)
		{
			FCamera C = Co; C.Width = W; C.Height = Ht;
			H::FUniVolumeView V = ToHlsl(Sc.View);
			V.ThetaPixel = float(2.0 * C.TanX() / double(W));   // the march texel's (the engine's VV.ThetaPixel)
			return MarchAll(Sc, C, V);
		};
		const FMarchRun Full = MarchAt(Ow, Oh), Half = MarchAt(Hw, Hh), Bud = MarchAt(Bw, Bh);
		const std::vector<double> Lh = Upsampled(Half, Hw, Hh, Co), Lb = Upsampled(Bud, Bw, Bh, Co);
		double Sf = 0.0, Sh = 0.0, Sb = 0.0;
		for (size_t I = 0; I < Full.Lum.size(); ++I) { Sf += Full.Lum[I]; Sh += Lh[I]; Sb += Lb[I]; }
		auto P95 = [&](const std::vector<double>& A)
		{
			std::vector<double> E;
			const double Mean = Sf / double(Full.Lum.size());
			for (size_t I = 0; I < A.size(); ++I) { if (Full.Lum[I] > 0.01 * Mean) { E.push_back(std::abs(A[I] - Full.Lum[I]) / Full.Lum[I]); } }
			std::sort(E.begin(), E.end());
			return E.empty() ? 0.0 : E[size_t(0.95 * double(E.size() - 1))];
		};
		const double Ph = P95(Lh), Pb = P95(Lb);
		WorstLight = std::max(WorstLight, std::abs(Sb / Sf - 1.0));
		WorstP95Budget = std::max(WorstP95Budget, Pb);
		WorstP95Half = std::max(WorstP95Half, Ph);
		RayRatio = double(Hw) * Hh / (double(Bw) * Bh);
		Detail += Uni::Fmt("%s[%s: half %dx%d light %+.2f%% p95 %.2f%%; budget %dx%d light %+.2f%% p95 %.2f%%; %.1f vs %.1f steps/px]",
			Detail.empty() ? "" : "; ", Vw.Name, Hw, Hh, 100.0 * (Sh / Sf - 1.0), 100.0 * Ph, Bw, Bh, 100.0 * (Sb / Sf - 1.0), 100.0 * Pb, Half.Steps, Bud.Steps);
	}
	Uni::Check(2, "T2.vol.march_budget", "D-196 march pixel budget: at 0.47x the rays of half resolution, the glow upsampled to the output keeps its light within 0.5% and its per-pixel p95 error against a full-resolution march under 3% (and within 1.5 points of half resolution's)")
		.Metric("ray_ratio", RayRatio).Metric("worst_light", WorstLight).Metric("worst_p95_budget", WorstP95Budget).Metric("worst_p95_half", WorstP95Half)
		.Pass(RayRatio > 1.8 && WorstLight < 0.005 && WorstP95Budget < 0.03 && WorstP95Budget < WorstP95Half + 0.015, Detail);
}

UNI_TEST(volume, sprite_keep)
{
	// D-197 (owner: ~30% fewer entities; D-198: half; D-205: 0.35): a galaxy's stars thinned to the
	// engine's SpriteKeep. Inside the Milky Way at 1 kpc from its centre (the dense core) and at the
	// solar circle: resident stars fall to ~keep of the full draw, the kept ones are the full draw's (same identities), and the
	// light over the whole sphere (sprites + glow) stays within 2%: the missing stars' light is
	// the galaxy's glow.
	// The light's excess at the core grows with the thinning: +1.17% at 0.7, +1.93% at 0.5, +2.50% at
	// 0.35 (the sprites there are 20% of the sky; their thinned share is the glow's expectation).
	const double Keep = UniScenes::FClusterTuning{}.StarKeep;   // the engine's default (D-197 0.7, D-198 0.5, D-205 0.35)
	FUniverseDef Full = UniScenes::GalaxyToStar();
	FUniverseDef Thin = UniScenes::GalaxyToStar();
	UniScenes::UseSpriteKeep(Thin, Keep, 1.0);
	FUniverseModel Mf(Full), Mt(Thin);
	FRefGenerator Gf(Mf), Gt(Mt);
	FVolumeBuilder Vf(Mf, Gf), Vt(Mt, Gt);
	struct FCase { const char* Name; FVec3d Pos; };
	const FCase Cases[] = { { "1 kpc from the centre", { 1.0 * Units::Kpc, 0.3 * Units::Kpc, 20.0 * Units::Pc } }, { "solar circle", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc } } };
	std::string Detail;
	double WorstRatio = 0.0, WorstKept = 1.0, WorstLight = 0.0;
	for (const FCase& C : Cases)
	{
		FCoord Cam = FCoord::Origin(); FVec3f Fr; AddDisplacement(Cam, Fr, C.Pos);
		FRefPipeline Pf(Gf), Pt(Gt);
		Pf.bVelocityLod = false; Pt.bVelocityLod = false;
		Pf.Update(Cam, FVec3f{}); Pt.Update(Cam, FVec3f{});
		std::unordered_set<u64> FullIds;
		double Nf = 0.0, Nt = 0.0, NtIn = 0.0;
		Pf.ForEachResident([&](const FCellKey&, const FObjectGPU& O, float, u64 Hash) { if (H::Uni_ObjectArchetype(UniCore::ToHlsl(O)) == UniScenes::kStar) { FullIds.insert(Hash); Nf += 1.0; } });
		Pt.ForEachResident([&](const FCellKey&, const FObjectGPU& O, float, u64 Hash) { if (H::Uni_ObjectArchetype(UniCore::ToHlsl(O)) == UniScenes::kStar) { Nt += 1.0; NtIn += FullIds.count(Hash) ? 1.0 : 0.0; } });
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 96; Cm.Height = 96; Cm.DeltaMag = 0.0; Cm.LookAlong({ 1, 0, 0 });
		double SpF = 0.0, SpT = 0.0;
		const double Lf = SphereLight(Pf, Vf.Build(Pf, Cam, FVec3f{}, Cm.View()), Cam, 96, &SpF);
		const double Lt = SphereLight(Pt, Vt.Build(Pt, Cam, FVec3f{}, Cm.View()), Cam, 96, &SpT);
		const double Ratio = Nt / std::max(Nf, 1.0), Kept = NtIn / std::max(Nt, 1.0), Le = Lt / Lf - 1.0;
		WorstRatio = std::max(WorstRatio, std::abs(Ratio - Keep));
		WorstKept = std::min(WorstKept, Kept);
		WorstLight = std::max(WorstLight, std::abs(Le));
		Detail += Uni::Fmt("%s[%s: %.0f -> %.0f resident stars (%.3f), %.2f%% of the kept in the full draw; light over the sphere %+.2f%%, sprites' share %.1f%% -> %.1f%%]",
			Detail.empty() ? "" : "; ", C.Name, Nf, Nt, Ratio, 100.0 * Kept, 100.0 * Le, 100.0 * SpF, 100.0 * SpT);
	}
	Uni::Check(2, "T2.vol.sprite_keep", "D-197 stars thinned to the engine's default (D-205: 0.35; UseSpriteKeep): resident stars keep +/- 0.03 of the full draw, at least 99% of the kept stars are full-draw stars (identities kept), and the light over the whole sphere (sprites + glow) within 3%, at 1 kpc from the Milky Way's centre and at the solar circle")
		.Metric("worst_count_ratio_off", WorstRatio).Metric("worst_kept_in_full", WorstKept).Metric("worst_light", WorstLight)
		.Pass(WorstRatio < 0.03 && WorstKept > 0.99 && WorstLight < 0.03, Detail);
}

UNI_TEST(volume, dust_tuning)
{
	// D-198 (owner: darker dust lanes): galaxies' dust x 2 (UseDustTuning). The Milky Way edge-on
	// from 40 kpc (its mid-plane lane) and from the solar camera toward the centre: how much of
	// the glow the dust takes, before and after, and what the denser dust does to the march
	// toward the centre (rays stop once opaque).
	struct FView { const char* Name; FVec3d Pos; FV3 Look; };
	const FView Views[] = { { "edge-on from 40 kpc", { 40.0 * Units::Kpc, 0.0, 0.0 }, { -1.0, 0.0, 0.0 } }, { "solar camera toward the centre", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { -1.0, 0.05, 0.02 } } };
	std::string Detail;
	double WorstDarker = 1.0, WorstStepGain = 0.0;
	for (const FView& Vw : Views)
	{
		FCoord Cam = FCoord::Origin(); FVec3f Fr; AddDisplacement(Cam, Fr, Vw.Pos);
		double Lum[3] = { 0, 0, 0 }, Steps[3] = { 0, 0, 0 };
		for (int Cfg = 0; Cfg < 3; ++Cfg)   // 0: no dust, 1: the D-132 dust, 2: D-198
		{
			FUniverseDef D = UniScenes::GalaxyToStar();
			if (Cfg == 0) { for (FArchetype& A : D.Archetypes) { A.Volume.bDust = false; } }
			if (Cfg == 2) { UniScenes::UseDustTuning(D, 2.0, 3.0); }
			FUniverseModel M(D);
			FRefGenerator G(M);
			FVolumeBuilder VB(M, G);
			FRefPipeline P(G);
			P.bVelocityLod = false;
			P.Update(Cam, FVec3f{});
			FCamera Cm; Cm.Coord = Cam; Cm.Width = 192; Cm.Height = 108; Cm.DeltaMag = 0.0; Cm.LookAlong(Vw.Look);
			const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
			const FMarchRun R = MarchAll(Sc, Cm, ToHlsl(Sc.View));
			for (double V : R.Lum) { Lum[Cfg] += V; }
			Steps[Cfg] = R.Steps;
		}
		const double Took1 = 1.0 - Lum[1] / Lum[0], Took2 = 1.0 - Lum[2] / Lum[0];
		WorstDarker = std::min(WorstDarker, Took2 - Took1);
		WorstStepGain = std::max(WorstStepGain, Steps[2] / std::max(Steps[1], 1e-9) - 1.0);
		Detail += Uni::Fmt("%s[%s: dust takes %.1f%% of the glow (D-132) -> %.1f%% (D-198); %.1f -> %.1f steps/px]", Detail.empty() ? "" : "; ", Vw.Name, 100.0 * Took1, 100.0 * Took2, Steps[1], Steps[2]);
	}
	Uni::Check(2, "T2.vol.dust_tuning", "D-198 galaxies' dust x 2: the dust takes more of the Milky Way's glow edge-on and toward the centre, and the march takes no more steps for it (opaque rays stop)")
		.Metric("least_extra_absorbed", WorstDarker).Metric("worst_step_change", WorstStepGain)
		.Pass(WorstDarker > 0.02 && WorstStepGain < 0.02, Detail);
}

UNI_TEST(volume, auto_budget)
{
	// D-200: the glow's pixel budget follows the GPU frame time (uu.VolumeTargetFps). A model
	// frame: GPU ms = the rest of the frame + the glow at full budget x the scale (its cost goes
	// with the pixels marched), with 5% frame-to-frame noise. At 60 fps (16.7 ms):
	// - a dense galaxy core: 6 ms of other work, the glow 17 ms at full budget: the budget falls to
	//   the first level that fits (x0.6: 16.2 ms) within 6 changes, then holds for 1000 frames;
	// - the load goes away (the glow 6 ms): it climbs back to full;
	// - no target: full budget.
	auto Run = [](double OtherMs, double VolMs, double TargetMs, int Frames, FVolumeAutoBudget& B, u32 Seed, double& OutLastMs)
	{
		u32 S = Seed;
		for (int F = 0; F < Frames; ++F)
		{
			S = S * 1664525u + 1013904223u;
			const double Noise = 1.0 + 0.05 * (double(S >> 8) / 16777216.0 * 2.0 - 1.0);
			OutLastMs = (OtherMs + VolMs * B.Scale()) * Noise;
			B.Update(OutLastMs, TargetMs, 20);
		}
	};
	const double Target = 1000.0 / 60.0;
	FVolumeAutoBudget B;
	double Last = 0.0;
	Run(6.0, 17.0, Target, 300, B, 7u, Last);
	const int Settled = B.Level;
	const u32 ChangesToSettle = B.Changes;
	const double SettledMs = 6.0 + 17.0 * B.Scale();
	Run(6.0, 17.0, Target, 1000, B, 11u, Last);
	const u32 ChangesHeld = B.Changes - ChangesToSettle;
	Run(6.0, 6.0, Target, 400, B, 13u, Last);
	const int Recovered = B.Level;
	FVolumeAutoBudget Off;
	Run(6.0, 30.0, 0.0, 200, Off, 17u, Last);
	Uni::Check(2, "T2.vol.auto_budget", "D-200 volume auto budget at 60 fps (model frame, 5% noise): with 6 ms of other work and the glow 17 ms at full budget it settles within 6 level changes on a level that fits (GPU under the target x 1.03), holds it for 1000 frames without a change, climbs back to full when the glow drops to 6 ms, and stays full with no target")
		.Metric("settled_level", double(Settled)).Metric("settled_ms", SettledMs).Metric("changes_to_settle", double(ChangesToSettle)).Metric("changes_held", double(ChangesHeld))
		.Pass(ChangesToSettle <= 6u && SettledMs <= 1.03 * Target && Settled > 0 && ChangesHeld == 0u && Recovered == 0 && Off.Level == 0,
			Uni::Fmt("settled on level %d (x%.2f of the budget, %.1f ms against %.1f) after %u changes; %u changes in the next 1000 frames; back to level %d once the glow is 6 ms; no target: level %d",
				Settled, FVolumeAutoBudget::Scales[Settled], SettledMs, Target, ChangesToSettle, ChangesHeld, Recovered, Off.Level));
}

UNI_TEST(volume, travel_load)
{
	// D-201 (owner: intergalactic speed still ~30 fps after D-200, which only scales the glow).
	// Travel sensitivity (D-154) adds up to uu.TravelSensitivity mag while flying fast, widening
	// every window: the resident objects and cells an intergalactic view holds (flythrough,
	// thinned as the engine's, 100 Mpc out) at dm 0, +0.5 and +1. Measured once at +1.5 and +2.5
	// (22 s of CPU generation, so not run here): 102,092 objects / 3,471 cells, and 307,803 /
	// 10,490, ~18x rest. The engine default goes 2.5 -> 0.5.
	FUniverseDef Def = UniScenes::Flythrough();
	UniScenes::UseSpriteKeep(Def, 0.5, 0.5);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FCoord Cam = FCoord::Origin(); FVec3f Fr; AddDisplacement(Cam, Fr, FVec3d{ 100.0 * Units::Mpc, 30.0 * Units::Mpc, 10.0 * Units::Mpc });
	double Objects[3] = { 0, 0, 0 }, Cells[3] = { 0, 0, 0 };
	const double Dms[3] = { 0.0, 0.5, 1.0 };
	for (int I = 0; I < 3; ++I)
	{
		FRefPipeline P(G); P.bVelocityLod = false; P.DeltaMag = Dms[I];
		P.Update(Cam, FVec3f{});
		std::unordered_set<u64> CellSet;
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU&, float, u64) { Objects[I] += 1.0; CellSet.insert(HashCell(K)); });
		Cells[I] = double(CellSet.size());
	}
	const double R05 = Objects[1] / std::max(Objects[0], 1.0), R10 = Objects[2] / std::max(Objects[0], 1.0);
	Uni::Check(2, "T2.vol.travel_load", "D-201 travel sensitivity's load at an intergalactic camera: resident objects at +0.5 mag at most 2.5x rest's (the new default's), and growing with dm (+1 over +0.5)")
		.Metric("objects_dm0", Objects[0]).Metric("ratio_dm05", R05).Metric("ratio_dm1", R10)
		.Pass(R05 < 2.5 && R10 > R05 && Objects[0] > 0.0,
			Uni::Fmt("dm 0: %.0f objects in %.0f cells; +0.5: %.0f in %.0f (%.2fx); +1: %.0f in %.0f (%.2fx); measured once: +1.5 5.9x, +2.5 17.7x (307,803 objects in 10,490 cells)",
				Objects[0], Cells[0], Objects[1], Cells[1], R05, Objects[2], Cells[2], R10));
}
