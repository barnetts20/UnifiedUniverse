#pragma once

// UniVolume.h
// CPU side of the volume pass (Docs/VOLUMETRICS.md, D-128..D-134). Engine-agnostic: the
// runtime calls it once per frame and uploads the result; univerify runs the same code.
//
//   FUnresolvedTable   per child archetype, built once per definition: for each luminosity
//                      slice, its mean flux L_s and G_s(x) = <10^-0.4M sat((x - M) / 0.5)>,
//                      both integrated over the slice with Uni_SliceMagAt / Uni_ColourAt, the
//                      functions placement draws with (lesson #1).
//   BuildEmissionLut   per frame: l(t) = sum_s P_s (L_s - F_L(s) G_s(m_lim - mu(t))), the flux
//                      per child NOT drawn as a sprite at distance t, times horizon dimming and
//                      exposure; plus its cumulative tail for the background (D-128, D-131).
//   FVolumeBuilder     per frame: carriers from the pipeline's resident contributors (exact
//                      128-bit camera deltas, the contributors' own rotation, parameters and
//                      normalising integral) and one periodic carrier per root emitter.
//   LuminosityBudget   the parent magnitude its children add up to (D-133 validation).

#include "UniGenRef.h"
#include "UniScaleLadder.h"
#include "UniShaderCores.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <type_traits>
#include <map>
#include <vector>

namespace UniCore
{
	namespace HV = UniHlsl;

	// GPU-MIRROR
	struct FCarrierGPU
	{
		float CamLocal[3];
		float InvRadius;
		float Rot0[3];
		u32 FieldId;
		float Rot1[3];
		u32 ParamIndex;
		float Rot2[3];
		u32 LutIndex;
		float Amplitude;
		float Weight;
		u32 Flags;
		u32 DustParamIndex;
		float MeanDensity;
		float FeatureScale;
		float TStart;
		float TEnd;
		float ResolveScale;
		float StepRatio;   // D-164: 0 = the view's
		u32 PaletteIndex;  // D-182: the galaxy's palette (a parameter block: A..D bulge, disc, arms, knots; E whole body), with UNI_CARRIER_PALETTE
		float Pad;
	};
	static_assert(sizeof(FCarrierGPU) == 112, "FCarrierGPU must match FUniCarrier (UniVolume.ush)");
	static_assert(sizeof(HV::FUniCarrier) == 112, "shim FUniCarrier layout");

	// GPU-MIRROR
	struct FVolumeViewGPU
	{
		float StepRatio;
		float MinStepCm;
		u32 MaxSteps;
		u32 NumCarriers;
		float ThetaPixel;
		float SliceLnTLo;
		float SliceLnTHi;
		u32 MaxSubsamples;   // D-157
		float DustSigmaRgb[3];
		float Pad1;
	};
	static_assert(sizeof(FVolumeViewGPU) == 48, "FVolumeViewGPU must match FUniVolumeView (UniVolume.ush)");
	static_assert(sizeof(HV::FUniVolumeView) == 48, "shim FUniVolumeView layout");

	inline HV::FUniCarrier ToHlsl(const FCarrierGPU& C) { HV::FUniCarrier H; std::memcpy(static_cast<void*>(&H), &C, sizeof(H)); return H; }
	inline HV::FUniVolumeView ToHlsl(const FVolumeViewGPU& V) { HV::FUniVolumeView H; std::memcpy(static_cast<void*>(&H), &V, sizeof(H)); return H; }

	// ======================================================================================
	// Unresolved-light tables (D-128)
	// ======================================================================================

	class FUnresolvedTable
	{
	public:
		struct FRow
		{
			int Level = 0;
			double P = 0.0;
			double LbarRgb[3] = { 0, 0, 0 };   // <colour * 10^-0.4M>
			double LbarV = 0.0;                // <10^-0.4M>, V band (luminosity budget)
			std::vector<double> G;             // kX * 3, RGB
		};
		static constexpr int kU = 4096;        // magnitude quadrature (stratified midpoints)
		static constexpr int kC = 8;           // colour-scatter quadrature
		static constexpr int kX = 1024;

		double XMin = 0.0, XMax = 1.0;
		std::vector<FRow> Rows;

		static FUnresolvedTable Build(const FUniverseModel& M, u32 ChildId)
		{
			FUnresolvedTable T;
			const FSliceTable& ST = M.SlicesOf(ChildId);
			const FArchetype& A = M.Definition().Arch(ChildId);
			const std::vector<float>& Lut = M.GpuLut();
			const HV::StructuredBuffer<float> LutBuf(Lut.data(), HV::uint(Lut.size()));
			const HV::float4 Colour(A.ColourModel[0], A.ColourModel[1], A.ColourModel[2], A.ColourModel[3]);
			T.XMin = A.AbsMag.MMin - 1.0;
			T.XMax = A.AbsMag.MMax + 1.0;
			for (const FSlice& S : ST.Slices)
			{
				const int Gi = M.SliceIndex(ChildId, S.Level);
				if (Gi < 0) { continue; }
				const FSliceGPU& SG = M.GpuSlices()[size_t(Gi)];
				FRow R;
				R.Level = S.Level;
				R.P = S.P;
				// Samples sorted by M (the inverse CDF is monotone in u): weights and prefix sums.
				std::vector<double> Ms(kU), W(size_t(kU) * 3);
				for (int I = 0; I < kU; ++I)
				{
					const float U = (float(I) + 0.5f) / float(kU);
					const float Mag = HV::Uni_SliceMagAt(U, 0.0f, 1.0f, SG.BrightM, SG.FaintM, LutBuf, SG.LutBase, SG.LutCount);
					Ms[size_t(I)] = Mag;
					const double L = std::pow(10.0, -0.4 * double(Mag));
					double C[3] = { 0, 0, 0 };
					for (int J = 0; J < kC; ++J)
					{
						// The colour placed objects carry is the PACKED one: integrate that.
						// Luminance-normalised, as the sprite's radiance (D-135).
						const HV::float3 Rgb = HV::Uni_UnpackColour(HV::Uni_PackColour(HV::Uni_ColourAt((float(J) + 0.5f) / float(kC), Mag, Colour)));
						const double Y = std::max(double(HV::Uni_Luminance(Rgb)), 1.0e-3);
						C[0] += Rgb.x / Y; C[1] += Rgb.y / Y; C[2] += Rgb.z / Y;
					}
					for (int K = 0; K < 3; ++K) { W[size_t(I) * 3 + K] = L * C[K] / double(kC) / double(kU); R.LbarRgb[K] += W[size_t(I) * 3 + K]; }
					R.LbarV += L / double(kU);
				}
				// G(x) = sum_i w_i sat((x - M_i) / 0.5): full weight for M_i <= x - 0.5, linear
				// between. Prefix sums over the sorted M_i make each x O(log n).
				std::vector<double> PW(size_t(kU + 1) * 3, 0.0), PWM(size_t(kU + 1) * 3, 0.0);
				for (int I = 0; I < kU; ++I)
				{
					for (int K = 0; K < 3; ++K)
					{
						PW[size_t(I + 1) * 3 + K] = PW[size_t(I) * 3 + K] + W[size_t(I) * 3 + K];
						PWM[size_t(I + 1) * 3 + K] = PWM[size_t(I) * 3 + K] + W[size_t(I) * 3 + K] * Ms[size_t(I)];
					}
				}
				R.G.assign(size_t(kX) * 3, 0.0);
				for (int X = 0; X < kX; ++X)
				{
					const double Xv = T.XMin + (T.XMax - T.XMin) * double(X) / double(kX - 1);
					const size_t Full = size_t(std::upper_bound(Ms.begin(), Ms.end(), Xv - 0.5) - Ms.begin());
					const size_t Part = size_t(std::lower_bound(Ms.begin(), Ms.end(), Xv) - Ms.begin());
					for (int K = 0; K < 3; ++K)
					{
						const double Wf = PW[Full * 3 + K];
						const double Wp = PW[Part * 3 + K] - PW[Full * 3 + K];
						const double WpM = PWM[Part * 3 + K] - PWM[Full * 3 + K];
						R.G[size_t(X) * 3 + K] = Wf + (Xv * Wp - WpM) / 0.5;
					}
				}
				T.Rows.push_back(std::move(R));
			}
			return T;
		}

		void G(const FRow& R, double X, double Out[3]) const
		{
			const double F = std::clamp((X - XMin) / (XMax - XMin), 0.0, 1.0) * double(kX - 1);
			const size_t I = std::min(size_t(F), size_t(kX - 2));
			const double T = F - double(I);
			for (int K = 0; K < 3; ++K) { Out[K] = R.G[I * 3 + K] + T * (R.G[(I + 1) * 3 + K] - R.G[I * 3 + K]); }
		}

		/** Mean V-band flux per child, all slices: the luminosity budget per child. */
		double MeanFluxV() const { double S = 0.0; for (const FRow& R : Rows) { S += R.P * R.LbarV; } return S; }
	};

	struct FLutFrame
	{
		double LimitingMag = UniHlsl::UNI_M_REF;
		double ExposureMag = UniHlsl::UNI_M_REF - 1.0;
		std::function<float(int)> LevelFade;    // F_L: the fraction of level L's sprites drawn
	};

	inline double LutT(int I) { return std::exp(double(HV::UNI_VOL_LN_T_LO) + (double(HV::UNI_VOL_LN_T_HI) - double(HV::UNI_VOL_LN_T_LO)) * double(I) / double(HV::UNI_VOL_LUT_SIZE - 1u)); }

	/** F_L per table row (fetched once per LUT build, not per sample). */
	inline std::vector<double> RowFades(const FUnresolvedTable& T, const FLutFrame& F)
	{
		std::vector<double> Fl;
		for (const FUnresolvedTable::FRow& R : T.Rows) { Fl.push_back(F.LevelFade ? std::clamp(double(F.LevelFade(R.Level)), 0.0, 1.0) : 0.0); }
		return Fl;
	}

	/** l(t) per child not drawn as a sprite, horizon-dimmed and exposure-scaled. */
	inline void UnresolvedAt(const FUnresolvedTable& T, const FLutFrame& F, const std::vector<double>& Fl, double Tcm, double Out[3])
	{
		Out[0] = Out[1] = Out[2] = 0.0;
		const double Mu = 5.0 * std::log10(std::max(Tcm, 1.0) / kTenPcCm);
		const double X = F.LimitingMag - Mu;
		for (size_t I = 0; I < T.Rows.size(); ++I)
		{
			const FUnresolvedTable::FRow& R = T.Rows[I];
			double G[3];
			T.G(R, X, G);
			for (int K = 0; K < 3; ++K) { Out[K] += R.P * (R.LbarRgb[K] - Fl[I] * G[K]); }
		}
		const double Dim = double(HV::Uni_HorizonDimming(float(Tcm)));
		const double Exp = std::pow(10.0, 0.4 * F.ExposureMag);
		for (int K = 0; K < 3; ++K) { Out[K] = std::max(Out[K], 0.0) * Dim * Exp; }
	}

	/** Appends two LUT blocks: emission l(t), then the tail integral of l from t to infinity
	 *  (cm). Returns the emission block index. */
	/** D-151: a power of two near 1 / (mean flux per child), applied to a table's LUT and undone
	 *  in its carriers' amplitudes. The march only uses LUT x amplitude, and power-of-two scaling
	 *  is exact in floating point, so results are unchanged bit for bit; but a rich cluster's tail
	 *  (flux ~1e10 x distance ~1e28 cm) no longer overflows a float. */
	inline double LutScale(const FUnresolvedTable& T)
	{
		const double F = T.MeanFluxV();
		return F > 0.0 && std::isfinite(F) ? std::exp2(-std::round(std::log2(F))) : 1.0;
	}

	inline u32 BuildEmissionLut(const FUnresolvedTable& T, const FLutFrame& F, std::vector<HV::float4>& Lut, double Scale = 1.0)
	{
		const u32 Block = u32(Lut.size() / HV::UNI_VOL_LUT_SIZE);
		const int N = int(HV::UNI_VOL_LUT_SIZE);
		const std::vector<double> Fl = RowFades(T, F);
		std::vector<std::array<double, 3>> E(static_cast<size_t>(N));
		for (int I = 0; I < N; ++I) { UnresolvedAt(T, F, Fl, LutT(I), E[size_t(I)].data()); Lut.push_back(HV::float4(float(E[size_t(I)][0] * Scale), float(E[size_t(I)][1] * Scale), float(E[size_t(I)][2] * Scale), 0.0f)); }
		// Tail: past t_hi the integrand falls as t^-2 (dimming) at constant l, so the remainder is
		// l(t_hi) t_hi. Inside, sub-sampled trapezoids in ln t (dt = t d ln t).
		std::vector<std::array<double, 3>> Tail(static_cast<size_t>(N));
		const double Dln = (double(HV::UNI_VOL_LN_T_HI) - double(HV::UNI_VOL_LN_T_LO)) / double(N - 1);
		std::array<double, 3> Acc{};
		for (int K = 0; K < 3; ++K) { Acc[size_t(K)] = E[size_t(N - 1)][size_t(K)] * LutT(N - 1); }
		Tail[size_t(N - 1)] = Acc;
		const int Sub = 16;
		for (int I = N - 2; I >= 0; --I)
		{
			for (int S = 0; S < Sub; ++S)
			{
				const double A = (double(S) + 0.5) / double(Sub);
				const double Tm = std::exp(std::log(LutT(I)) + A * Dln);
				double V[3];
				UnresolvedAt(T, F, Fl, Tm, V);
				for (int K = 0; K < 3; ++K) { Acc[size_t(K)] += V[K] * Tm * Dln / double(Sub); }
			}
			Tail[size_t(I)] = Acc;
		}
		for (int I = 0; I < N; ++I) { Lut.push_back(HV::float4(float(Tail[size_t(I)][0] * Scale), float(Tail[size_t(I)][1] * Scale), float(Tail[size_t(I)][2] * Scale), 0.0f)); }
		return Block;
	}

	/** D-133: the absolute magnitude a parent's children add up to (V band), or NAN. */
	inline double LuminosityBudget(const FUniverseModel& M, u32 ArchId, double ParentAbsMag)
	{
		const FArchetype& A = M.Definition().Arch(ArchId);
		double Flux = 0.0;
		for (const FUniEmitter& E : A.Emitters)
		{
			if (E.bDensityMode) { continue; }
			const FUnresolvedTable T = FUnresolvedTable::Build(M, E.ChildArchetypeId);
			const double N = std::isfinite(E.NTotalRefMag) ? E.NTotal * std::pow(10.0, -0.4 * (ParentAbsMag - E.NTotalRefMag)) : E.NTotal;
			Flux += N * T.MeanFluxV();
		}
		return Flux > 0.0 ? -2.5 * std::log10(Flux) : NAN;
	}

	/** Dust parameters from a contributor's (rolled) emission parameters: the same galaxy, with a
	 *  thinner disc, no bulge and arms shifted to their inner edge. D.y = sigma per field unit at
	 *  rho = 1, so a face-on column through the centre has optical depth DustTau. */
	inline FFieldParams DustFrom(const FFieldParams& Emission, const FVolumeProfile& P)
	{
		FFieldParams D = Emission;
		D.V[1] = Emission.V[1] * P.DustHeightRatio;   // scale height
		D.V[2] = 1.0f;                                // disc weight
		D.V[5] = 0.0f;                                // bulge weight
		D.V[10] = Emission.V[10] + P.DustArmPhase;    // arm phase
		D.V[13] = P.DustTau / (2.0f * std::max(D.V[1], 1.0e-6f));
		// D-180: a textured galaxy's dust is cloud-like: ridged lanes and superbubble holes from
		// galaxy_detail (D.z, D.w: read only by the field's dust terms), no star-forming knots.
		if (Emission.V[24] != 0.0f || Emission.V[28] != 0.0f || Emission.V[29] != 0.0f)
		{
			D.V[14] = P.DustLanes;
			D.V[15] = P.DustBubbles;
			D.V[29] = 0.0f;
		}
		return D;
	}

	// ======================================================================================
	// Carriers (D-129)
	// ======================================================================================

	struct FVolumeSettings
	{
		float StepRatio = 0.08f;
		float MinStepCm = 1.0e15f;
		u32 MaxSteps = 512;
		u32 MaxSubsamples = 8;   // D-157: rays per pixel axis cap (uu.VolumeMaxSubsamples)
		bool bLutStart = true;   // D-160: emission-only carriers start where their LUT becomes significant
		bool bPrefilter = true;  // D-160: galaxies sampled prefiltered over the pixel footprint (no disc / arm supersampling)
		u32 MaxCarriers = 128;   // D-161: 64 -> 128 (a cluster core had 62)
		double LocalStartCm = 3.0856775814913673e18;      // 1 pc
		double BackgroundStartCm = 3.0856775814913673e23; // 0.1 Mpc: nearer galaxies are resolved, and the first 0.1 Mpc of an
		                                                  // r_H-long column is ~2e-5 of the background (log steps from 1 pc cost ~350)
		double BackgroundPeriods = 0.5;                   // march extent (50 Mpc for the demo web); the analytic tail follows
		double SliceLoCm = 3.0856775814913673e18;         // transmittance slices: 1 pc ..
		double SliceHiCm = 3.0856775814913673e23;         // .. 100 kpc
		float DustSigmaRgb[3] = { 0.8f, 1.0f, 1.3f };
		float TexturedWebStepRatio = 0.2f;                // D-164: log step ratio of a textured web carrier (T3.webtex.background: same light as 0.08, 30% fewer steps)
	};

	struct FVolumeScene
	{
		std::vector<FCarrierGPU> Carriers;
		std::vector<FFieldParamsGPU> Params;
		std::vector<HV::float4> Lut;
		FVolumeViewGPU View{};
		size_t CarriersSkipped = 0;   // over the cap

		/** The same scene with only the periodic (background) carriers, or only the local ones.
		 *  Parameters and LUTs are shared, so indices stay valid. The engine marches the
		 *  background into a cube map and the rest per pixel, and composites
		 *  B = B_near + T_near x B_cube(dir) (D-136). */
		FVolumeScene Subset(bool bPeriodic) const
		{
			FVolumeScene O = *this;
			O.Carriers.clear();
			for (const FCarrierGPU& C : Carriers) { if (((C.Flags & HV::UNI_CARRIER_PERIODIC) != 0u) == bPeriodic) { O.Carriers.push_back(C); } }
			O.View.NumCarriers = u32(O.Carriers.size());
			return O;
		}

		HV::StructuredBuffer<HV::FUniCarrier> CarrierBuf() const { return HV::StructuredBuffer<HV::FUniCarrier>(reinterpret_cast<const HV::FUniCarrier*>(Carriers.data()), HV::uint(Carriers.size())); }
		HV::StructuredBuffer<HV::FUniFieldParams> ParamBuf() const { return HV::StructuredBuffer<HV::FUniFieldParams>(reinterpret_cast<const HV::FUniFieldParams*>(Params.data()), HV::uint(Params.size())); }
		HV::StructuredBuffer<HV::float4> LutBuf() const { return HV::StructuredBuffer<HV::float4>(Lut.data(), HV::uint(Lut.size())); }
	};
	static_assert(sizeof(HV::FUniFieldParams) == sizeof(FFieldParamsGPU), "field params layout");

	/** What the builder reads from the streaming side: the reference pipeline (univerify) or the
	 *  runtime streamer (the engine) both provide it, so both build identical scenes
	 *  (T2.stream.volume_parity). */
	struct FVolumeSource
	{
		const std::map<u32, FContributor>* Contributors = nullptr;
		std::function<const FObjectGPU*(const FObjectId&)> FindObject;
		std::function<float(int)> LevelFade;
		/** D-140: set for the runtime streamer. False means the contributor's normalising
		 *  integral is still being computed on a worker (the request starts it): skip its carrier
		 *  this frame rather than compute it on the game thread. Empty: compute inline. */
		std::function<bool(const FContributor&)> IntegralReady;
		/** D-161: optional: the contributors whose influence radius subtends more than an angle,
		 *  from the streamer's per-frame measured distances. The carrier build then rejects far
		 *  contributors with no per-contributor map walk or 128-bit work. */
		std::function<std::vector<const FContributor*>(double)> NearContributors;

		template <typename T, typename = void> struct THasMeasured : std::false_type {};
		template <typename T> struct THasMeasured<T, std::void_t<decltype(std::declval<const T&>().NearContributors(0.0))>> : std::true_type {};
		template <typename T, typename = void> struct THasRequestIntegral : std::false_type {};
		template <typename T> struct THasRequestIntegral<T, std::void_t<decltype(std::declval<const T&>().RequestIntegral(std::declval<const FContributor&>()))>> : std::true_type {};

		template <typename T> static FVolumeSource From(const T& P)
		{
			FVolumeSource S;
			S.Contributors = &P.Contributors();
			S.FindObject = [&P](const FObjectId& Id) { return P.FindObject(Id); };
			S.LevelFade = [&P](int L) { return P.LevelFade(L); };
			if constexpr (THasRequestIntegral<T>::value) { S.IntegralReady = [&P](const FContributor& C) { return P.RequestIntegral(C); }; }
			if constexpr (THasMeasured<T>::value)
			{
				S.NearContributors = [&P](double MinTheta) { return P.NearContributors(MinTheta); };
			}
			return S;
		}
	};

	class FVolumeBuilder
	{
	public:
		FVolumeBuilder(const FUniverseModel& InModel, const FRefGenerator& InGen) : Model(InModel), Gen(InGen)
		{
			const FUniverseDef& D = Model.Definition();
			auto Add = [&](u32 Child) { if (!Tables.count(Child)) { Tables[Child] = FUnresolvedTable::Build(Model, Child); } };
			for (const FUniEmitter& E : D.RootEmitters) { Add(E.ChildArchetypeId); }
			for (const FArchetype& A : D.Archetypes) { for (const FUniEmitter& E : A.Emitters) { Add(E.ChildArchetypeId); } }
		}

		const FUnresolvedTable& Table(u32 Child) const { return Tables.at(Child); }

		/** D-133 from the cached tables (no rebuild): what a parent's children add up to. */
		double LuminosityBudget(u32 ArchId, double ParentAbsMag) const
		{
			double Flux = 0.0;
			for (const FUniEmitter& E : Model.Definition().Arch(ArchId).Emitters)
			{
				if (E.bDensityMode) { continue; }
				const double N = std::isfinite(E.NTotalRefMag) ? E.NTotal * std::pow(10.0, -0.4 * (ParentAbsMag - E.NTotalRefMag)) : E.NTotal;
				Flux += N * Tables.at(E.ChildArchetypeId).MeanFluxV();
			}
			return Flux > 0.0 ? -2.5 * std::log10(Flux) : NAN;
		}

		/** Test hook: override every carrier's level fades (F_L) with a constant (< 0: real fades). */
		float ForceLevelFade = -1.0f;
		/** Test hook: skip local carriers, or the background. */
		bool bLocal = true, bBackground = true;

		/** From anything FVolumeSource::From accepts: FRefPipeline or FUniStreamer. */
		template <typename T>
		FVolumeScene Build(const T& P, const FCoord& Cam, const FVec3f& CamFrac, const HV::FUniView& View, const FVolumeSettings& S = FVolumeSettings()) const
		{
			return Build(FVolumeSource::From(P), Cam, CamFrac, View, S);
		}

		/** D-160: the distance below which a LUT block is negligible: the position of the entry
		 *  before the first whose largest channel exceeds 1e-6 of the block's peak (the LUT is
		 *  linear in ln t between entries, so emission starts there at the earliest). */
		static double LutSignificantStart(const std::vector<HV::float4>& InLut)
		{
			const size_t N = HV::UNI_VOL_LUT_SIZE;
			if (InLut.size() < N) { return 0.0; }
			double Peak = 0.0;
			for (size_t I = 0; I < N; ++I) { Peak = std::max(Peak, double(std::max({ InLut[I].x, InLut[I].y, InLut[I].z }))); }
			if (!(Peak > 0.0)) { return 0.0; }
			for (size_t I = 0; I < N; ++I)
			{
				if (double(std::max({ InLut[I].x, InLut[I].y, InLut[I].z })) > 1.0e-6 * Peak)
				{
					if (I == 0) { return 0.0; }
					const double LnT = double(HV::UNI_VOL_LN_T_LO) + double(I - 1) / double(N - 1) * (double(HV::UNI_VOL_LN_T_HI) - double(HV::UNI_VOL_LN_T_LO));
					return std::exp(LnT);
				}
			}
			return 0.0;
		}

		FVolumeScene Build(const FVolumeSource& P, const FCoord& Cam, const FVec3f& CamFrac, const HV::FUniView& View, const FVolumeSettings& S = FVolumeSettings()) const
		{
			FVolumeScene Out;
			std::map<u32, double> SigStart;   // D-160: per child archetype
			FLutFrame F;
			F.LimitingMag = View.LimitingMag;
			F.ExposureMag = View.ExposureMag;
			const float Force = ForceLevelFade;
			F.LevelFade = [&P, Force](int L) { return Force >= 0.0f ? Force : P.LevelFade(L); };
			std::map<u32, u32> Block;
			for (const auto& KV : Tables)
			{
				// The LUT depends only on (m_lim, m_exp, F_L per slice level): rebuild only when
				// those change (most frames they do not; a rebuild is ~4k table evaluations).
				// Quantised to 1e-3 (fades step by 1/8 per frame; magnitudes are user settings).
				auto Q = [](double V) { return std::round(V * 1000.0) / 1000.0; };
				std::vector<double> Key = { Q(F.LimitingMag), Q(F.ExposureMag) };
				for (double Fv : RowFades(KV.second, F)) { Key.push_back(Q(Fv)); }
				FLutCache& Cache = LutCache[KV.first];
				Block[KV.first] = u32(Out.Lut.size() / HV::UNI_VOL_LUT_SIZE);
				if (Cache.Key != Key)
				{
					Cache.Key = Key;
					Cache.Lut.clear();
					BuildEmissionLut(KV.second, F, Cache.Lut, LutScale(KV.second));
					++LutRebuilds;
				}
				Out.Lut.insert(Out.Lut.end(), Cache.Lut.begin(), Cache.Lut.end());
				SigStart[KV.first] = LutSignificantStart(Cache.Lut);
			}

			// Background: one periodic carrier per root emitter.
			if (bBackground)
			{
				for (const FContributor& C : Gen.RootContributors())
				{
					if (P.IntegralReady && !P.IntegralReady(C)) { continue; }   // D-140: on a worker; the background waits
					const int PB = C.Emitter->PeriodBits;
					const double Period = std::ldexp(1.0, PB);
					FCarrierGPU G{};
					for (int A = 0; A < 3; ++A)
					{
						const double Fr = (A == 0 ? CamFrac.X : (A == 1 ? CamFrac.Y : CamFrac.Z));
						G.CamLocal[A] = float(std::fmod(U128ToDouble(MaskLow(Cam.Axis(A), PB)) + Fr, Period) / Period);
					}
					G.InvRadius = float(1.0 / Period);
					G.Rot0[0] = 1; G.Rot1[1] = 1; G.Rot2[2] = 1;
					G.FieldId = C.Emitter->FieldId;
					G.ParamIndex = u32(Out.Params.size());
					Out.Params.push_back(C.Params.Pack());
					G.LutIndex = Block.at(C.Emitter->ChildArchetypeId);
					const double I = Gen.IntegralAll(C);   // the period mean
					G.Amplitude = float(C.NTotal * kTenPcCm * kTenPcCm * Period / I / LutScale(Tables.at(C.Emitter->ChildArchetypeId)));
					G.Weight = 1.0f;
					G.Flags = HV::UNI_CARRIER_PERIODIC | HV::UNI_CARRIER_TAIL;
					// D-164: a textured web is prefiltered by its mips over the pixel footprint.
					if (C.Emitter->FieldId == HV::UNI_FIELD_WEB && HV::Uni_WebIsTextured(C.Params.ToHlsl()))
					{
						G.Flags |= HV::UNI_CARRIER_PREFILTER;
						G.StepRatio = S.TexturedWebStepRatio;   // its field is smooth below a texel: log steps can be coarser
					}
					G.MeanDensity = float(I);
					G.FeatureScale = HV::Uni_FieldFeatureScale(C.Emitter->FieldId, C.Params.ToHlsl());
					G.ResolveScale = HV::Uni_FieldResolveScale(C.Emitter->FieldId, C.Params.ToHlsl());
					G.TStart = float(S.BackgroundStartCm);
					G.TEnd = float(S.BackgroundPeriods * Period);
					Out.Carriers.push_back(G);
				}
			}

			// Local carriers: resident contributors, ranked by angular size, capped.
			struct FCand { double Theta; FCarrierGPU G; FFieldParamsGPU Par; FFieldParamsGPU Dust; bool bDust; FFieldParamsGPU Palette; bool bPalette = false; };
			std::vector<FCand> Cands;
			if (bLocal)
			{
				const FUniverseDef& D = Model.Definition();
				const double RejectTheta = 0.5 * double(std::min(HV::UNI_VOL_HANDOFF_PX * View.ThetaPixel, HV::UNI_THETA_PROMOTE)) * (1.0 - 1.0e-6);
				// D-161: from the streamer, its per-frame measured offsets reject far contributors
				// first (no map walk, no 128-bit subtraction); the survivors (a handful) go through
				// the exact path below. Without them (the reference pipeline), every contributor.
				std::vector<const FContributor*> Near;
				if (P.NearContributors) { Near = P.NearContributors(RejectTheta); }
				else
				{
					Near.reserve(P.Contributors->size());
					for (const auto& KV : *P.Contributors) { Near.push_back(&KV.second); }
				}
				for (const FContributor* Cp : Near)
				{
					const FContributor& C = *Cp;
					if (C.bRoot) { continue; }
					const FVec3d Dd = ToDouble(Sub(Cam, C.Centre));
					const double Dx = Dd.X + CamFrac.X, Dy = Dd.Y + CamFrac.Y, Dz = Dd.Z + CamFrac.Z;
					const double R = std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
					// D-161: an object is never larger than its influence radius, and the handoff weight
					// is 0 until its angular radius passes half of min(8 px, theta_promote): reject on
					// the influence before the per-object work. Exact (the same carriers come out);
					// with ~17,500 contributors this loop was ~40 ms per frame on the game thread.
					if (R > 0.0 && C.InfluenceCm / R <= RejectTheta) { continue; }
					const FObjectGPU* O = P.FindObject(C.Parent);
					if (!O) { continue; }
					const HV::FUniObject HO = UniCore::ToHlsl(*O);
					const double Radius = std::exp2(double(HV::Uni_ObjectRadiusLog2(HO)));
					const double Theta = R > 0.0 ? Radius / R : 1.0;
					// Same handoff, visibility and level fade as the object's sprite (D-129).
					const float W = HV::Uni_CarrierHandoff(float(Theta), View.ThetaPixel);
					const float V = HV::Uni_ObjectVisibility(HO, float(std::max(R, 1.0e-3)), float(Theta), View);
					const float Fg = Force >= 0.0f ? 1.0f : P.LevelFade(C.Parent.Cell.Level);
					const float Weight = W * V * Fg;
					if (!(Weight > 0.0f)) { continue; }
					// D-140: requested here, at the start of the handoff ramp (the carrier's weight is
					// near 0), so it is normally ready long before the carrier matters.
					if (P.IntegralReady && !P.IntegralReady(C)) { continue; }
					FCand Cd;
					Cd.Theta = Theta;
					FCarrierGPU& G = Cd.G;
					G = FCarrierGPU{};
					const double Inv = 1.0 / C.InfluenceCm;
					for (int A = 0; A < 3; ++A)
					{
						G.CamLocal[A] = float((double(C.Rot[A * 3 + 0]) * Dx + double(C.Rot[A * 3 + 1]) * Dy + double(C.Rot[A * 3 + 2]) * Dz) * Inv);
						G.Rot0[A] = C.Rot[A];
						G.Rot1[A] = C.Rot[3 + A];
						G.Rot2[A] = C.Rot[6 + A];
					}
					G.InvRadius = float(Inv);
					G.FieldId = C.Emitter->FieldId;
					G.LutIndex = Block.at(C.Emitter->ChildArchetypeId);
					const double I = Gen.IntegralAll(C);
					G.Amplitude = float(C.NTotal * kTenPcCm * kTenPcCm / (I * C.InfluenceCm * C.InfluenceCm) / LutScale(Tables.at(C.Emitter->ChildArchetypeId)));
					G.Weight = Weight;
					G.Flags = 0u;
					G.MeanDensity = 0.0f;
					G.FeatureScale = HV::Uni_FieldFeatureScale(C.Emitter->FieldId, C.Params.ToHlsl());
					G.ResolveScale = HV::Uni_FieldResolveScale(C.Emitter->FieldId, C.Params.ToHlsl());
					G.TStart = float(S.LocalStartCm);
					G.TEnd = 3.0e38f;
					Cd.Par = C.Params.Pack();
					const FVolumeProfile& Prof = D.Arch(C.ParentArchetypeId).Volume;
					Cd.bDust = Prof.bDust && Prof.DustTau > 0.0f && C.Emitter->FieldId == HV::UNI_FIELD_GALAXY && !C.bEarly;   // D-172: early types are dust-poor
					if (Cd.bDust) { Cd.Dust = DustFrom(C.Params, Prof).Pack(); G.Flags |= HV::UNI_CARRIER_DUST; }
					// D-182: the palette's colours, computed once per carrier (not per sample).
					Cd.bPalette = C.Emitter->FieldId == HV::UNI_FIELD_GALAXY && C.Params.V[23] > 0.0f;
					if (Cd.bPalette)
					{
						const HV::FUniGalaxyPalette Pp = HV::Uni_GalaxyPalette(u32(C.Params.V[23]));
						const HV::float3 Ft = HV::Uni_GalaxyFarTint(Pp);
						const HV::float3 Cs[5] = { Pp.Bulge, Pp.Disc, Pp.Arm, Pp.Knot, Ft };
						FFieldParams Pb;
						for (int K = 0; K < 5; ++K) { Pb.V[4 * K] = Cs[K].x; Pb.V[4 * K + 1] = Cs[K].y; Pb.V[4 * K + 2] = Cs[K].z; }
						Cd.Palette = Pb.Pack();
						G.Flags |= HV::UNI_CARRIER_PALETTE;
					}
					// D-160: an emission-only carrier starts where its LUT does. Nearer than that every
					// child is resolved (drawn as a sprite), so the unresolved emission is < 1e-6 of
					// its peak, but the log-uniform steps from 1 pc there were ~190 per ray from inside
					// a cluster. Dust absorbs at any distance, so dusty carriers keep the 1 pc start.
					if (!Cd.bDust && S.bLutStart) { G.TStart = float(std::max(S.LocalStartCm, SigStart[C.Emitter->ChildArchetypeId])); }
					if (S.bPrefilter && C.Emitter->FieldId == HV::UNI_FIELD_GALAXY) { G.Flags |= HV::UNI_CARRIER_PREFILTER; }
					Cands.push_back(Cd);
				}
			}
			std::sort(Cands.begin(), Cands.end(), [](const FCand& A, const FCand& B) { return A.Theta > B.Theta; });
			// D-161: past the frame's cap, the kept carriers fade out as their angular size nears the
			// largest one left out, so carriers trade places continuously instead of flipping when
			// two cross in the ranking (a cluster core has more than the cap).
			if (Cands.size() > S.MaxCarriers)
			{
				const double ThetaDrop = Cands[S.MaxCarriers].Theta;
				for (size_t I = 0; I < S.MaxCarriers; ++I)
				{
					const double Th = Cands[I].Theta;
					Cands[I].G.Weight = float(double(Cands[I].G.Weight) * std::clamp((Th - ThetaDrop) / (0.25 * Th), 0.0, 1.0));
				}
			}
			for (size_t I = 0; I < Cands.size(); ++I)
			{
				if (Out.Carriers.size() >= S.MaxCarriers) { Out.CarriersSkipped = Cands.size() - I; break; }
				FCand& Cd = Cands[I];
				Cd.G.ParamIndex = u32(Out.Params.size());
				Out.Params.push_back(Cd.Par);
				if (Cd.bDust) { Cd.G.DustParamIndex = u32(Out.Params.size()); Out.Params.push_back(Cd.Dust); }
				if (Cd.bPalette) { Cd.G.PaletteIndex = u32(Out.Params.size()); Out.Params.push_back(Cd.Palette); }
				Out.Carriers.push_back(Cd.G);
			}

			FVolumeViewGPU& V = Out.View;
			V.StepRatio = S.StepRatio;
			V.MinStepCm = S.MinStepCm;
			V.MaxSteps = S.MaxSteps;
			V.MaxSubsamples = S.MaxSubsamples;
			V.NumCarriers = u32(Out.Carriers.size());
			V.ThetaPixel = View.ThetaPixel;
			V.SliceLnTLo = float(std::log(S.SliceLoCm));
			V.SliceLnTHi = float(std::log(S.SliceHiCm));
			for (int K = 0; K < 3; ++K) { V.DustSigmaRgb[K] = S.DustSigmaRgb[K]; }
			return Out;
		}

		/** LUT rebuilds so far (stats: most frames reuse the cached LUT). */
		mutable size_t LutRebuilds = 0;

	private:
		struct FLutCache { std::vector<double> Key; std::vector<HV::float4> Lut; };
		const FUniverseModel& Model;
		const FRefGenerator& Gen;
		std::map<u32, FUnresolvedTable> Tables;
		mutable std::map<u32, FLutCache> LutCache;
	};
}
