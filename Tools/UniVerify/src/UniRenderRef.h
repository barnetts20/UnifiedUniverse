#pragma once

// UniRenderRef.h
// CPU reference renderer for Tier 3. Projects every resident object through the shared
// project-and-cull core (UniProject.ush) exactly as the GPU pass will, splats sprites, draws
// promoted bodies as true-scale spheres into a "main pass" colour + distance buffer, and
// applies the sprite pixel shader's scene-depth test (Uni_SceneDepthKeep). Verification only.

#include "UniGenRef.h"
#include "UniScaleLadder.h"
#include "UniShaderCores.h"
#include "UniParallax.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace UniRender
{
	using namespace UniCore;
	namespace H = UniHlsl;

	struct FV3 { double X = 0, Y = 0, Z = 0; };
	inline FV3 operator+(FV3 A, FV3 B) { return { A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
	inline FV3 operator-(FV3 A, FV3 B) { return { A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }
	inline FV3 operator*(FV3 A, double S) { return { A.X * S, A.Y * S, A.Z * S }; }
	inline double Dot(FV3 A, FV3 B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
	inline FV3 Cross(FV3 A, FV3 B) { return { A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X }; }
	inline FV3 Norm(FV3 A) { const double L = std::sqrt(Dot(A, A)); return A * (1.0 / L); }

	struct FCamera
	{
		FCoord Coord;
		FVec3f Frac;
		FV3 Forward{ 1, 0, 0 }, Right{ 0, 1, 0 }, Up{ 0, 0, 1 };
		double FovX = 1.5707963267948966;   // 90 degrees
		int Width = 960, Height = 540;
		double DeltaMag = 0.0;
		u32 SystemArchetypeMask = 1u << 1;   // the demo's star archetype (UniDemoScenes kStar) has systems (D-150)
		u32 DiscArchetypeMask = 0u;          // D-183: archetypes drawn as inclined ellipses (0: every sprite round, as before)
		u32 FixedDiscSeeds[4] = { 0u, 0u, 0u, 0u };   // D-183: fixed-orientation anchors (disc normal world z)
		float HaloChroma = 1.0f;   // D-185: halo chroma; 1 here by default (sprites as before)

		void LookAlong(FV3 F, FV3 WorldUp = { 0, 0, 1 })
		{
			Forward = Norm(F);
			if (std::fabs(Dot(Forward, WorldUp)) > 0.99) { WorldUp = { 0, 1, 0 }; }
			Right = Norm(Cross(Forward, WorldUp));
			Up = Cross(Right, Forward);
		}
		double TanX() const { return std::tan(FovX * 0.5); }
		double TanY() const { return TanX() * double(Height) / double(Width); }
		double FocalPx() const { return double(Width) * 0.5 / TanX(); }

		H::FUniView View() const
		{
			H::FUniView V;
			V.Forward = H::float3(float(Forward.X), float(Forward.Y), float(Forward.Z));
			V.Right = H::float3(float(Right.X), float(Right.Y), float(Right.Z));
			V.Up = H::float3(float(Up.X), float(Up.Y), float(Up.Z));
			V.TanHalfFovX = float(TanX());
			V.TanHalfFovY = float(TanY());
			V.LimitingMag = H::Uni_LimitingMag(float(DeltaMag));
			V.SurfaceLimit = H::UNI_MU_REF + float(DeltaMag);
			V.CameraFraction = H::float3(Frac.X, Frac.Y, Frac.Z);
			V.ExposureMag = V.LimitingMag - 1.0f;
			V.ThetaPixel = float(1.0 / FocalPx());
			V.SystemArchetypeMask = SystemArchetypeMask;
			V.DiscArchetypeMask = DiscArchetypeMask;
			V.FixedDiscSeeds = H::uint4(FixedDiscSeeds[0], FixedDiscSeeds[1], FixedDiscSeeds[2], FixedDiscSeeds[3]);
			V.WarpVel = H::float3(0, 0, 0); V.WarpBeta = 0.0f; V.WarpDoppler = 0.0f; V.WarpBeaming = 0.0f; V.WarpPad0 = 0.0f; V.WarpPad1 = 0.0f;   // D-190: no warp optics
			return H::Uni_SpriteDefaults(V);   // D-156 sprite style
		}
	};

	/** Exact two's-complement cell delta CellOrigin - Camera, as the CPU uploads it (D-104). */
	inline void CellDelta(const FCellKey& K, const FCoord& Cam, H::uint4 Out[3])
	{
		const FDelta D = Sub(CellOrigin(K), Cam);
		for (int A = 0; A < 3; ++A) { Out[A] = ToUint4(D.TwosComplement(A)); }
	}

	struct FImage
	{
		int W = 0, H = 0;
		std::vector<float> Rgb;          // HDR, exposure units
		std::vector<double> SceneDist;   // main-pass RENDER-space distance per pixel (inf = sky), as the engine's depth buffer holds it (D-146)
		FImage(int InW, int InH) : W(InW), H(InH), Rgb(size_t(InW) * InH * 3, 0.0f), SceneDist(size_t(InW) * InH, std::numeric_limits<double>::infinity()) {}

		void Add(int X, int Y, float R, float G, float B)
		{
			if (X < 0 || Y < 0 || X >= W || Y >= H) { return; }
			float* P = &Rgb[(size_t(Y) * W + X) * 3];
			P[0] += R; P[1] += G; P[2] += B;
		}

		/** Tonemapped PPM: 1 - exp(-I), sRGB-ish gamma. */
		bool WritePpm(const std::string& Path) const
		{
			FILE* F = std::fopen(Path.c_str(), "wb");
			if (!F) { return false; }
			std::fprintf(F, "P6\n%d %d\n255\n", W, H);
			std::vector<unsigned char> Row(size_t(W) * 3);
			for (int Y = 0; Y < H; ++Y)
			{
				for (int X = 0; X < W * 3; ++X)
				{
					const double V = 1.0 - std::exp(-double(Rgb[size_t(Y) * W * 3 + X]));
					Row[size_t(X)] = (unsigned char)std::lround(255.0 * std::pow(std::max(0.0, std::min(1.0, V)), 1.0 / 2.2));
				}
				std::fwrite(Row.data(), 1, Row.size(), F);
			}
			std::fclose(F);
			return true;
		}
	};

	struct FFrameStats
	{
		size_t Resident = 0, Visible = 0, Promoted = 0, Occluded = 0;
		std::map<int, size_t> VisibleByLevel;
		double BrightestMag = 99.0;
	};

	/** Sprite splat: the core, a normalised Gaussian whose sigma is the sprite's angular radius
	 *  in pixels (at least ~0.6 px), plus the D-156 halo, so the total deposited equals the
	 *  sprite's radiance x (1 + HaloRatio). */
	inline bool Splat(FImage& Img, const FCamera& Cam, const H::FUniSprite& S)
	{
		const double Px = (double(S.NdcX) * 0.5 + 0.5) * Img.W;
		const double Py = (0.5 - double(S.NdcY) * 0.5) * Img.H;
		const double Sigma = std::max(0.6, double(S.AngularRadius) * Cam.FocalPx() * 0.5);
		const float DiscFrac = H::Uni_SpriteDiscFrac(S, float(Cam.FocalPx() * 0.5));   // D-148, in this renderer's pixel convention
		const double HaloRatio = double(S.HaloRatio);
		const double HaloSigma = HaloRatio > 0.0 ? std::max(Sigma, double(S.HaloRadius) * Cam.FocalPx() * 0.5) : 0.0;
		// Kernel radius: out to where the tails fall below 1e-3 exposure units, so a saturated
		// bright star is not truncated into a square. Capped at 160 px.
		const double Lum = double(std::max(S.Radiance.x, std::max(S.Radiance.y, S.Radiance.z)));
		const double Peak = Lum / (6.283185307179586 * Sigma * Sigma);
		double Reach = Sigma * std::sqrt(2.0 * std::log(std::max(Peak / 1.0e-3, 2.0)));
		if (HaloSigma > 0.0)
		{
			const double HPeak = Lum * HaloRatio / (6.283185307179586 * HaloSigma * HaloSigma);
			Reach = std::max(Reach, HaloSigma * std::sqrt(2.0 * std::log(std::max(HPeak / 1.0e-3, 2.0))));
		}
		// D-185: the halo's chroma.
		const bool bGlareColour = HaloRatio > 0.0 && Cam.HaloChroma != 1.0f;
		const int Rad = int(std::min(160.0, std::ceil(std::max({ 3.0 * Sigma, 3.0 * HaloSigma, Reach, double(DiscFrac) * Sigma + 2.0 }))));
		const float CoreW = H::Uni_SpriteCoreWeight(S);   // D-158: 0 for a promoted star's glare
		// D-183: a disc galaxy's core is an ellipse in the quad's (Side, Up) basis, built from the
		// sprite's direction as the vertex shader builds it; a pixel offset maps into it through
		// the camera's right / up (image y runs down).
		const float Q = H::Uni_SpriteEllipseQ(S, float(Sigma));
		double Sx[2] = { 1.0, 0.0 }, Uy[2] = { 0.0, 1.0 };
		if (Q < 1.0f)
		{
			const double Rl = std::sqrt(double(S.RenderPos.x) * S.RenderPos.x + double(S.RenderPos.y) * S.RenderPos.y + double(S.RenderPos.z) * S.RenderPos.z);
			const FV3 Dir = FV3{ S.RenderPos.x / Rl, S.RenderPos.y / Rl, S.RenderPos.z / Rl };
			const FV3 Side = Norm(Cross(Dir, Cam.Up + Cam.Right * 1.0e-3));
			const FV3 Up = Cross(Side, Dir);
			Sx[0] = Dot(Cam.Right, Side); Sx[1] = -Dot(Cam.Up, Side);   // a pixel step's component along Side: (x right, y down)
			Uy[0] = Dot(Cam.Right, Up); Uy[1] = -Dot(Cam.Up, Up);
		}
		auto Base = [&](double Ex, double Ey)
		{
			if (!(Q < 1.0f)) { return double(H::Uni_SpriteProfileHalo(float(std::sqrt(Ex * Ex + Ey * Ey)), float(Sigma), DiscFrac, float(HaloSigma), float(HaloRatio), CoreW)); }   // round: as before D-183, bit for bit
			const double U = Ex * Sx[0] + Ey * Sx[1], V = Ex * Uy[0] + Ey * Uy[1];
			return double(H::Uni_SpriteProfileEllipse(float(U), float(V), float(Sigma), DiscFrac, float(HaloSigma), float(HaloRatio), CoreW, S.EllipseAngle, Q));
		};
		// The halo alone (core weight 0): drawn in the chroma-scaled colour.
		auto Glare = [&](double Ex, double Ey)
		{
			return double(H::Uni_SpriteProfileHalo(float(std::sqrt(Ex * Ex + Ey * Ey)), float(Sigma), DiscFrac, float(HaloSigma), float(HaloRatio), 0.0f));
		};
		auto Profile = [&](double Ex, double Ey) { return Base(Ex, Ey); };
		const H::float3 HaloRad = bGlareColour ? H::Uni_SpriteChroma(S.Radiance, Cam.HaloChroma) : S.Radiance;
		const int Cx = int(std::floor(Px)), Cy = int(std::floor(Py));
		// Scene-depth test at the sprite centre pixel, as the pixel shader does per pixel.
		bool bAnyKept = false;
		double Norm = 0.0;
		for (int Dy = -Rad; Dy <= Rad; ++Dy) for (int Dx = -Rad; Dx <= Rad; ++Dx)
		{
			const double Ex = (Cx + Dx + 0.5) - Px, Ey = (Cy + Dy + 0.5) - Py;
			Norm += Profile(Ex, Ey);
		}
		for (int Dy = -Rad; Dy <= Rad; ++Dy) for (int Dx = -Rad; Dx <= Rad; ++Dx)
		{
			const int X = Cx + Dx, Y = Cy + Dy;
			if (X < 0 || Y < 0 || X >= Img.W || Y >= Img.H) { continue; }
			if (!H::Uni_SceneDepthKeep(S.TrueDistance, float(std::min(Img.SceneDist[size_t(Y) * Img.W + X], 3.0e38)))) { continue; }
			const double Ex = (X + 0.5) - Px, Ey = (Y + 0.5) - Py;
			if (!bGlareColour)
			{
				const double Wt = Profile(Ex, Ey) / Norm * (double(CoreW) + HaloRatio);
				Img.Add(X, Y, float(S.Radiance.x * Wt), float(S.Radiance.y * Wt), float(S.Radiance.z * Wt));
			}
			else
			{
				const double K = (double(CoreW) + HaloRatio) / Norm;
				const double Wa = Profile(Ex, Ey) * K, Wg = Glare(Ex, Ey) * K, Wc = std::max(Wa - Wg, 0.0);
				Img.Add(X, Y, float(S.Radiance.x * Wc + HaloRad.x * Wg), float(S.Radiance.y * Wc + HaloRad.y * Wg), float(S.Radiance.z * Wc + HaloRad.z * Wg));
			}
			bAnyKept = true;
		}
		return bAnyKept;
	}

	/** True-scale sphere into the main pass (colour + distance), by ray casting in double from
	 *  the exact camera-relative centre. */
	inline void DrawPromotedSphere(FImage& Img, const FCamera& Cam, FV3 Centre, double Radius, H::float3 Rgb)
	{
		const double Dist = std::sqrt(Dot(Centre, Centre));
		if (Dist <= Radius) { return; }
		const double Ang = std::asin(Radius / Dist);
		const FV3 Dir = Centre * (1.0 / Dist);
		const double Fz = Dot(Dir, Cam.Forward);
		if (Fz <= std::cos(Cam.FovX)) { return; }
		const double Nx = Dot(Dir, Cam.Right) / (Fz * Cam.TanX()), Ny = Dot(Dir, Cam.Up) / (Fz * Cam.TanY());
		const double Px = (Nx * 0.5 + 0.5) * Img.W, Py = (0.5 - Ny * 0.5) * Img.H;
		const double RadPx = std::tan(Ang) * Cam.FocalPx() / std::max(Fz, 1e-3) + 2.0;
		const int X0 = std::max(0, int(Px - RadPx)), X1 = std::min(Img.W - 1, int(Px + RadPx));
		const int Y0 = std::max(0, int(Py - RadPx)), Y1 = std::min(Img.H - 1, int(Py + RadPx));
		for (int Y = Y0; Y <= Y1; ++Y) for (int X = X0; X <= X1; ++X)
		{
			const double Ndx = ((X + 0.5) / Img.W - 0.5) * 2.0 * Cam.TanX(), Ndy = (0.5 - (Y + 0.5) / Img.H) * 2.0 * Cam.TanY();
			const FV3 Ray = Norm(Cam.Forward + Cam.Right * Ndx + Cam.Up * Ndy);
			const double B = Dot(Ray, Centre);
			const double C = Dot(Centre, Centre) - Radius * Radius;
			const double Disc = B * B - C;
			if (Disc < 0.0 || B <= 0.0) { continue; }
			// Placed as the parallax component places an actor (D-146): real space while its surface
			// is inside r0, otherwise scaled about the eye by f(d)/d. The depth buffer holds the
			// render-space distance.
			const double Dc = std::sqrt(Dot(Centre, Centre));
			const double Scale = ((Dc - Radius) < double(H::UNI_R0_CM) && Dc < UniCore::ParallaxMaxRealSpaceCm) ? 1.0 : UniCore::CompressD(Dc) / Dc;
			const double T = (B - std::sqrt(Disc)) * Scale;
			double& Sd = Img.SceneDist[size_t(Y) * Img.W + X];
			if (T < Sd)
			{
				Sd = T;
				float* P = &Img.Rgb[(size_t(Y) * Img.W + X) * 3];
				P[0] = Rgb.x * 4.0f; P[1] = Rgb.y * 4.0f; P[2] = Rgb.z * 4.0f;   // emissive body, overwrites
			}
		}
	}

	/** Render every resident object of a pipeline. Promotion is immediate in the reference (the
	 *  GPU path has 1-2 frames of readback latency, covered by the deferred handoff, UE-5). */
	inline FFrameStats RenderFrame(const FRefPipeline& P, const FCamera& Cam, FImage& Img)
	{
		FFrameStats St;
		const H::FUniView V = Cam.View();
		struct FPending { H::FUniSprite S; int Level; };
		std::vector<FPending> Sprites;
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float Fade, u64)
		{
			++St.Resident;
			H::uint4 D[3];
			CellDelta(K, Cam.Coord, D);
			const H::FUniSprite S = H::Uni_ProjectObject(D[0], D[1], D[2], u32(K.Level), Fade, UniCore::ToHlsl(O), V, false);
			if (S.Flags & H::UNI_SPRITE_PROMOTE_CANDIDATE)
			{
				// Main pass: point-like bodies as true-scale spheres; extended objects hand off to
				// volumetrics (not in the prototype), so they draw nothing here.
				if (!((O.A[3] >> 16) & H::UNI_OBJ_FLAG_EXTENDED))
				{
					const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
					const FVec3d Rel = ToDouble(Sub(ObjectCoord(K, Off), Cam.Coord));
					const FV3 C{ Rel.X - Cam.Frac.X, Rel.Y - Cam.Frac.Y, Rel.Z - Cam.Frac.Z };
					DrawPromotedSphere(Img, Cam, C, std::exp2(double(H::Uni_ObjectRadiusLog2(UniCore::ToHlsl(O)))), H::Uni_UnpackColour(O.B[3]));
					// D-158: a promoted point source keeps its glare around the actor.
					FObjectGPU Op = O;
					Op.A[3] |= (H::UNI_OBJ_FLAG_PROMOTED << 16);
					const H::FUniSprite Sg = H::Uni_ProjectObject(D[0], D[1], D[2], u32(K.Level), Fade, UniCore::ToHlsl(Op), V, true);
					if (Sg.Flags & H::UNI_SPRITE_VISIBLE) { Sprites.push_back({ Sg, K.Level }); }
				}
				++St.Promoted;
				return;
			}
			if (S.Flags & H::UNI_SPRITE_VISIBLE) { Sprites.push_back({ S, K.Level }); }
		});
		for (const FPending& Sp : Sprites)
		{
			if (Splat(Img, Cam, Sp.S)) { ++St.Visible; ++St.VisibleByLevel[Sp.Level]; }
			else { ++St.Occluded; }
		}
		return St;
	}
}
