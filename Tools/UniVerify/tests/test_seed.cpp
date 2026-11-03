// test_seed.cpp
// Tier 1 seed checks: channel constants, chi-squared spread for sequential roots, injectivity of
// identity seeds, and the FRandomStream port. The cross-language check is oracle/seed_oracle.py
// against `univerify seeddump`; engine parity is UE-2.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "UniSeed.h"

#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	/** Wilson-Hilferty chi-squared quantile. */
	double ChiQuantile(double Dof, double Z)
	{
		const double A = 2.0 / (9.0 * Dof);
		const double T = 1.0 - A + Z * std::sqrt(A);
		return Dof * T * T * T;
	}

	template <typename F>
	double ChiSquared64(int N, F&& Value01)
	{
		std::vector<double> Bins(64, 0.0);
		for (int I = 0; I < N; ++I)
		{
			const double V = Value01(I);
			int B = int(V * 64.0);
			B = B < 0 ? 0 : (B > 63 ? 63 : B);
			Bins[size_t(B)] += 1.0;
		}
		const double E = double(N) / 64.0;
		double Chi = 0.0;
		for (double B : Bins) { Chi += (B - E) * (B - E) / E; }
		return Chi;
	}
}

UNI_TEST(seed, channels)
{
	struct { const char* Name; u32 Value; } Ch[] = {
		{ "OBJECT", H::UNI_CH_OBJECT }, { "COUNT", H::UNI_CH_COUNT }, { "PROBE", H::UNI_CH_PROBE },
		{ "PLACE", H::UNI_CH_PLACE }, { "ACCEPT", H::UNI_CH_ACCEPT }, { "MAG", H::UNI_CH_MAG },
		{ "RADIUS", H::UNI_CH_RADIUS }, { "COLOUR", H::UNI_CH_COLOUR }, { "ORIENT", H::UNI_CH_ORIENT },
		{ "TILE", H::UNI_CH_TILE }, { "ANCHOR", H::UNI_CH_ANCHOR }, { "CONTRIBUTOR", H::UNI_CH_CONTRIBUTOR },
		{ "SLICE", H::UNI_CH_SLICE }, { "SHAPE", H::UNI_CH_SHAPE }, { "SYSTEM", H::UNI_CH_SYSTEM }, { "CLUMP", H::UNI_CH_CLUMP }, { "MORPH", H::UNI_CH_MORPH },
	};
	int Bad = 0;
	std::unordered_set<u32> Seen;
	for (const auto& C : Ch)
	{
		if (ChannelId(C.Name) != C.Value) { ++Bad; }
		if (ChannelIdUpper(std::string(C.Name)) != C.Value) { ++Bad; }
		if (!Seen.insert(C.Value).second) { ++Bad; } // distinct
	}
	// Case-insensitivity of the runtime form (FName semantics).
	if (ChannelIdUpper("ArmRadius") != ChannelIdUpper("ARMRADIUS") || ChannelIdUpper("armradius") != ChannelId("ARMRADIUS")) { ++Bad; }
	// Compile-time evaluation.
	static_assert(ChannelId("OBJECT") == 0x1B2E0F7Au, "constexpr FNV-1a");

	Uni::Check(1, "T1.seed.channels", "Shader channel constants equal ChannelId(name), all distinct, runtime form case-insensitive")
		.Metric("channels", double(sizeof(Ch) / sizeof(Ch[0]))).Metric("bad", Bad)
		.Pass(Bad == 0, Uni::Fmt("%zu channels, %d problems", sizeof(Ch) / sizeof(Ch[0]), Bad));
}

UNI_TEST(seed, quality)
{
	const int N = 200000;
	const double Lo = ChiQuantile(63.0, -2.5758), Hi = ChiQuantile(63.0, 2.5758);

	struct FCase { const char* Name; double Chi; };
	std::vector<FCase> Cases;

	Cases.push_back({ "MixSeed(root, PLACE)", ChiSquared64(N, [](int I) { return double(H::Uni_MixSeed(u32(I), H::UNI_CH_PLACE)) / 2147483648.0; }) });
	Cases.push_back({ "MixSeedIdx(7, PLACE, i)", ChiSquared64(N, [](int I) { return double(H::Uni_MixSeedIdx(7u, H::UNI_CH_PLACE, u32(I))) / 2147483648.0; }) });
	Cases.push_back({ "FRandomStream(MixSeed(root)).FRand", ChiSquared64(N, [](int I) { FRandomStreamLite S(MixSeed(I, H::UNI_CH_MAG)); return double(S.FRand()); }) });
	Cases.push_back({ "CellSeed(sequential x cells)", ChiSquared64(N, [](int I) {
		FCellKey K = CellOf(FCoord::Origin(), 60);
		K = Neighbour(K, I, 0, 0);
		return double(CellSeed(69u, K, 1u, 0u, 0u)) / 4294967296.0; }) });
	Cases.push_back({ "ObjectSeed(cell, i)", ChiSquared64(N, [](int I) { return double(H::Uni_ObjectSeed(0x1234u, u32(I))) / 4294967296.0; }) });
	Cases.push_back({ "DrawUnit(root, PLACE, 0)", ChiSquared64(N, [](int I) { return double(H::Uni_DrawUnit(u32(I), H::UNI_CH_PLACE, 0u)); }) });
	Cases.push_back({ "DrawUnit(seed, PLACE, k)", ChiSquared64(N, [](int I) { return double(H::Uni_DrawUnit(99u, H::UNI_CH_PLACE, u32(I))); }) });

	// The ProcSeed lesson, reproduced: without Avalanche, HashCombine of sequential roots is
	// badly clustered. Reported, not judged (it is the reason Avalanche exists).
	const double NoAvalanche = ChiSquared64(N, [](int I) { return double(H::Uni_HashCombine(u32(I), H::UNI_CH_PLACE) & 0x7FFFFFFFu) / 2147483648.0; });

	int Bad = 0;
	std::string Detail;
	for (const FCase& C : Cases)
	{
		const bool Ok = C.Chi >= Lo && C.Chi <= Hi;
		Bad += Ok ? 0 : 1;
		Detail += Uni::Fmt("%s%s=%.1f", Detail.empty() ? "" : ", ", C.Name, C.Chi);
	}
	auto Ch = Uni::Check(1, "T1.seed.quality", "Chi-squared over 64 bins within the 99% band for sequential inputs (Avalanche/MixSeed/CellSeed/ObjectSeed/draws)");
	Ch.Metric("band_lo", Lo).Metric("band_hi", Hi).Metric("failing", Bad).Metric("no_avalanche_chi2", NoAvalanche);
	Ch.Pass(Bad == 0, Uni::Fmt("band [%.1f, %.1f], 200k sequential inputs: %s. (Without Avalanche: %.3g, ProcSeed lesson reproduced)", Lo, Hi, Detail.c_str(), NoAvalanche));
}

UNI_TEST(seed, injective)
{
	// Identity seeds must not collide inside a cell (D-109): 2^15 generated indices per cell.
	long long Collisions = 0;
	for (u32 Cell = 0; Cell < 300u; ++Cell)
	{
		const u32 CS = H::Uni_Avalanche(Cell * 2654435761u + 17u);
		std::unordered_set<u32> Seen;
		Seen.reserve(40000);
		for (u32 I = 0; I < 32768u; ++I)
		{
			if (!Seen.insert(H::Uni_ObjectSeed(CS, I)).second) { ++Collisions; }
		}
	}
	// Neighbouring cells must not share seeds: CellSeed over a 64^3 block.
	long long CellCollisions = 0;
	{
		std::unordered_set<u32> Seen;
		Seen.reserve(300000);
		const FCellKey Base = CellOf(FCoord::Origin(), 70);
		for (int X = 0; X < 64; ++X) for (int Y = 0; Y < 64; ++Y) for (int Z = 0; Z < 64; ++Z)
		{
			if (!Seen.insert(CellSeed(69u, Neighbour(Base, X, Y, Z), 3u, 0u, 0u)).second) { ++CellCollisions; }
		}
	}
	// 262,144 cell seeds in 2^32: birthday expectation is about 8 collisions; allow 40.
	Uni::Check(1, "T1.seed.injective", "Object seeds injective in index (0 collisions over 300 cells x 2^15); cell seeds near birthday bound")
		.Metric("object_collisions", double(Collisions)).Metric("cell_collisions_64cubed", double(CellCollisions))
		.Pass(Collisions == 0 && CellCollisions <= 40,
			Uni::Fmt("object-seed collisions %lld (must be 0); cell-seed collisions over 64^3 cells %lld (birthday ~8)", Collisions, CellCollisions));
}

UNI_TEST(seed, stream_port)
{
	// FRandomStream semantics: an independent spelling of the engine algorithm.
	int Bad = 0;
	for (int Root = -1000; Root < 1000; ++Root)
	{
		u32 S = u32(Root);
		FRandomStreamLite L(Root);
		for (int K = 0; K < 8; ++K)
		{
			S = S * 196314165u + 907633515u;
			const u32 Bits = 0x3F800000u | (S >> 9);
			float F; std::memcpy(&F, &Bits, 4);
			if (L.FRand() != F - 1.0f) { ++Bad; }
		}
	}
	// RandRange stays inside [Min, Max] and hits both ends.
	FRandomStreamLite R(42);
	int MinHits = 0, MaxHits = 0, Out = 0;
	for (int I = 0; I < 10000; ++I)
	{
		const int V = R.RandRange(2, 6);
		Out += (V < 2 || V > 6) ? 1 : 0;
		MinHits += V == 2; MaxHits += V == 6;
	}
	// RollRange: Quantize snaps in absolute terms and re-clamps (ULS RollRange).
	const float Q = H::Uni_RollRange(5u, ChannelIdUpper("ArmCount"), 2.0f, 6.0f, 1.0f, 1.0f);
	const bool QOk = Q == std::floor(Q) && Q >= 2.0f && Q <= 6.0f;
	Uni::Check(1, "T1.seed.stream_port", "FRandomStream port: FRand sequence, RandRange bounds, RollRange quantize/clamp")
		.Metric("mismatches", Bad).Metric("out_of_range", Out)
		.Pass(Bad == 0 && Out == 0 && MinHits > 0 && MaxHits > 0 && QOk,
			Uni::Fmt("16,000 FRand draws, %d mismatches; RandRange(2,6) out of range %d, ends hit %d/%d; quantized roll %.1f", Bad, Out, MinHits, MaxHits, Q));
}
