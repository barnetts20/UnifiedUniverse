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

/** D-272: a galaxy's supermassive black hole (Docs/COMPACT_OBJECTS.md), from the galaxy's record:
 *  the same numbers its nucleus is drawn from. */
USTRUCT(BlueprintType)
struct UNIFIEDUNIVERSE_API FUniBlackHoleInfo
{
	GENERATED_BODY()

	/** False when the object is not a resident galaxy. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	bool bValid = false;

	/** Where it is: the galaxy's centre. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	FUniCoord Coord;

	/** Solar masses (~0.0014 x the bulge's mass, 0.3 dex scatter). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	double MassSolar = 0.0;

	/** Its accretion rate over the Eddington rate (1e-9 quiet .. 1 a quasar's). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	double EddingtonRatio = 0.0;

	/** Its accretion's bolometric luminosity, erg/s. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	double LuminosityErgS = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	double SchwarzschildRadiusCm = 0.0;

	/** The nucleus the galaxy shows, V absolute magnitude (99: too faint to draw). D-273: its mean
	 *  over directions (it is brighter seen face-on to its disc, dimmer edge-on). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	float NucleusAbsoluteMagnitude = 99.0f;

	/** D-273: its accretion disc's axis (unit, world axes; the disc turns counter-clockwise about it). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	FVector DiscAxis = FVector(0.0, 0.0, 1.0);

	/** D-273: its accretion disc's outer radius, cm (from the innermost stable orbit, 3 Schwarzschild radii). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	double DiscOuterRadiusCm = 0.0;

	/** D-273: its accretion disc's peak temperature, K. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|BlackHole")
	float DiscPeakTemperatureK = 0.0f;
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
