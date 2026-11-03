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

	// Star systems (D-142, D-143). Planet records for the reserved pool tail
	// (UniGpuLayout::DynamicPoolBase..), rewritten every frame, and the archetype mask the
	// project pass tests for system activation. Their dynamic cells are appended to Cells.
	TArray<UniCore::FObjectGPU> DynamicRecords;
	uint32 SystemArchetypeMask = 0;

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

struct FUniRenderStats
{
	std::atomic<uint32> SpritesLastFrame{ 0 };
	std::atomic<uint32> PromoteCandidatesLastFrame{ 0 };
	std::atomic<uint32> CellsLastFrame{ 0 };
	std::atomic<uint32> GenBatchesCompleted{ 0 };
	std::atomic<uint32> GenBatchesFailed{ 0 };
	std::atomic<uint32> VolumeCarriersLastFrame{ 0 };
	std::atomic<uint32> BackgroundFacesMarched{ 0 };
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

	/** First pool slot of the planet records (D-143); the generation pool ends below it. */
	static uint32 DynamicPoolBase();
	static uint32 DynamicPoolSlots();

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

	// Volume: the background direction map persists across frames (D-136). D-159: triple
	// buffered. A refresh marches all six faces of the BACK map from one snapshot of the
	// background's carriers, parameters, LUT and view (a few faces per frame), then rotates it to
	// CURRENT; the composite cross-fades from PREVIOUS to CURRENT over the next refresh's length.
	// Before, a refresh restarted whenever the background changed (every frame at speed), so the
	// first faces were re-marched forever and the rest kept stale light: one face brighter than
	// its neighbours, an "inside a cube" read.
	TRefCountPtr<IPooledRenderTarget> BackgroundMaps[3];
	int32 BgPrev = 0, BgCur = 1, BgBack = 2;
	bool bBgHaveCur = false;
	bool bBgFilling = false;
	uint32 BgFillMask = 0u;
	uint32 BgFillVersion = 0u;
	uint32 BgFilledVersion = ~0u;
	int32 BgBlendFrames = 0;
	uint32 BgLastVersion = ~0u;   // D-166: frames since the background last changed
	int32 BgStillFrames = 0;
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

private:
	struct FRequest;
	TMap<uint64, TSharedPtr<FRequest, ESPMode::ThreadSafe>> Requests;
	bool Poll(const UniCore::FGenBatch& Batch, bool bPlace);
};
