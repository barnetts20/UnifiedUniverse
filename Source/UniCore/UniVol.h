#pragma once

// UniVol.h
// Interim baked-field format (D-007) and the baked-field reference path.
//
// FILE: little endian.
//   char[4] "UVOL"; uint32 version = 1; uint32 sizeX, sizeY, sizeZ; uint32 channels;
//   uint32 format (0 = R16F, 1 = R32F); uint32 flags (bit 0 = periodic); uint32 mipCount
//   then mip 0 .. mipCount-1, tightly packed, x fastest, channel-interleaved.
//
// MIPS ARE INTEGRALS. Each mip texel is the exact mean of its 2x2x2 footprint, so a cell's
// integral is one fetch at the matching mip, and the chain is the hierarchy for placement by
// descent. The field is piecewise constant at mip 0 (nearest-texel), which is what makes the
// mip means exact integrals and descent an exact sampler: no rejection, no envelope.
//
// TILE SYMMETRY. Each period tile gets one of the 48 cube symmetries (axis permutation plus
// sign flips) from hash(level, tile coord, field id). Flips and permutations commute with
// mip reduction (the parent of a flipped child is the flipped parent), so integrals stay
// exact under any symmetry. Integer tile offsets would break that alignment at coarse mips,
// so the prototype uses symmetries only (D-112).
//
// GPU: UniGen.usf mirrors Fetch / SymmetryTexel / Descend with Texture3D.Load (no filtering,
// matching the piecewise-constant definition, lesson #5). That mirror is hand-written because
// textures are outside the shared subset; GPU parity is UE-4.

#include "UniSeed.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace UniCore
{
	/** fopen without MSVC's C4996 deprecation warning (fopen_s there, std::fopen elsewhere). */
	inline FILE* UniFOpen(const char* InPath, const char* InMode)
	{
#if defined(_MSC_VER)
		FILE* F = nullptr;
		return fopen_s(&F, InPath, InMode) == 0 ? F : nullptr;
#else
		return std::fopen(InPath, InMode);
#endif
	}
}

namespace UniCore
{
	class FUniVol
	{
	public:
		u32 Size = 0;            // cubic, power of two
		u32 Channels = 1;
		u32 Format = 1;          // 0 = R16F, 1 = R32F
		u32 Flags = 1;           // bit 0 periodic
		std::vector<std::vector<float>> Mips;   // channel 0, mip 0 .. N-1

		u32 MipCount() const { return u32(Mips.size()); }
		u32 MipSize(u32 Mip) const { return Size >> Mip; }
		bool IsPeriodic() const { return (Flags & 1u) != 0u; }

		float Texel(u32 Mip, int64_t X, int64_t Y, int64_t Z) const
		{
			const int64_t S = int64_t(MipSize(Mip));
			X = ((X % S) + S) % S; Y = ((Y % S) + S) % S; Z = ((Z % S) + S) % S;
			return Mips[Mip][size_t((Z * S + Y) * S + X)];
		}

		/** Periodic smooth positive test volume: exp of a sum of integer-frequency cosines, so it
		 *  tiles exactly. Then normalised to mean 1. */
		static FUniVol GeneratePeriodic(u32 InSize, u32 Seed, int Waves = 24, double Contrast = 1.6)
		{
			FUniVol V;
			V.Size = InSize;
			std::vector<float> D(size_t(InSize) * InSize * InSize);
			struct FWave { int Fx, Fy, Fz; double Phase, Amp; };
			std::vector<FWave> W;
			for (int I = 0; I < Waves; ++I)
			{
				FWave Wv;
				const auto R = [&](u32 K) { return UniHlsl::Uni_DrawU32(Seed, UniHlsl::UNI_CH_SHAPE, u32(I) * 8u + K); };
				const int MaxF = 1 + int(R(0) % 6u);
				Wv.Fx = int(R(1) % u32(2 * MaxF + 1)) - MaxF;
				Wv.Fy = int(R(2) % u32(2 * MaxF + 1)) - MaxF;
				Wv.Fz = int(R(3) % u32(2 * MaxF + 1)) - MaxF;
				if (Wv.Fx == 0 && Wv.Fy == 0 && Wv.Fz == 0) { Wv.Fx = 1; }
				Wv.Phase = double(R(4)) / 4294967296.0 * 6.283185307179586;
				Wv.Amp = 1.0 / std::sqrt(double(Wv.Fx * Wv.Fx + Wv.Fy * Wv.Fy + Wv.Fz * Wv.Fz));
				W.push_back(Wv);
			}
			double Norm = 0.0;
			for (const FWave& Wv : W) { Norm += Wv.Amp * Wv.Amp; }
			Norm = 1.0 / std::sqrt(Norm * 0.5);
			double Sum = 0.0;
			for (u32 Z = 0; Z < InSize; ++Z) for (u32 Y = 0; Y < InSize; ++Y) for (u32 X = 0; X < InSize; ++X)
			{
				double S = 0.0;
				for (const FWave& Wv : W)
				{
					S += Wv.Amp * std::cos(6.283185307179586 * (Wv.Fx * (X + 0.5) + Wv.Fy * (Y + 0.5) + Wv.Fz * (Z + 0.5)) / InSize + Wv.Phase);
				}
				const double Val = std::exp(Contrast * S * Norm);
				D[size_t((Z * InSize + Y) * InSize + X)] = float(Val);
				Sum += Val;
			}
			const float Mean = float(Sum / double(D.size()));
			for (float& F : D) { F /= Mean; }
			V.Mips.push_back(std::move(D));
			V.BuildMips();
			return V;
		}

		/** Mip i+1 texel = mean of its 8 children in mip i. */
		void BuildMips()
		{
			Mips.resize(1);
			u32 S = Size;
			while (S > 1)
			{
				const std::vector<float>& P = Mips.back();
				const u32 H = S / 2;
				std::vector<float> C(size_t(H) * H * H);
				for (u32 Z = 0; Z < H; ++Z) for (u32 Y = 0; Y < H; ++Y) for (u32 X = 0; X < H; ++X)
				{
					double Acc = 0.0;
					for (u32 K = 0; K < 8; ++K)
					{
						const u32 Cx = 2 * X + (K & 1u), Cy = 2 * Y + ((K >> 1) & 1u), Cz = 2 * Z + (K >> 2);
						Acc += P[size_t((Cz * S + Cy) * S + Cx)];
					}
					C[size_t((Z * H + Y) * H + X)] = float(Acc / 8.0);
				}
				Mips.push_back(std::move(C));
				S = H;
			}
		}

		bool Write(const std::string& Path) const
		{
			FILE* F = UniFOpen(Path.c_str(), "wb");
			if (!F) { return false; }
			const char Magic[4] = { 'U', 'V', 'O', 'L' };
			const u32 Header[8] = { 1u, Size, Size, Size, Channels, Format, Flags, MipCount() };
			std::fwrite(Magic, 1, 4, F);
			std::fwrite(Header, 4, 8, F);
			for (const std::vector<float>& M : Mips)
			{
				if (Format == 1u) { std::fwrite(M.data(), 4, M.size(), F); }
				else
				{
					std::vector<uint16_t> H(M.size());
					for (size_t I = 0; I < M.size(); ++I) { H[I] = uint16_t(UniHlsl::Uni_F32ToF16(M[I])); }
					std::fwrite(H.data(), 2, H.size(), F);
				}
			}
			std::fclose(F);
			return true;
		}

		bool Read(const std::string& Path)
		{
			FILE* F = UniFOpen(Path.c_str(), "rb");
			if (!F) { return false; }
			char Magic[4];
			u32 H[8];
			bool Ok = std::fread(Magic, 1, 4, F) == 4 && std::memcmp(Magic, "UVOL", 4) == 0 && std::fread(H, 4, 8, F) == 8 && H[0] == 1u;
			Ok = Ok && H[1] == H[2] && H[2] == H[3] && H[4] == 1u && H[5] <= 1u && H[7] >= 1u && H[7] <= 16u;
			if (Ok)
			{
				Size = H[1]; Channels = H[4]; Format = H[5]; Flags = H[6];
				Mips.clear();
				for (u32 M = 0; M < H[7] && Ok; ++M)
				{
					const size_t N = size_t(Size >> M) * (Size >> M) * (Size >> M);
					std::vector<float> D(N);
					if (Format == 1u) { Ok = std::fread(D.data(), 4, N, F) == N; }
					else
					{
						std::vector<uint16_t> Hf(N);
						Ok = std::fread(Hf.data(), 2, N, F) == N;
						for (size_t I = 0; I < N; ++I) { D[I] = UniHlsl::Uni_F16ToF32(Hf[I]); }
					}
					Mips.push_back(std::move(D));
				}
			}
			std::fclose(F);
			return Ok;
		}
	};

	/** One of the 48 cube symmetries: a permutation of axes and three sign flips. */
	struct FCubeSymmetry
	{
		int Perm[3] = { 0, 1, 2 };
		bool Flip[3] = { false, false, false };

		static FCubeSymmetry FromIndex(u32 I)
		{
			static const int Perms[6][3] = { {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0} };
			FCubeSymmetry S;
			const u32 P = (I % 48u) / 8u, F = I % 8u;
			for (int A = 0; A < 3; ++A) { S.Perm[A] = Perms[P][A]; S.Flip[A] = ((F >> A) & 1u) != 0u; }
			return S;
		}

		/** Texel index of logical texel T in a mip of side S. */
		std::array<int64_t, 3> Apply(const std::array<int64_t, 3>& T, int64_t S) const
		{
			std::array<int64_t, 3> R{};
			for (int A = 0; A < 3; ++A)
			{
				const int64_t V = T[size_t(Perm[A])];
				R[size_t(A)] = Flip[A] ? (S - 1 - V) : V;
			}
			return R;
		}
	};

	inline FCubeSymmetry TileSymmetry(u32 Level, const FU128 TileCoord[3], u32 FieldId)
	{
		u32 H = UniHlsl::Uni_SeedMix(UniHlsl::UNI_CH_TILE, Level);
		for (int A = 0; A < 3; ++A) { for (int N = 0; N < 4; ++N) { H = UniHlsl::Uni_SeedMix(H, TileCoord[A].L[N]); } }
		H = UniHlsl::Uni_SeedMix(H, FieldId);
		return FCubeSymmetry::FromIndex(H % 48u);
	}

	/** Baked-field cell sampler: a cell of the field grid at one mip, with its tile symmetry. */
	struct FBakedCellRef
	{
		const FUniVol* Vol = nullptr;
		u32 Mip = 0;                    // the mip whose texel equals the cell (or 0 if the cell is smaller than a texel)
		std::array<int64_t, 3> Texel{}; // logical texel at that mip
		FCubeSymmetry Sym;
		double SubTexelFraction = 1.0;  // cell width / texel width when the cell is smaller than a texel (per axis)

		float Fetch(u32 M, const std::array<int64_t, 3>& T) const
		{
			const std::array<int64_t, 3> P = Sym.Apply(T, int64_t(Vol->MipSize(M)));
			return Vol->Texel(M, P[0], P[1], P[2]);
		}

		/** Mean field over the cell. */
		double Mean() const { return Fetch(Mip, Texel); }

		/** Exact sample of the piecewise-constant field inside the cell: descend from the cell's
		 *  texel to mip 0 choosing children by mass, then uniform in the final texel. Returns
		 *  the cell-local position as 32-bit fixed point per axis. */
		void Descend(u32 Seed, u32 J, u32 OutOffset[3]) const
		{
			std::array<int64_t, 3> T = Texel;
			u32 K = 0;
			for (u32 M = Mip; M > 0; --M)
			{
				double W[8], Total = 0.0;
				for (u32 C = 0; C < 8; ++C)
				{
					const std::array<int64_t, 3> Ch = { 2 * T[0] + (C & 1u), 2 * T[1] + ((C >> 1) & 1u), 2 * T[2] + (C >> 2) };
					W[C] = Fetch(M - 1, Ch);
					Total += W[C];
				}
				const double U = double(UniHlsl::Uni_DrawUnit(Seed, UniHlsl::UNI_CH_PLACE, J * 64u + K++)) * Total;
				u32 Pick = 0;
				double Acc = W[0];
				while (Pick < 7u && U >= Acc) { ++Pick; Acc += W[Pick]; }
				T = { 2 * T[0] + (Pick & 1u), 2 * T[1] + ((Pick >> 1) & 1u), 2 * T[2] + (Pick >> 2) };
			}
			// Offset within the cell: position of texel T at mip 0 relative to the cell's
			// footprint (2^Mip texels per axis), plus a uniform draw inside the texel.
			const int64_t Span = int64_t(1) << Mip;
			for (int A = 0; A < 3; ++A)
			{
				const int64_t Local = T[size_t(A)] - Texel[size_t(A)] * Span;   // 0 .. Span-1
				const u32 Within = UniHlsl::Uni_DrawU32(Seed, UniHlsl::UNI_CH_PLACE, J * 64u + 32u + u32(A));
				// cell fraction = (Local + Within / 2^32) / Span, as 32-bit fixed point.
				const u64 Fixed = (u64(Local) << 32 | u64(Within)) >> Mip;
				OutOffset[A] = u32(Fixed);
			}
		}
	};
}
