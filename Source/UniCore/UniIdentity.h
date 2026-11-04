#pragma once

// UniIdentity.h
// Object identity: the exact tuple (L, cell limbs, index), plus a 64-bit hash for maps and GPU
// lookup. Identity never changes and keys an object's seed, events and save deltas.
//
// Indices from 2^15 upward are reserved for authored anchors, so they can never collide
// with generated objects (design doc, Spec: scale levels and object model).

#include "UniCoord.h"

namespace UniCore
{
	constexpr u32 kFirstAnchorIndex = 1u << 15;
	constexpr u32 kFirstCatalogueIndex = 1u << 20;   // D-167: catalogue objects (authored too: IsAnchor holds)
	constexpr u32 kFirstMemberIndex = 1u << 21;      // D-177: catalogue entries' authored bright members (entry x 64 + rank)

	struct FObjectId
	{
		FCellKey Cell;
		u32 Index = 0u;

		bool IsAnchor() const { return Index >= kFirstAnchorIndex; }

		friend bool operator==(const FObjectId& A, const FObjectId& B) { return A.Cell == B.Cell && A.Index == B.Index; }
		friend bool operator!=(const FObjectId& A, const FObjectId& B) { return !(A == B); }
	};

	/** splitmix64 finaliser. */
	inline u64 Mix64(u64 Z)
	{
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		return Z ^ (Z >> 31);
	}

	/** 64-bit hash of a cell key. For maps and lookups only. Seeds use UniSeed, which is
	 *  32-bit and shared with the GPU. */
	inline u64 HashCell(const FCellKey& K)
	{
		u64 H = Mix64(0x5EEDC0DEull + u64(u32(K.Level)));
		for (int I = 0; I < 3; ++I)
		{
			for (int N = 0; N < 4; ++N) { H = Mix64(H ^ (u64(K.C[I].L[N]) + 0x9E3779B97F4A7C15ull * u64(I * 4 + N + 1))); }
		}
		return H;
	}

	inline u64 HashObject(const FObjectId& Id)
	{
		return Mix64(HashCell(Id.Cell) ^ (u64(Id.Index) * 0xD6E8FEB86659FD93ull));
	}

	struct FCellKeyHasher { size_t operator()(const FCellKey& K) const { return size_t(HashCell(K)); } };
	struct FObjectIdHasher { size_t operator()(const FObjectId& K) const { return size_t(HashObject(K)); } };
}
