#pragma once

// UniShaders.h
// Global shader classes for UniGen.usf, UniProject.usf and UniSprite.usf. Every GPU struct
// these bind is mirrored in UniCore/UniGpuTypes.h with a static_assert (lesson #20).
// Buffer element counts and counter layouts are defined in UniGpuLayout below and nowhere else
// (lesson #22).

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "UniGpuTypes.h"

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
	inline constexpr uint32 GenThreads = 64;
	inline constexpr double ReadbackTimeoutSeconds = 2.0;                       // lesson #24 safety valve
}

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

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
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

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
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

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
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
		SHADER_PARAMETER(uint32, NumCells)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
		SHADER_PARAMETER(uint32, MaxSprites)
		SHADER_PARAMETER(uint32, MaxPromote)
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

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

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
END_SHADER_PARAMETER_STRUCT()

class FUniSpriteVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSpriteVS);
	SHADER_USE_PARAMETER_STRUCT(FUniSpriteVS, FGlobalShader);
	using FParameters = FUniSpriteParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

class FUniSpritePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSpritePS);
	SHADER_USE_PARAMETER_STRUCT(FUniSpritePS, FGlobalShader);
	using FParameters = FUniSpriteParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

BEGIN_SHADER_PARAMETER_STRUCT(FUniSpritePassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FUniSpriteParameters, Sprite)
	RDG_BUFFER_ACCESS(DrawArgs, ERHIAccess::IndirectArgs)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()
