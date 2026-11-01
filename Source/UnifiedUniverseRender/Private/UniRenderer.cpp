#include "UniRenderer.h"

#include "UnifiedUniverseRender.h"
#include "UniShaders.h"

#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "CommonRenderResources.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<float> CVarUniSpriteExtent(TEXT("uu.SpriteExtent"), 3.0f, TEXT("Sprite quad half size in Gaussian sigmas."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniGlare(TEXT("uu.GlareGain"), 0.35f, TEXT("Sprite footprint growth with flux."), ECVF_RenderThreadSafe);

namespace
{
	FIntVector WrappedGroups(uint32 Count, uint32& OutGroupsX)
	{
		// Lesson #23: at most 65,535 groups per dimension; wrap into 2D.
		const int32 MaxX = FMath::Max(GRHIGlobals.MaxDispatchThreadGroupsPerDimension.X, 1);
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
	bool bPending = false;
};

FUniRenderer& FUniRenderer::Get()
{
	static FUniRenderer Instance;
	return Instance;
}

void FUniRenderer::SetFrame_GameThread(FUniFrameData&& Data)
{
	check(IsInGameThread());
	// One payload, moved once into a shared holder, captured by value (lesson #25).
	TSharedRef<FUniFrameData, ESPMode::ThreadSafe> Payload = MakeShared<FUniFrameData, ESPMode::ThreadSafe>(MoveTemp(Data));
	ENQUEUE_RENDER_COMMAND(UniSetFrame)([Payload](FRHICommandListImmediate&)
	{
		FUniRenderer::Get().RenderFrame = *Payload;
	});
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

void FUniRenderer::RenderBackdrop_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassTexture& SceneColor,
	FRDGTextureRef SceneDepth, const FIntRect& DepthViewRect)
{
	check(IsInRenderingThread());
	const FUniFrameData& F = RenderFrame;
	if (!F.bEnabled || F.Cells.Num() == 0 || !SceneColor.IsValid() || !SceneDepth) { return; }
	RDG_EVENT_SCOPE(GraphBuilder, "UnifiedUniverse");

	FRDGBufferRef PoolBuf = RegisterPool_RenderThread(GraphBuilder);
	FRDGBufferRef CellBuf = UploadStructured(GraphBuilder, TEXT("UniCells"), F.Cells);

	FRDGBufferRef SpriteBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::SpriteBytes, UniGpuLayout::MaxSprites), TEXT("UniSprites"));
	FRDGBufferRef ArgsBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndirectParameters>(1), TEXT("UniSpriteArgs"));
	FRDGBufferRef PromoteBuf = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), UniGpuLayout::MaxPromote), TEXT("UniPromote"));
	FRDGBufferDesc PromoteCountDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1);
	PromoteCountDesc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef PromoteCountBuf = GraphBuilder.CreateBuffer(PromoteCountDesc, TEXT("UniPromoteCount"));

	FRDGBufferUAVRef ArgsUAV = GraphBuilder.CreateUAV(ArgsBuf, PF_R32_UINT);
	FRDGBufferUAVRef PromoteCountUAV = GraphBuilder.CreateUAV(PromoteCountBuf, PF_R32_UINT);
	AddClearUAVPass(GraphBuilder, ArgsUAV, 0u);          // the shader writes args[0] = 6
	AddClearUAVPass(GraphBuilder, PromoteCountUAV, 0u);

	// View basis and projection from the engine view. Universe axes are UE world axes (cm), and
	// camera-relative render positions are translated-world positions (PreViewTranslation = -origin).
	const FVector3f Forward(View.GetViewDirection());
	const FVector3f Right(View.GetViewRight());
	const FVector3f Up(View.GetViewUp());
	const FMatrix& Proj = View.ViewMatrices.GetProjectionMatrix();
	const float TanX = 1.0f / float(Proj.M[0][0]);
	const float TanY = 1.0f / float(Proj.M[1][1]);
	const FIntRect OutRect = SceneColor.ViewRect;
	const float PixelsPerRadian = float(OutRect.Width()) * 0.5f / TanX;
	const float LimitingMag = 6.5f + FMath::Clamp(F.DeltaMag, -5.0f, 5.0f);   // UNI_M_REF + dm (UniPhotometry.ush)

	{
		FUniProjectCS::FParameters* P = GraphBuilder.AllocParameters<FUniProjectCS::FParameters>();
		P->InCells = GraphBuilder.CreateSRV(CellBuf);
		P->InPool = GraphBuilder.CreateSRV(PoolBuf);
		P->OutSprites = GraphBuilder.CreateUAV(SpriteBuf);
		P->OutDrawArgs = ArgsUAV;
		P->OutPromote = GraphBuilder.CreateUAV(PromoteBuf);
		P->OutPromoteCount = PromoteCountUAV;
		P->NumCells = uint32(F.Cells.Num());
		P->MaxSprites = UniGpuLayout::MaxSprites;
		P->MaxPromote = UniGpuLayout::MaxPromote;
		P->ViewForward = Forward; P->ViewRight = Right; P->ViewUp = Up;
		P->TanHalfFovX = TanX; P->TanHalfFovY = TanY;
		P->LimitingMag = LimitingMag;
		P->CameraFraction = F.CameraFraction;
		P->ExposureMag = LimitingMag - 1.0f + F.ExposureBias;
		P->SurfaceLimit = 25.0f + FMath::Clamp(F.DeltaMag, -5.0f, 5.0f);         // UNI_MU_REF + dm
		P->ThetaPixel = 1.0f / PixelsPerRadian;
		P->GlareGain = CVarUniGlare.GetValueOnRenderThread();
		uint32 GroupsX = 1;
		const FIntVector Groups = WrappedGroups(uint32(F.Cells.Num()), GroupsX);
		P->DispatchGroupsX = GroupsX;
		TShaderMapRef<FUniProjectCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.ProjectAndCull %d cells", F.Cells.Num()), CS, P, Groups);
	}

	// Promotion candidates: count readback (ring of 3, never waited on).
	if (PromoteRing.Num() == 0) { for (int32 I = 0; I < 3; ++I) { PromoteRing.Add(MakeShared<FPromoteReadback>()); } }
	for (const TSharedPtr<FPromoteReadback>& R : PromoteRing)
	{
		if (R->bPending && R->Count->IsReady())
		{
			const uint32* Ptr = static_cast<const uint32*>(R->Count->Lock(sizeof(uint32)));   // render thread only (lesson #24)
			Stats.PromoteCandidatesLastFrame = Ptr ? *Ptr : 0u;
			R->Count->Unlock();
			R->bPending = false;
		}
	}
	{
		TSharedPtr<FPromoteReadback>& R = PromoteRing[PromoteRingNext];
		if (!R->bPending)
		{
			if (!R->Count.IsValid()) { R->Count = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniPromoteCountReadback")); }
			AddEnqueueCopyPass(GraphBuilder, R->Count.Get(), PromoteCountBuf, sizeof(uint32));
			R->bPending = true;
			PromoteRingNext = (PromoteRingNext + 1) % PromoteRing.Num();
		}
	}

	// Sprites, additive, into scene colour, tested against scene depth.
	{
		FUniSpritePassParameters* P = GraphBuilder.AllocParameters<FUniSpritePassParameters>();
		P->Sprite.InSprites = GraphBuilder.CreateSRV(SpriteBuf);
		P->Sprite.MaxSprites = UniGpuLayout::MaxSprites;
		P->Sprite.TranslatedWorldToClip = FMatrix44f(View.ViewMatrices.GetTranslatedViewProjectionMatrix());
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
		P->Sprite.SceneColourScale = FVector3f(F.SceneColourScale);
		P->Sprite.PixelsPerRadian = PixelsPerRadian;
		P->Sprite.SceneDepthTexture = SceneDepth;
		P->Sprite.SceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->DrawArgs = ArgsBuf;
		P->RenderTargets[0] = FRenderTargetBinding(SceneColor.Texture, ERenderTargetLoadAction::ELoad);

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
	}
	Stats.CellsLastFrame = uint32(F.Cells.Num());
}

// =============================================================================================
// FUniGpuGenBackend
// =============================================================================================

struct FUniGpuGenBackend::FRequest
{
	std::shared_ptr<UniCore::FGenBatch> Batch;
	TUniquePtr<FRHIGPUBufferReadback> CountReadback;      // probe: FTaskCountGPU per task
	TUniquePtr<FRHIGPUBufferReadback> AcceptedReadback;   // place: uint per task
	TUniquePtr<FRHIGPUBufferReadback> ObjectReadback;     // place: the read ranges, concatenated
	std::atomic<bool> bSubmitted{ false };
	std::atomic<bool> bCopied{ false };
	std::atomic<bool> bCopyEnqueued{ false };
	std::atomic<bool> bFailed{ false };
	double SubmitTime = 0.0;
	bool bPlace = false;
};

FUniGpuGenBackend::FUniGpuGenBackend() {}
FUniGpuGenBackend::~FUniGpuGenBackend()
{
	// In-flight requests hold their own shared pointers; the render commands keep them alive.
	FlushRenderingCommands();
}

UniCore::u32 FUniGpuGenBackend::PoolCapacity() const { return UniGpuLayout::PoolCapacity; }

void FUniGpuGenBackend::SubmitProbe(std::shared_ptr<UniCore::FGenBatch> Batch)
{
	check(IsInGameThread());
	TSharedPtr<FRequest, ESPMode::ThreadSafe> Req = MakeShared<FRequest, ESPMode::ThreadSafe>();
	Req->Batch = Batch;
	Req->SubmitTime = FPlatformTime::Seconds();
	// Allocated HERE, on the calling thread, not in the render command (lesson #24).
	Req->CountReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("UniGenCountReadback"));
	Requests.Add(Batch->Ticket, Req);

	TArray<UniCore::FGenTaskGPU> Tasks;
	Tasks.Append(Batch->Tasks.data(), int32(Batch->Tasks.size()));
	TSharedRef<TArray<UniCore::FGenTaskGPU>, ESPMode::ThreadSafe> Payload = MakeShared<TArray<UniCore::FGenTaskGPU>, ESPMode::ThreadSafe>(MoveTemp(Tasks));
	ENQUEUE_RENDER_COMMAND(UniGenProbe)([Req, Payload](FRHICommandListImmediate& RHICmdList)
	{
		const uint32 NumTasks = uint32(Payload->Num());
		if (NumTasks == 0) { Req->bSubmitted = true; return; }
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef TaskBuf = UploadStructured(GraphBuilder, TEXT("UniGenTasks"), *Payload);
		FRDGBufferDesc CountDesc = FRDGBufferDesc::CreateStructuredDesc(UniGpuLayout::TaskCountBytes, NumTasks);
		CountDesc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef CountBuf = GraphBuilder.CreateBuffer(CountDesc, TEXT("UniGenCounts"));
		FUniGenProbeCS::FParameters* P = GraphBuilder.AllocParameters<FUniGenProbeCS::FParameters>();
		P->InTasks = GraphBuilder.CreateSRV(TaskBuf);
		P->OutCounts = GraphBuilder.CreateUAV(CountBuf);
		P->NumTasks = NumTasks;
		uint32 GroupsX = 1;
		const FIntVector Groups = WrappedGroups(NumTasks, GroupsX);
		P->DispatchGroupsX = GroupsX;
		TShaderMapRef<FUniGenProbeCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Gen.Probe %u tasks", NumTasks), CS, P, Groups);
		AddEnqueueCopyPass(GraphBuilder, Req->CountReadback.Get(), CountBuf, NumTasks * UniGpuLayout::TaskCountBytes);
		GraphBuilder.Execute();
		Req->bSubmitted = true;
	});
}

void FUniGpuGenBackend::SubmitPlace(std::shared_ptr<UniCore::FGenBatch> Batch)
{
	check(IsInGameThread());
	TSharedPtr<FRequest, ESPMode::ThreadSafe>* Found = Requests.Find(Batch->Ticket);
	if (!Found) { return; }
	TSharedPtr<FRequest, ESPMode::ThreadSafe> Req = *Found;
	Req->bPlace = true;
	Req->bSubmitted = false; Req->bCopied = false; Req->bCopyEnqueued = false;
	Req->SubmitTime = FPlatformTime::Seconds();
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
			P->NumTasks = NumTasks;
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
		Req->bSubmitted = true;
	});
}

bool FUniGpuGenBackend::Poll(const UniCore::FGenBatch& Batch, bool bPlace)
{
	check(IsInGameThread());
	TSharedPtr<FRequest, ESPMode::ThreadSafe>* Found = Requests.Find(Batch.Ticket);
	if (!Found) { return true; }
	TSharedPtr<FRequest, ESPMode::ThreadSafe> Req = *Found;
	if (Req->bCopied)
	{
		if (bPlace) { Requests.Remove(Batch.Ticket); FUniRenderer::Get().Stats.GenBatchesCompleted++; }
		return true;
	}
	const bool bTimedOut = FPlatformTime::Seconds() - Req->SubmitTime > UniGpuLayout::ReadbackTimeoutSeconds;
	if (bTimedOut && !Req->bCopyEnqueued)
	{
		// Fail closed (lesson #1, #27): an empty result, never a CPU substitute.
		UE_LOG(LogUniRender, Warning, TEXT("UnifiedUniverse: generation batch %llu timed out in the %s phase; its cells stay empty."), Batch.Ticket, bPlace ? TEXT("place") : TEXT("probe"));
		std::shared_ptr<UniCore::FGenBatch> B = Req->Batch;
		if (!bPlace) { B->Counts.assign(B->Tasks.size(), UniCore::FTaskCountGPU{}); }
		else { B->Accepted.assign(B->Tasks.size(), 0u); B->ReadObjects.clear(); }
		Req->bFailed = true;
		Req->bCopied = true;
		FUniRenderer::Get().Stats.GenBatchesFailed++;
		return Poll(Batch, bPlace);
	}
	if (!Req->bSubmitted || Req->bCopyEnqueued) { return false; }
	FRHIGPUBufferReadback* RbA = bPlace ? Req->AcceptedReadback.Get() : Req->CountReadback.Get();
	FRHIGPUBufferReadback* RbB = bPlace ? Req->ObjectReadback.Get() : nullptr;
	if (Req->Batch->Tasks.empty() && !bPlace) { Req->Batch->Counts.clear(); Req->bCopied = true; return true; }
	if (!RbA->IsReady() || (RbB && !RbB->IsReady())) { return false; }
	// Lock() is render-thread only (lesson #24): marshal the copy there.
	Req->bCopyEnqueued = true;
	ENQUEUE_RENDER_COMMAND(UniGenCopy)([Req, bPlace](FRHICommandListImmediate&)
	{
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
			size_t Total = 0;
			for (const auto& R : B->ReadRanges) { Total += R.second; }
			B->ReadObjects.resize(Total);
			if (Total)
			{
				const void* Src = Req->ObjectReadback->Lock(uint32(Total * sizeof(UniCore::FObjectGPU)));
				FMemory::Memcpy(B->ReadObjects.data(), Src, Total * sizeof(UniCore::FObjectGPU));
				Req->ObjectReadback->Unlock();
			}
		}
		Req->bCopied = true;
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
