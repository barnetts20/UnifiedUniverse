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
#include "RHIStaticStates.h"
#include "HAL/IConsoleManager.h"
#include "Misc/EngineVersionComparison.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

static TAutoConsoleVariable<float> CVarUniSpriteExtent(TEXT("uu.SpriteExtent"), 3.0f, TEXT("Sprite quad half size in Gaussian sigmas."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniGlare(TEXT("uu.GlareGain"), 0.35f, TEXT("Sprite footprint growth with flux."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniVolumeDivisor(TEXT("uu.VolumeDivisor"), 2, TEXT("Volume march resolution divisor (1 = full, 2 = half, 4 = quarter)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniBackgroundSize(TEXT("uu.BackgroundSize"), 256, TEXT("Background direction-map face size (texels)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniVolumeDebug(TEXT("uu.VolumeDebug"), 0, TEXT("Volume debug view (lesson #37): 0 off, 1 steps per ray (magenta = truncated), 2 coverage (R carriers, G supersampling, B truncated), 3 emission (log10 heat), 4 dust transmittance."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> CVarUniVolumeDebugLogLo(TEXT("uu.VolumeDebugLogLo"), 0.0f, TEXT("uu.VolumeDebug 3: log10 radiance mapped to the bottom of the heat ramp (the ramp spans 6 decades)."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarUniBackgroundFaces(TEXT("uu.BackgroundFacesPerFrame"), 2, TEXT("Background faces re-marched per frame while dirty (1..6)."), ECVF_RenderThreadSafe);

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

uint32 FUniRenderer::DynamicPoolBase() { return UniGpuLayout::DynamicPoolBase; }
uint32 FUniRenderer::DynamicPoolSlots() { return UniGpuLayout::DynamicPoolSlots; }

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

void FUniRenderer::Release_RenderThread()
{
	check(IsInRenderingThread());
	Pool.SafeRelease();
	SlicesBuffer.SafeRelease();
	LutBuffer.SafeRelease();
	PromoteRing.Empty();
	ActivateRing.Empty();
	{
		FScopeLock Lock(&ActivationLock);
		ActivationsReady.Reset();
	}
	BackgroundCube.SafeRelease();
	BackgroundVersionRT = ~0u;
	BackgroundDirtyFaces = 0x3Fu;
	RenderFrame = FUniFrameData();
	bTablesDirty = true;
}

void FUniRenderer::RenderVolume_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output,
	FRDGTextureRef SceneDepth, const FIntRect& DepthViewRect, float PreExposure, FRDGTextureRef& OutSlices, FIntPoint& OutMarchSize)
{
	const FUniFrameData& F = RenderFrame;
	RDG_EVENT_SCOPE(GraphBuilder, "Uni.Volume");
	const FIntRect OutRect = Output.ViewRect;
	const FMatrix Proj = View.ViewMatrices.ComputeProjectionNoAAMatrix();   // post-TSR: unjittered (W2)
	const float TanX = 1.0f / float(Proj.M[0][0]);
	const float TanY = 1.0f / float(Proj.M[1][1]);
	const int32 Div = FMath::Clamp(CVarUniVolumeDivisor.GetValueOnRenderThread(), 1, 8);
	const FIntPoint MarchSize(FMath::Max(OutRect.Width() / Div, 1), FMath::Max(OutRect.Height() / Div, 1));
	OutMarchSize = MarchSize;

	// Shared inputs. The view's ThetaPixel is the march texel's, so prefiltering and
	// supersampling follow the resolution actually marched.
	UniCore::FVolumeViewGPU VV = F.VolumeView;
	VV.ThetaPixel = 2.0f * TanX / float(MarchSize.X);
	FRDGBufferRef ParamBuf = UploadStructured(GraphBuilder, TEXT("UniVolumeParams"), F.VolumeParams);
	FRDGBufferRef LutBuf = UploadStructured(GraphBuilder, TEXT("UniVolumeLut"), F.VolumeLut);
	auto FillCommon = [&](FUniVolumeCommonParameters& C, const TArray<UniCore::FCarrierGPU>& Carriers, const UniCore::FVolumeViewGPU& V)
	{
		TArray<UniCore::FVolumeViewGPU> One; One.Add(V);
		One[0].NumCarriers = uint32(Carriers.Num());
		C.InCarriers = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniVolumeCarriers"), Carriers));
		C.InParams = GraphBuilder.CreateSRV(ParamBuf);
		C.InLut = GraphBuilder.CreateSRV(LutBuf);
		C.InVolumeView = GraphBuilder.CreateSRV(UploadStructured(GraphBuilder, TEXT("UniVolumeView"), One));
		C.ViewForward = FVector3f(View.GetViewDirection());
		C.ViewRight = FVector3f(View.GetViewRight());
		C.ViewUp = FVector3f(View.GetViewUp());
		C.TanHalfFovX = TanX;
		C.TanHalfFovY = TanY;
		C.Pad0 = 0.0f;
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

	// 1. Background direction map: re-march dirty faces, a few per frame (D-136).
	const bool bBackground = F.BackgroundCarriers.Num() > 0;
	const int32 CubeSize = FMath::Clamp(CVarUniBackgroundSize.GetValueOnRenderThread(), 16, 1024);
	if (bBackground && (!BackgroundCube.IsValid() || BackgroundCube->GetDesc().Extent.X != CubeSize)) { BackgroundCube.SafeRelease(); BackgroundDirtyFaces = 0x3Fu; }
	FRDGTextureRef Cube = nullptr;
	if (bBackground)
	{
		if (!BackgroundCube.IsValid())
		{
			// Float32: radiance per steradian in exposure units overflows half floats (a galaxy disc
			// is ~7e4 per sr at the default exposure; sensitivity and exposure scale it further).
			const FRDGTextureDesc Desc = FRDGTextureDesc::Create2DArray(FIntPoint(CubeSize, CubeSize), PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV, 6);
			Cube = GraphBuilder.CreateTexture(Desc, TEXT("UniBackgroundCube"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Cube), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
			BackgroundCube = GraphBuilder.ConvertToExternalTexture(Cube);
		}
		else
		{
			Cube = GraphBuilder.RegisterExternalTexture(BackgroundCube, TEXT("UniBackgroundCube"));
		}
		if (F.BackgroundVersion != BackgroundVersionRT) { BackgroundVersionRT = F.BackgroundVersion; BackgroundDirtyFaces = 0x3Fu; }
		int32 Budget = FMath::Clamp(CVarUniBackgroundFaces.GetValueOnRenderThread(), 1, 6);
		for (uint32 Face = 0; Face < 6 && Budget > 0; ++Face)
		{
			if (!(BackgroundDirtyFaces & (1u << Face))) { continue; }
			FUniVolumeBackgroundCS::FParameters* P = GraphBuilder.AllocParameters<FUniVolumeBackgroundCS::FParameters>();
			FillCommon(P->Common, F.BackgroundCarriers, F.VolumeView);
			P->CubeSize = uint32(CubeSize);
			P->FaceBase = Face;
			P->OutCube = GraphBuilder.CreateUAV(Cube);
			TShaderMapRef<FUniVolumeBackgroundCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Volume.Background face %u", Face), CS, P, FIntVector(FMath::DivideAndRoundUp(CubeSize, 8), FMath::DivideAndRoundUp(CubeSize, 8), 1));
			BackgroundDirtyFaces &= ~(1u << Face);
			--Budget;
			Stats.BackgroundFacesMarched++;
		}
	}
	else
	{
		Cube = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(FIntPoint(1, 1), PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV, 6), TEXT("UniBackgroundDummy"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Cube), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
	}

	// 2. March the local carriers at reduced resolution.
	FRDGTextureRef VolumeTex = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(MarchSize, PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("UniVolume"));   // float32: see the cube
	FRDGTextureRef Slices = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2DArray(MarchSize, PF_R16F, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_UAV, UniGpuLayout::VolumeSlices), TEXT("UniVolumeSlices"));
	{
		FUniVolumeMarchCS::FParameters* P = GraphBuilder.AllocParameters<FUniVolumeMarchCS::FParameters>();
		FillCommon(P->Common, F.VolumeCarriers, VV);
		P->InvDeviceZToWorldZTransform = FVector4f(View.InvDeviceZToWorldZTransform);
		P->MarchToDepthUV = MarchToDepthUV;
		P->SceneDepthTexture = SceneDepth;
		P->SceneDepthSampler = PointClamp;
		P->MarchSize = FUintVector2(uint32(MarchSize.X), uint32(MarchSize.Y));
		P->DebugMode = uint32(FMath::Clamp(CVarUniVolumeDebug.GetValueOnRenderThread(), 0, 4));
		P->DebugLogLo = CVarUniVolumeDebugLogLo.GetValueOnRenderThread();
		P->OutVolume = GraphBuilder.CreateUAV(VolumeTex);
		P->OutSlices = GraphBuilder.CreateUAV(Slices);
		TShaderMapRef<FUniVolumeMarchCS> CS(GetGlobalShaderMap(View.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Uni.Volume.March %dx%d, %d carriers", MarchSize.X, MarchSize.Y, F.VolumeCarriers.Num()), CS, P, FComputeShaderUtils::GetGroupCount(MarchSize, 8));
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
		C.Pad1 = 0.0f;
		C.DebugMode = uint32(FMath::Clamp(CVarUniVolumeDebug.GetValueOnRenderThread(), 0, 4));
		C.DebugScale = 1.0f / FMath::Max(PreExposure, 1.0e-6f);
		C.VolumeTexture = VolumeTex;
		C.CubeTexture = Cube;
		C.LinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);
		TShaderMapRef<FUniVolumeCompositeVS> VS(GetGlobalShaderMap(View.GetFeatureLevel()));
		TShaderMapRef<FUniVolumeCompositePS> PS(GetGlobalShaderMap(View.GetFeatureLevel()));
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
		P->NumCells = uint32(F.Cells.Num());
		P->MaxSprites = UniGpuLayout::MaxSprites;
		P->MaxPromote = UniGpuLayout::MaxPromote;
		P->MaxActivate = UniGpuLayout::MaxActivate;
		P->SystemArchetypeMask = F.SystemArchetypeMask;
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
		Req->Place.bSubmitted = true;
	});
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
	FRHIGPUBufferReadback* RbB = bPlace ? Req->ObjectReadback.Get() : nullptr;
	if (!RbA->IsReady() || (RbB && !RbB->IsReady())) { return false; }
	// Lock() is render-thread only (lesson #24): marshal the copy there.
	Ph.bCopyEnqueued = true;
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
