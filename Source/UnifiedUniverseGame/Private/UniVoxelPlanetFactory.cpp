// UniVoxelPlanetFactory.cpp
// D-293: planets' detailed actors from VoxelPlanets' APlanetActor (see the header).

#include "UniVoxelPlanetFactory.h"

#include "UnifiedUniverseGameModule.h"
#include "UniPlanetProps.h"

#include "PlanetActor.h"
#include "PlanetGravityZone.h"
#include "VoxelDensity.h"
#include "VoxelLandSettings.h"
#include "AtmosphereParams.h"
#include "AtmosphereArchetype.h"
#include "SeededAtmosphereActor.h"
#include "FlowSimSubsystem.h"   // D-301: the weather's clock
#include "FlowSimTypes.h"

#include "Components/ActorComponent.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "UObject/SoftObjectPath.h"

static TAutoConsoleVariable<FString> CVarUniPlanetClass(TEXT("uu.Planet.Class"), FString(),
	TEXT("D-293: the class of rocky planets' detailed actors (a path such as /Game/Planets/BP_Planet.BP_Planet_C; an APlanetActor subclass). Empty: APlanetActor itself (no materials or sets of its own: set this to the project's planet Blueprint). Applies to planets acquired after the change."));
static TAutoConsoleVariable<FString> CVarUniPlanetGasGiantClass(TEXT("uu.Planet.GasGiantClass"), FString(),
	TEXT("D-293: the class of gas giants' detailed actors (an APlanetActor subclass set up as a gas giant). Empty: none (gas giants keep their placeholder spheres). Ice giants always keep theirs (D-290)."));
static TAutoConsoleVariable<int32> CVarUniPlanetSurface(TEXT("uu.Planet.Surface"), 1,
	TEXT("D-293: rocky planets' surface generator: 1 the terrestrial generator (plates, landforms, the bake; D-290), 0 the noise surface (no bake: faster, plainer)."));
static TAutoConsoleVariable<FString> CVarUniPlanetAtmoArchetype(TEXT("uu.Planet.AtmoArchetype"), TEXT("/CloudAtmosphere/NoiseRecipes/TerrestrialAtmosphereArchetype.TerrestrialAtmosphereArchetype"),
	TEXT("D-294: the atmosphere archetype a terrestrial planet's atmosphere draws from when its class sets none (the planet's AtmosphereSet empty: else the atmosphere's class defaults, an untuned look). Empty: none."));
static TAutoConsoleVariable<FString> CVarUniPlanetAtmoLook(TEXT("uu.Planet.AtmoLook"), TEXT("/CloudAtmosphere/NoiseRecipes/TerrestrialAtmosphereLookProfile.TerrestrialAtmosphereLookProfile"),
	TEXT("D-294: the look profile a terrestrial planet's atmosphere draws from when its class sets none. Empty: none."));
static TAutoConsoleVariable<FString> CVarUniPlanetGasGiantAtmoArchetype(TEXT("uu.Planet.GasGiantAtmoArchetype"), TEXT("/CloudAtmosphere/NoiseRecipes/GasGiantAtmosphereArchetype.GasGiantAtmosphereArchetype"),
	TEXT("D-294: the atmosphere archetype a gas giant's atmosphere draws from when its class sets none. Empty: none."));
static TAutoConsoleVariable<FString> CVarUniPlanetGasGiantAtmoLook(TEXT("uu.Planet.GasGiantAtmoLook"), TEXT("/CloudAtmosphere/NoiseRecipes/GasGiantAtmosphereLookProfile.GasGiantAtmosphereLookProfile"),
	TEXT("D-294: the look profile a gas giant's atmosphere draws from when its class sets none. Empty: none."));
static TAutoConsoleVariable<float> CVarUniPlanetAtmoThicknessKm(TEXT("uu.Planet.AtmoThicknessKm"), 60.0f,
	TEXT("D-295: a rocky planet's atmosphere's depth at Earth's gravity and temperature, km (Earth's sky is about 60 km of it: CloudAtmosphere's scale heights are fractions of it); scaled by the planet's temperature over its gravity (a scale height's kT / mg), set as the atmosphere's HeightScale (its depth over the radius, at least 0.005) and locked against the seed. 0: the seed's own (CloudAtmosphere's 0.03-0.07 of the radius)."));
static TAutoConsoleVariable<float> CVarUniPlanetCloudLayerKm(TEXT("uu.Planet.CloudLayerKm"), 12.0f,
	TEXT("D-295: a rocky planet's weather layer at Earth's gravity and temperature, km (Earth's troposphere ~12 km), scaled as uu.Planet.AtmoThicknessKm; set as the atmosphere's CloudThickness (its share of the atmosphere) and locked. 0: the seed's own."));
static TAutoConsoleVariable<float> CVarUniPlanetCloudStepFootprint(TEXT("uu.Planet.CloudStepFootprint"), 0.5f,
	TEXT("D-295: the planets' atmospheres' Sampling.StepFootprint (CloudAtmosphere stage RS1: the cloud noise read over this many step lengths at least; 0 the pixel's width alone). -1: the atmosphere's own."));
static TAutoConsoleVariable<int32> CVarUniPlanetCloudStepTaps(TEXT("uu.Planet.CloudStepTaps"), 4,
	TEXT("D-295: the planets' atmospheres' Sampling.StepTaps (CloudAtmosphere stage RS1: noise reads along each cloud step, averaged; 1 one read). 0: the atmosphere's own."));
static TAutoConsoleVariable<int32> CVarUniPlanetLandView(TEXT("uu.Planet.LandView"), 19,
	TEXT("D-294: the terrain's debug colours while planets have no materials of their own: voxel.Debug.LandView set to this when a planet is made (D-296: 19, the rivers' channels and valleys over the elevation's colours; 14 the landforms; 13 needs a bake with drainage, which a disk-cached bake lacks). -1: leave voxel.Debug.LandView alone. A value typed or set in an ini wins."));

static TAutoConsoleVariable<int32> CVarUniPlanetTickLate(TEXT("uu.Planet.TickLate"), 1,
	TEXT("D-298: a planet's actors (the planet, its terrain, ocean, gravity zone and atmosphere, and their components) tick in TG_PostUpdateWork, after the universe has placed the planet this frame (the universe ticks after the actor tick groups up to TG_PostPhysics). Else an atmosphere or ocean that reads the planet's place in its tick draws it a frame behind the terrain: in Cruise, where the planet moves past the pinned pawn, the atmosphere's shell is off the terrain by a frame's travel and half the planet shows the terrain through it. 0: TG_PrePhysics (UE's default), for comparison."));
static TAutoConsoleVariable<int32> CVarUniPlanetWeatherClock(TEXT("uu.Planet.WeatherClock"), 1,
	TEXT("D-301: the clouds' clock (CloudAtmosphere's sim). 1: the planet's own: the sim's day (one turn of the planet its Coriolis assumes) is the planet's solar day (uu.Planet.DayHours) on the universe clock, so the weather runs with the day and uu.TimeScale (a day a minute: a day's weather a minute; at 1, real time: slow); 0: the atmosphere's own speed (its SimSpeed) times uu.TimeScale; -1: the atmosphere's own, untouched. The planets turn themselves (D-299), so their atmospheres' own spin of the cloud field (SpinRatio) is set to 0 while they do."));
static TAutoConsoleVariable<float> CVarUniPlanetCloudPrefilter(TEXT("uu.Planet.CloudPrefilter"), 0.2f,
	TEXT("D-302: the planets' atmospheres' Sampling.NoisePrefilter (CloudAtmosphere stage RS3: the structure noise's spread its mips average away, so far cloud is the mean of the cloud, not round blobs cut from the mean noise; 0 off). Set when an atmosphere is made; r.CloudAtmosphere.NoisePrefilterScale scales it live. -1: the atmosphere's own."));

namespace
{
	// The density check's probe: twice the planet's radius from its centre, far above any surface
	// (a noise surface's sea and peaks are above the radius: up to NoiseAmplitudeRatio of it). A
	// built density puts it far above the water there (PointDepth well below 0); an unbuilt one
	// answers all defaults (PointDepth 0).
	constexpr double kProbeRadii = 2.0;
	constexpr float kProbeAboveM = -1000.0f;

	template <typename T>
	T* LoadPath(const FString& InPath)
	{
		const FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty()) { return nullptr; }
		T* A = LoadObject<T>(nullptr, *Path);
		if (!A) { UE_LOG(LogUnifiedUniverseGame, Warning, TEXT("UnifiedUniverse: '%s' is not a %s"), *Path, *T::StaticClass()->GetName()); }
		return A;
	}

	/** An empty model set from an archetype and a look (the console variables' paths). */
	void FillModelSet(FAtmosphereModelSet& S, const FString& ArchetypePath, const FString& LookPath)
	{
		if (S.Archetypes.Num() > 0 || S.Look) { return; }   // the class's own
		if (UAtmosphereArchetype* A = LoadPath<UAtmosphereArchetype>(ArchetypePath))
		{
			FAtmosphereArchetypeEntry E;
			E.Archetype = A;
			S.Archetypes.Add(E);
		}
		S.Look = LoadPath<UAtmosphereLookProfile>(LookPath);
	}

	/** D-295: the atmosphere at the planet's own scale, once per atmosphere actor (a tag marks it).
	 *  CloudAtmosphere's heights are fractions of the radius, its seeds draw a shell of 3-7% of it
	 *  (hundreds of km here); a real one is the planet's air: a depth that follows the scale height
	 *  kT / mg, Earth's at Earth's gravity and temperature. Locked, so every generation keeps it.
	 *  Before the atmosphere's first generation this is all; after it, one generation again (its
	 *  sim restarts once). */
	void ApplyAtmosphereScale(ASeededAtmosphereActor& A, const FUniBodyDesc& Desc, double SolarDayS)
	{
		const float Footprint = CVarUniPlanetCloudStepFootprint.GetValueOnGameThread();
		const int32 Taps = CVarUniPlanetCloudStepTaps.GetValueOnGameThread();
		if (Footprint >= 0.0f) { A.Sampling.StepFootprint = Footprint; }
		if (Taps > 0) { A.Sampling.StepTaps = FMath::Clamp(Taps, 1, 8); }
		// D-302: far cloud as the mean of the cloud (CloudAtmosphere stage RS3).
		const float Prefilter = CVarUniPlanetCloudPrefilter.GetValueOnGameThread();
		if (Prefilter >= 0.0f) { A.Sampling.NoisePrefilter = Prefilter; }
		// D-301: the planet turns itself (D-299): the atmosphere does not turn its clouds about it
		// as well (its field's frame is the planet's; locked, every generation keeps it).
		if (SolarDayS > 0.0)
		{
			A.Terrestrial.Planet.SpinRatio = 0.0f;
			A.GasGiant.Planet.SpinRatio = 0.0f;
			A.Locks.AddUnique(TEXT("Planet.SpinRatio"));
		}
		const double AtmoKm = double(CVarUniPlanetAtmoThicknessKm.GetValueOnGameThread());
		const double CloudKm = double(CVarUniPlanetCloudLayerKm.GetValueOnGameThread());
		if (Desc.PlanetType != 0 || Desc.RadiusCm <= 0.0 || AtmoKm <= 0.0)
		{
			if (A.Report.Num() > 0) { A.Generate(); }   // the locks above, for a generated giant
			UE_LOG(LogUnifiedUniverseGame, Log, TEXT("UnifiedUniverse: planet %08x's atmosphere: its own scale; cloud prefilter %.2f, field spin %s"),
				uint32(Desc.PlanetSeed), A.Sampling.NoisePrefilter, SolarDayS > 0.0 ? TEXT("off (the planet turns)") : TEXT("its own"));
			return;
		}
		const double Gravity = FMath::Max(Desc.SurfaceGravity, 1.0) / UniCore::PlanetPropsConst::EarthG;
		const double Temperature = (Desc.TeqK > 0.0 ? Desc.TeqK : 255.0) / 255.0;   // Earth's equilibrium temperature
		const double Scale = FMath::Clamp(Temperature / Gravity, 0.25, 4.0);
		const double ThicknessCm = AtmoKm * 1.0e5 * Scale;
		const double HeightScale = FMath::Clamp(ThicknessCm / Desc.RadiusCm, 0.005, 0.2);
		A.Terrestrial.Planet.HeightScale = float(HeightScale);
		A.Locks.AddUnique(TEXT("Planet.HeightScale"));
		double CloudShare = -1.0;
		if (CloudKm > 0.0)
		{
			CloudShare = FMath::Clamp(CloudKm * 1.0e5 * Scale / (HeightScale * Desc.RadiusCm), 0.02, 1.0);
			A.Terrestrial.Shape.CloudThickness = float(CloudShare);
			A.Locks.AddUnique(TEXT("Shape.CloudThickness"));
		}
		const bool bGenerated = A.Report.Num() > 0;
		if (bGenerated) { A.Generate(); }
		UE_LOG(LogUnifiedUniverseGame, Log, TEXT("UnifiedUniverse: planet %08x's atmosphere at its scale: %.0f km deep (HeightScale %.4f), weather layer %.1f km (CloudThickness %.3f); step footprint %.2f, %d taps, prefilter %.2f, field spin %s%s"),
			uint32(Desc.PlanetSeed), HeightScale * Desc.RadiusCm * 1.0e-5, HeightScale, CloudShare > 0.0 ? CloudShare * HeightScale * Desc.RadiusCm * 1.0e-5 : 0.0, CloudShare,
			A.Sampling.StepFootprint, A.Sampling.StepTaps, A.Sampling.NoisePrefilter, SolarDayS > 0.0 ? TEXT("off (the planet turns)") : TEXT("its own"),
			bGenerated ? TEXT("; regenerated (its sim restarts)") : TEXT(""));
	}

	/** D-298: one actor and its ticking components into a tick group (unchanged ones untouched). */
	void SetActorTickGroup(AActor& A, ETickingGroup Group)
	{
		if (A.PrimaryActorTick.TickGroup != Group) { A.SetTickGroup(Group); }
		for (UActorComponent* C : A.GetComponents())
		{
			if (C && C->PrimaryComponentTick.bCanEverTick && C->PrimaryComponentTick.TickGroup != Group) { C->SetTickGroup(Group); }
		}
	}

	APlanetActor* AsPlanet(AActor* Body) { return Cast<APlanetActor>(Body); }
	const APlanetActor* AsPlanet(const AActor* Body) { return Cast<const APlanetActor>(Body); }
}

UClass* UUniVoxelPlanetFactory::ClassFor(const FUniBodyDesc& Desc) const
{
	int32 Slot = -1;
	FString Path;
	if (Desc.PlanetType == 0) { Slot = 0; Path = CVarUniPlanetClass.GetValueOnGameThread(); }
	else if (Desc.PlanetType == 1) { Slot = 1; Path = CVarUniPlanetGasGiantClass.GetValueOnGameThread(); }
	else { return nullptr; }   // ice giants: the placeholder (D-290)
	Path.TrimStartAndEndInline();
	if (Path.IsEmpty()) { return Slot == 0 ? APlanetActor::StaticClass() : nullptr; }
	if (Loaded[Slot].IsValid() && LoadedPath[Slot] == Path) { return Loaded[Slot].Get(); }
	UClass* C = FSoftClassPath(Path).TryLoadClass<APlanetActor>();
	if (!C || !C->IsChildOf(APlanetActor::StaticClass()))
	{
		UE_LOG(LogUnifiedUniverseGame, Error, TEXT("UnifiedUniverse: %s '%s' is not an APlanetActor class%s"),
			Slot == 0 ? TEXT("uu.Planet.Class") : TEXT("uu.Planet.GasGiantClass"), *Path, Slot == 0 ? TEXT(": APlanetActor is used") : TEXT(": gas giants keep their placeholders"));
		C = Slot == 0 ? APlanetActor::StaticClass() : nullptr;
	}
	LoadedPath[Slot] = Path;
	Loaded[Slot] = C;
	return C;
}

bool UUniVoxelPlanetFactory::Supports(const FUniBodyDesc& Desc) const
{
	return Desc.RadiusCm > 0.0 && ClassFor(Desc) != nullptr;
}

void UUniVoxelPlanetFactory::Configure(APlanetActor& P, const FUniBodyDesc& Desc)
{
	// The surface's seed (channel PLANET_SURFACE), its low 31 bits: the actor's seeds are int32
	// and -1 means "the terrain's" for the ocean's and the atmosphere's.
	const int32 Seed = int32(uint32(Desc.SurfaceSeed) & 0x7FFFFFFFu);
	const bool bGiant = Desc.PlanetType != 0;
	if (!bGiant)
	{
		P.Generator = CVarUniPlanetSurface.GetValueOnGameThread() != 0 ? EVoxelSurfaceGenerator::Terrestrial : EVoxelSurfaceGenerator::Noise;
		P.TerrestrialSettings.Seed = Seed;
		P.SurfaceSettings.Seed = Seed;
		P.bEnableOcean = Desc.bOcean;
	}
	else
	{
		P.bEnableOcean = false;
	}
	P.OceanSeed = -1;
	P.AtmosphereSeed = -1;
	// D-294: the atmosphere's tuned archetype and look where the class has none (the atmosphere's
	// class defaults are an untuned look: thick, stretched cloud).
	FillModelSet(P.AtmosphereSet.Terrestrial, CVarUniPlanetAtmoArchetype.GetValueOnGameThread(), CVarUniPlanetAtmoLook.GetValueOnGameThread());
	FillModelSet(P.AtmosphereSet.GasGiant, CVarUniPlanetGasGiantAtmoArchetype.GetValueOnGameThread(), CVarUniPlanetGasGiantAtmoLook.GetValueOnGameThread());
	switch (UniCore::EPlanetAtmo(uint32(Desc.Atmosphere)))
	{
	case UniCore::EPlanetAtmo::None:
		P.bEnableAtmosphere = false;
		break;
	case UniCore::EPlanetAtmo::AirOnly:
		P.bEnableAtmosphere = true;
		P.AtmosphereType = EPlanetAtmosphereType::AirOnly;
		break;
	case UniCore::EPlanetAtmo::Terrestrial:
		P.bEnableAtmosphere = true;
		P.AtmosphereType = EPlanetAtmosphereType::Terrestrial;
		break;
	case UniCore::EPlanetAtmo::GasGiant:
		P.bEnableAtmosphere = true;
		P.AtmosphereType = EPlanetAtmosphereType::GasGiant;
		break;
	}
	P.bEnableGravity = true;
}

AActor* UUniVoxelPlanetFactory::Acquire(UWorld* World, const FUniBodyDesc& Desc, const FVector& WorldLocation)
{
	UClass* C = ClassFor(Desc);
	if (!World || !C) { return nullptr; }
	// D-294: the terrain's debug colours stand in for terrain materials (set by code: a typed or
	// ini value wins).
	const int32 LandView = CVarUniPlanetLandView.GetValueOnGameThread();
	if (LandView >= 0)
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Debug.LandView")))
		{
			V->Set(LandView, ECVF_SetByCode);
		}
	}
	// Spawned at its true radius (the actor's scale), so its first build is at that radius.
	const FTransform Xf(FQuat::Identity, WorldLocation, FVector(Desc.RadiusCm));
	APlanetActor* P = World->SpawnActorDeferred<APlanetActor>(C, Xf, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!P) { return nullptr; }
	Configure(*P, Desc);
#if WITH_EDITOR
	P->SetActorLabel(FString::Printf(TEXT("UniPlanet_%08x"), uint32(Desc.PlanetSeed)));
#endif
	P->FinishSpawning(Xf);
	P->OnAcquired(Desc.RadiusCm);
	UE_LOG(LogUnifiedUniverseGame, Log, TEXT("UnifiedUniverse: planet %08x as %s: radius %.0f km, %s surface seed %d, ocean %d, atmosphere %d, %.2f g"),
		uint32(Desc.PlanetSeed), *C->GetName(), Desc.RadiusCm * 1.0e-5, P->Generator == EVoxelSurfaceGenerator::Terrestrial ? TEXT("terrestrial") : TEXT("noise"),
		P->TerrestrialSettings.Seed, P->bEnableOcean ? 1 : 0, Desc.Atmosphere, Desc.SurfaceGravity / UniCore::PlanetPropsConst::EarthG);
	return P;
}

void UUniVoxelPlanetFactory::UpdateBody(AActor* Body, const FUniBodyDesc& Desc, const FVector& StarWorldLocation)
{
	APlanetActor* P = AsPlanet(Body);
	if (!P) { return; }
	P->SetStarWorldPosition(StarWorldLocation);
	// D-298: everything of the planet's that reads its place ticks after the universe placed it
	// (this frame's place, this frame's camera). Its parts are attached actors, some made later.
	{
		const ETickingGroup Group = CVarUniPlanetTickLate.GetValueOnGameThread() != 0 ? TG_PostUpdateWork : TG_PrePhysics;
		SetActorTickGroup(*P, Group);
		TArray<AActor*> Parts;
		P->GetAttachedActors(Parts, true, true);
		for (AActor* A : Parts)
		{
			if (A) { SetActorTickGroup(*A, Group); }
		}
	}
	// D-295: its atmosphere at its own scale, once the planet has made it.
	if (ASeededAtmosphereActor* Atmo = P->GetAtmosphereActor())
	{
		static const FName Applied(TEXT("UniAtmosphereScale"));
		if (!Atmo->Tags.Contains(Applied))
		{
			ApplyAtmosphereScale(*Atmo, Desc, SolarDay);
			Atmo->Tags.Add(Applied);
		}
		// D-301: the weather on the planet's clock.
		const int32 Mode = CVarUniPlanetWeatherClock.GetValueOnGameThread();
		UWorld* World = P->GetWorld();
		UFlowSimSubsystem* Sim = (Mode >= 0 && World) ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;
		if (Sim)
		{
			double Scale = FMath::Max(ClockRate, 0.0);
			const UFlowSimConfig* Config = Sim->GetConfig();
			if (Mode == 1 && SolarDay > 0.0 && Config && Config->SimSpeed > 0.0f)
			{
				// The sim's day: one turn of the planet its Coriolis assumes (4 pi / PlanetaryVorticity,
				// FlowSimSubsystem's log), in sim time; a planet's day of it each SolarDay of universe time.
				const double SimDay = 4.0 * UE_DOUBLE_PI / double(Config->GetPlanetaryVorticity());
				Scale *= SimDay / (double(Config->SimSpeed) * SolarDay);
			}
			if (!FMath::IsNearlyEqual(double(Sim->GetTimeScale()), Scale, Scale * 1.0e-6 + 1.0e-12)) { Sim->SetTimeScale(float(Scale)); }
		}
	}
	if (APlanetGravityZone* Z = P->GetGravityZone())
	{
		// The planet's pull (UniPlanetProps: from its radius and type, within play's limits); the
		// zone's direction is its own (towards the centre).
		const FVector G(0.0, 0.0, -Desc.SurfaceGravity);
		if (!Z->BaseVector.Equals(G, 1.0e-3)) { Z->BaseVector = G; }
	}
}

bool UUniVoxelPlanetFactory::DensityReady(const APlanetActor& P)
{
	const double R = P.GetActorScale3D().GetMax();
	const FVoxelWaterSample S = P.GetWaterAt(P.GetActorLocation() + FVector(0.0, 0.0, kProbeRadii * R));
	return S.PointDepth < kProbeAboveM;
}

bool UUniVoxelPlanetFactory::IsReady(AActor* Body) const
{
	const APlanetActor* P = AsPlanet(Body);
	return P && P->GetBakeProgress() >= 1.0f && P->GetTerrainActor() != nullptr && DensityReady(*P);
}

FUniGroundSample UUniVoxelPlanetFactory::GroundAt(AActor* Body, const FVector& WorldPoint) const
{
	FUniGroundSample Out;
	const APlanetActor* P = AsPlanet(Body);
	if (!P || !DensityReady(*P)) { return Out; }
	const FVoxelWaterSample S = P->GetWaterAt(WorldPoint);
	Out.bValid = true;
	Out.bDry = S.Kind == EVoxelWaterKind::None;
	Out.HeightM = double(S.GroundHeight);
	// The ground's radius: the point's distance, plus its depth below the water's surface (m), less
	// the water's surface above sea level, plus the ground's (GetWaterAt: PointDepth = (sea radius +
	// surface height - distance) / 100 m).
	const double Dist = (WorldPoint - P->GetActorLocation()).Size();
	Out.GroundRadiusCm = Dist + 100.0 * (double(S.PointDepth) - double(S.SurfaceHeight) + double(S.GroundHeight));
	return Out;
}

void UUniVoxelPlanetFactory::Park(AActor* Body)
{
	if (APlanetActor* P = AsPlanet(Body)) { P->OnReturnToPool(); }
}

void UUniVoxelPlanetFactory::Wake(AActor* Body, const FUniBodyDesc& Desc)
{
	if (APlanetActor* P = AsPlanet(Body)) { P->OnAcquired(Desc.RadiusCm); }
}

void UUniVoxelPlanetFactory::SetClock(double UniverseSecondsPerSecond, double SolarDayS)
{
	ClockRate = UniverseSecondsPerSecond;
	SolarDay = SolarDayS;
}

void UUniVoxelPlanetFactory::Release(AActor* Body)
{
	if (Body) { Body->Destroy(); }
}
