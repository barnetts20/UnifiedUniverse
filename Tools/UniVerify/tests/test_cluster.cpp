// test_cluster.cpp
// Galaxy clusters (D-151, D-152): the cluster field and its analytic normaliser, no reroll of
// existing content when clusters are off, a cluster's members (count, light, profile), the
// census of a web scene (density, placement on the web, residency), and reference stills.

#include "../src/UniTest.h"
#include "../scenes/UniScenes.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../src/UniVerifyCommon.h"
#include "UniStreamer.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sys/stat.h>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	/** The web scene as it was before D-151: three archetypes and the galaxy web only. */
	FUniverseDef LegacyWeb()
	{
		FUniverseDef D = UniScenes::Base();
		D.Archetypes.resize(3);
		D.RootEmitters.push_back(UniScenes::WebEmitter());
		return D;
	}

	FUniverseDef LegacyGalaxyToStar()
	{
		FUniverseDef D = UniScenes::Base();
		D.Archetypes.resize(3);
		D.Anchors.push_back(UniScenes::MilkyWayAnchor());
		return D;
	}

	/** A cluster anchored at the origin, M = -26, R = 0.5 Mpc, with two sub-clumps; no field
	 *  galaxies, so every galaxy is a member. */
	FUniverseDef AnchoredCluster(double AbsMag = -26.0)
	{
		FUniverseDef D = UniScenes::Base();
		FAnchor A;
		A.Coord = FCoord::Origin();
		A.ArchetypeId = UniScenes::kCluster;
		A.AbsMag = float(AbsMag);
		A.RadiusLog2 = float(std::log2(0.5 * Units::Mpc));
		FFieldParams P = UniScenes::ClusterExemplar();
		P.V[4] = 2.0f; P.V[5] = 0.6f; P.V[6] = 0.04f; P.V[7] = 4321.0f;
		A.EmitterParams = { P };
		A.bFixedOrientation = true;
		A.Seed = 0xC1057E01u;
		D.Anchors.push_back(A);
		return D;
	}

	std::map<std::string, std::vector<FObjectGPU>> Snapshot(const FRefPipeline& P)
	{
		std::map<std::string, std::vector<FObjectGPU>> Out;
		for (const auto& KV : P.CacheRef())
		{
			const std::string Key = std::to_string(KV.first.Level) + ToHex(KV.first.C[0]) + ToHex(KV.first.C[1]) + ToHex(KV.first.C[2]);
			Out[Key] = KV.second.Objects;
		}
		return Out;
	}

	bool SameRecords(const std::map<std::string, std::vector<FObjectGPU>>& A, const std::map<std::string, std::vector<FObjectGPU>>& B, size_t& OutCells, size_t& OutDiff)
	{
		OutCells = 0; OutDiff = 0;
		for (const auto& KV : A)
		{
			++OutCells;
			auto It = B.find(KV.first);
			const bool bSame = It != B.end() && It->second.size() == KV.second.size() && (KV.second.empty() || std::memcmp(KV.second.data(), It->second.data(), KV.second.size() * sizeof(FObjectGPU)) == 0);
			OutDiff += bSame ? 0 : 1;
		}
		OutDiff += (A.size() == B.size()) ? 0 : 1;
		return OutDiff == 0;
	}

	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
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

	float AutoGainStill(FImage& Img)
	{
		std::vector<float> L;
		for (size_t I = 0; I + 2 < Img.Rgb.size(); I += 3) { L.push_back(0.2126f * Img.Rgb[I] + 0.7152f * Img.Rgb[I + 1] + 0.0722f * Img.Rgb[I + 2]); }
		std::sort(L.begin(), L.end());
		const float P = L[size_t(double(L.size() - 1) * 0.995)];
		const float G = P > 0.0f ? 1.5f / P : 1.0f;
		for (float& V : Img.Rgb) { V *= G; }
		return G;
	}

	FCoord Moved(const FCoord& C, const FVec3d& D) { FCoord R = C; FVec3f F{}; AddDisplacement(R, F, D); return R; }
}

UNI_TEST(cluster, field)
{
	// The cluster field: zero outside its cut, the beta-model profile along an axis, and the
	// analytic normaliser (ClusterFieldIntegral) against a stratified Monte Carlo integral, over
	// random shapes with and without sub-clumps.
	Uni::FTestRng Rng(0xC1A5ull);
	double WorstInt = 0.0, WorstProfile = 0.0, MeanSigned = 0.0;
	long long Outside = 0;
	for (int Case = 0; Case < 24; ++Case)
	{
		FFieldParams Fp;
		Fp.V[0] = float(0.05 + 0.1 * Rng.Unit());
		Fp.V[1] = float(0.6 + 0.4 * Rng.Unit());
		Fp.V[2] = float(0.45 + 0.45 * Rng.Unit());
		Fp.V[3] = 1.0f;
		Fp.V[4] = float(Case % 4);
		Fp.V[5] = float(0.6 * Rng.Unit());
		Fp.V[6] = float(0.025 + 0.035 * Rng.Unit());
		Fp.V[7] = float(std::floor(Rng.Unit() * 1.0e6));
		const H::FUniFieldParams P = Fp.ToHlsl();
		const double Analytic = ClusterFieldIntegral(P);
		const int G = 40, S = 6;
		const double Cell = 2.0 / G;
		double Sum = 0.0;
		for (int Z = 0; Z < G; ++Z) for (int Y = 0; Y < G; ++Y) for (int X = 0; X < G; ++X)
		{
			for (int K = 0; K < S; ++K)
			{
				const H::float3 Pt(float(-1.0 + (X + Rng.Unit()) * Cell), float(-1.0 + (Y + Rng.Unit()) * Cell), float(-1.0 + (Z + Rng.Unit()) * Cell));
				const float V = H::Uni_FieldSample(H::UNI_FIELD_CLUSTER, Pt, P);
				if (V > 0.0f && H::dot(Pt, Pt) >= 1.0f) { ++Outside; }
				Sum += double(V);
			}
		}
		const double Mc = Sum / S * Cell * Cell * Cell;
		WorstInt = std::max(WorstInt, std::abs(Mc / Analytic - 1.0));
		MeanSigned += (Mc / Analytic - 1.0) / 24.0;
		// Profile along x with no clumps: (1 + x^2 / rc^2)^-1.5 inside the taper (0.65).
		FFieldParams Q = Fp; Q.V[4] = 0.0f;
		for (double Xv : { 0.0, 0.05, 0.2, 0.4, 0.6 })
		{
			const double Want = std::pow(1.0 + Xv * Xv / (double(Q.V[0]) * double(Q.V[0])), -1.5);
			const double Got = double(H::Uni_FieldSample(H::UNI_FIELD_CLUSTER, H::float3(float(Xv), 0.0f, 0.0f), Q.ToHlsl()));
			WorstProfile = std::max(WorstProfile, std::abs(Got / Want - 1.0));
		}
	}
	// The Monte Carlo itself scatters by ~0.7% for the sharpest cores: bound the bias (mean over
	// the 24 shapes) tightly and each shape loosely.
	Uni::Check(1, "T1.field.cluster", "Cluster field (D-151): analytic normaliser unbiased against a 384k-sample integral over 24 shapes (mean within 0.4%, each within 2.5%), beta profile exact on axis, zero outside the unit ball")
		.Metric("worst_integral", WorstInt).Metric("mean_signed", MeanSigned).Metric("worst_profile", WorstProfile).Metric("outside", double(Outside))
		.Pass(WorstInt < 0.025 && std::abs(MeanSigned) < 0.004 && WorstProfile < 1.0e-5 && Outside == 0,
			Uni::Fmt("analytic vs sampled integral: mean %+.2f%%, worst %.2f%%; worst on-axis profile error %.1e; %lld non-zero samples outside the unit ball", 100.0 * MeanSigned, 100.0 * WorstInt, WorstProfile, Outside));
}

UNI_TEST(cluster, box)
{
	// D-153: the 32 x 32 high-product is exact, and box mapping keeps every offset inside its box
	// and is the identity for a whole axis.
	Uni::FTestRng Rng(0xB0C5ull);
	long long Wrong = 0, Outside = 0, NotIdentity = 0;
	for (int I = 0; I < 2000000; ++I)
	{
		const u32 A = Rng.U32(), B = Rng.U32();
		const u32 Want = u32((u64(A) * u64(B)) >> 32);
		if (H::Uni_MulHi32(A, B) != Want) { ++Wrong; }
		const u32 Lo = Rng.U32() >> 1;
		const u32 Span = std::max<u32>(1u, Rng.U32() % (0xFFFFFFFFu - Lo));
		const u32 M = H::Uni_BoxMap(A, Lo, Span);
		if (M < Lo || u64(M) >= u64(Lo) + u64(Span)) { ++Outside; }
		if (H::Uni_BoxMap(A, Lo, 0u) != A) { ++NotIdentity; }
	}
	Uni::Check(1, "T1.box.map", "Sampling box (D-153): 2M random high-products exact, every mapped offset inside its box, span 0 the identity")
		.Metric("wrong", double(Wrong)).Metric("outside", double(Outside)).Metric("not_identity", double(NotIdentity))
		.Pass(Wrong == 0 && Outside == 0 && NotIdentity == 0, Uni::Fmt("%lld products wrong, %lld offsets outside, %lld identity failures", Wrong, Outside, NotIdentity));
}

UNI_TEST(cluster, no_reroll)
{
	// D-151 and D-152 must not move any existing content: with clusters off, the web scene is
	// the pre-D-151 web scene record for record; the galaxy-to-star scene is unchanged too.
	size_t CellsW = 0, DiffW = 0, CellsG = 0, DiffG = 0;
	{
		UniScenes::FClusterTuning Off;
		Off.bEnabled = false;
		FUniverseModel Ma(LegacyWeb()), Mb(UniScenes::WebToGalaxy(Off));
		FRefGenerator Ga(Ma), Gb(Mb);
		FRefPipeline Pa(Ga), Pb(Gb);
		Pa.bVelocityLod = Pb.bVelocityLod = false;
		Pa.Update(UniScenes::VoidCamera(), FVec3f{});
		Pb.Update(UniScenes::VoidCamera(), FVec3f{});
		SameRecords(Snapshot(Pa), Snapshot(Pb), CellsW, DiffW);
	}
	{
		FUniverseModel Ma(LegacyGalaxyToStar()), Mb(UniScenes::GalaxyToStar());
		FRefGenerator Ga(Ma), Gb(Mb);
		FRefPipeline Pa(Ga), Pb(Gb);
		Pa.bVelocityLod = Pb.bVelocityLod = false;
		Pa.Update(UniScenes::SolarCamera(), FVec3f{});
		Pb.Update(UniScenes::SolarCamera(), FVec3f{});
		SameRecords(Snapshot(Pa), Snapshot(Pb), CellsG, DiffG);
	}
	Uni::Check(2, "T2.cluster.no_reroll", "Clusters off reproduces the pre-D-151 web scene record for record; the galaxy-to-star scene is unchanged (D-152 moves no existing level)")
		.Metric("web_cells", double(CellsW)).Metric("web_diff", double(DiffW)).Metric("star_cells", double(CellsG)).Metric("star_diff", double(DiffG))
		.Pass(CellsW > 100 && CellsG > 100 && DiffW == 0 && DiffG == 0,
			Uni::Fmt("web: %zu cells, %zu differ; galaxy to star: %zu cells, %zu differ", CellsW, DiffW, CellsG, DiffG));
}

UNI_TEST(cluster, members)
{
	// An anchored rich cluster (M = -26) with no field galaxies, camera near its centre so every
	// member level is resident across its influence sphere. Members: count against the budget's
	// expectation (Poisson), summed light against the cluster's own magnitude (the members'
	// own variance), and the radial distribution against the field's (KS: envelope clipping in
	// coarse cells would flatten the core).
	FUniverseModel M(AnchoredCluster());
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = Moved(FCoord::Origin(), FVec3d{ 0.1 * Units::Mpc, -0.05 * Units::Mpc, 0.02 * Units::Mpc });
	P.Update(Cam, FVec3f{});
	const FAnchor& A = M.Definition().Anchors[0];
	const double Radius = std::exp2(double(A.RadiusLog2));
	const double Influence = UniScenes::MakeCluster().Emitters[0].InfluenceMul * Radius;
	const double Expected = UniScenes::MakeCluster().Emitters[0].NTotal * std::pow(10.0, -0.4 * (double(A.AbsMag) - (-26.0)));
	u32 ClusterId = 0;
	for (const auto& KV : P.Contributors()) { if (KV.second.ParentArchetypeId == UniScenes::kCluster) { ClusterId = KV.first; } }
	std::vector<double> Radii;
	double Flux = 0.0, Flux2 = 0.0;
	u64 Clipped = 0, Candidates = 0;
	for (const auto& KV : P.CacheRef())
	{
		const FCellContent& C = KV.second;
		for (const FTaskStats& T : C.Tasks) { if (T.ContributorId == ClusterId) { Clipped += T.Clipped; Candidates += T.Candidates; } }
		for (size_t I = 0; I < C.Objects.size(); ++I)
		{
			const FObjectGPU& O = C.Objects[I];
			if ((O.A[3] & 0xFFFFu) != UniScenes::kGalaxy || C.ObjectContributor[I] != ClusterId) { continue; }
			if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const FVec3d D = ToDouble(Sub(ObjectCoord(KV.first, Off), A.Coord));
			Radii.push_back(Length(D) / Influence);
			const double L = std::pow(10.0, -0.4 * double(H::Uni_ObjectAbsMag(ToHlsl(O))));
			Flux += L; Flux2 += L * L;
		}
	}
	const double N = double(Radii.size());
	const double MSum = -2.5 * std::log10(Flux);
	const double Lwant = std::pow(10.0, -0.4 * double(A.AbsMag));
	const double ZFlux = (Flux - Lwant) / std::sqrt(std::max(Flux2, 1e-300));
	// Expected radial CDF of the field (same parameters, identity rotation), by Monte Carlo.
	const H::FUniFieldParams Fp = A.EmitterParams[0].ToHlsl();
	Uni::FTestRng Rng(0x515Eull);
	std::vector<std::pair<double, double>> Mc;
	double Wsum = 0.0;
	for (int I = 0; I < 400000; ++I)
	{
		H::float3 Pt(0, 0, 0);
		do { Pt = H::float3(float(2 * Rng.Unit() - 1), float(2 * Rng.Unit() - 1), float(2 * Rng.Unit() - 1)); } while (H::dot(Pt, Pt) >= 1.0f);
		const double W = double(H::Uni_FieldSample(H::UNI_FIELD_CLUSTER, Pt, Fp));
		Mc.push_back({ std::sqrt(double(H::dot(Pt, Pt))), W });
		Wsum += W;
	}
	std::sort(Mc.begin(), Mc.end());
	std::sort(Radii.begin(), Radii.end());
	double Ks = 0.0, Acc = 0.0;
	size_t J = 0;
	for (size_t I = 0; I < Radii.size(); ++I)
	{
		while (J < Mc.size() && Mc[J].first <= Radii[I]) { Acc += Mc[J].second; ++J; }
		const double Fm = Acc / Wsum;
		Ks = std::max({ Ks, std::abs(Fm - double(I) / N), std::abs(Fm - double(I + 1) / N) });
	}
	const double KsLimit = 1.63 / std::sqrt(std::max(N, 1.0));   // 99%
	std::string Prof;
	for (double Rq : { 0.03, 0.06, 0.1, 0.2, 0.35, 0.6 })
	{
		double Am = 0.0; for (const auto& Pm : Mc) { if (Pm.first <= Rq) { Am += Pm.second; } else { break; } }
		const double Fm = double(std::lower_bound(Radii.begin(), Radii.end(), Rq) - Radii.begin()) / std::max(N, 1.0);
		Prof += Uni::Fmt(" r<%.2f: %.3f vs %.3f;", Rq, Fm, Am / Wsum);
	}
	std::map<int, int> ByLevel;
	for (const auto& KV : P.CacheRef()) { for (size_t I = 0; I < KV.second.Objects.size(); ++I) { if (KV.second.ObjectContributor[I] == ClusterId) { ++ByLevel[KV.first.Level]; } } }
	for (const auto& KV : ByLevel) { Prof += Uni::Fmt(" L%d:%d", KV.first, KV.second); }
	Uni::Check(2, "T2.cluster.members", "A rich cluster's members (D-151): count within 4 sigma of its budget, summed light within 4 sigma of its magnitude, radial profile matches the field (KS 99%)")
		.Metric("members", N).Metric("expected", Expected).Metric("summed_mag", MSum).Metric("z_flux", ZFlux).Metric("ks", Ks).Metric("ks_limit", KsLimit)
		.Metric("clipped_fraction", Candidates ? double(Clipped) / double(Candidates) : 0.0)
		.Pass(ClusterId != 0 && std::abs(N - Expected) < 4.0 * std::sqrt(Expected) && std::abs(ZFlux) < 4.0 && Ks < KsLimit,
			Uni::Fmt("%.0f members (expected %.0f); summed M = %.2f for a cluster of M = %.2f (%.1f sigma); radial KS %.3f (99%% limit %.3f); %.2f%% of candidates above the envelope; cumulative measured vs field:%s",
				N, Expected, MSum, double(A.AbsMag), ZFlux, Ks, KsLimit, Candidates ? 100.0 * double(Clipped) / double(Candidates) : 0.0, Prof.c_str()));
}

UNI_TEST(cluster, census)
{
	// The default web scene from intergalactic space: cluster number density over the resident
	// cluster cells against the emitter's (they span thousands of periods, so the mean is exact
	// up to Poisson), placement on the web (the galaxy web's density at clusters vs at field
	// galaxies vs at random), the members' share of all galaxies (analytic), and residency.
	const UniScenes::FClusterTuning T;
	FUniverseModel M(UniScenes::WebToGalaxy(T));
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	P.Update(UniScenes::VoidCamera(), FVec3f{});
	const FUniverseDef& D = M.Definition();
	const FUniEmitter& Field = D.RootEmitters[0];
	const FUniEmitter& Clu = D.RootEmitters[1];
	const double Members = UniScenes::MeanClusterMembers(D.Arch(UniScenes::kCluster));
	const double Share = Clu.NTotal * Members / (Field.NTotal + Clu.NTotal * Members);
	// Web density (the galaxy web's own parameters) at a coordinate.
	const H::FUniFieldParams Wp = Field.Exemplar.ToHlsl();
	const double Period = std::ldexp(1.0, Field.PeriodBits);
	auto WebAt = [&](const FCoord& C)
	{
		float Pf[3];
		for (int Ax = 0; Ax < 3; ++Ax) { Pf[Ax] = float(U128ToDouble(MaskLow(C.Axis(Ax), Field.PeriodBits)) / Period); }
		return double(H::Uni_FieldSample(H::UNI_FIELD_WEB, H::float3(Pf[0], Pf[1], Pf[2]), Wp));
	};
	long long Clusters = 0, FieldGal = 0;
	double WebC = 0.0, WebF = 0.0, ClusterVolume = 0.0;
	for (const auto& KV : P.CacheRef())
	{
		for (size_t I = 0; I < KV.second.Objects.size(); ++I)
		{
			const FObjectGPU& O = KV.second.Objects[I];
			if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
			const u32 Arch = O.A[3] & 0xFFFFu;
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			if (Arch == UniScenes::kCluster) { ++Clusters; WebC += WebAt(ObjectCoord(KV.first, Off)); }
			else if (Arch == UniScenes::kGalaxy && FieldGal < 200000)
			{
				// Field galaxies: placed by a root emitter, not by a cluster contributor.
				const auto It = P.Contributors().find(KV.second.ObjectContributor[I]);
				const bool bMember = It != P.Contributors().end() && It->second.ParentArchetypeId == UniScenes::kCluster;
				if (!bMember) { ++FieldGal; WebF += WebAt(ObjectCoord(KV.first, Off)); }
			}
		}
		if (KV.first.Level == M.LevelOf(UniScenes::kCluster, -26.0))
		{
			const double W = CellWidthCm(KV.first.Level) / Units::Mpc;
			ClusterVolume += W * W * W;
		}
	}
	Uni::FTestRng Rng(0xBEEFull);
	double WebR = 0.0;
	const int NR = 20000;
	for (int I = 0; I < NR; ++I)
	{
		const H::float3 Pt(float(Rng.Unit()), float(Rng.Unit()), float(Rng.Unit()));
		WebR += double(H::Uni_FieldSample(H::UNI_FIELD_WEB, Pt, Wp));
	}
	WebR /= NR;
	const double Density = ClusterVolume > 0.0 ? double(Clusters) / ClusterVolume : 0.0;
	const double Want = Clu.NTotal * std::pow(Units::Mpc, 3.0);
	const double Sig = std::sqrt(std::max(double(Clusters), 1.0)) / std::max(ClusterVolume, 1e-300);
	const double BiasC = Clusters ? WebC / double(Clusters) / WebR : 0.0;
	const double BiasF = FieldGal ? WebF / double(FieldGal) / WebR : 0.0;
	Uni::Check(2, "T2.cluster.census", "Web scene with clusters (D-151): cluster density matches the emitter (4 sigma + 2%), clusters sit on denser web than field galaxies, members are Fraction of all galaxies, residency bounded (D-152)")
		.Metric("clusters_resident", double(Clusters)).Metric("density", Density).Metric("density_expected", Want).Metric("bias_clusters", BiasC).Metric("bias_field", BiasF)
		.Metric("member_share", Share).Metric("contributors", double(P.Contributors().size()))
		.Pass(Clusters > 1000 && std::abs(Density - Want) < 4.0 * Sig + 0.02 * Want && BiasC > 1.5 * BiasF && std::abs(Share - T.Fraction) < 1e-9 && Clusters < 50000,
			Uni::Fmt("%lld clusters resident over %.3g Mpc^3: %.3g per Mpc^3 (emitter %.3g); web density at clusters %.2fx the mean, at field galaxies %.2fx; members %.1f%% of all galaxies (handle %.1f%%), %.0f per cluster on average; %zu contributors",
				Clusters, ClusterVolume, Density, Want, BiasC, BiasF, 100.0 * Share, 100.0 * T.Fraction, Members, P.Contributors().size()));
}

UNI_TEST(cluster, stills)
{
	// Reference stills with the volume: the anchored rich cluster from 8 Mpc at the naked-eye
	// limit and with +2.5 mag of sensitivity, and the same cluster from inside its core.
	FUniverseModel M(AnchoredCluster());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	struct FShot { const char* Name; FVec3d Offset; double Dm; const char* Title; };
	const double Mpc = Units::Mpc;
	const FShot Shots[] = {
		{ "still_cluster_8mpc", { 6.0 * Mpc, -4.0 * Mpc, 3.0 * Mpc }, 0.0, "Rich cluster (M = -26, ~490 galaxies) from 8 Mpc, naked-eye limit" },
		{ "still_cluster_8mpc_dm2p5", { 6.0 * Mpc, -4.0 * Mpc, 3.0 * Mpc }, 2.5, "Same, +2.5 mag sensitivity (travel)" },
		{ "still_cluster_core", { 0.4 * Mpc, 0.25 * Mpc, 0.1 * Mpc }, 0.0, "Inside the cluster, 0.5 Mpc from its centre" },
	};
	int Written = 0, Visible[3] = { 0, 0, 0 };
	std::string Detail;
	for (int K = 0; K < 3; ++K)
	{
		const FShot& S = Shots[K];
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.DeltaMag = S.Dm;
		const FCoord Cam = Moved(FCoord::Origin(), S.Offset);
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 480; Cm.Height = 270; Cm.DeltaMag = S.Dm;
		Cm.LookAlong({ -S.Offset.X, -S.Offset.Y, -S.Offset.Z });
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View());
		FImage Img(Cm.Width, Cm.Height);
		FVolumeStats Vs;
		const FFrameStats St = RenderFrameWithVolume(P, Sc, Cm, Img, &Vs);
		const float Gn = AutoGainStill(Img);
		Visible[K] = int(St.Visible);
		WriteFrame(Img, S.Name, { S.Title, Uni::Fmt("galaxies visible %zu of %zu resident; carriers %zu; display gain %.3g", St.Visible, St.Resident, Sc.Carriers.size(), Gn) });
		Detail += Uni::Fmt("%s%s: %zu visible of %zu resident", Detail.empty() ? "" : "; ", S.Name + 6, St.Visible, St.Resident);
		++Written;
	}
	Uni::Check(3, "T3.cluster.stills", "Reference stills of a rich cluster with the volume (8 Mpc at dm 0 and +2.5, inside the core): dozens of members resolved from 8 Mpc, over a hundred from inside")
		.Metric("frames", Written).Metric("visible_dm0", Visible[0]).Metric("visible_dm2p5", Visible[1]).Metric("visible_core", Visible[2])
		.Pass(Written == 3 && Visible[0] > 50 && Visible[1] >= Visible[0] && Visible[2] > 100, Detail);
}

#include "UniTravel.h"

UNI_TEST(cluster, travel)
{
	// D-154: travel sensitivity. No boost below its start speed, the full boost above its full
	// speed, monotone in between, and rate-limited (2 mag/s up, 1 down) over a scripted trip:
	// accelerate from rest to 30 Mpc/s in 2 s, cruise, stop dead.
	FTravelSensitivity T;
	double Prev = -1.0, WorstRate = 0.0;
	int NonMono = 0;
	for (int I = 0; I <= 400; ++I)
	{
		const double V = T.StartCmPerS * std::pow(10.0, -1.0 + 4.0 * I / 400.0);
		const double Tg = T.Target(V);
		if (Tg < Prev - 1e-12) { ++NonMono; }
		Prev = Tg;
	}
	const double Below = T.Target(0.5 * T.StartCmPerS), Above = T.Target(3.0 * T.FullCmPerS);
	const double Dt = 1.0 / 60.0;
	double Peak = 0.0, AfterStop = 0.0, Last = 0.0;
	for (int F = 0; F < 60 * 8; ++F)
	{
		const double Tm = F * Dt;
		const double V = Tm < 2.0 ? 1.0e26 * Tm / 2.0 : (Tm < 5.0 ? 1.0e26 : 0.0);
		const double Dm = T.Update(V, Dt);
		WorstRate = std::max(WorstRate, std::abs(Dm - Last) / Dt);
		Last = Dm;
		Peak = std::max(Peak, Dm);
		if (Tm > 5.0 + 2.6) { AfterStop = std::max(AfterStop, Dm); }
	}
	Uni::Check(1, "T1.travel.sensitivity", "Travel sensitivity (D-154): 0 below 100 kpc/s, +2.5 above 10 Mpc/s, monotone, at most 2 mag/s, back to 0 within 2.6 s of stopping")
		.Metric("below", Below).Metric("above", Above).Metric("non_monotone", NonMono).Metric("worst_rate", WorstRate).Metric("peak", Peak).Metric("after_stop", AfterStop)
		.Pass(Below == 0.0 && std::abs(Above - 2.5) < 1e-12 && NonMono == 0 && WorstRate <= 2.0 + 1e-9 && std::abs(Peak - 2.5) < 1e-9 && AfterStop == 0.0,
			Uni::Fmt("target %.2f below start, %.2f above full, %d non-monotone steps; scripted trip: peak %+.2f, worst rate %.2f mag/s, %.2f left 2.6 s after stopping", Below, Above, NonMono, Peak, WorstRate, AfterStop));
}

UNI_TEST(cluster, volume_cost)
{
	// D-160 diagnostic: the volume march's cost around a rich cluster, split by carrier kind, on
	// the CPU reference (the GPU runs the same core). Owner, in the engine: 42.7 ms of a 45.6 ms
	// frame in Uni.Volume.March at 967 x 636 with 30 carriers; 70 ms GPU after D-157. Three
	// cameras: 8 Mpc out, where uu.GotoCluster lands (3 influence radii), and inside the core.
	// Reports carriers, rays that hit one, steps per such ray and time; passes when it ran.
	FUniverseModel M(AnchoredCluster());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const double Mpc = Units::Mpc;
	double Influence = 0.0;
	{
		FRefPipeline P0(G);
		P0.bVelocityLod = false;
		P0.Update(Moved(FCoord::Origin(), FVec3d{ 6.0 * Mpc, -4.0 * Mpc, 3.0 * Mpc }), FVec3f{});
		for (const auto& KV : P0.Contributors()) { if (KV.second.Emitter && M.Definition().Arch(KV.second.Emitter->ChildArchetypeId).Name == "Galaxy" && KV.second.InfluenceCm > Influence && Length(ToDouble(Sub(KV.second.Centre, FCoord::Origin()))) < 1.0) { Influence = KV.second.InfluenceCm; } }
	}
	if (!(Influence > 0.0)) { Influence = 4.0 * Mpc; }
	const FVec3d Dir{ 0.7682212795973759, -0.5121475197315839, 0.3841106397986879 };
	struct FShot { const char* Name; double Dist; };
	const FShot Shots[] = { { "8 Mpc", 8.0 * Mpc }, { "GotoCluster (3 influence radii)", 3.0 * Influence }, { "core (0.5 Mpc)", 0.5 * Mpc } };
	std::string Detail;
	double MsWorst = 0.0, StepsPerRayWorst = 0.0;
	for (const FShot& Sh : Shots)
	{
		const FVec3d Off{ Dir.X * Sh.Dist, Dir.Y * Sh.Dist, Dir.Z * Sh.Dist };
		FRefPipeline P(G);
		P.bVelocityLod = false;
		const FCoord Cam = Moved(FCoord::Origin(), Off);
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 484; Cm.Height = 318; Cm.DeltaMag = 0.0;   // half of the engine's 967 x 636 march
		Cm.LookAlong({ -Off.X, -Off.Y, -Off.Z });
		const FVolumeScene Local = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
		int Clusters = 0, Galaxies = 0;
		for (const FCarrierGPU& C : Local.Carriers) { Clusters += (C.FieldId == H::UNI_FIELD_CLUSTER && C.Weight > 0.0f) ? 1 : 0; Galaxies += (C.FieldId == H::UNI_FIELD_GALAXY && C.Weight > 0.0f) ? 1 : 0; }
		std::string Parts;
		for (int K = 0; K < 3; ++K)
		{
			FVolumeScene S = Local;
			if (K > 0) { for (FCarrierGPU& C : S.Carriers) { if (C.FieldId != (K == 1 ? H::UNI_FIELD_CLUSTER : H::UNI_FIELD_GALAXY)) { C.Weight = 0.0f; } } }
			FImage Img(Cm.Width, Cm.Height);
			const auto T0 = std::chrono::steady_clock::now();
			const FVolumeStats St = RenderVolume(S, Cm, Img, nullptr, 1);
			const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
			const double PerRay = St.RaysWithCarrier ? double(St.Steps) / double(St.RaysWithCarrier) : 0.0;
			if (K == 0) { MsWorst = std::max(MsWorst, Ms); StepsPerRayWorst = std::max(StepsPerRayWorst, PerRay); }
			Parts += Uni::Fmt("%s%s %.0f ms, %.0f%% of rays hit, %.1f steps/ray (max %u), %zu supersampled, %zu truncated", Parts.empty() ? "" : " | ", K == 0 ? "all" : (K == 1 ? "cluster" : "galaxies"), Ms,
				100.0 * double(St.RaysWithCarrier) / double(std::max<size_t>(St.Rays, 1)), PerRay, St.MaxSteps, St.Supersampled, St.Truncated);
		}
		Detail += Uni::Fmt("%s[%s, %s: %d cluster + %d galaxy carriers] %s", Detail.empty() ? "" : "; ", Sh.Name, FormatDistance(Sh.Dist).c_str(), Clusters, Galaxies, Parts.c_str());
	}
	Uni::Check(2, "T2.vol.cost", "Diagnostic (D-160): volume march cost around a rich cluster on the CPU reference (8 Mpc, uu.GotoCluster's landing, inside the core), by carrier kind")
		.Metric("ms_worst", MsWorst).Metric("steps_per_ray_worst", StepsPerRayWorst).Metric("influence_mpc", Influence / Mpc)
		.Pass(MsWorst > 0.0, Detail);
}

UNI_TEST(cluster, volume_equivalence)
{
	// D-160: the volume speed-ups keep the light. From inside the rich cluster's core (the worst
	// case: 1 cluster + 63 galaxy carriers, every ray in the cluster), the D-157 march (from 1 pc,
	// galaxies supersampled) against the D-160 one (emission-only carriers from where their LUT
	// is significant, galaxies prefiltered over the pixel footprint): total light and the light
	// of the galaxy-covered pixels agree, and the cluster alone matches to rounding.
	// D-163: the galaxies' emission (dust off) must match within 5%; with dust, within 8%. A
	// prefiltered (vertically widened) dust lane mixes with the widened stars instead of screening
	// the far half of the disc, so it absorbs less: 10% of the light against 19% resolved, at this
	// resolution. That is the prefilter's known bias (D-160: +4%), larger since D-161/D-163 because
	// sub-pixel bulges now count (they sit above the lane).
	FUniverseModel M(AnchoredCluster());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const double Mpc = Units::Mpc;
	const FVec3d Dir{ 0.7682212795973759, -0.5121475197315839, 0.3841106397986879 };
	const FVec3d Off{ Dir.X * 0.5 * Mpc, Dir.Y * 0.5 * Mpc, Dir.Z * 0.5 * Mpc };
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = Moved(FCoord::Origin(), Off);
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 242; Cm.Height = 159; Cm.DeltaMag = 0.0;
	Cm.LookAlong({ -Off.X, -Off.Y, -Off.Z });
	FVolumeSettings Old; Old.bLutStart = false; Old.bPrefilter = false;
	const FVolumeSettings New;
	auto Render = [&](const FVolumeSettings& S, u32 OnlyField, FImage& Img, bool bNoDust = false)
	{
		FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(false);
		if (OnlyField != ~0u) { for (FCarrierGPU& C : Sc.Carriers) { if (C.FieldId != OnlyField) { C.Weight = 0.0f; } } }
		if (bNoDust) { for (FCarrierGPU& C : Sc.Carriers) { C.Flags &= ~H::UNI_CARRIER_DUST; } }
		const auto T0 = std::chrono::steady_clock::now();
		const FVolumeStats St = RenderVolume(Sc, Cm, Img, nullptr, 2);
		return std::make_pair(St, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count());
	};
	FImage A(Cm.Width, Cm.Height), B(Cm.Width, Cm.Height), Ca(Cm.Width, Cm.Height), Cb(Cm.Width, Cm.Height), Ga(Cm.Width, Cm.Height), Gb(Cm.Width, Cm.Height);
	const auto Ra = Render(Old, ~0u, A), Rb = Render(New, ~0u, B);
	Render(Old, H::UNI_FIELD_CLUSTER, Ca); Render(New, H::UNI_FIELD_CLUSTER, Cb);
	Render(Old, H::UNI_FIELD_GALAXY, Ga); Render(New, H::UNI_FIELD_GALAXY, Gb);
	FImage Ea(Cm.Width, Cm.Height), Eb(Cm.Width, Cm.Height);
	Render(Old, H::UNI_FIELD_GALAXY, Ea, true); Render(New, H::UNI_FIELD_GALAXY, Eb, true);
	auto Sum = [](const FImage& I) { double S = 0.0; for (float V : I.Rgb) { S += double(V); } return S; };
	// UNI_VOL_CONVERGE=1: the galaxies' light at 2x and 4x resolution, old and new (UNI_NODUST=1:
	// without dust). Note the carrier set changes with resolution (galaxies under ~8 pixels are
	// sprites, D-161), so the sums rise with it; compare old and new at the same resolution.
	// D-163 measurements (galaxies only): with dust 959 / 1022 (x1), 959 / 1015 (x2), 1048 / 1082
	// (x4); without dust 1179 / 1139 (x1), 1182 / 1175 (x2): emission converges, dust does not.
	if (std::getenv("UNI_VOL_CONVERGE"))
	{
		for (int Mul : { 2, 4 })
		{
			FCamera C2 = Cm; C2.Width = Cm.Width * Mul; C2.Height = Cm.Height * Mul;
			for (int K = 0; K < 2; ++K)
			{
				FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, C2.View(), K == 0 ? Old : New).Subset(false);
				for (FCarrierGPU& C : Sc.Carriers) { if (C.FieldId != H::UNI_FIELD_GALAXY) { C.Weight = 0.0f; } if (std::getenv("UNI_NODUST")) { C.Flags &= ~H::UNI_CARRIER_DUST; } }
				FImage I2(C2.Width, C2.Height);
				RenderVolume(Sc, C2, I2, nullptr, 4);
				std::printf("galaxies x%d %s: %.6g (base old %.6g new %.6g)\n", Mul, K == 0 ? "old" : "new", Sum(I2), Sum(Ga), Sum(Gb));
			}
		}
	}
	const double TotalErr = std::abs(Sum(B) / Sum(A) - 1.0);
	const double ClusterErr = std::abs(Sum(Cb) / Sum(Ca) - 1.0);
	const double GalaxyErr = Sum(Ga) > 0.0 ? std::abs(Sum(Gb) / Sum(Ga) - 1.0) : 0.0;
	const double EmitErr = Sum(Ea) > 0.0 ? std::abs(Sum(Eb) / Sum(Ea) - 1.0) : 0.0;   // galaxies without dust
	// Per pixel, where the old image is bright enough to see (1e-3 exposure units).
	double WorstPix = 0.0;
	for (size_t I = 0; I < A.Rgb.size(); ++I) { if (A.Rgb[I] > 1.0e-3f) { WorstPix = std::max(WorstPix, std::abs(double(B.Rgb[I]) / double(A.Rgb[I]) - 1.0)); } }
	Uni::Check(2, "T2.vol.equivalence", "D-160/D-163 volume speed-ups keep the light (inside a rich cluster's core, against the D-157 march): galaxies' emission (no dust) within 5%, with dust within 8% (prefiltered lanes absorb less), total within 8%, cluster within 0.1%, and faster")
		.Metric("total_err", TotalErr).Metric("cluster_err", ClusterErr).Metric("galaxy_err", GalaxyErr).Metric("galaxy_emission_err", EmitErr).Metric("worst_visible_pixel_err", WorstPix)
		.Metric("ms_old", Ra.second).Metric("ms_new", Rb.second).Metric("steps_old", double(Ra.first.Steps)).Metric("steps_new", double(Rb.first.Steps))
		.Pass(TotalErr < 0.08 && ClusterErr < 0.001 && GalaxyErr < 0.08 && EmitErr < 0.05 && Rb.second < Ra.second,
			Uni::Fmt("total light %+.3f%%, cluster %+.4f%%, galaxies %+.2f%% (emission without dust %+.2f%%), worst visible pixel %.1f%%; %.0f -> %.0f ms, %.3g -> %.3g steps (%.1fx)",
				100.0 * (Sum(B) / Sum(A) - 1.0), 100.0 * (Sum(Cb) / Sum(Ca) - 1.0), 100.0 * (Sum(Ga) > 0.0 ? Sum(Gb) / Sum(Ga) - 1.0 : 0.0), 100.0 * (Sum(Ea) > 0.0 ? Sum(Eb) / Sum(Ea) - 1.0 : 0.0), 100.0 * WorstPix,
				Ra.second, Rb.second, double(Ra.first.Steps), double(Rb.first.Steps), Ra.second / std::max(Rb.second, 1e-9)));
}

UNI_TEST(cluster, volume_build_cost)
{
	// D-161: the CPU carrier build per frame with ~17,500 contributors (the web scene with
	// clusters, from intergalactic space), as the engine runs it: from the streamer, whose
	// measured offsets reject far contributors first. Owner's engine logs: ~40 ms per frame of
	// "volume build" on the game thread before D-161. The carriers must be identical to the
	// full walk's (the reference pipeline's path), and the build under 1 ms here.
	FUniverseModel M(UniScenes::WebToGalaxy());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FCpuGenBackend Be(G, 2097152u);
	FUniStreamer S(G, Be);
	S.bVelocityLod = false;
	S.bAsyncIntegrals = false;
	const FCoord Cam = UniScenes::VoidCamera();
	S.Update(Cam);
	for (int I = 0; I < 80 && !S.AllReady(); ++I) { S.Update(Cam); }
	FCamera Cm; Cm.Coord = Cam;
	H::FUniView V = Cm.View();
	V.ThetaPixel = 1.0f / 960.0f;
	FVolumeSource Full = FVolumeSource::From(S);
	Full.NearContributors = nullptr;   // the full walk
	const FVolumeScene A = VB.Build(Full, Cam, FVec3f{}, V);
	const FVolumeScene B = VB.Build(FVolumeSource::From(S), Cam, FVec3f{}, V);
	const bool bSame = A.Carriers.size() == B.Carriers.size() && std::memcmp(A.Carriers.data(), B.Carriers.data(), A.Carriers.size() * sizeof(FCarrierGPU)) == 0;
	const int Reps = 10;
	auto Time = [&](const FVolumeSource& Src)
	{
		const auto T0 = std::chrono::steady_clock::now();
		for (int I = 0; I < Reps; ++I) { VB.Build(Src, Cam, FVec3f{}, V); }
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count() / Reps;
	};
	const double MsFull = Time(Full), MsFast = Time(FVolumeSource::From(S));
	Uni::Check(2, "T2.vol.build_cost", "Volume carrier build per frame with ~17,500 contributors (D-161): identical carriers to the full walk, under 2 ms on the reference")
		.Metric("ms_full", MsFull).Metric("ms", MsFast).Metric("contributors", double(S.Contributors().size())).Metric("carriers", double(B.Carriers.size())).Metric("identical", bSame ? 1.0 : 0.0)
		.Pass(bSame && MsFast < 2.0 && S.Contributors().size() > 10000, Uni::Fmt("%.2f ms per build (full walk %.2f ms), %zu contributors, %zu carriers, %s", MsFast, MsFull, S.Contributors().size(), B.Carriers.size(), bSame ? "identical" : "DIFFERENT"));
}

UNI_TEST(cluster, volume_continuity)
{
	// D-161 (owner: "in the middle of a galaxy cluster the density-marched galaxies flip around
	// frame to frame"): inside the rich cluster's core one ray crosses more galaxies' spheres
	// than it can march. Moving the camera a little (0.25 kpc per frame, 12 frames), no visible
	// block of pixels may jump: carriers past the per-ray and per-frame caps fade out by
	// significance instead of being cut by their order in the list.
	FUniverseModel M(AnchoredCluster());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const double Mpc = Units::Mpc;
	const FVec3d Dir{ 0.7682212795973759, -0.5121475197315839, 0.3841106397986879 };
	FRefPipeline P(G);
	P.bVelocityLod = false;
	FImage Prev(1, 1);
	double WorstJump = 0.0;
	int Frames = 0, MaxActive = 0;
	for (int F = 0; F < 12; ++F)
	{
		const double D = 0.3 * Mpc + F * 0.25 * Units::Kpc;
		const FVec3d Off{ Dir.X * D, Dir.Y * D, Dir.Z * D };
		const FCoord Cam = Moved(FCoord::Origin(), Off);
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 160; Cm.Height = 106; Cm.DeltaMag = 0.0;
		Cm.LookAlong({ -Dir.X, -Dir.Y, -Dir.Z });
		const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
		FImage Img(Cm.Width, Cm.Height);
		RenderVolume(Sc, Cm, Img, nullptr, 2);
		MaxActive = std::max(MaxActive, int(Sc.Carriers.size()));
		if (F > 0)
		{
			// 10 x 10 pixel blocks: parallax moves near galaxies' features by a pixel or two per
			// frame (legitimately); a flipped carrier changes a block's light wholesale.
			const int Bs = 10;
			for (int By = 0; By + Bs <= Img.H; By += Bs) for (int Bx = 0; Bx + Bs <= Img.W; Bx += Bs)
			{
				double A = 0.0, B = 0.0;
				for (int Y = By; Y < By + Bs; ++Y) for (int X = Bx; X < Bx + Bs; ++X)
				{
					const size_t I = (size_t(Y) * Img.W + X) * 3 + 1;
					A += double(Prev.Rgb[I]); B += double(Img.Rgb[I]);
				}
				if (std::max(A, B) > 1.0e-2) { const double J = std::abs(B - A) / std::max(A, B); WorstJump = std::max(WorstJump, J); }
			}
		}
		Prev = Img;
		++Frames;
	}
	Uni::Check(2, "T2.vol.continuity", "Inside a rich cluster's core, moving 0.25 kpc per frame: no visible 10 x 10 pixel block of the volume changes by more than 10% between frames (D-161: no carrier flipping)")
		.Metric("worst_jump", WorstJump).Metric("carriers", MaxActive).Metric("frames", Frames)
		.Pass(Frames == 12 && WorstJump < 0.10, Uni::Fmt("%d frames, up to %d carriers: worst frame-to-frame change of a visible block %.1f%%", Frames, MaxActive, 100.0 * WorstJump));
}
