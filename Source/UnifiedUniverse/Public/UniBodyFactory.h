// UniBodyFactory.h
// D-293 (SHIPS_PLANETS.md §4): a planet's detailed actor (a world to stand on) comes from a body
// factory that a game module registers, so the universe's own modules never depend on the planet
// plugin. The universe decides when a planet needs its detailed actor (its frame, D-291), where it
// is, and what it is (its desc, from UniPlanetProps.h); the factory makes it, keeps it lit by its
// star, says when it is ready to replace the placeholder, answers ground queries for the start's
// spawn point, and parks or releases it. Without a factory, planets keep their placeholder spheres.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UniBodyFactory.generated.h"

/** What a planet is, as a world (UniCore::FPlanetProps plus its identity). */
USTRUCT(BlueprintType)
struct UNIFIEDUNIVERSE_API FUniBodyDesc
{
	GENERATED_BODY()

	/** 0 rocky, 1 gas giant, 2 ice giant (UniCore::EPlanetType). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	int32 PlanetType = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	double RadiusCm = 0.0;

	/** The planet's own seed (uint32), its star's, and its ordinal in the system. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	int64 PlanetSeed = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	int64 StarSeed = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	int32 Ordinal = -1;

	/** The seed its surface grows from (channel PLANET_SURFACE, uint32). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	int64 SurfaceSeed = 0;

	/** 0 none, 1 air alone, 2 terrestrial (clouds and weather), 3 gas giant (UniCore::EPlanetAtmo). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	int32 Atmosphere = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	bool bOcean = false;

	/** Surface gravity, cm/s^2 (within play's limits). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	double SurfaceGravity = 980.665;

	/** Equilibrium temperature, K. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	double TeqK = 0.0;
};

/** The ground under a point, as the planet's own density has it (before any terrain is meshed). */
USTRUCT(BlueprintType)
struct UNIFIEDUNIVERSE_API FUniGroundSample
{
	GENERATED_BODY()

	/** False until the planet can answer (its density built). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	bool bValid = false;

	/** No sea, lake or river there. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	bool bDry = false;

	/** The ground's height, m above sea level. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	double HeightM = 0.0;

	/** The ground's distance from the planet's centre, cm (sea level need not be the planet's
	 *  radius: a noise surface's sea is above it). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe|Body")
	double GroundRadiusCm = 0.0;
};

/** Makes planets' detailed actors. Subclass it in a game module and register the class with
 *  UUniSubsystem::RegisterBodyFactoryClass (from the module's startup). */
UCLASS(Abstract)
class UNIFIEDUNIVERSE_API UUniBodyFactory : public UObject
{
	GENERATED_BODY()

public:
	/** Whether this factory makes this kind of planet. */
	virtual bool Supports(const FUniBodyDesc& Desc) const { return false; }

	/** Make the planet's actor at WorldLocation (it then builds itself over a few frames). */
	virtual AActor* Acquire(UWorld* World, const FUniBodyDesc& Desc, const FVector& WorldLocation) { return nullptr; }

	/** D-301: every frame, before the bodies' updates: universe seconds a second of world time
	 *  (uu.TimeScale) and the planets' solar day (uu.Planet.DayHours; 0: they do not turn), so a
	 *  body's own clocks (its weather) keep the planet's pace. */
	virtual void SetClock(double UniverseSecondsPerSecond, double SolarDayS) {}

	/** Every frame while live: its star's world position (parallax-placed: its direction is what
	 *  matters), so its sky and light face the star. */
	virtual void UpdateBody(AActor* Body, const FUniBodyDesc& Desc, const FVector& StarWorldLocation) {}

	/** Ready to replace the placeholder (built where the camera is). */
	virtual bool IsReady(AActor* Body) const { return true; }

	/** The ground under a world point, from the planet's density. */
	virtual FUniGroundSample GroundAt(AActor* Body, const FVector& WorldPoint) const { return FUniGroundSample(); }

	/** Park a body the frame has left: no per-frame work, kept for a warm return (D-292). */
	virtual void Park(AActor* Body) {}

	/** Wake a parked body (the same planet). */
	virtual void Wake(AActor* Body, const FUniBodyDesc& Desc) {}

	/** Let it go for good. */
	virtual void Release(AActor* Body);
};
