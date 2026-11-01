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
#include "RenderGraphResources.h"
#include "UniStreamer.h"

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
};

struct FUniRenderStats
{
	std::atomic<uint32> SpritesLastFrame{ 0 };
	std::atomic<uint32> PromoteCandidatesLastFrame{ 0 };
	std::atomic<uint32> CellsLastFrame{ 0 };
	std::atomic<uint32> GenBatchesCompleted{ 0 };
	std::atomic<uint32> GenBatchesFailed{ 0 };
};

class UNIFIEDUNIVERSERENDER_API FUniRenderer
{
public:
	static FUniRenderer& Get();

	/** Game thread. Copies the payload to the render thread. */
	void SetFrame_GameThread(FUniFrameData&& Data);

	/** Game thread. Slice table and LUTs, once per universe definition. */
	void SetTables_GameThread(TArray<UniCore::FSliceGPU> Slices, TArray<float> Lut);

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

	// Promotion readbacks, a small ring so the render thread never waits on the GPU.
	struct FPromoteReadback;
	TArray<TSharedPtr<FPromoteReadback>> PromoteRing;
	int32 PromoteRingNext = 0;
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
