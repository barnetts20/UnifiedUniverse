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
	inline constexpr uint32 TaskBytes = sizeof(UniCore::FGenTaskGPU);           // 272 (D-153 sampling box, D-178 field params 128)
	inline constexpr uint32 TaskCountBytes = sizeof(UniCore::FTaskCountGPU);    // 48 (D-214)
	inline constexpr uint32 SliceBytes = sizeof(UniCore::FSliceGPU);            // 64
	inline constexpr uint32 CellBytes = sizeof(UniCore::FCellGPU);              // 64
	inline constexpr uint32 SpriteBytes = 64;                                   // FUniSprite in UniProject.ush (D-156: + halo)
	inline constexpr uint32 DrawArgsElements = 4;                               // {6, instances, 0, 0}
	inline constexpr uint32 PoolCapacity = 2097152;                             // D-005: 64 MB
	inline constexpr uint32 MaxSprites = 1u << 20;                              // 64 MB of sprites
	inline constexpr uint32 MaxPromote = 256;
	inline constexpr uint32 DynamicPoolSlots = 256;                             // D-143: planet records, rewritten every frame
	inline constexpr uint32 DynamicPoolBase = PoolCapacity - DynamicPoolSlots;  // the streamer allocates below this
	inline constexpr uint32 MaxActivate = 32;                                   // D-142: system activations per frame
	inline constexpr uint32 MaxQueryResults = 4096;                             // D-193: hits read back per resident query
	inline constexpr uint32 PickResultWords = 16;                               // D-193: UniQuery.usf OutPickResult
	inline constexpr uint32 GenThreads = 64;
	inline constexpr double ReadbackTimeoutSeconds = 2.0;                       // lesson #24 safety valve
	inline constexpr uint32 CarrierBytes = sizeof(UniCore::FCarrierGPU);        // 112
	inline constexpr uint32 VolumeViewBytes = sizeof(UniCore::FVolumeViewGPU);  // 48
	inline constexpr uint32 FieldParamsBytes = sizeof(UniCore::FFieldParamsGPU); // 128 (D-178)
	inline constexpr uint32 VolumeSlices = 8;                                   // UNI_VOL_SLICES (UniVolume.ush)
	inline constexpr uint32 VolumeLutEntries = 256;                             // UNI_VOL_LUT_SIZE
	inline constexpr uint32 MaxCarriers = 128;                                  // D-161
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
	float HaloRadius;     // D-156
	float HaloRatio;
	float EllipseAngle;   // D-183
	float EllipseAxis;
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
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniWebTexture)   // D-164 (UniWebTexture.ush)
		SHADER_PARAMETER_SAMPLER(SamplerState, UniWebSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalWarpFine)    // D-179 (UniWebTexture.ush)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalWarpLarge)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalDetail)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, UniGalMap)   // D-214: galaxies place from their maps
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_GEN_PASS"), 0);
	}
};

/** D-214: the split place pass, EVAL (UniGen.usf pass 1): one group per chunk of candidates. */
class FUniGenEvalCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniGenEvalCS);
	SHADER_USE_PARAMETER_STRUCT(FUniGenEvalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniGenTask>, InTasks)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniTaskCount>, InCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, InPlan)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InPlanTotal)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, InAlloc)   // D-207: with UseAlloc (one-trip batches)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkMask)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkCount)
		SHADER_PARAMETER(uint32, UseAlloc)
		SHADER_PARAMETER(uint32, NumTasks)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniWebTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, UniWebSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalWarpFine)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalWarpLarge)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalDetail)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, UniGalMap)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_GEN_PASS"), 1);
	}
};

/** D-214: SCAN (pass 4): one thread per task, each chunk's accepted count before it. */
class FUniGenScanCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniGenScanCS);
	SHADER_USE_PARAMETER_STRUCT(FUniGenScanCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, InPlan)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InChunkCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkPrefix)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutAccepted)
		SHADER_PARAMETER(uint32, NumTasks)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_GEN_PASS"), 4);
	}
};

/** D-214: WRITE (pass 5): one group per chunk: ranks, records, a task's tail and blanks. */
class FUniGenWriteCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniGenWriteCS);
	SHADER_USE_PARAMETER_STRUCT(FUniGenWriteCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniGenTask>, InTasks)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniTaskCount>, InCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, InPlan)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InPlanTotal)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, InAlloc)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniSlice>, InSlices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InLut)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InChunkMask)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InChunkCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InChunkPrefix)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUniObject>, OutPool)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutAccepted)
		SHADER_PARAMETER(uint32, UseAlloc)
		SHADER_PARAMETER(uint32, NumTasks)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniWebTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, UniWebSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalWarpFine)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalWarpLarge)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalDetail)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, UniGalMap)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_GEN_PASS"), 5);
	}
};

/** D-207 / D-214: ALLOC (pass 3), one thread: pool ranges and plans of a one-trip batch. */
class FUniGenAllocCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniGenAllocCS);
	SHADER_USE_PARAMETER_STRUCT(FUniGenAllocCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniGenTask>, InTasks)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniTaskCount>, InCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, OutAlloc)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, OutPlan)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutPlanTotal)
		SHADER_PARAMETER(uint32, NumTasks)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
		SHADER_PARAMETER(uint32, ArenaBase)
		SHADER_PARAMETER(uint32, ArenaCap)
		SHADER_PARAMETER(uint32, MaxPerCell)
		SHADER_PARAMETER(uint32, CandidateCap)
		SHADER_PARAMETER(uint32, ChunkCap)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_GEN_PASS"), 3);
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
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutSpriteSource)   // D-193
		SHADER_PARAMETER(uint32, NumCells)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
		SHADER_PARAMETER(uint32, MaxSprites)
		SHADER_PARAMETER(uint32, MaxPromote)
		SHADER_PARAMETER(uint32, MaxActivate)
		SHADER_PARAMETER(uint32, SystemArchetypeMask)
		SHADER_PARAMETER(float, WarpAmount)        // D-187
		SHADER_PARAMETER(FVector4f, WarpVelBeta)   // D-190
		SHADER_PARAMETER(FVector4f, WarpDopplerKQ)
		SHADER_PARAMETER(float, ArrivalFrames)
		SHADER_PARAMETER(uint32, DiscArchetypeMask)   // D-183
		SHADER_PARAMETER(FUintVector4, FixedDiscSeeds)   // D-183
		SHADER_PARAMETER(uint32, WriteSpriteSource)      // D-193
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, InHandoffs)   // D-196
		SHADER_PARAMETER(uint32, NumHandoffs)
		SHADER_PARAMETER(uint32, SplatMode)                                       // D-223
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutQuadIndex)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutQuadArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutSplatIndex)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutSplatCount)
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
		SHADER_PARAMETER(float, SpriteCorePx)        // D-156 sprite style
		SHADER_PARAMETER(float, SpriteCoreSat)
		SHADER_PARAMETER(float, SpriteHaloGain)
		SHADER_PARAMETER(float, SpriteHaloGrowthPx)
		SHADER_PARAMETER(float, SpriteHaloMaxPx)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

/** D-193: the pick passes (UniQuery.usf): the frame's sprites and their sources, a cursor. */
BEGIN_SHADER_PARAMETER_STRUCT(FUniPickParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniCellGPU>, InCells)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniObject>, InPool)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniSprite>, InSprites)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InSpriteSource)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InDrawArgs)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutPickBest)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutPickResult)
	SHADER_PARAMETER(uint32, MaxSprites)
	SHADER_PARAMETER(FVector2f, CursorNdc)
	SHADER_PARAMETER(FVector2f, HalfViewPx)
	SHADER_PARAMETER(float, PixelsPerRadian)
	SHADER_PARAMETER(float, PickMinPx)
END_SHADER_PARAMETER_STRUCT()

#define UNI_DECLARE_PICK_CS(Name) \
	class Name : public FGlobalShader \
	{ \
	public: \
		DECLARE_GLOBAL_SHADER(Name); \
		using FParameters = FUniPickParameters; \
		SHADER_USE_PARAMETER_STRUCT(Name, FGlobalShader); \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); } \
	};
UNI_DECLARE_PICK_CS(FUniPickScoreCS)
UNI_DECLARE_PICK_CS(FUniPickSelectCS)
UNI_DECLARE_PICK_CS(FUniPickResolveCS)
#undef UNI_DECLARE_PICK_CS

/** D-193: resident queries (UniQuery.usf QueryCS): one group of 64 per cell. */
class FUniQueryCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniQueryCS);
	SHADER_USE_PARAMETER_STRUCT(FUniQueryCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniCellGPU>, InCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniObject>, InPool)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutQueryMin)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InQueryMin)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, OutQueryHits)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUniObject>, OutQueryRecords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutQueryCount)
		SHADER_PARAMETER(uint32, NumCells)
		SHADER_PARAMETER(uint32, DispatchGroupsX)
		SHADER_PARAMETER(uint32, QueryMode)
		SHADER_PARAMETER(FUintVector4, QueryDeltaX)
		SHADER_PARAMETER(FUintVector4, QueryDeltaY)
		SHADER_PARAMETER(FUintVector4, QueryDeltaZ)
		SHADER_PARAMETER(float, QueryRadius)
		SHADER_PARAMETER(uint32, QueryArchMask)
		SHADER_PARAMETER(float, QueryMaxAbsMag)
		SHADER_PARAMETER(uint32, QueryMaxResults)
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
	SHADER_PARAMETER(FVector4f, WarpVelBeta)     // D-190
	SHADER_PARAMETER(FVector4f, WarpDopplerKQ)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniWebTexture)   // D-164
	SHADER_PARAMETER_SAMPLER(SamplerState, UniWebSampler)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalWarpFine)    // D-179
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalWarpLarge)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, UniGalDetail)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, UniGalMap)  // D-212; D-214: one slot per galaxy
END_SHADER_PARAMETER_STRUCT()

/** D-212: the galaxy map's GPU bake (UNI_VOL_PASS 5): rows of mip 0, then 2 x 2 mip means. */
class FUniGalMapBakeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniGalMapBakeCS);
	SHADER_USE_PARAMETER_STRUCT(FUniGalMapBakeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniFieldParams>, InMapParams)
		SHADER_PARAMETER(uint32, MapSize)
		SHADER_PARAMETER(uint32, MapRow0)
		SHADER_PARAMETER(uint32, MapRows)
		SHADER_PARAMETER(uint32, bMapDust)
		SHADER_PARAMETER(uint32, MapSlot)   // D-214: the array slot baked
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutMap)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_VOL_PASS"), 5);
	}
};

class FUniGalMapDownCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniGalMapDownCS);
	SHADER_USE_PARAMETER_STRUCT(FUniGalMapDownCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MapSize)
		SHADER_PARAMETER(uint32, MapSlot)   // D-214
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, InMapMip)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutMap)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_VOL_PASS"), 5);
	}
};

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
		SHADER_PARAMETER(uint32, WebDebugMode)        // D-169
		SHADER_PARAMETER(float, WebDebugDepthCm)
		SHADER_PARAMETER(float, WebDebugThreshold)
		SHADER_PARAMETER(float, WebDebugOpacity)
		SHADER_PARAMETER(uint32, bSliceMarch)         // D-184
		SHADER_PARAMETER(FUintVector2, SliceOffset)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutVolume)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, OutSlices)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutDepth)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_VOL_PASS"), 0);
	}
};

/** D-184: temporal slicing's reconstruction (UNI_VOL_PASS 3). */
class FUniVolumeReconstructCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniVolumeReconstructCS);
	SHADER_USE_PARAMETER_STRUCT(FUniVolumeReconstructCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FUniVolumeCommonParameters, Common)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(FVector4f, MarchToDepthUV)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
		SHADER_PARAMETER(FUintVector2, MarchSize)
		SHADER_PARAMETER(FUintVector2, HistSize)   // D-215: the history's size (kept across a resize)
		SHADER_PARAMETER(FVector3f, PrevForward)
		SHADER_PARAMETER(float, PrevTanX)
		SHADER_PARAMETER(FVector3f, PrevRight)
		SHADER_PARAMETER(float, PrevTanY)
		SHADER_PARAMETER(FVector3f, PrevUp)
		SHADER_PARAMETER(uint32, TemporalPhase)
		SHADER_PARAMETER(FVector3f, CamStep)
		SHADER_PARAMETER(uint32, bHistory)
		SHADER_PARAMETER(FVector4f, PrevWarpVelBeta)   // D-190
		SHADER_PARAMETER(FVector4f, TemporalClamp)     // D-194: (on, tight px, max motion px, 0)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FreshVolume)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float>, FreshSlices)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, FreshDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, HistVolume)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float>, HistSlices)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, HistDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutVolume)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, OutSlices)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutDepth)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
	{
		FGlobalShader::ModifyCompilationEnvironment(P, Env);
		Env.SetDefine(TEXT("UNI_VOL_PASS"), 3);
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
	SHADER_PARAMETER(float, BackgroundBlend)   // D-159: previous -> current background map
	SHADER_PARAMETER(uint32, DebugMode)
	SHADER_PARAMETER(float, DebugScale)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, VolumeTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, CubeTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, CubePrevTexture)
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
	SHADER_PARAMETER(float, HaloChroma)   // D-185
	SHADER_PARAMETER(float, SpriteFloor)  // D-188
	SHADER_PARAMETER(FVector4f, WarpA)    // D-187: warp, streak exposure frames, longest streak px, arrival stretch
	SHADER_PARAMETER(FVector3f, CamStep)  // D-187: camera move over the last frame, cm
	SHADER_PARAMETER(float, WarpPad)
	SHADER_PARAMETER(FVector4f, WarpVelBeta)   // D-190
	SHADER_PARAMETER(uint32, MoteMode)         // D-192: 1: this draw is the warp motes (Uni_MoteSprite), not the sprite buffer
	SHADER_PARAMETER(FUintVector4, MoteCell0)
	SHADER_PARAMETER(FUintVector4, MoteCell1)
	SHADER_PARAMETER(FVector4f, MoteFrac0)
	SHADER_PARAMETER(FVector4f, MoteFrac1)
	SHADER_PARAMETER(FVector4f, MoteScale)     // cell cm (two levels), flux, core angular radius
	SHADER_PARAMETER(FVector4f, MoteDopplerKQ)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float>, VolumeSlices)
	SHADER_PARAMETER(uint32, bVolume)
	SHADER_PARAMETER(float, SliceLnTLo)
	SHADER_PARAMETER(float, SliceLnTHi)
	SHADER_PARAMETER(FVector4f, OutToMarch)
	SHADER_PARAMETER(FUintVector2, MarchSize)
	SHADER_PARAMETER(uint32, bQuadIndex)                                    // D-223: instance i draws InQuadIndex[i]
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InQuadIndex)
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

/** D-223: the splat path's deposit (UniSprite.usf SplatCS): each plain point sprite's light into
 *  the fixed-point buffers, after its scene-depth test and the dust at its centre. */
class FUniSplatCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FUniSplatCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUniSprite>, InSprites)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InSplatIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InSplatCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutSplatLum)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutSplatCol)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutSplatTiles)   // D-223 amendment: the tiles written
		SHADER_PARAMETER(FUintVector2, SplatTiles)
		SHADER_PARAMETER(uint32, MaxSprites)
		SHADER_PARAMETER(FUintVector2, SplatSize)
		SHADER_PARAMETER(FVector4f, OutRectMinSize)
		SHADER_PARAMETER(uint32, SplatThreads)
		SHADER_PARAMETER(FVector3f, ViewForward)
		SHADER_PARAMETER(FVector3f, SceneColourScale)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(FVector4f, DepthUVScaleBias)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float>, VolumeSlices)
		SHADER_PARAMETER(uint32, bVolume)
		SHADER_PARAMETER(float, SliceLnTLo)
		SHADER_PARAMETER(float, SliceLnTHi)
		SHADER_PARAMETER(FVector4f, OutToMarch)
		SHADER_PARAMETER(FUintVector2, MarchSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

/** D-223: the splat path's rows (UniSprite.usf SplatGatherCS): the 2x luminance gathered to 1x
 *  columns, the colour's rows blurred at 1x. */
class FUniSplatGatherCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSplatGatherCS);
	SHADER_USE_PARAMETER_STRUCT(FUniSplatGatherCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InSplatLum)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InSplatCol)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutGatherLum)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutGatherCol)
		SHADER_PARAMETER(FUintVector2, SplatSize)
		SHADER_PARAMETER(float, SplatCorePx)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InTileLevel)   // D-223 amendment
		SHADER_PARAMETER(FUintVector2, SplatTiles)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

/** D-223 amendment: each tile's level from the tiles the deposit marked (UniSprite.usf SplatTilesCS). */
class FUniSplatTilesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSplatTilesCS);
	SHADER_USE_PARAMETER_STRUCT(FUniSplatTilesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InSplatTiles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutTileLevel)
		SHADER_PARAMETER(FUintVector2, SplatTiles)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

/** D-223 amendment: the persistent deposit buffers zeroed where this frame wrote them, the marks
 *  reset (UniSprite.usf SplatZeroCS). */
class FUniSplatZeroCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSplatZeroCS);
	SHADER_USE_PARAMETER_STRUCT(FUniSplatZeroCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InTileLevel)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutSplatLum)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutSplatCol)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutSplatTiles)
		SHADER_PARAMETER(FUintVector2, SplatSize)
		SHADER_PARAMETER(FUintVector2, SplatTiles)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

/** D-223: the splat path's columns and composite (UniSprite.usf SplatResolveVS / PS). */
BEGIN_SHADER_PARAMETER_STRUCT(FUniSplatResolveParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, InGatherLum)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, InGatherCol)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InTileLevel)   // D-223 amendment
	SHADER_PARAMETER(FUintVector2, SplatTiles)
	SHADER_PARAMETER(FUintVector2, SplatSize)
	SHADER_PARAMETER(FVector4f, OutRectMinSize)
	SHADER_PARAMETER(float, SplatCorePx)
	SHADER_PARAMETER(FVector2f, TanHalfFov)
	SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
	SHADER_PARAMETER(FVector4f, DepthUVScaleBias)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
END_SHADER_PARAMETER_STRUCT()

class FUniSplatResolveVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSplatResolveVS);
	SHADER_USE_PARAMETER_STRUCT(FUniSplatResolveVS, FGlobalShader);
	using FParameters = FUniSplatResolveParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

class FUniSplatResolvePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniSplatResolvePS);
	SHADER_USE_PARAMETER_STRUCT(FUniSplatResolvePS, FGlobalShader);
	using FParameters = FUniSplatResolveParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};

BEGIN_SHADER_PARAMETER_STRUCT(FUniSplatResolvePassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FUniSplatResolveParameters, Resolve)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

/** D-164: writes one mip of the web texture from a buffer of packed RGBA16F texels (UniWebTexUpload.usf). */
class FUniWebTexUploadCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FUniWebTexUploadCS);
	SHADER_USE_PARAMETER_STRUCT(FUniWebTexUploadCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InTexels)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, OutMip)
		SHADER_PARAMETER(uint32, MipSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) { return UniShadersEnabled() && IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5); }
};
