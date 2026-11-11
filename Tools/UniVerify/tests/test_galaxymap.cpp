// D-212: the galaxy map (UniGalaxyMap.h; Uni_GalaxyMapTexel / Uni_GalaxyMapEval in UniFields.ush):
// the camera's own galaxy marched from its baked 2D map against its field. (D-211's clipmap, which
// this file first tested, was removed in D-213.)
#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniGalaxyMap.h"
#include "UniWebTexture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FCacheTex
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FCacheTex()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};
	const FCacheTex& GalTex() { static const FCacheTex S; return S; }

	struct FGalBindAll
	{
		explicit FGalBindAll(const FCacheTex& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FGalBindAll() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	FCoord CamAt(const FVec3d& P) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, P); return C; }

	struct FRun { std::vector<double> Lum; double Steps = 0.0; double Ms = 0.0; };

	FRun March(const FVolumeScene& Sc, const FCamera& Cm)
	{
		FRun R;
		R.Lum.assign(size_t(Cm.Width) * Cm.Height, 0.0);
		const auto V = ToHlsl(Sc.View);
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		const auto T0 = std::chrono::steady_clock::now();
		for (int Y = 0; Y < Cm.Height; ++Y)
		{
			for (int X = 0; X < Cm.Width; ++X)
			{
				bool bS = false;
				const H::FUniVolumeResult Rs = MarchPixel(Cb, Pb, Lb, V, Cm, Cm.Width, Cm.Height, X, Y, 3.0e38f, bS);
				R.Lum[size_t(Y) * Cm.Width + X] = H::Uni_Luminance(Rs.Radiance);
				R.Steps += Rs.Steps;
			}
		}
		R.Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
		R.Steps /= double(Cm.Width * Cm.Height);
		return R;
	}

	void Compare(const FRun& A, const FRun& Ref, double& OutLight, double& OutP95)
	{
		double Sa = 0.0, Sr = 0.0;
		for (size_t I = 0; I < Ref.Lum.size(); ++I) { Sa += A.Lum[I]; Sr += Ref.Lum[I]; }
		OutLight = Sa / std::max(Sr, 1e-300) - 1.0;
		const double Mean = Sr / double(Ref.Lum.size());
		std::vector<double> E;
		for (size_t I = 0; I < Ref.Lum.size(); ++I) { if (Ref.Lum[I] > 0.01 * Mean) { E.push_back(std::abs(A.Lum[I] - Ref.Lum[I]) / Ref.Lum[I]); } }
		std::sort(E.begin(), E.end());
		OutP95 = E.empty() ? 0.0 : E[size_t(0.95 * double(E.size() - 1))];
	}

	FUniverseDef CacheScene(const FCacheTex& Tx)
	{
		FUniverseDef D = UniScenes::Flythrough();
		UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
		UniScenes::UseGalaxyPalettes(D);
		{
			for (FAnchor& A : D.Anchors) { for (FFieldParams& P : A.EmitterParams) { P.V[28] = 0.0f; P.V[29] = 0.0f; } }
			for (FArchetype& A : D.Archetypes) { A.Volume.DustLanes = 0.0f; A.Volume.DustBubbles = 0.0f; }
		}
		return D;
	}
}

UNI_TEST(volume, galaxy_map)
{
	// D-212: the camera's own galaxy marched from its baked 2D map (in-plane light, dust, the
	// modes' disc centre, arm part; the warp, vertical profile, bulge and detail read per sample)
	// against its field, in the six views of T2.vol.galaxy_cache, at 1024^2 and 2048^2.
	const FCacheTex& Tx = GalTex();
	if (!Tx.bOk) { Uni::Check(2, "T2.vol.galaxy_map", "D-212 galaxy map").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FGalBindAll Bind(Tx);
	FUniverseModel M(CacheScene(Tx));
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FView { const char* Name; FVec3d Pos; FV3 Look; };
	const FVec3d Core{ 0.50 * Units::Kpc, 0.15 * Units::Kpc, 0.10 * Units::Kpc };
	const FView Views[] = {
		{ "core toward the centre", Core, { -0.5, -0.15, -0.1 } },
		{ "core along the disc", Core, { -0.15, 0.5, 0.0 } },
		{ "core out of the plane", Core, { 0.2, 0.1, 1.0 } },
		{ "0.13 kpc from the centre", { 0.12 * Units::Kpc, 0.05 * Units::Kpc, 0.02 * Units::Kpc }, { -1.0, -0.4, -0.15 } },
		{ "solar toward the centre", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { -1.0, 0.05, 0.02 } },
		{ "solar along the disc", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { 0.05, 1.0, 0.02 } },
	};
	std::string Detail;
	double WorstLight[2] = { 0, 0 }, WorstP95[2] = { 0, 0 }, BakeMs[2] = { 0, 0 };
	const u32 Sizes[2] = { 1024u, 2048u };
	FGalaxyMap Maps[2];
	bool bBaked = false;
	for (const FView& Vw : Views)
	{
		const FCoord Cam = CamAt(Vw.Pos);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 160; Cm.Height = 90; Cm.LookAlong(Vw.Look);
		FVolumeScene Sa = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
		const FRun A = March(Sa, Cm);
		const FCarrierGPU* Home = HomeCarrierOf(Sa);
		if (!Home) { Detail += Uni::Fmt("%s%s: NO HOME", Detail.empty() ? "" : "; ", Vw.Name); WorstLight[0] = WorstLight[1] = 1.0; continue; }
		if (!bBaked)
		{
			for (int K = 0; K < 2; ++K)
			{
				const auto T0 = std::chrono::steady_clock::now();
				Maps[K].Bake(*Home, Sa.Params, Sizes[K], 2);
				BakeMs[K] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
			}
			bBaked = true;
		}
		Detail += Uni::Fmt("%s%s: field %.0f ms", Detail.empty() ? "" : "; ", Vw.Name, A.Ms);
		for (int K = 0; K < 2; ++K)
		{
			FVolumeScene Sc = Sa;
			PublishGalaxyMap(Sc.View, Sc.Home.Carrier, Sizes[K]);
			BindGalaxyMap(0u, &Maps[K]);
			const FRun Cr = March(Sc, Cm);
			BindGalaxyMap(0u, nullptr);
			double Le = 0.0, P95 = 0.0;
			Compare(Cr, A, Le, P95);
			WorstLight[K] = std::max(WorstLight[K], std::abs(Le));
			WorstP95[K] = std::max(WorstP95[K], P95);
			Detail += Uni::Fmt(", %u^2 light %+.2f%% p95 %.2f%% %.0f ms", Sizes[K], 100.0 * Le, 100.0 * P95, Cr.Ms);
		}
	}
	Uni::Check(2, "T2.vol.galaxy_map", "D-212 the camera's own galaxy marched from its 2D map against its field, six views inside the home galaxy: at 2048^2 light within 0.2% and per-pixel p95 under 1% (the map's bilinear filter and half floats, and the mips in place of the arms' footprint blend); 1024^2 reported")
		.Metric("light_1024", WorstLight[0]).Metric("p95_1024", WorstP95[0]).Metric("light_2048", WorstLight[1]).Metric("p95_2048", WorstP95[1])
		.Metric("bake_ms_1024", BakeMs[0]).Metric("bake_ms_2048", BakeMs[1]).Metric("bytes_2048", double(Maps[1].Bytes()))
		.Pass(WorstLight[1] < 0.002 && WorstP95[1] < 0.01,
			Detail + Uni::Fmt("; bake (2 threads) %.0f / %.0f ms, %.1f / %.1f MB with mips", BakeMs[0], BakeMs[1], double(Maps[0].Bytes()) / 1048576.0, double(Maps[1].Bytes()) / 1048576.0));
}
