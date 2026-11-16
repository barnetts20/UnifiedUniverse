#pragma once

// UniVerifyCommon.h
// Run options shared by every univerify command.

#include <string>

#include "UniHlslShim.h"

namespace UniVerify
{
	struct FOptions
	{
		/** Random cases per fuzz test. The tooling spec requires 10^6; --quick lowers it for
		 *  iteration, and a quick run can never report the fuzz check green. */
		int FuzzCases = 1000000;
		int FullScale = 1;

		/** Where generated artefacts (images, dumps, scene logs) go. */
		std::string OutDir = "Verification/work";
		std::string ImageDir = "Verification/images";
		std::string SceneDir = "Tools/UniVerify/scenes";
	};

	inline FOptions& Options() { static FOptions O; return O; }

	/** D-262: the nebula layer's checks measure its own machinery with D-258's galaxy shaping off
	 *  (UNI_GAL_SHAPE 0: D-179's linear knots, no structure). With the shaping the grid's cell-
	 *  averaged dust against the per-sample knots' young light undercounts the reflection nebulae
	 *  (B-041; the layer is off by default, D-260); paradigm E rebuilds the nebulae's bricks. */
	struct FGalShapeOff
	{
		UniHlsl::float4 Saved = UniHlsl::GGalShape;
		FGalShapeOff() { UniHlsl::GGalShape = UniHlsl::float4(0.0f, 0.0f, 0.0f, 0.0f); }
		~FGalShapeOff() { UniHlsl::GGalShape = Saved; }
		FGalShapeOff(const FGalShapeOff&) = delete;
		FGalShapeOff& operator=(const FGalShapeOff&) = delete;
	};
}
