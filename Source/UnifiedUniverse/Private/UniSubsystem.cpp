#include "UniSubsystem.h"

#include "UnifiedUniverseModule.h"
#include "UniDemoScenes.h"
#include "UniRenderer.h"
#include "UniRenderHooks.h"
#include "UniStreamer.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "InputCoreTypes.h"
#include "UniScaleLadder.h"

namespace
{
	constexpr double kCruiseOn = 1.0e8;    // cm/s (D-008)
	constexpr double kCruiseOff = 5.0e7;
	constexpr double kAnchorRecentre = 1.0e8;

	TWeakObjectPtr<UUniSubsystem> GLastSubsystem;
}

bool UUniSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UUniSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	GLastSubsystem = this;
}

void UUniSubsystem::Deinitialize()
{
	DisableUniverse();
	Super::Deinitialize();
}

TStatId UUniSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UUniSubsystem, STATGROUP_Tickables);
}

UUniSubsystem* UUniSubsystem::FindForConsole()
{
	return GLastSubsystem.Get();
}

void UUniSubsystem::EnableUniverse(const FString& Scene)
{
	DisableUniverse();
	if (!FUniRenderer::ShadersEnabled())
	{
		UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: uu.Shaders=0, so the plugin's shaders were not compiled and nothing can render. Remove the setting once the shader errors are fixed (Docs/BUILD_TRIAGE.md)."));
		return;
	}
	SceneName = Scene.IsEmpty() ? TEXT("galaxy") : Scene.ToLower();
	UniCore::FUniverseDef Def = SceneName == TEXT("web") ? UniScenes::WebToGalaxy() : (SceneName == TEXT("flythrough") ? UniScenes::Flythrough() : UniScenes::GalaxyToStar());
	Model = MakeUnique<UniCore::FUniverseModel>(MoveTemp(Def));
	for (const std::string& Problem : Model->Validate())
	{
		UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: archetype validation: %s"), UTF8_TO_TCHAR(Problem.c_str()));
	}
	Generator = MakeUnique<UniCore::FRefGenerator>(*Model);
	Backend = MakeUnique<FUniGpuGenBackend>();
	Streamer = MakeUnique<UniCore::FUniStreamer>(*Generator, *Backend);

	TArray<UniCore::FSliceGPU> Slices;
	Slices.Append(Model->GpuSlices().data(), int32(Model->GpuSlices().size()));
	TArray<float> Lut;
	Lut.Append(Model->GpuLut().data(), int32(Model->GpuLut().size()));
	FUniRenderer::Get().SetTables_GameThread(MoveTemp(Slices), MoveTemp(Lut));

	const UniCore::FCoord Start = SceneName == TEXT("web") ? UniScenes::VoidCamera() : UniScenes::SolarCamera();
	Camera = FUniCoord::Make(Start);
	LocalAnchor = FUniCoord::Make(Start);   // Local mode: camera = anchor + the pawn's view location
	CameraFraction = FVector3f::ZeroVector;
	MotionMode = EUniMotionMode::Local;
	bEnabled = true;
	UniRenderHooks::EnsureRegistered();
	UniRenderHooks::SetActive(true);
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: enabled scene '%s' at %s"), *SceneName, *Camera.ToHexString());
}

void UUniSubsystem::DisableUniverse()
{
	if (!bEnabled) { return; }
	bEnabled = false;
	bFlying = false;
	UniRenderHooks::SetActive(false);
	FUniFrameData Off;
	Off.bEnabled = false;
	FUniRenderer::Get().SetFrame_GameThread(MoveTemp(Off));
	Streamer.Reset();
	Backend.Reset();       // flushes rendering commands: in-flight requests are released safely
	Generator.Reset();
	Model.Reset();
}

void UUniSubsystem::SetCamera(const FUniCoord& Coord)
{
	Camera = Coord;
	CameraFraction = FVector3f::ZeroVector;
}

void UUniSubsystem::Teleport(const FUniCoord& Coord)
{
	SetCamera(Coord);
	// Keep Local mode consistent: the camera lands at Coord with the pawn where it is.
	UniCore::FCoord A = Coord.ToCore();
	UniCore::FVec3f F{};
	const FVector ViewLoc = GetViewLocation();
	UniCore::AddDisplacement(A, F, UniCore::FVec3d{ -ViewLoc.X, -ViewLoc.Y, -ViewLoc.Z });
	LocalAnchor = FUniCoord::Make(A);
}

void UUniSubsystem::SetVelocity(const FVector& CmPerSecond)
{
	Velocity = CmPerSecond;
}

void UUniSubsystem::SetSensitivity(float InDeltaMag)
{
	DeltaMag = FMath::Clamp(InDeltaMag, -5.0f, 5.0f);
	if (Streamer) { Streamer->DeltaMag = DeltaMag; }
}

FVector UUniSubsystem::GetViewLocation(FRotator* OutRotation) const
{
	const UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		if (OutRotation) { *OutRotation = PC->PlayerCameraManager->GetCameraRotation(); }
		return PC->PlayerCameraManager->GetCameraLocation();
	}
	if (OutRotation) { *OutRotation = FRotator::ZeroRotator; }
	return FVector::ZeroVector;
}

void UUniSubsystem::SetFreeFly(bool bOn, double CmPerSecond)
{
	if (CmPerSecond > 0.0) { SetFlySpeed(CmPerSecond); }
	if (bOn == bFreeFly) { return; }
	bFreeFly = bOn;
	bFlying = false;
	bCruiseLatched = false;
	Velocity = FVector::ZeroVector;
	if (bOn)
	{
		MotionMode = EUniMotionMode::Cruise;
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: free flight on at %s/s (WASD/QE, mouse wheel x2 / x0.5, uu.Fly to stop)"), UTF8_TO_TCHAR(UniCore::FormatDistance(FlySpeed).c_str()));
	}
	else
	{
		// Back to Local without a jump: the anchor absorbs the pawn's current view location.
		MotionMode = EUniMotionMode::Local;
		UniCore::FCoord A = Camera.ToCore();
		UniCore::FVec3f F{};
		const FVector ViewLoc = GetViewLocation();
		UniCore::AddDisplacement(A, F, UniCore::FVec3d{ -ViewLoc.X, -ViewLoc.Y, -ViewLoc.Z });
		LocalAnchor = FUniCoord::Make(A);
		CameraFraction = FVector3f::ZeroVector;
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: free flight off (Local mode)"));
	}
}

void UUniSubsystem::SetCruise(const FVector& CmPerSecond)
{
	bFreeFly = false;
	bFlying = false;
	Velocity = CmPerSecond;
	bCruiseLatched = !CmPerSecond.IsNearlyZero();
}

void UUniSubsystem::DrawHud(double Speed) const
{
	if (!GEngine) { return; }
	const UniCore::FDelta FromOrigin = UniCore::Sub(Camera.ToCore(), UniCore::FCoord::Origin());
	const double R = UniCore::Length(UniCore::ToDouble(FromOrigin));
	const FString Line = FString::Printf(TEXT("UU %s  speed %s/s (%.3g cm/s)%s  |  %s from the demo origin  |  dm %+.1f  exposure %+.1f"),
		bFreeFly ? TEXT("FLY") : TEXT("CRUISE"),
		UTF8_TO_TCHAR(UniCore::FormatDistance(Speed).c_str()), Speed,
		bFreeFly ? TEXT("  [WASD/QE move, wheel x2 / x0.5]") : TEXT(""),
		UTF8_TO_TCHAR(UniCore::FormatDistance(R).c_str()), DeltaMag, ExposureBias);
	GEngine->AddOnScreenDebugMessage(int32(0x5EED0001), 0.0f, FColor(120, 220, 255), Line);
}

void UUniSubsystem::StartFlythrough(float Seconds)
{
	bFreeFly = false;
	bCruiseLatched = false;
	if (!bEnabled || SceneName != TEXT("flythrough")) { EnableUniverse(TEXT("flythrough")); }
	// The target: the brightest star within 100 pc of the solar camera, resolved once on the CPU
	// through the reference generator (the design's Resolve). The GPU generates the same star
	// (D-105: identical in all but rare Poisson boundary cases; the dive still ends 1 m above
	// the resolved coordinate either way).
	UniCore::FRefPipeline Resolve(*Generator);
	Resolve.Update(UniScenes::SolarCamera(), UniCore::FVec3f{});
	double Best = 99.0;
	Resolve.ForEachResident([&](const UniCore::FCellKey& K, const UniCore::FObjectGPU& O, float, UniCore::u64)
	{
		if ((O.A[3] & 0xFFFFu) != UniScenes::kStar) { return; }
		const UniCore::u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
		const UniCore::FCoord C = UniCore::ObjectCoord(K, Off);
		const double D = UniCore::Length(UniCore::ToDouble(UniCore::Sub(C, UniScenes::SolarCamera())));
		const float M = UniHlsl::Uni_ObjectAbsMag(UniCore::ToHlsl(O));
		if (D < 100.0 * UniCore::Units::Pc && M < Best)
		{
			Best = M;
			FlyTarget = FUniCoord::Make(C);
			FlyTargetRadius = std::exp2(double(UniHlsl::Uni_ObjectRadiusLog2(UniCore::ToHlsl(O))));
		}
	});
	FlyDuration = FMath::Max(1.0, double(Seconds));
	FlyElapsed = 0.0;
	bFlying = true;
	MotionMode = EUniMotionMode::Cruise;
	// Face the target.
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		PC->SetControlRotation((-FlyDirection).Rotation());
	}
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: flythrough to a star of M = %.2f over %.0f s"), Best, FlyDuration);
}

void UUniSubsystem::Tick(float DeltaTime)
{
	if (!bEnabled || !Streamer) { return; }
	++FrameCounter;
	FRotator ViewRot;
	const FVector ViewLoc = GetViewLocation(&ViewRot);

	UniCore::FCoord C = Camera.ToCore();
	UniCore::FVec3f Frac{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z };
	if (bFlying)
	{
		FlyElapsed += DeltaTime;
		const double T = FMath::Clamp(FlyElapsed / FlyDuration, 0.0, 1.0);
		const double Surface = 1.0e28 * FMath::Pow(100.0 / 1.0e28, T);
		const FVector D = FlyDirection * (FlyTargetRadius + Surface);
		C = FlyTarget.ToCore();
		Frac = UniCore::FVec3f{};
		UniCore::AddDisplacement(C, Frac, UniCore::FVec3d{ D.X, D.Y, D.Z });
		if (T >= 1.0) { bFlying = false; Velocity = FVector::ZeroVector; }   // ends in Cruise, parked 1 m above the star
	}
	else if (bFreeFly)
	{
		// Free flight: direction from WASD/QE in the view frame (no key held: no motion), speed
		// from the wheel. Integrated straight into the universal coordinate at any magnitude.
		APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		FVector Dir = FVector::ZeroVector;
		if (PC)
		{
			const FRotationMatrix R(ViewRot);
			if (PC->IsInputKeyDown(EKeys::W)) { Dir += R.GetUnitAxis(EAxis::X); }
			if (PC->IsInputKeyDown(EKeys::S)) { Dir -= R.GetUnitAxis(EAxis::X); }
			if (PC->IsInputKeyDown(EKeys::D)) { Dir += R.GetUnitAxis(EAxis::Y); }
			if (PC->IsInputKeyDown(EKeys::A)) { Dir -= R.GetUnitAxis(EAxis::Y); }
			if (PC->IsInputKeyDown(EKeys::E)) { Dir += FVector::UpVector; }
			if (PC->IsInputKeyDown(EKeys::Q)) { Dir -= FVector::UpVector; }
			if (PC->WasInputKeyJustPressed(EKeys::MouseScrollUp)) { SetFlySpeed(FlySpeed * 2.0); }
			if (PC->WasInputKeyJustPressed(EKeys::MouseScrollDown)) { SetFlySpeed(FlySpeed * 0.5); }
		}
		Velocity = Dir.GetSafeNormal() * FlySpeed;
		MotionMode = EUniMotionMode::Cruise;
		const FVector Step = Velocity * double(DeltaTime);
		UniCore::AddDisplacement(C, Frac, UniCore::FVec3d{ Step.X, Step.Y, Step.Z });
		DrawHud(Velocity.Size());
	}
	else
	{
		// D-008 motion modes with hysteresis.
		const double Speed = Velocity.Size();
		// Local mode computes camera = anchor + view location, so on entering Local the anchor is
		// the camera MINUS the view location (else the camera jumps by the pawn offset).
		auto AnchorFor = [&ViewLoc](const FUniCoord& Cam)
		{
			UniCore::FCoord A = Cam.ToCore();
			UniCore::FVec3f F{};
			UniCore::AddDisplacement(A, F, UniCore::FVec3d{ -ViewLoc.X, -ViewLoc.Y, -ViewLoc.Z });
			return FUniCoord::Make(A);
		};
		// A latched uu.Cruise stays in Cruise at any non-zero speed (the D-008 threshold would
		// otherwise ignore every speed below 1e8 cm/s).
		if (MotionMode == EUniMotionMode::Local && (Speed > kCruiseOn || (bCruiseLatched && Speed > 0.0))) { MotionMode = EUniMotionMode::Cruise; }
		else if (MotionMode == EUniMotionMode::Cruise && Speed < kCruiseOff && !(bCruiseLatched && Speed > 0.0)) { MotionMode = EUniMotionMode::Local; LocalAnchor = AnchorFor(Camera); }
		if (bCruiseLatched) { DrawHud(Speed); }
		if (MotionMode == EUniMotionMode::Cruise)
		{
			const FVector Step = Velocity * double(DeltaTime);
			UniCore::AddDisplacement(C, Frac, UniCore::FVec3d{ Step.X, Step.Y, Step.Z });
		}
		else
		{
			// Local: camera = local anchor + view location (the pawn moves normally).
			C = LocalAnchor.ToCore();
			Frac = UniCore::FVec3f{};
			UniCore::AddDisplacement(C, Frac, UniCore::FVec3d{ ViewLoc.X, ViewLoc.Y, ViewLoc.Z });
			if (ViewLoc.Size() > kAnchorRecentre && (FrameCounter % 300) == 0)
			{
				UE_LOG(LogUnifiedUniverse, Verbose, TEXT("UnifiedUniverse: pawn is %.3g cm from the local anchor; anchor re-centring (shifting promoted actors) is M3 work."), ViewLoc.Size());
			}
		}
	}
	Camera = FUniCoord::Make(C);
	CameraFraction = FVector3f(Frac.X, Frac.Y, Frac.Z);

	Streamer->DeltaMag = DeltaMag;
	Streamer->Update(C);
	PushFrame();
}

void UUniSubsystem::PushFrame()
{
	FUniFrameData F;
	const std::vector<UniCore::FCellGPU> Cells = Streamer->CellTable();
	F.Cells.Append(Cells.data(), int32(Cells.size()));
	F.CameraFraction = CameraFraction;
	F.DeltaMag = DeltaMag;
	F.ExposureBias = ExposureBias;
	F.bEnabled = true;
	F.FrameNumber = FrameCounter;
	FUniRenderer::Get().SetFrame_GameThread(MoveTemp(F));
}

void UUniSubsystem::LogStats() const
{
	if (!Streamer) { UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: disabled")); return; }
	const UniCore::FUniStreamer::FStats S = Streamer->Stats();
	const FUniRenderStats& R = FUniRenderer::Get().Stats;
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: scene '%s' mode %s dm %.1f | cells ready %llu pending %llu | pool %llu / %u | batches %llu (gpu done %u failed %u) | regenerations %llu | max gather %llu | alloc failures %llu | worst latency %llu frames | last frame: %u cells drawn, %u promotion candidates | camera %s"),
		*SceneName, MotionMode == EUniMotionMode::Cruise ? TEXT("Cruise") : TEXT("Local"), DeltaMag,
		uint64(S.CellsReady), uint64(S.CellsPending), uint64(S.PoolUsed), Backend ? Backend->PoolCapacity() : 0u,
		uint64(S.Batches), R.GenBatchesCompleted.load(), R.GenBatchesFailed.load(), uint64(S.Regenerations), uint64(S.MaxGather),
		uint64(S.AllocFailures), uint64(S.MaxLatencyFrames), R.CellsLastFrame.load(), R.PromoteCandidatesLastFrame.load(), *Camera.ToHexString());
}

// ---------------------------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------------------------

namespace
{
	UUniSubsystem* Sub(UWorld* World)
	{
		if (World) { if (UUniSubsystem* S = World->GetSubsystem<UUniSubsystem>()) { return S; } }
		return UUniSubsystem::FindForConsole();
	}

	FAutoConsoleCommandWithWorldAndArgs CmdEnable(TEXT("uu.Enable"), TEXT("uu.Enable [galaxy|web|flythrough]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->EnableUniverse(Args.Num() ? Args[0] : FString()); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdDisable(TEXT("uu.Disable"), TEXT("Stop the universe."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->DisableUniverse(); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdFly(TEXT("uu.Flythrough"), TEXT("uu.Flythrough [seconds]: 3 Gpc to 1 m above a bright star."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->StartFlythrough(Args.Num() ? FCString::Atof(*Args[0]) : 60.0f); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdCruise(TEXT("uu.Cruise"), TEXT("uu.Cruise <cm/s>: move along the view direction."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UUniSubsystem* S = Sub(World);
			if (!S || !World) { return; }
			FVector Dir = FVector::ForwardVector;
			if (APlayerController* PC = World->GetFirstPlayerController()) { if (PC->PlayerCameraManager) { Dir = PC->PlayerCameraManager->GetCameraRotation().Vector(); } }
			S->SetCruise(Dir * (Args.Num() ? FCString::Atod(*Args[0]) : 0.0));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdFreeFly(TEXT("uu.Fly"), TEXT("uu.Fly [cm/s]: toggle free flight (steer with the view, WASD/QE, mouse wheel x2 / x0.5). With a speed: on at that speed."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UUniSubsystem* S = Sub(World);
			if (!S) { return; }
			if (Args.Num()) { S->SetFreeFly(true, FCString::Atod(*Args[0])); }
			else { S->SetFreeFly(!S->IsFreeFlying()); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSpeed(TEXT("uu.Speed"), TEXT("uu.Speed <cm/s>: free-flight speed, any magnitude (e.g. 3e5 = 3 km/s, 3e10 = c, 3e21 = 1 kpc/s)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World); S && Args.Num()) { S->SetFlySpeed(FCString::Atod(*Args[0])); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTeleport(TEXT("uu.Teleport"), TEXT("uu.Teleport <solar|void>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World))
			{
				const bool bVoid = Args.Num() && Args[0].Equals(TEXT("void"), ESearchCase::IgnoreCase);
				S->Teleport(FUniCoord::Make(bVoid ? UniScenes::VoidCamera() : UniScenes::SolarCamera()));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSens(TEXT("uu.Sensitivity"), TEXT("uu.Sensitivity <dm>, -5..5"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->SetSensitivity(Args.Num() ? FCString::Atof(*Args[0]) : 0.0f); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdExposure(TEXT("uu.Exposure"), TEXT("uu.Exposure <mag>: exposure bias (negative = brighter)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->SetExposureBias(Args.Num() ? FCString::Atof(*Args[0]) : 0.0f); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdStats(TEXT("uu.Stats"), TEXT("Log streaming and render statistics."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->LogStats(); }
		}));
}
