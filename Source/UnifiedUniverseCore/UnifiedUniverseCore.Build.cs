// UnifiedUniverseCore: FUniCoord and the Blueprint-facing coordinate, cell and object-id
// types, UniSeed, archetype/emitter data, CPU resolve. Wraps the engine-agnostic
// Source/UniCore headers (D-015), which are header-only and are reached through the
// include paths below rather than built as a module of their own.

using System.IO;
using UnrealBuildTool;

public class UnifiedUniverseCore : ModuleRules
{
	public UnifiedUniverseCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// The engine-agnostic UniCore headers were built offline with g++ and clang only. Shadowed
		// names there must not fail an MSVC build tonight; tighten once UE-1 is green.
#if UE_5_5_OR_LATER
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Warning;
#else
		ShadowVariableWarningLevel = WarningLevel.Warning;
#endif

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		// Header-only engine-agnostic core and the shared shader cores it compiles as C++.
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UniCore"));
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "..", "Shaders", "Private", "Core"));
	}
}
