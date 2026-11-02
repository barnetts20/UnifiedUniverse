#pragma once

// UniShaders.h
// Global shader classes for UniGen.usf, UniProject.usf, UniSprite.usf and UniVolume.usf. Every GPU struct
// these bind is mirrored in UniCore/UniGpuTypes.h with a static_assert (lesson #20).
// Buffer element counts and counter layouts are defined in UniGpuLayout below and nowhere else
// (lesson #22).

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "UniGpuTypes.h"
#include "UniVolume.h"

/** Escape hatch (uu.Shaders, read-only, set at startup; Docs/BUILD_TRIAGE.md). A global
 *  shader that fails to compile can stop the editor from starting. With uu.Shaders=0 none of
 *  this plugin's shaders is compiled, every pass and generation batch is skipped (batches fail
 *  closed), and the editor opens so the errors can be read and fixed. */
bool UniShadersEnabled();

namespace UniGpuLayout
{
	inline constexpr uint32 ObjectBytes = sizeof(UniCore::FObjectGPU);          // 32
	inline constexpr uint32 TaskBytes = sizeof(UniCore::FGenTaskGPU);           // 176
	inline constexpr uint32 TaskCountBytes = sizeof(UniCore::FTaskCountGPU);    // 16
	inline constexpr uint32 SliceBytes = sizeof(UniCore::FSliceGPU);            // 64
	inline constexpr uint32 CellBytes = sizeof(UniCore::FCellGPU);              // 64
	inline constexpr uint32 SpriteBytes = 48;                                   // FUniSprite in UniProject.ush
	inline constexpr uint32 DrawArgsElements = 4;                               // {6, instances, 0, 0}
	inline constexpr uint32 PoolCapacity = 2097152;                             // D-005: 64 MB
	inline constexpr uint32 MaxSprites = 1u << 20;                              // 48 MB of sprites
	inline constexpr uint32 MaxPromote = 256;
	inline constexpr uint32 DynamicPoolSlots = 256;                             // D-143: planet records, rewritten every frame
	inline constexpr uint32 DynamicPoolBase = PoolCapacity - DynamicPoolSlots;  // the streamer allocates below this
	inline constexpr uint32 MaxActivate = 32;                                   // D-142: system activations per frame
	inline constexpr uint32 GenThreads = 64;
	inline constexpr double ReadbackTimeoutSeconds = 2.0;                       // lesson #24 safety valve
	inline constexpr uint32 CarrierBytes = sizeof(UniCore::FCarrierGPU);        // 112
	inline constexpr uint32 VolumeViewBytes = sizeof(UniCore::FVolumeViewGPU);  // 48
	inline constexpr uint32 FieldParamsBytes = sizeof(UniCore::FFieldParamsGPU); // 64
	inline constexpr uint32 VolumeSlices = 8;                                   // UNI_VOL_SLICES (UniVolume.ush)
	inline constexpr uint32 VolumeLutEntries = 256;                             // UNI_VOL_LUT_SIZE
	inline constexpr uint32 MaxCarriers = 64;
}
static_assert(UniGpuLayout::VolumeSlices == UniHlsl::UNI_VOL_SLICES, "slice count");
static_assert(UniGpuLayout::VolumeLutEntries == UniHlsl::UNI_VOL_LUT_SIZE, "volume LUT size");

/** Mirrors FUniSprite in UniProject.ush. */
struct FUniSpriteGPU
{
	FVector3f RenderPos;
	float AngularRadius;
	FVector3f Radiance;
	float TrueDistance;
	float NdcX;
	float NdcY;
	uint32 Flags;
	float PhysAngularRadius;
};
static_assert(sizeof(FUniSpriteGPU) == UniGpuLayout::SpriteBytes, "FUniSpriteGPU must match FUniSprite (UniProject.ush)");

class FUniGenProbeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniGenProbeCS);
	SHADER_USE_PARAMETER_STRUCT(FUniGenProbeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniGenTask>, InTasks)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUniTaskCount>, OutCounts)
		SHADER_PARAMETER(uint32, NumTasks)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_GEN_PASS"), 0);
	}
};

class FUniGenPlaceCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniGenPlaceCS);
	SHADER_USE_PARAMETER_STRUCT(FUniGenPlaceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniGenTask>, InTasks)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniTaskCount>, InCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniSlice>, InSlices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InLut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUniObject>, OutPool)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutAccepted)
		SHADER_PARAMETER(uint32, NumTasks)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_GEN_PASS"), 1);
	}
};

/** Scatters authored records (anchors) into the pool: OutPool[Index[i]] = Record[i]. */
class FUniScatterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FUniScatterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InScatterIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniObject>, InScatterRecord)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUniObject>, OutPool)
		SHADER_PARAMETER(uint32, NumWrites)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_GEN_PASS"), 2);
	}
};

class FUniProjectCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniProjectCS);
	SHADER_USE_PARAMETER_STRUCT(FUniProjectCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniCellGPU>, InCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniObject>, InPool)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUniSprite>, OutSprites)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutDrawArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutPromote)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutPromoteCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutActivate)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUniObject>, OutActivateRecord)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutActivateCount)
		SHADER_PARAMETER(uint32, NumCells)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
		SHADER_PARAMETER(uint32, MaxSprites)
		SHADER_PARAMETER(uint32, MaxPromote)
		SHADER_PARAMETER(uint32, MaxActivate)
		SHADER_PARAMETER(uint32, SystemArchetypeMask)
		SHADER_PARAMETER(FVector3f, ViewForward)
		SHADER_PARAMETER(float, TanHalfFovX)
		SHADER_PARAMETER(FVector3f, ViewRight)
		SHADER_PARAMETER(float, TanHalfFovY)
		SHADER_PARAMETER(FVector3f, ViewUp)
		SHADER_PARAMETER(float, LimitingMag)
		SHADER_PARAMETER(FVector3f, CameraFraction)
		SHADER_PARAMETER(float, ExposureMag)
		SHADER_PARAMETER(float, SurfaceLimit)
		SHADER_PARAMETER(float, ThetaPixel)
		SHADER_PARAMETER(float, GlareGain)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

/** Shared by the three volume passes: carriers, field parameters, LUTs, view (one element),
 *  all structured (lesson #2), plus the camera basis for ray directions. */
BEGIN_SHADER_PARAMETER_STRUCT(FUniVolumeCommonParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniCarrier>, InCarriers)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniFieldParams>, InParams)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InLut)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniVolumeView>, InVolumeView)
	SHADER_PARAMETER(FVector3f, ViewForward)
	SHADER_PARAMETER(float, TanHalfFovX)
	SHADER_PARAMETER(FVector3f, ViewRight)
	SHADER_PARAMETER(float, TanHalfFovY)
	SHADER_PARAMETER(FVector3f, ViewUp)
	SHADER_PARAMETER(float, Pad0)
END_SHADER_PARAMETER_STRUCT()

class FUniVolumeMarchCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniVolumeMarchCS);
	SHADER_USE_PARAMETER_STRUCT(FUniVolumeMarchCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FUniVolumeCommonParameters, Common)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(FVector4f, MarchToDepthUV)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
		SHADER_PARAMETER(FUintVector2, MarchSize)
		SHADER_PARAMETER(uint32, DebugMode)
		SHADER_PARAMETER(float, DebugLogLo)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutVolume)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, OutSlices)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_VOL_PASS"), 0);
	}
};

class FUniVolumeBackgroundCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniVolumeBackgroundCS);
	SHADER_USE_PARAMETER_STRUCT(FUniVolumeBackgroundCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FUniVolumeCommonParameters, Common)
		SHADER_PARAMETER(uint32, CubeSize)
		SHADER_PARAMETER(uint32, FaceBase)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutCube)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_VOL_PASS"), 1);
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FUniVolumeCompositeParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FUniVolumeCommonParameters, Common)
	SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
	SHADER_PARAMETER(FVector4f, MarchToDepthUV)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
	SHADER_PARAMETER(FUintVector2, MarchSize)
	SHADER_PARAMETER(FVector4f, OutRectMinSize)
	SHADER_PARAMETER(FVector4f, OutToDepthUV)
	SHADER_PARAMETER(FVector3f, SceneColourScale)
	SHADER_PARAMETER(float, FocalPx)
	SHADER_PARAMETER(uint32, CubeSize)
	SHADER_PARAMETER(uint32, bBackground)
	SHADER_PARAMETER(float, BackgroundStartCm)
	SHADER_PARAMETER(float, Pad1)
	SHADER_PARAMETER(uint32, DebugMode)
	SHADER_PARAMETER(float, DebugScale)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, VolumeTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, CubeTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
END_SHADER_PARAMETER_STRUCT()

class FUniVolumeCompositeVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniVolumeCompositeVS);
	SHADER_USE_PARAMETER_STRUCT(FUniVolumeCompositeVS, FGlobalShader);
	using FParameters = FUniVolumeCompositeParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_VOL_PASS"), 2);
	}
};

class FUniVolumeCompositePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniVolumeCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FUniVolumeCompositePS, FGlobalShader);
	using FParameters = FUniVolumeCompositeParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_VOL_PASS"), 2);
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FUniVolumeCompositePassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FUniVolumeCompositeParameters, Composite)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FUniSpriteParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniSprite>, InSprites)
	SHADER_PARAMETER(uint32, MaxSprites)
	SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
	SHADER_PARAMETER(FVector3f, ViewForward)
	SHADER_PARAMETER(FVector3f, ViewRight)
	SHADER_PARAMETER(FVector3f, ViewUp)
	SHADER_PARAMETER(float, SpriteExtent)
	SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
	SHADER_PARAMETER(FVector4f, DepthUVScaleBias)
	SHADER_PARAMETER(FVector3f, SceneColourScale)
	SHADER_PARAMETER(float, PixelsPerRadian)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float>, VolumeSlices)
	SHADER_PARAMETER(uint32, bVolume)
	SHADER_PARAMETER(float, SliceLnTLo)
	SHADER_PARAMETER(float, SliceLnTHi)
	SHADER_PARAMETER(FVector4f, OutToMarch)
	SHADER_PARAMETER(FUintVector2, MarchSize)
END_SHADER_PARAMETER_STRUCT()

class FUniSpriteVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSpriteVS);
	SHADER_USE_PARAMETER_STRUCT(FUniSpriteVS, FGlobalShader);
	using FParameters = FUniSpriteParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

class FUniSpritePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSpritePS);
	SHADER_USE_PARAMETER_STRUCT(FUniSpritePS, FGlobalShader);
	using FParameters = FUniSpriteParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

BEGIN_SHADER_PARAMETER_STRUCT(FUniSpritePassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FUniSpriteParameters, Sprite)
	RDG_BUFFER_ACCESS(DrawArgs, ERHIAccess::IndirectArgs)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()
