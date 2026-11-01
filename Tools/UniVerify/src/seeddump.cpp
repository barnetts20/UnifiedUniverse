// seeddump.cpp
// Prints seed values for fixed inputs as JSON lines, for oracle/seed_oracle.py.

#include "UniTest.h"
#include "UniSeed.h"

#include <cstdio>
#include <cstring>

using namespace UniCore;
namespace H = UniHlsl;

int UniSeedDump()
{
	std::printf("{\"variant_fast\":%d}\n", UNI_SEED_HASHCOMBINE_FAST);
	Uni::FTestRng R(0x5EED0001ull);
	for (int I = 0; I < 2000; ++I)
	{
		const u32 A = (I < 64) ? u32(I) : R.U32();
		const u32 C = R.U32();
		const u32 Idx = R.U32() & 0xFFFFu;
		u32 S = H::Uni_MixSeed(A, C);
		const u32 Root = S;
		u32 Fr[4];
		for (int K = 0; K < 4; ++K) { const float F = H::Uni_StreamFraction(S); std::memcpy(&Fr[K], &F, 4); }
		const u32 L = R.U32() % 129u;
		const H::uint4 X(R.U32(), R.U32(), R.U32(), R.U32()), Y(R.U32(), R.U32(), R.U32(), R.U32()), Z(R.U32(), R.U32(), R.U32(), R.U32());
		const u32 Cell = H::Uni_CellSeed(A, L, X, Y, Z, C & 0xFFFFu, Idx, I & 7);
		const u32 Obj = H::Uni_ObjectSeed(Cell, Idx);
		const u32 Draw = H::Uni_DrawU32(Cell, C, Idx);
		std::printf("{\"a\":%u,\"c\":%u,\"idx\":%u,\"av\":%u,\"hc\":%u,\"mix\":%u,\"mixi\":%u,\"fr\":[%u,%u,%u,%u],"
			"\"L\":%u,\"X\":[%u,%u,%u,%u],\"Y\":[%u,%u,%u,%u],\"Z\":[%u,%u,%u,%u],\"cell\":%u,\"obj\":%u,\"draw\":%u}\n",
			A, C, Idx, H::Uni_Avalanche(A), H::Uni_HashCombine(A, C), Root, H::Uni_MixSeedIdx(A, C, Idx), Fr[0], Fr[1], Fr[2], Fr[3],
			L, X.x, X.y, X.z, X.w, Y.x, Y.y, Y.z, Y.w, Z.x, Z.y, Z.z, Z.w, Cell, Obj, Draw);
	}
	const char* Names[] = { "ArmRadius", "armradius", "Disc.Radius", "NoiseOffset.X", "OBJECT", "x", "" };
	for (const char* N : Names) { std::printf("{\"name\":\"%s\",\"channel\":%u}\n", N, ChannelIdUpper(N)); }
	return 0;
}
