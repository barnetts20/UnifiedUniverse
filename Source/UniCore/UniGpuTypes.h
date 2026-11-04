#pragma once

// UniGpuTypes.h
// C++ mirrors of every struct the GPU reads or writes, in one place (lesson #22). Each is a
// flat multiple of 16 bytes with no member straddling a 16-byte boundary and carries a
// static_assert (lesson #20); verify_all's T4.static.gpu_structs check enforces the asserts
// for every struct marked GPU-MIRROR.
//
// Floats ride integer storage as bit patterns where the shader reads uint4 (lesson #20).

#include <cstdint>

namespace UniCore
{
	// GPU-MIRROR
	struct FObjectGPU
	{
		// Mirrors FUniObject in UniPlacement.ush: uint4 A, uint4 B. See the field table there.
		uint32_t A[4];
		uint32_t B[4];
	};
	static_assert(sizeof(FObjectGPU) == 32, "FObjectGPU must match FUniObject (UniPlacement.ush)");

	// GPU-MIRROR
	struct FCellGPU
	{
		// Per resident cell, uploaded every frame for project-and-cull (D-104):
		// exact two's-complement delta CellOrigin - CameraCoord per axis, then bookkeeping.
		uint32_t DeltaX[4];
		uint32_t DeltaY[4];
		uint32_t DeltaZ[4];
		uint32_t Level;
		uint32_t PoolStart;
		uint32_t PoolCount;
		uint32_t Flags;       // bit 0: level suspended (velocity LOD), bits 8..15: level fade 0..255
	};
	static_assert(sizeof(FCellGPU) == 64, "FCellGPU must match FUniCellGPU (UniProject.usf)");

	// GPU-MIRROR
	struct FFieldFrameGPU
	{
		// Mirrors FUniFieldFrame in UniFields.ush.
		float OriginP[3];
		float SizeP;
		float Rot0[3];
		uint32_t Periodic;
		float Rot1[3];
		float Pad0;
		float Rot2[3];
		float Pad1;
	};
	static_assert(sizeof(FFieldFrameGPU) == 64, "FFieldFrameGPU must match FUniFieldFrame (UniFields.ush)");

	// GPU-MIRROR
	struct FFieldParamsGPU
	{
		// Mirrors FUniFieldParams in UniFields.ush: eight float4 (D-178: was four).
		float V[32];
	};
	static_assert(sizeof(FFieldParamsGPU) == 128, "FFieldParamsGPU must match FUniFieldParams (UniFields.ush)");

	// GPU-MIRROR
	struct FGenTaskGPU
	{
		// One (cell, contributor, emitter) generation task. Geometry and identity only:
		// the shader probes the field and derives envelope, integral, lambda and count itself
		// (UltraLargeScale's FUniverseGenCell rule: nothing about the field is filled in here).
		FFieldFrameGPU Frame;            // 64
		FFieldParamsGPU Params;          // 128 (D-178)
		uint32_t CellSeed;               // Uni_CellSeed(...)
		uint32_t FieldId;
		uint32_t ChildArchetypeId;
		uint32_t SliceIndex;             // into the slice table (P(L), CDF interval)
		float LambdaScale;               // N_total * P(L) * cellVolume / integralAll (field units)
		uint32_t PoolBase;               // first object index for this task in the batch output
		uint32_t PoolCapacity;           // objects this task may write (per-cell truncation keeps lowest candidates)
		uint32_t IndexBase;              // object index within its cell for the first accepted object
		float Suppress[4];               // suppressor centre (cell units, from the cell origin) and radius; w <= 0: none
		uint32_t BoxLo[4];               // D-153: sampling box, in-cell offsets (xyz); Span 0 = whole axis
		uint32_t BoxSpan[4];
	};
	static_assert(sizeof(FGenTaskGPU) == 272, "FGenTaskGPU must match FUniGenTask (UniGenTask.ush)");

	// GPU-MIRROR
	struct FTaskCountGPU
	{
		// Mirrors FUniTaskCount (UniGenTask.ush): probe-phase result, read back per task.
		uint32_t Count;
		uint32_t CandidateBudget;
		float Mean;
		float Envelope;
	};
	static_assert(sizeof(FTaskCountGPU) == 16, "FTaskCountGPU must match FUniTaskCount (UniGenTask.ush)");

	// GPU-MIRROR
	struct FSliceGPU
	{
		// One (emitter, level) luminosity slice.
		float CdfLo;
		float CdfHi;
		uint32_t LutBase;
		uint32_t LutCount;
		float RadiusModel[4];            // log10 R = x + y (M - z) + w (2u - 1)
		float ColourModel[4];            // log10 T = x + y (M - z) + w (2u - 1)
		uint32_t ChildFlags;             // UNI_OBJ_FLAG_* set on every child (extended, has-contributor)
		uint32_t Pad;
		float BrightM;                   // slice magnitude interval: draws are clamped to it (D-117)
		float FaintM;
	};
	static_assert(sizeof(FSliceGPU) == 64, "FSliceGPU must match FUniSlice (UniGenTask.ush)");
}
