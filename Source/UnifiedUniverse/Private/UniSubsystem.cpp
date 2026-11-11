#include "UniSubsystem.h"

#include "UnifiedUniverseModule.h"
#include "UniDemoScenes.h"
#include "UniRenderer.h"
#include "UniRenderHooks.h"
#include "UniStreamer.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
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
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/TextureCube.h"
#include "InputCoreTypes.h"
#include "UniScaleLadder.h"
#include "UniCoordLibrary.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/PlatformTime.h"
#include "DynamicRHI.h"   // D-200: RHIGetGPUFrameCycles (the volume auto budget)
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"

#include <algorithm>
#include <string>

using namespace UniPerf;   // D-157 phase names

// D-151: cluster tuning handles (UniScenes::FClusterTuning). They are a content definition, so they
// apply when a universe is enabled (uu.Enable), not live. Defaults are the realistic distribution.
namespace UniClusterCVars
{
	const UniScenes::FClusterTuning Defaults;
	TAutoConsoleVariable<int32> Enable(TEXT("uu.Cluster.Enable"), 1, TEXT("Galaxy clusters in the web scenes (0: field galaxies only, the pre-D-151 universe)."));
	TAutoConsoleVariable<float> Fraction(TEXT("uu.Cluster.Fraction"), float(Defaults.Fraction), TEXT("Fraction of all galaxies that are cluster members (realistic 0.10-0.15)."));
	TAutoConsoleVariable<float> MStar(TEXT("uu.Cluster.MStar"), float(Defaults.MStar), TEXT("Cluster luminosity function: characteristic magnitude (brighter: more rich clusters)."));
	TAutoConsoleVariable<float> Alpha(TEXT("uu.Cluster.Alpha"), float(Defaults.Alpha), TEXT("Cluster luminosity function: slope (more negative: more poor clusters)."));
	TAutoConsoleVariable<float> BrightMag(TEXT("uu.Cluster.BrightMag"), float(Defaults.BrightMag), TEXT("Magnitude of the richest clusters."));
	TAutoConsoleVariable<float> FaintMag(TEXT("uu.Cluster.FaintMag"), float(Defaults.FaintMag), TEXT("Magnitude of the poorest clusters. Fainter admits groups, at a residency cost (D-152)."));
	TAutoConsoleVariable<float> BiasPower(TEXT("uu.Cluster.BiasPower"), float(Defaults.BiasPower), TEXT("Power on the web density for cluster placement (higher: more on filaments)."));
	TAutoConsoleVariable<float> NodeWeight(TEXT("uu.Cluster.NodeWeight"), float(Defaults.NodeWeight), TEXT("Weight of web nodes (where filaments meet) for cluster placement."));
	TAutoConsoleVariable<float> RadiusScale(TEXT("uu.Cluster.RadiusScale"), float(Defaults.RadiusScale), TEXT("Multiplies cluster radii."));
	TAutoConsoleVariable<float> CoreMin(TEXT("uu.Cluster.CoreMin"), float(Defaults.CoreMin), TEXT("Smallest core radius, as a fraction of the influence radius."));
	TAutoConsoleVariable<float> CoreMax(TEXT("uu.Cluster.CoreMax"), float(Defaults.CoreMax), TEXT("Largest core radius, as a fraction of the influence radius."));
	TAutoConsoleVariable<int32> MaxClumps(TEXT("uu.Cluster.MaxClumps"), Defaults.MaxClumps, TEXT("Most sub-clumps per cluster (0..4)."));
	TAutoConsoleVariable<float> ClumpWeight(TEXT("uu.Cluster.ClumpWeight"), float(Defaults.ClumpWeight), TEXT("Largest sub-clump weight."));
	TAutoConsoleVariable<float> EarlyCore(TEXT("uu.Cluster.EarlyCore"), float(Defaults.EarlyCore), TEXT("D-172: share of members that are early types (elliptical / lenticular) at a cluster's centre (0: off, members as before D-172)."));
	TAutoConsoleVariable<float> EarlyOuter(TEXT("uu.Cluster.EarlyOuter"), float(Defaults.EarlyOuter), TEXT("D-172: early-type share far from the centre (the field's, ~0.2)."));
	TAutoConsoleVariable<float> EarlyScale(TEXT("uu.Cluster.EarlyScale"), float(Defaults.EarlyScale), TEXT("D-172: radius over which the early-type share falls from EarlyCore toward EarlyOuter, in influence radii (~0.25 = the virial radius)."));
	TAutoConsoleVariable<int32> Groups(TEXT("uu.Group.Enable"), 1, TEXT("D-174: galaxy groups from the web's node list (each places dwarfs; 0: none). Needs the node list."));
	TAutoConsoleVariable<float> GroupDwarfs(TEXT("uu.Group.Dwarfs"), float(Defaults.GroupDwarfs), TEXT("D-174: dwarfs (M -16..-12) in the median-mass group; each group's count goes with its node's mass."));
	TAutoConsoleVariable<float> MemberKeep(TEXT("uu.Cluster.MemberKeep"), float(Defaults.MemberKeep), TEXT("D-197: share of cluster members and group dwarfs that are generated and drawn (the rest of their light is the cluster's glow). 1: all, as before D-197; D-198 default 0.5. Applies at uu.Enable."));
	TAutoConsoleVariable<float> StarKeep(TEXT("uu.Star.Keep"), float(Defaults.StarKeep), TEXT("D-197: share of every galaxy's stars that are generated and drawn (the rest of their light is the galaxy's glow). 1: all, as before D-197; D-198 0.5; D-205 0.35. Applies at uu.Enable."));
	TAutoConsoleVariable<float> DustScale(TEXT("uu.Galaxy.DustScale"), float(Defaults.DustScale), TEXT("D-198: galaxies' dust optical depth x this (darker lanes; 1: the D-132 dust). Applies at uu.Enable."));
	TAutoConsoleVariable<float> DustLanes(TEXT("uu.Galaxy.DustLanes"), float(Defaults.DustLanes), TEXT("D-198: contrast of textured galaxies' dust lanes (D-180: 2.2). Applies at uu.Enable."));
	TAutoConsoleVariable<float> EarlyColour(TEXT("uu.Cluster.EarlyColour"), float(Defaults.EarlyColourShift), TEXT("D-172: early types' colour shift in log10 K (negative: redder)."));

	UniScenes::FClusterTuning Read()
	{
		UniScenes::FClusterTuning T;
		T.bEnabled = Enable.GetValueOnGameThread() != 0;
		T.Fraction = FMath::Clamp(double(Fraction.GetValueOnGameThread()), 0.0, 0.95);
		T.MStar = MStar.GetValueOnGameThread();
		T.Alpha = Alpha.GetValueOnGameThread();
		T.BrightMag = BrightMag.GetValueOnGameThread();
		T.FaintMag = FMath::Max(double(FaintMag.GetValueOnGameThread()), T.BrightMag + 0.5);
		T.BiasPower = FMath::Max(double(BiasPower.GetValueOnGameThread()), 0.0);
		T.NodeWeight = FMath::Max(double(NodeWeight.GetValueOnGameThread()), 0.0);
		T.RadiusScale = FMath::Clamp(double(RadiusScale.GetValueOnGameThread()), 0.1, 10.0);
		T.CoreMin = FMath::Clamp(double(CoreMin.GetValueOnGameThread()), 0.01, 0.5);
		T.CoreMax = FMath::Clamp(double(CoreMax.GetValueOnGameThread()), T.CoreMin, 0.5);
		T.MaxClumps = FMath::Clamp(MaxClumps.GetValueOnGameThread(), 0, int32(UniHlsl::UNI_CLUSTER_MAX_CLUMPS));
		T.ClumpWeight = FMath::Max(double(ClumpWeight.GetValueOnGameThread()), 0.0);
		T.EarlyCore = FMath::Clamp(double(EarlyCore.GetValueOnGameThread()), 0.0, 1.0);
		T.EarlyOuter = FMath::Clamp(double(EarlyOuter.GetValueOnGameThread()), 0.0, 1.0);
		T.EarlyScale = FMath::Clamp(double(EarlyScale.GetValueOnGameThread()), 0.01, 2.0);
		T.EarlyColourShift = FMath::Clamp(double(EarlyColour.GetValueOnGameThread()), -0.5, 0.5);
		T.bGroups = Groups.GetValueOnGameThread() != 0;
		T.GroupDwarfs = FMath::Clamp(double(GroupDwarfs.GetValueOnGameThread()), 0.0, 1000.0);
		T.MemberKeep = FMath::Clamp(double(MemberKeep.GetValueOnGameThread()), 0.05, 1.0);   // D-197
		T.StarKeep = FMath::Clamp(double(StarKeep.GetValueOnGameThread()), 0.05, 1.0);
		T.DustScale = FMath::Clamp(double(DustScale.GetValueOnGameThread()), 0.0, 10.0);   // D-198
		T.DustLanes = FMath::Clamp(double(DustLanes.GetValueOnGameThread()), 0.0, 10.0);
		return T;
	}
}

// D-154: travel sensitivity. Speeds in cm/s (100 kpc/s = 3.09e23, 10 Mpc/s = 3.09e25).
static TAutoConsoleVariable<float> CVarUniTravelStart(TEXT("uu.TravelStart"), 3.0857e23f, TEXT("Speed (cm/s) where travel sensitivity starts (default 100 kpc/s)."));
static TAutoConsoleVariable<int32> CVarUniSubmitDefer(TEXT("uu.SubmitDeferPerLevel"), 48, TEXT("D-173: entering cells not needed within the latency (plus a margin) are built at most this many per level per frame; -1: no limit (before D-173). Used only with uu.StreamBuildBudget 0."));
// D-203: cell glow, a fixed build budget and windows that keep their rest size at speed.
static TAutoConsoleVariable<float> CVarUniTravelExposure(TEXT("uu.TravelExposure"), 1.5f, TEXT("D-205: magnitudes everything is brightened by at full travel speed (render only: sprites, glow and background; windows and what is generated unchanged), lerped by the travel curve (100 kpc/s to 10 Mpc/s). Brings back the visibility travel sensitivity gave nearby clusters at speed without widening windows. 0: off."));
static TAutoConsoleVariable<int32> CVarUniGenOneTrip(TEXT("uu.GenOneTrip"), 1, TEXT("D-207: cells whose objects never come back (stars) are generated in one GPU round trip (probe, allocation and place in one graph) instead of two (~6 -> ~3 frames). 0: two trips for every cell."));
static TAutoConsoleVariable<int32> CVarUniGalaxyMap(TEXT("uu.GalaxyMap"), 1, TEXT("D-212: the galaxy the camera is in is marched from its baked 2D map (in-plane light, dust, disc centre, arm part; baked once per galaxy on the GPU) with the vertical profile, bulge, warp and detail read per sample. 0: its field at every step (A/B). D-214: placement always reads the maps (a galaxy's stars come from its map); this toggles the volume only."));
static TAutoConsoleVariable<int32> CVarUniGalaxyMapSize(TEXT("uu.GalaxyMap.Size"), 1024, TEXT("D-212: the galaxy map's texels a side (a power of two, 256..4096): 1024 ~11 MB with mips, baked in one frame (p95 1% against the field; the owner sees no difference from 2048), 2048 ~43 MB (0.3%). Changing it rebakes."));
static TAutoConsoleVariable<int32> CVarUniGalaxyMapBudget(TEXT("uu.GalaxyMap.Budget"), 1048576, TEXT("D-212: texels of the galaxy maps the GPU bakes a frame (rows of a map): 1M bakes a 1024^2 map in one frame, 2048^2 in 4. The glow uses the field until the camera's galaxy's map is done; D-214: a galaxy's stars wait for theirs."));
static TAutoConsoleVariable<int32> CVarUniGalaxyMapSlots(TEXT("uu.GalaxyMap.Slots"), 4, TEXT("D-214: galaxy maps kept on the GPU at once (1..16; ~11 MB each at 1024^2): every galaxy stars are placed in, and the camera's own. A galaxy beyond them waits (glow) until a slot frees (unused for 64 frames). Changing it rebakes."));
static TAutoConsoleVariable<int32> CVarUniGenFrameCandidates(TEXT("uu.GenFrameCandidates"), 1000000, TEXT("D-214: the most candidate positions a frame's one-trip generation batch evaluates up front (a galaxy star takes ~3, a dense core cell thousands); cells past it wait a frame as glow, the most needed first. The hard cap on a frame's generation work. 0: no cap."));
static TAutoConsoleVariable<int32> CVarUniGenViewPriority(TEXT("uu.GenViewPriority"), 1, TEXT("D-214: cells in view are built before cells out of view (after the cells finer levels wait on). 0: by distance alone (D-203)."));
static TAutoConsoleVariable<int32> CVarUniCellGlow(TEXT("uu.CellGlow"), 1, TEXT("D-203: a cell still loading is drawn as glow (the volume adds its children's expected light from the field) and its sprites fade in once it is Ready, instead of a hole that pops. 0: before D-203."));
static TAutoConsoleVariable<int32> CVarUniStreamBuildBudget(TEXT("uu.StreamBuildBudget"), 96, TEXT("D-203: at most this many cells built per frame over every level, those needed soonest first; the rest wait as glow. 0: D-173's per-level rule (uu.SubmitDeferPerLevel)."));
static TAutoConsoleVariable<float> CVarUniCellFadeFrames(TEXT("uu.CellFadeFrames"), 8.0f, TEXT("D-203: frames over which a cell's sprites fade in (and its glow out) once it is Ready."));
static TAutoConsoleVariable<float> CVarUniTravelFull(TEXT("uu.TravelFull"), 3.0857e25f, TEXT("Speed (cm/s) where travel sensitivity is full (default 10 Mpc/s)."));

// D-157: instrumentation and volume march quality.
static TAutoConsoleVariable<int32> CVarUniPerfHud(TEXT("uu.PerfHud"), 0, TEXT("Show the universe's per-phase cost on screen (D-157)."));
static TAutoConsoleVariable<float> CVarUniPerfHitchMs(TEXT("uu.PerfHitchMs"), 50.0f, TEXT("Log the breakdown of any frame whose universe work on the game thread exceeds this many ms (0: off)."));
static TAutoConsoleVariable<int32> CVarUniVolumeMaxSteps(TEXT("uu.VolumeMaxSteps"), 512, TEXT("Volume march step budget per ray (fewer: faster, a long ray may stop short; uu.VolumeDebug 1 shows truncation in magenta)."));
static TAutoConsoleVariable<float> CVarUniVolumeStepRatio(TEXT("uu.VolumeStepRatio"), 0.32f, TEXT("Volume march step as a fraction of the distance along the ray, before each field's own step limit (larger: faster, coarser). D-195: 0.08 -> 0.24; D-199: 0.32."));
static TAutoConsoleVariable<float> CVarUniVolumeWebTravelHint(TEXT("uu.VolumeWebTravelHint"), 2.0f, TEXT("D-202: the background web's step hints x this at full travel speed (10 Mpc/s), from 1 at 100 kpc/s (the travel curve): fewer steps for the background marched per pixel while travelling. The direction map is always marched at 1. 1: no change."));
static TAutoConsoleVariable<float> CVarUniVolumeTargetFps(TEXT("uu.VolumeTargetFps"), 60.0f, TEXT("D-200: the glow's march resolution follows the GPU frame time to hold this frame rate: down a level (~8% fewer pixels a side) when the GPU is 3% over it, up when 15% under; at most once per uu.VolumeAutoHold frames, 7 levels down to 0.35 of uu.VolumeMaxMarchPixels. 0: off (the fixed budget)."));
static TAutoConsoleVariable<int32> CVarUniVolumeAutoHold(TEXT("uu.VolumeAutoHold"), 20, TEXT("D-200: frames between the volume auto budget's level changes (each change restarts the glow's temporal history)."));
static TAutoConsoleVariable<float> CVarUniVolumeWebStepRatio(TEXT("uu.VolumeWebStepRatio"), 0.4f, TEXT("D-199: log step ratio of a textured web carrier (the background, marched per pixel while travelling fast). D-164: 0.2; its texture's hint bounds the step past ~0.4."));
static TAutoConsoleVariable<float> CVarUniVolumeHintScale(TEXT("uu.VolumeHintScale"), 3.0f, TEXT("D-195 (2), D-198 (3): the own step limit (step hint) of every volume carrier the camera is inside (its galaxy, cluster) x this. 1: the fields' limits as written; larger: fewer steps along the disc and arms around the camera."));
static TAutoConsoleVariable<float> CVarUniVolumeStartPc(TEXT("uu.VolumeStartPc"), 10.0f, TEXT("D-195: local volume carriers start marching this far from the camera, in pc (the D-129 default was 1; the log march spends steps on every decade)."));
static TAutoConsoleVariable<int32> CVarUniVolumePrefilter(TEXT("uu.VolumePrefilter"), 1, TEXT("D-160: march galaxies prefiltered over the pixel footprint (thin disc and arms averaged, no supersampling for them). 0: the D-130 supersampling."));
static TAutoConsoleVariable<int32> CVarUniVolumeLutStart(TEXT("uu.VolumeLutStart"), 1, TEXT("D-160: emission-only carriers start marching where their unresolved light becomes significant, not at 1 pc. 0: from 1 pc."));
// D-164: the cosmic web from a baked texture.
static TAutoConsoleVariable<int32> CVarUniWebSource(TEXT("uu.WebSource"), 1, TEXT("D-164: the cosmic web's density. 1: a 3D texture with mips (uu.WebTexture) (placement and the volume read the same texels; much cheaper to march). 0: the procedural lattices (D-012). Applies at uu.Enable; switching rerolls the web's galaxies."));
static TAutoConsoleVariable<int32> CVarUniSkyWeb(TEXT("uu.SkyWeb"), 0, TEXT("D-189: the web in the sky (art choice, render-only): 0 physical; 1 near-web glow (the near web's light lifted where it is overdense); 2 dimmed tail (the uniform far background dimmed, so the near web carries contrast); 3 both. Live."));
static TAutoConsoleVariable<float> CVarUniSkyWebGlow(TEXT("uu.SkyWeb.Glow"), 2.0f, TEXT("D-189: near-web glow gain (x overdensity)."));
static TAutoConsoleVariable<float> CVarUniSkyWebDepth(TEXT("uu.SkyWeb.Depth"), 50.0f, TEXT("D-189: near-web glow depth, Mpc (it fades over the second half)."));
static TAutoConsoleVariable<float> CVarUniSkyWebTail(TEXT("uu.SkyWeb.Tail"), 0.1f, TEXT("D-189: the far background's share kept in mode 2 (1: physical)."));
static TAutoConsoleVariable<float> CVarUniWarp(TEXT("uu.Warp"), 0.0f, TEXT("D-187: warp effects (streaks, arrival, D-190 optics, D-192 motes), 0..1 x the warp amount from speed (the travel-sensitivity curve). 0: physical. D-196: off by default (owner: at intergalactic speed the galaxies vanished with it on, B-018); 1 to try it."));
static TAutoConsoleVariable<float> CVarUniWarpMotes(TEXT("uu.Warp.Motes"), 1.0f, TEXT("D-192: warp ambience, 0..1 (x the warp amount): decorative motes fixed in space around the camera, streaking past at speed so a void still reads as fast. 0: none. Live."));
static TAutoConsoleVariable<float> CVarUniWarpMoteFrames(TEXT("uu.Warp.MoteFrames"), 30.0f, TEXT("D-192: the motes' lattice cell, in frames of the camera's motion (the nearest motes pass in about this many frames)."));
static TAutoConsoleVariable<float> CVarUniWarpMoteMag(TEXT("uu.Warp.MoteMag"), 1.5f, TEXT("D-192: the brightest motes' magnitude relative to the exposure magnitude (fainter > 0); they spread over two magnitudes below it."));
static TAutoConsoleVariable<int32> CVarUniSpriteEllipses(TEXT("uu.SpriteEllipses"), 1, TEXT("D-183: disc galaxies' sprites are ellipses at their disc's inclination (energy unchanged; round below a few pixels). 0: every sprite round. Render-only, live."));
static TAutoConsoleVariable<int32> CVarUniGalaxyPalettes(TEXT("uu.GalaxyPalettes"), 1, TEXT("D-182: every galaxy its own colour palette (bulge, disc, arms, knots), which its stars and glow take (chroma only: brightness unchanged). 0: physical colours. Applies at uu.Enable; rerolls star and galaxy colours only."));
static TAutoConsoleVariable<int32> CVarUniGalaxyTextures(TEXT("uu.GalaxyTextures"), 1, TEXT("D-179: galaxies read the noise project's galaxy textures (warp_fine, warp_large, galaxy_detail in Plugins/UnifiedUniverse/Data/Noise): texture warps of discs, arms and bulges, disc noise and star-forming knots. 0: off. Applies at uu.Enable; rerolls galaxies' stars."));
static TAutoConsoleVariable<FString> CVarUniWebTexture(TEXT("uu.WebTexture"), TEXT("web_adhesion_density_mid"), TEXT("D-165: which web texture uu.WebSource 1 uses: a texture delivered by the noise project in Plugins/UnifiedUniverse/Data/Noise (web_adhesion_density_young / _mid / _old), or standin (the procedural web baked here). Applies at uu.Enable; rerolls the web."));
static TAutoConsoleVariable<int32> CVarUniWebTexSize(TEXT("uu.WebTexSize"), 128, TEXT("D-164: stand-in web texture texels per period (power of two, 32..512). 128: 0.78 Mpc texels for the demo's 100 Mpc period."));
static TAutoConsoleVariable<int32> CVarUniWebTexSupersample(TEXT("uu.WebTexSupersample"), 2, TEXT("D-164: samples per texel axis when baking the stand-in web texture (1..4)."));

static TAutoConsoleVariable<int32> CVarUniVolumeMaxSubsamples(TEXT("uu.VolumeMaxSubsamples"), 8, TEXT("Most rays per pixel axis where a small carrier is supersampled (1: never supersample; 8: up to 64 rays)."));

namespace
{
	constexpr double kCruiseOn = 1.0e8;    // cm/s (D-008)
	constexpr double kCruiseOff = 5.0e7;
	constexpr double kAnchorRecentre = 1.0e8;

	TWeakObjectPtr<UUniSubsystem> GLastSubsystem;

	/** D-157: adds the scope's wall time (ms) to a slot. */
	struct FPerfTimer
	{
		double& Out;
		uint64 Start;
		explicit FPerfTimer(double& InOut) : Out(InOut), Start(FPlatformTime::Cycles64()) {}
		~FPerfTimer() { Out += FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - Start); }
	};
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

// D-216: start-up without console commands.
static TAutoConsoleVariable<FString> CVarUniAutoStart(TEXT("uu.AutoStart"), TEXT("flythrough"), TEXT("D-216: the scene enabled when play begins (galaxy, web or flythrough, as uu.Enable); none: wait for uu.Enable. Set in DefaultEngine.ini [ConsoleVariables] to change it."));
static TAutoConsoleVariable<int32> CVarUniAutoFly(TEXT("uu.AutoFly"), 1, TEXT("D-216: with uu.AutoStart, begin in free flight (as uu.Fly) once the player's pawn exists. 0: Local mode."));

void UUniSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const FString Scene = CVarUniAutoStart.GetValueOnGameThread().TrimStartAndEnd();
	if (Scene.IsEmpty() || Scene.Equals(TEXT("none"), ESearchCase::IgnoreCase)) { return; }
	EnableUniverse(Scene);
	// The pawn is usually possessed after the world's begin play: free flight waits for it (Tick).
	bAutoFlyPending = bEnabled && CVarUniAutoFly.GetValueOnGameThread() != 0;
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
	ClusterTuning = UniClusterCVars::Read();
	Warp.Current = 0.0;
	bCameraJumped = true;
	UniCore::FUniverseDef Def = SceneName == TEXT("web") ? UniScenes::WebToGalaxy(ClusterTuning) : (SceneName == TEXT("flythrough") ? UniScenes::Flythrough(ClusterTuning) : UniScenes::GalaxyToStar());
	SetupWebTexture(Def);   // D-164: before the model, which bakes the definition's parameters
	SetupGalaxyTextures(Def);   // D-179: likewise
	if (CVarUniGalaxyPalettes.GetValueOnGameThread() != 0) { UniScenes::UseGalaxyPalettes(Def); }   // D-182
	UniScenes::UseSpriteKeep(Def, ClusterTuning.StarKeep, ClusterTuning.MemberKeep);   // D-197: after the web (it rebuilds cluster and group emitters)
	UniScenes::UseDustTuning(Def, ClusterTuning.DustScale, ClusterTuning.DustLanes);   // D-198
	Model = MakeUnique<UniCore::FUniverseModel>(MoveTemp(Def));
	for (const std::string& Problem : Model->Validate())
	{
		UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: archetype validation: %s"), UTF8_TO_TCHAR(Problem.c_str()));
	}
	Generator = MakeUnique<UniCore::FRefGenerator>(*Model);
	GalaxyMaps.Reset();
	Generator->Maps = &GalaxyMaps;   // D-214: galaxies place their stars from their maps
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
	// D-183: disc galaxies (archetypes with a galaxy-field emitter) draw as inclined ellipses.
	DiscArchetypeMask = 0;
	for (const UniCore::FArchetype& A : Model->Definition().Archetypes)
	{
		for (const UniCore::FUniEmitter& E : A.Emitters)
		{
			if (E.FieldId == UniHlsl::UNI_FIELD_GALAXY && A.Id < 32u) { DiscArchetypeMask |= 1u << A.Id; }
		}
	}
	FMemory::Memzero(FixedDiscSeeds, sizeof(FixedDiscSeeds));
	{
		int32 NFixed = 0;
		for (const UniCore::FAnchor& An : Model->Definition().Anchors)
		{
			if (An.bFixedOrientation && NFixed < 4) { FixedDiscSeeds[NFixed++] = An.Seed; }
		}
	}
	UniverseTime = 0.0;
	FrameCellTables.Reset();
	ActivationsUnmatched = 0;
	{
		TArray<FUniActivation> Stale;
		FUniRenderer::Get().DrainActivations_GameThread(Stale);   // from a previous universe
		TArray<FUniPickReadback> StalePicks;
		FUniRenderer::Get().DrainPicks_GameThread(StalePicks);     // D-193
		TArray<FUniQueryReadback> StaleQueries;
		FUniRenderer::Get().DrainQueries_GameThread(StaleQueries);
	}
	FailApiRequests();   // D-193: anything asked of the previous universe
	ResidentEvents.Reset();
	VisibilityWatch.Clear();
	PromotedReported.Reset();

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
	ClearSkyLight();   // D-186: waits for its worker (it reads the CPU textures unbound below)
	Systems.Reset();
	FrameCellTables.Reset();
	FailApiRequests();       // D-193: nothing will answer them now
	ResidentEvents.Reset();
	VisibilityWatch.Clear();
	PromotedReported.Reset();
	Streamer.Reset();
	Backend.Reset();       // flushes rendering commands: in-flight requests are released safely
	Generator.Reset();
	Model.Reset();
	UniCore::BindWebTexture(nullptr);   // D-164: after everything that samples it on the CPU
	WebTexture.Reset();
	for (int32 S = 0; S < 3; ++S) { UniCore::BindGalaxyTexture(S, nullptr); GalaxyTextures[S].Reset(); }   // D-179
}

void UUniSubsystem::SetupGalaxyTextures(UniCore::FUniverseDef& Def)
{
	// D-179: the galaxy textures, all three or none. Missing (or uu.GalaxyTextures 0), galaxies keep
	// their untextured field (the terms stay off: the definition never asks for a texture it does not
	// have), with an error naming the files when they were wanted.
	for (int32 S = 0; S < 3; ++S) { UniCore::BindGalaxyTexture(S, nullptr); GalaxyTextures[S].Reset(); FUniRenderer::Get().SetGalaxyTexture_GameThread(S, 0u, {}); }
	if (CVarUniGalaxyTextures.GetValueOnGameThread() == 0) { return; }
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnifiedUniverse"));
	const FString Dir = FPaths::ConvertRelativePathToFull((Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::ProjectPluginsDir() / TEXT("UnifiedUniverse")) / TEXT("Data") / TEXT("Noise"));
	const double T0 = FPlatformTime::Seconds();
	TUniquePtr<UniCore::FWebTexture> T[3];
	for (int32 S = 0; S < 3; ++S)
	{
		T[S] = MakeUnique<UniCore::FWebTexture>();
		std::string Err;
		if (!UniCore::FWebTexture::LoadDelivered(TCHAR_TO_UTF8(*Dir), UniCore::GalaxyTextureName(S), *T[S], &Err))
		{
			UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: galaxy texture '%s' not loaded (%s). Galaxies use their untextured field: regenerate it with the noise project into %s, or set uu.GalaxyTextures 0."),
				UTF8_TO_TCHAR(UniCore::GalaxyTextureName(S)), UTF8_TO_TCHAR(Err.c_str()), *Dir);
			return;
		}
	}
	if (T[0]->Size != T[1]->Size || T[1]->Size != T[2]->Size)
	{
		UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: galaxy textures differ in size (%u, %u, %u); they must match. Galaxies use their untextured field."), T[0]->Size, T[1]->Size, T[2]->Size);
		return;
	}
	for (int32 S = 0; S < 3; ++S)
	{
		TArray<TArray<FUintVector2>> Mips;
		for (uint32 M = 0; M < T[S]->NumMips(); ++M)
		{
			TArray<FUintVector2>& A = Mips.AddDefaulted_GetRef();
			A.SetNumUninitialized(int32(T[S]->TexelCount(M)));
			FMemory::Memcpy(A.GetData(), T[S]->Mips[M].data(), T[S]->Mips[M].size() * sizeof(uint16));   // RGBA16F halves, as the web texture
		}
		FUniRenderer::Get().SetGalaxyTexture_GameThread(S, T[S]->Size, MoveTemp(Mips));
		GalaxyTextures[S] = MoveTemp(T[S]);
		UniCore::BindGalaxyTexture(S, GalaxyTextures[S].Get());
	}
	const int32 N = UniScenes::UseGalaxyTextures(Def, GalaxyTextures[0]->Size);
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: galaxy textures warp_fine, warp_large, galaxy_detail %u^3 loaded (%.2f s); %d galaxy emitters and anchors textured (D-179)"),
		GalaxyTextures[0]->Size, FPlatformTime::Seconds() - T0, N);
}

void UUniSubsystem::SetupWebTexture(UniCore::FUniverseDef& Def)
{
	// D-164 / D-165: a universe with a cosmic web gets its texture, and its web emitters switch to
	// it. Placement on the CPU (normalising integrals) and on the GPU, and the volume, then all
	// read the same texels. uu.WebTexture names a texture delivered by the noise project
	// (Plugins/UnifiedUniverse/Data/Noise/<name>.bin + <name>_mips.bin, not tracked in git), or
	// "standin" for the procedural web baked here (cached in Saved/).
	UniCore::BindWebTexture(nullptr);
	WebTexture.Reset();
	UniCore::FWebBakeKey Key;
	if (CVarUniWebSource.GetValueOnGameThread() == 0 || !UniCore::FindWebKey(Def, 128u, 1u, Key))
	{
		FUniRenderer::Get().SetWebTexture_GameThread(0u, {});
		return;
	}
	TUniquePtr<UniCore::FWebTexture> T = MakeUnique<UniCore::FWebTexture>();
	const FString Name = CVarUniWebTexture.GetValueOnGameThread().TrimStartAndEnd();
	const double T0 = FPlatformTime::Seconds();
	int32 Switched = 0;
	if (!Name.Equals(TEXT("standin"), ESearchCase::IgnoreCase))
	{
		// D-165: a delivered texture. Missing, the universe has no web emitters at all: no web
		// galaxies and no web glow (fail closed), with this error, never the procedural web in its
		// place.
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnifiedUniverse"));
		const FString Dir = FPaths::ConvertRelativePathToFull((Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::ProjectPluginsDir() / TEXT("UnifiedUniverse")) / TEXT("Data") / TEXT("Noise"));
		std::string Err;
		if (!UniCore::FWebTexture::LoadDelivered(TCHAR_TO_UTF8(*Dir), TCHAR_TO_UTF8(*Name), *T, &Err))
		{
			UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: web texture '%s' not loaded (%s). The web shows nothing until it is there: regenerate it with the noise project (NoiseVolumeTileCli --set %s) into %s, or set uu.WebTexture standin / uu.WebSource 0."),
				*Name, UTF8_TO_TCHAR(Err.c_str()), *Name, *Dir);
			Def.RootEmitters.erase(std::remove_if(Def.RootEmitters.begin(), Def.RootEmitters.end(),
				[](const UniCore::FUniEmitter& E) { return E.FieldId == UniHlsl::UNI_FIELD_WEB; }), Def.RootEmitters.end());
			FUniRenderer::Get().SetWebTexture_GameThread(0u, {});
			return;
		}
		const UniHlsl::float4 Mean = T->Mean();
		const double Total = double(Mean.x) + Mean.y + Mean.z + Mean.w;
		const double NodeShare = Total > 0.0 ? double(Mean.z) / Total : 0.0;
		// D-167: clusters from the node list when it is there (else from the node channel).
		std::vector<UniCore::FWebNode> Nodes;
		std::string NodeErr;
		const bool bNodes = UniCore::LoadWebNodes(TCHAR_TO_UTF8(*Dir), TCHAR_TO_UTF8(*Name), Nodes, &NodeErr);
		Switched = UniScenes::UseDeliveredWeb(Def, ClusterTuning, T->Size, NodeShare, bNodes ? &Nodes : nullptr);
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: web texture '%s' %u^3 loaded (%.2f s); matter: walls %.3f, filaments %.3f, nodes %.3f, void %.3f; 200 Mpc tile"),
			*Name, T->Size, FPlatformTime::Seconds() - T0, Mean.x, Mean.y, Mean.z, Mean.w);
		if (bNodes && !Def.Catalogues.empty())
		{
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: clusters: the %d heaviest of %d nodes per tile (rank-mapped to the cluster luminosity function; members uu.Cluster.Fraction of galaxies)"), int32(Def.Catalogues[0].Entries.size()), int32(Nodes.size()));
			int32 GroupCount = 0;
			for (const UniCore::FCatalogue& C : Def.Catalogues) { if (C.ArchetypeId == UniScenes::kGroup) { GroupCount += int32(C.Entries.size()); } }
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: groups: %d per tile (the remaining nodes; dwarfs by node mass, %.0f at the median; uu.Group.Enable %d) (D-174)"), GroupCount, ClusterTuning.GroupDwarfs, ClusterTuning.bGroups ? 1 : 0);
		}
		else
		{
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: clusters from the node matter channel (%s); members %.1f%% of galaxies (the node share)"), bNodes ? TEXT("clusters disabled") : UTF8_TO_TCHAR(NodeErr.c_str()), 100.0 * NodeShare);
		}
	}
	else
	{
		const int32 Size = int32(FMath::RoundUpToPowerOfTwo(uint32(FMath::Clamp(CVarUniWebTexSize.GetValueOnGameThread(), 32, 512))));
		const int32 SS = FMath::Clamp(CVarUniWebTexSupersample.GetValueOnGameThread(), 1, 4);
		UniCore::FindWebKey(Def, uint32(Size), uint32(SS), Key);
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("UnifiedUniverse");
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString Path = Dir / FString::Printf(TEXT("WebTexture_%016llx.uweb"), (unsigned long long)Key.Hash());
		const std::string PathUtf8 = TCHAR_TO_UTF8(*Path);
		if (UniCore::FWebTexture::Load(PathUtf8, Key, *T))
		{
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: stand-in web texture %d^3 loaded from %s (%.2f s)"), Size, *Path, FPlatformTime::Seconds() - T0);
		}
		else
		{
			*T = UniCore::FWebTexture::Bake(Key);
			const bool bSaved = T->Save(PathUtf8);
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: stand-in web texture %d^3 baked in %.1f s (%d samples per texel)%s"), Size, FPlatformTime::Seconds() - T0, SS * SS * SS,
				bSaved ? *FString::Printf(TEXT(", cached at %s"), *Path) : TEXT(", NOT cached"));
		}
		Switched = UniCore::EnableWebTexture(Def, Key);
	}
	TArray<TArray<FUintVector2>> Mips;
	for (uint32 M = 0; M < T->NumMips(); ++M)
	{
		TArray<FUintVector2>& A = Mips.AddDefaulted_GetRef();
		A.SetNumUninitialized(int32(T->TexelCount(M)));
		FMemory::Memcpy(A.GetData(), T->Mips[M].data(), T->Mips[M].size() * sizeof(uint16));   // RGBA16F: two halves per uint, little endian
	}
	FUniRenderer::Get().SetWebTexture_GameThread(T->Size, MoveTemp(Mips));
	WebTexture = MoveTemp(T);
	UniCore::BindWebTexture(WebTexture.Get());
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: %d web emitter(s) use the web texture (uu.WebSource 0: procedural)"), Switched);
}

void UUniSubsystem::SetCamera(const FUniCoord& Coord)
{
	bCameraJumped = true;   // D-154: a jump is not travel speed
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
	EffectiveDeltaMag = DeltaMag;
	if (Streamer) { Streamer->DeltaMag = EffectiveDeltaMag; }
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
	const FString Line = FString::Printf(TEXT("UU %s  speed %s/s (%.3g cm/s)%s  |  %s from the demo origin  |  dm %+.1f (travel exposure %+.2f)  exposure %+.1f  |  t %.4g s (x%.3g)  systems %d, planets %d, promoted %d"),
		bFreeFly ? (MotionMode == EUniMotionMode::Local ? TEXT("FLY (local: real space, physics)") : TEXT("FLY (cruise)")) : TEXT("CRUISE"),
		UTF8_TO_TCHAR(UniCore::FormatDistance(Speed).c_str()), Speed,
		bFreeFly ? TEXT("  [WASD/QE move, wheel x2 / x0.5]") : TEXT(""),
		UTF8_TO_TCHAR(UniCore::FormatDistance(R).c_str()), EffectiveDeltaMag, TravelExposureMag, ExposureBias,
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
	if (bAutoFlyPending)   // D-216
	{
		const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC && PC->GetPawn()) { bAutoFlyPending = false; SetFreeFly(true); }
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Tick);
	for (double& M : PerfMs) { M = 0.0; }
	{
	FPerfTimer TickTimer(PerfMs[PerfTick]);
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

	// D-154: travel sensitivity from the camera's actual speed this frame (any motion mode; jumps
	// excluded), added to the owner's sensitivity and applied everywhere sensitivity is.
	{
		const double Dt = FMath::Max(double(DeltaTime), 1.0e-4);
		const UniCore::FVec3d Step = UniCore::ToDouble(UniCore::Sub(C, PrevCamera.ToCore()));
		const double Speed = bCameraJumped ? 0.0 : UniCore::Length(Step) / Dt;
		CameraSpeedCmPerS = Speed;   // D-216
		// D-184: the volume's temporal reprojection needs the camera's move (not across a jump).
		CameraStepCm = bCameraJumped ? FVector3d::ZeroVector : FVector3d(Step.X, Step.Y, Step.Z);
		bCameraStepValid = !bCameraJumped;
		// D-187: the warp amount, the travel curve normalised to 0..1 (render-only effects). D-213:
		// the D-154 travel sensitivity it was derived from is gone (D-205 replaced it with exposure).
		EffectiveDeltaMag = DeltaMag;
		Warp.MaxDeltaMag = 1.0;
		Warp.StartCmPerS = FMath::Max(double(CVarUniTravelStart.GetValueOnGameThread()), 1.0);
		Warp.FullCmPerS = FMath::Max(double(CVarUniTravelFull.GetValueOnGameThread()), Warp.StartCmPerS * 1.01);
		Warp.RiseMagPerS = 2.0;
		Warp.FallMagPerS = 1.0;
		Warp.Update(Speed, Dt);
		// D-205: render-only brightening at speed (the exposure travel sensitivity used to bring).
		// Quantised to 0.05 mag so the volume's tables rebuild at most ~30 times over the ramp.
		TravelExposureMag = FMath::RoundToFloat(FMath::Clamp(CVarUniTravelExposure.GetValueOnGameThread(), 0.0f, 5.0f) * float(FMath::Clamp(Warp.Current, 0.0, 1.0)) * 20.0f) / 20.0f;
		// D-192: the motes' lattice follows the speed (held while still, reset on a jump).
		if (bCameraJumped) { MoteScale.Reset(); }
		MoteScale.Update(UniCore::Length(Step) * (bCameraJumped ? 0.0 : 1.0), FMath::Max(double(CVarUniWarpMoteFrames.GetValueOnGameThread()), 1.0), Dt);
		PrevCamera = Camera;
		bCameraJumped = false;
	}
	Streamer->DeltaMag = EffectiveDeltaMag;
	Streamer->SubmitDeferPerLevel = CVarUniSubmitDefer.GetValueOnGameThread();   // D-173
	Streamer->bCellFade = CVarUniCellGlow.GetValueOnGameThread() != 0;            // D-203
	Streamer->bOneTrip = CVarUniGenOneTrip.GetValueOnGameThread() != 0;           // D-207
	Streamer->CellFadeFrames = FMath::Clamp(double(CVarUniCellFadeFrames.GetValueOnGameThread()), 0.0, 120.0);
	Streamer->BuildBudget = FMath::Max(CVarUniStreamBuildBudget.GetValueOnGameThread(), 0);
	{
		// D-214: the frame's generation cap, the split place pass's chunk grid (the cap's chunks
		// plus one partial chunk per task), and the view the build order prefers.
		const int32 Cap = FMath::Max(CVarUniGenFrameCandidates.GetValueOnGameThread(), 0);
		Streamer->FrameCandidateCap = UniCore::u64(Cap);
		Streamer->GenChunkCap = Cap > 0 ? uint32(Cap) / UniHlsl::UNI_GEN_CHUNK + 8192u : 65536u;
		Streamer->bViewPriority = CVarUniGenViewPriority.GetValueOnGameThread() != 0;
		const FVector Fw = ViewRot.Vector();
		Streamer->ViewDir = UniCore::FVec3d{ Fw.X, Fw.Y, Fw.Z };
		float FovDeg = 90.0f;
		if (UWorld* World = GetWorld()) { if (APlayerController* PC = World->GetFirstPlayerController()) { if (PC->PlayerCameraManager) { FovDeg = PC->PlayerCameraManager->GetFOVAngle(); } } }
		// The diagonal's half angle (aspect up to ~2:1), plus 10 degrees for turning.
		const double HalfX = FMath::DegreesToRadians(double(FMath::Clamp(FovDeg, 1.0f, 170.0f)) * 0.5);
		const double Half = FMath::Atan(FMath::Tan(HalfX) * 1.12) + FMath::DegreesToRadians(10.0);
		Streamer->ViewCos = Half >= UE_DOUBLE_PI ? -2.0 : FMath::Cos(Half);
	}
	{
		// D-214: the galaxy maps: their slots, a tick, and the bakes the GPU finished.
		const uint32 Slots = uint32(FMath::Clamp(CVarUniGalaxyMapSlots.GetValueOnGameThread(), 1, FUniRenderStats::MaxGalMapSlots));
		const uint32 Size = FMath::RoundUpToPowerOfTwo(uint32(FMath::Clamp(CVarUniGalaxyMapSize.GetValueOnGameThread(), 256, 4096)));
		GalaxyMaps.Configure(Slots, Size);
		GalaxyMaps.Tick();
		for (uint32 S = 0; S < GalaxyMaps.NumSlots(); ++S)
		{
			const uint32 E = FUniRenderer::Get().Stats.GalMapBaked[S].load();
			if (E != 0u && E != GalaxyMapEpochSeen[S] && GalaxyMaps.Entry(S).Epoch == E) { ++GalaxyMapBakesSeen; }
			GalaxyMapEpochSeen[S] = E;
			GalaxyMaps.MarkBaked(S, E);
		}
	}
	{
		// D-168: contributors the volume builder will ask for (above its reject angle) stay awake:
		// half that angle, from last frame's pixel size (as the builder uses).
		const double ThetaPx = double(FUniRenderer::Get().Stats.ThetaPixelLastFrame.load());
		const double Reject = 0.5 * FMath::Min(double(UniHlsl::UNI_VOL_HANDOFF_PX) * ThetaPx, double(UniHlsl::UNI_THETA_PROMOTE));
		Streamer->WakeTheta = FMath::Max(0.5 * Reject, 1.0e-6);
	}
	UniverseTime += double(DeltaTime) * TimeScale;   // B-002: orbits are on rails at this clock
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_StreamerUpdate);
		FPerfTimer T(PerfMs[PerfStreamer]);
		Streamer->Update(C);
	}
	// D-193: picks and queries read back, then this frame's events (the streamer just moved).
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Api); ProcessApiReadbacks(); UpdateApiEvents(ViewRot); }
	// M3: bodies close enough become actors, and every parallax-placed actor follows the camera
	// that just moved (same frame, same camera: no lag against the backdrop at any speed).
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Promotion); FPerfTimer T(PerfMs[PerfPromotion]); UpdatePromotion(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Parallax); FPerfTimer T(PerfMs[PerfParallax]); UpdateParallax(ViewLoc); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_LightRig); FPerfTimer T(PerfMs[PerfLightRig]); UpdateLightRig(); UpdateSkyLight(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_PushFrame); FPerfTimer T(PerfMs[PerfPushFrame]); PushFrame(); }
	}
	RecordPerf();
}

// ---------------------------------------------------------------------------------------------
// Instrumentation (D-157)
// ---------------------------------------------------------------------------------------------

const TCHAR* UUniSubsystem::PerfPhaseName(int32 Phase)
{
	switch (Phase)
	{
	case PerfTick: return TEXT("tick (all)");
	case PerfStreamer: return TEXT("streamer");
	case PerfPromotion: return TEXT("promotion");
	case PerfParallax: return TEXT("parallax");
	case PerfLightRig: return TEXT("light rig");
	case PerfPushFrame: return TEXT("push frame");
	case PerfSystems: return TEXT("  systems");
	case PerfVolumeBuild: return TEXT("  volume build");
	default: return TEXT("?");
	}
}

FString UUniSubsystem::StreamBreakdown(const UniCore::FStreamProfile& P) const
{
	FString S;
	for (int32 I = 0; I < int32(UniCore::EStreamPhase::Count); ++I)
	{
		if (P.Ms[I] < 0.05) { continue; }
		const char* Name = UniCore::StreamPhaseShortName(UniCore::EStreamPhase(I));
		S += FString::Printf(TEXT("%s%s %.1f"), S.IsEmpty() ? TEXT("") : TEXT(", "), UTF8_TO_TCHAR(Name), P.Ms[I]);
	}
	S += FString::Printf(TEXT(" | cells submitted %llu (tasks %llu), overlap tests %llu, growth checks %llu, read back %llu objects, contributors +%llu -%llu (awake %llu, asleep %llu; woke %llu, slept %llu), cells deferred %llu, gathered %llu, batch-cell scans %llu, cell table %llu"),
		P.CellsSubmitted, P.TasksSubmitted, P.OverlapTests, P.GrowthChecks, P.ObjectsReadBack, P.ContributorsAdded, P.ContributorsRemoved, P.Awake, P.Asleep, P.Woken, P.Slept, P.CellsDeferred, P.Gathered, P.BatchCellScans, P.CellTableEntries);
	return S;
}

void UUniSubsystem::RecordPerf()
{
	if (!Streamer) { return; }
	StreamLast = Streamer->Profile;
	const double A = PerfFrames == 0 ? 1.0 : 0.05;   // ~20-frame average
	for (int32 I = 0; I < PerfCount; ++I)
	{
		PerfAvg[I] += A * (PerfMs[I] - PerfAvg[I]);
		PerfPeak[I] = FMath::Max(PerfPeak[I], PerfMs[I]);
	}
	for (int32 I = 0; I < int32(UniCore::EStreamPhase::Count); ++I) { StreamAvg[I] += A * (StreamLast.Ms[I] - StreamAvg[I]); }
	if (PerfMs[PerfTick] >= StreamWorstTickMs) { StreamWorstTickMs = PerfMs[PerfTick]; StreamWorst = StreamLast; }
	++PerfFrames;
	const float Hitch = CVarUniPerfHitchMs.GetValueOnGameThread();
	if (Hitch > 0.0f && PerfMs[PerfTick] > double(Hitch))
	{
		++PerfHitches;
		UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: hitch %.1f ms on the game thread (streamer %.1f, push frame %.1f [systems %.1f, volume build %.1f], promotion %.1f) | streamer: %s"),
			PerfMs[PerfTick], PerfMs[PerfStreamer], PerfMs[PerfPushFrame], PerfMs[PerfSystems], PerfMs[PerfVolumeBuild], PerfMs[PerfPromotion], *StreamBreakdown(StreamLast));
	}
	if (CVarUniPerfHud.GetValueOnGameThread() != 0) { DrawPerfHud(); }
}

void UUniSubsystem::DrawPerfHud() const
{
	if (!GEngine) { return; }
	const FUniRenderStats& R = FUniRenderer::Get().Stats;
	FString L1 = TEXT("UU perf (ms, avg / peak since uu.Perf):");
	for (int32 I = 0; I < PerfCount; ++I) { L1 += FString::Printf(TEXT("  %s %.1f/%.1f"), PerfPhaseName(I), PerfAvg[I], PerfPeak[I]); }
	FString L2 = TEXT("UU streamer avg:");
	for (int32 I = 0; I < int32(UniCore::EStreamPhase::Count); ++I)
	{
		if (StreamAvg[I] < 0.05) { continue; }
		L2 += FString::Printf(TEXT("  %s %.1f"), UTF8_TO_TCHAR(UniCore::StreamPhaseShortName(UniCore::EStreamPhase(I))), StreamAvg[I]);
	}
	const UniCore::FStreamProfile& P = StreamLast;
	const FString L3 = FString::Printf(TEXT("UU work: sprites %u, cells drawn %u, carriers %u, contributors %llu | this frame: submitted %llu cells / %llu tasks, %llu overlap tests, read back %llu, +%llu -%llu contributors | hitches %llu"),
		R.SpritesLastFrame.load(), R.CellsLastFrame.load(), R.VolumeCarriersLastFrame.load(), uint64(Streamer ? Streamer->Contributors().size() : 0),
		P.CellsSubmitted, P.TasksSubmitted, P.OverlapTests, P.ObjectsReadBack, P.ContributorsAdded, P.ContributorsRemoved, PerfHitches);
	GEngine->AddOnScreenDebugMessage(int32(0x5EED0011), 0.0f, FColor(255, 210, 120), L1);
	GEngine->AddOnScreenDebugMessage(int32(0x5EED0012), 0.0f, FColor(255, 210, 120), L2);
	GEngine->AddOnScreenDebugMessage(int32(0x5EED0013), 0.0f, FColor(255, 210, 120), L3);
}

void UUniSubsystem::LogPerf()
{
	if (!Streamer) { UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: disabled")); return; }
	const FUniRenderStats& R = FUniRenderer::Get().Stats;
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse perf over %llu frames (game thread, ms; avg ~20 frames / peak since the last uu.Perf), %llu hitches over uu.PerfHitchMs:"), PerfFrames, PerfHitches);
	for (int32 I = 0; I < PerfCount; ++I) { UE_LOG(LogUnifiedUniverse, Log, TEXT("  %-14s %7.2f / %7.2f"), PerfPhaseName(I), PerfAvg[I], PerfPeak[I]); }
	FString Avg;
	for (int32 I = 0; I < int32(UniCore::EStreamPhase::Count); ++I) { Avg += FString::Printf(TEXT("%s%s %.2f"), Avg.IsEmpty() ? TEXT("") : TEXT(", "), UTF8_TO_TCHAR(UniCore::StreamPhaseShortName(UniCore::EStreamPhase(I))), StreamAvg[I]); }
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  streamer avg: %s"), *Avg);
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  worst frame (%.1f ms) streamer: %s"), StreamWorstTickMs, *StreamBreakdown(StreamWorst));
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  last frame streamer: %s"), *StreamBreakdown(StreamLast));
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  render: %u sprites, %u cells drawn, %u volume carriers, %u background faces marched so far; %llu contributors registered. GPU pass times: ProfileGPU (Ctrl+Shift+,)"),
		R.SpritesLastFrame.load(), R.CellsLastFrame.load(), R.VolumeCarriersLastFrame.load(), R.BackgroundFacesMarched.load(), uint64(Streamer->Contributors().size()));
	// D-200: the volume auto budget.
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  volume auto budget: level %d of %d (x%.2f of the march pixel budget), GPU %.1f ms smoothed, target %.1f fps, %u changes so far (uu.VolumeTargetFps)"),
		VolumeBudget.Level, UniCore::FVolumeAutoBudget::NumLevels - 1, VolumeBudget.Scale(), VolumeBudget.SmoothMs, CVarUniVolumeTargetFps.GetValueOnGameThread(), VolumeBudget.Changes);
	// D-196: what the volume march marched last frame, and how often the background was in it.
	// D-209: the slicing actually used (N x N, D-208) and, since the last uu.Perf, how often each.
	const uint32 MFlags = R.VolumeMarchFlags.load();
	const uint32 SliceN = (MFlags >> 8) & 0xffu;
	const FString SliceText = (MFlags & 2u) ? FString::Printf(TEXT(", 1/%u sliced"), SliceN * SliceN) : FString(TEXT(", not sliced"));
	const uint32 BgPp = R.BackgroundPerPixelFrames.load();
	static IConsoleVariable* CVarMaxMarchPx = IConsoleManager::Get().FindConsoleVariable(TEXT("uu.VolumeMaxMarchPixels"));
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  volume march: %ux%u (uu.VolumeMaxMarchPixels %d), %u rays last frame%s%s; background per pixel in %u of the last %llu frames (uu.BackgroundMode, uu.BackgroundPerPixelInterval)"),
		R.VolumeMarchWidth.load(), R.VolumeMarchHeight.load(), CVarMaxMarchPx ? CVarMaxMarchPx->GetInt() : -1, R.VolumeMarchRays.load(), *SliceText,
		(MFlags & 1u) ? TEXT(", with the background") : TEXT(""), BgPp - BgPerPixelAtPerf, PerfFrames - PerfFramesAtBg);
	BgPerPixelAtPerf = BgPp;
	PerfFramesAtBg = PerfFrames;
	uint32 Sl[FUniRenderStats::NumSliceCounts];
	for (int32 I = 0; I < FUniRenderStats::NumSliceCounts; ++I) { Sl[I] = FUniRenderer::Get().Stats.VolumeSliceCounts[I].exchange(0u); }   // R is const
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  volume slicing since the last uu.Perf: 1/16 %u, 1/9 %u, 1/4 %u frames; not sliced %u (temporal off or a debug view %u, no history %u, march size changed %u, camera jump %u, same frame rendered again %u, frames skipped %u)"),
		Sl[4], Sl[3], Sl[2], Sl[5] + Sl[6] + Sl[7] + Sl[8] + Sl[9] + Sl[10], Sl[5], Sl[6], Sl[7], Sl[8], Sl[9], Sl[10]);
	{
		uint32 Used = 0, Baking = 0;
		for (uint32 S = 0; S < GalaxyMaps.NumSlots(); ++S) { if (GalaxyMaps.Entry(S).Key != 0ull) { ++Used; if (!GalaxyMaps.Entry(S).bReady) { ++Baking; } } }
		UE_LOG(LogUnifiedUniverse, Log, TEXT("  galaxy maps (uu.GalaxyMap.*): %u of %u slots used (%u baking), %u^2 (%.1f MB with mips); the camera's galaxy %s; %llu maps baked in %u GPU frames, %llu requests, %llu evictions, %llu waits"),
			Used, GalaxyMaps.NumSlots(), Baking, GalaxyMaps.MapSize(), double(GalaxyMaps.NumSlots()) * double(GalaxyMaps.MapSize()) * GalaxyMaps.MapSize() * 8.0 * 4.0 / 3.0 / 1048576.0,
			GalaxyMapVolumeSlot >= 0 ? TEXT("from its map") : TEXT("from the field (no map yet, or uu.GalaxyMap 0)"),
			(unsigned long long)GalaxyMapBakesSeen, R.GalMapBakeFrames.load(), (unsigned long long)GalaxyMaps.Requests, (unsigned long long)GalaxyMaps.Evictions, (unsigned long long)GalaxyMaps.Waits);
		UE_LOG(LogUnifiedUniverse, Log, TEXT("  generation (uu.GenFrameCandidates %llu): %llu cells deferred by the cap, %llu waited for a galaxy map; since the last uu.Perf %llu one-trip batches, candidates planned %.0f a batch on average, %llu at most (GPU: the Uni.Gen.* scopes in ProfileGPU / Insights)"),
			(unsigned long long)(Streamer ? Streamer->FrameCandidateCap : 0ull), (unsigned long long)(Streamer ? Streamer->FusedDeferred : 0ull), (unsigned long long)(Streamer ? Streamer->MapWaits : 0ull),
			(unsigned long long)(Streamer ? Streamer->FusedPlannedBatches : 0ull), Streamer && Streamer->FusedPlannedBatches ? double(Streamer->FusedPlannedSum) / double(Streamer->FusedPlannedBatches) : 0.0,
			(unsigned long long)(Streamer ? Streamer->FusedPlannedMax : 0ull));
		if (Streamer) { Streamer->ResetGenStats(); }
	}
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  background map re-marched since the last uu.Perf: %u (camera moved %u, LUT layout %u, its LUT changed %u, sky web %u)"),
		BgDirtyCounts[0] + BgDirtyCounts[1] + BgDirtyCounts[2] + BgDirtyCounts[3], BgDirtyCounts[0], BgDirtyCounts[1], BgDirtyCounts[2], BgDirtyCounts[3]);
	for (uint32& C : BgDirtyCounts) { C = 0u; }
	for (double& Pk : PerfPeak) { Pk = 0.0; }
	StreamWorstTickMs = 0.0;
	StreamWorst = UniCore::FStreamProfile();
	PerfHitches = 0;
}

void UUniSubsystem::PushFrame()
{
	FUniFrameData F;
	UniCore::FFrameCells Table;   // D-193: keys, generations and planets' cells, for readbacks
	const std::vector<UniCore::FCellGPU> Cells = Streamer->CellTable(&Table.Keys, &Table.Generations);
	F.Cells.Append(Cells.data(), int32(Cells.size()));

	// Star systems (D-142, D-143). Activations name a cell by its index in the table of the
	// frame that produced them (2-4 frames ago), so the last few tables' keys are kept.
	if (Systems)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Systems);
		FPerfTimer SysTimer(PerfMs[PerfSystems]);
		const UniCore::FCoord Cam = Camera.ToCore();
		TArray<FUniActivation> Acts;
		FUniRenderer::Get().DrainActivations_GameThread(Acts);
		for (const FUniActivation& A : Acts)
		{
			const UniCore::FFrameCells* Then = FrameCellTables.Find(A.FrameNumber);
			if (!Then || A.CellIndex >= Then->Keys.size()) { ++ActivationsUnmatched; continue; }
			// D-193: the star's identity, so its system's bodies can be named (events, picks).
			UniCore::FObjectId StarId;
			int Planet = -1;
			UniCore::FCellKey RecordCell;
			const bool bNamed = UniCore::NameSlot(*Then, *Streamer, A.CellIndex, A.ObjectIndex, StarId, Planet, RecordCell) && Planet < 0;
			Systems->Report(Then->Keys[A.CellIndex], A.Record, Cam, FrameCounter, bNamed ? &StarId : nullptr);
		}
		Systems->Update(Cam);
		std::vector<UniCore::FCellGPU> SysCells;
		std::vector<UniCore::FObjectGPU> SysRecords;
		const std::set<UniCore::u32> Culled = Promoter.CulledSpriteIds();   // promoted planets: their actor draws them (D-147)
		Systems->Build(Cam, UniverseTime, FUniRenderer::DynamicPoolBase(), FUniRenderer::DynamicPoolSlots(), SysCells, SysRecords, &Culled, &Table.Bodies);
		F.Cells.Append(SysCells.data(), int32(SysCells.size()));
		F.DynamicRecords.Append(SysRecords.data(), int32(SysRecords.size()));
		F.SystemArchetypeMask = SystemArchetypeMask;
	}
	// D-193: this frame's table, named by its frame number (readbacks arrive 2-4 frames later).
	FrameCellTables.Add(FrameCounter, MoveTemp(Table));
	for (auto It = FrameCellTables.CreateIterator(); It; ++It) { if (It.Key() + 32 < FrameCounter) { It.RemoveCurrent(); } }
	F.CameraFraction = CameraFraction;
	F.CameraStepCm = FVector3f(CameraStepCm);   // D-184
	F.WarpAmount = float(Warp.Current) * FMath::Clamp(CVarUniWarp.GetValueOnGameThread(), 0.0f, 1.0f);   // D-187
	F.SpeedAmount = float(Warp.Current);   // D-198: the travel curve (0 at rest, 1 at 10 Mpc/s), whatever uu.Warp
	{
		// D-200: the glow's pixel budget from the GPU's last frame time (the volume is most of it).
		const float Fps = CVarUniVolumeTargetFps.GetValueOnGameThread();
		const double GpuMs = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
		F.VolumeBudgetScale = float(VolumeBudget.Update(GpuMs, Fps > 0.0f ? 1000.0 / double(Fps) : 0.0, FMath::Clamp(CVarUniVolumeAutoHold.GetValueOnGameThread(), 1, 600)));
	}
	F.bCameraStepValid = bCameraStepValid;
	{
		// D-192: the warp motes' lattice around the camera.
		const UniCore::FMoteLattice Ml = UniCore::MoteLattice(Camera.ToCore(), UniCore::FVec3f{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z }, MoteScale.Log2Cm,
			F.WarpAmount * FMath::Clamp(CVarUniWarpMotes.GetValueOnGameThread(), 0.0f, 1.0f));
		for (int32 K = 0; K < 2; ++K)
		{
			const UniCore::FMoteSlot& Sl = Ml.Slot[K];
			F.MoteCell[K] = FUintVector4(Sl.Cell[0], Sl.Cell[1], Sl.Cell[2], Sl.Level);
			F.MoteFrac[K] = FVector4f(Sl.Frac[0], Sl.Frac[1], Sl.Frac[2], Sl.Weight);
		}
		F.MoteScale = FVector4f(float(Ml.Slot[0].CellCm), float(Ml.Slot[1].CellCm), FMath::Pow(10.0f, -0.4f * CVarUniWarpMoteMag.GetValueOnGameThread()), 0.0f);
	}
	F.DiscArchetypeMask = CVarUniSpriteEllipses.GetValueOnGameThread() != 0 ? DiscArchetypeMask : 0u;   // D-183 (render-only: live)
	for (int32 K = 0; K < 4; ++K) { F.FixedDiscSeeds[K] = FixedDiscSeeds[K]; }
	F.DeltaMag = EffectiveDeltaMag;   // D-154: owner sensitivity + travel boost
	F.ExposureBias = ExposureBias + TravelExposureMag;   // D-205
	F.bEnabled = true;
	F.FrameNumber = FrameCounter;
	if (bVolume && VolumeBuilder)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_VolumeBuild);
		FPerfTimer VolTimer(PerfMs[PerfVolumeBuild]);
		// The carriers' handoff weights use the same view quantities as project-and-cull.
		UniHlsl::FUniView V{};
		V.LimitingMag = UniHlsl::Uni_LimitingMag(EffectiveDeltaMag);
		V.SurfaceLimit = UniHlsl::UNI_MU_REF + FMath::Clamp(EffectiveDeltaMag, -5.0f, 5.0f);
		V.ExposureMag = V.LimitingMag - 1.0f + ExposureBias + TravelExposureMag;   // D-205
		V.ThetaPixel = FUniRenderer::Get().Stats.ThetaPixelLastFrame.load();
		const UniCore::FCoord Cam = Camera.ToCore();
		UniCore::FVolumeSettings Settings;   // D-157: march quality from the console
		Settings.MaxSteps = UniCore::u32(FMath::Clamp(CVarUniVolumeMaxSteps.GetValueOnGameThread(), 16, 4096));
		Settings.StepRatio = FMath::Clamp(CVarUniVolumeStepRatio.GetValueOnGameThread(), 0.005f, 1.0f);
		Settings.HintScale = FMath::Clamp(CVarUniVolumeHintScale.GetValueOnGameThread(), 0.25f, 8.0f);
		Settings.TexturedWebStepRatio = FMath::Clamp(CVarUniVolumeWebStepRatio.GetValueOnGameThread(), 0.02f, 1.0f);   // D-199
		Settings.WebHintScale = float(FMath::Lerp(1.0, double(FMath::Clamp(CVarUniVolumeWebTravelHint.GetValueOnGameThread(), 1.0f, 4.0f)), FMath::Clamp(Warp.Current, 0.0, 1.0)));   // D-202
		Settings.bCellGlow = CVarUniCellGlow.GetValueOnGameThread() != 0;   // D-203
		Settings.LocalStartCm = double(FMath::Clamp(CVarUniVolumeStartPc.GetValueOnGameThread(), 0.01f, 1000.0f)) * 3.0856775814913673e18;
		Settings.MaxSubsamples = UniCore::u32(FMath::Clamp(CVarUniVolumeMaxSubsamples.GetValueOnGameThread(), 1, 8));
		Settings.bPrefilter = CVarUniVolumePrefilter.GetValueOnGameThread() != 0;
		Settings.bLutStart = CVarUniVolumeLutStart.GetValueOnGameThread() != 0;
		{
			// D-189: the web in the sky.
			const int32 SkyMode = CVarUniSkyWeb.GetValueOnGameThread();
			Settings.WebGlowGain = (SkyMode & 1) ? FMath::Max(CVarUniSkyWebGlow.GetValueOnGameThread(), 0.0f) : 0.0f;
			Settings.WebGlowDepthCm = float(double(FMath::Max(CVarUniSkyWebDepth.GetValueOnGameThread(), 0.1f)) * 3.0856775814913673e24);
			Settings.TailDim = (SkyMode & 2) ? 1.0f - FMath::Clamp(CVarUniSkyWebTail.GetValueOnGameThread(), 0.0f, 1.0f) : 0.0f;
		}
		const UniCore::FVolumeScene Sc = VolumeBuilder->Build(*Streamer, Cam, UniCore::FVec3f{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z }, V, Settings);
		for (const UniCore::FCarrierGPU& C : Sc.Carriers)
		{
			((C.Flags & UniHlsl::UNI_CARRIER_PERIODIC) ? F.BackgroundCarriers : F.VolumeCarriers).Add(C);
		}
		F.VolumeParams.Append(Sc.Params.data(), int32(Sc.Params.size()));
		F.VolumeLut.Reserve(int32(Sc.Lut.size()));
		for (const UniHlsl::float4& L : Sc.Lut) { F.VolumeLut.Add(FVector4f(L.x, L.y, L.z, L.w)); }
		F.VolumeView = Sc.View;
		// D-212 / D-214: the camera's galaxy's map (its whole map, dust too) from the cache; published
		// once baked. The cache's pending bakes (generation's too) ride every frame until done.
		{
			const bool bMap = CVarUniGalaxyMap.GetValueOnGameThread() != 0;
			const UniCore::FCarrierGPU* Home = UniCore::HomeCarrierOf(Sc);
			GalaxyMapVolumeSlot = -1;
			if (bMap && Home)
			{
				const int Slot = GalaxyMaps.AcquireFull(UniCore::FGalaxyMap::GalaxyMapRequestOf(*Home, Sc.Params));
				if (Slot >= 0)
				{
					GalaxyMapVolumeSlot = Slot;
					UniCore::PublishGalaxyMap(F.VolumeView, Sc.Home.Carrier, GalaxyMaps.MapSize(), uint32(Slot));
					F.GalMapVolumeEpoch = GalaxyMaps.Entry(uint32(Slot)).Epoch;
				}
			}
		}
		F.VolumeNearCm = float(Sc.NearCm);   // D-208
		F.BackgroundStartCm = float(Settings.BackgroundStartCm);
		F.bVolume = true;
		// D-196: each carrier's parent named as the project pass sees it (this frame's table), so
		// its sprite hands over exactly the share the carrier took.
		if (const UniCore::FFrameCells* Now = FrameCellTables.Find(FrameCounter))
		{
			for (const std::array<UniCore::u32, 4>& E : UniCore::CarrierHandoffTable(Sc.Handoffs, Now->Keys, *Streamer))
			{
				F.CarrierHandoffs.Add(FUintVector4(E[0], E[1], E[2], E[3]));
			}
		}
		StartSkyJob(Sc);   // D-186

		// Re-march the background map (D-136) when the camera has moved a quarter texel's
		// parallax at the background's start, or its LUT has changed by more than 1%.
		static IConsoleVariable* CVarBgSize = IConsoleManager::Get().FindConsoleVariable(TEXT("uu.BackgroundSize"));
		const double TexelRad = 2.0 / double(FMath::Max(CVarBgSize ? CVarBgSize->GetInt() : 256, 16));
		const UniCore::FVec3d Moved = UniCore::ToDouble(UniCore::Sub(Cam, BackgroundAnchor));
		// D-209: the LUT's layout is its blocks' end, not the buffer's size: the cell-glow grids
		// (D-203) after the blocks change size whenever a level starts or finishes loading, and
		// comparing the size re-marched the background (per pixel, uu.BackgroundMode 2) at rest.
		const int32 LutBlocks = int32(UniCore::LutBlocksEnd(F.VolumeView.GlowLevels, F.VolumeView.GlowBase, size_t(F.VolumeLut.Num())));
		const bool bMoved = FMath::Sqrt(Moved.X * Moved.X + Moved.Y * Moved.Y + Moved.Z * Moved.Z) > 0.25 * TexelRad * Settings.BackgroundStartCm;
		const bool bLayout = BackgroundLutLast.Num() != LutBlocks;
		bool bDirty = bMoved || bLayout;
		// Only the background's own LUT blocks (emission and tail) matter here.
		bool bLutChanged = false;
		for (const UniCore::FCarrierGPU& C : F.BackgroundCarriers)
		{
			for (uint32 Blk = C.LutIndex; Blk <= C.LutIndex + 1u && !bDirty; ++Blk)
			{
				for (int32 I = int32(Blk * UniHlsl::UNI_VOL_LUT_SIZE); !bDirty && I < int32((Blk + 1u) * UniHlsl::UNI_VOL_LUT_SIZE) && I < LutBlocks; ++I)
				{
					const FVector4f A = F.VolumeLut[I], B = BackgroundLutLast[I];
					bDirty = FMath::Abs(A.Y - B.Y) > 0.01f * FMath::Max(FMath::Abs(B.Y), 1.0e-30f);
					bLutChanged = bDirty;
				}
			}
		}
		// D-189: a sky-web change re-marches the background map too.
		const FVector3f SkyKey(Settings.TailDim, Settings.WebGlowGain, Settings.WebGlowDepthCm);
		const bool bSky = SkyKey != SkyWebLast;
		if (bSky) { bDirty = true; SkyWebLast = SkyKey; }
		if (bDirty)
		{
			++BackgroundVersion;
			BackgroundAnchor = Cam;
			BackgroundLutLast.Reset(LutBlocks);
			BackgroundLutLast.Append(F.VolumeLut.GetData(), LutBlocks);
			// D-209: why (uu.Perf), first reason only.
			++BgDirtyCounts[bMoved ? 0 : bLayout ? 1 : bLutChanged ? 2 : 3];
		}
		F.BackgroundVersion = BackgroundVersion;
	}
	FillApiRequests(F);   // D-193
	// D-214: the galaxy map array and the bakes the cache waits on (every frame, whatever renders:
	// generation waits on them too).
	F.GalMapSlots = GalaxyMaps.NumSlots();
	F.GalMapSize = GalaxyMaps.MapSize();
	F.GalMapConfig = GalaxyMaps.ConfigGeneration();
	F.GalMapBudget = uint32(FMath::Clamp(CVarUniGalaxyMapBudget.GetValueOnGameThread(), 4096, 1 << 24));
	for (const uint32 Slot : GalaxyMaps.PendingBakes())
	{
		const UniCore::FGalaxyMapCache::FEntry& E = GalaxyMaps.Entry(Slot);
		FUniFrameData::FGalMapBake& Bk = F.GalMapBakes.AddDefaulted_GetRef();
		Bk.Slot = Slot;
		Bk.Epoch = E.Epoch;
		Bk.bDust = E.Req.bDust ? 1u : 0u;
		Bk.Params[0] = E.Req.Par;
		Bk.Params[1] = E.Req.bDust ? E.Req.Dust : E.Req.Par;
	}
	FUniRenderer::Get().SetFrame_GameThread(MoveTemp(F));
}

// ---------------------------------------------------------------------------------------------
// Runtime API (D-193): Pick, QueryResident, FindNearest, events. The rules are UniCore's
// (UniQuery.h, Core/UniQuery.ush); this is the queueing, the frame bookkeeping and Blueprint.
// ---------------------------------------------------------------------------------------------

FUniObjectInfo UUniSubsystem::MakeInfo(const UniCore::FObjectInfo& I) const
{
	FUniObjectInfo R;
	R.bValid = true;
	R.Id = FUniObjectId::Make(I.Id);
	R.Planet = I.Planet;
	R.Coord = FUniCoord::Make(I.Coord);
	R.AbsoluteMagnitude = I.AbsMag;
	R.RadiusCm = I.RadiusCm;
	R.DistanceCm = I.DistanceCm;
	R.Seed = int32(I.Seed);
	if (I.Planet >= 0)
	{
		R.ArchetypeId = -1;
		R.Archetype = FName(TEXT("Planet"));
	}
	else
	{
		R.ArchetypeId = int32(I.Archetype);
		if (Model && I.Archetype < Model->Definition().Archetypes.size()) { R.Archetype = FName(UTF8_TO_TCHAR(Model->Definition().Arch(I.Archetype).Name.c_str())); }
	}
	return R;
}

FUniObjectInfo UUniSubsystem::BodyInfo(const UniCore::FPromotedBody& B) const
{
	FUniObjectInfo R;
	R.bValid = true;
	R.Coord = FUniCoord::Make(B.Coord);
	R.RadiusCm = B.RadiusCm;
	R.DistanceCm = UniCore::Length(UniCore::ToDouble(UniCore::Sub(B.Coord, Camera.ToCore())));
	R.Seed = int32(B.Id);
	R.Planet = -1;
	R.ArchetypeId = -1;
	R.Archetype = FName(B.bStar ? TEXT("Star") : TEXT("Planet"));
	if (Systems)
	{
		const auto& Act = Systems->ActiveSystems();
		const auto It = Act.find(B.SystemSeed);
		if (It != Act.end())
		{
			const UniCore::FActiveSystem& A = It->second;
			if (A.bStarIdKnown) { R.Id = FUniObjectId::Make(A.StarId); }
			if (B.bStar)
			{
				R.ArchetypeId = int32(A.StarArchetype);
				R.AbsoluteMagnitude = A.Sys.StarAbsMag;
				if (Model && A.StarArchetype < Model->Definition().Archetypes.size()) { R.Archetype = FName(UTF8_TO_TCHAR(Model->Definition().Arch(A.StarArchetype).Name.c_str())); }
			}
			else
			{
				for (int32 K = 0; K < int32(A.Sys.Planets.size()); ++K) { if (A.Sys.Planets[size_t(K)].Seed == B.Id) { R.Planet = K; break; } }
			}
		}
	}
	return R;
}

bool UUniSubsystem::BuildApiView(const FRotator& ViewRot, UniHlsl::FUniView& V, FVector2D* OutViewportPx) const
{
	V = UniHlsl::FUniView{};
	FVector2D Vp(0.0, 0.0);
	if (GEngine && GEngine->GameViewport) { GEngine->GameViewport->GetViewportSize(Vp); }
	if (OutViewportPx) { *OutViewportPx = Vp; }
	if (Vp.X < 1.0 || Vp.Y < 1.0) { return false; }
	float FovDeg = 90.0f;
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (PC->PlayerCameraManager) { FovDeg = PC->PlayerCameraManager->GetFOVAngle(); }
		}
	}
	const FRotationMatrix R(ViewRot);
	const FVector Fw = R.GetUnitAxis(EAxis::X), Rt = R.GetUnitAxis(EAxis::Y), Up = R.GetUnitAxis(EAxis::Z);
	V.Forward = UniHlsl::float3(float(Fw.X), float(Fw.Y), float(Fw.Z));
	V.Right = UniHlsl::float3(float(Rt.X), float(Rt.Y), float(Rt.Z));
	V.Up = UniHlsl::float3(float(Up.X), float(Up.Y), float(Up.Z));
	V.TanHalfFovX = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(FovDeg, 1.0f, 170.0f)) * 0.5f);
	V.TanHalfFovY = V.TanHalfFovX * float(Vp.Y / Vp.X);
	V.LimitingMag = UniHlsl::Uni_LimitingMag(EffectiveDeltaMag);
	V.SurfaceLimit = UniHlsl::UNI_MU_REF + FMath::Clamp(EffectiveDeltaMag, -5.0f, 5.0f);
	V.ExposureMag = V.LimitingMag - 1.0f + ExposureBias + TravelExposureMag;   // D-205
	V.ThetaPixel = FUniRenderer::Get().Stats.ThetaPixelLastFrame.load();
	V.CameraFraction = UniHlsl::float3(CameraFraction.X, CameraFraction.Y, CameraFraction.Z);
	return true;
}

int32 UUniSubsystem::QueuePick(FVector2D ScreenPosition, float RadiusPx, FApiPick&& P)
{
	P.Id = NextApiRequestId++;
	P.MinPx = FMath::Clamp(RadiusPx, 0.5f, 256.0f);
	FVector2D Vp(0.0, 0.0);
	if (GEngine && GEngine->GameViewport) { GEngine->GameViewport->GetViewportSize(Vp); }
	if (!bEnabled || !Streamer || Vp.X < 1.0 || Vp.Y < 1.0)
	{
		const FUniObjectInfo None = FUniObjectInfo();
		if (P.Native) { P.Native(false, None); }
		P.Bp.ExecuteIfBound(false, None);
		return P.Id;
	}
	P.CursorNdc = FVector2f(float(2.0 * ScreenPosition.X / Vp.X - 1.0), float(1.0 - 2.0 * ScreenPosition.Y / Vp.Y));
	const int32 Id = P.Id;
	PickQueue.Add(MoveTemp(P));
	return Id;
}

int32 UUniSubsystem::Pick(FVector2D ScreenPosition, FUniOnPicked OnResult, float RadiusPx)
{
	FApiPick P;
	P.Bp = OnResult;
	return QueuePick(ScreenPosition, RadiusPx, MoveTemp(P));
}

int32 UUniSubsystem::PickNative(FVector2D ScreenPosition, TFunction<void(bool, const FUniObjectInfo&)> OnResult, float RadiusPx)
{
	FApiPick P;
	P.Native = MoveTemp(OnResult);
	return QueuePick(ScreenPosition, RadiusPx, MoveTemp(P));
}

int32 UUniSubsystem::QueueQuery(FApiQuery&& Q)
{
	Q.Id = NextApiRequestId++;
	Q.MaxResults = FMath::Clamp(Q.MaxResults, 1, int32(FUniRenderer::MaxQueryResults()));
	const int32 Id = Q.Id;
	if (!bEnabled || !Streamer)
	{
		DeliverQuery(Q, TArray<FUniObjectInfo>(), 0);
		return Id;
	}
	QueryQueue.Add(MoveTemp(Q));
	return Id;
}

static uint32 ArchMaskOf(const TArray<int32>& Ids)
{
	uint32 Mask = 0u;
	for (int32 A : Ids) { if (A >= 0 && A < 32) { Mask |= 1u << uint32(A); } }
	return Mask;
}

int32 UUniSubsystem::QueryResident(const FUniCoord& Centre, double RadiusCm, const FUniQueryFilter& Filter, FUniOnQueryResult OnResult)
{
	FApiQuery Q;
	Q.Mode = UniHlsl::UNI_QUERY_MODE_SPHERE;
	Q.Centre = Centre;
	Q.RadiusCm = FMath::Max(RadiusCm, 0.0);
	Q.ArchMask = ArchMaskOf(Filter.ArchetypeIds);
	Q.MaxAbsMag = Filter.MaxAbsoluteMagnitude;
	Q.MaxResults = Filter.MaxResults;
	Q.Bp = OnResult;
	return QueueQuery(MoveTemp(Q));
}

int32 UUniSubsystem::QueryResidentNative(const FUniCoord& Centre, double RadiusCm, const FUniQueryFilter& Filter, TFunction<void(const TArray<FUniObjectInfo>&, int32)> OnResult)
{
	FApiQuery Q;
	Q.Mode = UniHlsl::UNI_QUERY_MODE_SPHERE;
	Q.Centre = Centre;
	Q.RadiusCm = FMath::Max(RadiusCm, 0.0);
	Q.ArchMask = ArchMaskOf(Filter.ArchetypeIds);
	Q.MaxAbsMag = Filter.MaxAbsoluteMagnitude;
	Q.MaxResults = Filter.MaxResults;
	Q.Native = MoveTemp(OnResult);
	return QueueQuery(MoveTemp(Q));
}

int32 UUniSubsystem::FindNearest(int32 ArchetypeId, const FUniCoord& From, FUniOnPicked OnResult)
{
	FApiQuery Q;
	Q.Mode = UniHlsl::UNI_QUERY_MODE_NEAREST;
	Q.Centre = From;
	Q.ArchMask = (ArchetypeId >= 0 && ArchetypeId < 32) ? (1u << uint32(ArchetypeId)) : 0u;
	Q.MaxResults = 16;   // ties at the minimum distance (normally one)
	Q.BpNearest = OnResult;
	if (Q.ArchMask == 0u) { Q.MaxAbsMag = -1.0e30f; }   // not a generated archetype (0..31): matches nothing
	return QueueQuery(MoveTemp(Q));
}

int32 UUniSubsystem::FindNearestNative(int32 ArchetypeId, const FUniCoord& From, TFunction<void(bool, const FUniObjectInfo&)> OnResult)
{
	FApiQuery Q;
	Q.Mode = UniHlsl::UNI_QUERY_MODE_NEAREST;
	Q.Centre = From;
	Q.ArchMask = (ArchetypeId >= 0 && ArchetypeId < 32) ? (1u << uint32(ArchetypeId)) : 0u;
	Q.MaxResults = 16;
	Q.NativeNearest = MoveTemp(OnResult);
	if (Q.ArchMask == 0u) { Q.MaxAbsMag = -1.0e30f; }
	return QueueQuery(MoveTemp(Q));
}

void UUniSubsystem::DeliverQuery(FApiQuery& Q, TArray<FUniObjectInfo>&& Objects, int32 Total)
{
	if (Q.Mode == UniHlsl::UNI_QUERY_MODE_NEAREST)
	{
		const bool bFound = Objects.Num() > 0;
		const FUniObjectInfo Info = bFound ? Objects[0] : FUniObjectInfo();
		if (Q.NativeNearest) { Q.NativeNearest(bFound, Info); }
		Q.BpNearest.ExecuteIfBound(bFound, Info);
		return;
	}
	if (Q.Native) { Q.Native(Objects, Total); }
	Q.Bp.ExecuteIfBound(Objects, Total);
}

void UUniSubsystem::FillApiRequests(FUniFrameData& F)
{
	// At most two of each in flight: the renderer's rings hold three.
	if (PickQueue.Num() > 0 && PicksInFlight.Num() < 2)
	{
		FApiPick P = MoveTemp(PickQueue[0]);
		PickQueue.RemoveAt(0);
		F.PickRequestId = P.Id;
		F.PickCursorNdc = P.CursorNdc;
		F.PickMinPx = P.MinPx;
		P.SentFrame = FrameCounter;
		PicksInFlight.Add(MoveTemp(P));
	}
	if (QueryQueue.Num() > 0 && QueriesInFlight.Num() < 2)
	{
		FApiQuery Q = MoveTemp(QueryQueue[0]);
		QueryQueue.RemoveAt(0);
		UniHlsl::uint4 D[3];
		UniCore::QueryDeltaUint4(Q.Centre.ToCore(), Camera.ToCore(), D);   // the frame's camera: the cell table's
		for (int32 A = 0; A < 3; ++A) { F.QueryDelta[A] = FUintVector4(D[A].x, D[A].y, D[A].z, D[A].w); }
		F.QueryRequestId = Q.Id;
		F.QueryMode = Q.Mode;
		F.QueryRadiusCm = float(FMath::Min(Q.RadiusCm, 3.0e38));
		F.QueryArchMask = Q.ArchMask;
		F.QueryMaxAbsMag = Q.MaxAbsMag;
		F.QueryMaxResults = uint32(Q.MaxResults);
		Q.SentFrame = FrameCounter;
		QueriesInFlight.Add(MoveTemp(Q));
	}
}

void UUniSubsystem::ProcessApiReadbacks()
{
	FUniRenderer& R = FUniRenderer::Get();
	const UniCore::FCoord Cam = Camera.ToCore();
	TArray<FUniPickReadback> Picks;
	R.DrainPicks_GameThread(Picks);
	for (const FUniPickReadback& Pr : Picks)
	{
		const int32 Ix = PicksInFlight.IndexOfByPredicate([&Pr](const FApiPick& P) { return P.Id == Pr.RequestId; });
		if (Ix == INDEX_NONE) { continue; }
		FApiPick P = MoveTemp(PicksInFlight[Ix]);
		PicksInFlight.RemoveAt(Ix);
		FUniObjectInfo Info;
		bool bFound = false;
		const UniCore::FFrameCells* Then = Pr.bFound ? FrameCellTables.Find(Pr.FrameNumber) : nullptr;
		if (Then)
		{
			UniCore::FObjectId Id;
			int Planet = -1;
			UniCore::FCellKey RecordCell;
			// A cell regenerated or evicted since names nothing (stale): reported as not found.
			if (UniCore::NameSlot(*Then, *Streamer, Pr.CellIndex, Pr.Slot, Id, Planet, RecordCell))
			{
				Info = MakeInfo(UniCore::MakeObjectInfo(Id, RecordCell, Pr.Record, Cam, Planet));
				bFound = true;
			}
		}
		if (P.Native) { P.Native(bFound, Info); }
		P.Bp.ExecuteIfBound(bFound, Info);
	}
	TArray<FUniQueryReadback> Queries;
	R.DrainQueries_GameThread(Queries);
	for (FUniQueryReadback& Qr : Queries)
	{
		const int32 Ix = QueriesInFlight.IndexOfByPredicate([&Qr](const FApiQuery& Q) { return Q.Id == Qr.RequestId; });
		if (Ix == INDEX_NONE) { continue; }
		FApiQuery Q = MoveTemp(QueriesInFlight[Ix]);
		QueriesInFlight.RemoveAt(Ix);
		// Nearest the centre first (ties: cell index, slot), as UniCore::QueryPool orders them.
		Qr.Hits.Sort([](const UniCore::FQueryHit& A, const UniCore::FQueryHit& B)
		{
			if (A.DistanceCm != B.DistanceCm) { return A.DistanceCm < B.DistanceCm; }
			if (A.CellIndex != B.CellIndex) { return A.CellIndex < B.CellIndex; }
			return A.Slot < B.Slot;
		});
		TArray<FUniObjectInfo> Out;
		const UniCore::FFrameCells* Then = FrameCellTables.Find(Qr.FrameNumber);
		if (Then)
		{
			for (const UniCore::FQueryHit& H : Qr.Hits)
			{
				UniCore::FObjectId Id;
				int Planet = -1;
				UniCore::FCellKey RecordCell;
				if (!UniCore::NameSlot(*Then, *Streamer, H.CellIndex, H.Slot, Id, Planet, RecordCell)) { continue; }
				Out.Add(MakeInfo(UniCore::MakeObjectInfo(Id, RecordCell, H.Record, Cam, Planet)));
				if (Out.Num() >= Q.MaxResults) { break; }
			}
		}
		DeliverQuery(Q, MoveTemp(Out), int32(Qr.Total));
	}
	// Lost requests (no view drew their frame, or a readback ring was full): fail them.
	const uint64 LostAfter = 90;
	for (int32 I = PicksInFlight.Num() - 1; I >= 0; --I)
	{
		if (FrameCounter <= PicksInFlight[I].SentFrame + LostAfter) { continue; }
		FApiPick P = MoveTemp(PicksInFlight[I]);
		PicksInFlight.RemoveAt(I);
		const FUniObjectInfo None = FUniObjectInfo();
		if (P.Native) { P.Native(false, None); }
		P.Bp.ExecuteIfBound(false, None);
	}
	for (int32 I = QueriesInFlight.Num() - 1; I >= 0; --I)
	{
		if (FrameCounter <= QueriesInFlight[I].SentFrame + LostAfter) { continue; }
		FApiQuery Q = MoveTemp(QueriesInFlight[I]);
		QueriesInFlight.RemoveAt(I);
		DeliverQuery(Q, TArray<FUniObjectInfo>(), 0);
	}
}

void UUniSubsystem::FailApiRequests()
{
	TArray<FApiPick> Picks = MoveTemp(PickQueue);
	Picks.Append(MoveTemp(PicksInFlight));
	PickQueue.Reset();
	PicksInFlight.Reset();
	TArray<FApiQuery> Qs = MoveTemp(QueryQueue);
	Qs.Append(MoveTemp(QueriesInFlight));
	QueryQueue.Reset();
	QueriesInFlight.Reset();
	const FUniObjectInfo None = FUniObjectInfo();
	for (FApiPick& P : Picks) { if (P.Native) { P.Native(false, None); } P.Bp.ExecuteIfBound(false, None); }
	for (FApiQuery& Q : Qs) { DeliverQuery(Q, TArray<FUniObjectInfo>(), 0); }
}

void UUniSubsystem::UpdateApiEvents(const FRotator& ViewRot)
{
	const UniCore::FCoord Cam = Camera.ToCore();
	std::vector<UniCore::FObjectInfo> Res;
	std::vector<UniCore::FObjectId> Ev;
	if (ResidentEvents.ArchMask() != EventArchMask) { ResidentEvents.SetArchMask(EventArchMask, *Streamer, Cam, Res, Ev); }
	ResidentEvents.Update(*Streamer, Cam, Res, Ev);
	for (const UniCore::FObjectId& Id : Ev) { OnObjectEvicted.Broadcast(FUniObjectId::Make(Id)); }
	for (const UniCore::FObjectInfo& I : Res) { OnObjectResident.Broadcast(MakeInfo(I)); }
	if (bLogEvents)
	{
		LoggedResident += Res.size();
		LoggedEvicted += Ev.size();
		const double Now = FPlatformTime::Seconds();
		if (Now - LastEventLog > 1.0)
		{
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse events: %llu resident, %llu evicted in the last %.1f s; %d reported now, %d watched"),
				LoggedResident, LoggedEvicted, Now - LastEventLog, int32(ResidentEvents.ReportedCount()), int32(VisibilityWatch.Count()));
			LoggedResident = LoggedEvicted = 0;
			LastEventLog = Now;
		}
	}
	if (VisibilityWatch.Count() > 0)
	{
		UniHlsl::FUniView V;
		if (BuildApiView(ViewRot, V))
		{
			std::vector<UniCore::FObjectInfo> Vis;
			std::vector<UniCore::FObjectId> Hid;
			VisibilityWatch.Update(*Streamer, Cam, V, Vis, Hid);
			for (const UniCore::FObjectId& Id : Hid) { OnObjectHidden.Broadcast(FUniObjectId::Make(Id)); }
			for (const UniCore::FObjectInfo& I : Vis) { OnObjectVisible.Broadcast(MakeInfo(I)); }
		}
	}
}

int32 UUniSubsystem::GetArchetypeId(FName Name) const
{
	if (!Model) { return -1; }
	const std::vector<UniCore::FArchetype>& A = Model->Definition().Archetypes;
	for (size_t I = 0; I < A.size(); ++I)
	{
		if (!A[I].Name.empty() && FName(UTF8_TO_TCHAR(A[I].Name.c_str())) == Name) { return int32(I); }
	}
	return -1;
}

bool UUniSubsystem::GetKnownObject(const FUniObjectId& Id, FUniObjectInfo& OutObject) const
{
	OutObject = FUniObjectInfo();
	if (!Streamer) { return false; }
	const UniCore::FObjectId C = Id.ToCore();
	const UniCore::FObjectGPU* O = Streamer->FindObject(C);
	if (!O) { return false; }
	OutObject = MakeInfo(UniCore::MakeObjectInfo(C, C.Cell, *O, Camera.ToCore()));
	return true;
}

void UUniSubsystem::SetResidentEventArchetypes(const TArray<int32>& ArchetypeIds)
{
	EventArchMask = ArchMaskOf(ArchetypeIds);   // applied on the next tick (objects already resident reported then)
}

void UUniSubsystem::WatchObject(const FUniObjectId& Id) { VisibilityWatch.Watch(Id.ToCore()); }
void UUniSubsystem::UnwatchObject(const FUniObjectId& Id) { VisibilityWatch.Unwatch(Id.ToCore()); }

void UUniSubsystem::LogStats() const
{
	if (!Streamer) { UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: disabled")); return; }
	const UniCore::FUniStreamer::FStats S = Streamer->Stats();
	const FUniRenderStats& R = FUniRenderer::Get().Stats;
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: scene '%s' mode %s dm %.1f | cells ready %llu pending %llu | pool %llu / %u | batches %llu (gpu done %u failed %u) | regenerations %llu | max gather %llu | alloc failures %llu | worst latency %llu frames | last frame: %u cells drawn, %u promotion candidates | camera %s"),
		*SceneName, MotionMode == EUniMotionMode::Cruise ? TEXT("Cruise") : TEXT("Local"), EffectiveDeltaMag,
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

static TAutoConsoleVariable<int32> CVarUniLightRig(
TEXT("uu.LightRig"), 1, TEXT("M3 placeholder: a movable directional light from the nearest active star, so promoted planets are lit (0 off)."));
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
	{
		// D-193: promoted / demoted events (bodies keyed by seed), collected then broadcast.
		TArray<FUniObjectInfo> Demoted, Promoted;
		for (auto It = PromotedReported.CreateIterator(); It; ++It)
		{
			if (P.count(It.Key()) == 0) { Demoted.Add(It.Value()); It.RemoveCurrent(); }
		}
		for (const auto& KV : P)
		{
			if (!PromotedReported.Contains(KV.first))
			{
				const FUniObjectInfo I = BodyInfo(KV.second);
				PromotedReported.Add(KV.first, I);
				Promoted.Add(I);
			}
		}
		for (const FUniObjectInfo& I : Demoted) { OnBodyDemoted.Broadcast(I); }
		for (const FUniObjectInfo& I : Promoted) { OnBodyPromoted.Broadcast(I); }
	}
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

// ---------------------------------------------------------------------------------------------
// D-186: sky light from the backdrop
// ---------------------------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarUniSkyLight(TEXT("uu.SkyLight"), 1, TEXT("D-186: a movable sky light lit by the backdrop (the galaxy's glow and the web around the camera), refreshed on a worker thread. 0: none."));
static TAutoConsoleVariable<int32> CVarUniSkyLightSize(TEXT("uu.SkyLightSize"), 16, TEXT("D-186: sky light cube texels per face (each the mean of 2 x 2 rays)."));
static TAutoConsoleVariable<float> CVarUniSkyLightInterval(TEXT("uu.SkyLightInterval"), 2.0f, TEXT("D-186: seconds between sky light refreshes (a refresh starts only when the last one has finished). D-216: 1 -> 2 (each refresh is a sky light recapture, which waits on the GPU)."));
static TAutoConsoleVariable<float> CVarUniSkyLightBlend(TEXT("uu.SkyLightBlend"), 0.0f, TEXT("D-186: seconds over which a new sky light cube blends in. D-216: 1 -> 0: every blend step recaptured the sky light (a GPU wait: 20-60 ms hitches under load, uu.Perf's light rig peak); 0 swaps the cube in one step."));
static TAutoConsoleVariable<float> CVarUniSkyLightIntensity(TEXT("uu.SkyLightIntensity"), 1.0f, TEXT("D-186: sky light intensity; 1: the cube holds what a screen pixel of that sky shows."));
static TAutoConsoleVariable<float> CVarUniSkyLightMaxSpeed(TEXT("uu.SkyLightMaxSpeed"), 3.0857e18f, TEXT("D-216: the sky light refreshes only while the camera moves slower than this (cm/s; default 1 pc/s): faster, the sky changes every frame and nothing is lit by it (bodies are promoted near rest), and each refresh recaptures the sky light (a GPU wait). 0: always."));

void UUniSubsystem::StartSkyJob(const UniCore::FVolumeScene& Sc)
{
	if (CVarUniSkyLight.GetValueOnGameThread() == 0 || bSkyJob) { return; }
	const double MaxSpeed = double(CVarUniSkyLightMaxSpeed.GetValueOnGameThread());
	if (MaxSpeed > 0.0 && CameraSpeedCmPerS > MaxSpeed) { return; }   // D-216: not while travelling
	const double Now = FPlatformTime::Seconds();
	if (Now - SkyLastStart < double(FMath::Max(CVarUniSkyLightInterval.GetValueOnGameThread(), 0.05f))) { return; }
	SkyLastStart = Now;
	const int32 N = FMath::Clamp(CVarUniSkyLightSize.GetValueOnGameThread(), 4, 64);
	// A texel holds radiance x one screen pixel's solid angle: what the screen shows of that sky.
	const double ThetaPx = double(FUniRenderer::Get().Stats.ThetaPixelLastFrame.load());
	const double Scale = ThetaPx * ThetaPx;
	TSharedPtr<UniCore::FVolumeScene, ESPMode::ThreadSafe> Copy = MakeShared<UniCore::FVolumeScene, ESPMode::ThreadSafe>(Sc);
	SkyJob = Async(EAsyncExecution::ThreadPool, [Copy, N, Scale]() { return UniCore::BuildSkyCube(*Copy, N, Scale); });
	bSkyJob = true;
}

void UUniSubsystem::UpdateSkyLight()
{
	UWorld* World = GetWorld();
	if (CVarUniSkyLight.GetValueOnGameThread() == 0 || !World)
	{
		ClearSkyLight();
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (bSkyJob && SkyJob.IsReady())
	{
		const UniCore::FSkyCube Cube = SkyJob.Get();
		SkyJob = TFuture<UniCore::FSkyCube>();
		bSkyJob = false;
		const int32 N = Cube.N;
		UTextureCube* Tex = N > 0 ? UTextureCube::CreateTransient(N, N, PF_FloatRGBA) : nullptr;
		FTexturePlatformData* PD = Tex ? Tex->GetPlatformData() : nullptr;
		if (PD && PD->Mips.Num() > 0)
		{
			Tex->SRGB = false;
			FTexture2DMipMap& Mip = PD->Mips[0];
			const int64 Bytes = int64(6) * N * N * int64(sizeof(FFloat16Color));
			Mip.BulkData.Lock(LOCK_READ_WRITE);
			FFloat16Color* Dst = reinterpret_cast<FFloat16Color*>(Mip.BulkData.Realloc(Bytes));
			for (int64 I = 0; I < int64(6) * N * N; ++I)
			{
				const UniHlsl::float3 B = Cube.Texels[size_t(I)];
				Dst[I] = FFloat16Color(FLinearColor(FMath::Clamp(B.x, 0.0f, 60000.0f), FMath::Clamp(B.y, 0.0f, 60000.0f), FMath::Clamp(B.z, 0.0f, 60000.0f), 1.0f));
			}
			Mip.BulkData.Unlock();
			Tex->UpdateResource();
			SkyCubePrev = SkyCubeCur ? SkyCubeCur.Get() : Tex;
			SkyCubeCur = Tex;
			SkyBlendStart = Now;
			bSkyBlending = true;
		}
	}
	if (!SkyCubeCur) { return; }
	ASkyLight* L = SkyLightActor.Get();
	USkyLightComponent* C = L ? L->GetLightComponent() : nullptr;
	if (!L)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		L = World->SpawnActorDeferred<ASkyLight>(ASkyLight::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!L) { return; }
		C = L->GetLightComponent();
		if (C)
		{
			C->SetMobility(EComponentMobility::Movable);
			C->SourceType = ESkyLightSourceType::SLS_SpecifiedCubemap;
			C->bLowerHemisphereIsBlack = false;   // the sky is all around
		}
		L->FinishSpawning(FTransform::Identity);
		SkyLightActor = L;
		bSkyBlending = true;
	}
	if (!C) { return; }
	C->SetIntensity(FMath::Max(CVarUniSkyLightIntensity.GetValueOnGameThread(), 0.0f));
	if (bSkyBlending)
	{
		const float Blend = FMath::Max(CVarUniSkyLightBlend.GetValueOnGameThread(), 0.0f);
		const float A = Blend > 0.0f ? float(FMath::Clamp((Now - SkyBlendStart) / double(Blend), 0.0, 1.0)) : 1.0f;
		if (A < 1.0f && SkyCubePrev && SkyCubePrev != SkyCubeCur)
		{
			C->SetCubemapBlend(SkyCubePrev, SkyCubeCur, A);
		}
		else
		{
			C->SetCubemapBlend(SkyCubeCur, SkyCubeCur, 1.0f);   // clear any blend
			C->SetCubemap(SkyCubeCur);
			bSkyBlending = false;
		}
	}
}

void UUniSubsystem::ClearSkyLight()
{
	if (bSkyJob) { SkyJob.Wait(); SkyJob = TFuture<UniCore::FSkyCube>(); bSkyJob = false; }
	const UWorld* World = GetWorld();
	if (ASkyLight* L = SkyLightActor.Get(); L && World && !World->bIsTearingDown) { L->Destroy(); }
	SkyLightActor.Reset();
	SkyCubePrev = nullptr;
	SkyCubeCur = nullptr;
	bSkyBlending = false;
	SkyLastStart = -1.0e9;
}

void UUniSubsystem::LogClusters() const
{
	const UniScenes::FClusterTuning& T = ClusterTuning;
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: clusters %s (scene '%s') | fraction %.3f, LF M* %.2f alpha %.2f over %.1f..%.1f, bias power %.2f, node weight %.2f, radius x%.2f, core %.2f..%.2f, clumps <= %d (weight %.2f) | early types %.2f -> %.2f over %.2f | groups %s (%.0f dwarfs at the median)"),
		T.bEnabled ? TEXT("on") : TEXT("off"), *SceneName, T.Fraction, T.MStar, T.Alpha, T.BrightMag, T.FaintMag, T.BiasPower, T.NodeWeight, T.RadiusScale, T.CoreMin, T.CoreMax, T.MaxClumps, T.ClumpWeight,
		T.EarlyCore, T.EarlyOuter, T.EarlyScale, T.bGroups ? TEXT("on") : TEXT("off"), T.GroupDwarfs);
	if (!Streamer) { return; }
	const UniCore::FCoord Cam = Camera.ToCore();
	// D-168: clusters far from any window they feed sleep (not in Contributors()), so count the
	// resident objects themselves. D-177: groups are labels (no emitter): every resident object.
	auto Census = [&](UniCore::u32 Arch, double RichMag, int32& N, int32& Rich, double& Nearest, double& NearestRich)
	{
		N = Rich = 0; Nearest = NearestRich = 0.0;
		Streamer->ForEachObjectOf(Arch, [&](const UniCore::FCoord& C, const UniCore::FObjectGPU& O)
		{
			const double M = double(UniHlsl::Uni_ObjectAbsMag(UniCore::ToHlsl(O)));
			const double D = UniCore::Length(UniCore::ToDouble(UniCore::Sub(C, Cam)));
			++N;
			if (N == 1 || D < Nearest) { Nearest = D; }
			if (M < RichMag) { ++Rich; if (Rich == 1 || D < NearestRich) { NearestRich = D; } }
		});
	};
	int32 N = 0, Rich = 0;
	double Nearest = 0.0, NearestRich = 0.0;
	Census(UniScenes::kCluster, -25.5, N, Rich, Nearest, NearestRich);
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  %d clusters resident (%d rich, M < -25.5); nearest %s, nearest rich %s. uu.GotoCluster [rich] jumps to one."),
		N, Rich, N ? UTF8_TO_TCHAR(UniCore::FormatDistance(Nearest).c_str()) : TEXT("-"), Rich ? UTF8_TO_TCHAR(UniCore::FormatDistance(NearestRich).c_str()) : TEXT("-"));
	Census(UniScenes::kGroup, -18.0, N, Rich, Nearest, NearestRich);
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  %d groups resident (%d rich, M < -18: ~50+ dwarfs); nearest %s, nearest rich %s. uu.GotoGroup [rich] jumps to one."),
		N, Rich, N ? UTF8_TO_TCHAR(UniCore::FormatDistance(Nearest).c_str()) : TEXT("-"), Rich ? UTF8_TO_TCHAR(UniCore::FormatDistance(NearestRich).c_str()) : TEXT("-"));
}

bool UUniSubsystem::GoToCluster(bool bRich) { return GoToHost(UniScenes::kCluster, bRich ? -25.5 : 99.0, TEXT("cluster")); }
bool UUniSubsystem::GoToGroup(bool bRich) { return GoToHost(UniScenes::kGroup, bRich ? -18.0 : 99.0, TEXT("group")); }

bool UUniSubsystem::GoToHost(UniCore::u32 Arch, double BrighterThan, const TCHAR* What)
{
	if (!bEnabled || !Streamer || !Model) { return false; }
	const UniCore::FCoord Cam = Camera.ToCore();
	const UniCore::FArchetype& A = Model->Definition().Arch(Arch);
	// D-177: a group is a label (no emitter): its extent is ~3 radii (its dwarfs and the web's
	// galaxies there come from the map, not from it).
	const UniCore::FUniEmitter* E = A.Emitters.empty() ? nullptr : &A.Emitters[0];
	const double InflMul = E ? E->InfluenceMul : 3.0;
	bool bFound = false;
	UniCore::FCoord Best;
	double BestD = 0.0, BestM = 0.0, BestInfl = 0.0;
	Streamer->ForEachObjectOf(Arch, [&](const UniCore::FCoord& C, const UniCore::FObjectGPU& O)
	{
		const UniHlsl::FUniObject H = UniCore::ToHlsl(O);
		const double M = double(UniHlsl::Uni_ObjectAbsMag(H));
		if (!(M < BrighterThan)) { return; }
		const double D = UniCore::Length(UniCore::ToDouble(UniCore::Sub(C, Cam)));
		if (bFound && D >= BestD) { return; }
		bFound = true; Best = C; BestD = D; BestM = M;
		BestInfl = InflMul * std::exp2(double(UniHlsl::Uni_ObjectRadiusLog2(H)));
	});
	if (!bFound)
	{
		UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: no %s%s resident yet (uu.Enable web or flythrough, then give its level a moment to fill)."), BrighterThan < 90.0 ? TEXT("rich ") : TEXT(""), What);
		return false;
	}
	// 3 influence radii out along the current direction from it (or +x), facing its centre; free
	// flight at a speed that crosses it in about 10 s.
	UniCore::FVec3d Dir = UniCore::ToDouble(UniCore::Sub(Cam, Best));
	const double L = UniCore::Length(Dir);
	Dir = L > 0.0 ? UniCore::FVec3d{ Dir.X / L, Dir.Y / L, Dir.Z / L } : UniCore::FVec3d{ 1.0, 0.0, 0.0 };
	const double Out = 3.0 * BestInfl;
	UniCore::FCoord To = Best;
	UniCore::FVec3f Frac{};
	UniCore::AddDisplacement(To, Frac, UniCore::FVec3d{ Dir.X * Out, Dir.Y * Out, Dir.Z * Out });
	SetFreeFly(true, 0.2 * BestInfl);
	SetFlySpeed(0.2 * BestInfl);
	SetCamera(FUniCoord::Make(To));
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr) { PC->SetControlRotation(FVector(-Dir.X, -Dir.Y, -Dir.Z).Rotation()); }
	// Members: a cluster's from its emitter (its fainter members) plus its authored bright ones
	// (D-177); a group's dwarfs from its light (D-174: its magnitude is their light), now drawn
	// from the node matter around it.
	double Members = 0.0;
	if (E) { Members = (std::isfinite(E->NTotalRefMag) ? E->NTotal * std::pow(10.0, -0.4 * (BestM - E->NTotalRefMag)) : E->NTotal) * E->SpriteKeep; }   // D-197: those generated
	if (Arch == UniScenes::kCluster) { for (const UniCore::FCatalogue& Cat : Model->Definition().Catalogues) { if (Cat.ArchetypeId == Arch && Cat.bBrightMembers) { Members *= 1.0 + Cat.BrightRatio; } } }
	if (Arch == UniScenes::kGroup) { Members = std::pow(10.0, -0.4 * BestM) / UniScenes::DwarfMeanFluxV(); }
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: jumped to a %s of M = %.2f (~%.0f %s) that was %s away; now %s from its centre."),
		What, BestM, Members, Arch == UniScenes::kGroup ? TEXT("dwarfs' worth of light, plus the web's bright galaxies there") : TEXT("galaxies"), UTF8_TO_TCHAR(UniCore::FormatDistance(BestD).c_str()), UTF8_TO_TCHAR(UniCore::FormatDistance(Out).c_str()));
	return true;
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

	FAutoConsoleCommandWithWorldAndArgs CmdClusters(TEXT("uu.Clusters"), TEXT("Log the cluster tuning and the resident clusters (D-151)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->LogClusters(); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdGotoGroup(TEXT("uu.GotoGroup"), TEXT("uu.GotoGroup [rich]: jump to 3 influence radii from the nearest resident galaxy group (rich: M < -18, ~50+ dwarfs). D-174."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->GoToGroup(Args.Num() > 0 && Args[0].Equals(TEXT("rich"), ESearchCase::IgnoreCase)); }
		}));
	FAutoConsoleCommandWithWorldAndArgs CmdGotoCluster(TEXT("uu.GotoCluster"), TEXT("uu.GotoCluster [rich]: jump to 3 influence radii from the nearest resident cluster (rich: M < -25.5)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->GoToCluster(Args.Num() > 0 && Args[0].Equals(TEXT("rich"), ESearchCase::IgnoreCase)); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdSystems(TEXT("uu.Systems"),
 TEXT("Log active star systems and their planets."),
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
	FAutoConsoleCommandWithWorldAndArgs CmdPerf(TEXT("uu.Perf"), TEXT("Log the universe's per-phase cost: averages, peaks since the last uu.Perf (then reset), the worst frame's streamer breakdown (D-157)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->LogPerf(); }
		}));

	// D-193: the runtime API from the console (results are logged when they arrive).
	FString ObjectLine(const FUniObjectInfo& I)
	{
		return FString::Printf(TEXT("%s%s %s at %s, M %.2f, radius %s, seed %08x"),
			*I.Archetype.ToString(), I.Planet >= 0 ? *FString::Printf(TEXT(" (planet %d of star)"), I.Planet) : TEXT(""),
			*UUniCoordLibrary::ObjectIdToString(I.Id), UTF8_TO_TCHAR(UniCore::FormatDistance(I.DistanceCm).c_str()), I.AbsoluteMagnitude,
			UTF8_TO_TCHAR(UniCore::FormatDistance(I.RadiusCm).c_str()), uint32(I.Seed));
	}

	FAutoConsoleCommandWithWorldAndArgs CmdPick(TEXT("uu.Pick"), TEXT("uu.Pick [x y]: log the object under the screen centre (or viewport pixel x, y). D-193."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UUniSubsystem* S = Sub(World);
			if (!S) { return; }
			FVector2D Pos(0.0, 0.0);
			if (Args.Num() >= 2) { Pos = FVector2D(FCString::Atod(*Args[0]), FCString::Atod(*Args[1])); }
			else if (GEngine && GEngine->GameViewport) { GEngine->GameViewport->GetViewportSize(Pos); Pos *= 0.5; }
			S->PickNative(Pos, [Pos](bool bFound, const FUniObjectInfo& I)
			{
				if (bFound) { UE_LOG(LogUnifiedUniverse, Log, TEXT("uu.Pick (%.0f, %.0f): %s"), Pos.X, Pos.Y, *ObjectLine(I)); }
				else { UE_LOG(LogUnifiedUniverse, Log, TEXT("uu.Pick (%.0f, %.0f): nothing there"), Pos.X, Pos.Y); }
			});
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdQuery(TEXT("uu.Query"), TEXT("uu.Query <radius pc> [archetype name]: log the resident objects within the radius of the camera (nearest 20). D-193."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UUniSubsystem* S = Sub(World);
			if (!S || Args.Num() < 1) { return; }
			const double Radius = FCString::Atod(*Args[0]) * 3.0856775814913673e18;
			FUniQueryFilter Filter;
			Filter.MaxResults = 4096;
			if (Args.Num() >= 2)
			{
				const int32 A = S->GetArchetypeId(FName(*Args[1]));
				if (A < 0) { UE_LOG(LogUnifiedUniverse, Warning, TEXT("uu.Query: no archetype '%s'"), *Args[1]); return; }
				Filter.ArchetypeIds.Add(A);
			}
			S->QueryResidentNative(S->GetCameraCoord(), Radius, Filter, [Radius](const TArray<FUniObjectInfo>& Objects, int32 Total)
			{
				UE_LOG(LogUnifiedUniverse, Log, TEXT("uu.Query %s: %d matches, %d returned"), UTF8_TO_TCHAR(UniCore::FormatDistance(Radius).c_str()), Total, Objects.Num());
				for (int32 I = 0; I < FMath::Min(Objects.Num(), 20); ++I) { UE_LOG(LogUnifiedUniverse, Log, TEXT("  %s"), *ObjectLine(Objects[I])); }
			});
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdNearest(TEXT("uu.Nearest"), TEXT("uu.Nearest <archetype name>: log the nearest resident object of an archetype. D-193."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UUniSubsystem* S = Sub(World);
			if (!S || Args.Num() < 1) { return; }
			const int32 A = S->GetArchetypeId(FName(*Args[0]));
			if (A < 0) { UE_LOG(LogUnifiedUniverse, Warning, TEXT("uu.Nearest: no archetype '%s'"), *Args[0]); return; }
			const FString Name = Args[0];
			S->FindNearestNative(A, S->GetCameraCoord(), [Name](bool bFound, const FUniObjectInfo& I)
			{
				if (bFound) { UE_LOG(LogUnifiedUniverse, Log, TEXT("uu.Nearest %s: %s"), *Name, *ObjectLine(I)); }
				else { UE_LOG(LogUnifiedUniverse, Log, TEXT("uu.Nearest %s: none resident"), *Name); }
			});
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdEvents(TEXT("uu.Events"), TEXT("uu.Events [archetype name ...|off]: resident / evicted events for these archetypes, counts logged each second. D-193."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UUniSubsystem* S = Sub(World);
			if (!S) { return; }
			TArray<int32> Ids;
			for (const FString& A : Args)
			{
				if (A.Equals(TEXT("off"), ESearchCase::IgnoreCase)) { Ids.Reset(); break; }
				const int32 Id = S->GetArchetypeId(FName(*A));
				if (Id >= 0) { Ids.Add(Id); } else { UE_LOG(LogUnifiedUniverse, Warning, TEXT("uu.Events: no archetype '%s'"), *A); }
			}
			S->SetResidentEventArchetypes(Ids);
			S->SetEventLogging(Ids.Num() > 0);
		}));
}
