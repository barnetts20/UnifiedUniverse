// test_bhstellar.cpp
// D-281 (owner 2026-11-17 22:35: "We should be able to scatter some stellar mass black holes around
// as well"): stellar black holes as X-ray binaries, a star's companion (UniCompact.ush's
// Uni_StarHasBlackHole / Uni_StellarBlackHole; UniSystem.h's StellarCompanionOf, the system
// manager's record; UniCompact.h's LensTargetOf at uu.Bh.StellarScale).
//   stellar   over 2e5 star seeds: the share with one the fraction (3 sigma), a larger fraction only
//             adding; masses in 5..25 M_sun, median 7-11; the share in outburst; the rates in range;
//             the orbit (radius 4.8..14.4 x the star's, Kepler's period, periodic, its normal
//             perpendicular to its plane); the planets unchanged; its light at most the cap x the
//             star's, faded to none at the activation radius and whole inside 0.6 of it; the system
//             manager's record (one per system with one, hidden without light, the cells, records
//             and names aligned) the lens pass's point exactly; the lens target at uu.Bh.StellarScale
//             (its disc and ring x the scale's, its disc resolved from a few AU); still
//             render/still_bh_stellar.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../src/UniLensRef.h"
#include "../scenes/UniScenes.h"
#include "UniCompact.h"
#include "UniSystem.h"
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
	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	FObjectGPU StarRecord(u32 Seed, float AbsMag, double RadiusCm)
	{
		return FromHlsl(H::Uni_MakeObject(H::uint3(0u, 0u, 0u), UniScenes::kStar, 0u, Seed, float(std::log2(RadiusCm)), AbsMag, 0.0f, H::float3(1.0f, 0.96f, 0.9f)));
	}

	/** The first seed from Start whose star has a black hole whose outburst state is bActive. */
	u32 SeedWith(u32 Start, bool bActive, const FStellarBhConfig& C)
	{
		for (u32 S = Start;; ++S)
		{
			if (!H::Uni_StarHasBlackHole(S, C.Fraction)) { continue; }
			const H::FUniBlackHole B = H::Uni_StellarBlackHole(S, C.Active);
			if ((B.Log10Eddington > -3.0f) == bActive) { return S; }
		}
	}

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
}

UNI_TEST(bh, stellar)
{
	std::string Txt;
	bool bOk = true;
	const FStellarBhConfig Cfg;
	{
		// Presence, mass, accretion over 2e5 seeds.
		const int N = 200000;
		size_t Have = 0, HaveBoth = 0, HaveWide = 0, Active = 0, MassBad = 0, RateBad = 0;
		std::vector<double> Masses;
		FStellarBhConfig Wide = Cfg; Wide.Fraction = 0.1f;
		for (int I = 0; I < N; ++I)
		{
			const u32 S = H::Uni_SeedMix(0x57E1u, u32(I));
			const bool A = H::Uni_StarHasBlackHole(S, Cfg.Fraction), B = H::Uni_StarHasBlackHole(S, Wide.Fraction);
			Have += A ? 1u : 0u; HaveWide += B ? 1u : 0u; HaveBoth += (A && B) ? 1u : 0u;
			if (!B) { continue; }
			const H::FUniBlackHole Bh = H::Uni_StellarBlackHole(S, Cfg.Active);
			const double M = std::pow(10.0, double(Bh.Log10Mass));
			Masses.push_back(M);
			if (!(M >= 5.0 * (1.0 - 1e-5) && M <= 25.0 * (1.0 + 1e-5))) { ++MassBad; }
			const bool bAct = Bh.Log10Eddington >= H::UNI_BH_XRB_ACTIVE_LO;
			Active += bAct ? 1u : 0u;
			if (bAct ? !(Bh.Log10Eddington <= H::UNI_BH_XRB_ACTIVE_HI) : !(Bh.Log10Eddington >= H::UNI_BH_XRB_QUIET_LO && Bh.Log10Eddington <= H::UNI_BH_XRB_QUIET_HI)) { ++RateBad; }
		}
		std::sort(Masses.begin(), Masses.end());
		const double Med = Masses.empty() ? 0.0 : Masses[Masses.size() / 2];
		const double Want = double(N) * double(Cfg.Fraction), Sig = std::sqrt(Want);
		const double ActShare = double(Active) / std::max<size_t>(Masses.size(), 1), ActSig = std::sqrt(0.3 * 0.7 / std::max<size_t>(Masses.size(), 1));
		const bool b = std::fabs(double(Have) - Want) < 3.0 * Sig && HaveBoth == Have && std::fabs(double(HaveWide) / N - 0.1) < 0.003
			&& MassBad == 0 && RateBad == 0 && Med > 7.0 && Med < 11.0 && std::fabs(ActShare - double(Cfg.Active)) < 4.0 * ActSig;
		bOk = bOk && b;
		Txt += Uni::Fmt("%zu of %d stars with one (want %.0f +- %.0f), at 0.1: %.4f, the 1e-3 set inside it %s; masses median %.1f M_sun, %zu out of 5..25; in outburst %.3f; rates %zu out of range; ",
			Have, N, Want, Sig, double(HaveWide) / N, HaveBoth == Have ? "yes" : "no", Med, MassBad, ActShare, RateBad);
	}
	{
		// The orbit and the planets.
		size_t OrbitBad = 0, PlanetsBad = 0, Checked = 0;
		FStellarBhConfig All = Cfg; All.Fraction = 1.0f;
		FStellarBhConfig None = Cfg; None.Fraction = 0.0f;
		for (u32 S = 1; S < 400; ++S)
		{
			const double Rstar = 6.957e10 * std::pow(10.0, 2.0 * H::Uni_DrawUnit(S, 0x77u, 0u) - 0.5);
			const FObjectGPU Star = StarRecord(S, 4.83f + 6.0f * (H::Uni_DrawUnit(S, 0x77u, 1u) - 0.5f), Rstar);
			const FSystem A = BuildSystem(Star, All), B = BuildSystem(Star, None);
			if (!A.Bh.bValid || B.Bh.bValid) { ++OrbitBad; continue; }
			++Checked;
			if (A.Planets.size() != B.Planets.size()) { ++PlanetsBad; }
			for (size_t K = 0; K < std::min(A.Planets.size(), B.Planets.size()); ++K)
			{
				if (A.Planets[K].Seed != B.Planets[K].Seed || A.Planets[K].SemiMajorCm != B.Planets[K].SemiMajorCm || A.Planets[K].RadiusCm != B.Planets[K].RadiusCm) { ++PlanetsBad; }
			}
			const FStellarBh& Bh = A.Bh;
			const double Ratio = Bh.SemiMajorCm / std::exp2(double(H::Uni_ObjectRadiusLog2(ToHlsl(Star))));
			if (!(Ratio >= 4.8 * (1.0 - 1e-5) && Ratio <= 14.4 * (1.0 + 1e-5))) { ++OrbitBad; }
			const double GM = SysConst::GMSun * (std::pow(10.0, double(Bh.Core.Log10Mass)) + A.MassSun);
			if (std::fabs(Bh.PeriodS / (SysConst::TwoPi * std::sqrt(std::pow(Bh.SemiMajorCm, 3.0) / GM)) - 1.0) > 1e-9) { ++OrbitBad; }
			for (double T : { 0.0, 1234.5, 0.37 * Bh.PeriodS, 1.0e9 })
			{
				const FVec3d P = StellarBhOffset(Bh, T), Q = StellarBhOffset(Bh, T + Bh.PeriodS);
				const double Dn = P.X * Bh.Normal[0] + P.Y * Bh.Normal[1] + P.Z * Bh.Normal[2];
				if (std::fabs(Length(P) / Bh.SemiMajorCm - 1.0) > 1e-9 || std::fabs(Dn) > 1e-6 * Bh.SemiMajorCm || Length(FVec3d{ P.X - Q.X, P.Y - Q.Y, P.Z - Q.Z }) > 1e-6 * Bh.SemiMajorCm) { ++OrbitBad; }
			}
		}
		bOk = bOk && OrbitBad == 0 && PlanetsBad == 0 && Checked > 300;
		Txt += Uni::Fmt("%zu systems: orbits %zu wrong, planets %zu changed; ", Checked, OrbitBad, PlanetsBad);
	}
	{
		// Its light: the cap, the fade.
		size_t CapBad = 0, FadeBad = 0;
		FStellarBhConfig All = Cfg; All.Fraction = 1.0f; All.Active = 1.0f;
		for (u32 S = 1; S < 300; ++S)
		{
			const FStellarBh B = StellarCompanionOf(S, 6.957e10, 1.0, All);
			const float StarM = 4.83f;
			const double Act = 100.0 * 1.495978707e13;
			const FStellarBhLight Far = StellarBhLightOf(B, StarM, Act, Act * 1.01, All), Edge = StellarBhLightOf(B, StarM, Act, Act, All),
				In = StellarBhLightOf(B, StarM, Act, 0.59 * Act, All), Mid = StellarBhLightOf(B, StarM, Act, 0.8 * Act, All);
			if (Far.AbsMagIso < H::UNI_BH_NONE_MAG || Edge.AbsMagIso < H::UNI_BH_NONE_MAG || In.Fade != 1.0f || !(Mid.Fade > 0.4f && Mid.Fade < 0.6f)) { ++FadeBad; }
			if (In.AbsMagIso < StarM + 2.5f * std::log10(1.0f / All.Light.Cap) - 1e-3f) { ++CapBad; }
		}
		bOk = bOk && CapBad == 0 && FadeBad == 0;
		Txt += Uni::Fmt("light over the cap %zu, fade wrong %zu; ", CapBad, FadeBad);
	}
	{
		// The system manager's record against the lens pass's point, and its bookkeeping.
		FStellarBhConfig C = Cfg; C.Fraction = 1.0f;
		size_t Bad = 0, Records = 0, Bhs = 0, Hidden = 0;
		double WorstMag = 0.0;
		for (u32 S = 11; S < 41; ++S)
		{
			FSystemManager Mgr;
			Mgr.BhConfig = C;
			const FObjectGPU Star = StarRecord(S, 4.83f, 6.957e10);
			const FCellKey StarCell = CellOf(FCoord::Origin(), 60);
			const FCoord StarC = CellOrigin(StarCell);
			FCoord Cam = StarC;
			FVec3f Fr{};
			const double Cd = (0.2 + 0.4 * H::Uni_DrawUnit(S, 0x78u, 0u)) * 100.0 * 1.495978707e13;
			AddDisplacement(Cam, Fr, FVec3d{ Cd * 0.6, -Cd * 0.48, Cd * 0.64 });
			Mgr.Report(StarCell, Star, Cam, 0);
			std::vector<FCellGPU> Cells; std::vector<FObjectGPU> Recs; std::vector<FPlanetCellRef> Refs;
			const double T = 5.0e4 * S;
			Mgr.Build(Cam, T, 1000u, 64u, Cells, Recs, nullptr, &Refs);
			if (Cells.size() != Recs.size() || Refs.size() != Recs.size()) { ++Bad; }
			Records += Recs.size();
			for (size_t I = 0; I < Recs.size(); ++I)
			{
				if ((Recs[I].A[3] & 0xFFFFu) != H::UNI_ARCH_DYNAMIC_BH) { continue; }
				++Bhs;
				if (Refs[I].bStarIdKnown || Refs[I].Planet != -1) { ++Bad; }
				const FActiveSystem& A = Mgr.ActiveSystems().begin()->second;
				FCoord Bc = A.Star; FVec3f Bf{};
				AddDisplacement(Bc, Bf, StellarBhOffset(A.Sys.Bh, T));
				const FVec3d Rel = ToDouble(Sub(Bc, Cam));
				const double RelCm[3] = { Rel.X, Rel.Y, Rel.Z };
				const FStellarBhLight Lt = StellarBhLightOf(A.Sys.Bh, A.Sys.StarAbsMag, A.Sys.ActivationCm, Length(Rel), C);
				FLensConfig Lc; Lc.LensScale = 4.0f;
				const FLensTarget Tg = LensTargetOf(A.Sys.Bh.Core, Lt.AbsMagIso, RelCm, A.Sys.Bh.Normal, A.Sys.Bh.Seed, 1.0e-3, Lc, &Lt.Style, double(C.SizeScale));
				const H::FUniObject Ho = ToHlsl(Recs[I]);
				const bool bHid = ((Recs[I].A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) != 0u;
				Hidden += bHid ? 1u : 0u;
				if (bHid != !(Tg.PointAppMag < H::UNI_BH_NONE_MAG)) { ++Bad; continue; }
				if (bHid) { continue; }
				const double Rec = double(H::Uni_ApparentMag(H::Uni_ObjectAbsMag(Ho), float(Length(Rel))));
				WorstMag = std::max(WorstMag, std::fabs(Rec - double(Tg.PointAppMag)));
				const H::float3 Pc = H::Uni_UnpackColour(Recs[I].B[3]);
				if (std::fabs(Pc.x - Tg.PointColour[0]) > 1e-6f || std::fabs(Pc.y - Tg.PointColour[1]) > 1e-6f || std::fabs(Pc.z - Tg.PointColour[2]) > 1e-6f) { ++Bad; }
				if (std::fabs(double(Tg.PointPhysAngRad) / (std::exp2(double(H::Uni_ObjectRadiusLog2(Ho))) / Length(Rel)) - 1.0) > 1e-5) { ++Bad; }
			}
		}
		bOk = bOk && Bad == 0 && Bhs == 30 && WorstMag < 1e-2;
		Txt += Uni::Fmt("the manager: %zu records, %zu black holes (%zu hidden), %zu problems, its point against the lens pass's within %.2g mag; ", Records, Bhs, Hidden, Bad, WorstMag);
	}
	{
		// The lens target at the art scale: a 10 M_sun hole seen from 1 AU and 20 AU.
		const H::FUniBlackHole Bh = H::Uni_BlackHoleOf(1.0f, -1.0f);
		const float Axis[3] = { 0.0f, 0.0f, 1.0f };
		FLensConfig Lc; Lc.LensScale = 4.0f;
		const double Au = 1.495978707e13, Th = 0.0016 / 2.0;   // ~0.8 mrad a pixel (90 degrees over 1920)
		const double R1[3] = { Au, 0.0, 0.0 }, R20[3] = { 20.0 * Au, 0.0, 0.0 };
		const FLensTarget P1 = LensTargetOf(Bh, -2.0f, R1, Axis, 1u, Th, Lc, nullptr, 1.0), S1 = LensTargetOf(Bh, -2.0f, R1, Axis, 1u, Th, Lc, nullptr, 30.0),
			S20 = LensTargetOf(Bh, -2.0f, R20, Axis, 1u, Th, Lc, nullptr, 30.0);
		const bool b = std::fabs(double(S1.DiscPx) / double(P1.DiscPx) - 30.0) < 0.05 && std::fabs(double(S1.RingPx) / double(P1.RingPx) - std::sqrt(30.0)) < 1e-3
			&& S1.DiscShare == 1.0f && S1.bActive && S20.bActive;
		bOk = bOk && b;
		Txt += Uni::Fmt("at x30 from 1 AU: disc %.1f px (physical %.2f), ring %.1f px (physical %.2f); from 20 AU ring %.2f px, disc %.1f px; ",
			double(S1.DiscPx), double(P1.DiscPx), double(S1.RingPx), double(P1.RingPx), double(S20.RingPx), double(S20.DiscPx));
	}
	{
		// Still: an X-ray binary in outburst and a quiescent one, from 3 disc radii, 20 degrees above
		// the disc (uu.GotoStellarBlackHole's view), at the engine's look (variety 1, lens scale 4).
		const int W = 480, Hh = 300;
		FImage Sheet(2 * W, Hh);
		std::string Lines;
		for (int K = 0; K < 2; ++K)
		{
			const u32 S = SeedWith(100u + 7u * u32(K), K == 0, Cfg);
			const H::FUniBlackHole Bh = H::Uni_StellarBlackHole(S, Cfg.Active);
			const u32 Bs = H::Uni_StellarBlackHoleSeed(S);
			const H::FUniBhStyle St = H::Uni_BhStyleOf(Bs, Bh, false, -1.0f, 1.0f, -1.0f);
			const float Peak = H::Uni_BhDiscPeakK(Bh);
			const float Outer = std::max(H::Uni_BhDiscOuterX(Peak) * St.ExtentMul / 4.0f, H::UNI_BH_ISCO_X + 1.0f);
			const double Dx = 3.0 * double(Outer), Ci = std::sin(20.0 * 3.14159265358979 / 180.0);
			FCamera C; C.Width = W; C.Height = Hh; C.FovX = 1.2;
			const double Sn = std::sqrt(1.0 - Ci * Ci);
			C.LookAlong(FV3{ -Sn, 0.0, -Ci }, FV3{ 0.0, 0.0, 1.0 });
			FImage In(W, Hh); StarFieldOf(In);
			FLensView L;
			L.CamRs = H::float3(float(Dx * Sn), 0.0f, float(Dx * Ci));
			L.Disc.Axis = H::float3(0.0f, 0.0f, 1.0f); L.Disc.PeakK = Peak; L.Disc.bDisc = 1.0f; L.Disc.OuterX = Outer;
			ApplyDiscLook(L.Disc, StyledLook(FDiscLook{}, St), 2.0, Bs);
			L.DiscScale = DiscArtScale(Peak, 1.0 / C.FocalPx(), 1.0) * double(St.BrightnessMul);
			L.DiscKnee = H::UNI_BH_DISC_KNEE;
			FImage Out(W, Hh);
			ApplyLens(In, C, L, Out, nullptr);
			for (int Y = 0; Y < Hh; ++Y) for (int X = 0; X < W; ++X) { const float* P = &Out.Rgb[(size_t(Y) * W + X) * 3]; Sheet.Add(K * W + X, Y, P[0], P[1], P[2]); }
			Lines += Uni::Fmt(",\"%s: %.1f M_sun at 1e%.1f of Eddington, peak %.0f K, palette %u, thickness x%.2f, from %.0f lens R_s\"", K == 0 ? "in outburst" : "quiescent",
				std::pow(10.0, double(Bh.Log10Mass)), double(Bh.Log10Eddington), double(Peak), St.Palette, double(St.ThicknessMul), Dx);
		}
		const std::string Base = RenderDir() + "/still_bh_stellar";
		Sheet.WritePpm(Base + ".ppm");
		if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb"))
		{
			std::fprintf(F, "{\"lines\":[\"D-281: stellar black holes (X-ray binaries), uu.GotoStellarBlackHole's view: 3 disc radii out, 20 degrees above the disc\"%s]}\n", Lines.c_str());
			std::fclose(F);
		}
	}
	Uni::Check(1, "T1.bh.stellar", "D-281 stellar black holes (a star's companion): the share of stars with one the fraction (3 sigma) and a larger fraction only adding; masses 5-25 M_sun, median 7-11; the outburst share (4 sigma); rates in range; orbits 4.8-14.4 star radii, Kepler's period, periodic, in their plane; planets unchanged; light at most the cap x the star's, none at the activation radius, whole inside 0.6 of it; the system manager's record one a system, aligned with its cells and names, its magnitude, colour and size the lens pass's point's (one half-float step, 0.01 mag); at uu.Bh.StellarScale 30 the disc 30x and the ring sqrt 30 x; still render/still_bh_stellar")
		.Pass(bOk, Txt);
}
