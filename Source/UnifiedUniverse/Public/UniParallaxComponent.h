#pragma once

// UniParallaxComponent.h
// UUniParallaxComponent: puts its owning actor where the universe says it is (M3, D-146).
//
// Add it to any actor (a planet blueprint, a station, a ship, AUniBodyActor) and give it a
// universal coordinate. Every frame, after the subsystem has moved the universal camera, the
// owner is placed by the parallax rule (UniCore::ParallaxPlace):
//
//   - surface within r0 (10,000 km): real space, unit scale, at its true offset from the camera;
//   - otherwise: scaled about the eye by f(d)/d, so its direction, angular size and parallax are
//     exactly those of a real actor at its true distance, consistent with the backdrop sprites.
//
// The owner never moves by its own physics while placed this way, so at any camera speed it
// moves in step with everything else. The owner's authored scale is captured once (BaseScale)
// and multiplied by the placement scale. The switch between real space and compressed placement
// is invisible on screen (both preserve projection) and has 1.25x hysteresis.

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UniCoordTypes.h"
#include "UniParallaxComponent.generated.h"

UENUM(BlueprintType)
enum class EUniParallaxMode : uint8
{
	/** The universe places the owner every frame (bodies, scenery). */
	Placed,
	/** Physics or gameplay moves the owner while it is in real space in Local mode; the
	 *  component writes its position back into Coord. Placed as usual otherwise (D-149). */
	Simulated
};

UCLASS(ClassGroup = (UnifiedUniverse), meta = (BlueprintSpawnableComponent))
class UNIFIEDUNIVERSE_API UUniParallaxComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UUniParallaxComponent();

	/** The owner's centre in universal coordinates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Universe")
	FUniCoord Coord;

	/** Bounding radius of the owner at BaseScale, cm: decides when its surface is inside r0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Universe")
	double RadiusCm = 100.0;

	/** The owner's scale at unit (real-space) placement. Captured from the actor on BeginPlay
	 *  unless set explicitly before. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Universe")
	FVector BaseScale = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Universe")
	bool bCaptureBaseScaleOnBeginPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Universe")
	EUniParallaxMode Mode = EUniParallaxMode::Placed;

	/** Last placement: true while at unit scale in real space. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Universe")
	bool bRealSpace = false;

	/** Last placement scale (1 in real space). */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Universe")
	double PlacementScale = 1.0;

	/** Last true distance from the camera to the centre, cm. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Universe")
	double TrueDistanceCm = 0.0;

	UFUNCTION(BlueprintCallable, Category = "Universe")
	void SetCoord(const FUniCoord& InCoord) { Coord = InCoord; }

	/** Called by UUniSubsystem every frame: place the owner. ViewLocation is the camera's UE
	 *  world location this frame; CameraCoord and CameraFraction its universal coordinate. */
	void ApplyPlacement(const FVector& ViewLocation, const FUniCoord& CameraCoord, const FVector3f& CameraFraction);

	/** Local mode (D-149): LocalAnchor is the universal coordinate of the UE world origin. A
	 *  Simulated owner in real space is left where physics put it and Coord follows it. */
	void ApplyPlacementLocal(const FVector& ViewLocation, const FUniCoord& CameraCoord, const FVector3f& CameraFraction, const FUniCoord& LocalAnchor);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	bool bBaseScaleCaptured = false;
};
