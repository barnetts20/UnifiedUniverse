// UnifiedUniverse: UUniSubsystem (camera, level windows, generation scheduling, promotion
// readback), the flythrough driver actor, and the A/B test entry points.

using UnrealBuildTool;

public class UnifiedUniverse : ModuleRules
{
	public UnifiedUniverse(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// The engine-agnostic UniCore headers were built offline with g++ and clang only. Shadowed
		// names there must not fail an MSVC build tonight; tighten once UE-1 is green.
		ShadowVariableWarningLevel = WarningLevel.Warning;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnifiedUniverseCore",
			"UnifiedUniverseRender",
		});
	}
}
