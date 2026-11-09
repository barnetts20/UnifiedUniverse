// test_webtex.cpp
// D-164: the cosmic web as a baked texture (UniWebTexture.h, UniField_WebTex).
//
//   T1.webtex.format      half rounding, bake = channels at texel centres, mips = exact means,
//                         the CPU sampler (texel centres, linear between, wrap, mip blend),
//                         cache round trip, channels regroup the procedural web exactly
//   T2.webtex.field       the stand-in (128^3, 2^3 samples per texel) against the procedural web:
//                         mean, correlation, bake time
//   T2.webtex.census      placement on the textured web: galaxy and cluster densities match their
//                         emitters, clusters on denser web than field galaxies
//   T3.webtex.background  the background map marched through the texture vs the procedural web:
//                         light, per-pixel agreement, steps and time per texel; stills of both

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniStreamer.h"
#include "UniTravel.h"
#include "UniWebTexture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <sys/stat.h>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		for (size_t I = 1; I < D.size(); ++I) { if (D[I] == '/') { ::mkdir(D.substr(0, I).c_str(), 0755); } }
		::mkdir(D.c_str(), 0755);
		return D;
	}

	void WriteFrame(const FImage& Img, const std::string& Name, const std::vector<std::string>& Hud)
	{
		const std::string Base = RenderDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb");
		if (!F) { return; }
		std::fprintf(F, "{\"lines\":[");
		for (size_t I = 0; I < Hud.size(); ++I) { std::fprintf(F, "%s\"%s\"", I ? "," : "", Hud[I].c_str()); }
		std::fprintf(F, "]}\n");
		std::fclose(F);
	}

	double Lum3(const double* Rgb) { return 0.2126 * Rgb[0] + 0.7152 * Rgb[1] + 0.0722 * Rgb[2]; }

	/** The web scene's bake key (its galaxy web), at the stand-in resolution. */
	FWebBakeKey SceneKey(uint32_t Size = 128u, uint32_t SS = 2u)
	{
		const FUniverseDef D = UniScenes::WebToGalaxy();
		FWebBakeKey K;
		FindWebKey(D, Size, SS, K);
		return K;
	}

	/** The stand-in, baked once per run (and its bake time). */
	const FWebTexture& StandIn(double* OutSeconds = nullptr)
	{
		static double Seconds = 0.0;
		static const FWebTexture T = [&]()
		{
			const auto T0 = std::chrono::steady_clock::now();
			FWebTexture R = FWebTexture::Bake(SceneKey());
			Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
			return R;
		}();
		if (OutSeconds) { *OutSeconds = Seconds; }
		return T;
	}

	struct FBindScope
	{
		explicit FBindScope(const FWebTexture& T) { BindWebTexture(&T); }
		~FBindScope() { BindWebTexture(nullptr); }
	};

	H::FUniFieldParams Textured(H::FUniFieldParams P, uint32_t Size) { P.C.z = 1.0f; P.C.w = float(Size); return P; }
}

UNI_TEST(webtex, format)
{
	std::string Bad;
	// Half rounding: within half an ulp (2^-11 relative) over the normal range; exact for halves.
	Uni::FTestRng Rng(0x4A1Full);
	int HalfBad = 0;
	for (int I = 0; I < 200000; ++I)
	{
		const float X = float(std::ldexp(Rng.Unit() * 2.0 - 1.0, int(Rng.Unit() * 30.0) - 14));
		if (std::fabs(X) < std::ldexp(1.0f, -14)) { continue; }   // normals only (denormals: absolute 2^-25)
		const float R = RoundToHalf(X);
		if (std::fabs(R - X) > std::ldexp(std::fabs(double(X)), -11) * 1.0000001 + 1e-30) { ++HalfBad; }
		if (RoundToHalf(R) != R) { ++HalfBad; }
	}
	if (HalfBad) { Bad += Uni::Fmt("half rounding: %d; ", HalfBad); }

	// Small bake, one sample per texel: texel = channels at its centre, half-rounded.
	FWebBakeKey K = SceneKey(32u, 1u);
	const FWebTexture T = FWebTexture::Bake(K, 2u);
	const H::FUniFieldParams Bp = K.BakeParams();
	int TexBad = 0;
	for (int I = 0; I < 2000; ++I)
	{
		const uint32_t X = uint32_t(Rng.Unit() * 32.0) % 32u, Y = uint32_t(Rng.Unit() * 32.0) % 32u, Z = uint32_t(Rng.Unit() * 32.0) % 32u;
		const H::float4 C = H::Uni_WebChannels(H::float3((X + 0.5f) / 32.0f, (Y + 0.5f) / 32.0f, (Z + 0.5f) / 32.0f), Bp);
		const H::float4& V = T.Texel(0, X, Y, Z);
		if (V.x != RoundToHalf(C.x) || V.y != RoundToHalf(C.y) || V.z != RoundToHalf(C.z) || V.w != 0.0f) { ++TexBad; }
	}
	if (TexBad) { Bad += Uni::Fmt("bake texels: %d; ", TexBad); }

	// Mips: every texel the half-rounded mean of its 8 children; the last mip is 1^3.
	int MipBad = 0;
	for (uint32_t M = 1; M < T.NumMips(); ++M)
	{
		const uint32_t S = T.MipSize(M);
		for (uint32_t Z = 0; Z < S; ++Z) for (uint32_t Y = 0; Y < S; ++Y) for (uint32_t X = 0; X < S; ++X)
		{
			double A = 0.0;
			for (int C = 0; C < 8; ++C) { A += T.Texel(M - 1, 2 * X + (C & 1), 2 * Y + ((C >> 1) & 1), 2 * Z + ((C >> 2) & 1)).y; }
			if (T.Texel(M, X, Y, Z).y != RoundToHalf(float(A / 8.0))) { ++MipBad; }
		}
	}
	if (MipBad || T.MipSize(T.NumMips() - 1) != 1u || T.NumMips() != 6u) { Bad += Uni::Fmt("mips: %d bad, %u levels; ", MipBad, T.NumMips()); }

	// Sampler: texel centres exact, midpoints the mean, wrap, linear across mips.
	FBindScope Bind(T);
	int SampBad = 0;
	for (int I = 0; I < 2000; ++I)
	{
		const uint32_t X = uint32_t(Rng.Unit() * 32.0) % 32u, Y = uint32_t(Rng.Unit() * 32.0) % 32u, Z = uint32_t(Rng.Unit() * 32.0) % 32u;
		const H::float3 Pc((X + 0.5f) / 32.0f, (Y + 0.5f) / 32.0f, (Z + 0.5f) / 32.0f);
		const H::float4 S0 = H::UniWebTexSample(Pc, 0.0f);
		if (std::fabs(S0.y - T.Texel(0, X, Y, Z).y) > 1e-6f) { ++SampBad; }
		const H::float4 Sm = H::UniWebTexSample(H::float3(Pc.x + 0.5f / 32.0f, Pc.y, Pc.z), 0.0f);
		const float Want = 0.5f * (T.Texel(0, X, Y, Z).y + T.Texel(0, (X + 1) % 32u, Y, Z).y);
		if (std::fabs(Sm.y - Want) > 1e-5f) { ++SampBad; }
		const H::float3 Pr(float(Rng.Unit()), float(Rng.Unit()), float(Rng.Unit()));
		const H::float4 A = H::UniWebTexSample(Pr, 0.0f), B = H::UniWebTexSample(H::float3(Pr.x + 1.0f, Pr.y - 1.0f, Pr.z + 2.0f), 0.0f);
		if (std::fabs(A.x - B.x) > 1e-4f || std::fabs(A.y - B.y) > 1e-4f) { ++SampBad; }
		const H::float4 M1 = H::UniWebTexSample(Pr, 1.0f), M2 = H::UniWebTexSample(Pr, 2.0f), M15 = H::UniWebTexSample(Pr, 1.5f);
		if (std::fabs(M15.y - 0.5f * (M1.y + M2.y)) > 1e-5f) { ++SampBad; }
	}
	// Across the tile boundary the sample is continuous: no seam.
	double Seam = 0.0, Interior = 0.0;
	for (int I = 0; I < 2000; ++I)
	{
		const float Y = float(Rng.Unit()), Z = float(Rng.Unit()), E = 0.25f / 32.0f;
		Seam = std::max(Seam, double(std::fabs(H::UniWebTexSample(H::float3(1.0f - E, Y, Z), 0.0f).y - H::UniWebTexSample(H::float3(E, Y, Z), 0.0f).y)));
		const float Xi = float(Rng.Unit()) * 0.9f + 0.05f;
		Interior = std::max(Interior, double(std::fabs(H::UniWebTexSample(H::float3(Xi - E, Y, Z), 0.0f).y - H::UniWebTexSample(H::float3(Xi + E, Y, Z), 0.0f).y)));
	}
	if (Seam > Interior * 1.05 + 1e-6) { ++SampBad; }
	if (SampBad) { Bad += Uni::Fmt("sampler: %d; ", SampBad); }

	// Cache round trip.
	const std::string Path = RenderDir() + "/webtex_test.uweb";
	FWebTexture L;
	const bool bRound = T.Save(Path) && FWebTexture::Load(Path, K, L) && L.NumMips() == T.NumMips() && L.Mips[0].size() == T.Mips[0].size()
		&& L.Mips == T.Mips;
	FWebBakeKey K2 = K; K2.Seed ^= 1u;
	FWebTexture L2;
	const bool bRejects = !FWebTexture::Load(Path, K2, L2);
	if (!bRound || !bRejects) { Bad += "cache round trip; "; }

	// The channels regroup the procedural web: galaxy web and cluster web (power, nodes).
	const FUniverseDef D = UniScenes::WebToGalaxy();
	double Worst = 0.0;
	for (const FUniEmitter& E : D.RootEmitters)
	{
		const H::FUniFieldParams P = E.Exemplar.ToHlsl();
		for (int I = 0; I < 5000; ++I)
		{
			const H::float3 Pt(float(Rng.Unit()), float(Rng.Unit()), float(Rng.Unit()));
			const H::float4 C = H::Uni_WebChannels(Pt, P);
			double Dn = double(P.A.w) * C.x + double(P.B.x) * C.y + (P.C.y > 0.0f ? double(P.C.y) * C.z : 0.0);
			if (P.C.x > 0.0f && P.C.x != 1.0f) { Dn = std::pow(std::max(Dn, 0.0), double(P.C.x)); }
			const double Want = H::UniField_Web(Pt, P);
			Worst = std::max(Worst, std::fabs(double(P.A.z) + Dn - Want) / std::max(std::fabs(Want), 1e-3));
		}
	}
	if (Worst > 1e-5) { Bad += Uni::Fmt("channels vs procedural: %.2g; ", Worst); }

	// Unbound, the textured field is NaN (fails closed), never a neutral value.
	BindWebTexture(nullptr);
	const bool bNan = std::isnan(H::UniField_Web(H::float3(0.3f, 0.4f, 0.5f), Textured(D.RootEmitters[0].Exemplar.ToHlsl(), 32u)));
	if (!bNan) { Bad += "unbound texture not NaN; "; }

	Uni::Check(1, "T1.webtex.format", "D-164 web texture: half rounding, bake = channels at texel centres, mips = exact means, sampler (centres, midpoints, wrap, no seam, mip blend), cache round trip, channels = procedural web, unbound fails closed")
		.Metric("channel_err", Worst).Metric("seam", Seam).Metric("interior", Interior)
		.Pass(Bad.empty(), Bad.empty() ? Uni::Fmt("32^3 bake, %u mips; channels match the procedural web to %.1g; seam step %.3g vs interior %.3g", T.NumMips(), Worst, Seam, Interior) : Bad);
}

UNI_TEST(webtex, field)
{
	double Secs = 0.0;
	const FWebTexture& T = StandIn(&Secs);
	FBindScope Bind(T);
	const FUniverseDef D = UniScenes::WebToGalaxy();
	Uni::FTestRng Rng(0x5EEDull);
	std::string Detail;
	bool bOk = true;
	for (size_t E = 0; E < D.RootEmitters.size(); ++E)
	{
		const H::FUniFieldParams Pp = D.RootEmitters[E].Exemplar.ToHlsl();
		const H::FUniFieldParams Pt = Textured(Pp, T.Size);
		double Sa = 0, Sb = 0, Saa = 0, Sbb = 0, Sab = 0;
		const int N = 200000;
		for (int I = 0; I < N; ++I)
		{
			const H::float3 P(float(Rng.Unit()), float(Rng.Unit()), float(Rng.Unit()));
			const double A = H::UniField_Web(P, Pp), B = H::UniField_Web(P, Pt);
			Sa += A; Sb += B; Saa += A * A; Sbb += B * B; Sab += A * B;
		}
		const double Ma = Sa / N, Mb = Sb / N;
		const double Corr = (Sab / N - Ma * Mb) / std::sqrt(std::max((Saa / N - Ma * Ma) * (Sbb / N - Mb * Mb), 1e-300));
		const double MeanErr = Mb / Ma - 1.0;
		const double CorrMin = E == 0 ? 0.9 : 0.8;
		const double MeanTol = E == 0 ? 0.01 : 0.15;   // the cluster web's power acts after filtering
		bOk = bOk && Corr > CorrMin && std::fabs(MeanErr) < MeanTol;
		Detail += Uni::Fmt("%s: mean %+.2f%%, correlation %.3f; ", E == 0 ? "galaxy web" : "cluster web", 100.0 * MeanErr, Corr);
	}
	Uni::Check(2, "T2.webtex.field", "D-164 stand-in web texture (128^3, 8 samples per texel) vs the procedural web at random points: galaxy web mean within 1%, correlation > 0.9; cluster web (power after filtering) mean within 15%, correlation > 0.8")
		.Metric("bake_seconds", Secs).Pass(bOk, Detail + Uni::Fmt("bake %.1f s (%u^3, %u mips)", Secs, T.Size, T.NumMips()));
}

UNI_TEST(webtex, census)
{
	const FWebTexture& T = StandIn();
	FBindScope Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const int Switched = EnableWebTexture(Def, T.Key);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	P.Update(UniScenes::VoidCamera(), FVec3f{});
	const FUniverseDef& D = M.Definition();
	const FUniEmitter& Field = D.RootEmitters[0];
	const FUniEmitter& Clu = D.RootEmitters[1];
	const H::FUniFieldParams Wp = Field.Exemplar.ToHlsl();
	const double Period = std::ldexp(1.0, Field.PeriodBits);
	auto WebAt = [&](const FCoord& C)
	{
		float Pf[3];
		for (int Ax = 0; Ax < 3; ++Ax) { Pf[Ax] = float(U128ToDouble(MaskLow(C.Axis(Ax), Field.PeriodBits)) / Period); }
		return double(H::Uni_FieldSample(H::UNI_FIELD_WEB, H::float3(Pf[0], Pf[1], Pf[2]), Wp));
	};
	const int LClu = M.LevelOf(UniScenes::kCluster, -26.0);
	long long Clusters = 0, FieldGal = 0;
	double WebC = 0.0, WebF = 0.0, ClusterVolume = 0.0;
	std::map<int, std::pair<long long, double>> GalByLevel;   // galaxies placed by the web, per level: count, volume
	for (const auto& KV : P.CacheRef())
	{
		long long WebGal = 0;
		for (size_t I = 0; I < KV.second.Objects.size(); ++I)
		{
			const FObjectGPU& O = KV.second.Objects[I];
			if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
			const u32 Arch = O.A[3] & 0xFFFFu;
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			if (Arch == UniScenes::kCluster) { ++Clusters; WebC += WebAt(ObjectCoord(KV.first, Off)); }
			else if (Arch == UniScenes::kGalaxy)
			{
				const auto It = P.Contributors().find(KV.second.ObjectContributor[I]);
				const bool bMember = It != P.Contributors().end() && It->second.ParentArchetypeId == UniScenes::kCluster;
				if (!bMember)
				{
					++WebGal;
					if (FieldGal < 200000) { ++FieldGal; WebF += WebAt(ObjectCoord(KV.first, Off)); }
				}
			}
		}
		const double W = CellWidthCm(KV.first.Level) / Units::Mpc;
		if (KV.first.Level == LClu) { ClusterVolume += W * W * W; }
		GalByLevel[KV.first.Level].first += WebGal;
		GalByLevel[KV.first.Level].second += W * W * W;
	}
	Uni::FTestRng Rng(0xBEEFull);
	double WebR = 0.0;
	const int NR = 20000;
	for (int I = 0; I < NR; ++I) { WebR += double(H::Uni_FieldSample(H::UNI_FIELD_WEB, H::float3(float(Rng.Unit()), float(Rng.Unit()), float(Rng.Unit())), Wp)); }
	WebR /= NR;
	const double Density = ClusterVolume > 0.0 ? double(Clusters) / ClusterVolume : 0.0;
	const double Want = Clu.NTotal * std::pow(Units::Mpc, 3.0);
	const double Sig = std::sqrt(std::max(double(Clusters), 1.0)) / std::max(ClusterVolume, 1e-300);
	const double BiasC = Clusters ? WebC / double(Clusters) / WebR : 0.0;
	const double BiasF = FieldGal ? WebF / double(FieldGal) / WebR : 0.0;
	// Field galaxies: the slice with the most galaxies, against its expected density.
	int LBest = 0; long long NBest = 0; double VBest = 0.0;
	for (const auto& KV : GalByLevel) { if (KV.second.first > NBest) { NBest = KV.second.first; VBest = KV.second.second; LBest = KV.first; } }
	const FSlice* Sl = M.SlicesOf(UniScenes::kGalaxy).ForLevel(LBest);
	const double WantGal = Sl ? Field.NTotal * Sl->P * std::pow(Units::Mpc, 3.0) : 0.0;
	const double GotGal = VBest > 0.0 ? double(NBest) / VBest : 0.0;
	const double SigGal = std::sqrt(std::max(double(NBest), 1.0)) / std::max(VBest, 1e-300);
	const bool bGal = Sl && std::abs(GotGal - WantGal) < 4.0 * SigGal + 0.02 * WantGal;
	Uni::Check(2, "T2.webtex.census", "D-164 placement on the textured web (web scene with clusters): cluster density matches the emitter (4 sigma + 2%), the busiest galaxy slice matches its share, clusters on denser web than field galaxies")
		.Metric("switched", Switched).Metric("clusters", double(Clusters)).Metric("density", Density).Metric("density_expected", Want).Metric("bias_clusters", BiasC).Metric("bias_field", BiasF)
		.Metric("galaxies", double(NBest)).Metric("galaxy_density", GotGal).Metric("galaxy_density_expected", WantGal)
		.Pass(Switched == 2 && Clusters > 1000 && std::abs(Density - Want) < 4.0 * Sig + 0.02 * Want && BiasC > 1.5 * BiasF && bGal,
			Uni::Fmt("%d web emitters textured; %lld clusters: %.3g per Mpc^3 (emitter %.3g); L%d galaxies %lld: %.3g per Mpc^3 (expected %.3g); web density at clusters %.2fx the mean, at field galaxies %.2fx",
				Switched, Clusters, Density, Want, LBest, NBest, GotGal, WantGal, BiasC, BiasF));
}

UNI_TEST(webtex, background)
{
	// The web scene from intergalactic space: the background map (6 x 96^2) and a 480 x 270 view
	// of the background alone, marched through the procedural web and through the texture.
	const FWebTexture& T = StandIn();
	FBindScope Bind(T);
	const FCoord Cam = UniScenes::VoidCamera();
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 480; Cm.Height = 270; Cm.LookAlong({ 1, 0.3, 0.1 });
	struct FRun { FBackgroundCube Cube; double CubeSec = 0.0; FImage Img{ 1, 1 }; FVolumeStats St; double ImgSec = 0.0; };
	FRun Runs[2];
	double TailShare = 0.0;   // the textured view's light with the tail (then: the tail's share)
	for (int K = 0; K < 2; ++K)
	{
		FUniverseDef Def = UniScenes::WebToGalaxy();
		if (K == 1) { EnableWebTexture(Def, T.Key); }
		FUniverseModel M(Def);
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		FRefPipeline P(G);
		P.Update(Cam, FVec3f{});
		FVolumeScene Bg = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(true);
		// The marched part only: past the march extent both are the same analytic mean (the tail),
		// which is most of the light and would hide any difference in structure.
		if (K == 1)
		{
			FVolumeScene Tail = Bg;
			FImage Ti(Cm.Width, Cm.Height);
			TailShare = Lum3(RenderVolume(Tail, Cm, Ti, nullptr, 2).Flux);
		}
		for (FCarrierGPU& C : Bg.Carriers) { C.Flags &= ~H::UNI_CARRIER_TAIL; }
		auto T0 = std::chrono::steady_clock::now();
		Runs[K].Cube = RenderBackgroundCube(Bg, 96);
		Runs[K].CubeSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
		Runs[K].Img = FImage(Cm.Width, Cm.Height);
		T0 = std::chrono::steady_clock::now();
		Runs[K].St = RenderVolume(Bg, Cm, Runs[K].Img, nullptr, 2);
		Runs[K].ImgSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
	}
	// Cube: total light and per-texel agreement (the texture is the procedural web averaged over
	// texels, so single texels differ where the web's walls are thinner than a texel).
	double Sa = 0, Sb = 0;
	std::vector<double> Errs;
	const auto& A = Runs[0].Cube.Tex; const auto& B = Runs[1].Cube.Tex;
	double Saa = 0, Sbb = 0, Sab = 0;
	for (size_t I = 0; I < A.size(); ++I)
	{
		const double La = 0.2126 * A[I].x + 0.7152 * A[I].y + 0.0722 * A[I].z, Lb = 0.2126 * B[I].x + 0.7152 * B[I].y + 0.0722 * B[I].z;
		Sa += La; Sb += Lb; Saa += La * La; Sbb += Lb * Lb; Sab += La * Lb;
		if (La > 0.0) { Errs.push_back(std::fabs(Lb - La) / La); }
	}
	std::sort(Errs.begin(), Errs.end());
	const double N = double(A.size());
	const double Corr = (Sab / N - Sa / N * Sb / N) / std::sqrt(std::max((Saa / N - Sa * Sa / (N * N)) * (Sbb / N - Sb * Sb / (N * N)), 1e-300));
	const double Total = Sb / Sa - 1.0;
	const double Median = Errs.empty() ? 0.0 : Errs[Errs.size() / 2];
	const double StepsA = double(Runs[0].Cube.Steps) / N, StepsB = double(Runs[1].Cube.Steps) / N;
	const double Speed = Runs[0].CubeSec / std::max(Runs[1].CubeSec, 1e-9);
	const double ImgTotal = Lum3(Runs[1].St.Flux) / std::max(Lum3(Runs[0].St.Flux), 1e-300) - 1.0;
	TailShare = TailShare > 0.0 ? 1.0 - Lum3(Runs[1].St.Flux) / TailShare : 0.0;
	// Stills with one display gain (the procedural one's 99.5th percentile).
	{
		std::vector<float> L;
		for (size_t I = 0; I + 2 < Runs[0].Img.Rgb.size(); I += 3) { L.push_back(0.2126f * Runs[0].Img.Rgb[I] + 0.7152f * Runs[0].Img.Rgb[I + 1] + 0.0722f * Runs[0].Img.Rgb[I + 2]); }
		std::sort(L.begin(), L.end());
		const float Ref = L.empty() ? 1.0f : std::max(L[size_t(double(L.size() - 1) * 0.995)], 1e-30f);
		const char* Names[2] = { "still_webtex_procedural", "still_webtex_texture" };
		for (int K = 0; K < 2; ++K)
		{
			FImage I = Runs[K].Img;
			for (float& V : I.Rgb) { V = V / Ref * 0.8f; }
			WriteFrame(I, Names[K], { K == 0 ? "Background web, procedural (D-136)" : "Background web, 128^3 texture (D-164)",
				Uni::Fmt("%.0f steps per ray, %.2f s for 480 x 270 on the reference", double(Runs[K].St.Steps) / std::max<double>(double(Runs[K].St.Rays), 1.0), Runs[K].ImgSec) });
		}
	}
	const bool bOk = std::fabs(Total) < 0.03 && Corr > 0.9 && Runs[1].Cube.Truncated == 0 && Speed > 3.0 && std::fabs(ImgTotal) < 0.03;
	Uni::Check(3, "T3.webtex.background", "D-164 background marched through the web texture vs the procedural web: marched part (no tail): cube light within 3%, texel correlation > 0.9, view light within 3%, no truncation, and at least 3x faster per texel")
		.Metric("total_err", Total).Metric("correlation", Corr).Metric("median_texel_err", Median).Metric("steps_procedural", StepsA).Metric("steps_texture", StepsB)
		.Metric("seconds_procedural", Runs[0].CubeSec).Metric("seconds_texture", Runs[1].CubeSec).Metric("speedup", Speed).Metric("view_total_err", ImgTotal).Metric("tail_share", TailShare)
		.Pass(bOk, Uni::Fmt("marched part (0.1-50 Mpc; the analytic tail beyond is %.0f%% of the view's light and identical): cube 6 x 96^2 light %+.2f%%, texel correlation %.3f, median texel %.1f%%; %.0f -> %.0f steps per texel, %.2f -> %.2f s (%.1fx); view light %+.2f%%",
			100.0 * TailShare, 100.0 * Total, Corr, 100.0 * Median, StepsA, StepsB, Runs[0].CubeSec, Runs[1].CubeSec, Speed, 100.0 * ImgTotal));
}

// ==========================================================================================
// D-165: the texture project's adhesion web (Data/Noise, not tracked: regenerate with
// NoiseVolumeTileCli --set web_adhesion_density_mid). The checks fail, not skip, without it.
// ==========================================================================================

namespace
{
	const char* kDeliveredDir = "Data/Noise";
	const char* kDeliveredName = "web_adhesion_density_mid";

	/** The delivered texture, loaded once per run (empty, with the reason, if missing). */
	const FWebTexture& Delivered(std::string* OutErr = nullptr)
	{
		static std::string Err;
		static const FWebTexture T = []()
		{
			FWebTexture R;
			FWebTexture::LoadDelivered(kDeliveredDir, kDeliveredName, R, &Err);
			return R;
		}();
		if (OutErr) { *OutErr = Err; }
		return T;
	}

	float CameraGainStill(FImage& Img)
	{
		std::vector<float> L;
		for (size_t I = 0; I + 2 < Img.Rgb.size(); I += 3) { L.push_back(0.2126f * Img.Rgb[I] + 0.7152f * Img.Rgb[I + 1] + 0.0722f * Img.Rgb[I + 2]); }
		std::sort(L.begin(), L.end());
		const float P = L.empty() ? 0.0f : L[size_t(double(L.size() - 1) * 0.995)];
		const float G = P > 0.0f ? 1.5f / P : 1.0f;
		for (float& V : Img.Rgb) { V *= G; }
		return G;
	}
}

UNI_TEST(webtex, delivered)
{
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	if (!T.IsValid())
	{
		Uni::Check(1, "T1.webtex.delivered", "D-165 the delivered adhesion web texture loads (Data/Noise/web_adhesion_density_mid, any power-of-two size): full mip chain, channel means as its sidecar states, mips = box means, periodic")
			.Pass(false, "not loaded: " + Err + " (regenerate it with the noise project: NoiseVolumeTileCli --set web_adhesion_density_mid)");
		return;
	}
	std::string Bad;
	// Channel means of mip 0 against the sidecar (<name>.json, each channel's "mean"), read from
	// it so a re-bake (another size or seed) is checked against its own numbers.
	double Want[4] = { -1, -1, -1, -1 };
	{
		std::string Text;
		if (FILE* F = std::fopen((std::string(kDeliveredDir) + "/" + kDeliveredName + ".json").c_str(), "rb"))
		{
			char Buf[65536];
			for (size_t N; (N = std::fread(Buf, 1, sizeof(Buf), F)) > 0;) { Text.append(Buf, N); }
			std::fclose(F);
		}
		const char* Names[4] = { "\"channel\": \"R\"", "\"channel\": \"G\"", "\"channel\": \"B\"", "\"channel\": \"A\"" };
		for (int C = 0; C < 4; ++C)
		{
			const size_t At = Text.find(Names[C]);
			const size_t Mn = At == std::string::npos ? std::string::npos : Text.find("\"mean\":", At);
			if (Mn != std::string::npos) { Want[C] = std::strtod(Text.c_str() + Mn + 7, nullptr); }
		}
		if (Want[0] < 0 || Want[1] < 0 || Want[2] < 0 || Want[3] < 0) { Bad += "sidecar means not found; "; }
	}
	double Sum[4] = { 0, 0, 0, 0 };
	for (size_t I = 0; I < T.TexelCount(0); ++I) { const H::float4 V = T.TexelAt(0, I); Sum[0] += V.x; Sum[1] += V.y; Sum[2] += V.z; Sum[3] += V.w; }
	double WorstMean = 0.0;
	for (int C = 0; C < 4; ++C) { WorstMean = std::max(WorstMean, std::fabs(Sum[C] / double(T.TexelCount(0)) - Want[C]) / Want[C]); }
	if (WorstMean > 1e-5) { Bad += Uni::Fmt("channel means off by %.2g; ", WorstMean); }
	const H::float4 Top = T.Mean();
	const double Total = double(Top.x) + Top.y + Top.z + Top.w;
	if (std::fabs(Total - 1.0) > 2e-3) { Bad += Uni::Fmt("top mip total %.5f (should be 1); ", Total); }
	// Mips: each texel the mean of its children, to within the two roundings (the generator
	// cascades in double from mip 0 and rounds once per level).
	size_t MipBad = 0, MipCount = 0;
	for (uint32_t M = 1; M < T.NumMips(); ++M)
	{
		const uint32_t S = T.MipSize(M);
		for (uint32_t Z = 0; Z < S; ++Z) for (uint32_t Y = 0; Y < S; ++Y) for (uint32_t X = 0; X < S; ++X)
		{
			double A[4] = { 0, 0, 0, 0 };
			for (int C = 0; C < 8; ++C)
			{
				const H::float4& V = T.Texel(M - 1, 2 * X + (C & 1), 2 * Y + ((C >> 1) & 1), 2 * Z + ((C >> 2) & 1));
				A[0] += V.x; A[1] += V.y; A[2] += V.z; A[3] += V.w;
			}
			const H::float4& P = T.Texel(M, X, Y, Z);
			const double Got[4] = { P.x, P.y, P.z, P.w };
			for (int C = 0; C < 4; ++C) { if (std::fabs(Got[C] - A[C] / 8.0) > 2e-3 * std::fabs(A[C] / 8.0) + 1e-4) { ++MipBad; } }
			++MipCount;
		}
	}
	if (MipBad) { Bad += Uni::Fmt("%zu mip texel channels off; ", MipBad); }
	// Periodic: across the tile edge the sampled field steps no more than inside.
	FBindScope Bind(T);
	Uni::FTestRng Rng(0xADE5ull);
	double Seam = 0.0, Interior = 0.0;
	const float E = 0.25f / float(T.Size);
	for (int I = 0; I < 4000; ++I)
	{
		const float Y = float(Rng.Unit()), Z = float(Rng.Unit());
		auto Tot = [](const H::float4& V) { return double(V.x) + V.y + V.z + V.w; };
		Seam = std::max(Seam, std::fabs(Tot(H::UniWebTexSample(H::float3(1.0f - E, Y, Z), 0.0f)) - Tot(H::UniWebTexSample(H::float3(E, Y, Z), 0.0f))));
		const float Xi = float(Rng.Unit()) * 0.9f + 0.05f;
		Interior = std::max(Interior, std::fabs(Tot(H::UniWebTexSample(H::float3(Xi - E, Y, Z), 0.0f)) - Tot(H::UniWebTexSample(H::float3(Xi + E, Y, Z), 0.0f))));
	}
	if (Seam > Interior * 1.5 + 1e-3) { Bad += Uni::Fmt("seam step %.3g vs interior %.3g; ", Seam, Interior); }
	Uni::Check(1, "T1.webtex.delivered", "D-165 the delivered adhesion web texture loads (Data/Noise/web_adhesion_density_mid, any power-of-two size): full mip chain, channel means as its sidecar states, mips = box means, periodic")
		.Metric("size", T.Size).Metric("mips", T.NumMips()).Metric("mean_err", WorstMean).Metric("node_share", Top.z / Total).Metric("seam", Seam).Metric("interior", Interior)
		.Pass(Bad.empty() && T.Size >= 32u && (T.Size & (T.Size - 1u)) == 0u && T.MipSize(T.NumMips() - 1u) == 1u, Bad.empty() ? Uni::Fmt("%u^3, %u mips; channel means match the sidecar to %.1g; matter: walls %.3f, filaments %.3f, nodes %.3f, void %.3f (total %.5f); %zu mip texels within rounding of their children's mean; seam step %.3g vs interior %.3g",
			T.Size, T.NumMips(), WorstMean, Top.x, Top.y, Top.z, Top.w, Total, MipCount, Seam, Interior) : Bad);
}

UNI_TEST(webtex, adhesion_census)
{
	// The web scene on the adhesion web, from intergalactic space: clusters and field galaxies at
	// their emitters' densities, clusters on the node matter, field galaxies on denser matter.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	if (!T.IsValid()) { Uni::Check(2, "T2.webtex.adhesion_census", "D-165 placement on the adhesion web").Pass(false, "texture not loaded: " + Err); return; }
	FBindScope Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	const double Share = Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w);
	const int Switched = UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Share);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	P.Update(UniScenes::VoidCamera(), FVec3f{});
	const FUniverseDef& D = M.Definition();
	const FUniEmitter& Field = D.RootEmitters[0];
	const FUniEmitter& Clu = D.RootEmitters[1];
	const double Period = std::ldexp(1.0, Field.PeriodBits);
	auto TexAt = [&](const FCoord& C)
	{
		float Pf[3];
		for (int Ax = 0; Ax < 3; ++Ax) { Pf[Ax] = float(U128ToDouble(MaskLow(C.Axis(Ax), Field.PeriodBits)) / Period); }
		return H::UniWebTexSample(H::float3(Pf[0], Pf[1], Pf[2]), 0.0f);
	};
	const int LClu = M.LevelOf(UniScenes::kCluster, -26.0);
	long long Clusters = 0, FieldGal = 0;
	double NodeAtC = 0.0, MatterAtF = 0.0, ClusterVolume = 0.0;
	std::map<int, std::pair<long long, double>> GalByLevel;
	for (const auto& KV : P.CacheRef())
	{
		long long WebGal = 0;
		for (size_t I = 0; I < KV.second.Objects.size(); ++I)
		{
			const FObjectGPU& O = KV.second.Objects[I];
			if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
			const u32 Arch = O.A[3] & 0xFFFFu;
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			if (Arch == UniScenes::kCluster) { ++Clusters; NodeAtC += TexAt(ObjectCoord(KV.first, Off)).z; }
			else if (Arch == UniScenes::kGalaxy)
			{
				const auto It = P.Contributors().find(KV.second.ObjectContributor[I]);
				const bool bMember = It != P.Contributors().end() && It->second.ParentArchetypeId == UniScenes::kCluster;
				if (!bMember)
				{
					++WebGal;
					if (FieldGal < 200000) { ++FieldGal; const H::float4 V = TexAt(ObjectCoord(KV.first, Off)); MatterAtF += double(V.x) + V.y + V.w; }
				}
			}
		}
		const double W = CellWidthCm(KV.first.Level) / Units::Mpc;
		if (KV.first.Level == LClu) { ClusterVolume += W * W * W; }
		GalByLevel[KV.first.Level].first += WebGal;
		GalByLevel[KV.first.Level].second += W * W * W;
	}
	const double Density = ClusterVolume > 0.0 ? double(Clusters) / ClusterVolume : 0.0;
	const double WantC = Clu.NTotal * std::pow(Units::Mpc, 3.0);
	const double SigC = std::sqrt(std::max(double(Clusters), 1.0)) / std::max(ClusterVolume, 1e-300);
	const double NodeBias = Clusters ? NodeAtC / double(Clusters) / Mean.z : 0.0;   // node matter at clusters / its mean
	const double FieldMean = double(Mean.x) + Mean.y + Mean.w;
	const double FieldBias = FieldGal ? MatterAtF / double(FieldGal) / FieldMean : 0.0;
	int LBest = 0; long long NBest = 0; double VBest = 0.0;
	for (const auto& KV : GalByLevel) { if (KV.second.first > NBest) { NBest = KV.second.first; VBest = KV.second.second; LBest = KV.first; } }
	const FSlice* Sl = M.SlicesOf(UniScenes::kGalaxy).ForLevel(LBest);
	const double WantGal = Sl ? Field.NTotal * Sl->P * std::pow(Units::Mpc, 3.0) : 0.0;
	const double GotGal = VBest > 0.0 ? double(NBest) / VBest : 0.0;
	const double SigGal = std::sqrt(std::max(double(NBest), 1.0)) / std::max(VBest, 1e-300);
	const bool bGal = Sl && std::abs(GotGal - WantGal) < 4.0 * SigGal + 0.02 * WantGal;
	const bool bClu = Clusters > 300 && std::abs(Density - WantC) < 4.0 * SigC + 0.02 * WantC;
	Uni::Check(2, "T2.webtex.adhesion_census", "D-165 placement on the adhesion web (web scene, 200 Mpc tile): cluster density matches its emitter (4 sigma + 2%), the busiest galaxy slice matches its share, clusters on node matter (> 5x its mean), field galaxies on denser matter than average (> 1.2x)")
		.Metric("switched", Switched).Metric("node_share", Share).Metric("clusters", double(Clusters)).Metric("density", Density).Metric("density_expected", WantC)
		.Metric("node_bias", NodeBias).Metric("field_bias", FieldBias).Metric("galaxies", double(NBest)).Metric("galaxy_density", GotGal).Metric("galaxy_density_expected", WantGal)
		.Pass(Switched == 2 && bClu && bGal && NodeBias > 5.0 && FieldBias > 1.2,
			Uni::Fmt("%d web emitters on %s; members %.1f%% of galaxies (the node matter's share); %lld clusters: %.3g per Mpc^3 (emitter %.3g), node matter at clusters %.1fx its mean; L%d galaxies %lld: %.3g per Mpc^3 (expected %.3g), matter at field galaxies %.2fx its mean",
				Switched, T.Name.c_str(), 100.0 * Share, Clusters, Density, WantC, NodeBias, LBest, NBest, GotGal, WantGal, FieldBias));
}

UNI_TEST(webtex, adhesion_stills)
{
	// Stills of the adhesion web: the background alone (the marched part, no tail), and the full
	// frame (galaxies, clusters and volume) from intergalactic space.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	if (!T.IsValid()) { Uni::Check(3, "T3.webtex.adhesion_stills", "D-165 stills of the adhesion web").Pass(false, "texture not loaded: " + Err); return; }
	FBindScope Bind(T);
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
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 480; Cm.Height = 270; Cm.LookAlong({ 1, 0.3, 0.1 });
	FVolumeScene Bg = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(true);
	for (FCarrierGPU& C : Bg.Carriers) { C.Flags &= ~H::UNI_CARRIER_TAIL; }
	FImage Bi(Cm.Width, Cm.Height);
	const auto T0 = std::chrono::steady_clock::now();
	const FVolumeStats Bs = RenderVolume(Bg, Cm, Bi, nullptr, 2);
	const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
	CameraGainStill(Bi);
	WriteFrame(Bi, "still_web_adhesion_background", { "Adhesion web (mid), background 0.1-100 Mpc (no tail), 200 Mpc tile (D-165)",
		Uni::Fmt("%.0f steps per ray, %.2f s for 480 x 270 on the reference", double(Bs.Steps) / std::max(double(Bs.Rays), 1.0), Secs) });
	const FVolumeScene All = VB.Build(P, Cam, FVec3f{}, Cm.View());
	FImage Fi(Cm.Width, Cm.Height);
	FVolumeStats Fs;
	const FFrameStats St = RenderFrameWithVolume(P, All, Cm, Fi, &Fs);
	const float Gain = CameraGainStill(Fi);
	WriteFrame(Fi, "still_web_adhesion_frame", { "Adhesion web (mid): galaxies, clusters and volume from intergalactic space (D-165)",
		Uni::Fmt("visible %zu of %zu resident; carriers %zu; display gain %.3g", St.Visible, St.Resident, All.Carriers.size(), Gain) });
	Uni::Check(3, "T3.webtex.adhesion_stills", "D-165 reference stills of the adhesion web (background alone; full frame), no truncated rays")
		.Metric("steps_per_ray", double(Bs.Steps) / std::max(double(Bs.Rays), 1.0)).Metric("visible", double(St.Visible)).Metric("truncated", double(Bs.Truncated + Fs.Truncated))
		.Pass(Bs.Truncated == 0 && Fs.Truncated == 0 && St.Visible > 0 && Lum3(Bs.Flux) > 0.0,
			Uni::Fmt("background %.0f steps per ray (%.2f s); frame: %zu of %zu resident visible", double(Bs.Steps) / std::max(double(Bs.Rays), 1.0), Secs, St.Visible, St.Resident));
}

UNI_TEST(webtex, nodes)
{
	// D-167: clusters from the node list (rank-mapped to the cluster LF): every resident cluster is
	// a catalogue entry at its exact tile position and magnitude, the heaviest node is the
	// brightest cluster, cluster density = K per tile, field galaxies at their density, and a
	// regenerated cell is identical.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	std::vector<FWebNode> Nodes;
	std::string NErr;
	const bool bNodes = LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &NErr);
	if (!T.IsValid() || !bNodes) { Uni::Check(2, "T2.webtex.nodes", "D-167 clusters from the adhesion node list").Pass(false, "not loaded: " + Err + NErr); return; }
	FBindScope Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	const double Share = Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w);
	const int Switched = UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Share, &Nodes);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	P.Update(UniScenes::VoidCamera(), FVec3f{});
	const FUniverseDef& D = M.Definition();
	const FCatalogue& Cat = D.Catalogues.at(0);
	const size_t K = Cat.Entries.size();
	const double Tile = std::ldexp(1.0, Cat.PeriodBits);
	// Entries by level.
	std::map<int, std::vector<size_t>> ByLevel;
	for (size_t I = 0; I < K; ++I) { ByLevel[M.LevelOf(UniScenes::kCluster, Cat.Entries[I].AbsMag)].push_back(I); }
	long long Clusters = 0, Matched = 0, FieldGal = 0;
	double ClusterVolTiles = 0.0;
	std::map<int, double> LevelVolTiles;
	std::map<int, long long> LevelClusters;
	double MatterAtF = 0.0;
	const FUniEmitter& Field = D.RootEmitters[0];
	std::map<int, std::pair<long long, double>> GalByLevel;
	for (const auto& KV : P.CacheRef())
	{
		const double W = CellWidthCm(KV.first.Level);
		LevelVolTiles[KV.first.Level] += std::pow(W / Tile, 3.0);
		long long WebGal = 0;
		for (size_t I = 0; I < KV.second.Objects.size(); ++I)
		{
			const FObjectGPU& O = KV.second.Objects[I];
			if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
			const u32 Arch = O.A[3] & 0xFFFFu;
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const FCoord C = ObjectCoord(KV.first, Off);
			if (Arch == UniScenes::kCluster)
			{
				++Clusters; ++LevelClusters[KV.first.Level];
				// Its tile position and magnitude are an entry's.
				double Pt[3];
				for (int Ax = 0; Ax < 3; ++Ax) { Pt[Ax] = U128ToDouble(MaskLow(C.Axis(Ax), Cat.PeriodBits)) / Tile; }
				const float Mg = H::Uni_ObjectAbsMag(UniCore::ToHlsl(O));
				for (size_t E : ByLevel[KV.first.Level])
				{
					double D2 = 0.0;
					for (int Ax = 0; Ax < 3; ++Ax) { double Dd = std::fabs(Pt[Ax] - Cat.Entries[E].Pos[Ax]); Dd = std::min(Dd, 1.0 - Dd); D2 += Dd * Dd; }
					if (D2 < 1e-18 && Mg == RoundToHalf(Cat.Entries[E].AbsMag)) { ++Matched; break; }   // objects store AbsMag as a half
				}
			}
			else if (Arch == UniScenes::kGalaxy)
			{
				const auto It = P.Contributors().find(KV.second.ObjectContributor[I]);
				const bool bMember = (It != P.Contributors().end() && It->second.ParentArchetypeId == UniScenes::kCluster) || KV.second.Indices[I] >= kFirstMemberIndex;   // D-177: authored bright members too
				if (!bMember)
				{
					++WebGal;
					if (FieldGal < 200000)
					{
						++FieldGal;
						float Pf[3];
						for (int Ax = 0; Ax < 3; ++Ax) { Pf[Ax] = float(U128ToDouble(MaskLow(C.Axis(Ax), Field.PeriodBits)) / std::ldexp(1.0, Field.PeriodBits)); }
						const H::float4 V = H::UniWebTexSample(H::float3(Pf[0], Pf[1], Pf[2]), 0.0f);
						MatterAtF += double(V.x) + V.y + V.z + V.w;
					}
				}
			}
		}
		GalByLevel[KV.first.Level].first += WebGal;
		GalByLevel[KV.first.Level].second += std::pow(W / Units::Mpc, 3.0);
	}
	// Clusters per tile, level by level (each level's cells hold its entries).
	double Expected = 0.0;
	for (const auto& KV : ByLevel) { Expected += double(KV.second.size()) * LevelVolTiles[KV.first]; ClusterVolTiles += LevelVolTiles[KV.first]; }
	const double CountRatio = Expected > 0.0 ? double(Clusters) / Expected : 0.0;
	// Regenerate one cluster cell: identical.
	bool bSame = true;
	for (const auto& KV : P.CacheRef())
	{
		if (!ByLevel.count(KV.first.Level)) { continue; }
		FCellContent Again;
		G.GenerateCell(KV.first, {}, Again);
		size_t Cl = 0;
		for (const FObjectGPU& O : KV.second.Objects) { if ((O.A[3] & 0xFFFFu) == UniScenes::kCluster) { ++Cl; } }
		size_t Cl2 = 0;
		for (size_t I = 0; I < Again.Objects.size(); ++I)
		{
			if ((Again.Objects[I].A[3] & 0xFFFFu) != UniScenes::kCluster) { continue; }
			++Cl2;
			bool bFound = false;
			for (const FObjectGPU& O : KV.second.Objects) { bFound = bFound || std::memcmp(&O, &Again.Objects[I], sizeof(O)) == 0; }
			bSame = bSame && bFound;
		}
		bSame = bSame && Cl == Cl2;
		break;
	}
	int LBest = 0; long long NBest = 0; double VBest = 0.0;
	for (const auto& KV : GalByLevel) { if (KV.second.first > NBest) { NBest = KV.second.first; VBest = KV.second.second; LBest = KV.first; } }
	const FSlice* Sl = M.SlicesOf(UniScenes::kGalaxy).ForLevel(LBest);
	const double WantGal = Sl ? Field.NTotal * Sl->P * std::pow(Units::Mpc, 3.0) : 0.0;
	const double GotGal = VBest > 0.0 ? double(NBest) / VBest : 0.0;
	const double SigGal = std::sqrt(std::max(double(NBest), 1.0)) / std::max(VBest, 1e-300);
	const bool bGal = Sl && std::abs(GotGal - WantGal) < 4.0 * SigGal + 0.02 * WantGal;
	const double FieldBias = FieldGal ? MatterAtF / double(FieldGal) : 0.0;   // total matter at field galaxies / its mean (1)
	const bool bRank = K > 1 && Cat.Entries[0].AbsMag < Cat.Entries[K - 1].AbsMag && Nodes[0].Mass >= Nodes[K - 1].Mass;
	Uni::Check(2, "T2.webtex.nodes", "D-167 clusters from the adhesion node list, rank-mapped to the cluster LF: every resident cluster is a catalogue entry (exact position, magnitude), counts = entries x tiles covered (within 15%), brightest = heaviest, a regenerated cell is identical; field galaxies at their density, on denser matter")
		.Metric("entries", double(K)).Metric("clusters", double(Clusters)).Metric("matched", double(Matched)).Metric("count_ratio", CountRatio)
		.Metric("galaxy_density", GotGal).Metric("galaxy_density_expected", WantGal).Metric("field_bias", FieldBias)
		.Pass(Switched == 1 && Clusters > 50 && Matched == Clusters && std::fabs(CountRatio - 1.0) < 0.15 && bRank && bSame && bGal && FieldBias > 1.2,
			Uni::Fmt("%zu of %zu nodes are clusters (M %.2f .. %.2f, heaviest brightest); %lld clusters resident, %lld at their entry's exact position and magnitude, %.3f of entries x tiles covered (%.2f tiles over %zu levels); regenerated cell %s; L%d field galaxies %.3g per Mpc^3 (expected %.3g), matter at field galaxies %.2fx the mean",
				K, Nodes.size(), Cat.Entries.empty() ? 0.0 : Cat.Entries[0].AbsMag, Cat.Entries.empty() ? 0.0 : Cat.Entries[K - 1].AbsMag, Clusters, Matched, CountRatio, ClusterVolTiles, ByLevel.size(), bSame ? "identical" : "DIFFERENT", LBest, GotGal, WantGal, FieldBias));
}

namespace
{
	/** CPU backend with GPU-like latency (as test_stream.cpp's), timing its own generation so the
	 *  streamer's bookkeeping can be told apart from it (the GPU does that part in the engine). */
	class FLagBackend : public IGenBackend
	{
	public:
		FLagBackend(FCpuGenBackend& In, int InLat) : Inner(In), Lat(InLat) {}
		void SubmitProbe(std::shared_ptr<FGenBatch> B) override { const auto T0 = Clock::now(); Inner.SubmitProbe(B); GenMs += Ms(T0); ProbeAt[B->Ticket] = Frame; }
		bool PollProbe(const FGenBatch& B) override { return Frame - ProbeAt[B.Ticket] >= Lat; }
		void SubmitPlace(std::shared_ptr<FGenBatch> B) override { const auto T0 = Clock::now(); Inner.SubmitPlace(B); GenMs += Ms(T0); PlaceAt[B->Ticket] = Frame; }
		bool PollPlace(const FGenBatch& B) override { return Frame - PlaceAt[B.Ticket] >= Lat; }
		u32 PoolCapacity() const override { return Inner.PoolCapacity(); }
		long Frame = 0;
		double GenMs = 0.0;
	private:
		using Clock = std::chrono::steady_clock;
		static double Ms(Clock::time_point T0) { return std::chrono::duration<double, std::milli>(Clock::now() - T0).count(); }
		FCpuGenBackend& Inner;
		int Lat;
		std::map<u64, long> ProbeAt, PlaceAt;
	};
}

UNI_TEST(webtex, travel)
{
	// D-168: the owner's intergalactic-speed hitches (game thread 50-700 ms per frame: Measure
	// 150 ms every frame over ~330k contributors, Register/Window 100-300 ms when a coarse window
	// shifted). The adhesion web with its node list, flown at 100 Mpc/s through the streamer
	// (3-frame latency), every emitter object registered at once (before) and woken by distance
	// (D-168). The streamer's bookkeeping (everything but the CPU backend's generation) per frame.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	std::vector<FWebNode> Nodes;
	if (!T.IsValid() || !LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &Err)) { Uni::Check(2, "T2.webtex.travel", "D-168 bookkeeping at intergalactic speed").Pass(false, "not loaded: " + Err); return; }
	FBindScope Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::Flythrough(Tun);   // the owner's scene: web, galaxies, stars
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w), &Nodes);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	struct FOut { double MeanMs = 0, WorstMs = 0, MeasureWorst = 0; size_t PeakAwake = 0, PeakAll = 0; double PhMean[int(EStreamPhase::Count)] = {}; FStreamProfile Worst; double GenMean = 0, WorstGen = 0; std::vector<double> FrameMs; u64 MaxCells = 0, MaxTasks = 0, MaxOverlap = 0, MaxGrowth = 0, Deferred = 0; };
	FOut Out[2];
	// D-168's claim: an object sleeps only while its influence cannot reach any window, so the
	// streamer generates exactly what it did with everything registered. Ready cells' objects at
	// a few frames of the flight, by cell key, compared between the two runs.
	using FSnap = std::unordered_map<FCellKey, std::vector<FObjectGPU>, FCellKeyHasher>;
	std::vector<FSnap> Snaps[2];
	auto Snapshot = [](FUniStreamer& S, const FCpuGenBackend& Cpu)
	{
		FSnap Out;
		std::vector<FCellKey> Keys;
		const std::vector<FCellGPU> Table = S.CellTable(&Keys);
		for (size_t I = 0; I < Table.size(); ++I)
		{
			std::vector<FObjectGPU>& V = Out[Keys[I]];
			V.assign(Cpu.PoolData().begin() + Table[I].PoolStart, Cpu.PoolData().begin() + Table[I].PoolStart + Table[I].PoolCount);
		}
		return Out;
	};
	const char* Only = std::getenv("UNI_TRAVEL_ONLY");   // profiling: "0" eager, "1" lazy
	for (int K = 0; K < 2; ++K)
	{
		if (Only && *Only - '0' != K) { continue; }
		FCpuGenBackend Cpu(G, 8388608u);
		FLagBackend Be(Cpu, 3);
		FUniStreamer S(G, Be);
		S.bAsyncIntegrals = false;
		S.bLazyWake = K == 1;
		S.SubmitDeferPerLevel = K == 1 ? S.SubmitDeferPerLevel : -1;   // D-173 on with D-168, off for the before-run
		if (const char* Df = std::getenv("UNI_DEFER")) { S.SubmitDeferPerLevel = std::atoi(Df); }
		FCoord C = UniScenes::SolarCamera();
		FVec3f F{};
		S.Update(C); ++Be.Frame;
		for (int I = 0; I < 300 && !S.AllReady(); ++I) { S.Update(C); ++Be.Frame; }
		// From rest in the Milky Way, accelerating x1.15 per frame (1 kpc/s to ~100 Mpc/s in 80
		// frames), then cruising: the flight in the owner's log.
		const int Frames = 140;
		double Speed = 1.0 * Units::Kpc;
		FTravelSensitivity Travel;   // D-154 as the engine drives it: up to +2.5 mag at speed
		for (int Fr = 0; Fr < Frames; ++Fr)
		{
			Speed = std::min(Speed * 1.15, 100.0 * Units::Mpc);
			const double Step = Speed / 60.0;
			S.DeltaMag = float(Travel.Update(Speed, 1.0 / 60.0));
			AddDisplacement(C, F, FVec3d{ Step, 0.37 * Step, 0.11 * Step });
			Be.GenMs = 0.0;
			S.Update(C); ++Be.Frame;
			const double Ms = std::max(S.Profile.Ms[int(EStreamPhase::Update)] - Be.GenMs, 0.0);
			Out[K].MeanMs += Ms / Frames;
			Out[K].FrameMs.push_back(Ms);
			Out[K].MaxCells = std::max<u64>(Out[K].MaxCells, S.Profile.CellsSubmitted);
			Out[K].MaxTasks = std::max<u64>(Out[K].MaxTasks, S.Profile.TasksSubmitted);
			Out[K].MaxOverlap = std::max<u64>(Out[K].MaxOverlap, S.Profile.OverlapTests);
			Out[K].MaxGrowth = std::max<u64>(Out[K].MaxGrowth, S.Profile.GrowthChecks);
			Out[K].Deferred += S.Profile.CellsDeferred;
			for (int P = 0; P < int(EStreamPhase::Count); ++P) { Out[K].PhMean[P] += S.Profile.Ms[P] / Frames; }
			Out[K].GenMean += Be.GenMs / Frames;
			if (std::getenv("UNI_TRAVEL_DUMP") && Ms > 25.0)
			{
				std::printf("    %s frame %d (%.2g Mpc/s, +%.2f mag): %.1f ms bookkeeping, gen %.1f |", K ? "lazy" : "eager", Fr, Speed / Units::Mpc, S.DeltaMag, Ms, Be.GenMs);
				for (int P = 0; P < int(EStreamPhase::Count); ++P) { if (S.Profile.Ms[P] > 0.5) { std::printf(" %s %.1f", StreamPhaseName(EStreamPhase(P)) + 11, S.Profile.Ms[P]); } }
				std::printf(" | Submit-own %.1f Batches-own %.1f deferred %u", S.Profile.Ms[int(EStreamPhase::Submit)] - S.Profile.Ms[int(EStreamPhase::Backend)], S.Profile.Ms[int(EStreamPhase::Batches)] - S.Profile.Ms[int(EStreamPhase::BackendPlace)], unsigned(S.Profile.CellsDeferred));
				std::printf(" | woke %u slept %u awake %u, +%u -%u, cells %u\n", unsigned(S.Profile.Woken), unsigned(S.Profile.Slept), unsigned(S.Profile.Awake), unsigned(S.Profile.ContributorsAdded), unsigned(S.Profile.ContributorsRemoved), unsigned(S.Profile.CellsSubmitted));
			}
			if (Ms > Out[K].WorstMs) { Out[K].Worst = S.Profile; Out[K].WorstGen = Be.GenMs; }
			Out[K].WorstMs = std::max(Out[K].WorstMs, Ms);
			Out[K].MeasureWorst = std::max(Out[K].MeasureWorst, S.Profile.Ms[int(EStreamPhase::Measure)]);
			Out[K].PeakAwake = std::max<size_t>(Out[K].PeakAwake, S.Contributors().size());
			if (Fr % 35 == 34) { Snaps[K].push_back(Snapshot(S, Cpu)); }
			Out[K].PeakAll = std::max<size_t>(Out[K].PeakAll, size_t(S.Profile.Awake + S.Profile.Asleep));
		}
	}
	const double Gain = Out[0].MeanMs / std::max(Out[1].MeanMs, 1e-6);
	const double GainMin = 1.8, WorstMax = 45.0;   // on the reference container (2 slow cores). D-177: the cluster split made the eager run cheap too (17 ms mean here), so the gain bar is lower and the worst-frame bar tighter
	size_t Cells = 0, Objects = 0, Differ = 0, NotYet = 0;
	if (!Only)
	{
		for (size_t I = 0; I < Snaps[0].size() && I < Snaps[1].size(); ++I)
		{
			// D-173: spreading changes when a cell becomes Ready (a deferred one is not in the table
			// yet), never what it holds: cells Ready in both runs must be identical.
			const FSnap& A = Snaps[0][I];
			const FSnap& B = Snaps[1][I];
			for (const auto& KV : A)
			{
				auto It = B.find(KV.first);
				if (It == B.end()) { ++NotYet; continue; }
				++Cells;
				Objects += KV.second.size();
				if (It->second.size() != KV.second.size()
					|| (!KV.second.empty() && std::memcmp(It->second.data(), KV.second.data(), KV.second.size() * sizeof(FObjectGPU)) != 0)) { ++Differ; }
			}
			for (const auto& KV : B) { if (!A.count(KV.first)) { ++NotYet; } }
		}
	}
	if (std::getenv("UNI_TRAVEL_DUMP"))
	{
		for (int K = 0; K < 2; ++K)
		{
			std::vector<double> Fs = Out[K].FrameMs;
			std::sort(Fs.begin(), Fs.end());
			if (!Fs.empty()) { std::printf("    %s bookkeeping: p50 %.1f p90 %.1f p95 %.1f max %.1f ms; frames over 33 ms: %zu\n", K ? "lazy" : "eager", Fs[Fs.size() / 2], Fs[size_t(0.9 * double(Fs.size() - 1))], Fs[size_t(0.95 * double(Fs.size() - 1))], Fs.back(), size_t(std::count_if(Fs.begin(), Fs.end(), [](double V) { return V > 33.0; }))); }
			std::printf("    %s per-frame peaks: cells submitted %llu, tasks %llu, overlap tests %llu, growth checks %llu; cell-frames deferred %llu\n", K ? "lazy" : "eager", (unsigned long long)Out[K].MaxCells, (unsigned long long)Out[K].MaxTasks, (unsigned long long)Out[K].MaxOverlap, (unsigned long long)Out[K].MaxGrowth, (unsigned long long)Out[K].Deferred);
			std::printf("    %s mean:", K ? "lazy" : "eager");
			for (int P = 0; P < int(EStreamPhase::Count); ++P) { if (Out[K].PhMean[P] > 0.05) { std::printf(" %s %.2f", StreamPhaseName(EStreamPhase(P)), Out[K].PhMean[P]); } }
			std::printf("\n    %s worst:", K ? "lazy" : "eager");
			for (int P = 0; P < int(EStreamPhase::Count); ++P) { if (Out[K].Worst.Ms[P] > 0.05) { std::printf(" %s %.2f", StreamPhaseName(EStreamPhase(P)), Out[K].Worst.Ms[P]); } }
			std::printf(" | gen %.2f mean, %.2f in the worst | woke %u slept %u awake %u asleep %u\n", Out[K].GenMean, Out[K].WorstGen, unsigned(Out[K].Worst.Woken), unsigned(Out[K].Worst.Slept), unsigned(Out[K].Worst.Awake), unsigned(Out[K].Worst.Asleep));
		}
	}
	Uni::Check(2, "T2.webtex.travel", "D-168/D-173 streamer bookkeeping on the flythrough scene (adhesion web, galaxies, stars) from rest to 100 Mpc/s (the whole update less the CPU backend's generation, which the GPU does in the engine), against every emitter object registered and no submission spreading: identical cells and objects through the flight, mean cost at least 1.8x lower, worst frame under 45 ms on the reference (2 slow cores; D-177), and a lower peak of cells submitted in one frame")
		.Metric("mean_ms_eager", Out[0].MeanMs).Metric("mean_ms_lazy", Out[1].MeanMs).Metric("worst_ms_eager", Out[0].WorstMs).Metric("worst_ms_lazy", Out[1].WorstMs)
		.Metric("awake_peak", double(Out[1].PeakAwake)).Metric("registered_peak_eager", double(Out[0].PeakAwake))
		.Metric("cells_differ", double(Differ)).Metric("peak_cells_before", double(Out[0].MaxCells)).Metric("peak_cells_after", double(Out[1].MaxCells))
		.Pass(Gain >= GainMin && Out[1].WorstMs < WorstMax && Differ == 0 && Cells > 0 && Out[1].MaxCells < Out[0].MaxCells && NotYet * 10 < Cells,
			Uni::Fmt("%zu cells / %zu objects Ready in both runs at 4 frames of the flight compared, %zu differ (%zu Ready in one run only); peak cells submitted in a frame %llu -> %llu (D-173, %llu cell-frames deferred); everything registered: %.1f ms mean, %.1f worst (measure up to %.1f), %zu contributors; woken by distance: %.2f ms mean, %.1f worst (measure up to %.2f), %zu awake of %zu emitter objects (%.1fx less per frame)",
				Cells, Objects, Differ, NotYet, (unsigned long long)Out[0].MaxCells, (unsigned long long)Out[1].MaxCells, (unsigned long long)Out[1].Deferred, Out[0].MeanMs, Out[0].WorstMs, Out[0].MeasureWorst, Out[0].PeakAwake, Out[1].MeanMs, Out[1].WorstMs, Out[1].MeasureWorst, Out[1].PeakAwake, Out[1].PeakAll, Gain));
}

namespace
{
	/** D-203: the streamer's frame as the GPU sees it (its cell table, fades as uploaded, and the
	 *  pool's records, with identities for the carrier handoffs), for RenderFrameWithVolume. */
	struct FStreamFrame
	{
		const FUniStreamer& S;
		const FCpuGenBackend& Pool;
		template <typename Fn> void ForEachResident(Fn&& F) const
		{
			std::vector<FCellKey> Keys;
			std::vector<u32> Gens;
			const std::vector<FCellGPU> Cells = S.CellTable(&Keys, &Gens);
			for (size_t I = 0; I < Cells.size(); ++I)
			{
				const float Fade = float((Cells[I].Flags >> 8u) & 255u) / 255.0f;
				for (u32 K = 0; K < Cells[I].PoolCount; ++K)
				{
					FObjectId Id;
					const u64 Hash = S.IdentityAt(Keys[I], Gens[I], K, Id) ? HashObject(Id) : 0u;
					F(Keys[I], Pool.PoolData()[Cells[I].PoolStart + K], Fade, Hash);
				}
			}
		}
	};
}

UNI_TEST(webtex, cell_glow)
{
	// D-203 (owner: intergalactic speed still heavy; do not stretch windows, let a fade in the
	// march cover what has not loaded). The owner's scene (adhesion web with its node list,
	// galaxies, clusters, stars), at rest in intergalactic space, then accelerating to 30 Mpc/s
	// and cruising, through the streamer with GPU-like latency, two ways:
	//   D-202  windows stretch ahead at speed (D-162), D-173's per-level spreading, travel
	//          sensitivity +0.5 at speed (D-201), no cell glow;
	//   D-203  windows keep their rest size, at most 96 cells built a frame, cell glow.
	// Per frame: the streamer's bookkeeping, the generation it asks for (the CPU backend's time:
	// the GPU's work in the engine) and the cells submitted. At two frames of the cruise: the
	// light over the whole sphere (sprites as the cell table fades them, plus the volume) against
	// a settled reference pipeline at the same camera, over the reference's sprite light (the
	// background web's analytic tail is most of the sky and never changes with loading).
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	std::vector<FWebNode> Nodes;
	if (!T.IsValid() || !LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &Err)) { Uni::Check(2, "T2.webtex.cell_glow", "D-203 cell glow at intergalactic speed").Pass(false, "not loaded: " + Err); return; }
	FBindScope Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::Flythrough(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w), &Nodes);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FRun { double MeanMs = 0, WorstMs = 0, MeanGen = 0, WorstGen = 0, MeanTotal = 0, WorstTotal = 0; u64 MaxCells = 0, MaxTasks = 0, Tasks = 0; std::vector<double> ErrOn, ErrOff; size_t MaxWindow = 0; };
	FRun Runs[2];
	const int Size = 24;
	const int Frames = 96;
	const int Samples[2] = { 64, 96 };
	for (int K = 0; K < 2; ++K)
	{
		const bool bNew = K == 1;
		FCpuGenBackend Cpu(G, 8388608u);
		FLagBackend Be(Cpu, 3);
		FUniStreamer S(G, Be);
		S.bAsyncIntegrals = false;
		if (bNew) { S.bCellFade = true; S.BuildBudget = 96; }
		FCoord C = UniScenes::VoidCamera();
		FVec3f F{};
		S.Update(C); ++Be.Frame;
		for (int I = 0; I < 400 && !S.AllReady(); ++I) { S.Update(C); ++Be.Frame; }
		for (int I = 0; I < 10; ++I) { S.Update(C); ++Be.Frame; }
		FTravelSensitivity Travel;
		Travel.MaxDeltaMag = 0.5;   // D-201
		double Speed = 100.0 * Units::Kpc;
		for (int Fr = 1; Fr <= Frames; ++Fr)
		{
			Speed = std::min(Speed * 1.15, 30.0 * Units::Mpc);
			const double Step = Speed / 60.0;
			S.DeltaMag = bNew ? 0.0f : float(Travel.Update(Speed, 1.0 / 60.0));
			AddDisplacement(C, F, FVec3d{ Step, 0.37 * Step, 0.11 * Step });
			Be.GenMs = 0.0;
			S.Update(C); ++Be.Frame;
			const double Ms = std::max(S.Profile.Ms[int(EStreamPhase::Update)] - Be.GenMs, 0.0);
			FRun& R = Runs[K];
			R.MeanMs += Ms / Frames; R.WorstMs = std::max(R.WorstMs, Ms);
			R.MeanGen += Be.GenMs / Frames; R.WorstGen = std::max(R.WorstGen, Be.GenMs);
			R.MeanTotal += (Ms + Be.GenMs) / Frames; R.WorstTotal = std::max(R.WorstTotal, Ms + Be.GenMs);
			R.MaxCells = std::max<u64>(R.MaxCells, S.Profile.CellsSubmitted);
			R.MaxTasks = std::max<u64>(R.MaxTasks, S.Profile.TasksSubmitted);
			R.Tasks += S.Profile.TasksSubmitted;
			for (const FUniStreamer::FLevelState& L : S.LevelStates()) { R.MaxWindow = std::max(R.MaxWindow, L.Cells); }
			if (Fr != Samples[0] && Fr != Samples[1]) { continue; }
			FCamera Cm; Cm.Coord = C; Cm.Width = Size; Cm.Height = Size;
			const H::FUniView V = Cm.View();
			const FStreamFrame Sf{ S, Cpu };
			FVolumeSettings On; On.bCellGlow = true;
			FVolumeSettings Off;
			FRefPipeline P(G);
			P.bVelocityLod = false;
			for (int I = 0; I < 12; ++I) { P.Update(C, FVec3f{}); }
			double SpRef = 0.0;
			const double LRef = SphereLight(P, VB.Build(P, C, FVec3f{}, V, Off), C, Size, &SpRef);
			const double Spr = std::max(SpRef * LRef, 1.0e-300);
			const double LOff = SphereLight(Sf, VB.Build(S, C, FVec3f{}, V, Off), C, Size);
			R.ErrOff.push_back((LOff - LRef) / Spr);
			if (bNew) { const double LOn = SphereLight(Sf, VB.Build(S, C, FVec3f{}, V, On), C, Size); R.ErrOn.push_back((LOn - LRef) / Spr); }
		}
	}
	const FRun& A = Runs[0];
	const FRun& B = Runs[1];
	double WorstOn = 0.0, WorstOffNew = 0.0, WorstOffOld = 0.0;
	for (double E : B.ErrOn) { WorstOn = std::max(WorstOn, std::fabs(E)); }
	for (double E : B.ErrOff) { WorstOffNew = std::max(WorstOffNew, std::fabs(E)); }
	for (double E : A.ErrOff) { WorstOffOld = std::max(WorstOffOld, std::fabs(E)); }
	std::string Light;
	for (size_t I = 0; I < B.ErrOn.size() && I < B.ErrOff.size() && I < A.ErrOff.size(); ++I)
	{
		Light += Uni::Fmt("%sframe %d: %+.1f%% (no glow %+.1f%%; D-202 %+.1f%%)", Light.empty() ? "" : "; ", Samples[I], 100.0 * B.ErrOn[I], 100.0 * B.ErrOff[I], 100.0 * A.ErrOff[I]);
	}
	Uni::Check(2, "T2.webtex.cell_glow", "D-203 at intergalactic speed (owner's scene, to 30 Mpc/s, 3-frame phases): with rest-size windows, 96 cells a frame and cell glow, the light over the sphere stays within 10% of the settled reference's sprite light and no further than without the glow (levels too fast to load are suspended into the glow already, D-128); at most 96 cells built a frame; less generation and bookkeeping than D-202 (stretched windows, travel sensitivity +0.5) on average")
		.Metric("err_on", WorstOn).Metric("err_off_new", WorstOffNew).Metric("err_off_old", WorstOffOld)
		.Metric("mean_ms_old", A.MeanMs).Metric("mean_ms_new", B.MeanMs).Metric("gen_ms_old", A.MeanGen).Metric("gen_ms_new", B.MeanGen)
		.Metric("worst_total_old", A.WorstTotal).Metric("worst_total_new", B.WorstTotal).Metric("max_cells_old", double(A.MaxCells)).Metric("max_cells_new", double(B.MaxCells))
		.Metric("tasks_old", double(A.Tasks)).Metric("tasks_new", double(B.Tasks)).Metric("max_window_old", double(A.MaxWindow)).Metric("max_window_new", double(B.MaxWindow))
		.Pass(WorstOn < 0.10 && WorstOn <= WorstOffNew + 0.005 && B.MaxCells <= 96u && B.MeanTotal < A.MeanTotal,
			Uni::Fmt("light against the settled reference's sprite light: %s. Per frame, D-202 -> D-203: bookkeeping %.2f -> %.2f ms mean; generation %.2f -> %.2f ms mean (worst frame, both: %.1f -> %.1f ms); cells submitted at most %llu -> %llu, tasks %llu -> %llu over the flight; largest window %zu -> %zu cells",
				Light.c_str(), A.MeanMs, B.MeanMs, A.MeanGen, B.MeanGen, A.WorstTotal, B.WorstTotal, (unsigned long long)A.MaxCells, (unsigned long long)B.MaxCells, (unsigned long long)A.Tasks, (unsigned long long)B.Tasks, A.MaxWindow, B.MaxWindow));
}

UNI_TEST(webtex, debug)
{
	// D-169: uu.WebDebug, the textured web in false colour (Uni_WebDebugMarch, the core the march
	// pass runs). Stills of the channel mix, the node channel and the placement field from
	// intergalactic space on the adhesion web with its node list; then checks that the views show
	// what they claim: clusters (the heaviest nodes) on bright node-view pixels, field galaxies on
	// bright placement-view pixels, every ray finite and within the step budget, and an unbound
	// texture reported (dark magenta), not drawn as an empty web.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	std::vector<FWebNode> Nodes;
	if (!T.IsValid() || !LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &Err)) { Uni::Check(2, "T2.webtex.debug", "D-169 web debug views").Pass(false, "not loaded: " + Err); return; }
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w), &Nodes);
	FBindScope Bind(T);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::VoidCamera();
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 320; Cm.Height = 180; Cm.LookAlong({ 1, 0.3, 0.1 });
	const FVolumeScene Bg = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(true);
	const H::FUniVolumeView V = ToHlsl(Bg.View);
	const auto Cb = Bg.CarrierBuf();
	const auto Pb = Bg.ParamBuf();
	const float Depth = float(150.0 * Units::Mpc);
	struct FView { u32 Mode; const char* Name; const char* Title; std::vector<float> Lum; u64 Steps = 0; u32 MaxSteps = 0; bool bFinite = true; double Covered = 0.0; };
	FView Views[] = { { 1u, "still_webdebug_channels", "uu.WebDebug 1: walls blue, filaments green, nodes red, void grey (D-169)", {} },
		{ 4u, "still_webdebug_nodes", "uu.WebDebug 4: node matter over its mean (D-169)", {} },
		{ 6u, "still_webdebug_placement", "uu.WebDebug 6: the galaxy web's field as placement reads it (D-169)", {} } };
	const auto T0 = std::chrono::steady_clock::now();
	for (FView& Vw : Views)
	{
		FImage Img(Cm.Width, Cm.Height);
		Vw.Lum.assign(size_t(Cm.Width) * Cm.Height, 0.0f);
		for (int Y = 0; Y < Cm.Height; ++Y)
		{
			for (int X = 0; X < Cm.Width; ++X)
			{
				const FV3 D = PixelDir(Cm, Cm.Width, Cm.Height, X + 0.5, Y + 0.5);
				u32 St = 0;
				const H::float4 C = H::Uni_WebDebugMarch(H::float3(float(D.X), float(D.Y), float(D.Z)), 3.0e38f, Cb, Pb, V, Vw.Mode, Depth, 2.0f, 0.3f, St);
				Vw.Steps += St;
				Vw.MaxSteps = std::max(Vw.MaxSteps, St);
				if (!std::isfinite(C.x) || !std::isfinite(C.y) || !std::isfinite(C.z)) { Vw.bFinite = false; }
				Vw.Covered += C.w;
				Vw.Lum[size_t(Y) * Cm.Width + X] = 0.2126f * C.x + 0.7152f * C.y + 0.0722f * C.z;
				// Display colour straight to the image (the PPM tonemaps 1 - exp(-I): invert it).
				auto Inv = [](float Vc) { return float(-std::log(1.0 - std::min(std::max(double(Vc), 0.0), 0.999))); };
				Img.Add(X, Y, Inv(C.x), Inv(C.y), Inv(C.z));
			}
		}
		WriteFrame(Img, Vw.Name, { Vw.Title, Uni::Fmt("adhesion web (mid), 150 Mpc deep, threshold 2x the mean; %.0f steps per ray", double(Vw.Steps) / (double(Cm.Width) * Cm.Height)) });
	}
	const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();

	// Resident clusters and field galaxies inside the view and the depth: their pixels' brightness
	// against the view's mean.
	auto PixelOf = [&](const FCoord& C, int& OutX, int& OutY, double& OutDist) -> bool
	{
		const FVec3d Dv = ToDouble(Sub(C, Cam));
		OutDist = Length(Dv);
		const double F = Dv.X * Cm.Forward.X + Dv.Y * Cm.Forward.Y + Dv.Z * Cm.Forward.Z;
		if (F <= 0.0) { return false; }
		const double Nx = (Dv.X * Cm.Right.X + Dv.Y * Cm.Right.Y + Dv.Z * Cm.Right.Z) / F / Cm.TanX();
		const double Ny = (Dv.X * Cm.Up.X + Dv.Y * Cm.Up.Y + Dv.Z * Cm.Up.Z) / F / Cm.TanY();
		OutX = int(std::floor((Nx * 0.5 + 0.5) * Cm.Width));
		OutY = int(std::floor((0.5 - Ny * 0.5) * Cm.Height));
		return OutX >= 0 && OutY >= 0 && OutX < Cm.Width && OutY < Cm.Height;
	};
	double SumNodes = 0.0, SumPlace = 0.0;
	long long NClusters = 0, NGal = 0;
	for (const auto& KV : P.CacheRef())
	{
		for (const FObjectGPU& O : KV.second.Objects)
		{
			if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
			const u32 Arch = O.A[3] & 0xFFFFu;
			if (Arch != UniScenes::kCluster && Arch != UniScenes::kGalaxy) { continue; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			int X = 0, Y = 0;
			double Dist = 0.0;
			if (!PixelOf(ObjectCoord(KV.first, Off), X, Y, Dist) || Dist > 0.5 * Depth) { continue; }   // the near half: less hidden behind nearer structure
			const size_t I = size_t(Y) * Cm.Width + X;
			if (Arch == UniScenes::kCluster) { SumNodes += Views[1].Lum[I]; ++NClusters; }
			else { SumPlace += Views[2].Lum[I]; ++NGal; }
		}
	}
	auto MeanOf = [](const std::vector<float>& L) { double S = 0.0; for (float V : L) { S += V; } return L.empty() ? 0.0 : S / double(L.size()); };
	const double NodeRatio = NClusters ? (SumNodes / double(NClusters)) / std::max(MeanOf(Views[1].Lum), 1e-12) : 0.0;
	const double PlaceRatio = NGal ? (SumPlace / double(NGal)) / std::max(MeanOf(Views[2].Lum), 1e-12) : 0.0;
	bool bFinite = true;
	u32 MaxSteps = 0;
	for (const FView& Vw : Views) { bFinite = bFinite && Vw.bFinite; MaxSteps = std::max(MaxSteps, Vw.MaxSteps); }
	const double StepsPerRay = double(Views[0].Steps) / (double(Cm.Width) * Cm.Height);
	const double Coverage = Views[0].Covered / (double(Cm.Width) * Cm.Height);
	// Unbound: no texture, so the view says so.
	H::float4 Unbound;
	{
		BindWebTexture(nullptr);
		u32 St = 0;
		Unbound = H::Uni_WebDebugMarch(H::float3(1.0f, 0.0f, 0.0f), 3.0e38f, Cb, Pb, V, 1u, Depth, 2.0f, 0.3f, St);
		BindWebTexture(&T);
	}
	const bool bUnbound = Unbound.x > 0.2f && Unbound.y == 0.0f && Unbound.z > 0.2f;
	Uni::Check(2, "T2.webtex.debug", "D-169 web debug views: clusters sit on bright pixels of the node view (2x the view's mean), field galaxies on bright pixels of the placement view (1.3x), every ray finite within the step budget, an unbound texture shown as dark magenta")
		.Metric("node_ratio", NodeRatio).Metric("placement_ratio", PlaceRatio).Metric("steps_per_ray", StepsPerRay).Metric("coverage", Coverage)
		.Pass(bFinite && MaxSteps < H::UNI_WEB_DEBUG_MAX_STEPS && NClusters > 0 && NGal > 0 && NodeRatio >= 2.0 && PlaceRatio >= 1.3 && bUnbound,
			Uni::Fmt("%lld clusters in view at %.1fx the node view's mean brightness, %lld field galaxies at %.2fx the placement view's; %.0f steps per ray (most %u), coverage %.2f; 3 views of %d x %d in %.1f s; unbound %s",
				NClusters, NodeRatio, NGal, PlaceRatio, StepsPerRay, MaxSteps, Coverage, Cm.Width, Cm.Height, Secs, bUnbound ? "dark magenta" : "NOT flagged"));
}

UNI_TEST(webtex, groups)
{
	// D-174, revised by D-177 (owner: groups become labels, dwarfs come from the map). The nodes
	// after the clusters are group objects: named catalogue entries with no emitter (no contributor,
	// no bookkeeping). Dwarfs (M -16..-12) come from one root emitter on the web's node matter with
	// the groups' total. Checks: the catalogue (every remaining node, heaviest brightest); resident
	// groups at their entries' exact positions and magnitudes, and none of them a contributor;
	// every field galaxy and cluster identical to the scene without groups; dwarfs within their
	// magnitudes, at the emitter's density (busiest dwarf level) and on node matter.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	std::vector<FWebNode> Nodes;
	if (!T.IsValid() || !LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &Err)) { Uni::Check(2, "T2.webtex.groups", "D-174/D-177 galaxy groups").Pass(false, "not loaded: " + Err); return; }
	FBindScope Bind(T);
	const H::float4 Mean = T.Mean();
	const double Share = Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w);
	auto MakeDef = [&](bool bGroups)
	{
		UniScenes::FClusterTuning Tun;
		Tun.bGroups = bGroups;
		FUniverseDef D = UniScenes::WebToGalaxy(Tun);
		UniScenes::UseDeliveredWeb(D, Tun, T.Size, Share, &Nodes);
		return D;
	};
	const FUniverseDef DefOn = MakeDef(true), DefOff = MakeDef(false);
	const FCatalogue* Gc = nullptr;
	for (const FCatalogue& C : DefOn.Catalogues) { if (C.ArchetypeId == UniScenes::kGroup) { Gc = &C; } }
	const FUniEmitter* De = nullptr;
	for (const FUniEmitter& E : DefOn.RootEmitters) { if (E.ChildArchetypeId == UniScenes::kDwarf) { De = &E; } }
	const size_t Clusters = DefOn.Catalogues.at(0).Entries.size();
	bool bOrder = Gc != nullptr;
	if (Gc) { for (size_t I = 1; I < Gc->Entries.size(); ++I) { bOrder = bOrder && Gc->Entries[I].AbsMag >= Gc->Entries[I - 1].AbsMag; } }
	const bool bLabel = DefOn.Arch(UniScenes::kGroup).Emitters.empty();
	struct FRun
	{
		std::map<std::string, std::vector<FObjectGPU>> Snap;
		size_t Groups = 0, Matched = 0, Dwarfs = 0, DwarfBad = 0, GroupContributors = 0;
		double NodeAtDwarfs = 0.0;
		std::map<int, std::pair<long long, double>> DwarfByLevel;   // level -> (dwarfs, volume Mpc^3)
	};
	auto Run = [&](const FUniverseDef& D, const FCoord& Cam, FRun& R)
	{
		FUniverseModel M(D);
		FRefGenerator G(M);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		const int Pb = 89;
		const double Tile = std::ldexp(1.0, Pb);
		const std::set<int> DwarfLevels = [&] { std::set<int> S; for (const FSlice& Sl : M.SlicesOf(UniScenes::kDwarf).Slices) { S.insert(Sl.Level); } return S; }();
		for (const auto& KV : P.CacheRef())
		{
			std::vector<FObjectGPU> Keep;
			long long Dw = 0;
			for (size_t I = 0; I < KV.second.Objects.size(); ++I)
			{
				const FObjectGPU& O = KV.second.Objects[I];
				const u32 A = O.A[3] & 0xFFFFu;
				if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
				const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
				if (A == UniScenes::kGalaxy || A == UniScenes::kCluster) { Keep.push_back(O); }
				if (A == UniScenes::kDwarf)
				{
					++R.Dwarfs; ++Dw;
					const float Mg = H::Uni_ObjectAbsMag(UniCore::ToHlsl(O));
					R.DwarfBad += (Mg < -16.01f || Mg > -11.99f) ? 1 : 0;
					const FCoord C = ObjectCoord(KV.first, Off);
					float Pf[3];
					for (int Ax = 0; Ax < 3; ++Ax) { Pf[Ax] = float(U128ToDouble(MaskLow(C.Axis(Ax), Pb)) / Tile); }
					R.NodeAtDwarfs += H::UniWebTexSample(H::float3(Pf[0], Pf[1], Pf[2]), 0.0f).z;
				}
				if (A == UniScenes::kGroup && Gc)
				{
					++R.Groups;
					const FCoord C = ObjectCoord(KV.first, Off);
					double Pt[3];
					for (int Ax = 0; Ax < 3; ++Ax) { Pt[Ax] = U128ToDouble(MaskLow(C.Axis(Ax), Pb)) / Tile; }
					const float Mg = H::Uni_ObjectAbsMag(UniCore::ToHlsl(O));
					for (const FCatalogueEntry& E : Gc->Entries)
					{
						double D2 = 0.0;
						for (int Ax = 0; Ax < 3; ++Ax) { double Dd = std::fabs(Pt[Ax] - E.Pos[Ax]); Dd = std::min(Dd, 1.0 - Dd); D2 += Dd * Dd; }
						if (D2 < 1e-18 && Mg == RoundToHalf(E.AbsMag)) { ++R.Matched; break; }
					}
				}
			}
			if (DwarfLevels.count(KV.first.Level))
			{
				const double W = CellWidthCm(KV.first.Level) / Units::Mpc;
				R.DwarfByLevel[KV.first.Level].first += Dw;
				R.DwarfByLevel[KV.first.Level].second += W * W * W;
			}
			const std::string Key = std::to_string(KV.first.Level) + ToHex(KV.first.C[0]) + ToHex(KV.first.C[1]) + ToHex(KV.first.C[2]);
			if (!Keep.empty()) { R.Snap[Key] = Keep; }
		}
		for (const auto& KV : P.Contributors()) { R.GroupContributors += KV.second.ParentArchetypeId == UniScenes::kGroup ? 1 : 0; }
	};
	FRun On, Off;
	Run(DefOn, UniScenes::VoidCamera(), On);
	Run(DefOff, UniScenes::VoidCamera(), Off);
	size_t CellsCmp = 0, CellsDiff = 0;
	for (const auto& KV : Off.Snap)
	{
		++CellsCmp;
		auto It = On.Snap.find(KV.first);
		if (It == On.Snap.end() || It->second.size() != KV.second.size() || std::memcmp(It->second.data(), KV.second.data(), KV.second.size() * sizeof(FObjectGPU)) != 0) { ++CellsDiff; }
	}
	CellsDiff += On.Snap.size() == Off.Snap.size() ? 0 : 1;
	// Dwarf density over every dwarf level against the emitter's (each level's slice share). A
	// handful of dwarfs are resident in the void, so this is a loose check; the total is checked
	// exactly: the emitter holds the dwarfs the groups did (their light / the mean dwarf flux).
	long long NBest = 0; double Want = 0.0, VBest = 0.0;
	int LBest = 0;
	double Sum = 0.0, Total = 0.0;
	if (De)
	{
		const FUniverseModel Mm(DefOn);
		for (const auto& KV : On.DwarfByLevel)
		{
			const FSlice* Sl = Mm.SlicesOf(UniScenes::kDwarf).ForLevel(KV.first);
			if (!Sl) { continue; }
			NBest += KV.second.first;
			Want += De->NTotal * Sl->P * std::pow(Units::Mpc, 3.0) * KV.second.second;   // expected count
			VBest += KV.second.second;
			if (!LBest || KV.second.first > On.DwarfByLevel[LBest].first) { LBest = KV.first; }
		}
		const double F = UniScenes::DwarfMeanFluxV();
		if (Gc) { for (const FCatalogueEntry& E : Gc->Entries) { Sum += std::pow(10.0, -0.4 * double(E.AbsMag)) / F; } }
		Total = De->NTotal * std::pow(std::ldexp(1.0, 89), 3.0);
	}
	const double Got = double(NBest);
	const double Sig = std::sqrt(std::max(Want, 1.0));
	const bool bTotal = Sum > 0.0 && std::abs(Total / Sum - 1.0) < 1e-6;
	const bool bDensity = Want > 0.0 && NBest > 20 && std::abs(Got - Want) < 4.0 * Sig + 0.05 * Want;
	const double NodeBias = On.Dwarfs ? On.NodeAtDwarfs / double(On.Dwarfs) / std::max(double(Mean.z), 1e-30) : 0.0;
	Uni::Check(2, "T2.webtex.groups", "D-174/D-177 galaxy groups from the node list: every node after the clusters a group, heaviest brightest; groups are labels (no emitter, no contributor) at their entries' exact positions and magnitudes; field galaxies and clusters identical to the scene without groups; dwarfs from the node matter: the emitter's total = the groups' dwarfs exactly, within M -16..-12, resident counts at the emitter's density (all dwarf levels, 4 sigma + 5%), on node matter (> 5x its mean)")
		.Metric("groups_per_tile", Gc ? double(Gc->Entries.size()) : 0.0).Metric("resident_groups", double(On.Groups)).Metric("dwarfs", double(On.Dwarfs)).Metric("cells_differ", double(CellsDiff))
		.Metric("dwarfs_counted", Got).Metric("dwarfs_expected", Want).Metric("dwarf_total_ratio", Sum > 0.0 ? Total / Sum : 0.0).Metric("dwarf_node_bias", NodeBias)
		.Pass(Gc && De && bOrder && bLabel && Gc->Entries.size() + Clusters <= Nodes.size() && Gc->Entries.size() > 1000 && On.Groups > 0 && On.Matched == On.Groups && On.GroupContributors == 0
			&& CellsDiff == 0 && CellsCmp > 0 && On.DwarfBad == 0 && Off.Groups == 0 && Off.Dwarfs == 0 && bTotal && bDensity && NodeBias > 5.0,
			Uni::Fmt("%zu groups per tile after %zu clusters (of %zu nodes), magnitudes %.2f .. %.2f, heaviest brightest %s, %s; from intergalactic space %zu groups resident, %zu at their entry exactly, %zu group contributors; %zu galaxy/cluster cells compared with groups off, %zu differ; dwarf emitter total %.6g per tile for the groups' %.6g; %zu dwarfs (%zu outside M -16..-12), %.0f counted for %.1f expected over %.3g Mpc^3 (most at L%d), node matter at dwarfs %.1fx its mean",
				Gc ? Gc->Entries.size() : size_t(0), Clusters, Nodes.size(), Gc && !Gc->Entries.empty() ? double(Gc->Entries.front().AbsMag) : 0.0, Gc && !Gc->Entries.empty() ? double(Gc->Entries.back().AbsMag) : 0.0, bOrder ? "yes" : "NO", bLabel ? "labels" : "STILL EMITTERS",
				On.Groups, On.Matched, On.GroupContributors, CellsCmp, CellsDiff, Total, Sum, On.Dwarfs, On.DwarfBad, Got, Want, VBest, LBest, NodeBias));
}

UNI_TEST(webtex, members)
{
	// D-177 (owner: hybrid split). A catalogue cluster's brightest members (galaxy levels from
	// L86, ~5% of its members) are authored around each copy from its own contributor; its
	// emitter places only the fainter ones (kMember). From the heaviest cluster's copy nearest the
	// void camera: every authored member inside its cluster's influence and the cut, within the
	// generator's reach bound; the members' count against their budget (NTotal x BrightRatio,
	// summed over the clusters within 30 Mpc); authored members bright, emitter members faint;
	// early types concentrated in cores (D-172); a cell's members identical regenerated and from a
	// fresh generator (the per-copy cache changes nothing); and the field sampler against plain
	// rejection on the cluster field (radial distribution).
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	std::vector<FWebNode> Nodes;
	if (!T.IsValid() || !LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &Err)) { Uni::Check(2, "T2.webtex.members", "D-177 authored bright members").Pass(false, "not loaded: " + Err); return; }
	FBindScope Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w), &Nodes);
	const FCatalogue& Cat = Def.Catalogues.at(0);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	// The camera at the heaviest cluster's copy nearest the void camera.
	const double Tile = std::ldexp(1.0, Cat.PeriodBits);
	FCoord Cam = UniScenes::VoidCamera();
	{
		double Pv[3];
		for (int Ax = 0; Ax < 3; ++Ax) { Pv[Ax] = U128ToDouble(MaskLow(Cam.Axis(Ax), Cat.PeriodBits)) / Tile; }
		auto Wrap = [](double X) { return X - std::round(X); };
		FVec3f Fr{};
		AddDisplacement(Cam, Fr, FVec3d{ Wrap(Cat.Entries[0].Pos[0] - Pv[0]) * Tile, Wrap(Cat.Entries[0].Pos[1] - Pv[1]) * Tile, Wrap(Cat.Entries[0].Pos[2] - Pv[2]) * Tile });
	}
	P.Update(Cam, FVec3f{});
	const double Cut = UniScenes::BrightMemberCut();
	// Authored members by entry (one copy of each entry within 100 Mpc: the tile is 200 Mpc).
	struct FMem { FCoord Pos; float M; bool bEarly; };
	std::map<u32, std::vector<FMem>> ByEntry;
	size_t Members = 0, FaintBad = 0, BrightBad = 0, FaintMembers = 0;
	std::set<FCellKey, bool (*)(const FCellKey&, const FCellKey&)> MemberCells([](const FCellKey& A, const FCellKey& B) { return HashCell(A) < HashCell(B); });
	for (const auto& KV : P.CacheRef())
	{
		for (size_t I = 0; I < KV.second.Objects.size(); ++I)
		{
			const FObjectGPU& O = KV.second.Objects[I];
			if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
			const u32 A = O.A[3] & 0xFFFFu;
			const float Mg = H::Uni_ObjectAbsMag(UniCore::ToHlsl(O));
			if (A == UniScenes::kMember) { ++FaintMembers; FaintBad += Mg < float(Cut) - 0.01f ? 1 : 0; }
			const u32 Ix = KV.second.Indices[I];
			if (A != UniScenes::kGalaxy || Ix < kFirstMemberIndex) { continue; }
			++Members;
			BrightBad += Mg > float(Cut) + 0.01f ? 1 : 0;
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			ByEntry[(Ix - kFirstMemberIndex) / 64u].push_back(FMem{ ObjectCoord(KV.first, Off), Mg, ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_EARLY) != 0u });
			if (MemberCells.size() < 400) { MemberCells.insert(KV.first); }
		}
	}
	// Clusters within 30 Mpc: members inside their influence and cut, the count against the budget,
	// early types by radius.
	size_t Near = 0, Outside = 0, Got = 0;
	double Want = 0.0, Worst = 0.0;
	long long CoreN = 0, CoreEarly = 0, OuterN = 0, OuterEarly = 0;
	for (const auto& KV : P.Contributors())
	{
		const FContributor& C = KV.second;
		if (C.ParentArchetypeId != UniScenes::kCluster || C.Parent.Index < kFirstCatalogueIndex || C.Parent.Index >= kFirstMemberIndex) { continue; }
		if (Length(ToDouble(Sub(C.Centre, Cam))) > 30.0 * Units::Mpc) { continue; }
		const u32 Entry = (C.Parent.Index - kFirstCatalogueIndex) % u32(Cat.Entries.size());
		++Near;
		Want += std::min(C.NTotal * Cat.BrightRatio, 64.0);
		const H::FUniFieldParams Fp = C.Params.ToHlsl();
		const auto It = ByEntry.find(Entry);
		if (It == ByEntry.end()) { continue; }
		for (const FMem& Mm : It->second)
		{
			const double Dd = Length(ToDouble(Sub(Mm.Pos, C.Centre)));
			++Got;
			const double R = Dd / C.InfluenceCm;
			Worst = std::max(Worst, R / std::max(double(Fp.A.w), 1e-9));
			Outside += R > double(Fp.A.w) * (1.0 + 1e-4) ? 1 : 0;
			if (R < 0.15) { ++CoreN; CoreEarly += Mm.bEarly ? 1 : 0; }
			else if (R > 0.4) { ++OuterN; OuterEarly += Mm.bEarly ? 1 : 0; }
		}
	}
	const double Z = Want > 0.0 ? (double(Got) - Want) / std::sqrt(Want) : 99.0;
	const double CoreFrac = CoreN ? double(CoreEarly) / double(CoreN) : 0.0;
	const double OuterFrac = OuterN ? double(OuterEarly) / double(OuterN) : 0.0;
	// Regenerated and from a fresh generator: identical member cells.
	size_t CellsCmp = 0, CellsDiff = 0;
	{
		FRefGenerator G2(M);
		for (const FCellKey& K : MemberCells)
		{
			FCellContent A, B, Cc;
			G.GenerateCell(K, {}, A);
			G.GenerateCell(K, {}, B);
			G2.GenerateCell(K, {}, Cc);
			++CellsCmp;
			auto Same = [](const FCellContent& X, const FCellContent& Y) { return X.Objects.size() == Y.Objects.size() && X.Indices == Y.Indices && (X.Objects.empty() || std::memcmp(X.Objects.data(), Y.Objects.data(), X.Objects.size() * sizeof(FObjectGPU)) == 0); };
			if (!Same(A, B) || !Same(A, Cc) || A.Hash != Cc.Hash || !G.HasAuthored(K)) { ++CellsDiff; }
		}
	}
	// The sampler against plain rejection on the same field (radial histogram, 10 bins of 0.1).
	double Ks = 0.0;
	{
		H::FUniFieldParams Fp{};
		Fp.A = H::float4(0.12f, 0.8f, 0.6f, 0.95f);
		Fp.B = H::float4(3.0f, 0.4f, 0.06f, 17.0f);
		const H::float3 Cc[4] = { H::Uni_ClusterCentre(Fp, 0u), H::Uni_ClusterCentre(Fp, 1u), H::Uni_ClusterCentre(Fp, 2u), H::Uni_ClusterCentre(Fp, 3u) };
		const int NB = 10, NS = 40000, NR = 4000;
		double Hs[NB] = {}, Hr[NB] = {};
		for (int I = 0; I < NS; ++I)
		{
			H::float3 Pf(0, 0, 0);
			if (FRefGenerator::SampleClusterField(Fp, Cc, H::Uni_SeedMix(0x51u, u32(I)), Pf)) { Hs[std::min(int(std::sqrt(double(H::dot(Pf, Pf))) / 0.1), NB - 1)] += 1.0 / NS; }
		}
		const float Env = 1.0f + 3.0f * 0.4f + 0.5f;   // above the field's maximum
		u32 St = 7u;
		for (int Gt = 0; Gt < NR; ++St)
		{
			const H::float3 Q(2.0f * H::Uni_DrawUnit(St, 1u, 0u) - 1.0f, 2.0f * H::Uni_DrawUnit(St, 1u, 1u) - 1.0f, 2.0f * H::Uni_DrawUnit(St, 1u, 2u) - 1.0f);
			if (H::dot(Q, Q) >= 1.0f) { continue; }
			if (H::Uni_DrawUnit(St, 2u, 0u) * Env < H::UniField_ClusterC(Q, Fp, Cc[0], Cc[1], Cc[2], Cc[3])) { Hr[std::min(int(std::sqrt(double(H::dot(Q, Q))) / 0.1), NB - 1)] += 1.0 / NR; ++Gt; }
		}
		double Cs = 0.0, Cr = 0.0;
		for (int B = 0; B < NB; ++B) { Cs += Hs[B]; Cr += Hr[B]; Ks = std::max(Ks, std::abs(Cs - Cr)); }
	}
	const double KsMax = 1.63 * std::sqrt(1.0 / 40000.0 + 1.0 / 4000.0);   // KS at 1%
	Uni::Check(2, "T2.webtex.members", "D-177 authored bright members of catalogue clusters (from the heaviest cluster): inside their cluster's influence and cut, their count within 4 sigma of NTotal x BrightRatio over the clusters within 30 Mpc, authored members brighter than the split and emitter members fainter, more early types in cores (r < 0.15) than outside (r > 0.4), cells identical regenerated and from a fresh generator, and the field sampler's radial distribution = plain rejection's (KS at 1%)")
		.Metric("clusters_near", double(Near)).Metric("members", double(Got)).Metric("members_expected", Want).Metric("budget_z", Z).Metric("worst_r_over_cut", Worst).Metric("early_core", CoreFrac).Metric("early_outer", OuterFrac).Metric("sampler_ks", Ks)
		.Pass(Near >= 3 && Got > 50 && std::abs(Z) < 4.0 && Outside == 0 && BrightBad == 0 && FaintBad == 0 && FaintMembers > 0 && CoreN > 10 && OuterN > 10 && CoreFrac > OuterFrac
			&& CellsCmp > 10 && CellsDiff == 0 && Ks < KsMax,
			Uni::Fmt("%zu clusters within 30 Mpc: %zu authored members for %.1f expected (%.1f sigma), %zu outside the cut (worst at %.3f of it); %zu authored members resident in all, %zu brighter than M %.2f fails, %zu emitter members of which %zu too bright; early types %.2f in cores (%lld), %.2f outside (%lld); %zu member cells regenerated and from a fresh generator, %zu differ; sampler KS %.4f (1%% bar %.4f)",
				Near, Got, Want, Z, Outside, Worst, Members, BrightBad, Cut, FaintMembers, FaintBad, CoreFrac, CoreN, OuterFrac, OuterN, CellsCmp, CellsDiff, Ks, KsMax));
}

UNI_TEST(webtex, bcg)
{
	// D-191: rich catalogue clusters (M < BcgMaxMag) carry a brightest cluster galaxy (a giant
	// early type at their centre, authored with their bright members; its share of the light 6% at
	// M -26.5, rising as L^-0.5 toward poorer clusters) and intracluster light (IclShare: a second
	// LUT block on their own volume carrier, never resolved); their members' counts shrink so the
	// total is unchanged. From the heaviest cluster's copy nearest the void camera, over the
	// clusters within 30 Mpc: every contributor's count scaled by 1 - BCG share - IclShare when
	// rich and untouched when not, with members + ICL + BCG = the cluster's light; exactly one BCG
	// per rich cluster and none elsewhere, at the centre, at its share's magnitude, early, the
	// cluster's brightest galaxy; and in the volume scene every rich cluster's carrier has its ICL
	// block (total light at least the unresolved light at every distance, equal with no sprites
	// drawn) and marches from the local start.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	std::vector<FWebNode> Nodes;
	if (!T.IsValid() || !LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &Err)) { Uni::Check(2, "T2.webtex.bcg", "D-191 brightest cluster galaxies and intracluster light").Pass(false, "not loaded: " + Err); return; }
	FBindScope Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::WebToGalaxy(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w), &Nodes);
	const FCatalogue& Cat = Def.Catalogues.at(0);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const double Tile = std::ldexp(1.0, Cat.PeriodBits);
	FCoord Cam = UniScenes::VoidCamera();
	{
		double Pv[3];
		for (int Ax = 0; Ax < 3; ++Ax) { Pv[Ax] = U128ToDouble(MaskLow(Cam.Axis(Ax), Cat.PeriodBits)) / Tile; }
		auto Wrap = [](double X) { return X - std::round(X); };
		FVec3f Fr{};
		AddDisplacement(Cam, Fr, FVec3d{ Wrap(Cat.Entries[0].Pos[0] - Pv[0]) * Tile, Wrap(Cat.Entries[0].Pos[1] - Pv[1]) * Tile, Wrap(Cat.Entries[0].Pos[2] - Pv[2]) * Tile });
	}
	P.Update(Cam, FVec3f{});
	auto EntryMag = [&](u32 E) { return H::Uni_F16ToF32(H::Uni_F32ToF16(Cat.Entries[E].AbsMag)); };
	// Authored members by entry: the BCG (rank 63) and the brightest of the rest.
	struct FBcg { FCoord Pos; float M; bool bEarly; };
	std::map<u32, std::vector<FBcg>> Bcgs;
	std::map<u32, float> BrightestOther;
	for (const auto& KV : P.CacheRef())
	{
		for (size_t I = 0; I < KV.second.Objects.size(); ++I)
		{
			const FObjectGPU& O = KV.second.Objects[I];
			const u32 Ix = KV.second.Indices[I];
			if ((O.A[3] & 0xFFFFu) != UniScenes::kGalaxy || Ix < kFirstMemberIndex) { continue; }
			const u32 Entry = (Ix - kFirstMemberIndex) / 64u;
			const float Mg = H::Uni_ObjectAbsMag(UniCore::ToHlsl(O));
			if ((Ix - kFirstMemberIndex) % 64u == 63u)
			{
				const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
				Bcgs[Entry].push_back(FBcg{ ObjectCoord(KV.first, Off), Mg, ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_EARLY) != 0u });
			}
			else
			{
				auto It = BrightestOther.find(Entry);
				if (It == BrightestOther.end() || Mg < It->second) { BrightestOther[Entry] = Mg; }
			}
		}
	}
	// Contributors within 30 Mpc.
	size_t Near = 0, Rich = 0, CountBad = 0, OneBcg = 0, BcgBad = 0, Brightest = 0, StrayBcg = 0;
	double WorstLight = 0.0, WorstOff = 0.0, WorstMag = 0.0;
	std::set<u32> NearEntries;
	double Gap = 99.0;   // the smallest magnitude gap from a BCG to its cluster's next brightest
	std::vector<float> IclRatios;
	for (const auto& KV : P.Contributors())
	{
		const FContributor& C = KV.second;
		if (C.ParentArchetypeId != UniScenes::kCluster || C.Parent.Index < kFirstCatalogueIndex || C.Parent.Index >= kFirstMemberIndex || C.EmitterIndex != 0) { continue; }
		if (Length(ToDouble(Sub(C.Centre, Cam))) > 30.0 * Units::Mpc) { continue; }
		const u32 Entry = (C.Parent.Index - kFirstCatalogueIndex) % u32(Cat.Entries.size());
		const float Mc = EntryMag(Entry);
		const bool bRich = Mc < Cat.BcgMaxMag;
		const double Share = Cat.BcgShareAt(Mc), Keep = 1.0 - Share - Cat.IclShare;
		++Near;
		NearEntries.insert(Entry);
		const FUniEmitter& Em = *C.Emitter;
		const double Full = std::isfinite(Em.NTotalRefMag) ? Em.NTotal * std::pow(10.0, -0.4 * (double(Mc) - Em.NTotalRefMag)) : Em.NTotal;
		const double Ratio = C.NTotal / Full;
		// Members' + ICL light over the cluster's, plus the BCG's share: 1.
		const double Light = Ratio * (1.0 + C.IclRatio) + Share;
		WorstLight = std::max(WorstLight, std::abs(Light - 1.0));
		CountBad += (std::abs(Ratio - (bRich ? Keep : 1.0)) > 1e-9 || (bRich ? std::abs(C.IclRatio - Cat.IclShare / Keep) > 1e-12 : C.IclRatio != 0.0)) ? 1 : 0;
		if (bRich) { IclRatios.push_back(float(C.IclRatio)); }
		if (!bRich) { StrayBcg += Bcgs.count(Entry) ? Bcgs[Entry].size() : 0; continue; }
		++Rich;
		const auto It = Bcgs.find(Entry);
		if (It == Bcgs.end() || It->second.size() != 1) { continue; }
		++OneBcg;
		const FBcg& B = It->second[0];
		const double Off = Length(ToDouble(Sub(B.Pos, C.Centre)));
		const double WantM = double(Mc) - 2.5 * std::log10(Share);
		WorstOff = std::max(WorstOff, Off / C.InfluenceCm);
		WorstMag = std::max(WorstMag, std::abs(double(B.M) - WantM));
		BcgBad += (!B.bEarly || Off > 1e-6 * C.InfluenceCm || std::abs(double(B.M) - WantM) > 0.02) ? 1 : 0;
		const auto Ot = BrightestOther.find(Entry);
		Brightest += (Ot == BrightestOther.end() || B.M < Ot->second) ? 1 : 0;
		if (Ot != BrightestOther.end()) { Gap = std::min(Gap, double(Ot->second) - double(B.M)); }
	}
	// BCGs of entries with no contributor near (copies farther away) are counted only if poor.
	for (const auto& KV : Bcgs) { if (!NearEntries.count(KV.first) && !(EntryMag(KV.first) < Cat.BcgMaxMag)) { StrayBcg += KV.second.size(); } }
	// The volume scene: rich clusters' carriers carry the ICL block.
	FVolumeBuilder VB(M, G);
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 320; Cm.Height = 180; Cm.LookAlong({ 1, 0.3, 0.1 });
	const FVolumeSettings Vs;
	size_t Icl = 0, Plain = 0, IclBad = 0, LutBelow = 0, LutUnequal = 0;
	double WorstEq = 0.0;
	const u32 Ls = H::UNI_VOL_LUT_SIZE;
	for (int Pass = 0; Pass < 2; ++Pass)
	{
		VB.ForceLevelFade = Pass == 0 ? -1.0f : 0.0f;   // pass 1: no sprites, every child unresolved
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View(), Vs);
		for (const FCarrierGPU& Ca : Sc.Carriers)
		{
			if (Ca.FieldId != H::UNI_FIELD_CLUSTER) { continue; }
			if (!(Ca.Flags & H::UNI_CARRIER_ICL)) { Plain += Pass == 0 ? 1 : 0; continue; }
			Icl += Pass == 0 ? 1 : 0;
			const bool bKnown = std::any_of(IclRatios.begin(), IclRatios.end(), [&](float R) { return std::abs(R - Ca.IclRatio) <= 1e-6f * R; });
			IclBad += (!bKnown || Ca.TStart != float(Vs.LocalStartCm) || Ca.PaletteIndex == Ca.LutIndex) ? 1 : 0;
			for (u32 I = 0; I < Ls; ++I)
			{
				const H::float4 E = Sc.Lut[size_t(Ca.LutIndex) * Ls + I], Tt = Sc.Lut[size_t(Ca.PaletteIndex) * Ls + I];
				const double Le = double(E.x) + E.y + E.z, Lt = double(Tt.x) + Tt.y + Tt.z;
				if (Pass == 0) { LutBelow += Lt < Le * (1.0 - 1e-6) ? 1 : 0; }
				else
				{
					const double D = std::abs(Lt - Le) / std::max(Lt, 1e-300);
					WorstEq = std::max(WorstEq, D);
					LutUnequal += D > 1e-6 ? 1 : 0;
				}
			}
		}
	}
	Uni::Check(2, "T2.webtex.bcg", "D-191 brightest cluster galaxies and intracluster light (from the heaviest cluster, clusters within 30 Mpc): counts x (1 - BCG share - IclShare) for rich clusters only, members + ICL + BCG = the cluster's light (1e-9); exactly one BCG per rich cluster and none for poorer ones, at the centre, at M - 2.5 log10 share (0.02), early, its cluster's brightest; rich carriers carry the ICL block (a rich cluster's ratio IclShare / keep, from the local start), its light at least the members' unresolved light at every distance and equal with no sprites drawn")
		.Metric("clusters_near", double(Near)).Metric("rich", double(Rich)).Metric("one_bcg", double(OneBcg)).Metric("bcg_brightest", double(Brightest)).Metric("bcg_gap", Gap).Metric("worst_light", WorstLight).Metric("worst_bcg_mag", WorstMag).Metric("icl_carriers", double(Icl)).Metric("plain_cluster_carriers", double(Plain)).Metric("icl_lut_eq", WorstEq)
		.Pass(Near >= 3 && Rich >= 1 && CountBad == 0 && WorstLight < 1e-9 && OneBcg == Rich && BcgBad == 0 && StrayBcg == 0 && Brightest == Rich
			&& Icl >= 1 && IclBad == 0 && LutBelow == 0 && LutUnequal == 0,
			Uni::Fmt("%zu clusters within 30 Mpc, %zu rich: %zu counts wrong, light off by %.1e at worst; %zu rich with exactly one BCG (%zu wrong: worst offset %.1e of the influence, magnitude %.4f off), %zu the brightest (smallest gap %.2f mag), %zu stray; volume: %zu ICL carriers (%zu wrong), %zu plain cluster carriers, ICL block below the members' at %zu entries, %zu unequal with no sprites (worst %.1e)",
				Near, Rich, CountBad, WorstLight, OneBcg, BcgBad, WorstOff, WorstMag, Brightest, Gap, StrayBcg, Icl, IclBad, Plain, LutBelow, LutUnequal, WorstEq));
}

UNI_TEST(webtex, catalogue_lookup)
{
	// D-174 follow-up (owner's engine log: game-thread spikes of 50-450 ms at speed in Submit and
	// Batches after groups landed). Every cell submitted, and again at its allocation, looked up
	// its catalogue objects by walking every entry at its level: 4,012 groups at L86, where a
	// travel-sensitivity window holds up to 13^3 cells and brings hundreds per shift. Now an
	// index by tile cell. Checks the lookup against the plain walk (re-implemented here from the
	// catalogue definition) over the cells around two cameras, and its cost per cell.
	std::string Err;
	std::vector<FWebNode> Nodes;
	if (!LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &Err)) { Uni::Check(2, "T2.webtex.catalogue_lookup", "D-174 catalogue lookup").Pass(false, "not loaded: " + Err); return; }
	UniScenes::FClusterTuning Tun;
	FUniverseDef D = UniScenes::WebToGalaxy(Tun);
	UniScenes::UseDeliveredWeb(D, Tun, 256u, 0.094, &Nodes);
	FUniverseModel M(D);
	FRefGenerator G(M);
	struct FHit { u32 Cat, Entry, Off[3], Copy; bool operator==(const FHit& O) const { return Cat == O.Cat && Entry == O.Entry && Off[0] == O.Off[0] && Off[1] == O.Off[1] && Off[2] == O.Off[2] && Copy == O.Copy; } };
	// The walk: every entry at the level, tested exactly (cells inside one tile only: L <= P), as
	// the generator did before (levels and tile positions precomputed, as it had them).
	std::vector<std::vector<int>> Lv(D.Catalogues.size());
	std::vector<std::vector<std::array<FU128, 3>>> Ev(D.Catalogues.size());
	for (u32 Ci = 0; Ci < D.Catalogues.size(); ++Ci)
	{
		const FCatalogue& Cat = D.Catalogues[Ci];
		const int P = Cat.PeriodBits;
		for (const FCatalogueEntry& En : Cat.Entries)
		{
			Lv[Ci].push_back(M.LevelOf(Cat.ArchetypeId, En.AbsMag));
			std::array<FU128, 3> E;
			for (int Ax = 0; Ax < 3; ++Ax)
			{
				const double F = std::clamp(En.Pos[Ax] - std::floor(En.Pos[Ax]), 0.0, 0.9999999999999999);
				const u64 M53 = u64(F * 9007199254740992.0);
				E[Ax] = P >= 53 ? Shl(FU128::FromU64(M53), P - 53) : Shr(FU128::FromU64(M53), 53 - P);
			}
			Ev[Ci].push_back(E);
		}
	}
	auto Walk = [&](const FCellKey& K)
	{
		std::vector<FHit> Out;
		for (u32 Ci = 0; Ci < D.Catalogues.size(); ++Ci)
		{
			const FCatalogue& Cat = D.Catalogues[Ci];
			const int P = Cat.PeriodBits, L = K.Level;
			if (L > P) { continue; }
			for (u32 Ei = 0; Ei < Cat.Entries.size(); ++Ei)
			{
				if (Lv[Ci][Ei] != L) { continue; }
				bool bIn = true;
				FHit H{ Ci, Ei, { 0, 0, 0 }, 0 };
				for (int Ax = 0; Ax < 3 && bIn; ++Ax)
				{
					const FU128& E = Ev[Ci][Ei][Ax];
					bIn = MaskLow(K.C[Ax], P - L) == Shr(E, L);
					const FU128 Rel = MaskLow(E, L);
					H.Off[Ax] = L >= 32 ? u32(Shr(Rel, L - 32).L[3]) : u32(Shl(Rel, 32 - L).L[3]);
				}
				if (bIn) { Out.push_back(H); }
			}
		}
		return Out;
	};
	size_t Cells = 0, Hits = 0, Mismatch = 0;
	double MsIndex = 0.0, MsWalk = 0.0;
	for (const FCoord& Cam : { UniScenes::VoidCamera(), UniScenes::SolarCamera() })
	{
		for (const auto& KV : G.CatalogueLevels())
		{
			const int L = KV.first;
			const FCellKey K0 = CellOf(Cam, L);
			for (int X = -6; X <= 6; ++X) for (int Y = -6; Y <= 6; ++Y) for (int Z = -6; Z <= 6; ++Z)
			{
				FCellKey K = K0;
				const int O[3] = { X, Y, Z };
				for (int Ax = 0; Ax < 3; ++Ax) { K.C[Ax] = O[Ax] >= 0 ? Add(K0.C[Ax], FU128::FromU64(u64(O[Ax]))) : Sub(K0.C[Ax], FU128::FromU64(u64(-O[Ax]))); }
				std::vector<FHit> A;
				const auto T0 = std::chrono::steady_clock::now();
				G.ForEachCatalogueObject(K, [&](const auto& R, const u32* Off, u32 Copy) { A.push_back(FHit{ R.Cat, R.Entry, { Off[0], Off[1], Off[2] }, Copy }); });
				const auto T1 = std::chrono::steady_clock::now();
				const std::vector<FHit> B = Walk(K);
				const auto T2 = std::chrono::steady_clock::now();
				MsIndex += std::chrono::duration<double, std::milli>(T1 - T0).count();
				MsWalk += std::chrono::duration<double, std::milli>(T2 - T1).count();
				++Cells;
				Hits += A.size();
				if (L <= 89 && !(A == B)) { ++Mismatch; }
			}
		}
	}
	const double UsPerCell = 1000.0 * MsIndex / double(std::max<size_t>(Cells, 1));
	Uni::Check(2, "T2.webtex.catalogue_lookup", "D-174 follow-up: a cell's catalogue objects found by tile-cell index are exactly the plain walk's (same entries, offsets and order) over 13^3 cells per catalogue level around two cameras, at least 10x faster than the walk on the reference")
		.Metric("cells", double(Cells)).Metric("objects", double(Hits)).Metric("mismatch", double(Mismatch)).Metric("us_per_cell", UsPerCell).Metric("us_per_cell_walk", 1000.0 * MsWalk / double(std::max<size_t>(Cells, 1)))
		.Pass(Cells > 0 && Hits > 0 && Mismatch == 0 && MsWalk > 10.0 * MsIndex,
			Uni::Fmt("%zu cells, %zu catalogue objects, %zu cells differ from the walk; %.2f us per cell (the walk: %.1f us)", Cells, Hits, Mismatch, UsPerCell, 1000.0 * MsWalk / double(std::max<size_t>(Cells, 1))));
}

UNI_TEST(webtex, skyweb)
{
	// D-189 (plan step G; owner: not sure, an A/B in the engine decides): the web in the sky. From
	// intergalactic space, the background (marched part + tail) of the adhesion web in the three
	// modes: physical; near-web glow (the near web's light lifted where it is overdense); dimmed
	// tail (the uniform far background x 0.1). The dimmed tail is exactly the marched part plus
	// 0.1 x the tail; both art modes raise the sky's contrast; stills of the three.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	if (!T.IsValid()) { Uni::Check(3, "T3.webtex.skyweb", "D-189 the web in the sky").Pass(false, "texture not loaded: " + Err); return; }
	FBindScope Bind(T);
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
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 240; Cm.Height = 135; Cm.LookAlong({ 1, 0.3, 0.1 });
	auto Render = [&](float Glow, float TailDim, bool bTail, double& OutLum) -> FImage
	{
		FVolumeSettings S;
		S.WebGlowGain = Glow;
		S.TailDim = TailDim;
		FVolumeScene Bg = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(true);
		if (!bTail) { for (FCarrierGPU& C : Bg.Carriers) { C.Flags &= ~H::UNI_CARRIER_TAIL; } }
		FImage Im(Cm.Width, Cm.Height);
		const FVolumeStats St = RenderVolume(Bg, Cm, Im, nullptr, 2);
		OutLum = Lum3(St.Flux);
		return Im;
	};
	auto Contrast = [&](const FImage& Im)
	{
		// std / mean of 8 x 8 block radiance (pixel energy over its solid angle: the pinhole's
		// cos^3 falloff is not structure)
		std::vector<double> B;
		for (int By = 0; By + 8 <= Im.H; By += 8) for (int Bx = 0; Bx + 8 <= Im.W; Bx += 8)
		{
			double S = 0;
			for (int Y = By; Y < By + 8; ++Y) for (int X = Bx; X < Bx + 8; ++X)
			{
				const float* Px = &Im.Rgb[(size_t(Y) * Im.W + X) * 3];
				S += (0.2126 * Px[0] + 0.7152 * Px[1] + 0.0722 * Px[2]) / PixelSolidAngle(Cm, Im.W, Im.H, X + 0.5, Y + 0.5);
			}
			B.push_back(S);
		}
		double Mn = 0, V = 0;
		for (double X : B) { Mn += X / B.size(); }
		for (double X : B) { V += (X - Mn) * (X - Mn) / B.size(); }
		return std::sqrt(V) / std::max(Mn, 1e-300);
	};
	double L0, LNoTail, LGlow, LDim;
	FImage Phys = Render(0.0f, 0.0f, true, L0);
	FImage NoTail = Render(0.0f, 0.0f, false, LNoTail);
	FImage Glow = Render(2.0f, 0.0f, true, LGlow);
	FImage Dim = Render(0.0f, 0.9f, true, LDim);
	// The dimmed tail, pixel by pixel: marched part + 0.3 x (physical - marched part).
	double WorstDim = 0.0;
	for (size_t I = 0; I < Phys.Rgb.size(); ++I)
	{
		const double Want = NoTail.Rgb[I] + 0.1 * (Phys.Rgb[I] - NoTail.Rgb[I]);
		WorstDim = std::max(WorstDim, std::abs(Dim.Rgb[I] - Want) / std::max(std::abs(Want), 1e-30));
	}
	const double C0 = Contrast(Phys), CGlow = Contrast(Glow), CDim = Contrast(Dim);
	// Stills, each at its own display gain.
	CameraGainStill(Phys); WriteFrame(Phys, "still_skyweb_physical", { "D-189 the web in the sky: physical (uu.SkyWeb 0)", Uni::Fmt("contrast %.3f", C0) });
	CameraGainStill(Glow); WriteFrame(Glow, "still_skyweb_glow", { "D-189: near-web glow (uu.SkyWeb 1: x (1 + 2 x overdensity) within 50 Mpc)", Uni::Fmt("contrast %.3f, light %.2fx", CGlow, LGlow / L0) });
	CameraGainStill(Dim); WriteFrame(Dim, "still_skyweb_dimtail", { "D-189: dimmed tail (uu.SkyWeb 2: the far background x 0.1)", Uni::Fmt("contrast %.3f, light %.2fx", CDim, LDim / L0) });
	Uni::Check(3, "T3.webtex.skyweb", "D-189 the web in the sky: the dimmed tail is the marched part + 0.1 x the tail (1e-4); the near-web glow adds light and both art modes raise the sky's 8 x 8 block radiance contrast over the physical sky (1.2x); stills of the three")
		.Metric("worst_dim", WorstDim).Metric("contrast_physical", C0).Metric("contrast_glow", CGlow).Metric("contrast_dim", CDim).Metric("glow_light", LGlow / L0).Metric("dim_light", LDim / L0)
		.Pass(WorstDim < 1e-4 && LGlow > L0 && CGlow > 1.2 * C0 && CDim > 1.2 * C0,
			Uni::Fmt("dimmed tail within %.1e of marched + 0.1 x tail; contrast physical %.3f, glow %.3f (light %.2fx), dimmed tail %.3f (light %.2fx)", WorstDim, C0, CGlow, LGlow / L0, CDim, LDim / L0));
}

UNI_TEST(webtex, travel_hint)
{
	// D-202 (owner: intergalactic speed still heavy). From intergalactic space the glow is the
	// background web alone (the engine's textured scene at the void camera: 73 steps per pixel,
	// all of them the two periodic web carriers; no local carrier on any ray), and while
	// travelling fast it is marched per pixel (D-196). Its step hints x 2 at full travel speed
	// (uu.VolumeWebTravelHint): the background map (6 x 96^2) at 2 against 1, marched part.
	// Measured: x 3 gives 23 steps a texel but a 6.1% median texel and correlation 0.973.
	const FWebTexture& T = StandIn();
	FBindScope Bind(T);
	const FCoord Cam = UniScenes::VoidCamera();
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 480; Cm.Height = 270; Cm.LookAlong({ 1, 0.3, 0.1 });
	FUniverseDef Def = UniScenes::WebToGalaxy();
	EnableWebTexture(Def, T.Key);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.Update(Cam, FVec3f{});
	FBackgroundCube Cubes[2];
	for (int K = 0; K < 2; ++K)
	{
		FVolumeSettings S; S.WebHintScale = K == 0 ? 1.0f : 2.0f;
		FVolumeScene Bg = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(true);
		for (FCarrierGPU& C : Bg.Carriers) { C.Flags &= ~H::UNI_CARRIER_TAIL; }
		Cubes[K] = RenderBackgroundCube(Bg, 96);
	}
	const auto& A = Cubes[0].Tex; const auto& B = Cubes[1].Tex;
	double Sa = 0, Sb = 0, Saa = 0, Sbb = 0, Sab = 0;
	std::vector<double> Errs;
	for (size_t I = 0; I < A.size(); ++I)
	{
		const double La = 0.2126 * A[I].x + 0.7152 * A[I].y + 0.0722 * A[I].z, Lb = 0.2126 * B[I].x + 0.7152 * B[I].y + 0.0722 * B[I].z;
		Sa += La; Sb += Lb; Saa += La * La; Sbb += Lb * Lb; Sab += La * Lb;
		if (La > 0.0) { Errs.push_back(std::fabs(Lb - La) / La); }
	}
	std::sort(Errs.begin(), Errs.end());
	const double N = double(A.size());
	const double Corr = (Sab / N - Sa / N * Sb / N) / std::sqrt(std::max((Saa / N - Sa * Sa / (N * N)) * (Sbb / N - Sb * Sb / (N * N)), 1e-300));
	const double Total = Sb / Sa - 1.0;
	const double Median = Errs.empty() ? 0.0 : Errs[Errs.size() / 2];
	const double P95 = Errs.empty() ? 0.0 : Errs[size_t(0.95 * double(Errs.size() - 1))];
	const double StepsA = double(Cubes[0].Steps) / N, StepsB = double(Cubes[1].Steps) / N;
	Uni::Check(2, "T2.webtex.travel_hint", "D-202 the background web's hints x 2 at full travel speed: the map's marched light within 0.5% of hints x 1, texel correlation >= 0.99, median texel within 2.5%, and at least 25% fewer steps")
		.Metric("light", Total).Metric("corr", Corr).Metric("median", Median).Metric("p95", P95).Metric("steps_x1", StepsA).Metric("steps_x2", StepsB)
		.Pass(std::fabs(Total) < 0.005 && Corr >= 0.99 && Median < 0.025 && StepsB < 0.75 * StepsA && Cubes[1].Truncated == 0,
			Uni::Fmt("cube 6 x 96^2: light %+.2f%%, texel correlation %.4f, median texel %.2f%%, p95 %.2f%%; %.1f -> %.1f steps per texel (%.0f%% fewer)",
				100.0 * Total, Corr, 100.0 * Median, 100.0 * P95, StepsA, StepsB, 100.0 * (1.0 - StepsB / std::max(StepsA, 1e-9))));
}

UNI_TEST(webtex, shared_texel)
{
	// D-206 (B-019). In the owner's scene the background is two periodic textured web carriers
	// (field galaxies on walls, filaments and void matter; group dwarfs on the node matter): the
	// same texture in the same frame, so every step read the same texel twice. The march now reads
	// it once for both (UniField_WebTexCh). The background map (6 x 64^2) from the void camera with
	// and without the shared read: bit for bit the same light, steps and truncation.
	std::string Err;
	const FWebTexture& T = Delivered(&Err);
	std::vector<FWebNode> Nodes;
	if (!T.IsValid() || !LoadWebNodes(kDeliveredDir, kDeliveredName, Nodes, &Err)) { Uni::Check(2, "T2.webtex.shared_texel", "D-206 one web texel read for both background carriers").Pass(false, "not loaded: " + Err); return; }
	FBindScope Bind(T);
	const UniScenes::FClusterTuning Tun;
	FUniverseDef Def = UniScenes::Flythrough(Tun);
	const H::float4 Mean = T.Mean();
	UniScenes::UseDeliveredWeb(Def, Tun, T.Size, Mean.z / (double(Mean.x) + Mean.y + Mean.z + Mean.w), &Nodes);
	FUniverseModel M(Def);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	const FCoord Cam = UniScenes::VoidCamera();
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 320; Cm.Height = 180;
	FVolumeScene Bg = VB.Build(P, Cam, FVec3f{}, Cm.View(), FVolumeSettings{}).Subset(true);
	size_t WebTex = 0, Pairs = 0;
	for (size_t I = 0; I < Bg.Carriers.size(); ++I)
	{
		const FCarrierGPU& C = Bg.Carriers[I];
		const H::FUniFieldParams& Pa = reinterpret_cast<const H::FUniFieldParams&>(Bg.Params[C.ParamIndex]);
		if (!H::Uni_CarrierWebTex(reinterpret_cast<const H::FUniCarrier&>(C), Pa)) { continue; }
		++WebTex;
		for (size_t J = 0; J < I; ++J)
		{
			if (H::Uni_CarrierSameWebTexel(reinterpret_cast<const H::FUniCarrier&>(C), Pa, reinterpret_cast<const H::FUniCarrier&>(Bg.Carriers[J]), reinterpret_cast<const H::FUniFieldParams&>(Bg.Params[Bg.Carriers[J].ParamIndex]))) { ++Pairs; break; }
		}
	}
	using Clock = std::chrono::steady_clock;
	FVolumeScene Off = Bg;
	Off.View.MarchFlags |= 1u;
	const auto T0 = Clock::now();
	const FBackgroundCube A = RenderBackgroundCube(Off, 64);
	const auto T1 = Clock::now();
	const FBackgroundCube B = RenderBackgroundCube(Bg, 64);
	const auto T2 = Clock::now();
	size_t Differ = 0;
	for (size_t I = 0; I < A.Tex.size(); ++I)
	{
		if (std::memcmp(&A.Tex[I], &B.Tex[I], sizeof(H::float3)) != 0) { ++Differ; }
	}
	const double Ma = std::chrono::duration<double, std::milli>(T1 - T0).count(), Mb = std::chrono::duration<double, std::milli>(T2 - T1).count();
	Uni::Check(2, "T2.webtex.shared_texel", "D-206 the owner's scene from the void camera: the background's two textured web carriers read the same texel; marching them with one read is bit for bit the march with two (every texel of a 6 x 64^2 map, steps, truncation)")
		.Metric("web_carriers", double(WebTex)).Metric("pairs", double(Pairs)).Metric("differ", double(Differ)).Metric("ms_two", Ma).Metric("ms_one", Mb)
		.Pass(Pairs >= 1 && Differ == 0 && A.Steps == B.Steps && A.Truncated == B.Truncated,
			Uni::Fmt("%zu textured web carriers, %zu reading an earlier one's texel; %zu of %zu texels differ, steps %llu / %llu; reference march %.0f -> %.0f ms (the CPU's trilinear emulation: %.0f%% less)",
				WebTex, Pairs, Differ, A.Tex.size(), (unsigned long long)A.Steps, (unsigned long long)B.Steps, Ma, Mb, 100.0 * (1.0 - Mb / std::max(Ma, 1e-9))));
}
