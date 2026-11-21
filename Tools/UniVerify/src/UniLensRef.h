#pragma once

// UniLensRef.h
// D-273: the CPU reference of the lens pass (Shaders/Private/UniLens.usf): per output pixel, its ray
// traced by the core's Uni_LensTrace; the scene behind sampled where the ray came from (bilinear,
// folded back into the view by Uni_LensSkyNdc), the disc's light added at the pass's radiance scale
// x the pixel's solid angle. Pixels outside the region, or whose scene surface is nearer than the
// black hole, are copied. The engine pass does the same with the volume's dust in front of the disc
// (here: none).

#include "UniRenderRef.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace UniRender
{
	/** What the lens pass is handed (FUniLensFrame, through the renderer). */
	struct FLensView
	{
		UniHlsl::float3 CamRs = UniHlsl::float3(0.0f, 0.0f, 0.0f);   // the camera relative to the black hole, R_s (world axes)
		UniHlsl::FUniLensDisc Disc{};
		double DiscScale = 0.0;      // exposure units per sr per Uni_BhDiscLight unit (x the disc's share)
		float DiscKnee = 0.0f;       // D-274: the disc's light per pixel softened above this (0: physical; the engine's default UNI_BH_DISC_KNEE)
		double RegionRad = 3.14159265;
		double BhDistanceCm = 1.0e30;
		UniHlsl::uint MaxSteps = UniHlsl::UNI_LENS_MAX_STEPS;
		// The nucleus as a point (core and glare, exposure units; Uni_PointStyle), at the black hole's direction.
		double PointCore[3] = { 0.0, 0.0, 0.0 };
		double PointCoreSigmaPx = 1.0;
		double PointHalo[3] = { 0.0, 0.0, 0.0 };
		double PointHaloSigmaPx = 1.0;
	};

	struct FLensStats
	{
		size_t Traced = 0, Captured = 0, Capped = 0, Copied = 0;
		double StepSum = 0.0, StepMax = 0.0;
		double DiscFlux[3] = { 0.0, 0.0, 0.0 };   // the disc's light summed over the pixels (exposure units)
		double PointFlux[3] = { 0.0, 0.0, 0.0 };  // the nucleus point's
	};

	/** The pixel's ray (unit, world), as the engine builds it from the unjittered projection. */
	inline UniHlsl::float3 PixelRay(const FCamera& C, double X, double Y, double& OutCosF)
	{
		const double Nx = (X / C.Width) * 2.0 - 1.0, Ny = 1.0 - (Y / C.Height) * 2.0;
		const FV3 D = Norm(FV3{ C.Forward.X + Nx * C.TanX() * C.Right.X + Ny * C.TanY() * C.Up.X,
			C.Forward.Y + Nx * C.TanX() * C.Right.Y + Ny * C.TanY() * C.Up.Y,
			C.Forward.Z + Nx * C.TanX() * C.Right.Z + Ny * C.TanY() * C.Up.Z });
		OutCosF = Dot(D, C.Forward);
		return UniHlsl::float3(float(D.X), float(D.Y), float(D.Z));
	}

	/** Bilinear at a continuous pixel position (centres at +0.5), clamped to the image. */
	inline void SampleBilinear(const FImage& In, double Px, double Py, float Out[3])
	{
		const double X = std::clamp(Px - 0.5, 0.0, double(In.W - 1)), Y = std::clamp(Py - 0.5, 0.0, double(In.H - 1));
		const int X0 = std::min(int(X), In.W - 1), Y0 = std::min(int(Y), In.H - 1);
		const int X1 = std::min(X0 + 1, In.W - 1), Y1 = std::min(Y0 + 1, In.H - 1);
		const double Fx = X - X0, Fy = Y - Y0;
		for (int K = 0; K < 3; ++K)
		{
			const double A = In.Rgb[(size_t(Y0) * In.W + X0) * 3 + K], B = In.Rgb[(size_t(Y0) * In.W + X1) * 3 + K];
			const double Cc = In.Rgb[(size_t(Y1) * In.W + X0) * 3 + K], Dd = In.Rgb[(size_t(Y1) * In.W + X1) * 3 + K];
			Out[K] = float((A * (1.0 - Fx) + B * Fx) * (1.0 - Fy) + (Cc * (1.0 - Fx) + Dd * Fx) * Fy);
		}
	}

	/** D-279: the sky-only pyramid (UniLens.usf SkyMaskCS / SkyDownCS / SkySample): mip 0 the scene
	 *  behind the hole premultiplied by that mask (alpha the mask), each mip the 2 x 2 average of
	 *  the one above (clamped), sampled bilinear per level, covered origins filled front to back
	 *  from the coarser levels. */
	struct FSkyPyramid
	{
		struct FLevel { int W = 0, H = 0; std::vector<float> Px; };   // RGBA premultiplied
		std::vector<FLevel> Levels;
		FSkyPyramid(const FImage& In, double BhDistanceCm)
		{
			FLevel L0; L0.W = In.W; L0.H = In.H; L0.Px.resize(size_t(In.W) * In.H * 4);
			// The mask grown by 2 px (the 5 x 5 window): the foreground's colour
			// reaches past its depth by a pixel or two (antialiasing; the depth at the internal
			// resolution, the colour upscaled), and that fringe is not sky.
			for (int Y = 0; Y < In.H; ++Y) for (int X = 0; X < In.W; ++X)
			{
				float M = 1.0f;
				for (int Dy = -2; Dy <= 2 && M > 0.0f; ++Dy) for (int Dx = -2; Dx <= 2; ++Dx)
				{
					const int Qx = std::clamp(X + Dx, 0, In.W - 1), Qy = std::clamp(Y + Dy, 0, In.H - 1);
					if (In.SceneDist[size_t(Qy) * In.W + Qx] < BhDistanceCm * 0.999) { M = 0.0f; break; }
				}
				const size_t I = size_t(Y) * In.W + X;
				for (int K = 0; K < 3; ++K) { L0.Px[I * 4 + K] = In.Rgb[I * 3 + K] * M; }
				L0.Px[I * 4 + 3] = M;
			}
			Levels.push_back(std::move(L0));
			int Mips = 1; for (int D = std::max(In.W, In.H); D > 1; D >>= 1) { ++Mips; }
			for (int M = 1; M < Mips; ++M)
			{
				const FLevel& Sv = Levels.back();
				FLevel Dv; Dv.W = std::max(In.W >> M, 1); Dv.H = std::max(In.H >> M, 1); Dv.Px.resize(size_t(Dv.W) * Dv.H * 4);
				for (int Y = 0; Y < Dv.H; ++Y) for (int X = 0; X < Dv.W; ++X)
				{
					for (int K = 0; K < 4; ++K)
					{
						float Sum = 0.0f;
						for (int Oy = 0; Oy < 2; ++Oy) for (int Ox = 0; Ox < 2; ++Ox)
						{
							const int Sx = std::min(2 * X + Ox, Sv.W - 1), Sy = std::min(2 * Y + Oy, Sv.H - 1);
							Sum += Sv.Px[(size_t(Sy) * Sv.W + Sx) * 4 + K];
						}
						Dv.Px[(size_t(Y) * Dv.W + X) * 4 + K] = Sum * 0.25f;
					}
				}
				Levels.push_back(std::move(Dv));
			}
		}
		void At(int Level, double U, double V, float Out[4]) const
		{
			const FLevel& Lv = Levels[size_t(Level)];
			const double X = std::clamp(U * Lv.W - 0.5, 0.0, double(Lv.W - 1)), Y = std::clamp(V * Lv.H - 0.5, 0.0, double(Lv.H - 1));
			const int X0 = std::min(int(X), Lv.W - 1), Y0 = std::min(int(Y), Lv.H - 1), X1 = std::min(X0 + 1, Lv.W - 1), Y1 = std::min(Y0 + 1, Lv.H - 1);
			const double Fx = X - X0, Fy = Y - Y0;
			for (int K = 0; K < 4; ++K)
			{
				const double A = Lv.Px[(size_t(Y0) * Lv.W + X0) * 4 + K], B = Lv.Px[(size_t(Y0) * Lv.W + X1) * 4 + K];
				const double C = Lv.Px[(size_t(Y1) * Lv.W + X0) * 4 + K], D = Lv.Px[(size_t(Y1) * Lv.W + X1) * 4 + K];
				Out[K] = float((A * (1.0 - Fx) + B * Fx) * (1.0 - Fy) + (C * (1.0 - Fx) + D * Fx) * Fy);
			}
		}
		/** Mip 0's coverage (1: all sky) at a continuous pixel position. */
		float Alpha(double Px, double Py) const { float S[4]; At(0, Px / Levels[0].W, Py / Levels[0].H, S); return S[3]; }

		/** D-279: the sky for a ray whose origin (Sx, Sy) may be covered, this pixel (Ox, Oy) being
		 *  sky: the covered stretch of the line towards this pixel found (bisection), the sample
		 *  mirrored out of it by its depth (continuous in the position: no contours; stars, not a
		 *  blur); still covered there: the pyramid's fill at the edge. */
		void Behind(double Sx, double Sy, double Ox, double Oy, float Out[3]) const
		{
			if (Alpha(Sx, Sy) >= 0.999f) { Sample(Sx, Sy, Out); return; }
			double Lo = 0.0, Hi = 1.0;
			for (int I = 0; I < 7; ++I)
			{
				const double M = 0.5 * (Lo + Hi);
				if (Alpha(Sx + (Ox - Sx) * M, Sy + (Oy - Sy) * M) >= 0.999f) { Hi = M; } else { Lo = M; }
			}
			const double Bx = Sx + (Ox - Sx) * Hi, By = Sy + (Oy - Sy) * Hi;
			const double Mx = std::clamp(2.0 * Bx - Sx, 0.5, Levels[0].W - 0.5), My = std::clamp(2.0 * By - Sy, 0.5, Levels[0].H - 0.5);
			if (Alpha(Mx, My) >= 0.999f) { Sample(Mx, My, Out); } else { Sample(Bx, By, Out); }
		}

		/** The sky at a continuous pixel position (centres at +0.5) of the full-resolution image. */
		void Sample(double Px, double Py, float Out[3]) const
		{
			const double U = Px / Levels[0].W, V = Py / Levels[0].H;
			float S0[4]; At(0, U, V, S0);
			if (S0[3] >= 0.999f) { for (int K = 0; K < 3; ++K) { Out[K] = S0[K] / S0[3]; } return; }
			float Acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (size_t L = 1; L < Levels.size(); ++L)
			{
				float Sl[4]; At(int(L), U, V, Sl);
				const float W = 1.0f - Acc[3];
				for (int K = 0; K < 4; ++K) { Acc[K] += Sl[K] * W; }
				if (Acc[3] > 0.98f) { break; }
			}
			for (int K = 0; K < 3; ++K) { Out[K] = S0[K] + (Acc[3] > 1.0e-5f ? Acc[K] / Acc[3] : 0.0f) * (1.0f - S0[3]); }
		}
	};

	/** The lens pass over In (the scene with its sprites and volume) into Out (same size). */
	inline void ApplyLens(const FImage& In, const FCamera& C, const FLensView& L, FImage& Out, FLensStats* Stats = nullptr)
	{
		namespace H = UniHlsl;
		Out = In;
		const FSkyPyramid Sky(In, L.BhDistanceCm);   // D-279
		const double Len = std::sqrt(double(L.CamRs.x) * L.CamRs.x + double(L.CamRs.y) * L.CamRs.y + double(L.CamRs.z) * L.CamRs.z);
		const FV3 BhDir = Len > 0.0 ? FV3{ -L.CamRs.x / Len, -L.CamRs.y / Len, -L.CamRs.z / Len } : FV3{ 1, 0, 0 };
		const double RegionCos = std::cos(std::min(L.RegionRad, 3.14159265));
		const double Focal = C.FocalPx();
		const H::float3 F(float(C.Forward.X), float(C.Forward.Y), float(C.Forward.Z));
		const H::float3 R(float(C.Right.X), float(C.Right.Y), float(C.Right.Z));
		const H::float3 U(float(C.Up.X), float(C.Up.Y), float(C.Up.Z));
		const H::float2 Tan(float(C.TanX()), float(C.TanY()));
		// The point's pixel (the black hole's own direction, unlensed).
		const double Fb = Dot(BhDir, C.Forward);
		const bool PointOn = Fb > 1.0e-4 && (L.PointCore[0] + L.PointCore[1] + L.PointCore[2]) > 0.0;
		const double PbX = PointOn ? (Dot(BhDir, C.Right) / (Fb * C.TanX()) * 0.5 + 0.5) * C.Width : 0.0;
		const double PbY = PointOn ? (0.5 - Dot(BhDir, C.Up) / (Fb * C.TanY()) * 0.5) * C.Height : 0.0;
		const double Sc0 = std::max(L.PointCoreSigmaPx, 0.3), Sh0 = std::max(L.PointHaloSigmaPx, 0.3);
		for (int Y = 0; Y < C.Height; ++Y)
		{
			for (int X = 0; X < C.Width; ++X)
			{
				double CosF = 1.0;
				const H::float3 D = PixelRay(C, X + 0.5, Y + 0.5, CosF);
				const size_t Pi = size_t(Y) * C.Width + X;
				if (double(D.x) * BhDir.X + double(D.y) * BhDir.Y + double(D.z) * BhDir.Z < RegionCos || In.SceneDist[Pi] < L.BhDistanceCm * 0.999)
				{
					if (Stats) { ++Stats->Copied; }
					continue;
				}
				const H::FUniLensResult Res = H::Uni_LensTrace(L.CamRs, D, L.Disc, L.MaxSteps);
				float Bg[3] = { 0.0f, 0.0f, 0.0f };
				if (Res.Captured < 0.5f && Res.Transmit > 0.0f)
				{
					const H::float2 N = H::Uni_LensSkyNdc(Res.Dir, F, R, U, Tan, true);
					const double Sx = std::clamp((double(N.x) * 0.5 + 0.5) * C.Width, 0.5, C.Width - 0.5), Sy = std::clamp((0.5 - double(N.y) * 0.5) * C.Height, 0.5, C.Height - 0.5);
					// D-279: as UniLens.usf: the sky-only pyramid fills an origin covered by the foreground.
					Sky.Behind(Sx, Sy, X + 0.5, Y + 0.5, Bg);
				}
				const double Omega = CosF * CosF * CosF / (Focal * Focal);
				const double Sc = L.DiscScale * Omega;
				float* P = &Out.Rgb[Pi * 3];
				const float Dc[3] = { Res.Disc.x, Res.Disc.y, Res.Disc.z };
				double Pt[3] = { 0.0, 0.0, 0.0 };
				if (PointOn)
				{
					const double D2 = (X + 0.5 - PbX) * (X + 0.5 - PbX) + (Y + 0.5 - PbY) * (Y + 0.5 - PbY);
					const double Gc = std::exp(-0.5 * D2 / (Sc0 * Sc0)) / (6.283185307179586 * Sc0 * Sc0), Gh = std::exp(-0.5 * D2 / (Sh0 * Sh0)) / (6.283185307179586 * Sh0 * Sh0);
					for (int K = 0; K < 3; ++K) { Pt[K] = L.PointCore[K] * Gc + L.PointHalo[K] * Gh; }
				}
				const H::float3 Ds = H::Uni_BhDiscSoften(H::float3(float(Dc[0] * Sc), float(Dc[1] * Sc), float(Dc[2] * Sc)), L.DiscKnee);   // D-274
				const float Dsk[3] = { Ds.x, Ds.y, Ds.z };
				for (int K = 0; K < 3; ++K)
				{
					const double Dk = L.DiscKnee > 0.0f ? double(Dsk[K]) : double(Dc[K]) * Sc;
					P[K] = float(std::min(double(Bg[K]) * double(Res.Transmit) + Dk + Pt[K], 6.0e4));
					if (Stats) { Stats->DiscFlux[K] += Dk; Stats->PointFlux[K] += Pt[K]; }
				}
				if (Stats)
				{
					++Stats->Traced;
					if (Res.Captured > 0.5f) { ++Stats->Captured; }
					if (Res.Steps >= float(L.MaxSteps)) { ++Stats->Capped; }
					Stats->StepSum += Res.Steps;
					Stats->StepMax = std::max(Stats->StepMax, double(Res.Steps));
				}
			}
		}
	}

	/** D-273: the disc's light seen from far (Dx R_s away, cos i = Mu to its axis; the axis z) x Dx^2,
	 *  at radiance scale 1 (Uni_BhDiscLight units x sr x R_s^2): every ray of a log-polar grid
	 *  about the black hole (Nr radii from 0.4 R_s to 1.3 x the outer radius, Nps azimuths)
	 *  traced, lensing and shifts and all, each x its solid angle. */
	inline void DiscFarLight(float PeakK, double Mu, double Dx, int Nr, int Nps, double OutRgb[3], UniHlsl::uint Palette = 0u, float PaletteMix = 0.0f)
	{
		namespace H = UniHlsl;
		H::FUniLensDisc D{};
		D.Axis = H::float3(0.0f, 0.0f, 1.0f); D.PeakK = PeakK; D.OuterX = H::Uni_BhDiscOuterX(PeakK); D.bDisc = 1.0f;
		D.Palette = float(Palette); D.PaletteMix = PaletteMix;   // D-280
		const double S = std::sqrt(std::max(1.0 - Mu * Mu, 0.0));
		const FV3 Fw{ -S, 0.0, -Mu };
		const FV3 E1 = Norm(Cross(Fw, std::fabs(Mu) > 0.999 ? FV3{ 1.0, 0.0, 0.0 } : FV3{ 0.0, 0.0, 1.0 })), E2 = Cross(E1, Fw);
		const H::float3 Cam(float(Dx * S), 0.0f, float(Dx * Mu));
		const double ThLo = 0.4 / Dx, ThHi = 1.3 * double(D.OuterX) / Dx, LnSpan = std::log(ThHi / ThLo);
		OutRgb[0] = OutRgb[1] = OutRgb[2] = 0.0;
		for (int Ir = 0; Ir < Nr; ++Ir)
		{
			const double Th = ThLo * std::exp(LnSpan * (Ir + 0.5) / Nr);
			const double Dth = Th * LnSpan / Nr, Ct = std::cos(Th), St = std::sin(Th);
			for (int Ip = 0; Ip < Nps; ++Ip)
			{
				const double Ps = 6.283185307179586 * (Ip + 0.5) / Nps, Cp = std::cos(Ps), Sp = std::sin(Ps);
				const FV3 Dd{ Fw.X * Ct + (E1.X * Cp + E2.X * Sp) * St, Fw.Y * Ct + (E1.Y * Cp + E2.Y * Sp) * St, Fw.Z * Ct + (E1.Z * Cp + E2.Z * Sp) * St };
				const H::FUniLensResult R = H::Uni_LensTrace(Cam, H::float3(float(Dd.X), float(Dd.Y), float(Dd.Z)), D, H::UNI_LENS_MAX_STEPS);
				const double W = St * Dth * (6.283185307179586 / Nps) * Dx * Dx;
				OutRgb[0] += R.Disc.x * W; OutRgb[1] += R.Disc.y * W; OutRgb[2] += R.Disc.z * W;
			}
		}
	}

	inline double LumOf(const double Rgb[3]) { return 0.2126 * Rgb[0] + 0.7152 * Rgb[1] + 0.0722 * Rgb[2]; }
}
