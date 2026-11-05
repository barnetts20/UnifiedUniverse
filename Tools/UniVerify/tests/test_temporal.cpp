// test_temporal.cpp
// D-184: temporal slicing of the volume march. The GPU marches one pixel of every 2 x 2 block
// per frame and reconstructs the rest from the previous frame (UniVolume.usf passes 0 and 3,
// the shared core's Uni_Temporal* rules); FTemporalVolume (UniVolumeRef.h) runs the same on
// the CPU. Against a full march of every frame, over camera paths inside and outside a galaxy:
// how much light and detail the reconstruction keeps, and how much it marches.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	/** A full march of every pixel, in the temporal reference's layout. */
	std::vector<H::float4> FullMarch(const FVolumeScene& Sc, const FCamera& Cam)
	{
		FTemporalVolume Full;
		Full.Step(Sc, Cam, FV3{ 0, 0, 0 }, false, false, 2);
		return Full.Vol;
	}

	FV3 Rotate(const FV3& V, const FV3& Axis, double A)
	{
		const FV3 K = Norm(Axis);
		const double C = std::cos(A), S = std::sin(A);
		return V * C + Cross(K, V) * S + K * (Dot(K, V) * (1.0 - C));
	}

	struct FPathStats
	{
		double WorstLight = 0.0;      // |sum temporal / sum full - 1|, worst frame
		double MeanP95 = 0.0;         // per-pixel relative error, 95th percentile, mean over frames
		double WorstP95 = 0.0;
		double WorstBlock = 0.0;      // worst 8 x 8 block's light, relative to the full march
		double HistoryShare = 0.0;    // reconstructed pixels taken from history
		double MarchedShare = 0.0;    // marched pixels / all
		double Ms = 0.0, MsFull = 0.0;
		int Frames = 0;
	};

	/** Fly a path: per frame the camera moves by Step (cm) and turns by Turn (rad) about Axis;
	 *  the volume is rebuilt for every frame's camera (as the engine does). Frames from the
	 *  fifth on are compared (the first four fill the history). */
	FPathStats FlyPath(const FUniverseDef& D, FCoord Cam0, FV3 Look, FV3 StepCm, FV3 Axis, double Turn, int Frames, int W, int Ht)
	{
		FUniverseModel M(D);
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		FTemporalVolume Tv;
		FPathStats St;
		FCoord Cam = Cam0;
		FV3 Fwd = Norm(Look);
		size_t Recon = 0, Hist = 0;
		for (int F = 0; F < Frames; ++F)
		{
			if (F > 0)
			{
				FVec3f Fr{};
				AddDisplacement(Cam, Fr, FVec3d{ StepCm.X, StepCm.Y, StepCm.Z });
				Fwd = Rotate(Fwd, Axis, Turn);
			}
			P.Update(Cam, FVec3f{});
			FCamera Cm; Cm.Coord = Cam; Cm.Width = W; Cm.Height = Ht; Cm.DeltaMag = 0.0;
			Cm.LookAlong(Fwd);
			const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
			const size_t H0 = Tv.FromHistory, R0 = Tv.FromHistory + Tv.FromFresh, M0 = Tv.Marched;
			const auto T0 = std::chrono::steady_clock::now();
			Tv.Step(Sc, Cm, F > 0 ? StepCm : FV3{ 0, 0, 0 }, F > 0, true, 2);
			const auto T1 = std::chrono::steady_clock::now();
			const std::vector<H::float4> Ref = FullMarch(Sc, Cm);
			const auto T2 = std::chrono::steady_clock::now();
			if (F < 4) { continue; }
			St.Ms += std::chrono::duration<double, std::milli>(T1 - T0).count();
			St.MsFull += std::chrono::duration<double, std::milli>(T2 - T1).count();
			Hist += Tv.FromHistory - H0; Recon += Tv.FromHistory + Tv.FromFresh - R0;
			St.MarchedShare += double(Tv.Marched - M0) / double(size_t(W) * Ht);
			double Sa = 0, Sb = 0;
			std::vector<double> La(Ref.size()), Lb(Ref.size());
			for (size_t I = 0; I < Ref.size(); ++I)
			{
				const double Om = PixelSolidAngle(Cm, W, Ht, double(I % W) + 0.5, double(I / W) + 0.5);
				La[I] = double(H::Uni_Luminance(H::float3(Tv.Vol[I].x, Tv.Vol[I].y, Tv.Vol[I].z))) * Om;
				Lb[I] = double(H::Uni_Luminance(H::float3(Ref[I].x, Ref[I].y, Ref[I].z))) * Om;
				Sa += La[I]; Sb += Lb[I];
			}
			St.WorstLight = std::max(St.WorstLight, std::abs(Sa / std::max(Sb, 1e-300) - 1.0));
			const double Mean = Sb / double(Ref.size());
			std::vector<double> Rel;
			for (size_t I = 0; I < Ref.size(); ++I) { if (Lb[I] > 0.01 * Mean) { Rel.push_back(std::abs(La[I] - Lb[I]) / Lb[I]); } }
			double P95 = 0.0;
			if (!Rel.empty()) { std::sort(Rel.begin(), Rel.end()); P95 = Rel[std::min(Rel.size() - 1, size_t(0.95 * double(Rel.size())))]; }
			St.MeanP95 += P95;
			St.WorstP95 = std::max(St.WorstP95, P95);
			for (int By = 0; By + 8 <= Ht; By += 8) for (int Bx = 0; Bx + 8 <= W; Bx += 8)
			{
				double A = 0, B = 0;
				for (int Y = By; Y < By + 8; ++Y) for (int X = Bx; X < Bx + 8; ++X) { A += La[size_t(Y) * W + X]; B += Lb[size_t(Y) * W + X]; }
				if (B > 64.0 * 0.05 * Mean) { St.WorstBlock = std::max(St.WorstBlock, std::abs(A - B) / B); }
			}
			++St.Frames;
		}
		if (St.Frames > 0) { St.MeanP95 /= St.Frames; St.MarchedShare /= St.Frames; }
		St.HistoryShare = Recon ? double(Hist) / double(Recon) : 0.0;
		return St;
	}
}

UNI_TEST(volume, temporal)
{
	// D-184 (owner: the GPU march is the frame's cost inside a galaxy; temporal slicing like the
	// engine's cloud renderer). Paths, each 16 frames, the first four filling the
	// history: inside the Milky Way at rest, turning 1 degree a frame (~1.8 px), flying 3 pc a
	// frame along the disc while turning, and from 60 kpc drifting 0.4 kpc a frame (~24 Mpc/s at
	// 60 fps), straight and turning. Every frame
	// marches a quarter of the pixels; the rest come from history or the fresh neighbours.
	const double Kpc = Units::Kpc, Pc = Units::Pc;
	const FUniverseDef D = UniScenes::GalaxyToStar();
	const FCoord Solar = UniScenes::SolarCamera();
	FCoord Far = Solar;
	{
		FVec3f Fr{};
		AddDisplacement(Far, Fr, FVec3d{ 0.0, -40.0 * Kpc, 45.0 * Kpc });
	}
	// The galaxy from 60 kpc is marched at 320 x 200: at 160 x 100 its arms are a pixel or two
	// wide and the reprojection's resampling dominates (7% mean error there, 1% at 320 x 200; the
	// engine marches at half of the screen, ~960 x 540).
	struct FCase { const char* Name; FCoord Cam; FV3 Look; FV3 Step; FV3 Axis; double Turn; int W, Ht; };
	const FCase Cases[] = {
		{ "inside, at rest", Solar, { -1, -0.12, -0.02 }, { 0, 0, 0 }, { 0, 0, 1 }, 0.0, 160, 100 },
		{ "inside, turning 1 deg/frame", Solar, { -1, -0.12, -0.02 }, { 0, 0, 0 }, { 0, 0, 1 }, 0.017453, 160, 100 },
		{ "inside, 3 pc/frame and turning", Solar, { -1, -0.12, -0.02 }, { -3.0 * Pc, 0.4 * Pc, 0.0 }, { 0.2, 0, 1 }, 0.008, 160, 100 },
		{ "60 kpc, drifting 0.4 kpc/frame", Far, { 0.0, 40.0, -45.0 }, { 0.4 * Kpc, 0.0, 0.0 }, { 0, 0, 1 }, 0.0, 320, 200 },
		{ "60 kpc, drifting and turning 0.3 deg/frame", Far, { 0.0, 40.0, -45.0 }, { 0.3 * Kpc, 0.2 * Kpc, 0.0 }, { 0.3, 0.2, 1 }, 0.005, 320, 200 },
	};
	std::string Detail;
	bool bOk = true;
	double WorstLight = 0.0, WorstP95 = 0.0, WorstBlock = 0.0, RestP95 = 1.0, Marched = 0.0, Speed = 0.0;
	for (const FCase& C : Cases)
	{
		const FPathStats S = FlyPath(D, C.Cam, C.Look, C.Step, C.Axis, C.Turn, 16, C.W, C.Ht);
		const bool bRest = C.Turn == 0.0 && C.Step.X == 0.0 && C.Step.Y == 0.0 && C.Step.Z == 0.0;
		if (bRest) { RestP95 = S.WorstP95; }
		else
		{
			WorstLight = std::max(WorstLight, S.WorstLight);
			WorstP95 = std::max(WorstP95, S.MeanP95);
			WorstBlock = std::max(WorstBlock, S.WorstBlock);
		}
		Marched = std::max(Marched, S.MarchedShare);
		Speed = std::max(Speed, S.MsFull / std::max(S.Ms, 1e-9));
		bOk = bOk && S.Frames == 12;
		Detail += Uni::Fmt("%s[%s] light %.2f%%, pixel error p95 %.1f%% (worst frame %.1f%%), worst 8x8 block %.1f%%, %.0f%% from history, %.0f%% marched, %.0f vs %.0f ms",
			Detail.empty() ? "" : "; ", C.Name, 100.0 * S.WorstLight, 100.0 * S.MeanP95, 100.0 * S.WorstP95, 100.0 * S.WorstBlock, 100.0 * S.HistoryShare, 100.0 * S.MarchedShare, S.Ms, S.MsFull);
	}
	Uni::Check(2, "T2.vol.temporal", "D-184 temporal slicing (a quarter of the march pixels per frame, the rest reprojected): at rest the reconstruction equals the full march (p95 under 0.1%); moving and turning inside and outside a galaxy, every frame's light within 1% of the full march, the mean per-pixel 95th-percentile error under 6%, no 8 x 8 block of significant light off by more than 10%; a quarter of the pixels marched")
		.Metric("rest_p95", RestP95).Metric("worst_light", WorstLight).Metric("worst_mean_p95", WorstP95).Metric("worst_block", WorstBlock).Metric("marched_share", Marched).Metric("speedup", Speed)
		.Pass(bOk && RestP95 < 1e-3 && WorstLight < 0.01 && WorstP95 < 0.06 && WorstBlock < 0.10 && Marched < 0.26, Detail);
}
