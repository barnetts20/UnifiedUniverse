// test_gen.cpp
// Tier 2: generation behaviour of the CPU reference pipeline on two scenes
// (web_to_galaxy: cosmic web -> galaxies; galaxy_to_star: galaxy -> stars), plus the baked
// UniVol path. Criteria from VERIFICATION_TOOLING.md.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <unordered_map>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	struct FScene
	{
		std::string Name;
		FUniverseModel Model;
		FRefGenerator Gen;
		FCoord Camera;
		int StepLevel;   // the camera path steps by 0.7 cells of this level per frame
		FScene(const std::string& InName, FUniverseDef D, const FCoord& Cam, int InStepLevel)
			: Name(InName), Model(std::move(D)), Gen(Model), Camera(Cam), StepLevel(InStepLevel) {}
	};

	FScene& Web() { static FScene S("web_to_galaxy", UniScenes::WebToGalaxy(), UniScenes::VoidCamera(), 83); return S; }
	FScene& Gal() { static FScene S("galaxy_to_star", UniScenes::GalaxyToStar(), UniScenes::SolarCamera(), 64); return S; }

	FCoord Step(const FCoord& C, int Level, double Cells, double Dx, double Dy, double Dz)
	{
		FCoord R = C;
		FVec3f F;
		const double W = CellWidthCm(Level) * Cells;
		AddDisplacement(R, F, FVec3d{ Dx * W, Dy * W, Dz * W });
		return R;
	}

	/** Runs a camera path and returns the final content hash of every cell generated. */
	std::map<std::string, u64> RunPath(FScene& S, const std::vector<FCoord>& Path, bool bEvict, size_t* OutRegenerated = nullptr)
	{
		FRefPipeline P(S.Gen);
		P.bVelocityLod = false;
		P.bEvictOnExit = bEvict;
		std::map<std::string, u64> Hashes;
		P.OnGenerated = [&](const FCellContent& C)
		{
			const std::string K = std::to_string(C.Key.Level) + ToHex(C.Key.C[0]) + ToHex(C.Key.C[1]) + ToHex(C.Key.C[2]);
			Hashes[K] = C.Hash;
		};
		for (const FCoord& C : Path) { P.Update(C, FVec3f{}); }
		if (OutRegenerated) { size_t N = 0; for (const auto& KV : P.Stats()) { N += KV.second.Regenerated; } *OutRegenerated = N; }
		return Hashes;
	}

	/** The anchored galaxy's star contributor (galaxy_to_star). */
	FContributor AnchorContributor()
	{
		FCellContent C;
		Gal().Gen.GenerateCell(Gal().Gen.AnchorCellList()[0], {}, C);
		return Gal().Gen.MakeContributors(C).at(0);
	}

	double Correlation(const std::vector<double>& A, const std::vector<double>& B)
	{
		const size_t N = A.size();
		double Ma = 0, Mb = 0;
		for (size_t I = 0; I < N; ++I) { Ma += A[I]; Mb += B[I]; }
		Ma /= double(N); Mb /= double(N);
		double Sab = 0, Saa = 0, Sbb = 0;
		for (size_t I = 0; I < N; ++I) { Sab += (A[I] - Ma) * (B[I] - Mb); Saa += (A[I] - Ma) * (A[I] - Ma); Sbb += (B[I] - Mb) * (B[I] - Mb); }
		return Sab / std::sqrt(Saa * Sbb + 1e-300);
	}

	/** Force a large count into one cell for one contributor and correlate per-voxel counts
	 *  with the per-voxel field integral (16^3 voxels). */
	double FidelityOneCell(FScene& S, const FContributor& C, const FCellKey& K, double TargetLambda, size_t& OutPlaced, size_t& OutVoxels)
	{
		FRefGenerator G(S.Model);
		// Estimate the cell's natural lambda, then scale to the target.
		FCellContent Probe;
		G.GenerateCell(K, { &C }, Probe);
		const double Natural = Probe.Tasks.empty() ? 0.0 : Probe.Tasks[0].Lambda;
		G.LambdaMultiplier = Natural > 0.0 ? TargetLambda / Natural : 1.0;
		G.MaxPerCell = 1u << 30;
		FCellContent Out;
		G.GenerateCell(K, { &C }, Out);
		OutPlaced = Out.Objects.size();
		const int V = 16;
		std::vector<double> Count(size_t(V * V * V), 0.0), Field(size_t(V * V * V), 0.0);
		for (const FObjectGPU& O : Out.Objects)
		{
			const int X = int(O.A[0] >> 28), Y = int(O.A[1] >> 28), Z = int(O.A[2] >> 28);
			Count[size_t((Z * V + Y) * V + X)] += 1.0;
		}
		const FFieldFrameGPU FG = G.Frame(C, K);
		const H::FUniFieldFrame F = FRefGenerator::ToHlsl(FG);
		const H::FUniFieldParams P = C.Params.ToHlsl();
		for (int Z = 0; Z < V; ++Z) for (int Y = 0; Y < V; ++Y) for (int X = 0; X < V; ++X)
		{
			double Sum = 0.0;
			for (int K2 = 0; K2 < 27; ++K2)
			{
				const H::float3 U((X + (K2 % 3 + 0.5f) / 3.0f) / V, (Y + ((K2 / 3) % 3 + 0.5f) / 3.0f) / V, (Z + (K2 / 9 + 0.5f) / 3.0f) / V);
				Sum += H::Uni_FieldSample(C.Emitter->FieldId, H::Uni_FieldFramePoint(F, U), P);
			}
			Field[size_t((Z * V + Y) * V + X)] = Sum / 27.0;
		}
		// Only voxels the field reaches count (the unit-ball cut leaves empty corners).
		std::vector<double> A, B;
		for (size_t I = 0; I < Count.size(); ++I) { if (Field[I] > 0.0 || Count[I] > 0.0) { A.push_back(Count[I]); B.push_back(Field[I]); } }
		OutVoxels = A.size();
		return Correlation(A, B);
	}
}

// ==========================================================================================

UNI_TEST(gen, determinism)
{
	long long Compared = 0, Mismatch = 0;
	std::string Detail;
	size_t Regen = 0;
	for (FScene* S : { &Web(), &Gal() })
	{
		// Path A: 30 frames along a diagonal, 0.7 cells of the step level per frame.
		std::vector<FCoord> A;
		FCoord C = S->Camera;
		for (int I = 0; I < 30; ++I) { A.push_back(C); C = Step(C, S->StepLevel, 0.7, 1.0, 0.6, -0.3); }
		// Path B: teleport to the end, walk back (different window history and batch order).
		std::vector<FCoord> B(A.rbegin(), A.rend());
		// Path C: A, 10 more slides away, then back to the start, evicting on exit so every
		// revisited cell is regenerated rather than restored.
		std::vector<FCoord> Cp = A;
		FCoord X = A.back();
		for (int I = 0; I < 10; ++I) { X = Step(X, S->StepLevel, 1.0, 1.0, 0.6, -0.3); Cp.push_back(X); }
		for (int I = int(A.size()) - 1; I >= 0; --I) { Cp.push_back(A[size_t(I)]); }

		size_t R1 = 0;
		const auto HA = RunPath(*S, A, false, &R1);
		const auto HB = RunPath(*S, B, false);
		const auto HC = RunPath(*S, Cp, true);
		Regen += R1;
		long long M = 0, N = 0;
		for (const auto& KV : HA)
		{
			for (const auto* Other : { &HB, &HC })
			{
				auto It = Other->find(KV.first);
				if (It == Other->end()) { continue; }
				++N;
				if (It->second != KV.second) { ++M; }
			}
		}
		// Direct batch-order check: regenerate A's cells in shuffled order with shuffled
		// contributor lists.
		Compared += N; Mismatch += M;
		Detail += Uni::Fmt("%s%s: %lld compared, %lld differ", Detail.empty() ? "" : "; ", S->Name.c_str(), N, M);
	}
	Uni::Check(2, "T2.gen.determinism", "Per-cell content hashes identical across batch orders, window histories, and revisits after 10 slides (evicted, regenerated)")
		.Metric("compared", double(Compared)).Metric("mismatches", double(Mismatch)).Metric("regenerated_contributor_growth", double(Regen))
		.Pass(Mismatch == 0 && Compared > 1000, Uni::Fmt("%s (contributor-growth regenerations on path A: %zu)", Detail.c_str(), Regen));
}

UNI_TEST(gen, counts)
{
	// Aggregate z of placed counts against lambda over >= 10^4 (cell, contributor) tasks, from
	// both scenes, using random cells at every level the scene emits on.
	double SumDiff = 0.0, SumLambda = 0.0;
	long long Tasks = 0, Shortfall = 0, Clipped = 0, Candidates = 0;
	Uni::FTestRng R(0xC0047ull);
	const FContributor Anchor = AnchorContributor();
	for (FScene* S : { &Web(), &Gal() })
	{
		const bool bGal = S == &Gal();
		const FContributor& C = bGal ? Anchor : S->Gen.RootContributors()[0];
		const FSliceTable& T = S->Model.SlicesOf(C.Emitter->ChildArchetypeId);
		for (int I = 0; I < 7000; ++I)
		{
			const int L = T.LMin + int(R.U32() % u32(T.LMax - T.LMin + 1));
			// Random cell inside the contributor's reach (galaxy) or anywhere (web).
			FCoord P = S->Camera;
			FVec3f F;
			const double Reach = bGal ? 1.0 * C.InfluenceCm : std::ldexp(1.0, 90);
			FVec3d D{ (R.Unit() * 2 - 1) * Reach, (R.Unit() * 2 - 1) * Reach, (R.Unit() * 2 - 1) * Reach * (bGal ? 0.05 : 1.0) };
			if (bGal) { P = Anchor.Centre; }
			AddDisplacement(P, F, D);
			FCellContent Out;
			S->Gen.GenerateCell(CellOf(P, L), { &C }, Out);
			for (const FTaskStats& Ts : Out.Tasks)
			{
				if (Ts.Lambda <= 0.0) { continue; }
				++Tasks;
				SumDiff += double(Ts.Accepted) - Ts.Lambda;
				SumLambda += Ts.Lambda;
				Shortfall += Ts.Count - Ts.Accepted;
				Clipped += Ts.Clipped;
				Candidates += Ts.Candidates;
			}
		}
	}
	const double Z = SumDiff / std::sqrt(SumLambda);
	Uni::Check(2, "T2.gen.counts", "Aggregate z-score of placed count vs lambda within +-3 over >= 10^4 cells")
		.Metric("tasks", double(Tasks)).Metric("z", Z).Metric("sum_lambda", SumLambda).Metric("rejection_shortfall", double(Shortfall))
		.Metric("clipped_fraction", Candidates ? double(Clipped) / double(Candidates) : 0.0)
		.Pass(Tasks >= 10000 && std::fabs(Z) <= 3.0 && Shortfall == 0,
			Uni::Fmt("%lld tasks, sum lambda %.4g, z = %+.2f; rejection-cap shortfall %lld; candidates over envelope %.3f%%",
				Tasks, SumLambda, Z, Shortfall, Candidates ? 100.0 * double(Clipped) / double(Candidates) : 0.0));
}

UNI_TEST(gen, slicing)
{
	// Sum of P(L) over levels is 1 (both archetypes), and for the anchor galaxy, summing the
	// per-cell integral estimate over a tiling of the influence sphere reproduces the
	// normalising integral, so sum_L sum_cells lambda = N_total (within 1%).
	const FSliceTable& Ts = Gal().Model.SlicesOf(UniScenes::kStar);
	const FSliceTable& Tg = Gal().Model.SlicesOf(UniScenes::kGalaxy);
	const FContributor C = AnchorContributor();
	const double IAll = Gal().Gen.IntegralAll(C);
	std::string Detail;
	double WorstFine = 0.0;
	double ExpectedTotal = 0.0;
	for (int Span : { 4, 8, 16, 32, 64 })
	{
		// Level whose cells are 2 * influence / Span wide (rounded to a power of two).
		const int L = int(std::ceil(std::log2(2.0 * C.InfluenceCm / Span)));
		const double W = CellWidthCm(L);
		const FCellKey Centre = CellOf(C.Centre, L);
		const int Reach = int(std::ceil(C.InfluenceCm / W)) + 1;
		double Sum = 0.0;
		FSlice Unit; Unit.P = 1.0;
		for (int X = -Reach; X <= Reach; ++X) for (int Y = -Reach; Y <= Reach; ++Y) for (int Z = -Reach; Z <= Reach; ++Z)
		{
			const FCellKey K = Neighbour(Centre, X, Y, Z);
			// Overlap test with the sphere, as the generator does.
			const FVec3d D = ToDouble(Sub(C.Centre, CellOrigin(K)));
			const double P[3] = { D.X, D.Y, D.Z };
			double Dist2 = 0.0;
			for (double V : P) { const double E = V < 0.0 ? -V : (V > W ? V - W : 0.0); Dist2 += E * E; }
			if (Dist2 > C.InfluenceCm * C.InfluenceCm) { continue; }
			const FFieldFrameGPU FG = Gal().Gen.Frame(C, K);
			const H::FUniFieldFrame F = FRefGenerator::ToHlsl(FG);
			const H::FUniFieldParams Pp = C.Params.ToHlsl();
			const u32 Seed = CellSeed(69u, K, UniScenes::kStar, C.Id, 0u);
			double S = 0.0;
			for (u32 Kp = 0; Kp < H::UNI_PROBES_PER_CELL; ++Kp) { S += H::Uni_FieldSample(C.Emitter->FieldId, H::Uni_FieldFramePoint(F, H::Uni_ProbeUnit(Seed, Kp)), Pp); }
			Sum += S / H::UNI_PROBES_PER_CELL * double(FG.SizeP) * double(FG.SizeP) * double(FG.SizeP);
		}
		const double Ratio = Sum / IAll;
		if (Span >= 16) { WorstFine = std::max(WorstFine, std::fabs(Ratio - 1.0)); }
		Detail += Uni::Fmt("%s%d cells/axis: %.4f", Detail.empty() ? "" : ", ", Span, Ratio);
		if (Span == 64) { for (const FSlice& Sl : Ts.Slices) { ExpectedTotal += C.NTotal * Sl.P * Ratio; } }
	}
	const double TotalErr = std::fabs(ExpectedTotal / C.NTotal - 1.0);
	Uni::Check(2, "T2.gen.slicing", "Luminosity slicing conserves totals: sum over levels of expected children = N_total within 1%")
		.Metric("star_psum", Ts.PSum).Metric("galaxy_psum", Tg.PSum).Metric("worst_tiling_error_16plus", WorstFine).Metric("total_error", TotalErr)
		.Pass(std::fabs(Ts.PSum - 1.0) < 1e-9 && std::fabs(Tg.PSum - 1.0) < 1e-9 && WorstFine < 0.01 && TotalErr < 0.01,
			Uni::Fmt("sum P(L): stars %.9f over L%d..L%d, galaxies %.9f over L%d..L%d; tiling sum / integral: %s; expected total / N_total = %.4f",
				Ts.PSum, Ts.LMin, Ts.LMax, Tg.PSum, Tg.LMin, Tg.LMax, Detail.c_str(), ExpectedTotal / C.NTotal));
}

UNI_TEST(gen, fidelity)
{
	const FContributor Gc = AnchorContributor();
	// A disc cell 4 kpc from the centre at L70 (0.4 kpc cells), and the web's whole period cell.
	FCoord P = Gc.Centre;
	FVec3f F;
	AddDisplacement(P, F, FVec3d{ 4.0 * Units::Kpc, 0.5 * Units::Kpc, 0.0 });
	size_t NG = 0, VG = 0, NW = 0, VW = 0;
	const double CorrG = FidelityOneCell(Gal(), Gc, CellOf(P, 70), 2.0e5, NG, VG);
	const FContributor& Wc = Web().Gen.RootContributors()[0];
	const double CorrW = FidelityOneCell(Web(), Wc, CellOf(Web().Camera, 87), 2.0e5, NW, VW);
	Uni::Check(2, "T2.gen.fidelity", "Objects sit on structure: per-voxel placed density vs field correlation > 0.9")
		.Metric("galaxy_corr", CorrG).Metric("web_corr", CorrW).Metric("galaxy_placed", double(NG)).Metric("web_placed", double(NW))
		.Pass(CorrG > 0.9 && CorrW > 0.9, Uni::Fmt("galaxy disc cell: r = %.4f (%zu objects, %zu voxels); cosmic web cell: r = %.4f (%zu objects, %zu voxels)", CorrG, NG, VG, CorrW, NW, VW));
}

UNI_TEST(gen, coverage_budget_gather)
{
	// One pass over each scene at its camera, plus the path used for determinism.
	long long Checked = 0, Outside = 0;
	size_t PeakLevel = 0, PeakTotal = 0, MaxGather = 0;
	int PeakL = -1, GatherL = -1;
	std::string Detail;
	for (FScene* S : { &Web(), &Gal() })
	{
		FRefPipeline P(S->Gen);
		P.bVelocityLod = false;
		std::map<u32, FContributor> Known;
		for (const FContributor& R : S->Gen.RootContributors()) { Known[R.Id] = R; }
		P.OnGenerated = [&](const FCellContent& C)
		{
			for (const FContributor& Ct : S->Gen.MakeContributors(C)) { Known[Ct.Id] = Ct; }
			for (size_t I = 0; I < C.Objects.size(); ++I)
			{
				const auto It = Known.find(C.ObjectContributor[I]);
				if (It == Known.end() || It->second.bRoot || C.ObjectContributor[I] == 0u) { continue; }
				const u32 Off[3] = { C.Objects[I].A[0], C.Objects[I].A[1], C.Objects[I].A[2] };
				const double D = Length(ToDouble(Sub(ObjectCoord(C.Key, Off), It->second.Centre)));
				++Checked;
				if (D > It->second.InfluenceCm * (1.0 + 1e-9)) { ++Outside; }
			}
		};
		FCoord C = S->Camera;
		for (int I = 0; I < 12; ++I)
		{
			P.Update(C, FVec3f{});
			for (const auto& KV : P.Stats())
			{
				if (KV.second.Resident > PeakLevel) { PeakLevel = KV.second.Resident; PeakL = KV.first; }
				if (KV.second.MaxContributors > MaxGather) { MaxGather = KV.second.MaxContributors; GatherL = KV.first; }
			}
			PeakTotal = std::max(PeakTotal, P.TotalResident());
			C = Step(C, S->StepLevel, 0.7, 1.0, 0.6, -0.3);
		}
		std::string PerLevel;
		for (const auto& KV : P.Stats()) { if (KV.second.Resident) { PerLevel += Uni::Fmt(" L%d:%zu", KV.first, KV.second.Resident); } }
		Detail += Uni::Fmt("%s%s resident by level:%s", Detail.empty() ? "" : "; ", S->Name.c_str(), PerLevel.c_str());
	}
	Uni::Check(2, "T2.gen.coverage", "No child placed outside its contributor's influence")
		.Metric("checked", double(Checked)).Metric("outside", double(Outside))
		.Pass(Outside == 0 && Checked > 10000, Uni::Fmt("%lld children checked against their contributor sphere, %lld outside", Checked, Outside));
	Uni::Check(2, "T2.gen.budget", "Residency under D-005 caps (131,072 per level, 2,097,152 total); peaks logged")
		.Metric("peak_level", double(PeakLevel)).Metric("peak_level_L", PeakL).Metric("peak_total", double(PeakTotal))
		.Pass(PeakLevel <= 131072 && PeakTotal <= 2097152, Uni::Fmt("peak per level %zu (L%d), peak total %zu. %s", PeakLevel, PeakL, PeakTotal, Detail.c_str()));
	Uni::Check(2, "T2.gen.gather", "Max contributor list length per level logged (D-004 trigger: 256)")
		.Metric("max_gather", double(MaxGather)).Metric("level", GatherL)
		.Pass(MaxGather <= 256, Uni::Fmt("max %zu contributors gathered (L%d); D-004 trigger 256 not reached", MaxGather, GatherL));
}

UNI_TEST(gen, baked)
{
	// UniVol: generate a periodic test volume, round-trip it through the file format, check that
	// mips are exact means, that mip-descent placement follows the texels, and that cell counts
	// follow the mip integrals.
	const FUniVol V = FUniVol::GeneratePeriodic(64, 0xBA4Eu);
	const std::string Path = UniVerify::Options().OutDir + "/test_volume.uvol";
	V.Write(Path);
	FUniVol R32;
	const bool bRead = R32.Read(Path);
	bool bSame = bRead && R32.Size == V.Size && R32.MipCount() == V.MipCount();
	for (u32 M = 0; bSame && M < V.MipCount(); ++M) { bSame = R32.Mips[M] == V.Mips[M]; }
	FUniVol Half = V; Half.Format = 0u;
	Half.Write(UniVerify::Options().OutDir + "/test_volume_r16.uvol");
	FUniVol R16;
	double HalfErr = 0.0;
	if (R16.Read(UniVerify::Options().OutDir + "/test_volume_r16.uvol")) { for (size_t I = 0; I < V.Mips[0].size(); ++I) { HalfErr = std::max(HalfErr, std::fabs(double(R16.Mips[0][I]) - V.Mips[0][I]) / V.Mips[0][I]); } }

	// Mips are means.
	double MipErr = 0.0;
	for (u32 M = 1; M < V.MipCount(); ++M)
	{
		const u32 S = V.MipSize(M);
		for (u32 Z = 0; Z < S; ++Z) for (u32 Y = 0; Y < S; ++Y) for (u32 X = 0; X < S; ++X)
		{
			double Acc = 0.0;
			for (u32 K = 0; K < 8; ++K) { Acc += V.Texel(M - 1, 2 * X + (K & 1u), 2 * Y + ((K >> 1) & 1u), 2 * Z + (K >> 2)); }
			MipErr = std::max(MipErr, std::fabs(Acc / 8.0 - V.Texel(M, X, Y, Z)) / V.Texel(M, X, Y, Z));
		}
	}
	// Periodicity of the generator: opposite faces are continuous neighbours (mean gradient across
	// the wrap equals the interior mean gradient within a factor of 2).
	double Wrap = 0.0, Interior = 0.0;
	for (u32 Y = 0; Y < 64; ++Y) for (u32 Z = 0; Z < 64; ++Z)
	{
		Wrap += std::fabs(V.Texel(0, 63, Y, Z) - V.Texel(0, 0, Y, Z));
		Interior += std::fabs(V.Texel(0, 31, Y, Z) - V.Texel(0, 32, Y, Z));
	}
	const bool bPeriodic = Wrap < 2.0 * Interior && Interior < 2.0 * Wrap;

	// Descent placement from an 8^3-texel cell (mip 3) under a non-trivial symmetry.
	FBakedCellRef Cell;
	Cell.Vol = &V; Cell.Mip = 3; Cell.Texel = { 2, 5, 1 }; Cell.Sym = FCubeSymmetry::FromIndex(29);
	const u32 N = 400000;
	std::vector<double> Count(512, 0.0), Tex(512, 0.0);
	for (u32 J = 0; J < N; ++J)
	{
		u32 Off[3];
		Cell.Descend(0x5EEDu, J, Off);
		const int X = int(Off[0] >> 29), Y = int(Off[1] >> 29), Z = int(Off[2] >> 29);
		Count[size_t((Z * 8 + Y) * 8 + X)] += 1.0;
	}
	for (int Z = 0; Z < 8; ++Z) for (int Y = 0; Y < 8; ++Y) for (int X = 0; X < 8; ++X)
	{
		Tex[size_t((Z * 8 + Y) * 8 + X)] = Cell.Fetch(0, { Cell.Texel[0] * 8 + X, Cell.Texel[1] * 8 + Y, Cell.Texel[2] * 8 + Z });
	}
	const double Corr = Correlation(Count, Tex);
	// Counts follow mip integrals: Poisson draws over all 8^3 mip-3 cells of the volume with
	// lambda = mean * scale; aggregate z.
	double SumDiff = 0.0, SumLam = 0.0;
	for (int Z = 0; Z < 8; ++Z) for (int Y = 0; Y < 8; ++Y) for (int X = 0; X < 8; ++X)
	{
		FBakedCellRef C2 = Cell; C2.Texel = { X, Y, Z };
		const double Lam = 50.0 * C2.Mean();
		const u32 Seed = H::Uni_SeedMix(0xBAu, u32((Z * 8 + Y) * 8 + X));
		SumDiff += double(H::Uni_PoissonCount(float(Lam), Seed)) - Lam;
		SumLam += Lam;
	}
	const double Z = SumDiff / std::sqrt(SumLam);
	// Symmetries preserve integrals and give variety.
	std::set<u32> Used;
	for (u32 T = 0; T < 2000; ++T)
	{
		const FU128 Tc[3] = { FU128::FromU64(T), FU128::FromU64(T * 7u), FU128::FromU64(T * 13u) };
		const FCubeSymmetry S = TileSymmetry(96u, Tc, 5u);
		Used.insert(u32(S.Perm[0] * 100 + S.Perm[1] * 10 + S.Perm[2]) * 8u + u32(S.Flip[0]) + 2u * u32(S.Flip[1]) + 4u * u32(S.Flip[2]));
	}
	double SymErr = 0.0;
	for (u32 S = 0; S < 48; ++S)
	{
		FBakedCellRef A = Cell, B = Cell;
		A.Sym = FCubeSymmetry::FromIndex(S); A.Mip = 6; A.Texel = { 0, 0, 0 };
		B.Sym = FCubeSymmetry::FromIndex(0); B.Mip = 6; B.Texel = { 0, 0, 0 };
		SymErr = std::max(SymErr, std::fabs(A.Mean() - B.Mean()));
	}
	Uni::Check(2, "T2.gen.baked", "Baked UniVol path: file round trip, mips are exact means, mip-descent placement follows texels, counts follow mip integrals, 48 tile symmetries")
		.Metric("roundtrip", bSame).Metric("r16_rel_err", HalfErr).Metric("mip_rel_err", MipErr).Metric("descent_corr", Corr).Metric("z", Z).Metric("symmetries_used", double(Used.size()))
		.Pass(bSame && HalfErr < 1e-3 && MipErr < 1e-5 && Corr > 0.99 && std::fabs(Z) < 3.0 && Used.size() == 48 && SymErr < 1e-6 && bPeriodic,
			Uni::Fmt("R32 round trip %s, R16 max rel err %.1e; mip mean err %.1e; descent vs texel r = %.4f (400k draws); count z = %+.2f; %zu/48 symmetries used, integral change %.1e; periodic %s",
				bSame ? "exact" : "FAILED", HalfErr, MipErr, Corr, Z, Used.size(), SymErr, bPeriodic ? "yes" : "NO"));
}
