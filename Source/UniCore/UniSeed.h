#pragma once

// UniSeed.h
// CPU face of UniSeed.ush: the same functions (compiled from the shader core through the
// shim) plus the parts HLSL cannot express, namely channel ids from names.
//
// Port of UltraLargeScale's ProcSeed (ProceduralSpaceActor.h), with its reasoning kept:
//   - ChannelId(const char*) is compile-time FNV-1a over the name, so the identifier IS the
//     name and nobody invents magic constants.
//   - ChannelIdUpper(text) is the runtime form for channels that are data (a rolled parameter
//     identified by its property name). It uppercases first, because FName comparison is
//     case-insensitive: "ArmRadius" and "armradius" are the same property and must be the
//     same stream. It hashes characters, never GetTypeHash(FName), whose value depends on
//     name-table registration order and so differs between runs.
//   - Avalanche (fmix32) is not optional: see UniSeed.ush.
//
// The UE-side ChannelIdUpper over an FName must hash the same characters. FName::ToString()
// is TCHAR (UTF-16 on Windows). For ASCII names the code units equal the bytes hashed here,
// so the two agree. UE-2 checks that on real names.

#include "UniShaderCores.h"

#include <cstdint>
#include <string>

namespace UniCore
{
	/** Compile-time FNV-1a, exactly ProcSeed::ChannelId(const char*). Callers pass the
	 *  uppercase spelling for code channels (the UNI_CH_* constants are uppercase names). */
	constexpr u32 ChannelId(const char* Name)
	{
		u32 Hash = 2166136261u;
		while (*Name)
		{
			Hash ^= static_cast<u32>(static_cast<unsigned char>(*Name++));
			Hash *= 16777619u;
		}
		return Hash;
	}

	/** Runtime form for data channels: ASCII-uppercased first (ProcSeed::ChannelId(FName)). */
	inline u32 ChannelIdUpper(const std::string& Name)
	{
		u32 Hash = 2166136261u;
		for (unsigned char C : Name)
		{
			if (C >= 'a' && C <= 'z') { C = static_cast<unsigned char>(C - 'a' + 'A'); }
			Hash ^= static_cast<u32>(C);
			Hash *= 16777619u;
		}
		return Hash;
	}

	inline u32 Avalanche(u32 V) { return UniHlsl::Uni_Avalanche(V); }
	inline u32 HashCombine(u32 A, u32 C) { return UniHlsl::Uni_HashCombine(A, C); }
	inline int32_t MixSeed(int32_t Root, u32 Channel) { return int32_t(UniHlsl::Uni_MixSeed(u32(Root), Channel)); }
	inline int32_t MixSeed(int32_t Root, u32 Channel, int32_t Index) { return int32_t(UniHlsl::Uni_MixSeedIdx(u32(Root), Channel, u32(Index))); }

	/** FRandomStream's generator, engine-free. Same sequence as FRandomStream(InitialSeed)
	 *  for FRand / GetUnsignedInt / RandRange (verified against the engine by UE-2). */
	class FRandomStreamLite
	{
	public:
		explicit FRandomStreamLite(int32_t InSeed) : Seed(u32(InSeed)) {}
		float FRand() { return UniHlsl::Uni_StreamFraction(Seed); }
		u32 GetUnsignedInt() { return UniHlsl::Uni_StreamUint(Seed); }
		int32_t RandRange(int32_t Min, int32_t Max) { return UniHlsl::Uni_StreamRandRange(Seed, Min, Max); }
		u32 State() const { return Seed; }
	private:
		u32 Seed;
	};

	/** ProcSeed::Stream: pure, so call it ONCE and draw from the local. */
	inline FRandomStreamLite Stream(int32_t Root, u32 Channel) { return FRandomStreamLite(MixSeed(Root, Channel)); }
	inline FRandomStreamLite Stream(int32_t Root, u32 Channel, int32_t Index) { return FRandomStreamLite(MixSeed(Root, Channel, Index)); }

	inline u32 CellSeed(u32 UniverseSeed, const FCellKey& K, u32 ArchetypeId, u32 ContributorId, u32 EmitterIndex)
	{
		return UniHlsl::Uni_CellSeed(UniverseSeed, u32(K.Level), ToUint4(K.C[0]), ToUint4(K.C[1]), ToUint4(K.C[2]),
			ArchetypeId, ContributorId, EmitterIndex);
	}
}
