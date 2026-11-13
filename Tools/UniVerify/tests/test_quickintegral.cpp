// test_quickintegral.cpp
// D-229 (owner: flying into a dense cluster, galaxy detail arrives late): a galaxy becomes a volume
// carrier only once its normalising integral is known. The exact one (D-140) is ~2.2M field
// samples (~0.2-0.3 s) on a worker, four at a time; flying into a rich cluster, hundreds of
// galaxies reach the volume handoff at once and stayed plain sprites for seconds.
// FUniStreamer::VolumeIntegral now gives the volume a galaxy's quick estimate
// (FRefGenerator::ComputeIntegralQuick, the largest on screen first) until the exact one arrives,
// then eases to it. Generation never reads the quick value.
//   quick_integral  the estimate against the exact integral on a rich cluster's galaxies.
//   quick_flyin     the streamer, its jobs delayed as on a busy pool (exact 12 frames, quick 1),
//                   flying into the cluster: the on-screen galaxy area left as sprites for want of
//                   an integral, with and without the quick estimates; no carrier's amplitude
//                   steps between frames; at rest, the pool idle, every carrier ends on the exact value.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../scenes/UniScenes.h"
#include "UniStreamer.h"
#include "UniVolume.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	/** A rich cluster (M = -26, R = 0.5 Mpc, two sub-clumps) at the origin; every galaxy a member. */
	FUniverseDef RichCluster()
	{
		FUniverseDef D = UniScenes::Base();
		FAnchor A;
		A.Coord = FCoord::Origin();
		A.ArchetypeId = UniScenes::kCluster;
		A.AbsMag = -26.0f;
		A.RadiusLog2 = float(std::log2(0.5 * Units::Mpc));
		FFieldParams P = UniScenes::ClusterExemplar();
		P.V[4] = 2.0f; P.V[5] = 0.6f; P.V[6] = 0.04f; P.V[7] = 4321.0f;
		A.EmitterParams = { P };
		A.bFixedOrientation = true;
		A.Seed = 0xC1057E01u;
		D.Anchors.push_back(A);
		return D;
	}

	FCoord At(double XMpc, double YMpc, double ZMpc)
	{
		FCoord C = FCoord::Origin();
		FVec3f F{};
		AddDisplacement(C, F, FVec3d{ XMpc * Units::Mpc, YMpc * Units::Mpc, ZMpc * Units::Mpc });
		return C;
	}

	struct FFlight
	{
		double SkippedArea = 0.0, Area = 0.0;
		int MaxWaitBig = 0;
		double MaxStep = 0.0;                 // largest frame-to-frame amplitude change of a carrier
		std::map<u64, float> FinalAmp;        // at rest, by parent
		u64 QuickUsed = 0, QuickComputed = 0;
		int MaxCands = 0;
	};

	u64 KeyOf(const FObjectId& Id) { return u64(HashObject(Id)); }

	FFlight Fly(bool bQuick)
	{
		FUniverseModel M(RichCluster());
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		FCpuGenBackend Be(G, 8388608u);
		FUniStreamer S(G, Be);
		S.bVelocityLod = false;
		S.bAsyncIntegrals = true;
		S.QuickJobs = bQuick ? 2u : 0u;
		// A busy pool: an exact integral lands 12 frames after it starts (0.2 s at 60 fps), a quick
		// batch the next frame.
		int Frame = 0, ExactDelay = 12;
		std::deque<std::pair<int, std::function<void()>>> Jobs;
		S.RunJob = [&](std::function<void()> J) { Jobs.push_back({ Frame + ExactDelay, std::move(J) }); };
		S.RunQuickJob = [&](std::function<void()> J) { Jobs.push_back({ Frame + 1, std::move(J) }); };
		auto RunDue = [&]()
		{
			for (auto It = Jobs.begin(); It != Jobs.end();) { if (It->first <= Frame) { It->second(); It = Jobs.erase(It); } else { ++It; } }
		};
		FFlight Out;
		std::map<u64, float> Prev;
		std::map<u64, int> FirstSeen;
		// Flight, then rest; in the rest's second half the pool is idle (an exact integral lands the
		// next frame) so every galaxy's exact value arrives (four at a time) and is eased to.
		const int Flight = 80, Rest = 70, Idle = 120;
		for (int F = 0; F < Flight + Rest + Idle; ++F, ++Frame)
		{
			if (F == Flight + Rest) { ExactDelay = 1; }
			RunDue();
			const double D = 4.0 - 0.05 * double(std::min(F, Flight - 1));
			const FCoord Cam = At(D, 0.3 * D / 4.0, 0.1);
			S.Update(Cam);
			FCamera Cm; Cm.Coord = Cam; Cm.Width = 960; Cm.Height = 540; Cm.DeltaMag = 0.0;
			Cm.LookAlong(FV3{ -1.0, -0.075, -0.025 });
			const FVolumeScene Sc = VB.Build(S, Cam, FVec3f{}, Cm.View()).Subset(false);
			// Built carriers' amplitudes by parent (the local carriers are the last Handoffs.size()).
			std::map<u64, float> Amp;
			const size_t L0 = Sc.Carriers.size() - Sc.Handoffs.size();
			for (size_t I = 0; I < Sc.Handoffs.size(); ++I) { Amp[KeyOf(Sc.Handoffs[I].Parent)] = Sc.Carriers[L0 + I].Amplitude; }
			for (const auto& KV : Amp)
			{
				auto It = Prev.find(KV.first);
				if (It != Prev.end() && It->second > 0.0f) { Out.MaxStep = std::max(Out.MaxStep, std::abs(double(KV.second) / double(It->second) - 1.0)); }
			}
			Prev = Amp;
			if (F == Flight + Rest + Idle - 1) { Out.FinalAmp = Amp; }
			if (F >= Flight + Rest) { continue; }
			// The galaxies in the handoff (the volume builder's own rule) and which got a carrier.
			const double ThetaPix = 1.0 / Cm.FocalPx();
			const double T1 = std::min(double(H::UNI_VOL_HANDOFF_PX) * ThetaPix, double(H::UNI_THETA_PROMOTE));
			int Cands = 0;
			for (const auto& KV : S.Contributors())
			{
				const FContributor& C = KV.second;
				if (!C.Emitter || C.Emitter->FieldId != H::UNI_FIELD_GALAXY) { continue; }
				const FObjectGPU* O = S.FindObject(C.Parent);
				if (!O) { continue; }
				const double R = Length(ToDouble(Sub(Cam, C.Centre)));
				const double Th = R > 0.0 ? std::exp2(double(H::Uni_ObjectRadiusLog2(ToHlsl(*O)))) / R : 1.0;
				const double X = std::clamp((Th - 0.5 * T1) / (0.5 * T1), 0.0, 1.0);
				const double W = X * X * (3.0 - 2.0 * X);
				if (!(W > 0.05)) { continue; }
				++Cands;
				const u64 K = KeyOf(C.Parent);
				if (!FirstSeen.count(K)) { FirstSeen[K] = F; }
				const double A = W * Th * Th;
				Out.Area += A;
				if (!Amp.count(K))
				{
					Out.SkippedArea += A;
					if (Th > 2.0 * T1) { Out.MaxWaitBig = std::max(Out.MaxWaitBig, F - FirstSeen[K]); }
				}
			}
			Out.MaxCands = std::max(Out.MaxCands, Cands);
		}
		// Let the remaining jobs finish before the streamer goes.
		Frame += 100;
		RunDue();
		Out.QuickUsed = S.QuickUsed;
		Out.QuickComputed = S.QuickComputed;
		return Out;
	}
}

UNI_TEST(stream, quick_integral)
{
	FUniverseModel M(RichCluster());
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	P.Update(At(0.1, -0.05, 0.02), FVec3f{});
	std::vector<const FContributor*> Gal;
	for (const auto& KV : P.Contributors()) { if (KV.second.Emitter && KV.second.Emitter->FieldId == H::UNI_FIELD_GALAXY) { Gal.push_back(&KV.second); } }
	const size_t N = std::min<size_t>(Gal.size(), 40);
	double Mean = 0.0, Worst = 0.0, QuickMs = 0.0, ExactMs = 0.0;
	bool bSame = true;
	for (size_t I = 0; I < N; ++I)
	{
		auto T0 = std::chrono::steady_clock::now();
		const double E = FRefGenerator::ComputeIntegral(*Gal[I]);
		ExactMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
		T0 = std::chrono::steady_clock::now();
		const double Q = FRefGenerator::ComputeIntegralQuick(*Gal[I]);
		QuickMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
		bSame = bSame && Q == FRefGenerator::ComputeIntegralQuick(*Gal[I]);   // deterministic
		const double Er = std::abs(Q / E - 1.0);
		Mean += Er; Worst = std::max(Worst, Er);
	}
	Mean /= double(std::max<size_t>(N, 1));
	const double Speed = ExactMs / std::max(QuickMs, 1e-9);
	Uni::Check(2, "T2.stream.quick_integral", "D-229 a galaxy's quick integral (4096 importance samples: ball, thin disc, core) against the exact one on a rich cluster's galaxies: mean within 3%, worst within 8%, over 100x faster, the same value every time")
		.Metric("mean_err", Mean).Metric("worst_err", Worst).Metric("speedup", Speed)
		.Pass(N >= 20 && Mean < 0.03 && Worst < 0.08 && Speed > 100.0 && bSame,
			Uni::Fmt("%zu galaxies: mean %.2f%%, worst %.2f%%; %.2f ms against %.0f ms (%.0fx)", N, 100.0 * Mean, 100.0 * Worst, QuickMs / double(N), ExactMs / double(N), Speed));
}

UNI_TEST(stream, quick_flyin)
{
	const FFlight Old = Fly(false);
	const FFlight New = Fly(true);
	// At rest every carrier ends on the exact value: the same amplitudes either way.
	double WorstEnd = 0.0;
	size_t Common = 0;
	for (const auto& KV : New.FinalAmp)
	{
		auto It = Old.FinalAmp.find(KV.first);
		if (It == Old.FinalAmp.end()) { continue; }
		++Common;
		WorstEnd = std::max(WorstEnd, std::abs(double(KV.second) / double(It->second) - 1.0));
	}
	const double SkipOld = Old.SkippedArea / std::max(Old.Area, 1e-300), SkipNew = New.SkippedArea / std::max(New.Area, 1e-300);
	Uni::Check(2, "T2.stream.quick_flyin", "D-229 flying into a rich cluster with the integrals' jobs delayed (exact 12 frames, quick 1): the on-screen area of galaxies in the handoff left as sprites for want of an integral drops from most of it to under 5%, no galaxy past twice the handoff waits more than 3 frames, no carrier's amplitude changes more than 1% between frames (the ease to the exact value over 30 frames), and at rest, once the pool is idle, every carrier is on the exact value")
		.Metric("skipped_before", SkipOld).Metric("skipped_after", SkipNew).Metric("max_wait_big", double(New.MaxWaitBig)).Metric("max_amp_step", New.MaxStep).Metric("end_diff", WorstEnd)
		.Pass(SkipNew < 0.05 && SkipOld > 0.3 && New.MaxWaitBig <= 3 && New.MaxStep < 0.01 && WorstEnd < 1e-6 && Common > 20,
			Uni::Fmt("up to %d galaxies in the handoff; area left as sprites %.1f%% -> %.1f%%; longest wait past 2x the handoff %d -> %d frames; largest amplitude step a frame %.3f%% (before %.3f%%); %llu quick estimates, %llu used; at rest %zu carriers in both, worst difference %.2g",
				New.MaxCands, 100.0 * SkipOld, 100.0 * SkipNew, Old.MaxWaitBig, New.MaxWaitBig, 100.0 * New.MaxStep, 100.0 * Old.MaxStep, (unsigned long long)New.QuickComputed, (unsigned long long)New.QuickUsed, Common, WorstEnd));
}
