#pragma once

// UniVerifyCommon.h
// Run options shared by every univerify command.

#include <string>

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
}
