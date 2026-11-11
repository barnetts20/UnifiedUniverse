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
		double MeanExcess = 0.0;      // D-194: light the reconstruction shows above the full march (trails), over the path's mean light
		double WorstExcess = 0.0;     // the worst frame's, over the same
		double WorstLightPath = 0.0;  // the worst frame's |light - full|, over the path's mean light
		std::vector<double> FrameExcess, FrameLight, FrameDiff;
		double HistoryShare = 0.0;    // reconstructed pixels taken from history
		double MarchedShare = 0.0;    // marched pixels / all
		double Ms = 0.0, MsFull = 0.0;
		int Frames = 0;
		double MeanN = 0.0;           // D-208: the interleave, averaged over the compared frames
		double WorstMarched = 0.0;    // D-215: the largest share of a frame's pixels marched
	};

	/** Fly a path: per frame the camera moves by Step (cm) and turns by Turn (rad) about Axis;
	 *  the volume is rebuilt for every frame's camera (as the engine does). Frames from the
	 *  fifth on are compared (the first four fill the history). */
	FPathStats FlyPath(const FUniverseDef& D, FCoord Cam0, FV3 Look, FV3 StepCm, FV3 Axis, double Turn, int Frames, int W, int Ht, bool bClamp = true, u32 MaxN = 2, int ResizeEvery = 0, bool bKeepAcrossResize = true)
	{
		FUniverseModel M(D);
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		FTemporalVolume Tv;
		Tv.bClamp = bClamp;   // D-194
		Tv.bKeepAcrossResize = bKeepAcrossResize;   // D-215
		FPathStats St;
		FCoord Cam = Cam0;
		FV3 Fwd = Norm(Look);
		size_t Recon = 0, Hist = 0;
		for (int F = 0; F < Frames; ++F)
		{
			// D-215: the auto budget's levels (x1, x0.92, x0.84 a side), changing every ResizeEvery frames.
			static const double LevelScale[3] = { 1.0, 0.92, 0.84 };
			const double Ls = ResizeEvery > 0 ? LevelScale[(F / ResizeEvery) % 3] : 1.0;
			const int Wf = std::max(8, int(std::lround(W * Ls))), Hf = std::max(8, int(std::lround(Ht * Ls)));
			if (F > 0)
			{
				FVec3f Fr{};
				AddDisplacement(Cam, Fr, FVec3d{ StepCm.X, StepCm.Y, StepCm.Z });
				Fwd = Rotate(Fwd, Axis, Turn);
			}
			P.Update(Cam, FVec3f{});
			FCamera Cm; Cm.Coord = Cam; Cm.Width = Wf; Cm.Height = Hf; Cm.DeltaMag = 0.0;
			Cm.LookAlong(Fwd);
			const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
			// D-208: the renderer's interleave rule (its motion estimate: the turn, plus the step over
			// the nearest marched structure).
			{
				const double ThetaPix = 2.0 * Cm.TanX() / double(Wf);
				const double StepLen = std::sqrt(Dot(StepCm, StepCm));
				const double Move = F > 0 && StepLen > 0.0 ? (Sc.NearCm > 0.0 ? StepLen / Sc.NearCm : 1.0e9) : 0.0;
				const double MotionPx = ((F > 0 ? std::abs(Turn) : 0.0) + Move) / ThetaPix;
				Tv.Interleave = H::Uni_TemporalInterleave(float(MotionPx), MaxN);
			}
			const size_t H0 = Tv.FromHistory, R0 = Tv.FromHistory + Tv.FromFresh, M0 = Tv.Marched;
			const auto T0 = std::chrono::steady_clock::now();
			Tv.Step(Sc, Cm, F > 0 ? StepCm : FV3{ 0, 0, 0 }, F > 0, true, 2);
			const auto T1 = std::chrono::steady_clock::now();
			const std::vector<H::float4> Ref = FullMarch(Sc, Cm);
			const auto T2 = std::chrono::steady_clock::now();
			if (F < 4) { continue; }
			St.MeanN += double(Tv.Interleave);
			St.Ms += std::chrono::duration<double, std::milli>(T1 - T0).count();
			St.MsFull += std::chrono::duration<double, std::milli>(T2 - T1).count();
			Hist += Tv.FromHistory - H0; Recon += Tv.FromHistory + Tv.FromFresh - R0;
			St.MarchedShare += double(Tv.Marched - M0) / double(size_t(Wf) * Hf);
			St.WorstMarched = std::max(St.WorstMarched, double(Tv.Marched - M0) / double(size_t(Wf) * Hf));   // D-215: the frame's spike
			double Sa = 0, Sb = 0;
			std::vector<double> La(Ref.size()), Lb(Ref.size());
			for (size_t I = 0; I < Ref.size(); ++I)
			{
				const double Om = PixelSolidAngle(Cm, Wf, Hf, double(I % Wf) + 0.5, double(I / Wf) + 0.5);
				La[I] = double(H::Uni_Luminance(H::float3(Tv.Vol[I].x, Tv.Vol[I].y, Tv.Vol[I].z))) * Om;
				Lb[I] = double(H::Uni_Luminance(H::float3(Ref[I].x, Ref[I].y, Ref[I].z))) * Om;
				Sa += La[I]; Sb += Lb[I];
			}
			St.WorstLight = std::max(St.WorstLight, std::abs(Sa / std::max(Sb, 1e-300) - 1.0));
			{
				// D-194: ghosting as excess light: per pixel, what the reconstruction shows above the
				// full march (with 5% grace), over the frame's light.
				double Ex = 0.0;
				for (size_t I = 0; I < Ref.size(); ++I) { Ex += std::max(0.0, La[I] - 1.05 * Lb[I]); }
				St.FrameExcess.push_back(Ex);
				St.FrameLight.push_back(Sb);
				St.FrameDiff.push_back(std::abs(Sa - Sb));
			}
			const double Mean = Sb / double(Ref.size());
			std::vector<double> Rel;
			for (size_t I = 0; I < Ref.size(); ++I) { if (Lb[I] > 0.01 * Mean) { Rel.push_back(std::abs(La[I] - Lb[I]) / Lb[I]); } }
			double P95 = 0.0;
			if (!Rel.empty()) { std::sort(Rel.begin(), Rel.end()); P95 = Rel[std::min(Rel.size() - 1, size_t(0.95 * double(Rel.size())))]; }
			St.MeanP95 += P95;
			St.WorstP95 = std::max(St.WorstP95, P95);
			for (int By = 0; By + 8 <= Hf; By += 8) for (int Bx = 0; Bx + 8 <= Wf; Bx += 8)
			{
				double A = 0, B = 0;
				for (int Y = By; Y < By + 8; ++Y) for (int X = Bx; X < Bx + 8; ++X) { A += La[size_t(Y) * Wf + X]; B += Lb[size_t(Y) * Wf + X]; }
				if (B > 64.0 * 0.05 * Mean) { St.WorstBlock = std::max(St.WorstBlock, std::abs(A - B) / B); }
			}
			++St.Frames;
		}
		if (St.Frames > 0) { St.MeanP95 /= St.Frames; St.MarchedShare /= St.Frames; St.MeanN /= St.Frames; }
		{
			// The path's mean light is the scale (a frame can hold almost none: the galaxy left the view).
			double L = 0.0, E = 0.0, Ew = 0.0, Dw = 0.0;
			for (size_t I = 0; I < St.FrameLight.size(); ++I) { L += St.FrameLight[I]; E += St.FrameExcess[I]; Ew = std::max(Ew, St.FrameExcess[I]); Dw = std::max(Dw, St.FrameDiff[I]); }
			L /= std::max<size_t>(St.FrameLight.size(), 1);
			E /= std::max<size_t>(St.FrameExcess.size(), 1);
			St.MeanExcess = E / std::max(L, 1e-300);
			St.WorstExcess = Ew / std::max(L, 1e-300);
			St.WorstLightPath = Dw / std::max(L, 1e-300);
		}
		St.HistoryShare = Recon ? double(Hist) / double(Recon) : 0.0;
		return St;
	}
}

UNI_TEST(volume, temporal)
{
	// D-184 (owner: the GPU march is the frame's cost inside a galaxy; temporal slicing like the
	// engine's cloud renderer). Paths, each 16 frames, the first four filling the
	// history: inside the Milky Way at rest, turning 1 degree a frame (~1.8 px), flying 3 pc a
	// frame along the disc while turning, and from 60 kpc drifting 0.4 kpc a frame (~24 kpc/s at
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

UNI_TEST(volume, temporal_fast)
{
	// D-194 (owner: ghosting in the glow at high speed). The Milky Way passed at warp speeds
	// (the travel curve's range: 100 kpc/s to 10 Mpc/s is 1.7 to 170 kpc a frame at 60 fps): from
	// 60 kpc, 3 kpc a frame toward and across it, 3 kpc a frame turning 0.5 degree a frame, and
	// from 150 kpc 12 kpc a frame (0.7 Mpc/s) sideways, the galaxy crossing the view (~13 px a
	// frame), 320 x 200, 16 frames each (the first four fill the history). Ghosting is measured as excess light: what the reconstruction shows above the
	// full march, over the path's mean light (a frame can hold almost none). The frames' light
	// errors are over the path's mean light for the same reason.
	const double Kpc = Units::Kpc;
	const FUniverseDef D = UniScenes::GalaxyToStar();
	FCoord Far = UniScenes::SolarCamera();
	{
		FVec3f Fr{};
		AddDisplacement(Far, Fr, FVec3d{ 0.0, -40.0 * Kpc, 45.0 * Kpc });
	}
	FCoord Wide = UniScenes::SolarCamera();   // 150 kpc from the centre (8 kpc along -x of the sun), 54 kpc to one side
	{
		FVec3f Fr{};
		AddDisplacement(Wide, Fr, FVec3d{ -8.0 * Kpc - 54.0 * Kpc, -100.0 * Kpc, 112.0 * Kpc });
	}
	struct FCase { const char* Name; FCoord Cam; FV3 Look; FV3 Step; FV3 Axis; double Turn; };
	const FCase Cases[] = {
		{ "3 kpc/frame toward and across", Far, { 0.0, 40.0, -45.0 }, { 0.0, 2.0 * Kpc, -2.2 * Kpc }, { 0, 0, 1 }, 0.0 },
		{ "3 kpc/frame, turning 0.5 deg/frame", Far, { 0.0, 40.0, -45.0 }, { 2.0 * Kpc, 1.0 * Kpc, -2.0 * Kpc }, { 0.3, 0.2, 1 }, 0.0087 },
		{ "12 kpc/frame sideways at 150 kpc", Wide, { 0.0, 100.0, -112.0 }, { 12.0 * Kpc, 0.0, 0.0 }, { 0, 0, 1 }, 0.0 },
	};
	std::string Detail;
	double WorstExcess = 0.0, MeanExcess = 0.0, WorstLight = 0.0, WorstP95 = 0.0, Marched = 0.0;
	double OldLight = 0.0, OldP95 = 0.0, OldExcess = 0.0, CloseP95 = 0.0;
	bool bOk = true;
	int Ci = 0;
	for (const FCase& C : Cases)
	{
		// The D-184 rule (the luminance band) for comparison.
		const FPathStats O = FlyPath(D, C.Cam, C.Look, C.Step, C.Axis, C.Turn, 16, 320, 200, false);
		OldLight = std::max(OldLight, O.WorstLightPath);
		OldP95 = std::max(OldP95, O.MeanP95);
		OldExcess = std::max(OldExcess, O.MeanExcess);
		Detail += Uni::Fmt("%s[%s, D-184 band] excess light %.2f%% (worst frame %.2f%%), light %.2f%%, p95 %.1f%%", Detail.empty() ? "" : "; ", C.Name, 100.0 * O.MeanExcess, 100.0 * O.WorstExcess, 100.0 * O.WorstLightPath, 100.0 * O.MeanP95);
		const FPathStats S = FlyPath(D, C.Cam, C.Look, C.Step, C.Axis, C.Turn, 16, 320, 200);
		WorstExcess = std::max(WorstExcess, S.WorstExcess);
		MeanExcess = std::max(MeanExcess, S.MeanExcess);
		WorstLight = std::max(WorstLight, S.WorstLightPath);
		WorstP95 = std::max(WorstP95, S.MeanP95);
		if (Ci++ < 2) { CloseP95 = std::max(CloseP95, S.MeanP95); }   // the passes 60 kpc from the galaxy
		Marched = std::max(Marched, S.MarchedShare);
		bOk = bOk && S.Frames == 12;
		Detail += Uni::Fmt("%s[%s] excess light %.2f%% (worst frame %.2f%%), light %.2f%%, pixel error p95 %.1f%%, %.0f%% from history, %.0f%% marched",
			Detail.empty() ? "" : "; ", C.Name, 100.0 * S.MeanExcess, 100.0 * S.WorstExcess, 100.0 * S.WorstLightPath, 100.0 * S.MeanP95, 100.0 * S.HistoryShare, 100.0 * S.MarchedShare);
	}
	Uni::Check(2, "T2.vol.temporal_fast", "D-194 temporal slicing at warp speeds (the Milky Way passed at 3 kpc a frame from 60 kpc, straight and turning, and at 12 kpc a frame from 150 kpc), against a full march every frame: with the history clamped into its fresh neighbours' range and the light's depth a harmonic mean, every frame's light within 3% of the full march (over the path's mean) and at most half the D-184 band's worst; the close passes' mean per-pixel p95 under 10%; excess light (trails, and blur where history is dropped) and the band's numbers reported")
		.Metric("worst_excess", WorstExcess).Metric("mean_excess", MeanExcess).Metric("worst_light", WorstLight).Metric("worst_mean_p95", WorstP95).Metric("marched_share", Marched)
		.Metric("band_worst_light", OldLight).Metric("band_worst_mean_p95", OldP95).Metric("band_mean_excess", OldExcess).Metric("close_mean_p95", CloseP95)
		.Pass(bOk && WorstLight < 0.03 && WorstLight < 0.5 * OldLight && CloseP95 < 0.10 && Marched < 0.26, Detail);
}

UNI_TEST(volume, interleave)
{
	// D-208 (B-017): the slicing's block grows to 3 x 3 or 4 x 4 (one pixel in 9 or 16 marched a
	// frame) while the image moves under 1.5 or 0.5 march pixels a frame (the renderer's motion
	// estimate: the turn, plus the camera's step over the nearest marched structure), and stays
	// the D-184 2 x 2 beyond. Paths of 24 frames (200 x 125, the first four filling the history),
	// the adaptive rule (at most 4) against 2 x 2, both against a full march every frame.
	FUniverseDef D = UniScenes::Flythrough();
	const int W = 200, Ht = 125;
	struct FCase { const char* Name; FCoord Cam; FV3 Look; FV3 Step; FV3 Axis; double Turn; };
	FCoord Out = FCoord::Origin();
	{ FVec3f Fr{}; AddDisplacement(Out, Fr, FVec3d{ 60.0 * Units::Kpc, 0.0, 6.0 * Units::Kpc }); }
	const double Deg = 3.14159265358979 / 180.0;
	const FCase Cases[] = {
		{ "inside, at rest", UniScenes::SolarCamera(), { -1, 0.1, 0.05 }, { 0, 0, 0 }, { 0, 0, 1 }, 0.0 },
		{ "inside, turning 0.02 deg", UniScenes::SolarCamera(), { -1, 0.1, 0.05 }, { 0, 0, 0 }, { 0, 0, 1 }, 0.02 * Deg },
		{ "inside, turning 0.2 deg", UniScenes::SolarCamera(), { -1, 0.1, 0.05 }, { 0, 0, 0 }, { 0, 0, 1 }, 0.2 * Deg },
		{ "inside, drifting 2e-3 pc", UniScenes::SolarCamera(), { -1, 0.1, 0.05 }, { 0, 2.0e-3 * Units::Pc, 0 }, { 0, 0, 1 }, 0.0 },
		{ "inside, turning 1 deg (2 x 2)", UniScenes::SolarCamera(), { -1, 0.1, 0.05 }, { 0, 0, 0 }, { 0, 0, 1 }, 1.0 * Deg },
		{ "60 kpc, turning 0.1 deg", Out, { -1, 0, -0.1 }, { 0, 0, 0 }, { 0, 0, 1 }, 0.1 * Deg },
	};
	bool bPass = true;
	std::string Detail;
	double MarchedA = 0.0, MarchedB = 0.0;
	for (const FCase& C : Cases)
	{
		const FPathStats A = FlyPath(D, C.Cam, C.Look, C.Step, C.Axis, C.Turn, 24, W, Ht, true, 2u);
		const FPathStats B = FlyPath(D, C.Cam, C.Look, C.Step, C.Axis, C.Turn, 24, W, Ht, true, 4u);
		MarchedA += A.MarchedShare; MarchedB += B.MarchedShare;
		// Bars: the light within 1%, and the per-pixel p95 within 2% plus the 2 x 2's own.
		const bool bOk = B.WorstLight < 0.01 && B.MeanP95 < A.MeanP95 + 0.02;
		bPass = bPass && bOk;
		Detail += Uni::Fmt("%s[%s: N %.1f, %.0f%% marched; light %.2f%% (2 x 2 %.2f%%), p95 %.2f%% (%.2f%%)]", Detail.empty() ? "" : "; ", C.Name, B.MeanN, 100.0 * B.MarchedShare,
			100.0 * B.WorstLight, 100.0 * A.WorstLight, 100.0 * B.MeanP95, 100.0 * A.MeanP95);
	}
	Uni::Check(2, "T2.vol.interleave", "D-208 the slicing's block grows with stillness (3 x 3 under 1.5 march px a frame, 4 x 4 under 0.5): over still and slow paths inside and outside a galaxy, the light within 1% of a full march and the per-pixel p95 within 2% of the 2 x 2's; 2 x 2 kept when moving")
		.Metric("marched_2x2", MarchedA / 6.0).Metric("marched_adaptive", MarchedB / 6.0)
		.Pass(bPass, Detail);
}

UNI_TEST(volume, temporal_resize)
{
	// D-215 (owner: frame drops to 30-40 fps while a dense region streams in; trace: the volume
	// march up to 115 ms in a frame, uu.Perf: 225 frames "not sliced: march size changed" for 225
	// auto-budget level changes). A new march size dropped the history, so that frame marched every
	// pixel: 16x the rays at 1/16. The history is now kept across a resize (within 2x), reprojected
	// at its own size. Inside the Milky Way, moving and turning, 1/16 slicing, the size changing
	// level every 4 frames (x1, x0.92, x0.84 a side): no frame marches more than a sliced frame's
	// share (the slicing follows the motion), and the light stays within the fixed-size run's error.
	const FUniverseDef D = UniScenes::GalaxyToStar();
	const FCoord Cam = UniScenes::SolarCamera();
	const FV3 Look{ 1.0, 0.3, -0.1 }, Step{ 20.0 * Units::Pc, 5.0 * Units::Pc, 0.0 }, Axis{ 0.2, 0.1, 1.0 };
	const double Turn = 0.002;
	const FPathStats Fixed = FlyPath(D, Cam, Look, Step, Axis, Turn, 20, 192, 120, true, 4, 0, true);
	const FPathStats Old = FlyPath(D, Cam, Look, Step, Axis, Turn, 20, 192, 120, true, 4, 4, false);
	const FPathStats Kept = FlyPath(D, Cam, Look, Step, Axis, Turn, 20, 192, 120, true, 4, 4, true);
	Uni::Check(2, "T2.vol.temporal_resize", "D-215 the march resized every 4 frames (the auto budget's levels) keeps its history: no frame marches more than the fixed-size run's sliced share (dropping it marched every pixel: the spike), light within the fixed-size run's error plus 1%, pixel p95 within 1.5x of it")
		.Metric("worst_marched_old", Old.WorstMarched).Metric("worst_marched_kept", Kept.WorstMarched).Metric("worst_marched_fixed", Fixed.WorstMarched)
		.Metric("light_fixed", Fixed.WorstLightPath).Metric("light_kept", Kept.WorstLightPath).Metric("p95_fixed", Fixed.MeanP95).Metric("p95_kept", Kept.MeanP95)
		.Pass(Old.WorstMarched > 0.9 && Kept.WorstMarched <= Fixed.WorstMarched + 0.01 && Kept.WorstLightPath < Fixed.WorstLightPath + 0.01 && Kept.MeanP95 < 1.5 * Fixed.MeanP95 + 0.01,
			Uni::Fmt("most of a frame marched: dropping the history at a resize %.0f%%, keeping it %.0f%% (fixed size %.0f%%); light over the path's mean, worst frame: fixed %.2f%%, resized and kept %.2f%% (dropped %.2f%%); pixel p95: fixed %.1f%%, kept %.1f%%, dropped %.1f%%; %.0f%% of reconstructed pixels from history (fixed %.0f%%)",
				100.0 * Old.WorstMarched, 100.0 * Kept.WorstMarched, 100.0 * Fixed.WorstMarched, 100.0 * Fixed.WorstLightPath, 100.0 * Kept.WorstLightPath, 100.0 * Old.WorstLightPath,
				100.0 * Fixed.MeanP95, 100.0 * Kept.MeanP95, 100.0 * Old.MeanP95, 100.0 * Kept.HistoryShare, 100.0 * Fixed.HistoryShare));
}

