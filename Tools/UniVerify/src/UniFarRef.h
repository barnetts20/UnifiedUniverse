#pragma once

// UniFarRef.h
// D-228: the far cache as the renderer runs it, on the CPU: UniCore::FFarCacheControl decides,
// the far map's faces are marched row by row into their spare layers from a snapshot of the scene
// (the local carriers' TStart raised to R, no cell glow: the renderer's FarCS), the march stops at
// R (TEnd) while the cache is used, the fresh slices take the map's transmittance (MarchCS), the
// history is converted at a switch (ReconstructCS), and the volume is composited near + T_near far
// (CompositePS; at march resolution here).

#include "UniRenderRef.h"
#include "UniVolumeRef.h"
#include "UniFarCache.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace UniRender
{
	namespace HF = UniHlsl;

	/** The scene with the local carriers clipped: the near part (TEnd at R) or the far (TStart at R,
	 *  no cell glow: kept across frames, as the background map is). */
	inline UniCore::FVolumeScene FarSplit(const UniCore::FVolumeScene& Sc, double R, bool bFar)
	{
		UniCore::FVolumeScene O = Sc;
		for (UniCore::FCarrierGPU& C : O.Carriers)
		{
			if ((C.Flags & HF::UNI_CARRIER_PERIODIC) != 0u) { continue; }
			if (bFar) { C.TStart = std::max(C.TStart, float(R)); }
			else { C.TEnd = std::min(C.TEnd, float(R)); }
		}
		if (bFar)
		{
			O.View.GlowLevels = 0u;
			for (int K = 0; K < 4; ++K) { O.View.GlowMask[K] = 0u; }
		}
		return O;
	}

	/** The far map: 12 layers (face f shows layer f or f + 6: LayerBits), (rgb, luminance T) and
	 *  luminance T at the view's slice boundaries. */
	struct FFarMap
	{
		int N = 0;
		u32 LayerBits = 0;
		std::vector<HF::float4> Tex;    // 12 * N * N
		std::vector<float> Sl;          // 12 * N * N * UNI_VOL_SLICES
		u64 Steps = 0;
		u64 Rows = 0;
		int MaxRowsInFrame = 0;

		void Reset(int InN)
		{
			N = InN;
			LayerBits = 0;
			Tex.assign(size_t(12) * N * N, HF::float4(0, 0, 0, 1));
			Sl.assign(size_t(12) * N * N * HF::UNI_VOL_SLICES, 1.0f);
		}
		int Shown(int Face) const { return Face + (((LayerBits >> Face) & 1u) ? 6 : 0); }
		int Spare(int Face) const { return Face + (((LayerBits >> Face) & 1u) ? 0 : 6); }
		size_t Idx(int Layer, int X, int Y) const { X = std::clamp(X, 0, N - 1); Y = std::clamp(Y, 0, N - 1); return (size_t(Layer) * N + Y) * N + X; }

		/** Bilinear within the shown layer of the direction's face (the renderer's LinearClamp). */
		template <class TFn> auto Bilinear(const FV3& D, TFn&& At) const
		{
			float X, Y;
			const int F = int(HF::Uni_CubeDirToFace(HF::float3(float(D.X), float(D.Y), float(D.Z)), float(N), X, Y));
			const int L = Shown(F);
			const float Fx = X - 0.5f, Fy = Y - 0.5f;
			const int X0 = int(std::floor(Fx)), Y0 = int(std::floor(Fy));
			const float Tx = Fx - float(X0), Ty = Fy - float(Y0);
			const auto A = At(Idx(L, X0, Y0)) * (1 - Tx) + At(Idx(L, X0 + 1, Y0)) * Tx;
			const auto B = At(Idx(L, X0, Y0 + 1)) * (1 - Tx) + At(Idx(L, X0 + 1, Y0 + 1)) * Tx;
			return A * (1 - Ty) + B * Ty;
		}
		HF::float4 Sample(const FV3& D) const { return Bilinear(D, [&](size_t I) { return Tex[I]; }); }
		float SliceAt(const FV3& D, u32 K) const { return Bilinear(D, [&](size_t I) { return Sl[I * HF::UNI_VOL_SLICES + K]; }); }

		/** Rows [R0, R1) of a face into its spare layer, from the far scene (the snapshot). */
		void MarchRows(const UniCore::FVolumeScene& Far, float StartCm, int Face, int R0, int R1, int Threads = 2)
		{
			HF::FUniVolumeView V = UniCore::ToHlsl(Far.View);
			V.NebulaThetaPixel = V.ThetaPixel;   // D-235: as FarCS
			V.ThetaPixel = 2.0f / float(N);
			const auto Cb = Far.CarrierBuf(); const auto Pb = Far.ParamBuf(); const auto Lb = Far.LutBuf();
			const int L = Spare(Face);
			std::vector<u64> RowSteps(size_t(std::max(R1 - R0, 0)), 0);
			auto Work = [&](int T0)
			{
				for (int Y = R0 + T0; Y < R1; Y += Threads)
				{
					for (int X = 0; X < N; ++X)
					{
						const HF::FUniVolumeResult R = HF::Uni_VolumeMarch(HF::Uni_CubeTexelDir(u32(Face), X + 0.5f, Y + 0.5f, float(N)), 3.0e38f, Cb, Pb, Lb, V);
						const size_t I = Idx(L, X, Y);
						Tex[I] = HF::float4(R.Radiance.x, R.Radiance.y, R.Radiance.z, HF::Uni_Luminance(R.Transmittance));
						for (u32 K = 0; K < HF::UNI_VOL_SLICES; ++K) { Sl[I * HF::UNI_VOL_SLICES + K] = HF::Uni_FarSliceT(K, HF::Uni_Luminance(R.SliceT[K]), StartCm, V); }
						RowSteps[size_t(Y - R0)] += R.Steps;
					}
				}
			};
			std::vector<std::thread> Ts;
			for (int T = 1; T < Threads; ++T) { Ts.emplace_back(Work, T); }
			Work(0);
			for (std::thread& T : Ts) { T.join(); }
			for (u64 S : RowSteps) { Steps += S; }
			Rows += u64(std::max(R1 - R0, 0));
		}
	};

	/** One run of the far cache with today's slicing underneath: Step per frame, then Out (the
	 *  composited volume) and OutSlices (each pixel's slices, the whole path). */
	struct FFarSim
	{
		UniCore::FFarCacheSettings S;
		UniCore::FFarCacheControl Ctl;
		FFarMap Map;
		FTemporalVolume Tv;
		UniCore::FVolumeScene Snap;     // the fill's snapshot (far scene)
		bool bHaveSnap = false;
		UniCore::FFarSceneKey LastKey;
		bool bHaveKey = false;
		std::vector<HF::float4> Out;
		UniCore::FFarCacheDecision Last;
		u64 StepsLast = 0;              // this frame's march + fill steps
		int SceneChanges = 0;           // frames FFarSceneKey saw a change

		/** Sc: this frame's local scene. InView: CubeFacesInView of the camera (0: compute). */
		void Step(const UniCore::FVolumeScene& Sc, const FCamera& Cam, const FV3& StepCm, bool bStepValid, int Threads = 2)
		{
			const bool bReset = Map.N != S.FaceSize;   // the map (re)created: nothing in it
			if (bReset) { Map.Reset(S.FaceSize); }
			UniCore::FFarCacheFrame Fr;
			Fr.bEligible = UniCore::CameraInsideLocalCarrier(Sc.Carriers.data(), Sc.Carriers.size());
			Fr.bLoading = Sc.View.GlowLevels != 0u;
			Fr.bStepValid = bStepValid;
			Fr.Step[0] = StepCm.X; Fr.Step[1] = StepCm.Y; Fr.Step[2] = StepCm.Z;
			{
				const double Fw[3] = { Cam.Forward.X, Cam.Forward.Y, Cam.Forward.Z }, Rt[3] = { Cam.Right.X, Cam.Right.Y, Cam.Right.Z }, Up[3] = { Cam.Up.X, Cam.Up.Y, Cam.Up.Z };
				Fr.FacesInView = UniCore::CubeFacesInView(Fw, Rt, Up, Cam.TanX(), Cam.TanY());
			}
			{
				const UniCore::FFarSceneKey Key = UniCore::FFarSceneKey::Build(Sc.Carriers.data(), Sc.Carriers.size(),
					reinterpret_cast<const float*>(Sc.Params.data()), Sc.Params.size() * (sizeof(Sc.Params[0]) / sizeof(float)),
					reinterpret_cast<const float*>(Sc.Lut.data()), Sc.Lut.size() * 4u);
				Fr.bSceneChanged = bHaveKey && UniCore::FFarSceneKey::Changed(LastKey, Key);
				SceneChanges += Fr.bSceneChanged ? 1 : 0;
				LastKey = Key;
				bHaveKey = true;
			}
			Fr.bReset = bReset;
			const UniCore::FFarCacheDecision D = Ctl.Update(S, Fr);
			Last = D;
			const u64 M0 = Map.Steps;
			if (D.bSnapshot) { Snap = FarSplit(Sc, S.StartCm, true); bHaveSnap = true; }
			if (D.FillFace >= 0)
			{
				Map.MarchRows(Snap, float(S.StartCm), D.FillFace, D.RowBegin, D.RowEnd, Threads);
				Map.MaxRowsInFrame = std::max(Map.MaxRowsInFrame, D.RowEnd - D.RowBegin);
			}
			Map.LayerBits = D.LayerBits;
			// The march: the near part while the cache is used, the whole otherwise.
			const UniCore::FVolumeScene Near = D.bUse ? FarSplit(Sc, S.StartCm, false) : Sc;
			if (D.bUse) { Tv.FarSlice = [this](const FV3& Dd, u32 K) { return Map.SliceAt(Dd, K); }; }
			else { Tv.FarSlice = nullptr; }
			Tv.HistFarSign = D.HistSign;
			Tv.HistFar = [this](const FV3& Dd) { return Map.Sample(Dd); };
			if (D.bDropHistory) { Tv.bHave = false; }
			const size_t T0 = Tv.Steps;
			Tv.Step(Near, Cam, StepCm, bStepValid, true, Threads);
			StepsLast = u64(Tv.Steps - T0) + (Map.Steps - M0);
			Out = Tv.Vol;
			if (D.bUse)
			{
				for (int Y = 0; Y < Cam.Height; ++Y) for (int X = 0; X < Cam.Width; ++X)
				{
					HF::float4& O = Out[size_t(Y) * Cam.Width + X];
					O = HF::Uni_FarComposite(O, Map.Sample(PixelDir(Cam, Cam.Width, Cam.Height, X + 0.5, Y + 0.5)));
				}
			}
		}
	};
}
