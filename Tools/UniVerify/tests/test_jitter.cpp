// test_jitter.cpp
// D-240 (owner 2026-11-14: "the march has slices and inconsistencies in the detail layers of the
// nebulae ... take a look at the way the cloud atmosphere layer handles ... its march"): the
// volume march's jittered steps (Uni_VolumeMarchJ) and the accumulation's rules.
//  - off (MarchFlags bit 3 clear, or a ray without jitter): the D-130 march bit for bit;
//  - the draws: uniform; the lookup moves with the draw index (consecutive draws uncorrelated
//    across pixels); neighbouring rays' lattice phases differ;
//  - the accumulation's rules: alpha 1, 1/2, ... down to 1/MaxSamples; no rejection inside the
//    box; the age capped;
//  - inside the Milky Way's textured disc (the nebulae's gas layer, the dust lanes): against the
//    march at an eighth of the steps, the mean of 16 jittered frames' coherent error (4 x 4 block
//    means) under one unjittered march's, and the CPU temporal reference (2 x 2 slicing, accumulating over 16 samples, a still
//    camera, 64 frames) within 1.3x the jittered mean's error.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../src/UniBlueNoise.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	FCoord JtCamAt(const FVec3d& P) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, P); return C; }

	std::vector<float> JtMarch(const FVolumeScene& Sc, const FCamera& Cm, u32 Flags, float Shift, bool bWrapper = false)
	{
		auto V = ToHlsl(Sc.View);
		V.MarchFlags |= Flags; V.JitterShift = Shift;
		if (Flags & 2u) { V.MaxSteps = 16384u; }
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		std::vector<float> Rgb(size_t(Cm.Width) * Cm.Height * 3, 0.0f);
		std::atomic<int> Next{ 0 };
		auto Work = [&]()
		{
			for (int Y = Next++; Y < Cm.Height; Y = Next++) for (int X = 0; X < Cm.Width; ++X)
			{
				H::FUniVolumeResult R;
				if (bWrapper)
				{
					const FV3 Dd = PixelDir(Cm, Cm.Width, Cm.Height, X + 0.5, Y + 0.5);
					R = H::Uni_VolumeMarch(H::float3(float(Dd.X), float(Dd.Y), float(Dd.Z)), 3.0e38f, Cb, Pb, Lb, V);
				}
				else
				{
					bool bS = false;
					R = MarchPixel(Cb, Pb, Lb, V, Cm, Cm.Width, Cm.Height, X, Y, 3.0e38f, bS);
				}
				const size_t I = (size_t(Y) * Cm.Width + X) * 3;
				Rgb[I] = R.Radiance.x; Rgb[I + 1] = R.Radiance.y; Rgb[I + 2] = R.Radiance.z;
			}
		};
		std::vector<std::thread> Ts; for (int T = 1; T < 2; ++T) { Ts.emplace_back(Work); }
		Work(); for (std::thread& T : Ts) { T.join(); }
		return Rgb;
	}

	double JtLum(const std::vector<float>& F, size_t I) { return 0.2126 * F[3 * I] + 0.7152 * F[3 * I + 1] + 0.0722 * F[3 * I + 2]; }

	double JtErr(const std::vector<float>& A, const std::vector<float>& Ref)
	{
		const size_t N = A.size() / 3;
		double M = 0.0, E = 0.0;
		for (size_t I = 0; I < N; ++I) { M += JtLum(Ref, I) / N; }
		for (size_t I = 0; I < N; ++I) { E += std::abs(JtLum(A, I) - JtLum(Ref, I)) / std::max(M, 1e-300) / N; }
		return E;
	}

	/** The coherent error: JtErr over 4 x 4 pixel blocks' means (a slice or ring spans many pixels
	 *  and stays; a jittered frame's noise averages down by 4). */
	double JtErrBlock(const std::vector<float>& A, const std::vector<float>& Ref, int W, int Ht)
	{
		std::vector<float> Ab, Rb;
		for (int By = 0; By + 4 <= Ht; By += 4) for (int Bx = 0; Bx + 4 <= W; Bx += 4)
		{
			float Sa[3] = {}, Sr[3] = {};
			for (int Y = By; Y < By + 4; ++Y) for (int X = Bx; X < Bx + 4; ++X) for (int C = 0; C < 3; ++C)
			{
				Sa[C] += A[(size_t(Y) * W + X) * 3 + C] / 16.0f; Sr[C] += Ref[(size_t(Y) * W + X) * 3 + C] / 16.0f;
			}
			for (int C = 0; C < 3; ++C) { Ab.push_back(Sa[C]); Rb.push_back(Sr[C]); }
		}
		return JtErr(Ab, Rb);
	}
}

UNI_TEST(volume, jitter)
{
	// The draws.
	double Mean = 0.0, Corr = 0.0, PhaseStep = 0.0;
	int Hist[16] = {};
	const int Np = 256 * 256;
	{
		double Sxy = 0.0, Sx = 0.0, Sy = 0.0, Sxx = 0.0, Syy = 0.0;
		size_t Nc = 0;
		for (int Y = 0; Y < 256; ++Y) for (int X = 0; X < 256; ++X) for (u32 K = 0; K < 8; ++K)
		{
			const float A = H::Uni_VolumeDraw(float(X), float(Y), K, 0.37f), B = H::Uni_VolumeDraw(float(X), float(Y), K + 1u, 0.37f);
			Mean += A / (double(Np) * 8.0);
			Hist[std::min(int(A * 16.0f), 15)]++;
			Sxy += double(A) * B; Sx += A; Sy += B; Sxx += double(A) * A; Syy += double(B) * B; ++Nc;
		}
		const double Mx = Sx / Nc, My = Sy / Nc;
		Corr = (Sxy / Nc - Mx * My) / std::sqrt(std::max((Sxx / Nc - Mx * Mx) * (Syy / Nc - My * My), 1e-30));
		for (int Y = 0; Y < 256; ++Y) for (int X = 0; X + 1 < 256; ++X)
		{
			PhaseStep += std::abs(double(H::Uni_VolumeDraw(float(X + 1), float(Y), H::UNI_VOL_DRAW_PHASE, 0.0f)) - double(H::Uni_VolumeDraw(float(X), float(Y), H::UNI_VOL_DRAW_PHASE, 0.0f))) / (256.0 * 255.0);
		}
	}
	double HistWorst = 0.0;
	for (int B = 0; B < 16; ++B) { HistWorst = std::max(HistWorst, std::abs(double(Hist[B]) / (double(Np) * 8.0 / 16.0) - 1.0)); }
	// The accumulation's rules.
	const bool bRules = H::Uni_TemporalAlpha(0.0f, 16.0f) == 1.0f && H::Uni_TemporalAlpha(1.0f, 16.0f) == 0.5f && std::abs(H::Uni_TemporalAlpha(100.0f, 16.0f) - 1.0f / 16.0f) < 1e-7f
		&& H::Uni_TemporalAlpha(5.0f, 1.0f) == 1.0f
		&& H::Uni_TemporalReject(H::float4(0.5f, 0.5f, 0.5f, 0.5f), H::float4(0.5f, 0.5f, 0.5f, 0.5f), H::float4(0.2f, 0.2f, 0.2f, 0.2f), H::float4(1, 1, 1, 0.9f), 2.0f) == 0.0f
		&& H::Uni_TemporalReject(H::float4(9.0f, 0.5f, 0.5f, 0.5f), H::float4(2.0f, 0.5f, 0.5f, 0.5f), H::float4(0.2f, 0.2f, 0.2f, 0.2f), H::float4(1, 1, 1, 0.9f), 2.0f) == 1.0f
		&& H::Uni_TemporalAgeFresh(15.5f, 0.0f, 16.0f) == 16.0f && H::Uni_TemporalAgeFresh(10.0f, 1.0f, 16.0f) == 1.0f;

	// Inside the Milky Way's textured disc.
	FWebTexture T[3];
	bool bTex = true;
	for (int S = 0; S < 3; ++S) { std::string E; if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bTex = false; } }
	if (!bTex) { Uni::Check(2, "T2.vol.jitter", "D-240 jittered steps").Pass(false, "textures not loaded"); return; }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &T[I]); }
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	FUniverseModel Mo(D);
	FRefGenerator G(Mo);
	FVolumeBuilder VB(Mo, G);
	const int W = 96, Ht = 54;
	FCamera Cm; Cm.Coord = JtCamAt({ -8.0 * Units::Kpc, 0.0, 20.0 * Units::Pc }); Cm.Width = W; Cm.Height = Ht; Cm.LookAlong({ 1.0, 0.25, -0.02 });
	FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cm.Coord, FVec3f{});
	FVolumeSettings Sv; Sv.NebulaGain = 1.0f; Sv.NebulaReflect = 6.0f;
	const FVolumeScene Sc = VB.Build(Pp, Cm.Coord, FVec3f{}, Cm.View(), Sv).Subset(false);
	// Off: bit for bit (the wrapper; a jittered ray with the flag clear).
	const std::vector<float> A0 = JtMarch(Sc, Cm, 0u, 0.0f, true), A1 = JtMarch(Sc, Cm, 0u, 0.7f);
	size_t OffDiff = 0;
	for (size_t I = 0; I < A0.size(); ++I) { OffDiff += A0[I] != A1[I] ? 1 : 0; }
	const std::vector<float> Ref = JtMarch(Sc, Cm, 2u, 0.0f);
	std::vector<float> Jm(A0.size(), 0.0f);
	const int Nj = 16;
	for (int K = 0; K < Nj; ++K)
	{
		const std::vector<float> J = JtMarch(Sc, Cm, H::UNI_VOL_JITTER_FLAG, float(std::fmod(K * 0.6180339887, 1.0)));
		for (size_t I = 0; I < J.size(); ++I) { Jm[I] += J[I] / float(Nj); }
	}
	FVolumeScene Sj = Sc;
	Sj.View.MarchFlags |= H::UNI_VOL_JITTER_FLAG;
	FTemporalVolume Tv;
	Tv.MaxSamples = 16.0f;
	for (int F = 0; F < 64; ++F) { Tv.Step(Sj, Cm, FV3{ 0, 0, 0 }, true, true, 2); }
	std::vector<float> Tr(A0.size(), 0.0f);
	for (size_t I = 0; I < Tv.Vol.size(); ++I) { Tr[3 * I] = Tv.Vol[I].x; Tr[3 * I + 1] = Tv.Vol[I].y; Tr[3 * I + 2] = Tv.Vol[I].z; }
	double AgeMean = 0.0;
	for (float A : Tv.Age) { AgeMean += A / double(Tv.Age.size()); }
	const double Eu = JtErr(A0, Ref), Ej = JtErr(Jm, Ref), Et = JtErr(Tr, Ref);
	const double Bu = JtErrBlock(A0, Ref, W, Ht), Bj = JtErrBlock(Jm, Ref, W, Ht), Bt = JtErrBlock(Tr, Ref, W, Ht);
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }

	const bool bPass = std::abs(Mean - 0.5) < 0.005 && HistWorst < 0.05 && std::abs(Corr) < 0.1 && PhaseStep > 0.2 && bRules && OffDiff == 0 && Bj < Bu && Bt < 1.3 * Bj && Et < 1.3 * Ej;
	Uni::Check(2, "T2.vol.jitter", "D-240 jittered steps: off, the D-130 march bit for bit; the draws uniform (mean within 0.005, 16 bins within 5%), consecutive draws uncorrelated across pixels (|r| < 0.1), neighbouring rays' phases a fifth apart on average at least; the accumulation's rules; inside the Milky Way's textured disc against the march at an eighth of the steps, the mean of 16 jittered frames' coherent error (4 x 4 block means: the slices, not the noise) under one unjittered march's and the temporal reference (2 x 2, 16 samples, 64 still frames) within 1.3x of it, per block and per pixel")
		.Metric("draw_mean", Mean).Metric("draw_corr", Corr).Metric("phase_step", PhaseStep).Metric("err_unjittered", Eu).Metric("err_jittered_mean", Ej).Metric("err_temporal", Et).Metric("block_unjittered", Bu).Metric("block_jittered_mean", Bj).Metric("block_temporal", Bt)
		.Pass(bPass, Uni::Fmt("draws mean %.4f, worst bin %.1f%%, consecutive r %+.3f, neighbouring phases %.3f apart; rules %s; off: %zu values differ; vs an eighth of the steps, 4 x 4 blocks (per pixel): unjittered %.2f%% (%.2f%%), mean of %d jittered %.2f%% (%.2f%%), temporal %.2f%% (%.2f%%) (age %.1f of 16, %zu accumulated, %zu restarted)",
			Mean, 100.0 * HistWorst, Corr, PhaseStep, bRules ? "ok" : "OFF", OffDiff, 100.0 * Bu, 100.0 * Eu, Nj, 100.0 * Bj, 100.0 * Ej, 100.0 * Bt, 100.0 * Et, AgeMean, Tv.Accumulated, Tv.Restarted));
}

UNI_TEST(volume, blue_noise)
{
	// D-246 (owner 2026-11-14: "quite grainy compared to the cloud atmosphere shader"; "we want to
	// use the same blue noise as the atmosphere"): the draws from a blue-noise tile (the generated
	// stand-in for the engine's FastBlueNoise_scalar_128x128x8), the lookup moved per draw by the
	// R2 sequence (CloudAtmosphere's Atmo_TextureJitter).
	//  - the tile: uniform (mean 0.5, 16 bins within 1%), neighbours far apart (mean |difference|
	//    over 0.4; white noise 1/3), next to no power at low frequency (below 1/8 cycle per texel
	//    under 1% of white noise's; measured 0.04%). The 4 x 4 means' spread is reported, not
	//    held: a box's sidelobes pass the tile's high frequencies (0.42x white's for this tile);
	//  - the draws: consecutive indices uncorrelated (|r| < 0.1), uniform;
	//  - inside the Milky Way's disc (T2.vol.jitter's view), one jittered frame against the march at
	//    an eighth of the steps: per pixel about as noisy as D-240's hashed draws (within 1.3x); its
	//    4 x 4 blocks' grain (their error over the floor, the mean of all 16 frames' blocks) under
	//    0.8x theirs. Measured 0.73x: D-240's phase draw was already interleaved gradient noise
	//    (blue-ish), so the tile's gain is modest; the grain's real fix is D-246's resolve
	//    (T2.vol.resolve).
	const std::vector<float>& Tile = UniVerify::BlueNoiseTile(128);
	double Mean = 0.0, Nd = 0.0, BlockSd = 0.0;
	int Bins[16] = {};
	for (int Y = 0; Y < 128; ++Y) for (int X = 0; X < 128; ++X)
	{
		const float V = Tile[size_t(Y) * 128 + X];
		Mean += V / 16384.0; Bins[std::min(int(V * 16.0f), 15)]++;
		Nd += (std::abs(V - Tile[size_t(Y) * 128 + (X + 1) % 128]) + std::abs(V - Tile[size_t((Y + 1) % 128) * 128 + X])) / (2.0 * 16384.0);
	}
	{
		double S = 0.0, S2 = 0.0; int Nb = 0;
		for (int By = 0; By < 128; By += 4) for (int Bx = 0; Bx < 128; Bx += 4)
		{
			double M = 0.0;
			for (int Y = By; Y < By + 4; ++Y) for (int X = Bx; X < Bx + 4; ++X) { M += Tile[size_t(Y) * 128 + X] / 16.0; }
			S += M; S2 += M * M; ++Nb;
		}
		BlockSd = std::sqrt(std::max(S2 / Nb - (S / Nb) * (S / Nb), 0.0));
	}
	const double WhiteBlockSd = std::sqrt(1.0 / 12.0 / 16.0);
	const double LowPower = UniVerify::BlueNoiseLowPower(Tile, 128, 1.0 / 8.0);
	double BinWorst = 0.0;
	for (int B = 0; B < 16; ++B) { BinWorst = std::max(BinWorst, std::abs(Bins[B] / 1024.0 - 1.0)); }
	double Corr = 0.0;
	{
		UniVerify::FBlueNoiseBind Bn(true);
		double Sxy = 0, Sx = 0, Sy = 0, Sxx = 0, Syy = 0; size_t Nc = 0;
		for (int Y = 0; Y < 128; ++Y) for (int X = 0; X < 128; ++X) for (u32 K = 0; K < 8; ++K)
		{
			const double A = H::Uni_VolumeDraw(float(X), float(Y), K, 0.37f), B = H::Uni_VolumeDraw(float(X), float(Y), K + 1u, 0.37f);
			Sxy += A * B; Sx += A; Sy += B; Sxx += A * A; Syy += B * B; ++Nc;
		}
		const double Mx = Sx / Nc, My = Sy / Nc;
		Corr = (Sxy / Nc - Mx * My) / std::sqrt(std::max((Sxx / Nc - Mx * Mx) * (Syy / Nc - My * My), 1e-30));
	}
	// The march: one jittered frame, hashed and blue.
	FWebTexture T[3];
	bool bTex = true;
	for (int S = 0; S < 3; ++S) { std::string E; if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bTex = false; } }
	if (!bTex) { Uni::Check(2, "T2.vol.blue_noise", "D-246 blue noise").Pass(false, "textures not loaded"); return; }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &T[I]); }
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	FUniverseModel Mo(D);
	FRefGenerator G(Mo);
	FVolumeBuilder VB(Mo, G);
	const int W = 96, Ht = 54;
	FCamera Cm; Cm.Coord = JtCamAt({ -8.0 * Units::Kpc, 0.0, 20.0 * Units::Pc }); Cm.Width = W; Cm.Height = Ht; Cm.LookAlong({ 1.0, 0.25, -0.02 });
	FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cm.Coord, FVec3f{});
	FVolumeSettings Sv; Sv.NebulaGain = 1.0f; Sv.NebulaReflect = 6.0f;
	const FVolumeScene Sc = VB.Build(Pp, Cm.Coord, FVec3f{}, Cm.View(), Sv).Subset(false);
	const std::vector<float> Ref = JtMarch(Sc, Cm, 2u, 0.0f);
	double Ew = 0, Bw = 0, Eb = 0, Bb = 0;
	const int Nf = 8;
	std::vector<float> Mf(Ref.size(), 0.0f);
	for (int F = 0; F < Nf; ++F)
	{
		const float Sh = float(std::fmod(F * 0.6180339887, 1.0));
		std::vector<float> Wf, Bf;
		{ UniVerify::FBlueNoiseBind Off(false); Wf = JtMarch(Sc, Cm, H::UNI_VOL_JITTER_FLAG, Sh); }
		{ UniVerify::FBlueNoiseBind On(true); Bf = JtMarch(Sc, Cm, H::UNI_VOL_JITTER_FLAG, Sh); }
		Ew += JtErr(Wf, Ref) / Nf; Bw += JtErrBlock(Wf, Ref, W, Ht) / Nf;
		Eb += JtErr(Bf, Ref) / Nf; Bb += JtErrBlock(Bf, Ref, W, Ht) / Nf;
		for (size_t I = 0; I < Mf.size(); ++I) { Mf[I] += (Wf[I] + Bf[I]) / float(2 * Nf); }
	}
	// The floor: the mean of all 16 frames' blocks (the march's own error against the reference,
	// what no draw pattern removes); the grain is the rest.
	const double Floor = JtErrBlock(Mf, Ref, W, Ht);
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
	const bool bPass = std::abs(Mean - 0.5) < 0.001 && BinWorst < 0.01 && Nd > 0.4 && LowPower < 0.01 && std::abs(Corr) < 0.1
		&& Eb < 1.3 * Ew && (Bb - Floor) < 0.8 * (Bw - Floor);
	Uni::Check(2, "T2.vol.blue_noise", "D-246 the volume's draws from a blue-noise tile (the engine's FastBlueNoise_scalar_128x128x8 in the renderer; a generated void-and-cluster tile here), the lookup moved per draw by the R2 sequence: the tile uniform (16 bins within 1%), neighbours over 0.4 apart, its power below 1/8 cycle per texel under 1% of white noise's; consecutive draws uncorrelated (|r| < 0.1); one jittered frame inside the Milky Way's disc against an eighth of the steps: per pixel within 1.3x D-240's hashed draws, its 4 x 4 blocks' grain (error over the mean of 16 frames') under 0.8x theirs")
		.Metric("neighbour_diff", Nd).Metric("low_power", LowPower).Metric("block_sd", BlockSd).Metric("draw_corr", Corr).Metric("err_white", Ew).Metric("err_blue", Eb).Metric("block_white", Bw).Metric("block_blue", Bb).Metric("block_floor", Floor)
		.Pass(bPass, Uni::Fmt("tile mean %.4f, worst bin %.2f%%, neighbours %.3f apart (white 0.333), power below 1/8 cycle %.4f of white's, 4 x 4 means' spread %.4f (white %.4f); consecutive draws r %+.3f; one frame (mean of %d) per pixel: hashed %.2f%%, blue %.2f%%; 4 x 4 blocks: hashed %.2f%%, blue %.2f%% (floor %.2f%%: grain %.2fx)",
			Mean, 100.0 * BinWorst, Nd, LowPower, BlockSd, WhiteBlockSd, Corr, Nf, 100.0 * Ew, 100.0 * Eb, 100.0 * Bw, 100.0 * Bb, 100.0 * Floor, (Bb - Floor) / std::max(Bw - Floor, 1e-30)));
}
