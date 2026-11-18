// test_travellook.cpp
// D-230 (owner: intergalactic travel is mostly black with a few galaxies drifting by; the web's
// light is physical and very faint): the travel look, render-only art choices ramped by the travel
// curve.
//   travel_shift  Uni_TravelShift: ahead bluer, behind redder, the side unchanged, luminance kept,
//                 off at 0 or with no motion; ShiftMotion's edge cases. D-268: the strength's own
//                 curve from the speed (in earlier, D-230's top at the reference speed, 4x the
//                 shift's log at 4x that speed), the blue side's boost past the reference only.
//   motion_web    the motion web in the march, from intergalactic space with the delivered web: off
//                 is bit for bit today's march; the light it adds is linear in its gain, drawn
//                 once a step though two web carriers read the texel, faded in near the camera and
//                 out before its depth; the shift keeps every pixel's luminance. D-231: its
//                 organic detail (one galaxy_detail read) keeps its light on average and adds
//                 structure; off is bit for bit D-230's.
//   web_ramp      D-231: the motion web's speed ramp: off below its start, about half by 20-50
//                 kpc/s (a galaxy crossed in a second or two), full at its end, eased in time.
//   web_regions   D-233: the detail's look changes by region (filaments, knots, cloud, cells): the
//                 weights, each channel's light and contrast, the ridged-only weights are D-231's
//                 pattern, the frame's added light kept; a slice of the web tile with and without.
//   web_swim      D-231 (D-233: with the regions): the detail does not slide in flight.
//   web_dust      D-269: the web's dark dust: off bit for bit, dims with its optical depth, once a
//                 step, grey or reddened, the pattern keeps its amount, the depth fade, the grain.
//   slice_reach   D-271: the sprites' transmittance (the march's slices) follows the web's dust
//                 out to 50 Mpc (the far segment); the near slices unchanged; none: D-130's.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniTravel.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <cstdio>
#include <sys/stat.h>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

UNI_TEST(look, travel_shift)
{
	bool bOk = true;
	std::string Detail;
	const H::float3 Vel(1.0f, 0.0f, 0.0f);
	const H::float3 C(0.8f, 0.7f, 0.6f);
	const float Y0 = H::Uni_Luminance(C);
	const H::float3 Ahead = H::Uni_TravelShift(C, H::float3(1, 0, 0), Vel, 0.5f, 1.0f, 0.5f, 0.0f);
	const H::float3 Behind = H::Uni_TravelShift(C, H::float3(-1, 0, 0), Vel, 0.5f, 1.0f, 0.5f, 0.0f);
	const H::float3 Side = H::Uni_TravelShift(C, H::float3(0, 1, 0), Vel, 0.5f, 1.0f, 0.5f, 0.0f);
	const H::float3 Off = H::Uni_TravelShift(C, H::float3(1, 0, 0), Vel, 0.0f, 1.0f, 0.5f, 0.0f);
	const H::float3 NoMotion = H::Uni_TravelShift(C, H::float3(1, 0, 0), H::float3(0, 0, 0), 0.5f, 1.0f, 0.5f, 0.0f);
	const double BrA = double(Ahead.z / Ahead.x), BrB = double(Behind.z / Behind.x), Br0 = double(C.z / C.x);
	if (!(BrA > Br0 * 1.5 && BrB < Br0 / 1.5)) { bOk = false; }
	for (const H::float3& X : { Ahead, Behind, Side }) { if (std::abs(double(H::Uni_Luminance(X)) / double(Y0) - 1.0) > 1e-5) { bOk = false; } }
	auto Same = [](H::float3 A, H::float3 B) { return A.x == B.x && A.y == B.y && A.z == B.z; };
	if (!Same(Side, C) || !Same(Off, C) || !Same(NoMotion, C)) { bOk = false; }
	Detail += Uni::Fmt("blue/red %.3f at rest, %.3f ahead, %.3f behind; the side, off and no motion unchanged %s; luminance kept", Br0, BrA, BrB, Same(Side, C) && Same(Off, C) && Same(NoMotion, C) ? "yes" : "NO");
	// D-268: up to the old top end (0.5) the tint is D-230's (Uni_DopplerTint at exp(0.5 cos)).
	double TintDiff = 0.0;
	for (int I = 0; I <= 16; ++I)
	{
		const float Cs = -1.0f + float(I) / 8.0f;
		const H::float3 Dir(Cs, std::sqrt(std::max(1.0f - Cs * Cs, 0.0f)), 0.0f);
		const H::float3 A = H::Uni_TravelShift(C, Dir, Vel, 0.5f, 1.0f, 0.5f, 1.0f);
		const H::float3 B = H::Uni_DopplerTint(C, std::exp(0.5f * Cs), 1.0f, 0.0f);
		TintDiff = std::max({ TintDiff, std::abs(double(A.x - B.x)), std::abs(double(A.y - B.y)), std::abs(double(A.z - B.z)) });
	}
	if (!(TintDiff < 1e-5)) { bOk = false; }
	// D-268: the strength from the speed (kpc/s here): 0 below the start, uu.Shift at the reference,
	// uu.Shift.Top at full and above; monotone; its slope carried through the reference; in earlier
	// than D-230's (the travel curve, 100 kpc/s - 10 Mpc/s smoothstep in log, ramped 0.15 - 0.75).
	const double S0 = 150.0, SR = 2200.0, SF = 8900.0, AR = 0.5, AT = 2.0;
	auto Shift = [&](double V) { return TravelShiftTarget(V, S0, SR, SF, AR, AT); };
	auto Old = [](double V)
	{
		const double T = std::clamp(std::log(V / 100.0) / std::log(100.0), 0.0, 1.0);
		const double Cv = T * T * (3.0 - 2.0 * T);
		const double X = std::clamp((Cv - 0.15) / 0.6, 0.0, 1.0);
		return 0.5 * X * X * (3.0 - 2.0 * X);
	};
	double Prev = -1.0;
	bool bMono = true, bEarlier = true;
	for (int I = 0; I <= 400; ++I)
	{
		const double V = 50.0 * std::pow(20000.0 / 50.0, I / 400.0);
		const double R = Shift(V);
		bMono = bMono && R >= Prev;
		Prev = R;
		if (V < 700.0 && R < Old(V)) { bEarlier = false; }
	}
	const double SlopeIn = (Shift(SR) - Shift(SR * std::exp(-1e-4))) / 1e-4, SlopeOut = (Shift(SR * std::exp(1e-4)) - Shift(SR)) / 1e-4;
	const bool bEnds = Shift(140.0) == 0.0 && std::abs(Shift(SR) - AR) < 1e-12 && std::abs(Shift(SF) - AT) < 1e-12 && Shift(30000.0) == AT;
	const bool bSlope = std::abs(SlopeOut / SlopeIn - 1.0) < 0.01;
	if (!(bMono && bEarlier && bEnds && bSlope)) { bOk = false; }
	Detail += Uni::Fmt("; D-268: the tint to 0.5 D-230's within %.1e; the shift 0 below 150 kpc/s, x%.2f at 300 (D-230 x%.2f), x%.2f at 600 (x%.2f), x%.2f at 1 Mpc/s (x%.2f), x%.2f at 2.2 (x%.2f), x%.2f at 4.4, x%.2f at 8.9 and above; monotone %s, earlier below 700 kpc/s %s, slope through the reference %.3f / %.3f",
		TintDiff, std::exp(Shift(300.0)), std::exp(Old(300.0)), std::exp(Shift(600.0)), std::exp(Old(600.0)), std::exp(Shift(1000.0)), std::exp(Old(1000.0)), std::exp(Shift(2200.0)), std::exp(Old(2200.0)), std::exp(Shift(4400.0)), std::exp(Shift(8900.0)), bMono ? "yes" : "NO", bEarlier ? "yes" : "NO", SlopeIn, SlopeOut);
	// D-268: the boost: none anywhere at the reference's shift; at the top straight ahead x exp(1.5)
	// (Boost 1), behind and at the side luminance kept; the factor capped at e^3.
	const H::float3 RefAhead = H::Uni_TravelShift(C, H::float3(1, 0, 0), Vel, 0.5f, 1.0f, 0.5f, 1.0f);
	const H::float3 TopAhead = H::Uni_TravelShift(C, H::float3(1, 0, 0), Vel, 2.0f, 1.0f, 0.5f, 1.0f);
	const H::float3 TopBehind = H::Uni_TravelShift(C, H::float3(-1, 0, 0), Vel, 2.0f, 1.0f, 0.5f, 1.0f);
	const H::float3 TopCone = H::Uni_TravelShift(C, H::float3(0.25f, std::sqrt(1.0f - 0.0625f), 0), Vel, 2.0f, 1.0f, 0.5f, 1.0f);   // shift 0.5 there
	const double LRef = double(H::Uni_Luminance(RefAhead)) / Y0, LTop = double(H::Uni_Luminance(TopAhead)) / Y0;
	const double LBehind = double(H::Uni_Luminance(TopBehind)) / Y0, LCone = double(H::Uni_Luminance(TopCone)) / Y0;
	const double BrTop = double(TopAhead.z / TopAhead.x), BrTopB = double(TopBehind.z / TopBehind.x);
	const H::float3 Capped = H::Uni_TravelShift(C, H::float3(1, 0, 0), Vel, 5.0f, 1.0f, 0.5f, 0.0f);
	const H::float3 AtCap = H::Uni_TravelShift(C, H::float3(1, 0, 0), Vel, 3.0f, 1.0f, 0.5f, 0.0f);
	const bool bBoost = std::abs(LRef - 1.0) < 1e-5 && std::abs(LTop / std::exp(1.5) - 1.0) < 1e-4 && std::abs(LBehind - 1.0) < 1e-5 && std::abs(LCone - 1.0) < 1e-4
		&& BrTop > BrA * 10.0 && BrTopB < BrB / 10.0 && Capped.x == AtCap.x && Capped.z == AtCap.z;
	if (!bBoost) { bOk = false; }
	Detail += Uni::Fmt("; at the top blue/red %.2f ahead, %.4f behind; luminance x%.4f ahead (x%.4f at the reference's shift), x%.4f where the shift is 0.5, x%.4f behind; capped at e^3 %s",
		BrTop, BrTopB, LTop, LRef, LCone, LBehind, Capped.x == AtCap.x ? "yes" : "NO");
	// D-268: the ramp in time: rises to 95% within 1 s, falls slower, never overshoots.
	FTravelShiftRamp Ramp;
	double MaxV = 0.0;
	for (int I = 0; I < 60; ++I) { MaxV = std::max(MaxV, Ramp.Update(2.0, 1.0 / 60.0)); }
	const double Up1s = Ramp.Value;
	for (int I = 0; I < 60; ++I) { Ramp.Update(0.0, 1.0 / 60.0); }
	const double Down1s = Ramp.Value;
	const bool bRamp = Up1s > 1.9 && MaxV <= 2.0 && Down1s > 0.05 && Down1s < 0.5;
	if (!bRamp) { bOk = false; }
	Detail += Uni::Fmt("; eased: %.3f of 2 after 1 s up, %.3f left 1 s after", Up1s, Down1s);
	// ShiftMotion: a zero or non-finite step gives no direction and no amount.
	float Va[4];
	ShiftMotion(0.0, 0.0, 0.0, 0.5, Va);
	const bool bZero = Va[0] == 0.0f && Va[3] == 0.0f;
	ShiftMotion(3.0e24, 4.0e24, 0.0, 0.5, Va);   // 5 Mpc a frame: the unit direction, in double
	const bool bUnit = std::abs(double(Va[0]) - 0.6) < 1e-6 && std::abs(double(Va[1]) - 0.8) < 1e-6 && Va[3] == 0.5f;
	if (!bZero || !bUnit) { bOk = false; }
	Detail += Uni::Fmt("; zero step: none %s; a 5 Mpc step: unit %s", bZero ? "yes" : "NO", bUnit ? "yes" : "NO");
	Uni::Check(1, "T1.look.travel_shift", "D-230 the travel shift: light ahead bluer and behind redder (blue/red x1.5 at 0.5), the side, 0 and no motion unchanged, luminance kept; ShiftMotion's edge cases. D-268: up to 0.5 the tint is D-230's; its strength from the speed: 0 below the start, in no later than D-230's below 700 kpc/s, uu.Shift at the reference, uu.Shift.Top at full, monotone, its slope continuous at the reference (1%); no boost up to the reference's shift, x exp(Boost x excess) ahead past it, luminance kept behind; capped at e^3; eased in time")
		.Metric("br_ahead", BrA).Metric("br_behind", BrB).Pass(bOk, Detail);
}

UNI_TEST(look, web_ramp)
{
	const double Kpc = 3.0856775814913673e21;
	const double S0 = 5.0 * Kpc, S1 = 5000.0 * Kpc;
	const double At2 = FMotionWebRamp::Target(2.0 * Kpc, S0, S1), At20 = FMotionWebRamp::Target(20.0 * Kpc, S0, S1), At50 = FMotionWebRamp::Target(50.0 * Kpc, S0, S1);
	const double At1M = FMotionWebRamp::Target(1000.0 * Kpc, S0, S1), At10M = FMotionWebRamp::Target(10000.0 * Kpc, S0, S1);
	bool bMono = true;
	double Prev = -1.0;
	for (int I = 0; I <= 200; ++I) { const double V = FMotionWebRamp::Target(S0 * std::pow(S1 / S0 * 2.0, I / 200.0), S0, S1); bMono = bMono && V >= Prev; Prev = V; }
	// In time: a step from rest to 30 kpc/s reaches 90% of its target within 1 s and never
	// overshoots; speed jitter of +-50% a frame at 30 kpc/s moves it under 2% a frame.
	FMotionWebRamp R;
	double Over = 0.0, At1s = 0.0;
	const double T30 = FMotionWebRamp::Target(30.0 * Kpc, S0, S1);
	for (int F = 0; F < 120; ++F) { R.Update(30.0 * Kpc, 1.0 / 60.0, S0, S1); Over = std::max(Over, R.Value - T30); if (F == 59) { At1s = R.Value; } }
	double Jit = 0.0;
	for (int F = 0; F < 240; ++F)
	{
		const double Before = R.Value;
		R.Update((F % 2 ? 45.0 : 15.0) * Kpc, 1.0 / 60.0, S0, S1);
		Jit = std::max(Jit, std::abs(R.Value - Before));
	}
	const bool bOk = At2 == 0.0 && At20 > 0.3 && At20 < 0.6 && At50 > 0.45 && At50 < 0.75 && At1M > 0.9 && At10M == 1.0 && bMono && Over <= 1e-12 && At1s > 0.9 * T30 && Jit < 0.02;
	Uni::Check(1, "T1.look.web_ramp", "D-231 the motion web's speed ramp (5 kpc/s to 5 Mpc/s, ease-out on log speed): off at 2 kpc/s, 0.3-0.6 at 20 kpc/s and 0.45-0.75 at 50 (a galaxy crossed in a second or two), over 0.9 at 1 Mpc/s, full at 10, monotone; eased in time: 90% of a step within 1 s, no overshoot, frame jitter under 2%")
		.Metric("at_20kpcs", At20).Metric("at_50kpcs", At50).Metric("at_1mpcs", At1M).Metric("jitter", Jit)
		.Pass(bOk, Uni::Fmt("2 kpc/s %.2f, 20 kpc/s %.2f, 50 kpc/s %.2f, 1 Mpc/s %.2f, 10 Mpc/s %.2f, monotone %s; from rest to 30 kpc/s: %.0f%% of %.2f at 1 s, overshoot %.1g; +-50%% speed jitter moves it %.2f%% a frame",
			At2, At20, At50, At1M, At10M, bMono ? "yes" : "NO", 100.0 * At1s / std::max(T30, 1e-12), T30, Over, 100.0 * Jit));
}

namespace
{
	struct FBind
	{
		explicit FBind(const FWebTexture& T) { BindWebTexture(&T); }
		~FBind() { BindWebTexture(nullptr); }
	};

	/** Per-pixel radiance of a background-only frame. */
	std::vector<H::float3> Frame(const FVolumeScene& Sc, const FCamera& Cm)
	{
		FImage Img(Cm.Width, Cm.Height);
		RenderVolume(Sc, Cm, Img);
		std::vector<H::float3> Out(size_t(Cm.Width) * Cm.Height);
		for (size_t I = 0; I < Out.size(); ++I) { Out[I] = H::float3(Img.Rgb[I * 3], Img.Rgb[I * 3 + 1], Img.Rgb[I * 3 + 2]); }
		return Out;
	}
	double SumLum(const std::vector<H::float3>& A) { double S = 0.0; for (const H::float3& V : A) { S += H::Uni_Luminance(V); } return S; }
}

UNI_TEST(look, motion_web)
{
	FWebTexture T;
	std::string Err;
	if (!FWebTexture::LoadDelivered("Data/Noise", "web_adhesion_density_mid", T, &Err)) { Uni::Check(2, "T2.look.motion_web", "D-230 the motion web").Pass(false, "texture not loaded: " + Err); return; }
	FBind Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w));
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::VoidCamera();
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 128; Cm.Height = 72; Cm.LookAlong({ 1.0, 0.3, 0.1 });
	const double Mpc = Units::Mpc;
	FWebTexture Gt[3];
	for (int K = 0; K < 3; ++K) { std::string E; FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(K), Gt[K], &E); BindGalaxyTexture(K, &Gt[K]); }
	struct FUnbind { ~FUnbind() { for (int K = 0; K < 3; ++K) { BindGalaxyTexture(K, nullptr); } } } Unbind;
	double Detail = 0.0;
	auto Scene = [&](double Gain, double NearMpc, double DepthMpc)
	{
		FVolumeSettings S;
		S.MotionWebDetailGain = float(Detail);
		S.MotionWebDetailCm = float(20.0 * Units::Mpc);
		S.MotionWebStepCm = float(0.5 * Units::Mpc);
		S.MotionWebGain = float(Gain * 7.0e4 / Mpc);
		S.MotionWebInvRef = float(1.0 / ((double(Mean.y) + double(Mean.z)) * 3.0));
		S.MotionWebNearCm = float(NearMpc * Mpc);
		S.MotionWebDepthCm = float(DepthMpc * Mpc);
		return VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(true);
	};
	const FVolumeScene S0 = Scene(0.0, 4.0, 50.0);
	const FVolumeScene SDef = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(true);
	size_t WebCarriers = 0;
	for (const FCarrierGPU& C : S0.Carriers) { if ((C.Flags & H::UNI_CARRIER_PERIODIC) && C.FieldId == H::UNI_FIELD_WEB) { ++WebCarriers; } }
	const std::vector<H::float3> A0 = Frame(S0, Cm), ADef = Frame(SDef, Cm);
	bool bOffSame = A0.size() == ADef.size();
	for (size_t I = 0; bOffSame && I < A0.size(); ++I) { bOffSame = A0[I].x == ADef[I].x && A0[I].y == ADef[I].y && A0[I].z == ADef[I].z; }
	const double L0 = SumLum(A0);
	const double D1 = SumLum(Frame(Scene(0.01, 4.0, 50.0), Cm)) - L0;
	const double D2 = SumLum(Frame(Scene(0.02, 4.0, 50.0), Cm)) - L0;
	const double Lin = D1 > 0.0 ? D2 / (2.0 * D1) : 0.0;
	// Once a step: the same scene with the cluster web's carrier removed (the galaxy web's alone
	// reads the texel) adds the same light within the steps' difference (two would double it).
	double Once = 0.0;
	{
		auto Strip = [&](FVolumeScene Sc)
		{
			bool bFirst = true;
			std::vector<FCarrierGPU> Keep;
			for (const FCarrierGPU& C : Sc.Carriers)
			{
				const bool bWeb = (C.Flags & H::UNI_CARRIER_PERIODIC) && C.FieldId == H::UNI_FIELD_WEB;
				if (bWeb && !bFirst) { continue; }
				if (bWeb) { bFirst = false; }
				Keep.push_back(C);
			}
			Sc.Carriers = Keep;
			Sc.View.NumCarriers = u32(Keep.size());
			return Sc;
		};
		const double B0 = SumLum(Frame(Strip(Scene(0.0, 4.0, 50.0)), Cm));
		const double B1 = SumLum(Frame(Strip(Scene(0.01, 4.0, 50.0)), Cm)) - B0;
		Once = B1 > 0.0 ? D1 / B1 : 0.0;
	}
	// Fades: everything within 2 Mpc faded in only past 4 adds a sliver of the default; depth 25
	// adds less than depth 50.
	const double DShallow = SumLum(Frame(Scene(0.01, 4.0, 2.0), Cm)) - L0;
	const double D25 = SumLum(Frame(Scene(0.01, 4.0, 25.0), Cm)) - L0;
	// The shift keeps every pixel's luminance (the volume as MarchCS colours it).
	double WorstLum = 0.0, Br0 = 0.0, BrS = 0.0;
	{
		FCamera Cs = Cm;
		Cs.ShiftAmount = 0.5; Cs.ShiftK = 1.0; Cs.ShiftVel = Norm(FV3{ 1.0, 0.3, 0.1 });
		const FVolumeScene S1 = Scene(0.01, 4.0, 50.0);
		const std::vector<H::float3> Plain = Frame(S1, Cm), Shifted = Frame(S1, Cs);
		for (size_t I = 0; I < Plain.size(); ++I)
		{
			const double Lp = H::Uni_Luminance(Plain[I]), Ls = H::Uni_Luminance(Shifted[I]);
			if (Lp > 0.0) { WorstLum = std::max(WorstLum, std::abs(Ls / Lp - 1.0)); }
			Br0 += Plain[I].z / std::max(Plain[I].x, 1e-30f); BrS += Shifted[I].z / std::max(Shifted[I].x, 1e-30f);
		}
	}
	// D-231: the detail. Gain 0 is bit for bit the plain motion web; at 0.8 the added light stays
	// within 35% on average and its pixel-to-pixel contrast rises (organic structure).
	double DetailRatio = 0.0, ContrastPlain = 0.0, ContrastDetail = 0.0;
	bool bDetailOffSame = true;
	{
		auto Added = [&](double Dg, std::vector<double>& Out)
		{
			Detail = Dg;
			const std::vector<H::float3> A = Frame(Scene(0.01, 4.0, 50.0), Cm);
			Out.resize(A.size());
			double Sum = 0.0;
			for (size_t I = 0; I < A.size(); ++I) { Out[I] = H::Uni_Luminance(A[I]) - H::Uni_Luminance(A0[I]); Sum += Out[I]; }
			Detail = 0.0;
			return Sum;
		};
		std::vector<double> Ap, Ad, A0d;
		const double Sp = Added(0.0, Ap), Sd = Added(1.0, Ad);
		Detail = 0.0;
		const std::vector<H::float3> Z = Frame(Scene(0.01, 4.0, 50.0), Cm);
		const std::vector<H::float3> Z2 = Frame(Scene(0.01, 4.0, 50.0), Cm);
		for (size_t I = 0; I < Z.size(); ++I) { bDetailOffSame = bDetailOffSame && Z[I].x == Z2[I].x && Z[I].y == Z2[I].y && Z[I].z == Z2[I].z && std::abs(H::Uni_Luminance(Z[I]) - H::Uni_Luminance(A0[I]) - Ap[I]) < 1e-12 * (1.0 + std::abs(Ap[I])); }
		DetailRatio = Sp > 0.0 ? Sd / Sp : 0.0;
		auto Contrast = [](const std::vector<double>& V)
		{
			double M = 0.0, D = 0.0;
			for (size_t I = 1; I < V.size(); ++I) { if (V[I] > 0.0 && V[I - 1] > 0.0) { D += std::abs(V[I] - V[I - 1]); M += V[I]; } }
			return M > 0.0 ? D / M : 0.0;
		};
		ContrastPlain = Contrast(Ap); ContrastDetail = Contrast(Ad);
	}
	const bool bOk = bOffSame && bDetailOffSame && DetailRatio > 0.65 && DetailRatio < 1.35 && ContrastDetail > 1.25 * ContrastPlain && WebCarriers >= 2 && D1 > 0.0 && std::abs(Lin - 1.0) < 0.002 && Once > 0.85 && Once < 1.15
		&& DShallow < 0.02 * D1 && D25 < D1 && WorstLum < 1e-4 && BrS > Br0;
	Uni::Check(2, "T2.look.motion_web", "D-230 the motion web from intergalactic space with the delivered web: off is bit for bit today's march; the light it adds is linear in its gain, drawn once a step though two web carriers read the texel (within 15% of one carrier's), under 2% of it from within 2 Mpc (faded in over 4), less at depth 25 than 50; the shift keeps every pixel's luminance and turns the view ahead bluer; D-231: its detail at 1.0 (steps capped at 0.5 Mpc in the web's band) keeps the added light within 35% and raises its pixel-to-pixel contrast over 1.25x at 128 x 72")
		.Metric("added_light", D1 / std::max(L0, 1e-300)).Metric("linearity", Lin).Metric("once_ratio", Once).Metric("shallow_share", DShallow / std::max(D1, 1e-300)).Metric("shift_lum_err", WorstLum)
		.Metric("detail_light_ratio", DetailRatio).Metric("detail_contrast", ContrastDetail / std::max(ContrastPlain, 1e-300))
		.Pass(bOk, Uni::Fmt("detail 1.0 (20 Mpc, steps 0.5 Mpc): light x%.2f, pixel-to-pixel contrast x%.1f; %zu web carriers; off identical %s; adds %.1f%% to the frame's light at 0.01, x%.4f at twice the gain, %.3f of one carrier's with two; from within 2 Mpc %.2f%% of it, depth 25 Mpc %.0f%%; shift: luminance within %.1e, blue/red x%.2f ahead",
			DetailRatio, ContrastDetail / std::max(ContrastPlain, 1e-300), WebCarriers, bOffSame ? "yes" : "NO", 100.0 * D1 / std::max(L0, 1e-300), Lin, Once, 100.0 * DShallow / std::max(D1, 1e-300), 100.0 * D25 / std::max(D1, 1e-300), WorstLum, BrS / std::max(Br0, 1e-300)));
}

UNI_TEST(look, web_swim)
{
	// D-231 (owner: the flight showed the motion web's detail sliding). Flying at 10 Mpc/s (0.167 Mpc a
	// frame), how much more the march's image changes frame to frame than a fine reference's (steps
	// an eighth, the detail blurred over the march's own step: MarchFlags bits 1 and 2) does. Real
	// motion shows in both; swimming through the march's sample shells only in the march. With the
	// detail sampled on the march's long steps: 2.27% of the light a frame (37% over the reference's
	// change); the plain motion web: 0.25%; steps capped at 0.5 Mpc in the web's band, the detail
	// blurred over them: 0.03-0.05%.
	FWebTexture T;
	std::string Err;
	if (!FWebTexture::LoadDelivered("Data/Noise", "web_adhesion_density_mid", T, &Err)) { Uni::Check(2, "T2.look.web_swim", "D-231 the motion web does not swim").Pass(false, "texture not loaded: " + Err); return; }
	FBind Bind(T);
	FWebTexture Gt[3];
	for (int K = 0; K < 3; ++K) { std::string E; FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(K), Gt[K], &E); BindGalaxyTexture(K, &Gt[K]); }
	struct FUnbind { ~FUnbind() { for (int K = 0; K < 3; ++K) { BindGalaxyTexture(K, nullptr); } } } Unbind;
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w));
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const double Mpc = Units::Mpc;
	const FV3 Ahead = Norm(FV3{ 1.0, 0.3, 0.1 });
	const FV3 Look = Norm(FV3{ Ahead.X + 0.25, Ahead.Y - 0.5, Ahead.Z });
	const int W = 128, Ht = 72, Frames = 6;
	double DA = 0.0, DR = 0.0;
	std::vector<double> PrevA, PrevR;
	for (int F = 0; F < Frames; ++F)
	{
		FCoord Cam = UniScenes::VoidCamera();
		{ FVec3f Fr{}; AddDisplacement(Cam, Fr, FVec3d{ Ahead.X * 0.167 * F * Mpc, Ahead.Y * 0.167 * F * Mpc, Ahead.Z * 0.167 * F * Mpc }); }
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = W; Cm.Height = Ht; Cm.LookAlong(Look);
		FVolumeSettings S;
		S.MotionWebGain = float(0.01 * 7.0e4 / Mpc);
		S.MotionWebInvRef = float(1.0 / ((double(Mean.y) + double(Mean.z)) * 3.0));
		S.MotionWebNearCm = float(4.0 * Mpc);
		S.MotionWebDepthCm = float(50.0 * Mpc);
		S.MotionWebDetailGain = 1.0f;
		S.MotionWebDetailCm = float(20.0 * Mpc);
		S.MotionWebStepCm = float(0.5 * Mpc);
		S.MotionWebRegionCm = float(200.0 * Mpc);   // D-233: as the subsystem's default
		FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(true);
		for (FCarrierGPU& C : Sc.Carriers) { C.Flags &= ~H::UNI_CARRIER_TAIL; }
		FVolumeScene Fine = Sc;
		Fine.View.MarchFlags |= 2u | 4u;
		Fine.View.MaxSteps *= 8u;
		const std::vector<H::float3> A = Frame(Sc, Cm), R = Frame(Fine, Cm);
		std::vector<double> La(A.size()), Lr(A.size());
		double Sum = 0.0;
		for (size_t I = 0; I < A.size(); ++I) { La[I] = H::Uni_Luminance(A[I]); Lr[I] = H::Uni_Luminance(R[I]); Sum += Lr[I]; }
		if (F > 0)
		{
			double a = 0.0, r = 0.0;
			for (size_t I = 0; I < A.size(); ++I) { a += std::abs(La[I] - PrevA[I]); r += std::abs(Lr[I] - PrevR[I]); }
			DA += a / std::max(Sum, 1e-300); DR += r / std::max(Sum, 1e-300);
		}
		PrevA = La; PrevR = Lr;
	}
	DA /= double(Frames - 1); DR /= double(Frames - 1);
	const double Excess = DA - DR;
	Uni::Check(2, "T2.look.web_swim", "D-231 flying at 10 Mpc/s with the motion web and its detail (steps capped at 0.5 Mpc in its band, the detail blurred over them; D-233: mixed by 200 Mpc regions), the march's image changes frame to frame no more than 0.3% of the light beyond what a fine reference's does (on the march's long steps: 2.27%)")
		.Metric("excess", Excess).Metric("march_change", DA).Metric("reference_change", DR)
		.Pass(DR > 0.0 && Excess < 0.003, Uni::Fmt("frame-to-frame change: march %.2f%%, reference %.2f%% of the light (excess %.2f%%, %.0f%% of the reference's)", 100.0 * DA, 100.0 * DR, 100.0 * Excess, 100.0 * Excess / std::max(DR, 1e-300)));
}

namespace
{
	std::string LookRenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}
}

UNI_TEST(look, web_regions)
{
	// D-233 (owner: "it would be nice for the universe web noise to get balanced in different ways
	// based on regions so that different areas of the web have different noise modulations").
	FWebTexture T;
	std::string Err;
	if (!FWebTexture::LoadDelivered("Data/Noise", "web_adhesion_density_mid", T, &Err)) { Uni::Check(2, "T2.look.web_regions", "D-233 regional web detail").Pass(false, "texture not loaded: " + Err); return; }
	FBind Bind(T);
	FWebTexture Gt[3];
	for (int K = 0; K < 3; ++K) { std::string E; FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(K), Gt[K], &E); BindGalaxyTexture(K, &Gt[K]); }
	struct FUnbind { ~FUnbind() { for (int K = 0; K < 3; ++K) { BindGalaxyTexture(K, nullptr); } } } Unbind;

	// The weights over a web tile (one region period): they sum to 1, each channel leads somewhere,
	// one usually leads clearly.
	int Lead[4] = { 0, 0, 0, 0 };
	std::vector<double> MaxShare;
	double WorstSum = 0.0;
	const int G = 40;
	for (int X = 0; X < G; ++X) for (int Y = 0; Y < G; ++Y) for (int Z = 0; Z < G; ++Z)
	{
		const H::float4 W = H::Uni_MotionWebRegionWeights(H::float3((X + 0.5f) / G, (Y + 0.5f) / G, (Z + 0.5f) / G), 1.0f);
		const float V[4] = { W.x, W.y, W.z, W.w };
		int Best = 0;
		for (int K = 1; K < 4; ++K) { if (V[K] > V[Best]) { Best = K; } }
		++Lead[Best];
		MaxShare.push_back(V[Best]);
		WorstSum = std::max(WorstSum, std::abs(double(W.x + W.y + W.z + W.w) - 1.0));
	}
	std::sort(MaxShare.begin(), MaxShare.end());
	const double Median = MaxShare[MaxShare.size() / 2];
	const double N3 = double(G) * G * G;
	const double LeadMin = double(std::min(std::min(Lead[0], Lead[1]), std::min(Lead[2], Lead[3]))) / N3;

	// Each channel alone: its pattern's mean (the light) and contrast against the ridged one's, over
	// the detail texture at mip 0; the ridged weights give D-231's R^3 bit for bit.
	double Mean[4] = { 0, 0, 0, 0 }, Sq[4] = { 0, 0, 0, 0 };
	size_t RidgedBad = 0;
	const int Ns = 200000;
	for (int I = 0; I < Ns; ++I)
	{
		const H::float3 Pt(H::Uni_DrawUnit(0xD233u, 0x1u, u32(3 * I)), H::Uni_DrawUnit(0xD233u, 0x1u, u32(3 * I + 1)), H::Uni_DrawUnit(0xD233u, 0x1u, u32(3 * I + 2)));
		const H::float4 Dt = UNI_GAL_DETAIL(Pt, 0.0f);
		for (int K = 0; K < 4; ++K)
		{
			const H::float4 W(K == 0 ? 1.0f : 0.0f, K == 1 ? 1.0f : 0.0f, K == 2 ? 1.0f : 0.0f, K == 3 ? 1.0f : 0.0f);
			const double V = double(H::Uni_MotionWebRegionPattern(Dt, W));
			Mean[K] += V / Ns; Sq[K] += V * V / Ns;
			if (K == 0)
			{
				const float R = Dt.x / H::UNI_MOTION_WEB_DETAIL_MEAN;
				RidgedBad += float(V) != R * R * R ? 1 : 0;
			}
		}
	}
	double WorstMean = 0.0, WorstContrast = 0.0;
	const double C0 = std::sqrt(std::max(Sq[0] - Mean[0] * Mean[0], 0.0)) / Mean[0];
	for (int K = 1; K < 4; ++K)
	{
		WorstMean = std::max(WorstMean, std::abs(Mean[K] / Mean[0] - 1.0));
		WorstContrast = std::max(WorstContrast, std::abs(std::sqrt(std::max(Sq[K] - Mean[K] * Mean[K], 0.0)) / Mean[K] / C0 - 1.0));
	}

	// The frame (intergalactic void, 10 Mpc/s look): the added light with regions within 15% of
	// the ridged detail's, and the frames differ.
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mn = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mn.z / (double(Mn.x) + Mn.y + Mn.z + Mn.w));
	FUniverseModel M(Def);
	FRefGenerator Gn(M);
	FVolumeBuilder VB(M, Gn);
	FRefPipeline P(Gn);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::VoidCamera();
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 128; Cm.Height = 72; Cm.LookAlong({ 1.0, 0.3, 0.1 });
	const double Mpc = Units::Mpc;
	auto Scene = [&](double Gain, double RegionMpc)
	{
		FVolumeSettings S;
		S.MotionWebGain = float(Gain * 7.0e4 / Mpc);
		S.MotionWebInvRef = float(1.0 / ((double(Mn.y) + double(Mn.z)) * 3.0));
		S.MotionWebNearCm = float(4.0 * Mpc);
		S.MotionWebDepthCm = float(50.0 * Mpc);
		S.MotionWebDetailGain = 1.0f;
		S.MotionWebDetailCm = float(20.0 * Mpc);
		S.MotionWebStepCm = float(0.5 * Mpc);
		S.MotionWebRegionCm = float(RegionMpc * Mpc);
		return VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(true);
	};
	const double L0 = SumLum(Frame(Scene(0.0, 0.0), Cm));
	const std::vector<H::float3> Ar = Frame(Scene(0.01, 0.0), Cm), Ag = Frame(Scene(0.01, 200.0), Cm);
	const double Dr = SumLum(Ar) - L0, Dg = SumLum(Ag) - L0;
	const double FrameRatio = Dr > 0.0 ? Dg / Dr : 0.0;
	double FrameDiff = 0.0;
	for (size_t I = 0; I < Ar.size(); ++I) { FrameDiff += std::abs(double(H::Uni_Luminance(Ar[I])) - double(H::Uni_Luminance(Ag[I]))); }
	FrameDiff /= std::max(Dr, 1e-300);

	// A slice through the web tile (200 Mpc square, 8 Mpc thick): the motion web's emission with
	// the regions (left) and the ridged detail everywhere (right).
	{
		const int Px = 300, Nz = 16;
		FImage Img(Px * 2, Px);
		FVolumeScene Sc = Scene(0.01, 200.0);
		H::FUniVolumeView V = ToHlsl(Sc.View);
		const float InvR = float(1.0 / (200.0 * Mpc));   // one web tile is one field unit
		std::vector<double> Lr(size_t(Px) * Px * 2, 0.0);
		double Peak = 0.0;
		for (int Side = 0; Side < 2; ++Side)
		{
			V.MotionWebRegionCm = Side == 0 ? float(200.0 * Mpc) : 0.0f;
			for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
			{
				double Sum = 0.0;
				for (int Z = 0; Z < Nz; ++Z)
				{
					const H::float3 Pt((X + 0.5f) / Px, (Y + 0.5f) / Px, 0.3f + 0.04f * (Z + 0.5f) / Nz);
					const H::float4 Ch = UNI_WEB_TEX_SAMPLE(Pt, 0.0f);
					const H::float3 E = H::Uni_MotionWebEmission(Ch, Pt, 1.0f / Px, 0.04f / Nz, InvR, float(20.0 * Mpc), V);
					Sum += double(H::Uni_Luminance(E));
				}
				Lr[size_t(Side) * Px * Px + size_t(Y) * Px + X] = Sum;
				Peak = std::max(Peak, Sum);
			}
		}
		for (int Side = 0; Side < 2; ++Side) for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
		{
			const float I = float(6.0 * Lr[size_t(Side) * Px * Px + size_t(Y) * Px + X] / std::max(Peak, 1e-300));
			Img.Add(Side * Px + X, Y, I * 0.82f, I * 0.9f, I);
		}
		const std::string Base = LookRenderDir() + "/still_motion_web_regions";
		Img.WritePpm(Base + ".ppm");
		if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb"))
		{
			std::fprintf(F, "{\"lines\":[\"D-233: a 200 Mpc slice (8 Mpc thick) of the motion web, regions (left) and the ridged detail everywhere (right)\",\"6 x the peak into the PPM tonemap\"]}\n");
			std::fclose(F);
		}
	}

	const bool bOk = WorstSum < 1e-5 && LeadMin > 0.12 && Median > 0.5 && Median < 0.85 && RidgedBad == 0 && WorstMean < 0.05 && WorstContrast < 0.25
		&& FrameRatio > 0.85 && FrameRatio < 1.15 && FrameDiff > 0.05;
	Uni::Check(2, "T2.look.web_regions", "D-233 the motion web's detail by region: the weights sum to 1, each channel leads in over 12% of a tile, one leads with 50-85% at the median; each channel alone has the ridged one's light within 5% and contrast within 25%, the ridged weights give D-231's pattern bit for bit; the frame's added light within 15% of the ridged detail's, and the frame changes")
		.Metric("lead_min", LeadMin).Metric("median_share", Median).Metric("mean_worst", WorstMean).Metric("contrast_worst", WorstContrast).Metric("frame_ratio", FrameRatio).Metric("frame_diff", FrameDiff)
		.Pass(bOk, Uni::Fmt("weights: sum within %.1e, lead R %.0f%% G %.0f%% B %.0f%% A %.0f%%, median leading share %.2f; alone: light within %.1f%%, contrast within %.0f%% of the ridged (%.2f), ridged weights %zu different from R^3; frame: added light x%.3f, per-pixel change %.0f%% of it",
			WorstSum, 100.0 * Lead[0] / N3, 100.0 * Lead[1] / N3, 100.0 * Lead[2] / N3, 100.0 * Lead[3] / N3, Median, 100.0 * WorstMean, 100.0 * WorstContrast, C0, RidgedBad, FrameRatio, 100.0 * FrameDiff));
}

UNI_TEST(look, web_dust)
{
	// D-269 (owner 2026-11-16: "dark matter dust type absorption in the universe cosmic web field ...
	// like dark knots or filaments", with the galaxy march's lessons): the web's dark dust, from the
	// void camera with the delivered web and the galaxy textures (galaxy_shape for the pattern), at
	// 128 x 72. Held:
	// - off (no optical depth) is bit for bit the march without it;
	// - the frame dims with the optical depth, about as the absorption doubles (ln of the light's
	//   ratio x1.6-2.2 from 1 to 2 per 100 Mpc);
	// - once a step: one web carrier reading the texel gives the same dimming as two (2%);
	// - grey (Redden 0) keeps the frame's blue/red within 0.5%; reddened (1) lowers it;
	// - the pattern keeps the dust's amount: the frame with it within 5% of the frame with the
	//   matter alone (mean 1; a clumpy medium lets a little more through);
	// - nothing past a 0.5 Mpc depth: the frame within 5e-4 of the frame without (the sliver inside
	//   it, faded in from the camera: 1.5e-4);
	// - the grain: 8 jitter phases, the spread over them with the dust at most 2 points over without.
	FWebTexture T;
	std::string Err;
	if (!FWebTexture::LoadDelivered("Data/Noise", "web_adhesion_density_mid", T, &Err)) { Uni::Check(2, "T2.look.web_dust", "D-269 the web's dark dust").Pass(false, "texture not loaded: " + Err); return; }
	FBind Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w));
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::VoidCamera();
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 128; Cm.Height = 72; Cm.LookAlong({ 1.0, 0.3, 0.1 });
	const double Mpc = Units::Mpc;
	FWebTexture Gt[3];
	for (int K = 0; K < 3; ++K) { std::string E; FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(K), Gt[K], &E); BindGalaxyTexture(K, &Gt[K]); }
	struct FUnbind { ~FUnbind() { for (int K = 0; K < 3; ++K) { BindGalaxyTexture(K, nullptr); } } } Unbind;
	struct FDust { double Tau = 0.0, Detail = 0.8, Knots = 0.5, Redden = 0.5, DepthMpc = 50.0; };
	auto Scene = [&](const FDust& Ds)
	{
		FVolumeSettings S;
		if (Ds.Tau > 0.0)
		{
			S.WebDustTauCm = float(Ds.Tau / (100.0 * Mpc)); S.WebDustInvMean = float(1.0 / (double(Mean.y) + double(Mean.z)));
			S.WebDustNearCm = float(2.0 * Mpc); S.WebDustDepthCm = float(Ds.DepthMpc * Mpc);
			S.WebDustDetail = float(Ds.Detail); S.WebDustDetailCm = float(30.0 * Mpc); S.WebDustKnots = float(Ds.Knots);
			S.WebDustPeak = 30.0f; S.WebDustTexN = float(Gt[2].Size); S.WebDustRedden = float(Ds.Redden);
		}
		return VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(true);
	};
	auto BlueRed = [](const std::vector<H::float3>& A) { double B = 0.0, R = 0.0; for (const H::float3& V : A) { B += V.z; R += V.x; } return B / std::max(R, 1e-300); };
	const FVolumeScene SDef = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(true);
	const std::vector<H::float3> A0 = Frame(Scene(FDust{}), Cm), ADef = Frame(SDef, Cm);
	bool bOff = A0.size() == ADef.size();
	for (size_t I = 0; bOff && I < A0.size(); ++I) { bOff = A0[I].x == ADef[I].x && A0[I].y == ADef[I].y && A0[I].z == ADef[I].z; }
	const double L0 = SumLum(A0), Br0 = BlueRed(A0);
	FDust D1; D1.Tau = 1.0;
	FDust D2; D2.Tau = 2.0;
	const std::vector<H::float3> A1 = Frame(Scene(D1), Cm);
	const double L1 = SumLum(A1), L2 = SumLum(Frame(Scene(D2), Cm));
	const double Dbl = std::log(L2 / L0) / std::min(std::log(L1 / L0), -1e-12);
	// Once a step: the scene with one web carrier against two.
	double Once = 0.0;
	{
		auto Strip = [&](FVolumeScene Sc)
		{
			bool bFirst = true;
			std::vector<FCarrierGPU> Keep;
			for (const FCarrierGPU& C : Sc.Carriers)
			{
				const bool bWeb = (C.Flags & H::UNI_CARRIER_PERIODIC) && C.FieldId == H::UNI_FIELD_WEB;
				if (bWeb && !bFirst) { continue; }
				if (bWeb) { bFirst = false; }
				Keep.push_back(C);
			}
			Sc.Carriers = Keep;
			Sc.View.NumCarriers = u32(Keep.size());
			return Sc;
		};
		const double B0 = SumLum(Frame(Strip(Scene(FDust{})), Cm)), B1 = SumLum(Frame(Strip(Scene(D1)), Cm));
		Once = std::log(B1 / B0) / std::min(std::log(L1 / L0), -1e-12);
	}
	FDust Grey = D1; Grey.Redden = 0.0;
	FDust Red = D1; Red.Redden = 1.0;
	const double BrGrey = BlueRed(Frame(Scene(Grey), Cm)), BrRed = BlueRed(Frame(Scene(Red), Cm));
	FDust Plain = D1; Plain.Detail = 0.0;
	const double LPlain = SumLum(Frame(Scene(Plain), Cm));
	FDust Shallow = D1; Shallow.DepthMpc = 0.5;
	const double LShallow = SumLum(Frame(Scene(Shallow), Cm));
	// The grain: 8 jitter phases' spread a pixel, with and without the dust.
	auto Spread = [&](const FDust& Ds)
	{
		FVolumeScene Sc = Scene(Ds);
		Sc.View.MarchFlags |= H::UNI_VOL_JITTER_FLAG;
		const size_t Np = 8, N = size_t(Cm.Width) * Cm.Height;
		std::vector<double> S1(N, 0.0), S2(N, 0.0);
		for (size_t Ph = 0; Ph < Np; ++Ph)
		{
			Sc.View.JitterShift = float(Ph) * 0.61803398875f;
			const std::vector<H::float3> A = Frame(Sc, Cm);
			for (size_t I = 0; I < N; ++I) { const double L = H::Uni_Luminance(A[I]); S1[I] += L / Np; S2[I] += L * L / Np; }
		}
		double Ss = 0.0, Sm = 0.0;
		for (size_t I = 0; I < N; ++I) { Sm += S1[I]; Ss += std::sqrt(std::max(S2[I] - S1[I] * S1[I], 0.0)); }
		return Ss / std::max(Sm, 1e-300);
	};
	const double Sp0 = Spread(FDust{}), Sp1 = Spread(D1);
	const bool bOk = bOff && L1 < L0 && L2 < L1 && Dbl > 1.6 && Dbl < 2.2 && std::abs(Once - 1.0) < 0.02
		&& std::abs(BrGrey / Br0 - 1.0) < 0.005 && BrRed < Br0 && std::abs(LPlain / L1 - 1.0) < 0.05 && std::abs(LShallow / L0 - 1.0) < 5e-4 && Sp1 - Sp0 < 0.02;
	Uni::Check(2, "T2.look.web_dust", "D-269 the web's dark dust (void camera, delivered web, galaxy_shape's pattern): off bit for bit; the frame dims with the optical depth (ln ratio x1.6-2.2 as it doubles); once a step (one web carrier as two, 2%); grey keeps the frame's blue/red (0.5%), reddened lowers it; the pattern keeps the dust's amount (5%); nothing past a 0.5 Mpc depth (5e-4); the grain over 8 jitter phases at most 2 points over the frame without")
		.Metric("light_tau1", L1 / L0).Metric("doubling", Dbl).Metric("once", Once).Metric("spread_x", Sp1 / std::max(Sp0, 1e-300))
		.Pass(bOk, Uni::Fmt("off bit for bit %s; light x%.4f at 1 per 100 Mpc, x%.4f at 2 (ln ratio x%.3f); one web carrier x%.4f of two's dimming; blue/red %.4f without, %.4f grey, %.4f reddened; the matter alone x%.4f of the patterned frame's light; 0.5 Mpc depth x%.6f; spread %.3f%% without, %.3f%% with",
			bOff ? "yes" : "NO", L1 / L0, L2 / L0, Dbl, Once, Br0, BrGrey, BrRed, LPlain / L1, LShallow / L0, 100.0 * Sp0, 100.0 * Sp1));
}

UNI_TEST(look, slice_reach)
{
	// D-271 (the galaxy stack review: the slices ended at 100 kpc, so a sprite past it, a galaxy
	// behind the web's dark dust, D-269, was never dimmed): the far segment, 4 slices evenly in t
	// from 100 kpc to 50 Mpc. From the void camera with the delivered web and the engine's web dust
	// (3 per 100 Mpc, its pattern), at 48 x 27 rays and 20 distances from 0.5 to 50 Mpc, against the
	// true transmittance (the march stopped at each distance). Held:
	// - the near slices (1 pc .. 100 kpc) bit for bit with and without the far segment;
	// - without it (SliceFarCm at or below 100 kpc, the harness default), every sprite past 100 kpc
	//   reads the slice at 100 kpc (D-130's) exactly;
	// - with it, the mean error a distance under 0.045 and its mean over the distances under 0.012
	//   (without: the dust's whole dimming, 0.30 at 50 Mpc; four slices cannot place each filament:
	//   with the boundaries exact, experiments/slicefar_d271, 0.032 and 0.0098);
	// - the far cache's slice rule (Uni_FarSliceT): a far slice ending at or before the cache's start
	//   is 1, after it the far march's.
	FWebTexture T;
	std::string Err;
	if (!FWebTexture::LoadDelivered("Data/Noise", "web_adhesion_density_mid", T, &Err)) { Uni::Check(2, "T2.look.slice_reach", "D-271 the slices' far segment").Pass(false, "texture not loaded: " + Err); return; }
	FBind Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w));
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::VoidCamera();
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 48; Cm.Height = 27; Cm.LookAlong({ 1.0, 0.3, 0.1 });
	const double Mpc = Units::Mpc;
	FWebTexture Gt[3];
	for (int K = 0; K < 3; ++K) { std::string E; FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(K), Gt[K], &E); BindGalaxyTexture(K, &Gt[K]); }
	struct FUnbind { ~FUnbind() { for (int K = 0; K < 3; ++K) { BindGalaxyTexture(K, nullptr); } } } Unbind;
	auto Scene = [&](double FarCm)
	{
		FVolumeSettings S;
		S.WebDustTauCm = float(3.0 / (100.0 * Mpc)); S.WebDustInvMean = float(1.0 / (double(Mean.y) + double(Mean.z)));
		S.WebDustNearCm = float(2.0 * Mpc); S.WebDustDepthCm = float(50.0 * Mpc);
		S.WebDustDetail = 1.0f; S.WebDustDetailCm = float(30.0 * Mpc); S.WebDustKnots = 0.3f;
		S.WebDustPeak = 30.0f; S.WebDustTexN = float(Gt[2].Size); S.WebDustRedden = 0.5f;
		S.SliceFarCm = FarCm;
		return VB.Build(P, Cam, FVec3f{}, Cm.View(), S);
	};
	const FVolumeScene S0 = Scene(0.0), S1 = Scene(50.0 * Mpc);
	const H::FUniVolumeView V0 = ToHlsl(S0.View), V1 = ToHlsl(S1.View);
	const auto Cb = S1.CarrierBuf(); const auto Pb = S1.ParamBuf(); const auto Lb = S1.LutBuf();
	const auto Cb0 = S0.CarrierBuf(); const auto Pb0 = S0.ParamBuf(); const auto Lb0 = S0.LutBuf();
	const int Nd = 20;
	std::vector<double> Dist(Nd), Err1(Nd, 0.0), Err0(Nd, 0.0), TSum(Nd, 0.0);
	for (int I = 0; I < Nd; ++I) { Dist[size_t(I)] = 0.5 * Mpc * std::pow(100.0, double(I) / (Nd - 1)); }
	const double HiCm = std::exp(double(V1.SliceLnTHi));
	size_t NearDiffer = 0, LegacyBad = 0, Rays = 0;
	for (int Y = 0; Y < Cm.Height; ++Y) for (int X = 0; X < Cm.Width; ++X)
	{
		const FV3 D = PixelDir(Cm, Cm.Width, Cm.Height, X + 0.5, Y + 0.5);
		const H::float3 Dc(float(D.X), float(D.Y), float(D.Z));
		H::FUniVolumeResult R1 = H::Uni_VolumeMarch(Dc, 3.0e38f, Cb, Pb, Lb, V1);
		H::FUniVolumeResult R0 = H::Uni_VolumeMarch(Dc, 3.0e38f, Cb0, Pb0, Lb0, V0);
		for (u32 K = 0; K < H::UNI_VOL_SLICES_NEAR; ++K) { NearDiffer += (R1.SliceT[K].x != R0.SliceT[K].x || R1.SliceT[K].y != R0.SliceT[K].y || R1.SliceT[K].z != R0.SliceT[K].z) ? 1 : 0; }
		for (int I = 0; I < Nd; ++I)
		{
			const double Td = H::Uni_Luminance(H::Uni_VolumeMarch(Dc, float(Dist[size_t(I)]), Cb, Pb, Lb, V1).Transmittance);
			const double T1 = H::Uni_Luminance(H::Uni_SliceTransmittance(R1.SliceT, float(Dist[size_t(I)]), V1));
			const H::float3 T0v = H::Uni_SliceTransmittance(R0.SliceT, float(Dist[size_t(I)]), V0);
			const H::float3 At = R0.SliceT[H::UNI_VOL_SLICES_NEAR - 1u];
			if (Dist[size_t(I)] > HiCm && !(T0v.x == At.x && T0v.y == At.y && T0v.z == At.z)) { ++LegacyBad; }
			Err1[size_t(I)] += std::abs(T1 - Td); Err0[size_t(I)] += std::abs(double(H::Uni_Luminance(T0v)) - Td); TSum[size_t(I)] += Td;
		}
		++Rays;
	}
	double Worst1 = 0.0, Mean1 = 0.0, Worst0 = 0.0, Mean0 = 0.0;
	for (int I = 0; I < Nd; ++I)
	{
		const double E1 = Err1[size_t(I)] / double(Rays), E0 = Err0[size_t(I)] / double(Rays);
		Worst1 = std::max(Worst1, E1); Mean1 += E1 / Nd; Worst0 = std::max(Worst0, E0); Mean0 += E0 / Nd;
	}
	// The far cache's rule: a cache from 20 Mpc on holds nothing at the far boundaries before it.
	bool bFarRule = true;
	for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K)
	{
		const float Bk = K < H::UNI_VOL_SLICES_NEAR ? float(std::exp(double(V1.SliceLnTLo) + (double(V1.SliceLnTHi) - double(V1.SliceLnTLo)) * double(K + 1u) / H::UNI_VOL_SLICES_NEAR))
			: float(HiCm + (double(V1.SliceFarCm) - HiCm) * double(K + 1u - H::UNI_VOL_SLICES_NEAR) / double(H::UNI_VOL_SLICES - H::UNI_VOL_SLICES_NEAR));
		const float Got = H::Uni_FarSliceT(K, 0.25f, float(20.0 * Mpc), V1);
		const double Rel = double(Bk) / (20.0 * Mpc);
		if (Rel < 0.999) { bFarRule = bFarRule && Got == 1.0f; }
		else if (Rel > 1.001) { bFarRule = bFarRule && Got == 0.25f; }
	}
	const bool bOk = NearDiffer == 0 && LegacyBad == 0 && Worst1 < 0.045 && Mean1 < 0.012 && bFarRule && Rays > 1000;
	Uni::Check(2, "T2.look.slice_reach", "D-271 the sprites' transmittance past 100 kpc (the slices' far segment: 4 evenly in t to 50 Mpc), void camera, the engine's web dust: the near slices bit for bit with and without it; without it every sprite past 100 kpc reads the slice at 100 kpc (D-130's) exactly; with it the error against the true transmittance (0.5-50 Mpc) under 0.045 at every distance and 0.012 on average (without: 0.30, 0.07); the far cache's slice rule")
		.Metric("err_mean", Mean1).Metric("err_worst", Worst1).Metric("err_mean_without", Mean0).Metric("t_50mpc", TSum.back() / double(Rays))
		.Pass(bOk, Uni::Fmt("%zu rays: near slices %zu different; without the far segment %zu sprites past 100 kpc off the 100 kpc slice; error a distance: mean %.4f, worst %.4f (without: %.4f, %.4f; the true transmittance at 50 Mpc %.3f on average); far cache rule %s",
			Rays, NearDiffer, LegacyBad, Mean1, Worst1, Mean0, Worst0, TSum.back() / double(Rays), bFarRule ? "yes" : "NO"));
}

UNI_TEST(look, dust_curve)
{
	// D-270 (B-036): uu.Volume.DustCurve, the galaxy dust's colour from D-132's reddening toward
	// Cardelli, Clayton & Mathis' R_V 3.1 curve at the screen primaries' wavelengths. Held: the
	// formula within 1.5% of their Table 3 (U..K, R_V 3.1), 0 D-132's exactly and the harness's
	// default, 1 the curve's ratios at the channels (green 1), red < green < blue at every blend,
	// each channel monotone in the blend, out-of-range blends clamped.
	struct FBand { const char* N; double L, V; };
	const FBand Bands[] = { { "U", 0.365, 1.569 }, { "B", 0.44, 1.337 }, { "V", 0.55, 1.0 }, { "R", 0.70, 0.751 }, { "I", 0.90, 0.479 }, { "J", 1.25, 0.282 }, { "H", 1.6, 0.190 }, { "K", 2.2, 0.114 } };
	bool bPass = true;
	double WorstBand = 0.0;
	for (const FBand& B : Bands) { WorstBand = std::max(WorstBand, std::abs(CardelliAlav(B.L) / B.V - 1.0)); }
	bPass = bPass && WorstBand <= 0.015;
	float C0[3], C1[3], Lo[3], Hi[3];
	DustCurveRgb(0.0f, C0); DustCurveRgb(1.0f, C1); DustCurveRgb(-1.0f, Lo); DustCurveRgb(5.0f, Hi);
	const FVolumeSettings D0;
	bool bZero = true, bOne = true, bClamp = true;
	for (int K = 0; K < 3; ++K)
	{
		bZero = bZero && C0[K] == D0.DustSigmaRgb[K];
		const double Want = CardelliAlav(DustCurveLambdaUm[K]) / CardelliAlav(DustCurveLambdaUm[1]);
		bOne = bOne && std::abs(double(C1[K]) - Want) <= 1e-6;
		bClamp = bClamp && Lo[K] == C0[K] && Hi[K] == C1[K];
	}
	bool bOrder = true, bMono = true;
	float Prev[3]; DustCurveRgb(0.0f, Prev);
	for (int I = 1; I <= 20; ++I)
	{
		float C[3]; DustCurveRgb(float(I) / 20.0f, C);
		bOrder = bOrder && C[0] < C[1] && C[1] < C[2] && C[1] == 1.0f;
		for (int K = 0; K < 3; ++K) { bMono = bMono && (C1[K] >= C0[K] ? C[K] >= Prev[K] : C[K] <= Prev[K]); Prev[K] = C[K]; }
	}
	bPass = bPass && bZero && bOne && bClamp && bOrder && bMono;
	Uni::Check(1, "T1.look.dust_curve", "D-270 uu.Volume.DustCurve: Cardelli's A/A_V within 1.5% of their Table 3 (U..K, R_V 3.1); 0 D-132's (0.8, 1, 1.3) and the harness default exactly, 1 Cardelli's ratios at the channels (green 1), red < green < blue and each channel monotone across the blend, clamped outside [0, 1]")
		.Metric("worst_band", WorstBand)
		.Pass(bPass, Uni::Fmt("worst band %.2f%%; 0: (%.3f, %.3f, %.3f)%s; 1: (%.4f, %.4f, %.4f)%s; order %s, monotone %s, clamped %s",
			100.0 * WorstBand, C0[0], C0[1], C0[2], bZero ? " = D-132" : " DIFFERS", C1[0], C1[1], C1[2], bOne ? " = Cardelli" : " DIFFERS", bOrder ? "yes" : "NO", bMono ? "yes" : "NO", bClamp ? "yes" : "NO"));
}
