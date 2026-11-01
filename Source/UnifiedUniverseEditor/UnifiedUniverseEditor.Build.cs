// UnifiedUniverseEditor: baker asset import (UniVol, D-007), archetype validation, debug views.
// Day 1 holds only the module shell; the UniVol importer is P6 stretch work.

using UnrealBuildTool;

public class UnifiedUniverseEditor : ModuleRules
{
	public UnifiedUniverseEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"UnifiedUniverseCore",
		});
	}
}
