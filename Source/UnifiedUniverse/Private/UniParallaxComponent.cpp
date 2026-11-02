#include "UniParallaxComponent.h"

#include "UniSubsystem.h"
#include "UniParallax.h"
#include "UniLocalFrame.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

UUniParallaxComponent::UUniParallaxComponent()
{
	// Placed by the subsystem after it moves the universal camera, not by its own tick.
	PrimaryComponentTick.bCanEverTick = false;
}

void UUniParallaxComponent::BeginPlay()
{
	Super::BeginPlay();
	if (bCaptureBaseScaleOnBeginPlay && !bBaseScaleCaptured && GetOwner())
	{
		BaseScale = GetOwner()->GetActorScale3D();
		bBaseScaleCaptured = true;
	}
	if (UWorld* World = GetWorld())
	{
		if (UUniSubsystem* S = World->GetSubsystem<UUniSubsystem>()) { S->RegisterParallax(this); }
	}
}

void UUniParallaxComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UWorld* World = GetWorld())
	{
		if (UUniSubsystem* S = World->GetSubsystem<UUniSubsystem>()) { S->UnregisterParallax(this); }
	}
	Super::EndPlay(Reason);
}

void UUniParallaxComponent::ApplyPlacement(const FVector& ViewLocation, const FUniCoord& CameraCoord, const FVector3f& CameraFraction)
{
	AActor* Owner = GetOwner();
	if (!Owner) { return; }
	const UniCore::FParallaxPlacement P = UniCore::ParallaxPlace(Coord.ToCore(), RadiusCm, CameraCoord.ToCore(),
		UniCore::FVec3f{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z }, bRealSpace);
	bRealSpace = P.bRealSpace;
	PlacementScale = P.Scale;
	TrueDistanceCm = P.Distance;
	// Universe axes are UE world axes (cm); the camera's world location is the eye.
	Owner->SetActorLocation(ViewLocation + FVector(P.Offset.X, P.Offset.Y, P.Offset.Z), false, nullptr, ETeleportType::TeleportPhysics);
	Owner->SetActorScale3D(BaseScale * P.Scale);
}

void UUniParallaxComponent::ApplyPlacementLocal(const FVector& ViewLocation, const FUniCoord& CameraCoord, const FVector3f& CameraFraction, const FUniCoord& LocalAnchor)
{
	AActor* Owner = GetOwner();
	if (Mode == EUniParallaxMode::Simulated && bRealSpace && Owner)
	{
		// Physics owns the position: the universe follows the actor, not the other way round.
		const FVector L = Owner->GetActorLocation();
		UniCore::FVec3f Frac{};
		Coord = FUniCoord::Make(UniCore::LocalToUniversal(LocalAnchor.ToCore(), UniCore::FVec3d{ L.X, L.Y, L.Z }, Frac));
		const UniCore::FParallaxPlacement P = UniCore::ParallaxPlace(Coord.ToCore(), RadiusCm, CameraCoord.ToCore(),
			UniCore::FVec3f{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z }, true);
		TrueDistanceCm = P.Distance;
		if (P.bRealSpace)
		{
			PlacementScale = 1.0;
			Owner->SetActorScale3D(BaseScale);
			return;
		}
		// Left real space: from here on the universe places it.
	}
	ApplyPlacement(ViewLocation, CameraCoord, CameraFraction);
}
