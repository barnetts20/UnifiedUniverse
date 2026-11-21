#include "UniParallaxComponent.h"

#include "UniSubsystem.h"
#include "UniParallax.h"
#include "UniLocalFrame.h"
#include "UniSpin.h"

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

void UUniParallaxComponent::ApplyPlacement(const FVector& ViewLocation, const FUniCoord& CameraCoord, const FVector3f& CameraFraction, double WorldYaw)
{
	AActor* Owner = GetOwner();
	if (!Owner) { return; }
	const UniCore::FParallaxPlacement P = UniCore::ParallaxPlace(Coord.ToCore(), RadiusCm, CameraCoord.ToCore(),
		UniCore::FVec3f{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z }, bRealSpace);
	bRealSpace = P.bRealSpace;
	PlacementScale = P.Scale;
	TrueDistanceCm = P.Distance;
	// The offset in the world's axes (D-299: they turn against the universe's by WorldYaw); the
	// camera's world location is the eye.
	const UniCore::FVec3d Off = UniCore::RotateYaw(P.Offset, -WorldYaw);
	Owner->SetActorLocation(ViewLocation + FVector(Off.X, Off.Y, Off.Z), false, nullptr, ETeleportType::TeleportPhysics);
	Owner->SetActorScale3D(BaseScale * P.Scale);
}

void UUniParallaxComponent::ApplyPlacementLocal(const FVector& ViewLocation, const FUniCoord& CameraCoord, const FVector3f& CameraFraction, const FUniCoord& LocalAnchor, const FVector3f& LocalAnchorFrac, double WorldYaw)
{
	AActor* Owner = GetOwner();
	if (Mode == EUniParallaxMode::Simulated && bRealSpace && Owner)
	{
		// Physics owns the position: the universe follows the actor, not the other way round.
		const FVector Lw = Owner->GetActorLocation();
		const UniCore::FVec3d L = UniCore::RotateYaw(UniCore::FVec3d{ Lw.X, Lw.Y, Lw.Z }, WorldYaw);   // D-299: the universe's axes
		UniCore::FVec3f Frac{};
		// D-293: the origin's sub-cm part (a planet's exact centre in its frame).
		Coord = FUniCoord::Make(UniCore::LocalToUniversal(LocalAnchor.ToCore(), UniCore::FVec3d{ L.X + double(LocalAnchorFrac.X), L.Y + double(LocalAnchorFrac.Y), L.Z + double(LocalAnchorFrac.Z) }, Frac));
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
	ApplyPlacement(ViewLocation, CameraCoord, CameraFraction, WorldYaw);
}
