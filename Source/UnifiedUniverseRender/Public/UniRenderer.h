#pragma once

// UniRenderer.h
// The render module's plain-C++ face (no UObjects: the module loads at PostConfigInit).
//
//   FUniGpuGenBackend   UniCore::IGenBackend on the GPU: the probe and place passes of
//                       UniGen.usf, with UltraLargeScale's request discipline (lessons #24, #25):
//                       readbacks allocated on the calling (game) thread, render commands
//                       enqueued from the game thread with one by-value payload, Lock() only on
//                       the render thread, a 2 s timeout that fails closed.
//   FUniRenderer        per-frame state pushed by the game thread (cell table, camera fraction,
//                       sensitivity, exposure) and consumed by the view extension
//                       (UniRenderHooks.cpp), plus the persistent object pool.
//
// Game thread: SetFrame_GameThread, the backend's Submit*/Poll*.
// Render thread: everything named *_RenderThread.

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "RenderGraphResources.h"
#include "RendererInterface.h"   // IPooledRenderTarget
#include "UniStreamer.h"
#include "UniQuery.h"   // D-193
#include "UniVolume.h"

#include <atomic>
#include <memory>

class FRDGBuilder;
class FSceneView;
struct FScreenPassTexture;
struct FScreenPassRenderTarget;

/** What the game thread hands the renderer each frame (one by-value payload, lesson #25). */
struct FUniFrameData
{
	TArray<UniCore::FCellGPU> Cells;
	FVector3f CameraFraction = FVector3f::ZeroVector;
	float DeltaMag = 0.0f;
	float ExposureBias = 0.0f;     // magnitudes added to m_exposure (brighter < 0)
	float SceneColourScale = 1.0f; // exposure units -> scene colour (lesson #36), tuned in UE
	bool bEnabled = false;
	uint64 FrameNumber = 0;
	// D-184: the camera's move since the previous frame (cm, now minus then), for the volume's
	// temporal reprojection; not valid across a jump (teleport, goto, uu.Enable).
	FVector3f CameraStepCm = FVector3f::ZeroVector;
	bool bCameraStepValid = false;
	float WarpAmount = 0.0f;   // D-187: 0 at rest, 1 at full warp (the travel curve), drives streaks and arrival
	float SpeedAmount = 0.0f;  // D-198: the travel curve itself (not x uu.Warp): the volume march's budget falls with it
	float VolumeBudgetScale = 1.0f;   // D-200: the auto budget's scale of the march pixel budget (1: off)
	float VolumeNearCm = 0.0f;        // D-208: the nearest marched structure's distance (FVolumeScene::NearCm; 0: unknown)
	// D-212 / D-214: the galaxy map array (UniCore::FGalaxyMapCache): its slots and texels a side
	// (a change of either, or of Config, recreates it), the bakes the cache waits on (a slot, the
	// epoch its contents belong to, the light's and dust's parameters), the texels the GPU may
	// bake a frame, and the epoch the volume's map slot must hold (VolumeView.MapSlot; 0: none).
	// The renderer bakes them over frames and reports each slot's epoch when done.
	struct FGalMapBake
	{
		uint32 Slot = 0;
		uint32 Epoch = 0;
		uint32 bDust = 0;
		UniCore::FFieldParamsGPU Params[2] = {};
	};
	uint32 GalMapSlots = 0;
	uint32 GalMapSize = 0;
	uint32 GalMapConfig = 0;
	uint32 GalMapBudget = 0;
	uint32 GalMapVolumeEpoch = 0;
	TArray<FGalMapBake> GalMapBakes;
	// D-192: warp ambience motes (UniCore::MoteLattice): per lattice slot the camera's cell (w: the
	// level) and its fraction in it (w: the slot's weight, 0: none); cell sizes (cm) and the
	// motes' flux (exposure units) in MoteScale (x, y, z).
	FUintVector4 MoteCell[2] = { FUintVector4(0, 0, 0, 0), FUintVector4(0, 0, 0, 0) };
	FVector4f MoteFrac[2] = { FVector4f(0.0f, 0.0f, 0.0f, 0.0f), FVector4f(0.0f, 0.0f, 0.0f, 0.0f) };
	FVector4f MoteScale = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);

	// D-193: at most one pick and one resident query per frame (the subsystem queues the rest).
	// The pick scores this frame's sprites under a cursor; the query walks this frame's cell
	// table. Results come back a few frames later (DrainPicks / DrainQueries), named by
	// (cell index in this frame's table, slot).
	int32 PickRequestId = 0;                            // 0: none
	FVector2f PickCursorNdc = FVector2f::ZeroVector;    // [-1, 1] across the view, y up
	float PickMinPx = 4.0f;
	int32 QueryRequestId = 0;                           // 0: none
	uint32 QueryMode = 0u;                              // UniHlsl::UNI_QUERY_MODE_SPHERE or _NEAREST
	FUintVector4 QueryDelta[3] = { FUintVector4(0, 0, 0, 0), FUintVector4(0, 0, 0, 0), FUintVector4(0, 0, 0, 0) };   // centre - camera, exact
	float QueryRadiusCm = 0.0f;
	uint32 QueryArchMask = 0u;                          // 0: every archetype
	float QueryMaxAbsMag = 99.0f;
	uint32 QueryMaxResults = 256u;                      // at most UniGpuLayout::MaxQueryResults

	// Star systems (D-142, D-143). Planet records for the reserved pool tail
	// (UniGpuLayout::DynamicPoolBase..), rewritten every frame, and the archetype mask the
	// project pass tests for system activation. Their dynamic cells are appended to Cells.
	TArray<UniCore::FObjectGPU> DynamicRecords;
	uint32 SystemArchetypeMask = 0;
	// D-183: archetypes (id < 32) whose sprites are inclined ellipses (disc galaxies); 0: all round.
	uint32 DiscArchetypeMask = 0;
	uint32 FixedDiscSeeds[4] = { 0, 0, 0, 0 };   // D-183: fixed-orientation anchors' seeds (their disc normal is world z)

	// Volume pass (Docs/VOLUMETRICS.md, UniCore::FVolumeBuilder). Local carriers are marched per
	// pixel; background (periodic) carriers into the direction map whenever BackgroundVersion
	// changes (D-136). Parameters and LUT are shared by both.
	bool bVolume = false;
	TArray<UniCore::FCarrierGPU> VolumeCarriers;
	TArray<UniCore::FCarrierGPU> BackgroundCarriers;
	TArray<UniCore::FFieldParamsGPU> VolumeParams;
	TArray<FVector4f> VolumeLut;
	UniCore::FVolumeViewGPU VolumeView{};
	float BackgroundStartCm = 3.0856775e23f;
	uint32 BackgroundVersion = 0;
	/** D-196: each local carrier's parent object as (cell index in Cells, slot, share bits, 0),
	 *  sorted by (cell index, slot): its sprite draws 1 - share. Objects not listed keep their
	 *  whole sprite (empty with the volume off). */
	TArray<FUintVector4> CarrierHandoffs;
};

/** A star the project pass found inside its system radius (D-142), as read back. CellIndex
 *  indexes the cell table of frame FrameNumber. */
struct FUniActivation
{
	uint64 FrameNumber = 0;
	uint32 CellIndex = 0;
	uint32 ObjectIndex = 0;
	UniCore::FObjectGPU Record{};
};

/** D-193: a pick as read back. CellIndex indexes the cell table of frame FrameNumber. */
struct FUniPickReadback
{
	uint64 FrameNumber = 0;
	int32 RequestId = 0;
	bool bFound = false;
	uint32 CellIndex = 0;
	uint32 Slot = 0;
	float Score = 0.0f;
	UniCore::FObjectGPU Record{};
};

/** D-193: a resident query as read back: Total matches (Hits holds at most the request's
 *  maximum, unordered), each named by (cell index in frame FrameNumber's table, slot). */
struct FUniQueryReadback
{
	uint64 FrameNumber = 0;
	int32 RequestId = 0;
	uint32 Total = 0;
	TArray<UniCore::FQueryHit> Hits;
};

struct FUniRenderStats
{
	std::atomic<uint32> SpritesLastFrame{ 0 };
	std::atomic<uint32> PromoteCandidatesLastFrame{ 0 };
	std::atomic<uint32> CellsLastFrame{ 0 };
	std::atomic<uint32> GenBatchesCompleted{ 0 };
	std::atomic<uint32> GenBatchesFailed{ 0 };
	std::atomic<uint32> VolumeCarriersLastFrame{ 0 };
	std::atomic<uint32> BackgroundFacesMarched{ 0 };
	std::atomic<uint32> BackgroundPerPixelFrames{ 0 };   // D-196: frames the background was marched per pixel
	std::atomic<uint32> VolumeMarchWidth{ 0 };           // D-196: the last frame's march size and rays
	std::atomic<uint32> VolumeMarchHeight{ 0 };
	std::atomic<uint32> VolumeMarchRays{ 0 };
	std::atomic<uint32> VolumeMarchFlags{ 0 };           // D-196: 1 per-pixel background, 2 sliced; D-209: bits 8..15 the slicing's N
	/** D-209: volume frames by slicing since the last uu.Perf (the game thread swaps them to 0):
	 *  [2..4] sliced N x N; not sliced: [5] temporal off or a debug view, [6] no history, [7] march
	 *  size changed, [8] camera jump, [9] the same frame rendered again, [10] frames skipped. */
	static constexpr int32 NumSliceCounts = 11;
	std::atomic<uint32> VolumeSliceCounts[NumSliceCounts] = {};
	/** D-214: per slot of the galaxy map array, the epoch whose bake is done (0: none). */
	static constexpr int32 MaxGalMapSlots = 16;
	std::atomic<uint32> GalMapBaked[MaxGalMapSlots] = {};
	std::atomic<uint32> GalMapBakeFrames{ 0 };       // frames the GPU spent baking maps (total)
	std::atomic<uint32> ActivationsLastReadback{ 0 };
	/** The last rendered view's angular pixel size (rad): the game thread builds the next
	 *  frame's carriers with it (one frame late, harmless: it only sets handoff weights). */
	std::atomic<float> ThetaPixelLastFrame{ 1.0f / 960.0f };
};

class UNIFIEDUNIVERSERENDER_API FUniRenderer
{
public:
	static FUniRenderer& Get();

	/** False when uu.Shaders=0 (nothing compiled): every pass is skipped. */
	static bool ShadersEnabled();

	/** TEST ONLY (UnifiedUniverse.Render.GpuParity): copies the first Num pool records to the CPU,
	 *  blocking the calling (game) thread for up to TimeoutSeconds. Never call at runtime
	 *  (lesson #24). */
	bool ReadbackPoolBlocking_GameThread(TArray<UniCore::FObjectGPU>& Out, uint32 Num, double TimeoutSeconds = 10.0);

	/** Game thread. Copies the payload to the render thread. */
	void SetFrame_GameThread(FUniFrameData&& Data);

	/** Game thread: system activations read back since the last call (D-142). */
	void DrainActivations_GameThread(TArray<FUniActivation>& Out);

	/** Game thread (D-193): picks and resident queries read back since the last call. */
	void DrainPicks_GameThread(TArray<FUniPickReadback>& Out);
	void DrainQueries_GameThread(TArray<FUniQueryReadback>& Out);

	/** First pool slot of the planet records (D-143); the generation pool ends below it. */
	static uint32 DynamicPoolBase();
	static uint32 DynamicPoolSlots();
	/** D-193: the most hits one resident query reads back. */
	static uint32 MaxQueryResults();

	/** Game thread. Slice table and LUTs, once per universe definition. */
	void SetTables_GameThread(TArray<UniCore::FSliceGPU> Slices, TArray<float> Lut);

	/** Game thread (D-164): the web texture's texels, every mip (Size >> m)^3, x fastest, as
	 *  UniCore::FWebTexture holds them: RGBA16F packed two halves per uint (R | G << 16,
	 *  B | A << 16). Uploaded on the render thread before the next pass that samples it. Size 0
	 *  clears it (passes then bind a 1^3 NaN dummy, so a textured web that is sampled without its
	 *  texture places and draws nothing). */
	void SetWebTexture_GameThread(uint32 Size, TArray<TArray<FUintVector2>> Mips);

	/** Render thread (D-164): the web texture registered into a graph (uploaded on first use
	 *  after SetWebTexture), or the NaN dummy. */
	FRDGTextureRef RegisterWebTexture_RenderThread(FRDGBuilder& GraphBuilder);

	/** Game thread (D-179): one galaxy texture (Slot 0 warp_fine, 1 warp_large, 2 galaxy_detail),
	 *  every mip, packed as SetWebTexture's. Size 0 clears it (the NaN dummy is bound). */
	void SetGalaxyTexture_GameThread(int32 Slot, uint32 Size, TArray<TArray<FUintVector2>> Mips);


	/** Render thread (D-179): galaxy texture Slot registered into a graph (uploaded on first use
	 *  after SetGalaxyTexture), or the NaN dummy. */
	FRDGTextureRef RegisterGalaxyTexture_RenderThread(FRDGBuilder& GraphBuilder, int32 Slot);
	FRDGTextureRef RegisterGalaxyMap_RenderThread(FRDGBuilder& GraphBuilder);   // D-212 / D-214: the map array (a 1x1x1 zero dummy without one)

	/** Render thread: draws the backdrop additively into Output (a render-targetable copy of
	 *  scene colour, made by the view extension). Called from the view extension. */
	void RenderBackdrop_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output,
		FRDGTextureRef SceneDepth, const FIntRect& DepthViewRect, float PreExposure);

	/** Render thread: drop every GPU resource (engine pre-exit, before RHI shutdown). */
	void Release_RenderThread();

	/** Render thread: the persistent pool, registered into a graph (created on first use). */
	FRDGBufferRef RegisterPool_RenderThread(FRDGBuilder& GraphBuilder);

	FRDGBufferRef RegisterSlices_RenderThread(FRDGBuilder& GraphBuilder);
	FRDGBufferRef RegisterLut_RenderThread(FRDGBuilder& GraphBuilder);

	FUniRenderStats Stats;

	/** Render-thread copy of the latest frame payload. */
	FUniFrameData RenderFrame;

private:
	TRefCountPtr<FRDGPooledBuffer> Pool;
	TArray<UniCore::FSliceGPU> SlicesRT;
	TArray<float> LutRT;
	TRefCountPtr<FRDGPooledBuffer> SlicesBuffer;
	TRefCountPtr<FRDGPooledBuffer> LutBuffer;
	bool bTablesDirty = true;

	// D-164: the web texture (render thread).
	TArray<TArray<FUintVector2>> WebMipsRT;
	uint32 WebSizeRT = 0;
	bool bWebDirty = false;
	TRefCountPtr<IPooledRenderTarget> WebTexture;
	TRefCountPtr<IPooledRenderTarget> WebDummy;

	// D-179: the galaxy textures (render thread), by slot.
	static constexpr int32 GalaxyTextureSlots = 3;
	TArray<TArray<FUintVector2>> GalMipsRT[GalaxyTextureSlots];
	uint32 GalSizeRT[GalaxyTextureSlots] = { 0, 0, 0 };
	bool bGalDirty[GalaxyTextureSlots] = { false, false, false };
	// D-212 / D-214: the galaxy map array (render thread): per slot the bake under way (its epoch,
	// rows done, whether complete).
	struct FGalMapSlotRT
	{
		uint32 Epoch = 0;
		uint32 RowsDone = 0;
		bool bComplete = false;
		bool bDust = false;
		TArray<UniCore::FFieldParamsGPU> Params;
	};
	TRefCountPtr<IPooledRenderTarget> GalMapTexture, GalMapDummy;
	uint32 GalMapSlotsRT = 0, GalMapSizeRT = 0, GalMapConfigRT = 0, GalMapBudgetRT = 0;
	TArray<FGalMapSlotRT> GalMapSlotRT;
	void QueueGalaxyMap_RenderThread(const FUniFrameData& F);
	/** D-214: bakes the queued slots' rows (the frame's texel budget), in their own graph, every
	 *  frame whatever renders: generation waits on them. */
	void BakeGalaxyMaps_RenderThread(FRHICommandListImmediate& RHICmdList);
	/** D-214: is Slot's bake of Epoch complete? */
	bool GalaxyMapComplete_RenderThread(uint32 Slot, uint32 Epoch) const { return int32(Slot) < GalMapSlotRT.Num() && GalMapSlotRT[Slot].bComplete && GalMapSlotRT[Slot].Epoch == Epoch && Epoch != 0u; }
	TRefCountPtr<IPooledRenderTarget> GalTexture[GalaxyTextureSlots];

	/** The 1^3 NaN dummy any unbound field texture reads (fail closed). */
	FRDGTextureRef RegisterNanDummy_RenderThread(FRDGBuilder& GraphBuilder);

	// Volume: the background direction map persists across frames (D-136). D-159: triple
	// buffered. A refresh marches all six faces of the BACK map from one snapshot of the
	// background's carriers, parameters, LUT and view (a few faces per frame), then rotates it to
	// CURRENT; the composite cross-fades from PREVIOUS to CURRENT over the next refresh's length.
	// Before, a refresh restarted whenever the background changed (every frame at speed), so the
	// first faces were re-marched forever and the rest kept stale light: one face brighter than
	// its neighbours, an "inside a cube" read.
	TRefCountPtr<IPooledRenderTarget> BackgroundMaps[3];
	// D-184: temporal slicing of the march. The previous frame's reconstructed output (radiance +
	// transmittance, slices, light depth + scene depth) and its view, for the reprojection.
	TRefCountPtr<IPooledRenderTarget> TemporalVolume, TemporalSlices, TemporalDepth;
	// D-223 amendment: the splat path's deposit buffers and tile marks persist (zeroed where each
	// frame wrote them, SplatZeroCS), recreated cleared when the buffers' size changes.
	TRefCountPtr<FRDGPooledBuffer> SplatLumBuffer, SplatColBuffer, SplatTileBuffer;
	FUintVector2 SplatBufferSize = FUintVector2(0u, 0u);
	FIntPoint TemporalSize = FIntPoint(0, 0);
	FVector3f TemporalPrevForward = FVector3f::ZeroVector, TemporalPrevRight = FVector3f::ZeroVector, TemporalPrevUp = FVector3f::ZeroVector;
	float TemporalPrevTanX = 1.0f, TemporalPrevTanY = 1.0f;
	uint64 TemporalLastFrame = 0;
	FVector4f TemporalPrevWarp = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);   // D-190: the history's warp optics
	uint32 TemporalPhase = 0;          // the packed phase (D-208: step | N << 8)
	uint32 TemporalStep = 0;           // D-208: counts sliced frames
	uint32 TemporalInterleave = 2;     // D-208: the last frame's N (for uu.perf)
	bool bTemporalHave = false;
	int32 BgPrev = 0, BgCur = 1, BgBack = 2;
	bool bBgHaveCur = false;
	bool bBgFilling = false;
	uint32 BgFillMask = 0u;
	uint32 BgFillVersion = 0u;
	uint32 BgFilledVersion = ~0u;
	int32 BgBlendFrames = 0;
	uint32 BgLastVersion = ~0u;   // D-166: frames since the background last changed
	int32 BgStillFrames = 0;
	int32 BgDirtyInterval = 1 << 30;   // D-196: frames between the background's last two changes
	bool bBgPerPixel = false;          // D-196: marching the background per pixel (hysteresis)
	TArray<UniCore::FCarrierGPU> BgSnapCarriers;
	TArray<UniCore::FFieldParamsGPU> BgSnapParams;
	TArray<FVector4f> BgSnapLut;
	UniCore::FVolumeViewGPU BgSnapView{};
	void RenderVolume_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output,
		FRDGTextureRef SceneDepth, const FIntRect& DepthViewRect, float PreExposure, FRDGTextureRef& OutSlices, FIntPoint& OutMarchSize);

	// Promotion readbacks, a small ring so the render thread never waits on the GPU.
	struct FPromoteReadback;
	TArray<TSharedPtr<FPromoteReadback>> PromoteRing;
	int32 PromoteRingNext = 0;

	// System activations (D-142): a ring of readbacks, results handed to the game thread.
	struct FActivateReadback;
	TArray<TSharedPtr<FActivateReadback>> ActivateRing;
	int32 ActivateRingNext = 0;
	FCriticalSection ActivationLock;
	TArray<FUniActivation> ActivationsReady;

	// D-193: picks and resident queries, one of each per frame, read back through rings.
	struct FPickReadback;
	struct FQueryReadback;
	TArray<TSharedPtr<FPickReadback>> PickRing;
	TArray<TSharedPtr<FQueryReadback>> QueryRing;
	uint64 LastPickFrame = 0;    // render thread: a frame's request runs once (the first view that draws it)
	uint64 LastQueryFrame = 0;
	FCriticalSection ApiLock;
	TArray<FUniPickReadback> PicksReady;
	TArray<FUniQueryReadback> QueriesReady;
	void CollectApiReadbacks_RenderThread();
	void AddQuery_RenderThread(FRDGBuilder& GraphBuilder, const FUniFrameData& F, ERHIFeatureLevel::Type FeatureLevel, FRDGBufferRef CellBuf, FRDGBufferRef PoolBuf);
};

/** GPU generation backend for UniCore::FUniStreamer. Game thread only. */
class UNIFIEDUNIVERSERENDER_API FUniGpuGenBackend : public UniCore::IGenBackend
{
public:
	FUniGpuGenBackend();
	virtual ~FUniGpuGenBackend() override;

	virtual void SubmitProbe(std::shared_ptr<UniCore::FGenBatch> Batch) override;
	virtual bool PollProbe(const UniCore::FGenBatch& Batch) override;
	virtual void SubmitPlace(std::shared_ptr<UniCore::FGenBatch> Batch) override;
	virtual bool PollPlace(const UniCore::FGenBatch& Batch) override;
	virtual UniCore::u32 PoolCapacity() const override;
	/** D-207 (B-020): one-trip batches: probe, allocation (UniGen.usf pass 3) and place in one
	 *  graph; the counts and accepted counts are read back. */
	virtual bool SupportsFused() const override { return true; }
	virtual void SubmitFused(std::shared_ptr<UniCore::FGenBatch> Batch) override;
	virtual bool PollFused(const UniCore::FGenBatch& Batch) override;

private:
	struct FRequest;
	TMap<uint64, TSharedPtr<FRequest, ESPMode::ThreadSafe>> Requests;
	bool Poll(const UniCore::FGenBatch& Batch, bool bPlace);
};
