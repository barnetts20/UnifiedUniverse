#pragma once

// UniVoxelPlanetFactory.h
// D-293 (SHIPS_PLANETS.md §4): the universe's planets as VoxelPlanets' APlanetActor. The planet's
// desc (UniPlanetProps.h: its surface seed, air, seas, pull) goes onto the actor before its first
// tick; the actor is spawned at its true radius (the actor's scale), at the planet's frame's origin,
// and builds itself (its terrestrial bake, then terrain, ocean, atmosphere and gravity zone).
//
//   uu.Planet.Class <path>            the class for rocky planets (empty: APlanetActor; set it to
//                                     the project's planet Blueprint for its materials and sets)
//   uu.Planet.GasGiantClass <path>    the class for gas giants (empty: none, the placeholder stays)
//   uu.Planet.Surface <0|1>           0: the noise surface instead of the terrestrial generator

#include "CoreMinimal.h"
#include "UniBodyFactory.h"
#include "UniVoxelPlanetFactory.generated.h"

class APlanetActor;

UCLASS()
class UNIFIEDUNIVERSEGAME_API UUniVoxelPlanetFactory : public UUniBodyFactory
{
	GENERATED_BODY()

public:
	virtual bool Supports(const FUniBodyDesc& Desc) const override;
	virtual AActor* Acquire(UWorld* World, const FUniBodyDesc& Desc, const FVector& WorldLocation) override;
	virtual void UpdateBody(AActor* Body, const FUniBodyDesc& Desc, const FVector& StarWorldLocation) override;
	virtual bool IsReady(AActor* Body) const override;
	virtual FUniGroundSample GroundAt(AActor* Body, const FVector& WorldPoint) const override;
	virtual void Park(AActor* Body) override;
	virtual void Wake(AActor* Body, const FUniBodyDesc& Desc) override;
	virtual void Release(AActor* Body) override;
	virtual void SetClock(double UniverseSecondsPerSecond, double SolarDayS) override;

private:
	/** The class for a desc (null: this factory makes none). Loaded once per path. */
	UClass* ClassFor(const FUniBodyDesc& Desc) const;
	/** The desc's settings onto a planet not yet built (before its first tick). */
	static void Configure(APlanetActor& P, const FUniBodyDesc& Desc);
	/** Whether the planet's density can answer yet (GetWaterAt is all defaults until then). */
	static bool DensityReady(const APlanetActor& P);

	mutable FString LoadedPath[2];
	mutable TWeakObjectPtr<UClass> Loaded[2];

	/** D-301: SetClock's universe seconds per world second and solar day (s). */
	double ClockRate = 1.0;
	double SolarDay = 0.0;
};
