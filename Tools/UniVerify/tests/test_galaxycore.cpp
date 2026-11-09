// D-210: a galaxy's core (arms' inner radius, scale-free disc warp) and the march's per-step
// savings (galaxy constants read once a frame; shared terms; the next step's limit computed in
// the sample pass).
#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniWebTexture.h"

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
	struct FTexSet
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FTexSet()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};
	const FTexSet& Textures() { static const FTexSet S; return S; }

	struct FBind
	{
		explicit FBind(const FTexSet& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FBind() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	void WriteStill(const FImage& Img, const std::string& Name, const std::vector<std::string>& Hud)
	{
		const std::string Base = RenderDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb");
		if (!F) { return; }
		std::fprintf(F, "{\"lines\":[");
		for (size_t I = 0; I < Hud.size(); ++I)
		{
			std::string E;
			for (char C : Hud[I]) { if (C == '"' || C == '\\') { E += '\\'; } E += C; }
			std::fprintf(F, "%s\"%s\"", I ? "," : "", E.c_str());
		}
		std::fprintf(F, "]}\n");
		std::fclose(F);
	}

	float GainOf(const FImage& Img)
	{
		std::vector<float> L;
		for (size_t I = 0; I + 2 < Img.Rgb.size(); I += 3) { L.push_back(0.2126f * Img.Rgb[I] + 0.7152f * Img.Rgb[I + 1] + 0.0722f * Img.Rgb[I + 2]); }
		std::sort(L.begin(), L.end());
		const float P = L[size_t(double(L.size() - 1) * 0.995)];
		return P > 0.0f ? 1.5f / P : 1.0f;
	}

	void ApplyGain(FImage& Img, float G) { for (float& V : Img.Rgb) { V *= G; } }

	/** The flythrough scene with the engine's galaxy look (textures, palettes); the Milky Way's
	 *  arms' inner radius set to InInner (D-210 default: the exemplar's). */
	FUniverseDef CoreScene(const FTexSet& Tx, float InInner)
	{
		FUniverseDef D = UniScenes::Flythrough();
		UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
		UniScenes::UseGalaxyPalettes(D);
		if (InInner >= 0.0f) { for (FAnchor& A : D.Anchors) { for (FFieldParams& P : A.EmitterParams) { P.V[11] = InInner; } } }
		return D;
	}

	FCoord CamAt(const FVec3d& P) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, P); return C; }
}

UNI_TEST(galaxy, core)
{
	// D-210 (owner's core screenshot: contour-like swirls around the centre). A log spiral winds
	// without limit toward the centre, so the arms (and the dust's) became tight rings in the
	// bulge, swirled by the disc warp (a fixed displacement, ~225 pc for the home galaxy). With
	// the arms' inner radius (C.w, disc scale lengths; 0.25 for every galaxy) the arms go to their
	// azimuthal mean inside it and the disc warp's displacement grows linearly up to it.
	// 1. Outside the inner radius (plus the warp's reach) the field is the old one bit for bit.
	// 2. Inside, the ring mean of the field is kept (untextured, modes on: the arms alone change).
	// 3. Inside half the radius the arms carry no azimuthal structure.
	// 4. Stills: the home galaxy's core from 0.54 kpc, before (C.w 0) and after.
	const FTexSet& Tx = Textures();
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.core", "D-210 the galaxy core").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FBind Bind(Tx);
	FUniverseDef Dt = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(Dt, Tx.T[0].Size);
	FFieldParams Pn = Dt.Anchors.at(0).EmitterParams.at(0);
	FFieldParams Po = Pn; Po.V[11] = 0.0f;
	const H::FUniFieldParams Hn = Pn.ToHlsl(), Ho = Po.ToHlsl();
	const double Ri = double(Pn.V[11]) * double(Pn.V[0]);   // field units
	const double Reach = 3.0 * double(Pn.V[24]);

	// 1. Outside.
	size_t Outside = 0, OutsideDiffer = 0;
	for (u32 K = 0; K < 20000; ++K)
	{
		const double U0 = double(H::Uni_DrawUnit(0xC0E1u, 0x10u, K)), U1 = double(H::Uni_DrawUnit(0xC0E1u, 0x11u, K)), U2 = double(H::Uni_DrawUnit(0xC0E1u, 0x12u, K));
		const double R = Ri + Reach + (0.6 - Ri - Reach) * U0, Th = 6.283185307 * U1, Z = 0.04 * (2.0 * U2 - 1.0);
		const H::float3 Pt(float(R * std::cos(Th)), float(R * std::sin(Th)), float(Z));
		const float Foot = (K & 1u) ? float(0.01 * U2) : 0.0f;
		const float A = H::UniField_GalaxyFiltered(Pt, Hn, Foot), B = H::UniField_GalaxyFiltered(Pt, Ho, Foot);
		++Outside;
		if (std::memcmp(&A, &B, sizeof(float)) != 0) { ++OutsideDiffer; }
	}

	// 2, 3. Rings inside (untextured: the texture terms would mix in the warp's own change).
	FUniverseDef Du = UniScenes::Flythrough();
	FFieldParams Un = Du.Anchors.at(0).EmitterParams.at(0);
	FFieldParams Uo = Un; Uo.V[11] = 0.0f;
	const H::FUniFieldParams Hun = Un.ToHlsl(), Huo = Uo.ToHlsl();
	const double Riu = double(Un.V[11]) * double(Un.V[0]);
	double WorstRing = 0.0, ArmSpreadNew = 0.0, ArmSpreadOld = 0.0;
	for (double F : { 0.3, 0.45, 0.6, 0.75, 0.9 })
	{
		const double R = F * Riu;
		double Sn = 0.0, So = 0.0, An = 0.0, An2 = 0.0, Ao = 0.0, Ao2 = 0.0;
		const int N = 8192;
		for (int I = 0; I < N; ++I)
		{
			const double Th = 6.283185307 * (I + 0.5) / N;
			const H::float3 Pt(float(R * std::cos(Th)), float(R * std::sin(Th)), 0.0f);
			H::float4 Cn, Co;
			Sn += H::UniField_GalaxyFilteredC(Pt, Hun, 0.0f, Cn);
			So += H::UniField_GalaxyFilteredC(Pt, Huo, 0.0f, Co);
			const double Fn = (Cn.z + Cn.w) / std::max(double(Cn.y), 1e-30), Fo = (Co.z + Co.w) / std::max(double(Co.y), 1e-30);
			An += Fn; An2 += Fn * Fn; Ao += Fo; Ao2 += Fo * Fo;
		}
		WorstRing = std::max(WorstRing, std::abs(Sn / So - 1.0));
		if (F <= 0.45)
		{
			const double Mn = An / N, Mo = Ao / N;
			ArmSpreadNew = std::max(ArmSpreadNew, std::sqrt(std::max(An2 / N - Mn * Mn, 0.0)) / std::max(Mn, 1e-30));
			ArmSpreadOld = std::max(ArmSpreadOld, std::sqrt(std::max(Ao2 / N - Mo * Mo, 0.0)) / std::max(Mo, 1e-30));
		}
	}

	// 4. Stills (sprites and volume) from the owner's spot toward the centre.
	double Steps[2] = { 0, 0 };
	int Written = 0;
	float Gn = 0.0f;
	for (int Cfg = 0; Cfg < 2; ++Cfg)
	{
		FUniverseModel M(CoreScene(Tx, Cfg == 0 ? 0.0f : -1.0f));
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		const FCoord Cam = CamAt({ 0.50 * Units::Kpc, 0.15 * Units::Kpc, 0.10 * Units::Kpc });
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 480; Cm.Height = 270; Cm.LookAlong({ -0.5, -0.15, -0.1 });
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View());
		FImage Img(Cm.Width, Cm.Height);
		FVolumeStats Vs;
		RenderFrameWithVolume(P, Sc, Cm, Img, &Vs);
		Steps[Cfg] = double(Vs.Steps) / double(std::max<size_t>(Vs.Rays, 1));
		if (Cfg == 0) { Gn = GainOf(Img); }
		ApplyGain(Img, Gn);   // the same display gain for both
		WriteStill(Img, Cfg == 0 ? "still_galaxy_core_d209" : "still_galaxy_core_d210",
			{ Cfg == 0 ? "D-209: the home galaxy's core from 0.54 kpc toward the centre (arms to the centre)" : "D-210: the same, arms' inner radius 0.25 disc scale lengths, scale-free disc warp",
			  Uni::Fmt("volume %.1f steps/ray; display gain x%.3g (the D-209 frame's, both)", Steps[Cfg], double(Gn)) });
		++Written;
	}

	Uni::Check(2, "T2.galaxy.core", "D-210 the galaxy core: outside the arms' inner radius (plus the warp's reach) the field is unchanged bit for bit; inside, ring means within 1% of the old field's (untextured) and no arm structure inside half the radius (relative spread of the arms' share under 1%, from over 10%); stills before/after")
		.Metric("outside_differ", double(OutsideDiffer)).Metric("worst_ring", WorstRing).Metric("arm_spread_new", ArmSpreadNew).Metric("arm_spread_old", ArmSpreadOld)
		.Metric("steps_old", Steps[0]).Metric("steps_new", Steps[1])
		.Pass(OutsideDiffer == 0 && WorstRing < 0.01 && ArmSpreadNew < 0.01 && ArmSpreadOld > 0.1 && Written == 2,
			Uni::Fmt("outside: %zu points, %zu differ; ring means within %.3f%%; arms' relative spread inside half the radius %.2f%% (was %.1f%%); core view %.1f -> %.1f steps/ray",
				Outside, OutsideDiffer, 100.0 * WorstRing, 100.0 * ArmSpreadNew, 100.0 * ArmSpreadOld, Steps[0], Steps[1]));
}

UNI_TEST(volume, galaxy_const)
{
	// D-210: the march reads each galaxy's per-object hashes from the builder's constant block
	// (FVolumeSettings::bGalaxyConst, engine uu.VolumeGalaxyConst) instead of hashing ~10 times a
	// step: bit for bit the same light, transmittance and steps in the home galaxy's core and from
	// the solar camera (textured, palettes, dust). The same views pin the rest of the round, each
	// verified bit for bit during development: the shared radial and winding terms, and the next
	// step's limit computed in the sample pass.
	const FTexSet& Tx = Textures();
	if (!Tx.bOk) { Uni::Check(2, "T2.vol.galaxy_const", "D-210 galaxy constants").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FBind Bind(Tx);
	FUniverseModel M(CoreScene(Tx, -1.0f));
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FView { const char* Name; FVec3d Pos; FV3 Look; };
	const FView Views[] = {
		{ "core toward the centre", { 0.50 * Units::Kpc, 0.15 * Units::Kpc, 0.10 * Units::Kpc }, { -0.5, -0.15, -0.1 } },
		{ "core along the disc", { 0.50 * Units::Kpc, 0.15 * Units::Kpc, 0.10 * Units::Kpc }, { -0.15, 0.5, 0.0 } },
		{ "solar toward the centre", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { -1.0, 0.05, 0.02 } },
	};
	size_t Px = 0, Differ = 0;
	u32 Base = 0;
	std::string Detail;
	for (const FView& Vw : Views)
	{
		const FCoord Cam = CamAt(Vw.Pos);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 96; Cm.Height = 54; Cm.LookAlong(Vw.Look);
		FVolumeSettings On, Off;
		Off.bGalaxyConst = false;
		const FVolumeScene Sa = VB.Build(P, Cam, FVec3f{}, Cm.View(), On).Subset(false);
		const FVolumeScene Sb = VB.Build(P, Cam, FVec3f{}, Cm.View(), Off).Subset(false);
		Base = std::max(Base, Sa.View.GalaxyConstBase);
		const auto Va = ToHlsl(Sa.View), Vb = ToHlsl(Sb.View);
		const auto Ca = Sa.CarrierBuf(); const auto Pa = Sa.ParamBuf(); const auto La = Sa.LutBuf();
		const auto Cb = Sb.CarrierBuf(); const auto Pb = Sb.ParamBuf(); const auto Lb = Sb.LutBuf();
		size_t D = 0;
		double St = 0.0;
		for (int Y = 0; Y < Cm.Height; ++Y)
		{
			for (int X = 0; X < Cm.Width; ++X)
			{
				bool bA = false, bB = false;
				const H::FUniVolumeResult A = MarchPixel(Ca, Pa, La, Va, Cm, Cm.Width, Cm.Height, X, Y, 3.0e38f, bA);
				const H::FUniVolumeResult B = MarchPixel(Cb, Pb, Lb, Vb, Cm, Cm.Width, Cm.Height, X, Y, 3.0e38f, bB);
				const bool bSame = std::memcmp(&A.Radiance, &B.Radiance, sizeof(A.Radiance)) == 0 && std::memcmp(&A.Transmittance, &B.Transmittance, sizeof(A.Transmittance)) == 0 && A.Steps == B.Steps;
				D += bSame ? 0u : 1u;
				St += A.Steps;
				++Px;
			}
		}
		Differ += D;
		Detail += Uni::Fmt("%s%s: %zu px differ, %.1f steps/px", Detail.empty() ? "" : "; ", Vw.Name, D, St / double(Cm.Width * Cm.Height));
	}
	Uni::Check(2, "T2.vol.galaxy_const", "D-210 the march with the galaxy constant blocks is bit for bit the march hashing inline (light, transmittance, steps) in the home galaxy's core and from the solar camera, textured with palettes and dust")
		.Metric("pixels", double(Px)).Metric("differ", double(Differ)).Metric("const_base", double(Base))
		.Pass(Differ == 0 && Base > 0u, Detail);
}
