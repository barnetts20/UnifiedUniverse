#include "UniRenderer.h"

#include "UnifiedUniverseRender.h"
#include "UniShaders.h"
#include "UniTravel.h"

#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "CommonRenderResources.h"
#include "RHIStaticStates.h"
#include "HAL/IConsoleManager.h"
#include "Misc/EngineVersionComparison.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include <limits>

static TAutoConsoleVariable<float> CVarUniSpriteExtent(TEXT("uu.SpriteExtent"), 3.0f, TEXT("Sprite quad half size in Gaussian sigmas."), ECVF_RenderThreadSafe);
// D-156 sprite style: a pinpoint core plus a faint, capped halo (replaces uu.GlareGain).
static TAutoConsoleVariable<float> CVarUniSpriteCorePx(TEXT("uu.Sprite.CorePx"), UniHlsl::UNI_SPRITE_CORE_PX, TEXT("Core size of an unresolved sprite (Gaussian sigma, pixels). Brightness no longer enlarges it (D-156)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniSpriteSaturation(TEXT("uu.Sprite.Saturation"), UniHlsl::UNI_SPRITE_CORE_SAT, TEXT("Intensity (exposure units: 1 = a star at the exposure magnitude) where a sprite's core stops brightening. 0: physical (all light in the core, no halo; the engine's bloom does the glare)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniSpriteHaloGain(TEXT("uu.Sprite.HaloGain"), UniHlsl::UNI_SPRITE_HALO_GAIN, TEXT("Halo light per doubling of brightness over saturation, in units of the saturation level (0: no halo)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniSpriteHaloGrowth(TEXT("uu.Sprite.HaloGrowth"), UniHlsl::UNI_SPRITE_HALO_GROWTH_PX, TEXT("Halo size growth per doubling of brightness over saturation (pixels of Gaussian sigma)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniSpriteHaloChroma(TEXT("uu.Sprite.HaloChroma"), UniHlsl::UNI_SPRITE_HALO_CHROMA, TEXT("D-185: halo colour saturation x the star's (luminance kept; 1: the star's colour)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniSpriteFloor(TEXT("uu.Sprite.Floor"), UniHlsl::UNI_SPRITE_FLOOR, TEXT("D-188: scene colour (pre-exposed) below which a sprite's quad may end: it reaches until its core and halo fall below this (3 to 8 sigmas)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWarpStreakFrames(TEXT("uu.Warp.StreakFrames"), 1.0f, TEXT("D-187: warp streak length in frames of motion (the camera's move only), at full warp."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWarpStreakMax(TEXT("uu.Warp.StreakMax"), 64.0f, TEXT("D-187: longest warp streak, pixels."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWarpArrival(TEXT("uu.Warp.Arrival"), 8.0f, TEXT("D-187: at warp, frames over which a new cell's objects fade in, streaking in from ahead (0: off)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWarpArrivalStretch(TEXT("uu.Warp.ArrivalStretch"), 6.0f, TEXT("D-187: an arriving object's streak is 1 + this x (1 - arrival) times longer."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWarpAberration(TEXT("uu.Warp.Aberration"), 0.5f, TEXT("D-190: warp optics' effective speed (v/c) at full warp: the sky crowds towards the motion (0.5: the forward cone ~1.7x tighter). 0: none."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWarpDoppler(TEXT("uu.Warp.Doppler"), 1.0f, TEXT("D-190: Doppler colour strength at warp: blue ahead, red behind (luminance kept). 0: none."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWarpBeaming(TEXT("uu.Warp.Beaming"), 0.0f, TEXT("D-190: Doppler beaming exponent at warp (brighter ahead, dimmer behind). 0: light kept."), ECVF_RenderThreadSafe);

/** D-190: the warp optics for a frame: the camera's motion (unit) and the effective speed. */
static FVector4f WarpVelBetaOf(const FUniFrameData& F)
{
	const float Beta = FMath::Clamp(F.WarpAmount, 0.0f, 1.0f) * FMath::Clamp(CVarUniWarpAberration.GetValueOnRenderThread(), 0.0f, 0.9f);
	if (!F.bCameraStepValid) { return FVector4f(0.0f, 0.0f, 0.0f, 0.0f); }
	// D-199 (B-018): in double; FVector3f::Size() overflows past 6 pc a frame (UniCore::WarpMotion).
	float Vb[4];
	UniCore::WarpMotion(double(F.CameraStepCm.X), double(F.CameraStepCm.Y), double(F.CameraStepCm.Z), double(Beta), Vb);
	return FVector4f(Vb[0], Vb[1], Vb[2], Vb[3]);
}
static FVector4f WarpDopplerKQOf()
{
	return FVector4f(FMath::Clamp(CVarUniWarpDoppler.GetValueOnRenderThread(), 0.0f, 4.0f), FMath::Clamp(CVarUniWarpBeaming.GetValueOnRenderThread(), -4.0f, 4.0f), 0.0f, 0.0f);
}

static TAutoConsoleVariable<float> CVarUniSpriteHaloMax(TEXT("uu.Sprite.HaloMax"), UniHlsl::UNI_SPRITE_HALO_MAX_PX, TEXT("Largest halo of an unresolved sprite (Gaussian sigma, pixels)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniVolumeTemporalClamp(TEXT("uu.VolumeTemporal.Clamp"), 1, TEXT("D-194: reprojected history is clamped into its fresh neighbours' range, loose at rest and tight once it moves (less ghosting at speed). 0: the D-184 luminance band."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniVolumeTemporalTightPx(TEXT("uu.VolumeTemporal.TightPx"), 2.0f, TEXT("D-194: the motion (march pixels per frame) at which the history clamp is tight (no slack outside the fresh neighbours' range)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniVolumeTemporalMaxPx(TEXT("uu.VolumeTemporal.MaxMotionPx"), 8.0f, TEXT("D-194: beyond this motion (march pixels per frame) history is not used: the fresh neighbours are (blur instead of trails)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniVolumeInterleave(TEXT("uu.VolumeInterleave"), 4, TEXT("D-208: most pixels per side of the temporal slicing's block: 4 marches 1 pixel in 16 a frame while the image is near still (under 0.1 march pixels a frame), 3 one in 9 under 0.5, otherwise 2 x 2 (one in 4, D-184). 2: always 2 x 2."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniVolumeTemporal(TEXT("uu.VolumeTemporal"), 1, TEXT("D-184: temporal slicing of the volume march: each frame marches one pixel of every 2 x 2 block and reprojects the rest from the previous frame (about a quarter of the march's cost). 0: march every pixel every frame. Off in the debug views."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniVolumeTravelPixelScale(TEXT("uu.VolumeTravelPixelScale"), 0.35f, TEXT("D-198: the volume march's pixel budget (uu.VolumeMaxMarchPixels) at full travel speed (10 Mpc/s), as a fraction; it falls smoothly from 1 at 100 kpc/s. 1: no change with speed."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniVolumeMaxMarchPixels(TEXT("uu.VolumeMaxMarchPixels"), 262144, TEXT("D-196: the volume march is at most this many pixels (before temporal slicing): past it the march resolution drops below output / uu.VolumeDivisor, aspect kept. 0: no limit (half resolution on any viewport)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniVolumeDivisor(TEXT("uu.VolumeDivisor"), 2, TEXT("Volume march resolution divisor (1 = full, 2 = half, 4 = quarter)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniBackgroundSize(TEXT("uu.BackgroundSize"), 256, TEXT("Background direction-map face size (texels)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniVolumeDebug(TEXT("uu.VolumeDebug"), 0, TEXT("Volume debug view (lesson #37): 0 off, 1 steps per ray (magenta = truncated), 2 coverage (R carriers, G supersampling, B truncated), 3 emission (log10 heat), 4 dust transmittance."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniVolumeDebugLogLo(TEXT("uu.VolumeDebugLogLo"), 0.0f, TEXT("uu.VolumeDebug 3: log10 radiance mapped to the bottom of the heat ramp (the ramp spans 6 decades)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniWebDebug(TEXT("uu.WebDebug"), 0, TEXT("D-169: the textured cosmic web in false colour (replaces the volume; sprites still drawn). 0 off, 1 channels (walls blue, filaments green, nodes red, void grey), 2 walls, 3 filaments, 4 nodes, 5 void (each over its own mean, heat ramp), 6 placement (the galaxy web's field as placement reads it)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWebDebugDepth(TEXT("uu.WebDebugDepth"), 150.0f, TEXT("D-169: uu.WebDebug marches this far (Mpc). The tile is 200 Mpc; past it the web repeats."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWebDebugThreshold(TEXT("uu.WebDebugThreshold"), 2.0f, TEXT("D-169: uu.WebDebug shows density above this multiple of the mean (full strength 100x above it)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniWebDebugOpacity(TEXT("uu.WebDebugOpacity"), 0.3f, TEXT("D-169: uu.WebDebug opacity per texel at full strength (higher: only the nearest structure shows)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniBackgroundMode(TEXT("uu.BackgroundMode"), 2, TEXT("D-166: how the background (the periodic web) is marched. 0: into the direction map (D-136/D-159), a few faces per frame. 1: per pixel with the local volume, every frame. 2: per pixel while the background is changing (travel), the map once it has been still for uu.BackgroundStillFrames."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniBackgroundPerPixelInterval(TEXT("uu.BackgroundPerPixelInterval"), 0, TEXT("D-196: uu.BackgroundMode 2 marches the background per pixel only while it changes at least this often (frames between changes): fast travel. Slower changes refresh the direction map (cross-faded). 0: automatic, where a map refresh per change costs more than marching it per pixel."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniBackgroundStillFrames(TEXT("uu.BackgroundStillFrames"), 30, TEXT("D-166: uu.BackgroundMode 2 switches to the direction map after this many frames without a background change."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniBackgroundFaces(TEXT("uu.BackgroundFacesPerFrame"), 2, TEXT("Background map faces marched per frame during a refresh (1..6; D-159: a refresh is always all six faces from one snapshot, cross-faded in)."), ECVF_RenderThreadSafe);

namespace
{
	FIntVector WrappedGroups(uint32 Count, uint32& OutGroupsX)
	{
		// Lesson #23: at most 65,535 groups per dimension; wrap into 2D.
#if UE_VERSION_OLDER_THAN(5, 3, 0)
		const int32 MaxX = FMath::Max(int32(GRHIMaxDispatchThreadGroupsPerDimension.X), 1);
#else
		const int32 MaxX = FMath::Max(GRHIGlobals.MaxDispatchThreadGroupsPerDimension.X, 1);
#endif
		const int32 X = FMath::Clamp(int32(Count), 1, MaxX);
		const int32 Y = FMath::DivideAndRoundUp(FMath::Max(int32(Count), 1), X);
		OutGroupsX = uint32(X);
		return FIntVector(X, Y, 1);
	}

	template <typename T>
	FRDGBufferRef UploadStructured(FRDGBuilder& GraphBuilder, const TCHAR* Name, const TArray<T>& Data)
	{
		// Never zero-sized: a one-element dummy keeps the SRV valid for an empty batch.
		static const T Zero{};
		const T* Ptr = Data.Num() ? Data.GetData() : &Zero;
		const uint32 Num = FMath::Max(Data.Num(), 1);
		return CreateStructuredBuffer(GraphBuilder, Name, sizeof(T), Num, Ptr, uint64(Num) * sizeof(T), ERDGInitialDataFlags::None);
	}
}

// =============================================================================================
// FUniRenderer
// =============================================================================================

struct FUniRenderer::FPromoteReadback
{
	TUniquePtr<FRHIGPUBufferReadback> Count;
	TUniquePtr<FRHIGPUBufferReadback> Args;   // D-157: the sprite draw args (instance count = sprites drawn)
	bool bPending = false;
};

struct FUniRenderer::FActivateReadback
{
	TUniquePtr<FRHIGPUBufferReadback> Count;
	TUniquePtr<FRHIGPUBufferReadback> Index;
	TUniquePtr<FRHIGPUBufferReadback> Records;
	uint64 FrameNumber = 0;
	bool bPending = false;
};

// D-193: a pick's result words (UniQuery.usf OutPickResult), and a resident query's count,
// hits and records, with the request they answer.
struct FUniRenderer::FPickReadback
{
	TUniquePtr<FRHIGPUBufferReadback> Result;
	uint64 FrameNumber = 0;
	int32 RequestId = 0;
	bool bPending = false;
};

struct FUniRenderer::FQueryReadback
{
	TUniquePtr<FRHIGPUBufferReadback> Count;
	TUniquePtr<FRHIGPUBufferReadback> Hits;
	TUniquePtr<FRHIGPUBufferReadback> Records;
	uint64 FrameNumber = 0;
	int32 RequestId = 0;
	uint32 MaxResults = 0;
	bool bPending = false;
};

void FUniRenderer::DrainPicks_GameThread(TArray<FUniPickReadback>& Out)
{
	check(IsInGameThread());
	FScopeLock Lock(&ApiLock);
	Out.Append(PicksReady);
	PicksReady.Reset();
}

void FUniRenderer::DrainQueries_GameThread(TArray<FUniQueryReadback>& Out)
{
	check(IsInGameThread());
	FScopeLock Lock(&ApiLock);
	Out.Append(MoveTemp(QueriesReady));
	QueriesReady.Reset();
}

void FUniRenderer::CollectApiReadbacks_RenderThread()
{
	// Finished readbacks only; never waited on (lesson #24).
	for (const TSharedPtr<FPickReadback>& R : PickRing)
	{
		if (!R->bPending || !R->Result->IsReady()) { continue; }
		const uint32* W = static_cast<const uint32*>(R->Result->Lock(UniGpuLayout::PickResultWords * sizeof(uint32)));
		FUniPickReadback P;
		P.FrameNumber = R->FrameNumber;
		P.RequestId = R->RequestId;
		if (W)
		{
			P.bFound = W[0] != 0u;
			P.CellIndex = W[2];
			P.Slot = W[3];
			P.Score = FMath::Max(0.0f, *reinterpret_cast<const float*>(&W[4]));
			for (int32 I = 0; I < 4; ++I) { P.Record.A[I] = W[8 + I]; P.Record.B[I] = W[12 + I]; }
		}
		R->Result->Unlock();
		R->bPending = false;
		FScopeLock Lock(&ApiLock);
		PicksReady.Add(P);
		if (PicksReady.Num() > 64) { PicksReady.RemoveAt(0, PicksReady.Num() - 64); }
	}
	for (const TSharedPtr<FQueryReadback>& R : QueryRing)
	{
		if (!R->bPending || !R->Count->IsReady() || !R->Hits->IsReady() || !R->Records->IsReady()) { continue; }
		const uint32* CountPtr = static_cast<const uint32*>(R->Count->Lock(sizeof(uint32)));
		const uint32 Total = CountPtr ? *CountPtr : 0u;
		R->Count->Unlock();
		FUniQueryReadback Q;
		Q.FrameNumber = R->FrameNumber;
		Q.RequestId = R->RequestId;
		Q.Total = Total;
		const uint32 N = FMath::Min(Total, R->MaxResults);
		if (N > 0)
		{
			const FUintVector4* H = static_cast<const FUintVector4*>(R->Hits->Lock(N * sizeof(FUintVector4)));
			TArray<FUintVector4> HitCopy;
			HitCopy.Append(H, int32(N));
			R->Hits->Unlock();
			const UniCore::FObjectGPU* Rec = static_cast<const UniCore::FObjectGPU*>(R->Records->Lock(N * UniGpuLayout::ObjectBytes));
			Q.Hits.Reserve(int32(N));
			for (uint32 I = 0; I < N; ++I)
			{
				UniCore::FQueryHit Hit;
				Hit.CellIndex = HitCopy[int32(I)].X;
				Hit.Slot = HitCopy[int32(I)].Y;
				const uint32 Bits = HitCopy[int32(I)].Z;
				Hit.DistanceCm = *reinterpret_cast<const float*>(&Bits);
				Hit.Record = Rec[I];
				Q.Hits.Add(Hit);
			}
			R->Records->Unlock();
		}
		R->bPending = false;
		FScopeLock Lock(&ApiLock);
		QueriesReady.Add(MoveTemp(Q));
		if (QueriesReady.Num() > 16) { QueriesReady.RemoveAt(0, QueriesReady.Num() - 16); }
	}
}

void FUniRenderer::AddQuery_RenderThread(FRDGBuilder& GraphBuilder, const FUniFrameData& F, ERHIFeatureLevel::Type FeatureLevel, FRDGBufferRef CellBuf, FRDGBufferRef PoolBuf)
{
	if (QueryRing.Num() == 0) { for (int32 I = 0; I < 3; ++I) { QueryRing.Add(MakeShared<FQueryReadback>()); } }
	TSharedPtr<FQueryReadback> Slot;
	for (const TSharedPtr<FQueryReadback>& R : QueryRing) { if (!R->bPending) { Slot = R; break; } }
	if (!Slot.IsValid()) { return; }   // all in flight: the request is dropped (the subsystem times it out and resends)
	const uint32 MaxResults = FMath::Clamp(F.QueryMaxResults, 1u, UniGpuLayout::MaxQueryResults);
	FRDGBufferDesc MinDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
	FRDGBufferRef MinBuf = GraphBuilder.CreateBuffer(MinDesc, TEXT("UniQueryMin"));
	FRDGBufferRef MinDummy = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("UniQueryMinDummy"));
	FRDGBufferDesc HitDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), MaxResults);
	HitDesc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef HitBuf = GraphBuilder.CreateBuffer(HitDesc, TEXT("UniQueryHits"));
	FRDGBufferDesc RecDesc = FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::ObjectBytes, MaxResults);
	RecDesc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef RecBuf = GraphBuilder.CreateBuffer(RecDesc, TEXT("UniQueryRecords"));
	FRDGBufferDesc CountDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
	CountDesc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef CountBuf = GraphBuilder.CreateBuffer(CountDesc, TEXT("UniQueryCount"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(MinBuf), 0xFFFFFFFFu);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(MinDummy), 0xFFFFFFFFu);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HitBuf), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RecBuf), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CountBuf), 0u);
	uint32 GroupsX = 1;
	const FIntVector Groups = WrappedGroups(uint32(F.Cells.Num()), GroupsX);
	TShaderMapRef<FUniQueryCS> CS(GetGlobalShaderMap(FeatureLevel));
	const bool bNearest = F.QueryMode == UniHlsl::UNI_QUERY_MODE_NEAREST;
	for (int32 Pass = 0; Pass < (bNearest ? 2 : 1); ++Pass)
	{
		FUniQueryCS::FParameters* P = GraphBuilder.AllocParameters<FUniQueryCS::FParameters>();
		P->InCells = GraphBuilder.CreateSRV(CellBuf);
		P->InPool = GraphBuilder.CreateSRV(PoolBuf);
		// Mode 2 reads the minimum the mode-1 pass wrote; a buffer is never bound both ways in one pass.
		P->OutQueryMin = GraphBuilder.CreateUAV(Pass == 1 ? MinDummy : MinBuf);
		P->InQueryMin = GraphBuilder.CreateSRV(Pass == 1 ? MinBuf : MinDummy);
		P->OutQueryHits = GraphBuilder.CreateUAV(HitBuf);
		P->OutQueryRecords = GraphBuilder.CreateUAV(RecBuf);
		P->OutQueryCount = GraphBuilder.CreateUAV(CountBuf);
		P->NumCells = uint32(F.Cells.Num());
		P->DispatchGroupsX = GroupsX;
		P->QueryMode = bNearest ? (Pass == 0 ? 1u : 2u) : 0u;
		P->QueryDeltaX = F.QueryDelta[0];
		P->QueryDeltaY = F.QueryDelta[1];
		P->QueryDeltaZ = F.QueryDelta[2];
		P->QueryRadius = F.QueryRadiusCm;
		P->QueryArchMask = F.QueryArchMask;
		P->QueryMaxAbsMag = F.QueryMaxAbsMag;
		P->QueryMaxResults = MaxResults;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Query %s", bNearest ? (Pass == 0 ? TEXT("nearest (min)") : TEXT("nearest (select)")) : TEXT("sphere")), CS, P, Groups);
	}
	if (!Slot->Count.IsValid())
	{
		Slot->Count = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniQueryCountReadback"));
		Slot->Hits = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniQueryHitsReadback"));
		Slot->Records = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniQueryRecordsReadback"));
	}
	AddEnqueueCopyPass(GraphBuilder, Slot->Count.Get(), CountBuf, sizeof(uint32));
	AddEnqueueCopyPass(GraphBuilder, Slot->Hits.Get(), HitBuf, MaxResults * sizeof(FUintVector4));
	AddEnqueueCopyPass(GraphBuilder, Slot->Records.Get(), RecBuf, MaxResults * UniGpuLayout::ObjectBytes);
	Slot->FrameNumber = F.FrameNumber;
	Slot->RequestId = F.QueryRequestId;
	Slot->MaxResults = MaxResults;
	Slot->bPending = true;
}

uint32 FUniRenderer::DynamicPoolBase() { return UniGpuLayout::DynamicPoolBase; }
uint32 FUniRenderer::DynamicPoolSlots() { return UniGpuLayout::DynamicPoolSlots; }
uint32 FUniRenderer::MaxQueryResults() { return UniGpuLayout::MaxQueryResults; }

void FUniRenderer::DrainActivations_GameThread(TArray<FUniActivation>& Out)
{
	check(IsInGameThread());
	FScopeLock Lock(&ActivationLock);
	Out.Append(ActivationsReady);
	ActivationsReady.Reset();
}

FUniRenderer& FUniRenderer::Get()
{
	static FUniRenderer Instance;
	return Instance;
}

bool FUniRenderer::ShadersEnabled()
{
	return UniShadersEnabled();
}

bool FUniRenderer::ReadbackPoolBlocking_GameThread(TArray<UniCore::FObjectGPU>& Out, uint32 Num, double TimeoutSeconds)
{
	check(IsInGameThread());
	Out.Reset();
	if (!UniShadersEnabled() || Num == 0) { return Num == 0; }
	Num = FMath::Min(Num, UniGpuLayout::PoolCapacity);
	// Allocated on the calling thread (lesson #24); shared so the render commands keep it alive.
	TSharedRef<FRHIGPUBufferReadback, ESPMode::ThreadSafe> Rb = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("UniPoolParityReadback"));
	ENQUEUE_RENDER_COMMAND(UniPoolReadback)([Rb, Num](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef PoolBuf = FUniRenderer::Get().RegisterPool_RenderThread(GraphBuilder);
		AddEnqueueCopyPass(GraphBuilder, &Rb.Get(), PoolBuf, Num * UniGpuLayout::ObjectBytes);
		GraphBuilder.Execute();
	});
	const double Start = FPlatformTime::Seconds();
	std::atomic<bool> bReady{ false };
	while (!bReady.load() && FPlatformTime::Seconds() - Start < TimeoutSeconds)
	{
		ENQUEUE_RENDER_COMMAND(UniPoolReadbackPoll)([Rb, &bReady](FRHICommandListImmediate&) { bReady = Rb->IsReady(); });
		FlushRenderingCommands();
		if (!bReady.load()) { FPlatformProcess::Sleep(0.01f); }
	}
	if (!bReady.load()) { return false; }
	Out.SetNumUninitialized(int32(Num));
	UniCore::FObjectGPU* Dst = Out.GetData();
	ENQUEUE_RENDER_COMMAND(UniPoolReadbackCopy)([Rb, Dst, Num](FRHICommandListImmediate&)
	{
		const void* Src = Rb->Lock(Num * UniGpuLayout::ObjectBytes);   // render thread only (lesson #24)
		FMemory::Memcpy(Dst, Src, Num * UniGpuLayout::ObjectBytes);
		Rb->Unlock();
	});
	FlushRenderingCommands();
	return true;
}

void FUniRenderer::SetFrame_GameThread(FUniFrameData&& Data)
{
	check(IsInGameThread());
	// One payload, moved once into a shared holder, captured by value (lesson #25).
	TSharedRef<FUniFrameData, ESPMode::ThreadSafe> Payload = MakeShared<FUniFrameData, ESPMode::ThreadSafe>(MoveTemp(Data));
	ENQUEUE_RENDER_COMMAND(UniSetFrame)([Payload](FRHICommandListImmediate&)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_SetFrame_RT);
		FUniRenderer::Get().QueueGalaxyCache_RenderThread(*Payload);   // D-211: every frame's boxes, even a frame not rendered
		FUniRenderer::Get().QueueGalaxyMap_RenderThread(*Payload);     // D-212
		FUniRenderer::Get().RenderFrame = MoveTemp(*Payload);   // the payload is not used again: move, not copy
	});
}

void FUniRenderer::QueueGalaxyCache_RenderThread(const FUniFrameData& F)
{
	check(IsInRenderingThread());
	if (F.GalCacheN == 0u)
	{
		GalCachePending.Reset();
		GalCachePendingVoxels = 0;
		bGalCacheReady = false;
		GalCacheEpoch = ~0u;
		return;
	}
	if (F.GalCacheEpoch != GalCacheEpoch)
	{
		// A reset bakes every level from scratch: what the textures held no longer counts.
		GalCachePending.Reset();
		GalCachePendingVoxels = 0;
		GalCacheEpoch = F.GalCacheEpoch;
		bGalCacheReady = true;
	}
	for (int32 I = 0; I + 1 < F.GalCacheBoxes.Num(); I += 2)
	{
		const FUintVector4& Sz = F.GalCacheBoxes[I + 1];
		GalCachePending.Add(F.GalCacheBoxes[I]);
		GalCachePending.Add(Sz);
		GalCachePendingVoxels += uint64(Sz.X) * Sz.Y * Sz.Z;
	}
	// More than two full bakes waiting: the volume pass is not running (hidden viewport, uu.Volume 0).
	// Drop the queue; the game thread resets the cache and the march leaves it until then.
	const uint64 Full = uint64(F.GalCacheN) * F.GalCacheN * F.GalCacheN * FMath::Max(F.GalCacheLevels, 1u);
	if (GalCachePendingVoxels > 2u * Full)
	{
		GalCachePending.Reset();
		GalCachePendingVoxels = 0;
		bGalCacheReady = false;
		Stats.bGalCacheLost = true;
	}
}

void FUniRenderer::SetTables_GameThread(TArray<UniCore::FSliceGPU> Slices, TArray<float> Lut)
{
	check(IsInGameThread());
	struct FTables { TArray<UniCore::FSliceGPU> Slices; TArray<float> Lut; };
	TSharedRef<FTables, ESPMode::ThreadSafe> Payload = MakeShared<FTables, ESPMode::ThreadSafe>();
	Payload->Slices = MoveTemp(Slices);
	Payload->Lut = MoveTemp(Lut);
	ENQUEUE_RENDER_COMMAND(UniSetTables)([Payload](FRHICommandListImmediate&)
	{
		FUniRenderer& R = FUniRenderer::Get();
		R.SlicesRT = Payload->Slices;
		R.LutRT = Payload->Lut;
		R.bTablesDirty = true;
	});
}

void FUniRenderer::SetWebTexture_GameThread(uint32 Size, TArray<TArray<FUintVector2>> Mips)
{
	check(IsInGameThread());
	struct FWeb { uint32 Size = 0; TArray<TArray<FUintVector2>> Mips; };
	TSharedRef<FWeb, ESPMode::ThreadSafe> Payload = MakeShared<FWeb, ESPMode::ThreadSafe>();
	Payload->Size = Size;
	Payload->Mips = MoveTemp(Mips);
	ENQUEUE_RENDER_COMMAND(UniSetWebTexture)([Payload](FRHICommandListImmediate&)
	{
		FUniRenderer& R = FUniRenderer::Get();
		R.WebSizeRT = Payload->Size;
		R.WebMipsRT = MoveTemp(Payload->Mips);
		R.bWebDirty = true;
	});
}

FRDGTextureRef FUniRenderer::RegisterWebTexture_RenderThread(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	// One mip from a buffer of packed half texels (UniWebTexUpload.usf).
	auto Upload = [&GraphBuilder](FRDGTextureRef Tex, int32 Mip, uint32 MipSize, const TArray<FUintVector2>& Texels)
	{
		FUniWebTexUploadCS::FParameters* P = GraphBuilder.AllocParameters<FUniWebTexUploadCS::FParameters>();
		P->InTexels = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniWebTexels"), Texels));
		P->OutMip = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Tex, uint8(Mip)));
		P->MipSize = MipSize;
		const int32 G = FMath::DivideAndRoundUp(int32(MipSize), 4);
		TShaderMapRef<FUniWebTexUploadCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.WebTexture.Upload mip %d (%u^3)", Mip, MipSize), CS, P, FIntVector(G, G, G));
	};
	if (bWebDirty)
	{
		bWebDirty = false;
		WebTexture.SafeRelease();
		if (WebSizeRT > 0u && WebMipsRT.Num() > 0)
		{
			const FRDGTextureDesc Desc = FRDGTextureDesc::Create3D(FIntVector(int32(WebSizeRT)), PF_FloatRGBA, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV, uint8(WebMipsRT.Num()));
			FRDGTextureRef Tex = GraphBuilder.CreateTexture(Desc, TEXT("UniWebTexture"));
			for (int32 M = 0; M < WebMipsRT.Num(); ++M)
			{
				const uint32 S = FMath::Max(WebSizeRT >> M, 1u);
				if (uint64(WebMipsRT[M].Num()) != uint64(S) * S * S) { UE_LOG(LogTemp, Error, TEXT("UnifiedUniverse: web texture mip %d has %d texels, expected %u^3"), M, WebMipsRT[M].Num(), S); continue; }
				Upload(Tex, M, S, WebMipsRT[M]);   // UploadStructured copies the texels into the graph
			}
			WebMipsRT.Empty();
			WebTexture = GraphBuilder.ConvertToExternalTexture(Tex);
			return Tex;
		}
		WebMipsRT.Empty();
	}
	if (WebTexture.IsValid()) { return GraphBuilder.RegisterExternalTexture(WebTexture, TEXT("UniWebTexture")); }
	return RegisterNanDummy_RenderThread(GraphBuilder);
}

/** One mip of a field texture from a buffer of packed half texels (UniWebTexUpload.usf). */
static void UniUploadFieldMip(FRDGBuilder& GraphBuilder, FRDGTextureRef Tex, int32 Mip, uint32 MipSize, const TArray<FUintVector2>& Texels)
{
	FUniWebTexUploadCS::FParameters* P = GraphBuilder.AllocParameters<FUniWebTexUploadCS::FParameters>();
	P->InTexels = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniFieldTexels"), Texels));
	P->OutMip = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Tex, uint8(Mip)));
	P->MipSize = MipSize;
	const int32 G = FMath::DivideAndRoundUp(int32(MipSize), 4);
	TShaderMapRef<FUniWebTexUploadCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.FieldTexture.Upload mip %d (%u^3)", Mip, MipSize), CS, P, FIntVector(G, G, G));
}

FRDGTextureRef FUniRenderer::RegisterNanDummy_RenderThread(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	if (!WebDummy.IsValid())
	{
		// Fail closed: a field texture sampled without its texture reads NaN (nothing passes a
		// density test), never a plausible value (lesson #1).
		FRDGTextureRef D = GraphBuilder.CreateTexture(FRDGTextureDesc::Create3D(FIntVector(1, 1, 1), PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV, 1), TEXT("UniWebTextureDummy"));
		TArray<FUintVector2> Nan;
		Nan.Add(FUintVector2(0x7E007E00u, 0x7E007E00u));   // four half quiet NaNs
		UniUploadFieldMip(GraphBuilder, D, 0, 1u, Nan);
		WebDummy = GraphBuilder.ConvertToExternalTexture(D);
		return D;
	}
	return GraphBuilder.RegisterExternalTexture(WebDummy, TEXT("UniWebTextureDummy"));
}

void FUniRenderer::SetGalaxyTexture_GameThread(int32 Slot, uint32 Size, TArray<TArray<FUintVector2>> Mips)
{
	check(IsInGameThread());
	if (Slot < 0 || Slot >= GalaxyTextureSlots) { return; }
	struct FGal { int32 Slot = 0; uint32 Size = 0; TArray<TArray<FUintVector2>> Mips; };
	TSharedRef<FGal, ESPMode::ThreadSafe> Payload = MakeShared<FGal, ESPMode::ThreadSafe>();
	Payload->Slot = Slot;
	Payload->Size = Size;
	Payload->Mips = MoveTemp(Mips);
	ENQUEUE_RENDER_COMMAND(UniSetGalaxyTexture)([Payload](FRHICommandListImmediate&)
	{
		FUniRenderer& R = FUniRenderer::Get();
		R.GalSizeRT[Payload->Slot] = Payload->Size;
		R.GalMipsRT[Payload->Slot] = MoveTemp(Payload->Mips);
		R.bGalDirty[Payload->Slot] = true;
	});
}

void FUniRenderer::QueueGalaxyMap_RenderThread(const FUniFrameData& F)
{
	check(IsInRenderingThread());
	if (F.GalMapEpoch == 0u)
	{
		if (GalMapEpochRT != 0u) { GalMapTexture.SafeRelease(); GalMapEpochRT = 0u; bGalMapComplete = false; }
		return;
	}
	if (F.GalMapEpoch != GalMapEpochRT)
	{
		// A new galaxy (or size): bake it from the first row; the old map stays out of use.
		GalMapEpochRT = F.GalMapEpoch;
		GalMapSizeRT = F.GalMapSize;
		bGalMapDustRT = F.bGalMapDust != 0u;
		GalMapParamsRT = { F.GalMapParams[0], F.GalMapParams[1] };
		GalMapRowsDone = 0u;
		bGalMapComplete = false;
		GalMapTexture.SafeRelease();
		Stats.GalMapBakeFrames = 0u;
	}
	GalMapBudgetRT = F.GalMapBudget;
}

FRDGTextureRef FUniRenderer::RegisterGalaxyMap_RenderThread(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	if (GalMapEpochRT != 0u && !bGalMapComplete && GalMapSizeRT > 0u && GalMapParamsRT.Num() == 2)
	{
		// D-212: bake a chunk of rows a frame (the GPU's share of the texel budget), then the mips.
		const uint32 Size = GalMapSizeRT;
		const uint32 NumMips = uint32(FMath::FloorLog2(Size)) + 1u;
		FRDGTextureRef Tex;
		if (!GalMapTexture.IsValid())
		{
			Tex = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(int32(Size), int32(Size)), PF_FloatRGBA, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV, uint8(NumMips)), TEXT("UniGalMap"));
			GalMapTexture = GraphBuilder.ConvertToExternalTexture(Tex);
		}
		else { Tex = GraphBuilder.RegisterExternalTexture(GalMapTexture, TEXT("UniGalMap")); }
		const uint32 Rows = FMath::Clamp(FMath::Max(GalMapBudgetRT, Size) / Size, 1u, Size - GalMapRowsDone);
		{
			FUniGalMapBakeCS::FParameters* P = GraphBuilder.AllocParameters<FUniGalMapBakeCS::FParameters>();
			P->InMapParams = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniGalMapParams"), GalMapParamsRT));
			P->MapSize = Size;
			P->MapRow0 = GalMapRowsDone;
			P->MapRows = Rows;
			P->bMapDust = bGalMapDustRT ? 1u : 0u;
			P->OutMap = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Tex, 0));
			TShaderMapRef<FUniGalMapBakeCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.GalaxyMap.Bake rows %u-%u of %u", GalMapRowsDone, GalMapRowsDone + Rows, Size), CS, P,
				FIntVector(FMath::DivideAndRoundUp(int32(Size), 8), FMath::DivideAndRoundUp(int32(Rows), 8), 1));
		}
		GalMapRowsDone += Rows;
		Stats.GalMapBakeFrames.fetch_add(1u);
		if (GalMapRowsDone >= Size)
		{
			for (uint32 M = 1u; M < NumMips; ++M)
			{
				FUniGalMapDownCS::FParameters* P = GraphBuilder.AllocParameters<FUniGalMapDownCS::FParameters>();
				const uint32 S = FMath::Max(Size >> M, 1u);
				P->MapSize = S;
				P->InMapMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(Tex, int32(M - 1u)));
				P->OutMap = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Tex, uint8(M)));
				TShaderMapRef<FUniGalMapDownCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.GalaxyMap.Mip %u (%u^2)", M, S), CS, P, FIntVector(FMath::DivideAndRoundUp(int32(S), 8), FMath::DivideAndRoundUp(int32(S), 8), 1));
			}
			bGalMapComplete = true;
			Stats.GalMapReadyEpoch = GalMapEpochRT;
		}
		return Tex;
	}
	if (GalMapTexture.IsValid() && bGalMapComplete) { return GraphBuilder.RegisterExternalTexture(GalMapTexture, TEXT("UniGalMap")); }
	if (!GalMapDummy.IsValid())
	{
		FRDGTextureRef D = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("UniGalMapDummy"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(D), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
		GalMapDummy = GraphBuilder.ConvertToExternalTexture(D);
		return D;
	}
	return GraphBuilder.RegisterExternalTexture(GalMapDummy, TEXT("UniGalMapDummy"));
}

FRDGTextureRef FUniRenderer::RegisterGalaxyTexture_RenderThread(FRDGBuilder& GraphBuilder, int32 Slot)
{
	check(IsInRenderingThread());
	if (Slot < 0 || Slot >= GalaxyTextureSlots) { return RegisterNanDummy_RenderThread(GraphBuilder); }
	static const TCHAR* Names[GalaxyTextureSlots] = { TEXT("UniGalWarpFine"), TEXT("UniGalWarpLarge"), TEXT("UniGalDetail") };
	if (bGalDirty[Slot])
	{
		bGalDirty[Slot] = false;
		GalTexture[Slot].SafeRelease();
		const uint32 Size = GalSizeRT[Slot];
		TArray<TArray<FUintVector2>>& Mips = GalMipsRT[Slot];
		if (Size > 0u && Mips.Num() > 0)
		{
			const FRDGTextureDesc Desc = FRDGTextureDesc::Create3D(FIntVector(int32(Size)), PF_FloatRGBA, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV, uint8(Mips.Num()));
			FRDGTextureRef Tex = GraphBuilder.CreateTexture(Desc, Names[Slot]);
			for (int32 M = 0; M < Mips.Num(); ++M)
			{
				const uint32 S = FMath::Max(Size >> M, 1u);
				if (uint64(Mips[M].Num()) != uint64(S) * S * S) { UE_LOG(LogTemp, Error, TEXT("UnifiedUniverse: galaxy texture %d mip %d has %d texels, expected %u^3"), Slot, M, Mips[M].Num(), S); continue; }
				UniUploadFieldMip(GraphBuilder, Tex, M, S, Mips[M]);   // UploadStructured copies the texels into the graph
			}
			Mips.Empty();
			GalTexture[Slot] = GraphBuilder.ConvertToExternalTexture(Tex);
			return Tex;
		}
		Mips.Empty();
	}
	if (GalTexture[Slot].IsValid()) { return GraphBuilder.RegisterExternalTexture(GalTexture[Slot], Names[Slot]); }
	return RegisterNanDummy_RenderThread(GraphBuilder);
}

FRDGBufferRef FUniRenderer::RegisterPool_RenderThread(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	if (!Pool.IsValid())
	{
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::ObjectBytes, UniGpuLayout::PoolCapacity);
		Desc.Usage |= EBufferUsageFlags::SourceCopy;   // readback of emitter objects (see AddEnqueueCopyPass notes, ULS)
		FRDGBufferRef Created = GraphBuilder.CreateBuffer(Desc, TEXT("UniObjectPool"));
		// Zeroed for tidiness only. A zero record is NOT hidden; correctness comes from the cell
		// table, which lists a range only once its place pass has written or blanked every slot.
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Created), 0u);
		Pool = GraphBuilder.ConvertToExternalBuffer(Created);
		return Created;
	}
	return GraphBuilder.RegisterExternalBuffer(Pool, TEXT("UniObjectPool"));
}

FRDGBufferRef FUniRenderer::RegisterSlices_RenderThread(FRDGBuilder& GraphBuilder)
{
	if (bTablesDirty || !SlicesBuffer.IsValid())
	{
		FRDGBufferRef S = UploadStructured(GraphBuilder, TEXT("UniSlices"), SlicesRT);
		FRDGBufferRef L = UploadStructured(GraphBuilder, TEXT("UniLut"), LutRT);
		SlicesBuffer = GraphBuilder.ConvertToExternalBuffer(S);
		LutBuffer = GraphBuilder.ConvertToExternalBuffer(L);
		bTablesDirty = false;
		return S;
	}
	return GraphBuilder.RegisterExternalBuffer(SlicesBuffer, TEXT("UniSlices"));
}

FRDGBufferRef FUniRenderer::RegisterLut_RenderThread(FRDGBuilder& GraphBuilder)
{
	if (!LutBuffer.IsValid()) { RegisterSlices_RenderThread(GraphBuilder); }
	return GraphBuilder.RegisterExternalBuffer(LutBuffer, TEXT("UniLut"));
}

void FUniRenderer::Release_RenderThread()
{
	check(IsInRenderingThread());
	Pool.SafeRelease();
	SlicesBuffer.SafeRelease();
	LutBuffer.SafeRelease();
	WebTexture.SafeRelease();
	WebDummy.SafeRelease();
	WebMipsRT.Empty();
	for (int32 S = 0; S < GalaxyTextureSlots; ++S) { GalTexture[S].SafeRelease(); GalMipsRT[S].Empty(); }   // D-179
	GalMapTexture.SafeRelease(); GalMapDummy.SafeRelease(); GalMapEpochRT = 0u; bGalMapComplete = false; GalMapRowsDone = 0u;   // D-212
	GalCacheA.SafeRelease(); GalCacheB.SafeRelease(); GalCachePending.Reset(); GalCachePendingVoxels = 0; bGalCacheReady = false; GalCacheEpoch = ~0u;   // D-211
	PromoteRing.Empty();
	ActivateRing.Empty();
	{
		FScopeLock Lock(&ActivationLock);
		ActivationsReady.Reset();
	}
	PickRing.Empty();    // D-193
	QueryRing.Empty();
	{
		FScopeLock Lock(&ApiLock);
		PicksReady.Reset();
		QueriesReady.Reset();
	}
	for (TRefCountPtr<IPooledRenderTarget>& M : BackgroundMaps) { M.SafeRelease(); }
	TemporalVolume.SafeRelease(); TemporalSlices.SafeRelease(); TemporalDepth.SafeRelease();   // D-184
	bTemporalHave = false;
	bBgHaveCur = bBgFilling = false;
	BgFillMask = 0u;
	BgFilledVersion = ~0u;
	BgBlendFrames = 0;
	RenderFrame = FUniFrameData();
	bTablesDirty = true;
}

void FUniRenderer::RenderVolume_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output,
	FRDGTextureRef SceneDepth, const FIntRect& DepthViewRect, float PreExposure, FRDGTextureRef& OutSlices, FIntPoint& OutMarchSize)
{
	const FUniFrameData& F = RenderFrame;
	TRACE_CPUPROFILER_EVENT_SCOPE(Uni_RenderVolume);
	RDG_EVENT_SCOPE(GraphBuilder, "Uni.Volume");
	const FIntRect OutRect = Output.ViewRect;
	const FMatrix Proj = View.ViewMatrices.ComputeProjectionNoAAMatrix();   // post-TSR: unjittered (W2)
	const float TanX = 1.0f / float(Proj.M[0][0]);
	const float TanY = 1.0f / float(Proj.M[1][1]);
	const int32 Div = FMath::Clamp(CVarUniVolumeDivisor.GetValueOnRenderThread(), 1, 8);
	FIntPoint MarchSize(1, 1);
	// D-196: a pixel budget; D-198: it falls with speed (the travel curve) to uu.VolumeTravelPixelScale
	// of itself at 10 Mpc/s: past ~8 march pixels of motion a frame the history is dropped and the
	// glow is the fresh quarter alone (D-194), so detail is not seen there anyway.
	// Quantised to quarters of the curve: the march size (and so its history) changes only there.
	const double SpeedQ = FMath::RoundToDouble(double(FMath::Clamp(F.SpeedAmount, 0.0f, 1.0f)) * 4.0) / 4.0;
	const double TravelScale = FMath::Lerp(1.0, double(FMath::Clamp(CVarUniVolumeTravelPixelScale.GetValueOnRenderThread(), 0.05f, 1.0f)), SpeedQ);
	// D-200: the auto budget (the GPU frame time) can take it lower still; the smaller wins.
	const double BudgetScale = FMath::Min(TravelScale, double(FMath::Clamp(F.VolumeBudgetScale, 0.05f, 1.0f)));
	UniCore::VolumeMarchSize(OutRect.Width(), OutRect.Height(), Div, double(FMath::Max(CVarUniVolumeMaxMarchPixels.GetValueOnRenderThread(), 0)) * BudgetScale, MarchSize.X, MarchSize.Y);
	OutMarchSize = MarchSize;

	// Shared inputs. The view's ThetaPixel is the march texel's, so prefiltering and
	// supersampling follow the resolution actually marched.
	UniCore::FVolumeViewGPU VV = F.VolumeView;
	VV.ThetaPixel = 2.0f * TanX / float(MarchSize.X);
	FRDGBufferRef ParamBuf = UploadStructured(GraphBuilder, TEXT("UniVolumeParams"), F.VolumeParams);
	FRDGBufferRef LutBuf = UploadStructured(GraphBuilder, TEXT("UniVolumeLut"), F.VolumeLut);
	FRDGTextureRef WebTex = RegisterWebTexture_RenderThread(GraphBuilder);   // D-164
	FRDGTextureRef GalTex[GalaxyTextureSlots];
	for (int32 S = 0; S < GalaxyTextureSlots; ++S) { GalTex[S] = RegisterGalaxyTexture_RenderThread(GraphBuilder, S); }   // D-179
	FRHISamplerState* WebSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	// D-211: the galaxy cache: bake the queued boxes, then the march reads the textures.
	FRDGTextureRef CacheTexA = nullptr, CacheTexB = nullptr;
	if (F.GalCacheN > 0u && VV.CacheCarrier != 0u && F.VolumeCarriers.IsValidIndex(int32(VV.CacheCarrier) - 1))
	{
		const FIntVector Size(int32(F.GalCacheN), int32(F.GalCacheN), int32(F.GalCacheLevels * (F.GalCacheN + 2u)));
		if (!GalCacheA.IsValid() || !GalCacheB.IsValid() || GalCacheSize != Size)
		{
			// New textures hold nothing: the cache is usable only if every box since its last
			// reset is still queued (the first frame after a reset), else the game thread resets.
			const bool bWasBaking = GalCacheA.IsValid();
			CacheTexA = GraphBuilder.CreateTexture(FRDGTextureDesc::Create3D(Size, PF_G16R16F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("UniGalCacheA"));
			CacheTexB = GraphBuilder.CreateTexture(FRDGTextureDesc::Create3D(Size, PF_R8G8B8A8, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_UAV), TEXT("UniGalCacheB"));
			GalCacheA = GraphBuilder.ConvertToExternalTexture(CacheTexA);
			GalCacheB = GraphBuilder.ConvertToExternalTexture(CacheTexB);
			GalCacheSize = Size;
			Stats.GalCacheBytes = uint32(FMath::Min<uint64>(uint64(Size.X) * Size.Y * Size.Z * 8u, 0xFFFFFFFFull));
			if (bWasBaking) { bGalCacheReady = false; GalCachePending.Reset(); GalCachePendingVoxels = 0; Stats.bGalCacheLost = true; }
		}
		else
		{
			CacheTexA = GraphBuilder.RegisterExternalTexture(GalCacheA, TEXT("UniGalCacheA"));
			CacheTexB = GraphBuilder.RegisterExternalTexture(GalCacheB, TEXT("UniGalCacheB"));
		}
		if (GalCachePending.Num() >= 2 && GalCachePendingVoxels > 0u)
		{
			// Each box's first voxel (the thread that starts it).
			TArray<FUintVector4> Boxes = MoveTemp(GalCachePending);
			uint32 First = 0u;
			for (int32 I = 0; I + 1 < Boxes.Num(); I += 2) { Boxes[I + 1].W = First; First += Boxes[I + 1].X * Boxes[I + 1].Y * Boxes[I + 1].Z; }
			const uint32 Rows = FMath::DivideAndRoundUp(First, 64u * 65535u);
			const uint32 PerRow = FMath::DivideAndRoundUp(First, Rows);
			FUniGalCacheBakeCS::FParameters* P = GraphBuilder.AllocParameters<FUniGalCacheBakeCS::FParameters>();
			P->InCarriers = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniGalCacheCarriers"), F.VolumeCarriers));
			P->InParams = GraphBuilder.CreateSRV(ParamBuf);
			P->InCacheBoxes = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniGalCacheBoxes"), Boxes));
			P->NumCacheBoxes = uint32(Boxes.Num() / 2);
			P->CacheVoxels = First;
			P->CacheThreadsPerRow = FMath::DivideAndRoundUp(PerRow, 64u) * 64u;
			P->CacheN = F.GalCacheN;
			P->CacheLevels = F.GalCacheLevels;
			P->CacheCarrier = VV.CacheCarrier - 1u;
			P->CacheV0 = F.GalCacheV0;
			P->CacheZCap = F.GalCacheZCap;
			P->CacheScale = F.GalCacheScale;
			P->CacheFootScale = F.GalCacheFootScale;
			P->UniGalWarpFine = GalTex[0];
			P->UniGalWarpLarge = GalTex[1];
			P->UniGalDetail = GalTex[2];
			P->UniWebSampler = WebSampler;
			P->OutCacheA = GraphBuilder.CreateUAV(CacheTexA);
			P->OutCacheB = GraphBuilder.CreateUAV(CacheTexB);
			TShaderMapRef<FUniGalCacheBakeCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Volume.CacheBake %u voxels, %d boxes", First, Boxes.Num() / 2), CS, P,
				FIntVector(int32(P->CacheThreadsPerRow / 64u), int32(Rows), 1));
			Stats.GalCacheVoxelsBaked.fetch_add(First);
			GalCachePendingVoxels = 0;
		}
		if (!bGalCacheReady) { VV.CacheCarrier = 0u; }
	}
	else
	{
		VV.CacheCarrier = 0u;
	}
	if (!CacheTexA) { CacheTexA = RegisterNanDummy_RenderThread(GraphBuilder); CacheTexB = CacheTexA; }
	FRDGTextureRef MapTex = RegisterGalaxyMap_RenderThread(GraphBuilder);   // D-212
	if (!(bGalMapComplete && F.GalMapEpoch == GalMapEpochRT && GalMapEpochRT != 0u)) { VV.MapCarrier = 0u; }   // a map baked for this frame's request only
	auto FillCommonWith = [&](FUniVolumeCommonParameters& C, const TArray<UniCore::FCarrierGPU>& Carriers, const UniCore::FVolumeViewGPU& V, FRDGBufferRef InParamBuf, FRDGBufferRef InLutBuf)
	{
		C.UniWebTexture = WebTex;
		C.UniWebSampler = WebSampler;
		C.UniGalWarpFine = GalTex[0];   // D-179
		C.UniGalWarpLarge = GalTex[1];
		C.UniGalDetail = GalTex[2];
		C.UniGalCacheA = CacheTexA;     // D-211
		C.UniGalCacheB = CacheTexB;
		C.UniGalMap = MapTex;           // D-212
		TArray<UniCore::FVolumeViewGPU> One; One.Add(V);
		One[0].NumCarriers = uint32(Carriers.Num());
		C.InCarriers = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniVolumeCarriers"), Carriers));
		C.InParams = GraphBuilder.CreateSRV(InParamBuf);
		C.InLut = GraphBuilder.CreateSRV(InLutBuf);
		C.InVolumeView = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniVolumeView"), One));
		C.ViewForward = FVector3f(View.GetViewDirection());
		C.ViewRight = FVector3f(View.GetViewRight());
		C.ViewUp = FVector3f(View.GetViewUp());
		C.TanHalfFovX = TanX;
		C.TanHalfFovY = TanY;
		C.Pad0 = 0.0f;
		C.WarpVelBeta = WarpVelBetaOf(F);   // D-190
		C.WarpDopplerKQ = WarpDopplerKQOf();
	};
	auto FillCommon = [&](FUniVolumeCommonParameters& C, const TArray<UniCore::FCarrierGPU>& Carriers, const UniCore::FVolumeViewGPU& V)
	{
		FillCommonWith(C, Carriers, V, ParamBuf, LutBuf);
	};
	FRHISamplerState* PointClamp = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	const FIntPoint DepthExtent = SceneDepth->Desc.Extent;
	// March pixel p (0..MarchSize) -> depth UV; output pixel (absolute) -> depth UV.
	const FVector4f MarchToDepthUV(
		float(DepthViewRect.Width()) / float(MarchSize.X) / float(DepthExtent.X), float(DepthViewRect.Height()) / float(MarchSize.Y) / float(DepthExtent.Y),
		float(DepthViewRect.Min.X) / float(DepthExtent.X), float(DepthViewRect.Min.Y) / float(DepthExtent.Y));
	const FVector2f OScale(float(DepthViewRect.Width()) / float(FMath::Max(OutRect.Width(), 1)) / float(DepthExtent.X),
		float(DepthViewRect.Height()) / float(FMath::Max(OutRect.Height(), 1)) / float(DepthExtent.Y));
	const FVector4f OutToDepthUV(OScale.X, OScale.Y,
		float(DepthViewRect.Min.X) / float(DepthExtent.X) - float(OutRect.Min.X) * OScale.X,
		float(DepthViewRect.Min.Y) / float(DepthExtent.Y) - float(OutRect.Min.Y) * OScale.Y);

	// 1. Background direction map (D-136), triple buffered (D-159): a refresh marches all six
	// faces of the back map from one snapshot, a few faces per frame, then rotates it in; the
	// composite cross-fades from the previous map to the current one.
	// D-166: per-pixel background. While the background changes (travel), a map refresh lags it
	// and costs about what marching it per pixel does now that the web is a texture (D-164), so
	// it is marched with the local carriers instead; at rest the map is cheaper and is used.
	const int32 BgMode = FMath::Clamp(CVarUniBackgroundMode.GetValueOnRenderThread(), 0, 2);
	if (F.BackgroundVersion != BgLastVersion)
	{
		BgDirtyInterval = BgLastVersion == ~0u ? (1 << 30) : BgStillFrames + 1;
		BgLastVersion = F.BackgroundVersion;
		BgStillFrames = 0;
	}
	else { ++BgStillFrames; }
	// D-169: the web debug view marches the background carriers (the web) per pixel.
	const int32 WebDebug = FMath::Clamp(CVarUniWebDebug.GetValueOnRenderThread(), 0, 6);
	const uint32 VolDebugMode = WebDebug > 0 ? 5u : uint32(FMath::Clamp(CVarUniVolumeDebug.GetValueOnRenderThread(), 0, 4));
	{
		// D-196: mode 2 marches the background per pixel only while it changes so often that a
		// map refresh per change (six faces) would cost more than marching it with every pixel
		// (about a quarter of the march's pixels a frame, sliced): fast travel. A change now and
		// then (drifting inside a galaxy, a LUT tweak) refreshes the map, cross-faded, instead:
		// before, any change put the background into every pixel's march for StillFrames frames,
		// about doubling the march's steps inside a galaxy. It leaves per-pixel marching once
		// the background has been still for StillFrames frames, as before (hysteresis).
		const int32 Cs = FMath::Clamp(CVarUniBackgroundSize.GetValueOnRenderThread(), 16, 1024);
		const double MapRays = 6.0 * double(Cs) * double(Cs);
		const double PixelRays = FMath::Max(double(MarchSize.X) * double(MarchSize.Y) * 0.25, 1.0);
		const int32 Want = CVarUniBackgroundPerPixelInterval.GetValueOnRenderThread();
		const int32 Interval = Want > 0 ? Want : FMath::Clamp(int32(FMath::CeilToDouble(MapRays / PixelRays)), 2, 120);
		const int32 Still = FMath::Max(CVarUniBackgroundStillFrames.GetValueOnRenderThread(), 1);
		if (BgMode != 2) { bBgPerPixel = false; }
		else if (!bBgPerPixel) { bBgPerPixel = BgStillFrames == 0 && BgDirtyInterval <= Interval; }
		else if (BgStillFrames >= Still) { bBgPerPixel = false; }
	}
	const bool bPerPixelBackground = F.BackgroundCarriers.Num() > 0 && (WebDebug > 0 || BgMode == 1 || (BgMode == 2 && bBgPerPixel));
	if (bPerPixelBackground) { Stats.BackgroundPerPixelFrames++; }
	if (bPerPixelBackground)
	{
		// The map is stale once we move; when the map comes back it is marched whole first (no
		// cross-fade from stale light).
		bBgHaveCur = false;
		bBgFilling = false;
		BgFilledVersion = ~0u;
	}
	const bool bBackgroundIn = F.BackgroundCarriers.Num() > 0 && !bPerPixelBackground;
	const int32 CubeSize = FMath::Clamp(CVarUniBackgroundSize.GetValueOnRenderThread(), 16, 1024);
	const int32 FacesPerFrame = FMath::Clamp(CVarUniBackgroundFaces.GetValueOnRenderThread(), 1, 6);
	bool bBackground = false;
	float BackgroundBlend = 1.0f;
	FRDGTextureRef Cube = nullptr;
	FRDGTextureRef CubePrev = nullptr;
	if (bBackgroundIn)
	{
		bool bRecreate = false;
		for (const TRefCountPtr<IPooledRenderTarget>& M : BackgroundMaps) { bRecreate = bRecreate || !M.IsValid() || M->GetDesc().Extent.X != CubeSize; }
		FRDGTextureRef Maps[3];
		if (bRecreate)
		{
			// Float32: radiance per steradian in exposure units overflows half floats (a galaxy disc
			// is ~7e4 per sr at the default exposure; sensitivity and exposure scale it further).
			const FRDGTextureDesc Desc = FRDGTextureDesc::Create2DArray(FIntPoint(CubeSize, CubeSize), PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV, 6);
			for (int32 I = 0; I < 3; ++I)
			{
				Maps[I] = GraphBuilder.CreateTexture(Desc, TEXT("UniBackgroundMap"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Maps[I]), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
				BackgroundMaps[I] = GraphBuilder.ConvertToExternalTexture(Maps[I]);
			}
			bBgHaveCur = bBgFilling = false;
			BgFilledVersion = ~0u;
		}
		else
		{
			for (int32 I = 0; I < 3; ++I) { Maps[I] = GraphBuilder.RegisterExternalTexture(BackgroundMaps[I], TEXT("UniBackgroundMap")); }
		}
		// Start a refresh only when none is running: the one in progress finishes from its own
		// snapshot, so all six faces of a map always agree.
		if (!bBgFilling && F.BackgroundVersion != BgFilledVersion)
		{
			BgSnapCarriers = F.BackgroundCarriers;
			BgSnapParams = F.VolumeParams;
			BgSnapLut = F.VolumeLut;
			BgSnapView = F.VolumeView;
			BgSnapView.WebHintScale = 1.0f;   // D-202: the map is kept, so it is marched at full quality (the speed's looser hints are per-pixel only)
			// D-203: no cell glow in the map: it is kept across frames while cells load and fade in,
			// so their glow would stay baked in after their sprites arrive (per pixel it is fresh).
			BgSnapView.GlowLevels = 0u;
			for (int32 K = 0; K < 4; ++K) { BgSnapView.GlowMask[K] = 0u; }
			BgSnapView.CacheCarrier = 0u;   // D-211: the map marches the background carriers alone
			BgSnapView.MapCarrier = 0u;     // D-212
			BgFillVersion = F.BackgroundVersion;
			BgFillMask = 0x3Fu;
			bBgFilling = true;
		}
		if (bBgFilling)
		{
			FRDGBufferRef SnapParamBuf = UploadStructured(GraphBuilder, TEXT("UniBackgroundParams"), BgSnapParams);
			FRDGBufferRef SnapLutBuf = UploadStructured(GraphBuilder, TEXT("UniBackgroundLut"), BgSnapLut);
			int32 Budget = bBgHaveCur ? FacesPerFrame : 6;   // the first map is marched at once: no black sky while it fills
			for (uint32 Face = 0; Face < 6 && Budget > 0; ++Face)
			{
				if (!(BgFillMask & (1u << Face))) { continue; }
				FUniVolumeBackgroundCS::FParameters* P = GraphBuilder.AllocParameters<FUniVolumeBackgroundCS::FParameters>();
				FillCommonWith(P->Common, BgSnapCarriers, BgSnapView, SnapParamBuf, SnapLutBuf);
				P->CubeSize = uint32(CubeSize);
				P->FaceBase = Face;
				P->OutCube = GraphBuilder.CreateUAV(Maps[BgBack]);
				TShaderMapRef<FUniVolumeBackgroundCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
				RDG_EVENT_SCOPE(GraphBuilder, "Uni.Volume.BackgroundMap");   // D-196: ProfileGPU shows scopes
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Volume.Background face %u", Face), CS, P, FIntVector(FMath::DivideAndRoundUp(CubeSize, 8), FMath::DivideAndRoundUp(CubeSize, 8), 1));
				BgFillMask &= ~(1u << Face);
				--Budget;
				Stats.BackgroundFacesMarched++;
			}
			if (BgFillMask == 0u)
			{
				// Rotate: previous <- current <- back <- the old previous (overwritten next refresh).
				const int32 OldPrev = BgPrev;
				BgPrev = BgCur;
				BgCur = BgBack;
				BgBack = OldPrev;
				if (!bBgHaveCur) { BgPrev = BgCur; bBgHaveCur = true; }   // nothing to fade from
				BgFilledVersion = BgFillVersion;
				bBgFilling = false;
				BgBlendFrames = 0;
			}
		}
		if (bBgHaveCur)
		{
			bBackground = true;
			Cube = Maps[BgCur];
			CubePrev = Maps[BgPrev];
			const int32 FadeFrames = FMath::DivideAndRoundUp(6, FacesPerFrame);   // one refresh's length
			BackgroundBlend = FMath::Clamp(float(BgBlendFrames + 1) / float(FadeFrames), 0.0f, 1.0f);
			++BgBlendFrames;
		}
	}
	if (!bBackground)
	{
		Cube = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(FIntPoint(1, 1), PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV, 6), TEXT("UniBackgroundDummy"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Cube), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
		CubePrev = Cube;
	}

	// 2. March the local carriers at reduced resolution (D-166: with the background's, per pixel).
	TArray<UniCore::FCarrierGPU> PerPixelCarriers;
	if (bPerPixelBackground) { PerPixelCarriers = F.VolumeCarriers; PerPixelCarriers.Append(F.BackgroundCarriers); }
	const TArray<UniCore::FCarrierGPU>& MarchCarriers = bPerPixelBackground ? PerPixelCarriers : F.VolumeCarriers;
	FRDGTextureRef VolumeTex = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(MarchSize, PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("UniVolume"));   // float32: see the cube
	FRDGTextureRef Slices = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(MarchSize, PF_R16F, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_UAV, UniGpuLayout::VolumeSlices), TEXT("UniVolumeSlices"));
	FRDGTextureRef DepthTex = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(MarchSize, PF_G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("UniVolumeDepth"));

	// D-184: temporal slicing. With a usable history (same march size, consecutive frames, no
	// camera jump) the march marches one pixel of every 2 x 2 block into the fresh textures and
	// the reconstruct pass fills the frame's volume from them and the reprojected history.
	// Without it every pixel is marched straight into the frame's volume (which becomes the
	// history). Debug views march every pixel.
	const bool bTemporalOn = CVarUniVolumeTemporal.GetValueOnRenderThread() != 0 && VolDebugMode == 0u && MarchSize.X >= 4 && MarchSize.Y >= 4;
	const bool bHistory = bTemporalHave && TemporalVolume.IsValid() && TemporalSlices.IsValid() && TemporalDepth.IsValid();
	const bool bSlice = bTemporalOn && bHistory && TemporalSize == MarchSize && F.bCameraStepValid && F.FrameNumber == TemporalLastFrame + 1;
	// D-209: why a frame was not sliced (uu.Perf), first reason only.
	const int32 NoSlice = !bTemporalOn ? 5 : !bHistory ? 6 : TemporalSize != MarchSize ? 7 : !F.bCameraStepValid ? 8 : F.FrameNumber == TemporalLastFrame ? 9 : 10;
	// D-208: the interleave from how far the image moves a frame (march pixels): the view's turn,
	// plus the camera's step over the nearest marched structure's distance.
	uint32 Interleave = 2u;
	if (bSlice)
	{
		const float ThetaPix = 2.0f * TanX / float(FMath::Max(MarchSize.X, 1));
		const FVector3f Fw(View.GetViewDirection()), Rt(View.GetViewRight());
		const float Turn = FMath::Max(FVector3f::CrossProduct(Fw, TemporalPrevForward).Size(), FVector3f::CrossProduct(Rt, TemporalPrevRight).Size());
		const float StepCm = F.CameraStepCm.Size();
		const float Move = StepCm > 0.0f ? (F.VolumeNearCm > 0.0f ? StepCm / F.VolumeNearCm : 1.0e9f) : 0.0f;
		const float MotionPx = (Turn + Move) / FMath::Max(ThetaPix, 1.0e-9f);
		Interleave = UniHlsl::Uni_TemporalInterleave(MotionPx, uint32(FMath::Clamp(CVarUniVolumeInterleave.GetValueOnRenderThread(), 2, 4)));
	}
	const uint32 Phase = bSlice ? UniHlsl::Uni_TemporalPack(++TemporalStep, Interleave) : 0u;
	const UniHlsl::uint2 SliceOff = UniHlsl::Uni_TemporalOffset(Phase);
	const FUintVector2 SliceOffset(SliceOff.x, SliceOff.y);
	FRDGTextureRef MarchVol = VolumeTex, MarchSlices = Slices, MarchDepth = DepthTex;
	if (bSlice)
	{
		MarchVol = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(MarchSize, PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("UniVolumeFresh"));
		MarchSlices = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(MarchSize, PF_R16F, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_UAV, UniGpuLayout::VolumeSlices), TEXT("UniVolumeFreshSlices"));
		MarchDepth = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(MarchSize, PF_G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("UniVolumeFreshDepth"));
	}
	{
		FUniVolumeMarchCS::FParameters* P = GraphBuilder.AllocParameters<FUniVolumeMarchCS::FParameters>();
		FillCommon(P->Common, MarchCarriers, VV);
		P->InvDeviceZToWorldZTransform = FVector4f(View.InvDeviceZToWorldZTransform);
		P->MarchToDepthUV = MarchToDepthUV;
		P->SceneDepthTexture = SceneDepth;
		P->SceneDepthSampler = PointClamp;
		P->MarchSize = FUintVector2(uint32(MarchSize.X), uint32(MarchSize.Y));
		P->DebugMode = VolDebugMode;
		P->DebugLogLo = CVarUniVolumeDebugLogLo.GetValueOnRenderThread();
		P->WebDebugMode = uint32(FMath::Max(WebDebug, 1));
		P->WebDebugDepthCm = float(double(FMath::Max(CVarUniWebDebugDepth.GetValueOnRenderThread(), 0.1f)) * 3.0856775814913673e24);
		P->WebDebugThreshold = FMath::Max(CVarUniWebDebugThreshold.GetValueOnRenderThread(), 1.0e-3f);
		P->WebDebugOpacity = FMath::Max(CVarUniWebDebugOpacity.GetValueOnRenderThread(), 0.0f);
		P->bSliceMarch = bSlice ? Interleave : 0u;   // D-208: N (2..4)
		P->SliceOffset = SliceOffset;
		P->OutVolume = GraphBuilder.CreateUAV(MarchVol);
		P->OutSlices = GraphBuilder.CreateUAV(MarchSlices);
		P->OutDepth = GraphBuilder.CreateUAV(MarchDepth);
		TShaderMapRef<FUniVolumeMarchCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
		const FIntPoint Grid = bSlice ? FIntPoint(FMath::DivideAndRoundUp(MarchSize.X, int32(Interleave)), FMath::DivideAndRoundUp(MarchSize.Y, int32(Interleave))) : MarchSize;
		TemporalInterleave = bSlice ? Interleave : 1u;
		Stats.VolumeMarchWidth = uint32(MarchSize.X);
		Stats.VolumeMarchHeight = uint32(MarchSize.Y);
		Stats.VolumeMarchRays = uint32(Grid.X) * uint32(Grid.Y);
		Stats.VolumeMarchFlags = (bPerPixelBackground ? 1u : 0u) | (bSlice ? 2u : 0u) | ((bSlice ? Interleave : 1u) << 8);
		Stats.VolumeSliceCounts[bSlice ? int32(Interleave) : NoSlice].fetch_add(1u);
		// D-196: ProfileGPU shows scopes (not pass names): the scope says what was marched.
		RDG_EVENT_SCOPE(GraphBuilder, "%s", bPerPixelBackground ? TEXT("Uni.Volume.March+Background") : TEXT("Uni.Volume.March"));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Volume.March %dx%d, %d carriers%s%s", MarchSize.X, MarchSize.Y, MarchCarriers.Num(), bPerPixelBackground ? TEXT(" (with the background)") : TEXT(""), bSlice ? (Interleave == 4u ? TEXT(", 1/16 sliced") : (Interleave == 3u ? TEXT(", 1/9 sliced") : TEXT(", 1/4 sliced"))) : TEXT("")), CS, P, FComputeShaderUtils::GetGroupCount(Grid, 8));
	}
	if (bSlice)
	{
		FUniVolumeReconstructCS::FParameters* P = GraphBuilder.AllocParameters<FUniVolumeReconstructCS::FParameters>();
		FillCommon(P->Common, MarchCarriers, VV);
		P->InvDeviceZToWorldZTransform = FVector4f(View.InvDeviceZToWorldZTransform);
		P->MarchToDepthUV = MarchToDepthUV;
		P->SceneDepthTexture = SceneDepth;
		P->SceneDepthSampler = PointClamp;
		P->MarchSize = FUintVector2(uint32(MarchSize.X), uint32(MarchSize.Y));
		P->PrevForward = TemporalPrevForward; P->PrevTanX = TemporalPrevTanX;
		P->PrevRight = TemporalPrevRight; P->PrevTanY = TemporalPrevTanY;
		P->PrevUp = TemporalPrevUp; P->TemporalPhase = Phase;
		P->CamStep = F.CameraStepCm; P->bHistory = 1u;
		P->PrevWarpVelBeta = TemporalPrevWarp;   // D-190
		P->TemporalClamp = FVector4f(CVarUniVolumeTemporalClamp.GetValueOnRenderThread() != 0 ? 1.0f : 0.0f,
			FMath::Max(CVarUniVolumeTemporalTightPx.GetValueOnRenderThread(), 0.01f), FMath::Max(CVarUniVolumeTemporalMaxPx.GetValueOnRenderThread(), 0.0f), 0.0f);   // D-194
		P->FreshVolume = MarchVol; P->FreshSlices = MarchSlices; P->FreshDepth = MarchDepth;
		P->HistVolume = GraphBuilder.RegisterExternalTexture(TemporalVolume, TEXT("UniVolumeHistory"));
		P->HistSlices = GraphBuilder.RegisterExternalTexture(TemporalSlices, TEXT("UniVolumeHistorySlices"));
		P->HistDepth = GraphBuilder.RegisterExternalTexture(TemporalDepth, TEXT("UniVolumeHistoryDepth"));
		P->OutVolume = GraphBuilder.CreateUAV(VolumeTex);
		P->OutSlices = GraphBuilder.CreateUAV(Slices);
		P->OutDepth = GraphBuilder.CreateUAV(DepthTex);
		TShaderMapRef<FUniVolumeReconstructCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
		RDG_EVENT_SCOPE(GraphBuilder, "Uni.Volume.Reconstruct");   // D-196
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Volume.Reconstruct %dx%d", MarchSize.X, MarchSize.Y), CS, P, FComputeShaderUtils::GetGroupCount(MarchSize, 8));
	}
	// D-184: this frame's volume is the next frame's history.
	if (bTemporalOn)
	{
		GraphBuilder.QueueTextureExtraction(VolumeTex, &TemporalVolume);
		GraphBuilder.QueueTextureExtraction(Slices, &TemporalSlices);
		GraphBuilder.QueueTextureExtraction(DepthTex, &TemporalDepth);
		TemporalSize = MarchSize;
		TemporalPrevForward = FVector3f(View.GetViewDirection());
		TemporalPrevRight = FVector3f(View.GetViewRight());
		TemporalPrevUp = FVector3f(View.GetViewUp());
		TemporalPrevTanX = TanX;
		TemporalPrevTanY = TanY;
		TemporalLastFrame = F.FrameNumber;
		TemporalPrevWarp = WarpVelBetaOf(F);   // D-190
		TemporalPhase = Phase;
		bTemporalHave = true;
	}
	else
	{
		bTemporalHave = false;
	}

	// 3. Composite: upsample + background, additive into the output (before the sprites).
	{
		FUniVolumeCompositePassParameters* P = GraphBuilder.AllocParameters<FUniVolumeCompositePassParameters>();
		FUniVolumeCompositeParameters& C = P->Composite;
		FillCommon(C.Common, F.VolumeCarriers, VV);
		C.InvDeviceZToWorldZTransform = FVector4f(View.InvDeviceZToWorldZTransform);
		C.MarchToDepthUV = MarchToDepthUV;
		C.SceneDepthTexture = SceneDepth;
		C.SceneDepthSampler = PointClamp;
		C.MarchSize = FUintVector2(uint32(MarchSize.X), uint32(MarchSize.Y));
		C.OutRectMinSize = FVector4f(float(OutRect.Min.X), float(OutRect.Min.Y), float(OutRect.Width()), float(OutRect.Height()));
		C.OutToDepthUV = OutToDepthUV;
		C.SceneColourScale = FVector3f(F.SceneColourScale * PreExposure);
		C.FocalPx = float(OutRect.Width()) * 0.5f / TanX;
		C.CubeSize = bBackground ? uint32(CubeSize) : 1u;
		C.bBackground = bBackground ? 1u : 0u;
		C.BackgroundStartCm = F.BackgroundStartCm;
		C.BackgroundBlend = BackgroundBlend;   // D-159
		C.DebugMode = VolDebugMode;
		C.DebugScale = 1.0f / FMath::Max(PreExposure, 1.0e-6f);
		C.VolumeTexture = VolumeTex;
		C.CubeTexture = Cube;
		C.CubePrevTexture = CubePrev;
		C.LinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);
		TShaderMapRef<FUniVolumeCompositeVS> VS(GetGlobalShaderMap(View.GetFeatureLevel()));
		TShaderMapRef<FUniVolumeCompositePS> PS(GetGlobalShaderMap(View.GetFeatureLevel()));
		RDG_EVENT_SCOPE(GraphBuilder, "Uni.Volume.Composite");   // D-196
		GraphBuilder.AddPass(RDG_EVENT_NAME("Uni.Volume.Composite"), P, ERDGPassFlags::Raster,
			[P, VS, PS, OutRect](FRHICommandList& RHICmdList)
			{
				RHICmdList.SetViewport(float(OutRect.Min.X), float(OutRect.Min.Y), 0.0f, float(OutRect.Max.X), float(OutRect.Max.Y), 1.0f);
				FGraphicsPipelineStateInitializer Pso;
				RHICmdList.ApplyCachedRenderTargets(Pso);
				Pso.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI();
				Pso.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				Pso.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				Pso.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				Pso.BoundShaderState.VertexShaderRHI = VS.GetVertexShader();
				Pso.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
				Pso.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, Pso, 0);
				SetShaderParameters(RHICmdList, VS, VS.GetVertexShader(), P->Composite);
				SetShaderParameters(RHICmdList, PS, PS.GetPixelShader(), P->Composite);
				RHICmdList.DrawPrimitive(0, 1, 1);
			});
	}
	OutSlices = Slices;
	Stats.VolumeCarriersLastFrame = uint32(F.VolumeCarriers.Num());
}

void FUniRenderer::RenderBackdrop_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& SceneColor,
	FRDGTextureRef SceneDepth, const FIntRect& DepthViewRect, float PreExposure)
{
	check(IsInRenderingThread());
	const FUniFrameData& F = RenderFrame;
	if (!UniShadersEnabled() || !F.bEnabled || F.Cells.Num() == 0 || !SceneColor.IsValid() || !SceneDepth) { return; }
	TRACE_CPUPROFILER_EVENT_SCOPE(Uni_RenderBackdrop);
	RDG_EVENT_SCOPE(GraphBuilder, "UnifiedUniverse");

	FRDGBufferRef PoolBuf = RegisterPool_RenderThread(GraphBuilder);
	FRDGBufferRef CellBuf = UploadStructured(GraphBuilder, TEXT("UniCells"), F.Cells);

	FRDGBufferRef SpriteBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::SpriteBytes, UniGpuLayout::MaxSprites), TEXT("UniSprites"));
	FRDGBufferDesc ArgsDesc = FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndirectParameters>(1);
	ArgsDesc.Usage |= EBufferUsageFlags::SourceCopy;   // D-157: the sprite count is read back for uu.Perf
	FRDGBufferRef ArgsBuf = GraphBuilder.CreateBuffer(ArgsDesc, TEXT("UniSpriteArgs"));
	FRDGBufferRef PromoteBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), UniGpuLayout::MaxPromote), TEXT("UniPromote"));
	FRDGBufferDesc PromoteCountDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1);
	PromoteCountDesc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef PromoteCountBuf = GraphBuilder.CreateBuffer(PromoteCountDesc, TEXT("UniPromoteCount"));

	FRDGBufferUAVRef ArgsUAV = GraphBuilder.CreateUAV(ArgsBuf, PF_R32_UINT);
	FRDGBufferUAVRef PromoteCountUAV = GraphBuilder.CreateUAV(PromoteCountBuf, PF_R32_UINT);
	AddClearUAVPass(GraphBuilder, ArgsUAV, 0u);          // the shader writes args[0] = 6
	AddClearUAVPass(GraphBuilder, PromoteCountUAV, 0u);

	// System activations (D-142): indices, records and a count, read back through a ring.
	FRDGBufferDesc ActIndexDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), UniGpuLayout::MaxActivate);
	ActIndexDesc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef ActIndexBuf = GraphBuilder.CreateBuffer(ActIndexDesc, TEXT("UniActivate"));
	FRDGBufferDesc ActRecordDesc = FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::ObjectBytes, UniGpuLayout::MaxActivate);
	ActRecordDesc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef ActRecordBuf = GraphBuilder.CreateBuffer(ActRecordDesc, TEXT("UniActivateRecord"));
	FRDGBufferDesc ActCountDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1);
	ActCountDesc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef ActCountBuf = GraphBuilder.CreateBuffer(ActCountDesc, TEXT("UniActivateCount"));
	FRDGBufferUAVRef ActCountUAV = GraphBuilder.CreateUAV(ActCountBuf, PF_R32_UINT);
	AddClearUAVPass(GraphBuilder, ActCountUAV, 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ActIndexBuf), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ActRecordBuf), 0u);

	// D-193: finished pick / query readbacks; this frame's pick (once per frame: the first view
	// drawing it) needs each sprite's source from the project pass.
	CollectApiReadbacks_RenderThread();
	if (PickRing.Num() == 0) { for (int32 I = 0; I < 3; ++I) { PickRing.Add(MakeShared<FPickReadback>()); } }
	TSharedPtr<FPickReadback> PickSlot;
	if (F.PickRequestId != 0 && F.FrameNumber != LastPickFrame)
	{
		for (const TSharedPtr<FPickReadback>& R : PickRing) { if (!R->bPending) { PickSlot = R; break; } }
	}
	const bool bPick = PickSlot.IsValid();
	FRDGBufferRef SpriteSourceBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), bPick ? UniGpuLayout::MaxSprites : 1u), TEXT("UniSpriteSource"));
	if (!bPick) { AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SpriteSourceBuf), 0u); }   // bound, never written

	// Planet records (D-143) into the reserved pool tail, before the project pass reads them.
	if (F.DynamicRecords.Num() > 0)
	{
		const int32 N = FMath::Min(F.DynamicRecords.Num(), int32(UniGpuLayout::DynamicPoolSlots));
		TArray<uint32> Index;
		TArray<UniCore::FObjectGPU> Records;
		Index.SetNumUninitialized(N);
		Records.Append(F.DynamicRecords.GetData(), N);
		for (int32 I = 0; I < N; ++I) { Index[I] = UniGpuLayout::DynamicPoolBase + uint32(I); }
		FUniScatterCS::FParameters* P = GraphBuilder.AllocParameters<FUniScatterCS::FParameters>();
		P->InScatterIndex = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniDynamicIndex"), Index));
		P->InScatterRecord = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniDynamicRecord"), Records));
		P->OutPool = GraphBuilder.CreateUAV(PoolBuf);
		P->NumWrites = uint32(N);
		TShaderMapRef<FUniScatterCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Systems.PlanetRecords %d", N), CS, P, FIntVector(FMath::DivideAndRoundUp(N, 64), 1, 1));
	}

	// View basis and projection from the engine view. Universe axes are UE world axes (cm), and
	// camera-relative render positions are translated-world positions (PreViewTranslation = -origin).
	const FVector3f Forward(View.GetViewDirection());
	const FVector3f Right(View.GetViewRight());
	const FVector3f Up(View.GetViewUp());
	// The pass runs after TSR: use the UNJITTERED projection, or stars wobble by the TAA jitter.
	const FMatrix Proj = View.ViewMatrices.ComputeProjectionNoAAMatrix();
	const float TanX = 1.0f / float(Proj.M[0][0]);
	const float TanY = 1.0f / float(Proj.M[1][1]);
	const FIntRect OutRect = SceneColor.ViewRect;
	const float PixelsPerRadian = float(OutRect.Width()) * 0.5f / TanX;
	const float LimitingMag = 6.5f + FMath::Clamp(F.DeltaMag, -5.0f, 5.0f);   // UNI_M_REF + dm (UniPhotometry.ush)
	Stats.ThetaPixelLastFrame = 1.0f / PixelsPerRadian;

	{
		FUniProjectCS::FParameters* P = GraphBuilder.AllocParameters<FUniProjectCS::FParameters>();
		P->InCells = GraphBuilder.CreateSRV(CellBuf);
		P->InPool = GraphBuilder.CreateSRV(PoolBuf);
		P->OutSprites = GraphBuilder.CreateUAV(SpriteBuf);
		P->OutDrawArgs = ArgsUAV;
		P->OutPromote = GraphBuilder.CreateUAV(PromoteBuf);
		P->OutPromoteCount = PromoteCountUAV;
		P->OutActivate = GraphBuilder.CreateUAV(ActIndexBuf);
		P->OutActivateRecord = GraphBuilder.CreateUAV(ActRecordBuf);
		P->OutActivateCount = ActCountUAV;
		P->OutSpriteSource = GraphBuilder.CreateUAV(SpriteSourceBuf);   // D-193
		P->WriteSpriteSource = bPick ? 1u : 0u;
		P->InHandoffs = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniCarrierHandoffs"), F.CarrierHandoffs));   // D-196
		P->NumHandoffs = uint32(F.CarrierHandoffs.Num());
		P->NumCells = uint32(F.Cells.Num());
		P->MaxSprites = UniGpuLayout::MaxSprites;
		P->MaxPromote = UniGpuLayout::MaxPromote;
		P->MaxActivate = UniGpuLayout::MaxActivate;
		P->SystemArchetypeMask = F.SystemArchetypeMask;
		P->WarpAmount = FMath::Clamp(F.WarpAmount, 0.0f, 1.0f);   // D-187
		P->WarpVelBeta = WarpVelBetaOf(F);   // D-190
		P->WarpDopplerKQ = WarpDopplerKQOf();
		P->ArrivalFrames = FMath::Max(CVarUniWarpArrival.GetValueOnRenderThread(), 0.0f);
		P->DiscArchetypeMask = F.DiscArchetypeMask;   // D-183
		P->FixedDiscSeeds = FUintVector4(F.FixedDiscSeeds[0], F.FixedDiscSeeds[1], F.FixedDiscSeeds[2], F.FixedDiscSeeds[3]);
		P->ViewForward = Forward; P->ViewRight = Right; P->ViewUp = Up;
		P->TanHalfFovX = TanX; P->TanHalfFovY = TanY;
		P->LimitingMag = LimitingMag;
		P->CameraFraction = F.CameraFraction;
		P->ExposureMag = LimitingMag - 1.0f + F.ExposureBias;
		P->SurfaceLimit = 25.0f + FMath::Clamp(F.DeltaMag, -5.0f, 5.0f);         // UNI_MU_REF + dm
		P->ThetaPixel = 1.0f / PixelsPerRadian;
		P->SpriteCorePx = FMath::Clamp(CVarUniSpriteCorePx.GetValueOnRenderThread(), 0.25f, 16.0f);
		P->SpriteCoreSat = FMath::Max(CVarUniSpriteSaturation.GetValueOnRenderThread(), 0.0f);
		P->SpriteHaloGain = FMath::Max(CVarUniSpriteHaloGain.GetValueOnRenderThread(), 0.0f);
		P->SpriteHaloGrowthPx = FMath::Max(CVarUniSpriteHaloGrowth.GetValueOnRenderThread(), 0.0f);
		P->SpriteHaloMaxPx = FMath::Clamp(CVarUniSpriteHaloMax.GetValueOnRenderThread(), 0.0f, 64.0f);
		uint32 GroupsX = 1;
		const FIntVector Groups = WrappedGroups(uint32(F.Cells.Num()), GroupsX);
		P->DispatchGroupsX = GroupsX;
		TShaderMapRef<FUniProjectCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.ProjectAndCull %d cells", F.Cells.Num()), CS, P, Groups);
	}

	// D-193: the pick, over the sprites just written: best score, lowest index at it, then its
	// source and record, read back through the ring.
	if (bPick)
	{
		LastPickFrame = F.FrameNumber;
		FRDGBufferRef BestBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2), TEXT("UniPickBest"));
		FRDGBufferDesc ResultDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), UniGpuLayout::PickResultWords);
		ResultDesc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef ResultBuf = GraphBuilder.CreateBuffer(ResultDesc, TEXT("UniPickResult"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BestBuf), 0u);     // [0] best score bits, [1] ~index: 0 is "none"
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ResultBuf), 0u);
		auto Params = [&]()
		{
			FUniPickParameters* P = GraphBuilder.AllocParameters<FUniPickParameters>();
			P->InCells = GraphBuilder.CreateSRV(CellBuf);
			P->InPool = GraphBuilder.CreateSRV(PoolBuf);
			P->InSprites = GraphBuilder.CreateSRV(SpriteBuf);
			P->InSpriteSource = GraphBuilder.CreateSRV(SpriteSourceBuf);
			P->InDrawArgs = GraphBuilder.CreateSRV(ArgsBuf, PF_R32_UINT);
			P->OutPickBest = GraphBuilder.CreateUAV(BestBuf);
			P->OutPickResult = GraphBuilder.CreateUAV(ResultBuf);
			P->MaxSprites = UniGpuLayout::MaxSprites;
			P->CursorNdc = F.PickCursorNdc;
			P->HalfViewPx = FVector2f(float(OutRect.Width()) * 0.5f, float(OutRect.Height()) * 0.5f);
			P->PixelsPerRadian = PixelsPerRadian;
			P->PickMinPx = FMath::Max(F.PickMinPx, 0.5f);
			return P;
		};
		const FIntVector SpriteGroups(int32(FMath::DivideAndRoundUp(UniGpuLayout::MaxSprites, 64u)), 1, 1);
		TShaderMapRef<FUniPickScoreCS> ScoreCS(GetGlobalShaderMap(View.GetFeatureLevel()));
		TShaderMapRef<FUniPickSelectCS> SelectCS(GetGlobalShaderMap(View.GetFeatureLevel()));
		TShaderMapRef<FUniPickResolveCS> ResolveCS(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Pick.Score"), ScoreCS, Params(), SpriteGroups);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Pick.Select"), SelectCS, Params(), SpriteGroups);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Pick.Resolve"), ResolveCS, Params(), FIntVector(1, 1, 1));
		if (!PickSlot->Result.IsValid()) { PickSlot->Result = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniPickReadback")); }
		AddEnqueueCopyPass(GraphBuilder, PickSlot->Result.Get(), ResultBuf, UniGpuLayout::PickResultWords * sizeof(uint32));
		PickSlot->FrameNumber = F.FrameNumber;
		PickSlot->RequestId = F.PickRequestId;
		PickSlot->bPending = true;
	}

	// Promotion candidates: count readback (ring of 3, never waited on).
	if (PromoteRing.Num() == 0) { for (int32 I = 0; I < 3; ++I) { PromoteRing.Add(MakeShared<FPromoteReadback>()); } }
	for (const TSharedPtr<FPromoteReadback>& R : PromoteRing)
	{
		if (R->bPending && R->Count->IsReady() && R->Args->IsReady())
		{
			const uint32* Ptr = static_cast<const uint32*>(R->Count->Lock(sizeof(uint32)));   // render thread only (lesson #24)
			Stats.PromoteCandidatesLastFrame = Ptr ? *Ptr : 0u;
			R->Count->Unlock();
			const uint32* Args = static_cast<const uint32*>(R->Args->Lock(sizeof(FRHIDrawIndirectParameters)));
			Stats.SpritesLastFrame = Args ? FMath::Min(Args[1], UniGpuLayout::MaxSprites) : 0u;   // {vertices, instances, 0, 0}
			R->Args->Unlock();
			R->bPending = false;
		}
	}
	{
		TSharedPtr<FPromoteReadback>& R = PromoteRing[PromoteRingNext];
		if (!R->bPending)
		{
			if (!R->Count.IsValid()) { R->Count = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniPromoteCountReadback")); }
			if (!R->Args.IsValid()) { R->Args = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniSpriteArgsReadback")); }
			AddEnqueueCopyPass(GraphBuilder, R->Count.Get(), PromoteCountBuf, sizeof(uint32));
			AddEnqueueCopyPass(GraphBuilder, R->Args.Get(), ArgsBuf, sizeof(FRHIDrawIndirectParameters));
			R->bPending = true;
			PromoteRingNext = (PromoteRingNext + 1) % PromoteRing.Num();
		}
	}

	// System activations: collect finished readbacks (never waited on), then enqueue this frame's.
	if (ActivateRing.Num() == 0) { for (int32 I = 0; I < 4; ++I) { ActivateRing.Add(MakeShared<FActivateReadback>()); } }
	for (const TSharedPtr<FActivateReadback>& R : ActivateRing)
	{
		if (!R->bPending || !R->Count->IsReady() || !R->Index->IsReady() || !R->Records->IsReady()) { continue; }
		const uint32* CountPtr = static_cast<const uint32*>(R->Count->Lock(sizeof(uint32)));   // render thread only (lesson #24)
		const uint32 Count = FMath::Min(CountPtr ? *CountPtr : 0u, UniGpuLayout::MaxActivate);
		R->Count->Unlock();
		Stats.ActivationsLastReadback = Count;
		if (Count > 0)
		{
			const FUintVector2* Idx = static_cast<const FUintVector2*>(R->Index->Lock(Count * sizeof(FUintVector2)));
			TArray<FUintVector2> IdxCopy;
			IdxCopy.Append(Idx, int32(Count));
			R->Index->Unlock();
			const UniCore::FObjectGPU* Rec = static_cast<const UniCore::FObjectGPU*>(R->Records->Lock(Count * UniGpuLayout::ObjectBytes));
			FScopeLock Lock(&ActivationLock);
			for (uint32 I = 0; I < Count; ++I)
			{
				FUniActivation A;
				A.FrameNumber = R->FrameNumber;
				A.CellIndex = IdxCopy[int32(I)].X;
				A.ObjectIndex = IdxCopy[int32(I)].Y;
				A.Record = Rec[I];
				ActivationsReady.Add(A);
			}
			R->Records->Unlock();
			// Bounded: the game thread drains every frame; if it stops (paused), keep the newest.
			if (ActivationsReady.Num() > 1024) { ActivationsReady.RemoveAt(0, ActivationsReady.Num() - 1024); }
		}
		R->bPending = false;
	}
	if (F.SystemArchetypeMask != 0u)
	{
		TSharedPtr<FActivateReadback>& R = ActivateRing[ActivateRingNext];
		if (!R->bPending)
		{
			if (!R->Count.IsValid())
			{
				R->Count = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniActivateCountReadback"));
				R->Index = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniActivateIndexReadback"));
				R->Records = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniActivateRecordReadback"));
			}
			AddEnqueueCopyPass(GraphBuilder, R->Count.Get(), ActCountBuf, sizeof(uint32));
			AddEnqueueCopyPass(GraphBuilder, R->Index.Get(), ActIndexBuf, UniGpuLayout::MaxActivate * sizeof(FUintVector2));
			AddEnqueueCopyPass(GraphBuilder, R->Records.Get(), ActRecordBuf, UniGpuLayout::MaxActivate * UniGpuLayout::ObjectBytes);
			R->FrameNumber = F.FrameNumber;
			R->bPending = true;
			ActivateRingNext = (ActivateRingNext + 1) % ActivateRing.Num();
		}
	}

	// D-193: this frame's resident query (once per frame), over the cell table and pool.
	if (F.QueryRequestId != 0 && F.FrameNumber != LastQueryFrame)
	{
		LastQueryFrame = F.FrameNumber;
		AddQuery_RenderThread(GraphBuilder, F, View.GetFeatureLevel(), CellBuf, PoolBuf);
	}

	// Volume (D-128..D-136): drawn first so sprites can read its transmittance slices.
	FRDGTextureRef VolumeSlices = nullptr;
	FIntPoint MarchSize(1, 1);
	if (F.bVolume) { RenderVolume_RenderThread(GraphBuilder, View, SceneColor, SceneDepth, DepthViewRect, PreExposure, VolumeSlices, MarchSize); }
	if (!VolumeSlices)
	{
		VolumeSlices = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(FIntPoint(1, 1), PF_R16F, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_UAV, UniGpuLayout::VolumeSlices), TEXT("UniVolumeSlicesDummy"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VolumeSlices), FLinearColor::White);
	}

	// Sprites, additive, into scene colour, tested against scene depth.
	{
		FUniSpritePassParameters* P = GraphBuilder.AllocParameters<FUniSpritePassParameters>();
		P->Sprite.InSprites = GraphBuilder.CreateSRV(SpriteBuf);
		P->Sprite.MaxSprites = UniGpuLayout::MaxSprites;
		P->Sprite.TranslatedWorldToClip = FMatrix44f(View.ViewMatrices.GetTranslatedViewMatrix() * Proj);
		P->Sprite.ViewForward = Forward; P->Sprite.ViewRight = Right; P->Sprite.ViewUp = Up;
		P->Sprite.SpriteExtent = CVarUniSpriteExtent.GetValueOnRenderThread();
		P->Sprite.InvDeviceZToWorldZTransform = FVector4f(View.InvDeviceZToWorldZTransform);
		// Output pixel -> depth UV. After TSR the output is at display resolution and the depth at
		// internal resolution, so map the output view rect onto the depth view rect.
		const FIntPoint DepthExtent = SceneDepth->Desc.Extent;
		const FVector2f Scale(float(DepthViewRect.Width()) / float(FMath::Max(OutRect.Width(), 1)) / float(DepthExtent.X),
			float(DepthViewRect.Height()) / float(FMath::Max(OutRect.Height(), 1)) / float(DepthExtent.Y));
		const FVector2f Bias(float(DepthViewRect.Min.X) / float(DepthExtent.X) - float(OutRect.Min.X) * Scale.X,
			float(DepthViewRect.Min.Y) / float(DepthExtent.Y) - float(OutRect.Min.Y) * Scale.Y);
		P->Sprite.DepthUVScaleBias = FVector4f(Scale.X, Scale.Y, Bias.X, Bias.Y);
		P->Sprite.SceneColourScale = FVector3f(F.SceneColourScale * PreExposure);   // scene colour is pre-exposed here
		P->Sprite.PixelsPerRadian = PixelsPerRadian;
		P->Sprite.HaloChroma = FMath::Clamp(CVarUniSpriteHaloChroma.GetValueOnRenderThread(), 0.0f, 4.0f);   // D-185
		P->Sprite.SpriteFloor = FMath::Max(CVarUniSpriteFloor.GetValueOnRenderThread(), 1.0e-8f);           // D-188
		P->Sprite.WarpA = FVector4f(F.bCameraStepValid ? FMath::Clamp(F.WarpAmount, 0.0f, 1.0f) : 0.0f, FMath::Max(CVarUniWarpStreakFrames.GetValueOnRenderThread(), 0.0f),
			FMath::Clamp(CVarUniWarpStreakMax.GetValueOnRenderThread(), 0.0f, 512.0f), FMath::Max(CVarUniWarpArrivalStretch.GetValueOnRenderThread(), 0.0f));   // D-187
		P->Sprite.CamStep = F.bCameraStepValid ? F.CameraStepCm : FVector3f::ZeroVector;
		P->Sprite.WarpPad = 0.0f;
		P->Sprite.WarpVelBeta = WarpVelBetaOf(F);   // D-190
		P->Sprite.MoteMode = 0u;                    // D-192: the motes are a second draw (below)
		P->Sprite.MoteCell0 = F.MoteCell[0];
		P->Sprite.MoteCell1 = F.MoteCell[1];
		P->Sprite.MoteFrac0 = F.MoteFrac[0];
		P->Sprite.MoteFrac1 = F.MoteFrac[1];
		P->Sprite.MoteScale = FVector4f(F.MoteScale.X, F.MoteScale.Y, F.MoteScale.Z, FMath::Max(CVarUniSpriteCorePx.GetValueOnRenderThread(), 0.25f) / FMath::Max(PixelsPerRadian, 1.0f));
		P->Sprite.MoteDopplerKQ = WarpDopplerKQOf();
		P->Sprite.SceneDepthTexture = SceneDepth;
		P->Sprite.SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->Sprite.VolumeSlices = VolumeSlices;
		P->Sprite.bVolume = F.bVolume ? 1u : 0u;
		P->Sprite.SliceLnTLo = F.VolumeView.SliceLnTLo;
		P->Sprite.SliceLnTHi = F.VolumeView.SliceLnTHi;
		P->Sprite.OutToMarch = FVector4f(float(MarchSize.X) / float(FMath::Max(OutRect.Width(), 1)), float(MarchSize.Y) / float(FMath::Max(OutRect.Height(), 1)),
			-float(OutRect.Min.X) * float(MarchSize.X) / float(FMath::Max(OutRect.Width(), 1)), -float(OutRect.Min.Y) * float(MarchSize.Y) / float(FMath::Max(OutRect.Height(), 1)));
		P->Sprite.MarchSize = FUintVector2(uint32(MarchSize.X), uint32(MarchSize.Y));
		P->DrawArgs = ArgsBuf;
		P->RenderTargets[0] = FRenderTargetBinding(SceneColor.Texture, ERenderTargetLoadAction::ELoad);   // a render-targetable copy (R1)

		TShaderMapRef<FUniSpriteVS> VS(GetGlobalShaderMap(View.GetFeatureLevel()));
		TShaderMapRef<FUniSpritePS> PS(GetGlobalShaderMap(View.GetFeatureLevel()));
		GraphBuilder.AddPass(RDG_EVENT_NAME("Uni.Sprites"), P, ERDGPassFlags::Raster,
			[P, VS, PS, OutRect](FRHICommandList& RHICmdList)
			{
				RHICmdList.SetViewport(float(OutRect.Min.X), float(OutRect.Min.Y), 0.0f, float(OutRect.Max.X), float(OutRect.Max.Y), 1.0f);
				FGraphicsPipelineStateInitializer Pso;
				RHICmdList.ApplyCachedRenderTargets(Pso);
				Pso.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI();
				Pso.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				Pso.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				Pso.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				Pso.BoundShaderState.VertexShaderRHI = VS.GetVertexShader();
				Pso.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
				Pso.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, Pso, 0);
				SetShaderParameters(RHICmdList, VS, VS.GetVertexShader(), P->Sprite);
				SetShaderParameters(RHICmdList, PS, PS.GetPixelShader(), P->Sprite);
				RHICmdList.DrawPrimitiveIndirect(P->DrawArgs->GetIndirectRHICallBuffer(), 0);
			});
		// D-192: the warp motes, the same shaders over UNI_MOTE_COUNT procedural instances, only
		// while a lattice slot has weight (at warp, uu.Warp.Motes on).
		if (F.bCameraStepValid && (F.MoteFrac[0].W > 0.0f || F.MoteFrac[1].W > 0.0f))
		{
			FUniSpritePassParameters* Pm = GraphBuilder.AllocParameters<FUniSpritePassParameters>();
			*Pm = *P;
			Pm->Sprite.MoteMode = 1u;
			GraphBuilder.AddPass(RDG_EVENT_NAME("Uni.WarpMotes"), Pm, ERDGPassFlags::Raster,
				[Pm, VS, PS, OutRect](FRHICommandList& RHICmdList)
				{
					RHICmdList.SetViewport(float(OutRect.Min.X), float(OutRect.Min.Y), 0.0f, float(OutRect.Max.X), float(OutRect.Max.Y), 1.0f);
					FGraphicsPipelineStateInitializer Pso;
					RHICmdList.ApplyCachedRenderTargets(Pso);
					Pso.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI();
					Pso.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
					Pso.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
					Pso.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
					Pso.BoundShaderState.VertexShaderRHI = VS.GetVertexShader();
					Pso.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
					Pso.PrimitiveType = PT_TriangleList;
					SetGraphicsPipelineState(RHICmdList, Pso, 0);
					SetShaderParameters(RHICmdList, VS, VS.GetVertexShader(), Pm->Sprite);
					SetShaderParameters(RHICmdList, PS, PS.GetPixelShader(), Pm->Sprite);
					RHICmdList.DrawPrimitive(0, 2, UniHlsl::UNI_MOTE_COUNT);
				});
		}
	}
	Stats.CellsLastFrame = uint32(F.Cells.Num());
}

// =============================================================================================
// FUniGpuGenBackend
// =============================================================================================

struct FUniGpuGenBackend::FRequest
{
	/** One phase's progress. Separate per phase: a stale probe command must never mark the
	 *  place phase submitted (R3 in the UE review). */
	struct FPhase
	{
		std::atomic<bool> bSubmitted{ false };     // render thread: graph executed, copies enqueued
		std::atomic<bool> bCopied{ false };        // render thread: results copied into the batch
		std::atomic<bool> bCopyEnqueued{ false };  // game thread: copy command sent
		std::atomic<double> StartTime{ 0.0 };      // enqueue time, then the render thread's execution time
	};
	std::shared_ptr<UniCore::FGenBatch> Batch;
	TUniquePtr<FRHIGPUBufferReadback> CountReadback;      // probe: FTaskCountGPU per task
	TUniquePtr<FRHIGPUBufferReadback> AcceptedReadback;   // place: uint per task
	TUniquePtr<FRHIGPUBufferReadback> ObjectReadback;     // place: the read ranges, concatenated
	FPhase Probe;
	FPhase Place;
	bool bFused = false;                                  // D-207: one trip; Place tracks it, counts come back with it
};

FUniGpuGenBackend::FUniGpuGenBackend() {}
FUniGpuGenBackend::~FUniGpuGenBackend()
{
	// In-flight requests hold their own shared pointers; the render commands keep them alive.
	FlushRenderingCommands();
}

// The streamer allocates below the planet records' reserved tail (D-143).
UniCore::u32 FUniGpuGenBackend::PoolCapacity() const { return UniGpuLayout::DynamicPoolBase; }

void FUniGpuGenBackend::SubmitProbe(std::shared_ptr<UniCore::FGenBatch> Batch)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Uni_GenSubmitProbe);
	TSharedPtr<FRequest, ESPMode::ThreadSafe> Req = MakeShared<FRequest, ESPMode::ThreadSafe>();
	Req->Batch = Batch;
	Req->Probe.StartTime = FPlatformTime::Seconds();
	if (!UniShadersEnabled())
	{
		// uu.Shaders=0: nothing to dispatch. The batch fails closed on its first poll.
		Batch->bFailed = true;
		Req->Probe.bCopied = true;
		Requests.Add(Batch->Ticket, Req);
		return;
	}
	// Allocated HERE, on the calling thread, not in the render command (lesson #24).
	Req->CountReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniGenCountReadback"));
	Requests.Add(Batch->Ticket, Req);

	TArray<UniCore::FGenTaskGPU> Tasks;
	Tasks.Append(Batch->Tasks.data(), int32(Batch->Tasks.size()));
	TSharedRef<TArray<UniCore::FGenTaskGPU>, ESPMode::ThreadSafe> Payload = MakeShared<TArray<UniCore::FGenTaskGPU>, ESPMode::ThreadSafe>(MoveTemp(Tasks));
	ENQUEUE_RENDER_COMMAND(UniGenProbe)([Req, Payload](FRHICommandListImmediate& RHICmdList)
	{
		Req->Probe.StartTime = FPlatformTime::Seconds();   // the timeout runs from execution, not from enqueue
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_GenProbe_RT);
		const uint32 NumTasks = uint32(Payload->Num());
		if (NumTasks == 0) { Req->Probe.bSubmitted = true; return; }
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef TaskBuf = UploadStructured(GraphBuilder, TEXT("UniGenTasks"), *Payload);
		FRDGBufferDesc CountDesc = FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::TaskCountBytes, NumTasks);
		CountDesc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef CountBuf = GraphBuilder.CreateBuffer(CountDesc, TEXT("UniGenCounts"));
		FUniGenProbeCS::FParameters* P = GraphBuilder.AllocParameters<FUniGenProbeCS::FParameters>();
		P->InTasks = GraphBuilder.CreateSRV(TaskBuf);
		P->OutCounts = GraphBuilder.CreateUAV(CountBuf);
		P->NumTasks = NumTasks;
		P->UniWebTexture = FUniRenderer::Get().RegisterWebTexture_RenderThread(GraphBuilder);   // D-164
		P->UniWebSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
		P->UniGalWarpFine = FUniRenderer::Get().RegisterGalaxyTexture_RenderThread(GraphBuilder, 0);   // D-179
		P->UniGalWarpLarge = FUniRenderer::Get().RegisterGalaxyTexture_RenderThread(GraphBuilder, 1);
		P->UniGalDetail = FUniRenderer::Get().RegisterGalaxyTexture_RenderThread(GraphBuilder, 2);
		uint32 GroupsX = 1;
		const FIntVector Groups = WrappedGroups(NumTasks, GroupsX);
		P->DispatchGroupsX = GroupsX;
		TShaderMapRef<FUniGenProbeCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Gen.Probe %u tasks", NumTasks), CS, P, Groups);
		AddEnqueueCopyPass(GraphBuilder, Req->CountReadback.Get(), CountBuf, NumTasks * UniGpuLayout::TaskCountBytes);
		GraphBuilder.Execute();
		Req->Probe.bSubmitted = true;
	});
}

void FUniGpuGenBackend::SubmitPlace(std::shared_ptr<UniCore::FGenBatch> Batch)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Uni_GenSubmitPlace);
	TSharedPtr<FRequest, ESPMode::ThreadSafe>* Found = Requests.Find(Batch->Ticket);
	if (!Found) { return; }
	TSharedPtr<FRequest, ESPMode::ThreadSafe> Req = *Found;
	Req->Place.StartTime = FPlatformTime::Seconds();
	Req->AcceptedReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniGenAcceptedReadback"));
	Req->ObjectReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniGenObjectReadback"));

	struct FPlacePayload
	{
		TArray<UniCore::FGenTaskGPU> Tasks;
		TArray<UniCore::FTaskCountGPU> Counts;
		TArray<TPair<uint32, uint32>> Ranges;
		TArray<uint32> WriteIndex;
		TArray<UniCore::FObjectGPU> WriteRecord;
	};
	TSharedRef<FPlacePayload, ESPMode::ThreadSafe> Payload = MakeShared<FPlacePayload, ESPMode::ThreadSafe>();
	Payload->Tasks.Append(Batch->Tasks.data(), int32(Batch->Tasks.size()));
	Payload->Counts.Append(Batch->Counts.data(), int32(Batch->Counts.size()));
	for (const auto& R : Batch->ReadRanges) { Payload->Ranges.Add(TPair<uint32, uint32>(R.first, R.second)); }
	for (const auto& W : Batch->Writes) { Payload->WriteIndex.Add(W.first); Payload->WriteRecord.Add(W.second); }

	ENQUEUE_RENDER_COMMAND(UniGenPlace)([Req, Payload](FRHICommandListImmediate& RHICmdList)
	{
		Req->Place.StartTime = FPlatformTime::Seconds();
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_GenPlace_RT);
		const uint32 NumTasks = uint32(Payload->Tasks.Num());
		FRDGBuilder GraphBuilder(RHICmdList);
		FUniRenderer& R = FUniRenderer::Get();
		FRDGBufferRef PoolBuf = R.RegisterPool_RenderThread(GraphBuilder);
		FRDGBufferRef SliceBuf = R.RegisterSlices_RenderThread(GraphBuilder);
		FRDGBufferRef LutBuf = R.RegisterLut_RenderThread(GraphBuilder);
		FRDGBufferDesc AccDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(NumTasks, 1u));
		AccDesc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef AccBuf = GraphBuilder.CreateBuffer(AccDesc, TEXT("UniGenAccepted"));
		if (NumTasks > 0)
		{
			FUniGenPlaceCS::FParameters* P = GraphBuilder.AllocParameters<FUniGenPlaceCS::FParameters>();
			P->InTasks = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniGenTasks"), Payload->Tasks));
			P->InCounts = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniGenCountsIn"), Payload->Counts));
			P->InSlices = GraphBuilder.CreateSRV(SliceBuf);
			P->InLut = GraphBuilder.CreateSRV(LutBuf);
			P->OutPool = GraphBuilder.CreateUAV(PoolBuf);
			P->OutAccepted = GraphBuilder.CreateUAV(AccBuf);
			P->InAlloc = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniGenAllocNone"), TArray<FUintVector4>()));   // D-207: unused (UseAlloc 0)
			P->UseAlloc = 0u;
			P->NumTasks = NumTasks;
			P->UniWebTexture = R.RegisterWebTexture_RenderThread(GraphBuilder);   // D-164
			P->UniWebSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
			P->UniGalWarpFine = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 0);   // D-179
			P->UniGalWarpLarge = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 1);
			P->UniGalDetail = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 2);
			uint32 GroupsX = 1;
			const FIntVector Groups = WrappedGroups(NumTasks, GroupsX);
			P->DispatchGroupsX = GroupsX;
			TShaderMapRef<FUniGenPlaceCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Gen.Place %u tasks", NumTasks), CS, P, Groups);
		}
		else
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AccBuf), 0u);
		}
		if (Payload->WriteIndex.Num() > 0)
		{
			FUniScatterCS::FParameters* P = GraphBuilder.AllocParameters<FUniScatterCS::FParameters>();
			P->InScatterIndex = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniScatterIndex"), Payload->WriteIndex));
			P->InScatterRecord = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniScatterRecord"), Payload->WriteRecord));
			P->OutPool = GraphBuilder.CreateUAV(PoolBuf);
			P->NumWrites = uint32(Payload->WriteIndex.Num());
			TShaderMapRef<FUniScatterCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Gen.ScatterAnchors"), CS, P, FIntVector(FMath::DivideAndRoundUp(Payload->WriteIndex.Num(), 64), 1, 1));
		}
		// Emitter objects back to the CPU (contributors): copy the ranges into one staging buffer.
		uint32 TotalObjects = 0;
		for (const TPair<uint32, uint32>& Rg : Payload->Ranges) { TotalObjects += Rg.Value; }
		FRDGBufferDesc StageDesc = FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::ObjectBytes, FMath::Max(TotalObjects, 1u));
		StageDesc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef Stage = GraphBuilder.CreateBuffer(StageDesc, TEXT("UniGenReadObjects"));
		uint64 Dst = 0;
		for (const TPair<uint32, uint32>& Rg : Payload->Ranges)
		{
			AddCopyBufferPass(GraphBuilder, Stage, Dst, PoolBuf, uint64(Rg.Key) * UniGpuLayout::ObjectBytes, uint64(Rg.Value) * UniGpuLayout::ObjectBytes);
			Dst += uint64(Rg.Value) * UniGpuLayout::ObjectBytes;
		}
		if (TotalObjects == 0) { AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Stage), 0u); }
		AddEnqueueCopyPass(GraphBuilder, Req->AcceptedReadback.Get(), AccBuf, FMath::Max(NumTasks, 1u) * sizeof(uint32));
		AddEnqueueCopyPass(GraphBuilder, Req->ObjectReadback.Get(), Stage, FMath::Max(TotalObjects, 1u) * UniGpuLayout::ObjectBytes);
		GraphBuilder.Execute();
		Req->Place.bSubmitted = true;
	});
}

void FUniGpuGenBackend::SubmitFused(std::shared_ptr<UniCore::FGenBatch> Batch)
{
	check(IsInGameThread());
	TRACE_CPUPROFILER_EVENT_SCOPE(Uni_GenSubmitFused);
	TSharedPtr<FRequest, ESPMode::ThreadSafe> Req = MakeShared<FRequest, ESPMode::ThreadSafe>();
	Req->Batch = Batch;
	Req->bFused = true;
	Req->Probe.bCopied = true;   // one phase: Place tracks the whole trip
	Req->Place.StartTime = FPlatformTime::Seconds();
	if (!UniShadersEnabled())
	{
		Batch->bFailed = true;
		Req->Place.bCopied = true;
		Requests.Add(Batch->Ticket, Req);
		return;
	}
	// Allocated HERE, on the calling thread, not in the render command (lesson #24).
	Req->CountReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniGenCountReadback"));
	Req->AcceptedReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniGenAcceptedReadback"));
	Requests.Add(Batch->Ticket, Req);

	struct FFusedPayload
	{
		TArray<UniCore::FGenTaskGPU> Tasks;
		uint32 ArenaBase = 0, ArenaCap = 0, MaxPerCell = 0;
	};
	TSharedRef<FFusedPayload, ESPMode::ThreadSafe> Payload = MakeShared<FFusedPayload, ESPMode::ThreadSafe>();
	Payload->Tasks.Append(Batch->Tasks.data(), int32(Batch->Tasks.size()));
	Payload->ArenaBase = Batch->ArenaBase;
	Payload->ArenaCap = Batch->ArenaCap;
	Payload->MaxPerCell = Batch->MaxPerCell;

	ENQUEUE_RENDER_COMMAND(UniGenFused)([Req, Payload](FRHICommandListImmediate& RHICmdList)
	{
		Req->Place.StartTime = FPlatformTime::Seconds();   // the timeout runs from execution, not from enqueue
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_GenFused_RT);
		const uint32 NumTasks = uint32(Payload->Tasks.Num());
		if (NumTasks == 0) { Req->Place.bSubmitted = true; return; }
		FRDGBuilder GraphBuilder(RHICmdList);
		FUniRenderer& R = FUniRenderer::Get();
		FRDGBufferRef TaskBuf = UploadStructured(GraphBuilder, TEXT("UniGenTasks"), Payload->Tasks);
		FRDGBufferDesc CountDesc = FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::TaskCountBytes, NumTasks);
		CountDesc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef CountBuf = GraphBuilder.CreateBuffer(CountDesc, TEXT("UniGenCounts"));
		FRDGBufferRef AllocBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), NumTasks), TEXT("UniGenAlloc"));
		FRDGBufferDesc AccDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumTasks);
		AccDesc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef AccBuf = GraphBuilder.CreateBuffer(AccDesc, TEXT("UniGenAccepted"));
		uint32 GroupsX = 1;
		const FIntVector Groups = WrappedGroups(NumTasks, GroupsX);
		{
			FUniGenProbeCS::FParameters* P = GraphBuilder.AllocParameters<FUniGenProbeCS::FParameters>();
			P->InTasks = GraphBuilder.CreateSRV(TaskBuf);
			P->OutCounts = GraphBuilder.CreateUAV(CountBuf);
			P->NumTasks = NumTasks;
			P->DispatchGroupsX = GroupsX;
			P->UniWebTexture = R.RegisterWebTexture_RenderThread(GraphBuilder);   // D-164
			P->UniWebSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
			P->UniGalWarpFine = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 0);   // D-179
			P->UniGalWarpLarge = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 1);
			P->UniGalDetail = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 2);
			TShaderMapRef<FUniGenProbeCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Gen.Probe %u tasks (one trip)", NumTasks), CS, P, Groups);
		}
		{
			FUniGenAllocCS::FParameters* P = GraphBuilder.AllocParameters<FUniGenAllocCS::FParameters>();
			P->InTasks = GraphBuilder.CreateSRV(TaskBuf);
			P->InCounts = GraphBuilder.CreateSRV(CountBuf);
			P->OutAlloc = GraphBuilder.CreateUAV(AllocBuf);
			P->NumTasks = NumTasks;
			P->DispatchGroupsX = 1u;
			P->ArenaBase = Payload->ArenaBase;
			P->ArenaCap = Payload->ArenaCap;
			P->MaxPerCell = Payload->MaxPerCell;
			TShaderMapRef<FUniGenAllocCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Gen.Alloc %u tasks", NumTasks), CS, P, FIntVector(1, 1, 1));
		}
		{
			FUniGenPlaceCS::FParameters* P = GraphBuilder.AllocParameters<FUniGenPlaceCS::FParameters>();
			P->InTasks = GraphBuilder.CreateSRV(TaskBuf);
			P->InCounts = GraphBuilder.CreateSRV(CountBuf);
			P->InSlices = GraphBuilder.CreateSRV(R.RegisterSlices_RenderThread(GraphBuilder));
			P->InLut = GraphBuilder.CreateSRV(R.RegisterLut_RenderThread(GraphBuilder));
			P->OutPool = GraphBuilder.CreateUAV(R.RegisterPool_RenderThread(GraphBuilder));
			P->OutAccepted = GraphBuilder.CreateUAV(AccBuf);
			P->InAlloc = GraphBuilder.CreateSRV(AllocBuf);
			P->UseAlloc = 1u;
			P->NumTasks = NumTasks;
			P->DispatchGroupsX = GroupsX;
			P->UniWebTexture = R.RegisterWebTexture_RenderThread(GraphBuilder);
			P->UniWebSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
			P->UniGalWarpFine = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 0);
			P->UniGalWarpLarge = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 1);
			P->UniGalDetail = R.RegisterGalaxyTexture_RenderThread(GraphBuilder, 2);
			TShaderMapRef<FUniGenPlaceCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Gen.Place %u tasks (one trip)", NumTasks), CS, P, Groups);
		}
		AddEnqueueCopyPass(GraphBuilder, Req->CountReadback.Get(), CountBuf, NumTasks * UniGpuLayout::TaskCountBytes);
		AddEnqueueCopyPass(GraphBuilder, Req->AcceptedReadback.Get(), AccBuf, NumTasks * sizeof(uint32));
		GraphBuilder.Execute();
		Req->Place.bSubmitted = true;
	});
}

bool FUniGpuGenBackend::PollFused(const UniCore::FGenBatch& Batch)
{
	return Poll(Batch, true);
}

bool FUniGpuGenBackend::Poll(const UniCore::FGenBatch& Batch, bool bPlace)
{
	check(IsInGameThread());
	TSharedPtr<FRequest, ESPMode::ThreadSafe>* Found = Requests.Find(Batch.Ticket);
	if (!Found) { return true; }
	TSharedPtr<FRequest, ESPMode::ThreadSafe> Req = *Found;
	FRequest::FPhase& Ph = bPlace ? Req->Place : Req->Probe;
	if (Ph.bCopied)
	{
		if (bPlace || Req->Batch->bFailed)
		{
			Requests.Remove(Batch.Ticket);
			if (!Req->Batch->bFailed) { FUniRenderer::Get().Stats.GenBatchesCompleted++; }
		}
		return true;
	}
	// Fail closed on timeout (lessons #1, #24, #27): the batch is marked failed and its cells are
	// not drawn; never a CPU substitute. The clock starts when the render thread executes the
	// command, so render-thread latency does not count, with a long cap before that.
	const double Now = FPlatformTime::Seconds();
	const double Limit = Ph.bSubmitted ? UniGpuLayout::ReadbackTimeoutSeconds : 5.0 * UniGpuLayout::ReadbackTimeoutSeconds;
	if (Now - Ph.StartTime.load() > Limit && !Ph.bCopyEnqueued)
	{
		UE_LOG(LogUniRender, Warning, TEXT("UnifiedUniverse: generation batch %llu timed out in the %s phase; its cells fail closed."), Batch.Ticket, bPlace ? TEXT("place") : TEXT("probe"));
		Req->Batch->bFailed = true;
		Ph.bCopied = true;
		FUniRenderer::Get().Stats.GenBatchesFailed++;
		return Poll(Batch, bPlace);
	}
	if (!Ph.bSubmitted || Ph.bCopyEnqueued) { return false; }
	if (!bPlace && Req->Batch->Tasks.empty()) { Req->Batch->Counts.clear(); Ph.bCopied = true; return true; }
	FRHIGPUBufferReadback* RbA = bPlace ? Req->AcceptedReadback.Get() : Req->CountReadback.Get();
	FRHIGPUBufferReadback* RbB = bPlace ? (Req->bFused ? Req->CountReadback.Get() : Req->ObjectReadback.Get()) : nullptr;   // D-207: a one trip brings its counts back with it
	if (!RbA->IsReady() || (RbB && !RbB->IsReady())) { return false; }
	// Lock() is render-thread only (lesson #24): marshal the copy there.
	Ph.bCopyEnqueued = true;
	ENQUEUE_RENDER_COMMAND(UniGenCopy)([Req, bPlace](FRHICommandListImmediate&)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Uni_GenCopy_RT);
		std::shared_ptr<UniCore::FGenBatch> B = Req->Batch;
		const size_t N = B->Tasks.size();
		if (!bPlace)
		{
			B->Counts.resize(N);
			if (N)
			{
				const void* Src = Req->CountReadback->Lock(uint32(N * sizeof(UniCore::FTaskCountGPU)));
				FMemory::Memcpy(B->Counts.data(), Src, N * sizeof(UniCore::FTaskCountGPU));
				Req->CountReadback->Unlock();
			}
			Req->Probe.bCopied = true;
		}
		else
		{
			B->Accepted.resize(N);
			if (N)
			{
				const void* Src = Req->AcceptedReadback->Lock(uint32(N * sizeof(uint32)));
				FMemory::Memcpy(B->Accepted.data(), Src, N * sizeof(uint32));
				Req->AcceptedReadback->Unlock();
			}
			if (Req->bFused)
			{
				// D-207: the counts (the streamer recomputes the ranges from them, FuseAllocate).
				B->Counts.resize(N);
				if (N)
				{
					const void* Src = Req->CountReadback->Lock(uint32(N * sizeof(UniCore::FTaskCountGPU)));
					FMemory::Memcpy(B->Counts.data(), Src, N * sizeof(UniCore::FTaskCountGPU));
					Req->CountReadback->Unlock();
				}
				Req->Place.bCopied = true;
				return;
			}
			size_t Total = 0;
			for (const auto& R : B->ReadRanges) { Total += R.second; }
			B->ReadObjects.resize(Total);
			if (Total)
			{
				const void* Src = Req->ObjectReadback->Lock(uint32(Total * sizeof(UniCore::FObjectGPU)));
				FMemory::Memcpy(B->ReadObjects.data(), Src, Total * sizeof(UniCore::FObjectGPU));
				Req->ObjectReadback->Unlock();
			}
			Req->Place.bCopied = true;
		}
	});
	return false;
}

bool FUniGpuGenBackend::PollProbe(const UniCore::FGenBatch& Batch)
{
	const bool bDone = Poll(Batch, false);
	return bDone;
}

bool FUniGpuGenBackend::PollPlace(const UniCore::FGenBatch& Batch)
{
	return Poll(Batch, true);
}
