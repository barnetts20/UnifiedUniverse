// test_compact.cpp
// D-272 (Docs/COMPACT_OBJECTS.md, phase A): black holes (UniCompact.ush) and galaxies' nuclei.
//   mass_relation  a galaxy's black hole mass against Haring & Rix's 0.0014 x the bulge: the
//                  residual's mean and spread (0.3 dex), the bulge shares by type, the units
//                  (Eddington luminosity, Schwarzschild radius).
//   eddington      the Eddington ratio's mixture: the accreting share, the accreting branch's
//                  distribution against its analytic CDF, the quiet branch's range.
//   nucleus        the nucleus: never over the cap, the black hole's V light below it, the gain,
//                  none under 1e-6 of the host; the CPU lookup (UniCompact.h) is the core's.
//   nucleus_sprite the project pass's nucleus: the galaxies' own sprites bit for bit with it on,
//                  a point at the galaxy's position, its light the point-source light of its
//                  magnitude, never handed off; stills of the brightest nuclei.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniCompact.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	/** A synthetic galaxy record: seed, V magnitude, early type or not. */
	H::FUniObject GalaxyRecord(u32 Seed, float AbsMag, bool bEarly)
	{
		return H::Uni_MakeObject(H::uint3(0u, 0u, 0u), 2u, bEarly ? H::UNI_OBJ_FLAG_EARLY | H::UNI_OBJ_FLAG_EXTENDED : H::UNI_OBJ_FLAG_EXTENDED, Seed, 70.0f, AbsMag, 22.0f, H::float3(1.0f, 0.9f, 0.8f));
	}

	struct FSample { u32 Seed; float Mag; bool bEarly; };
	std::vector<FSample> Samples(int N)
	{
		std::vector<FSample> S;
		for (int I = 0; I < N; ++I)
		{
			const u32 Seed = H::Uni_SeedMix(0xB1ACu, u32(I));
			S.push_back({ Seed, -23.5f + 7.5f * H::Uni_DrawUnit(Seed, 0x51u, 0u), H::Uni_DrawUnit(Seed, 0x51u, 1u) < 0.25f });
		}
		return S;
	}

	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}
}

UNI_TEST(bh, mass_relation)
{
	// The galaxy relation over 10^4 records (M_V -23.5..-16, a quarter early types): the residual
	// log M_BH - log(0.0014 M_bulge) in units of 0.3 dex has mean within 0.03 and spread 0.95-1.03
	// (a normal held within 3 sigma: 0.987); bulge shares inside their ranges, the discs' log-uniform
	// (median within 3% of sqrt(0.08 x 0.45)); a Milky Way-like disc (M_V -20.9) has a median mass of
	// 5e6..3e7 (the relation's: the Milky Way's own 4.3e6 sits below it, a pseudo-bulge); the units:
	// L_Edd(1 M_sun) 1.26e38 erg/s, R_s(1 M_sun) 2.953e5 cm.
	const std::vector<FSample> S = Samples(10000);
	double Sum = 0.0, Sum2 = 0.0;
	size_t OutOfRange = 0;
	std::vector<double> DiscBt;
	for (const FSample& X : S)
	{
		const H::FUniObject O = GalaxyRecord(X.Seed, X.Mag, X.bEarly);
		const H::FUniBlackHole B = H::Uni_GalaxyBlackHole(O, 0.4f);
		const double Bulge = double(H::Uni_GalaxyLog10BulgeMass(X.Seed, H::Uni_ObjectAbsMag(O), X.bEarly));
		const double Z = (double(B.Log10Mass) - (std::log10(0.0014) + Bulge)) / 0.3;
		Sum += Z; Sum2 += Z * Z;
		const double Bt = H::Uni_GalaxyBulgeShare(X.Seed, X.bEarly);
		if (X.bEarly ? (Bt < 0.6 || Bt > 1.0) : (Bt < 0.08 || Bt > 0.45)) { ++OutOfRange; }
		if (!X.bEarly) { DiscBt.push_back(Bt); }
	}
	const double N = double(S.size());
	const double Mean = Sum / N, Sd = std::sqrt(std::max(Sum2 / N - Mean * Mean, 0.0));
	std::sort(DiscBt.begin(), DiscBt.end());
	const double BtMed = DiscBt[DiscBt.size() / 2], BtWant = std::sqrt(0.08 * 0.45);
	std::vector<double> Mw;
	for (u32 I = 0; I < 2001; ++I) { Mw.push_back(double(H::Uni_GalaxyBlackHole(GalaxyRecord(H::Uni_SeedMix(0x3A11u, I), -20.9f, false), 0.4f).Log10Mass)); }
	std::sort(Mw.begin(), Mw.end());
	const double MwMed = std::pow(10.0, Mw[Mw.size() / 2]);
	const H::FUniBlackHole One = H::Uni_BlackHoleOf(0.0f, 0.0f);
	const double Ledd = std::pow(10.0, double(One.Log10LumBol)), Rs = double(H::Uni_BhSchwarzschildCm(0.0f));
	const bool bOk = std::abs(Mean) < 0.03 && Sd > 0.95 && Sd < 1.03 && OutOfRange == 0 && std::abs(BtMed / BtWant - 1.0) < 0.03
		&& MwMed > 5.0e6 && MwMed < 3.0e7 && std::abs(Ledd / 1.26e38 - 1.0) < 1e-4 && std::abs(Rs / 2.9532e5 - 1.0) < 1e-3;
	Uni::Check(1, "T1.bh.mass_relation", "D-272 a galaxy's black hole (Haring & Rix 0.0014 x the bulge, 0.3 dex): over 10^4 records the residual's mean within 0.03 sigma and spread 0.95-1.03 sigma; bulge shares in range, the discs' median within 3% of log-uniform's; a Milky Way-like disc's median 5e6-3e7 M_sun; L_Edd and R_s of a solar mass")
		.Metric("resid_mean", Mean).Metric("resid_sd", Sd).Metric("disc_bt_median", BtMed).Metric("mw_median", MwMed)
		.Pass(bOk, Uni::Fmt("residual mean %+.4f sigma, spread %.4f sigma; %zu bulge shares out of range, discs' median %.4f (log-uniform %.4f); M_V -20.9 disc: median %.3g M_sun; L_Edd(1) %.4g erg/s, R_s(1) %.5g cm",
			Mean, Sd, OutOfRange, BtMed, BtWant, MwMed, Ledd, Rs));
}

UNI_TEST(bh, eddington)
{
	// The mixture at the default accreting share 0.4 over 10^5 records: the accreting share within
	// 0.01; the accreting branch's distribution (p(log lambda) ~ lambda^-0.65 on [-4, 0]) within 0.01
	// of its CDF at every decile (KS); the quiet branch within [-9, -5] and its median within 0.1 of
	// -7; share 0: none accreting, 1: all.
	const int N = 100000;
	std::vector<double> Act, Quiet;
	for (int I = 0; I < N; ++I)
	{
		const float U = H::Uni_DrawUnit(H::Uni_SeedMix(0xEDD1u, u32(I)), H::UNI_CH_BH_EDDINGTON, 0u);
		const double X = H::Uni_BhLog10EddingtonAt(U, 0.4f);
		(X >= -4.0 ? Act : Quiet).push_back(X);
	}
	const double Share = double(Act.size()) / N;
	std::sort(Act.begin(), Act.end());
	std::sort(Quiet.begin(), Quiet.end());
	const double A = 0.65, Lo = std::pow(10.0, -A * -4.0), Hi = 1.0;
	double Ks = 0.0;
	for (int D = 1; D < 10; ++D)
	{
		const double X = Act[size_t(double(Act.size()) * D / 10.0)];
		const double Cdf = (Lo - std::pow(10.0, -A * X)) / (Lo - Hi);
		Ks = std::max(Ks, std::abs(Cdf - D / 10.0));
	}
	const bool bQuietRange = !Quiet.empty() && Quiet.front() >= -9.0 && Quiet.back() <= -5.0;
	const double QuietMed = Quiet.empty() ? 0.0 : Quiet[Quiet.size() / 2];
	size_t Act0 = 0, Act1 = 0;
	for (int I = 0; I < 1000; ++I)
	{
		const float U = H::Uni_DrawUnit(H::Uni_SeedMix(0xEDD2u, u32(I)), H::UNI_CH_BH_EDDINGTON, 0u);
		Act0 += H::Uni_BhLog10EddingtonAt(U, 0.0f) >= -4.0f ? 1 : 0;
		Act1 += H::Uni_BhLog10EddingtonAt(U, 1.0f) >= -4.0f ? 1 : 0;
	}
	const double Above2 = double(std::count_if(Act.begin(), Act.end(), [](double X) { return X > -2.0; })) / N;
	const bool bOk = std::abs(Share - 0.4) < 0.01 && Ks < 0.01 && bQuietRange && std::abs(QuietMed + 7.0) < 0.1 && Act0 == 0 && Act1 == 1000;
	Uni::Check(1, "T1.bh.eddington", "D-272 the Eddington ratio's mixture (10^5 records, accreting share 0.4): the share within 0.01; the accreting branch (lambda^-0.65 per log on 1e-4..1) within 0.01 of its CDF at every decile; the quiet branch in [1e-9, 1e-5], median 1e-7 (0.1 dex); share 0 none, 1 all")
		.Metric("share", Share).Metric("ks", Ks).Metric("above_1e-2", Above2)
		.Pass(bOk, Uni::Fmt("accreting %.4f; decile KS %.4f; quiet in range %s, median %.3f; share 0: %zu accreting, 1: %zu of 1000; above 1e-2 of Eddington: %.2f%% of galaxies",
			Share, Ks, bQuietRange ? "yes" : "NO", QuietMed, Act0, Act1, 100.0 * Above2));
}

UNI_TEST(bh, nucleus)
{
	// The nucleus over 10^4 records (default share, cap 0.5, gain 1): never brighter than the cap
	// (1e-4 mag); below it the black hole's V light exactly (1e-3 mag); none drawn under 1e-6 of
	// the host; gain 2 0.7526 mag brighter where both are under the cap; the CPU lookup
	// (UniCore::GalaxyBlackHole) the core's numbers; the shares at 1% and 10% of the host reported.
	const std::vector<FSample> S = Samples(10000);
	size_t OverCap = 0, OffBelow = 0, NoneWrong = 0, GainBad = 0, CpuBad = 0, Drawn = 0, At1 = 0, At10 = 0, Capped = 0;
	const float CapMag = -2.5f * std::log10(0.5f);
	for (const FSample& X : S)
	{
		const H::FUniObject O = GalaxyRecord(X.Seed, X.Mag, X.bEarly);
		const float Host = H::Uni_ObjectAbsMag(O);
		const H::FUniBlackHole B = H::Uni_GalaxyBlackHole(O, 0.4f);
		const float Mn = H::Uni_NucleusAbsMag(B, Host, 0.5f, 1.0f);
		const double Ratio = std::pow(10.0, -0.4 * (double(B.AbsMagV) - double(Host)));
		if (Ratio < 1e-6) { NoneWrong += Mn < H::UNI_BH_NONE_MAG ? 1 : 0; continue; }
		++Drawn;
		if (Mn < Host + CapMag - 1e-4f) { ++OverCap; }
		if (Ratio < 0.5 && std::abs(Mn - B.AbsMagV) > 1e-3f) { ++OffBelow; }
		if (Ratio >= 0.5) { ++Capped; }
		At1 += Ratio >= 0.01 ? 1 : 0; At10 += Ratio >= 0.1 ? 1 : 0;
		const float M2 = H::Uni_NucleusAbsMag(B, Host, 0.5f, 2.0f);
		if (Ratio * 2.0 < 0.5 && std::abs((Mn - M2) - 0.7526f) > 1e-3f) { ++GainBad; }
		FObjectGPU G = FromHlsl(O);
		const FBlackHoleInfo I = GalaxyBlackHole(G);
		if (!I.bValid || I.NucleusAbsMag != Mn || std::abs(std::log10(I.MassSolar) - double(B.Log10Mass)) > 1e-6 || std::abs(std::log10(I.EddingtonRatio) - double(B.Log10Eddington)) > 1e-6) { ++CpuBad; }
	}
	const bool bOk = OverCap == 0 && OffBelow == 0 && NoneWrong == 0 && GainBad == 0 && CpuBad == 0;
	Uni::Check(1, "T1.bh.nucleus", "D-272 the nucleus (10^4 records; share 0.4, cap 0.5, gain 1): never over the cap; under it the black hole's V light (1e-3 mag); none under 1e-6 of the host; gain 2 0.7526 mag brighter under the cap; the CPU lookup the core's numbers")
		.Metric("at_1pct", double(At1) / S.size()).Metric("at_10pct", double(At10) / S.size()).Metric("capped", double(Capped) / S.size())
		.Pass(bOk, Uni::Fmt("%zu drawn (over 1e-6 of the host) of %zu: %zu over the cap, %zu off the black hole's light below it, %zu drawn under 1e-6, gain %zu wrong, CPU lookup %zu different; nuclei at 1%% of the host or more %.2f%%, at 10%% %.2f%%, capped %.2f%%",
			Drawn, S.size(), OverCap, OffBelow, NoneWrong, GainBad, CpuBad, 100.0 * At1 / S.size(), 100.0 * At10 / S.size(), 100.0 * Capped / S.size()));
}

UNI_TEST(bh, nucleus_sprite)
{
	// The project pass's nucleus over the web scene's resident galaxies (void camera):
	// - every galaxy's own sprite bit for bit with the nuclei on and off;
	// - a visible nucleus: a point (not extended) at its galaxy's distance and direction, its light
	//   (physical sprite mode: all in the core) the point-source light of its magnitude there
	//   (1e-4; the half-float magnitude its record carries), the same whatever share of the galaxy its volume carrier takes;
	// - visible nuclei with the defaults and with every black hole accreting x30 (the owner's "ramp
	//   their frequency up to find instances to test"): more of them;
	// - stills: the brightest nuclei at 12 galaxy radii, volume and sprites, with and without.
	FUniverseModel M(UniScenes::WebToGalaxy());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::VoidCamera();
	P.Update(Cam, FVec3f{});
	u32 GalMask = 0;
	for (const FArchetype& A : M.Definition().Archetypes)
	{
		for (const FUniEmitter& E : A.Emitters) { if (E.FieldId == H::UNI_FIELD_GALAXY && A.Id < 32u) { GalMask |= 1u << A.Id; } }
	}
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 640; Cm.Height = 360; Cm.DeltaMag = 4.0; Cm.LookAlong({ 1.0, 0.3, 0.1 });
	Cm.DiscArchetypeMask = GalMask;
	// The web scene's galaxies are 10-30 Mpc from the void camera, mostly fainter than m_lim as
	// points: the views see to m 20 (exposure 19), the same for every configuration.
	auto Deep = [](const FCamera& C) { H::FUniView V = C.View(); V.LimitingMag = 20.0f; V.ExposureMag = 19.0f; return V; };
	size_t Galaxies = 0, OwnDiffer = 0, Visible = 0, VisibleBoost = 0, NotPoint = 0, PlaceBad = 0, LightBad = 0, ShareBad = 0;
	double WorstLight = 0.0;
	P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float Fade, u64)
	{
		const u32 Arch = O.A[3] & 0xFFFFu;
		if (Arch >= 32u || !((GalMask >> Arch) & 1u)) { return; }
		++Galaxies;
		H::uint4 D[3];
		CellDelta(K, Cam, D);
		const H::FUniObject Ho = ToHlsl(O);
		FCamera Off = Cm, On = Cm, Boost = Cm, Phys = Cm;
		On.NucleusArchetypeMask = GalMask;
		Boost.NucleusArchetypeMask = GalMask; Boost.NucleusActive = 1.0f; Boost.NucleusGain = 30.0f;
		Phys.NucleusArchetypeMask = GalMask;
		H::FUniView Vp = Deep(Phys); Vp.CoreSat = 0.0f;   // physical mode: the core holds all the light
		const H::FUniSprite S0 = H::Uni_ProjectObjectH(D[0], D[1], D[2], u32(K.Level), Fade, Ho, Deep(Off), false, -1.0f);
		const H::FUniSprite S1 = H::Uni_ProjectObjectH(D[0], D[1], D[2], u32(K.Level), Fade, Ho, Deep(On), false, -1.0f);
		if (std::memcmp(&S0, &S1, sizeof(S0)) != 0) { ++OwnDiffer; }
		const H::FUniSprite Sn = H::Uni_ProjectNucleusH(D[0], D[1], D[2], u32(K.Level), Fade, Ho, Deep(On));
		const H::FUniSprite Sb = H::Uni_ProjectNucleusH(D[0], D[1], D[2], u32(K.Level), Fade, Ho, Deep(Boost));
		VisibleBoost += (Sb.Flags & H::UNI_SPRITE_VISIBLE) ? 1 : 0;
		if (!(Sn.Flags & H::UNI_SPRITE_VISIBLE)) { return; }
		++Visible;
		if (Sn.Flags & H::UNI_SPRITE_EXTENDED) { ++NotPoint; }
		if (std::abs(Sn.TrueDistance / S0.TrueDistance - 1.0f) > 1e-6f || ((S0.Flags & H::UNI_SPRITE_VISIBLE) && (std::abs(Sn.NdcX - S0.NdcX) > 1e-5f || std::abs(Sn.NdcY - S0.NdcY) > 1e-5f))) { ++PlaceBad; }
		const H::FUniSprite Sp = H::Uni_ProjectNucleusH(D[0], D[1], D[2], u32(K.Level), Fade, Ho, Vp);
		const float Mn = H::Uni_NucleusAbsMag(H::Uni_GalaxyBlackHole(Ho, Vp.NucleusActive), H::Uni_ObjectAbsMag(Ho), Vp.NucleusCap, Vp.NucleusGain);   // its mean over directions
		const double R = Sp.TrueDistance;
		// D-273: towards the camera, the disc's light by direction (UNI_BH_DISC_BEAM at the angle to its axis).
		const H::FUniBlackHole Bk = H::Uni_GalaxyBlackHole(Ho, Vp.NucleusActive);
		const H::float3 Ax = H::Uni_BhDiscAxis(Ho.B.x, H::Uni_DiscNormal(Ho.B.x));
		const H::float3 Rl = H::Uni_ObjectRelativeC(H::Uni_CellRel(D[0], D[1], D[2], u32(K.Level)), H::uint3(Ho.A.x, Ho.A.y, Ho.A.z), Vp.CameraFraction);   // as the core reads it
		const float Mu = std::fabs(H::dot(Ax, Rl)) / std::max(H::Uni_SafeLength(Rl), 1.0e-3f);
		const float Md = H::Uni_F16ToF32(H::Uni_F32ToF16(Mn - 2.5f * std::log10(std::max(H::Uni_BhDiscBeam(H::Uni_BhDiscPeakK(Bk), Mu), 1.0e-6f))));
		const double Want = std::pow(10.0, -0.4 * (double(H::Uni_ApparentMag(Md, float(R))) - double(Vp.ExposureMag))) * double(H::Uni_MagFade(H::Uni_ApparentMag(Md, float(R)), Vp.LimitingMag)) * std::clamp(double(Fade), 0.0, 1.0) * double(H::Uni_HorizonDimming(float(R)));
		const double Got = H::Uni_Luminance(Sp.Radiance);
		const double Err = std::abs(Got / std::max(Want, 1e-300) - 1.0);
		WorstLight = std::max(WorstLight, Err);
		if (Err > 1e-4) { ++LightBad; }
		// Never handed off: the carrier's share does not enter it (the galaxy's own sprite with its
		// whole light handed off is not visible; the nucleus is the same).
		const H::FUniSprite Sh = H::Uni_ProjectObjectH(D[0], D[1], D[2], u32(K.Level), Fade, Ho, Deep(On), false, 1.0f);
		if ((Sh.Flags & H::UNI_SPRITE_VISIBLE) && (O.A[3] >> 16 & H::UNI_OBJ_FLAG_HAS_CONTRIBUTOR)) { ++ShareBad; }
	});

	// Stills: the galaxies with the brightest nuclei (relative to the host) among those with a
	// contributor, at 12 radii, the defaults with and without the nuclei.
	struct FCand { const FContributor* Ct; float Ratio; };
	std::vector<FCand> Cands;
	for (const auto& KV : P.Contributors())
	{
		const FContributor& Ct = KV.second;
		if (Ct.Emitter == nullptr || Ct.Emitter->FieldId != H::UNI_FIELD_GALAXY) { continue; }
		auto It = P.CacheRef().find(Ct.Parent.Cell);
		if (It == P.CacheRef().end() || Ct.Parent.Index >= It->second.Objects.size()) { continue; }
		const H::FUniObject Ho = ToHlsl(It->second.Objects[Ct.Parent.Index]);
		const float Host = H::Uni_ObjectAbsMag(Ho);
		const float Mn = H::Uni_NucleusAbsMag(H::Uni_GalaxyBlackHole(Ho, H::UNI_BH_ACTIVE_SHARE), Host, 0.5f, 1.0f);
		if (Mn < H::UNI_BH_NONE_MAG) { Cands.push_back({ &Ct, float(std::pow(10.0, -0.4 * double(Mn - Host))) }); }
	}
	std::sort(Cands.begin(), Cands.end(), [](const FCand& A, const FCand& B) { return A.Ratio > B.Ratio; });
	const int Tile = 160, NShow = std::min<int>(4, int(Cands.size()));
	FImage Sheet(std::max(1, NShow) * Tile, 2 * Tile);
	std::string Ratios;
	for (int I = 0; I < NShow; ++I)
	{
		const FContributor& Ct = *Cands[size_t(I)].Ct;
		auto It = P.CacheRef().find(Ct.Parent.Cell);
		const FObjectGPU& O = It->second.Objects[Ct.Parent.Index];
		const double Radius = std::exp2(double(H::Uni_ObjectRadiusLog2(ToHlsl(O))));
		const FV3 Dir = Norm(FV3{ Ct.Rot[6] + 0.6, Ct.Rot[7] - 0.3, Ct.Rot[8] + 0.2 });   // about 40 degrees off its pole
		FCoord CamG = Ct.Centre;
		{ FVec3f Fr{}; AddDisplacement(CamG, Fr, FVec3d{ -Dir.X * 12.0 * Radius, -Dir.Y * 12.0 * Radius, -Dir.Z * 12.0 * Radius }); }
		FRefPipeline Pg(G);
		Pg.bVelocityLod = false;
		Pg.Update(CamG, FVec3f{});
		for (int Row = 0; Row < 2; ++Row)
		{
			FCamera Cg; Cg.Coord = CamG; Cg.Width = Tile; Cg.Height = Tile; Cg.FovX = 0.25; Cg.LookAlong(Dir);
			Cg.DiscArchetypeMask = GalMask;
			Cg.NucleusArchetypeMask = Row == 1 ? GalMask : 0u;
			const FVolumeScene Sc = VB.Build(Pg, CamG, FVec3f{}, Cg.View());
			FImage Img(Tile, Tile);
			RenderFrameWithVolume(Pg, Sc, Cg, Img);
			for (int Y = 0; Y < Tile; ++Y) for (int X = 0; X < Tile; ++X)
			{
				const float* Px = &Img.Rgb[(size_t(Y) * Tile + X) * 3];
				Sheet.Add(I * Tile + X, Row * Tile + Y, Px[0], Px[1], Px[2]);
			}
		}
		Ratios += Uni::Fmt("%s%.3f", I ? ", " : "", double(Cands[size_t(I)].Ratio));
	}
	if (NShow > 0)
	{
		const std::string Base = RenderDir() + "/still_bh_nuclei";
		Sheet.WritePpm(Base + ".ppm");
		if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb"))
		{
			std::fprintf(F, "{\"lines\":[\"D-272: the web scene's brightest nuclei (nucleus / host %s) at 12 galaxy radii; top without the nuclei, bottom with\",\"volume and sprites; defaults (accreting share 0.4, cap 0.5, gain 1)\"]}\n", Ratios.c_str());
			std::fclose(F);
		}
	}
	const bool bOk = Galaxies > 100 && OwnDiffer == 0 && Visible > 0 && VisibleBoost > Visible && NotPoint == 0 && PlaceBad == 0 && LightBad == 0 && ShareBad == 0 && NShow > 0;
	Uni::Check(2, "T2.bh.nucleus_sprite", "D-272 the project pass's nucleus (web scene, void camera): every galaxy's own sprite bit for bit with the nuclei on and off; a visible nucleus a point at its galaxy's distance and direction, its light the point-source light of its magnitude (1e-4, physical mode), whatever its galaxy's carrier takes; more visible with every black hole accreting x30; stills of the brightest")
		.Metric("galaxies", double(Galaxies)).Metric("visible", double(Visible)).Metric("visible_boost", double(VisibleBoost)).Metric("light_worst", WorstLight)
		.Pass(bOk, Uni::Fmt("%zu galaxies: own sprites %zu different; nuclei visible %zu (all accreting x30: %zu); %zu not points, %zu misplaced, light %zu off (worst %.2g); %zu galaxy sprites visible with all their light handed off; stills of %d (nucleus / host %s)",
			Galaxies, OwnDiffer, Visible, VisibleBoost, NotPoint, PlaceBad, LightBad, WorstLight, ShareBad, NShow, Ratios.c_str()));
}
