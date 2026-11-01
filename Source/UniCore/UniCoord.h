#pragma once

// UniCoord.h
// The universal coordinate: three axes of four uint32 limbs, a 128-bit unsigned fixed-point
// value in cm per axis. Limb 0 is the most significant. Engine-agnostic C++17, header only,
// no UE includes (D-015).
//
// The root cube is [0, 2^128) cm per axis and WRAPS: the universe is a 3-torus, so every
// operation has a defined result at the edge. The nominal origin is 2^127 on each axis (D-002).
//
// INDEPENDENT OF UniCoord.ush ON PURPOSE. This file is written with 64-bit limb arithmetic;
// the shader core with 32-bit carry chains. The harness compares the two with each other and
// with a Python big-int oracle, so a bug has to be made twice, identically, to pass.
//
// Type names live in namespace UniCore (FCoord, FDelta, FCellKey). The Blueprint-facing
// USTRUCTs FUniCoord / FUniCellKey in UnifiedUniverseCore wrap these (D-108).

#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>

namespace UniCore
{
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using i64 = std::int64_t;

	struct FVec3d { double X = 0.0, Y = 0.0, Z = 0.0; };
	struct FVec3f { float X = 0.0f, Y = 0.0f, Z = 0.0f; };

	// ======================================================================================
	// FU128: one axis. L[0] is the most significant limb.
	// ======================================================================================

	struct FU128
	{
		u32 L[4] = { 0u, 0u, 0u, 0u };

		static FU128 Make(u32 A, u32 B, u32 C, u32 D) { FU128 R; R.L[0] = A; R.L[1] = B; R.L[2] = C; R.L[3] = D; return R; }
		static FU128 FromU64(u64 V) { return Make(0u, 0u, u32(V >> 32), u32(V)); }

		/** Sign-extended from a signed 64-bit value (two's complement). */
		static FU128 FromI64(i64 V)
		{
			const u32 Fill = V < 0 ? 0xFFFFFFFFu : 0u;
			return Make(Fill, Fill, u32(u64(V) >> 32), u32(u64(V)));
		}

		static FU128 Pow2(int Bit)
		{
			FU128 R;
			if (Bit >= 0 && Bit < 128) { R.L[3 - Bit / 32] = 1u << (Bit % 32); }
			return R;
		}

		bool IsZero() const { return (L[0] | L[1] | L[2] | L[3]) == 0u; }
		bool IsNegative() const { return (L[0] & 0x80000000u) != 0u; }

		friend bool operator==(const FU128& A, const FU128& B) { return std::memcmp(A.L, B.L, sizeof(A.L)) == 0; }
		friend bool operator!=(const FU128& A, const FU128& B) { return !(A == B); }

		/** Unsigned comparison: -1, 0, 1. */
		static int Compare(const FU128& A, const FU128& B)
		{
			for (int I = 0; I < 4; ++I)
			{
				if (A.L[I] != B.L[I]) { return A.L[I] < B.L[I] ? -1 : 1; }
			}
			return 0;
		}
	};

	inline FU128 Add(const FU128& A, const FU128& B)
	{
		FU128 R;
		u64 Carry = 0;
		for (int I = 3; I >= 0; --I)
		{
			const u64 S = u64(A.L[I]) + u64(B.L[I]) + Carry;
			R.L[I] = u32(S);
			Carry = S >> 32;
		}
		return R;
	}

	inline FU128 Neg(const FU128& A)
	{
		FU128 R;
		u64 Carry = 1;
		for (int I = 3; I >= 0; --I)
		{
			const u64 S = u64(u32(~A.L[I])) + Carry;
			R.L[I] = u32(S);
			Carry = S >> 32;
		}
		return R;
	}

	inline FU128 Sub(const FU128& A, const FU128& B) { return Add(A, Neg(B)); }

	/** Logical shift right, S in [0, 128]; S >= 128 gives zero. */
	inline FU128 Shr(const FU128& A, int S)
	{
		FU128 R;
		if (S <= 0) { return A; }
		if (S >= 128) { return R; }
		// Work in two u64 halves.
		u64 Hi = (u64(A.L[0]) << 32) | A.L[1];
		u64 Lo = (u64(A.L[2]) << 32) | A.L[3];
		if (S >= 64) { Lo = Hi >> (S - 64); Hi = 0; }
		else { Lo = (Lo >> S) | (Hi << (64 - S)); Hi >>= S; }
		// S == 64 path: Hi >> 0 is fine; S in (0, 64): (64 - S) in (0, 64).
		R.L[0] = u32(Hi >> 32); R.L[1] = u32(Hi); R.L[2] = u32(Lo >> 32); R.L[3] = u32(Lo);
		return R;
	}

	/** Shift left, S in [0, 128]; bits past 127 are lost. */
	inline FU128 Shl(const FU128& A, int S)
	{
		FU128 R;
		if (S <= 0) { return A; }
		if (S >= 128) { return R; }
		u64 Hi = (u64(A.L[0]) << 32) | A.L[1];
		u64 Lo = (u64(A.L[2]) << 32) | A.L[3];
		if (S >= 64) { Hi = Lo << (S - 64); Lo = 0; }
		else { Hi = (Hi << S) | (Lo >> (64 - S)); Lo <<= S; }
		R.L[0] = u32(Hi >> 32); R.L[1] = u32(Hi); R.L[2] = u32(Lo >> 32); R.L[3] = u32(Lo);
		return R;
	}

	/** Keeps the low Bits bits, Bits in [0, 128]. */
	inline FU128 MaskLow(const FU128& A, int Bits)
	{
		if (Bits >= 128) { return A; }
		if (Bits <= 0) { return FU128(); }
		return Sub(A, Shl(Shr(A, Bits), Bits));
	}

	/** Highest set bit 0..127, or -1. */
	inline int Msb(const FU128& A)
	{
		for (int I = 0; I < 4; ++I)
		{
			if (A.L[I] != 0u)
			{
				int B = 31;
				while (((A.L[I] >> B) & 1u) == 0u) { --B; }
				return (3 - I) * 32 + B;
			}
		}
		return -1;
	}

	inline FU128 Abs(const FU128& A) { return A.IsNegative() ? Neg(A) : A; }

	/** Remainder of an unsigned 128-bit value by a small positive modulus. */
	inline u32 ModSmall(const FU128& A, u32 M)
	{
		u64 R = 0;
		for (int I = 0; I < 4; ++I) { R = ((R << 32) | A.L[I]) % M; }
		return u32(R);
	}

	/** Lowercase hex, 32 digits, most significant first. */
	inline std::string ToHex(const FU128& A)
	{
		static const char* Digits = "0123456789abcdef";
		std::string S(32, '0');
		for (int I = 0; I < 4; ++I)
		{
			for (int N = 0; N < 8; ++N) { S[I * 8 + N] = Digits[(A.L[I] >> (28 - 4 * N)) & 0xFu]; }
		}
		return S;
	}

	inline bool FromHex(const std::string& S, FU128& Out)
	{
		if (S.size() != 32) { return false; }
		FU128 R;
		for (int C = 0; C < 32; ++C)
		{
			const char Ch = S[C];
			u32 V;
			if (Ch >= '0' && Ch <= '9') { V = u32(Ch - '0'); }
			else if (Ch >= 'a' && Ch <= 'f') { V = u32(Ch - 'a' + 10); }
			else if (Ch >= 'A' && Ch <= 'F') { V = u32(Ch - 'A' + 10); }
			else { return false; }
			R.L[C / 8] = (R.L[C / 8] << 4) | V;
		}
		Out = R;
		return true;
	}

	// ======================================================================================
	// Correctly rounded conversion (round half to even)
	// ======================================================================================

	/** Rounds |A| (unsigned) to MantBits significant bits. Returns the significand and sets
	 *  Exp so that value = Mant * 2^Exp exactly. Zero gives Mant = 0. */
	inline u64 RoundToBits(const FU128& A, int MantBits, int& Exp)
	{
		const int P = Msb(A);
		Exp = 0;
		if (P < 0) { return 0; }
		if (P < MantBits)
		{
			// Fits exactly; value is in the low 64 bits because MantBits <= 53.
			return (u64(A.L[2]) << 32) | A.L[3];
		}
		const int Drop = P - (MantBits - 1);             // bits below the significand
		const FU128 Top = Shr(A, Drop);                  // MantBits bits
		u64 Mant = (u64(Top.L[2]) << 32) | Top.L[3];
		const FU128 Rest = Sub(A, Shl(Top, Drop));
		const FU128 Half = FU128::Pow2(Drop - 1);
		const int C = FU128::Compare(Rest, Half);
		if (C > 0 || (C == 0 && (Mant & 1u)))
		{
			++Mant;
			if (Mant == (u64(1) << MantBits)) { Mant >>= 1; ++Exp; }
		}
		Exp += Drop;
		return Mant;
	}

	inline double U128ToDouble(const FU128& A)
	{
		int E;
		const u64 M = RoundToBits(A, 53, E);
		return std::ldexp(double(M), E);
	}

	inline float U128ToFloat(const FU128& A)
	{
		int E;
		const u64 M = RoundToBits(A, 24, E);
		// M < 2^24 and E <= 104 for |A| < 2^128: float(M) is exact; ldexpf overflows to inf
		// only for E + 24 > 128, which happens only for magnitudes rounding to 2^128.
		return std::ldexp(float(M), E);
	}

	/** Exact integer value of float(A) as FU128 (A unsigned, result finite). */
	inline FU128 U128RoundedFloatExact(const FU128& A)
	{
		int E;
		const u64 M = RoundToBits(A, 24, E);
		return Shl(FU128::FromU64(M), E);
	}

	inline float S128ToFloat(const FU128& A)
	{
		const float F = U128ToFloat(Abs(A));
		return A.IsNegative() ? -F : F;
	}

	inline double S128ToDouble(const FU128& A)
	{
		const double D = U128ToDouble(Abs(A));
		return A.IsNegative() ? -D : D;
	}

	// ======================================================================================
	// Coordinates, deltas, cells
	// ======================================================================================

	/** Absolute position: per axis a 128-bit unsigned value in cm, limb 0 most significant.
	 *  48 bytes. Exactly the design doc's `struct FUniCoord { uint32 X[4], Y[4], Z[4]; }`. */
	struct FCoord
	{
		u32 X[4] = { 0u, 0u, 0u, 0u };
		u32 Y[4] = { 0u, 0u, 0u, 0u };
		u32 Z[4] = { 0u, 0u, 0u, 0u };

		FU128 Axis(int I) const
		{
			const u32* P = I == 0 ? X : (I == 1 ? Y : Z);
			return FU128::Make(P[0], P[1], P[2], P[3]);
		}

		void SetAxis(int I, const FU128& V)
		{
			u32* P = I == 0 ? X : (I == 1 ? Y : Z);
			for (int N = 0; N < 4; ++N) { P[N] = V.L[N]; }
		}

		friend bool operator==(const FCoord& A, const FCoord& B) { return std::memcmp(&A, &B, sizeof(FCoord)) == 0; }
		friend bool operator!=(const FCoord& A, const FCoord& B) { return !(A == B); }

		/** The nominal origin, 2^127 on every axis. */
		static FCoord Origin()
		{
			FCoord C;
			C.X[0] = C.Y[0] = C.Z[0] = 0x80000000u;
			return C;
		}

		/** Origin plus a signed whole-cm offset per axis. Convenient for tests and scenes. */
		static FCoord FromOriginOffset(i64 Dx, i64 Dy, i64 Dz)
		{
			const FCoord O = Origin();
			FCoord C;
			C.SetAxis(0, Add(O.Axis(0), FU128::FromI64(Dx)));
			C.SetAxis(1, Add(O.Axis(1), FU128::FromI64(Dy)));
			C.SetAxis(2, Add(O.Axis(2), FU128::FromI64(Dz)));
			return C;
		}
	};
	static_assert(sizeof(FCoord) == 48, "FCoord must be 48 bytes");

	/** Per axis: a 128-bit magnitude plus a sign (the design doc's FUniDelta). */
	struct FDelta
	{
		FU128 Mag[3];
		bool Neg[3] = { false, false, false };

		/** The same value as two's complement, the form the GPU consumes (D-104). */
		FU128 TwosComplement(int I) const { return Neg[I] ? UniCore::Neg(Mag[I]) : Mag[I]; }
	};

	/** A - B on the torus: the shortest signed difference per axis. A two's-complement
	 *  difference of exactly 2^127 (the antipode) is read as negative, magnitude 2^127. */
	inline FDelta Sub(const FCoord& A, const FCoord& B)
	{
		FDelta D;
		for (int I = 0; I < 3; ++I)
		{
			const FU128 T = Sub(A.Axis(I), B.Axis(I));
			D.Neg[I] = T.IsNegative();
			D.Mag[I] = D.Neg[I] ? Neg(T) : T;
		}
		return D;
	}

	/** Correctly rounded to double. The design asks for 'exact to double rounding'; this is
	 *  the strict form of that: one rounding, half to even. */
	inline FVec3d ToDouble(const FDelta& D)
	{
		FVec3d R;
		double* Out[3] = { &R.X, &R.Y, &R.Z };
		for (int I = 0; I < 3; ++I)
		{
			const double V = U128ToDouble(D.Mag[I]);
			*Out[I] = D.Neg[I] ? -V : V;
		}
		return R;
	}

	struct FHiLo { FVec3f Hi; FVec3f Lo; };

	/** Hi = float(d), Lo = float(d - Hi): about 2^-48 of |d|. */
	inline FHiLo ToHiLo(const FDelta& D)
	{
		FHiLo R;
		float* Hi[3] = { &R.Hi.X, &R.Hi.Y, &R.Hi.Z };
		float* Lo[3] = { &R.Lo.X, &R.Lo.Y, &R.Lo.Z };
		for (int I = 0; I < 3; ++I)
		{
			const float H = U128ToFloat(D.Mag[I]);
			const FU128 Residual = Sub(D.Mag[I], U128RoundedFloatExact(D.Mag[I]));
			const float L = S128ToFloat(Residual);
			*Hi[I] = D.Neg[I] ? -H : H;
			*Lo[I] = D.Neg[I] ? -L : L;
		}
		return R;
	}

	/** An implicit-octree node: level L in [0, 128] and the cell coordinate Coord >> L per
	 *  axis, which has 128 - L significant bits. */
	struct FCellKey
	{
		int Level = 0;
		FU128 C[3];

		friend bool operator==(const FCellKey& A, const FCellKey& B)
		{
			return A.Level == B.Level && A.C[0] == B.C[0] && A.C[1] == B.C[1] && A.C[2] == B.C[2];
		}
		friend bool operator!=(const FCellKey& A, const FCellKey& B) { return !(A == B); }
	};

	inline FCellKey CellOf(const FCoord& P, int Level)
	{
		FCellKey K;
		K.Level = Level;
		for (int I = 0; I < 3; ++I) { K.C[I] = Shr(P.Axis(I), Level); }
		return K;
	}

	/** Minimum corner of a cell. */
	inline FCoord CellOrigin(const FCellKey& K)
	{
		FCoord P;
		for (int I = 0; I < 3; ++I) { P.SetAxis(I, Shl(K.C[I], K.Level)); }
		return P;
	}

	inline FCellKey Parent(const FCellKey& K)
	{
		FCellKey P;
		P.Level = K.Level + 1;
		for (int I = 0; I < 3; ++I) { P.C[I] = Shr(K.C[I], 1); }
		return P;
	}

	/** Child by octant: bit 0 = x, bit 1 = y, bit 2 = z. */
	inline FCellKey Child(const FCellKey& K, int Octant)
	{
		FCellKey C;
		C.Level = K.Level - 1;
		for (int I = 0; I < 3; ++I)
		{
			C.C[I] = Add(Shl(K.C[I], 1), FU128::FromU64(u64((Octant >> I) & 1)));
		}
		return C;
	}

	/** +-steps at the key's level, wrapping over the 2^(128 - L) cells of the root. */
	inline FCellKey Neighbour(const FCellKey& K, i64 Dx, i64 Dy, i64 Dz)
	{
		FCellKey N = K;
		const i64 Step[3] = { Dx, Dy, Dz };
		for (int I = 0; I < 3; ++I)
		{
			N.C[I] = MaskLow(Add(K.C[I], FU128::FromI64(Step[I])), 128 - K.Level);
		}
		return N;
	}

	/** Splits a double displacement of any magnitude into an integer carry and a sub-cm
	 *  fraction. Fraction is in [0, 1) cm. Non-finite displacements are rejected (returns
	 *  false, nothing changes). Magnitudes beyond 2^128 wrap, as the torus does. */
	inline bool AddDisplacement(FCoord& Coord, FVec3f& Fraction, const FVec3d& Cm)
	{
		const double In[3] = { Cm.X, Cm.Y, Cm.Z };
		float* Frac[3] = { &Fraction.X, &Fraction.Y, &Fraction.Z };
		for (int I = 0; I < 3; ++I)
		{
			if (!std::isfinite(In[I])) { return false; }
		}
		for (int I = 0; I < 3; ++I)
		{
			const double V = double(*Frac[I]) + In[I];
			const double N = std::floor(V);
			double F = V - N;                                 // exact for |V| < 2^52
			FU128 Carry;
			const double AbsN = std::fabs(N);
			if (AbsN < 9.0e18)
			{
				Carry = FU128::FromI64(i64(N));
			}
			else
			{
				// N is an integer-valued double: N = M * 2^(E - 53) with a 53-bit M.
				int E;
				const double Fr = std::frexp(AbsN, &E);       // AbsN = Fr * 2^E, Fr in [0.5, 1)
				const u64 M = u64(std::ldexp(Fr, 53));
				Carry = Shl(FU128::FromU64(M), E - 53);         // E > 63 here; >= 181 gives 0
				if (N < 0.0) { Carry = Neg(Carry); }
			}
			float FracF = float(F);
			if (FracF >= 1.0f) { FracF = 0.0f; Carry = Add(Carry, FU128::FromU64(1)); }
			if (FracF < 0.0f) { FracF = 0.0f; }
			*Frac[I] = FracF;
			Coord.SetAxis(I, Add(Coord.Axis(I), Carry));
		}
		return true;
	}

	/** Coord + whole-cm signed delta (two's complement per axis). */
	inline FCoord Offset(const FCoord& P, const FU128& Dx, const FU128& Dy, const FU128& Dz)
	{
		FCoord R;
		R.SetAxis(0, Add(P.Axis(0), Dx));
		R.SetAxis(1, Add(P.Axis(1), Dy));
		R.SetAxis(2, Add(P.Axis(2), Dz));
		return R;
	}

	/** Coordinate as 96 hex digits, X then Y then Z (the Blueprint string form). */
	inline std::string ToHexString(const FCoord& P)
	{
		return ToHex(P.Axis(0)) + ToHex(P.Axis(1)) + ToHex(P.Axis(2));
	}

	inline bool FromHexString(const std::string& S, FCoord& Out)
	{
		if (S.size() != 96) { return false; }
		FU128 A[3];
		for (int I = 0; I < 3; ++I)
		{
			if (!FromHex(S.substr(size_t(I) * 32, 32), A[I])) { return false; }
		}
		for (int I = 0; I < 3; ++I) { Out.SetAxis(I, A[I]); }
		return true;
	}

	/** |delta| as double, overflow-safe (components up to 2^127). */
	inline double Length(const FVec3d& V)
	{
		const double M = std::fmax(std::fabs(V.X), std::fmax(std::fabs(V.Y), std::fabs(V.Z)));
		if (M == 0.0) { return 0.0; }
		const double X = V.X / M, Y = V.Y / M, Z = V.Z / M;
		return M * std::sqrt(X * X + Y * Y + Z * Z);
	}
} // namespace UniCore
