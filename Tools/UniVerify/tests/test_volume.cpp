// test_volume.cpp
// Tier 3: the volume pass (Docs/VOLUMETRICS.md, D-128..D-134), through the shared core
// Uni_VolumeMarch and the CPU builder in UniVolume.h.
//   march         adaptive march vs a brute-force fine march (outside face-on / edge-on, inside)
//   conservation  sprites + volume = total light, over the whole sphere, swept over dm
//   handoff       galaxy sprite -> carrier: in-frame flux continuous while approaching
//   nested        camera inside the galaxy inside the web: union march = sum of parts
//   precision     translation invariance at far coordinates; 0.001 cm camera steps
//   background    the extragalactic background is finite and converged
//   budget        luminosity budget of the demo galaxy (D-133), reported
//   frames        reference stills with the volume

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	std::string VolDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	std::string Esc(const std::string& S) { std::string O; for (char C : S) { if (C == '"' || C == '\\') { O += '\\'; } O += C; } return O; }

	void WriteVolFrame(const FImage& Img, const std::string& Name, const std::vector<std::string>& Hud)
	{
		const std::string Base = VolDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb");
		if (!F) { return; }
		std::fprintf(F, "{\"lines\":[");
		for (size_t I = 0; I < Hud.size(); ++I) { std::fprintf(F, "%s\"%s\"", I ? "," : "", Esc(Hud[I]).c_str()); }
		std::fprintf(F, "]}\n");
		std::fclose(F);
	}

	FCoord Moved(const FCoord& C, const FV3& D)
	{
		FCoord R = C;
		FVec3f F;
		AddDisplacement(R, F, FVec3d{ D.X, D.Y, D.Z });
		return R;
	}

	FUniverseDef NoDust(FUniverseDef D)
	{
		for (FArchetype& A : D.Archetypes) { A.Volume.bDust = false; }
		return D;
	}

	/** Brute-force reference for one ray: fine log + uniform steps, no field hints, no
	 *  prefiltering, double accumulation, through the same field, LUT and carrier geometry. */
	H::float3 BruteForce(const FVolumeScene& Sc, H::float3 Dir, float TScene, double* OutTau = nullptr)
	{
		const auto Pb = Sc.ParamBuf();
		const auto Lb = Sc.LutBuf();
		const H::FUniVolumeView V = ToHlsl(Sc.View);
		struct FI { H::FUniCarrier C; H::float3 Ld; double T0, T1; };
		std::vector<FI> Act;
		double TMin = 1e300, TMax = 0;
		for (const FCarrierGPU& G : Sc.Carriers)
		{
			FI I; I.C = ToHlsl(G);
			if (!(I.C.Weight > 0.0f)) { continue; }
			I.Ld = H::Uni_CarrierLocalDir(I.C, Dir);
			float A = 0, B = 0;
			if (!H::Uni_CarrierInterval(I.C, I.Ld, TScene, A, B)) { continue; }
			I.T0 = A; I.T1 = B;
			TMin = std::min(TMin, I.T0); TMax = std::max(TMax, I.T1);
			Act.push_back(I);
		}
		double Bv[3] = { 0, 0, 0 }, Tr[3] = { 1, 1, 1 };
		if (Act.empty()) { if (OutTau) { *OutTau = 0; } return H::float3(0, 0, 0); }
		double T = TMin;
		const double Uniform = (TMax - TMin) / 4.0e5;
		while (T < TMax)
		{
			double Next = TMax;
			bool bAny = false;
			for (const FI& I : Act) { if (T < I.T0) { Next = std::min(Next, I.T0); } else if (T < I.T1) { bAny = true; Next = std::min(Next, I.T1); } }
			if (!bAny) { T = Next; continue; }
			double S = std::min(std::max(0.002 * T, 1.0e14), Uniform);
			S = std::max(std::min(S, Next - T), 1e-9 * T);
			const double Tm = T + 0.5 * S;
			double E[3] = { 0, 0, 0 }, Tau[3] = { 0, 0, 0 };
			for (const FI& I : Act)
			{
				if (Tm < I.T0 || Tm >= I.T1) { continue; }
				const H::FUniFieldParams Par = Pb[I.C.ParamIndex];
				const H::float3 P = H::Uni_CarrierPoint(I.C, I.Ld, float(Tm));
				const double Du = S * double(I.C.InvRadius);
				const double Rho = H::Uni_FieldSample(I.C.FieldId, P, Par);
				if (Rho > 0)
				{
					const H::float3 L = H::Uni_VolumeLut(Lb, I.C.LutIndex, float(Tm));
					const double K = double(I.C.Amplitude) * double(I.C.Weight) * Rho * Du;
					E[0] += L.x * K; E[1] += L.y * K; E[2] += L.z * K;
				}
				if (I.C.Flags & H::UNI_CARRIER_DUST)
				{
					const H::FUniFieldParams D = Pb[I.C.DustParamIndex];
					const double Rd = H::Uni_FieldSample(I.C.FieldId, P, D);
					const double Sig[3] = { V.DustSigmaRgb.x, V.DustSigmaRgb.y, V.DustSigmaRgb.z };
					for (int K = 0; K < 3; ++K) { Tau[K] += Sig[K] * double(D.D.y) * Rd * Du; }
				}
			}
			for (int K = 0; K < 3; ++K)
			{
				const double Ex = std::exp(-Tau[K]);
				const double W = Tau[K] > 1e-9 ? (1 - Ex) / Tau[K] : 1.0;
				Bv[K] += Tr[K] * E[K] * W;
				Tr[K] *= Ex;
			}
			T += S;
		}
		for (const FI& I : Act)
		{
			if (!(I.C.Flags & H::UNI_CARRIER_TAIL) || I.T1 < double(I.C.TEnd) || TScene <= I.C.TEnd) { continue; }
			const H::float3 Tl = H::Uni_VolumeLut(Lb, I.C.LutIndex + 1u, I.C.TEnd);
			const double K = double(I.C.Amplitude) * double(I.C.Weight) * double(I.C.MeanDensity) * double(I.C.InvRadius);
			Bv[0] += Tr[0] * Tl.x * K; Bv[1] += Tr[1] * Tl.y * K; Bv[2] += Tr[2] * Tl.z * K;
		}
		if (OutTau) { *OutTau = -std::log(std::max(Tr[1], 1e-300)); }
		return H::float3(float(Bv[0]), float(Bv[1]), float(Bv[2]));
	}

	/** Sum of a scene's volume luminance flux over the whole sphere (six 90-degree cube faces). */
	double SphereVolumeFlux(const FVolumeScene& Sc, const FCoord& Coord, int N, size_t* Truncated = nullptr, u64* Steps = nullptr)
	{
		const FV3 Dirs[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		double Sum = 0.0;
		for (const FV3& D : Dirs)
		{
			FCamera Cam; Cam.Coord = Coord; Cam.Width = N; Cam.Height = N; Cam.LookAlong(D);
			FImage Img(N, N);
			const FVolumeStats St = RenderVolume(Sc, Cam, Img);
			Sum += 0.2126 * St.Flux[0] + 0.7152 * St.Flux[1] + 0.0722 * St.Flux[2];   // luminance
			if (Truncated) { *Truncated += St.Truncated; }
			if (Steps) { *Steps += St.Steps; }
		}
		return Sum;
	}

	/** Luminance flux of every resident object's sprite over the whole sphere, before any
	 *  frustum, exactly as project-and-cull computes it; and the Poisson spread sqrt(sum f^2). */
	void SphereSpriteFlux(const FRefPipeline& P, const FCamera& Cam, double& Sum, double& Spread)
	{
		const H::FUniView V = Cam.View();
		double S = 0.0, S2 = 0.0;
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float Fade, u64)
		{
			const H::FUniObject HO = UniCore::ToHlsl(O);
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const FVec3d Rel = ToDouble(Sub(ObjectCoord(K, Off), Cam.Coord));
			const double R = std::max(std::sqrt(Rel.X * Rel.X + Rel.Y * Rel.Y + Rel.Z * Rel.Z), 1e-3);
			const double Radius = std::exp2(double(H::Uni_ObjectRadiusLog2(HO)));
			const float Th = float(Radius / R);
			const u32 Fl = H::Uni_ObjectFlags(HO);
			if (H::Uni_PromoteTest(float(R), float(Radius), false) && !(Fl & H::UNI_OBJ_FLAG_EXTENDED)) { return; }   // promoted bodies: real geometry, negligible here
			const bool bCarrier = (Fl & (H::UNI_OBJ_FLAG_EXTENDED | H::UNI_OBJ_FLAG_HAS_CONTRIBUTOR)) == (H::UNI_OBJ_FLAG_EXTENDED | H::UNI_OBJ_FLAG_HAS_CONTRIBUTOR);
			const double Share = bCarrier ? 1.0 - H::Uni_CarrierHandoff(Th, V.ThetaPixel) : 1.0;
			const double M = H::Uni_ApparentMag(H::Uni_ObjectAbsMag(HO), float(R));
			const double I = std::pow(10.0, -0.4 * (M - V.ExposureMag)) * H::Uni_ObjectVisibility(HO, float(R), Th, V) * std::clamp(double(Fade), 0.0, 1.0) * Share * H::Uni_HorizonDimming(float(R));
			S += I; S2 += I * I;   // luminance: radiance = colour / Y x I (D-135)
		});
		Sum = S; Spread = std::sqrt(S2);
	}
}

// ==========================================================================================

UNI_TEST(volume, march)
{
	// Rays through the anchored galaxy from outside (face-on, edge-on) and inside (the solar
	// camera, in-plane and out of plane), with dust: the adaptive march vs a brute-force march.
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FCase { const char* Name; FV3 Offset; bool bSolar; FV3 Look; };
	const double Kpc = Units::Kpc;
	const FCase Cases[] = {
		{ "face-on 60 kpc", { 0, 0, 60 * Kpc }, false, { 0, 0, -1 } },
		{ "edge-on 60 kpc", { 60 * Kpc, 0, 1.0 * Kpc }, false, { -1, 0, -0.0167 } },
		{ "inside, towards centre", { 0, 0, 0 }, true, { -1, -0.12, -0.02 } },
		{ "inside, out of plane", { 0, 0, 0 }, true, { -0.3, 0.1, 1 } },
	};
	double Worst = 0.0, WorstTau = 0.0;
	size_t Rays = 0, Trunc = 0, Compared = 0;
	u64 StepSum = 0; u32 StepMax = 0;
	std::string Detail;
	for (const FCase& C : Cases)
	{
		FRefPipeline P(G);
		const FCoord Cam = C.bSolar ? UniScenes::SolarCamera() : Moved(FCoord::Origin(), C.Offset);
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 96; Cm.Height = 54; Cm.LookAlong(C.Look);
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View());
		const H::FUniVolumeView V = ToHlsl(Sc.View);
		double CaseWorst = 0.0;
		for (int Y = 0; Y < 9; ++Y) for (int X = 0; X < 16; ++X)
		{
			const FV3 D = PixelDir(Cm, 16, 9, X + 0.5, Y + 0.5);
			const H::float3 Dir(float(D.X), float(D.Y), float(D.Z));
			const H::FUniVolumeResult R = H::Uni_VolumeMarch(Dir, 3.0e38f, Sc.CarrierBuf(), Sc.ParamBuf(), Sc.LutBuf(), V);
			++Rays; Trunc += R.Truncated; StepSum += R.Steps; StepMax = std::max(StepMax, R.Steps);
			if (!R.Active) { continue; }
			double Tau = 0;
			const H::float3 Ref = BruteForce(Sc, Dir, 3.0e38f, &Tau);
			if (!(Ref.y > 0.0f)) { continue; }
			++Compared;
			const double Err = std::fabs(double(R.Radiance.y) - double(Ref.y)) / double(Ref.y);
			const double TauErr = std::fabs(-std::log(std::max(double(R.Transmittance.y), 1e-300)) - Tau);
			// Fully opaque rays (tau > 7) stop early by design: compare emission only.
			Worst = std::max(Worst, Err); CaseWorst = std::max(CaseWorst, Err);
			if (Tau < 7.0) { WorstTau = std::max(WorstTau, TauErr); }
		}
		Detail += Uni::Fmt("%s: worst %.3f%%; ", C.Name, CaseWorst * 100.0);
	}
	const double MeanSteps = Rays ? double(StepSum) / double(Rays) : 0.0;
	Uni::Check(3, "T3.vol.march", "Adaptive march (log steps, field step hints, interval union) vs brute-force fine march: emission within 1%, optical depth within 0.02, no truncation")
		.Metric("rays", double(Rays)).Metric("compared", double(Compared)).Metric("worst_rel_err", Worst).Metric("worst_tau_err", WorstTau)
		.Metric("mean_steps", MeanSteps).Metric("max_steps", StepMax).Metric("truncated", double(Trunc))
		.Pass(Worst < 0.01 && WorstTau < 0.02 && Trunc == 0 && Compared > 100,
			Uni::Fmt("%zu rays (%zu compared): worst emission error %.3f%%, worst tau error %.4f, steps mean %.0f max %u, %zu truncated. %s",
				Rays, Compared, Worst * 100.0, WorstTau, MeanSteps, StepMax, Trunc, Detail.c_str()));
}

UNI_TEST(volume, conservation)
{
	// D-128 over the whole sphere at the solar camera, no dust: the volume with the real fades
	// plus the realised sprites must equal the volume with every fade at zero (all light
	// unresolved), within the sprites' Poisson spread. Swept over sensitivity: as dm grows,
	// light moves from the volume into sprites and the sum must not change.
	FUniverseModel M(NoDust(UniScenes::GalaxyToStar()));
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const FCoord Cam = UniScenes::SolarCamera();
	const int N = 40;
	double WorstZ = 0.0, WorstRel = 0.0;
	std::string Detail;
	bool bOk = true;
	for (double Dm : { -1.0, 0.0, 2.0 })
	{
		FRefPipeline P(G);
		P.DeltaMag = Dm;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.DeltaMag = Dm;
		const H::FUniView V = Cm.View();
		VB.ForceLevelFade = -1.0f;
		const double Vol = SphereVolumeFlux(VB.Build(P, Cam, FVec3f{}, V), Cam, N);
		VB.ForceLevelFade = 0.0f;
		const double All = SphereVolumeFlux(VB.Build(P, Cam, FVec3f{}, V), Cam, N);
		VB.ForceLevelFade = -1.0f;
		double Spr = 0, Spread = 0;
		SphereSpriteFlux(P, Cm, Spr, Spread);
		const double Diff = (Vol + Spr) - All;
		const double Z = Spread > 0 ? Diff / Spread : 0.0;
		const double Rel = Diff / All;
		// Pass: within 3 sigma of the sprites' Poisson spread, or within 1% of the total.
		const bool bThis = std::fabs(Z) < 3.0 || std::fabs(Rel) < 0.01;
		bOk = bOk && bThis;
		WorstZ = std::max(WorstZ, std::fabs(Z));
		WorstRel = std::max(WorstRel, std::fabs(Rel));
		Detail += Uni::Fmt("dm %+.0f: volume %.4g + sprites %.4g = %.4g vs all-unresolved %.4g (%+.2f%%, z %+.2f; sprites carry %.1f%%); ",
			Dm, Vol, Spr, Vol + Spr, All, Rel * 100.0, Z, 100.0 * Spr / All);
	}
	Uni::Check(3, "T3.vol.conservation", "Sprites + volume = total light over the whole sphere (D-128), at dm = -1, 0, +2: within 3 sigma of the sprites' Poisson spread or 1%")
		.Metric("worst_z", WorstZ).Metric("worst_rel", WorstRel)
		.Pass(bOk, Detail);
}

namespace
{
	double Lum(const double* Rgb) { return 0.2126 * Rgb[0] + 0.7152 * Rgb[1] + 0.0722 * Rgb[2]; }

	/** The demo, with N_total rescaled so the anchored galaxy's stars add up to its magnitude. */
	FUniverseDef BudgetConsistent(FUniverseDef D, double* OutBudgetMag = nullptr)
	{
		FUniverseModel M0(D);
		const double Mb = LuminosityBudget(M0, UniScenes::kGalaxy, -21.0);
		if (OutBudgetMag) { *OutBudgetMag = Mb; }
		for (FEmitter& E : D.Archetypes[UniScenes::kGalaxy].Emitters) { E.NTotal *= std::pow(10.0, 0.4 * (Mb - (-21.0))); }
		return D;
	}
}

UNI_TEST(volume, budget)
{
	// D-133: what the demo galaxy's stars add up to, against its drawn magnitude. Reported for the
	// owner's decision; the check passes when the validator measures it and the corrected model
	// closes it.
	FUniverseModel M(UniScenes::GalaxyToStar());
	const double Mb = LuminosityBudget(M, UniScenes::kGalaxy, -21.0);
	double Mb0 = 0;
	FUniverseModel Mc(BudgetConsistent(UniScenes::GalaxyToStar(), &Mb0));
	const double Mc2 = LuminosityBudget(Mc, UniScenes::kGalaxy, -21.0);
	const double Ncons = Mc.Definition().Arch(UniScenes::kGalaxy).Emitters[0].NTotal;
	Uni::Check(3, "T3.vol.budget", "Luminosity budget (D-133): the demo galaxy's children vs its drawn magnitude (reported); a rescaled N_total closes it to < 0.01 mag")
		.Metric("children_mag", Mb).Metric("drawn_mag", -21.0).Metric("mismatch_mag", Mb + 21.0).Metric("consistent_ntotal", Ncons)
		.Pass(std::isfinite(Mb) && std::fabs(Mc2 + 21.0) < 0.01,
			Uni::Fmt("2e11 stars from the placeholder stellar function sum to M = %.2f; the galaxy is drawn at M = -21.00 (mismatch %.2f mag, %.1fx). N_total = %.3g would close it (corrected model: M = %.3f). Owner decision (D-133).",
				Mb, Mb + 21.0, std::pow(10.0, -0.4 * (Mb + 21.0)), Ncons, Mc2));
}

UNI_TEST(volume, handoff)
{
	// Approach the anchored galaxy from 6 Mpc to 300 kpc on an inclined line, with a budget-
	// consistent model and no dust. The in-frame luminosity, normalised by 1 / d^2 and horizon
	// dimming, must stay constant while the galaxy's light moves from its sprite to its carrier.
	FUniverseModel M(NoDust(BudgetConsistent(UniScenes::GalaxyToStar())));
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const FV3 U = Norm({ 0.5, 0.3, 0.8 });
	const int Steps = 12;
	double Ref = 0.0, Worst = 0.0;
	std::string Detail;
	double ShareSpriteFirst = 0, ShareSpriteLast = 0;
	for (int I = 0; I < Steps; ++I)
	{
		const double D = 6.0 * Units::Mpc * std::pow(0.3 * Units::Mpc / (6.0 * Units::Mpc), double(I) / double(Steps - 1));
		FRefPipeline P(G);
		const FCoord Cam = Moved(FCoord::Origin(), U * D);
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 640; Cm.Height = 360; Cm.LookAlong(U * -1.0);
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View());
		FImage Img(Cm.Width, Cm.Height);
		FVolumeStats Vs;
		RenderFrameWithVolume(P, Sc, Cm, Img, &Vs);
		const double Vol = Lum(Vs.Flux), Spr = Lum(Vs.SpriteFlux);
		const double Norm = (Vol + Spr) * D * D / double(H::Uni_HorizonDimming(float(D)));
		if (I == 0) { Ref = Norm; ShareSpriteFirst = Spr / (Vol + Spr); }
		if (I == Steps - 1) { ShareSpriteLast = Spr / (Vol + Spr); }
		const double Dev = Norm / Ref - 1.0;
		Worst = std::max(Worst, std::fabs(Dev));
		if (I % 3 == 0 || I == Steps - 1) { Detail += Uni::Fmt("[%s: sprite %.0f%% %+.2f%%] ", FormatDistance(D).c_str(), 100.0 * Spr / (Vol + Spr), Dev * 100.0); }
	}
	Uni::Check(3, "T3.vol.handoff", "Galaxy sprite -> volume carrier while approaching 6 Mpc -> 300 kpc (budget-consistent model): in-frame luminosity x d^2 constant within 2%")
		.Metric("worst_dev", Worst).Metric("sprite_share_far", ShareSpriteFirst).Metric("sprite_share_near", ShareSpriteLast)
		.Pass(Worst < 0.02 && ShareSpriteFirst > 0.99 && ShareSpriteLast < 0.01,
			Uni::Fmt("worst deviation %.2f%%; the sprite carries %.1f%% at 6 Mpc and %.1f%% at 300 kpc. %s", Worst * 100.0, ShareSpriteFirst * 100.0, ShareSpriteLast * 100.0, Detail.c_str()));
}

UNI_TEST(volume, nested)
{
	// Camera inside the anchored galaxy, inside the cosmic web: the union march over both
	// carriers equals the galaxy alone plus the background alone (no dust, so emission adds).
	FUniverseModel M(NoDust(UniScenes::Flythrough()));
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const FCoord Cam = UniScenes::SolarCamera();
	FRefPipeline P(G);
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 64; Cm.Height = 36; Cm.LookAlong({ -0.3, 0.1, 1 });
	const H::FUniView V = Cm.View();
	auto Flux = [&](bool bL, bool bB) { VB.bLocal = bL; VB.bBackground = bB; FImage Img(Cm.Width, Cm.Height); const FVolumeStats S = RenderVolume(VB.Build(P, Cam, FVec3f{}, V), Cm, Img); return Lum(S.Flux); };
	const double Both = Flux(true, true), Gal = Flux(true, false), Bg = Flux(false, true);
	VB.bLocal = VB.bBackground = true;
	const double Rel = (Both - (Gal + Bg)) / Both;
	Uni::Check(3, "T3.vol.nested", "Nested carriers (galaxy inside the web): union march = galaxy alone + background alone within 1%")
		.Metric("rel", Rel).Metric("background_share", Bg / Both)
		.Pass(std::fabs(Rel) < 0.01 && Bg > 0.0 && Gal > 0.0,
			Uni::Fmt("union %.5g, galaxy %.5g + background %.5g = %.5g (%+.3f%%); the background is %.3f%% of the light out of the plane", Both, Gal, Bg, Gal + Bg, Rel * 100.0, 100.0 * Bg / Both));
}

UNI_TEST(volume, precision)
{
	// The same galaxy and the same camera offset, once at the nominal origin and once 2^126 + 1e35
	// cm away: carriers come from exact 128-bit deltas, so the images must agree. Then the camera
	// steps 0.001 cm per frame: the image must not change by more than 1e-4 relative.
	auto Render = [](const FCoord& Anchor, const FVec3d& Off, const FVec3f& Frac)
	{
		FUniverseDef D = NoDust(UniScenes::GalaxyToStar());
		D.Anchors[0].Coord = Anchor;
		FUniverseModel M(D);
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		VB.bBackground = false;
		FRefPipeline P(G);
		FCoord Cam = Anchor; FVec3f F;
		AddDisplacement(Cam, F, Off);
		P.Update(Cam, Frac);
		FCamera Cm; Cm.Coord = Cam; Cm.Frac = Frac; Cm.Width = 64; Cm.Height = 36; Cm.LookAlong({ -1, -0.12, -0.02 });
		FImage Img(Cm.Width, Cm.Height);
		RenderVolume(VB.Build(P, Cam, Frac, Cm.View()), Cm, Img);
		return Img.Rgb;
	};
	const FVec3d Off{ 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc };
	FCoord Far = FCoord::Origin();
	{
		FVec3f F;
		AddDisplacement(Far, F, FVec3d{ 8.5070591730234616e37, 1.0e35, -3.0e37 });   // ~2^126 along x
	}
	const std::vector<float> A = Render(FCoord::Origin(), Off, FVec3f{});
	const std::vector<float> B = Render(Far, Off, FVec3f{});
	double MaxRel = 0.0, Peak = 0.0;
	for (float X : A) { Peak = std::max(Peak, double(X)); }
	for (size_t I = 0; I < A.size(); ++I) { MaxRel = std::max(MaxRel, std::fabs(double(A[I]) - double(B[I])) / Peak); }
	double MaxStep = 0.0;
	std::vector<float> Prev = A;
	for (int K = 1; K <= 3; ++K)
	{
		const std::vector<float> C = Render(FCoord::Origin(), Off, FVec3f{ 0.001f * float(K), 0.0f, 0.0f });
		for (size_t I = 0; I < C.size(); ++I) { MaxStep = std::max(MaxStep, std::fabs(double(C[I]) - double(Prev[I])) / Peak); }
		Prev = C;
	}
	Uni::Check(3, "T3.vol.precision", "Volume translation invariance: the same scene at the origin and ~2^126 cm away renders identically (< 1e-6 of peak); 0.001 cm camera steps change < 1e-4 of peak")
		.Metric("far_vs_origin", MaxRel).Metric("step_change", MaxStep)
		.Pass(MaxRel < 1e-6 && MaxStep < 1e-4,
			Uni::Fmt("origin vs far: max difference %.2e of peak; 0.001 cm steps: max change %.2e of peak", MaxRel, MaxStep));
}

UNI_TEST(volume, background)
{
	// The extragalactic background from intergalactic space: finite with horizon dimming (D-131),
	// and converged: marching 2 periods instead of the default 0.5 before the analytic tail
	// changes the all-sky luminosity by < 2%. Reports the mean surface brightness.
	FUniverseModel M(UniScenes::WebToGalaxy());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const FCoord Cam = UniScenes::VoidCamera();
	FRefPipeline P(G);
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam;
	const H::FUniView V = Cm.View();
	FVolumeSettings S2, S4; S4.BackgroundPeriods = 2.0;
	size_t Tr = 0, Tr4 = 0; u64 Steps = 0, Steps4 = 0;
	const int N = 24;
	const double F2 = SphereVolumeFlux(VB.Build(P, Cam, FVec3f{}, V, S2), Cam, N, &Tr, &Steps);
	const double F4 = SphereVolumeFlux(VB.Build(P, Cam, FVec3f{}, V, S4), Cam, N, &Tr4, &Steps4);
	// Mean radiance in flux units per sr -> mag / arcsec^2 (exposure units carry 10^(0.4 m_exp)).
	const double B = F2 / (4.0 * 3.14159265358979) * std::pow(10.0, -0.4 * double(V.ExposureMag));
	const double Arcsec2 = std::pow(3.14159265358979 / 180.0 / 3600.0, 2.0);
	const double Mu = -2.5 * std::log10(B * Arcsec2);
	const double Rel = (F4 - F2) / F2;
	Uni::Check(3, "T3.vol.background", "Extragalactic background (D-131): finite, converged (0.5 vs 2 marched periods within 2%), no truncation; mean surface brightness reported")
		.Metric("mu_mag_arcsec2", Mu).Metric("rel_change", Rel).Metric("truncated", double(Tr)).Metric("mean_steps", double(Steps) / double(6 * N * N))
		.Pass(std::isfinite(Mu) && std::fabs(Rel) < 0.02 && Tr == 0,
			Uni::Fmt("mean %.2f mag/arcsec^2 (mu_ref %.0f); 0.5 -> 2 periods: %+.2f%%; default (0.5 periods): %.0f steps per ray, %zu truncated (2 periods: %.0f steps, %zu truncated at the 512 budget)", Mu, double(H::UNI_MU_REF), Rel * 100.0, double(Steps) / double(6 * N * N), Tr, double(Steps4) / double(6 * N * N), Tr4));
}

namespace
{
	void Gain(FImage& Img, float G) { for (float& V : Img.Rgb) { V *= G; } }

	/** Display-only auto exposure for inspection stills: the 99.5th-percentile pixel luminance
	 *  maps to 1.5 (about 78% after the 1 - exp(-I) tonemap). Returns the gain applied. */
	float AutoGain(FImage& Img)
	{
		std::vector<float> L;
		for (size_t I = 0; I + 2 < Img.Rgb.size(); I += 3) { L.push_back(0.2126f * Img.Rgb[I] + 0.7152f * Img.Rgb[I + 1] + 0.0722f * Img.Rgb[I + 2]); }
		std::sort(L.begin(), L.end());
		const float P = L[size_t(double(L.size() - 1) * 0.995)];
		const float G = P > 0.0f ? 1.5f / P : 1.0f;
		Gain(Img, G);
		return G;
	}

	std::vector<std::string> VolHud(const std::string& Title, const FFrameStats& St, const FVolumeStats& Vs, const FVolumeScene& Sc, float G)
	{
		return {
			Title,
			Uni::Fmt("sprites %zu visible of %zu resident  carriers %zu", St.Visible, St.Resident, Sc.Carriers.size()),
			Uni::Fmt("volume: %.0f steps/ray (max %u), %zu truncated, %zu px supersampled", Vs.Rays ? double(Vs.Steps) / double(Vs.Rays) : 0.0, Vs.MaxSteps, Vs.Truncated, Vs.Supersampled),
			Uni::Fmt("light: volume %.0f%%, sprites %.0f%%   display gain x%.3g (auto)", 100.0 * Lum(Vs.Flux) / std::max(Lum(Vs.Flux) + Lum(Vs.SpriteFlux), 1e-30), 100.0 * Lum(Vs.SpriteFlux) / std::max(Lum(Vs.Flux) + Lum(Vs.SpriteFlux), 1e-30), double(G)),
		};
	}
}

UNI_TEST(volume, frames)
{
	// Reference stills with the volume (dust on): the anchored galaxy face-on from 60 kpc and
	// edge-on from 30 kpc, and from the solar camera towards the centre and out of the plane.
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FShot { const char* Name; bool bSolar; FV3 Offset; FV3 Look; float Gain; };
	const double Kpc = Units::Kpc;
	const FShot Shots[] = {
		{ "still_vol_galaxy_faceon_60kpc", false, { 6 * Kpc, -4 * Kpc, 60 * Kpc }, { -0.1, 0.066, -1 }, 0.0f },
		{ "still_vol_galaxy_edgeon_30kpc", false, { 30 * Kpc, 0, 0.5 * Kpc }, { -1, 0, -0.0167 }, 0.0f },
		{ "still_vol_interior_towards_centre", true, { 0, 0, 0 }, { -1, -0.12, -0.02 }, 0.0f },
		{ "still_vol_interior_out_of_plane", true, { 0, 0, 0 }, { -0.3, 0.1, 1 }, 0.0f },
	};
	std::string Detail;
	size_t Trunc = 0;
	int Written = 0;
	for (const FShot& S : Shots)
	{
		FRefPipeline P(G);
		const FCoord Cam = S.bSolar ? UniScenes::SolarCamera() : Moved(FCoord::Origin(), S.Offset);
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 480; Cm.Height = 270; Cm.LookAlong(S.Look);
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View());
		FImage Img(Cm.Width, Cm.Height);
		FVolumeStats Vs;
		const FFrameStats St = RenderFrameWithVolume(P, Sc, Cm, Img, &Vs);
		Trunc += Vs.Truncated;
		const float Gn = S.Gain > 0.0f ? (Gain(Img, S.Gain), S.Gain) : AutoGain(Img);
		WriteVolFrame(Img, S.Name, VolHud(S.Name + 6, St, Vs, Sc, Gn));
		Detail += Uni::Fmt("%s: %.0f steps/ray, volume %.0f%% of the light; ", S.Name + 10, double(Vs.Steps) / double(std::max<size_t>(Vs.Rays, 1)), 100.0 * Lum(Vs.Flux) / std::max(Lum(Vs.Flux) + Lum(Vs.SpriteFlux), 1e-30));
		++Written;
	}
	Uni::Check(3, "T3.vol.frames", "Reference stills with the volume (galaxy face-on 60 kpc / edge-on 30 kpc, interior x2), no truncation")
		.Metric("frames", Written).Metric("truncated", double(Trunc))
		.Pass(Written == 4 && Trunc == 0, Detail);
}

UNI_TEST(volume, flythrough)
{
	// The T3.flythrough path (3 Gpc -> 1 m above the brightest star within 100 pc of the solar
	// camera) with the volume on, for the contact sheet; budget-consistent model (D-133) so the
	// sheet shows the intended handoff. Velocity LOD suspends the star levels for the first part
	// of the dive: with the volume the galaxy stays visible there as glow (D-128).
	FUniverseModel M(BudgetConsistent(UniScenes::Flythrough()));
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FCoord Target; double TargetR = 7e10; double BestMag = 99;
	{
		FRefPipeline P0(G);
		P0.Update(UniScenes::SolarCamera(), FVec3f{});
		P0.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float, u64)
		{
			if ((O.A[3] & 0xFFFFu) != UniScenes::kStar) { return; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const FCoord C = ObjectCoord(K, Off);
			const double D = Length(ToDouble(Sub(C, UniScenes::SolarCamera())));
			const float Mag = H::Uni_ObjectAbsMag(UniCore::ToHlsl(O));
			if (D < 100.0 * Units::Pc && Mag < BestMag) { BestMag = Mag; Target = C; TargetR = std::exp2(double(H::Uni_ObjectRadiusLog2(UniCore::ToHlsl(O)))); }
		});
	}
	FRefPipeline P(G);
	const int Frames = 2400, Shots = 48;
	const FV3 U = Norm({ 0.35, 0.25, 0.9 });
	const double S0 = 1.0e28, S1 = 100.0;
	FCamera Cam; Cam.Width = 320; Cam.Height = 180;
	int Shot = 0, GlowOnly = 0;
	size_t Trunc = 0;
	u64 Steps = 0, Rays = 0;
	std::string Notes;
	for (int F = 0; F < Frames; ++F)
	{
		const double T = double(F) / double(Frames - 1);
		const double S = S0 * std::pow(S1 / S0, T);
		Cam.Coord = Moved(Target, U * (TargetR + S));
		Cam.LookAlong(U * -1.0, { 0, 0, 1 });
		P.Update(Cam.Coord, FVec3f{});
		const bool bShot = Shot < Shots && F >= int(std::lround(double(Shot) * double(Frames - 1) / double(Shots - 1)));
		if (!bShot) { continue; }
		const FVolumeScene Sc = VB.Build(P, Cam.Coord, FVec3f{}, Cam.View());
		FImage Img(Cam.Width, Cam.Height);
		FVolumeStats Vs;
		const FFrameStats St = RenderFrameWithVolume(P, Sc, Cam, Img, &Vs);
		Trunc += Vs.Truncated; Steps += Vs.Steps; Rays += Vs.Rays;
		if (Vs.Truncated) { Notes += Uni::Fmt("{shot %02d %s: %zu truncated, max %u steps} ", Shot, FormatDistance(S).c_str(), Vs.Truncated, Vs.MaxSteps); }
		const double Lv = Lum(Vs.Flux), Ls = Lum(Vs.SpriteFlux);
		if (Lv > 0.0 && Ls < 0.01 * Lv) { ++GlowOnly; }
		std::vector<std::string> Hud = { Uni::Fmt("%02d  %s", Shot, FormatDistance(S).c_str()), Uni::Fmt("vis %zu  vol %.0f%%", St.Visible, 100.0 * Lv / std::max(Lv + Ls, 1e-30)) };
		WriteVolFrame(Img, Uni::Fmt("volfly_%02d", Shot), Hud);
		if (Shot % 8 == 0 || Shot == Shots - 1) { Notes += Uni::Fmt("[%02d %s: volume %.0f%%] ", Shot, FormatDistance(S).c_str(), 100.0 * Lv / std::max(Lv + Ls, 1e-30)); }
		++Shot;
	}
	Uni::Check(3, "T3.vol.flythrough", "Flythrough with the volume (budget-consistent model): 48-frame contact sheet, no truncation; shots where the galaxy shows only as glow are counted")
		.Metric("shots", Shot).Metric("truncated", double(Trunc)).Metric("glow_only_shots", GlowOnly).Metric("mean_steps", Rays ? double(Steps) / double(Rays) : 0.0)
		.Pass(Shot == Shots && Trunc == 0,
			Uni::Fmt("%d shots, %zu truncated rays, %.0f steps per ray on average; %d shots carry < 1%% of their light in sprites. %s", Shot, Trunc, Rays ? double(Steps) / double(Rays) : 0.0, GlowOnly, Notes.c_str()));
}

UNI_TEST(volume, cube)
{
	// D-136: the background marched once into a 6 x 128^2 direction map and composited per pixel
	// as B_near + T_near x B_cube(dir), against the union march of everything. Cases: inside
	// the dusty galaxy inside the web (towards the centre and out of the plane), and
	// intergalactic space. Per-pixel and total luminosity errors.
	struct FCase { const char* Name; bool bWeb; FV3 Look; };
	const FCase Cases[] = { { "galaxy+web, towards centre", false, { -1, -0.12, -0.02 } }, { "galaxy+web, out of plane", false, { -0.3, 0.1, 1 } }, { "void, web only", true, { 1, 0.3, 0.1 } } };
	double WorstTotal = 0.0, WorstP99 = 0.0;
	size_t Trunc = 0;
	u64 CubeSteps = 0; size_t CubeTexels = 0;
	std::string Detail;
	for (const FCase& C : Cases)
	{
		FUniverseModel M(C.bWeb ? UniScenes::WebToGalaxy() : UniScenes::Flythrough());
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		const FCoord Cam = C.bWeb ? UniScenes::VoidCamera() : UniScenes::SolarCamera();
		FRefPipeline P(G);
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 160; Cm.Height = 90; Cm.LookAlong(C.Look);
		const FVolumeScene All = VB.Build(P, Cam, FVec3f{}, Cm.View());
		const FBackgroundCube Cube = RenderBackgroundCube(All.Subset(true), 128);
		CubeSteps += Cube.Steps; CubeTexels += Cube.Tex.size(); Trunc += Cube.Truncated;
		FImage A(Cm.Width, Cm.Height), B(Cm.Width, Cm.Height);
		const FVolumeStats Sa = RenderVolume(All, Cm, A);
		const FVolumeStats Sb = RenderVolume(All.Subset(false), Cm, B, nullptr, 2, &Cube);
		Trunc += Sa.Truncated + Sb.Truncated;
		std::vector<double> Errs;
		for (size_t I = 0; I + 2 < A.Rgb.size(); I += 3)
		{
			const double La = 0.2126 * A.Rgb[I] + 0.7152 * A.Rgb[I + 1] + 0.0722 * A.Rgb[I + 2];
			const double Lb2 = 0.2126 * B.Rgb[I] + 0.7152 * B.Rgb[I + 1] + 0.0722 * B.Rgb[I + 2];
			if (La > 0.0) { Errs.push_back(std::fabs(Lb2 - La) / La); }
		}
		std::sort(Errs.begin(), Errs.end());
		const double P99 = Errs.empty() ? 0.0 : Errs[size_t(double(Errs.size() - 1) * 0.99)];
		const double Tot = std::fabs(Lum(Sb.Flux) - Lum(Sa.Flux)) / Lum(Sa.Flux);
		WorstTotal = std::max(WorstTotal, Tot); WorstP99 = std::max(WorstP99, P99);
		Detail += Uni::Fmt("%s: total %+.3f%%, p99 pixel %.2f%%; ", C.Name, 100.0 * (Lum(Sb.Flux) - Lum(Sa.Flux)) / Lum(Sa.Flux), 100.0 * P99);
	}
	Uni::Check(3, "T3.vol.cube", "Background cube + separable composite (D-136) vs the union march: total luminosity within 0.5%, 99th-percentile pixel within 3%")
		.Metric("worst_total", WorstTotal).Metric("worst_p99", WorstP99).Metric("cube_steps_per_texel", CubeTexels ? double(CubeSteps) / double(CubeTexels) : 0.0).Metric("truncated", double(Trunc))
		.Pass(WorstTotal < 0.005 && WorstP99 < 0.03 && Trunc == 0,
			Uni::Fmt("%s cube: %.0f steps per texel, %zu truncated", Detail.c_str(), CubeTexels ? double(CubeSteps) / double(CubeTexels) : 0.0, Trunc));
}
