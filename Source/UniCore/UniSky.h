#pragma once

// UniSky.h
// D-186: the sky light's cubemap (design spec: the backdrop lights and reflects in promoted
// bodies). A small cube (D3D / Unreal face order and orientation) marched through the frame's
// volume scene, local and background carriers together, to the horizon: the galaxy's glow and
// the web's light around the camera, as the screen shows them. Stars drawn as sprites are not in
// it (a few percent of a galaxy's light; the nearest star lights the scene through the light rig).
// Engine-agnostic; the engine runs it on a worker thread every second or so and blends.

#include "UniVolume.h"

#include <cmath>
#include <vector>

namespace UniCore
{
	/** Unit direction through texel (X, Y) of face F (0..5: +X, -X, +Y, -Y, +Z, -Z) of an N x N
	 *  cube, Direct3D's cubemap convention (Unreal samples cubes with it). */
	inline FVec3d SkyTexelDir(int F, double X, double Y, int N)
	{
		const double U = 2.0 * X / N - 1.0, V = 2.0 * Y / N - 1.0;
		FVec3d D{ 0, 0, 0 };
		switch (F)
		{
		case 0: D = FVec3d{ 1.0, -V, -U }; break;
		case 1: D = FVec3d{ -1.0, -V, U }; break;
		case 2: D = FVec3d{ U, 1.0, V }; break;
		case 3: D = FVec3d{ U, -1.0, -V }; break;
		case 4: D = FVec3d{ U, -V, 1.0 }; break;
		default: D = FVec3d{ -U, -V, -1.0 }; break;
		}
		const double L = std::sqrt(D.X * D.X + D.Y * D.Y + D.Z * D.Z);
		return FVec3d{ D.X / L, D.Y / L, D.Z / L };
	}

	/** Solid angle of texel (X, Y) of an N x N face (the same for every face). */
	inline double SkyTexelSolidAngle(int X, int Y, int N)
	{
		auto A = [](double U, double V) { return std::atan2(U * V, std::sqrt(U * U + V * V + 1.0)); };
		const double U0 = 2.0 * X / N - 1.0, U1 = 2.0 * (X + 1) / N - 1.0, V0 = 2.0 * Y / N - 1.0, V1 = 2.0 * (Y + 1) / N - 1.0;
		return A(U1, V1) - A(U0, V1) - A(U1, V0) + A(U0, V0);
	}

	struct FSkyCube
	{
		int N = 0;
		std::vector<HV::float3> Texels;   // 6 * N * N, face-major then rows: radiance x Scale
		u64 Steps = 0;
	};

	/** March every texel through the scene (all its carriers, to the horizon: no scene
	 *  surfaces) as Sub x Sub rays over its area, each with its share of the texel as the
	 *  prefilter footprint, and scale the radiance (exposure units per sr) by Scale: the engine
	 *  passes the screen pixel's solid angle, so a texel holds what a screen pixel of that sky
	 *  shows. One ray per texel at 16 a face puts a thin galactic band's light out by ~8%. */
	inline FSkyCube BuildSkyCube(const FVolumeScene& Sc, int N, double Scale, int Sub = 2)
	{
		FSkyCube C;
		C.N = N;
		C.Texels.assign(size_t(6) * N * N, HV::float3(0, 0, 0));
		Sub = Sub < 1 ? 1 : Sub;
		HV::FUniVolumeView V = ToHlsl(Sc.View);
		V.ThetaPixel = float(2.0 / (double(N) * Sub));
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		const float Inv = float(Scale) / float(Sub * Sub);
		for (int F = 0; F < 6; ++F) for (int Y = 0; Y < N; ++Y) for (int X = 0; X < N; ++X)
		{
			HV::float3 Acc(0, 0, 0);
			for (int J = 0; J < Sub; ++J) for (int I = 0; I < Sub; ++I)
			{
				const FVec3d D = SkyTexelDir(F, X + (I + 0.5) / Sub, Y + (J + 0.5) / Sub, N);
				const HV::FUniVolumeResult R = HV::Uni_VolumeMarch(HV::float3(float(D.X), float(D.Y), float(D.Z)), 3.0e38f, Cb, Pb, Lb, V);
				Acc += R.Radiance;
				C.Steps += R.Steps;
			}
			C.Texels[(size_t(F) * N + Y) * N + X] = Acc * Inv;
		}
		return C;
	}
}
