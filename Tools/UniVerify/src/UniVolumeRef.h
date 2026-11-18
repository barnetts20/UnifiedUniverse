#pragma once

// UniVolumeRef.h
// CPU reference for the volume pass (Docs/VOLUMETRICS.md): marches every pixel through the
// shared core (Uni_VolumeMarch), deposits radiance x the pixel's exact solid angle, and
// composites sprites attenuated by the transmittance slices, as the GPU pass will.
// Verification only. Multithreaded over rows.

#include "UniRenderRef.h"
#include "UniVolume.h"

#include <atomic>
#include <functional>
#include <thread>
#include <unordered_map>

namespace UniRender
{
	struct FVolumeStats
	{
		size_t Rays = 0, Truncated = 0, RaysWithCarrier = 0, Supersampled = 0;
		u64 Steps = 0;
		u32 MaxSteps = 0;
		double Flux[3] = { 0, 0, 0 };     // sum of deposited volume energy
		double SpriteFlux[3] = { 0, 0, 0 };
	};

	/** Exact solid angle of pixel (X, Y): cos^3(off-axis angle) / f^2 for a pinhole camera. */
	inline double PixelSolidAngle(const FCamera& Cam, int Wd, int Ht, double X, double Y)
	{
		const double F = double(Wd) * 0.5 / Cam.TanX();
		const double Nx = (X / Wd - 0.5) * 2.0 * Cam.TanX(), Ny = (0.5 - Y / Ht) * 2.0 * Cam.TanY();
		const double C = 1.0 / std::sqrt(1.0 + Nx * Nx + Ny * Ny);
		return C * C * C / (F * F);
	}

	inline FV3 PixelDir(const FCamera& Cam, int Wd, int Ht, double X, double Y)
	{
		const double Nx = (X / Wd - 0.5) * 2.0 * Cam.TanX(), Ny = (0.5 - Y / Ht) * 2.0 * Cam.TanY();
		return Norm(Cam.Forward + Cam.Right * Nx + Cam.Up * Ny);
	}

	/** The background cube (D-136): every texel of six N x N faces marched through the periodic
	 *  carriers only, with the texel's angular size as the prefilter footprint. */
	struct FBackgroundCube
	{
		int N = 0;
		float StartCm = 0.0f;   // the background's start: nearer scene surfaces block it
		std::vector<H::float3> Tex;   // 6 * N * N
		u64 Steps = 0;
		size_t Truncated = 0;

		H::float3 At(int F, int X, int Y) const { X = std::clamp(X, 0, N - 1); Y = std::clamp(Y, 0, N - 1); return Tex[(size_t(F) * N + Y) * N + X]; }
		/** Bilinear within the face (seams clamp: the background is low-frequency). */
		H::float3 Sample(const H::float3& D) const
		{
			float X, Y;
			const int F = int(H::Uni_CubeDirToFace(D, float(N), X, Y));
			const float Fx = X - 0.5f, Fy = Y - 0.5f;
			const int X0 = int(std::floor(Fx)), Y0 = int(std::floor(Fy));
			const float Tx = Fx - float(X0), Ty = Fy - float(Y0);
			const H::float3 A = At(F, X0, Y0) * (1 - Tx) + At(F, X0 + 1, Y0) * Tx;
			const H::float3 B = At(F, X0, Y0 + 1) * (1 - Tx) + At(F, X0 + 1, Y0 + 1) * Tx;
			return A * (1 - Ty) + B * Ty;
		}
	};

	inline FBackgroundCube RenderBackgroundCube(const FVolumeScene& Bg, int N)
	{
		FBackgroundCube C;
		C.N = N;
		C.Tex.assign(size_t(6) * N * N, H::float3(0, 0, 0));
		C.StartCm = 3.0e38f;
		for (const FCarrierGPU& G : Bg.Carriers) { C.StartCm = std::min(C.StartCm, G.TStart); }
		H::FUniVolumeView V = ToHlsl(Bg.View);
		V.NebulaThetaPixel = V.ThetaPixel;   // D-235: as BackgroundCS
		V.ThetaPixel = 2.0f / float(N);
		const auto Cb = Bg.CarrierBuf(); const auto Pb = Bg.ParamBuf(); const auto Lb = Bg.LutBuf();
		for (int F = 0; F < 6; ++F) for (int Y = 0; Y < N; ++Y) for (int X = 0; X < N; ++X)
		{
			const H::FUniVolumeResult R = H::Uni_VolumeMarch(H::Uni_CubeTexelDir(u32(F), X + 0.5f, Y + 0.5f, float(N)), 3.0e38f, Cb, Pb, Lb, V);
			C.Tex[(size_t(F) * N + Y) * N + X] = R.Radiance;
			C.Steps += R.Steps; C.Truncated += R.Truncated;
		}
		return C;
	}

	/** One march-grid pixel, as the GPU march pass does it: supersampled where a small
	 *  carrier's finest feature is under half a pixel (D-130), the light's depth averaged by
	 *  the sub-rays' light (D-184). D-247: Nx, Ny the noise coordinate (-1: the pixel's). */
	template <class TC, class TP, class TL>
	inline H::FUniVolumeResult MarchPixel(const TC& Cb, const TP& Pb, const TL& Lb, const H::FUniVolumeView& V, const FCamera& Cam, int Wd, int Ht, int X, int Y, float TScene, bool& bOutSuper, int Nx = -1, int Ny = -1)
	{
		if (Nx < 0 || Ny < 0) { Nx = X; Ny = Y; }
		bOutSuper = false;
		const FV3 D = PixelDir(Cam, Wd, Ht, X + 0.5, Y + 0.5);
		const H::float3 Dc(float(D.X), float(D.Y), float(D.Z));
		// Supersampling where a small carrier's finest feature is under half a pixel.
		u32 SS = 1u;
		for (u32 C = 0; C < V.NumCarriers; ++C)
		{
			const H::FUniCarrier Ca = Cb[C];
			float T0, T1;
			const H::float3 Ld = H::Uni_CarrierLocalDir(Ca, Dc);
			if (Ca.Weight > 0.0f && H::Uni_CarrierInterval(Ca, Ld, TScene, T0, T1)) { SS = std::max(SS, H::Uni_CarrierSubsamplesRay(Ca, V.ThetaPixel)); }   // D-160, D-163
		}
		SS = H::Uni_ClampSubsamples(SS, V);   // D-157
		H::FUniVolumeResult R;
		if (SS == 1u)
		{
			R = H::Uni_VolumeMarchJ(Dc, TScene, Cb, Pb, Lb, V, H::float3(float(Nx), float(Ny), 1.0f));   // D-240: jittered with MarchFlags bit 3
		}
		else
		{
			H::float3 Acc(0, 0, 0), AccT(0, 0, 0);
			H::float3 AccS[H::UNI_VOL_SLICES];
			u32 Steps = 0, Tr = 0, Act = 0;
			float Dw = 0.0f, Dwl = 0.0f;
			for (u32 J = 0; J < SS; ++J) for (u32 I = 0; I < SS; ++I)
			{
				const FV3 Ds = PixelDir(Cam, Wd, Ht, X + (I + 0.5) / SS, Y + (J + 0.5) / SS);
				const H::FUniVolumeResult Rs = H::Uni_VolumeMarchJ(H::float3(float(Ds.X), float(Ds.Y), float(Ds.Z)), TScene, Cb, Pb, Lb, V, H::float3(float(Nx * int(SS) + int(I)), float(Ny * int(SS) + int(J)), 1.0f));   // D-240
				Acc += Rs.Radiance; AccT += Rs.Transmittance; Steps += Rs.Steps; Tr += Rs.Truncated; Act = std::max(Act, Rs.Active);
				Dw += Rs.DepthWeight; if (Rs.LnDepth < H::UNI_VOL_LN_DEPTH_NONE - 1.0f) { Dwl += Rs.DepthWeight * H::Uni_LightInvDepth(Rs.LnDepth); }   // D-194: harmonic
				for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { AccS[K] = (I == 0 && J == 0) ? Rs.SliceT[K] : AccS[K] + Rs.SliceT[K]; }
			}
			const float Inv = 1.0f / float(SS * SS);
			R.Radiance = Acc * Inv; R.Transmittance = AccT * Inv; R.Steps = Steps; R.Truncated = Tr ? 1u : 0u; R.Active = Act; R.Pad = 0u;
			for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { R.SliceT[K] = AccS[K] * Inv; }
			R.DepthWeight = Dw * Inv;
			R.LnDepth = Dw > 0.0f && Dwl > 0.0f ? H::Uni_LightLnDepthFromInv(Dwl / Dw) : H::UNI_VOL_LN_DEPTH_NONE;
			bOutSuper = true;
		}
		return R;
	}

	/** March every pixel; adds B * Omega to Img and returns per-pixel slices if asked. With a
	 *  cube, Sc holds the local carriers only and the background comes from the cube. */
	inline FVolumeStats RenderVolume(const FVolumeScene& Sc, const FCamera& Cam, FImage& Img, std::vector<H::float3>* SlicesOut = nullptr, int Threads = 2, const FBackgroundCube* Cube = nullptr)
	{
		FVolumeStats St;
		const H::FUniVolumeView V = ToHlsl(Sc.View);
		const auto Cb = Sc.CarrierBuf();
		const auto Pb = Sc.ParamBuf();
		const auto Lb = Sc.LutBuf();
		if (SlicesOut) { SlicesOut->assign(size_t(Img.W) * Img.H * H::UNI_VOL_SLICES, H::float3(1, 1, 1)); }
		std::atomic<int> NextRow{ 0 };
		std::vector<FVolumeStats> Per(static_cast<size_t>(Threads));
		auto Work = [&](int Tid)
		{
			FVolumeStats& S = Per[size_t(Tid)];
			for (int Y = NextRow++; Y < Img.H; Y = NextRow++)
			{
				for (int X = 0; X < Img.W; ++X)
				{
					const FV3 D = PixelDir(Cam, Img.W, Img.H, X + 0.5, Y + 0.5);
					const double Scene = Img.SceneDist[size_t(Y) * Img.W + X];
					const float TScene = std::isfinite(Scene) ? H::Uni_SceneTrueDistance(float(Scene)) : 3.0e38f;   // render space -> true (D-146)
					const H::float3 Dc(float(D.X), float(D.Y), float(D.Z));
					bool bSuper = false;
					H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cam, Img.W, Img.H, X, Y, TScene, bSuper);
					if (bSuper) { ++S.Supersampled; }
					if (Cube && TScene > Cube->StartCm) { R.Radiance = R.Radiance + R.Transmittance * Cube->Sample(Dc); }
					// D-230: the travel shift, as MarchCS and the composite apply it (DopplerOf).
					R.Radiance = H::Uni_TravelShift(R.Radiance, Dc, H::float3(float(Cam.ShiftVel.X), float(Cam.ShiftVel.Y), float(Cam.ShiftVel.Z)), float(Cam.ShiftAmount), float(Cam.ShiftK), float(Cam.ShiftBoostFrom), float(Cam.ShiftBoost));
					const double Om = PixelSolidAngle(Cam, Img.W, Img.H, X + 0.5, Y + 0.5);
					float* P = &Img.Rgb[(size_t(Y) * Img.W + X) * 3];
					P[0] += float(R.Radiance.x * Om); P[1] += float(R.Radiance.y * Om); P[2] += float(R.Radiance.z * Om);
					S.Flux[0] += R.Radiance.x * Om; S.Flux[1] += R.Radiance.y * Om; S.Flux[2] += R.Radiance.z * Om;
					++S.Rays;
					if (R.Active) { ++S.RaysWithCarrier; }
					S.Steps += R.Steps;
					S.MaxSteps = std::max(S.MaxSteps, R.Steps);
					S.Truncated += R.Truncated;
					if (SlicesOut) { for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { (*SlicesOut)[(size_t(Y) * Img.W + X) * H::UNI_VOL_SLICES + K] = R.SliceT[K]; } }
				}
			}
		};
		std::vector<std::thread> Ts;
		for (int T = 1; T < Threads; ++T) { Ts.emplace_back(Work, T); }
		Work(0);
		for (std::thread& T : Ts) { T.join(); }
		for (const FVolumeStats& S : Per)
		{
			St.Rays += S.Rays; St.Truncated += S.Truncated; St.RaysWithCarrier += S.RaysWithCarrier; St.Steps += S.Steps; St.Supersampled += S.Supersampled;
			St.MaxSteps = std::max(St.MaxSteps, S.MaxSteps);
			for (int K = 0; K < 3; ++K) { St.Flux[K] += S.Flux[K]; }
		}
		return St;
	}

	/** D-184: the GPU's temporally sliced march (UniVolume.usf, passes 0 and 3) on the CPU. Per
	 *  frame it marches the phase's fresh pixel of every 2 x 2 block and reconstructs the others
	 *  from the previous frame's output through the shared core's rules, then keeps the output
	 *  as the next frame's history. bTemporal false, or no usable history, marches every pixel
	 *  (and still keeps history).
	 *  The march grid has no scene surfaces here (sky everywhere), as in the volume checks. */
	struct FTemporalVolume
	{
		int W = 0, Ht = 0;
		int HW = 0, HH = 0;              // D-215: the history's size (the last frame's; may differ from this frame's)
		/** D-215: a new size (the volume's auto budget) keeps the history, reprojected at its own
		 *  size, when within 2x either way; false: every pixel marched (pre-D-215). */
		bool bKeepAcrossResize = true;
		size_t Resizes = 0, ResizesKept = 0;
		std::vector<H::float4> Vol;      // radiance rgb (exposure units per sr), luminance transmittance a
		std::vector<float> Slices;       // luminance transmittance at the slice boundaries
		std::vector<H::float2> Depth;    // ln depth of the light, ln scene distance
		std::vector<float> Age;          // D-240: samples each pixel's history holds
		// D-240: accumulation (Uni_TemporalAlpha): a fresh pixel blends into its clamped history
		// over this many samples; 1: it replaces it (D-184). The jitter shift follows the pixel's
		// sample count (the scene's MarchFlags bit 3 jitters the march).
		float MaxSamples = 1.0f;
		size_t Accumulated = 0, Restarted = 0;
		// D-246: CloudAtmosphere's resolve (Uni_Resolve*): every pixel blends the bilinear fill of the
		// 4 x 4 fresh samples about it into its history, clamped to their moments (mean +- gamma
		// sigma, compressed at Exposure: 0 takes 1 / the fresh samples' mean luminance); two ages.
		bool bCloud = false;
		float Exposure = 0.0f;
		bool bCellNoise = true;          // D-247: the sliced march's noise coordinate per cell (false: D-246's per pixel)
		std::vector<float> AgeOwn;       // D-246: the pixel's own samples in its history
		std::vector<H::float2> Mom;      // D-251: its own samples' compressed luminance moments (E[l], E[l^2])
		std::vector<float> Var;          // D-251: this frame's variance of each pixel's value (compressed luminance; cloud resolve)
		float LastExp = 1.0f;            // D-251: the compression's exposure this frame
		bool bHave = false;
		FCamera Prev;
		u32 Frame = 0;
		size_t Marched = 0, FromHistory = 0, FromFresh = 0, Steps = 0;
		// D-194: the history clamped into the fresh neighbours' range (tight from ClampTightPx of
		// motion), not used beyond MaxMotionPx. bClamp false: the D-184 luminance band.
		bool bClamp = true;
		float ClampTightPx = 2.0f;
		float MaxMotionPx = 8.0f;
		size_t TooFast = 0;
		// D-208: the slicing's block (2: D-184's 2 x 2); set before each Step to vary it.
		u32 Interleave = 2;
		// D-228: the far cache (UniFarRef.h sets these per frame, as the renderer's passes do).
		// FarSlice: the far map's transmittance at slice K in a direction, multiplied into each
		// fresh pixel's slices (MarchCS). HistFarSign / HistFar: the history converted once at a
		// switch (ReconstructCS: Uni_FarHistoryConvert with the far map's texel at the pixel).
		std::function<float(const FV3&, u32)> FarSlice;
		int HistFarSign = 0;
		std::function<H::float4(const FV3&)> HistFar;

		static float Lum(const H::float4& V) { return H::Uni_Luminance(H::float3(V.x, V.y, V.z)); }

		void Step(const FVolumeScene& Sc, const FCamera& Cam, const FV3& CamStepCm, bool bStepValid, bool bTemporal, int Threads = 2)
		{
			if (Cam.Width != W || Cam.Height != Ht)
			{
				const bool bNear = bHave && W > 0 && Ht > 0 && Cam.Width * 2 >= W && Cam.Width <= 2 * W && Cam.Height * 2 >= Ht && Cam.Height <= 2 * Ht;
				if (bHave) { ++Resizes; }
				if (bKeepAcrossResize && bNear) { ++ResizesKept; } else { bHave = false; }
				W = Cam.Width; Ht = Cam.Height;
			}
			if (!bHave) { HW = W; HH = Ht; }
			const size_t N = size_t(W) * Ht;
			const u32 Phase = H::Uni_TemporalPack(Frame, Interleave);   // D-208 (2: Frame & 3, as before)
			H::FUniTemporalView TV;
			TV.PrevForward = H::float3(float(Prev.Forward.X), float(Prev.Forward.Y), float(Prev.Forward.Z));
			TV.PrevRight = H::float3(float(Prev.Right.X), float(Prev.Right.Y), float(Prev.Right.Z));
			TV.PrevUp = H::float3(float(Prev.Up.X), float(Prev.Up.Y), float(Prev.Up.Z));
			TV.PrevTanX = float(Prev.TanX()); TV.PrevTanY = float(Prev.TanY());
			TV.Phase = Phase;
			TV.CamStep = H::float3(float(CamStepCm.X), float(CamStepCm.Y), float(CamStepCm.Z));
			TV.bHistory = (bHave && bStepValid && bTemporal) ? 1u : 0u;
			TV.PrevVel = H::float3(0, 0, 0); TV.PrevBeta = 0.0f;   // D-190: no warp optics here
			// Without usable history (first frame, a jump, a new size) every pixel is marched, as the
			// renderer does.
			const bool bSlice = bTemporal && TV.bHistory != 0u;
			H::FUniVolumeView V = ToHlsl(Sc.View);
			// D-240: the jitter shift moves by the golden ratio with each sample a pixel takes (one
			// per N x N frames while sliced, one a frame otherwise).
			{
				const u32 Nn = H::Uni_TemporalN(Phase);
				const u32 K = bSlice ? Frame / (Nn * Nn) : Frame;
				V.JitterShift = float(std::fmod(double(K) * 0.6180339887498949, 1.0));
			}
			const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
			const float TScene = 3.0e38f, LnTp = std::log(TScene);
			// Pass 0: the fresh pixels (all of them without slicing).
			std::vector<H::float4> FVol(N, H::float4(0, 0, 0, 1));
			std::vector<float> FSl(N * H::UNI_VOL_SLICES, 1.0f);
			std::vector<H::float2> FDep(N, H::float2(H::UNI_VOL_LN_DEPTH_NONE, LnTp));
			std::atomic<int> NextRow{ 0 };
			std::atomic<size_t> MarchedA{ 0 }, StepsA{ 0 };
			auto Work = [&]()
			{
				for (int Y = NextRow++; Y < Ht; Y = NextRow++)
				{
					for (int X = 0; X < W; ++X)
					{
						if (bSlice && !H::Uni_TemporalFresh(H::uint2(u32(X), u32(Y)), Phase)) { continue; }
						bool bSuper = false;
						// D-247: sliced with the tile bound, the cell's noise coordinate (as MarchCS).
						int Nx = -1, Ny = -1;
						if (bSlice && H::GBlueNoise && H::GBlueNoiseW > 0u && bCellNoise)
						{
							const u32 Nn = H::Uni_TemporalN(Phase);
							const H::uint2 Nc = H::Uni_VolumeNoiseCell(H::uint2(u32(X) / Nn, u32(Y) / Nn), H::Uni_TemporalRank(Phase));
							Nx = int(Nc.x); Ny = int(Nc.y);
						}
						const H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cam, W, Ht, X, Y, TScene, bSuper, Nx, Ny);
						const size_t I = size_t(Y) * W + X;
						FVol[I] = H::float4(R.Radiance.x, R.Radiance.y, R.Radiance.z, H::Uni_Luminance(R.Transmittance));
						for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { FSl[I * H::UNI_VOL_SLICES + K] = H::Uni_Luminance(R.SliceT[K]); }
						if (FarSlice)
						{
							const FV3 Dd = PixelDir(Cam, W, Ht, X + 0.5, Y + 0.5);
							for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { FSl[I * H::UNI_VOL_SLICES + K] *= FarSlice(Dd, K); }
						}
						FDep[I] = H::float2(R.LnDepth, LnTp);
						++MarchedA; StepsA += R.Steps;
					}
				}
			};
			std::vector<std::thread> Ts;
			for (int T = 1; T < Threads; ++T) { Ts.emplace_back(Work); }
			Work();
			for (std::thread& T : Ts) { T.join(); }
			Marched += MarchedA; Steps += StepsA;
			// Pass 3: reconstruct the rest; D-240: and blend each fresh pixel into its history.
			std::vector<H::float4> OVol = FVol;
			std::vector<float> OSl = FSl;
			std::vector<H::float2> ODep = FDep;
			std::vector<float> OAge(N, 1.0f);   // D-240: a marched pixel without history holds its one sample
			std::vector<float> OAgeOwn(N, 1.0f);   // D-246
			std::vector<H::float2> OMom(N, H::float2(0.0f, 0.0f));   // D-251
			std::vector<float> OVar(N, 0.0f);
			if (bSlice)
			{
				const H::uint2 Size = H::uint2(u32(W), u32(Ht));
				const H::float2 SizeF = H::float2(float(W), float(Ht));
				const H::uint2 HSize = H::uint2(u32(HW), u32(HH));   // D-215
				const H::float2 HSizeF = H::float2(float(HW), float(HH));
				const bool bAccumulate = MaxSamples > 1.0f;
				// The history reprojected to pixel (X, Y) at ln depth LnD (bilinear, Catmull-Rom where
				// all 16 texels are on this pixel's surface; D-228's conversion). False: none.
				float HOwnLast = 0.0f;   // D-246: the pixel's own samples, with the last Reproject's taps
				H::float2 HMomLast(0.0f, 0.0f);   // D-251: its moments, bilinear with the same taps
				auto Reproject = [&](int X, int Y, float LnD, H::float4& Hv, float* Hsl, float& HDep, float& HAge, float& Mo) -> bool
				{
					Hv = H::float4(0, 0, 0, 0); HDep = 0.0f; HAge = 0.0f; Mo = 0.0f; HOwnLast = 0.0f; HMomLast = H::float2(0.0f, 0.0f);
					for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { Hsl[S] = 0.0f; }
					if (TV.bHistory == 0u) { return false; }
					const FV3 Dd = PixelDir(Cam, W, Ht, X + 0.5, Y + 0.5);
					H::float2 Pp(0, 0);
					if (!H::Uni_TemporalPrevPixel(H::float3(float(Dd.X), float(Dd.Y), float(Dd.Z)), LnD, TV, HSizeF, Pp)) { return false; }
					const H::FUniTemporalTaps Ht4 = H::Uni_TemporalHistTaps(Pp, HSize);
					const size_t Qh[4] = { size_t(Ht4.Q0.y) * HW + Ht4.Q0.x, size_t(Ht4.Q1.y) * HW + Ht4.Q1.x, size_t(Ht4.Q2.y) * HW + Ht4.Q2.x, size_t(Ht4.Q3.y) * HW + Ht4.Q3.x };
					const H::float4 Wh = H::Uni_TemporalHistW(Ht4.W, LnTp, H::float4(Depth[Qh[0]].y, Depth[Qh[1]].y, Depth[Qh[2]].y, Depth[Qh[3]].y));
					const float Wha[4] = { Wh.x, Wh.y, Wh.z, Wh.w };
					const float Shw = Wh.x + Wh.y + Wh.z + Wh.w;
					if (!(Shw >= 0.5f)) { return false; }
					HAge = 3.0e38f;
					float HOwn = 3.0e38f;
					for (int K = 0; K < 4; ++K)
					{
						const float Wk = Wha[K] / Shw;
						Hv = Hv + Vol[Qh[K]] * Wk;
						for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { Hsl[S] += Slices[Qh[K] * H::UNI_VOL_SLICES + S] * Wk; }
						HDep += Depth[Qh[K]].x * Wk;
						if (!Mom.empty() && Mom.size() == Vol.size()) { HMomLast = H::float2(HMomLast.x + Mom[Qh[K]].x * Wk, HMomLast.y + Mom[Qh[K]].y * Wk); }   // D-251
						if (Wha[K] > 0.0f) { HAge = std::min(HAge, Age.empty() ? 0.0f : Age[Qh[K]]); HOwn = std::min(HOwn, AgeOwn.empty() ? 0.0f : AgeOwn[Qh[K]]); }   // D-240: the least the taps hold
					}
					if (!(HAge < 1.0e30f)) { HAge = 0.0f; }
					HOwnLast = HOwn < 1.0e30f ? HOwn : 0.0f;
					const float Fx = Pp.x - 0.5f, Fy = Pp.y - 0.5f;
					const int X0 = int(std::floor(Fx)), Y0 = int(std::floor(Fy));
					if (X0 - 1 >= 0 && Y0 - 1 >= 0 && X0 + 2 < HW && Y0 + 2 < HH)
					{
						const H::float4 Cx = H::Uni_CatmullRomWeights(Fx - float(X0)), Cy = H::Uni_CatmullRomWeights(Fy - float(Y0));
						const float Cxa[4] = { Cx.x, Cx.y, Cx.z, Cx.w }, Cya[4] = { Cy.x, Cy.y, Cy.z, Cy.w };
						H::float4 Cr(0, 0, 0, 0);
						bool bSame = true;
						for (int J = 0; J < 4; ++J) for (int K = 0; K < 4; ++K)
						{
							const size_t Qc = size_t(Y0 - 1 + J) * HW + size_t(X0 - 1 + K);
							bSame = bSame && H::Uni_TemporalDepthWeight(LnTp, Depth[Qc].y) > 0.9f;
							Cr = Cr + Vol[Qc] * (Cxa[K] * Cya[J]);
						}
						if (bSame) { Hv = H::float4(std::max(Cr.x, 0.0f), std::max(Cr.y, 0.0f), std::max(Cr.z, 0.0f), std::min(std::max(Cr.w, 0.0f), 1.0f)); }
					}
					if (HistFarSign != 0 && HistFar) { Hv = H::Uni_FarHistoryConvert(Hv, HistFar(Dd), float(HistFarSign)); }   // D-228
					Mo = H::Uni_TemporalMotionPx(H::Uni_TemporalToGrid(Pp, HSizeF, SizeF), H::uint2(u32(X), u32(Y)));   // D-215: in this frame's pixels
					return true;
				};
				if (bCloud)
				{
					// D-246: CloudAtmosphere's resolve (MainResolveCS), every pixel.
					const int Nn = int(H::Uni_TemporalN(Phase));
					const H::uint2 Oo = H::Uni_TemporalOffset(Phase);
					const int Ox = int(Oo.x), Oy = int(Oo.y);
					const int LastX = (W - 1 - Ox) / Nn, LastY = (Ht - 1 - Oy) / Nn;   // the last fresh sample on each axis
					float Exp = Exposure;
					if (!(Exp > 0.0f))
					{
						double Sl = 0.0; size_t Nl = 0;
						for (int Ky = 0; Ky <= LastY; ++Ky) for (int Kx = 0; Kx <= LastX; ++Kx) { Sl += Lum(FVol[size_t(Ky * Nn + Oy) * W + size_t(Kx * Nn + Ox)]); ++Nl; }
						Exp = Sl > 0.0 ? float(double(Nl) / Sl) : 1.0f;
					}
					const float MaxS = std::max(MaxSamples, 1.0f);
					OAgeOwn.assign(N, 0.0f);
					LastExp = Exp;
					for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
					{
						const size_t I = size_t(Y) * W + X;
						const int Rx = X - Ox, Ry = Y - Oy;
						const int Bx = (Rx + Nn) / Nn - 1, By = (Ry + Nn) / Nn - 1;
						const int Mx = Rx - Bx * Nn, My = Ry - By * Nn;
						const float Fx = float(Mx) / float(Nn), Fy = float(My) / float(Nn);
						const bool bFresh = Mx == 0 && My == 0 && Bx >= 0 && By >= 0 && Bx <= LastX && By <= LastY;
						H::float4 Fill(0, 0, 0, 0), Own(0, 0, 0, 0), M1(0, 0, 0, 0), M2(0, 0, 0, 0);
						float FillSl[H::UNI_VOL_SLICES] = {};
						float FillTot = 0.0f, MomTot = 0.0f, BestW = 0.0f, BestDepth = H::UNI_VOL_LN_DEPTH_NONE, OwnDepth = H::UNI_VOL_LN_DEPTH_NONE;
						size_t OwnI = I;
						for (int Tap = 0; Tap < 16; ++Tap)
						{
							const int Dx = (Tap & 3) - 1, Dy = (Tap >> 2) - 1;
							const int Kx = std::clamp(Bx + Dx, 0, LastX), Ky = std::clamp(By + Dy, 0, LastY);
							const int Sx = Kx * Nn + Ox, Sy = Ky * Nn + Oy;
							const bool bUsable = Sx < W && Sy < Ht && Kx == Bx + Dx && Ky == By + Dy;
							const size_t Q = size_t(Sy) * W + size_t(Sx);
							const float Guide = bUsable ? H::Uni_TemporalDepthWeight(LnTp, FDep[Q].y) + 1.0e-3f : 0.0f;
							const H::float4 C = FVol[Q];
							if (Tap == 5) { Own = C; OwnI = Q; OwnDepth = FDep[Q].x; }
							const float Ddx = float(Dx) - Fx, Ddy = float(Dy) - Fy;
							const float Fw = H::Uni_ResolveFillW(Ddx, Ddy) * Guide;
							Fill = Fill + C * Fw;
							for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { FillSl[S] += FSl[Q * H::UNI_VOL_SLICES + S] * Fw; }
							FillTot += Fw;
							if (Fw > BestW) { BestW = Fw; BestDepth = FDep[Q].x; }
							const float Mw = H::Uni_ResolveMomentW(Ddx, Ddy) * Guide;
							const H::float4 Cc = H::Uni_ResolveCompress(C, Exp);
							M1 = M1 + Cc * Mw;
							M2 = M2 + H::float4(Cc.x * Cc.x, Cc.y * Cc.y, Cc.z * Cc.z, Cc.w * Cc.w) * Mw;
							MomTot += Mw;
						}
						Fill = Fill * (1.0f / std::max(FillTot, 1.0e-6f));
						for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { FillSl[S] /= std::max(FillTot, 1.0e-6f); }
						M1 = M1 * (1.0f / std::max(MomTot, 1.0e-6f));
						const H::float4 Mm2 = M2 * (1.0f / std::max(MomTot, 1.0e-6f));
						const H::float4 Sig(std::sqrt(std::max(Mm2.x - M1.x * M1.x, 0.0f)), std::sqrt(std::max(Mm2.y - M1.y * M1.y, 0.0f)),
							std::sqrt(std::max(Mm2.z - M1.z * M1.z, 0.0f)), std::sqrt(std::max(Mm2.w - M1.w * M1.w, 0.0f)));
						const float LnD = bFresh ? OwnDepth : BestDepth;
						H::float4 Hv; float Hsl[H::UNI_VOL_SLICES]; float HDep = 0.0f, HAge = 0.0f, Mo = 0.0f;
						bool bValid = Reproject(X, Y, LnD, Hv, Hsl, HDep, HAge, Mo);
						const float HOwn = HOwnLast;
						const H::float2 HMom = HMomLast;   // D-251
						if (bValid && Mo > MaxMotionPx) { bValid = false; ++TooFast; }
						const float Motion = H::Uni_ResolveMotion(Mo);
						const float Gamma = H::Uni_ResolveGamma(Motion);
						H::float4 Hist(0, 0, 0, 0);
						float AgeH = 0.0f, OwnH = 0.0f, Rej = 1.0f;
						if (bValid)
						{
							const H::float4 Hc = H::Uni_ResolveCompress(Hv, Exp);
							const H::float4 Cl = H::Uni_ResolveClamp(Hc, M1, Sig, Gamma);
							Rej = H::Uni_ResolveReject(Hc, Cl, Sig, Gamma);
							const bool bMoved = Cl.x != Hc.x || Cl.y != Hc.y || Cl.z != Hc.z || Cl.w != Hc.w;
							Hist = bMoved ? H::Uni_ResolveExpand(Cl, Exp) : Hv;
							AgeH = HAge * (1.0f - Rej); OwnH = HOwn * (1.0f - Rej);
						}
						if (bFresh)
						{
							const float Al = bValid ? std::max(1.0f / (AgeH + 1.0f), 1.0f / MaxS) : 1.0f;
							OVol[I] = Hist * (1.0f - Al) + Own * Al;
							for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { OSl[I * H::UNI_VOL_SLICES + S] = (bValid ? Hsl[S] : 0.0f) + (FSl[OwnI * H::UNI_VOL_SLICES + S] - (bValid ? Hsl[S] : 0.0f)) * Al; }
							ODep[I] = FDep[OwnI];
							OAge[I] = std::min(AgeH + 1.0f, MaxS); OAgeOwn[I] = std::min(OwnH + 1.0f, MaxS);
							{
								// D-251: the own sample into the moments (uniform over the own samples, down to 1 / MaxS).
								const H::float4 Oc = H::Uni_ResolveCompress(Own, Exp);
								const float Lo = H::Uni_Luminance(H::float3(Oc.x, Oc.y, Oc.z));
								const float Am = bValid ? std::max(1.0f / (OwnH + 1.0f), 1.0f / MaxS) : 1.0f;
								OMom[I] = H::float2(HMom.x * (1.0f - Am) + Lo * Am, HMom.y * (1.0f - Am) + Lo * Lo * Am);
							}
							if (bValid) { ++Accumulated; if (Rej > 0.5f) { ++Restarted; } }
						}
						else if (bValid)
						{
							const float Share = H::Uni_ResolveFillShare(Motion, OwnH);
							OVol[I] = Hist * (1.0f - Share) + Fill * Share;
							for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { OSl[I * H::UNI_VOL_SLICES + S] = Hsl[S] + (FillSl[S] - Hsl[S]) * Share; }
							ODep[I] = H::float2(HDep, LnTp);
							OAge[I] = std::min(AgeH + Share, MaxS); OAgeOwn[I] = OwnH;
							OMom[I] = HMom;   // D-251
							++FromHistory;
						}
						else
						{
							OVol[I] = Fill;
							for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { OSl[I * H::UNI_VOL_SLICES + S] = FillSl[S]; }
							ODep[I] = H::float2(BestDepth, LnTp);
							OAge[I] = 0.0f; OAgeOwn[I] = 0.0f;
							OMom[I] = H::float2(0.0f, 0.0f);   // D-251
							++FromFresh;
							}
							// D-251: the variance of this pixel's value.
							OVar[I] = H::Uni_DenoiseVariance(OMom[I], OAgeOwn[I], OAge[I]);
					}
				}
				else
				for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
				{
					const H::uint2 Px = H::uint2(u32(X), u32(Y));
					const size_t I = size_t(Y) * W + X;
					if (H::Uni_TemporalFresh(Px, Phase))
					{
						if (!bAccumulate) { continue; }
						// D-240: the fresh sample into its history, clamped into the box of it and its
						// fresh neighbours N away.
						const H::FUniTemporalTaps Rg = H::Uni_TemporalRing(Px, Phase, Size);
						const size_t Qr[4] = { size_t(Rg.Q0.y) * W + Rg.Q0.x, size_t(Rg.Q1.y) * W + Rg.Q1.x, size_t(Rg.Q2.y) * W + Rg.Q2.x, size_t(Rg.Q3.y) * W + Rg.Q3.x };
						const float Wra[4] = { Rg.W.x, Rg.W.y, Rg.W.z, Rg.W.w };
						H::float4 Lo4 = FVol[I], Hi4 = FVol[I];
						for (int K = 0; K < 4; ++K)
						{
							if (!(Wra[K] > 0.0f)) { continue; }
							const H::float4& Fv = FVol[Qr[K]];
							Lo4 = H::float4(std::min(Lo4.x, Fv.x), std::min(Lo4.y, Fv.y), std::min(Lo4.z, Fv.z), std::min(Lo4.w, Fv.w));
							Hi4 = H::float4(std::max(Hi4.x, Fv.x), std::max(Hi4.y, Fv.y), std::max(Hi4.z, Fv.z), std::max(Hi4.w, Fv.w));
						}
						H::float4 Hv; float Hsl[H::UNI_VOL_SLICES]; float HDep = 0.0f, HAge = 0.0f, Mo = 0.0f;
						if (Reproject(X, Y, FDep[I].x, Hv, Hsl, HDep, HAge, Mo) && (!bClamp || Mo <= MaxMotionPx))
						{
							float Rej = 0.0f;
							H::float4 Hc = Hv;
							if (bClamp)
							{
								const float Kc = H::Uni_TemporalClampK(Mo, ClampTightPx);
								Hc = H::Uni_TemporalClamp(Hv, Lo4, Hi4, Kc);
								Rej = H::Uni_TemporalReject(Hv, Hc, Lo4, Hi4, Kc);
							}
							else if (!H::Uni_TemporalAccept(Lum(Hv), Lum(Lo4), Lum(Hi4))) { Rej = 1.0f; }
							const float Ae = HAge * (1.0f - Rej);
							const float Al = H::Uni_TemporalAlpha(Ae, MaxSamples);
							OVol[I] = H::float4(Hc.x + (FVol[I].x - Hc.x) * Al, Hc.y + (FVol[I].y - Hc.y) * Al, Hc.z + (FVol[I].z - Hc.z) * Al, Hc.w + (FVol[I].w - Hc.w) * Al);
							for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { OSl[I * H::UNI_VOL_SLICES + S] = Hsl[S] + (FSl[I * H::UNI_VOL_SLICES + S] - Hsl[S]) * Al; }
							OAge[I] = H::Uni_TemporalAgeFresh(HAge, Rej, MaxSamples);
							++Accumulated;
							if (Rej > 0.5f) { ++Restarted; }
						}
						continue;
					}
					const H::FUniTemporalTaps Tp = H::Uni_TemporalTaps(Px, Phase, Size);
					const size_t Q[4] = { size_t(Tp.Q0.y) * W + Tp.Q0.x, size_t(Tp.Q1.y) * W + Tp.Q1.x, size_t(Tp.Q2.y) * W + Tp.Q2.x, size_t(Tp.Q3.y) * W + Tp.Q3.x };
					const H::float4 Ws = H::Uni_TemporalSpatialW(Tp.W, LnTp, H::float4(FDep[Q[0]].y, FDep[Q[1]].y, FDep[Q[2]].y, FDep[Q[3]].y));
					const float Wsa[4] = { Ws.x, Ws.y, Ws.z, Ws.w };
					const float Wba[4] = { Tp.W.x, Tp.W.y, Tp.W.z, Tp.W.w };
					const float Sw = Ws.x + Ws.y + Ws.z + Ws.w;
					H::float4 Sp(0, 0, 0, 0);
					float Lo = 3.0e38f, Hi = 0.0f;
					H::float4 Lo4(3.0e38f, 3.0e38f, 3.0e38f, 3.0e38f), Hi4(0, 0, 0, 0);
					for (int K = 0; K < 4; ++K)
					{
						Sp = Sp + FVol[Q[K]] * (Wsa[K] / Sw);
						if (Wba[K] > 0.0f)
						{
							Lo = std::min(Lo, Lum(FVol[Q[K]])); Hi = std::max(Hi, Lum(FVol[Q[K]]));
							const H::float4& Fv = FVol[Q[K]];
							Lo4 = H::float4(std::min(Lo4.x, Fv.x), std::min(Lo4.y, Fv.y), std::min(Lo4.z, Fv.z), std::min(Lo4.w, Fv.w));
							Hi4 = H::float4(std::max(Hi4.x, Fv.x), std::max(Hi4.y, Fv.y), std::max(Hi4.z, Fv.z), std::max(Hi4.w, Fv.w));
						}
					}
					const float LnD = H::Uni_TemporalLightLnDepth(Ws, H::float4(FDep[Q[0]].x, FDep[Q[1]].x, FDep[Q[2]].x, FDep[Q[3]].x));
					bool bHist = false;
					H::float4 Hv; float Hsl[H::UNI_VOL_SLICES]; float HDep = 0.0f, HAge = 0.0f, Mo = 0.0f;
					float Rej = 0.0f;
					if (Reproject(X, Y, LnD, Hv, Hsl, HDep, HAge, Mo))
					{
						if (bClamp)
						{
							// D-194: clamp into the fresh range (tight once moving); too fast: not used.
							if (Mo <= MaxMotionPx)
							{
								const float Kc = H::Uni_TemporalClampK(Mo, ClampTightPx);
								const H::float4 Hc = H::Uni_TemporalClamp(Hv, Lo4, Hi4, Kc);
								Rej = H::Uni_TemporalReject(Hv, Hc, Lo4, Hi4, Kc);
								Hv = Hc;
								bHist = true;
							}
							else { ++TooFast; }
						}
						else { bHist = H::Uni_TemporalAccept(Lum(Hv), Lo, Hi); }
					}
					if (bHist)
					{
						OVol[I] = Hv;
						for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { OSl[I * H::UNI_VOL_SLICES + S] = Hsl[S]; }
						ODep[I] = H::float2(HDep, LnTp);
						OAge[I] = HAge * (1.0f - Rej);   // D-240: no sample taken here
						++FromHistory;
					}
					else
					{
						OVol[I] = Sp;
						for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S)
						{
							float A = 0.0f;
							for (int K = 0; K < 4; ++K) { A += FSl[Q[K] * H::UNI_VOL_SLICES + S] * (Wsa[K] / Sw); }
							OSl[I * H::UNI_VOL_SLICES + S] = A;
						}
						ODep[I] = H::float2(LnD, LnTp);
						OAge[I] = 0.0f;
						++FromFresh;
					}
				}
			}
			if (!bSlice)
			{
				// D-251: a frame marched in full starts each pixel's moments with its sample (MarchCS's
				// bWriteMoments); its variance the neighbourhood's (none read here).
				float Exp = Exposure;
				if (!(Exp > 0.0f))
				{
					double Sl = 0.0;
					for (size_t I = 0; I < N; ++I) { Sl += Lum(FVol[I]); }
					Exp = Sl > 0.0 ? float(double(N) / Sl) : 1.0f;
				}
				LastExp = Exp;
				for (size_t I = 0; I < N; ++I)
				{
					const H::float4 Oc = H::Uni_ResolveCompress(FVol[I], Exp);
					const float Lo = H::Uni_Luminance(H::float3(Oc.x, Oc.y, Oc.z));
					OMom[I] = H::float2(Lo, Lo * Lo);
				}
			}
			Age.swap(OAge);
			AgeOwn.swap(OAgeOwn);
			Mom.swap(OMom); Var.swap(OVar);   // D-251
			Vol.swap(OVol); Slices.swap(OSl); Depth.swap(ODep);
			HW = W; HH = Ht;
			Prev = Cam;
			bHave = true;
			++Frame;
		}
	};

	/** D-251: the volume's a-trous denoise (UniVolume.usf DenoiseCS): Iters passes over InVol with
	 *  the variance InVar (compressed luminance at InExp) and the scene depths InDep.y (ln) as the
	 *  guide; the light filtered (transmittance kept). As the GPU pass, tap for tap. */
	inline std::vector<H::float4> DenoiseVolume(const std::vector<H::float4>& InVol, const std::vector<float>& InVar, const std::vector<H::float2>& InDep, int W, int Ht, float InExp, int Iters, float InSigma = H::UNI_DN_SIGMA)
	{
		std::vector<H::float4> C = InVol;
		std::vector<float> V = InVar;
		auto Lc = [InExp](const H::float4& X) { const H::float4 Q = H::Uni_ResolveCompress(X, InExp); return H::Uni_Luminance(H::float3(Q.x, Q.y, Q.z)); };
		for (int It = 0; It < Iters; ++It)
		{
			const int St = 1 << It;
			std::vector<H::float4> C2(C.size());
			std::vector<float> V2(V.size());
			for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
			{
				const size_t I = size_t(Y) * W + X;
				const float Lp = Lc(C[I]);
				float Vb = 0.0f, Wb = 0.0f;
				for (int Dy = -1; Dy <= 1; ++Dy) for (int Dx = -1; Dx <= 1; ++Dx)
				{
					const int Xq = std::clamp(X + Dx, 0, W - 1), Yq = std::clamp(Y + Dy, 0, Ht - 1);
					const float Wk = (Dx == 0 ? 0.5f : 0.25f) * (Dy == 0 ? 0.5f : 0.25f);
					Vb += Wk * V[size_t(Yq) * W + Xq]; Wb += Wk;
				}
				const float Den = InSigma * std::sqrt(std::max(Vb / Wb, 0.0f));
				float Sw = 0.0f, Va = 0.0f;
				H::float4 Acc(0, 0, 0, 0);
				for (int Ky = -2; Ky <= 2; ++Ky) for (int Kx = -2; Kx <= 2; ++Kx)
				{
					const int Xq = X + Kx * St, Yq = Y + Ky * St;
					if (Xq < 0 || Yq < 0 || Xq >= W || Yq >= Ht) { continue; }
					const size_t Q = size_t(Yq) * W + Xq;
					const float Wq = H::Uni_DenoiseKernel(Kx) * H::Uni_DenoiseKernel(Ky) * H::Uni_TemporalDepthWeight(InDep[I].y, InDep[Q].y) * H::Uni_DenoiseLumWeight(Lp, Lc(C[Q]), Den);
					Acc = Acc + C[Q] * Wq; Sw += Wq; Va += Wq * Wq * V[Q];
				}
				const float Inv = 1.0f / std::max(Sw, 1.0e-20f);
				C2[I] = H::float4(Acc.x * Inv, Acc.y * Inv, Acc.z * Inv, C[I].w);
				V2[I] = Va * Inv * Inv;
			}
			C.swap(C2); V.swap(V2);
		}
		return C;
	}

	/** RenderFrame with the volume: promoted bodies, then the volume (it needs their distance),
	 *  then sprites attenuated by the slice transmittance at their true distance. D-196: a
	 *  carrier's parent sprite draws 1 - the share the scene's handoffs give it (0 when it has no
	 *  carrier), as the engine's project pass does; bCurveHandoff: the handoff curve instead (the
	 *  behaviour before D-196, for comparisons).
	 *  D-203: P is anything with ForEachResident(Fn(key, object, fade, identity hash)): the
	 *  reference pipeline, or a view of the runtime streamer's cell table and pool. */
	template <typename TSource>
	inline FFrameStats RenderFrameWithVolume(const TSource& P, const FVolumeScene& Sc, const FCamera& Cam, FImage& Img, FVolumeStats* VolOut = nullptr, bool bVolume = true, bool bCurveHandoff = false)
	{
		FFrameStats St;
		const H::FUniView V = Cam.View();
		std::vector<H::FUniSprite> Sprites;
		std::unordered_map<u64, float> Share;
		if (bVolume) { for (const FCarrierHandoff& Hd : Sc.Handoffs) { Share[HashObject(Hd.Parent)] = Hd.Hand; } }
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float Fade, u64 Hash)
		{
			++St.Resident;
			H::uint4 D[3];
			CellDelta(K, Cam.Coord, D);
			const H::FUniObject Ho = UniCore::ToHlsl(O);
			float Sh = -1.0f;
			if (!bCurveHandoff) { const auto It = Share.find(Hash); Sh = It != Share.end() ? It->second : 0.0f; }
			const H::FUniSprite S = H::Uni_ProjectObjectH(D[0], D[1], D[2], u32(K.Level), Fade, Ho, V, false, Sh);
			if (V.NucleusArchetypeMask != 0u)   // D-272: a galaxy's nucleus, as the project pass appends it (never handed off)
			{
				const H::FUniSprite Sn = H::Uni_ProjectNucleusH(D[0], D[1], D[2], u32(K.Level), Fade, Ho, V);
				if (Sn.Flags & H::UNI_SPRITE_VISIBLE) { Sprites.push_back(Sn); }
			}
			if (S.Flags & H::UNI_SPRITE_PROMOTE_CANDIDATE)
			{
				if (!((O.A[3] >> 16) & H::UNI_OBJ_FLAG_EXTENDED))
				{
					const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
					const FVec3d Rel = ToDouble(Sub(ObjectCoord(K, Off), Cam.Coord));
					const FV3 C{ Rel.X - Cam.Frac.X, Rel.Y - Cam.Frac.Y, Rel.Z - Cam.Frac.Z };
					DrawPromotedSphere(Img, Cam, C, std::exp2(double(H::Uni_ObjectRadiusLog2(UniCore::ToHlsl(O)))), H::Uni_UnpackColour(O.B[3]));
				}
				++St.Promoted;
				return;
			}
			if (S.Flags & H::UNI_SPRITE_VISIBLE) { Sprites.push_back(S); }
		});
		std::vector<H::float3> Slices;
		FVolumeStats Vs;
		if (bVolume) { Vs = RenderVolume(Sc, Cam, Img, &Slices); }
		const H::FUniVolumeView VV = ToHlsl(Sc.View);
		for (H::FUniSprite S : Sprites)
		{
			if (bVolume)
			{
				const int Px = std::clamp(int((double(S.NdcX) * 0.5 + 0.5) * Img.W), 0, Img.W - 1);
				const int Py = std::clamp(int((0.5 - double(S.NdcY) * 0.5) * Img.H), 0, Img.H - 1);
				H::float3 Sl[H::UNI_VOL_SLICES];
				for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { Sl[K] = Slices[(size_t(Py) * Img.W + Px) * H::UNI_VOL_SLICES + K]; }
				const H::float3 T = H::Uni_SliceTransmittance(Sl, S.TrueDistance, VV);
				S.Radiance = S.Radiance * T;
			}
			Vs.SpriteFlux[0] += S.Radiance.x; Vs.SpriteFlux[1] += S.Radiance.y; Vs.SpriteFlux[2] += S.Radiance.z;
			if (Splat(Img, Cam, S)) { ++St.Visible; }
			else { ++St.Occluded; }
		}
		if (VolOut) { *VolOut = Vs; }
		return St;
	}

	/** D-196/D-197: the light over the whole sphere (six 90-degree faces of Size^2), sprites +
	 *  volume, as luminance; OutSprites: the sprites' share of it. */
	template <typename TSource>
	inline double SphereLight(const TSource& P, const FVolumeScene& Sc, const FCoord& Cam, int Size, double* OutSprites = nullptr)
	{
		const FV3 Looks[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0.001, 0, 1 }, { 0.001, 0, -1 } };
		double L = 0.0, Sp = 0.0;
		for (const FV3& Lk : Looks)
		{
			FCamera Cm; Cm.Coord = Cam; Cm.Width = Size; Cm.Height = Size; Cm.DeltaMag = 0.0; Cm.LookAlong(Lk);
			FImage Img(Cm.Width, Cm.Height);
			FVolumeStats Vs;
			RenderFrameWithVolume(P, Sc, Cm, Img, &Vs);
			L += 0.2126 * (Vs.Flux[0] + Vs.SpriteFlux[0]) + 0.7152 * (Vs.Flux[1] + Vs.SpriteFlux[1]) + 0.0722 * (Vs.Flux[2] + Vs.SpriteFlux[2]);
			Sp += 0.2126 * Vs.SpriteFlux[0] + 0.7152 * Vs.SpriteFlux[1] + 0.0722 * Vs.SpriteFlux[2];
		}
		if (OutSprites) { *OutSprites = L > 0.0 ? Sp / L : 0.0; }
		return L;
	}
}
