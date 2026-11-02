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
#include "Async/Async.h"
#include "UniParallaxComponent.h"
#include "UniBodyActor.h"
#include "UniLocalFrame.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/Pawn.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
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
	VolumeBuilder = MakeUnique<UniCore::FVolumeBuilder>(*Model, *Generator);
	BackgroundLutLast.Reset();
	++BackgroundVersion;
	// D-133: the volume shows a parent's own magnitude and its children's summed light side by
	// side; warn when they disagree (the demo galaxy does, by 2.18 mag).
	for (const UniCore::FArchetype& A : Model->Definition().Archetypes)
	{
		if (A.Emitters.empty() || A.AbsMag.Pdf.empty()) { continue; }
		const double Mc = VolumeBuilder->LuminosityBudget(A.Id, -21.0);
		if (std::isfinite(Mc) && std::fabs(Mc + 21.0) > 0.1)
		{
			UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: luminosity budget (D-133): a %s of M = -21 has children adding up to M = %.2f; its volume glow will not match its sprite."), UTF8_TO_TCHAR(A.Name.c_str()), Mc);
		}
	}
	// D-140: normalising integrals (~0.2 s each, the first time a galaxy's stars are needed) run
	// on the engine's background thread pool instead of hitching the game thread.
	Streamer->RunJob = [](std::function<void()> InJob)
	{
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Work = MoveTemp(InJob)]() { Work(); });
	};

	// Star systems (D-142): which archetypes have them, and a fresh clock.
	Systems = MakeUnique<UniCore::FSystemManager>();
	SystemArchetypeMask = 0;
	for (const UniCore::FArchetype& A : Model->Definition().Archetypes)
	{
		if (A.bSystems && A.Id < 32u) { SystemArchetypeMask |= 1u << A.Id; }
	}
	UniverseTime = 0.0;
	CellKeysByFrame.Reset();
	ActivationsUnmatched = 0;
	{
		TArray<FUniActivation> Stale;
		FUniRenderer::Get().DrainActivations_GameThread(Stale);   // from a previous universe
	}

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
	if (bFreeFly) { SetPawnMovementSuppressed(false); bFreeFly = false; }
	bEnabled = false;
	bFlying = false;
	UniRenderHooks::SetActive(false);
	FUniFrameData Off;
	Off.bEnabled = false;
	FUniRenderer::Get().SetFrame_GameThread(MoveTemp(Off));
	VolumeBuilder.Reset();
	ClearPromotion();
	Systems.Reset();
	CellKeysByFrame.Reset();
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
	SetPawnMovementSuppressed(bOn);   // free flight drives the pawn itself (D-149)
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
	if (bFreeFly) { SetPawnMovementSuppressed(false); }
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
	const FString Line = FString::Printf(TEXT("UU %s  speed %s/s (%.3g cm/s)%s  |  %s from the demo origin  |  dm %+.1f  exposure %+.1f  |  t %.4g s (x%.3g)  systems %d, planets %d, promoted %d"),
		bFreeFly ? (MotionMode == EUniMotionMode::Local ? TEXT("FLY (local: real space, physics)") : TEXT("FLY (cruise)")) : TEXT("CRUISE"),
		UTF8_TO_TCHAR(UniCore::FormatDistance(Speed).c_str()), Speed,
		bFreeFly ? TEXT("  [WASD/QE move, wheel x2 / x0.5]") : TEXT(""),
		UTF8_TO_TCHAR(UniCore::FormatDistance(R).c_str()), DeltaMag, ExposureBias,
		UniverseTime, TimeScale, Systems ? int32(Systems->ActiveSystems().size()) : 0, Systems ? int32(Systems->PlanetCount()) : 0, int32(Promoter.Promoted().size()));
	GEngine->AddOnScreenDebugMessage(int32(0x5EED0001), 0.0f, FColor(120, 220, 255), Line);
}

void UUniSubsystem::StartFlythrough(float Seconds)
{
	if (bFreeFly) { SetPawnMovementSuppressed(false); }
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
	FVector ViewLoc = GetViewLocation(&ViewRot);   // updated below if the pawn is moved or re-centred this frame
	// Local mode computes camera = anchor + view location, so on entering Local the anchor is the
	// camera MINUS the view location (else the camera jumps by the pawn offset).
	auto AnchorFor = [&ViewLoc](const FUniCoord& Cam)
	{
		UniCore::FCoord A = Cam.ToCore();
		UniCore::FVec3f F{};
		UniCore::AddDisplacement(A, F, UniCore::FVec3d{ -ViewLoc.X, -ViewLoc.Y, -ViewLoc.Z });
		return FUniCoord::Make(A);
	};

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
		const FVector Step = Velocity * double(DeltaTime);
		// D-149: one set of controls at every speed. Below 5e7 cm/s (and until 1e8 cm/s, the D-008
		// hysteresis) the pawn really moves through the UE world (Local mode): nearby bodies are
		// still in the world and physics is stable. Above, the universe moves past a pinned pawn
		// (Cruise). The pawn's own movement is suppressed while free flight drives it.
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		const bool bLocal = Pawn && (MotionMode == EUniMotionMode::Local ? FlySpeed < kCruiseOn : FlySpeed < kCruiseOff);
		if (bLocal)
		{
			if (MotionMode != EUniMotionMode::Local) { LocalAnchor = AnchorFor(Camera); MotionMode = EUniMotionMode::Local; }
			Pawn->SetActorLocation(Pawn->GetActorLocation() + Step, false, nullptr, ETeleportType::None);
			ViewLoc += Step;
			RecentreLocalIfNeeded(ViewLoc);
			UniCore::FVec3f F{};
			C = UniCore::LocalToUniversal(LocalAnchor.ToCore(), UniCore::FVec3d{ ViewLoc.X, ViewLoc.Y, ViewLoc.Z }, F);
			Frac = F;
		}
		else
		{
			MotionMode = EUniMotionMode::Cruise;
			UniCore::AddDisplacement(C, Frac, UniCore::FVec3d{ Step.X, Step.Y, Step.Z });
		}
		DrawHud(Velocity.Size());
	}
	else
	{
		// D-008 motion modes with hysteresis.
		const double Speed = Velocity.Size();
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
			RecentreLocalIfNeeded(ViewLoc);
			UniCore::FVec3f F{};
			C = UniCore::LocalToUniversal(LocalAnchor.ToCore(), UniCore::FVec3d{ ViewLoc.X, ViewLoc.Y, ViewLoc.Z }, F);
			Frac = F;
		}
	}
	Camera = FUniCoord::Make(C);
	CameraFraction = FVector3f(Frac.X, Frac.Y, Frac.Z);

	Streamer->DeltaMag = DeltaMag;
	UniverseTime += double(DeltaTime) * TimeScale;   // B-002: orbits are on rails at this clock
	Streamer->Update(C);
	// M3: bodies close enough become actors, and every parallax-placed actor follows the camera
	// that just moved (same frame, same camera: no lag against the backdrop at any speed).
	UpdatePromotion();
	UpdateParallax(ViewLoc);
	UpdateLightRig();
	PushFrame();
}

void UUniSubsystem::PushFrame()
{
	FUniFrameData F;
	std::vector<UniCore::FCellKey> Keys;
	const std::vector<UniCore::FCellGPU> Cells = Streamer->CellTable(&Keys);
	F.Cells.Append(Cells.data(), int32(Cells.size()));

	// Star systems (D-142, D-143). Activations name a cell by its index in the table of the
	// frame that produced them (2-4 frames ago), so the last few tables' keys are kept.
	if (Systems)
	{
		const UniCore::FCoord Cam = Camera.ToCore();
		TArray<FUniActivation> Acts;
		FUniRenderer::Get().DrainActivations_GameThread(Acts);
		for (const FUniActivation& A : Acts)
		{
			const std::vector<UniCore::FCellKey>* Table = CellKeysByFrame.Find(A.FrameNumber);
			if (!Table || A.CellIndex >= Table->size()) { ++ActivationsUnmatched; continue; }
			Systems->Report((*Table)[A.CellIndex], A.Record, Cam, FrameCounter);
		}
		Systems->Update(Cam);
		CellKeysByFrame.Add(FrameCounter, MoveTemp(Keys));
		for (auto It = CellKeysByFrame.CreateIterator(); It; ++It) { if (It.Key() + 16 < FrameCounter) { It.RemoveCurrent(); } }
		std::vector<UniCore::FCellGPU> SysCells;
		std::vector<UniCore::FObjectGPU> SysRecords;
		const std::set<UniCore::u32> Culled = Promoter.CulledSpriteIds();   // promoted planets: their actor draws them (D-147)
		Systems->Build(Cam, UniverseTime, FUniRenderer::DynamicPoolBase(), FUniRenderer::DynamicPoolSlots(), SysCells, SysRecords, &Culled);
		F.Cells.Append(SysCells.data(), int32(SysCells.size()));
		F.DynamicRecords.Append(SysRecords.data(), int32(SysRecords.size()));
		F.SystemArchetypeMask = SystemArchetypeMask;
	}
	F.CameraFraction = CameraFraction;
	F.DeltaMag = DeltaMag;
	F.ExposureBias = ExposureBias;
	F.bEnabled = true;
	F.FrameNumber = FrameCounter;
	if (bVolume && VolumeBuilder)
	{
		// The carriers' handoff weights use the same view quantities as project-and-cull.
		UniHlsl::FUniView V{};
		V.LimitingMag = UniHlsl::Uni_LimitingMag(DeltaMag);
		V.SurfaceLimit = UniHlsl::UNI_MU_REF + FMath::Clamp(DeltaMag, -5.0f, 5.0f);
		V.ExposureMag = V.LimitingMag - 1.0f + ExposureBias;
		V.ThetaPixel = FUniRenderer::Get().Stats.ThetaPixelLastFrame.load();
		const UniCore::FCoord Cam = Camera.ToCore();
		const UniCore::FVolumeSettings Settings;
		const UniCore::FVolumeScene Sc = VolumeBuilder->Build(*Streamer, Cam, UniCore::FVec3f{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z }, V, Settings);
		for (const UniCore::FCarrierGPU& C : Sc.Carriers)
		{
			((C.Flags & UniHlsl::UNI_CARRIER_PERIODIC) ? F.BackgroundCarriers : F.VolumeCarriers).Add(C);
		}
		F.VolumeParams.Append(Sc.Params.data(), int32(Sc.Params.size()));
		F.VolumeLut.Reserve(int32(Sc.Lut.size()));
		for (const UniHlsl::float4& L : Sc.Lut) { F.VolumeLut.Add(FVector4f(L.x, L.y, L.z, L.w)); }
		F.VolumeView = Sc.View;
		F.BackgroundStartCm = float(Settings.BackgroundStartCm);
		F.bVolume = true;

		// Re-march the background map (D-136) when the camera has moved a quarter texel's
		// parallax at the background's start, or its LUT has changed by more than 1%.
		static IConsoleVariable* CVarBgSize = IConsoleManager::Get().FindConsoleVariable(TEXT("uu.BackgroundSize"));
		const double TexelRad = 2.0 / double(FMath::Max(CVarBgSize ? CVarBgSize->GetInt() : 256, 16));
		const UniCore::FVec3d Moved = UniCore::ToDouble(UniCore::Sub(Cam, BackgroundAnchor));
		bool bDirty = BackgroundLutLast.Num() != F.VolumeLut.Num() || FMath::Sqrt(Moved.X * Moved.X + Moved.Y * Moved.Y + Moved.Z * Moved.Z) > 0.25 * TexelRad * Settings.BackgroundStartCm;
		// Only the background's own LUT blocks (emission and tail) matter here.
		for (const UniCore::FCarrierGPU& C : F.BackgroundCarriers)
		{
			for (uint32 Blk = C.LutIndex; Blk <= C.LutIndex + 1u && !bDirty; ++Blk)
			{
				for (int32 I = int32(Blk * UniHlsl::UNI_VOL_LUT_SIZE); !bDirty && I < int32((Blk + 1u) * UniHlsl::UNI_VOL_LUT_SIZE) && I < F.VolumeLut.Num(); ++I)
				{
					const FVector4f A = F.VolumeLut[I], B = BackgroundLutLast[I];
					bDirty = FMath::Abs(A.Y - B.Y) > 0.01f * FMath::Max(FMath::Abs(B.Y), 1.0e-30f);
				}
			}
		}
		if (bDirty) { ++BackgroundVersion; BackgroundAnchor = Cam; BackgroundLutLast = F.VolumeLut; }
		F.BackgroundVersion = BackgroundVersion;
	}
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
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: volume %s | %u local carriers last frame | background faces marched %u | LUT rebuilds %llu"),
		bVolume ? TEXT("on") : TEXT("off"), R.VolumeCarriersLastFrame.load(), R.BackgroundFacesMarched.load(), uint64(VolumeBuilder ? VolumeBuilder->LutRebuilds : 0));
	// Per level, coarse first (D-137/D-139): how full each window is, how wide, and whether
	// velocity LOD has suspended it. "parents" is how many levels it waits for.
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: generation latency %.1f frames (submit to ready)"), Streamer->LatencyFrames());
	const std::vector<UniCore::FUniStreamer::FLevelState> Levels = Streamer->LevelStates();
	for (auto It = Levels.rbegin(); It != Levels.rend(); ++It)
	{
		const UniCore::FUniStreamer::FLevelState& L = *It;
		UE_LOG(LogUnifiedUniverse, Log, TEXT("  L%d (cells %s): ready %llu / %llu, half-width %d, fade %.2f%s, parents %llu"),
			L.Level, UTF8_TO_TCHAR(UniCore::FormatDistance(UniCore::CellWidthCm(L.Level)).c_str()), uint64(L.ReadyCells), uint64(L.Cells),
			L.HalfWidth, L.Fade, L.bSuspended ? TEXT(", SUSPENDED (velocity LOD)") : TEXT(""), uint64(L.Parents));
	}
}

// ---------------------------------------------------------------------------------------------
// Promotion and real space (M3, D-146, D-147)
// ---------------------------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarUniLightRig(TEXT("uu.LightRig"), 1, TEXT("M3 placeholder: a movable directional light from the nearest active star, so promoted planets are lit (0 off)."));
static TAutoConsoleVariable<float> CVarUniLightRigLux(TEXT("uu.LightRigLux"), 10.0f, TEXT("Intensity of the light rig, lux."));

void UUniSubsystem::RegisterParallax(UUniParallaxComponent* C)
{
	if (C) { ParallaxComponents.AddUnique(C); }
}

void UUniSubsystem::UnregisterParallax(UUniParallaxComponent* C)
{
	ParallaxComponents.Remove(C);
}

void UUniSubsystem::ClearPromotion()
{
	// During world teardown the actors go with the world; only destroy them while it lives.
	const UWorld* World = GetWorld();
	const bool bLive = World && !World->bIsTearingDown;
	for (auto& KV : Bodies) { if (AUniBodyActor* B = KV.Value.Get(); B && bLive) { B->Destroy(); } }
	Bodies.Reset();
	Promoter.Clear();
	if (ADirectionalLight* L = LightRig.Get(); L && bLive) { L->Destroy(); }
	LightRig.Reset();
}

void UUniSubsystem::SetPawnMovementSuppressed(bool bSuppress)
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UFloatingPawnMovement* M = Pawn ? Pawn->FindComponentByClass<UFloatingPawnMovement>() : nullptr;
	if (!M) { return; }
	if (bSuppress && SavedPawnMaxSpeed < 0.0f) { SavedPawnMaxSpeed = M->MaxSpeed; M->MaxSpeed = 0.0f; M->StopMovementImmediately(); }
	else if (!bSuppress && SavedPawnMaxSpeed >= 0.0f) { M->MaxSpeed = SavedPawnMaxSpeed; SavedPawnMaxSpeed = -1.0f; }
}

void UUniSubsystem::RecentreLocalIfNeeded(FVector& InOutViewLoc)
{
	if (InOutViewLoc.Size() <= kAnchorRecentre) { return; }
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn) { return; }
	// Whole centimetres: anchor + world position is unchanged to the bit (UniLocalFrame.h).
	const FVector PawnLoc = Pawn->GetActorLocation();
	const UniCore::FVec3d S = UniCore::RecentreShift(UniCore::FVec3d{ PawnLoc.X, PawnLoc.Y, PawnLoc.Z });
	UniCore::FCoord A = LocalAnchor.ToCore();
	UniCore::ShiftAnchor(A, S);
	LocalAnchor = FUniCoord::Make(A);
	const FVector Shift(-S.X, -S.Y, -S.Z);
	Pawn->SetActorLocation(PawnLoc + Shift, false, nullptr, ETeleportType::TeleportPhysics);
	InOutViewLoc += Shift;
	for (const TWeakObjectPtr<UUniParallaxComponent>& W : ParallaxComponents)
	{
		UUniParallaxComponent* C = W.Get();
		if (C && C->Mode == EUniParallaxMode::Simulated && C->bRealSpace && C->GetOwner()) { C->GetOwner()->AddActorWorldOffset(Shift, false, nullptr, ETeleportType::TeleportPhysics); }
	}
	OnLocalRecentre.Broadcast(Shift);
	// The camera cache is what this frame renders from: refresh it, or the view lags the shift by a frame.
	if (PC->PlayerCameraManager) { PC->PlayerCameraManager->UpdateCamera(0.0f); }
	++Recentres;
	UE_LOG(LogUnifiedUniverse, Verbose, TEXT("UnifiedUniverse: Local frame re-centred by %s (re-centre %llu)"), *Shift.ToString(), Recentres);
}

void UUniSubsystem::UpdatePromotion()
{
	UWorld* World = GetWorld();
	if (!Systems || !World) { return; }
	Promoter.Update(*Systems, Camera.ToCore(), UniverseTime, FrameCounter);
	const std::map<UniCore::u32, UniCore::FPromotedBody>& P = Promoter.Promoted();
	// Demoted: destroy (pooling comes with real body actors).
	for (auto It = Bodies.CreateIterator(); It; ++It)
	{
		if (P.count(It.Key()) == 0 || !It.Value().IsValid())
		{
			if (AUniBodyActor* B = It.Value().Get()) { B->Destroy(); }
			It.RemoveCurrent();
		}
	}
	// Promoted: spawn, then keep the coordinate current (planets move on their orbits).
	for (const auto& KV : P)
	{
		const UniCore::FPromotedBody& Body = KV.second;
		TWeakObjectPtr<AUniBodyActor>* Found = Bodies.Find(KV.first);
		AUniBodyActor* Actor = Found ? Found->Get() : nullptr;
		if (!Actor)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Params.ObjectFlags |= RF_Transient;
			Actor = World->SpawnActor<AUniBodyActor>(AUniBodyActor::StaticClass(), FTransform::Identity, Params);
			if (!Actor) { continue; }
			Actor->InitBody(Body.bStar, Body.RadiusCm, FLinearColor(Body.Rgb[0], Body.Rgb[1], Body.Rgb[2]), KV.first);
			Bodies.Add(KV.first, Actor);
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: promoted %s %08x (radius %s) to an actor"),
				Body.bStar ? TEXT("star") : TEXT("planet"), KV.first, UTF8_TO_TCHAR(UniCore::FormatDistance(Body.RadiusCm).c_str()));
		}
		Actor->Parallax->Coord = FUniCoord::Make(Body.Coord);
	}
}

void UUniSubsystem::UpdateParallax(const FVector& ViewLoc)
{
	for (int32 I = ParallaxComponents.Num() - 1; I >= 0; --I)
	{
		UUniParallaxComponent* C = ParallaxComponents[I].Get();
		if (!C) { ParallaxComponents.RemoveAtSwap(I); continue; }
		if (MotionMode == EUniMotionMode::Local) { C->ApplyPlacementLocal(ViewLoc, Camera, CameraFraction, LocalAnchor); }
		else { C->ApplyPlacement(ViewLoc, Camera, CameraFraction); }
	}
}

void UUniSubsystem::UpdateLightRig()
{
	UWorld* World = GetWorld();
	const bool bWant = CVarUniLightRig.GetValueOnGameThread() != 0 && Systems && !Systems->ActiveSystems().empty() && World;
	if (!bWant)
	{
		if (ADirectionalLight* L = LightRig.Get()) { L->Destroy(); }
		LightRig.Reset();
		return;
	}
	// The nearest active star lights the scene; its light travels from the star towards the camera.
	const UniCore::FCoord Cam = Camera.ToCore();
	const UniCore::FActiveSystem* Near = nullptr;
	double Best = 0.0;
	for (const auto& KV : Systems->ActiveSystems())
	{
		const double D = UniCore::Length(UniCore::ToDouble(UniCore::Sub(KV.second.Star, Cam)));
		if (!Near || D < Best) { Near = &KV.second; Best = D; }
	}
	ADirectionalLight* L = LightRig.Get();
	if (!L)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		L = World->SpawnActorDeferred<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!L) { return; }
		L->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		L->FinishSpawning(FTransform::Identity);
		LightRig = L;
	}
	const UniCore::FVec3d D = UniCore::ToDouble(UniCore::Sub(Cam, Near->Star));
	const FVector Dir = FVector(D.X, D.Y, D.Z).GetSafeNormal();
	L->SetActorRotation(Dir.Rotation());
	if (ULightComponent* C = L->GetLightComponent())
	{
		C->SetLightColor(FLinearColor(Near->Sys.StarRgb[0], Near->Sys.StarRgb[1], Near->Sys.StarRgb[2]));
		C->SetIntensity(CVarUniLightRigLux.GetValueOnGameThread());
	}
}

void UUniSubsystem::LogSystems() const
{
	if (!Systems) { UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: disabled")); return; }
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: clock %.6g s (x%.3g) | %d active systems, %d planets | activations %llu, deactivations %llu, rejected %llu, unmatched readbacks %llu | last readback %u"),
		UniverseTime, TimeScale, int32(Systems->ActiveSystems().size()), int32(Systems->PlanetCount()), uint64(Systems->Activations), uint64(Systems->Deactivations),
		uint64(Systems->Rejected), ActivationsUnmatched, FUniRenderer::Get().Stats.ActivationsLastReadback.load());
	const UniCore::FCoord Cam = Camera.ToCore();
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  promoted bodies: %d (promotions %llu, demotions %llu)"), int32(Promoter.Promoted().size()), uint64(Promoter.Promotions), uint64(Promoter.Demotions));
	for (const auto& KV : Bodies)
	{
		if (const AUniBodyActor* B = KV.Value.Get())
		{
			UE_LOG(LogUnifiedUniverse, Log, TEXT("    %s %08x: %s away, %s, placement scale %.4g"), B->bStar ? TEXT("star  ") : TEXT("planet"), KV.Key,
				UTF8_TO_TCHAR(UniCore::FormatDistance(B->Parallax->TrueDistanceCm).c_str()), B->Parallax->bRealSpace ? TEXT("REAL SPACE (unit scale)") : TEXT("compressed"), B->Parallax->PlacementScale);
		}
	}
	for (const auto& KV : Systems->ActiveSystems())
	{
		const UniCore::FActiveSystem& A = KV.second;
		const double D = UniCore::Length(UniCore::ToDouble(UniCore::Sub(A.Star, Cam)));
		UE_LOG(LogUnifiedUniverse, Log, TEXT("  star %08x: M = %.2f, %.2f L_sun, %.2f M_sun, %s away (activation radius %s), %d planets"),
			KV.first, A.Sys.StarAbsMag, A.Sys.LumSun, A.Sys.MassSun, UTF8_TO_TCHAR(UniCore::FormatDistance(D).c_str()),
			UTF8_TO_TCHAR(UniCore::FormatDistance(A.Sys.ActivationCm).c_str()), int32(A.Sys.Planets.size()));
		for (const UniCore::FPlanet& P : A.Sys.Planets)
		{
			const UniCore::FVec3d Off = UniCore::PlanetOffset(P, UniverseTime);
			UniCore::FCoord Pc = A.Star;
			UniCore::FVec3f Pf{};
			UniCore::AddDisplacement(Pc, Pf, Off);
			const UniCore::FPlanetPhotometry Ph = UniCore::PlanetPhotometry(P, double(A.Sys.StarAbsMag), Off, UniCore::ToDouble(UniCore::Sub(Cam, Pc)));
			UE_LOG(LogUnifiedUniverse, Log, TEXT("    %s: a %.3f AU, e %.2f, R %.2f R_earth, period %.3g yr | %s away, phase %.0f deg, m = %.2f"),
				P.Type == UniCore::EPlanetType::Rocky ? TEXT("rocky    ") : (P.Type == UniCore::EPlanetType::GasGiant ? TEXT("gas giant") : TEXT("ice giant")),
				P.SemiMajorCm / UniCore::Units::Au, P.Ecc, P.RadiusCm / UniCore::SysConst::EarthRadius, P.PeriodS / 3.15576e7,
				UTF8_TO_TCHAR(UniCore::FormatDistance(Ph.Delta).c_str()), FMath::RadiansToDegrees(Ph.Alpha), Ph.AppMag);
		}
	}
}

bool UUniSubsystem::GoToSystem(bool bToPlanet)
{
	if (!bEnabled || !Generator) { return false; }
	// Resolve on the CPU through the reference generator (the same content the GPU makes, D-105),
	// and pick the nearest star with a gas giant.
	const UniCore::FCoord Here = Camera.ToCore();
	UniCore::FRefPipeline Resolve(*Generator);
	Resolve.bVelocityLod = false;
	Resolve.Update(Here, UniCore::FVec3f{});
	double Best = 100.0 * UniCore::Units::Pc;
	UniCore::FCoord Star;
	UniCore::FSystem Sys;
	bool bFound = false;
	Resolve.ForEachResident([&](const UniCore::FCellKey& K, const UniCore::FObjectGPU& O, float, UniCore::u64)
	{
		const UniCore::u32 Arch = O.A[3] & 0xFFFFu;
		if (Arch >= 32u || ((SystemArchetypeMask >> Arch) & 1u) == 0u || ((O.A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN)) { return; }
		const UniCore::u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
		const UniCore::FCoord C = UniCore::ObjectCoord(K, Off);
		const double D = UniCore::Length(UniCore::ToDouble(UniCore::Sub(C, Here)));
		if (D >= Best) { return; }
		UniCore::FSystem S = UniCore::BuildSystem(O);
		bool bGiant = false;
		for (const UniCore::FPlanet& P : S.Planets) { bGiant = bGiant || P.Type == UniCore::EPlanetType::GasGiant; }
		if (!bGiant) { return; }
		Best = D; Star = C; Sys = MoveTemp(S); bFound = true;
	});
	if (!bFound)
	{
		UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: no star with a gas giant within 100 pc."));
		return false;
	}
	if (bToPlanet)
	{
		// 30 radii out on the day side of the system's first gas giant, slightly above its orbit
		// plane, facing it: it promotes on approach (at about 50 radii it is already a candidate,
		// so it promotes within a frame or two of the system activating).
		const UniCore::FPlanet* Giant = nullptr;
		for (const UniCore::FPlanet& P : Sys.Planets) { if (P.Type == UniCore::EPlanetType::GasGiant) { Giant = &P; break; } }
		const UniCore::FVec3d Off = UniCore::PlanetOffset(*Giant, UniverseTime);
		const double Lo = UniCore::Length(Off);
		const FVector Out = FVector(-Off.X / Lo, -Off.Y / Lo, 0.25).GetSafeNormal();
		UniCore::FCoord To = Star;
		UniCore::FVec3f Frac{};
		UniCore::AddDisplacement(To, Frac, UniCore::FVec3d{ Off.X + Out.X * 30.0 * Giant->RadiusCm, Off.Y + Out.Y * 30.0 * Giant->RadiusCm, Off.Z + Out.Z * 30.0 * Giant->RadiusCm });
		SetFreeFly(true, 2.0 * Giant->RadiusCm);
		SetFlySpeed(2.0 * Giant->RadiusCm);   // 2 radii per second
		SetCamera(FUniCoord::Make(To));
		if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr) { PC->SetControlRotation((-Out).Rotation()); }
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: jumped to 30 radii from a gas giant (R = %.1f R_earth, a = %.2f AU) of a star %s away. Fly in (W) to promote it to an actor; uu.Systems lists it."),
			Giant->RadiusCm / UniCore::SysConst::EarthRadius, Giant->SemiMajorCm / UniCore::Units::Au, UTF8_TO_TCHAR(UniCore::FormatDistance(Best).c_str()));
		return true;
	}
	// Above the system's plane, outside its widest orbit, looking at the star; moving at a speed
	// that crosses the system in about 20 s.
	const double Sp = FMath::Max(Sys.SpanCm, UniCore::Units::Au);
	UniCore::FCoord To = Star;
	UniCore::FVec3f Frac{};
	UniCore::AddDisplacement(To, Frac, UniCore::FVec3d{ 0.25 * Sp, -0.9 * Sp, 0.7 * Sp });
	SetFreeFly(true, 0.1 * Sp);
	SetFlySpeed(0.1 * Sp);
	SetCamera(FUniCoord::Make(To));
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		PC->SetControlRotation(FVector(-0.25, 0.9, -0.7).GetSafeNormal().Rotation());
	}
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: jumped into the system of a star of M = %.2f, %s away (%d planets, widest orbit %s). It activates once its cell is drawn; uu.Systems lists it."),
		Sys.StarAbsMag, UTF8_TO_TCHAR(UniCore::FormatDistance(Best).c_str()), int32(Sys.Planets.size()), UTF8_TO_TCHAR(UniCore::FormatDistance(Sys.SpanCm).c_str()));
	return true;
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

	FAutoConsoleCommandWithWorldAndArgs CmdVolume(TEXT("uu.Volume"), TEXT("uu.Volume <0|1>: the volume pass (unresolved glow, dust, background)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->SetVolumeEnabled(Args.Num() ? FCString::Atoi(*Args[0]) != 0 : !S->IsVolumeEnabled()); }
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

	FAutoConsoleCommandWithWorldAndArgs CmdTimeScale(TEXT("uu.TimeScale"), TEXT("uu.TimeScale <x>: universe clock rate (1 = real time, 3.15e7 = a year per second, negative runs backwards)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World); S && Args.Num()) { S->SetTimeScale(FCString::Atod(*Args[0])); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTime(TEXT("uu.Time"), TEXT("uu.Time <seconds>: set the universe clock."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World); S && Args.Num()) { S->SetUniverseTime(FCString::Atod(*Args[0])); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSystems(TEXT("uu.Systems"), TEXT("Log active star systems and their planets."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->LogSystems(); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdGotoSystem(TEXT("uu.GotoSystem"), TEXT("uu.GotoSystem [planet]: jump into the nearest star system with a gas giant (free flight); 'planet' jumps to 30 radii from the giant."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->GoToSystem(Args.Num() > 0 && Args[0].Equals(TEXT("planet"), ESearchCase::IgnoreCase)); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdStats(TEXT("uu.Stats"), TEXT("Log streaming and render statistics."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->LogStats(); }
		}));
}
