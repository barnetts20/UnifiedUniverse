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
					// Supersampling where a small carrier's finest feature is under half a pixel.
					u32 SS = 1u;
					for (u32 C = 0; C < V.NumCarriers; ++C)
					{
						const H::FUniCarrier Ca = Cb[C];
						float T0, T1;
						if (Ca.Weight > 0.0f && H::Uni_CarrierInterval(Ca, H::Uni_CarrierLocalDir(Ca, Dc), TScene, T0, T1)) { SS = std::max(SS, H::Uni_CarrierSubsamples(Ca, V.ThetaPixel)); }
					}
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
						for (u32 J = 0; J < SS; ++J) for (u32 I = 0; I < SS; ++I)
						{
							const FV3 Ds = PixelDir(Cam, Img.W, Img.H, X + (I + 0.5) / SS, Y + (J + 0.5) / SS);
							const H::FUniVolumeResult Rs = H::Uni_VolumeMarch(H::float3(float(Ds.X), float(Ds.Y), float(Ds.Z)), TScene, Cb, Pb, Lb, V);
							Acc += Rs.Radiance; AccT += Rs.Transmittance; Steps += Rs.Steps; Tr += Rs.Truncated; Act = std::max(Act, Rs.Active);
							for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { AccS[K] = (I == 0 && J == 0) ? Rs.SliceT[K] : AccS[K] + Rs.SliceT[K]; }
						}
						const float Inv = 1.0f / float(SS * SS);
						R.Radiance = Acc * Inv; R.Transmittance = AccT * Inv; R.Steps = Steps; R.Truncated = Tr ? 1u : 0u; R.Active = Act;
						for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { R.SliceT[K] = AccS[K] * Inv; }
						++S.Supersampled;
					}
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

	/** RenderFrame with the volume: promoted bodies, then the volume (it needs their distance),
	 *  then sprites attenuated by the slice transmittance at their true distance. */
	inline FFrameStats RenderFrameWithVolume(const FRefPipeline& P, const FVolumeScene& Sc, const FCamera& Cam, FImage& Img, FVolumeStats* VolOut = nullptr, bool bVolume = true)
	{
		FFrameStats St;
		const H::FUniView V = Cam.View();
		std::vector<H::FUniSprite> Sprites;
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float Fade, u64)
		{
			++St.Resident;
			H::uint4 D[3];
			CellDelta(K, Cam.Coord, D);
			const H::FUniSprite S = H::Uni_ProjectObject(D[0], D[1], D[2], u32(K.Level), Fade, UniCore::ToHlsl(O), V, false);
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
}
