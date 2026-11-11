// UniSplatRef.h
// D-223: the CPU reference of the sprite splat path (UniSprite.usf SplatCS, SplatGatherCS,
// SplatResolvePS) and of the quad path it replaces for plain point sprites (MainVS / MainPS for a
// round core with no streak). Both use the shared core (UniProject.ush) the shaders use, pass for
// pass, so the offline checks measure what the engine draws.
//
// Conventions (as the engine's): output pixel centres at +0.5; a sprite's centre in output pixels
// from its NDC, x right, y down; the buffers cover the view plus UNI_SPLAT_PAD pixels each side.

#pragma once

#include "UniRenderRef.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace UniRender
{
	/** The quad path for a round sprite with no streak: the sprite pass's reach (D-188: at least
	 *  Extent footprints, the core and halo out to the floor) and per-pixel profile with its soft
	 *  edge (Uni_SpriteProfileHalo x Uni_SpriteEdgeWindow), radiance x InScale. The quad is square
	 *  in the sprite's (Side, Up) basis; for a round profile only its corners differ from the
	 *  screen-aligned square used here, where the profile is under the floor. */
	inline void QuadSprite(int W, int Ht, std::vector<float>& Rgb, const H::FUniSprite& S, float InPpr, float InScale, float InFloor, float InExtent)
	{
		const double Px = (double(S.NdcX) * 0.5 + 0.5) * W;
		const double Py = (0.5 - double(S.NdcY) * 0.5) * Ht;
		const float Sigma = std::max(S.AngularRadius * InPpr, 0.5f);
		const float FootPx = std::max(H::Uni_SpriteFootprint(S) * InPpr, 0.5f);
		const float HaloPx = S.HaloRatio > 0.0f ? std::max(S.HaloRadius * InPpr, 0.5f) : 0.0f;
		const float Disc = H::Uni_SpriteDiscFrac(S, InPpr);
		const float CoreW = H::Uni_SpriteCoreWeight(S);
		const H::float3 Rad = S.Radiance * InScale;
		const float Lum = H::Uni_Luminance(Rad);
		const float Reach = std::max(H::Uni_SpriteReachPx(Lum, Sigma, Disc, HaloPx, S.HaloRatio, CoreW, InFloor, InExtent), InExtent * FootPx);
		const int X0 = std::max(0, int(std::floor(Px - Reach))), X1 = std::min(W - 1, int(std::ceil(Px + Reach)));
		const int Y0 = std::max(0, int(std::floor(Py - Reach))), Y1 = std::min(Ht - 1, int(std::ceil(Py + Reach)));
		for (int Y = Y0; Y <= Y1; ++Y) for (int X = X0; X <= X1; ++X)
		{
			const float Ex = float(X + 0.5 - Px), Ey = float(Y + 0.5 - Py);
			if (std::abs(Ex) > Reach || std::abs(Ey) > Reach) { continue; }
			const float R = std::sqrt(Ex * Ex + Ey * Ey);
			const float Wt = H::Uni_SpriteProfileHalo(R, Sigma, Disc, HaloPx, S.HaloRatio, CoreW) * H::Uni_SpriteEdgeWindow(R, Reach);
			float* P = &Rgb[(size_t(Y) * W + X) * 3];
			P[0] += Rad.x * Wt; P[1] += Rad.y * Wt; P[2] += Rad.z * Wt;
		}
	}

	/** The splat path: fixed-point buffers (SplatCS deposits) and the gather (SplatGatherCS:
	 *  horizontal; SplatResolvePS: vertical, colour ratio x luminance shape). */
	struct FSplatRef
	{
		int W = 0, Ht = 0, Wp = 0, Hp = 0, Tw = 0, Th = 0;
		std::vector<uint32_t> Lum2x;   // (2 Hp) rows of (2 Wp)
		std::vector<uint32_t> Col;     // Hp rows of Wp, x 3
		std::vector<uint32_t> Occ;     // D-223 tiles: Th rows of Tw, 1 where the deposit wrote
		uint64_t Overflow = 0;         // adds that would wrap a uint32 (the GPU wraps; reported here)

		FSplatRef(int InW, int InHt) : W(InW), Ht(InHt), Wp(InW + 2 * H::UNI_SPLAT_PAD), Hp(InHt + 2 * H::UNI_SPLAT_PAD),
			Tw((Wp + H::UNI_SPLAT_TILE - 1) / H::UNI_SPLAT_TILE), Th((Hp + H::UNI_SPLAT_TILE - 1) / H::UNI_SPLAT_TILE),
			Lum2x(size_t(4) * Wp * Hp, 0u), Col(size_t(3) * Wp * Hp, 0u), Occ(size_t(Tw) * Th, 0u) {}

		/** The tile holding padded 1x pixel (X, Y). */
		size_t TileOf(int X, int Y) const { return size_t(Y / H::UNI_SPLAT_TILE) * Tw + size_t(X / H::UNI_SPLAT_TILE); }

		void Add(uint32_t& Dst, uint32_t V)
		{
			if (uint64_t(Dst) + V > 0xFFFFFFFFull) { ++Overflow; }
			Dst += V;
		}

		/** SplatCS for one sprite: its light (radiance x scale x transmittance) at its centre. */
		void Deposit(const H::FUniSprite& S, float InScale, float InT = 1.0f)
		{
			const float X = float((double(S.NdcX) * 0.5 + 0.5) * W) + float(H::UNI_SPLAT_PAD);
			const float Y = float((0.5 - double(S.NdcY) * 0.5) * Ht) + float(H::UNI_SPLAT_PAD);
			if (!(X >= 0.0f && Y >= 0.0f && X < float(Wp) && Y < float(Hp))) { return; }
			const H::float3 Rad = S.Radiance * (InScale * InT);
			const float L = H::Uni_Luminance(Rad);
			if (!(L > 0.0f)) { return; }
			// Luminance: bilinear into the 2x grid (subtexel centres at (i + 0.5) / 2).
			const float Qx = 2.0f * X - 0.5f, Qy = 2.0f * Y - 0.5f;
			const float Fx0 = std::floor(Qx), Fy0 = std::floor(Qy);
			const int I0 = int(Fx0), J0 = int(Fy0);
			const float Ax = Qx - Fx0, Ay = Qy - Fy0;
			for (int Dj = 0; Dj < 2; ++Dj) for (int Di = 0; Di < 2; ++Di)
			{
				const int I = I0 + Di, J = J0 + Dj;
				if (I < 0 || J < 0 || I >= 2 * Wp || J >= 2 * Hp) { continue; }
				const float Wt = (Di ? Ax : 1.0f - Ax) * (Dj ? Ay : 1.0f - Ay);
				const uint32_t U = H::Uni_SplatEncode(L * Wt);
				if (U != 0u) { Add(Lum2x[size_t(J) * (2 * Wp) + I], U); Occ[TileOf(I / 2, J / 2)] = 1u; }
			}
			// Colour: the nearest 1x texel.
			const int Cx = std::min(int(X), Wp - 1), Cy = std::min(int(Y), Hp - 1);
			uint32_t* C = &Col[(size_t(Cy) * Wp + Cx) * 3];
			Occ[TileOf(Cx, Cy)] = 1u;
			Add(C[0], H::Uni_SplatEncode(Rad.x));
			Add(C[1], H::Uni_SplatEncode(Rad.y));
			Add(C[2], H::Uni_SplatEncode(Rad.z));
		}

		/** SplatTilesCS: each tile's level from the marked tiles around it (Chebyshev distance):
		 *  2 within one tile (gathered, resolved), 1 within two (gathered rows written as zeros),
		 *  0 otherwise (skipped). */
		std::vector<uint8_t> TileLevels() const
		{
			std::vector<uint8_t> Lv(Occ.size(), 0u);
			for (int Ty = 0; Ty < Th; ++Ty) for (int Tx = 0; Tx < Tw; ++Tx)
			{
				int D = 3;
				for (int Dy = -2; Dy <= 2; ++Dy) for (int Dx = -2; Dx <= 2; ++Dx)
				{
					const int X = Tx + Dx, Y = Ty + Dy;
					if (X < 0 || Y < 0 || X >= Tw || Y >= Th || Occ[size_t(Y) * Tw + X] == 0u) { continue; }
					D = std::min(D, std::max(std::abs(Dx), std::abs(Dy)));
				}
				Lv[size_t(Ty) * Tw + Tx] = uint8_t(D <= 1 ? 2 : D == 2 ? 1 : 0);
			}
			return Lv;
		}

		/** SplatZeroCS: the level-2 tiles' deposits zeroed and every mark reset (the buffers
		 *  persist between frames on the GPU). */
		void ZeroTouched(const std::vector<uint8_t>& InLv)
		{
			for (int Y = 0; Y < Hp; ++Y) for (int X = 0; X < Wp; ++X)
			{
				if (InLv[TileOf(X, Y)] != 2u) { continue; }
				for (int K = 0; K < 3; ++K) { Col[(size_t(Y) * Wp + X) * 3 + K] = 0u; }
				for (int Dj = 0; Dj < 2; ++Dj) for (int Di = 0; Di < 2; ++Di) { Lum2x[size_t(2 * Y + Dj) * (2 * Wp) + 2 * X + Di] = 0u; }
			}
			std::fill(Occ.begin(), Occ.end(), 0u);
		}

		/** The gather into an RGB image of the view (W x Ht), added to InOutRgb. InCorePx: the
		 *  core sigma (px), as the shaders take it. InLv: the tile levels (tiled, as the engine
		 *  runs it: rows the gather skips hold NaN, so a resolve reading one shows); null: every
		 *  tile (the untiled reference). */
		void Resolve(float InCorePx, std::vector<float>& InOutRgb, const std::vector<uint8_t>* InLv = nullptr) const
		{
			const float Sb = H::Uni_SplatBlurSigma(std::max(InCorePx, 0.5f));
			const int R = H::Uni_SplatReach(Sb);
			const float N2 = H::Uni_SplatNorm2x(Sb, R);
			const float Sc = std::max(InCorePx, 0.5f);
			const int Rc = H::Uni_SplatReach(Sc);
			const float N1 = H::Uni_SplatNorm1x(Sc, Rc);
			// SplatGatherCS: luminance rows of the 2x grid gathered to 1x columns; colour rows at 1x.
			const float Skipped = InLv ? std::numeric_limits<float>::quiet_NaN() : 0.0f;
			std::vector<float> Lh(size_t(Wp) * 2 * Hp, Skipped), Ch(size_t(Wp) * Hp * 3, Skipped);
			auto Level = [&](int X, int Y) { return InLv ? int((*InLv)[TileOf(X, Y)]) : 2; };
			for (int J = 0; J < 2 * Hp; ++J) for (int X = 0; X < Wp; ++X)
			{
				const int Lj = Level(X, J / 2);
				if (Lj == 0) { continue; }
				if (Lj == 1) { Lh[size_t(J) * Wp + X] = 0.0f; continue; }
				float A = 0.0f;
				for (int I = 2 * X - 2 * R - 1; I <= 2 * X + 2 * R + 2; ++I)
				{
					if (I < 0 || I >= 2 * Wp) { continue; }
					const uint32_t U = Lum2x[size_t(J) * (2 * Wp) + I];
					if (U == 0u) { continue; }
					A += H::Uni_SplatDecode(U) * H::Uni_SplatTap(float(X) + 0.5f - (float(I) + 0.5f) * 0.5f, Sb);
				}
				Lh[size_t(J) * Wp + X] = A / N2;
			}
			for (int Y = 0; Y < Hp; ++Y) for (int X = 0; X < Wp; ++X)
			{
				const int Ly = Level(X, Y);
				if (Ly == 0) { continue; }
				if (Ly == 1) { for (int K = 0; K < 3; ++K) { Ch[(size_t(Y) * Wp + X) * 3 + K] = 0.0f; } continue; }
				float A[3] = { 0, 0, 0 };
				for (int I = X - Rc; I <= X + Rc; ++I)
				{
					if (I < 0 || I >= Wp) { continue; }
					const uint32_t* C = &Col[(size_t(Y) * Wp + I) * 3];
					if ((C[0] | C[1] | C[2]) == 0u) { continue; }
					const float T = H::Uni_SplatTap(float(X - I), Sc);
					for (int K = 0; K < 3; ++K) { A[K] += H::Uni_SplatDecode(C[K]) * T; }
				}
				for (int K = 0; K < 3; ++K) { Ch[(size_t(Y) * Wp + X) * 3 + K] = A[K] / N1; }
			}
			// SplatResolvePS: columns, then the colour ratio x the luminance's shape.
			for (int Yv = 0; Yv < Ht; ++Yv) for (int Xv = 0; Xv < W; ++Xv)
			{
				const int X = Xv + H::UNI_SPLAT_PAD, Y = Yv + H::UNI_SPLAT_PAD;
				if (Level(X, Y) != 2) { continue; }
				float L = 0.0f;
				for (int J = 2 * Y - 2 * R - 1; J <= 2 * Y + 2 * R + 2; ++J)
				{
					if (J < 0 || J >= 2 * Hp) { continue; }
					L += Lh[size_t(J) * Wp + X] * H::Uni_SplatTap(float(Y) + 0.5f - (float(J) + 0.5f) * 0.5f, Sb);
				}
				L /= N2;
				if (!(L > 0.0f)) { continue; }
				H::float3 C(0.0f, 0.0f, 0.0f);
				for (int J = Y - Rc; J <= Y + Rc; ++J)
				{
					if (J < 0 || J >= Hp) { continue; }
					const float* P = &Ch[(size_t(J) * Wp + X) * 3];
					const float T = H::Uni_SplatTap(float(Y - J), Sc);
					C = C + H::float3(P[0], P[1], P[2]) * T;
				}
				C = C * (1.0f / N1);
				const float Lc = H::Uni_Luminance(C);
				const H::float3 Out = Lc > 0.0f ? C * (L / Lc) : H::float3(L, L, L);
				float* O = &InOutRgb[(size_t(Yv) * W + Xv) * 3];
				O[0] += Out.x; O[1] += Out.y; O[2] += Out.z;
			}
		}
	};
}
