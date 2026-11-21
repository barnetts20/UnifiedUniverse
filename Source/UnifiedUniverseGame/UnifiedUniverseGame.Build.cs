// UnifiedUniverseGame: the universe's game-side glue to the vxl plugins (D-293, SHIPS_PLANETS.md
// §4). It registers a body factory with UUniSubsystem that makes planets' detailed actors from
// VoxelPlanets' APlanetActor (terrain, ocean, atmosphere, gravity zone). The universe's own modules
// (UnifiedUniverseCore, UnifiedUniverseRender, UnifiedUniverse) stay independent of every other
// plugin; only this module depends on VoxelPlanets (and through it on GravPlugin, CloudAtmosphere
// and UltraLargeScale, which VoxelPlanets' public headers include).

using UnrealBuildTool;

public class UnifiedUniverseGame : ModuleRules
{
	public UnifiedUniverseGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;   // as VoxelPlanets (its headers are included here)

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
			"UnifiedUniverseCore",
			"UnifiedUniverse",
			"UnifiedUniverseRender",   // D-296: FUniRenderer::SetAtmosphereQuery
			"RenderCore",              // FRDGBuilder (the query's signature)
			"RHI",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"VoxelPlanets",         // APlanetActor, APlanetGravityZone, FVoxelWaterSample
			"GravPlugin",           // AGravityZone (BaseVector)
			"CloudAtmosphere",      // EPlanetAtmosphereType
			"UltraLargeScale",      // IPooledActor, IStarLit (APlanetActor's interfaces)
			"VoxelPlugin",          // reached by VoxelPlanets' public headers
			"RealtimeMeshComponent",
		});
	}
}
