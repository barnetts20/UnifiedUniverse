#pragma once

// UniArchetype.h
// Archetype, emitter, anchor and suppressor data as plain structs (UE UUniArchetype assets
// wrap these later), plus luminosity slicing.
//
// PIPELINE ORDER (lesson #15), per contributor: exemplar params -> ranges (Min, Max, Bias,
// Quantize; a stream per parameter keyed by NAME, lesson #14) -> bags (none yet) -> logic hook
// (none yet). bDefaultsOnly returns the exemplar verbatim (lesson #16).
//
// SPLIT (lesson #17): rolled field parameters live in FFieldParams. Anything that sizes a
// buffer (probe counts, caps) is config and never rolled.

#include "UniGpuTypes.h"
#include "UniIdentity.h"
#include "UniLevel.h"
#include "UniSeed.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace UniCore
{
	// ======================================================================================
	// Field parameters: one marshal site (lesson #2)
	// ======================================================================================

	struct FFieldParams
	{
		float V[16] = { 0 };

		/** THE marshal site. Field cores read A = V[0..3], B = V[4..7], C = V[8..11], D = V[12..15]. */
		FFieldParamsGPU Pack() const { FFieldParamsGPU G; for (int I = 0; I < 16; ++I) { G.V[I] = V[I]; } return G; }

		UniHlsl::FUniFieldParams ToHlsl() const
		{
			UniHlsl::FUniFieldParams P;
			P.A = UniHlsl::float4(V[0], V[1], V[2], V[3]);
			P.B = UniHlsl::float4(V[4], V[5], V[6], V[7]);
			P.C = UniHlsl::float4(V[8], V[9], V[10], V[11]);
			P.D = UniHlsl::float4(V[12], V[13], V[14], V[15]);
			return P;
		}

		void SetUint(int Slot, u32 Bits) { float F; std::memcpy(&F, &Bits, 4); V[Slot] = F; }
	};

	struct FParamRange
	{
		std::string Name;   // stream key: ChannelIdUpper(Name)
		int Slot = 0;       // index into FFieldParams::V
		float Min = 0.0f, Max = 0.0f, Bias = 1.0f, Quantize = 0.0f;
	};

	// ======================================================================================
	// Magnitude distribution (a tabulated PDF baked to CDF and inverse-CDF LUTs, D-014)
	// ======================================================================================

	struct FMagnitudeLut
	{
		double MMin = 0.0, MMax = 1.0;
		std::vector<double> Pdf;          // relative density at uniformly spaced M, MMin..MMax
		std::vector<double> Cdf;          // derived, same grid
		std::vector<float> InvCdf;        // derived, kInvCdfSize entries uniform in u
		static constexpr int kInvCdfSize = 512;

		/** From (M, log10 phi) control points, piecewise linear in log phi, sampled finely. */
		static FMagnitudeLut FromLogControlPoints(const std::vector<std::pair<double, double>>& Points, int Samples = 2049)
		{
			FMagnitudeLut L;
			L.MMin = Points.front().first;
			L.MMax = Points.back().first;
			L.Pdf.resize(size_t(Samples));
			for (int I = 0; I < Samples; ++I)
			{
				const double M = L.MMin + (L.MMax - L.MMin) * double(I) / double(Samples - 1);
				size_t K = 0;
				while (K + 2 < Points.size() && Points[K + 1].first < M) { ++K; }
				const double T = (M - Points[K].first) / (Points[K + 1].first - Points[K].first);
				L.Pdf[size_t(I)] = std::pow(10.0, Points[K].second + T * (Points[K + 1].second - Points[K].second));
			}
			L.Build();
			return L;
		}

		/** Schechter-shaped: phi(M) ~ 10^(0.4 (a + 1) (M* - M)) exp(-10^(0.4 (M* - M))). */
		static FMagnitudeLut Schechter(double MStar, double Alpha, double MBright, double MFaint, int Samples = 2049)
		{
			FMagnitudeLut L;
			L.MMin = MBright; L.MMax = MFaint;
			L.Pdf.resize(size_t(Samples));
			for (int I = 0; I < Samples; ++I)
			{
				const double M = MBright + (MFaint - MBright) * double(I) / double(Samples - 1);
				const double X = std::pow(10.0, 0.4 * (MStar - M));
				L.Pdf[size_t(I)] = std::pow(X, Alpha + 1.0) * std::exp(-X);
			}
			L.Build();
			return L;
		}

		void Build()
		{
			const size_t N = Pdf.size();
			Cdf.assign(N, 0.0);
			for (size_t I = 1; I < N; ++I) { Cdf[I] = Cdf[I - 1] + 0.5 * (Pdf[I] + Pdf[I - 1]); }
			const double Total = Cdf[N - 1];
			for (double& C : Cdf) { C /= Total; }
			InvCdf.resize(kInvCdfSize);
			size_t K = 0;
			for (int I = 0; I < kInvCdfSize; ++I)
			{
				const double U = double(I) / double(kInvCdfSize - 1);
				while (K + 2 < N && Cdf[K + 1] < U) { ++K; }
				const double Span = Cdf[K + 1] - Cdf[K];
				const double T = Span > 0.0 ? std::clamp((U - Cdf[K]) / Span, 0.0, 1.0) : 0.0;
				InvCdf[size_t(I)] = float(MAt(K) + T * (MAt(K + 1) - MAt(K)));
			}
		}

		double MAt(size_t I) const { return MMin + (MMax - MMin) * double(I) / double(Pdf.size() - 1); }

		double CdfAt(double M) const
		{
			if (M <= MMin) { return 0.0; }
			if (M >= MMax) { return 1.0; }
			const double T = (M - MMin) / (MMax - MMin) * double(Pdf.size() - 1);
			const size_t I = std::min(size_t(T), Pdf.size() - 2);
			return Cdf[I] + (T - double(I)) * (Cdf[I + 1] - Cdf[I]);
		}
	};

	// ======================================================================================
	// Archetypes and emitters
	// ======================================================================================

	enum class EVisibility { Point, Extended };

	struct FEmitter
	{
		u32 ChildArchetypeId = 0;
		u32 FieldId = 0;                      // UNI_FIELD_*
		FFieldParams Exemplar;
		std::vector<FParamRange> Ranges;

		/** Expected children per parent. The field gives only the shape (lesson #8). */
		double NTotal = 0.0;
		/** If finite: N = NTotal * 10^(-0.4 (M_parent - NTotalRefMag)), so brighter parents
		 *  hold proportionally more children (D-111). */
		double NTotalRefMag = NAN;

		/** Root emitters: NTotal is a number density per cm^3, and the periodic field is mean-
		 *  normalised over its period of 2^PeriodBits cm. */
		bool bDensityMode = false;
		int PeriodBits = 0;

		/** Contributor radius = InfluenceMul * parent physical radius. Bounds the overlap test
		 *  and is the field's unit ball. */
		double InfluenceMul = 1.0;
		bool bRandomOrientation = true;
	};

	struct FArchetype
	{
		u32 Id = 0;                           // stable, never reused
		std::string Name;
		EVisibility Visibility = EVisibility::Point;
		FMagnitudeLut AbsMag;
		float RadiusModel[4] = { 0, 0, 0, 0 };   // log10 R_cm = x + y (M - z) + w (2u - 1)
		float ColourModel[4] = { 3.76f, 0, 0, 0 }; // log10 T = x + y (M - z) + w (2u - 1)
		double ThetaMin = UniHlsl::UNI_THETA_MIN;
		std::vector<FEmitter> Emitters;
		bool bDefaultsOnly = false;          // lesson #16

		double RadiusMaxCm(double M) const
		{
			return std::pow(10.0, double(RadiusModel[0]) + double(RadiusModel[1]) * (M - double(RadiusModel[2])) + std::fabs(double(RadiusModel[3])));
		}
	};

	struct FAnchor
	{
		FCoord Coord;
		u32 ArchetypeId = 0;
		float AbsMag = 0.0f;
		float RadiusLog2 = 0.0f;
		std::vector<FFieldParams> EmitterParams;   // per emitter; empty = roll as usual
		bool bFixedOrientation = true;              // identity rotation
		u32 Seed = 0;
	};

	struct FSuppressor
	{
		FCoord Centre;
		double RadiusCm = 0.0;
		u32 ArchetypeId = 0;                        // children of this archetype are masked
	};

	// ======================================================================================
	// Luminosity slicing
	// ======================================================================================

	struct FSlice
	{
		int Level = 0;
		double P = 0.0;         // fraction of children whose relevance maps to this level
		double CdfLo = 0.0, CdfHi = 0.0;
	};

	struct FSliceTable
	{
		std::vector<FSlice> Slices;   // contiguous levels, coarse first
		int LMin = 0, LMax = -1;
		double PSum = 0.0;
		bool bMonotonic = true;

		const FSlice* ForLevel(int L) const
		{
			if (L < LMin || L > LMax) { return nullptr; }
			for (const FSlice& S : Slices) { if (S.Level == L) { return &S; } }
			return nullptr;
		}
	};

	/** The whole content definition: archetypes, root emitters, anchors, suppressors. */
	struct FUniverseDef
	{
		u32 Seed = 69;
		std::vector<FArchetype> Archetypes;     // index = Id
		std::vector<FEmitter> RootEmitters;     // the root archetype: no position, covers everything
		std::vector<FAnchor> Anchors;
		std::vector<FSuppressor> Suppressors;

		const FArchetype& Arch(u32 Id) const { return Archetypes[Id]; }
	};

	/** Derived tables, built once per definition (and rebuilt whenever it changes, lesson #28). */
	class FUniverseModel
	{
	public:
		explicit FUniverseModel(FUniverseDef InDef) : Def(std::move(InDef)) { Build(); }

		const FUniverseDef& Definition() const { return Def; }

		/** Slices of an archetype's children, by child archetype id. */
		const FSliceTable& SlicesOf(u32 ChildId) const { return Slices.at(ChildId); }

		/** Relevance radius of an object of this archetype with absolute magnitude M, using the
		 *  maximum radius of its scatter (D-102) and the influence term (D-101). */
		double Relevance(u32 ArchId, double M) const
		{
			const FArchetype& A = Def.Arch(ArchId);
			double R = RelevanceFlux(M);
			if (A.Visibility == EVisibility::Extended) { R = std::max(R, RelevanceAngular(A.RadiusMaxCm(M), A.ThetaMin)); }
			R = std::max(R, InfluenceRelevance(ArchId, M));
			return R;
		}

		/** D-101: an object with emitters must be resident wherever its children's windows can
		 *  touch its influence sphere: influence radius plus the reach of the coarsest child
		 *  level's largest window, sqrt(3) (N_max + 1) 2^Lmax. */
		double InfluenceRelevance(u32 ArchId, double M) const
		{
			const FArchetype& A = Def.Arch(ArchId);
			double R = 0.0;
			for (const FEmitter& E : A.Emitters)
			{
				const FSliceTable& T = Slices.at(E.ChildArchetypeId);
				if (T.LMax < 0) { continue; }
				const double Reach = std::sqrt(3.0) * double(UniHlsl::UNI_WINDOW_N_MAX + 1) * std::ldexp(1.0, T.LMax);
				R = std::max(R, E.InfluenceMul * A.RadiusMaxCm(M) + Reach);
			}
			return R;
		}

		int LevelOf(u32 ArchId, double M) const { return LevelForRelevance(Relevance(ArchId, M)); }

		/** Validation (UE: archetype validation in the editor module). Returns problems. */
		std::vector<std::string> Validate() const
		{
			std::vector<std::string> P;
			for (const FArchetype& A : Def.Archetypes)
			{
				for (const FEmitter& E : A.Emitters)
				{
					for (const FParamRange& R : E.Ranges)
					{
						if (R.Min > R.Max) { P.push_back(A.Name + ": range " + R.Name + " has Min > Max"); }
						if (R.Bias <= 0.0f) { P.push_back(A.Name + ": range " + R.Name + " has Bias <= 0"); }
						if (R.Quantize > 0.0f && R.Quantize > R.Max - R.Min) { P.push_back(A.Name + ": range " + R.Name + " quantize exceeds span"); }
						if (R.Slot < 0 || R.Slot > 15) { P.push_back(A.Name + ": range " + R.Name + " slot out of range"); }
					}
					// Child levels strictly finer than every parent level.
					const FSliceTable& Child = Slices.at(E.ChildArchetypeId);
					const FSliceTable& Self = Slices.count(A.Id) ? Slices.at(A.Id) : Child;
					if (Child.LMax >= 0 && Self.LMax >= 0 && &Self != &Child && Child.LMax >= Self.LMin)
					{
						P.push_back(A.Name + ": child " + Def.Arch(E.ChildArchetypeId).Name + " levels not strictly finer");
					}
				}
			}
			for (const auto& KV : Slices)
			{
				if (!KV.second.bMonotonic) { P.push_back(Def.Arch(KV.first).Name + ": relevance not monotonic in M"); }
			}
			return P;
		}

	private:
		void Build()
		{
			// Bottom-up: an archetype's slice table needs its own children's tables for the
			// influence term. Emitter graphs are acyclic (validated), so recursion terminates.
			for (const FArchetype& A : Def.Archetypes) { BuildSlices(A.Id, 0); }
		}

		void BuildSlices(u32 Id, int Depth)
		{
			if (Slices.count(Id) || Depth > 16) { return; }
			const FArchetype& A = Def.Arch(Id);
			for (const FEmitter& E : A.Emitters) { BuildSlices(E.ChildArchetypeId, Depth + 1); }

			FSliceTable T;
			if (A.AbsMag.Pdf.empty()) { Slices[Id] = T; return; }
			std::map<int, FSlice> ByLevel;
			const size_t N = A.AbsMag.Pdf.size();
			int PrevLevel = 1 << 20;
			for (size_t I = 0; I + 1 < N; ++I)
			{
				// One sub-interval of the M grid: mass from the CDF, level from its midpoint.
				const double M0 = A.AbsMag.MAt(I), M1 = A.AbsMag.MAt(I + 1);
				const int L = LevelForRelevance(RelevanceNoCache(A, 0.5 * (M0 + M1)));
				if (L > PrevLevel) { T.bMonotonic = false; }   // brighter (smaller M) first: L non-increasing
				PrevLevel = L;
				FSlice& S = ByLevel[L];
				const double C0 = A.AbsMag.Cdf[I], C1 = A.AbsMag.Cdf[I + 1];
				if (S.P == 0.0) { S.Level = L; S.CdfLo = C0; }
				S.CdfHi = C1;
				S.P += C1 - C0;
			}
			for (auto It = ByLevel.rbegin(); It != ByLevel.rend(); ++It) { T.Slices.push_back(It->second); T.PSum += It->second.P; }
			T.LMin = ByLevel.begin()->first;
			T.LMax = ByLevel.rbegin()->first;
			Slices[Id] = T;
		}

		double RelevanceNoCache(const FArchetype& A, double M) const
		{
			double R = RelevanceFlux(M);
			if (A.Visibility == EVisibility::Extended) { R = std::max(R, RelevanceAngular(A.RadiusMaxCm(M), A.ThetaMin)); }
			R = std::max(R, InfluenceRelevance(A.Id, M));
			return R;
		}

		FUniverseDef Def;
		std::map<u32, FSliceTable> Slices;
	};
}
