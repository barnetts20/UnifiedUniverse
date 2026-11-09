#pragma once

// UniHlslShim.h
// The HLSL surface the Unified Universe shader cores use, in C++. Include this FIRST, then
// the .ush INSIDE the namespace, and one source compiles for both targets:
//
//     #include "UniHlslShim.h"
//     namespace UniHlsl {
//     #include "UniCoord.ush"
//     }
//
// Modelled on UltraLargeScale's GalaxyHLSLShim.h, with the same two rules:
//
// 1. THE NAMESPACE IS NEVER OPENED with `using`. MSVC's <cmath> declares float overloads of
//    sqrt/abs/exp/... at global scope, which libstdc++ does not. Compiling the cores inside
//    UniHlsl makes unqualified lookup find UniHlsl::sqrt and stop: name hiding, not
//    overloading, so there is no ambiguity on either compiler.
// 2. ONLY THE SUBSET IS PROVIDED. Anything a core uses that is missing here is a compile
//    error. That is the point: a build break, never a silent divergence.
//
// Differences from the galaxy shim, both needed by 128-bit coordinate maths:
//   - Integer vectors (uint2/3/4, int3) exist, with member access only.
//   - asuint / asfloat / firstbithigh are provided, bit-exact.
//
// PRECISION: float32 throughout, matching the shader. Compile with -ffp-contract=off
// (MSVC: /fp:precise, the UE default) so a*b+c is never fused into an FMA the GPU path
// does not take. Transcendentals (exp, log, pow, sin) come from the C runtime here and from
// the GPU's hardware approximations there. They agree to a few ULP, not bit for bit.
// Integer-only cores (coordinates, seeds, cell keys) ARE bit-identical. Anything computed
// after a transcendental is statistically identical but may differ in rare boundary cases
// (D-105).
//
// RESOURCES: StructuredBuffer<T> is a read-only view over a C++ array, so a core can take a
// LUT as a parameter. Textures stay outside the shared subset with one exception (D-164): the
// web density texture, which cores reach only through the macro UNI_WEB_TEX_SAMPLE(P, Mip).
// In HLSL it expands to a SampleLevel on the global declared in Shaders/Private/
// UniWebTexture.ush; here, to UniWebTexSample below: the same wrap / trilinear / linear-mip
// filter over the CPU copy of the same texels (UniCore::FWebTexture binds it). Unbound, it
// returns NaN, never a neutral value (lesson #1): placement then accepts nothing.

#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
#endif

#define UNI_CPP 1
#define UNI_FN inline
#define UNI_INOUT(T) T&
#define UNI_OUT(T) T&
#define UNI_PRECISE
#define UNI_LOOP
#define UNI_UNROLL

namespace UniHlsl
{
	typedef unsigned int uint;
	static_assert(sizeof(uint) == 4, "HLSL uint is 32 bits");
	static_assert(sizeof(float) == 4, "HLSL float is 32 bits");

	// -------------------------------------------------------------------------------------
	// VECTOR TYPES. Lowercase members to match HLSL. Constructors take every component
	// explicitly; HLSL's scalar splat constructor is deliberately not provided.
	// -------------------------------------------------------------------------------------

	struct float2
	{
		float x, y;
		float2() : x(0.0f), y(0.0f) {}
		float2(float InX, float InY) : x(InX), y(InY) {}
	};

	struct float3
	{
		float x, y, z;
		float3() : x(0.0f), y(0.0f), z(0.0f) {}
		float3(float InX, float InY, float InZ) : x(InX), y(InY), z(InZ) {}

		float3& operator+=(const float3& R) { x += R.x; y += R.y; z += R.z; return *this; }
		float3& operator-=(const float3& R) { x -= R.x; y -= R.y; z -= R.z; return *this; }
		float3& operator*=(float S) { x *= S; y *= S; z *= S; return *this; }
	};

	inline float3 operator+(const float3& A, const float3& B) { return float3(A.x + B.x, A.y + B.y, A.z + B.z); }
	inline float3 operator-(const float3& A, const float3& B) { return float3(A.x - B.x, A.y - B.y, A.z - B.z); }
	inline float3 operator*(const float3& A, const float3& B) { return float3(A.x * B.x, A.y * B.y, A.z * B.z); }
	inline float3 operator*(const float3& A, float S) { return float3(A.x * S, A.y * S, A.z * S); }
	inline float3 operator*(float S, const float3& A) { return float3(A.x * S, A.y * S, A.z * S); }
	inline float3 operator/(const float3& A, float S) { return float3(A.x / S, A.y / S, A.z / S); }
	inline float3 operator-(const float3& A) { return float3(-A.x, -A.y, -A.z); }

	struct float4
	{
		float x, y, z, w;
		float4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
		float4(float InX, float InY, float InZ, float InW) : x(InX), y(InY), z(InZ), w(InW) {}
	};

	inline float4 operator+(const float4& A, const float4& B) { return float4(A.x + B.x, A.y + B.y, A.z + B.z, A.w + B.w); }
	inline float4 operator*(const float4& A, float S) { return float4(A.x * S, A.y * S, A.z * S, A.w * S); }
	inline float4 operator*(float S, const float4& A) { return float4(A.x * S, A.y * S, A.z * S, A.w * S); }

	struct uint2
	{
		uint x, y;
		uint2() : x(0u), y(0u) {}
		uint2(uint InX, uint InY) : x(InX), y(InY) {}
	};

	struct uint3
	{
		uint x, y, z;
		uint3() : x(0u), y(0u), z(0u) {}
		uint3(uint InX, uint InY, uint InZ) : x(InX), y(InY), z(InZ) {}
	};

	struct uint4
	{
		uint x, y, z, w;
		uint4() : x(0u), y(0u), z(0u), w(0u) {}
		uint4(uint InX, uint InY, uint InZ, uint InW) : x(InX), y(InY), z(InZ), w(InW) {}
	};

	struct int3
	{
		int x, y, z;
		int3() : x(0), y(0), z(0) {}
		int3(int InX, int InY, int InZ) : x(InX), y(InY), z(InZ) {}
	};

	// -------------------------------------------------------------------------------------
	// RESOURCES
	// -------------------------------------------------------------------------------------

	/** Read-only view over caller-owned memory. Out-of-range reads return a zeroed T, which
	 *  matches D3D's defined behaviour for out-of-bounds structured buffer loads. */
	template <typename T>
	struct StructuredBuffer
	{
		const T* Data = nullptr;
		uint Count = 0u;

		StructuredBuffer() = default;
		StructuredBuffer(const T* InData, uint InCount) : Data(InData), Count(InCount) {}

		T operator[](uint Index) const { return (Data && Index < Count) ? Data[Index] : T(); }
	};

	// -------------------------------------------------------------------------------------
	// BIT INTRINSICS. Bit-exact by construction.
	// -------------------------------------------------------------------------------------

	inline uint asuint(float X) { uint R; std::memcpy(&R, &X, 4); return R; }
	inline float asfloat(uint X) { float R; std::memcpy(&R, &X, 4); return R; }

	/** HLSL firstbithigh on uint: index of the highest set bit, or 0xFFFFFFFF for zero. */
	inline uint firstbithigh(uint X)
	{
		if (X == 0u) { return 0xFFFFFFFFu; }
		uint Index = 0u;
		while (X >>= 1u) { ++Index; }
		return Index;
	}

	/** HLSL firstbitlow on uint: index of the lowest set bit, or 0xFFFFFFFF for zero (D-203). */
	inline uint firstbitlow(uint X)
	{
		if (X == 0u) { return 0xFFFFFFFFu; }
		uint Index = 0u;
		while ((X & 1u) == 0u) { X >>= 1u; ++Index; }
		return Index;
	}

	inline uint countbits(uint X)
	{
		uint Count = 0u;
		while (X) { Count += X & 1u; X >>= 1u; }
		return Count;
	}

	// -------------------------------------------------------------------------------------
	// FLOAT INTRINSICS. Concrete float overloads, never templates, so an exact match beats
	// the int and double candidates the standard library contributes.
	// -------------------------------------------------------------------------------------

	inline float max(float A, float B) { return A > B ? A : B; }
	inline float min(float A, float B) { return A < B ? A : B; }
	inline uint max(uint A, uint B) { return A > B ? A : B; }
	inline uint min(uint A, uint B) { return A < B ? A : B; }
	inline int max(int A, int B) { return A > B ? A : B; }
	inline int min(int A, int B) { return A < B ? A : B; }

	inline float abs(float X) { return std::fabs(X); }
	inline int abs(int X) { return X < 0 ? -X : X; }
	inline float sqrt(float X) { return std::sqrt(X); }
	inline float rsqrt(float X) { return 1.0f / std::sqrt(X); }
	inline float exp(float X) { return std::exp(X); }
	inline float exp2(float X) { return std::exp2(X); }
	inline float log(float X) { return std::log(X); }
	inline float log2(float X) { return std::log2(X); }
	inline float log10(float X) { return std::log10(X); }
	inline float pow(float X, float Y) { return std::pow(X, Y); }
	inline float sin(float X) { return std::sin(X); }
	inline float cos(float X) { return std::cos(X); }
	inline float atan2(float Y, float X) { return std::atan2(Y, X); }
	inline float floor(float X) { return std::floor(X); }
	inline float ceil(float X) { return std::ceil(X); }
	inline float frac(float X) { return X - std::floor(X); }

	// HLSL round() is round-half-to-even; nearbyint honours the default rounding mode, which
	// is half-to-even. std::round rounds half away from zero and would disagree on .5.
	inline float round(float X) { return std::nearbyint(X); }

	inline float clamp(float X, float A, float B) { return X < A ? A : (X > B ? B : X); }
	inline uint clamp(uint X, uint A, uint B) { return X < A ? A : (X > B ? B : X); }
	inline int clamp(int X, int A, int B) { return X < A ? A : (X > B ? B : X); }
	inline float saturate(float X) { return clamp(X, 0.0f, 1.0f); }
	inline float lerp(float A, float B, float T) { return A + (B - A) * T; }
	inline float sign(float X) { return X > 0.0f ? 1.0f : (X < 0.0f ? -1.0f : 0.0f); }
	inline float smoothstep(float A, float B, float X)
	{
		const float T = saturate((X - A) / (B - A));
		return T * T * (3.0f - 2.0f * T);
	}

	inline float dot(const float3& A, const float3& B) { return A.x * B.x + A.y * B.y + A.z * B.z; }
	inline float dot(const float4& A, const float4& B) { return A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w; }
	inline float length(const float3& A) { return sqrt(dot(A, A)); }
	inline float3 normalize(const float3& A) { return A * rsqrt(dot(A, A)); }
	inline float3 cross(const float3& A, const float3& B)
	{
		return float3(A.y * B.z - A.z * B.y, A.z * B.x - A.x * B.z, A.x * B.y - A.y * B.x);
	}
	inline float3 abs(const float3& A) { return float3(abs(A.x), abs(A.y), abs(A.z)); }
	inline float3 floor(const float3& A) { return float3(floor(A.x), floor(A.y), floor(A.z)); }
	inline float3 frac(const float3& A) { return float3(frac(A.x), frac(A.y), frac(A.z)); }
	inline float3 min(const float3& A, const float3& B) { return float3(min(A.x, B.x), min(A.y, B.y), min(A.z, B.z)); }
	inline float3 max(const float3& A, const float3& B) { return float3(max(A.x, B.x), max(A.y, B.y), max(A.z, B.z)); }

	// -------------------------------------------------------------------------------------
	// WEB TEXTURE (D-164). The CPU twin of Texture3D<float4>.SampleLevel with an address-wrap,
	// min/mag/mip-linear sampler: texel centres at (i + 0.5) / size, wrap on every axis,
	// trilinear within a mip, linear between mips, mip clamped to [0, mips - 1].
	// -------------------------------------------------------------------------------------

	/** IEEE binary16 to float, exact (denormals, infinities and NaN included). */
	inline float UniHalfToFloatSlow(uint16_t H)
	{
		const uint32_t Sign = uint32_t(H & 0x8000u) << 16;
		const uint32_t Exp = (H >> 10) & 0x1Fu;
		uint32_t Man = H & 0x3FFu;
		uint32_t X;
		if (Exp == 0u)
		{
			if (Man == 0u) { X = Sign; }
			else
			{
				int E = -1;
				do { ++E; Man <<= 1; } while ((Man & 0x400u) == 0u);
				X = Sign | (uint32_t(127 - 15 - E) << 23) | ((Man & 0x3FFu) << 13);
			}
		}
		else if (Exp == 31u) { X = Sign | 0x7F800000u | (Man << 13); }
		else { X = Sign | ((Exp + 127u - 15u) << 23) | (Man << 13); }
		float F; std::memcpy(&F, &X, 4);
		return F;
	}

	/** The same through a 64K-entry table (built once, thread-safe): the CPU sampler decodes 32
	 *  halves per trilinear-between-mips sample. */
	inline float UniHalfToFloat(uint16_t H)
	{
		struct FTable { float V[65536]; FTable() { for (uint32_t I = 0; I < 65536u; ++I) { V[I] = UniHalfToFloatSlow(uint16_t(I)); } } };
		static const FTable T;
		return T.V[H];
	}

	struct FWebTexView
	{
		const uint16_t* const* Mips = nullptr;   // Mips[m]: (Size >> m)^3 RGBA16F texels (4 halves), x fastest
		uint Size = 0u;
		uint NumMips = 0u;
	};

	/** Bound by UniCore::BindWebTexture before any core samples it; read-only afterwards. */
	inline FWebTexView UniWebTextureView;

	inline float4 UniWebTexTrilinear(const FWebTexView& V, uint Mip, const float3& P)
	{
		const int S = int(V.Size >> Mip);
		const uint16_t* D = V.Mips[Mip];
		const float X = P.x * float(S) - 0.5f, Y = P.y * float(S) - 0.5f, Z = P.z * float(S) - 0.5f;
		const float Fx = std::floor(X), Fy = std::floor(Y), Fz = std::floor(Z);
		const float Ax = X - Fx, Ay = Y - Fy, Az = Z - Fz;
		auto Wrap = [S](int I) { const int R = I % S; return R < 0 ? R + S : R; };
		const int X0 = Wrap(int(Fx)), Y0 = Wrap(int(Fy)), Z0 = Wrap(int(Fz));
		const int X1 = Wrap(X0 + 1), Y1 = Wrap(Y0 + 1), Z1 = Wrap(Z0 + 1);
		auto At = [&](int I, int J, int K)
		{
			const uint16_t* H = D + ((size_t(K) * size_t(S) + size_t(J)) * size_t(S) + size_t(I)) * 4u;
			return float4(UniHalfToFloat(H[0]), UniHalfToFloat(H[1]), UniHalfToFloat(H[2]), UniHalfToFloat(H[3]));
		};
		auto Lerp4 = [](const float4& A, const float4& B, float T) { return float4(A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T, A.z + (B.z - A.z) * T, A.w + (B.w - A.w) * T); };
		const float4 C00 = Lerp4(At(X0, Y0, Z0), At(X1, Y0, Z0), Ax);
		const float4 C10 = Lerp4(At(X0, Y1, Z0), At(X1, Y1, Z0), Ax);
		const float4 C01 = Lerp4(At(X0, Y0, Z1), At(X1, Y0, Z1), Ax);
		const float4 C11 = Lerp4(At(X0, Y1, Z1), At(X1, Y1, Z1), Ax);
		return Lerp4(Lerp4(C00, C10, Ay), Lerp4(C01, C11, Ay), Az);
	}

	inline float4 UniWebTexSample(const float3& P, float Mip)
	{
		const FWebTexView& V = UniWebTextureView;
		if (!V.Mips || V.NumMips == 0u)
		{
			const float N = std::nanf("");
			return float4(N, N, N, N);
		}
		const float M = clamp(Mip, 0.0f, float(V.NumMips - 1u));
		const uint M0 = uint(M);
		const float T = M - float(M0);
		const float4 A = UniWebTexTrilinear(V, M0, P);
		if (T <= 0.0f || M0 + 1u >= V.NumMips) { return A; }
		const float4 B = UniWebTexTrilinear(V, M0 + 1u, P);
		return float4(A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T, A.z + (B.z - A.z) * T, A.w + (B.w - A.w) * T);
	}

	/** D-165: the bound texture's channel means over the tile (its last mip's texel; the exact
	 *  mean of the trilinear field, which preserves texel means over a period). CPU only: the
	 *  normalising integral of a linear textured web. NaN when unbound. */
	inline float4 UniWebTexMean()
	{
		const FWebTexView& V = UniWebTextureView;
		if (!V.Mips || V.NumMips == 0u) { const float N = std::nanf(""); return float4(N, N, N, N); }
		const uint16_t* H = V.Mips[V.NumMips - 1u];
		return float4(UniHalfToFloat(H[0]), UniHalfToFloat(H[1]), UniHalfToFloat(H[2]), UniHalfToFloat(H[3]));
	}

	/** D-179: the galaxy textures (slots: warp_fine, warp_large, galaxy_detail), bound by
	 *  UniCore::BindGalaxyTexture; the same filter as the web texture. Unbound: NaN. */
	static const int UNI_GAL_TEX_COUNT = 3;
	inline FWebTexView UniGalaxyTextureViews[UNI_GAL_TEX_COUNT];

	inline float4 UniGalaxyTexSample(int Slot, const float3& P, float Mip)
	{
		const FWebTexView& V = UniGalaxyTextureViews[Slot];
		if (!V.Mips || V.NumMips == 0u)
		{
			const float N = std::nanf("");
			return float4(N, N, N, N);
		}
		const float M = clamp(Mip, 0.0f, float(V.NumMips - 1u));
		const uint M0 = uint(M);
		const float T = M - float(M0);
		const float4 A = UniWebTexTrilinear(V, M0, P);
		if (T <= 0.0f || M0 + 1u >= V.NumMips) { return A; }
		const float4 B = UniWebTexTrilinear(V, M0 + 1u, P);
		return float4(A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T, A.z + (B.z - A.z) * T, A.w + (B.w - A.w) * T);
	}

	/** D-212: the galaxy map (UniGalaxyMap.h), bound by UniCore::BindGalaxyMap: float4 texels per
	 *  mip (already rounded to half floats), Size >> m a side, x fastest; filtered like the GPU's
	 *  trilinear sampler (bilinear in a mip, linear between mips), wrapping. Unbound: NaN. */
	struct FGalMapView
	{
		const float* const* Mips = nullptr;
		int Size = 0;
		int NumMips = 0;
	};
	inline FGalMapView UniGalMapView;

	inline float4 UniGalMapBilinear(const float* D, int S, const float2& UV)
	{
		const float X = UV.x * float(S) - 0.5f, Y = UV.y * float(S) - 0.5f;
		const float Fx = std::floor(X), Fy = std::floor(Y);
		const float Ax = X - Fx, Ay = Y - Fy;
		auto Wrap = [S](int I) { const int R = I % S; return R < 0 ? R + S : R; };
		const int X0 = Wrap(int(Fx)), Y0 = Wrap(int(Fy)), X1 = Wrap(X0 + 1), Y1 = Wrap(Y0 + 1);
		auto At = [&](int I, int J) { const float* T = D + (size_t(J) * size_t(S) + size_t(I)) * 4u; return float4(T[0], T[1], T[2], T[3]); };
		auto Lerp4 = [](const float4& A, const float4& B, float T) { return float4(A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T, A.z + (B.z - A.z) * T, A.w + (B.w - A.w) * T); };
		return Lerp4(Lerp4(At(X0, Y0), At(X1, Y0), Ax), Lerp4(At(X0, Y1), At(X1, Y1), Ax), Ay);
	}

	inline float4 UniGalMapSample(const float2& UV, float Mip)
	{
		const FGalMapView& V = UniGalMapView;
		if (!V.Mips || V.NumMips == 0) { const float N = std::nanf(""); return float4(N, N, N, N); }
		const float M = clamp(Mip, 0.0f, float(V.NumMips - 1));
		const int M0 = int(M);
		const float T = M - float(M0);
		const float4 A = UniGalMapBilinear(V.Mips[M0], std::max(V.Size >> M0, 1), UV);
		if (T <= 0.0f || M0 + 1 >= V.NumMips) { return A; }
		const float4 B = UniGalMapBilinear(V.Mips[M0 + 1], std::max(V.Size >> (M0 + 1), 1), UV);
		return float4(A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T, A.z + (B.z - A.z) * T, A.w + (B.w - A.w) * T);
	}

} // namespace UniHlsl

#define UNI_GAL_MAP(UV, Mip) UniGalMapSample(UV, Mip)
#define UNI_WEB_TEX_SAMPLE(P, Mip) UniWebTexSample(P, Mip)
#define UNI_GAL_WARP_FINE(P, Mip) UniGalaxyTexSample(0, P, Mip)
#define UNI_GAL_WARP_LARGE(P, Mip) UniGalaxyTexSample(1, P, Mip)
#define UNI_GAL_DETAIL(P, Mip) UniGalaxyTexSample(2, P, Mip)
