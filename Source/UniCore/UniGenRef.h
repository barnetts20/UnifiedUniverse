#pragma once

// UniGenRef.h
// The CPU reference generation pipeline. It exists to VERIFY the GPU design offline (D-015);
// it is never a runtime fallback (lesson #1). Every per-probe and per-candidate operation calls
// the shared shader cores (UniFields.ush, UniPlacement.ush, UniSeed.ush) through the shim, so
// the logic verified here is the logic the GPU runs. The orchestration (loops standing in for
// dispatches, windows, gather) mirrors the GPU passes in the design:
//
//   1. gather contributors and compute lambda per cell     -> GatherForCell, probe loop
//   2. prefix-sum counts and allocate pool ranges          -> IndexBase accumulation
//   3. place objects and draw their properties              -> candidate loop
//   4. register new contributors for finer levels           -> RegisterContributors
//   5. apply persistence deltas                             -> (not in the prototype)
//
// DETERMINISM CONTRACT. A cell's content is a pure function of (universe, cell key, the set
// of contributors overlapping it). The set is content-determined as long as every overlapping
// contributor is resident, which the influence term of the relevance radius guarantees
// (D-101). When a contributor appears that overlaps already-generated cells, those cells are
// regenerated with the full set (D-113). Tier 2's determinism check verifies the contract.

#include "UniArchetype.h"
#include "UniVol.h"
#include "UniWindow.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace UniCore
{
	namespace H = UniHlsl;

	/** Exact absolute coordinate of an object from its cell and in-cell offset. */
	inline FCoord ObjectCoord(const FCellKey& K, const u32 Offset[3])
	{
		FCoord C = CellOrigin(K);
		for (int A = 0; A < 3; ++A)
		{
			const FU128 Off = K.Level >= 32 ? Shl(FU128::FromU64(Offset[A]), K.Level - 32) : FU128::FromU64(u64(Offset[A]) >> (32 - K.Level));
			C.SetAxis(A, Add(C.Axis(A), Off));
		}
		return C;
	}

	/** D-151: the cluster field's integral over the unit ball, analytically (UniField_Cluster):
	 *  the beta-model body is q1 q2 4 pi int_0^Cut (1 + s^2/rc^2)^-1.5 taper(s) s^2 ds (exact, the
	 *  ellipsoidal map has Jacobian q1 q2), each Plummer clump 4 pi a^3 / 3 (its tail beyond the
	 *  soft edge, < 0.3% of the total, is ignored; T1.field.cluster measures it). */
	inline double ClusterFieldIntegral(const H::FUniFieldParams& P)
	{
		const double Cut = double(P.A.w);
		const double Rc = std::max(double(P.A.x), 1.0e-4);
		const double Q1 = std::max(double(P.A.y), 0.05), Q2 = std::max(double(P.A.z), 0.05);
		const int N = 8192;
		double Body = 0.0;
		for (int I = 0; I < N; ++I)
		{
			const double S = (double(I) + 0.5) / double(N) * Cut;
			const double X = 1.0 + S * S / (Rc * Rc);
			const double W = double(H::Uni_ClusterTaper(float(S), float(Cut)));
			Body += W * S * S / (X * std::sqrt(X));
		}
		Body *= Cut / double(N) * 4.0 * 3.14159265358979323846 * Q1 * Q2;
		const double NClumps = std::min(std::floor(std::max(double(P.B.x), 0.0)), double(H::UNI_CLUSTER_MAX_CLUMPS));
		const double Clumps = (NClumps > 0.0 && P.B.y > 0.0f) ? NClumps * double(P.B.y) * 4.0 / 3.0 * 3.14159265358979323846 * std::pow(std::max(double(P.B.z), 1.0e-4), 3.0) : 0.0;
		return Body + Clumps;
	}

	/** D-151: fields whose normaliser is analytic: no sampled integral, nothing to wait for. */
	inline bool FieldHasAnalyticIntegral(u32 FieldId) { return FieldId == H::UNI_FIELD_CLUSTER; }

	inline H::FUniObject ToHlsl(const FObjectGPU& O)
	{
		H::FUniObject R;
		R.A = H::uint4(O.A[0], O.A[1], O.A[2], O.A[3]);
		R.B = H::uint4(O.B[0], O.B[1], O.B[2], O.B[3]);
		return R;
	}

	inline FObjectGPU FromHlsl(const H::FUniObject& O)
	{
		FObjectGPU R;
		R.A[0] = O.A.x; R.A[1] = O.A.y; R.A[2] = O.A.z; R.A[3] = O.A.w;
		R.B[0] = O.B.x; R.B[1] = O.B.y; R.B[2] = O.B.z; R.B[3] = O.B.w;
		return R;
	}

	// ======================================================================================
	// Contributors
	// ======================================================================================

	struct FContributor
	{
		u32 Id = 0;
		bool bRoot = false;
		FObjectId Parent;
		u32 ParentArchetypeId = 0;
		int EmitterIndex = 0;
		const FUniEmitter* Emitter = nullptr;
		FCoord Centre;
		double InfluenceCm = 0.0;
		float Rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		FFieldParams Params;
		double NTotal = 0.0;
		mutable double IntegralAll = -1.0;   // field-space integral over the unit ball / period (lazy, cached)
	};

	struct FTaskStats
	{
		u32 ContributorId = 0;
		double Lambda = 0.0;
		u32 Count = 0;          // Poisson draw (after truncation)
		u32 Accepted = 0;
		u32 Emitted = 0;        // accepted minus suppressed
		u32 Candidates = 0;
		u32 Clipped = 0;        // candidates whose density exceeded the envelope
		u32 Truncated = 0;      // dropped by the per-cell cap (logged)
		double Mean = 0.0, Envelope = 0.0;
		double FieldAtCentre = 0.0;
	};

	struct FCellContent
	{
		FCellKey Key;
		std::vector<FObjectGPU> Objects;
		std::vector<u32> Indices;             // identity index per object: acceptance index, or 2^15 + anchor
		std::vector<u32> ObjectContributor;   // contributor id per object (anchors: 0)
		std::vector<FTaskStats> Tasks;
		std::vector<u32> ContributorSet;      // sorted ids used to generate this cell
		u64 Hash = 0;
	};

	/** FNV-1a 64 over a cell's objects. Order is part of identity (index order). */
	inline u64 HashContent(const FCellContent& C)
	{
		u64 H = 1469598103934665603ull;
		auto Mix = [&](u32 V) { for (int B = 0; B < 4; ++B) { H ^= (V >> (8 * B)) & 0xFFu; H *= 1099511628211ull; } };
		Mix(u32(C.Key.Level));
		for (int A = 0; A < 3; ++A) { for (int N = 0; N < 4; ++N) { Mix(C.Key.C[A].L[N]); } }
		for (const FObjectGPU& O : C.Objects) { for (int I = 0; I < 4; ++I) { Mix(O.A[I]); Mix(O.B[I]); } }
		return H;
	}

	/** Surface brightness: the shared core (UniPlacement.ush), so GPU and CPU agree. */
	inline float SurfaceBrightness(float AbsMag, float RadiusLog2) { return UniHlsl::Uni_SurfaceBrightness(AbsMag, RadiusLog2); }

	// ======================================================================================
	// Reference generator
	// ======================================================================================

	class FRefGenerator
	{
	public:
		explicit FRefGenerator(const FUniverseModel& InModel) : Model(InModel)
		{
			const FUniverseDef& D = Model.Definition();
			for (size_t I = 0; I < D.RootEmitters.size(); ++I)
			{
				FContributor C;
				C.bRoot = true;
				C.Id = H::Uni_SeedMix(D.Seed, H::UNI_CH_CONTRIBUTOR ^ u32(I));
				C.EmitterIndex = int(I);
				C.Emitter = &D.RootEmitters[I];
				C.Params = C.Emitter->Exemplar;
				C.NTotal = C.Emitter->NTotal;
				Roots.push_back(C);
			}
			// Anchors: placed at their level, with reserved indices (2^15 + anchor index).
			for (size_t I = 0; I < D.Anchors.size(); ++I)
			{
				const FAnchor& A = D.Anchors[I];
				const int L = Model.LevelOf(A.ArchetypeId, A.AbsMag);
				AnchorLevels.push_back(L);
				AnchorCells.push_back(CellOf(A.Coord, L));
			}
			// D-167: catalogue entries, by level: each one's position in its tile as an exact
			// tile-local coordinate (cm, < 2^PeriodBits).
			for (size_t Ci = 0; Ci < D.Catalogues.size(); ++Ci)
			{
				const FCatalogue& Cat = D.Catalogues[Ci];
				for (size_t Ei = 0; Ei < Cat.Entries.size(); ++Ei)
				{
					FCatRef R;
					R.Cat = u32(Ci);
					R.Entry = u32(Ei);
					R.Level = Model.LevelOf(Cat.ArchetypeId, Cat.Entries[Ei].AbsMag);
					for (int Ax = 0; Ax < 3; ++Ax)
					{
						const double F = std::clamp(Cat.Entries[Ei].Pos[Ax] - std::floor(Cat.Entries[Ei].Pos[Ax]), 0.0, 0.9999999999999999);
						const u64 M53 = u64(F * 9007199254740992.0);   // 53 bits of the fraction
						R.E[Ax] = Cat.PeriodBits >= 53 ? Shl(FU128::FromU64(M53), Cat.PeriodBits - 53) : Shr(FU128::FromU64(M53), 53 - Cat.PeriodBits);
					}
					CatByLevel[R.Level].push_back(CatRefs.size());
					CatRefs.push_back(R);
				}
			}
		}

		/** D-167: levels at which catalogues place objects, with their archetypes. */
		std::map<int, std::set<u32>> CatalogueLevels() const
		{
			std::map<int, std::set<u32>> R;
			const FUniverseDef& D = Model.Definition();
			for (const FCatRef& C : CatRefs) { R[C.Level].insert(D.Catalogues[C.Cat].ArchetypeId); }
			return R;
		}
		bool CatalogueAtLevel(int L) const { return CatByLevel.count(L) != 0; }

		/** Authored objects (anchors, catalogue entries) in cell K? */
		bool HasAuthored(const FCellKey& K) const
		{
			for (const FCellKey& A : AnchorCells) { if (A == K) { return true; } }
			bool bAny = false;
			ForEachCatalogueObject(K, [&](const FCatRef&, const u32*, u32) { bAny = true; });
			return bAny;
		}

		/** D-167: every catalogue object in cell K: (entry, in-cell offset in 2^-32 cell units,
		 *  copy within the cell: 0 unless the cell holds several tiles). */
		template <typename F>
		void ForEachCatalogueObject(const FCellKey& K, F&& Fn) const
		{
			const auto It = CatByLevel.find(K.Level);
			if (It == CatByLevel.end()) { return; }
			const FUniverseDef& D = Model.Definition();
			const int L = K.Level;
			for (size_t Ri : It->second)
			{
				const FCatRef& R = CatRefs[Ri];
				const int P = D.Catalogues[R.Cat].PeriodBits;
				if (L <= P)
				{
					// The cell lies inside one tile: the entry is in it if its cell index within the
					// tile is the cell's.
					bool bIn = true;
					u32 Off[3];
					for (int Ax = 0; Ax < 3 && bIn; ++Ax)
					{
						bIn = MaskLow(K.C[Ax], P - L) == Shr(R.E[Ax], L);
						const FU128 Rel = MaskLow(R.E[Ax], L);
						Off[Ax] = L >= 32 ? u32(Shr(Rel, L - 32).L[3]) : u32(Shl(Rel, 32 - L).L[3]);
					}
					if (bIn) { Fn(R, Off, 0u); }
				}
				else
				{
					// The cell holds (2^(L-P))^3 whole tiles (cells are aligned to their size).
					const int Sh = std::min(L - P, 3);
					const u32 N = 1u << Sh;
					for (u32 Tz = 0; Tz < N; ++Tz) for (u32 Ty = 0; Ty < N; ++Ty) for (u32 Tx = 0; Tx < N; ++Tx)
					{
						const u32 T[3] = { Tx, Ty, Tz };
						u32 Off[3];
						for (int Ax = 0; Ax < 3; ++Ax)
						{
							const FU128 Rel = Add(Shl(FU128::FromU64(T[Ax]), P), R.E[Ax]);
							Off[Ax] = L >= 32 ? u32(Shr(Rel, L - 32).L[3]) : u32(Shl(Rel, 32 - L).L[3]);
						}
						Fn(R, Off, (Tz * N + Ty) * N + Tx);
					}
				}
			}
		}

		const FUniverseModel& ModelRef() const { return Model; }
		const std::vector<FContributor>& RootContributors() const { return Roots; }

		/** Configurable for tests (lesson #17: config, not rolled). */
		u32 MaxPerCell = H::UNI_MAX_GENERATED_PER_CELL;
		/** Test hook: scale every lambda (fidelity tests force large counts). 1 in production. */
		double LambdaMultiplier = 1.0;

		/** Does contributor C emit at level L and overlap cell K? Root contributors overlap all. */
		bool Overlaps(const FContributor& C, const FCellKey& K) const
		{
			const FSliceTable& T = Model.SlicesOf(C.Emitter->ChildArchetypeId);
			if (K.Level < T.LMin || K.Level > T.LMax) { return false; }
			if (C.bRoot) { return true; }
			const FVec3d D = ToDouble(Sub(C.Centre, CellOrigin(K)));   // centre relative to cell min corner
			const double W = CellWidthCm(K.Level);
			const double P[3] = { D.X, D.Y, D.Z };
			double Dist2 = 0.0;
			for (double V : P) { const double E = V < 0.0 ? -V : (V > W ? V - W : 0.0); Dist2 += E * E; }
			return Dist2 <= C.InfluenceCm * C.InfluenceCm;
		}

		/** Field frame of cell K for contributor C. */
		FFieldFrameGPU Frame(const FContributor& C, const FCellKey& K) const
		{
			FFieldFrameGPU F{};
			const double W = CellWidthCm(K.Level);
			if (C.Emitter->bDensityMode || C.bRoot)
			{
				// Periodic: exact (coord mod 2^PeriodBits) / 2^PeriodBits.
				const int PB = C.Emitter->PeriodBits;
				const FCoord O = CellOrigin(K);
				for (int A = 0; A < 3; ++A) { F.OriginP[A] = float(U128ToDouble(MaskLow(O.Axis(A), PB)) / std::ldexp(1.0, PB)); }
				F.SizeP = float(W / std::ldexp(1.0, PB));
				F.Periodic = 1u;
				F.Rot0[0] = 1; F.Rot1[1] = 1; F.Rot2[2] = 1;
			}
			else
			{
				const FVec3d D = ToDouble(Sub(CellOrigin(K), C.Centre));
				F.OriginP[0] = float(D.X / C.InfluenceCm); F.OriginP[1] = float(D.Y / C.InfluenceCm); F.OriginP[2] = float(D.Z / C.InfluenceCm);
				F.SizeP = float(W / C.InfluenceCm);
				F.Periodic = 0u;
				for (int I = 0; I < 3; ++I) { F.Rot0[I] = C.Rot[I]; F.Rot1[I] = C.Rot[3 + I]; F.Rot2[I] = C.Rot[6 + I]; }
			}
			return F;
		}

		static H::FUniFieldFrame ToHlsl(const FFieldFrameGPU& F)
		{
			H::FUniFieldFrame R;
			R.OriginP = H::float3(F.OriginP[0], F.OriginP[1], F.OriginP[2]);
			R.SizeP = F.SizeP;
			R.Rot0 = H::float3(F.Rot0[0], F.Rot0[1], F.Rot0[2]);
			R.Rot1 = H::float3(F.Rot1[0], F.Rot1[1], F.Rot1[2]);
			R.Rot2 = H::float3(F.Rot2[0], F.Rot2[1], F.Rot2[2]);
			R.Periodic = F.Periodic;
			R.Pad0 = R.Pad1 = 0.0f;
			return R;
		}

		/** Field-space integral: over the unit ball's bounding cube [-1, 1]^3 for contributor
		 *  fields, the mean over one period [0, 1)^3 for periodic fields. Computed with THE SAME
		 *  estimator the cells use (128 stratified probes per cell) over a 32^3 tiling, so cell
		 *  integrals and the normaliser agree and slicing conserves N_total (D-115). One jittered
		 *  sample per stratum on a 64^3 grid was measured 4% high on the galaxy field: its thin
		 *  disc and bulge cusp are sharper than a stratum. Cached per contributor (lesson #28). */
		double IntegralAll(const FContributor& C) const
		{
			if (C.IntegralAll >= 0.0) { return C.IntegralAll; }
			// D-140: also cached by contributor id. A contributor is re-created from its parent
			// object every time the parent's cell reloads (the record above starts at -1 again),
			// but its id fixes its parameters, so the value is the same.
			auto It = IntegralCache.find(C.Id);
			if (It != IntegralCache.end()) { C.IntegralAll = It->second; return C.IntegralAll; }
			C.IntegralAll = ComputeIntegral(C);
			IntegralCache[C.Id] = C.IntegralAll;
			if (!FieldHasAnalyticIntegral(C.Emitter->FieldId)) { ++InlineIntegrals; }
			return C.IntegralAll;
		}

		/** D-140: integrals computed inline (on the calling thread), for the checks. */
		mutable size_t InlineIntegrals = 0;

		/** D-140: is the integral known without computing it? */
		bool HasIntegral(const FContributor& C) const { return C.IntegralAll >= 0.0 || FieldHasAnalyticIntegral(C.Emitter->FieldId) || IntegralCache.count(C.Id) != 0; }
		/** D-140: store a value computed elsewhere (the streamer's worker), by id. */
		void StoreIntegral(u32 Id, double Value) const { IntegralCache[Id] = Value; }

		/** The integral itself: pure (reads only the contributor and the shared field core), so
		 *  it can run on a worker thread. About 2.4M field evaluations for a local field (D-123). */
		static double ComputeIntegral(const FContributor& C)
		{
			const bool bPeriodic = C.Emitter->bDensityMode || C.bRoot;
			const H::FUniFieldParams P = C.Params.ToHlsl();
			if (!bPeriodic && C.Emitter->FieldId == H::UNI_FIELD_CLUSTER) { return ClusterFieldIntegral(P); }   // D-151
			// D-165: a textured web without a power is linear in its channels: its period mean is
			// exact from the texture's means. Sampling it misses by several % on spiky channels
			// (the adhesion web's node matter peaks at ~1250x its mean), and counts follow the mean.
			if (bPeriodic && C.Emitter->FieldId == H::UNI_FIELD_WEB && H::Uni_WebIsTextured(P) && !(P.C.x > 0.0f && P.C.x != 1.0f))
			{
				const H::float4 Mn = H::UniWebTexMean();
				double Mean = double(P.A.z) + double(P.A.w) * Mn.x + double(P.B.x) * Mn.y;
				if (P.C.y > 0.0f) { Mean += double(P.C.y) * Mn.z; }
				if (P.D.x > 0.0f) { Mean += double(P.D.x) * Mn.w; }
				return Mean;
			}
			const int G = 32;
			const float Cell = bPeriodic ? 1.0f / float(G) : 2.0f / float(G);
			const u32 Seed0 = H::Uni_SeedMix(C.Id, 0xA11A11u);
			double Sum = 0.0;
			for (int Z = 0; Z < G; ++Z) for (int Y = 0; Y < G; ++Y) for (int X = 0; X < G; ++X)
			{
				const H::float3 O = bPeriodic ? H::float3(X * Cell, Y * Cell, Z * Cell) : H::float3(-1.0f + X * Cell, -1.0f + Y * Cell, -1.0f + Z * Cell);
				if (!bPeriodic)
				{
					// Tiles entirely outside the unit ball hold no field (every local field is cut there).
					const float Cx = std::max(std::max(-O.x - Cell, O.x), 0.0f), Cy = std::max(std::max(-O.y - Cell, O.y), 0.0f), Cz = std::max(std::max(-O.z - Cell, O.z), 0.0f);
					if (Cx * Cx + Cy * Cy + Cz * Cz >= 1.0f) { continue; }
				}
				const u32 Seed = H::Uni_SeedMix(Seed0, u32((Z * G + Y) * G + X));
				double S = 0.0;
				for (u32 K = 0; K < H::UNI_PROBES_PER_CELL; ++K)
				{
					const H::float3 U = H::Uni_ProbeUnit(Seed, K);
					S += H::Uni_FieldSample(C.Emitter->FieldId, H::float3(O.x + U.x * Cell, O.y + U.y * Cell, O.z + U.z * Cell), P);
				}
				Sum += S / double(H::UNI_PROBES_PER_CELL) * double(Cell) * double(Cell) * double(Cell);
			}
			return Sum;   // periodic: period volume is 1, so this is the mean
		}

		/** lambda per unit of probe mean, for cell K and contributor C (slice already chosen). */
		double LambdaScale(const FContributor& C, const FCellKey& K, const FSlice& S, float SizeP) const
		{
			if (C.Emitter->bDensityMode || C.bRoot)
			{
				const double W = CellWidthCm(K.Level);
				return C.NTotal * S.P * W * W * W / IntegralAll(C);
			}
			const double V = double(SizeP) * double(SizeP) * double(SizeP);
			return C.NTotal * S.P * V / IntegralAll(C);
		}

		/** The GPU task for contributor C in cell K (lambda scale, frame, seed, slice, suppressor).
		 *  Returns false if C does not emit at K's level. Pool fields are filled by allocation. */
		bool BuildTask(const FContributor& C, const FCellKey& K, FGenTaskGPU& Out) const
		{
			const FUniverseDef& D = Model.Definition();
			const u32 Child = C.Emitter->ChildArchetypeId;
			const FSlice* S = Model.SlicesOf(Child).ForLevel(K.Level);
			const int SliceIdx = Model.SliceIndex(Child, K.Level);
			if (!S || S->P <= 0.0 || SliceIdx < 0) { return false; }
			Out = FGenTaskGPU{};
			Out.Frame = Frame(C, K);
			Out.Params = C.Params.Pack();
			Out.CellSeed = CellSeed(D.Seed, K, Child, C.Id, u32(C.EmitterIndex));
			Out.FieldId = C.Emitter->FieldId;
			Out.ChildArchetypeId = Child;
			Out.SliceIndex = u32(SliceIdx);
			// D-153: the sampling box (whole cell unless the emitter asks for one), and lambda over it.
			const double BoxFrac = SampleBox(C, K, Out.BoxLo, Out.BoxSpan);
			if (!(BoxFrac > 0.0)) { return false; }
			Out.LambdaScale = float(LambdaScale(C, K, *S, Out.Frame.SizeP) * BoxFrac * LambdaMultiplier);
			// One suppressor per task (the first that touches the cell), in cell units.
			const double W = CellWidthCm(K.Level);
			for (const FSuppressor& Sp : D.Suppressors)
			{
				if (Sp.ArchetypeId != Child) { continue; }
				const FVec3d Rel = ToDouble(Sub(Sp.Centre, CellOrigin(K)));
				const double P[3] = { Rel.X, Rel.Y, Rel.Z };
				double Dist2 = 0.0;
				for (double V : P) { const double E = V < 0.0 ? -V : (V > W ? V - W : 0.0); Dist2 += E * E; }
				if (Dist2 >= Sp.RadiusCm * Sp.RadiusCm) { continue; }
				Out.Suppress[0] = float(Rel.X / W); Out.Suppress[1] = float(Rel.Y / W); Out.Suppress[2] = float(Rel.Z / W);
				Out.Suppress[3] = float(Sp.RadiusCm / W);
				break;
			}
			return true;
		}

		/** D-153: the part of cell K that C's influence sphere's bounding box overlaps, as in-cell
		 *  offsets per axis, widened outward to whole 2^-32 steps; returns its volume fraction
		 *  (0: no overlap). Span 0 = whole axis. Without bSampleBox, or for root contributors, the
		 *  whole cell (fraction 1). */
		double SampleBox(const FContributor& C, const FCellKey& K, uint32_t OutLo[4], uint32_t OutSpan[4]) const
		{
			for (int A = 0; A < 4; ++A) { OutLo[A] = 0u; OutSpan[A] = 0u; }
			if (C.bRoot || !C.Emitter || !C.Emitter->bSampleBox) { return 1.0; }
			const double W = CellWidthCm(K.Level);
			const FVec3d Rel = ToDouble(Sub(C.Centre, CellOrigin(K)));
			const double Ctr[3] = { Rel.X / W, Rel.Y / W, Rel.Z / W };
			const double R = C.InfluenceCm / W;
			double Frac = 1.0;
			for (int A = 0; A < 3; ++A)
			{
				const double Lo = std::clamp(Ctr[A] - R, 0.0, 1.0), Hi = std::clamp(Ctr[A] + R, 0.0, 1.0);
				if (!(Hi > Lo)) { return 0.0; }
				if (Lo <= 0.0 && Hi >= 1.0) { continue; }                 // whole axis: identity
				const double T32 = 4294967296.0;
				const double L = std::floor(Lo * T32);
				const double S = std::min(std::ceil(Hi * T32), T32) - L;
				if (S >= T32) { continue; }
				OutLo[A] = u32(L);
				OutSpan[A] = u32(std::max(S, 1.0));
				Frac *= double(OutSpan[A]) / T32;
			}
			return Frac;
		}

		static H::FUniGenTask ToHlsl(const FGenTaskGPU& T)
		{
			H::FUniGenTask R;
			R.Frame = ToHlsl(T.Frame);
			R.Params.A = H::float4(T.Params.V[0], T.Params.V[1], T.Params.V[2], T.Params.V[3]);
			R.Params.B = H::float4(T.Params.V[4], T.Params.V[5], T.Params.V[6], T.Params.V[7]);
			R.Params.C = H::float4(T.Params.V[8], T.Params.V[9], T.Params.V[10], T.Params.V[11]);
			R.Params.D = H::float4(T.Params.V[12], T.Params.V[13], T.Params.V[14], T.Params.V[15]);
			R.CellSeed = T.CellSeed; R.FieldId = T.FieldId; R.ChildArchetypeId = T.ChildArchetypeId; R.SliceIndex = T.SliceIndex;
			R.LambdaScale = T.LambdaScale; R.PoolBase = T.PoolBase; R.PoolCapacity = T.PoolCapacity; R.IndexBase = T.IndexBase;
			R.Suppress = H::float4(T.Suppress[0], T.Suppress[1], T.Suppress[2], T.Suppress[3]);
			R.BoxLo = H::uint4(T.BoxLo[0], T.BoxLo[1], T.BoxLo[2], T.BoxLo[3]);
			R.BoxSpan = H::uint4(T.BoxSpan[0], T.BoxSpan[1], T.BoxSpan[2], T.BoxSpan[3]);
			return R;
		}

		H::FUniSlice SliceHlsl(u32 Index) const
		{
			const FSliceGPU& G = Model.GpuSlices()[Index];
			H::FUniSlice S;
			S.CdfLo = G.CdfLo; S.CdfHi = G.CdfHi; S.LutBase = G.LutBase; S.LutCount = G.LutCount;
			S.RadiusModel = H::float4(G.RadiusModel[0], G.RadiusModel[1], G.RadiusModel[2], G.RadiusModel[3]);
			S.ColourModel = H::float4(G.ColourModel[0], G.ColourModel[1], G.ColourModel[2], G.ColourModel[3]);
			S.ChildFlags = G.ChildFlags; S.Pad = 0u; S.BrightM = G.BrightM; S.FaintM = G.FaintM;
			return S;
		}

		/** Probe phase on the CPU: the GPU's PASS 0 for one task. */
		FTaskCountGPU RunProbe(const FGenTaskGPU& T) const
		{
			const H::FUniGenTask HT = ToHlsl(T);
			float Sum = 0.0f, Max = 0.0f;
			for (u32 K = 0; K < H::UNI_PROBES_PER_CELL; ++K) { const float V = H::Uni_TaskProbe(HT, K); Sum += V; Max = std::max(Max, V); }
			const H::FUniTaskCount C = H::Uni_TaskCount(HT, Sum, Max);
			return FTaskCountGPU{ C.Count, C.CandidateBudget, C.Mean, C.Envelope };
		}

		/** Place phase on the CPU: the GPU's PASS 1 for one task, candidates in index order (the
		 *  GPU's chunked scan gives the same order). Writes T.PoolCapacity records into Out
		 *  (accepted objects, suppressed and unfilled slots as blanks) and fills St. */
		void RunPlace(const FGenTaskGPU& T, const FTaskCountGPU& C, std::vector<FObjectGPU>& OutRecords, FTaskStats& St) const
		{
			const H::FUniGenTask HT = ToHlsl(T);
			const H::FUniSlice S = SliceHlsl(T.SliceIndex);
			const std::vector<float>& L = Model.GpuLut();
			const H::StructuredBuffer<float> Lut(L.data(), u32(L.size()));
			OutRecords.assign(T.PoolCapacity, FromHlsl(H::Uni_BlankObject()));
			u32 Accepted = 0;
			for (u32 J = 0; Accepted < T.PoolCapacity && J < C.CandidateBudget; ++J)
			{
				H::uint3 Off; float Dens;
				const bool bAcc = H::Uni_TaskCandidate(HT, C.Envelope, J, Off, Dens);
				++St.Candidates;
				if (double(Dens) > double(C.Envelope)) { ++St.Clipped; }
				if (!bAcc) { continue; }
				const u32 Local = Accepted++;
				if (H::Uni_TaskSuppressed(HT, Off)) { continue; }          // masked: index consumed, slot stays blank
				OutRecords[Local] = FromHlsl(H::Uni_TaskMakeObject(HT, S, Lut, Local, Off));
				++St.Emitted;
			}
			St.Accepted = Accepted;
		}

		/** Generate one cell from the given contributors (all that overlap it): build tasks,
		 *  probe, allocate (prefix sum of n, per-cell truncation keeps the lowest indices), place. */
		void GenerateCell(const FCellKey& K, std::vector<const FContributor*> Contribs, FCellContent& Out) const
		{
			const FUniverseDef& D = Model.Definition();
			Out = FCellContent();
			Out.Key = K;
			std::sort(Contribs.begin(), Contribs.end(), [](const FContributor* A, const FContributor* B) { return A->Id != B->Id ? A->Id < B->Id : A->EmitterIndex < B->EmitterIndex; });
			u32 IndexBase = 0;

			for (const FContributor* C : Contribs)
			{
				if (!Overlaps(*C, K)) { continue; }
				Out.ContributorSet.push_back(C->Id);
				FGenTaskGPU T;
				if (!BuildTask(*C, K, T)) { continue; }
				FTaskStats St;
				St.ContributorId = C->Id;
				const FTaskCountGPU Cnt = RunProbe(T);
				St.Mean = Cnt.Mean;
				St.Envelope = Cnt.Envelope;
				St.Lambda = double(T.LambdaScale) * double(Cnt.Mean);
				u32 N = Cnt.Count;
				const u32 Room = IndexBase < MaxPerCell ? MaxPerCell - IndexBase : 0u;
				if (N > Room) { St.Truncated = N - Room; N = Room; }
				St.Count = N;
				T.IndexBase = IndexBase;
				T.PoolCapacity = N;
				std::vector<FObjectGPU> Records;
				RunPlace(T, Cnt, Records, St);
				for (u32 I = 0; I < N; ++I)
				{
					if ((Records[I].A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }   // blank (suppressed or unfilled)
					Out.Objects.push_back(Records[I]);
					Out.Indices.push_back(IndexBase + I);
					Out.ObjectContributor.push_back(C->Id);
				}
				IndexBase += N;   // by the drawn count: known after the probe phase (UniGenTask.ush)
				Out.Tasks.push_back(St);
			}

			// Anchors that live in this cell, at reserved indices.
			for (size_t I = 0; I < D.Anchors.size(); ++I)
			{
				if (!(AnchorCells[I] == K)) { continue; }
				const FAnchor& A = D.Anchors[I];
				const FCoord O = CellOrigin(K);
				u32 Off[3];
				for (int Ax = 0; Ax < 3; ++Ax)
				{
					const FU128 Rel = Sub(A.Coord.Axis(Ax), O.Axis(Ax));   // 0 .. 2^L - 1
					Off[Ax] = K.Level >= 32 ? u32(Shr(Rel, K.Level - 32).L[3]) : u32(Shl(Rel, 32 - K.Level).L[3]);
				}
				const FArchetype& AA = D.Arch(A.ArchetypeId);
				const u32 Flags = H::UNI_OBJ_FLAG_ANCHOR | (AA.Visibility == EUniVisibility::Extended ? H::UNI_OBJ_FLAG_EXTENDED : 0u) | (AA.Emitters.empty() ? 0u : H::UNI_OBJ_FLAG_HAS_CONTRIBUTOR);
				const float SB = AA.Visibility == EUniVisibility::Extended ? SurfaceBrightness(A.AbsMag, A.RadiusLog2) : 0.0f;
				const H::float3 Rgb = H::Uni_TemperatureColour(std::pow(10.0f, AA.ColourModel[0]));
				Out.Objects.push_back(FromHlsl(H::Uni_MakeObject(H::uint3(Off[0], Off[1], Off[2]), A.ArchetypeId, Flags, A.Seed, A.RadiusLog2, A.AbsMag, SB, Rgb)));
				Out.Indices.push_back(kFirstAnchorIndex + u32(I));
				Out.ObjectContributor.push_back(0u);
			}

			// D-167: catalogue objects in this cell, at reserved indices. Magnitude and position are
			// the entry's; radius and colour are drawn from the copy's own seed, as for generated
			// objects (Uni_TaskMakeObject), so copies differ.
			ForEachCatalogueObject(K, [&](const FCatRef& R, const u32* Off, u32 Copy)
			{
				const FCatalogue& Cat = D.Catalogues[R.Cat];
				const FArchetype& AA = D.Arch(Cat.ArchetypeId);
				const u32 Local = Copy * u32(Cat.Entries.size()) + R.Entry;
				const u32 Seed = H::Uni_ObjectSeed(CellSeed(D.Seed, K, Cat.ArchetypeId, kCatalogueContributor + R.Cat, 0u), Local);
				const float M = Cat.Entries[R.Entry].AbsMag;
				const H::float4 Rm(AA.RadiusModel[0], AA.RadiusModel[1], AA.RadiusModel[2], AA.RadiusModel[3]);
				const H::float4 Cm(AA.ColourModel[0], AA.ColourModel[1], AA.ColourModel[2], AA.ColourModel[3]);
				const float RL2 = H::Uni_DrawRadiusLog2(Seed, M, Rm);
				const H::float3 Rgb = H::Uni_DrawColour(Seed, M, Cm);
				const bool bExt = AA.Visibility == EUniVisibility::Extended;
				const u32 Flags = H::UNI_OBJ_FLAG_ANCHOR | (bExt ? H::UNI_OBJ_FLAG_EXTENDED : 0u) | (AA.Emitters.empty() ? 0u : H::UNI_OBJ_FLAG_HAS_CONTRIBUTOR);
				const float SB = bExt ? SurfaceBrightness(M, RL2) : 0.0f;
				Out.Objects.push_back(FromHlsl(H::Uni_MakeObject(H::uint3(Off[0], Off[1], Off[2]), Cat.ArchetypeId, Flags, Seed, RL2, M, SB, Rgb)));
				Out.Indices.push_back(kFirstCatalogueIndex + Local);
				Out.ObjectContributor.push_back(0u);
			});
			Out.Hash = HashContent(Out);
		}

		/** Contributors created by the objects of a generated cell (pass 4). */
		std::vector<FContributor> MakeContributors(const FCellContent& C) const
		{
			std::vector<FContributor> Out;
			for (size_t I = 0; I < C.Objects.size(); ++I) { MakeContributorsFor(C, I, Out); }
			return Out;
		}

		/** D-168: the contributors of object I of a cell only (appended to Out). */
		void MakeContributorsFor(const FCellContent& C, size_t I, std::vector<FContributor>& Out) const
		{
			const FUniverseDef& D = Model.Definition();
			{
				const FObjectGPU& O = C.Objects[I];
				const u32 Arch = O.A[3] & 0xFFFFu;
				const FArchetype& A = D.Arch(Arch);
				if (A.Emitters.empty()) { return; }
				const bool bAnchor = ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_ANCHOR) != 0u;
				const FAnchor* Anc = nullptr;
				if (bAnchor) { for (const FAnchor& X : D.Anchors) { if (X.Seed == O.B[0]) { Anc = &X; } } }
				const H::FUniObject HO = UniCore::ToHlsl(O);
				const float M = H::Uni_ObjectAbsMag(HO);
				const double Radius = std::exp2(double(H::Uni_ObjectRadiusLog2(HO)));
				const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
				for (size_t E = 0; E < A.Emitters.size(); ++E)
				{
					const FUniEmitter& Em = A.Emitters[E];
					FContributor Ct;
					Ct.Parent.Cell = C.Key;
					Ct.Parent.Index = C.Indices[I];
					Ct.Id = H::Uni_SeedMix(H::Uni_SeedMix(O.B[0], H::UNI_CH_CONTRIBUTOR), u32(E));
					Ct.ParentArchetypeId = Arch;
					Ct.EmitterIndex = int(E);
					Ct.Emitter = &Em;
					Ct.Centre = ObjectCoord(C.Key, Off);
					Ct.InfluenceCm = Em.InfluenceMul * Radius;
					// Exemplar -> ranges (keyed by name) -> [bags] -> [logic hook] (lesson #15).
					Ct.Params = Em.Exemplar;
					if (Anc && E < Anc->EmitterParams.size()) { Ct.Params = Anc->EmitterParams[E]; }
					else if (!A.bDefaultsOnly)
					{
						for (const FParamRange& R : Em.Ranges)
						{
							Ct.Params.V[R.Slot] = H::Uni_RollRange(O.B[0], ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize);
						}
					}
					Ct.NTotal = std::isfinite(Em.NTotalRefMag) ? Em.NTotal * std::pow(10.0, -0.4 * (double(M) - Em.NTotalRefMag)) : Em.NTotal;
					if (Em.bRandomOrientation && !(Anc && Anc->bFixedOrientation)) { RandomRotation(O.B[0], Ct.Rot); }
					Out.push_back(Ct);
				}
			}
		}

		const std::vector<int>& AnchorLevelList() const { return AnchorLevels; }
		const std::vector<FCellKey>& AnchorCellList() const { return AnchorCells; }

	private:
		mutable std::unordered_map<u32, double> IntegralCache;   // D-140: by contributor id
		/** Uniform random rotation (quaternion from three uniforms, Shoemake). Rows into M. */
		static void RandomRotation(u32 Seed, float M[9])
		{
			const double U1 = H::Uni_DrawUnit(Seed, H::UNI_CH_ORIENT, 0u), U2 = H::Uni_DrawUnit(Seed, H::UNI_CH_ORIENT, 1u), U3 = H::Uni_DrawUnit(Seed, H::UNI_CH_ORIENT, 2u);
			const double A = std::sqrt(1 - U1), B = std::sqrt(U1);
			const double X = A * std::sin(6.283185307179586 * U2), Y = A * std::cos(6.283185307179586 * U2);
			const double Z = B * std::sin(6.283185307179586 * U3), W = B * std::cos(6.283185307179586 * U3);
			const double R[9] = {
				1 - 2 * (Y * Y + Z * Z), 2 * (X * Y - Z * W), 2 * (X * Z + Y * W),
				2 * (X * Y + Z * W), 1 - 2 * (X * X + Z * Z), 2 * (Y * Z - X * W),
				2 * (X * Z - Y * W), 2 * (Y * Z + X * W), 1 - 2 * (X * X + Y * Y) };
			for (int I = 0; I < 9; ++I) { M[I] = float(R[I]); }
		}

		const FUniverseModel& Model;
		std::vector<FContributor> Roots;
		std::vector<int> AnchorLevels;
		std::vector<FCellKey> AnchorCells;
		// D-167: catalogue entries and their levels.
		struct FCatRef { u32 Cat = 0; u32 Entry = 0; int Level = 0; FU128 E[3]; };
		std::vector<FCatRef> CatRefs;
		std::map<int, std::vector<size_t>> CatByLevel;
		static constexpr u32 kCatalogueContributor = 0xCA7A0000u;   // the "contributor" in a catalogue copy's cell seed
	};

	// ======================================================================================
	// Streaming pipeline: per-level windows driving generation coarse to fine
	// ======================================================================================

	struct FLevelStats
	{
		size_t Resident = 0;          // objects in the window
		size_t MaxContributors = 0;   // gather list length (D-004 trigger: 256)
		size_t Generated = 0;         // cells generated (cumulative)
		size_t Restored = 0;          // cells restored from cache (cumulative)
		size_t Regenerated = 0;       // cells regenerated because their contributor set grew (D-113)
		size_t Truncated = 0;         // objects dropped by per-cell caps (cumulative)
	};

	class FRefPipeline
	{
	public:
		explicit FRefPipeline(const FRefGenerator& InGen) : Gen(InGen)
		{
			const FUniverseDef& D = Gen.ModelRef().Definition();
			std::set<int> Ls;
			for (const FUniEmitter& E : D.RootEmitters) { AddRange(Ls, E.ChildArchetypeId, 0); }
			for (const FAnchor& A : D.Anchors) { Ls.insert(Gen.ModelRef().LevelOf(A.ArchetypeId, A.AbsMag)); AddRange(Ls, A.ArchetypeId, 1); }
			for (const auto& KV : Gen.CatalogueLevels()) { Ls.insert(KV.first); for (u32 A : KV.second) { AddRange(Ls, A, 1); } }   // D-167
			for (int L : Ls) { Levels.emplace(L, FLevel(L)); }
		}

		/** Levels the pipeline tracks, coarse first. */
		std::vector<int> LevelList() const { std::vector<int> R; for (auto It = Levels.rbegin(); It != Levels.rend(); ++It) { R.push_back(It->first); } return R; }

		double DeltaMag = 0.0;
		/** Evict a cell's content (and the contributors its objects own) as soon as it leaves
		 *  its window, instead of keeping it in the cell cache. Tests use it to prove that
		 *  regeneration, and the relevance guarantee behind it, are deterministic. */
		bool bEvictOnExit = false;
		int VelocityK = UniHlsl::UNI_VELOCITY_LOD_K;
		bool bVelocityLod = true;
		std::function<void(const FCellContent&)> OnGenerated;   // test hook
		/** Test hook: a cell entered a window (generated, restored or regenerated) this frame,
		 *  with its level's render fade at that moment. Pop analysis listens here. */
		std::function<void(const FCellContent&, float)> OnEnterWindow;

		/** Advance one frame with the camera at Coord + Frac. Generates every entering cell of
		 *  every non-suspended level, coarse to fine, so a cell's parent window is always
		 *  complete before it generates. The reference generates instantly; GPU latency across
		 *  levels is an M2 measurement (UE). */
		void Update(const FCoord& Coord, const FVec3f& Frac)
		{
			double Moved = 0.0;
			if (bHasCamera) { Moved = Length(ToDouble(Sub(Coord, Camera))); }
			Camera = Coord; CameraFrac = Frac; bHasCamera = true;
			const double Reach = double(UniHlsl::Uni_WindowReach(float(DeltaMag)));   // D-162: 5^3 at dm 0
			const double NoLead[3] = { 0.0, 0.0, 0.0 };
			bMeasureDirty = true;   // the camera moved: every offset changes

			for (auto It = Levels.rbegin(); It != Levels.rend(); ++It)
			{
				FLevel& Lv = It->second;
				const int L = It->first;
				const double CellsPerFrame = Moved / CellWidthCm(L);
				bool bSusp = bVelocityLod && Lv.Lod.Update(CellsPerFrame, VelocityK);
				// D-116: a level that resumes before it has fully faded out, and whose window would
				// jump, keeps fading out first. Content then never blinks from partial brightness.
				const bool bJump = Lv.Win.IsValid() && CentreJump(Lv.Win.Geometry(), CellOf(Camera, L)) > 1;
				if (bSusp) { Lv.bHeld = true; }
				else if (Lv.bHeld && bJump && Lv.Fade > 0.0f) { bSusp = true; }
				else { Lv.bHeld = false; }
				Lv.Fade = bSusp ? std::max(0.0f, Lv.Fade - 0.125f) : std::min(1.0f, Lv.Fade + 0.125f);
				if (bSusp && Lv.Win.IsValid()) { continue; }   // keeps contents; render fades it

				// D-126: a sensitivity resize keeps every cell still in the window (content and
				// fade); only the new outer shell enters, and it lies beyond relevance.
				bool bResized = false;
				auto OldInWindow = Lv.InWindow;
				// D-162: the same window plan as the runtime streamer (at rest: no forward stretch).
				const int MinSide[3] = { 1, 1, 1 };
				const FWindowGeometry Next = PlanWindow(L, Camera, Reach, NoLead, NoLead, MinSide, &Lv.Win);
				if (!Lv.Win.IsValid() || !Lv.Win.Geometry().SameShape(Next))
				{
					if (Lv.Win.IsValid()) { Lv.InWindow.clear(); bResized = true; }
					Lv.Win = FLevelWindow(Next);
				}

				// Gather first, against the window the camera is about to have: contributors
				// emitting at L that overlap it (D-004 list). With none, the level is inactive and
				// costs nothing; reactivation rebuilds it (design: Level windows, Activation).
				std::vector<const FContributor*> Gathered = Gather(Next);
				Lv.Stats.MaxContributors = std::max(Lv.Stats.MaxContributors, Gathered.size());
				std::vector<u32> GatherIds;
				GatherIds.reserve(Gathered.size());
				for (const FContributor* C : Gathered) { GatherIds.push_back(C->Id); }
				const bool bGatherChanged = GatherIds != Lv.GatherIds;
				Lv.GatherIds = std::move(GatherIds);
				if (Gathered.empty() && !AnchorInWindow(Next))
				{
					if (Lv.Win.IsValid())
					{
						for (const FCellKey& K : Lv.InWindow) { if (bEvictOnExit) { Evict(K); } }
						Lv.Win = FLevelWindow(Next);
						Lv.InWindow.clear();
					}
					Lv.bActive = false;
					Lv.Stats.Resident = 0;
					continue;
				}
				Lv.bActive = true;

				const FWindowDiff Diff = Lv.Win.Recentre(Next);
				// A rebuilt level, or a window that jumped more than one cell (resume after velocity
				// LOD, teleport), can hold cells inside relevance: it fades in from zero (D-116).
				if (((Diff.bRebuilt && !bResized) || bJump) && !bFirstFrame) { Lv.Fade = 0.0f; }

				for (const FCellKey& K : Diff.Exiting) { Lv.InWindow.erase(K); if (bEvictOnExit) { Evict(K); } }
				for (const FWindowCell& WC : Diff.Entering)
				{
					const bool bWasResident = bResized && OldInWindow.count(WC.Key) != 0;
					EnsureCell(Lv, WC.Key, Gathered, !bWasResident);   // kept cells do not "enter"
					Lv.InWindow.insert(WC.Key);
				}
				if (bResized && bEvictOnExit) { for (const FCellKey& K : OldInWindow) { if (!Lv.InWindow.count(K)) { Evict(K); } } }
				// D-113: cells whose contributor set grew are regenerated. A cell's set is the gathered
				// list filtered by the exact overlap test, so with the same gathered list (and no
				// resize) every kept cell would find its own set again: skipping is exact (D-151).
				if (bGatherChanged || bResized) { for (const FCellKey& K : Lv.InWindow) { EnsureCell(Lv, K, Gathered, false); } }

				size_t Res = 0;
				for (const FCellKey& K : Lv.InWindow) { Res += Cache.at(K).Objects.size(); }
				Lv.Stats.Resident = Res;
			}
			bFirstFrame = false;
		}

		/** Every resident object (in a window): its cell, record, level fade and identity hash. */
		void ForEachResident(const std::function<void(const FCellKey&, const FObjectGPU&, float, u64)>& Fn) const
		{
			for (const auto& KV : Levels)
			{
				for (const FCellKey& K : KV.second.InWindow)
				{
					const FCellContent& C = Cache.at(K);
					for (size_t I = 0; I < C.Objects.size(); ++I)
					{
						FObjectId Id; Id.Cell = K; Id.Index = C.Indices[I];
						Fn(K, C.Objects[I], KV.second.Fade, HashObject(Id));
					}
				}
			}
		}

		const std::map<int, FLevelStats> Stats() const
		{
			std::map<int, FLevelStats> S;
			for (const auto& KV : Levels) { S[KV.first] = KV.second.Stats; }
			return S;
		}

		const std::unordered_map<FCellKey, FCellContent, FCellKeyHasher>& CacheRef() const { return Cache; }
		size_t TotalResident() const { size_t N = 0; for (const auto& KV : Levels) { N += KV.second.Stats.Resident; } return N; }
		const FCoord& CameraCoord() const { return Camera; }
		const FVec3f& CameraFraction() const { return CameraFrac; }
		float LevelFade(int L) const { auto It = Levels.find(L); return It == Levels.end() ? 0.0f : It->second.Fade; }
		bool LevelSuspended(int L) const { auto It = Levels.find(L); return It != Levels.end() && It->second.Lod.bSuspended; }
		bool LevelActive(int L) const { auto It = Levels.find(L); return It != Levels.end() && It->second.bActive; }
		size_t ContributorCount() const { return Registered.size(); }
		/** Resident contributors by id (the volume builds its carriers from these). */
		const std::map<u32, FContributor>& Contributors() const { return Registered; }
		/** The resident object an id names, or nullptr. */
		const FObjectGPU* FindObject(const FObjectId& Id) const
		{
			auto It = Cache.find(Id.Cell);
			if (It == Cache.end()) { return nullptr; }
			for (size_t I = 0; I < It->second.Indices.size(); ++I) { if (It->second.Indices[I] == Id.Index) { return &It->second.Objects[I]; } }
			return nullptr;
		}

	private:
		struct FLevel
		{
			std::vector<u32> GatherIds;   // last frame's gathered ids (D-151: skip the D-113 pass when unchanged)
			explicit FLevel(int L) : Win(L, UniHlsl::UNI_WINDOW_N) {}
			FLevelWindow Win;
			FVelocityLod Lod;
			float Fade = 1.0f;
			bool bActive = false;
			bool bHeld = false;   // suspended, or still fading out before a resume jump
			std::set<FCellKey, bool (*)(const FCellKey&, const FCellKey&)> InWindow{ &KeyLess };
			FLevelStats Stats;
		};

		static bool KeyLess(const FCellKey& A, const FCellKey& B)
		{
			if (A.Level != B.Level) { return A.Level < B.Level; }
			for (int I = 0; I < 3; ++I) { const int C = FU128::Compare(A.C[I], B.C[I]); if (C) { return C < 0; } }
			return false;
		}

		void AddRange(std::set<int>& Ls, u32 ParentArch, int Depth)
		{
			if (Depth > 8) { return; }
			const FUniverseDef& D = Gen.ModelRef().Definition();
			const FSliceTable& T = Gen.ModelRef().SlicesOf(ParentArch);
			if (Depth == 0) { for (int L = T.LMin; L <= T.LMax; ++L) { Ls.insert(L); } }
			for (const FUniEmitter& E : D.Arch(ParentArch).Emitters)
			{
				const FSliceTable& C = Gen.ModelRef().SlicesOf(E.ChildArchetypeId);
				for (int L = C.LMin; L <= C.LMax; ++L) { Ls.insert(L); }
				AddRange(Ls, E.ChildArchetypeId, Depth + 1);
			}
		}

		/** Chebyshev distance in cells between a window's centre and a new centre (toroidal),
		 *  saturating at 2^30. */
		static int64_t CentreJump(const FWindowGeometry& G, const FCellKey& NewCentre)
		{
			int64_t J = 0;
			const int Bits = G.AxisBits();
			for (int A = 0; A < 3; ++A)
			{
				const FU128 D = MaskLow(Sub(NewCentre.C[A], G.Centre.C[A]), Bits);
				const FU128 N = MaskLow(Sub(G.Centre.C[A], NewCentre.C[A]), Bits);
				const FU128 M = FU128::Compare(D, N) < 0 ? D : N;
				const int64_t V = (M.L[0] | M.L[1] | M.L[2]) != 0u || M.L[3] > (1u << 30) ? (int64_t(1) << 30) : int64_t(M.L[3]);
				J = std::max(J, V);
			}
			return J;
		}

		/** Authored anchors make their own level active: nothing emits them. So does a catalogue
		 *  (D-167): it repeats every tile, so every window at its level holds some of it. */
		bool AnchorInWindow(const FWindowGeometry& G) const
		{
			if (Gen.CatalogueAtLevel(G.Level)) { return true; }
			const std::vector<int>& Ls = Gen.AnchorLevelList();
			const std::vector<FCellKey>& Ks = Gen.AnchorCellList();
			for (size_t I = 0; I < Ls.size(); ++I)
			{
				if (Ls[I] != G.Level) { continue; }
				for (const FWindowCell& C : G.Cells()) { if (C.Key == Ks[I]) { return true; } }
			}
			return false;
		}

		/** D-151: every registered contributor's child level range and camera offset, once per
		 *  frame (as the runtime streamer does, D-138), in id order. */
		struct FMeasuredRef { const FContributor* C; int LMin, LMax; FVec3d D; };
		std::vector<FMeasuredRef> Measured;
		bool bMeasureDirty = true;   // set whenever Registered changes, so a contributor registered by a
		                             // coarser level this frame is gathered by finer levels this frame

		void MeasureContributors()
		{
			Measured.clear();
			Measured.reserve(Registered.size());
			for (const auto& KV : Registered)
			{
				const FSliceTable& T = Gen.ModelRef().SlicesOf(KV.second.Emitter->ChildArchetypeId);
				Measured.push_back({ &KV.second, T.LMin, T.LMax, ToDouble(Sub(KV.second.Centre, Camera)) });
			}
		}

		std::vector<const FContributor*> Gather(const FWindowGeometry& G)
		{
			if (bMeasureDirty) { MeasureContributors(); bMeasureDirty = false; }
			std::vector<const FContributor*> Out;
			const int L = G.Level;
			for (const FContributor& R : Gen.RootContributors())
			{
				const FSliceTable& T = Gen.ModelRef().SlicesOf(R.Emitter->ChildArchetypeId);
				if (L >= T.LMin && L <= T.LMax) { Out.push_back(&R); }
			}
			// Window AABB relative to the centre cell's origin: [Lo w, (Hi+1) w] per axis. Offsets are
			// camera-relative doubles; the margin covers their rounding, so the list is a superset
			// of the exact one and the exact per-cell overlap test (EnsureCell) decides.
			const double W = CellWidthCm(L);
			const FVec3d Ow = ToDouble(Sub(CellOrigin(G.Centre), Camera));
			double Lo[3], Hi[3];   // D-162: per-axis box
			for (int A = 0; A < 3; ++A) { Lo[A] = G.Lo[A] * W; Hi[A] = (G.Hi[A] + 1) * W; }
			const bool bCovered = G.AxisCovered();
			for (const FMeasuredRef& M : Measured)
			{
				if (L < M.LMin || L > M.LMax) { continue; }
				if (!bCovered)
				{
					const double P[3] = { M.D.X - Ow.X, M.D.Y - Ow.Y, M.D.Z - Ow.Z };
					double Dist2 = 0.0;
					for (int A = 0; A < 3; ++A)
					{
						const double V = P[A];
						const double E = V < Lo[A] ? Lo[A] - V : (V > Hi[A] ? V - Hi[A] : 0.0);
						Dist2 += E * E;
					}
					const double R = M.C->InfluenceCm * (1.0 + 1.0e-9) + 1.0e-9 * (Length(M.D) + Length(Ow)) + 1.0;
					if (Dist2 > R * R) { continue; }
				}
				Out.push_back(M.C);
			}
			return Out;
		}

		/** Generate (or restore, or regenerate) a cell so its content uses every gathered
		 *  contributor that overlaps it. */
		void EnsureCell(FLevel& Lv, const FCellKey& K, const std::vector<const FContributor*>& Gathered, bool bEntering)
		{
			std::vector<const FContributor*> Mine;
			std::vector<u32> Ids;
			for (const FContributor* C : Gathered) { if (Gen.Overlaps(*C, K)) { Mine.push_back(C); Ids.push_back(C->Id); } }
			std::sort(Ids.begin(), Ids.end());
			auto It = Cache.find(K);
			if (It != Cache.end())
			{
				if (It->second.ContributorSet == Ids)
				{
					if (bEntering) { ++Lv.Stats.Restored; if (OnEnterWindow) { OnEnterWindow(It->second, Lv.Fade); } }
					return;
				}
				if (!bEntering && Ids.size() <= It->second.ContributorSet.size() && std::includes(It->second.ContributorSet.begin(), It->second.ContributorSet.end(), Ids.begin(), Ids.end()))
				{
					return;   // a contributor left the window list but still exists: content unchanged
				}
				Unregister(It->second);
				++Lv.Stats.Regenerated;
			}
			FCellContent C;
			Gen.GenerateCell(K, Mine, C);
			for (const FTaskStats& T : C.Tasks) { Lv.Stats.Truncated += T.Truncated; }
			++Lv.Stats.Generated;
			for (FContributor& Ct : Gen.MakeContributors(C)) { Registered[Ct.Id] = Ct; bMeasureDirty = true; }
			if (OnGenerated) { OnGenerated(C); }
			if (OnEnterWindow) { OnEnterWindow(C, Lv.Fade); }
			Cache[K] = std::move(C);
		}

		void Evict(const FCellKey& K)
		{
			auto It = Cache.find(K);
			if (It == Cache.end()) { return; }
			Unregister(It->second);
			Cache.erase(It);
		}

		void Unregister(const FCellContent& C)
		{
			for (const FContributor& Ct : Gen.MakeContributors(C)) { Registered.erase(Ct.Id); bMeasureDirty = true; }
		}

		const FRefGenerator& Gen;
		std::map<int, FLevel> Levels;
		std::unordered_map<FCellKey, FCellContent, FCellKeyHasher> Cache;
		std::map<u32, FContributor> Registered;
		FCoord Camera;
		FVec3f CameraFrac;
		bool bHasCamera = false;
		bool bFirstFrame = true;
	};
}
