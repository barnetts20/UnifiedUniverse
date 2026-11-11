// test_slide.cpp
// D-219 (owner: "substantial slice sliding in motion"; look at CloudAtmosphere). Flights inside
// and outside the Milky Way, every frame against a fine reference (every step an eighth:
// MarchFlags bit 1): each frame's error, and how much the error pattern changes from one frame to
// the next (the crawl the eye sees as sliding), for a full march every frame (step midpoints) and
// for the temporal slicing (D-184 / D-194 / D-208 / D-215), adaptive and at 2 x 2.
// Measured with this harness (D-219): the full march is within ~0.1% of the reference with
// no crawl, so the sliding is the slicing's reconstruction, not the march's sample shells;
// CloudAtmosphere's per-step jitter and accumulating resolve made both numbers worse on our
// content (Tools/UniVerify/experiments/d219_*).

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	/** The fine reference: every pixel, every step an eighth as long, midpoints. */
	std::vector<H::float4> FineMarch(const FVolumeScene& Sc, const FCamera& Cam)
	{
		FVolumeScene F = Sc;
		F.View.MarchFlags |= 2u;
		F.View.MaxSteps *= 8u;
		FTemporalVolume Full;
		Full.Step(F, Cam, FV3{ 0, 0, 0 }, false, false, 2);
		return Full.Vol;
	}

	FV3 Rotate(const FV3& V, const FV3& Axis, double A)
	{
		const FV3 K = Norm(Axis);
		const double C = std::cos(A), S = std::sin(A);
		return V * C + Cross(K, V) * S + K * (Dot(K, V) * (1.0 - C));
	}

	struct FSlide
	{
		double Error = 0.0;   // mean over frames of sum |A - R| / sum R (light-weighted relative error)
		double P95 = 0.0;     // mean over frames of the per-pixel relative error's 95th percentile
		double Crawl = 0.0;   // mean over frames of sum |E_t - E_t-1| / sum R (E = A - R): the error moving
		double Light = 0.0;   // worst frame |sum A / sum R - 1|
		int Frames = 0;
	};

	/** One flight; frames before Warm fill the history and are not compared. bSlice false: every
	 *  pixel marched every frame. MaxN: the slicing's largest block (D-208). */
	FSlide Fly(const FUniverseDef& D, FCoord Cam, FV3 Look, FV3 StepCm, FV3 Axis, double Turn, int Frames, int Warm, int W, int Ht, bool bSlice, u32 MaxN)
	{
		FUniverseModel M(D);
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		FTemporalVolume Tv;
		FSlide St;
		FV3 Fwd = Norm(Look);
		std::vector<double> PrevE;
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
			{
				// The renderer's interleave rule (D-208), as in test_temporal.
				const double ThetaPix = 2.0 * Cm.TanX() / double(W);
				const double StepLen = std::sqrt(Dot(StepCm, StepCm));
				const double Move = F > 0 && StepLen > 0.0 ? (Sc.NearCm > 0.0 ? StepLen / Sc.NearCm : 1.0e9) : 0.0;
				const double MotionPx = ((F > 0 ? std::abs(Turn) : 0.0) + Move) / ThetaPix;
				Tv.Interleave = H::Uni_TemporalInterleave(float(MotionPx), MaxN);
			}
			Tv.Step(Sc, Cm, F > 0 ? StepCm : FV3{ 0, 0, 0 }, F > 0, bSlice, 2);
			if (F < Warm - 1) { continue; }
			const std::vector<H::float4> Ref = FineMarch(Sc, Cm);
			std::vector<double> E(Ref.size()), Lb(Ref.size());
			double Sa = 0, Sb = 0, Se = 0;
			for (size_t I = 0; I < Ref.size(); ++I)
			{
				const double Om = PixelSolidAngle(Cm, W, Ht, double(I % W) + 0.5, double(I / W) + 0.5);
				const double La = double(H::Uni_Luminance(H::float3(Tv.Vol[I].x, Tv.Vol[I].y, Tv.Vol[I].z))) * Om;
				Lb[I] = double(H::Uni_Luminance(H::float3(Ref[I].x, Ref[I].y, Ref[I].z))) * Om;
				E[I] = La - Lb[I];
				Sa += La; Sb += Lb[I]; Se += std::abs(E[I]);
			}
			if (F >= Warm)
			{
				double Sc2 = 0.0;
				for (size_t I = 0; I < E.size(); ++I) { Sc2 += std::abs(E[I] - PrevE[I]); }
				St.Crawl += Sc2 / std::max(Sb, 1e-300);
				St.Error += Se / std::max(Sb, 1e-300);
				St.Light = std::max(St.Light, std::abs(Sa / std::max(Sb, 1e-300) - 1.0));
				const double Mean = Sb / double(Ref.size());
				std::vector<double> Rel;
				for (size_t I = 0; I < Ref.size(); ++I) { if (Lb[I] > 0.01 * Mean) { Rel.push_back(std::abs(E[I]) / Lb[I]); } }
				if (!Rel.empty()) { std::sort(Rel.begin(), Rel.end()); St.P95 += Rel[std::min(Rel.size() - 1, size_t(0.95 * double(Rel.size())))]; }
				++St.Frames;
			}
			PrevE = E;
		}
		if (St.Frames > 0) { St.Error /= St.Frames; St.Crawl /= St.Frames; St.P95 /= St.Frames; }
		return St;
	}
}

UNI_TEST(volume, slide)
{
	const double Kpc = Units::Kpc, Pc = Units::Pc;
	const FUniverseDef D = UniScenes::GalaxyToStar();
	const FCoord Solar = UniScenes::SolarCamera();
	FCoord Far = Solar;
	{ FVec3f Fr{}; AddDisplacement(Far, Fr, FVec3d{ 0.0, -40.0 * Kpc, 45.0 * Kpc }); }
	const double Deg = 3.14159265358979 / 180.0;
	struct FCase { const char* Name; FCoord Cam; FV3 Look; FV3 Step; FV3 Axis; double Turn; };
	const FCase Cases[] = {
		{ "inside, 3 pc/frame and turning 0.1 deg", Solar, { -1, -0.12, -0.02 }, { -3.0 * Pc, 0.4 * Pc, 0.0 }, { 0.2, 0, 1 }, 0.1 * Deg },
		{ "60 kpc, drifting 0.1 kpc/frame", Far, { 0.0, 40.0, -45.0 }, { 0.1 * Kpc, 0.0, 0.0 }, { 0, 0, 1 }, 0.0 },
		{ "60 kpc, 0.36 kpc/frame and turning 0.3 deg", Far, { 0.0, 40.0, -45.0 }, { 0.3 * Kpc, 0.2 * Kpc, 0.0 }, { 0.3, 0.2, 1 }, 0.005 },
	};
	const int W = 160, Ht = 100, Frames = 20, Warm = 8;
	std::string Detail;
	double MarchErr = 0.0, MarchCrawl = 0.0, SliceErr = 0.0, SliceCrawl = 0.0, Slice2Err = 0.0;
	for (const FCase& C : Cases)
	{
		const FSlide Full = Fly(D, C.Cam, C.Look, C.Step, C.Axis, C.Turn, Frames, Warm, W, Ht, false, 4u);
		const FSlide Sl = Fly(D, C.Cam, C.Look, C.Step, C.Axis, C.Turn, Frames, Warm, W, Ht, true, 4u);
		const FSlide Sl2 = Fly(D, C.Cam, C.Look, C.Step, C.Axis, C.Turn, Frames, Warm, W, Ht, true, 2u);
		MarchErr = std::max(MarchErr, Full.Error); MarchCrawl = std::max(MarchCrawl, Full.Crawl);
		SliceErr = std::max(SliceErr, Sl.Error); SliceCrawl = std::max(SliceCrawl, Sl.Crawl);
		Slice2Err = std::max(Slice2Err, Sl2.Error);
		Detail += Uni::Fmt("%s[%s] full march: error %.2f%% crawl %.2f%%; sliced (N up to 4): error %.2f%% p95 %.1f%% crawl %.2f%% light %.2f%%; sliced 2 x 2: error %.2f%% crawl %.2f%%",
			Detail.empty() ? "" : "; ", C.Name, 100.0 * Full.Error, 100.0 * Full.Crawl, 100.0 * Sl.Error, 100.0 * Sl.P95, 100.0 * Sl.Crawl, 100.0 * Sl.Light, 100.0 * Sl2.Error, 100.0 * Sl2.Crawl);
	}
	Uni::Check(2, "T2.vol.slide", "D-219 where the sliding in motion comes from, against a fine reference (steps an eighth): a full march every frame is within 0.5% with under 0.2% crawl on every path (the march's sample shells do not slide); the temporal slicing's error and crawl are reported, bounded at 6% (160 x 100; ~2-3% at 320-480 wide)")
		.Metric("march_error", MarchErr).Metric("march_crawl", MarchCrawl).Metric("slice_error", SliceErr).Metric("slice_crawl", SliceCrawl).Metric("slice2_error", Slice2Err)
		.Pass(MarchErr < 0.005 && MarchCrawl < 0.002 && SliceErr < 0.06 && SliceCrawl < 0.06, Detail);
}
