#pragma once

// UniBodyActor.h
// AUniBodyActor: the placeholder for a promoted star or planet (M3, D-147). A sphere at the
// body's true radius, placed by UUniParallaxComponent, tinted by the body's colour. It stands
// in until real body actors exist (the owner's planet will replace the planet case); the
// subsystem spawns one per promoted body and destroys it on demotion.
//
// Materials: no assets are shipped yet. The sphere uses the engine's basic-shape material with
// its "Color" parameter (lit by the subsystem's light rig); stars try a few engine emissive
// materials first and fall back to the same. Collision is off for the placeholder.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UniBodyActor.generated.h"

class UStaticMeshComponent;
class UUniParallaxComponent;
class UMaterialInstanceDynamic;

UCLASS()
class UNIFIEDUNIVERSE_API AUniBodyActor : public AActor
{
	GENERATED_BODY()

public:
	AUniBodyActor();

	/** Set up as a body of the given radius and colour (linear, brightest channel 1). */
	void InitBody(bool bInStar, double InRadiusCm, const FLinearColor& InColour, uint32 InBodyId);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Universe")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Universe")
	TObjectPtr<UUniParallaxComponent> Parallax;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Universe")
	bool bStar = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Universe")
	int64 BodyId = 0;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Universe")
	double RadiusCm = 0.0;

private:
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;
};
