// UniBlueNoise.h
// D-246: a blue-noise tile for the volume's draws offline (Uni_VolumeDraw's UNI_VOL_NOISE), standing
// in for the engine's FastBlueNoise_scalar_128x128x8 that the renderer binds (CloudAtmosphere's).
// Void and cluster (Ulichney 1993) on a torus, a Gaussian of sigma 1.5 texels, deterministic:
// every texel's rank over the tile's count, at its centre ((rank + 0.5) / N): uniform in [0, 1)
// with the energy at the highest spatial frequencies.
#pragma once

#include "UniShaderCores.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace UniVerify
{
	/** The tile, Size x Size (built once per size). */
	inline const std::vector<float>& BlueNoiseTile(int Size = 128)
	{
		static std::vector<float> Tile;
		static int Built = 0;
		if (Built == Size) { return Tile; }
		const int N = Size * Size;
		const double Sigma = 1.5;
		const int R = 6;   // the kernel's reach (4 sigma)
		std::vector<double> Kern(size_t(2 * R + 1) * size_t(2 * R + 1));
		for (int Y = -R; Y <= R; ++Y) for (int X = -R; X <= R; ++X) { Kern[size_t(Y + R) * size_t(2 * R + 1) + size_t(X + R)] = std::exp(-(X * X + Y * Y) / (2.0 * Sigma * Sigma)); }
		std::vector<uint8_t> Bin(size_t(N), 0);
		std::vector<double> Energy(size_t(N), 0.0);
		auto Splat = [&](int I, double Sign)
		{
			const int Cx = I % Size, Cy = I / Size;
			for (int Y = -R; Y <= R; ++Y) for (int X = -R; X <= R; ++X)
			{
				const int Px = (Cx + X + Size) % Size, Py = (Cy + Y + Size) % Size;
				Energy[size_t(Py) * size_t(Size) + size_t(Px)] += Sign * Kern[size_t(Y + R) * size_t(2 * R + 1) + size_t(X + R)];
			}
		};
		auto Tightest = [&](uint8_t Value)
		{
			int Best = -1; double E = -1e300;
			for (int I = 0; I < N; ++I) { if (Bin[size_t(I)] == Value && Energy[size_t(I)] > E) { E = Energy[size_t(I)]; Best = I; } }
			return Best;
		};
		auto Voidest = [&](uint8_t Value)
		{
			int Best = -1; double E = 1e300;
			for (int I = 0; I < N; ++I) { if (Bin[size_t(I)] == Value && Energy[size_t(I)] < E) { E = Energy[size_t(I)]; Best = I; } }
			return Best;
		};
		// The initial pattern: a tenth of the texels from a fixed hash, relaxed until the tightest
		// cluster is the largest void.
		const int Ones = N / 10;
		{
			uint32_t S = 0x6E0Au;
			int Placed = 0;
			while (Placed < Ones)
			{
				S = UniHlsl::Uni_Avalanche(S + 0x9E3779B9u);
				const int I = int(S % uint32_t(N));
				if (Bin[size_t(I)]) { continue; }
				Bin[size_t(I)] = 1; Splat(I, 1.0); ++Placed;
			}
			for (int Iter = 0; Iter < 4 * N; ++Iter)
			{
				const int C = Tightest(1);
				Bin[size_t(C)] = 0; Splat(C, -1.0);
				const int V = Voidest(0);
				if (V == C) { Bin[size_t(C)] = 1; Splat(C, 1.0); break; }
				Bin[size_t(V)] = 1; Splat(V, 1.0);
			}
		}
		std::vector<int> Rank(size_t(N), 0);
		// Phase 1: rank the initial ones, tightest cluster last removed first.
		{
			std::vector<uint8_t> B0 = Bin;
			std::vector<double> E0 = Energy;
			for (int K = Ones - 1; K >= 0; --K)
			{
				const int C = Tightest(1);
				Bin[size_t(C)] = 0; Splat(C, -1.0);
				Rank[size_t(C)] = K;
			}
			Bin = B0; Energy = E0;
		}
		// Phase 2 and 3: fill the largest void each time up to all texels (on the torus the ones'
		// largest void is the zeros' tightest cluster, so Ulichney's third phase is this one).
		for (int K = Ones; K < N; ++K)
		{
			const int V = Voidest(0);
			Bin[size_t(V)] = 1; Splat(V, 1.0);
			Rank[size_t(V)] = K;
		}
		Tile.assign(size_t(N), 0.0f);
		for (int I = 0; I < N; ++I) { Tile[size_t(I)] = float((double(Rank[size_t(I)]) + 0.5) / double(N)); }
		Built = Size;
		return Tile;
	}

	/** The tile's mean power over the frequencies 0 < |f| <= InFMax (cycles per texel), relative to
	 *  white noise's (1): blue noise has next to none at low frequency. A separable DFT. */
	inline double BlueNoiseLowPower(const std::vector<float>& InTile, int Size, double InFMax)
	{
		const double Pi = 3.14159265358979323846;
		std::vector<double> Re(size_t(Size) * size_t(Size)), Im(Re.size()), R2(Re.size()), I2(Re.size());
		std::vector<double> Cs(static_cast<size_t>(Size)), Sn(static_cast<size_t>(Size));
		for (int I = 0; I < Size; ++I) { Cs[size_t(I)] = std::cos(2.0 * Pi * I / Size); Sn[size_t(I)] = std::sin(2.0 * Pi * I / Size); }
		for (int Y = 0; Y < Size; ++Y) for (int K = 0; K < Size; ++K)
		{
			double A = 0.0, B = 0.0;
			for (int X = 0; X < Size; ++X)
			{
				const double V = double(InTile[size_t(Y) * size_t(Size) + size_t(X)]) - 0.5;
				const size_t Ph = size_t((K * X) % Size);
				A += V * Cs[Ph]; B -= V * Sn[Ph];
			}
			Re[size_t(Y) * size_t(Size) + size_t(K)] = A; Im[size_t(Y) * size_t(Size) + size_t(K)] = B;
		}
		double Sum = 0.0; int Count = 0;
		for (int L = 0; L < Size; ++L) for (int K = 0; K < Size; ++K)
		{
			const int Kk = K < Size / 2 ? K : K - Size, Ll = L < Size / 2 ? L : L - Size;
			const double F = std::sqrt(double(Kk * Kk + Ll * Ll)) / Size;
			if (F <= 0.0 || F > InFMax) { continue; }
			double A = 0.0, B = 0.0;
			for (int Y = 0; Y < Size; ++Y)
			{
				const size_t Ph = size_t((L * Y) % Size);
				const double Xr = Re[size_t(Y) * size_t(Size) + size_t(K)], Xi = Im[size_t(Y) * size_t(Size) + size_t(K)];
				A += Xr * Cs[Ph] + Xi * Sn[Ph]; B += Xi * Cs[Ph] - Xr * Sn[Ph];
			}
			Sum += (A * A + B * B) / (double(Size) * double(Size) / 12.0); ++Count;
		}
		return Count > 0 ? Sum / Count : 0.0;
	}

	/** Binds the tile to the shim's UNI_VOL_NOISE for its lifetime (the previous binding after). */
	struct FBlueNoiseBind
	{
		const float* P; UniHlsl::uint W, H;
		explicit FBlueNoiseBind(bool bOn = true, int Size = 128) : P(UniHlsl::GBlueNoise), W(UniHlsl::GBlueNoiseW), H(UniHlsl::GBlueNoiseH)
		{
			if (bOn) { const std::vector<float>& T = BlueNoiseTile(Size); UniHlsl::GBlueNoise = T.data(); UniHlsl::GBlueNoiseW = UniHlsl::uint(Size); UniHlsl::GBlueNoiseH = UniHlsl::uint(Size); }
			else { UniHlsl::GBlueNoise = nullptr; UniHlsl::GBlueNoiseW = 0u; UniHlsl::GBlueNoiseH = 0u; }
		}
		~FBlueNoiseBind() { UniHlsl::GBlueNoise = P; UniHlsl::GBlueNoiseW = W; UniHlsl::GBlueNoiseH = H; }
	};
}
