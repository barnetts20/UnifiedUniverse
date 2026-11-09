#pragma once

// UniVolumeRef.h
// CPU reference for the volume pass (Docs/VOLUMETRICS.md): marches every pixel through the
// shared core (Uni_VolumeMarch), deposits radiance x the pixel's exact solid angle, and
// composites sprites attenuated by the transmittance slices, as the GPU pass will.
// Verification only. Multithreaded over rows.

#include "UniRenderRef.h"
#include "UniVolume.h"

#include <atomic>
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
	 *  the sub-rays' light (D-184). */
	template <class TC, class TP, class TL>
	inline H::FUniVolumeResult MarchPixel(const TC& Cb, const TP& Pb, const TL& Lb, const H::FUniVolumeView& V, const FCamera& Cam, int Wd, int Ht, int X, int Y, float TScene, bool& bOutSuper)
	{
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
			R = H::Uni_VolumeMarch(Dc, TScene, Cb, Pb, Lb, V);
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
				const H::FUniVolumeResult Rs = H::Uni_VolumeMarch(H::float3(float(Ds.X), float(Ds.Y), float(Ds.Z)), TScene, Cb, Pb, Lb, V);
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
		std::vector<H::float4> Vol;      // radiance rgb (exposure units per sr), luminance transmittance a
		std::vector<float> Slices;       // luminance transmittance at the slice boundaries
		std::vector<H::float2> Depth;    // ln depth of the light, ln scene distance
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

		static float Lum(const H::float4& V) { return H::Uni_Luminance(H::float3(V.x, V.y, V.z)); }

		void Step(const FVolumeScene& Sc, const FCamera& Cam, const FV3& CamStepCm, bool bStepValid, bool bTemporal, int Threads = 2)
		{
			if (Cam.Width != W || Cam.Height != Ht) { W = Cam.Width; Ht = Cam.Height; bHave = false; }
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
			const H::FUniVolumeView V = ToHlsl(Sc.View);
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
						const H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cam, W, Ht, X, Y, TScene, bSuper);
						const size_t I = size_t(Y) * W + X;
						FVol[I] = H::float4(R.Radiance.x, R.Radiance.y, R.Radiance.z, H::Uni_Luminance(R.Transmittance));
						for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { FSl[I * H::UNI_VOL_SLICES + K] = H::Uni_Luminance(R.SliceT[K]); }
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
			// Pass 3: reconstruct the rest.
			std::vector<H::float4> OVol = FVol;
			std::vector<float> OSl = FSl;
			std::vector<H::float2> ODep = FDep;
			if (bSlice)
			{
				const H::uint2 Size = H::uint2(u32(W), u32(Ht));
				const H::float2 SizeF = H::float2(float(W), float(Ht));
				for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
				{
					const H::uint2 Px = H::uint2(u32(X), u32(Y));
					if (H::Uni_TemporalFresh(Px, Phase)) { continue; }
					const size_t I = size_t(Y) * W + X;
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
					H::float4 Hv(0, 0, 0, 0);
					float Hsl[H::UNI_VOL_SLICES] = {};
					float HDep = 0.0f;
					if (TV.bHistory != 0u)
					{
						const FV3 Dd = PixelDir(Cam, W, Ht, X + 0.5, Y + 0.5);
						H::float2 Pp(0, 0);
						if (H::Uni_TemporalPrevPixel(H::float3(float(Dd.X), float(Dd.Y), float(Dd.Z)), LnD, TV, SizeF, Pp))
						{
							const H::FUniTemporalTaps Ht4 = H::Uni_TemporalHistTaps(Pp, Size);
							const size_t Qh[4] = { size_t(Ht4.Q0.y) * W + Ht4.Q0.x, size_t(Ht4.Q1.y) * W + Ht4.Q1.x, size_t(Ht4.Q2.y) * W + Ht4.Q2.x, size_t(Ht4.Q3.y) * W + Ht4.Q3.x };
							const H::float4 Wh = H::Uni_TemporalHistW(Ht4.W, LnTp, H::float4(Depth[Qh[0]].y, Depth[Qh[1]].y, Depth[Qh[2]].y, Depth[Qh[3]].y));
							const float Wha[4] = { Wh.x, Wh.y, Wh.z, Wh.w };
							const float Shw = Wh.x + Wh.y + Wh.z + Wh.w;
							if (Shw >= 0.5f)
							{
								for (int K = 0; K < 4; ++K)
								{
									const float Wk = Wha[K] / Shw;
									Hv = Hv + Vol[Qh[K]] * Wk;
									for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { Hsl[S] += Slices[Qh[K] * H::UNI_VOL_SLICES + S] * Wk; }
									HDep += Depth[Qh[K]].x * Wk;
								}
								// Radiance and transmittance by Catmull-Rom where all 16 texels are on the grid and
								// on this pixel's surface (the shader's rule: Uni_CatmullRomWeights).
								const float Fx = Pp.x - 0.5f, Fy = Pp.y - 0.5f;
								const int X0 = int(std::floor(Fx)), Y0 = int(std::floor(Fy));
								if (X0 - 1 >= 0 && Y0 - 1 >= 0 && X0 + 2 < W && Y0 + 2 < Ht)
								{
									const H::float4 Cx = H::Uni_CatmullRomWeights(Fx - float(X0)), Cy = H::Uni_CatmullRomWeights(Fy - float(Y0));
									const float Cxa[4] = { Cx.x, Cx.y, Cx.z, Cx.w }, Cya[4] = { Cy.x, Cy.y, Cy.z, Cy.w };
									H::float4 Cr(0, 0, 0, 0);
									bool bSame = true;
									for (int J = 0; J < 4; ++J) for (int K = 0; K < 4; ++K)
									{
										const size_t Qc = size_t(Y0 - 1 + J) * W + size_t(X0 - 1 + K);
										bSame = bSame && H::Uni_TemporalDepthWeight(LnTp, Depth[Qc].y) > 0.9f;
										Cr = Cr + Vol[Qc] * (Cxa[K] * Cya[J]);
									}
									if (bSame) { Hv = H::float4(std::max(Cr.x, 0.0f), std::max(Cr.y, 0.0f), std::max(Cr.z, 0.0f), std::min(std::max(Cr.w, 0.0f), 1.0f)); }
								}
								if (bClamp)
								{
									// D-194: clamp into the fresh range (tight once moving); too fast: not used.
									const float Mo = H::Uni_TemporalMotionPx(Pp, Px);
									if (Mo <= MaxMotionPx) { Hv = H::Uni_TemporalClamp(Hv, Lo4, Hi4, H::Uni_TemporalClampK(Mo, ClampTightPx)); bHist = true; }
									else { ++TooFast; }
								}
								else { bHist = H::Uni_TemporalAccept(Lum(Hv), Lo, Hi); }
							}
						}
					}
					if (bHist)
					{
						OVol[I] = Hv;
						for (u32 S = 0; S < H::UNI_VOL_SLICES; ++S) { OSl[I * H::UNI_VOL_SLICES + S] = Hsl[S]; }
						ODep[I] = H::float2(HDep, LnTp);
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
						++FromFresh;
					}
				}
			}
			Vol.swap(OVol); Slices.swap(OSl); Depth.swap(ODep);
			Prev = Cam;
			bHave = true;
			++Frame;
		}
	};

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
