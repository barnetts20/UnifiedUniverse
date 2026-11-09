#pragma once

// UniApiTypes.h
// D-193: the runtime API's Blueprint-facing types (design doc, Spec: runtime API). The engine-
// agnostic logic behind them is UniCore's UniQuery.h; UUniSubsystem exposes the calls.

#include "CoreMinimal.h"
#include "UniCoordTypes.h"
#include "UniApiTypes.generated.h"

/** An object as the runtime API reports it: from a pick, a query, an event or a lookup. */
USTRUCT(BlueprintType)
struct UNIFIEDUNIVERSE_API FUniObjectInfo
{
	GENERATED_BODY()

	/** False when nothing was found (a failed pick or lookup). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	bool bValid = false;

	/** Stable identity. For a planet: its star's identity (see Planet). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	FUniObjectId Id;

	/** A planet of the star system of the star Id: its ordinal in the system. -1: the object Id itself. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	int32 Planet = -1;

	/** Exact position (at the time it was found: planets move on their orbits). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	FUniCoord Coord;

	/** The archetype's id in the universe definition (-1 for a planet). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	int32 ArchetypeId = -1;

	/** The archetype's name ("Star", "Galaxy", ... ; "Planet" for planets). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	FName Archetype;

	/** Absolute magnitude, V band (for a planet: its effective magnitude as drawn). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	float AbsoluteMagnitude = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	double RadiusCm = 0.0;

	/** Distance from the camera when it was reported. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	double DistanceCm = 0.0;

	/** The object's seed (its generated properties' stream). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Object")
	int32 Seed = 0;
};

/** What a resident query matches. */
USTRUCT(BlueprintType)
struct UNIFIEDUNIVERSE_API FUniQueryFilter
{
	GENERATED_BODY()

	/** Archetype ids to include (ids 0..31; empty: every archetype). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Universe|Query")
	TArray<int32> ArchetypeIds;

	/** Only objects at least this bright (absolute magnitude at most this). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Universe|Query")
	float MaxAbsoluteMagnitude = 99.0f;

	/** At most this many objects are returned (nearest first), 1..4096. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Universe|Query", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 MaxResults = 256;
};

/** A pick or FindNearest result: bFound false when nothing was there (or the request was lost). */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FUniOnPicked, bool, bFound, const FUniObjectInfo&, Object);

/** A resident query's result: the objects (nearest the centre first, at most the filter's
 *  MaxResults) and how many matched in all. */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FUniOnQueryResult, const TArray<FUniObjectInfo>&, Objects, int32, TotalMatches);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUniOnObjectEvent, const FUniObjectInfo&, Object);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUniOnObjectIdEvent, const FUniObjectId&, Id);
