#pragma once

// UniShaderCores.h
// The GPU shader cores (Shaders/Private/Core/*.ush), compiled as C++ through UniHlslShim.h.
//
// This is how the CPU reference pipeline and the tests run THE SAME SOURCE the GPU runs
// (D-015, lesson #1). Nothing in UniCore re-implements a shader core's logic for its own
// use. UniCoord.h is the exception by design: it is an independent implementation of the
// coordinate API, cross-checked against UniCoord.ush in the harness.
//
// Include path requirements: Shaders/Private/Core must be on the include path (CMake in
// Tools/UniVerify, and PublicIncludePaths in UnifiedUniverseCore.Build.cs).

#include "UniHlslShim.h"

namespace UniHlsl
{
#include "UniCoord.ush"
#include "UniSeed.ush"
#include "UniPhotometry.ush"
}

#include "UniCoord.h"

namespace UniCore
{
	/** FU128 (L[0] most significant) to the shader's uint4 (.x most significant). */
	inline UniHlsl::uint4 ToUint4(const FU128& A) { return UniHlsl::uint4(A.L[0], A.L[1], A.L[2], A.L[3]); }
	inline FU128 FromUint4(const UniHlsl::uint4& A) { return FU128::Make(A.x, A.y, A.z, A.w); }
}
