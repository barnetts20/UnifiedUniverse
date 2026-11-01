#pragma once

// UniCoordTypes.h
// Blueprint-facing wrappers for the engine-agnostic UniCore coordinate types (D-108).
//
// GAMEPLAY RULE (design doc, Spec: universal coordinate): absolute positions are never
// FVector. Anything that stores a universe position stores an FUniCoord. FVector is only for
// positions inside the real-space bubble, relative to the local anchor.
//
// Limbs are uint32 bit patterns, limb 0 most significant. They are UPROPERTYs so that saving,
// copying and details-panel display work with no custom serialisation. Blueprint graphs reach
// them through UUniCoordLibrary (hex strings, distances, cells) rather than limb by limb,
// because Blueprint has no uint32 or fixed-size array pins.

#include "CoreMinimal.h"
#include "UniCoord.h"
#include "UniIdentity.h"
#include "UniCoordTypes.generated.h"

USTRUCT(BlueprintType)
struct UNIFIEDUNIVERSECORE_API FUniCoord
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Universe|Coordinate")
	uint32 X[4];

	UPROPERTY(EditAnywhere, Category = "Universe|Coordinate")
	uint32 Y[4];

	UPROPERTY(EditAnywhere, Category = "Universe|Coordinate")
	uint32 Z[4];

	/** Defaults to the nominal origin, 2^127 on each axis (D-002), not to zero: zero is the
	 *  antipode of the origin on the root torus. */
	FUniCoord() { FromCore(UniCore::FCoord::Origin(), *this); }

	UniCore::FCoord ToCore() const
	{
		UniCore::FCoord C;
		for (int32 I = 0; I < 4; ++I) { C.X[I] = X[I]; C.Y[I] = Y[I]; C.Z[I] = Z[I]; }
		return C;
	}

	static void FromCore(const UniCore::FCoord& C, FUniCoord& Out)
	{
		for (int32 I = 0; I < 4; ++I) { Out.X[I] = C.X[I]; Out.Y[I] = C.Y[I]; Out.Z[I] = C.Z[I]; }
	}

	static FUniCoord Make(const UniCore::FCoord& C) { FUniCoord R; FromCore(C, R); return R; }

	bool operator==(const FUniCoord& O) const { return ToCore() == O.ToCore(); }
	bool operator!=(const FUniCoord& O) const { return !(*this == O); }

	FString ToHexString() const { return FString(UTF8_TO_TCHAR(UniCore::ToHexString(ToCore()).c_str())); }
};

FORCEINLINE uint32 GetTypeHash(const FUniCoord& C)
{
	const UniCore::FCoord K = C.ToCore();
	uint32 H = 0;
	for (int32 I = 0; I < 4; ++I) { H = HashCombine(H, HashCombine(K.X[I], HashCombine(K.Y[I], K.Z[I]))); }
	return H;
}

/** An implicit-octree cell: level L in [0, 128] and Coord >> L per axis. */
USTRUCT(BlueprintType)
struct UNIFIEDUNIVERSECORE_API FUniCellKey
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Universe|Cell", meta = (ClampMin = "0", ClampMax = "128"))
	int32 Level = 0;

	UPROPERTY(EditAnywhere, Category = "Universe|Cell")
	uint32 X[4];

	UPROPERTY(EditAnywhere, Category = "Universe|Cell")
	uint32 Y[4];

	UPROPERTY(EditAnywhere, Category = "Universe|Cell")
	uint32 Z[4];

	FUniCellKey()
	{
		for (int32 I = 0; I < 4; ++I) { X[I] = Y[I] = Z[I] = 0u; }
	}

	UniCore::FCellKey ToCore() const
	{
		UniCore::FCellKey K;
		K.Level = Level;
		K.C[0] = UniCore::FU128::Make(X[0], X[1], X[2], X[3]);
		K.C[1] = UniCore::FU128::Make(Y[0], Y[1], Y[2], Y[3]);
		K.C[2] = UniCore::FU128::Make(Z[0], Z[1], Z[2], Z[3]);
		return K;
	}

	static FUniCellKey Make(const UniCore::FCellKey& K)
	{
		FUniCellKey R;
		R.Level = K.Level;
		for (int32 I = 0; I < 4; ++I) { R.X[I] = K.C[0].L[I]; R.Y[I] = K.C[1].L[I]; R.Z[I] = K.C[2].L[I]; }
		return R;
	}

	bool operator==(const FUniCellKey& O) const { return ToCore() == O.ToCore(); }
	bool operator!=(const FUniCellKey& O) const { return !(*this == O); }
};

FORCEINLINE uint32 GetTypeHash(const FUniCellKey& K)
{
	const uint64 H = UniCore::HashCell(K.ToCore());
	return uint32(H) ^ uint32(H >> 32);
}

/** Stable object identity: (L, cell, index). Keys seeds, events and save deltas. */
USTRUCT(BlueprintType)
struct UNIFIEDUNIVERSECORE_API FUniObjectId
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Universe|Object")
	FUniCellKey Cell;

	/** Generated objects use 0 .. 2^15 - 1; authored anchors 2^15 and up. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Universe|Object")
	int32 Index = 0;

	UniCore::FObjectId ToCore() const
	{
		UniCore::FObjectId Id;
		Id.Cell = Cell.ToCore();
		Id.Index = uint32(Index);
		return Id;
	}

	static FUniObjectId Make(const UniCore::FObjectId& Id)
	{
		FUniObjectId R;
		R.Cell = FUniCellKey::Make(Id.Cell);
		R.Index = int32(Id.Index);
		return R;
	}

	/** 64-bit hash for maps and GPU lookup. */
	uint64 Hash64() const { return UniCore::HashObject(ToCore()); }

	bool operator==(const FUniObjectId& O) const { return Cell == O.Cell && Index == O.Index; }
	bool operator!=(const FUniObjectId& O) const { return !(*this == O); }
};

FORCEINLINE uint32 GetTypeHash(const FUniObjectId& Id)
{
	const uint64 H = Id.Hash64();
	return uint32(H) ^ uint32(H >> 32);
}
