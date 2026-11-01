// UnifiedUniverseRender: shader directory mapping, the scene view extension, the generation
// and project-and-cull RDG passes, and the GPU buffers.
//
// LoadingPhase is PostConfigInit (see the .uplugin) so the /Plugin/UnifiedUniverse shader
// mapping exists before any global shader is compiled. For that reason this module does NOT
// depend on UnifiedUniverseCore (a UObject module loaded at Default): it reaches the
// engine-agnostic UniCore headers directly through include paths, and the game module pushes
// plain-C++ frame data into it.

using System.IO;
using UnrealBuildTool;

public class UnifiedUniverseRender : ModuleRules
{
	public UnifiedUniverseRender(ReadOnlyTargetRules Target) : base(Target)
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
			"RenderCore",
			"RHI",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Renderer",
			"Projects",
			"RHICore",   // Renderer-private headers (SceneRendering.h) reach into RHICore
		});

		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UniCore"));
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "..", "Shaders", "Private", "Core"));

		// FViewInfo (internal view rect, UniRenderHooks.cpp) lives in the Renderer's private
		// headers. A common plugin pattern; it is the most version-fragile line in the plugin.
		PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("Renderer"), "Private"));
		PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("Renderer"), "Internal"));
	}
}
