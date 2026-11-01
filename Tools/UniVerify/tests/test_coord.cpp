// test_coord.cpp
// Tier 1 coordinate checks: fuzz (native vs shader core vs __int128), targeted edges,
// hi/lo precision, hex round trip. The Python big-int oracle runs separately against a dump
// written by `univerify fuzzdump` (oracle/coord_oracle.py), so all three implementations and
// the oracle agree before anything is called green.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "UniShaderCores.h"

#include <cmath>
#include <cstring>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	typedef unsigned __int128 u128;

	u128 ToNative(const FU128& A)
	{
		return (u128(A.L[0]) << 96) | (u128(A.L[1]) << 64) | (u128(A.L[2]) << 32) | u128(A.L[3]);
	}

	FU128 FromNative(u128 V)
	{
		return FU128::Make(u32(V >> 96), u32(V >> 64), u32(V >> 32), u32(V));
	}

	u32 FBits(float F) { u32 B; std::memcpy(&B, &F, 4); return B; }
	u64 DBits(double D) { u64 B; std::memcpy(&B, &D, 8); return B; }

	/** Exact integer value of an integer-valued float/double, as sign + magnitude. */
	FU128 ExactMag(double V)
	{
		V = std::fabs(V);
		if (V == 0.0) { return FU128(); }
		int E;
		const double Fr = std::frexp(V, &E);
		const u64 M = u64(std::ldexp(Fr, 53));
		return E >= 53 ? Shl(FU128::FromU64(M), E - 53) : FU128::FromU64(M >> (53 - E));
	}

	FU128 RandomAxis(Uni::FTestRng& R)
	{
		// Mix of fully random and edge-biased limbs, plus occasional small values near the
		// origin and near the wrap.
		const unsigned Mode = R.U32() % 8u;
		if (Mode == 0u) { return FU128::Make(0u, 0u, 0u, R.EdgeLimb()); }
		if (Mode == 1u) { return FU128::Make(0xFFFFFFFFu, 0xFFFFFFFFu, R.EdgeLimb(), R.EdgeLimb()); }
		if (Mode == 2u) { return FU128::Make(0x80000000u, R.EdgeLimb(), R.EdgeLimb(), R.EdgeLimb()); }
		return FU128::Make(R.EdgeLimb(), R.EdgeLimb(), R.EdgeLimb(), R.EdgeLimb());
	}

	FCoord RandomCoord(Uni::FTestRng& R)
	{
		FCoord C;
		for (int I = 0; I < 3; ++I) { C.SetAxis(I, RandomAxis(R)); }
		return C;
	}

	/** B near A on some cases so deltas cover every magnitude, not just ~2^127. */
	FCoord NearCoord(Uni::FTestRng& R, const FCoord& A)
	{
		FCoord B;
		for (int I = 0; I < 3; ++I)
		{
			const int Bits = int(R.U32() % 129u);
			FU128 D = MaskLow(FU128::Make(R.U32(), R.U32(), R.U32(), R.U32()), Bits);
			if (R.U32() & 1u) { D = Neg(D); }
			B.SetAxis(I, Add(A.Axis(I), D));
		}
		return B;
	}

	double RandomDisplacement(Uni::FTestRng& R)
	{
		const unsigned Mode = R.U32() % 6u;
		const double Sign = (R.U32() & 1u) ? -1.0 : 1.0;
		if (Mode == 0u) { return Sign * R.Unit(); }                                  // sub-cm
		if (Mode == 1u) { return Sign * std::ldexp(R.Unit(), int(R.U32() % 60u)); }   // < 2^60
		if (Mode == 2u) { return Sign * std::ldexp(1.0 + R.Unit(), 60 + int(R.U32() % 80u)); } // 2^60..2^140
		if (Mode == 3u) { return Sign * std::ldexp(1.0 + R.Unit(), 120 + int(R.U32() % 200u)); } // huge, wraps
		if (Mode == 4u) { return 0.0; }
		return Sign * double(R.U32() % 1000u);                                          // integers
	}
}

// ==========================================================================================

UNI_TEST(coord, fuzz)
{
	const int N = UniVerify::Options().FuzzCases;
	Uni::FTestRng R(0xC00D5EEDull);
	long long Mismatch[10] = { 0 };
	const char* Names[10] = { "sub", "cellof", "neighbour", "shift", "todouble", "hilo", "objrel", "adddisp", "u128add", "mask" };
	Uni::Timer T;

	for (int Case = 0; Case < N; ++Case)
	{
		const FCoord A = RandomCoord(R);
		const FCoord B = (Case & 1) ? NearCoord(R, A) : RandomCoord(R);
		const int L = int(R.U32() % 129u);
		const int Step[3] = { int(R.U32() % 7u) - 3, int(R.U32() % 7u) - 3, (R.U32() & 1u) ? 1 : -1 };

		// --- Sub: native vs __int128 vs shader core
		const FDelta D = Sub(A, B);
		for (int I = 0; I < 3; ++I)
		{
			const u128 Ref = ToNative(A.Axis(I)) - ToNative(B.Axis(I));
			const bool RefNeg = (Ref >> 127) != 0;
			const u128 RefMag = RefNeg ? (u128(0) - Ref) : Ref;
			const H::uint4 Ush = H::Uni_U128Sub(ToUint4(A.Axis(I)), ToUint4(B.Axis(I)));
			if (D.Neg[I] != RefNeg || ToNative(D.Mag[I]) != RefMag || ToNative(FromUint4(Ush)) != Ref) { ++Mismatch[0]; }

			// --- Add (shader) vs native
			const H::uint4 UshAdd = H::Uni_U128Add(ToUint4(A.Axis(I)), ToUint4(B.Axis(I)));
			if (ToNative(FromUint4(UshAdd)) != ToNative(A.Axis(I)) + ToNative(B.Axis(I))) { ++Mismatch[8]; }

			// --- Shifts and mask
			const u128 AN = ToNative(A.Axis(I));
			const u128 RefShr = L >= 128 ? 0 : (AN >> L);
			const u128 RefShl = L >= 128 ? 0 : (AN << L);
			if (ToNative(Shr(A.Axis(I), L)) != RefShr || ToNative(FromUint4(H::Uni_U128Shr(ToUint4(A.Axis(I)), u32(L)))) != RefShr
				|| ToNative(Shl(A.Axis(I), L)) != RefShl || ToNative(FromUint4(H::Uni_U128Shl(ToUint4(A.Axis(I)), u32(L)))) != RefShl)
			{
				++Mismatch[3];
			}
			const u128 RefMask = L >= 128 ? AN : (AN & ((u128(1) << L) - 1));
			if (ToNative(MaskLow(A.Axis(I), L)) != RefMask || ToNative(FromUint4(H::Uni_U128MaskLow(ToUint4(A.Axis(I)), u32(L)))) != RefMask)
			{
				++Mismatch[9];
			}

			// --- Float conversion: native double, native float, shader float, libgcc reference
			const double Dd = U128ToDouble(D.Mag[I]);
			const float Df = U128ToFloat(D.Mag[I]);
			const float DfUsh = H::Uni_U128ToFloat(ToUint4(D.Mag[I]));
			if (DBits(Dd) != DBits(double(RefMag))) { ++Mismatch[4]; }
			if (FBits(Df) != FBits(float(RefMag)) || FBits(DfUsh) != FBits(Df)) { ++Mismatch[4]; }
		}

		// --- Hi/lo: native vs shader
		{
			const FHiLo HL = ToHiLo(D);
			const float* NH[3] = { &HL.Hi.X, &HL.Hi.Y, &HL.Hi.Z };
			const float* NL[3] = { &HL.Lo.X, &HL.Lo.Y, &HL.Lo.Z };
			for (int I = 0; I < 3; ++I)
			{
				float UH, UL;
				H::Uni_S128ToHiLo(ToUint4(D.TwosComplement(I)), UH, UL);
				if (FBits(UH) != FBits(*NH[I]) || FBits(UL) != FBits(*NL[I])) { ++Mismatch[5]; }
			}
		}

		// --- CellOf and Neighbour
		{
			const FCellKey K = CellOf(A, L);
			const FCellKey Nb = Neighbour(K, Step[0], Step[1], Step[2]);
			const int Bits = 128 - L;
			for (int I = 0; I < 3; ++I)
			{
				const u128 RefCell = L >= 128 ? 0 : (ToNative(A.Axis(I)) >> L);
				const u128 UshCell = ToNative(FromUint4(H::Uni_CellAxis(ToUint4(A.Axis(I)), u32(L))));
				if (ToNative(K.C[I]) != RefCell || UshCell != RefCell) { ++Mismatch[1]; }

				const u128 Mod = Bits >= 128 ? ~u128(0) : ((u128(1) << Bits) - 1);
				const u128 RefN = (RefCell + u128(__int128(Step[I]))) & Mod;
				const u128 UshN = ToNative(FromUint4(H::Uni_CellStepAxis(ToUint4(K.C[I]), Step[I], u32(L))));
				if (ToNative(Nb.C[I]) != RefN || UshN != RefN) { ++Mismatch[2]; }
			}
		}

		// --- Object relative vector (D-104): shader core vs native
		{
			const FCellKey K = CellOf(A, L);
			const FDelta CD = Sub(CellOrigin(K), B);
			const u32 Off[3] = { R.U32(), R.U32(), R.U32() };
			const float Fr[3] = { float(R.Unit()) * 0.999f, float(R.Unit()) * 0.999f, float(R.Unit()) * 0.999f };
			const H::float3 Ush = H::Uni_ObjectRelative(ToUint4(CD.TwosComplement(0)), ToUint4(CD.TwosComplement(1)), ToUint4(CD.TwosComplement(2)),
				H::uint3(Off[0], Off[1], Off[2]), u32(L), H::float3(Fr[0], Fr[1], Fr[2]));
			const float UshA[3] = { Ush.x, Ush.y, Ush.z };
			for (int I = 0; I < 3; ++I)
			{
				const FU128 OffCm = L >= 32 ? Shl(FU128::FromU64(Off[I]), L - 32) : FU128::FromU64(u64(Off[I]) >> (32 - L));
				const float Ref = S128ToFloat(Add(CD.TwosComplement(I), OffCm)) - Fr[I];
				if (FBits(Ref) != FBits(UshA[I])) { ++Mismatch[6]; }
			}
		}

		// --- AddDisplacement vs a __int128 reference of the same rule
		{
			FCoord C = A;
			FVec3f Frac{ float(R.Unit()) * 0.999f, float(R.Unit()) * 0.999f, float(R.Unit()) * 0.999f };
			const FVec3f Frac0 = Frac;
			const FVec3d Disp{ RandomDisplacement(R), RandomDisplacement(R), RandomDisplacement(R) };
			AddDisplacement(C, Frac, Disp);
			const double DA[3] = { Disp.X, Disp.Y, Disp.Z };
			const float F0[3] = { Frac0.X, Frac0.Y, Frac0.Z };
			const float F1[3] = { Frac.X, Frac.Y, Frac.Z };
			for (int I = 0; I < 3; ++I)
			{
				const double V = double(F0[I]) + DA[I];
				const double Nf = std::floor(V);
				const FU128 M = ExactMag(Nf);
				u128 Carry = ToNative(M);
				if (Nf < 0.0) { Carry = u128(0) - Carry; }
				float Fr = float(V - Nf);
				if (Fr >= 1.0f) { Fr = 0.0f; Carry += 1; }
				const u128 Expect = ToNative(A.Axis(I)) + Carry;
				if (ToNative(C.Axis(I)) != Expect || FBits(Fr) != FBits(F1[I])) { ++Mismatch[7]; }
			}
		}
	}

	long long Total = 0;
	std::string Detail;
	for (int I = 0; I < 10; ++I)
	{
		Total += Mismatch[I];
		if (Mismatch[I]) { Detail += Uni::Fmt(" %s=%lld", Names[I], Mismatch[I]); }
	}

	auto C = Uni::Check(1, "T1.coord.fuzz", "Coordinate fuzz: UniCoord.h vs UniCoord.ush (via shim) vs __int128, all ops");
	C.Metric("cases", N).Metric("mismatches", double(Total)).Metric("seconds", T.Seconds());
	C.Pass(Total == 0 && N >= 1000000,
		Uni::Fmt("%d cases x 3 axes, %lld mismatches%s (%.1f s)", N, Total, Detail.c_str(), T.Seconds()));
}

UNI_TEST(coord, edges)
{
	int Fail = 0, Total = 0;
	auto Expect = [&](bool B) { ++Total; if (!B) { ++Fail; } };

	const FU128 Max = FU128::Make(~0u, ~0u, ~0u, ~0u);
	const FU128 One = FU128::FromU64(1);

	// Root wrap: max + 1 = 0, 0 - 1 = max.
	Expect(Add(Max, One).IsZero());
	Expect(Sub(FU128(), One) == Max);
	Expect(H::Uni_U128IsZero(H::Uni_U128Add(ToUint4(Max), ToUint4(One))));

	// Borrow chains across all four limbs: 2^96 - 1, 2^64 - 1, 2^32 - 1.
	for (int B = 32; B <= 96; B += 32)
	{
		const FU128 P = FU128::Pow2(B);
		const FU128 R = Sub(P, One);
		Expect(Msb(R) == B - 1 && Add(R, One) == P);
		const H::uint4 U = H::Uni_U128Sub(ToUint4(P), ToUint4(One));
		Expect(FromUint4(U) == R);
	}
	// Full borrow chain: 2^127 - 1 from the top limb down.
	Expect(Sub(FU128::Pow2(127), One) == FU128::Make(0x7FFFFFFFu, ~0u, ~0u, ~0u));

	// Max magnitude: antipodal delta is -2^127 with magnitude 2^127; converts to 2^127 exactly.
	{
		FCoord A = FCoord::Origin();
		FCoord B; // all zero: A - B = 2^127 on every axis
		const FDelta D = Sub(A, B);
		Expect(D.Neg[0] && D.Mag[0] == FU128::Pow2(127));
		Expect(ToDouble(D).X == -std::ldexp(1.0, 127));
		const FHiLo HL = ToHiLo(D);
		Expect(HL.Hi.X == -std::ldexp(1.0f, 127) && HL.Lo.X == 0.0f);
		Expect(std::isfinite(H::Uni_S128ToFloat(ToUint4(D.TwosComplement(0)))));
	}

	// Zero delta.
	{
		const FCoord A = FCoord::Origin();
		const FDelta D = Sub(A, A);
		Expect(!D.Neg[0] && D.Mag[0].IsZero() && ToDouble(D).X == 0.0);
		const FHiLo HL = ToHiLo(D);
		Expect(HL.Hi.X == 0.0f && HL.Lo.X == 0.0f);
	}

	// Shortest image across the wrap: (2^128 - 1) - 1 = -2 not 2^128 - 2.
	{
		FCoord A, B;
		A.SetAxis(0, Max); B.SetAxis(0, One);
		const FDelta D = Sub(A, B);
		Expect(D.Neg[0] && D.Mag[0] == FU128::FromU64(2));
	}

	// Level shifts: L = 0, 1, 31, 32, 33, 63, 64, 65, 96, 127, 128.
	{
		const FCoord P = FCoord::FromOriginOffset(123456789, -987654321, 1);
		const int Ls[] = { 0, 1, 31, 32, 33, 63, 64, 65, 96, 127, 128 };
		for (int L : Ls)
		{
			const FCellKey K = CellOf(P, L);
			const FCoord O = CellOrigin(K);
			// Origin <= P < Origin + 2^L on every axis.
			for (int I = 0; I < 3; ++I)
			{
				const FU128 Off = Sub(P.Axis(I), O.Axis(I));
				Expect(L >= 128 || FU128::Compare(Off, FU128::Pow2(L)) < 0);
			}
			if (L < 128)
			{
				Expect(Parent(K) == CellOf(P, L + 1));
				bool Found = false;
				for (int Oc = 0; Oc < 8; ++Oc) { Found = Found || Child(Parent(K), Oc) == K; }
				Expect(Found);
			}
		}
		// Root level: one cell, neighbours wrap to itself.
		const FCellKey Root = CellOf(P, 128);
		Expect(Neighbour(Root, 1, -1, 5) == Root);
		// Level 127: two cells per axis.
		const FCellKey K127 = CellOf(P, 127);
		Expect(Neighbour(Neighbour(K127, 1, 1, 1), 1, 1, 1) == K127);
	}

	// AddDisplacement at extreme magnitudes.
	{
		FCoord C = FCoord::Origin();
		FVec3f F;
		AddDisplacement(C, F, FVec3d{ 0.25, -0.25, 1.0e300 });
		// x: +0.25 cm fraction, no carry; y: -0.25 -> carry -1, fraction 0.75; z: 1e300 is a
		// multiple of 2^128 (it is an integer double with exponent > 181), so no change.
		Expect(C.Axis(0) == FCoord::Origin().Axis(0) && F.X == 0.25f);
		Expect(C.Axis(1) == Sub(FCoord::Origin().Axis(1), One) && F.Y == 0.75f);
		Expect(C.Axis(2) == FCoord::Origin().Axis(2) && F.Z == 0.0f);

		// 2^127 wraps the origin to zero.
		FCoord C2 = FCoord::Origin();
		FVec3f F2;
		AddDisplacement(C2, F2, FVec3d{ std::ldexp(1.0, 127), -std::ldexp(1.0, 127), std::ldexp(1.0, 128) });
		Expect(C2.Axis(0).IsZero() && C2.Axis(1).IsZero() && C2.Axis(2) == FCoord::Origin().Axis(2));

		// Non-finite rejected.
		FCoord C3 = FCoord::Origin();
		FVec3f F3;
		Expect(!AddDisplacement(C3, F3, FVec3d{ NAN, 0.0, 0.0 }) && C3 == FCoord::Origin());

		// Many small steps accumulate exactly: 1e6 steps of 0.001 cm = 1000 cm, to float
		// fraction precision.
		FCoord C4 = FCoord::Origin();
		FVec3f F4;
		for (int I = 0; I < 1000000; ++I) { AddDisplacement(C4, F4, FVec3d{ 0.001, 0.0, 0.0 }); }
		const FDelta D4 = Sub(C4, FCoord::Origin());
		const double Moved = U128ToDouble(D4.Mag[0]) + F4.X;
		Expect(std::fabs(Moved - 1000.0) < 0.1);
	}

	// Hex round trip.
	{
		Uni::FTestRng R(7);
		for (int I = 0; I < 1000; ++I)
		{
			const FCoord A = RandomCoord(R);
			FCoord B;
			Expect(FromHexString(ToHexString(A), B) && A == B);
		}
		FCoord Dummy;
		Expect(!FromHexString("xyz", Dummy));
	}

	Uni::Check(1, "T1.coord.edges", "Targeted edges: borrow chains, root wrap, max magnitude, zero, level shifts, extreme AddDisplacement, hex")
		.Metric("cases", Total).Metric("failures", Fail)
		.Pass(Fail == 0, Uni::Fmt("%d targeted expectations, %d failed", Total, Fail));
}

UNI_TEST(coord, hilo)
{
	const int N = UniVerify::Options().FuzzCases;
	Uni::FTestRng R(0x4110ull);
	double WorstLog2 = -1000.0;
	long long Over = 0;

	for (int Case = 0; Case < N; ++Case)
	{
		const FCoord A = RandomCoord(R);
		const FCoord B = (Case & 1) ? NearCoord(R, A) : RandomCoord(R);
		const FDelta D = Sub(A, B);
		const FHiLo HL = ToHiLo(D);
		const float Hi[3] = { HL.Hi.X, HL.Hi.Y, HL.Hi.Z };
		const float Lo[3] = { HL.Lo.X, HL.Lo.Y, HL.Lo.Z };
		for (int I = 0; I < 3; ++I)
		{
			if (D.Mag[I].IsZero()) { continue; }
			// Exact error |Mag - (|Hi| +- |Lo|)| in integers. Hi, Lo are integer valued here
			// because Mag is an integer and conversions of integers >= 2^24 are integers.
			const FU128 HiE = ExactMag(Hi[I]);
			const FU128 LoE = ExactMag(Lo[I]);
			const bool LoSameSign = (Lo[I] == 0.0f) || ((Lo[I] < 0.0f) == (Hi[I] < 0.0f));
			const FU128 Approx = LoSameSign ? Add(HiE, LoE) : Sub(HiE, LoE);
			const FU128 Err = Abs(Sub(D.Mag[I], Approx));
			if (Err.IsZero()) { continue; }
			const double Rel = U128ToDouble(Err) / U128ToDouble(D.Mag[I]);
			const double L2 = std::log2(Rel);
			WorstLog2 = std::max(WorstLog2, L2);
			if (L2 > -46.0) { ++Over; }
		}
	}

	Uni::Check(1, "T1.coord.hilo", "ToHiLo relative error <= 2^-46 over the fuzz set")
		.Metric("cases", N).Metric("worst_log2_rel_error", WorstLog2).Metric("over_limit", double(Over))
		.Pass(Over == 0, Uni::Fmt("worst relative error 2^%.2f over %d cases x 3 axes (limit 2^-46), %lld over", WorstLog2, N, Over));
}
