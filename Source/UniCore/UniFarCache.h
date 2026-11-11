#pragma once

// UniFarCache.h
// D-228 (PARADIGMS #2; measured D-227, costed at engine resolution in D-228): the far cache.
// While the camera is inside a galaxy and nearly still, the volume's light from beyond StartCm
// (the split, R) is held in a six-face direction map, and the per-pixel march stops at R:
//
//     B = B_near + T_near * (B_far(dir) + T_far(dir) * B_background)
//
// Each face has its own anchor (the camera position its light was marched from). A face is
// re-marched into its spare layer over FillFrames frames from one snapshot of the scene (no
// one-frame spike: one face in one frame is ~3x a frame's march), then swapped in. Faces in view
// are refreshed once the camera has moved DueTexels of parallax at R since their anchor; faces
// out of view only past OutOfViewTexels (refreshing them all would cost what the cache saves:
// D-228's measurement). Mode on below OnMotion texels of parallax a frame held for OnFrames
// frames with every face marched (and those in view fresh); off above OffMotion (hysteresis). At a switch the temporal
// history changes meaning (whole march <-> near part), so the reconstruct converts it once
// (HistSign; Uni_FarHistoryConvert).
//
// Engine-agnostic: the renderer feeds it a frame's facts and does what it says; univerify drives
// the same class against the reference march.

#include "UniVolume.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace UniCore
{
	struct FFarCacheSettings
	{
		bool bEnabled = true;
		double StartCm = 0.0;            // R: the split
		int FaceSize = 256;              // texels a side
		double OnMotion = 0.02;          // parallax at R a frame, in texels: on below (held OnFrames frames)
		double OffMotion = 0.04;         // off above
		int OnFrames = 30;
		double DueTexels = 0.3;          // a face in view is re-marched past this parallax since its anchor
		double OutOfViewTexels = 1.2;    // a face out of view, past this
		int FillFrames = 8;              // frames one face's re-march is spread over

		double TexelRad() const { return 2.0 / double(std::max(FaceSize, 1)); }
		int RowsPerFrame() const { return std::max(1, (FaceSize + std::max(FillFrames, 1) - 1) / std::max(FillFrames, 1)); }
	};

	/** One frame's facts, as the renderer sees them. */
	struct FFarCacheFrame
	{
		bool bEligible = false;          // inside a local carrier, background on its map, no debug view
		bool bLoading = false;           // cells loading or fading in (their glow is in the march, not in a kept map)
		bool bStepValid = false;         // CameraStep is this frame's step (false: a jump)
		double Step[3] = { 0, 0, 0 };    // camera now minus then, cm (render space)
		uint32_t FacesInView = 0;        // bit per face (CubeFacesInView)
		bool bSceneChanged = false;      // the local carriers or their LUT changed (FFarSceneKey)
		bool bReset = false;             // the map was (re)created: nothing in it
	};

	enum class EFarReason : uint32_t { On = 0, Disabled, Outside, Loading, Jump, Moving, Settling, Warming, Count };

	inline const char* FarReasonName(EFarReason R)
	{
		switch (R)
		{
		case EFarReason::On: return "on";
		case EFarReason::Disabled: return "disabled";
		case EFarReason::Outside: return "outside";
		case EFarReason::Loading: return "cells loading";
		case EFarReason::Jump: return "jump";
		case EFarReason::Moving: return "moving";
		case EFarReason::Settling: return "settling";
		case EFarReason::Warming: return "warming";
		default: return "?";
		}
	}

	/** What the renderer does this frame. */
	struct FFarCacheDecision
	{
		bool bSnapshot = false;          // take the scene snapshot for a new fill (before marching rows)
		int FillFace = -1;               // the face being filled (its spare layer), -1: none
		int RowBegin = 0, RowEnd = 0;    // rows of it to march this frame
		bool bFillDone = false;          // after marching: the spare layer becomes the face's layer
		uint32_t LayerBits = 0;          // bit f: face f shows layer f + 6 (after this frame's swap)
		bool bUse = false;               // this frame composites near + far (the march stops at R)
		int HistSign = 0;                // -1: the history is a whole march, make it near-only; +1: the reverse
		bool bDropHistory = false;       // the history cannot be converted (the map was lost while in use)
		EFarReason Reason = EFarReason::Disabled;
		double Motion = 0.0;             // parallax at R this frame, texels
	};

	class FFarCacheControl
	{
	public:
		bool bWant = false;
		int Still = 0;
		bool bUsePrev = false;
		bool bValid[6] = {};             // the face's shown layer holds a whole march
		double Moved[6][3] = {};         // camera displacement since the shown layer's anchor, cm
		bool bForceDue[6] = {};          // the scene changed since the face was marched
		uint32_t LayerBits = 0;
		int FillFace = -1;
		int FillRow = 0;
		double FillMoved[3] = { 0, 0, 0 };   // since the fill's snapshot
		// Counters (uu.Perf).
		uint64_t FacesFilled = 0, Switches = 0, FramesOn = 0;

		static double Len(const double V[3]) { return std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]); }

		void InvalidateAll()
		{
			for (int F = 0; F < 6; ++F) { bValid[F] = false; bForceDue[F] = false; Moved[F][0] = Moved[F][1] = Moved[F][2] = 0.0; }
			FillFace = -1;
			FillRow = 0;
		}

		/** The next face to fill: invalid faces (in view first), then faces in view past DueTexels
		 *  (the farthest moved first), then faces out of view past OutOfViewTexels. -1: none due. */
		int PickFace(const FFarCacheSettings& S, uint32_t InView) const
		{
			const double Unit = S.TexelRad() * S.StartCm;
			for (int Pass = 0; Pass < 2; ++Pass)
			{
				for (int F = 0; F < 6; ++F)
				{
					const bool bIn = (InView >> F) & 1u;
					if (!bValid[F] && (Pass == 0) == bIn) { return F; }
				}
			}
			int Best = -1;
			double BestM = 0.0;
			for (int F = 0; F < 6; ++F)
			{
				const bool bIn = (InView >> F) & 1u;
				const double M = Len(Moved[F]) / std::max(Unit, 1e-30);
				const double Lim = bIn ? S.DueTexels : S.OutOfViewTexels;
				const double Key = (bForceDue[F] ? 1.0e30 : M) * (bIn ? 1.0 : 1.0e-3);   // in view first
				if ((bForceDue[F] || M > Lim) && (Best < 0 || Key > BestM)) { Best = F; BestM = Key; }
			}
			return Best;
		}

		FFarCacheDecision Update(const FFarCacheSettings& S, const FFarCacheFrame& Fr)
		{
			FFarCacheDecision D;
			const double Unit = S.TexelRad() * S.StartCm;
			D.Motion = Fr.bStepValid && Unit > 0.0 ? Len(Fr.Step) / Unit : 0.0;
			const bool bLost = Fr.bReset || !Fr.bStepValid;
			if (bLost) { InvalidateAll(); }
			if (Fr.bSceneChanged)
			{
				for (int F = 0; F < 6; ++F) { bForceDue[F] = bValid[F]; }
				FillFace = -1;   // its snapshot predates the change
				FillRow = 0;
			}
			if (Fr.bStepValid)
			{
				for (int F = 0; F < 6; ++F) { for (int K = 0; K < 3; ++K) { Moved[F][K] += Fr.Step[K]; } }
				for (int K = 0; K < 3; ++K) { FillMoved[K] += Fr.Step[K]; }
			}

			// Want, with hysteresis.
			if (!S.bEnabled || !(S.StartCm > 0.0)) { bWant = false; Still = 0; D.Reason = EFarReason::Disabled; }
			else if (!Fr.bEligible) { bWant = false; Still = 0; D.Reason = EFarReason::Outside; }
			else if (Fr.bLoading) { bWant = false; Still = 0; D.Reason = EFarReason::Loading; }
			else if (!Fr.bStepValid) { bWant = false; Still = 0; D.Reason = EFarReason::Jump; }
			else if (D.Motion > S.OffMotion) { bWant = false; Still = 0; D.Reason = EFarReason::Moving; }
			else if (D.Motion < S.OnMotion)
			{
				++Still;
				if (Still >= S.OnFrames) { bWant = true; }
				D.Reason = bWant ? EFarReason::On : EFarReason::Settling;
			}
			else { D.Reason = bWant ? EFarReason::On : EFarReason::Moving; }   // between: as it was

			// Fills run while wanted, and while settling (the map warms meanwhile).
			const bool bFill = S.bEnabled && S.StartCm > 0.0 && Fr.bEligible && !Fr.bLoading && Fr.bStepValid && (bWant || Still > 0);
			if (!bFill) { FillFace = -1; FillRow = 0; }
			else
			{
				if (FillFace < 0)
				{
					FillFace = PickFace(S, Fr.FacesInView);
					if (FillFace >= 0)
					{
						FillRow = 0;
						FillMoved[0] = FillMoved[1] = FillMoved[2] = 0.0;
						D.bSnapshot = true;
					}
				}
				if (FillFace >= 0)
				{
					D.FillFace = FillFace;
					D.RowBegin = FillRow;
					D.RowEnd = std::min(S.FaceSize, FillRow + S.RowsPerFrame());
					FillRow = D.RowEnd;
					if (FillRow >= S.FaceSize)
					{
						D.bFillDone = true;
						LayerBits ^= 1u << FillFace;
						bValid[FillFace] = true;
						bForceDue[FillFace] = false;
						for (int K = 0; K < 3; ++K) { Moved[FillFace][K] = FillMoved[K]; }
						++FacesFilled;
						FillFace = -1;
						FillRow = 0;
					}
				}
			}

			// On: every face marched, and (to switch on, not to stay on: no flapping) the faces in view
			// fresh, within DueTexels of their anchors.
			bool bAll = true, bFresh = true;
			for (int F = 0; F < 6; ++F)
			{
				bAll = bAll && bValid[F];
				if ((Fr.FacesInView >> F) & 1u) { bFresh = bFresh && !bForceDue[F] && Len(Moved[F]) <= S.DueTexels * Unit; }
			}
			D.bUse = bWant && bAll && (bUsePrev || bFresh);
			if (bWant && !bAll) { D.Reason = EFarReason::Warming; }
			D.LayerBits = LayerBits;
			if (D.bUse != bUsePrev)
			{
				++Switches;
				// A switch on a jump or a lost map: the history is dropped (a jump drops it anyway).
				if (bLost) { D.bDropHistory = bUsePrev && Fr.bStepValid; }
				else { D.HistSign = D.bUse ? -1 : +1; }
			}
			if (D.bUse) { ++FramesOn; }
			bUsePrev = D.bUse;
			return D;
		}
	};

	/** D-228: the cube faces (Uni_CubeDirToFace's numbering) a view sees: a grid of its pixel
	 *  directions, a little past the edges (bilinear taps at a face's border). */
	inline uint32_t CubeFacesInView(const double InFwd[3], const double InRight[3], const double InUp[3], double InTanX, double InTanY)
	{
		uint32_t Mask = 0u;
		for (int J = 0; J <= 8; ++J)
		{
			for (int I = 0; I <= 16; ++I)
			{
				const double A = (double(I) / 8.0 - 1.0) * InTanX * 1.05, B = (double(J) / 4.0 - 1.0) * InTanY * 1.05;
				const double D[3] = { InFwd[0] + InRight[0] * A + InUp[0] * B, InFwd[1] + InRight[1] * A + InUp[1] * B, InFwd[2] + InRight[2] * A + InUp[2] * B };
				const double Ax = std::abs(D[0]), Ay = std::abs(D[1]), Az = std::abs(D[2]);
				uint32_t Face;
				if (Ax >= Ay && Ax >= Az) { Face = D[0] >= 0.0 ? 0u : 1u; }
				else if (Ay >= Az) { Face = D[1] >= 0.0 ? 2u : 3u; }
				else { Face = D[2] >= 0.0 ? 4u : 5u; }
				Mask |= 1u << Face;
			}
		}
		return Mask;
	}

	/** D-228: what the far map's light depends on besides the camera's position: the local carriers
	 *  (which, how bright) and their emission LUT blocks and parameter blocks. Compared frame to
	 *  frame; a change of more than 1% re-marches every face (in view first). */
	struct FFarSceneKey
	{
		std::vector<uint32_t> Ids;       // per local carrier: field, flags, LUT block, parameter blocks
		std::vector<float> Weights;      // per local carrier
		std::vector<float> Lut;          // their emission blocks, rgb
		std::vector<uint32_t> Params;    // their parameter blocks, raw

		static FFarSceneKey Build(const FCarrierGPU* InCarriers, size_t InCount, const float* InParams, size_t InParamFloats, const float* InLut, size_t InLutFloats)
		{
			FFarSceneKey K;
			constexpr size_t ParamFloats = sizeof(FFieldParamsGPU) / sizeof(float);
			constexpr size_t BlockFloats = size_t(HV::UNI_VOL_LUT_SIZE) * 4u;
			for (size_t I = 0; I < InCount; ++I)
			{
				const FCarrierGPU& C = InCarriers[I];
				if ((C.Flags & HV::UNI_CARRIER_PERIODIC) != 0u) { continue; }
				K.Ids.push_back(C.FieldId); K.Ids.push_back(C.Flags); K.Ids.push_back(C.LutIndex); K.Ids.push_back(C.ParamIndex);
				K.Ids.push_back(C.DustParamIndex); K.Ids.push_back(C.PaletteIndex);
				K.Weights.push_back(C.Weight);
				const size_t L0 = size_t(C.LutIndex) * BlockFloats;
				for (size_t E = 0; E < size_t(HV::UNI_VOL_LUT_SIZE); ++E)
				{
					for (size_t Ch = 0; Ch < 3; ++Ch) { const size_t Q = L0 + E * 4u + Ch; K.Lut.push_back(Q < InLutFloats ? InLut[Q] : 0.0f); }
				}
				const size_t P0 = size_t(C.ParamIndex) * ParamFloats;
				for (size_t E = 0; E < ParamFloats; ++E)
				{
					uint32_t U = 0u;
					if (P0 + E < InParamFloats) { std::memcpy(&U, &InParams[P0 + E], sizeof(U)); }
					K.Params.push_back(U);
				}
			}
			return K;
		}

		/** True when B differs from A by more than InTol (relative; the LUT against its block's peak). */
		static bool Changed(const FFarSceneKey& A, const FFarSceneKey& B, double InTol = 0.01)
		{
			if (A.Ids != B.Ids || A.Params != B.Params || A.Weights.size() != B.Weights.size() || A.Lut.size() != B.Lut.size()) { return true; }
			for (size_t I = 0; I < A.Weights.size(); ++I)
			{
				if (std::abs(double(A.Weights[I]) - double(B.Weights[I])) > InTol * std::max(std::abs(double(A.Weights[I])), 1e-30)) { return true; }
			}
			const size_t Block = size_t(HV::UNI_VOL_LUT_SIZE) * 3u;
			for (size_t B0 = 0; B0 < A.Lut.size(); B0 += Block)
			{
				double Peak = 0.0;
				for (size_t I = B0; I < std::min(B0 + Block, A.Lut.size()); ++I) { Peak = std::max(Peak, std::abs(double(A.Lut[I]))); }
				for (size_t I = B0; I < std::min(B0 + Block, A.Lut.size()); ++I)
				{
					if (std::abs(double(A.Lut[I]) - double(B.Lut[I])) > InTol * std::max(Peak, 1e-30)) { return true; }
				}
			}
			return false;
		}
	};
}
