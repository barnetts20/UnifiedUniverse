#include "UniSubsystem.h"
#include "UniTuningSettings.h"

#include "UnifiedUniverseModule.h"
#include "UniDemoScenes.h"
#include "UniHome.h"   // D-293: the home scene (UniSubsystemPlanets.cpp may land in another unity file)
#include "UniRenderer.h"
#include "UniRenderHooks.h"
#include "UniStreamer.h"
#include "UniCompact.h"   // D-272

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
#include "Engine/Texture2D.h"   // D-246: the blue noise
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
	TAutoConsoleVariable<float> DustHeight(TEXT("uu.Galaxy.DustHeight"), float(Defaults.DustHeight), TEXT("D-263: galaxies' dust layer scale height x this (how fast the dust falls off from the disc's plane; the stars' is unchanged). The face-on optical depth is kept, so a thinner layer is denser: darker edge-on and along the plane (rays in the disc end sooner), the same face-on. 1: half the stars' height (0.67: a third, ~real: the Milky Way's ~100 pc of dust under ~300 pc of stars). Render-only. Applies at uu.Enable."));
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
		T.DustHeight = FMath::Clamp(double(DustHeight.GetValueOnGameThread()), 0.05, 4.0);   // D-263
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
static TAutoConsoleVariable<int32> CVarUniGalaxyMapSize(TEXT("uu.GalaxyMap.Size"), 512, TEXT("D-212, D-229: the galaxy map's texels a side (a power of two, 256..4096). 512 (default, D-229) ~2.7 MB with mips, a quarter of 1024's bake: within 0.3% in light of 1024 everywhere, per-pixel p95 up to 1.7% within ~0.5 kpc of a core (1024: 0.8%). 1024 ~11 MB; 2048 ~43 MB. Changing it rebakes (stars placed from it change where an accept decision flips)."));
static TAutoConsoleVariable<int32> CVarUniGalaxyMapBudget(TEXT("uu.GalaxyMap.Budget"), 1048576, TEXT("D-212: texels of the galaxy maps the GPU bakes a frame (rows of a map): 1M bakes a 1024^2 map in one frame, 2048^2 in 4. The glow uses the field until the camera's galaxy's map is done; D-214: a galaxy's stars wait for theirs."));
static TAutoConsoleVariable<int32> CVarUniGalaxyMapSlots(TEXT("uu.GalaxyMap.Slots"), 8, TEXT("D-214, D-229: galaxy maps kept on the GPU at once (1..16; ~2.7 MB each at 512^2, ~11 MB at 1024^2): every galaxy stars are placed in, and the camera's own. A galaxy beyond them waits (glow) until a slot frees (unused for 64 frames). 8 at 512^2 is half 4 at 1024^2's memory. Changing it rebakes."));
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
// D-230: the travel look (render only, art choices): the motion web and the travel shift. D-231:
// the motion web its own ramp from the speed; D-268: the travel shift too (uu.TravelLook.* gone).
static TAutoConsoleVariable<float> CVarUniMotionWeb(TEXT("uu.MotionWeb"), 0.01f, TEXT("D-230: the motion web: while travelling, the cosmic web's filaments and nodes glow faintly so the structure shows motion (render only). Strength at full travel: a galaxy disc's surface brightness per Mpc of web (0.01: a few-Mpc filament at a few % of a disc). 0: off."));
static TAutoConsoleVariable<float> CVarUniMotionWebStartKpcS(TEXT("uu.MotionWeb.StartKpcS"), 5.0f, TEXT("D-231: the speed (kpc/s) where the motion web starts to come in (crossing a galaxy in a second or two, ~15-50 kpc/s, shows it at about half strength)."));
static TAutoConsoleVariable<float> CVarUniMotionWebFullKpcS(TEXT("uu.MotionWeb.FullKpcS"), 5000.0f, TEXT("D-231: the speed (kpc/s) where the motion web is at full strength (ease-out on log speed between: it comes in early and reaches full slowly)."));
static TAutoConsoleVariable<float> CVarUniMotionWebDetail(TEXT("uu.MotionWeb.Detail"), 1.0f, TEXT("D-231: the motion web's organic detail: one galaxy_detail read (its ridged filaments) multiplies it. 0: none, 1: the full pattern."));
static TAutoConsoleVariable<float> CVarUniMotionWebStepMpc(TEXT("uu.MotionWeb.StepMpc"), 0.5f, TEXT("D-231: with the detail on, the background march's step cap (Mpc) where the motion web is (half its near distance to its depth), so the detail is sampled instead of swimming (~2x the background's steps while travelling). 0: no cap (free; the detail then blurs away over the march's long steps)."));
static TAutoConsoleVariable<float> CVarUniMotionWebDetailMpc(TEXT("uu.MotionWeb.DetailMpc"), 20.0f, TEXT("D-231: the detail texture's period (Mpc; rounded to a whole number per 200 Mpc web tile). Its ridges are 1/4 to 1/32 of it."));
static TAutoConsoleVariable<float> CVarUniMotionWebRegionMpc(TEXT("uu.MotionWeb.RegionMpc"), 200.0f, TEXT("D-233: the period (Mpc; at most one per 200 Mpc web tile, else a whole number per tile) of the regions over which the detail's look changes: filaments, knots, cloud or cells, mixed by a smooth second read of the detail texture (regions ~50 Mpc across at 200). 0: the ridged filaments everywhere (D-231)."));
static TAutoConsoleVariable<float> CVarUniNebula(TEXT("uu.Nebula"), 0.0f, TEXT("D-260: off by default (owner: the shaped knots and structure are closer to the nebulae wanted, without a layer of their own). D-234: emission nebulae (H II regions) in galaxies: pink glows on the arms' star-forming knots, in a gas layer as thin as the dust, at this gain x the galaxy's full light per star (they stay as the stars resolve). Galaxies with a palette use its knot colour. 0: off."));
static TAutoConsoleVariable<float> CVarUniNebulaReflect(TEXT("uu.Nebula.Reflect"), 6.0f, TEXT("D-234: reflection nebulae: galaxies' dust lit blue by the young clusters around it (the knots), at this gain. 0: off."));
static TAutoConsoleVariable<float> CVarUniNebulaFine(TEXT("uu.Nebula.Fine"), 1.0f, TEXT("D-237, D-241: the nebulae's detail layers: their structure eroded by warped walls (~40 pc in the Milky Way) and texture (~10 pc), with dark lanes in the dust, near the camera; level of detail by the pixel's footprint. 0: off (their mean)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaDark(TEXT("uu.Nebula.Dark"), 0.35f, TEXT("D-242: the nebulae's mean dark share: each spiral nebula balances glow (an H II region) against dark dust (an opaque cloud glowing at its rims, seen up close; from afar the dust is D-241's) by its own share around this mean. 0: all glow (~14% brighter), 1: all dust; 0.35 keeps D-241's light. Below 0: off (D-241)."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarUniNebulaRefine(TEXT("uu.Nebula.Refine"), 1, TEXT("D-244: inside a spiral's gas layer, a long march step is split into up to this many sub-samples about a nebula knot texel apart, each evaluating the whole galaxy field (CloudAtmosphere-style fine steps inside the structure): far knots stay sharp this many times farther out (offline at 4: ~2.5x the galaxy work along the disc, one frame's noise 6% -> 12%). 0 or 1: off."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaFineReachKpc(TEXT("uu.Nebula.FineReachKpc"), 7.0f, TEXT("D-237, D-241: the nebulae's detail layers out to this distance, faded over its second half at each sample's own distance. D-248: 1 -> 7 (owner)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalaxyDiffuseDust(TEXT("uu.Galaxy.DiffuseDust"), 1.0f, TEXT("D-250: dusty galaxies' low background dust (render only), x this opacity: along the disc's mid-plane at ~the solar circle it reaches tau 1 over the galaxy's own reach (uu.Galaxy.DustReachMinKpc..MaxKpc by its seed), so the far side of the disc hazes out; looking out of the plane stays clear. It fades in past the galaxy's sprite handoff (D-245). 0: off."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalaxyDustReachMinKpc(TEXT("uu.Galaxy.DustReachMinKpc"), 0.75f, TEXT("D-250: the shortest diffuse-dust reach a galaxy rolls (kpc; log-uniform to uu.Galaxy.DustReachMaxKpc)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalaxyDustReachMaxKpc(TEXT("uu.Galaxy.DustReachMaxKpc"), 10.0f, TEXT("D-250: the longest diffuse-dust reach a galaxy rolls (kpc)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniVolumeStepFoot(TEXT("uu.Volume.StepFoot"), 0.5f, TEXT("D-255: the camera's galaxy's map read over at least this x the march's step along the disc (the cone rule: one sample stands for its step; 0: the pixel's footprint alone)."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarUniVolumeSegmentTaps(TEXT("uu.Volume.SegmentTaps"), 4, TEXT("D-262 (paradigm C; default 4: against a fine reference 0.67% per pixel and a 1.3% spread over jitter phases, D-255's one read 3.1% and 4.7%, at ~2x the march's cost on the CPU): the camera's galaxy's march step read at up to this many points along its run in the disc's plane, each over the pixel's footprint and uu.Volume.StepFoot x its share of the run (sharp across the ray, averaged along it). 1: one read, D-255's (StepFoot blurs across the ray too)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniVolumeOpaqueCutoff(TEXT("uu.Volume.OpaqueCutoff"), 0.001f, TEXT("D-262: a volume ray stops when its transmittance falls under this (every channel). Higher ends rays in thick dust sooner (edge-on discs: the costliest rays): 0.01-0.02 is common in cloud renderers; the light behind is lost. 0.001: the old constant."), ECVF_Default);
// D-265: the camera's galaxy's detail layers (render only; the map path's march).
static TAutoConsoleVariable<float> CVarUniGalDetailFreq0(TEXT("uu.Galaxy.Detail.Freq0"), 4.0f, TEXT("D-265: detail layer 0's frequency, x the galaxy's detail texture's (its knots' scale; the Milky Way's: an 8 kpc period, 62 pc texels): 4 = features a quarter the size (2 kpc, 16 pc texels; at the defaults resolved to ~1.2 kpc from the camera). Read from galaxy_shape (mean 1 at every mip), render only, near the camera: past where the march's step resolves it, it fades to its mean. 0: off."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailLight0(TEXT("uu.Galaxy.Detail.Light0"), 0.5f, TEXT("D-265: detail layer 0's light amplitude (0..1): the light x 1 + this x (the shaped fBm - 1). Mean 1: the brightness is kept."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailDust0(TEXT("uu.Galaxy.Detail.Dust0"), 0.5f, TEXT("D-265: detail layer 0's dust amplitude (0..1): the dust x 1 + this x (the shaped ridged channel - 1)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailFreq1(TEXT("uu.Galaxy.Detail.Freq1"), 16.0f, TEXT("D-265: detail layer 1's frequency, x the detail texture's (16: a sixteenth the size, the Milky Way's 500 pc and 4 pc texels; its reach a quarter of layer 0's, ~300 pc at the defaults). 0: off."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailLight1(TEXT("uu.Galaxy.Detail.Light1"), 0.5f, TEXT("D-265: detail layer 1's light amplitude (0..1)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailDust1(TEXT("uu.Galaxy.Detail.Dust1"), 0.5f, TEXT("D-265: detail layer 1's dust amplitude (0..1)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailSteps(TEXT("uu.Galaxy.Detail.Steps"), 16.0f, TEXT("D-265: the steps each detail layer may spend at its resolving step past where the march's own step reaches it (its reach: each finer layer a shorter reach for the same steps). More: the detail further out, a costlier march in the disc. 0: no step cap (the layers only read, faded by the march's own step)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailStepTexels(TEXT("uu.Galaxy.Detail.StepTexels"), 4.0f, TEXT("D-265: a detail layer's resolving step in its texels (dust: one step; light: uu.Volume.SegmentTaps taps a step). Smaller: sharper, more steps."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailFoot(TEXT("uu.Galaxy.Detail.Foot"), 0.5f, TEXT("D-265: a detail layer is read over at least this x its share of the step (the fade where the step does not resolve it: mean-kept blur instead of grain). 0: 0.5."), ECVF_Default);
// D-266: the bubble of detail about the camera.
static TAutoConsoleVariable<float> CVarUniGalDetailRange0(TEXT("uu.Galaxy.Detail.Range0"), 0.33f, TEXT("D-266: detail layer 0 (the mid detail) reaches this far from the camera, in the galaxy's radii (the Milky Way's 24 kpc: 0.33 ~ 8 kpc), fading in from the coarse structure over uu.Galaxy.Detail.Fade0 of it; past it the layer is not read (no cost). 0: no limit (fades only by the march's step)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailFade0(TEXT("uu.Galaxy.Detail.Fade0"), 1.0f, TEXT("D-266: the share of layer 0's range it fades in over (1: all of it, full strength only at the camera; 0.5: full within half its range, fading over the outer half)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailRange1(TEXT("uu.Galaxy.Detail.Range1"), 0.08f, TEXT("D-266: detail layer 1 (the fine detail) reaches this far from the camera, in the galaxy's radii (0.08 ~ 2 kpc in the Milky Way: a quarter of layer 0's, as its frequency is 4x); past it not read. 0: no limit."), ECVF_Default);
// D-267: the knots in the detail layers (the nebulae as part of the field).
static TAutoConsoleVariable<float> CVarUniGalDetailKnots0(TEXT("uu.Galaxy.Detail.Knots0"), 0.5f, TEXT("D-267: detail layer 0's knot amplitude (0..1): the knots' young light x 1 + this x (the layer's knot channel, G^8 over its mean, - 1): each knot of the map breaks into the layer's clumps (the Milky Way's ~16 pc texels). Mean 1: the light kept."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailKnots1(TEXT("uu.Galaxy.Detail.Knots1"), 0.5f, TEXT("D-267: detail layer 1's knot amplitude (0..1): the clumps within the clumps (~4 pc texels)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailKnotTint(TEXT("uu.Galaxy.Detail.KnotTint"), 0.5f, TEXT("D-267: the knots' young light toward the galaxy's nebula colours (its three, D-236), luminance kept, by the knots' share of the light: every galaxy at any distance (render only). 0: the stars' colours."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalDetailFade1(TEXT("uu.Galaxy.Detail.Fade1"), 0.5f, TEXT("D-266: the share of layer 1's range it fades in over (0.5: full strength within half its range: the bubble of detail about the camera)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniGalaxyDiffuseDustHeight(TEXT("uu.Galaxy.DiffuseDustHeight"), 1.5f, TEXT("D-250: the diffuse dust's scale height over the galaxy's dust layer's (thicker than the lanes)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaScale(TEXT("uu.Nebula.Scale"), 1.0f, TEXT("D-248: the nebulae's size (x 0.25..4; 2: twice as large, half as many across the disc). Away from 1 they leave the stars' knots (the young clusters stay where they are). Render only."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaPrevalence(TEXT("uu.Nebula.Prevalence"), 1.0f, TEXT("D-248: how much of the gas layer is nebula, over the default's (~11%): 0.5 half as much, 2 twice (x0.05..x5.6). Each nebula keeps its brightness, so their light grows with it. Render only."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaFineScale(TEXT("uu.Nebula.FineScale"), 1.0f, TEXT("D-248: the nebulae's detail layers' size (x; walls ~40 pc and texture ~10 pc in the Milky Way at 1): larger is softer, less detail inside a march step (less grain). Render only."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaFineOctave(TEXT("uu.Nebula.FineOctave"), 4.0f, TEXT("D-248: the texture layer's frequency over the walls' (4: a quarter of their size). Render only."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaStepRatio(TEXT("uu.Nebula.StepRatio"), 0.08f, TEXT("D-251: the march's step ratio (step = this x the distance) through a spiral's gas layer within the nebulae's detail reach (uu.Nebula.FineReachKpc), where the nebulae's detail is: the detail sampled along the ray instead of drawn at one jittered point of a long step (the grain). 0: the carrier's own ratio (uu.VolumeStepRatio's hints). Render only."), ECVF_Default);
// D-252: the nebula grid (UniCore/UniNebGrid.h; Core/UniVolume.ush THE NEBULA GRID). Render only.
static TAutoConsoleVariable<int32> CVarUniNebulaGrid(TEXT("uu.Nebula.Grid"), 1, TEXT("D-252: the nebula grid: the camera's spiral's nebulae baked into galaxy-local cells (each the average of its cell, the same from any camera), read along each step instead of drawn at one jittered point (the grain): softer, stable. 0: off (D-251's march)."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarUniNebulaGridCells(TEXT("uu.Nebula.GridCells"), 128, TEXT("D-252: the grid's cells across a level (128 or 256): 256 is sharper at mid distance (cells half as wide) for about 4x the memory (uu.Perf shows it)."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarUniNebulaGridSamples(TEXT("uu.Nebula.GridSamples"), 1, TEXT("D-252: the bake's samples a grid point (1, or 8: a 2 x 2 x 2 stratified average, 8x the bake's cost; slightly closer to the converged look)."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarUniNebulaGridBudget(TEXT("uu.Nebula.GridBudget"), 128, TEXT("D-252: the grid's bricks (8^3 cells) processed a frame at most: classified and, holding a nebula, baked. Fewer: cheaper, and a fresh grid or fast travel takes longer to fill (the next level is read meanwhile)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaGridFetches(TEXT("uu.Nebula.GridFetches"), 32.0f, TEXT("D-252: a march step's grid reads at most (a longer step reads a coarser level)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaGridCellPc(TEXT("uu.Nebula.GridCellPc"), 4.0f, TEXT("D-252: the finest level's cell (pc in the Milky Way, scaled with the galaxy's size); each level doubles it."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaGridReachKpc(TEXT("uu.Nebula.GridReachKpc"), 64.0f, TEXT("D-252: the grid's levels cover at least this far from the camera (kpc; at most 12 levels); beyond, the march's field, whose knots blend to their mean with distance (D-241). D-253: 8 -> 32 (owner: the nebulae were lost past a distance). D-254: 32 -> 64, and the grid follows the nearest spiral whose disc is within it (from outside too)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaGridPool(TEXT("uu.Nebula.GridPool"), 1.25f, TEXT("D-252: the grid's pool slots over the share of bricks expected to hold a nebula (past it a brick is read from the next level; uu.Perf shows the overflow)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaGridLayer(TEXT("uu.Nebula.GridLayer"), 3.0f, TEXT("D-252: the grid's levels cover the gas layer to this many of its scale heights either side of its mid-plane."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaPeak(TEXT("uu.Nebula.Peak"), 200.0f, TEXT("D-253: the nebulae's brightest parts held softly under this x the nebulae's mean density (a nebula's median is ~20x, its cores up to ~800x): lower dims the cores more, the faint parts kept. 0: off (D-252's)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaFineStepLod(TEXT("uu.Nebula.FineStepLod"), 0.0f, TEXT("D-251: the nebulae's detail is prefiltered over at least this x the march's step (on top of the pixel's footprint): detail finer than a step, which a ray cannot resolve, is averaged instead of drawn at a jittered point of the step (the grain). 0: the pixel's footprint alone (D-241..D-250). Render only."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaFineLodBias(TEXT("uu.Nebula.FineLodBias"), 0.0f, TEXT("D-248: mips added to the detail layers' reads by the pixel's footprint: +1 blurs each layer an octave sooner and fades it out an octave nearer (less grain), -1 the reverse. Render only."), ECVF_Default);
static TAutoConsoleVariable<float> CVarUniNebulaNearKpc(TEXT("uu.Nebula.NearKpc"), 0.0f, TEXT("D-234: the nebulae fade in from this distance (kpc) to 3x (render only). D-235: 0 by default (they were lost up close)."));
static TAutoConsoleVariable<float> CVarUniMotionWebThreshold(TEXT("uu.MotionWeb.Threshold"), 3.0f, TEXT("D-230: the motion web shows the web where its filament + node matter is above this multiple of its mean (full two decades above). Lower: more of the web's shape (walls and voids); higher: only clusters and the densest filaments."));
static TAutoConsoleVariable<float> CVarUniMotionWebNear(TEXT("uu.MotionWeb.Near"), 4.0f, TEXT("D-230: the motion web fades in over its first this many Mpc (nearer structure would fill the view as soft blobs: the web texture's texels are 0.78 Mpc, 200 Mpc / 256)."));
static TAutoConsoleVariable<float> CVarUniMotionWebDepth(TEXT("uu.MotionWeb.Depth"), 50.0f, TEXT("D-230: the motion web fades out over the second half of this many Mpc (it is gone before the march or the web tile ends: no edge)."));
// D-269: the cosmic web's dark dust (render only, an art choice: real intergalactic dust is far thinner).
static TAutoConsoleVariable<float> CVarUniWebDust(TEXT("uu.WebDust"), 3.0f, TEXT("D-269: dark dust in the cosmic web: absorption following its filaments and nodes, shaped into dark filaments or knots (render only). Optical depth per 100 Mpc where the filament + node matter is at its mean (filaments and nodes are tens of times that, held under uu.WebDust.Peak); it reddens as the galaxies' dust does. 0: off."));
static TAutoConsoleVariable<float> CVarUniWebDustNearMpc(TEXT("uu.WebDust.NearMpc"), 2.0f, TEXT("D-269: the web's dust fades in over this many Mpc from the camera (the home group not fogged)."));
static TAutoConsoleVariable<float> CVarUniWebDustDepthMpc(TEXT("uu.WebDust.DepthMpc"), 50.0f, TEXT("D-269: the web's dust fades out over the second half of this many Mpc (the far sky not dimmed as a whole)."));
static TAutoConsoleVariable<float> CVarUniWebDustDetailMpc(TEXT("uu.WebDust.DetailMpc"), 30.0f, TEXT("D-269: the dark pattern's period, Mpc (galaxy_shape tiled a whole number of times a 200 Mpc web tile: 30 rounds to 28.6; features of ~5 Mpc, 220 kpc texels; 10 was too fine to read 10-50 Mpc away). Read over the pixel's footprint or the step: blurred to its mean where the march does not resolve it, never grain."));
static TAutoConsoleVariable<float> CVarUniWebDustDetail(TEXT("uu.WebDust.Detail"), 1.0f, TEXT("D-269: the dark pattern's amplitude (0: the dust follows the web's matter alone; 1: all of the pattern). Mean 1: the dust's total kept."));
static TAutoConsoleVariable<float> CVarUniWebDustKnots(TEXT("uu.WebDust.Knots"), 0.3f, TEXT("D-269: the dark pattern: 0 dark filaments (galaxy_shape's ridged channel^4), 1 dark knots (its knots^8), between a mix."));
static TAutoConsoleVariable<float> CVarUniWebDustRedden(TEXT("uu.WebDust.Redden"), 0.5f, TEXT("D-269: the web dust's colour: 0 grey (dark, every colour alike), 1 the galaxies' dust's reddening (blue absorbed most)."));
static TAutoConsoleVariable<float> CVarUniWebDustPeak(TEXT("uu.WebDust.Peak"), 30.0f, TEXT("D-269: the web's matter held softly under this x its mean for the dust (nodes are hundreds of times the mean: not opaque bricks). 0: no hold."));
static TAutoConsoleVariable<float> CVarUniShift(TEXT("uu.Shift"), 0.5f, TEXT("D-230: the travel shift: while travelling, light ahead is shifted blue and behind red, as a Doppler shift would (render only). D-268: ln of the shift factor straight ahead at uu.Shift.RefKpcS (0.5: x1.65, D-230's full strength); past it the blue side brightens (uu.Shift.Boost). 0: off."));
static TAutoConsoleVariable<float> CVarUniShiftTop(TEXT("uu.Shift.Top"), 2.0f, TEXT("D-268: ln of the travel shift's factor straight ahead at uu.Shift.FullKpcS and above (2: x7.4; up to 3). From the reference speed it climbs faster and faster to this."));
static TAutoConsoleVariable<float> CVarUniShiftStartKpcS(TEXT("uu.Shift.StartKpcS"), 150.0f, TEXT("D-268: the speed (kpc/s) where the travel shift starts to come in (D-230's: ~308)."));
static TAutoConsoleVariable<float> CVarUniShiftRefKpcS(TEXT("uu.Shift.RefKpcS"), 2200.0f, TEXT("D-268: the reference speed (kpc/s) where the travel shift is uu.Shift (D-230's full strength, reached at ~2200 kpc/s), and past which the blue side brightens."));
static TAutoConsoleVariable<float> CVarUniShiftFullKpcS(TEXT("uu.Shift.FullKpcS"), 8900.0f, TEXT("D-268: the speed (kpc/s) where the travel shift reaches uu.Shift.Top (4x D-230's full speed)."));
static TAutoConsoleVariable<float> CVarUniShiftBoost(TEXT("uu.Shift.Boost"), 1.0f, TEXT("D-268: the blue side's brightening past the reference: x exp(this x (the shift's ln there - uu.Shift)): straight ahead at the top, 1: x4.5. 0: luminance kept everywhere (D-230)."));
static TAutoConsoleVariable<float> CVarUniShiftColour(TEXT("uu.Shift.Colour"), 1.0f, TEXT("D-230: the travel shift's colour strength (the Doppler tint's exponent)."));
static TAutoConsoleVariable<float> CVarUniWarp(TEXT("uu.Warp"), 0.0f, TEXT("D-187: warp effects (streaks, arrival, D-190 optics, D-192 motes), 0..1 x the warp amount from speed (the travel-sensitivity curve). 0: physical. D-196: off by default (owner: at intergalactic speed the galaxies vanished with it on, B-018); 1 to try it."));
static TAutoConsoleVariable<float> CVarUniWarpMotes(TEXT("uu.Warp.Motes"), 1.0f, TEXT("D-192: warp ambience, 0..1 (x the warp amount): decorative motes fixed in space around the camera, streaking past at speed so a void still reads as fast. 0: none. Live."));
static TAutoConsoleVariable<float> CVarUniWarpMoteFrames(TEXT("uu.Warp.MoteFrames"), 30.0f, TEXT("D-192: the motes' lattice cell, in frames of the camera's motion (the nearest motes pass in about this many frames)."));
static TAutoConsoleVariable<float> CVarUniWarpMoteMag(TEXT("uu.Warp.MoteMag"), 1.5f, TEXT("D-192: the brightest motes' magnitude relative to the exposure magnitude (fainter > 0); they spread over two magnitudes below it."));
static TAutoConsoleVariable<int32> CVarUniSpriteEllipses(TEXT("uu.SpriteEllipses"), 1, TEXT("D-183: disc galaxies' sprites are ellipses at their disc's inclination (energy unchanged; round below a few pixels). 0: every sprite round. Render-only, live."));
// D-272 (COMPACT_OBJECTS.md phase A): galaxies' nuclei, their supermassive black holes' accretion light (render only, live).
static TAutoConsoleVariable<int32> CVarUniBhNucleus(TEXT("uu.Bh.Nucleus"), 1, TEXT("D-272: every galaxy's supermassive black hole shows as a nucleus: a point at its centre with its accretion light (most are far too faint to see; a few percent are Seyfert-bright). 0: none. Render only, live."));
static TAutoConsoleVariable<float> CVarUniBhActiveFraction(TEXT("uu.Bh.ActiveFraction"), 0.4f, TEXT("D-272: the share of galaxies whose black hole is accreting (Eddington ratio 1e-4..1, most low; the rest 1e-9..1e-5). 0..1; raise it to find bright nuclei to test. Render only (a gameplay lookup reads the same value)."));
static TAutoConsoleVariable<float> CVarUniBhNucleusCap(TEXT("uu.Bh.NucleusCap"), 0.5f, TEXT("D-272: a nucleus is at most this x its galaxy's light (the galaxy's streaming level comes from its own light). Render only, live."));
static TAutoConsoleVariable<float> CVarUniBhNucleusGain(TEXT("uu.Bh.NucleusGain"), 1.0f, TEXT("D-272: the nuclei's light x this (art; 1 physical), under the cap. Render only, live."));
// D-273 (COMPACT_OBJECTS.md phase D): the near field of the black hole the camera is nearest (lensing and its accretion disc; render only, live).
static TAutoConsoleVariable<int32> CVarUniBhLens(TEXT("uu.Bh.Lens"), 1, TEXT("D-273: the black hole of a galaxy the camera is in or near lenses the scene behind it (its shadow, photon ring and Einstein ring) once its ring is uu.Bh.LensRingMinPx across, and shows its accretion disc once that is resolved. 0: off (the nucleus stays a sprite). Needs the volume (the camera's galaxies come from it). Render only, live."));
static TAutoConsoleVariable<int32> CVarUniBhDisc(TEXT("uu.Bh.Disc"), 1, TEXT("D-273: with uu.Bh.Lens, the accretion disc is drawn as the nucleus resolves (the nucleus sprite hands its light to it). 0: lensing alone, the nucleus a sprite throughout. Render only, live."));
static TAutoConsoleVariable<float> CVarUniBhLensRingMinPx(TEXT("uu.Bh.LensRingMinPx"), 0.25f, TEXT("D-273: the lens pass runs while the black hole's Einstein ring (a source at infinity) is at least this many pixels in radius, or its disc is drawn."));
static TAutoConsoleVariable<float> CVarUniBhDiscFromPx(TEXT("uu.Bh.DiscFromPx"), 2.0f, TEXT("D-273: the disc takes over the nucleus's light from this outer radius (pixels) .."));
static TAutoConsoleVariable<float> CVarUniBhDiscToPx(TEXT("uu.Bh.DiscToPx"), 6.0f, TEXT("D-273: .. to all of it at this (smoothstep between)."));
static TAutoConsoleVariable<float> CVarUniBhLensBendMinPx(TEXT("uu.Bh.LensBendMinPx"), 0.1f, TEXT("D-273: the lens pass's region reaches where the deflection is this many pixels (pixels beyond it are copied)."));
static TAutoConsoleVariable<float> CVarUniBhLensScale(TEXT("uu.Bh.LensScale"), 4.0f, TEXT("D-277: the black hole lenses as if this x heavier (art): its shadow, photon ring and lensing this x larger against its disc, which keeps its physical size (its inner edge out at the heavier hole's innermost stable orbit). 1: physical. D-278: 4 (was 2)."));
// D-280: each black hole's look, rolled about the uu.Bh.Disc* settings (render only).
static TAutoConsoleVariable<float> CVarUniBhDiscVariety(TEXT("uu.Bh.DiscVariety"), 1.0f, TEXT("D-280: how far each black hole's look strays from the uu.Bh.Disc* settings (its palette, spiral, thickness, opacity, clouds, bands, heat, erosion, period, brightness and extent, rolled from its seed, its accretion rate and its host): 0 every black hole the settings' look exactly (D-278), 1 the full spread."));
static TAutoConsoleVariable<int32> CVarUniBhDiscPalette(TEXT("uu.Bh.DiscPalette"), -1, TEXT("D-280: the accretion discs' colours: -1 each black hole's rolled palette; 0 the blackbody (physical colours); 1..7 every black hole that palette (1 gold, 2 quasar blue, 3 ember, 4 violet, 5 teal, 6 rose, 7 ice and fire)."));
// D-281: stellar black holes (X-ray binaries: a star's companion).
static TAutoConsoleVariable<float> CVarUniBhStellarFraction(TEXT("uu.Bh.StellarFraction"), UniHlsl::UNI_BH_STELLAR_FRACTION, TEXT("D-281: the share of stars with a stellar black hole companion (5..25 M_sun, an X-ray binary fed by the star), found when the star's system activates (art; raising it only adds). 0: none. Applies to systems activated from then on."));
static TAutoConsoleVariable<float> CVarUniBhStellarActive(TEXT("uu.Bh.StellarActive"), UniHlsl::UNI_BH_STELLAR_ACTIVE, TEXT("D-281: the share of stellar black holes in outburst (1e-2..1 of Eddington: bright, hot, thin discs); the rest quiescent (1e-8..1e-4: dim tori)."));
static TAutoConsoleVariable<float> CVarUniBhStellarScale(TEXT("uu.Bh.StellarScale"), UniHlsl::UNI_BH_STELLAR_SCALE, TEXT("D-281: a stellar black hole's lensing and disc drawn as if it were this x larger (art; render only): at 30 a 10 M_sun hole's disc is ~1 R_sun, resolved from a few AU. 1: physical (its disc ~3e4 km)."));
static TAutoConsoleVariable<int32> CVarUniBhLensMaxSteps(TEXT("uu.Bh.LensMaxSteps"), 400, TEXT("D-273: the most RK4 steps a ray takes; a ray still winding at the photon sphere after them is taken as fallen in (16..2000)."));
static TAutoConsoleVariable<int32> CVarUniGalaxyPalettes(TEXT("uu.GalaxyPalettes"), 1, TEXT("D-182: every galaxy its own colour palette (bulge, disc, arms, knots), which its stars and glow take (chroma only: brightness unchanged). 0: physical colours. Applies at uu.Enable; rerolls star and galaxy colours only."));
static TAutoConsoleVariable<int32> CVarUniGalaxyTextures(TEXT("uu.GalaxyTextures"), 1, TEXT("D-179: galaxies read the noise project's galaxy textures (warp_fine, warp_large, galaxy_detail in Plugins/UnifiedUniverse/Data/Noise): texture warps of discs, arms and bulges, disc noise and star-forming knots. 0: off. Applies at uu.Enable; rerolls galaxies' stars."));
// D-249 (owner 2026-11-15: "configure the volume textures used for the various components ... to
// test different versions"): each galaxy texture's file in Data/Noise (<name>.bin + <name>_mips.bin).
static TAutoConsoleVariable<FString> CVarUniGalTexWarpFine(TEXT("uu.GalaxyTexture.WarpFine"), TEXT("warp_fine"), TEXT("D-249: the galaxies' fine warp texture (a file in Plugins/UnifiedUniverse/Data/Noise, without .bin). Applies at uu.Enable or uu.ReloadTextures; rerolls galaxies' stars (placement reads it)."));
static TAutoConsoleVariable<FString> CVarUniGalTexWarpLarge(TEXT("uu.GalaxyTexture.WarpLarge"), TEXT("warp_large"), TEXT("D-249: the galaxies' large warp texture (Data/Noise). Applies at uu.Enable or uu.ReloadTextures; rerolls galaxies' stars."));
static TAutoConsoleVariable<FString> CVarUniGalTexDetail(TEXT("uu.GalaxyTexture.Detail"), TEXT("galaxy_detail"), TEXT("D-249: the galaxies' detail texture (Data/Noise: R ridged lanes, G star-forming knots, B fBm, A superbubbles; the nebulae's knots too). Applies at uu.Enable or uu.ReloadTextures; rerolls galaxies' stars."));
static TAutoConsoleVariable<FString> CVarUniGalTexNebulaDetail(TEXT("uu.GalaxyTexture.NebulaDetail"), TEXT(""), TEXT("D-249: the nebulae's detail layers' texture (Data/Noise; walls from R ridged and A Worley-walls, D-243's split from B fBm). Empty: uu.GalaxyTexture.Detail's. Render only (no reroll). Applies at uu.Enable or uu.ReloadTextures."));
static TAutoConsoleVariable<float> CVarUniVolumeDustGain(TEXT("uu.Volume.DustGain"), 1.0f, TEXT("D-261: every galaxy dust layer's optical depth in the volume x this (render only; the light / dust balance: 0 no extinction, 2 twice as dark). uu.Galaxy.DustScale scales the dust itself (at uu.Enable), uu.Galaxy.DiffuseDust the background dust."));
static TAutoConsoleVariable<float> CVarUniVolumeDustCurve(TEXT("uu.Volume.DustCurve"), 0.0f, TEXT("D-270: the galaxy dust's colour (render only): 0 D-132's reddening (red 0.8, green 1, blue 1.3 of the green's optical depth), 1 Cardelli's extinction curve for R_V 3.1 at the screen's primaries (0.886, 1, 1.233: a little less red), in between a blend. The web dust's reddening follows it."));
static TAutoConsoleVariable<float> CVarUniGalaxyKnots(TEXT("uu.Galaxy.Knots"), 1.0f, TEXT("D-258/D-259: textured galaxies' star-forming knots, galaxy_detail's knot channel to uu.Galaxy.KnotPower (prefiltered: galaxy_shape), x this amplitude over the one that keeps the knots' share of the light the linear term's (1). 0: D-179's linear knots. Applies at uu.Enable (stars follow the light)."));
static TAutoConsoleVariable<float> CVarUniGalaxyKnotPower(TEXT("uu.Galaxy.KnotPower"), 8.0f, TEXT("D-258/D-259: the knots' power (GAMER's star-forming clumps: 8; higher: sparser, brighter knots). galaxy_shape is rebaked with it. Applies at uu.Enable."));
static TAutoConsoleVariable<float> CVarUniGalaxyDustStructure(TEXT("uu.Galaxy.DustStructure"), 4.0f, TEXT("D-258/D-259: the large dust lanes: galaxy_detail's ridged channel read at uu.Galaxy.StructureFreq, to this power (GAMER's dust: 4), multiplying the dust. 0: off. Applies at uu.Enable."));
static TAutoConsoleVariable<float> CVarUniGalaxyLightStructure(TEXT("uu.Galaxy.LightStructure"), 3.0f, TEXT("D-258/D-259: the arms' streaks: galaxy_detail's fBm read at uu.Galaxy.StructureFreq, to this power, multiplying the disc's light. 0: off. Applies at uu.Enable."));
static TAutoConsoleVariable<float> CVarUniGalaxyStructureFreq(TEXT("uu.Galaxy.StructureFreq"), 0.25f, TEXT("D-258/D-259: the structure read's frequency over the galaxy detail's (its roll DetailFreq: 2-4 periods; 0.25: the structure 4x larger than the detail, GAMER's noise at the galaxy's own scale). 0: no structure read. Applies at uu.Enable."));
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

// D-261: a re-enable requested by the tuning page (FPlatformTime seconds; 0: none).
static double GUniReenableAt = 0.0;

void UUniSubsystem::RequestReenable()
{
	GUniReenableAt = FPlatformTime::Seconds() + 0.5;
}

void UUniSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	GLastSubsystem = this;
	// D-261: the tuning page's saved values (DefaultGame.ini) to their console variables, before
	// anything reads them (uu.AutoStart enables later, from the first tick).
	if (const UUniTuningSettings* T = GetDefault<UUniTuningSettings>()) { T->ApplyAll(); }
}

// D-216: start-up without console commands.
static TAutoConsoleVariable<FString> CVarUniAutoStart(TEXT("uu.AutoStart"), TEXT("flythrough"), TEXT("D-216: the scene enabled when play begins (galaxy, web, flythrough or home, as uu.Enable; D-293: home starts on foot on the home planet); none: wait for uu.Enable. Set in DefaultEngine.ini [ConsoleVariables] to change it."));
static TAutoConsoleVariable<int32> CVarUniAutoFly(TEXT("uu.AutoFly"), 1, TEXT("D-216: with uu.AutoStart, begin in free flight (as uu.Fly) once the player's pawn exists. 0: Local mode."));

void UUniSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const FString Scene = CVarUniAutoStart.GetValueOnGameThread().TrimStartAndEnd();
	if (Scene.IsEmpty() || Scene.Equals(TEXT("none"), ESearchCase::IgnoreCase)) { return; }
	EnableUniverse(Scene);
	// The pawn is usually possessed after the world's begin play: free flight waits for it (Tick).
	bAutoFlyPending = bEnabled && CVarUniAutoFly.GetValueOnGameThread() != 0 && SceneName != TEXT("home");   // D-293: home starts on foot
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
	WorldTurn.Reset();   // D-299: a new start: the world's axes are the universe's
	++NebGridEpoch;   // D-252: the textures may have changed: the nebula grid rebakes
	ClusterTuning = UniClusterCVars::Read();
	Warp.Current = 0.0;
	bCameraJumped = true;
	// D-293: "home" is the flythrough universe plus the home star (UniHome.h).
	UniCore::FUniverseDef Def = SceneName == TEXT("web") ? UniScenes::WebToGalaxy(ClusterTuning)
		: (SceneName == TEXT("flythrough") ? UniScenes::Flythrough(ClusterTuning) : (SceneName == TEXT("home") ? UniScenes::Home(ClusterTuning) : UniScenes::GalaxyToStar()));
	SetupWebTexture(Def);   // D-164: before the model, which bakes the definition's parameters
	SetupGalaxyTextures(Def);   // D-179: likewise
	if (CVarUniGalaxyPalettes.GetValueOnGameThread() != 0) { UniScenes::UseGalaxyPalettes(Def); }   // D-182
	UniScenes::UseSpriteKeep(Def, ClusterTuning.StarKeep, ClusterTuning.MemberKeep);   // D-197: after the web (it rebuilds cluster and group emitters)
	UniScenes::UseDustTuning(Def, ClusterTuning.DustScale, ClusterTuning.DustLanes, ClusterTuning.DustHeight);   // D-198, D-263
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
	LocalAnchorFrac = FVector3f::ZeroVector;
	CameraFraction = FVector3f::ZeroVector;
	MotionMode = EUniMotionMode::Local;
	bEnabled = true;
	// D-293: planets' detailed actors from the game modules' factories; the home start.
	BodyFactories.Reset();
	for (const TSubclassOf<UUniBodyFactory>& Fc : BodyFactoryClasses())
	{
		if (Fc) { BodyFactories.Add(NewObject<UUniBodyFactory>(this, Fc)); }
	}
	if (SceneName == TEXT("home")) { BeginHomeStart(); }
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
	ReleaseDetailedBodies();   // D-293: before the placeholders (they may be hidden for them)
	ClearPromotion();
	BodyFrame = FPlanetFrame();
	WorldTurn.Reset();   // D-299
	if (bPawnHeld) { HoldPawn(false); }
	StartPhase = EStartPhase::None;
	BodyFactories.Reset();
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
	for (int32 S = 0; S < 4; ++S) { UniCore::BindGalaxyTexture(S, nullptr); GalaxyTextures[S].Reset(); }   // D-179, D-249 (D-259: slot 2's unbinds galaxy_shape)
}

void UUniSubsystem::SetupGalaxyTextures(UniCore::FUniverseDef& Def)
{
	// D-179: the galaxy textures, all three or none. Missing (or uu.GalaxyTextures 0), galaxies keep
	// their untextured field (the terms stay off: the definition never asks for a texture it does not
	// have), with an error naming the files when they were wanted.
	for (int32 S = 0; S < 4; ++S) { UniCore::BindGalaxyTexture(S, nullptr); GalaxyTextures[S].Reset(); FUniRenderer::Get().SetGalaxyTexture_GameThread(S, 0u, {}); }
	FUniRenderer::Get().SetGalaxyTexture_GameThread(4, 0u, {});   // D-259: galaxy_shape (unbound with slot 2)
	// D-258 / D-259: the shaping (UNI_GAL_SHAPE: the CPU cores' and, as UniGalShapeTune, the shaders'),
	// and galaxy_shape's powers (baked when galaxy_detail binds). A power 0 turns its term off.
	{
		const float KnotPow = FMath::Max(CVarUniGalaxyKnotPower.GetValueOnGameThread(), 0.0f);
		const float DustPow = FMath::Max(CVarUniGalaxyDustStructure.GetValueOnGameThread(), 0.0f);
		const float LightPow = FMath::Max(CVarUniGalaxyLightStructure.GetValueOnGameThread(), 0.0f);
		const float Knots = KnotPow > 0.0f ? FMath::Max(CVarUniGalaxyKnots.GetValueOnGameThread(), 0.0f) : 0.0f;
		const float Freq = FMath::Max(CVarUniGalaxyStructureFreq.GetValueOnGameThread(), 0.0f);
		UniCore::FGalaxyShapePowers& Pw = UniCore::GalaxyShapePowers();
		Pw.Dust = DustPow; Pw.Light = LightPow; Pw.Knot = KnotPow;
		UniHlsl::GGalShape = UniHlsl::float4(Knots, DustPow > 0.0f ? 1.0f : 0.0f, LightPow > 0.0f ? 1.0f : 0.0f, (DustPow > 0.0f || LightPow > 0.0f) ? Freq : 0.0f);
		FUniRenderer::Get().SetGalaxyShapeTune_GameThread(FVector4f(UniHlsl::GGalShape.x, UniHlsl::GGalShape.y, UniHlsl::GGalShape.z, UniHlsl::GGalShape.w));
	}
	if (CVarUniGalaxyTextures.GetValueOnGameThread() == 0) { return; }
	// D-249: the files by console variable (the defaults are the delivered names).
	const FString Names[4] = { CVarUniGalTexWarpFine.GetValueOnGameThread().TrimStartAndEnd(), CVarUniGalTexWarpLarge.GetValueOnGameThread().TrimStartAndEnd(),
		CVarUniGalTexDetail.GetValueOnGameThread().TrimStartAndEnd(), CVarUniGalTexNebulaDetail.GetValueOnGameThread().TrimStartAndEnd() };
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnifiedUniverse"));
	const FString Dir = FPaths::ConvertRelativePathToFull((Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::ProjectPluginsDir() / TEXT("UnifiedUniverse")) / TEXT("Data") / TEXT("Noise"));
	const double T0 = FPlatformTime::Seconds();
	TUniquePtr<UniCore::FWebTexture> T[3];
	for (int32 S = 0; S < 3; ++S)
	{
		T[S] = MakeUnique<UniCore::FWebTexture>();
		std::string Err;
		if (!UniCore::FWebTexture::LoadDelivered(TCHAR_TO_UTF8(*Dir), TCHAR_TO_UTF8(*Names[S]), *T[S], &Err))
		{
			UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: galaxy texture '%s' (%s) not loaded (%s). Galaxies use their untextured field: regenerate it with the noise project into %s, or set uu.GalaxyTextures 0."),
				*Names[S], UTF8_TO_TCHAR(UniCore::GalaxyTextureName(S)), UTF8_TO_TCHAR(Err.c_str()), *Dir);
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
		UniCore::BindGalaxyTexture(S, GalaxyTextures[S].Get());   // D-259: slot 2 derives and binds galaxy_shape (slot 4)
	}
	// D-259: galaxy_shape (derived from galaxy_detail with the powers above) to the GPU.
	if (const UniCore::FWebTexture* Gs = UniCore::BoundGalaxyShapeTexture())
	{
		TArray<TArray<FUintVector2>> Mips;
		for (uint32 M = 0; M < Gs->NumMips(); ++M)
		{
			TArray<FUintVector2>& A = Mips.AddDefaulted_GetRef();
			A.SetNumUninitialized(int32(Gs->TexelCount(M)));
			FMemory::Memcpy(A.GetData(), Gs->Mips[M].data(), Gs->Mips[M].size() * sizeof(uint16));
		}
		FUniRenderer::Get().SetGalaxyTexture_GameThread(4, Gs->Size, MoveTemp(Mips));
	}
	const int32 N = UniScenes::UseGalaxyTextures(Def, GalaxyTextures[0]->Size);
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: galaxy textures %s, %s, %s %u^3 loaded (%.2f s); %d galaxy emitters and anchors textured (D-179, D-249)"),
		*Names[0], *Names[1], *Names[2], GalaxyTextures[0]->Size, FPlatformTime::Seconds() - T0, N);
	// D-249: the nebulae's detail texture, render only: its own file when one is named (any size: the
	// reads are in its periods), otherwise galaxy_detail's (slot 3 left empty). A file that fails to
	// load leaves the nebulae on galaxy_detail, with an error.
	if (!Names[3].IsEmpty() && Names[3] != Names[2])
	{
		TUniquePtr<UniCore::FWebTexture> Tn = MakeUnique<UniCore::FWebTexture>();
		std::string Err;
		if (!UniCore::FWebTexture::LoadDelivered(TCHAR_TO_UTF8(*Dir), TCHAR_TO_UTF8(*Names[3]), *Tn, &Err))
		{
			UE_LOG(LogUnifiedUniverse, Error, TEXT("UnifiedUniverse: nebula detail texture '%s' not loaded (%s); the nebulae keep %s (D-249)."), *Names[3], UTF8_TO_TCHAR(Err.c_str()), *Names[2]);
		}
		else
		{
			TArray<TArray<FUintVector2>> Mips;
			for (uint32 M = 0; M < Tn->NumMips(); ++M)
			{
				TArray<FUintVector2>& A = Mips.AddDefaulted_GetRef();
				A.SetNumUninitialized(int32(Tn->TexelCount(M)));
				FMemory::Memcpy(A.GetData(), Tn->Mips[M].data(), Tn->Mips[M].size() * sizeof(uint16));
			}
			FUniRenderer::Get().SetGalaxyTexture_GameThread(3, Tn->Size, MoveTemp(Mips));
			GalaxyTextures[3] = MoveTemp(Tn);
			UniCore::BindGalaxyTexture(3, GalaxyTextures[3].Get());
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: nebula detail texture %s %u^3 loaded (D-249)"), *Names[3], GalaxyTextures[3]->Size);
		}
	}
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

void UUniSubsystem::ReloadTextures()
{
	// D-249: the textures load at enable (before the model, which bakes the definition's
	// parameters), so a reload re-enables the scene and puts the camera and its motion back.
	if (!bEnabled)
	{
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: not enabled; the textures load at uu.Enable."));
		return;
	}
	const FUniCoord Cam = Camera;
	const EUniMotionMode Mode = MotionMode;
	const FVector Vel = Velocity;
	const bool bFly = bFreeFly;
	EnableUniverse(SceneName);
	if (!bEnabled) { return; }
	Teleport(Cam);
	MotionMode = Mode;
	Velocity = Vel;
	if (bFly) { SetFreeFly(true); }
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: textures reloaded (D-249)."));
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
	UniCore::AddDisplacement(A, F, ToUniversalAxes(-ViewLoc));   // D-299
	LocalAnchor = FUniCoord::Make(A);
	LocalAnchorFrac = FVector3f::ZeroVector;   // D-293
	if (BodyFrame.bActive) { LeaveBodyFrame(TEXT("teleport")); }
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
	if (bOn && bPawnHeld) { HoldPawn(false); }   // D-293: free flight takes over a pawn the home start holds
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
		UniCore::AddDisplacement(A, F, ToUniversalAxes(-ViewLoc));   // D-299
		LocalAnchor = FUniCoord::Make(A);
		LocalAnchorFrac = FVector3f::ZeroVector;   // D-293
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
		PC->SetControlRotation(WorldRotationOf(-FlyDirection));   // D-299
	}
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: flythrough to a star of M = %.2f over %.0f s"), Best, FlyDuration);
}

void UUniSubsystem::Tick(float DeltaTime)
{
	if (!bEnabled || !Streamer) { return; }
	if (GUniReenableAt > 0.0 && FPlatformTime::Seconds() >= GUniReenableAt)   // D-261: the tuning page
	{
		GUniReenableAt = 0.0;
		ReloadTextures();
		return;
	}
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
	// B-002: orbits are on rails at this clock. D-293: advanced first, so a planet's frame (its
	// centre this frame), its drawn record and its promoted body agree on the time.
	UniverseTime += double(DeltaTime) * TimeScale;
	FRotator ViewRot;
	FVector ViewLoc = GetViewLocation(&ViewRot);   // updated below if the pawn is moved or re-centred this frame
	// Local mode computes camera = anchor + view location, so on entering Local the anchor is the
	// camera MINUS the view location (else the camera jumps by the pawn offset).
	auto AnchorFor = [this, &ViewLoc](const FUniCoord& Cam)
	{
		UniCore::FCoord A = Cam.ToCore();
		UniCore::FVec3f F{};
		UniCore::AddDisplacement(A, F, ToUniversalAxes(-ViewLoc));   // D-299: in the universe's axes
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
			if (MotionMode != EUniMotionMode::Local) { LocalAnchor = AnchorFor(Camera); LocalAnchorFrac = FVector3f::ZeroVector; MotionMode = EUniMotionMode::Local; }
			Pawn->SetActorLocation(Pawn->GetActorLocation() + Step, false, nullptr, ETeleportType::None);
			ViewLoc += Step;
			UpdateBodyFrame(ViewLoc);   // D-293: a planet's frame (no re-centre in it: the planet stays put)
			if (!BodyFrame.bActive) { RecentreLocalIfNeeded(ViewLoc); }
			UniCore::FVec3f F{};
			C = LocalCamera(ViewLoc, F);
			Frac = F;
		}
		else
		{
			if (BodyFrame.bActive) { LeaveBodyFrame(TEXT("cruise")); }
			MotionMode = EUniMotionMode::Cruise;
			UniCore::AddDisplacement(C, Frac, ToUniversalAxes(Step));   // D-299: the view's step in the universe's axes
		}
		DrawHud(Velocity.Size());
	}
	else
	{
		// D-008 motion modes with hysteresis.
		const double Speed = Velocity.Size();
		// A latched uu.Cruise stays in Cruise at any non-zero speed (the D-008 threshold would
		// otherwise ignore every speed below 1e8 cm/s).
		if (MotionMode == EUniMotionMode::Local && (Speed > kCruiseOn || (bCruiseLatched && Speed > 0.0)))
		{
			MotionMode = EUniMotionMode::Cruise;
			if (BodyFrame.bActive) { LeaveBodyFrame(TEXT("cruise")); }
		}
		else if (MotionMode == EUniMotionMode::Cruise && Speed < kCruiseOff && !(bCruiseLatched && Speed > 0.0)) { MotionMode = EUniMotionMode::Local; LocalAnchor = AnchorFor(Camera); LocalAnchorFrac = FVector3f::ZeroVector; }
		if (bCruiseLatched) { DrawHud(Speed); }
		if (MotionMode == EUniMotionMode::Cruise)
		{
			const FVector Step = Velocity * double(DeltaTime);   // D-299: Velocity is in the world's axes
			UniCore::AddDisplacement(C, Frac, ToUniversalAxes(Step));
		}
		else
		{
			// Local: camera = local anchor + view location (the pawn moves normally). D-293: in a
			// planet's frame the anchor is the planet's exact centre this frame and nothing re-centres.
			UpdateBodyFrame(ViewLoc);
			if (!BodyFrame.bActive) { RecentreLocalIfNeeded(ViewLoc); }
			UniCore::FVec3f F{};
			C = LocalCamera(ViewLoc, F);
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
		// D-231: the motion web's own ramp, from the speed itself (it starts well below the travel curve).
		MotionWebRamp.Update(Speed, Dt, double(FMath::Max(CVarUniMotionWebStartKpcS.GetValueOnGameThread(), 0.001f)) * 3.0856775814913673e21,
			double(FMath::Max(CVarUniMotionWebFullKpcS.GetValueOnGameThread(), 0.002f)) * 3.0856775814913673e21);
		// D-268: the travel shift's strength from the speed, eased.
		{
			const double Kpc = 3.0856775814913673e21;
			const double Target = UniCore::TravelShiftTarget(Speed, double(FMath::Max(CVarUniShiftStartKpcS.GetValueOnGameThread(), 0.001f)) * Kpc,
				double(FMath::Max(CVarUniShiftRefKpcS.GetValueOnGameThread(), 0.001f)) * Kpc, double(FMath::Max(CVarUniShiftFullKpcS.GetValueOnGameThread(), 0.001f)) * Kpc,
				double(FMath::Clamp(CVarUniShift.GetValueOnGameThread(), 0.0f, 3.0f)), double(FMath::Clamp(CVarUniShiftTop.GetValueOnGameThread(), 0.0f, 3.0f)));
			TravelShiftRamp.Update(Target, Dt);
		}
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
		Streamer->ViewDir = ToUniversalAxes(ViewRot.Vector());   // D-299
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
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_StreamerUpdate);
		FPerfTimer T(PerfMs[PerfStreamer]);
		Streamer->Update(C);
	}
	// D-193: picks and queries read back, then this frame's events (the streamer just moved).
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Api); ProcessApiReadbacks(); UpdateApiEvents(ViewRot); }
	// M3: bodies close enough become actors, and every parallax-placed actor follows the camera
	// that just moved (same frame, same camera: no lag against the backdrop at any speed).
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Promotion); FPerfTimer T(PerfMs[PerfPromotion]); UpdatePromotion(); UpdateDetailedBodies(ViewLoc); UpdateHomeStart(ViewLoc); }
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
	// D-229: the volume's integrals (D-140's exact ones on workers; a galaxy's quick estimate
	// meanwhile, so its carrier is not held back).
	if (Streamer)
	{
		UE_LOG(LogUnifiedUniverse, Log, TEXT("  volume integrals: %d exact jobs running; so far %llu quick estimates, %llu carriers built on one (eased to the exact), %llu carrier-frames waiting for any"),
			int32(Streamer->PendingIntegrals()), (unsigned long long)Streamer->QuickComputed, (unsigned long long)Streamer->QuickUsed, (unsigned long long)Streamer->VolumeIntegralWaits);
	}
	// D-228: the far cache.
	{
		static IConsoleVariable* CVarFar = IConsoleManager::Get().FindConsoleVariable(TEXT("uu.VolumeFar"));
		static IConsoleVariable* CVarFarStart = IConsoleManager::Get().FindConsoleVariable(TEXT("uu.VolumeFarStartPc"));
		UE_LOG(LogUnifiedUniverse, Log, TEXT("  far cache (uu.VolumeFar %d, split %.0f pc, %u^2 faces): %s, parallax %.3f texels a frame (on below uu.VolumeFarOnMotion, off above uu.VolumeFarOffMotion); so far %u frames on, %u switches, %u faces re-marched, %u scene changes, %u history drops"),
			CVarFar ? CVarFar->GetInt() : -1, CVarFarStart ? CVarFarStart->GetFloat() : -1.0f, R.FarFaceSize.load(), UTF8_TO_TCHAR(UniCore::FarReasonName(UniCore::EFarReason(R.FarReason.load()))), R.FarMotion.load(),
			R.FarFramesOn.load(), R.FarSwitches.load(), R.FarFacesFilled.load(), R.FarSceneChanges.load(), R.FarHistoryDrops.load());
	}
	// D-252: the nebula grid.
	{
		const uint32 Nl = R.NebGridLevels.load();
		if (Nl == 0u)
		{
			UE_LOG(LogUnifiedUniverse, Log, TEXT("  nebula grid (uu.Nebula.Grid %d): off (or no spiral with nebulae about the camera)"), CVarUniNebulaGrid.GetValueOnGameThread());
		}
		else
		{
			FString Lv;
			uint32 Used = 0, Slots = 0, Over = 0;
			for (uint32 K = 0; K < Nl && K < uint32(FUniRenderStats::MaxNebGridLevels); ++K)
			{
				const uint32 Sk = R.NebGridSlots[K].load(), Fk = FMath::Min(R.NebGridFree[K].load(), Sk), Ok = R.NebGridOverflow[K].load();
				Lv += FString::Printf(TEXT("%s%u/%u%s"), K ? TEXT(", ") : TEXT(""), Sk - Fk, Sk, Ok ? *FString::Printf(TEXT(" (+%u overflow)"), Ok) : TEXT(""));
				Used += Sk - Fk; Slots += Sk; Over += Ok;
			}
			UE_LOG(LogUnifiedUniverse, Log, TEXT("  nebula grid (uu.Nebula.GridCells %d, samples %d, budget %d): %u levels, %.1f MB; slots used by level %s; %u bricks waiting, %u processed last frame, %llu in all, %llu level resets%s"),
				CVarUniNebulaGridCells.GetValueOnGameThread(), CVarUniNebulaGridSamples.GetValueOnGameThread(), CVarUniNebulaGridBudget.GetValueOnGameThread(), Nl, double(R.NebGridPoolKiB.load()) / 1024.0, *Lv,
				R.NebGridPending.load(), R.NebGridProcessed.load(), (unsigned long long)R.NebGridProcessedTotal.load(), (unsigned long long)R.NebGridLevelResets.load(),
				Over ? TEXT(" (overflow: raise uu.Nebula.GridPool)") : TEXT(""));
		}
	}
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
		Systems->BhConfig = StellarBhConfig();   // D-281
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
		const std::set<UniCore::u32> Culled = SpriteCulledIds();   // promoted planets: their actor draws them (D-147; D-297: once it is ready)
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
		// D-230: the travel shift (the render thread takes the direction from the camera's step);
		// D-268: its strength from the speed (TravelShiftRamp, updated with the camera).
		F.ShiftAmount = float(TravelShiftRamp.Value);
		F.ShiftK = FMath::Clamp(CVarUniShiftColour.GetValueOnGameThread(), 0.0f, 4.0f);
		F.ShiftBoostFrom = FMath::Clamp(CVarUniShift.GetValueOnGameThread(), 0.0f, 3.0f);
		F.ShiftBoost = FMath::Clamp(CVarUniShiftBoost.GetValueOnGameThread(), 0.0f, 4.0f);
	}
	{
		// D-200: the glow's pixel budget from the GPU's last frame time (the volume is most of it).
		const float Fps = CVarUniVolumeTargetFps.GetValueOnGameThread();
		const double GpuMs = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
		F.VolumeBudgetScale = float(VolumeBudget.Update(GpuMs, Fps > 0.0f ? 1000.0 / double(Fps) : 0.0, FMath::Clamp(CVarUniVolumeAutoHold.GetValueOnGameThread(), 1, 600)));
	}
	F.bCameraStepValid = bCameraStepValid;
	{
		// D-246: the blue-noise tile the volume's draws read (FastBlueNoise_scalar_128x128x8, the
		// texture CloudAtmosphere binds); loaded once, held here, its resource handed on (null
		// until the engine has created it: the hashed draws meanwhile).
		if (!VolumeBlueNoise && !bVolumeBlueNoiseTried)
		{
			bVolumeBlueNoiseTried = true;
			VolumeBlueNoise = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineMaterials/FastBlueNoise_scalar_128x128x8.FastBlueNoise_scalar_128x128x8"));
			if (!VolumeBlueNoise) { UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: FastBlueNoise_scalar_128x128x8 not found: the volume's draws stay hashed (D-246).")); }
		}
		F.BlueNoiseResource = VolumeBlueNoise ? VolumeBlueNoise->GetResource() : nullptr;
	}
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
	F.NucleusArchetypeMask = CVarUniBhNucleus.GetValueOnGameThread() != 0 ? DiscArchetypeMask : 0u;   // D-272: every galaxy archetype
	F.NucleusParams = FVector4f(NucleusConfig().Active, NucleusConfig().Cap, NucleusConfig().Gain, 0.0f);
	{
		const UniCore::FStyleConfig Stc = StyleConfig();   // D-280: the nuclei's colours are their discs' palettes
		F.NucleusStyle = FVector4f(Stc.Variety, Stc.PaletteMode, 0.0f, 0.0f);
	}
	for (int32 K = 0; K < 4; ++K) { F.FixedDiscSeeds[K] = FixedDiscSeeds[K]; }
	F.DeltaMag = EffectiveDeltaMag;   // D-154: owner sensitivity + travel boost
	F.ExposureBias = ExposureBias + TravelExposureMag;   // D-205
	F.BackdropScale = BackdropDayScale;                  // D-296
	F.WorldYaw = WorldTurn.Yaw;                          // D-299
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
		{ float Curve[3]; UniCore::DustCurveRgb(CVarUniVolumeDustCurve.GetValueOnGameThread(), Curve); const float Dg = FMath::Max(CVarUniVolumeDustGain.GetValueOnGameThread(), 0.0f); for (int32 K = 0; K < 3; ++K) { Settings.DustSigmaRgb[K] = Curve[K] * Dg; } }   // D-261; D-270: the curve
		{
			// D-189: the web in the sky.
			const int32 SkyMode = CVarUniSkyWeb.GetValueOnGameThread();
			Settings.WebGlowGain = (SkyMode & 1) ? FMath::Max(CVarUniSkyWebGlow.GetValueOnGameThread(), 0.0f) : 0.0f;
			Settings.WebGlowDepthCm = float(double(FMath::Max(CVarUniSkyWebDepth.GetValueOnGameThread(), 0.1f)) * 3.0856775814913673e24);
			Settings.NebulaGain = FMath::Max(CVarUniNebula.GetValueOnGameThread(), 0.0f);            // D-234
			Settings.NebulaReflect = FMath::Max(CVarUniNebulaReflect.GetValueOnGameThread(), 0.0f);
			Settings.NebulaNearCm = float(double(FMath::Max(CVarUniNebulaNearKpc.GetValueOnGameThread(), 0.0f)) * 3.0856775814913673e21);
			Settings.NebulaFineGain = FMath::Clamp(CVarUniNebulaFine.GetValueOnGameThread(), 0.0f, 2.0f);   // D-237
			{ const float Dk = CVarUniNebulaDark.GetValueOnGameThread(); Settings.NebulaDark = Dk < 0.0f ? -1.0f : FMath::Min(Dk, 1.0f); }   // D-242
			Settings.NebulaRefine = uint32(FMath::Clamp(CVarUniNebulaRefine.GetValueOnGameThread(), 0, 16));   // D-244
			Settings.NebulaFineReachCm = float(double(FMath::Max(CVarUniNebulaFineReachKpc.GetValueOnGameThread(), 0.0f)) * 3.0856775814913673e21);
			Settings.NebulaScale = CVarUniNebulaScale.GetValueOnGameThread();             // D-248 (UniCore::NebulaTune clamps)
			{
				// D-250: the diffuse dust.
				const double Lo = double(FMath::Clamp(CVarUniGalaxyDustReachMinKpc.GetValueOnGameThread(), 0.01f, 1000.0f));
				const double Hi = FMath::Max(double(FMath::Clamp(CVarUniGalaxyDustReachMaxKpc.GetValueOnGameThread(), 0.01f, 1000.0f)), Lo);
				Settings.DiffuseDustReachLoCm = float(Lo * 3.0856775814913673e21);
				Settings.DiffuseDustReachHiCm = float(Hi * 3.0856775814913673e21);
				Settings.DiffuseDustGain = FMath::Clamp(CVarUniGalaxyDiffuseDust.GetValueOnGameThread(), 0.0f, 100.0f);
				Settings.DiffuseDustHeight = FMath::Clamp(CVarUniGalaxyDiffuseDustHeight.GetValueOnGameThread(), 0.05f, 20.0f);
			}
			Settings.StepFoot = FMath::Clamp(CVarUniVolumeStepFoot.GetValueOnGameThread(), 0.0f, 4.0f);   // D-255
			Settings.SegmentTaps = UniCore::u32(FMath::Clamp(CVarUniVolumeSegmentTaps.GetValueOnGameThread(), 1, 16));   // D-262
			Settings.OpaqueCutoff = FMath::Clamp(CVarUniVolumeOpaqueCutoff.GetValueOnGameThread(), 1.0e-6f, 0.5f);   // D-262
			Settings.DetailFreq0 = FMath::Max(CVarUniGalDetailFreq0.GetValueOnGameThread(), 0.0f);   // D-265
			Settings.DetailLight0 = FMath::Clamp(CVarUniGalDetailLight0.GetValueOnGameThread(), 0.0f, 1.0f);
			Settings.DetailDust0 = FMath::Clamp(CVarUniGalDetailDust0.GetValueOnGameThread(), 0.0f, 1.0f);
			Settings.DetailFreq1 = FMath::Max(CVarUniGalDetailFreq1.GetValueOnGameThread(), 0.0f);
			Settings.DetailLight1 = FMath::Clamp(CVarUniGalDetailLight1.GetValueOnGameThread(), 0.0f, 1.0f);
			Settings.DetailDust1 = FMath::Clamp(CVarUniGalDetailDust1.GetValueOnGameThread(), 0.0f, 1.0f);
			Settings.DetailSteps = FMath::Clamp(CVarUniGalDetailSteps.GetValueOnGameThread(), 0.0f, 1024.0f);
			Settings.DetailStepTexels = FMath::Clamp(CVarUniGalDetailStepTexels.GetValueOnGameThread(), 0.25f, 64.0f);
			Settings.DetailFoot = FMath::Clamp(CVarUniGalDetailFoot.GetValueOnGameThread(), 0.0f, 4.0f);
			Settings.DetailRange0 = FMath::Clamp(CVarUniGalDetailRange0.GetValueOnGameThread(), 0.0f, 4.0f);   // D-266
			Settings.DetailRange1 = FMath::Clamp(CVarUniGalDetailRange1.GetValueOnGameThread(), 0.0f, 4.0f);
			Settings.DetailFade0 = FMath::Clamp(CVarUniGalDetailFade0.GetValueOnGameThread(), 0.01f, 1.0f);
			Settings.DetailFade1 = FMath::Clamp(CVarUniGalDetailFade1.GetValueOnGameThread(), 0.01f, 1.0f);
			Settings.DetailKnot0 = FMath::Clamp(CVarUniGalDetailKnots0.GetValueOnGameThread(), 0.0f, 1.0f);   // D-267
			Settings.DetailKnot1 = FMath::Clamp(CVarUniGalDetailKnots1.GetValueOnGameThread(), 0.0f, 1.0f);
			Settings.DetailKnotTint = FMath::Clamp(CVarUniGalDetailKnotTint.GetValueOnGameThread(), 0.0f, 1.0f);
			Settings.NebulaPrevalence = CVarUniNebulaPrevalence.GetValueOnGameThread();
			Settings.NebulaFineScale = CVarUniNebulaFineScale.GetValueOnGameThread();
			Settings.NebulaFineOctave = CVarUniNebulaFineOctave.GetValueOnGameThread();
			Settings.NebulaFineLodBias = CVarUniNebulaFineLodBias.GetValueOnGameThread();
			Settings.NebulaFineStepLod = CVarUniNebulaFineStepLod.GetValueOnGameThread();   // D-251
			Settings.NebulaStepRatio = FMath::Max(CVarUniNebulaStepRatio.GetValueOnGameThread(), 0.0f);
			Settings.NebulaPeak = FMath::Max(CVarUniNebulaPeak.GetValueOnGameThread(), 0.0f);   // D-253
			Settings.TailDim = (SkyMode & 2) ? 1.0f - FMath::Clamp(CVarUniSkyWebTail.GetValueOnGameThread(), 0.0f, 1.0f) : 0.0f;
		}
		{
			// D-230: the motion web, ramped by the travel curve; it reads the web texture's
			// filament and node channels, so it needs their mean (no texture: off).
			// D-231: its own ramp, from the speed (MotionWebRamp, updated with the camera).
			const double Strength = double(FMath::Max(CVarUniMotionWeb.GetValueOnGameThread(), 0.0f)) * MotionWebRamp.Value;
			const UniHlsl::float4 Mean = WebTexture.IsValid() ? WebTexture->Mean() : UniHlsl::float4(0.0f, 0.0f, 0.0f, 0.0f);
			const double FnMean = double(Mean.y) + double(Mean.z);
			if (Strength > 0.0 && FnMean > 0.0)
			{
				const double Mpc = 3.0856775814913673e24;
				Settings.MotionWebGain = float(Strength * 7.0e4 / Mpc);   // a galaxy disc's surface brightness (~7e4 per sr, VOLUMETRICS section 8) per Mpc
				Settings.MotionWebInvRef = float(1.0 / (FnMean * double(FMath::Max(CVarUniMotionWebThreshold.GetValueOnGameThread(), 1.0f))));
				Settings.MotionWebNearCm = float(double(FMath::Max(CVarUniMotionWebNear.GetValueOnGameThread(), 0.01f)) * Mpc);
				Settings.MotionWebDepthCm = float(double(FMath::Max(CVarUniMotionWebDepth.GetValueOnGameThread(), 1.0f)) * Mpc);
				// D-231: the organic detail (galaxy_detail; off when the galaxy textures are not loaded).
				const bool bDetailTex = GalaxyTextures[2].IsValid();
				Settings.MotionWebDetailGain = bDetailTex ? FMath::Clamp(CVarUniMotionWebDetail.GetValueOnGameThread(), 0.0f, 1.0f) : 0.0f;
				Settings.MotionWebDetailCm = float(double(FMath::Max(CVarUniMotionWebDetailMpc.GetValueOnGameThread(), 0.5f)) * Mpc);
				Settings.MotionWebStepCm = float(double(FMath::Max(CVarUniMotionWebStepMpc.GetValueOnGameThread(), 0.0f)) * Mpc);
				Settings.MotionWebRegionCm = float(double(FMath::Max(CVarUniMotionWebRegionMpc.GetValueOnGameThread(), 0.0f)) * Mpc);   // D-233
			}
		}
		{
			// D-269: the web's dark dust; it reads the web texture's filament and node channels over
			// their mean (no texture: off) and galaxy_shape for its pattern (none loaded: the matter alone).
			const UniHlsl::float4 Mean = WebTexture.IsValid() ? WebTexture->Mean() : UniHlsl::float4(0.0f, 0.0f, 0.0f, 0.0f);
			const double FnMean = double(Mean.y) + double(Mean.z);
			const double Tau = double(FMath::Clamp(CVarUniWebDust.GetValueOnGameThread(), 0.0f, 100.0f));
			if (Tau > 0.0 && FnMean > 0.0)
			{
				const double Mpc = 3.0856775814913673e24;
				Settings.WebDustTauCm = float(Tau / (100.0 * Mpc));
				Settings.WebDustInvMean = float(1.0 / FnMean);
				Settings.WebDustNearCm = float(double(FMath::Max(CVarUniWebDustNearMpc.GetValueOnGameThread(), 0.01f)) * Mpc);
				Settings.WebDustDepthCm = float(double(FMath::Max(CVarUniWebDustDepthMpc.GetValueOnGameThread(), 1.0f)) * Mpc);
				const bool bShape = GalaxyTextures[2].IsValid();
				Settings.WebDustDetail = bShape ? FMath::Clamp(CVarUniWebDustDetail.GetValueOnGameThread(), 0.0f, 1.0f) : 0.0f;
				Settings.WebDustDetailCm = float(double(FMath::Max(CVarUniWebDustDetailMpc.GetValueOnGameThread(), 0.5f)) * Mpc);
				Settings.WebDustKnots = FMath::Clamp(CVarUniWebDustKnots.GetValueOnGameThread(), 0.0f, 1.0f);
				Settings.WebDustPeak = FMath::Max(CVarUniWebDustPeak.GetValueOnGameThread(), 0.0f);
				Settings.WebDustTexN = bShape ? float(GalaxyTextures[2]->Size) : 128.0f;
				Settings.WebDustRedden = FMath::Clamp(CVarUniWebDustRedden.GetValueOnGameThread(), 0.0f, 1.0f);
			}
			// D-271: the sprites' transmittance (the march's slices) out to 50 Mpc, or to the web dust's
			// depth if farther: a galaxy behind a dark filament, or behind another galaxy's dust, dims.
			Settings.SliceFarCm = FMath::Max(50.0 * 3.0856775814913673e24, double(Settings.WebDustDepthCm));
		}
		const UniCore::FVolumeScene Sc = VolumeBuilder->Build(*Streamer, Cam, UniCore::FVec3f{ CameraFraction.X, CameraFraction.Y, CameraFraction.Z }, V, Settings);
		UpdateLensTarget(F, Sc, Cam);   // D-273
		for (const UniCore::FCarrierGPU& C : Sc.Carriers)
		{
			((C.Flags & UniHlsl::UNI_CARRIER_PERIODIC) ? F.BackgroundCarriers : F.VolumeCarriers).Add(C);
		}
		F.VolumeParams.Append(Sc.Params.data(), int32(Sc.Params.size()));
		F.VolumeLut.Reserve(int32(Sc.Lut.size()));
		for (const UniHlsl::float4& L : Sc.Lut) { F.VolumeLut.Add(FVector4f(L.x, L.y, L.z, L.w)); }
		F.VolumeView = Sc.View;
		{
			// D-248: the nebulae's tuning, to the GPU passes and (when it changes) the C++ cores'
			// globals, which the sky light's worker march reads (written here only on a change: the
			// worker reads them while a job runs).
			const UniCore::FNebulaTune Nt = UniCore::NebulaTune(Settings);
			F.NebTune = FVector4f(Nt.T[0], Nt.T[1], Nt.T[2], Nt.T[3]);
			F.NebTune2 = FVector4f(Nt.T2[0], Nt.T2[1], Nt.T2[2], Nt.T2[3]);
			F.NebTune3 = FVector4f(Nt.T3[0], Nt.T3[1], Nt.T3[2], Nt.T3[3]);   // D-251
			if (UniHlsl::GNebTune.x != Nt.T[0] || UniHlsl::GNebTune.y != Nt.T[1] || UniHlsl::GNebTune.z != Nt.T[2] || UniHlsl::GNebTune.w != Nt.T[3]
				|| UniHlsl::GNebTune2.x != Nt.T2[0] || UniHlsl::GNebTune2.y != Nt.T2[1] || UniHlsl::GNebTune2.z != Nt.T2[2] || UniHlsl::GNebTune2.w != Nt.T2[3]
				|| UniHlsl::GNebTune3.x != Nt.T3[0] || UniHlsl::GNebTune3.y != Nt.T3[1])
			{
				UniCore::ApplyNebulaTune(Nt);
			}
		}
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
		{
			// D-252: the nebula grid, for the camera's spiral with its nebulae on: its shape in the
			// spiral's field units (the handles are physical lengths), what its bake reads (a change rebakes),
			// the camera and the layer's mid-plane.
			// D-254 (owner: "the level where i go from no nebulae to populated nebulae ... pops in some
			// middling distance from the galaxy"): not only the galaxy the camera is in (D-211's home: the
			// grid switched on at its sphere, its nebulae appearing with it) but the nearest spiral whose
			// disc is within the grid's reach, from outside too: the grid takes over where its coarsest
			// levels and the field's distance level of detail both show the nebulae's mean.
			F.NebGrid = FUniFrameData::FNebGridFrame();
			const double ReachCm = double(FMath::Max(CVarUniNebulaGridReachKpc.GetValueOnGameThread(), 0.1f)) * 3.0856775814913673e21;
			const UniCore::FCarrierGPU* Home = UniCore::HomeCarrierOf(Sc);
			const UniCore::FCarrierGPU* Pick = nullptr;
			double PickR = 0.0;
			if (CVarUniNebulaGrid.GetValueOnGameThread() != 0 && Settings.NebulaGain > 0.0f)
			{
				for (const UniCore::FCarrierGPU& Cg : Sc.Carriers)
				{
					if ((Cg.Flags & UniHlsl::UNI_CARRIER_PERIODIC) != 0u || Cg.FieldId != UniHlsl::UNI_FIELD_GALAXY || !(Cg.Weight > 0.0f) || !(Cg.NebulaHide < 1.0f) || Cg.ParamIndex >= Sc.Params.size()) { continue; }
					UniHlsl::FUniFieldParams Pq;
					std::memcpy(static_cast<void*>(&Pq), &Sc.Params[Cg.ParamIndex], sizeof(Pq));
					if (!(Pq.H.y > 0.0f) || UniHlsl::Uni_GalaxyEarlyNebula(Pq) > 0.0f) { continue; }
					// The camera's distance from the galaxy's centre in its radii; the disc within reach.
					const double Rc = std::sqrt(double(Cg.CamLocal[0]) * Cg.CamLocal[0] + double(Cg.CamLocal[1]) * Cg.CamLocal[1] + double(Cg.CamLocal[2]) * Cg.CamLocal[2]);
					const double RadCm = 1.0 / double(FMath::Max(Cg.InvRadius, 1.0e-30f));
					if ((Rc - 1.0) * RadCm > ReachCm) { continue; }
					if (!Pick || Rc < PickR || (&Cg == Home)) { Pick = &Cg; PickR = Rc; }
					if (&Cg == Home) { break; }
				}
			}
			if (Pick)
			{
				UniHlsl::FUniFieldParams Par;
				std::memcpy(static_cast<void*>(&Par), &Sc.Params[Pick->ParamIndex], sizeof(Par));
				{
					const double RadiusCm = 1.0 / double(FMath::Max(Pick->InvRadius, 1.0e-30f));
					const double Pc = 3.0856775814913673e18;
					const UniCore::u32 GridCells = CVarUniNebulaGridCells.GetValueOnGameThread() >= 256 ? 256u : 128u;
					UniCore::FNebGridConfig Cfg;
					Cfg.Cells = GridCells;
					Cfg.Cell0 = double(FMath::Clamp(CVarUniNebulaGridCellPc.GetValueOnGameThread(), 0.5f, 64.0f)) * Pc / RadiusCm;
					Cfg.LayerHalf = double(FMath::Clamp(CVarUniNebulaGridLayer.GetValueOnGameThread(), 0.5f, 8.0f)) * double(UNI_NEBULA_HEIGHT) * double(Par.A.y) + 50.0 * Pc / RadiusCm;
					Cfg.KnotTexel = double(UniHlsl::Uni_GalaxyNebulaKnotTexel(Par));
					Cfg.Levels = UniCore::NebGridLevelsFor(ReachCm / RadiusCm, GridCells, Cfg.Cell0);
					Cfg.PoolScale = double(FMath::Clamp(CVarUniNebulaGridPool.GetValueOnGameThread(), 0.1f, 8.0f));
					Cfg.Samples = CVarUniNebulaGridSamples.GetValueOnGameThread() >= 8 ? 8u : 1u;
					// What the bake reads: the galaxy (its parameters, its seeds among them), the nebulae's
					// tuning, detail and dark share, the textures (each enable).
					uint64 Key = 0x6A09E667F3BCC909ull;
					auto Mix = [&Key](uint64 InBits) { Key = (Key ^ InBits) * 0x100000001B3ull; Key ^= Key >> 29; };
					auto MixF = [&Mix](float InValue) { uint32 Bits; std::memcpy(&Bits, &InValue, 4); Mix(uint64(Bits)); };
					for (int32 Word = 0; Word < int32(sizeof(Par) / 4); ++Word) { uint32 Bits; std::memcpy(&Bits, reinterpret_cast<const uint8*>(&Par) + 4 * Word, 4); Mix(uint64(Bits) + uint64(Word) * 0x9E3779B97F4A7C15ull); }
					MixF(F.NebTune.X); MixF(F.NebTune.Y); MixF(F.NebTune.Z); MixF(F.NebTune.W);
					MixF(F.NebTune2.X); MixF(F.NebTune2.Y); MixF(F.NebTune2.Z); MixF(F.NebTune2.W);
					MixF(F.NebTune3.Y);   // D-253: the peak
					MixF(Settings.NebulaFineGain); MixF(Settings.NebulaDark);
					Mix(uint64(NebGridEpoch));
					F.NebGrid.bOn = true;
					F.NebGrid.ParamIndex = Pick->ParamIndex;
					// The camera in its field units: the home's in double (D-211), another's from its carrier.
					for (int32 Axis = 0; Axis < 3; ++Axis) { F.NebGrid.CamField[Axis] = (Pick == Home) ? Sc.Home.CamField[Axis] : double(Pick->CamLocal[Axis]); }
					float Zc = 0.0f, Fl = 1.0f;
					UniHlsl::Uni_GalaxyLayerMid(UniHlsl::float3(float(F.NebGrid.CamField[0]), float(F.NebGrid.CamField[1]), float(F.NebGrid.CamField[2])), Par, UniHlsl::Uni_GalaxyConstOf(Par).Hp, Zc, Fl);
					F.NebGrid.MidZ = double(Zc);
					F.NebGrid.Config = Cfg;
					F.NebGrid.Key = Key;
					F.NebGrid.Budget = UniCore::u32(FMath::Clamp(CVarUniNebulaGridBudget.GetValueOnGameThread(), 1, 65535));
					F.NebGrid.FetchMax = FMath::Clamp(CVarUniNebulaGridFetches.GetValueOnGameThread(), 1.0f, 128.0f);
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
	// D-299: the view's basis in the universe's axes.
	const FRotationMatrix R(ViewRot);
	const UniCore::FVec3d Fw = ToUniversalAxes(R.GetUnitAxis(EAxis::X)), Rt = ToUniversalAxes(R.GetUnitAxis(EAxis::Y)), Up = ToUniversalAxes(R.GetUnitAxis(EAxis::Z));
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

UniCore::FStellarBhConfig UUniSubsystem::StellarBhConfig() const
{
	UniCore::FStellarBhConfig C;
	C.Fraction = FMath::Clamp(CVarUniBhStellarFraction.GetValueOnGameThread(), 0.0f, 1.0f);
	C.Active = FMath::Clamp(CVarUniBhStellarActive.GetValueOnGameThread(), 0.0f, 1.0f);
	C.SizeScale = FMath::Clamp(CVarUniBhStellarScale.GetValueOnGameThread(), 1.0f, 1.0e4f);
	C.Light = NucleusConfig();
	C.Style = StyleConfig();
	return C;
}

UniCore::FStyleConfig UUniSubsystem::StyleConfig() const
{
	UniCore::FStyleConfig C;
	C.Variety = FMath::Clamp(CVarUniBhDiscVariety.GetValueOnGameThread(), 0.0f, 1.0f);
	C.PaletteMode = float(FMath::Clamp(CVarUniBhDiscPalette.GetValueOnGameThread(), -1, int32(UniHlsl::UNI_BH_PALETTES) - 1));
	return C;
}

UniCore::FNucleusConfig UUniSubsystem::NucleusConfig() const
{
	UniCore::FNucleusConfig C;
	C.Active = FMath::Clamp(CVarUniBhActiveFraction.GetValueOnGameThread(), 0.0f, 1.0f);
	C.Cap = FMath::Clamp(CVarUniBhNucleusCap.GetValueOnGameThread(), 0.0f, 10.0f);
	C.Gain = FMath::Max(CVarUniBhNucleusGain.GetValueOnGameThread(), 0.0f);
	return C;
}

void UUniSubsystem::UpdateLensTarget(FUniFrameData& F, const UniCore::FVolumeScene& Sc, const UniCore::FCoord& Cam)
{
	// D-273: the black hole the lens pass draws: of the galaxies the camera is in or near (the volume's
	// home galaxy and its local carriers), the one whose ring or disc covers the most pixels. While it
	// runs the pass draws that galaxy's nucleus too (its sprite held back: in scene colour it would be
	// lensed as if it were behind the black hole).
	F.Lens = FUniFrameData::FLensFrame();
	F.NucleusParams.W = 0.0f;
	F.NucleusHandSeed = 0u;
	LensLast = UniCore::FLensTarget();
	if (!Streamer || CVarUniBhLens.GetValueOnGameThread() == 0) { return; }
	UniCore::FLensConfig Lc;
	Lc.bLens = true;
	Lc.bDisc = CVarUniBhDisc.GetValueOnGameThread() != 0 && CVarUniBhNucleus.GetValueOnGameThread() != 0;
	Lc.RingMinPx = FMath::Max(CVarUniBhLensRingMinPx.GetValueOnGameThread(), 0.01f);
	Lc.DiscFromPx = FMath::Max(CVarUniBhDiscFromPx.GetValueOnGameThread(), 0.1f);
	Lc.DiscToPx = FMath::Max(CVarUniBhDiscToPx.GetValueOnGameThread(), Lc.DiscFromPx + 0.1f);
	Lc.BendMinPx = FMath::Max(CVarUniBhLensBendMinPx.GetValueOnGameThread(), 0.01f);
	Lc.MaxSteps = UniCore::u32(FMath::Clamp(CVarUniBhLensMaxSteps.GetValueOnGameThread(), 16, 2000));
	Lc.LensScale = FMath::Clamp(CVarUniBhLensScale.GetValueOnGameThread(), 1.0f, 10.0f);   // D-277
	const double ThetaPixel = double(FMath::Max(FUniRenderer::Get().Stats.ThetaPixelLastFrame.load(), 1.0e-6f));
	const UniCore::FNucleusConfig Nc = NucleusConfig();
	const UniCore::FStyleConfig Stc = StyleConfig();   // D-280
	TArray<UniCore::FObjectId, TInlineAllocator<16>> Cands;
	if (Sc.Home.Carrier >= 0) { Cands.Add(Sc.Home.Parent); }
	for (const UniCore::FCarrierHandoff& Hd : Sc.Handoffs) { Cands.AddUnique(Hd.Parent); }
	UniCore::FLensTarget Best;
	UniHlsl::FUniBhStyle BestStyle{};
	for (const UniCore::FObjectId& Id : Cands)
	{
		const UniCore::FObjectGPU* O = Streamer->FindObject(Id);
		if (!O) { continue; }
		const uint32 Arch = O->A[3] & 0xFFFFu;
		if (Arch >= 32u || ((DiscArchetypeMask >> Arch) & 1u) == 0u) { continue; }
		const UniHlsl::FUniObject Ho = UniCore::ToHlsl(*O);
		const UniHlsl::FUniBlackHole Bh = UniHlsl::Uni_GalaxyBlackHole(Ho, Nc.Active);
		const float Mn = CVarUniBhNucleus.GetValueOnGameThread() != 0 ? UniHlsl::Uni_NucleusAbsMag(Bh, UniHlsl::Uni_ObjectAbsMag(Ho), Nc.Cap, Nc.Gain) : UniHlsl::UNI_BH_NONE_MAG;
		const UniCore::FVec3d Rel = UniCore::ToDouble(UniCore::Sub(UniCore::ObjectCoord(Id.Cell, O->A), Cam));
		const double RelCm[3] = { Rel.X - double(CameraFraction.X), Rel.Y - double(CameraFraction.Y), Rel.Z - double(CameraFraction.Z) };
		const uint32 Seed = O->B[0];
		bool bFixed = false;
		for (int32 K = 0; K < 4; ++K) { bFixed |= Seed != 0u && FixedDiscSeeds[K] == Seed; }
		float Axis[3];
		UniCore::GalaxyDiscAxis(Seed, bFixed, Axis);
		// D-280: its look (its host archetype's arms for the spiral).
		const UniHlsl::FUniBhStyle Sty = UniCore::GalaxyBhStyle(*O, Model ? &Model->Definition().Arch(Arch) : nullptr, Stc, Nc.Active);
		const UniCore::FLensTarget T = UniCore::LensTargetOf(Bh, Mn, RelCm, Axis, Seed, ThetaPixel, Lc, &Sty);
		if (T.bActive && (!Best.bActive || T.Score > Best.Score)) { Best = T; BestStyle = Sty; }
	}
	// D-281: the stellar black holes of the active star systems.
	bool bBestStellar = false;
	if (Systems)
	{
		const UniCore::FStellarBhConfig Sbc = StellarBhConfig();
		for (const auto& KV : Systems->ActiveSystems())
		{
			const UniCore::FActiveSystem& Act = KV.second;
			const UniCore::FStellarBh& Sb = Act.Sys.Bh;
			if (!Sb.bValid) { continue; }
			UniCore::FCoord Bc = Act.Star;
			UniCore::FVec3f BcFrac{};
			UniCore::AddDisplacement(Bc, BcFrac, UniCore::StellarBhOffset(Sb, UniverseTime));
			const UniCore::FVec3d Rel = UniCore::ToDouble(UniCore::Sub(Bc, Cam));
			const double RelCm[3] = { Rel.X - double(CameraFraction.X), Rel.Y - double(CameraFraction.Y), Rel.Z - double(CameraFraction.Z) };
			const double Dist = FMath::Sqrt(RelCm[0] * RelCm[0] + RelCm[1] * RelCm[1] + RelCm[2] * RelCm[2]);
			const UniCore::FStellarBhLight Lt = UniCore::StellarBhLightOf(Sb, Act.Sys.StarAbsMag, Act.Sys.ActivationCm, Dist, Sbc);
			const float Mn = CVarUniBhNucleus.GetValueOnGameThread() != 0 ? Lt.AbsMagIso : UniHlsl::UNI_BH_NONE_MAG;
			const UniCore::FLensTarget T = UniCore::LensTargetOf(Sb.Core, Mn, RelCm, Sb.Normal, Sb.Seed, ThetaPixel, Lc, &Lt.Style, double(Sbc.SizeScale));
			if (T.bActive && (!Best.bActive || T.Score > Best.Score)) { Best = T; BestStyle = Lt.Style; bBestStellar = true; }
		}
	}
	LensStellarLast = bBestStellar;
	if (!Best.bActive || !(Best.DistanceCm > 0.0)) { return; }
	LensLast = Best;
	LensStyleLast = BestStyle;
	FUniFrameData::FLensFrame& L = F.Lens;
	L.bActive = true;
	L.CamRs = FVector3f(float(-Best.RelCm[0] / Best.RsCm), float(-Best.RelCm[1] / Best.RsCm), float(-Best.RelCm[2] / Best.RsCm));
	L.BhDir = FVector3f(float(Best.RelCm[0] / Best.DistanceCm), float(Best.RelCm[1] / Best.DistanceCm), float(Best.RelCm[2] / Best.DistanceCm));
	L.BhDistanceCm = float(Best.DistanceCm);
	L.RegionRad = Best.RegionRad;
	L.DiscAxis = FVector3f(Best.Axis[0], Best.Axis[1], Best.Axis[2]);
	L.DiscOuterX = Best.OuterX;
	L.DiscPeakK = Best.PeakK;
	L.bDisc = Best.bDisc;
	L.DiscShare = Best.DiscShare;
	L.DiscLog10Scale = Best.DiscLog10Scale;
	L.MaxSteps = Lc.MaxSteps;
	L.TimeS = GetWorld() ? double(GetWorld()->GetTimeSeconds()) : 0.0;   // D-276: the disc turns with the game's clock (pauses with it)
	L.DiscSeed = Best.NucleusSeed;
	L.PointAppMag = Best.PointAppMag;
	L.PointPhysAngRad = Best.PointPhysAngRad;
	L.PointColour = FVector3f(Best.PointColour[0], Best.PointColour[1], Best.PointColour[2]);
	// D-280: its look about the uu.Bh.Disc* settings (the renderer applies it to them).
	L.DiscStyle0 = FVector4f(BestStyle.SpiralMul, BestStyle.ThicknessMul, BestStyle.OpacityMul, BestStyle.CloudsMul);
	L.DiscStyle1 = FVector4f(BestStyle.BandsMul, BestStyle.HeatMul, BestStyle.ErosionMul, BestStyle.PeriodMul);
	L.DiscStyle2 = FVector4f(BestStyle.BrightnessMul, BestStyle.PaletteMix, float(BestStyle.Palette), BestStyle.ExtentMul);
	if (Best.PointAppMag < UniHlsl::UNI_BH_NONE_MAG && !bBestStellar)
	{
		// The pass draws this nucleus (its point x (1 - share), its disc x share): the sprite is held back.
		F.NucleusParams.W = 1.0f;
		F.NucleusHandSeed = Best.NucleusSeed;
	}
	if (bBestStellar)
	{
		// D-281: likewise its companion record's sprite (the pass draws its point and disc).
		for (UniCore::FObjectGPU& R : F.DynamicRecords)
		{
			if (R.B[0] == Best.NucleusSeed && (R.A[3] & 0xFFFFu) == UniHlsl::UNI_ARCH_DYNAMIC_BH) { R.A[3] |= UniHlsl::UNI_OBJ_FLAG_PROMOTED << 16; }
		}
	}
}

bool UUniSubsystem::GetGalaxyBlackHole(const FUniObjectId& Id, FUniBlackHoleInfo& OutBlackHole) const
{
	OutBlackHole = FUniBlackHoleInfo();
	if (!Streamer) { return false; }
	const UniCore::FObjectId C = Id.ToCore();
	const UniCore::FObjectGPU* O = Streamer->FindObject(C);
	if (!O) { return false; }
	const uint32 Arch = O->A[3] & 0xFFFFu;
	if (Arch >= 32u || ((DiscArchetypeMask >> Arch) & 1u) == 0u) { return false; }   // galaxies only (a galaxy-field emitter)
	const UniCore::FBlackHoleInfo B = UniCore::GalaxyBlackHole(*O, NucleusConfig());
	OutBlackHole.bValid = B.bValid;
	OutBlackHole.MassSolar = B.MassSolar;
	OutBlackHole.EddingtonRatio = B.EddingtonRatio;
	OutBlackHole.LuminosityErgS = B.LuminosityErgS;
	OutBlackHole.SchwarzschildRadiusCm = B.SchwarzschildRadiusCm;
	OutBlackHole.NucleusAbsoluteMagnitude = B.NucleusAbsMag;
	{
		// D-273: its disc (the lens pass's).
		const uint32 Seed = O->B[0];
		bool bFixed = false;
		for (int32 K = 0; K < 4; ++K) { bFixed |= Seed != 0u && FixedDiscSeeds[K] == Seed; }
		float Axis[3];
		UniCore::GalaxyDiscAxis(Seed, bFixed, Axis);
		OutBlackHole.DiscAxis = FVector(Axis[0], Axis[1], Axis[2]);
		OutBlackHole.DiscOuterRadiusCm = double(B.DiscOuterRs) * B.SchwarzschildRadiusCm;
		OutBlackHole.DiscPeakTemperatureK = B.DiscPeakK;
	}
	OutBlackHole.Coord = FUniCoord::Make(UniCore::ObjectCoord(C.Cell, O->A));   // the galaxy's centre
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
	if (LensLast.bActive)   // D-273
	{
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: black hole lens: %.4g R_s away (R_s %.3g cm), Einstein ring %.1f px, disc %s (outer %.0f R_s = %.1f px, peak %.0f K, %.0f%% of the nucleus's light), region %.3g rad, pass %s"),
			LensLast.DistanceCm / LensLast.RsCm, LensLast.RsCm, double(LensLast.RingPx), LensLast.bDisc ? TEXT("on") : TEXT("off"), double(LensLast.OuterX), double(LensLast.DiscPx), double(LensLast.PeakK),
			100.0 * double(LensLast.DiscShare), double(LensLast.RegionRad), R.LensPassesLastFrame.load() ? TEXT("ran") : TEXT("did not run"));
		if (LensStellarLast) { UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: black hole lens: a stellar black hole (an X-ray binary's; uu.Bh.StellarScale x%.0f)"), double(StellarBhConfig().SizeScale)); }
		static const TCHAR* PaletteNames[UniHlsl::UNI_BH_PALETTES] = { TEXT("blackbody"), TEXT("gold"), TEXT("quasar blue"), TEXT("ember"), TEXT("violet"), TEXT("teal"), TEXT("rose"), TEXT("ice and fire") };
		const UniHlsl::FUniBhStyle& Sl = LensStyleLast;   // D-280
		UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: black hole look (x the uu.Bh.Disc* settings): palette %s (%.0f%%), spiral x%.2f, thickness x%.2f, opacity x%.2f, clouds x%.2f, bands x%.2f, heat x%.2f, erosion x%.2f, period x%.2f, brightness x%.2f, extent x%.2f"),
			PaletteNames[FMath::Min(Sl.Palette, UniHlsl::UNI_BH_PALETTES - 1u)], 100.0 * double(Sl.PaletteMix), double(Sl.SpiralMul), double(Sl.ThicknessMul), double(Sl.OpacityMul), double(Sl.CloudsMul),
			double(Sl.BandsMul), double(Sl.HeatMul), double(Sl.ErosionMul), double(Sl.PeriodMul), double(Sl.BrightnessMul), double(Sl.ExtentMul));
	}
	else { UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: black hole lens: none (no galaxy's black hole near enough%s)"), bVolume ? TEXT("") : TEXT("; the volume is off")); }
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
TEXT("uu.LightRig"), 1, TEXT("M3 placeholder: a movable directional light from the nearest active star, so promoted planets are lit (0 off). D-294: off while the planet whose frame the world is has its detailed body live and ready (its atmosphere lights the scene)."));
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
	PromotedLinks.Reset();   // D-297
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
	if (WorldTurn.Yaw == 0.0)
	{
		UniCore::ShiftAnchor(A, S);
	}
	else
	{
		// D-299: the world turned against the universe: the shift in the universe's axes is not
		// whole centimetres; the anchor's fraction takes the rest.
		UniCore::FVec3f Fr{ LocalAnchorFrac.X, LocalAnchorFrac.Y, LocalAnchorFrac.Z };
		UniCore::AddDisplacement(A, Fr, ToUniversalAxes(FVector(S.X, S.Y, S.Z)));
		LocalAnchorFrac = FVector3f(Fr.X, Fr.Y, Fr.Z);
	}
	LocalAnchor = FUniCoord::Make(A);
	ShiftWorld(FVector(-S.X, -S.Y, -S.Z), InOutViewLoc);
	++Recentres;
	UE_LOG(LogUnifiedUniverse, Verbose, TEXT("UnifiedUniverse: Local frame re-centred by %s (re-centre %llu)"), *FVector(-S.X, -S.Y, -S.Z).ToString(), Recentres);
}

void UUniSubsystem::ShiftWorld(const FVector& Shift, FVector& InOutViewLoc)
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (APawn* Pawn = PC ? PC->GetPawn() : nullptr)
	{
		Pawn->SetActorLocation(Pawn->GetActorLocation() + Shift, false, nullptr, ETeleportType::TeleportPhysics);
	}
	InOutViewLoc += Shift;
	for (const TWeakObjectPtr<UUniParallaxComponent>& W : ParallaxComponents)
	{
		UUniParallaxComponent* C = W.Get();
		if (C && C->Mode == EUniParallaxMode::Simulated && C->bRealSpace && C->GetOwner()) { C->GetOwner()->AddActorWorldOffset(Shift, false, nullptr, ETeleportType::TeleportPhysics); }
	}
	OnLocalRecentre.Broadcast(Shift);
	// The camera cache is what this frame renders from: refresh it, or the view lags the shift by a frame.
	if (PC && PC->PlayerCameraManager) { PC->PlayerCameraManager->UpdateCamera(0.0f); }
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
	// Demoted: destroy (pooling comes with real body actors). D-297: and forget whether a factory
	// makes them.
	for (auto It = PromotedLinks.CreateIterator(); It; ++It)
	{
		if (P.count(It.Key()) == 0) { It.RemoveCurrent(); }
	}
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
		// D-297: a planet a body factory makes gets its detailed body (UpdateDetailedBodies), not a
		// placeholder sphere.
		if (!WantsPlaceholder(Body)) { continue; }
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
		if (MotionMode == EUniMotionMode::Local) { C->ApplyPlacementLocal(ViewLoc, Camera, CameraFraction, LocalAnchor, LocalAnchorFrac, WorldTurn.Yaw); }
		else { C->ApplyPlacement(ViewLoc, Camera, CameraFraction, WorldTurn.Yaw); }
	}
}

void UUniSubsystem::UpdateLightRig()
{
	UWorld* World = GetWorld();
	// D-294: a planet's detailed body that is live and ready lights the scene itself (its
	// atmosphere's sun light, aimed by IStarLit): a second directional light would compete with it
	// for the atmosphere and forward shading.
	const bool bBodyLit = bDetailedLit;   // D-297: any live, ready detailed body with air
	const bool bWant = CVarUniLightRig.GetValueOnGameThread() != 0 && Systems && !Systems->ActiveSystems().empty() && World && !bBodyLit;
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
	const FVector Dir = ToWorldAxes(D).GetSafeNormal();   // D-299: the world's axes
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
	const double Yaw = WorldTurn.Yaw;   // D-299: the cube in the world's axes (the sky light samples it so)
	SkyJob = Async(EAsyncExecution::ThreadPool, [Copy, N, Scale, Yaw]() { return UniCore::BuildSkyCube(*Copy, N, Scale, 2, Yaw); });
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
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr) { PC->SetControlRotation(WorldRotationOf(FVector(-Dir.X, -Dir.Y, -Dir.Z))); }
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

bool UUniSubsystem::GoToStellarBlackHole()
{
	// D-281: as GoToSystem: the nearest resident star (the reference generator's content) with a
	// black hole companion, then 3 disc radii from the hole, 20 degrees above its disc, facing it.
	if (!bEnabled || !Generator) { return false; }
	const UniCore::FCoord Here = Camera.ToCore();
	UniCore::FRefPipeline Resolve(*Generator);
	Resolve.bVelocityLod = false;
	Resolve.Update(Here, UniCore::FVec3f{});
	const UniCore::FStellarBhConfig Sbc = StellarBhConfig();
	double Best = 1000.0 * UniCore::Units::Pc;
	UniCore::FCoord Star;
	UniCore::FSystem Sys;
	bool bFound = false;
	size_t Stars = 0;
	Resolve.ForEachResident([&](const UniCore::FCellKey& K, const UniCore::FObjectGPU& O, float, UniCore::u64)
	{
		const UniCore::u32 Arch = O.A[3] & 0xFFFFu;
		if (Arch >= 32u || ((SystemArchetypeMask >> Arch) & 1u) == 0u || ((O.A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN)) { return; }
		++Stars;
		if (!UniHlsl::Uni_StarHasBlackHole(O.B[0], Sbc.Fraction)) { return; }
		const UniCore::u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
		const UniCore::FCoord C = UniCore::ObjectCoord(K, Off);
		const double D = UniCore::Length(UniCore::ToDouble(UniCore::Sub(C, Here)));
		if (D >= Best) { return; }
		Best = D; Star = C; Sys = UniCore::BuildSystem(O, Sbc); bFound = true;
	});
	if (!bFound || !Sys.Bh.bValid)
	{
		UE_LOG(LogUnifiedUniverse, Warning, TEXT("UnifiedUniverse: no stellar black hole among the %llu resident stars within 1 kpc (uu.Bh.StellarFraction %.4g; raise it, or go where the stars are dense)."), uint64(Stars), double(Sbc.Fraction));
		return false;
	}
	const UniCore::FStellarBh& Sb = Sys.Bh;
	const UniHlsl::FUniBhStyle Sty = UniHlsl::Uni_BhStyleOf(Sb.Seed, Sb.Core, false, -1.0f, Sbc.Style.Variety, Sbc.Style.PaletteMode);
	const double Rs = std::pow(10.0, double(UniHlsl::UNI_BH_LOG_RS_SUN_CM) + double(Sb.Core.Log10Mass)) * double(Sbc.SizeScale);
	const double Outer = double(UniHlsl::Uni_BhDiscOuterX(UniHlsl::Uni_BhDiscPeakK(Sb.Core))) * double(FMath::Max(Sty.ExtentMul, 0.05f)) * Rs;
	const UniCore::FVec3d Off = UniCore::StellarBhOffset(Sb, UniverseTime);
	const FVector N(Sb.Normal[0], Sb.Normal[1], Sb.Normal[2]);
	const FVector Along = FVector(Sb.E1[0], Sb.E1[1], Sb.E1[2]);
	const FVector Out = (Along * FMath::Cos(FMath::DegreesToRadians(20.0)) + N * FMath::Sin(FMath::DegreesToRadians(20.0))).GetSafeNormal();
	const double R = 3.0 * Outer;
	UniCore::FCoord To = Star;
	UniCore::FVec3f Frac{};
	UniCore::AddDisplacement(To, Frac, UniCore::FVec3d{ Off.X + Out.X * R, Off.Y + Out.Y * R, Off.Z + Out.Z * R });
	SetFreeFly(true, 0.2 * Outer);
	SetFlySpeed(0.2 * Outer);   // a fifth of a disc radius a second
	SetCamera(FUniCoord::Make(To));
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr) { PC->SetControlRotation(WorldRotationOf(-Out)); }
	UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: jumped to a stellar black hole of %.1f M_sun (%s, 1e%.1f of Eddington) beside a star of M = %.2f %s away; its disc %s across (x%.0f, uu.Bh.StellarScale), now %s from it. Its orbit about the star: %s, %.2f h."),
		FMath::Pow(10.0, double(Sb.Core.Log10Mass)), Sb.Core.Log10Eddington > -3.0f ? TEXT("in outburst") : TEXT("quiescent"), double(Sb.Core.Log10Eddington), Sys.StarAbsMag,
		UTF8_TO_TCHAR(UniCore::FormatDistance(Best).c_str()), UTF8_TO_TCHAR(UniCore::FormatDistance(2.0 * Outer).c_str()), double(Sbc.SizeScale), UTF8_TO_TCHAR(UniCore::FormatDistance(R).c_str()),
		UTF8_TO_TCHAR(UniCore::FormatDistance(Sb.SemiMajorCm).c_str()), Sb.PeriodS / 3600.0);
	return true;
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
		if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr) { PC->SetControlRotation(WorldRotationOf(-Out)); }
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
		PC->SetControlRotation(WorldRotationOf(FVector(-0.25, 0.9, -0.7).GetSafeNormal()));
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

	FAutoConsoleCommandWithWorldAndArgs CmdEnable(TEXT("uu.Enable"), TEXT("uu.Enable [galaxy|web|flythrough|home]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->EnableUniverse(Args.Num() ? Args[0] : FString()); }
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdReloadTextures(TEXT("uu.ReloadTextures"), TEXT("D-249: reload the volume textures (uu.GalaxyTexture.*, uu.WebTexture) by re-enabling the current scene where the camera is (the galaxies' and the web's stars reroll when their textures change)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->ReloadTextures(); }
		}));

	// D-261: the tuning page (Project Settings > Plugins > Unified Universe Tuning).
	FAutoConsoleCommandWithWorldAndArgs CmdTuningImport(TEXT("uu.Tuning.Import"), TEXT("D-261: the uu.* console variables' current values into the tuning page (after trying values at the console)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld*)
		{
			GetMutableDefault<UUniTuningSettings>()->ImportAll();
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: tuning page imported from the console variables (uu.Tuning.Save to keep them)."));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTuningSave(TEXT("uu.Tuning.Save"), TEXT("D-261: the tuning page's values to DefaultGame.ini (applied when play begins)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld*)
		{
			UUniTuningSettings* T = GetMutableDefault<UUniTuningSettings>();
			const bool bOk = T->TryUpdateDefaultConfigFile();
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: tuning page %s DefaultGame.ini."), bOk ? TEXT("saved to") : TEXT("NOT saved to (is it writable?)"));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTuningSavePreset(TEXT("uu.Tuning.SavePreset"), TEXT("D-261: uu.Tuning.SavePreset <name>: every tuning variable's current value to Saved/UniTuningPresets/<name>.txt (A/B configurations)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			const bool bOk = Args.Num() > 0 && UUniTuningSettings::SavePreset(Args[0]);
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: preset %s %s (%s)."), Args.Num() ? *Args[0] : TEXT("(no name)"), bOk ? TEXT("saved") : TEXT("NOT saved"), *UUniTuningSettings::PresetDir());
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTuningLoadPreset(TEXT("uu.Tuning.LoadPreset"), TEXT("D-261: uu.Tuning.LoadPreset <name>: a saved preset's values (the page follows; the scene re-enables if a value needs it)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			const bool bOk = Args.Num() > 0 && UUniTuningSettings::LoadPreset(Args[0]);
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: preset %s %s."), Args.Num() ? *Args[0] : TEXT("(no name)"), bOk ? TEXT("loaded") : TEXT("NOT found"));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTuningReset(TEXT("uu.Tuning.Reset"), TEXT("D-264: uu.Tuning.Reset [filter]: the tuning variables back to their defaults (as the module loaded them), all or those whose name starts with the filter (uu.Volume) or whose page category contains it (Nebulae, March|Steps). The page follows; the scene re-enables if a value needs it. DefaultGame.ini is untouched (uu.Tuning.Save keeps the reset)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
		{
			const FString Filter = FString::Join(Args, TEXT(" "));
			const int32 N = UUniTuningSettings::ResetToDefaults(Filter);
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: %d tuning variables%s reset to their defaults (uu.Tuning.Save keeps it)."), N, Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" matching '%s'"), *Filter));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTuningPresets(TEXT("uu.Tuning.Presets"), TEXT("D-261: list the saved tuning presets."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld*)
		{
			UE_LOG(LogUnifiedUniverse, Log, TEXT("UnifiedUniverse: presets in %s: %s"), *UUniTuningSettings::PresetDir(), *FString::Join(UUniTuningSettings::ListPresets(), TEXT(", ")));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdTuningReenable(TEXT("uu.Tuning.Reenable"), TEXT("D-261: re-enable the scene where the camera is (textures, shaping, populations: the settings that apply at enable)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->ReloadTextures(); }
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

	FAutoConsoleCommandWithWorldAndArgs CmdGotoStellarBh(TEXT("uu.GotoStellarBlackHole"), TEXT("uu.GotoStellarBlackHole: jump to the nearest stellar black hole (a star's companion, D-281), 3 disc radii out, above its disc (free flight)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UUniSubsystem* S = Sub(World)) { S->GoToStellarBlackHole(); }
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
