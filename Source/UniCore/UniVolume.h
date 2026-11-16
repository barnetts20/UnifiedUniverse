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
		u32 PaletteIndex;  // D-182: the galaxy's palette (a parameter block: A..D bulge, disc, arms, knots; E whole body), with UNI_CARRIER_PALETTE; D-191: a cluster's total-light LUT block, with UNI_CARRIER_ICL
		float IclRatio;    // D-191: intracluster light / members' light, with UNI_CARRIER_ICL
		float NebulaHide = 0.0f;   // D-245: a galaxy's nebulae hidden (1) to shown (0) past the handoff
		float CarrierPad0 = 0.0f;
		float CarrierPad1 = 0.0f;
		float CarrierPad2 = 0.0f;
	};
	static_assert(sizeof(FCarrierGPU) == 128, "FCarrierGPU must match FUniCarrier (UniVolume.ush)");
	static_assert(sizeof(HV::FUniCarrier) == 128, "shim FUniCarrier layout");

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
		float WebHintScale;     // D-202: the step hints of periodic (background) carriers x this (0: 1)
		float TailDim;          // D-189 (0: physical)
		float WebGlowGain;
		float WebGlowDepthCm;
		float HintScale;        // D-195: the step hints of carriers around the camera x this (0: 1)
		u32 GlowMask[4];        // D-203: levels with cells still loading or fading in (their grids follow the LUT blocks)
		u32 GlowBase;           // float4 index of the first grid header in the LUT buffer
		u32 GlowLevels;         // set bits in GlowMask (0: no cell glow)
		u32 MarchFlags;         // D-206: bit 0: no shared web texel reads (checks only); D-219: bit 1: every step an eighth (the checks' fine reference); D-231: bit 2: with bit 1, the motion web's detail blurred over the unrefined step (checks only); D-240: bit 3: jittered steps
		u32 GalaxyConstBase;    // D-210: params index of the galaxy constant blocks (one per parameter block); 0: hash inline
		u32 MapCarrier;         // D-212: the mapped carrier's index + 1 (0: none)
		u32 MapSize;            // texels a side of the map's mip 0
		u32 MapSlot;            // D-214: its slot in the map array
		u32 NebulaRefine;          // D-244: the most sub-samples of a nebula step (0, 1: none)
		// D-230: the motion web (render only; Uni_MotionWebEmission).
		float MotionWebGain;     // radiance per cm at full strength (0: off)
		float MotionWebNearCm;   // faded in over [0, this]
		float MotionWebDepthCm;  // faded out over [half this, this]
		float MotionWebInvRef;   // 1 / (the filament + node channels' mean x the threshold)
		// D-231: the motion web's organic detail (one galaxy_detail read multiplies it).
		float MotionWebDetailCm;   // the detail texture's period, cm (0: none)
		float MotionWebDetailGain; // 0: none, 1: the full ridged pattern
		float MotionWebStepCm;     // with the detail: the step cap within the web's band (0: none)
		float MotionWebRegionCm;   // D-233: the regions' period, cm (0: the ridged channel alone)
		float NebulaGain;          // D-234: the emission nebulae's gain (0: off)
		float NebulaReflect;       // D-234: the reflection nebulae's gain (0: off)
		float NebulaNearCm;        // D-234: the nebulae fade in from here to 3x
		float NebulaThetaPixel;    // D-235: the cube passes set it to the screen's ThetaPixel (0: ThetaPixel)
		float NebulaFineGain;      // D-237, D-241: the nebulae's detail layers' strength (0: off)
		float NebulaDark;          // D-242: the nebulae's mean dark share (0..1; below 0: none)
		float NebulaFineReachCm;   // D-237: the detail's reach (faded over its second half)
		float JitterShift;         // D-240: every jitter draw's shift this sample (the renderer sets it per frame)
		float DiffuseDustReachLoCm;   // D-250: the galaxies' diffuse dust reach range, cm (log-uniform by seed)
		float DiffuseDustReachHiCm;
		float DiffuseDustGain;        // D-250: x its opacity (0: off)
		float DiffuseDustHeight;      // D-250: its scale height over the dust set's
		u32 NebGridCount;             // D-252: the nebula grid's levels (0: off; the renderer sets it)
		u32 NebGridParam;             // D-252: the parameter index of the spiral it holds
		float NebGridFetchMax;        // D-252: a step's grid reads at most this many
		float StepFoot;               // D-255: a galaxy's field over at least this x the step along the disc (0: the pixel's)
		u32 SegmentTaps;              // D-262: the camera's galaxy's step read at up to this many points along its run (paradigm C; 0, 1: one)
		float OpaqueCutoff;           // D-262: a ray stops under this transmittance (0: 1e-3)
		float DetailSteps;            // D-265: a detail layer's steps at its resolving step past the log step's reach (0: no step cap)
		float DetailStepTexels;       // D-265: the resolving step in the layer's texels
		float DetailFreq0;            // D-265: layer 0 x the detail's frequency (0: off), its light and dust amplitudes
		float DetailLight0;
		float DetailDust0;
		float DetailFreq1;            // D-265: layer 1
		float DetailLight1;
		float DetailDust1;
		float DetailFoot;             // D-265: a layer read over at least this x the step's share (0: 0.5)
		float DetailPad;
	};
	static_assert(sizeof(FVolumeViewGPU) == 256, "FVolumeViewGPU must match FUniVolumeView (UniVolume.ush)");
	static_assert(sizeof(HV::FUniVolumeView) == 256, "shim FUniVolumeView layout");

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

	/** l(t) per child not drawn as a sprite, horizon-dimmed and exposure-scaled. D-197: Keep, the
	 *  share of the children generated (FUniEmitter::SpriteKeep): only those can be drawn. */
	inline void UnresolvedAt(const FUnresolvedTable& T, const FLutFrame& F, const std::vector<double>& Fl, double Tcm, double Out[3], double Keep = 1.0)
	{
		Out[0] = Out[1] = Out[2] = 0.0;
		const double Mu = 5.0 * std::log10(std::max(Tcm, 1.0) / kTenPcCm);
		const double X = F.LimitingMag - Mu;
		for (size_t I = 0; I < T.Rows.size(); ++I)
		{
			const FUnresolvedTable::FRow& R = T.Rows[I];
			double G[3];
			T.G(R, X, G);
			for (int K = 0; K < 3; ++K) { Out[K] += R.P * (R.LbarRgb[K] - Keep * Fl[I] * G[K]); }
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

	/** D-203: bDrawn appends, after the emission and tail blocks, one block per child level q from
	 *  the table's first level: D_q(t), the light drawn as sprites at that level (what the
	 *  emission block subtracts for it), horizon-dimmed and exposure-scaled like l(t). The march
	 *  adds (1 - cell fade) D_q(t) where a cell is still loading (Uni_CellGlow). The emission
	 *  block's first entry's w holds first level + 256 x count (0: none). D-234: its second
	 *  entry's w holds the luminance of the full light per child (every row's mean, none drawn),
	 *  exposure-scaled, not horizon-dimmed: the nebulae's scale (Uni_VolumeLutFull). */
	inline u32 BuildEmissionLut(const FUnresolvedTable& T, const FLutFrame& F, std::vector<HV::float4>& Lut, double Scale = 1.0, double Keep = 1.0, bool bDrawn = false)
	{
		const u32 Block = u32(Lut.size() / HV::UNI_VOL_LUT_SIZE);
		const int N = int(HV::UNI_VOL_LUT_SIZE);
		const std::vector<double> Fl = RowFades(T, F);
		std::vector<std::array<double, 3>> E(static_cast<size_t>(N));
		for (int I = 0; I < N; ++I) { UnresolvedAt(T, F, Fl, LutT(I), E[size_t(I)].data(), Keep); Lut.push_back(HV::float4(float(E[size_t(I)][0] * Scale), float(E[size_t(I)][1] * Scale), float(E[size_t(I)][2] * Scale), 0.0f)); }
		{
			double Full[3] = { 0.0, 0.0, 0.0 };
			for (const FUnresolvedTable::FRow& R : T.Rows) { for (int K = 0; K < 3; ++K) { Full[K] += R.P * R.LbarRgb[K]; } }
			const double Exp = std::pow(10.0, 0.4 * F.ExposureMag);
			Lut[size_t(Block) * size_t(N) + 1u].w = float((0.2126 * Full[0] + 0.7152 * Full[1] + 0.0722 * Full[2]) * Exp * Scale);   // D-234
		}
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
				UnresolvedAt(T, F, Fl, Tm, V, Keep);
				for (int K = 0; K < 3; ++K) { Acc[size_t(K)] += V[K] * Tm * Dln / double(Sub); }
			}
			Tail[size_t(I)] = Acc;
		}
		for (int I = 0; I < N; ++I) { Lut.push_back(HV::float4(float(Tail[size_t(I)][0] * Scale), float(Tail[size_t(I)][1] * Scale), float(Tail[size_t(I)][2] * Scale), 0.0f)); }
		if (bDrawn && !T.Rows.empty())
		{
			int Lo = T.Rows.front().Level, Hi = Lo;
			for (const FUnresolvedTable::FRow& R : T.Rows) { Lo = std::min(Lo, R.Level); Hi = std::max(Hi, R.Level); }
			const int Count = Hi - Lo + 1;
			if (Lo >= 0 && Lo < 128 && Count <= int(HV::UNI_VOL_GLOW_LEVELS))
			{
				const double Exp = std::pow(10.0, 0.4 * F.ExposureMag);
				for (int Q = 0; Q < Count; ++Q)
				{
					for (int I = 0; I < N; ++I)
					{
						const double Tcm = LutT(I);
						const double X = F.LimitingMag - 5.0 * std::log10(std::max(Tcm, 1.0) / kTenPcCm);
						double D[3] = { 0.0, 0.0, 0.0 };
						for (size_t Ri = 0; Ri < T.Rows.size(); ++Ri)
						{
							const FUnresolvedTable::FRow& R = T.Rows[Ri];
							if (R.Level != Lo + Q || !(Fl[Ri] > 0.0)) { continue; }
							double G[3];
							T.G(R, X, G);
							for (int K = 0; K < 3; ++K) { D[K] += R.P * Keep * Fl[Ri] * G[K]; }
						}
						const double Dim = double(HV::Uni_HorizonDimming(float(Tcm)));
						Lut.push_back(HV::float4(float(D[0] * Dim * Exp * Scale), float(D[1] * Dim * Exp * Scale), float(D[2] * Dim * Exp * Scale), 0.0f));
					}
				}
				Lut[size_t(Block) * size_t(N)].w = float(Lo + 256 * Count);
			}
		}
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
			D.V[25] = P.DustClumps;   // D-234: dark clouds (G.y: no bulge reads it on a dust set)
		}
		return D;
	}

	// ======================================================================================
	// Carriers (D-129)
	// ======================================================================================

	/** D-196: the volume march's size for an output view: the output / Divisor, then scaled
	 *  down (aspect kept) to at most MaxPixels when that is positive. The glow is smooth, and
	 *  its cost is the rays marched: on a large viewport half resolution was 0.5 Mpx (about
	 *  130k rays a frame, sliced), for no visible gain over ~0.26 Mpx. Small carriers are
	 *  supersampled by the march texel's size either way (Uni_CarrierSubsamplesRay). */
	inline void VolumeMarchSize(int InOutW, int InOutH, int InDivisor, double InMaxPixels, int& OutW, int& OutH)
	{
		const int Div = std::max(InDivisor, 1);
		OutW = std::max(InOutW / Div, 1);
		OutH = std::max(InOutH / Div, 1);
		const double N = double(OutW) * double(OutH);
		if (InMaxPixels > 0.0 && N > InMaxPixels)
		{
			const double K = std::sqrt(InMaxPixels / N);
			OutW = std::max(int(double(OutW) * K), 4);
			OutH = std::max(int(double(OutH) * K), 4);
		}
	}

	/** D-209: the end (float4 entries) of the LUT's blocks: the cell-glow grids (D-203) follow
	 *  them and change size whenever a level starts or finishes loading, so a test for "the LUT's
	 *  layout changed" (the background map's) compares this, not the buffer's size. */
	inline u32 LutBlocksEnd(u32 InGlowLevels, u32 InGlowBase, size_t InLutSize)
	{
		return InGlowLevels > 0u ? std::min(InGlowBase, u32(InLutSize)) : u32(InLutSize);
	}

	/** D-200: the volume march's pixel budget follows the GPU frame time (dynamic resolution for
	 *  the glow alone). Levels are fixed (each ~0.92x the march's linear size; 1 down to 0.35 of
	 *  the budget), changed at most once per HoldFrames, so the march size, and its history,
	 *  changes rarely. Down when the smoothed GPU time is over the target by 3%; up when under it
	 *  by 15% (hysteresis: a level that only just fits is kept). Off (scale 1) without a target. */
	struct FVolumeAutoBudget
	{
		static constexpr int NumLevels = 7;
		static constexpr double Scales[NumLevels] = { 1.0, 0.84, 0.71, 0.6, 0.5, 0.42, 0.35 };
		int Level = 0;
		int SinceChange = 0;
		double SmoothMs = 0.0;
		bool bPrimed = false;
		u32 Changes = 0;

		double Scale() const { return Scales[std::clamp(Level, 0, NumLevels - 1)]; }

		/** One frame: the GPU's last frame time (ms) against the target (ms; <= 0: off). Returns
		 *  the budget's scale for this frame. */
		double Update(double InGpuMs, double InTargetMs, int InHoldFrames = 20)
		{
			if (!(InTargetMs > 0.0) || !(InGpuMs > 0.0) || !std::isfinite(InGpuMs))
			{
				if (!(InTargetMs > 0.0)) { Level = 0; SinceChange = 0; bPrimed = false; }
				return Scale();
			}
			SmoothMs = bPrimed ? SmoothMs + 0.15 * (InGpuMs - SmoothMs) : InGpuMs;
			bPrimed = true;
			++SinceChange;
			if (SinceChange < std::max(InHoldFrames, 1)) { return Scale(); }
			if (SmoothMs > 1.03 * InTargetMs && Level < NumLevels - 1) { ++Level; SinceChange = 0; ++Changes; }
			else if (SmoothMs < 0.85 * InTargetMs && Level > 0) { --Level; SinceChange = 0; ++Changes; }
			return Scale();
		}
	};

	struct FVolumeSettings
	{
		float StepRatio = 0.32f;  // D-195: 0.08 -> 0.24; D-199: 0.32 (T2.vol.perf_inside p95 <= 0.61%, T3.vol.march 0.72%)
		float HintScale = 3.0f;   // D-195: step hints x 2 in carriers around the camera (uu.VolumeHintScale); D-198: x 3 (T2.vol.perf_inside p95 0.41%)
		float WebHintScale = 1.0f; // D-202: the background web's step hints x this (the engine raises it with travel speed)
		float MinStepCm = 1.0e15f;
		u32 MaxSteps = 512;
		u32 MaxSubsamples = 8;   // D-157: rays per pixel axis cap (uu.VolumeMaxSubsamples)
		bool bLutStart = true;   // D-160: emission-only carriers start where their LUT becomes significant
		bool bPrefilter = true;  // D-160: galaxies sampled prefiltered over the pixel footprint (no disc / arm supersampling)
		u32 MaxCarriers = 128;   // D-161: 64 -> 128 (a cluster core had 62)
		double LocalStartCm = 3.0856775814913673e19;      // D-195: 1 -> 10 pc (uu.VolumeStartPc; the first 10 pc of a
		                                                  // galaxy column is ~1e-4 of it and cost ~60 log steps)
		double BackgroundStartCm = 3.0856775814913673e23; // 0.1 Mpc: nearer galaxies are resolved, and the first 0.1 Mpc of an
		                                                  // r_H-long column is ~2e-5 of the background (log steps from 1 pc cost ~350)
		double BackgroundPeriods = 0.5;                   // march extent (50 Mpc for the demo web); the analytic tail follows
		double SliceLoCm = 3.0856775814913673e18;         // transmittance slices: 1 pc ..
		double SliceHiCm = 3.0856775814913673e23;         // .. 100 kpc
		float DustSigmaRgb[3] = { 0.8f, 1.0f, 1.3f };
		float TailDim = 0.0f;                             // D-189: the web in the sky (uu.SkyWeb): 0 physical
		float WebGlowGain = 0.0f;
		float WebGlowDepthCm = 1.5428387907456837e26f;    // 50 Mpc
		/** D-230: the motion web (uu.MotionWeb*): the textured web's filaments and nodes as a faint
		 *  glow while travelling (ramped by the travel curve on the game thread; 0: off). */
		float MotionWebGain = 0.0f;                       // radiance per cm at full strength
		float MotionWebNearCm = 3.0856775814913673e24f;   // 1 Mpc
		float MotionWebDepthCm = 1.5428387907456837e26f;  // 50 Mpc
		float MotionWebInvRef = 0.0f;                     // 1 / (filament + node mean x threshold)
		float MotionWebDetailCm = 0.0f;                   // D-231: the detail's period, cm (0: none)
		float MotionWebDetailGain = 0.0f;                 // D-231: 0 none .. 1 the full pattern
		float MotionWebStepCm = 0.0f;                     // D-231: the step cap within the web's band with the detail on (0: none)
		float MotionWebRegionCm = 0.0f;                   // D-233: the detail's regions' period, cm (0: the ridged channel alone)
		float NebulaGain = 0.0f;                          // D-234: the emission nebulae (x the galaxy's unresolved light per star; 0: off)
		float NebulaReflect = 0.0f;                       // D-234: the reflection nebulae (0: off)
		float NebulaNearCm = 0.0f;                        // D-234: the nebulae fade in from here to 3x (0: none)
		float NebulaFineGain = 1.0f;                      // D-237, D-241: the nebulae's detail layers (with NebulaGain; 0: off)
		float NebulaDark = 0.35f;                         // D-242: the nebulae's mean dark share: 0 all glow, 1 all dust (below 0: none, D-241's)
		u32 NebulaRefine = 1u;                            // D-244: the most sub-samples of a step inside a spiral's gas layer, each the whole field (0, 1: off; uu.Nebula.Refine)
		bool bNebulaHandoffFade = true;                   // D-245: a galaxy's nebulae fade in past the handoff (false: D-244's, at once; checks)
		float NebulaFineReachCm = 3.0856775814913673e21f; // D-237: 1 kpc: the detail out to here (D-248: the engine's uu.Nebula.FineReachKpc is 7; the checks keep 1)
		float NebulaScale = 1.0f;                         // D-248: the nebulae's size (x; render only; NebulaTune)
		float NebulaPrevalence = 1.0f;                    // D-248: their coverage over the default's (~11% of the gas layer)
		float NebulaFineScale = 1.0f;                     // D-248: the detail layers' size (x)
		float NebulaFineOctave = 4.0f;                    // D-248: the texture layer's frequency over the walls'
		float NebulaFineLodBias = 0.0f;                   // D-248: mips added to the detail reads (+: fades sooner)
		float NebulaFineStepLod = 0.0f;                   // D-251: the detail's footprint at least this x the march's step (0: the pixel's)
		float NebulaStepRatio = 0.0f;                     // D-251: the step ratio through a spiral's gas layer within the detail's reach (0: the carrier's)
		float NebulaPeak = 0.0f;                          // D-253: the nebulae's shape softly held under this x its mean (0: off)
		// D-250: the galaxies' diffuse dust: each dusty galaxy's mid-plane reach (tau 1) at 2.5 disc
		// scale lengths, log-uniform in [lo, hi] by its seed; x gain (0: off: the checks' default; the
		// engine's uu.Galaxy.DiffuseDust is 1); its scale height x the dust set's.
		float DiffuseDustReachLoCm = 2.3142581861185255e21f;   // 0.75 kpc
		float DiffuseDustReachHiCm = 3.0856775814913673e22f;   // 10 kpc
		float DiffuseDustGain = 0.0f;
		float DiffuseDustHeight = 1.5f;
		float StepFoot = 0.0f;                            // D-255: a galaxy's field over at least this x the march's step along the disc (the cone rule; 0: the pixel's footprint alone)
		u32 SegmentTaps = 1u;                             // D-262: the camera's galaxy's step read at up to this many points along its run (paradigm C; 1: one, D-255's)
		float OpaqueCutoff = 1.0e-3f;                     // D-262: a ray stops when its transmittance is under this (uu.Volume.OpaqueCutoff)
		// D-265: the camera's galaxy's detail layers (render only; Uni_GalaxyDetailLayers). Off here
		// (the checks' march as before); the engine's defaults are uu.Galaxy.Detail*'s.
		float DetailSteps = 0.0f;                         // a layer's steps at its resolving step past the log step's reach (0: no step cap)
		float DetailStepTexels = 4.0f;                    // the resolving step in the layer's texels
		float DetailFreq0 = 0.0f, DetailLight0 = 0.0f, DetailDust0 = 0.0f;   // layer 0: x the detail's frequency (0: off), amplitudes 0..1
		float DetailFreq1 = 0.0f, DetailLight1 = 0.0f, DetailDust1 = 0.0f;   // layer 1
		float DetailFoot = 0.5f;                          // a layer read over at least this x the step's share
		/** D-203: cell glow: the LUT carries each table's drawn light per level, and the source's
		 *  loading cells (FVolumeSource::CellGlow) are drawn as glow by the march. */
		bool bCellGlow = false;
		/** D-210: write the galaxy constant blocks (FUniVolumeView::GalaxyConstBase): the march reads
		 *  each galaxy's per-object hashes instead of computing them per step (bit for bit the same). */
		bool bGalaxyConst = true;
		float TexturedWebStepRatio = 0.4f;                // D-164: log step ratio of a textured web carrier (T3.webtex.background: same light as 0.08, 30% fewer steps);
		                                                  // D-199: 0.2 -> 0.4 (50 -> 40 steps a texel; cube light -0.13%, correlation 0.999, unchanged: the texture's hint bounds it)
	};

	/** D-248 (owner 2026-11-15: handles for the nebulae's frequency, scale and prevalence, and the
	 *  detail's frequencies and fade): the values UniFields.ush's UNI_NEB_TUNE / UNI_NEB_TUNE2 take
	 *  (render only). Scale: the nebulae's size (the knot read's frequency over it); Prevalence:
	 *  their coverage over the default's (~11% of the gas layer): the knot thresholds shift by d
	 *  where the measured coverage is that multiple (kNebulaPrevalence, sampled: d against the
	 *  coverage, the knot shape's mean E[X^2 J] and the eroded shape's mean E[D2^2 J] at full
	 *  detail over the Milky Way's gas layer, each over d = 0's; experiments/nebula_prevalence_d248), the shape means scaled by
	 *  them so the far blend and the near detail keep one light; each nebula keeps its brightness,
	 *  so the light grows with the coverage; the eroded shape's mean also follows the scale
	 *  (kNebulaErodeByScale). FineScale: the detail's size; FineOctave: the texture
	 *  layer's frequency over the walls'; FineLodBias: mips added to the detail reads' (+1: each
	 *  layer fades an octave sooner). Defaults: D-241..D-247's constants exactly. */
	struct FNebulaTune
	{
		std::array<float, 4> T{ { 1.0f, 0.0f, 0.01122f, 0.00168205f } };
		std::array<float, 4> T2{ { 1.0f, 4.0f, 0.0f, 0.0f } };
		std::array<float, 4> T3{ { 0.0f, 0.0f, 0.0f, 0.0f } };   // D-251: (the gas layer's step ratio, 0, 0, 0)
	};

	struct FNebulaPrevalenceRow { float D, Cover, Mean, Mean2; };
	inline constexpr FNebulaPrevalenceRow kNebulaPrevalence[] = {
		{ -0.300f, 0.057638f, 0.026664f, 0.009411f },
		{ -0.275f, 0.070813f, 0.039304f, 0.016409f },
		{ -0.250f, 0.086986f, 0.056205f, 0.027056f },
		{ -0.225f, 0.111818f, 0.078462f, 0.042930f },
		{ -0.200f, 0.143839f, 0.107703f, 0.066187f },
		{ -0.175f, 0.184736f, 0.146051f, 0.099949f },
		{ -0.150f, 0.239819f, 0.196276f, 0.148616f },
		{ -0.125f, 0.311011f, 0.261856f, 0.217205f },
		{ -0.100f, 0.401085f, 0.346702f, 0.308659f },
		{ -0.075f, 0.510872f, 0.455879f, 0.425968f },
		{ -0.050f, 0.642295f, 0.595562f, 0.573182f },
		{ -0.025f, 0.800735f, 0.773656f, 0.760715f },
		{ 0.000f, 1.000000f, 1.000000f, 1.000000f },
		{ 0.025f, 1.241742f, 1.285522f, 1.298749f },
		{ 0.050f, 1.525167f, 1.645147f, 1.674183f },
		{ 0.075f, 1.846671f, 2.095955f, 2.148812f },
		{ 0.100f, 2.204186f, 2.657163f, 2.750275f },
		{ 0.125f, 2.592359f, 3.350356f, 3.517101f },
		{ 0.150f, 3.009852f, 4.200084f, 4.502619f },
		{ 0.175f, 3.444601f, 5.226611f, 5.747475f },
		{ 0.200f, 3.892498f, 6.449276f, 7.291674f },
		{ 0.225f, 4.340946f, 7.889145f, 9.197646f },
		{ 0.250f, 4.780291f, 9.561095f, 11.516197f },
		{ 0.275f, 5.204367f, 11.474563f, 14.290104f },
		{ 0.300f, 5.608983f, 13.636032f, 17.570494f }
	};

	/** D-248: the erosion's share of a nebula's light (the eroded shape's mean over the structure's
	 *  E[X^2 J], full detail) at its scale over the default's, scale 1/4 .. 4 in half octaves: the
	 *  detail layers read the same texture as the knots at a fixed size, so their overlap changes
	 *  with the nebulae's size (Milky Way gas layer, two sample sets within ~3%; past x4 it falls
	 *  away, so the scale is held to 1/4 .. 4). */
	inline constexpr float kNebulaErodeByScale[9] = { 0.8951f, 0.9145f, 0.9316f, 0.9152f, 1.0000f, 1.0517f, 1.1006f, 1.1391f, 1.1559f };

	inline float NebulaErodeFactor(float InScale)
	{
		const float L = std::clamp(2.0f * std::log2(std::clamp(InScale, 0.25f, 4.0f)) + 4.0f, 0.0f, 8.0f);
		const int I = std::min(int(L), 7);
		const float T = L - float(I);
		return kNebulaErodeByScale[I] + (kNebulaErodeByScale[I + 1] - kNebulaErodeByScale[I]) * T;
	}

	/** D-248: the thresholds' shift and the shape means' factors for a prevalence (coverage over the
	 *  default's), log-interpolated in the coverage; clamped to the table (x0.058 .. x5.61). */
	inline FNebulaPrevalenceRow NebulaPrevalenceRow(float InPrevalence)
	{
		constexpr size_t N = sizeof(kNebulaPrevalence) / sizeof(kNebulaPrevalence[0]);
		const float P = std::clamp(InPrevalence, kNebulaPrevalence[0].Cover, kNebulaPrevalence[N - 1].Cover);
		if (P == 1.0f) { return { 0.0f, 1.0f, 1.0f, 1.0f }; }
		size_t I = 0;
		while (I + 2 < N && kNebulaPrevalence[I + 1].Cover < P) { ++I; }
		const FNebulaPrevalenceRow& A = kNebulaPrevalence[I];
		const FNebulaPrevalenceRow& B = kNebulaPrevalence[I + 1];
		const double T = std::clamp((std::log(double(P)) - std::log(double(A.Cover))) / (std::log(double(B.Cover)) - std::log(double(A.Cover))), 0.0, 1.0);
		auto Lg = [T](float X, float Y) { return float(std::exp(std::log(double(X)) + (std::log(double(Y)) - std::log(double(X))) * T)); };
		return { float(double(A.D) + (double(B.D) - double(A.D)) * T), P, Lg(A.Mean, B.Mean), Lg(A.Mean2, B.Mean2) };
	}

	inline FNebulaTune NebulaTune(float InScale, float InPrevalence, float InFineScale, float InFineOctave, float InFineLodBias, float InFineStepLod = 0.0f, float InStepRatio = 0.0f, float InPeak = 0.0f)
	{
		FNebulaTune R;
		const FNebulaPrevalenceRow Pr = NebulaPrevalenceRow(InPrevalence);
		const float Sc = std::clamp(InScale, 0.25f, 4.0f);
		R.T[0] = Sc == 1.0f ? 1.0f : 1.0f / Sc;
		R.T[1] = Pr.D;
		R.T[2] = 0.01122f * Pr.Mean;
		R.T[3] = 0.00168205f * Pr.Mean2 * (Sc == 1.0f ? 1.0f : NebulaErodeFactor(Sc));
		R.T2[0] = 1.0f / std::clamp(InFineScale, 0.1f, 10.0f);
		R.T2[1] = std::clamp(InFineOctave, 1.0f, 16.0f);
		R.T2[2] = std::clamp(InFineLodBias, -4.0f, 4.0f);
		R.T2[3] = std::clamp(InFineStepLod, 0.0f, 4.0f);   // D-251
		R.T3[0] = InStepRatio > 0.0f ? std::clamp(InStepRatio, 0.005f, 1.0f) : 0.0f;
		R.T3[1] = InPeak > 0.0f ? std::clamp(InPeak, 1.0f, 100000.0f) : 0.0f;   // D-253
		return R;
	}

	inline FNebulaTune NebulaTune(const FVolumeSettings& InS)
	{
		return NebulaTune(InS.NebulaScale, InS.NebulaPrevalence, InS.NebulaFineScale, InS.NebulaFineOctave, InS.NebulaFineLodBias, InS.NebulaFineStepLod, InS.NebulaStepRatio, InS.NebulaPeak);
	}

	/** D-248: the shim's tuning globals set for the harness (UNI_NEB_TUNE in the C++ cores). */
	inline void ApplyNebulaTune(const FNebulaTune& InT)
	{
		HV::GNebTune = HV::float4(InT.T[0], InT.T[1], InT.T[2], InT.T[3]);
		HV::GNebTune2 = HV::float4(InT.T2[0], InT.T2[1], InT.T2[2], InT.T2[3]);
		HV::GNebTune3 = HV::float4(InT.T3[0], InT.T3[1], InT.T3[2], InT.T3[3]);
	}

	/** D-196: a local carrier's share of its parent object's light, as built this frame: the
	 *  handoff curve (Uni_CarrierHandoff) x the cap fade (D-161). The object's sprite draws the
	 *  rest (1 - Hand); an object with no entry (no carrier this frame: over the cap, its
	 *  integral not ready, its contributor asleep, or no volume) keeps its whole sprite. */
	struct FCarrierHandoff
	{
		FObjectId Parent;
		float Hand = 0.0f;
	};

	/** D-211: the camera's own galaxy: the local prefiltered galaxy carrier whose cut sphere holds
	 *  the camera (the most weighted when several do), for the galaxy map (D-212). */
	struct FCarrierHome
	{
		int Carrier = -1;           // its index among the scene's local (non-periodic) carriers, as the engine and Subset(false) order them; -1: none
		FObjectId Parent;           // its object (the cache's identity)
		double CamField[3] = { 0.0, 0.0, 0.0 };   // the camera in its field units, in double
	};

	struct FVolumeScene
	{
		std::vector<FCarrierGPU> Carriers;
		std::vector<FCarrierHandoff> Handoffs;   // D-196: one per local carrier built
		std::vector<FFieldParamsGPU> Params;
		std::vector<HV::float4> Lut;
		FVolumeViewGPU View{};
		FCarrierHome Home;            // D-211
		size_t CarriersSkipped = 0;   // over the cap
		/** D-208: the nearest marched structure (cm): over the carriers, where the march starts
		 *  (TStart) or, from outside a local carrier, its sphere if farther. The renderer's
		 *  temporal slicing measures the camera's step against it. 0: no carrier. */
		double NearCm = 0.0;

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
			if (bPeriodic) { O.Home = FCarrierHome{}; O.View.MapCarrier = 0u; }   // D-211: the home is a local carrier
			return O;
		}

		HV::StructuredBuffer<HV::FUniCarrier> CarrierBuf() const { return HV::StructuredBuffer<HV::FUniCarrier>(reinterpret_cast<const HV::FUniCarrier*>(Carriers.data()), HV::uint(Carriers.size())); }
		HV::StructuredBuffer<HV::FUniFieldParams> ParamBuf() const { return HV::StructuredBuffer<HV::FUniFieldParams>(reinterpret_cast<const HV::FUniFieldParams*>(Params.data()), HV::uint(Params.size())); }
		HV::StructuredBuffer<HV::float4> LutBuf() const { return HV::StructuredBuffer<HV::float4>(Lut.data(), HV::uint(Lut.size())); }
	};
	/** D-224: true when the camera is inside a local (non-periodic) carrier's sphere: in a galaxy,
	 *  or a cluster's or group's glow. The renderer then keeps the background on its direction map
	 *  instead of marching it per pixel while travelling (uu.BackgroundMode 2): per pixel, every
	 *  marched ray inside a galaxy also crosses the web, 7-10x the march's cost (explore.hitch), while
	 *  the map's refresh is a fixed two faces a frame, cross-faded. */
	inline bool CameraInsideLocalCarrier(const FCarrierGPU* InCarriers, size_t InCount)
	{
		for (size_t I = 0; I < InCount; ++I)
		{
			const FCarrierGPU& C = InCarriers[I];
			if ((C.Flags & HV::UNI_CARRIER_PERIODIC) != 0u) { continue; }
			const float D2 = C.CamLocal[0] * C.CamLocal[0] + C.CamLocal[1] * C.CamLocal[1] + C.CamLocal[2] * C.CamLocal[2];
			if (D2 < 1.0f) { return true; }
		}
		return false;
	}

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
		/** D-229: set for the runtime streamer (FUniStreamer::VolumeIntegral): the integral a local
		 *  carrier uses this frame (the exact one, a galaxy's quick estimate meanwhile, eased between
		 *  them), given its angular size (the largest are estimated first); -1: none yet, skip it.
		 *  Empty: IntegralReady and the exact integral, as before. */
		std::function<double(const FContributor&, double)> VolumeIntegral;
		/** D-161: optional: the contributors whose influence radius subtends more than an angle,
		 *  from the streamer's per-frame measured distances. The carrier build then rejects far
		 *  contributors with no per-contributor map walk or 128-bit work. */
		std::function<std::vector<const FContributor*>(double)> NearContributors;
		/** D-203: optional: the levels with cells still loading or fading in (FUniStreamer). */
		std::function<std::vector<FCellGlowLevel>()> CellGlow;

		template <typename T, typename = void> struct THasMeasured : std::false_type {};
		template <typename T> struct THasMeasured<T, std::void_t<decltype(std::declval<const T&>().NearContributors(0.0))>> : std::true_type {};
		template <typename T, typename = void> struct THasCellGlow : std::false_type {};
		template <typename T> struct THasCellGlow<T, std::void_t<decltype(std::declval<const T&>().CellGlow())>> : std::true_type {};
		template <typename T, typename = void> struct THasVolumeIntegral : std::false_type {};
		template <typename T> struct THasVolumeIntegral<T, std::void_t<decltype(std::declval<const T&>().VolumeIntegral(std::declval<const FContributor&>(), 0.0))>> : std::true_type {};
		template <typename T, typename = void> struct THasRequestIntegral : std::false_type {};
		template <typename T> struct THasRequestIntegral<T, std::void_t<decltype(std::declval<const T&>().RequestIntegral(std::declval<const FContributor&>()))>> : std::true_type {};

		template <typename T> static FVolumeSource From(const T& P)
		{
			FVolumeSource S;
			S.Contributors = &P.Contributors();
			S.FindObject = [&P](const FObjectId& Id) { return P.FindObject(Id); };
			S.LevelFade = [&P](int L) { return P.LevelFade(L); };
			if constexpr (THasRequestIntegral<T>::value) { S.IntegralReady = [&P](const FContributor& C) { return P.RequestIntegral(C); }; }
			if constexpr (THasVolumeIntegral<T>::value) { S.VolumeIntegral = [&P](const FContributor& C, double Theta) { return P.VolumeIntegral(C, Theta); }; }
			if constexpr (THasMeasured<T>::value)
			{
				S.NearContributors = [&P](double MinTheta) { return P.NearContributors(MinTheta); };
			}
			if constexpr (THasCellGlow<T>::value) { S.CellGlow = [&P]() { return P.CellGlow(); }; }
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
		/** D-203 test hooks: per-level fades (F_L) in place of the source's, and cell glow grids in
		 *  place of the source's (with bCellGlow). */
		std::map<int, float> ForceLevelFades;
		const std::vector<FCellGlowLevel>* GlowOverride = nullptr;

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
			const std::map<int, float>* ForceL = &ForceLevelFades;
			F.LevelFade = [&P, Force, ForceL](int L) { const auto It = ForceL->find(L); return It != ForceL->end() ? It->second : (Force >= 0.0f ? Force : P.LevelFade(L)); };
			std::map<u32, u32> Block;
			for (const auto& KV : Tables)
			{
				// The LUT depends only on (m_lim, m_exp, F_L per slice level): rebuild only when
				// those change (most frames they do not; a rebuild is ~4k table evaluations).
				// Quantised to 1e-3 (fades step by 1/8 per frame; magnitudes are user settings).
				auto Q = [](double V) { return std::round(V * 1000.0) / 1000.0; };
				std::vector<double> Key = { Q(F.LimitingMag), Q(F.ExposureMag), S.bCellGlow ? 1.0 : 0.0 };
				for (double Fv : RowFades(KV.second, F)) { Key.push_back(Q(Fv)); }
				FLutCache& Cache = LutCache[KV.first];
				Block[KV.first] = u32(Out.Lut.size() / HV::UNI_VOL_LUT_SIZE);
				if (Cache.Key != Key)
				{
					Cache.Key = Key;
					Cache.Lut.clear();
					BuildEmissionLut(KV.second, F, Cache.Lut, LutScale(KV.second), 1.0, S.bCellGlow);
					++LutRebuilds;
				}
				Out.Lut.insert(Out.Lut.end(), Cache.Lut.begin(), Cache.Lut.end());
				SigStart[KV.first] = LutSignificantStart(Cache.Lut);
			}

			// D-197: emission blocks for thinned emitters (SpriteKeep < 1: only Keep of the children
			// exist to be drawn), per (child, keep), cached like the full blocks.
			std::map<std::pair<u32, int>, u32> KeepBlock;
			std::map<std::pair<u32, int>, double> KeepSigStart;
			auto KeepBlockOf = [&](u32 Child, double Keep, double& OutSigStart) -> u32
			{
				const int Kq = int(std::lround(std::clamp(Keep, 0.0, 1.0) * 1000.0));
				if (Kq >= 1000) { OutSigStart = SigStart[Child]; return Block.at(Child); }
				const std::pair<u32, int> Id{ Child, Kq };
				const auto It = KeepBlock.find(Id);
				if (It != KeepBlock.end()) { OutSigStart = KeepSigStart[Id]; return It->second; }
				auto Q = [](double V) { return std::round(V * 1000.0) / 1000.0; };
				std::vector<double> Key = { Q(F.LimitingMag), Q(F.ExposureMag), double(Kq), S.bCellGlow ? 1.0 : 0.0 };
				for (double Fv : RowFades(Tables.at(Child), F)) { Key.push_back(Q(Fv)); }
				FLutCache& Cache = KeepLutCache[Id];
				if (Cache.Key != Key)
				{
					Cache.Key = Key;
					Cache.Lut.clear();
					BuildEmissionLut(Tables.at(Child), F, Cache.Lut, LutScale(Tables.at(Child)), double(Kq) / 1000.0, S.bCellGlow);
					++LutRebuilds;
				}
				const u32 B = u32(Out.Lut.size() / HV::UNI_VOL_LUT_SIZE);
				Out.Lut.insert(Out.Lut.end(), Cache.Lut.begin(), Cache.Lut.end());
				KeepBlock[Id] = B;
				KeepSigStart[Id] = LutSignificantStart(Cache.Lut);
				OutSigStart = KeepSigStart[Id];
				return B;
			};

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
					double KeepSigRoot = 0.0;
					G.LutIndex = KeepBlockOf(C.Emitter->ChildArchetypeId, C.Keep, KeepSigRoot);   // D-197
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

			// D-191: total-light LUT blocks (no child drawn as a sprite) for intracluster light,
			// built the first time a carrier needs one (once per scene and child), cached like the
			// emission blocks.
			std::map<u32, u32> TotalBlock;
			auto TotalBlockOf = [&](u32 Child) -> u32
			{
				const auto It = TotalBlock.find(Child);
				if (It != TotalBlock.end()) { return It->second; }
				FLutFrame Ft = F;
				Ft.LevelFade = nullptr;   // F_L = 0: every child's light
				const std::vector<double> Key = { std::round(double(Ft.ExposureMag) * 1000.0) / 1000.0 };
				FLutCache& Cache = TotalLutCache[Child];
				if (Cache.Key != Key)
				{
					Cache.Key = Key;
					Cache.Lut.clear();
					BuildEmissionLut(Tables.at(Child), Ft, Cache.Lut, LutScale(Tables.at(Child)));
					++LutRebuilds;
				}
				const u32 B = u32(Out.Lut.size() / HV::UNI_VOL_LUT_SIZE);
				Out.Lut.insert(Out.Lut.end(), Cache.Lut.begin(), Cache.Lut.end());
				TotalBlock[Child] = B;
				return B;
			};

			// Local carriers: resident contributors, ranked by angular size, capped.
			struct FCand { double Theta; FCarrierGPU G; FFieldParamsGPU Par; FFieldParamsGPU Dust; bool bDust; FFieldParamsGPU Palette; bool bPalette = false; FObjectId Parent; float Hand = 0.0f; double CamD[3] = { 0.0, 0.0, 0.0 }; };
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
					// near 0), so it is normally ready long before the carrier matters. D-229: in a
					// rich cluster hundreds reach it at once: a galaxy's quick estimate meanwhile.
					double I = -1.0;
					if (P.VolumeIntegral)
					{
						I = P.VolumeIntegral(C, Theta);
						if (!(I > 0.0)) { continue; }
					}
					else if (P.IntegralReady && !P.IntegralReady(C)) { continue; }
					FCand Cd;
					Cd.Theta = Theta;
					Cd.Parent = C.Parent;   // D-196: the sprite hands over exactly what the carrier takes
					Cd.Hand = W;
					FCarrierGPU& G = Cd.G;
					G = FCarrierGPU{};
					const double Inv = 1.0 / C.InfluenceCm;
					for (int A = 0; A < 3; ++A)
					{
						Cd.CamD[A] = (double(C.Rot[A * 3 + 0]) * Dx + double(C.Rot[A * 3 + 1]) * Dy + double(C.Rot[A * 3 + 2]) * Dz) * Inv;   // D-211
						G.CamLocal[A] = float(Cd.CamD[A]);
						G.Rot0[A] = C.Rot[A];
						G.Rot1[A] = C.Rot[3 + A];
						G.Rot2[A] = C.Rot[6 + A];
					}
					G.InvRadius = float(Inv);
					G.FieldId = C.Emitter->FieldId;
					double KeepSig = 0.0;
					G.LutIndex = KeepBlockOf(C.Emitter->ChildArchetypeId, C.Keep, KeepSig);   // D-197
					if (!(I > 0.0)) { I = Gen.IntegralAll(C); }
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
					if (!Cd.bDust && S.bLutStart) { G.TStart = float(std::max(S.LocalStartCm, KeepSig)); }
					if (S.bPrefilter && C.Emitter->FieldId == HV::UNI_FIELD_GALAXY) { G.Flags |= HV::UNI_CARRIER_PREFILTER; }
					// D-245: a galaxy's nebulae fade in past the handoff (the sprite has none of their light).
					if (C.Emitter->FieldId == HV::UNI_FIELD_GALAXY && S.bNebulaHandoffFade) { G.NebulaHide = HV::Uni_CarrierNebulaHide(float(Theta), View.ThetaPixel); }
					// D-191: a rich cluster's intracluster light rides on its own carrier: the same
					// field sample, a second LUT block (every child's light: never resolved as
					// sprites) at IclRatio x its members' amplitude. It is there at any distance, so
					// the carrier marches from the local start (the members' significance start
					// would skip it from inside the cluster).
					if (C.IclRatio > 0.0 && C.Emitter->FieldId == HV::UNI_FIELD_CLUSTER)
					{
						G.Flags |= HV::UNI_CARRIER_ICL;
						G.PaletteIndex = TotalBlockOf(C.Emitter->ChildArchetypeId);
						G.IclRatio = float(C.IclRatio);
						if (!Cd.bDust) { G.TStart = float(S.LocalStartCm); }
					}
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
					const double Keep = std::clamp((Th - ThetaDrop) / (0.25 * Th), 0.0, 1.0);
					Cands[I].G.Weight = float(double(Cands[I].G.Weight) * Keep);
					Cands[I].Hand = float(double(Cands[I].Hand) * Keep);   // D-196
				}
			}
			float HomeWeight = 0.0f;   // D-211
			for (size_t I = 0; I < Cands.size(); ++I)
			{
				if (Out.Carriers.size() >= S.MaxCarriers) { Out.CarriersSkipped = Cands.size() - I; break; }
				FCand& Cd = Cands[I];
				Cd.G.ParamIndex = u32(Out.Params.size());
				Out.Params.push_back(Cd.Par);
				if (Cd.bDust) { Cd.G.DustParamIndex = u32(Out.Params.size()); Out.Params.push_back(Cd.Dust); }
				if (Cd.bPalette) { Cd.G.PaletteIndex = u32(Out.Params.size()); Out.Params.push_back(Cd.Palette); }
				// D-211: the camera's own galaxy (inside its cut sphere, prefiltered), the most weighted.
				if (Cd.G.FieldId == HV::UNI_FIELD_GALAXY && (Cd.G.Flags & HV::UNI_CARRIER_PREFILTER) != 0u
					&& Cd.CamD[0] * Cd.CamD[0] + Cd.CamD[1] * Cd.CamD[1] + Cd.CamD[2] * Cd.CamD[2] < 1.0
					&& (Out.Home.Carrier < 0 || Cd.G.Weight > HomeWeight))
				{
					int Local = 0;
					for (const FCarrierGPU& Q : Out.Carriers) { if ((Q.Flags & HV::UNI_CARRIER_PERIODIC) == 0u) { ++Local; } }
					Out.Home.Carrier = Local;
					Out.Home.Parent = Cd.Parent;
					for (int A = 0; A < 3; ++A) { Out.Home.CamField[A] = Cd.CamD[A]; }
					HomeWeight = Cd.G.Weight;
				}
				Out.Carriers.push_back(Cd.G);
				if (Cd.Hand > 0.0f) { Out.Handoffs.push_back(FCarrierHandoff{ Cd.Parent, Cd.Hand }); }   // D-196
			}

			// D-208: the nearest marched structure.
			Out.NearCm = 0.0;
			for (const FCarrierGPU& C : Out.Carriers)
			{
				double D = double(C.TStart);
				if ((C.Flags & HV::UNI_CARRIER_PERIODIC) == 0u && C.InvRadius > 0.0f)
				{
					const double L = std::sqrt(double(C.CamLocal[0]) * C.CamLocal[0] + double(C.CamLocal[1]) * C.CamLocal[1] + double(C.CamLocal[2]) * C.CamLocal[2]);
					if (L > 1.0) { D = std::max(D, (L - 1.0) / double(C.InvRadius)); }
				}
				if (D > 0.0 && (Out.NearCm <= 0.0 || D < Out.NearCm)) { Out.NearCm = D; }
			}
			FVolumeViewGPU& V = Out.View;
			// D-203: the loading levels' cell grids, after every LUT block: per level (ascending) a
			// header pair, (corner relative to the camera, cm; cell width) and (sides; the float
			// index of its first fade), then the fades, four to an entry.
			if (S.bCellGlow)
			{
				std::vector<FCellGlowLevel> Glow;
				if (GlowOverride) { Glow = *GlowOverride; }
				else if (P.CellGlow) { Glow = P.CellGlow(); }
				Glow.erase(std::remove_if(Glow.begin(), Glow.end(), [](const FCellGlowLevel& G) { return G.Level < 0 || G.Level >= 128 || G.Fades.empty() || !(G.CellCm > 0.0); }), Glow.end());
				std::sort(Glow.begin(), Glow.end(), [](const FCellGlowLevel& A, const FCellGlowLevel& B) { return A.Level < B.Level; });
				Glow.erase(std::unique(Glow.begin(), Glow.end(), [](const FCellGlowLevel& A, const FCellGlowLevel& B) { return A.Level == B.Level; }), Glow.end());
				if (!Glow.empty())
				{
					V.GlowBase = u32(Out.Lut.size());
					V.GlowLevels = u32(Glow.size());
					size_t FadeAt = (Out.Lut.size() + 2u * Glow.size()) * 4u;
					std::vector<float> Fades;
					const double Fr[3] = { CamFrac.X, CamFrac.Y, CamFrac.Z };
					for (const FCellGlowLevel& G : Glow)
					{
						V.GlowMask[G.Level >> 5] |= 1u << (G.Level & 31);
						Out.Lut.push_back(HV::float4(float(G.OriginCm[0] - Fr[0]), float(G.OriginCm[1] - Fr[1]), float(G.OriginCm[2] - Fr[2]), float(G.CellCm)));
						Out.Lut.push_back(HV::float4(float(G.Side[0]), float(G.Side[1]), float(G.Side[2]), float(FadeAt + Fades.size())));
						Fades.insert(Fades.end(), G.Fades.begin(), G.Fades.end());
					}
					while (Fades.size() % 4u) { Fades.push_back(1.0f); }
					for (size_t I = 0; I < Fades.size(); I += 4u) { Out.Lut.push_back(HV::float4(Fades[I], Fades[I + 1], Fades[I + 2], Fades[I + 3])); }
				}
			}
			// D-210: one galaxy constant block per parameter block, after them all (the march reads
			// a galaxy's hashes there instead of hashing at every step; other fields never read it).
			V.GalaxyConstBase = 0u;
			if (S.bGalaxyConst && !Out.Params.empty())
			{
				const size_t NP = Out.Params.size();
				V.GalaxyConstBase = u32(NP);
				for (size_t I = 0; I < NP; ++I)
				{
					HV::FUniFieldParams Hp;
					std::memcpy(static_cast<void*>(&Hp), &Out.Params[I], sizeof(Hp));
					const HV::FUniFieldParams Kb = HV::Uni_GalaxyConstPack(HV::Uni_GalaxyConstOf(Hp));
					FFieldParamsGPU G;
					std::memcpy(static_cast<void*>(&G), &Kb, sizeof(G));
					Out.Params.push_back(G);
				}
			}
			V.StepRatio = S.StepRatio;
			V.MinStepCm = S.MinStepCm;
			V.MaxSteps = S.MaxSteps;
			V.MaxSubsamples = S.MaxSubsamples;
			V.NumCarriers = u32(Out.Carriers.size());
			V.ThetaPixel = View.ThetaPixel;
			V.SliceLnTLo = float(std::log(S.SliceLoCm));
			V.SliceLnTHi = float(std::log(S.SliceHiCm));
			for (int K = 0; K < 3; ++K) { V.DustSigmaRgb[K] = S.DustSigmaRgb[K]; }
			V.WebHintScale = S.WebHintScale;
			V.TailDim = S.TailDim;
			V.WebGlowGain = S.WebGlowGain;
			V.WebGlowDepthCm = S.WebGlowDepthCm;
			V.MotionWebGain = S.MotionWebGain;         // D-230
			V.MotionWebNearCm = S.MotionWebNearCm;
			V.MotionWebDepthCm = S.MotionWebDepthCm;
			V.MotionWebInvRef = S.MotionWebInvRef;
			V.MotionWebDetailCm = S.MotionWebDetailCm;   // D-231
			V.MotionWebDetailGain = S.MotionWebDetailGain;
			V.MotionWebStepCm = S.MotionWebStepCm;
			V.MotionWebRegionCm = S.MotionWebRegionCm;   // D-233
			V.NebulaGain = S.NebulaGain;                 // D-234
			V.NebulaReflect = S.NebulaReflect;
			V.NebulaNearCm = S.NebulaNearCm;
			V.NebulaThetaPixel = 0.0f;   // D-235: set by the cube passes
			V.NebulaFineGain = S.NebulaFineGain;         // D-237
			V.NebulaDark = S.NebulaDark;                 // D-242
			V.NebulaRefine = S.NebulaRefine;             // D-244
			V.NebulaFineReachCm = S.NebulaFineReachCm;
			V.DiffuseDustReachLoCm = S.DiffuseDustReachLoCm;   // D-250
			V.DiffuseDustReachHiCm = S.DiffuseDustReachHiCm;
			V.DiffuseDustGain = S.DiffuseDustGain;
			V.DiffuseDustHeight = S.DiffuseDustHeight;
			V.StepFoot = S.StepFoot;                     // D-255
			V.SegmentTaps = S.SegmentTaps;               // D-262
			V.OpaqueCutoff = S.OpaqueCutoff;             // D-262
			V.DetailSteps = S.DetailSteps;               // D-265
			V.DetailStepTexels = S.DetailStepTexels;
			V.DetailFreq0 = S.DetailFreq0; V.DetailLight0 = S.DetailLight0; V.DetailDust0 = S.DetailDust0;
			V.DetailFreq1 = S.DetailFreq1; V.DetailLight1 = S.DetailLight1; V.DetailDust1 = S.DetailDust1;
			V.DetailFoot = S.DetailFoot; V.DetailPad = 0.0f;
			V.JitterShift = 0.0f;   // D-240: set per frame by the renderer
			V.HintScale = S.HintScale;
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
		mutable std::map<u32, FLutCache> TotalLutCache;   // D-191
		mutable std::map<std::pair<u32, int>, FLutCache> KeepLutCache;   // D-197
	};
	/** D-211: the scene's home carrier (FVolumeScene::Home: its index among the local carriers). */
	inline const FCarrierGPU* HomeCarrierOf(const FVolumeScene& Sc)
	{
		if (Sc.Home.Carrier < 0) { return nullptr; }
		int Local = 0;
		for (const FCarrierGPU& C : Sc.Carriers)
		{
			if ((C.Flags & HV::UNI_CARRIER_PERIODIC) != 0u) { continue; }
			if (Local == Sc.Home.Carrier) { return &C; }
			++Local;
		}
		return nullptr;
	}
}
