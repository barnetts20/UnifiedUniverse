#pragma once

// UniSubsystem.h
// UUniSubsystem: the one gameplay entry point (design: Spec: runtime API). Owns the universal
// camera, the streaming core (UniCore::FUniStreamer) with the GPU generation backend, and pushes
// the per-frame cell table to the renderer. Nothing outside the plugin touches GPU buffers or
// window state.
//
// Prototype scope (REVIEW_TONIGHT): camera, Local / Cruise motion modes (D-008), teleport,
// sensitivity, the scripted flythrough, stats. Resolve, query, pick, events, persistence and the
// promotion actor pool are stubs or absent (M3, M4).
//
// Console (any level, PIE or standalone):
//   uu.Enable [galaxy|web|flythrough]   start a universe at its demo camera
//   uu.Flythrough [seconds]             3 Gpc -> 1 m from a bright star (default 60 s)
//   uu.Cruise <cm/s>                    move along the view direction (0: Local mode)
//   uu.Teleport <solar|void>            jump the camera (coarse-to-fine rebuild)
//   uu.Sensitivity <dm>                 limiting-magnitude offset, -5..5
//   uu.Exposure <mag>                   exposure bias in magnitudes (negative = brighter)
//   uu.Stats                            log streaming and render statistics
//   uu.Disable

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "UniCoordTypes.h"
#include "UniSubsystem.generated.h"

namespace UniCore { class FUniverseModel; class FRefGenerator; class FUniStreamer; class IGenBackend; }

UENUM(BlueprintType)
enum class EUniMotionMode : uint8
{
	Local,    // the pawn moves in the main scene; camera = local anchor + view location
	Cruise    // the pawn is pinned; velocity integrates into the universal coordinate
};

UCLASS()
class UNIFIEDUNIVERSE_API UUniSubsystem : public UWorldSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bEnabled; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }

	/** Start a demo universe: "galaxy" (default), "web" or "flythrough". */
	UFUNCTION(BlueprintCallable, Category = "Universe")
	void EnableUniverse(const FString& Scene);

	UFUNCTION(BlueprintCallable, Category = "Universe")
	void DisableUniverse();

	UFUNCTION(BlueprintCallable, Category = "Universe")
	void SetCamera(const FUniCoord& Coord);

	/** Jump: windows rebuild coarse to fine over the following frames. */
	UFUNCTION(BlueprintCallable, Category = "Universe")
	void Teleport(const FUniCoord& Coord);

	/** cm/s along the view direction. Above 1e8 cm/s the mode switches to Cruise, below 5e7 back
	 *  to Local (D-008). */
	UFUNCTION(BlueprintCallable, Category = "Universe")
	void SetVelocity(const FVector& CmPerSecond);

	UFUNCTION(BlueprintPure, Category = "Universe")
	EUniMotionMode GetMotionMode() const { return MotionMode; }

	UFUNCTION(BlueprintCallable, Category = "Universe")
	void SetSensitivity(float DeltaMag);

	UFUNCTION(BlueprintPure, Category = "Universe")
	FUniCoord GetCameraCoord() const { return Camera; }

	/** Scripted dive from 3 Gpc to 1 m above a bright star of the anchored galaxy. */
	UFUNCTION(BlueprintCallable, Category = "Universe")
	void StartFlythrough(float Seconds = 60.0f);

	void SetExposureBias(float Mag) { ExposureBias = Mag; }
	void LogStats() const;

	static UUniSubsystem* FindForConsole();

private:
	void PushFrame();
	FVector GetViewLocation(FRotator* OutRotation = nullptr) const;

	bool bEnabled = false;
	FString SceneName;
	TUniquePtr<UniCore::FUniverseModel> Model;
	TUniquePtr<UniCore::FRefGenerator> Generator;
	TUniquePtr<UniCore::IGenBackend> Backend;
	TUniquePtr<UniCore::FUniStreamer> Streamer;

	FUniCoord Camera;
	FUniCoord LocalAnchor;
	FVector3f CameraFraction = FVector3f::ZeroVector;
	FVector Velocity = FVector::ZeroVector;
	EUniMotionMode MotionMode = EUniMotionMode::Local;
	float DeltaMag = 0.0f;
	float ExposureBias = 0.0f;
	uint64 FrameCounter = 0;

	// Flythrough
	bool bFlying = false;
	double FlyElapsed = 0.0, FlyDuration = 60.0;
	FUniCoord FlyTarget;
	double FlyTargetRadius = 0.0;
	FVector FlyDirection = FVector(0.35, 0.25, 0.9).GetSafeNormal();
};
