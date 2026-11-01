// UnifiedUniverse: UUniSubsystem (camera, level windows, generation scheduling, promotion
// readback), the flythrough driver actor, and the A/B test entry points.

using UnrealBuildTool;

public class UnifiedUniverse : ModuleRules
{
	public UnifiedUniverse(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

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
