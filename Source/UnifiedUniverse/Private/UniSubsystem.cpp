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
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/PlatformTime.h"
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
		return T;
	}
}

// D-154: travel sensitivity. Speeds in cm/s (100 kpc/s = 3.09e23, 10 Mpc/s = 3.09e25).
static TAutoConsoleVariable<float> CVarUniTravelSensitivity(TEXT("uu.TravelSensitivity"), 2.5f, TEXT("Most sensitivity (mag) added while travelling fast (0: off). D-154."));
static TAutoConsoleVariable<float> CVarUniTravelStart(TEXT("uu.TravelStart"), 3.0857e23f, TEXT("Speed (cm/s) where travel sensitivity starts (default 100 kpc/s)."));
static TAutoConsoleVariable<int32> CVarUniSubmitDefer(TEXT("uu.SubmitDeferPerLevel"), 48, TEXT("D-173: entering cells not needed within the latency (plus a margin) are built at most this many per level per frame; -1: no limit (before D-173)."));
static TAutoConsoleVariable<float> CVarUniTravelFull(TEXT("uu.TravelFull"), 3.0857e25f, TEXT("Speed (cm/s) where travel sensitivity is full (default 10 Mpc/s)."));

// D-157: instrumentation and volume march quality.
static TAutoConsoleVariable<int32> CVarUniPerfHud(TEXT("uu.PerfHud"), 0, TEXT("Show the universe's per-phase cost on screen (D-157)."));
static TAutoConsoleVariable<float> CVarUniPerfHitchMs(TEXT("uu.PerfHitchMs"), 50.0f, TEXT("Log the breakdown of any frame whose universe work on the game thread exceeds this many ms (0: off)."));
static TAutoConsoleVariable<int32> CVarUniVolumeMaxSteps(TEXT("uu.VolumeMaxSteps"), 512, TEXT("Volume march step budget per ray (fewer: faster, a long ray may stop short; uu.VolumeDebug 1 shows truncation in magenta)."));
static TAutoConsoleVariable<float> CVarUniVolumeStepRatio(TEXT("uu.VolumeStepRatio"), 0.08f, TEXT("Volume march step as a fraction of the distance along the ray, before each field's own step limit (larger: faster, coarser)."));
static TAutoConsoleVariable<int32> CVarUniVolumePrefilter(TEXT("uu.VolumePrefilter"), 1, TEXT("D-160: march galaxies prefiltered over the pixel footprint (thin disc and arms averaged, no supersampling for them). 0: the D-130 supersampling."));
static TAutoConsoleVariable<int32> CVarUniVolumeLutStart(TEXT("uu.VolumeLutStart"), 1, TEXT("D-160: emission-only carriers start marching where their unresolved light becomes significant, not at 1 pc. 0: from 1 pc."));
// D-164: the cosmic web from a baked texture.
static TAutoConsoleVariable<int32> CVarUniWebSource(TEXT("uu.WebSource"), 1, TEXT("D-164: the cosmic web's density. 1: a 3D texture with mips (uu.WebTexture) (placement and the volume read the same texels; much cheaper to march). 0: the procedural lattices (D-012). Applies at uu.Enable; switching rerolls the web's galaxies."));
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
	Travel.Current = 0.0;
	bCameraJumped = true;
	UniCore::FUniverseDef Def = SceneName == TEXT("web") ? UniScenes::WebToGalaxy(ClusterTuning) : (SceneName == TEXT("flythrough") ? UniScenes::Flythrough(ClusterTuning) : UniScenes::GalaxyToStar());
	SetupWebTexture(Def);   // D-164: before the model, which bakes the definition's parameters
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
	UniCore::BindWebTexture(nullptr);   // D-164: after everything that samples it on the CPU
	WebTexture.Reset();
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
	EffectiveDeltaMag = FMath::Clamp(DeltaMag + float(Travel.Current), -5.0f, 5.0f);
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
	const FString Line = FString::Printf(TEXT("UU %s  speed %s/s (%.3g cm/s)%s  |  %s from the demo origin  |  dm %+.1f (travel %+.1f)  exposure %+.1f  |  t %.4g s (x%.3g)  systems %d, planets %d, promoted %d"),
		bFreeFly ? (MotionMode == EUniMotionMode::Local ? TEXT("FLY (local: real space, physics)") : TEXT("FLY (cruise)")) : TEXT("CRUISE"),
		UTF8_TO_TCHAR(UniCore::FormatDistance(Speed).c_str()), Speed,
		bFreeFly ? TEXT("  [WASD/QE move, wheel x2 / x0.5]") : TEXT(""),
		UTF8_TO_TCHAR(UniCore::FormatDistance(R).c_str()), EffectiveDeltaMag, Travel.Current, ExposureBias,
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
		const double Speed = bCameraJumped ? 0.0 : UniCore::Length(UniCore::ToDouble(UniCore::Sub(C, PrevCamera.ToCore()))) / Dt;
		Travel.MaxDeltaMag = FMath::Clamp(double(CVarUniTravelSensitivity.GetValueOnGameThread()), 0.0, 5.0);
		Travel.StartCmPerS = FMath::Max(double(CVarUniTravelStart.GetValueOnGameThread()), 1.0);
		Travel.FullCmPerS = FMath::Max(double(CVarUniTravelFull.GetValueOnGameThread()), Travel.StartCmPerS * 1.01);
		Travel.Update(Speed, Dt);
		EffectiveDeltaMag = FMath::Clamp(DeltaMag + float(Travel.Current), -5.0f, 5.0f);
		PrevCamera = Camera;
		bCameraJumped = false;
	}
	Streamer->DeltaMag = EffectiveDeltaMag;
	Streamer->SubmitDeferPerLevel = CVarUniSubmitDefer.GetValueOnGameThread();   // D-173
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
	// M3: bodies close enough become actors, and every parallax-placed actor follows the camera
	// that just moved (same frame, same camera: no lag against the backdrop at any speed).
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Promotion); FPerfTimer T(PerfMs[PerfPromotion]); UpdatePromotion(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Parallax); FPerfTimer T(PerfMs[PerfParallax]); UpdateParallax(ViewLoc); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(Uni_LightRig); FPerfTimer T(PerfMs[PerfLightRig]); UpdateLightRig(); }
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
		const char* Name = UniCore::StreamPhaseName(UniCore::EStreamPhase(I));
		S += FString::Printf(TEXT("%s%s %.1f"), S.IsEmpty() ? TEXT("") : TEXT(", "), UTF8_TO_TCHAR(Name + 11), P.Ms[I]);   // drop "Uni.Stream."
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
		L2 += FString::Printf(TEXT("  %s %.1f"), UTF8_TO_TCHAR(UniCore::StreamPhaseName(UniCore::EStreamPhase(I)) + 11), StreamAvg[I]);
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
	for (int32 I = 0; I < int32(UniCore::EStreamPhase::Count); ++I) { Avg += FString::Printf(TEXT("%s%s %.2f"), Avg.IsEmpty() ? TEXT("") : TEXT(", "), UTF8_TO_TCHAR(UniCore::StreamPhaseName(UniCore::EStreamPhase(I)) + 11), StreamAvg[I]); }
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  streamer avg: %s"), *Avg);
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  worst frame (%.1f ms) streamer: %s"), StreamWorstTickMs, *StreamBreakdown(StreamWorst));
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  last frame streamer: %s"), *StreamBreakdown(StreamLast));
	UE_LOG(LogUnifiedUniverse, Log, TEXT("  render: %u sprites, %u cells drawn, %u volume carriers, %u background faces marched so far; %llu contributors registered. GPU pass times: ProfileGPU (Ctrl+Shift+,)"),
		R.SpritesLastFrame.load(), R.CellsLastFrame.load(), R.VolumeCarriersLastFrame.load(), R.BackgroundFacesMarched.load(), uint64(Streamer->Contributors().size()));
	for (double& Pk : PerfPeak) { Pk = 0.0; }
	StreamWorstTickMs = 0.0;
	StreamWorst = UniCore::FStreamProfile();
	PerfHitches = 0;
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
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_Systems);
		FPerfTimer SysTimer(PerfMs[PerfSystems]);
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
	F.DeltaMag = EffectiveDeltaMag;   // D-154: owner sensitivity + travel boost
	F.ExposureBias = ExposureBias;
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
		V.ExposureMag = V.LimitingMag - 1.0f + ExposureBias;
		V.ThetaPixel = FUniRenderer::Get().Stats.ThetaPixelLastFrame.load();
		const UniCore::FCoord Cam = Camera.ToCore();
		UniCore::FVolumeSettings Settings;   // D-157: march quality from the console
		Settings.MaxSteps = UniCore::u32(FMath::Clamp(CVarUniVolumeMaxSteps.GetValueOnGameThread(), 16, 4096));
		Settings.StepRatio = FMath::Clamp(CVarUniVolumeStepRatio.GetValueOnGameThread(), 0.005f, 1.0f);
		Settings.MaxSubsamples = UniCore::u32(FMath::Clamp(CVarUniVolumeMaxSubsamples.GetValueOnGameThread(), 1, 8));
		Settings.bPrefilter = CVarUniVolumePrefilter.GetValueOnGameThread() != 0;
		Settings.bLutStart = CVarUniVolumeLutStart.GetValueOnGameThread() != 0;
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
	if (E) { Members = std::isfinite(E->NTotalRefMag) ? E->NTotal * std::pow(10.0, -0.4 * (BestM - E->NTotalRefMag)) : E->NTotal; }
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
}
