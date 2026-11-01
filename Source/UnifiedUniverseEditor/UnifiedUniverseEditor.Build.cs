// UnifiedUniverseEditor: baker asset import (UniVol, D-007), archetype validation, debug views.
// Day 1 holds only the module shell; the UniVol importer is P6 stretch work.

using UnrealBuildTool;

public class UnifiedUniverseEditor : ModuleRules
{
	public UnifiedUniverseEditor(ReadOnlyTargetRules Target) : base(Target)
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
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"UnifiedUniverseCore",
		});
	}
}
