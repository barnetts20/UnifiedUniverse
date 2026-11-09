#pragma once

// UniSubsystem.h
// UUniSubsystem: the one gameplay entry point (design: Spec: runtime API). Owns the universal
// camera, the streaming core (UniCore::FUniStreamer) with the GPU generation backend, and pushes
// the per-frame cell table to the renderer. Nothing outside the plugin touches GPU buffers or
// window state.
//
// Prototype scope (REVIEW_TONIGHT): camera, Local / Cruise motion modes (D-008), teleport,
// sensitivity, the scripted flythrough, stats. D-193: Pick, QueryResident, FindNearest, events
// (resident / evicted, visible / hidden for watched objects, promoted / demoted bodies) and
// GetKnownObject. Resolve and persistence are absent (M4).
//
// Console (any level, PIE or standalone):
//   uu.Enable [galaxy|web|flythrough]   start a universe at its demo camera
//   uu.Flythrough [seconds]             3 Gpc -> 1 m from a bright star (default 60 s)
//   uu.Fly [cm/s]                       toggle free flight: steer with the view, WASD/QE to
//                                       move, mouse wheel doubles / halves the speed
//   uu.Speed <cm/s>                     free-flight speed (any magnitude, 1 .. 1e32)
//   uu.Cruise <cm/s>                    move along the view direction (0: stop, back to Local)
//   uu.Teleport <solar|void>            jump the camera (coarse-to-fine rebuild)
//   uu.Sensitivity <dm>                 limiting-magnitude offset, -5..5 (plus the travel boost)
//   uu.TravelStart / uu.TravelFull <cm/s>  speeds where the travel boost starts / is full
//   uu.Exposure <mag>                   exposure bias in magnitudes (negative = brighter)
//   uu.Volume <0|1>                     the volume pass: unresolved glow, dust, background
//   uu.Stats                            log streaming and render statistics
//   uu.Perf                             log per-phase cost: averages and peaks since the last
//                                       uu.Perf, the worst frame's streamer breakdown, work counts (D-157)
//   uu.PerfHud <0|1>                    the same, live on screen
//   uu.PerfHitchMs <ms>                 log the breakdown of any frame whose universe work on the
//                                       game thread exceeds this (0: off; default 50)
//   uu.VolumeMaxSteps / uu.VolumeStepRatio / uu.VolumeMaxSubsamples   volume march quality (D-157)
//   uu.Sprite.<handle> <value>          sprite style (D-156): CorePx, Saturation, HaloGain,
//                                       HaloGrowth, HaloMax
//   uu.TimeScale <x>                    universe clock rate (1 = real time; orbits run on it)
//   uu.Time <seconds>                   set the universe clock
//   uu.Systems                          log active star systems and their planets
//   uu.Clusters                         log the cluster tuning and the resident clusters (D-151)
//   uu.GotoCluster [rich]               jump to 3 influence radii from the nearest resident cluster
//   uu.GotoGroup [rich]                 the same for the nearest galaxy group (D-174)
//                                       (rich: M < -25.5), facing it
//   uu.Cluster.<handle> <value>         cluster tuning (Enable, Fraction, MStar, Alpha, BrightMag,
//                                       FaintMag, BiasPower, NodeWeight, RadiusScale, CoreMin,
//                                       CoreMax, MaxClumps, ClumpWeight); applied on the next uu.Enable
//   uu.GotoSystem [planet]              jump into the nearest star system with a gas giant
//                                       (planet: to 30 radii from the giant, to watch it promote)
//   uu.LightRig <0|1>                   the directional light from the active star (M3 placeholder)
//   uu.SkyLight <0|1>                   a sky light lit by the backdrop (D-186; uu.SkyLightSize, Interval,
//                                       Blend, Intensity)
//   uu.VolumeTemporal <0|1>             temporal slicing of the glow's march (D-184)
//   uu.Sprite.HaloChroma / Floor        halo colour on bright stars (D-185); how far a sprite's quad
//                                       reaches before its light is cut (D-188)
//   uu.Pick [x y]                       log the object under the screen centre (or pixel x, y) (D-193)
//   uu.Query <radius pc> [archetype]    log the resident objects within a radius of the camera
//   uu.Nearest <archetype>              log the nearest resident object of an archetype
//   uu.Events [archetype ...|off]       log resident / evicted events for archetypes (counts per second)
//   uu.Disable

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "UniCoordTypes.h"
// Complete types, not forward declarations: UHT generates this class's destructor in
// UniSubsystem.gen.cpp, which sees only this header, and TUniquePtr must be able to delete them.
#include "UniStreamer.h"
#include "UniVolume.h"
#include "UniGalaxyMap.h"
#include "UniSystem.h"
#include "UniPromotion.h"
#include "UniDemoScenes.h"
#include "UniTravel.h"
#include "UniProfile.h"
#include "UniWebTexture.h"
#include "UniSky.h"
#include "UniMotes.h"
#include "UniQuery.h"   // D-193
#include "UniApiTypes.h"
#include "UniSubsystem.generated.h"

class UTextureCube;
class ASkyLight;
struct FUniFrameData;

/** D-157: game-thread phases of the universe's frame (plain C++, not reflected). */
namespace UniPerf
{
	enum EPhase : int32 { PerfTick, PerfStreamer, PerfPromotion, PerfParallax, PerfLightRig, PerfPushFrame, PerfSystems, PerfVolumeBuild, PerfCount };
	inline constexpr int32 StreamPhaseCount = int32(UniCore::EStreamPhase::Count);
}

UENUM(BlueprintType)
enum class EUniMotionMode : uint8
{
	Local,    // the pawn moves in the main scene; camera = local anchor + view location
	Cruise    // the pawn is pinned; velocity integrates into the universal coordinate
};

/** Broadcast when the Local-mode frame is re-centred (D-149): every actor in the UE world that is
 *  not placed by the universe should move by WorldShift (Simulated parallax actors and the pawn
 *  already have). Universal positions do not change. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUniOnLocalRecentre, FVector, WorldShift);

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
	/** The volume pass (Docs/VOLUMETRICS.md): unresolved light, dust, background. */
	void SetVolumeEnabled(bool bOn) { bVolume = bOn; }
	bool IsVolumeEnabled() const { return bVolume; }
	void LogStats() const;
	/** D-157: per-phase cost, averaged and peak since the last call (which resets the peaks). */
	void LogPerf();

	/** Free flight for testing: velocity follows the view every frame, WASD/QE give the
	 *  direction, the mouse wheel doubles or halves the speed. Works at any speed: the universal
	 *  camera integrates the velocity directly (Cruise), whatever the D-008 threshold says. */
	void SetFreeFly(bool bOn, double CmPerSecond = -1.0);
	bool IsFreeFlying() const { return bFreeFly; }
	void SetFlySpeed(double CmPerSecond) { FlySpeed = FMath::Clamp(CmPerSecond, 1.0, 1.0e32); }
	double GetFlySpeed() const { return FlySpeed; }
	/** uu.Cruise: a straight-line velocity that stays in Cruise until set to 0. */
	void SetCruise(const FVector& CmPerSecond);

	/** Universe clock (B-002): seconds since the epoch; orbits are evaluated at it. */
	void SetTimeScale(double Scale) { TimeScale = Scale; }
	double GetTimeScale() const { return TimeScale; }
	void SetUniverseTime(double Seconds) { UniverseTime = Seconds; }
	double GetUniverseTime() const { return UniverseTime; }
	void LogSystems() const;
	/** D-151: cluster tuning in effect, and the resident clusters. */
	void LogClusters() const;
	/** Jump to 3 influence radii from the nearest resident cluster (bRich: M < -25.5). */
	bool GoToCluster(bool bRich);
	bool GoToGroup(bool bRich);   // D-174
	bool GoToHost(UniCore::u32 Arch, double BrighterThan, const TCHAR* What);
	/** Resolve the nearest star (within 100 pc) whose system has a gas giant, and jump into it
	 *  in free flight, facing the star. */
	bool GoToSystem(bool bToPlanet = false);

	/** M3 (D-146): actors placed by the parallax rule register here and are placed every frame
	 *  right after the universal camera moves. */
	void RegisterParallax(class UUniParallaxComponent* C);
	void UnregisterParallax(class UUniParallaxComponent* C);

	static UUniSubsystem* FindForConsole();

	/** D-149: the Local-mode frame moved by WorldShift (integer cm); universal positions unchanged. */
	UPROPERTY(BlueprintAssignable, Category = "Universe")
	FUniOnLocalRecentre OnLocalRecentre;

	/** The universal coordinate of the UE world origin in Local mode. */
	UFUNCTION(BlueprintPure, Category = "Universe")
	FUniCoord GetLocalAnchor() const { return LocalAnchor; }

	// ---- Runtime API (D-193) --------------------------------------------------------------
	// Lookups run on the GPU over what is resident and come back a few frames later; at most one
	// pick and one query are in each frame (the rest queue). Every result names objects by
	// FUniObjectId. Events are batched once per frame, on the game thread.

	/** The object under a screen position (game viewport pixels, as GetMousePosition gives):
	 *  the brightest sprite whose light reaches it, weighted by nearness (within RadiusPx at
	 *  least). Returns the request id; OnResult is called with bFound false if nothing is there. */
	UFUNCTION(BlueprintCallable, Category = "Universe|Query")
	int32 Pick(FVector2D ScreenPosition, FUniOnPicked OnResult, float RadiusPx = 4.0f);
	int32 PickNative(FVector2D ScreenPosition, TFunction<void(bool, const FUniObjectInfo&)> OnResult, float RadiusPx = 4.0f);

	/** Resident objects within RadiusCm of Centre (exact distances), nearest first. Only what is
	 *  loaded around the camera: a query far from it finds what the windows hold there. */
	UFUNCTION(BlueprintCallable, Category = "Universe|Query")
	int32 QueryResident(const FUniCoord& Centre, double RadiusCm, const FUniQueryFilter& Filter, FUniOnQueryResult OnResult);
	int32 QueryResidentNative(const FUniCoord& Centre, double RadiusCm, const FUniQueryFilter& Filter, TFunction<void(const TArray<FUniObjectInfo>&, int32)> OnResult);

	/** The nearest resident object of an archetype to From (bFound false if none is resident). */
	UFUNCTION(BlueprintCallable, Category = "Universe|Query")
	int32 FindNearest(int32 ArchetypeId, const FUniCoord& From, FUniOnPicked OnResult);
	int32 FindNearestNative(int32 ArchetypeId, const FUniCoord& From, TFunction<void(bool, const FUniObjectInfo&)> OnResult);

	/** The archetype id of a name in the current universe ("Star", "Galaxy", ...; -1: none). */
	UFUNCTION(BlueprintPure, Category = "Universe|Query")
	int32 GetArchetypeId(FName Name) const;

	/** Immediately, an object the CPU knows (resident galaxies, clusters, groups and other
	 *  parents; anchors). False for stars and anything not resident: use a query. */
	UFUNCTION(BlueprintCallable, Category = "Universe|Query")
	bool GetKnownObject(const FUniObjectId& Id, FUniObjectInfo& OutObject) const;

	/** Which archetypes raise OnObjectResident / OnObjectEvicted (ids 0..31 of objects the CPU
	 *  knows: galaxies, clusters, groups and anchors; empty: none, the default). Objects already
	 *  resident are reported at once. */
	UFUNCTION(BlueprintCallable, Category = "Universe|Events")
	void SetResidentEventArchetypes(const TArray<int32>& ArchetypeIds);

	/** OnObjectVisible / OnObjectHidden for this object (one the CPU knows) from now on. */
	UFUNCTION(BlueprintCallable, Category = "Universe|Events")
	void WatchObject(const FUniObjectId& Id);
	UFUNCTION(BlueprintCallable, Category = "Universe|Events")
	void UnwatchObject(const FUniObjectId& Id);

	UPROPERTY(BlueprintAssignable, Category = "Universe|Events")
	FUniOnObjectEvent OnObjectResident;
	UPROPERTY(BlueprintAssignable, Category = "Universe|Events")
	FUniOnObjectIdEvent OnObjectEvicted;
	/** A watched object came into view and bright enough to see (as a sprite or as glow). */
	UPROPERTY(BlueprintAssignable, Category = "Universe|Events")
	FUniOnObjectEvent OnObjectVisible;
	UPROPERTY(BlueprintAssignable, Category = "Universe|Events")
	FUniOnObjectIdEvent OnObjectHidden;
	/** A star or planet became an actor (M3) / went back to a sprite. */
	UPROPERTY(BlueprintAssignable, Category = "Universe|Events")
	FUniOnObjectEvent OnBodyPromoted;
	UPROPERTY(BlueprintAssignable, Category = "Universe|Events")
	FUniOnObjectEvent OnBodyDemoted;

	/** uu.Events: log resident / evicted counts (console testing). */
	void SetEventLogging(bool bOn) { bLogEvents = bOn; }

private:
	void PushFrame();
	void SetupWebTexture(UniCore::FUniverseDef& Def);   // D-164
	void SetupGalaxyTextures(UniCore::FUniverseDef& Def);   // D-179
	FVector GetViewLocation(FRotator* OutRotation = nullptr) const;

	bool bEnabled = false;
	FString SceneName;
	/** D-164: the web texture of the current universe (bound to the CPU sampler; outlives the
	 *  generator and the streamer's integral jobs, so it is released after them). */
	TUniquePtr<UniCore::FWebTexture> WebTexture;
	/** D-179: the galaxy textures (warp_fine, warp_large, galaxy_detail), bound to the CPU sampler
	 *  like the web texture and released with it. */
	TUniquePtr<UniCore::FWebTexture> GalaxyTextures[3];
	TUniquePtr<UniCore::FUniverseModel> Model;
	TUniquePtr<UniCore::FRefGenerator> Generator;
	TUniquePtr<UniCore::IGenBackend> Backend;
	TUniquePtr<UniCore::FUniStreamer> Streamer;
	TUniquePtr<UniCore::FVolumeBuilder> VolumeBuilder;
	bool bVolume = true;
	uint32 BackgroundVersion = 0;
	UniCore::FCoord BackgroundAnchor;
	TArray<FVector4f> BackgroundLutLast;
	FVector3f SkyWebLast = FVector3f::ZeroVector;   // D-189: the sky-web settings the background map was marched with
	uint32 BgDirtyCounts[4] = { 0u, 0u, 0u, 0u };   // D-209: background map re-marches since the last uu.Perf: moved, LUT layout, its LUT, sky web
	uint32 BgPerPixelAtPerf = 0;                    // D-209: the renderer's per-pixel background count at the last uu.Perf
	uint64 PerfFramesAtBg = 0;                      // D-209: PerfFrames at the last uu.Perf

	// Travel sensitivity (D-154): the owner's sensitivity plus a speed-driven boost
	UniCore::FTravelSensitivity Warp;   // D-187: the same curve, 0..1, for the warp effects
	UniCore::FVolumeAutoBudget VolumeBudget;   // D-200: the glow's pixel budget follows the GPU frame time
	// D-212: the camera's own galaxy's map: the request the GPU bakes (uu.GalaxyMap).
	uint32 GalaxyMapEpoch = 0, GalaxyMapSerial = 0, GalaxyMapSize = 0;
	UniCore::FObjectId GalaxyMapId;
	UniCore::FFieldParamsGPU GalaxyMapParams[2] = {};
	bool bGalaxyMapDust = false;
	double GalaxyMapStart = 0.0, GalaxyMapBakeMs = 0.0;
	uint64 GalaxyMapBakes = 0;
	UniCore::FMoteScale MoteScale;      // D-192: the warp motes' lattice scale (follows the speed)
	float EffectiveDeltaMag = 0.0f;
	/** D-205: magnitudes everything is brightened by at speed (uu.TravelExposure x the travel curve). */
	float TravelExposureMag = 0.0f;
	FUniCoord PrevCamera;
	bool bCameraJumped = true;
	FVector3d CameraStepCm = FVector3d::ZeroVector;   // D-184: camera move this frame (cm), for the volume's reprojection
	bool bCameraStepValid = false;

	// Instrumentation (D-157). Game-thread phases of the universe's frame, in ms.
	static const TCHAR* PerfPhaseName(int32 Phase);
	double PerfMs[UniPerf::PerfCount] = {};
	double PerfAvg[UniPerf::PerfCount] = {};
	double PerfPeak[UniPerf::PerfCount] = {};
	double StreamAvg[UniPerf::StreamPhaseCount] = {};
	UniCore::FStreamProfile StreamLast;        // the last frame's streamer breakdown and counts
	UniCore::FStreamProfile StreamWorst;       // the worst frame's since the last uu.Perf
	double StreamWorstTickMs = 0.0;
	uint64 PerfFrames = 0;
	uint64 PerfHitches = 0;
	void RecordPerf();
	FString StreamBreakdown(const UniCore::FStreamProfile& P) const;
	void DrawPerfHud() const;

	// Clusters (D-151): the tuning the current universe was built with
	UniScenes::FClusterTuning ClusterTuning;

	// Star systems (D-142..D-145)
	TUniquePtr<UniCore::FSystemManager> Systems;
	uint32 SystemArchetypeMask = 0;
	uint32 DiscArchetypeMask = 0;   // D-183: archetypes emitting through the galaxy field (inclined sprites)
	uint32 FixedDiscSeeds[4] = { 0, 0, 0, 0 };   // D-183: seeds of fixed-orientation anchors (up to four)
	double UniverseTime = 0.0;
	double TimeScale = 1.0;
	/** The last few frames' cell tables (keys, generations, planets' dynamic cells): GPU
	 *  readbacks (activations, picks, queries) name objects by (cell index, slot) in them. */
	TMap<uint64, UniCore::FFrameCells> FrameCellTables;
	uint64 ActivationsUnmatched = 0;

	// Promotion and real space (M3, D-146, D-147)
	UniCore::FPromotionManager Promoter;
	TMap<uint32, TWeakObjectPtr<class AUniBodyActor>> Bodies;
	TArray<TWeakObjectPtr<class UUniParallaxComponent>> ParallaxComponents;
	TWeakObjectPtr<class ADirectionalLight> LightRig;
	uint64 Recentres = 0;
	float SavedPawnMaxSpeed = -1.0f;   // the pawn's own movement speed, restored when free flight ends
	void SetPawnMovementSuppressed(bool bSuppress);
	/** D-149: re-centre the Local frame if the pawn is more than 1,000 km from the anchor. */
	void RecentreLocalIfNeeded(FVector& InOutViewLoc);
	void UpdatePromotion();
	void UpdateParallax(const FVector& ViewLoc);
	void UpdateLightRig();
	void ClearPromotion();

	// D-186: the sky light from the backdrop. A small cube marched on a worker thread from a copy
	// of the frame's volume scene every uu.SkyLightInterval seconds, uploaded as a transient
	// cubemap and blended in over uu.SkyLightBlend seconds on a movable sky light.
	TWeakObjectPtr<ASkyLight> SkyLightActor;
	UPROPERTY(Transient)
	TObjectPtr<UTextureCube> SkyCubePrev;
	UPROPERTY(Transient)
	TObjectPtr<UTextureCube> SkyCubeCur;
	TFuture<UniCore::FSkyCube> SkyJob;
	bool bSkyJob = false;
	bool bSkyBlending = false;
	double SkyLastStart = -1.0e9;
	double SkyBlendStart = 0.0;
	void StartSkyJob(const UniCore::FVolumeScene& Sc);
	void UpdateSkyLight();
	void ClearSkyLight();

	FUniCoord Camera;
	FUniCoord LocalAnchor;
	FVector3f CameraFraction = FVector3f::ZeroVector;
	FVector Velocity = FVector::ZeroVector;
	EUniMotionMode MotionMode = EUniMotionMode::Local;
	float DeltaMag = 0.0f;
	float ExposureBias = 0.0f;
	uint64 FrameCounter = 0;

	// Runtime API (D-193)
	struct FApiPick
	{
		int32 Id = 0;
		FVector2f CursorNdc = FVector2f::ZeroVector;
		float MinPx = 4.0f;
		FUniOnPicked Bp;
		TFunction<void(bool, const FUniObjectInfo&)> Native;
		uint64 SentFrame = 0;
	};
	struct FApiQuery
	{
		int32 Id = 0;
		uint32 Mode = 0u;   // UNI_QUERY_MODE_SPHERE / _NEAREST
		FUniCoord Centre;
		double RadiusCm = 0.0;
		uint32 ArchMask = 0u;
		float MaxAbsMag = 99.0f;
		int32 MaxResults = 256;
		FUniOnQueryResult Bp;
		FUniOnPicked BpNearest;
		TFunction<void(const TArray<FUniObjectInfo>&, int32)> Native;
		TFunction<void(bool, const FUniObjectInfo&)> NativeNearest;
		uint64 SentFrame = 0;
	};
	TArray<FApiPick> PickQueue, PicksInFlight;
	TArray<FApiQuery> QueryQueue, QueriesInFlight;
	int32 NextApiRequestId = 1;
	UniCore::FResidentEvents ResidentEvents;
	UniCore::FVisibilityWatch VisibilityWatch;
	TMap<uint32, FUniObjectInfo> PromotedReported;   // body seed -> as reported promoted
	bool bLogEvents = false;
	uint64 LoggedResident = 0, LoggedEvicted = 0;
	double LastEventLog = 0.0;
	uint32 EventArchMask = 0u;   // wanted resident-event archetypes; applied on the next tick
	int32 QueuePick(FVector2D ScreenPosition, float RadiusPx, FApiPick&& P);
	int32 QueueQuery(FApiQuery&& Q);
	void ProcessApiReadbacks();
	void FillApiRequests(FUniFrameData& F);
	void FailApiRequests();
	void UpdateApiEvents(const FRotator& ViewRot);
	void DeliverQuery(FApiQuery& Q, TArray<FUniObjectInfo>&& Objects, int32 Total);
	FUniObjectInfo MakeInfo(const UniCore::FObjectInfo& I) const;
	FUniObjectInfo BodyInfo(const UniCore::FPromotedBody& B) const;
	bool BuildApiView(const FRotator& ViewRot, UniHlsl::FUniView& OutV, FVector2D* OutViewportPx = nullptr) const;

	// Free flight and latched cruise (test controls)
	bool bFreeFly = false;
	bool bCruiseLatched = false;
	double FlySpeed = 1.0e5;   // 1 km/s
	void DrawHud(double Speed) const;

	// Flythrough
	bool bFlying = false;
	double FlyElapsed = 0.0, FlyDuration = 60.0;
	FUniCoord FlyTarget;
	double FlyTargetRadius = 0.0;
	FVector FlyDirection = FVector(0.35, 0.25, 0.9).GetSafeNormal();
};
