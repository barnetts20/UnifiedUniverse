// UniSubsystemPlanets.cpp
// D-293 (SHIPS_PLANETS.md; D-282, D-290, D-291, D-292): planets as worlds.
//   - The planet's frame: within a few radii of a planet (Local mode), the UE world's origin is the
//     planet's exact centre, following its orbit, so the planet never moves under the player and
//     the universe moves around it instead (translation only: planets do not spin yet).
//   - Detailed bodies: the frame planet's world-to-stand-on actor, from a game module's body factory
//     (UniBodyFactory.h), pinned at the origin; it replaces the placeholder sphere once ready, and
//     is parked (not destroyed) when the frame moves on, for a warm return.
//   - The home start (uu.AutoStart home / uu.Enable home): the home system activated at once on the
//     CPU, the world in the home planet's frame, the pawn held above a spawn point until the ground
//     is there, then released onto it.

#include "UniSubsystem.h"

#include "UnifiedUniverseModule.h"
#include "UniBodyActor.h"
#include "UniBodyFactory.h"
#include "UniHome.h"
#include "UniLocalFrame.h"
#include "UniParallax.h"
#include "UniPlanetProps.h"
#include "UniScaleLadder.h"

#include "Engine/World.h"
#include "Engine/EngineTypes.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/CapsuleComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include <algorithm>
#include <cmath>

static TAutoConsoleVariable<int32> CVarUniPlanetFrame(TEXT("uu.Planet.Frame"), 1, TEXT("D-293: near a planet (Local mode) the world is its frame: its centre the origin, the planet still, the universe moving round it. 0: never (the planet moves along its orbit through the world, as before)."));
static TAutoConsoleVariable<float> CVarUniPlanetFrameRadii(TEXT("uu.Planet.FrameRadii"), 3.0f, TEXT("D-293: the planet's frame holds within this many of its radii of its centre (entered there, left at uu.Planet.FrameExit x it)."));
static TAutoConsoleVariable<float> CVarUniPlanetFrameExit(TEXT("uu.Planet.FrameExit"), 1.2f, TEXT("D-293: the frame is left at this many times its radius (hysteresis)."));
static TAutoConsoleVariable<int32> CVarUniPlanetPool(TEXT("uu.Planet.Pool"), 1, TEXT("D-292: planets' detailed actors kept parked (dormant) after their frame is left, for an instant return; beyond this many the oldest is released. Each holds GBs of GPU memory."));
static TAutoConsoleVariable<int32> CVarUniPlanetMaxLive(TEXT("uu.Planet.MaxLive"), 1, TEXT("D-297: planets with a live detailed body at once (the frame planet first, then the nearest promoted ones); the rest keep their sprites until they are among the nearest. Each holds GBs of GPU memory."));
static TAutoConsoleVariable<int32> CVarUniPlanetPlaceholders(TEXT("uu.Planet.Placeholders"), 0, TEXT("D-297: 1: promoted planets a body factory makes get the old placeholder sphere as well (A/B); 0: never (their sprite stands in until the detailed body is ready). Planets no factory makes (ice giants; gas giants without uu.Planet.GasGiantClass) and stars always get theirs. Applies to planets promoted after the change."));
static TAutoConsoleVariable<float> CVarUniPlanetMinG(TEXT("uu.Planet.MinG"), 0.3f, TEXT("D-293: play's lower limit on a planet's surface gravity, in g (applies to planets acquired after the change)."));
static TAutoConsoleVariable<float> CVarUniPlanetMaxG(TEXT("uu.Planet.MaxG"), 2.0f, TEXT("D-293: play's upper limit on a planet's surface gravity, in g."));
static TAutoConsoleVariable<float> CVarUniHomeTimeout(TEXT("uu.Home.Timeout"), 300.0f, TEXT("D-293: seconds the home start waits for the planet's ground, then for its collision, before giving up (logged as an error; the pawn stays held, uu.Fly moves it). Generous: a first run compiles the planet's shaders."));
static TAutoConsoleVariable<float> CVarUniPlanetDaySkyHide(TEXT("uu.Planet.DaySkyHide"), 1.0f, TEXT("D-296: how much of the backdrop (the galaxy's glow and the stars) a planet's day sky hides, at noon near the ground: its light x (1 - this x the daylight). The sprites also fade by the atmosphere's own sky brightness per pixel (uu.Sprite.SkyFadeStart / End); this is the glow's, which is drawn before the atmosphere. 0: none."));
static TAutoConsoleVariable<float> CVarUniPlanetDaySkyHeightKm(TEXT("uu.Planet.DaySkyHeightKm"), 40.0f, TEXT("D-296: the height above the planet's surface by which its day sky no longer hides the backdrop (from a fifth of it up)."));
static TAutoConsoleVariable<float> CVarUniHomeWaitAltitude(TEXT("uu.Home.WaitAltitudeM"), 3000.0f, TEXT("D-293: metres above sea level the pawn waits, held, while the home planet builds."));
static TAutoConsoleVariable<float> CVarUniPlanetDayHours(TEXT("uu.Planet.DayHours"), 24.0f,
	TEXT("D-299: the planets' solar day, hours of universe time (uu.TimeScale 1: real hours; it scales with the clock as the orbits do): every planet turns about the universe's +Z once in 1 / (1 / day + 1 / year), so its sun comes back once a day and the year shows in the day's drift. The world turns with the planet whose frame it is in (the ground stays put, the sky goes round). 0: no spin (planets keep their turn at the epoch)."));

namespace
{
	UUniSubsystem* PlanetsSub(UWorld* World)
	{
		return World ? World->GetSubsystem<UUniSubsystem>() : nullptr;
	}

	const TCHAR* AtmoName(int32 A)
	{
		switch (A) { case 1: return TEXT("air alone"); case 2: return TEXT("weather"); case 3: return TEXT("gas giant"); default: return TEXT("none"); }
	}
}

// ---------------------------------------------------------------------------------------------
// Body factories
// ---------------------------------------------------------------------------------------------

void UUniBodyFactory::Release(AActor* Body)
{
	if (Body) { Body->Destroy(); }
}

TArray<TSubclassOf<UUniBodyFactory>>& UUniSubsystem::BodyFactoryClasses()
{
	static TArray<TSubclassOf<UUniBodyFactory>> Classes;
	return Classes;
}

void UUniSubsystem::RegisterBodyFactoryClass(TSubclassOf<UUniBodyFactory> FactoryClass)
{
	if (FactoryClass) { BodyFactoryClasses().AddUnique(FactoryClass); }
}

void UUniSubsystem::UnregisterBodyFactoryClass(TSubclassOf<UUniBodyFactory> FactoryClass)
{
	BodyFactoryClasses().Remove(FactoryClass);
}

UUniBodyFactory* UUniSubsystem::FactoryFor(const FUniBodyDesc& Desc) const
{
	for (const TObjectPtr<UUniBodyFactory>& F : BodyFactories)
	{
		if (F && F->Supports(Desc)) { return F.Get(); }
	}
	return nullptr;
}

AActor* UUniSubsystem::GetFramePlanetActor() const
{
	if (!BodyFrame.bActive) { return nullptr; }
	const FDetailedBody* D = DetailedBodies.Find(uint32(BodyFrame.Desc.PlanetSeed));
	return D && !D->bParked ? D->Actor.Get() : nullptr;
}

// ---------------------------------------------------------------------------------------------
// The planet's frame
// ---------------------------------------------------------------------------------------------

bool UUniSubsystem::PlanetCentreOf(uint32 StarSeed, int32 Ordinal, double TimeS, UniCore::FCoord& OutCentre, UniCore::FVec3f& OutFrac, const UniCore::FActiveSystem** OutSystem) const
{
	if (!Systems) { return false; }
	const auto& Act = Systems->ActiveSystems();
	const auto It = Act.find(StarSeed);
	if (It == Act.end() || Ordinal < 0 || size_t(Ordinal) >= It->second.Sys.Planets.size()) { return false; }
	UniCore::PlanetCentreAt(It->second.Star, It->second.Sys.Planets[size_t(Ordinal)], TimeS, OutCentre, OutFrac);
	if (OutSystem) { *OutSystem = &It->second; }
	return true;
}

FUniBodyDesc UUniSubsystem::DescOf(const UniCore::FActiveSystem& A, int32 Ordinal) const
{
	const UniCore::FPlanet& P = A.Sys.Planets[size_t(Ordinal)];
	UniCore::FPlanetGravityLimits Lim;
	Lim.MinG = double(FMath::Max(CVarUniPlanetMinG.GetValueOnGameThread(), 0.01f));
	Lim.MaxG = double(FMath::Max(CVarUniPlanetMaxG.GetValueOnGameThread(), CVarUniPlanetMinG.GetValueOnGameThread()));
	const UniCore::FPlanetProps Pr = UniCore::PlanetPropsOf(A.Sys, P, double(FMath::Max(CVarUniPlanetFrameRadii.GetValueOnGameThread(), 1.05f)), Lim);
	FUniBodyDesc D;
	D.PlanetType = int32(P.Type);
	D.RadiusCm = P.RadiusCm;
	D.PlanetSeed = int64(P.Seed);
	D.StarSeed = int64(A.Sys.StarSeed);
	D.Ordinal = Ordinal;
	D.SurfaceSeed = int64(Pr.SurfaceSeed);
	D.Atmosphere = int32(Pr.Atmosphere);
	D.bOcean = Pr.bOcean;
	D.SurfaceGravity = Pr.SurfaceGravity;
	D.TeqK = Pr.TeqK;
	return D;
}

UniCore::FCoord UUniSubsystem::LocalCamera(const FVector& ViewLoc, UniCore::FVec3f& OutFrac) const
{
	// D-299: the view in the universe's axes (the world turns with the frame's planet).
	const UniCore::FVec3d V = ToUniversalAxes(ViewLoc);
	return UniCore::LocalToUniversal(LocalAnchor.ToCore(),
		UniCore::FVec3d{ V.X + double(LocalAnchorFrac.X), V.Y + double(LocalAnchorFrac.Y), V.Z + double(LocalAnchorFrac.Z) }, OutFrac);
}

double UUniSubsystem::PlanetSpinNow(const UniCore::FActiveSystem& A, int32 Ordinal) const
{
	if (Ordinal < 0 || size_t(Ordinal) >= A.Sys.Planets.size()) { return 0.0; }
	const double DayS = 3600.0 * double(FMath::Max(CVarUniPlanetDayHours.GetValueOnGameThread(), 0.0f));
	if (DayS <= 0.0) { return 0.0; }
	return UniCore::SpinAngle(UniverseTime, UniCore::PlanetSpinPeriodS(A.Sys.Planets[size_t(Ordinal)], DayS));
}

void UUniSubsystem::LeaveBodyFrame(const TCHAR* Why)
{
	if (!BodyFrame.bActive) { return; }
	// The origin stays where the planet's centre last was: nothing in the world moves now; the
	// planet (its placeholder, parked detail) moves on along its orbit through the world.
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: left planet %08x's frame (%s)"), uint32(BodyFrame.Desc.PlanetSeed), Why);
	BodyFrame = FPlanetFrame();
	WorldTurn.Hold();   // D-299: the world keeps its turn; the planet turns on in it
}

void UUniSubsystem::UpdateBodyFrame(FVector& InOutViewLoc)
{
	if (!Systems || MotionMode != EUniMotionMode::Local) { LeaveBodyFrame(TEXT("not in Local mode")); return; }
	if (BodyFrame.bActive)
	{
		UniCore::FCoord O;
		UniCore::FVec3f F;
		const UniCore::FActiveSystem* Sys = nullptr;
		if (!PlanetCentreOf(BodyFrame.StarSeed, BodyFrame.Ordinal, UniverseTime, O, F, &Sys) || !Sys)
		{
			LeaveBodyFrame(TEXT("its system deactivated"));
		}
		else if (CVarUniPlanetFrame.GetValueOnGameThread() == 0)
		{
			LeaveBodyFrame(TEXT("uu.Planet.Frame 0"));
		}
		else if (InOutViewLoc.Size() > BodyFrame.FrameRadiusCm * double(FMath::Max(CVarUniPlanetFrameExit.GetValueOnGameThread(), 1.0f)))
		{
			LeaveBodyFrame(TEXT("beyond its frame radius"));
		}
		else
		{
			// Follow it: the origin is its exact centre this frame (D-291), the world's axes turned
			// with it (D-299).
			LocalAnchor = FUniCoord::Make(O);
			LocalAnchorFrac = FVector3f(F.X, F.Y, F.Z);
			WorldTurn.Follow(uint32(BodyFrame.Desc.PlanetSeed), PlanetSpinNow(*Sys, BodyFrame.Ordinal));
			return;
		}
	}
	if (CVarUniPlanetFrame.GetValueOnGameThread() == 0) { return; }
	// Not in a frame: the nearest planet whose frame radius holds the camera.
	UniCore::FVec3f CamF{};
	const UniCore::FCoord Cam = LocalCamera(InOutViewLoc, CamF);
	const double FrameRadii = double(FMath::Max(CVarUniPlanetFrameRadii.GetValueOnGameThread(), 1.05f));
	const UniCore::FActiveSystem* Best = nullptr;
	int32 BestOrdinal = -1;
	double BestRel = 1.0;
	UniCore::FCoord BestO;
	UniCore::FVec3f BestF{};
	for (const auto& KV : Systems->ActiveSystems())
	{
		const UniCore::FActiveSystem& A = KV.second;
		for (size_t I = 0; I < A.Sys.Planets.size(); ++I)
		{
			const UniCore::FPlanet& P = A.Sys.Planets[I];
			UniCore::FCoord O;
			UniCore::FVec3f F;
			UniCore::PlanetCentreAt(A.Star, P, UniverseTime, O, F);
			const UniCore::FVec3d D = UniCore::ToDouble(UniCore::Sub(Cam, O));
			const double Dist = UniCore::Length(UniCore::FVec3d{ D.X + double(CamF.X) - double(F.X), D.Y + double(CamF.Y) - double(F.Y), D.Z + double(CamF.Z) - double(F.Z) });
			const double Rel = Dist / (FrameRadii * P.RadiusCm);
			if (Rel < BestRel) { BestRel = Rel; Best = &A; BestOrdinal = int32(I); BestO = O; BestF = F; }
		}
	}
	if (!Best) { return; }
	// Enter: the world moves so the planet's centre is the origin (D-293: FrameShift keeps every
	// universal point where it is, so nothing is seen to move).
	const UniCore::FVec3d Sh = UniCore::FrameShift(LocalAnchor.ToCore(), UniCore::FVec3f{ LocalAnchorFrac.X, LocalAnchorFrac.Y, LocalAnchorFrac.Z }, BestO, BestF);
	LocalAnchor = FUniCoord::Make(BestO);
	LocalAnchorFrac = FVector3f(BestF.X, BestF.Y, BestF.Z);
	ShiftWorld(ToWorldAxes(Sh), InOutViewLoc);   // D-299: the shift in the world's axes
	BodyFrame.bActive = true;
	BodyFrame.StarSeed = Best->Sys.StarSeed;
	BodyFrame.Ordinal = BestOrdinal;
	BodyFrame.Desc = DescOf(*Best, BestOrdinal);
	BodyFrame.RadiusCm = BodyFrame.Desc.RadiusCm;
	BodyFrame.FrameRadiusCm = FrameRadii * BodyFrame.RadiusCm;
	WorldTurn.Follow(uint32(BodyFrame.Desc.PlanetSeed), PlanetSpinNow(*Best, BestOrdinal));   // D-299: from the world's turn now, no jump
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: entered planet %08x's frame (star %08x #%d, radius %s, %s%s, %.2f g); the world moved by %s"),
		uint32(BodyFrame.Desc.PlanetSeed), BodyFrame.StarSeed, BestOrdinal, UTF8_TO_TCHAR(UniCore::FormatDistance(BodyFrame.RadiusCm).c_str()),
		AtmoName(BodyFrame.Desc.Atmosphere), BodyFrame.Desc.bOcean ? TEXT(", seas") : TEXT(""), BodyFrame.Desc.SurfaceGravity / UniCore::PlanetPropsConst::EarthG,
		UTF8_TO_TCHAR(UniCore::FormatDistance(UniCore::Length(Sh)).c_str()));
}

// ---------------------------------------------------------------------------------------------
// Detailed bodies
// ---------------------------------------------------------------------------------------------

bool UUniSubsystem::BodyDescOf(uint32 StarSeed, uint32 PlanetSeed, FUniBodyDesc& OutDesc) const
{
	if (!Systems) { return false; }
	const auto& Act = Systems->ActiveSystems();
	const auto It = Act.find(StarSeed);
	if (It == Act.end()) { return false; }
	for (size_t I = 0; I < It->second.Sys.Planets.size(); ++I)
	{
		if (It->second.Sys.Planets[I].Seed == PlanetSeed)
		{
			OutDesc = DescOf(It->second, int32(I));
			return true;
		}
	}
	return false;
}

bool UUniSubsystem::HasDetailedBody(uint32 PlanetSeed) const
{
	// D-297: a planet a factory makes is never a placeholder sphere; its sprite stands in until
	// its detailed body is live and ready.
	const FPromotedBodyLink* L = PromotedLinks.Find(PlanetSeed);
	return L && L->bSupported;
}

std::set<UniCore::u32> UUniSubsystem::SpriteCulledIds() const
{
	// Promoted planets whose actor draws them: a placeholder, or a live, ready detailed body. A
	// planet waiting for its detailed body (building, or beyond uu.Planet.MaxLive) keeps its sprite.
	std::set<UniCore::u32> Out;
	for (const auto& KV : Promoter.Promoted())
	{
		if (KV.second.bStar) { continue; }
		const TWeakObjectPtr<AUniBodyActor>* B = Bodies.Find(KV.first);
		const FDetailedBody* D = DetailedBodies.Find(KV.first);
		if ((B && B->IsValid()) || (D && !D->bParked && D->bReady)) { Out.insert(KV.first); }
	}
	return Out;
}

bool UUniSubsystem::WantsPlaceholder(const UniCore::FPromotedBody& B)
{
	// D-297: a star, or a planet no factory makes, gets the placeholder sphere; a planet a factory
	// makes gets none (uu.Planet.Placeholders 1: both, for A/B). Decided once per promotion.
	if (B.bStar) { return true; }
	FPromotedBodyLink* L = PromotedLinks.Find(B.Id);
	if (!L)
	{
		FUniBodyDesc Desc;
		FPromotedBodyLink N;
		N.bSupported = BodyDescOf(B.SystemSeed, B.Id, Desc) && FactoryFor(Desc) != nullptr;
		L = &PromotedLinks.Add(B.Id, N);
	}
	return !L->bSupported || CVarUniPlanetPlaceholders.GetValueOnGameThread() != 0;
}

void UUniSubsystem::UpdateDetailedBodies(const FVector& ViewLoc)
{
	BackdropDayScale = 1.0f;   // D-296: set below while a planet with air is the frame
	bDetailedLit = false;
	UWorld* World = GetWorld();
	if (!World || !Systems) { return; }
	const double Now = FPlatformTime::Seconds();
	const uint32 FrameSeed = BodyFrame.bActive ? uint32(BodyFrame.Desc.PlanetSeed) : 0u;
	const UniCore::FCoord Cam = Camera.ToCore();
	const UniCore::FVec3f CamF{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z };

	// D-301: the clock the bodies' own clocks (their weather) follow.
	const double DayS = 3600.0 * double(FMath::Max(CVarUniPlanetDayHours.GetValueOnGameThread(), 0.0f));
	for (const TObjectPtr<UUniBodyFactory>& Factory : BodyFactories)
	{
		if (Factory) { Factory->SetClock(TimeScale, DayS); }
	}

	// D-297: the planets that get a detailed body this frame: every promoted planet a factory
	// makes (D-119: within 10,000 km of its surface or 0.02 rad across), the frame planet first,
	// then the nearest, up to uu.Planet.MaxLive.
	struct FWant { uint32 Seed; uint32 StarSeed; double Gap; };
	TArray<FWant> Want;
	for (const auto& KV : Promoter.Promoted())
	{
		const UniCore::FPromotedBody& B = KV.second;
		if (B.bStar || !HasDetailedBody(KV.first)) { continue; }
		const double Gap = UniCore::Length(UniCore::ToDouble(UniCore::Sub(B.Coord, Cam))) - B.RadiusCm;
		Want.Add(FWant{ KV.first, B.SystemSeed, KV.first == FrameSeed ? -1.0e300 : Gap });
	}
	if (BodyFrame.bActive && FactoryFor(BodyFrame.Desc) && !Want.ContainsByPredicate([&](const FWant& W) { return W.Seed == FrameSeed; }))
	{
		Want.Add(FWant{ FrameSeed, BodyFrame.StarSeed, -1.0e300 });
	}
	Want.Sort([](const FWant& A, const FWant& B) { return A.Gap < B.Gap; });
	const int32 MaxLive = FMath::Max(1, CVarUniPlanetMaxLive.GetValueOnGameThread());
	if (Want.Num() > MaxLive) { Want.SetNum(MaxLive); }

	TSet<uint32> Live;
	for (const FWant& W : Want)
	{
		FUniBodyDesc Desc;
		if (W.Seed == FrameSeed && BodyFrame.bActive) { Desc = BodyFrame.Desc; }
		else if (!BodyDescOf(W.StarSeed, W.Seed, Desc)) { continue; }
		UUniBodyFactory* Fac = FactoryFor(Desc);
		const UniCore::FActiveSystem* Sys = nullptr;
		UniCore::FCoord O;
		UniCore::FVec3f F;
		if (!Fac || !PlanetCentreOf(uint32(Desc.StarSeed), Desc.Ordinal, UniverseTime, O, F, &Sys)) { continue; }
		// Where it is: the frame planet at the origin, exactly (D-291); any other at its true
		// place about the camera, at its true size (as UltraLargeScale carried the planet actor:
		// moved every frame, never scaled: its geometry is its radius).
		FVector Where = FVector::ZeroVector;
		if (W.Seed != FrameSeed)
		{
			const UniCore::FVec3d Rel = UniCore::ToDouble(UniCore::Sub(O, Cam));
			Where = ViewLoc + ToWorldAxes(UniCore::FVec3d{ Rel.X + double(F.X) - double(CamF.X), Rel.Y + double(F.Y) - double(CamF.Y), Rel.Z + double(F.Z) - double(CamF.Z) });
		}
		// D-299: its turn in the world: its spin less the world's turn (constant for the frame
		// planet, which the world turns with).
		const FQuat Turn(FVector::UpVector, WorldTurn.BodyYaw(W.Seed, PlanetSpinNow(*Sys, Desc.Ordinal)));
		FDetailedBody* D = DetailedBodies.Find(W.Seed);
		AActor* Body = D ? D->Actor.Get() : nullptr;
		if (!Body)
		{
			DetailedBodies.Remove(W.Seed);
			Body = Fac->Acquire(World, Desc, Where);
			if (!Body) { continue; }
			FDetailedBody N;
			N.Actor = Body;
			N.Factory = Fac;
			N.Desc = Desc;
			DetailedBodies.Add(W.Seed, N);
			D = DetailedBodies.Find(W.Seed);
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: planet %08x's detailed body acquired (%s), %s from its surface"), W.Seed, *Body->GetClass()->GetName(),
				UTF8_TO_TCHAR(UniCore::FormatDistance(FMath::Max(W.Gap, 0.0)).c_str()));
		}
		else if (D->bParked)
		{
			Fac->Wake(Body, D->Desc);
			D->bParked = false;
			D->bReady = false;
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: planet %08x's detailed body woken (warm return)"), W.Seed);
		}
		Live.Add(W.Seed);
		if (!Body->GetActorLocation().Equals(Where, 0.01) || !Body->GetActorQuat().Equals(Turn, 1.0e-10))   // the frame planet's turn is constant: set once
		{
			Body->SetActorLocationAndRotation(Where, Turn, false, nullptr, ETeleportType::TeleportPhysics);
		}
		// Its star, placed as the backdrop places it (the direction is what its sky needs).
		const UniCore::FParallaxPlacement Sp = UniCore::ParallaxPlace(Sys->Star, Sys->Sys.StarRadiusCm, Cam, CamF, false);
		Fac->UpdateBody(Body, D->Desc, ViewLoc + ToWorldAxes(Sp.Offset));   // D-299: the world's axes
		// D-296: daylight at the eye: the star's elevation (civil twilight, +-6 degrees, to full
		// day), near the ground (gone by uu.Planet.DaySkyHeightKm), on the frame planet with air.
		if (W.Seed == FrameSeed && D->Desc.Atmosphere != 0)
		{
			const double Rv = ViewLoc.Size();
			const FVector Up = Rv > 1.0 ? ViewLoc / Rv : FVector::UpVector;
			const FVector ToStar = ToWorldAxes(Sp.Offset).GetSafeNormal();
			const double SinElev = FVector::DotProduct(Up, ToStar);
			const double Day = FMath::SmoothStep(-0.105, 0.105, SinElev);
			const double TopCm = 1.0e5 * double(FMath::Max(CVarUniPlanetDaySkyHeightKm.GetValueOnGameThread(), 0.1f));
			const double Low = 1.0 - FMath::SmoothStep(0.2 * TopCm, TopCm, Rv - D->Desc.RadiusCm);
			BackdropDayScale = float(1.0 - FMath::Clamp(double(CVarUniPlanetDaySkyHide.GetValueOnGameThread()), 0.0, 1.0) * Day * Low);
		}
		const bool bWas = D->bReady;
		D->bReady = Fac->IsReady(Body);
		bDetailedLit = bDetailedLit || (D->bReady && D->Desc.Atmosphere != 0);
		if (D->bReady && !bWas) { UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: planet %08x's detailed body ready: it replaces its sprite"), W.Seed); }
	}

	// The rest: parked (D-292), the oldest released beyond the pool.
	int32 Parked = 0;
	for (auto It = DetailedBodies.CreateIterator(); It; ++It)
	{
		FDetailedBody& D = It.Value();
		AActor* Body = D.Actor.Get();
		UUniBodyFactory* Fac = D.Factory.Get();
		if (!Body || !Fac) { It.RemoveCurrent(); continue; }
		if (Live.Contains(It.Key())) { continue; }
		if (!D.bParked)
		{
			Fac->Park(Body);
			D.bParked = true;
			D.bReady = false;
			D.ParkedAt = Now;
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: planet %08x's detailed body parked"), It.Key());
		}
		++Parked;
	}
	const int32 Pool = FMath::Max(0, CVarUniPlanetPool.GetValueOnGameThread());
	while (Parked > Pool)
	{
		uint32 Oldest = 0;
		double OldestAt = 1.0e300;
		for (const auto& KV : DetailedBodies)
		{
			if (KV.Value.bParked && KV.Value.ParkedAt < OldestAt) { OldestAt = KV.Value.ParkedAt; Oldest = KV.Key; }
		}
		FDetailedBody Gone;
		if (!DetailedBodies.RemoveAndCopyValue(Oldest, Gone)) { break; }
		if (UUniBodyFactory* Fac = Gone.Factory.Get()) { Fac->Release(Gone.Actor.Get()); }
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: planet %08x's detailed body released (pool %d)"), Oldest, Pool);
		--Parked;
	}
	// Placeholders (uu.Planet.Placeholders 1) hide while their detailed body stands in for them.
	for (const auto& KV : Bodies)
	{
		AUniBodyActor* B = KV.Value.Get();
		if (!B) { continue; }
		const FDetailedBody* D = DetailedBodies.Find(KV.Key);
		const bool bHide = D && !D->bParked && D->bReady;
		if (B->IsHidden() != bHide) { B->SetActorHiddenInGame(bHide); }
	}
}

void UUniSubsystem::ReleaseDetailedBodies()
{
	for (auto& KV : DetailedBodies)
	{
		if (UUniBodyFactory* Fac = KV.Value.Factory.Get()) { Fac->Release(KV.Value.Actor.Get()); }
	}
	DetailedBodies.Reset();
}

// ---------------------------------------------------------------------------------------------
// The home start
// ---------------------------------------------------------------------------------------------

void UUniSubsystem::HoldPawn(bool bHold)
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	UCharacterMovementComponent* Move = Ch ? Ch->GetCharacterMovement() : nullptr;
	if (Move)
	{
		if (bHold) { Move->StopMovementImmediately(); Move->DisableMovement(); }
		else { Move->SetMovementMode(MOVE_Falling); }
	}
	bPawnHeld = bHold;
}

void UUniSubsystem::BeginHomeStart()
{
	if (!Model || !Systems) { return; }
	const UniCore::FUniverseDef& Def = Model->Definition();
	int32 AnchorIndex = -1;
	for (int32 I = int32(Def.Anchors.size()) - 1; I >= 0; --I)
	{
		if (Def.Anchors[size_t(I)].ArchetypeId == UniScenes::kStar) { AnchorIndex = I; break; }
	}
	if (AnchorIndex < 0) { UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: the home scene has no home star anchor")); return; }
	const UniCore::FAnchor& An = Def.Anchors[size_t(AnchorIndex)];
	UniCore::FCellKey K;
	const UniCore::FObjectGPU Rec = UniScenes::HomeStarRecord(*Model, An, K);
	Systems->BhConfig = StellarBhConfig();
	const UniCore::FSystem Sys = UniCore::BuildSystem(Rec, Systems->BhConfig);
	const int32 Ordinal = UniScenes::HomePlanetOf(Sys);
	if (Ordinal < 0) { UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: the home star (seed %u) has no home planet"), An.Seed); return; }
	const UniCore::u32 Off[3] = { Rec.A[0], Rec.A[1], Rec.A[2] };
	const UniCore::FCoord StarC = UniCore::ObjectCoord(K, Off);
	const UniCore::FPlanet& P = Sys.Planets[size_t(Ordinal)];
	UniCore::FCoord O;
	UniCore::FVec3f F;
	UniCore::PlanetCentreAt(StarC, P, UniverseTime, O, F);
	const UniCore::FVec3d ToPlanet = UniCore::PlanetOffset(P, UniverseTime);
	StartCandidates = UniScenes::HomeSpawnCandidates(P.Seed, UniCore::FVec3d{ -ToPlanet.X, -ToPlanet.Y, -ToPlanet.Z });
	// D-299: in the world's axes (the world turns with the planet from here: they stay on the ground).
	for (UniCore::FVec3d& C : StartCandidates) { C = WorldTurn.ToWorld(C); }
	const UniCore::FVec3d Dir0 = StartCandidates.empty() ? UniCore::FVec3d{ 0.0, 0.0, 1.0 } : StartCandidates[0];
	StartUp = FVector(Dir0.X, Dir0.Y, Dir0.Z);
	StartSpawn = StartUp * (P.RadiusCm + 100.0 * double(FMath::Max(CVarUniHomeWaitAltitude.GetValueOnGameThread(), 10.0f)));
	// The world in the home planet's frame from the first frame: its centre the origin, the camera
	// where the pawn will wait.
	LocalAnchor = FUniCoord::Make(O);
	LocalAnchorFrac = FVector3f(F.X, F.Y, F.Z);
	UniCore::FVec3f CamF{};
	Camera = FUniCoord::Make(LocalCamera(StartSpawn, CamF));
	CameraFraction = FVector3f(CamF.X, CamF.Y, CamF.Z);
	// The home system at once (the GPU's report a few frames later is a duplicate).
	UniCore::FObjectId StarId;
	StarId.Cell = K;
	StarId.Index = UniCore::kFirstAnchorIndex + UniCore::u32(AnchorIndex);
	Systems->Report(K, Rec, Camera.ToCore(), FrameCounter, &StarId);
	const UniCore::FActiveSystem* Act = nullptr;
	UniCore::FCoord O2;
	UniCore::FVec3f F2;
	if (!PlanetCentreOf(Sys.StarSeed, Ordinal, UniverseTime, O2, F2, &Act) || !Act)
	{
		UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: the home system did not activate"));
		return;
	}
	BodyFrame.bActive = true;
	BodyFrame.StarSeed = Sys.StarSeed;
	BodyFrame.Ordinal = Ordinal;
	BodyFrame.Desc = DescOf(*Act, Ordinal);
	BodyFrame.RadiusCm = P.RadiusCm;
	BodyFrame.FrameRadiusCm = double(FMath::Max(CVarUniPlanetFrameRadii.GetValueOnGameThread(), 1.05f)) * P.RadiusCm;
	StartPhase = EStartPhase::WaitPawn;
	StartPhaseSince = FPlatformTime::Seconds();
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: home start: star seed %u, planet #%d %08x (radius %s, %s%s, %.2f g, Teq %.0f K); waiting %.0f m up for the ground (%d spawn candidates)"),
		An.Seed, Ordinal, P.Seed, UTF8_TO_TCHAR(UniCore::FormatDistance(P.RadiusCm).c_str()), AtmoName(BodyFrame.Desc.Atmosphere), BodyFrame.Desc.bOcean ? TEXT(", seas") : TEXT(""),
		BodyFrame.Desc.SurfaceGravity / UniCore::PlanetPropsConst::EarthG, BodyFrame.Desc.TeqK, double(CVarUniHomeWaitAltitude.GetValueOnGameThread()), int32(StartCandidates.size()));
}

void UUniSubsystem::UpdateHomeStart(const FVector& /*ViewLoc*/)
{
	if (StartPhase == EStartPhase::None || StartPhase == EStartPhase::Done) { return; }
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn) { return; }
	const double Now = FPlatformTime::Seconds();
	const double Timeout = double(FMath::Max(CVarUniHomeTimeout.GetValueOnGameThread(), 1.0f));
	auto Place = [&](const FVector& Where)
	{
		// Upright on the planet (up = away from its centre), facing along the ground towards the star.
		const FVector Up = Where.GetSafeNormal();
		FVector Fwd = FVector::CrossProduct(Up, FVector::CrossProduct(FVector(0.0, 0.0, 1.0), Up)).GetSafeNormal();
		if (Fwd.IsNearlyZero()) { Fwd = FVector::CrossProduct(Up, FVector(1.0, 0.0, 0.0)).GetSafeNormal(); }
		const FRotator Rot = FRotationMatrix::MakeFromZX(Up, Fwd).Rotator();
		Pawn->SetActorLocationAndRotation(Where, Rot, false, nullptr, ETeleportType::TeleportPhysics);
		PC->SetControlRotation(FRotationMatrix::MakeFromXZ(Fwd, Up).Rotator());
	};
	if (!BodyFrame.bActive)
	{
		UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: home start abandoned: the world left the home planet's frame"));
		if (bPawnHeld) { HoldPawn(false); }
		StartPhase = EStartPhase::Done;
		return;
	}
	switch (StartPhase)
	{
	case EStartPhase::WaitPawn:
	{
		Place(StartSpawn);
		HoldPawn(true);
		StartPhase = EStartPhase::WaitGround;
		StartPhaseSince = Now;
		break;
	}
	case EStartPhase::WaitGround:
	{
		if (!bPawnHeld) { HoldPawn(true); }
		if (!Pawn->GetActorLocation().Equals(StartSpawn, 1.0)) { Place(StartSpawn); }   // a late possession or a game mode's move
		const FDetailedBody* D = DetailedBodies.Find(uint32(BodyFrame.Desc.PlanetSeed));
		AActor* Body = D ? D->Actor.Get() : nullptr;
		UUniBodyFactory* Fac = D ? D->Factory.Get() : nullptr;
		if (!FactoryFor(BodyFrame.Desc))
		{
			if (Now - StartPhaseSince > 5.0)
			{
				UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: home start: no body factory makes this planet (the UnifiedUniverseGame module with VoxelPlanets): the pawn stays held above it"));
				StartPhase = EStartPhase::Done;   // held: nothing to stand on
			}
			return;
		}
		const double R = BodyFrame.RadiusCm;
		auto Ground = [&](const UniCore::FVec3d& Dir)
		{
			UniScenes::FGroundSample G;
			if (!Body || !Fac) { return G; }
			const FUniGroundSample S = Fac->GroundAt(Body, FVector(Dir.X, Dir.Y, Dir.Z) * R);
			G.bValid = S.bValid;
			G.bDry = S.bDry;
			G.HeightM = S.HeightM;
			return G;
		};
		if (Body && Fac && Ground(UniCore::FVec3d{ StartUp.X, StartUp.Y, StartUp.Z }).bValid)
		{
			const int32 Chosen = UniScenes::PickHomeSpawn(StartCandidates, R, Ground);
			const UniCore::FVec3d Dir = Chosen >= 0 ? StartCandidates[size_t(Chosen)] : UniCore::FVec3d{ StartUp.X, StartUp.Y, StartUp.Z };
			StartUp = FVector(Dir.X, Dir.Y, Dir.Z);
			// The ground's own radius (sea level need not be the planet's radius), 3 m above it.
			const FUniGroundSample G = Fac->GroundAt(Body, StartUp * R);
			const double GroundR = G.GroundRadiusCm > 0.5 * R ? G.GroundRadiusCm : R + 100.0 * FMath::Max(G.HeightM, 0.0);
			StartSpawn = StartUp * (GroundR + 300.0);
			Place(StartSpawn);
			StartPhase = EStartPhase::WaitCollision;
			StartPhaseSince = Now;
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: home start: spawn candidate %d (%s ground %.0f m above sea level, %s from the centre); waiting for its collision"),
				Chosen, G.bDry ? TEXT("dry") : TEXT("wet"), G.HeightM, UTF8_TO_TCHAR(UniCore::FormatDistance(GroundR).c_str()));
		}
		else if (Now - StartPhaseSince > Timeout)
		{
			// Released here it would fall into the planet: it stays held (uu.Fly moves it).
			UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: home start: the planet's ground did not answer in %.0f s (uu.Planets; is it the terrestrial generator, uu.Planet.Surface 1?); the pawn stays held where it waits"), Timeout);
			StartPhase = EStartPhase::Done;
		}
		break;
	}
	case EStartPhase::WaitCollision:
	{
		if (!bPawnHeld) { HoldPawn(true); }
		FCollisionQueryParams Q(SCENE_QUERY_STAT(UniHomeStart), true, Pawn);
		FHitResult Hit;
		const FVector From = StartSpawn + StartUp * 5000.0;
		const FVector To = StartSpawn - StartUp * 30000.0;
		bool bHit = World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q);
		if (!bHit) { bHit = World->LineTraceSingleByChannel(Hit, From, To, ECC_WorldStatic, Q); }
		if (bHit)
		{
			double HalfHeight = 100.0;
			if (const ACharacter* Ch = Cast<ACharacter>(Pawn)) { if (const UCapsuleComponent* Cap = Ch->GetCapsuleComponent()) { HalfHeight = Cap->GetScaledCapsuleHalfHeight(); } }
			Place(Hit.ImpactPoint + StartUp * (HalfHeight + 10.0));
			HoldPawn(false);
			StartPhase = EStartPhase::Done;
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: home start: on the ground (%.1f s after the spawn point was chosen)"), Now - StartPhaseSince);
		}
		else if (Now - StartPhaseSince > Timeout)
		{
			UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: home start: no collision under the spawn point after %.0f s (traced on Visibility and WorldStatic); the pawn stays held there (uu.Fly moves it)"), Timeout);
			StartPhase = EStartPhase::Done;
		}
		break;
	}
	default:
		break;
	}
}

// ---------------------------------------------------------------------------------------------
// uu.Planets
// ---------------------------------------------------------------------------------------------

void UUniSubsystem::LogPlanets() const
{
	if (BodyFrame.bActive)
	{
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: in planet %08x's frame (star %08x #%d): radius %s, frame radius %s, %s%s, %.2f g, Teq %.0f K; the view %s from its centre"),
			uint32(BodyFrame.Desc.PlanetSeed), BodyFrame.StarSeed, BodyFrame.Ordinal, UTF8_TO_TCHAR(UniCore::FormatDistance(BodyFrame.RadiusCm).c_str()),
			UTF8_TO_TCHAR(UniCore::FormatDistance(BodyFrame.FrameRadiusCm).c_str()), AtmoName(BodyFrame.Desc.Atmosphere), BodyFrame.Desc.bOcean ? TEXT(", seas") : TEXT(""),
			BodyFrame.Desc.SurfaceGravity / UniCore::PlanetPropsConst::EarthG, BodyFrame.Desc.TeqK, UTF8_TO_TCHAR(UniCore::FormatDistance(GetViewLocation().Size()).c_str()));
	}
	else
	{
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: not in a planet's frame (uu.Planet.Frame %d)"), CVarUniPlanetFrame.GetValueOnGameThread());
	}
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: %d body factories; %d detailed bodies:"), BodyFactories.Num(), DetailedBodies.Num());
	for (const auto& KV : DetailedBodies)
	{
		const AActor* A = KV.Value.Actor.Get();
		UE_LOG(LogUnifiedUniverse, Log, TEXT("  %08x %s: %s%s"), KV.Key, A ? *A->GetName() : TEXT("(gone)"), KV.Value.bParked ? TEXT("parked") : TEXT("live"), KV.Value.bReady ? TEXT(", ready") : TEXT(""));
	}
	static const TCHAR* Phases[] = { TEXT("none"), TEXT("waiting for the pawn"), TEXT("waiting for the ground"), TEXT("waiting for collision"), TEXT("done") };
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: home start %s%s"), Phases[int32(StartPhase)], bPawnHeld ? TEXT(" (pawn held)") : TEXT(""));
}

static FAutoConsoleCommandWithWorldAndArgs GUniCmdPlanets(TEXT("uu.Planets"), TEXT("D-293: the planet's frame, the planets' detailed bodies and the home start."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (const UUniSubsystem* S = PlanetsSub(World)) { S->LogPlanets(); }
	}));
