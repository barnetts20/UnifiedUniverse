// D-212: the galaxy map (UniGalaxyMap.h; Uni_GalaxyMapTexel / Uni_GalaxyMapEval in UniFields.ush):
// the camera's own galaxy marched from its baked 2D map against its field. (D-211's clipmap, which
// this file first tested, was removed in D-213.) D-255: T2.vol.galaxy_map_stepfoot, the map read over
// the march's step along the disc (uu.Volume.StepFoot). D-262: T2.vol.galaxy_segment. D-265:
// T2.vol.galaxy_detail_layers, the camera's galaxy's detail layers and their step cap.
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
	// against its field, in the six views of T2.vol.galaxy_cache, at 512^2 (the default, D-229),
	// 1024^2 and 2048^2.
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
	double WorstLight[3] = { 0, 0, 0 }, WorstP95[3] = { 0, 0, 0 }, BakeMs[3] = { 0, 0, 0 };
	const u32 Sizes[3] = { 512u, 1024u, 2048u };
	FGalaxyMap Maps[3];
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
		if (!Home) { Detail += Uni::Fmt("%s%s: NO HOME", Detail.empty() ? "" : "; ", Vw.Name); WorstLight[0] = WorstLight[1] = WorstLight[2] = 1.0; continue; }
		if (!bBaked)
		{
			for (int K = 0; K < 3; ++K)
			{
				const auto T0 = std::chrono::steady_clock::now();
				Maps[K].Bake(*Home, Sa.Params, Sizes[K], 2);
				BakeMs[K] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
			}
			bBaked = true;
		}
		Detail += Uni::Fmt("%s%s: field %.0f ms", Detail.empty() ? "" : "; ", Vw.Name, A.Ms);
		for (int K = 0; K < 3; ++K)
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
	Uni::Check(2, "T2.vol.galaxy_map", "D-212, D-229 the camera's own galaxy marched from its 2D map against its field, six views inside the home galaxy: at 512^2 (the default) light within 0.5% and per-pixel p95 under 2.5% (it is coarsest right at the core); at 2048^2 within 0.2% and 1% (the map's bilinear filter and half floats, and the mips in place of the arms' footprint blend); 1024^2 reported")
		.Metric("light_512", WorstLight[0]).Metric("p95_512", WorstP95[0]).Metric("light_1024", WorstLight[1]).Metric("p95_1024", WorstP95[1]).Metric("light_2048", WorstLight[2]).Metric("p95_2048", WorstP95[2])
		.Metric("bake_ms_512", BakeMs[0]).Metric("bake_ms_1024", BakeMs[1]).Metric("bake_ms_2048", BakeMs[2]).Metric("bytes_512", double(Maps[0].Bytes()))
		.Pass(WorstLight[0] < 0.005 && WorstP95[0] < 0.025 && WorstLight[2] < 0.002 && WorstP95[2] < 0.01,
			Detail + Uni::Fmt("; bake (2 threads) %.0f / %.0f / %.0f ms, %.1f / %.1f / %.1f MB with mips", BakeMs[0], BakeMs[1], BakeMs[2], double(Maps[0].Bytes()) / 1048576.0, double(Maps[1].Bytes()) / 1048576.0, double(Maps[2].Bytes()) / 1048576.0));
}

UNI_TEST(volume, galaxy_map_nebulae)
{
	// D-234: the nebulae from the camera's own galaxy's map (Uni_GalaxyMapEvalN: the map's arm
	// part, the field's knots and gas layer) against its field, with the home galaxy's knots and
	// cloud dust on (CacheScene turns them off), three views at 512^2: the nebulae's added light
	// and the frames.
	const FCacheTex& Tx = GalTex();
	if (!Tx.bOk) { Uni::Check(2, "T2.vol.galaxy_map_nebulae", "D-234 nebulae from the galaxy map").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FGalBindAll Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	FUniverseModel M(D);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FView { const char* Name; FVec3d Pos; FV3 Look; };
	const FView Views[] = {
		{ "solar toward the centre", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { -1.0, 0.05, 0.02 } },
		{ "solar along the disc", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { 0.05, 1.0, 0.02 } },
		{ "3 kpc above the disc", { 5.0 * Units::Kpc, 2.0 * Units::Kpc, 3.0 * Units::Kpc }, { -0.6, -0.2, -0.5 } },
	};
	FVolumeSettings S;
	S.NebulaGain = 1.0f; S.NebulaReflect = 6.0f;
	std::string Detail;
	double WorstLight = 0.0, WorstP95 = 0.0, WorstAdded = 0.0;
	FGalaxyMap Map;
	bool bBaked = false;
	for (const FView& Vw : Views)
	{
		const FCoord Cam = CamAt(Vw.Pos);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 128; Cm.Height = 72; Cm.LookAlong(Vw.Look);
		const FVolumeScene On = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(false);
		const FVolumeScene Off = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
		const FCarrierGPU* Home = HomeCarrierOf(On);
		if (!Home) { Detail += Uni::Fmt("%s%s: NO HOME", Detail.empty() ? "" : "; ", Vw.Name); WorstLight = 1.0; continue; }
		if (!bBaked) { Map.Bake(*Home, On.Params, 512u, 2); bBaked = true; }
		const FRun Fa = March(On, Cm), Fo = March(Off, Cm);
		FVolumeScene Ma = On, Mo = Off;
		PublishGalaxyMap(Ma.View, Ma.Home.Carrier, 512u);
		PublishGalaxyMap(Mo.View, Mo.Home.Carrier, 512u);
		BindGalaxyMap(0u, &Map);
		const FRun Ra = March(Ma, Cm), Ro = March(Mo, Cm);
		BindGalaxyMap(0u, nullptr);
		double Le = 0.0, P95 = 0.0;
		Compare(Ra, Fa, Le, P95);
		double Sfa = 0, Sfo = 0, Sra = 0, Sro = 0;
		for (size_t I = 0; I < Fa.Lum.size(); ++I) { Sfa += Fa.Lum[I]; Sfo += Fo.Lum[I]; Sra += Ra.Lum[I]; Sro += Ro.Lum[I]; }
		const double AddF = Sfa - Sfo, AddM = Sra - Sro;
		const double AddErr = std::abs(AddM / std::max(AddF, 1e-300) - 1.0);
		WorstLight = std::max(WorstLight, std::abs(Le));
		WorstP95 = std::max(WorstP95, P95);
		WorstAdded = std::max(WorstAdded, AddErr);
		Detail += Uni::Fmt("%s%s: nebulae +%.1f%% (field), map light %+.2f%% p95 %.2f%%, added light %+.1f%%", Detail.empty() ? "" : "; ", Vw.Name, 100.0 * AddF / std::max(Sfo, 1e-300), 100.0 * Le, 100.0 * P95, 100.0 * (AddM / std::max(AddF, 1e-300) - 1.0));
	}
	Uni::Check(2, "T2.vol.galaxy_map_nebulae", "D-234 the camera's own galaxy with nebulae, knots and cloud dust, from its 512^2 map against its field, three views: the frame's light within 1% and per-pixel p95 under 5%, the nebulae's added light within 5%")
		.Metric("light", WorstLight).Metric("p95", WorstP95).Metric("added", WorstAdded)
		.Pass(WorstLight < 0.01 && WorstP95 < 0.05 && WorstAdded < 0.05, Detail);
}

UNI_TEST(volume, galaxy_map_stepfoot)
{
	// D-255 (the cone rule; uu.Volume.StepFoot): the camera's galaxy's map and texture terms read
	// over at least StepFoot x the step's run along the disc (the height profiles keep the pixel's
	// footprint). From the solar circle along and across the disc, jittered marches at 8 phases
	// (the temporal path's samples): the spread over the phases (the grain the accumulation sees)
	// lower at 0.5 than at 0, the frame's light within 5%; 0 is the old read.
	const FCacheTex& Tx = GalTex();
	if (!Tx.bOk) { Uni::Check(2, "T2.vol.galaxy_map_stepfoot", "D-255 starlight over the step").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FGalBindAll Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	FUniverseModel M(D);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FView { const char* Name; FVec3d Pos; FV3 Look; };
	const FView Views[] = {
		{ "solar toward the centre", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { -1.0, 0.05, 0.02 } },
		{ "solar along the disc", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { 0.05, 1.0, 0.02 } },
	};
	std::string Detail;
	bool bPass = true;
	double WorstLight = 0.0, WorstSpread = 0.0;
	FGalaxyMap Map;
	bool bBaked = false;
	for (const FView& Vw : Views)
	{
		const FCoord Cam = CamAt(Vw.Pos);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 96; Cm.Height = 54; Cm.LookAlong(Vw.Look);
		double Mean[2] = { 0.0, 0.0 }, Spread[2] = { 0.0, 0.0 };
		for (int K = 0; K < 2; ++K)
		{
			FVolumeSettings S;
			S.StepFoot = K == 0 ? 0.0f : 0.5f;
			FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(false);
			const FCarrierGPU* Home = HomeCarrierOf(Sc);
			if (!Home) { bPass = false; Detail += Uni::Fmt("%s%s: NO HOME", Detail.empty() ? "" : "; ", Vw.Name); break; }
			if (!bBaked) { Map.Bake(*Home, Sc.Params, 512u, 2); bBaked = true; }
			PublishGalaxyMap(Sc.View, Sc.Home.Carrier, 512u);
			Sc.View.MarchFlags |= H::UNI_VOL_JITTER_FLAG;
			BindGalaxyMap(0u, &Map);
			const int Np = 8;
			std::vector<double> S1(size_t(Cm.Width) * Cm.Height, 0.0), S2 = S1;
			for (int Ph = 0; Ph < Np; ++Ph)
			{
				Sc.View.JitterShift = float(Ph) * 0.61803398875f;
				const FRun R = March(Sc, Cm);
				for (size_t I = 0; I < R.Lum.size(); ++I) { S1[I] += R.Lum[I]; S2[I] += R.Lum[I] * R.Lum[I]; }
			}
			BindGalaxyMap(0u, nullptr);
			double Sm = 0.0, Ss = 0.0;
			for (size_t I = 0; I < S1.size(); ++I)
			{
				const double Mu = S1[I] / Np;
				Sm += Mu;
				Ss += std::sqrt(std::max(S2[I] / Np - Mu * Mu, 0.0));
			}
			Mean[K] = Sm / double(S1.size());
			Spread[K] = Ss / std::max(Sm, 1e-300);
		}
		const double Light = Mean[1] / std::max(Mean[0], 1e-300) - 1.0;
		const double SpreadX = Spread[1] / std::max(Spread[0], 1e-300);
		WorstLight = std::max(WorstLight, std::abs(Light));
		WorstSpread = std::max(WorstSpread, SpreadX);
		bPass = bPass && std::abs(Light) <= 0.05 && SpreadX < 1.0;
		Detail += Uni::Fmt("%s%s: spread over the phases %.3f%% -> %.3f%% (x%.2f), light %+.2f%%", Detail.empty() ? "" : "; ", Vw.Name, 100.0 * Spread[0], 100.0 * Spread[1], SpreadX, 100.0 * Light);
	}
	Uni::Check(2, "T2.vol.galaxy_map_stepfoot", "D-255 uu.Volume.StepFoot 0.5: from the solar circle along and across the disc (the 512^2 map, 8 jitter phases) the spread over the phases lower than at 0, the frame's light within 5%")
		.Metric("spread_x", WorstSpread).Metric("light", WorstLight)
		.Pass(bPass, Detail);
}

UNI_TEST(volume, galaxy_segment)
{
	// D-262 (paradigm C; owner 2026-11-15: the knots sharper outside the galaxy than in it,
	// uu.Volume.StepFoot 0 "resolves it"): the camera's galaxy's step read at up to SegmentTaps
	// points along its run (each over the pixel's footprint and StepFoot x its share of the run).
	// From the solar circle along and across the disc and 300 pc above it looking down at 30
	// degrees, jittered marches at 8 phases: per configuration the spread over the phases (the
	// grain the accumulation sees), the 8-phase mean's per-pixel error against a fine reference
	// (an eighth of the steps, unjittered, StepFoot 0, one tap: the sharp field), the frame's light
	// and the cost (taps and time). Held: SegmentTaps 4 with StepFoot 0.5 nearer the reference than
	// D-255 (one tap, 0.5) with a spread no more than 1.5x its and at most StepFoot 0's.
	const FCacheTex& Tx = GalTex();
	if (!Tx.bOk) { Uni::Check(2, "T2.vol.galaxy_segment", "D-262 paradigm C").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FGalBindAll Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	FUniverseModel M(D);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FView { const char* Name; FVec3d Pos; FV3 Look; };
	const FView Views[] = {
		{ "solar toward the centre", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { -1.0, 0.05, 0.02 } },
		{ "solar along the disc", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { 0.05, 1.0, 0.02 } },
		{ "300 pc above, down 30 deg", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 300.0 * Units::Pc }, { -0.85, 0.1, -0.5 } },
	};
	struct FCfg { const char* Name; float Foot; u32 Taps; };
	const FCfg Cfgs[] = { { "foot 0", 0.0f, 1u }, { "foot 0.5 (D-255)", 0.5f, 1u }, { "foot 0.5 taps 4", 0.5f, 4u }, { "foot 0.5 taps 8", 0.5f, 8u }, { "foot 0 taps 8", 0.0f, 8u } };
	constexpr int NC = int(sizeof(Cfgs) / sizeof(Cfgs[0]));
	const char* OnlyEnv = std::getenv("UNI_SEG_VIEWS");   // exploration: fewer views
	const int NV = OnlyEnv ? std::max(1, std::min(3, std::atoi(OnlyEnv))) : 3;
	std::string Detail;
	bool bPass = true;
	double SumErr[NC] = {}, SumSpread[NC] = {}, SumLight[NC] = {}, SumMs[NC] = {};
	FGalaxyMap Map;
	bool bBaked = false;
	for (int Vi = 0; Vi < NV; ++Vi)
	{
		const FView& Vw = Views[Vi];
		const FCoord Cam = CamAt(Vw.Pos);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 96; Cm.Height = 54; Cm.LookAlong(Vw.Look);
		auto Scene = [&](float Foot, u32 Taps) -> FVolumeScene
		{
			FVolumeSettings S;
			S.StepFoot = Foot; S.SegmentTaps = Taps;
			FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(false);
			const FCarrierGPU* Home = HomeCarrierOf(Sc);
			if (Home && !bBaked) { Map.Bake(*Home, Sc.Params, 512u, 2); bBaked = true; }
			if (Home) { PublishGalaxyMap(Sc.View, Sc.Home.Carrier, 512u); }
			return Sc;
		};
		FVolumeScene Rs = Scene(0.0f, 1u);
		BindGalaxyMap(0u, &Map);   // after the bake (the binding takes the map's mips)
		if (!bBaked) { bPass = false; Detail += Uni::Fmt("%s%s: NO HOME", Detail.empty() ? "" : "; ", Vw.Name); BindGalaxyMap(0u, nullptr); continue; }
		Rs.View.MarchFlags |= 2u; Rs.View.MaxSteps = 16384u;
		const FRun Ref = March(Rs, Cm);
		double RefMean = 0.0;
		for (double L : Ref.Lum) { RefMean += L; }
		RefMean /= double(Ref.Lum.size());
		std::string Vd;
		for (int C = 0; C < NC; ++C)
		{
			FVolumeScene Sc = Scene(Cfgs[C].Foot, Cfgs[C].Taps);
			Sc.View.MarchFlags |= H::UNI_VOL_JITTER_FLAG;
			const int Np = 8;
			std::vector<double> S1(Ref.Lum.size(), 0.0), S2 = S1;
			double Ms = 0.0;
			for (int Ph = 0; Ph < Np; ++Ph)
			{
				Sc.View.JitterShift = float(Ph) * 0.61803398875f;
				const FRun R = March(Sc, Cm);
				Ms += R.Ms;
				for (size_t I = 0; I < R.Lum.size(); ++I) { S1[I] += R.Lum[I]; S2[I] += R.Lum[I] * R.Lum[I]; }
			}
			double Sm = 0.0, Ss = 0.0, Err = 0.0;
			for (size_t I = 0; I < S1.size(); ++I)
			{
				const double Mu = S1[I] / Np;
				Sm += Mu;
				Ss += std::sqrt(std::max(S2[I] / Np - Mu * Mu, 0.0));
				Err += std::abs(Mu - Ref.Lum[I]);
			}
			const double Spread = Ss / std::max(Sm, 1e-300), E = Err / std::max(RefMean * double(S1.size()), 1e-300), Light = Sm / std::max(RefMean * double(S1.size()), 1e-300) - 1.0;
			SumErr[C] += E / NV; SumSpread[C] += Spread / NV; SumLight[C] += Light / NV; SumMs[C] += Ms / Np / NV;
			Vd += Uni::Fmt("%s%s err %.2f%% spread %.3f%% light %+.1f%% %.0f ms", Vd.empty() ? "" : ", ", Cfgs[C].Name, 100.0 * E, 100.0 * Spread, 100.0 * Light, Ms / Np);
		}
		BindGalaxyMap(0u, nullptr);
		Detail += Uni::Fmt("%s%s: %s", Detail.empty() ? "" : "; ", Vw.Name, Vd.c_str());
	}
	std::string Sum;
	for (int C = 0; C < NC; ++C) { Sum += Uni::Fmt("%s%s err %.2f%% spread %.3f%% light %+.1f%% %.0f ms", Sum.empty() ? "" : ", ", Cfgs[C].Name, 100.0 * SumErr[C], 100.0 * SumSpread[C], 100.0 * SumLight[C], SumMs[C]); }
	std::printf("    galaxy_segment mean: %s\n    %s\n", Sum.c_str(), Detail.c_str());
	bPass = bPass && SumErr[2] < SumErr[1] && SumSpread[2] <= 1.5 * SumSpread[1] && SumSpread[2] <= SumSpread[0];
	Uni::Check(2, "T2.vol.galaxy_segment", "D-262 paradigm C: the camera's galaxy's step read at up to SegmentTaps points along its run; from the solar circle along and across the disc and from 300 pc above, 8 jitter phases against a fine reference (an eighth of the steps, unjittered, StepFoot 0): SegmentTaps 4 at StepFoot 0.5 nearer the reference than D-255's one tap, its spread over the phases within 1.5x D-255's and no more than StepFoot 0's")
		.Metric("err_d255", SumErr[1]).Metric("err_taps4", SumErr[2]).Metric("err_taps8", SumErr[3]).Metric("spread_d255", SumSpread[1]).Metric("spread_taps4", SumSpread[2]).Metric("spread_foot0", SumSpread[0])
		.Pass(bPass, "mean: " + Sum + "; " + Detail);
}

UNI_TEST(volume, galaxy_detail_layers)
{
	// D-265 (owner 2026-11-16: a layered density march: detail layers near the camera, steps scaled
	// by their frequency): the camera's galaxy with the engine's default layers (x4 and x16 the
	// detail's frequency, light and dust 0.5, 16 steps a layer at 4 of its texels), from the solar
	// circle toward the centre and from 300 pc above looking down: 8 jitter phases against each
	// configuration's own fine reference (an eighth of the steps, unjittered, StepFoot 0, one tap).
	// Held: the layers keep the frame's light within 4% of the layers off (each multiplier's mean is 1
	// at every mip; a frame holds only a few of the x4 layer's 2 kpc features, so its own mean
	// wanders: -1.1% toward the centre, -2.5% from above when set); with the step cap the spread over
	// the phases (grain) no more than the layers read without it (the step's fade alone) and the
	// error against the reference under 2.5% (1.1%, 1.8% when set); the steps a ray at most 2x the
	// layers off (x1.50).
	const FCacheTex& Tx = GalTex();
	if (!Tx.bOk) { Uni::Check(2, "T2.vol.galaxy_detail_layers", "D-265 detail layers").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FGalBindAll Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	FUniverseModel M(D);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FView { const char* Name; FVec3d Pos; FV3 Look; };
	const FView Views[] = {
		{ "toward the centre", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 20.0 * Units::Pc }, { -1.0, 0.05, 0.02 } },
		{ "300 pc above, down 30 deg", { 8.0 * Units::Kpc, 1.0 * Units::Kpc, 300.0 * Units::Pc }, { -0.85, 0.1, -0.5 } },
	};
	struct FCfg { const char* Name; bool bOn; float Steps; };
	const FCfg Cfgs[] = { { "off", false, 0.0f }, { "layers, no step cap", true, 0.0f }, { "layers, 16 steps", true, 16.0f } };
	constexpr int NC = 3, NV = 2;
	double Err[NC] = {}, Spread[NC] = {}, Light[NC] = {}, Steps[NC] = {};
	std::string Detail;
	bool bPass = true;
	FGalaxyMap Map;
	bool bBaked = false;
	for (const FView& Vw : Views)
	{
		const FCoord Cam = CamAt(Vw.Pos);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 96; Cm.Height = 54; Cm.LookAlong(Vw.Look);
		auto Scene = [&](const FCfg& C, bool bRef) -> FVolumeScene
		{
			FVolumeSettings S;
			S.StepFoot = bRef ? 0.0f : 0.5f; S.SegmentTaps = bRef ? 1u : 4u;
			if (C.bOn) { S.DetailFreq0 = 4.0f; S.DetailLight0 = 0.5f; S.DetailDust0 = 0.5f; S.DetailFreq1 = 16.0f; S.DetailLight1 = 0.5f; S.DetailDust1 = 0.5f; }
			S.DetailSteps = C.Steps; S.DetailStepTexels = 4.0f; S.DetailFoot = 0.5f;
			FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(false);
			const FCarrierGPU* Home = HomeCarrierOf(Sc);
			if (Home && !bBaked) { Map.Bake(*Home, Sc.Params, 512u, 2); bBaked = true; }
			if (Home) { PublishGalaxyMap(Sc.View, Sc.Home.Carrier, 512u); }
			if (bRef) { Sc.View.MarchFlags |= 2u; Sc.View.MaxSteps = 65536u; }
			return Sc;
		};
		{ FVolumeScene S0 = Scene(Cfgs[0], true); (void)S0; }
		BindGalaxyMap(0u, &Map);
		if (!bBaked) { bPass = false; Detail += Uni::Fmt("%s%s: NO HOME", Detail.empty() ? "" : "; ", Vw.Name); BindGalaxyMap(0u, nullptr); continue; }
		double OffMean = 0.0;
		std::string Vd;
		for (int C = 0; C < NC; ++C)
		{
			const FRun Ref = March(Scene(Cfgs[C], true), Cm);
			double RefMean = 0.0;
			for (double L : Ref.Lum) { RefMean += L; }
			RefMean /= double(Ref.Lum.size());
			FVolumeScene Sc = Scene(Cfgs[C], false);
			Sc.View.MarchFlags |= H::UNI_VOL_JITTER_FLAG;
			const int Np = 8;
			std::vector<double> S1(Ref.Lum.size(), 0.0), S2 = S1;
			double St = 0.0;
			for (int Ph = 0; Ph < Np; ++Ph)
			{
				Sc.View.JitterShift = float(Ph) * 0.61803398875f;
				const FRun R = March(Sc, Cm);
				St += R.Steps / Np;
				for (size_t I = 0; I < R.Lum.size(); ++I) { S1[I] += R.Lum[I]; S2[I] += R.Lum[I] * R.Lum[I]; }
			}
			double Sm = 0.0, Ss = 0.0, Ea = 0.0;
			for (size_t I = 0; I < S1.size(); ++I)
			{
				const double Mu = S1[I] / Np;
				Sm += Mu;
				Ss += std::sqrt(std::max(S2[I] / Np - Mu * Mu, 0.0));
				Ea += std::abs(Mu - Ref.Lum[I]);
			}
			const double N = double(S1.size());
			if (C == 0) { OffMean = Sm / N; }
			const double E = Ea / std::max(RefMean * N, 1e-300), Sp = Ss / std::max(Sm, 1e-300), Li = Sm / N / std::max(OffMean, 1e-300) - 1.0;
			Err[C] = std::max(Err[C], E); Spread[C] += Sp / NV; Light[C] = std::abs(Li) > std::abs(Light[C]) ? Li : Light[C]; Steps[C] += St / NV;
			Vd += Uni::Fmt("%s%s err %.2f%% spread %.3f%% light %+.2f%% steps %.1f", Vd.empty() ? "" : ", ", Cfgs[C].Name, 100.0 * E, 100.0 * Sp, 100.0 * Li, St);
		}
		BindGalaxyMap(0u, nullptr);
		Detail += Uni::Fmt("%s%s: %s", Detail.empty() ? "" : "; ", Vw.Name, Vd.c_str());
	}
	const double StepsX = Steps[2] / std::max(Steps[0], 1e-9);
	bPass = bPass && std::abs(Light[1]) <= 0.04 && std::abs(Light[2]) <= 0.04 && Spread[2] <= Spread[1] && Err[2] < 0.025 && StepsX <= 2.0;
	Uni::Check(2, "T2.vol.galaxy_detail_layers", "D-265 detail layers (x4, x16; light and dust 0.5; 16 steps a layer at 4 texels): from the solar circle toward the centre and 300 pc above, 8 jitter phases against each configuration's fine reference: the light within 4% of the layers off, with the step cap the grain no more than the layers' without it, the error under 2.5%, the steps at most 2x")
		.Metric("light_on", Light[2]).Metric("spread_off", Spread[0]).Metric("spread_nocap", Spread[1]).Metric("spread_cap", Spread[2]).Metric("err_cap", Err[2]).Metric("steps_x", StepsX)
		.Pass(bPass, Detail + Uni::Fmt("; steps x%.2f", StepsX));
}
