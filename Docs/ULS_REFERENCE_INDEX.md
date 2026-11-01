# UltraLargeScale reference index

Read-only reference for the Unified Universe prototype. Find these in the project's UltraLargeScale plugin (`Source/` and `Shaders/`). Never modify them.

| Need | UltraLargeScale file(s) | What to take |
| --- | --- | --- |
| Seed mechanics | `ProceduralSpaceActor.h` (`namespace ProcSeed`) | Port exactly: `ChannelId` (both forms, name uppercased), `Avalanche`, `MixSeed`, `Stream`. Keep the comments on why. |
| Toroidal window maths | `FTierStreamingSystem.h` (`PosMod`, `SlotOf`, `FlattenResidues`) | Slot bijection over a sliding window |
| Child subdivision / placement keys | `FTierStreamingSystem.h` (`SubdivideCells`, `ETierChildCoords`) | Exact-tiling rule; coord is the placement key |
| Scale readout | `ULSScaleLadder.h` | Port as `UniScaleLadder`, unchanged in behaviour |
| GPU dispatch and readback pattern | `UniverseEntityGen.h/.cpp`, `GalaxyEntityGen.h/.cpp` | Request object, readback allocation, game-thread enqueue, single payload, 2D group wrapping, counter layout, timeout |
| Probe / generate passes | `UniverseEntityGen.usf`, `GalaxyEntityGen.usf`, `UniversePlacement.ush`, `GalaxyPlacement.ush` | Probe-envelope-budget structure, compaction, per-slot ceilings |
| Calibration | `UniverseDataGenerator.cpp`, `GalaxyDataGenerator.cpp` | Scattered sampling, percentile divisor, cache keyed per tier |
| Field marshalling | `UniverseParams.h` (`Pack`, `FillShaderParameters`), `GalaxyParams.h` | One marshal site; raw inputs, derivation in shader |
| Archetype authoring | `GalaxyArchetype.h/.cpp` | Exemplar → ranges → bags → logic hook; name-keyed streams; Validate |
| Cosmic web field (M2b port) | `UniverseDensityCore.ush`, `UniverseDensityRaymarch.ush` | Needs `/UniverseNoisePack` volumes |
| Galaxy field (M2b port) | `GalaxyDensityCore.ush`, `GalaxyDensityRaymarch.ush` | Needs the galaxy volume textures |
| Actor pooling | `ActorPoolManager.h/.cpp`, `PooledActor.h/.cpp` | Generic pool; acquire by registered token (lesson #29) |
| Real-space wrapper | `ParallaxProxyActor.h/.cpp` | How a wrapped body is spawned and kept warm across pooling |
| Module setup | `UltraLargeScale.cpp`, `UltraLargeScale.Build.cs` | Guarded shader directory mapping; module dependencies |
