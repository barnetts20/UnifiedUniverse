// fuzzdump.cpp
// Writes coordinate fuzz cases, inputs plus the C++ (UniCoord.h) and shader-core results, for
// oracle/coord_oracle.py to recompute with Python big integers. The record layout is mirrored
// by FUZZ_DTYPE in the oracle; change both together.

#include "UniTest.h"
#include "UniShaderCores.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
#pragma pack(push, 1)
	struct FFuzzRecord
	{
		// Inputs
		u32 A[12]; u32 B[12]; u32 Level; int32_t Step[3]; u32 Off[3]; float Frac0[3]; double Disp[3];
		// Outputs
		u32 DeltaTc[12];   // Sub(A, B) as two's complement per axis
		double Dbl[3];     // ToDouble(Sub(A, B))
		float Hi[3];       // ToHiLo
		float Lo[3];
		u32 Cell[12];      // CellOf(A, L)
		u32 Neigh[12];     // Neighbour(CellOf(A, L), Step)
		u32 Added[12];     // AddDisplacement(A, Frac0, Disp)
		float FracOut[3];
		float ObjRel[3];   // Uni_ObjectRelative(CellOrigin(CellOf(A,L)) - B, Off, L, Frac0), shader core
	};
#pragma pack(pop)
	static_assert(sizeof(FFuzzRecord) == 424, "FFuzzRecord layout is mirrored by coord_oracle.py");

	void Put(u32* Out, const FCoord& C) { for (int I = 0; I < 3; ++I) { const FU128 V = C.Axis(I); for (int N = 0; N < 4; ++N) { Out[I * 4 + N] = V.L[N]; } } }
	void PutAxis(u32* Out, int I, const FU128& V) { for (int N = 0; N < 4; ++N) { Out[I * 4 + N] = V.L[N]; } }

	FCoord RandCoord(Uni::FTestRng& R)
	{
		FCoord C;
		for (int I = 0; I < 3; ++I)
		{
			const unsigned M = R.U32() % 6u;
			if (M == 0u) { C.SetAxis(I, FU128::Make(0u, 0u, R.EdgeLimb(), R.EdgeLimb())); }
			else if (M == 1u) { C.SetAxis(I, FU128::Make(0x80000000u, 0u, R.EdgeLimb(), R.EdgeLimb())); }
			else { C.SetAxis(I, FU128::Make(R.EdgeLimb(), R.EdgeLimb(), R.EdgeLimb(), R.EdgeLimb())); }
		}
		return C;
	}

	double RandDisp(Uni::FTestRng& R)
	{
		const unsigned M = R.U32() % 5u;
		const double S = (R.U32() & 1u) ? -1.0 : 1.0;
		if (M == 0u) { return S * R.Unit(); }
		if (M == 1u) { return S * std::ldexp(R.Unit(), int(R.U32() % 64u)); }
		if (M == 2u) { return S * std::ldexp(1.0 + R.Unit(), 60 + int(R.U32() % 90u)); }
		if (M == 3u) { return S * std::ldexp(1.0 + R.Unit(), 128 + int(R.U32() % 100u)); }
		return 0.0;
	}
}

int UniFuzzDump(int N, const char* Path)
{
	FILE* F = std::fopen(Path, "wb");
	if (!F) { std::fprintf(stderr, "cannot open %s\n", Path); return 2; }
	Uni::FTestRng R(0x0AC1E5EEDull);
	std::vector<FFuzzRecord> Buf(4096);
	int Written = 0;
	while (Written < N)
	{
		const int Batch = std::min<int>(int(Buf.size()), N - Written);
		for (int K = 0; K < Batch; ++K)
		{
			FFuzzRecord& Rec = Buf[size_t(K)];
			const FCoord A = RandCoord(R);
			FCoord B = RandCoord(R);
			if (R.U32() & 1u)
			{
				// Near pair: B = A + a random signed delta of random width.
				for (int I = 0; I < 3; ++I)
				{
					FU128 D = MaskLow(FU128::Make(R.U32(), R.U32(), R.U32(), R.U32()), int(R.U32() % 129u));
					if (R.U32() & 1u) { D = Neg(D); }
					B.SetAxis(I, Add(A.Axis(I), D));
				}
			}
			const int L = int(R.U32() % 129u);
			Put(Rec.A, A); Put(Rec.B, B);
			Rec.Level = u32(L);
			for (int I = 0; I < 3; ++I)
			{
				Rec.Step[I] = int32_t(R.U32() % 9u) - 4;
				Rec.Off[I] = R.U32();
				Rec.Frac0[I] = float(R.Unit()) * 0.999f;
				Rec.Disp[I] = RandDisp(R);
			}

			const FDelta D = Sub(A, B);
			const FVec3d Dd = ToDouble(D);
			const FHiLo HL = ToHiLo(D);
			for (int I = 0; I < 3; ++I) { PutAxis(Rec.DeltaTc, I, D.TwosComplement(I)); }
			Rec.Dbl[0] = Dd.X; Rec.Dbl[1] = Dd.Y; Rec.Dbl[2] = Dd.Z;
			Rec.Hi[0] = HL.Hi.X; Rec.Hi[1] = HL.Hi.Y; Rec.Hi[2] = HL.Hi.Z;
			Rec.Lo[0] = HL.Lo.X; Rec.Lo[1] = HL.Lo.Y; Rec.Lo[2] = HL.Lo.Z;

			const FCellKey K0 = CellOf(A, L);
			const FCellKey Kn = Neighbour(K0, Rec.Step[0], Rec.Step[1], Rec.Step[2]);
			for (int I = 0; I < 3; ++I) { PutAxis(Rec.Cell, I, K0.C[I]); PutAxis(Rec.Neigh, I, Kn.C[I]); }

			FCoord Moved = A;
			FVec3f Fr{ Rec.Frac0[0], Rec.Frac0[1], Rec.Frac0[2] };
			AddDisplacement(Moved, Fr, FVec3d{ Rec.Disp[0], Rec.Disp[1], Rec.Disp[2] });
			Put(Rec.Added, Moved);
			Rec.FracOut[0] = Fr.X; Rec.FracOut[1] = Fr.Y; Rec.FracOut[2] = Fr.Z;

			const FDelta CD = Sub(CellOrigin(K0), B);
			const H::float3 Rel = H::Uni_ObjectRelative(ToUint4(CD.TwosComplement(0)), ToUint4(CD.TwosComplement(1)), ToUint4(CD.TwosComplement(2)),
				H::uint3(Rec.Off[0], Rec.Off[1], Rec.Off[2]), u32(L), H::float3(Rec.Frac0[0], Rec.Frac0[1], Rec.Frac0[2]));
			Rec.ObjRel[0] = Rel.x; Rec.ObjRel[1] = Rel.y; Rec.ObjRel[2] = Rel.z;
		}
		std::fwrite(Buf.data(), sizeof(FFuzzRecord), size_t(Batch), F);
		Written += Batch;
	}
	std::fclose(F);
	std::printf("wrote %d records to %s\n", N, Path);
	return 0;
}
