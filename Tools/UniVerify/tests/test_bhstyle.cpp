// test_bhstyle.cpp
// D-280 (owner 2026-11-17 22:35): each black hole's look (UniCompact.ush: Uni_BhStyleOf, the
// palettes; UniCompact.h: StyledLook, GalaxyBhStyle).
//   style     over 10^4 galaxy records: variety 0 every multiplier exactly 1 and the blackbody (D-278's
//             look); variety 1 every multiplier in its documented range; tori only on slow
//             accretors, thin discs on fast ones; every palette used, the blue / white ones more
//             often on fast accretors, the warm ones more on early types; the spiral following the
//             host's arms; variety 1/2 the square root of 1's; a forced palette everywhere.
//   palette   a palette changes the colour only: the light's luminance the blackbody's (1e-4); palette
//             0 the blackbody bit for bit (light, nucleus colour); the disc's colour averaged over
//             directions against the nucleus sprite's in the same palette (the hand-off's colour); the
//             volumetric disc's cost at the look's extremes; still render/still_bh_styles.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../src/UniLensRef.h"
#include "../scenes/UniScenes.h"
#include "UniCompact.h"
#include "UniDemoScenes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FRec { u32 Seed; float Mag; bool bEarly; };
	std::vector<FRec> Records(int N)
	{
		std::vector<FRec> S;
		for (int I = 0; I < N; ++I)
		{
			const u32 Seed = H::Uni_SeedMix(0xB15Eu, u32(I));
			S.push_back({ Seed, -23.5f + 7.5f * H::Uni_DrawUnit(Seed, 0x51u, 0u), H::Uni_DrawUnit(Seed, 0x51u, 1u) < 0.25f });
		}
		return S;
	}

	FObjectGPU GalaxyObject(const FRec& R)
	{
		return FromHlsl(H::Uni_MakeObject(H::uint3(0u, 0u, 0u), UniScenes::kGalaxy, R.bEarly ? H::UNI_OBJ_FLAG_EARLY | H::UNI_OBJ_FLAG_EXTENDED : H::UNI_OBJ_FLAG_EXTENDED,
			R.Seed, 70.0f, R.Mag, 22.0f, H::float3(1.0f, 0.9f, 0.8f)));
	}

	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	float Mul(const H::FUniBhStyle& S, int K)
	{
		const float V[10] = { S.SpiralMul, S.ThicknessMul, S.OpacityMul, S.CloudsMul, S.BandsMul, S.HeatMul, S.ErosionMul, S.PeriodMul, S.BrightnessMul, S.ExtentMul };
		return V[K];
	}

	/** Spearman's rank correlation. */
	double Spearman(const std::vector<double>& A, const std::vector<double>& B)
	{
		auto Ranks = [](const std::vector<double>& X)
		{
			std::vector<size_t> I(X.size());
			for (size_t K = 0; K < I.size(); ++K) { I[K] = K; }
			std::sort(I.begin(), I.end(), [&](size_t P, size_t Q) { return X[P] < X[Q]; });
			std::vector<double> R(X.size());
			for (size_t K = 0; K < I.size(); ++K) { R[I[K]] = double(K); }
			return R;
		};
		const std::vector<double> Ra = Ranks(A), Rb = Ranks(B);
		const double N = double(A.size()), M = (N - 1.0) * 0.5;
		double Sab = 0.0, Saa = 0.0, Sbb = 0.0;
		for (size_t K = 0; K < A.size(); ++K) { Sab += (Ra[K] - M) * (Rb[K] - M); Saa += (Ra[K] - M) * (Ra[K] - M); Sbb += (Rb[K] - M) * (Rb[K] - M); }
		return Sab / std::sqrt(std::max(Saa * Sbb, 1e-300));
	}

	/** A synthetic star field over a faint glow (as test_lens's stills). */
	void StarFieldOf(FImage& I)
	{
		for (int K = 0; K < 1500; ++K)
		{
			const double X = I.W * H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K)), Y = I.H * H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K + 1));
			const double B = 0.05 + 2.0 * std::pow(double(H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K + 2))), 6.0);
			I.Add(int(X), int(Y), float(B), float(B), float(B));
		}
		for (size_t P = 0; P < I.Rgb.size(); P += 3) { I.Rgb[P] += 0.01f; I.Rgb[P + 1] += 0.01f; I.Rgb[P + 2] += 0.013f; }
	}

	/** One black hole's disc in its look at the engine's settings (lens scale 4), as the lens pass
	 *  draws it: from Dx lens R_s at cos i to its axis. */
	FImage StyledView(const H::FUniBlackHole& Bh, const H::FUniBhStyle& S, double Dx, double CosI, int W, int Hh, FLensStats* St = nullptr)
	{
		const float Ks = 4.0f;
		const float Peak = H::Uni_BhDiscPeakK(Bh);
		FCamera C; C.Width = W; C.Height = Hh; C.FovX = 1.2;
		const double Sn = std::sqrt(std::max(1.0 - CosI * CosI, 0.0));
		C.LookAlong(FV3{ -Sn, 0.0, -CosI }, FV3{ 0.0, 0.0, 1.0 });
		FImage In(W, Hh); StarFieldOf(In);
		FLensView L;
		L.CamRs = H::float3(float(Dx * Sn), 0.0f, float(Dx * CosI));
		L.Disc.Axis = H::float3(0.0f, 0.0f, 1.0f); L.Disc.PeakK = Peak; L.Disc.bDisc = 1.0f;
		L.Disc.OuterX = std::max(H::Uni_BhDiscOuterX(Peak) * S.ExtentMul / Ks, H::UNI_BH_ISCO_X + 1.0f);
		ApplyDiscLook(L.Disc, StyledLook(FDiscLook{}, S), 2.0, 4321u);
		L.DiscScale = DiscArtScale(Peak, 1.0 / C.FocalPx(), 1.0) * double(S.BrightnessMul);
		L.DiscKnee = H::UNI_BH_DISC_KNEE;
		FImage Out(W, Hh);
		ApplyLens(In, C, L, Out, St);
		return Out;
	}
}

UNI_TEST(bh, style)
{
	const std::vector<FRec> Recs = Records(10000);
	const FArchetype Galaxy = UniScenes::MakeGalaxy();
	// The multipliers' bounds (Uni_BhStyleOf): spiral, thickness, opacity, clouds, bands, heat, erosion, period, brightness, extent.
	const double Lo[10] = { 0.5, 0.707107, 0.3 * 0.757858, 0.51, 0.5, 0.574349, 0.6, 0.5 * 0.840896, 0.6 * 0.812252, 0.55 * 0.75 * 0.784584 };
	const double Hi[10] = { 2.0, 7.071068, 1.319508, 1.15, 2.0, 1.741101, 1.2, 2.5 * 1.189207, 1.6 * 1.231144, 1.274561 };
	size_t ZeroBad = 0, RangeBad = 0, HalfBad = 0, ForcedBad = 0, DetBad = 0;
	size_t Tori = 0, ToriFast = 0, Accreting = 0, ThinSlow = 0;
	size_t PalCount[8] = {}, HotFast = 0, HotSlow = 0, NFast = 0, NSlow = 0, WarmEarly = 0, WarmLate = 0, NEarly = 0, NLate = 0;
	std::vector<double> Wind, Spir;
	std::vector<std::pair<double, size_t>> ByEdd;
	for (const FRec& R : Recs)
	{
		const FObjectGPU O = GalaxyObject(R);
		const H::FUniObject Ho = ToHlsl(O);
		const H::FUniBlackHole Bh = H::Uni_GalaxyBlackHole(Ho, H::UNI_BH_ACTIVE_SHARE);
		FStyleConfig C0; C0.Variety = 0.0f;
		FStyleConfig C1;
		FStyleConfig Ch; Ch.Variety = 0.5f;
		const H::FUniBhStyle S0 = GalaxyBhStyle(O, &Galaxy, C0), S1 = GalaxyBhStyle(O, &Galaxy, C1), Sh = GalaxyBhStyle(O, &Galaxy, Ch), S1b = GalaxyBhStyle(O, &Galaxy, C1);
		for (int K = 0; K < 10; ++K)
		{
			if (Mul(S0, K) != 1.0f) { ++ZeroBad; }
			if (!(double(Mul(S1, K)) >= Lo[K] * (1.0 - 1e-4) && double(Mul(S1, K)) <= Hi[K] * (1.0 + 1e-4))) { ++RangeBad; }
			if (std::fabs(double(Mul(Sh, K)) / std::sqrt(double(Mul(S1, K))) - 1.0) > 1e-5) { ++HalfBad; }
			if (Mul(S1, K) != Mul(S1b, K)) { ++DetBad; }
		}
		if (S0.Palette != 0u || S0.PaletteMix != 0.0f) { ++ZeroBad; }
		if (S1.Palette != S1b.Palette || S1.PaletteMix != S1b.PaletteMix || S1.Palette != Sh.Palette) { ++DetBad; }
		if (S1.Palette != 0u && !(S1.PaletteMix >= H::UNI_BH_PAL_MIX_LO && S1.PaletteMix <= H::UNI_BH_PAL_MIX_HI)) { ++RangeBad; }
		for (int P = 0; P < 8; ++P)
		{
			FStyleConfig Cf; Cf.PaletteMode = float(P);
			const H::FUniBhStyle Sf = GalaxyBhStyle(O, &Galaxy, Cf);
			if (Sf.Palette != u32(P) || Sf.PaletteMix != (P == 0 ? 0.0f : H::UNI_BH_PAL_MIX_FORCED)) { ++ForcedBad; }
		}
		++PalCount[std::min<u32>(S1.Palette, 7u)];
		const bool bHot = S1.Palette == 2u || S1.Palette == 5u || S1.Palette == 7u, bWarm = S1.Palette == 1u || S1.Palette == 3u || S1.Palette == 6u;
		const bool bAccreting = Bh.Log10Eddington >= H::UNI_BH_EDD_ACTIVE_LO;
		if (bAccreting)
		{
			++Accreting;
			if (S1.ThicknessMul > 2.5f) { ++Tori; }
			ByEdd.push_back({ double(Bh.Log10Eddington), size_t(&R - &Recs[0]) });
			if (Bh.Log10Eddington > -2.5f) { ++NFast; HotFast += bHot ? 1u : 0u; }
		}
		if (Bh.Log10Eddington < -5.0f) { ++NSlow; HotSlow += bHot ? 1u : 0u; }
		if (S1.ThicknessMul > 2.5f && Bh.Log10Eddington > H::UNI_BH_THICK_LOG_EDD_HI) { ++ToriFast; }
		if (S1.ThicknessMul < 1.5f && Bh.Log10Eddington < H::UNI_BH_THICK_LOG_EDD_LO) { ++ThinSlow; }
		if (R.bEarly) { ++NEarly; WarmEarly += bWarm ? 1u : 0u; } else { ++NLate; WarmLate += bWarm ? 1u : 0u; }
		const float W = HostWinding01(Galaxy, R.Seed, R.bEarly);
		if (!R.bEarly)
		{
			if (!(W >= 0.0f && W <= 1.0f)) { ++RangeBad; }
			Wind.push_back(double(W)); Spir.push_back(double(S1.SpiralMul));
		}
		else if (W != -1.0f) { ++RangeBad; }
	}
	const double ToriShare = double(Tori) / std::max<size_t>(Accreting, 1);
	const double HotF = double(HotFast) / std::max<size_t>(NFast, 1), HotS = double(HotSlow) / std::max<size_t>(NSlow, 1);
	const double WarmE = double(WarmEarly) / std::max<size_t>(NEarly, 1), WarmL = double(WarmLate) / std::max<size_t>(NLate, 1);
	const double Rho = Spearman(Wind, Spir);
	size_t Unused = 0;
	for (size_t P : PalCount) { Unused += P == 0 ? 1u : 0u; }
	const bool bOk = ZeroBad == 0 && RangeBad == 0 && HalfBad == 0 && ForcedBad == 0 && DetBad == 0 && ToriFast == 0 && ThinSlow == 0
		&& ToriShare > 0.15 && ToriShare < 0.4 && HotF > HotS + 0.1 && WarmE > WarmL + 0.03 && Rho > 0.6 && Unused == 0;
	Uni::Check(1, "T1.bh.style", "D-280 each black hole's look (10^4 galaxy records): variety 0 every multiplier exactly 1 and the blackbody; variety 1 every multiplier in its range, a rolled palette's mix in [0.65, 0.9]; tori (thickness over 2.5x) 15-40% of the accreting and never above 10^-3.5 of Eddington, no thin disc below 10^-4.3; every palette used; blue / white palettes 10 points more often on fast accretors (> 10^-2.5) than on quiet ones; warm palettes 3 points more often on early types; a disc galaxy's spiral following its arms' winding (Spearman over 0.6); variety 1/2 the square root of 1's (1e-5); forced palettes everywhere; deterministic")
		.Metric("tori_share", ToriShare).Metric("winding_spearman", Rho).Metric("hot_fast", HotF).Metric("hot_quiet", HotS)
		.Pass(bOk, Uni::Fmt("variety 0: %zu off; ranges %zu off; variety 1/2 %zu off; forced %zu off; repeat %zu off; tori %.1f%% of %zu accreting (%zu above 10^-3.5, %zu thin below 10^-4.3); palettes %zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu (blackbody .. ice and fire); blue/white on fast accretors %.1f%%, on quiet %.1f%%; warm on early types %.1f%%, late %.1f%%; spiral vs arms' winding Spearman %.3f",
			ZeroBad, RangeBad, HalfBad, ForcedBad, DetBad, 100.0 * ToriShare, Accreting, ToriFast, ThinSlow,
			PalCount[0], PalCount[1], PalCount[2], PalCount[3], PalCount[4], PalCount[5], PalCount[6], PalCount[7], 100.0 * HotF, 100.0 * HotS, 100.0 * WarmE, 100.0 * WarmL, Rho));
}

UNI_TEST(lens, palette)
{
	std::string Txt;
	bool bOk = true;
	{
		// The blackbody bit for bit, and a palette the colour only.
		size_t Exact = 0, LumBad = 0;
		double WorstLum = 0.0;
		for (int I = 0; I < 400; ++I)
		{
			const float T = 1500.0f * std::pow(2.0e4f / 1.5f, float(I) / 399.0f);
			for (float Peak : { 6000.0f, 30000.0f, 3.0e5f })
			{
				const H::float3 A = H::Uni_BhDiscLight(T), B = H::Uni_BhDiscLightP(T, Peak, 0u, 0.8f), Cz = H::Uni_BhDiscLightP(T, Peak, 3u, 0.0f);
				if (!(A.x == B.x && A.y == B.y && A.z == B.z && A.x == Cz.x && A.y == Cz.y && A.z == Cz.z)) { ++Exact; }
				for (H::uint P = 1u; P < H::UNI_BH_PALETTES; ++P)
				{
					const H::float3 Pc = H::Uni_BhDiscLightP(T, Peak, P, 0.9f);
					const double La = double(H::Uni_Luminance(A)), Lp = double(H::Uni_Luminance(Pc));
					const double E = La > 1e-30 ? std::fabs(Lp / La - 1.0) : std::fabs(Lp);
					WorstLum = std::max(WorstLum, E);
					if (E > 1e-4) { ++LumBad; }
				}
			}
		}
		for (float Lm : { 5.0f, 7.0f, 9.0f })
		{
			for (float Le : { -4.0f, -2.0f, 0.0f })
			{
				const H::FUniBlackHole Bh = H::Uni_BlackHoleOf(Lm, Le);
				const H::float3 A = H::Uni_NucleusColour(Bh), B = H::Uni_NucleusColourP(Bh, 0u, 0.9f);
				if (!(A.x == B.x && A.y == B.y && A.z == B.z)) { ++Exact; }
			}
		}
		bOk = bOk && Exact == 0 && LumBad == 0;
		Txt += Uni::Fmt("palette 0 / mix 0 the blackbody: %zu differ; a palette's luminance worst %.2g of the blackbody's; ", Exact, WorstLum);
	}
	{
		// The hand-off's colour: the thin disc in each palette averaged over directions (as
		// T1.lens.disc_light) against the nucleus's in that palette.
		double Worst = 0.0;
		for (H::uint P = 1u; P < H::UNI_BH_PALETTES; ++P)
		{
			for (float Peak : { 8000.0f, 50000.0f, 3.0e5f })
			{
				double Rgb[3] = { 0.0, 0.0, 0.0 };
				const int Nm = 5;
				for (int Im = 0; Im < Nm; ++Im)
				{
					const double Mu = double(Im) / double(Nm - 1);
					double F[3];
					DiscFarLight(Peak, Mu, 1.0e5, 96, 48, F, P, 0.8f);
					const double W = (Im == 0 || Im == Nm - 1) ? 1.0 : (Im % 2 ? 4.0 : 2.0);
					for (int K = 0; K < 3; ++K) { Rgb[K] += F[K] * W; }
				}
				// Uni_NucleusColourP's taps at this peak.
				H::float3 Sum(0.0f, 0.0f, 0.0f);
				const float Outer = H::Uni_BhDiscOuterX(Peak);
				for (H::uint I = 0; I < H::UNI_BH_COLOUR_TAPS; ++I)
				{
					const float X = H::UNI_BH_ISCO_X * std::exp(std::log(Outer / H::UNI_BH_ISCO_X) * ((float(I) + 0.5f) / float(H::UNI_BH_COLOUR_TAPS)));
					Sum = Sum + H::Uni_BhDiscLightP(Peak * H::Uni_BhDiscProfile(X), Peak, P, 0.8f) * (H::Uni_BhDiscOpacity(X, Outer) * X * X);
				}
				const double Ts = double(Sum.x) + Sum.y + Sum.z, Tr = Rgb[0] + Rgb[1] + Rgb[2];
				for (int K = 0; K < 3; ++K) { Worst = std::max(Worst, std::fabs(Rgb[K] / Tr - (K == 0 ? Sum.x : (K == 1 ? Sum.y : Sum.z)) / Ts)); }
			}
		}
		bOk = bOk && Worst < 0.03;
		Txt += Uni::Fmt("the disc's colour over directions against its nucleus's, every palette: worst %.4f chromaticity; ", Worst);
	}
	{
		// Cost at the look's extremes: the thickest, most transparent torus and the thinnest, densest
		// disc (the multipliers' bounds), samples a ray over three views.
		FDiscLook Base;
		H::FUniBhStyle Torus{}; Torus.SpiralMul = 2.0f; Torus.ThicknessMul = 7.07f; Torus.OpacityMul = 0.23f; Torus.CloudsMul = 0.51f; Torus.BandsMul = 2.0f; Torus.HeatMul = 1.74f; Torus.ErosionMul = 1.2f; Torus.PeriodMul = 1.0f; Torus.BrightnessMul = 1.0f; Torus.ExtentMul = 0.33f;
		H::FUniBhStyle Thin = Torus; Thin.ThicknessMul = 0.707f; Thin.OpacityMul = 1.32f; Thin.CloudsMul = 1.15f; Thin.ExtentMul = 1.27f;
		std::vector<float> Used;
		size_t Past = 0;
		for (const H::FUniBhStyle* S : { &Torus, &Thin })
		{
			for (double Ci : { 0.1, 0.4, 0.9 })
			{
				const H::FUniBlackHole Bh = H::Uni_BlackHoleOf(7.0f, -2.0f);
				FCamera C; C.Width = 120; C.Height = 80; C.FovX = 1.2;
				const double Sn = std::sqrt(1.0 - Ci * Ci);
				C.LookAlong(FV3{ -Sn, 0.0, -Ci }, FV3{ 0.0, 0.0, 1.0 });
				H::FUniLensDisc D{}; D.Axis = H::float3(0.0f, 0.0f, 1.0f); D.PeakK = H::Uni_BhDiscPeakK(Bh); D.bDisc = 1.0f;
				D.OuterX = std::max(H::Uni_BhDiscOuterX(D.PeakK) * S->ExtentMul / 4.0f, H::UNI_BH_ISCO_X + 1.0f);
				ApplyDiscLook(D, StyledLook(Base, *S), 2.0, 9u);
				const double Dx = std::max(2.0 * double(D.OuterX), 25.0);
				for (int Y = 0; Y < C.Height; ++Y) for (int X = 0; X < C.Width; ++X)
				{
					double Cf; const H::float3 Dr = PixelRay(C, X + 0.5, Y + 0.5, Cf);
					const H::FUniLensResult R = H::Uni_LensTrace(H::float3(float(Dx * Sn), 0.0f, float(Dx * Ci)), Dr, D, H::UNI_LENS_MAX_STEPS);
					if (R.Pad0 > 0.0f) { Used.push_back(R.Pad0); if (R.Pad0 > float(H::UNI_LENS_VOL_SAMPLES)) { ++Past; } }
				}
			}
		}
		std::sort(Used.begin(), Used.end());
		double Mean = 0.0; for (float U : Used) { Mean += U; }
		Mean /= std::max<size_t>(Used.size(), 1);
		const double P99 = Used.empty() ? 0.0 : Used[Used.size() * 99 / 100];
		const double PastShare = double(Past) / std::max<size_t>(Used.size(), 1);
		bOk = bOk && Mean < 80.0 && PastShare < 0.02;
		Txt += Uni::Fmt("the look's extremes: samples a ray mean %.1f, p99 %.0f, past the budget %.2f%%; ", Mean, P99, 100.0 * PastShare);
	}
	{
		// Still: eight black holes as galaxies roll them (engine defaults: variety 1, lens scale 4):
		// the first accreting one of each palette 1..7 with a thin disc, and a torus.
		const FArchetype Galaxy = UniScenes::MakeGalaxy();
		std::vector<std::pair<FRec, H::FUniBhStyle>> Pick(8);
		std::vector<bool> Have(8, false);
		for (const FRec& R : Records(4000))
		{
			const FObjectGPU O = GalaxyObject(R);
			const H::FUniBlackHole Bh = H::Uni_GalaxyBlackHole(ToHlsl(O), H::UNI_BH_ACTIVE_SHARE);
			if (Bh.Log10Eddington < H::UNI_BH_EDD_ACTIVE_LO) { continue; }
			const H::FUniBhStyle S = GalaxyBhStyle(O, &Galaxy, FStyleConfig{});
			const bool bTorus = S.ThicknessMul > 3.5f;
			const size_t Slot = bTorus ? 0u : size_t(S.Palette);
			if (Slot >= 8 || Have[Slot] || (!bTorus && (S.Palette == 0u || S.ThicknessMul > 1.6f))) { continue; }
			Have[Slot] = true;
			Pick[Slot] = { R, S };
		}
		const int W = 400, Hh = 250;
		FImage Sheet(4 * W, 2 * Hh);
		std::string Lines;
		static const char* Names[8] = { "torus", "gold", "quasar blue", "ember", "violet", "teal", "rose", "ice and fire" };
		for (int I = 0; I < 8; ++I)
		{
			if (!Have[size_t(I)]) { continue; }
			const FObjectGPU O = GalaxyObject(Pick[size_t(I)].first);
			const H::FUniBlackHole Bh = H::Uni_GalaxyBlackHole(ToHlsl(O), H::UNI_BH_ACTIVE_SHARE);
			const H::FUniBhStyle& S = Pick[size_t(I)].second;
			const float Outer = std::max(H::Uni_BhDiscOuterX(H::Uni_BhDiscPeakK(Bh)) * S.ExtentMul / 4.0f, H::UNI_BH_ISCO_X + 1.0f);
			const double Dx = std::max(1.9 * double(Outer), 22.0);
			const FImage Img = StyledView(Bh, S, Dx, I % 2 ? 0.35 : 0.14, W, Hh);
			for (int Y = 0; Y < Hh; ++Y) for (int X = 0; X < W; ++X) { const float* P = &Img.Rgb[(size_t(Y) * W + X) * 3]; Sheet.Add((I % 4) * W + X, (I / 4) * Hh + Y, P[0], P[1], P[2]); }
			Lines += Uni::Fmt(",\"%d %s: palette %u at %.0f%%, 1e%.1f M_sun at 1e%.1f Edd, spiral x%.2f, thickness x%.2f, opacity x%.2f, extent x%.2f (%.0f lens R_s), brightness x%.2f\"",
				I, Names[I], S.Palette, 100.0 * double(S.PaletteMix), double(Bh.Log10Mass), double(Bh.Log10Eddington), double(S.SpiralMul), double(S.ThicknessMul), double(S.OpacityMul), double(S.ExtentMul), double(Outer), double(S.BrightnessMul));
		}
		const std::string Base = RenderDir() + "/still_bh_styles";
		Sheet.WritePpm(Base + ".ppm");
		if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb"))
		{
			std::fprintf(F, "{\"lines\":[\"D-280: eight black holes as their galaxies roll them (variety 1, lens scale 4, the engine's uu.Bh.Disc* centre)\"%s]}\n", Lines.c_str());
			std::fclose(F);
		}
	}
	Uni::Check(1, "T1.lens.palette", "D-280 the discs' palettes: palette 0 (or mix 0) the blackbody bit for bit (the disc's light, the nucleus's colour); a palette changes the colour only (luminance within 1e-4 of the blackbody's); the disc's colour averaged over directions within 0.03 chromaticity of its nucleus sprite's in the same palette (the hand-off); at the look's extremes (the thickest torus, the densest thin disc) under 80 volume samples a ray on average, under 2% past the budget; still render/still_bh_styles")
		.Pass(bOk, Txt);
}
