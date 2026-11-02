# Build triage: Unified Universe, first build

*For tonight's first compile (REVIEW_TONIGHT §3). Nothing in the plugin has been compiled yet. The list below is what two independent reviews and the session flagged as most likely to break, with the fix for each. Work from the top.*

## 0. If the editor will not start

A global shader that fails to compile can stop the editor during startup ("Failed to compile global shader ..."). The plugin has an escape hatch:

- **Command line, once:** add `-ini:Engine:[SystemSettings]:uu.Shaders=0` to the editor shortcut or the command.
- **Or persistently:** add these lines to `Config/DefaultEngine.ini`. This is a project file; I have not touched it, and you can remove the lines afterwards.
  ```
  [SystemSettings]
  uu.Shaders=0
  ```

With `uu.Shaders=0` none of the plugin's shaders is compiled. Every pass and generation batch is skipped (batches fail closed), and `uu.Enable` logs an error instead of starting. The editor opens, so you can read the shader errors and fix them. Remove the setting when they are fixed.

**Unverified:** I could not run the engine here. If the `-ini:` override is ignored on your version, use the ini lines.

**To see shader errors in full:** `r.ShaderDevelopmentMode=1` (same `[SystemSettings]` section) makes the engine print the error text and offer a retry instead of failing. The log is `Saved/Logs/<Project>.log`; search for `UniGen.usf`, `UniProject.usf` or `UniSprite.usf`.

## 1. C++ (UE-1)

Roughly most likely first.

| Symptom | Where | Cause | Fix |
| --- | --- | --- | --- |
| `SubscribeToPostProcessingPass`: "member function declared with 'override' does not override" | `UniRenderHooks.cpp` | The signature changed between versions; the guard is `UE_VERSION_NEWER_THAN(5, 4, 99)` | Swap which branch of the `#if` your version takes, or copy the signature from `SceneViewExtension.h` in your engine |
| `SceneRendering.h`: cannot open include file | `UniRenderHooks.cpp` (FViewInfo) | Renderer private headers are not on the include path | In `UnifiedUniverseRender.Build.cs`, check that the `Renderer/Private` path (and `Renderer/Internal` on 5.4+) points at your engine's folders |
| `GRHIGlobals` undeclared | `UniRenderer.cpp` | It only exists from 5.3 | Guarded: older versions use `GRHIMaxDispatchThreadGroupsPerDimension` |
| `CopyFromSlice` / `FScreenPassTextureSlice` unknown | `UniRenderHooks.cpp` | Only in 5.3+ | Guarded by `UE_VERSION_NEWER_THAN(5, 2, 99)` |
| `FRHIGPUBufferReadback`: unresolved external | Render module link | Missing `RHICore` | Already in `PublicDependencyModuleNames`; check it is present |
| `ShadowVariableWarningLevel` obsolete / unknown | any `*.Build.cs` | Moved to `CppCompileWarningSettings` in 5.5 | Guarded by `#if UE_5_5_OR_LATER`. If your UBT predates the define, delete the line |
| C4458 / shadowing *warnings* in `UniCore` headers | MSVC | Offline builds were g++/clang | Set to warnings, not errors. Ignore tonight |
| `AddEnqueueCopyPass`: no matching overload | `UniRenderer.cpp` | The readback parameter type differs per version | Pass `Readback` (pointer) and `Buffer`; for buffers the size argument is in bytes |
| `CreateStructuredBuffer` with `ERDGInitialDataFlags` | `UniRenderer.cpp` | Overload set differs | Use the `(GraphBuilder, Name, BytesPerElement, NumElements, Data, Bytes)` form; default flags copy the data |
| UHT: "incomplete type" in `UniSubsystem.gen.cpp` | UnifiedUniverse module | `TUniquePtr` of a forward-declared type | Fixed (B1): `UniStreamer.h` is included before `.generated.h`. If it recurs, include the type's header there |
| `FUintVector2` not a shader parameter type | `UniShaders.h` (volume branch only) | Probably fine on 5.x | Use `FIntPoint` in C++ and `int2` in HLSL |

## 2. Shaders (UE-3)

| Symptom | File | Fix |
| --- | --- | --- |
| `X4026: thread sync operation must be in non-varying flow control` (FXC/SM5) | `UniGen.usf`, place pass | Delete the marked `if (GsBase >= Capacity) { break; }` line. Results do not change |
| `Core/...` include not found | any `.usf` | Includes are relative to the including file (`/Plugin/UnifiedUniverse/Private/Core/...`). Check `AddShaderSourceDirectoryMapping` ran: log line "UnifiedUniverse plugin not found" means the plugin directory did not resolve |
| "implicit truncation of vector type" as an error | cores | Make the swizzle explicit (`.xyz`), or construct the narrower type |
| `StructuredBuffer` as a function parameter | `UniPlacement.ush`, `UniGenTask.ush` | Legal HLSL (resource parameters are inlined). If a compiler rejects it, pass the buffer as a global and drop the parameter |
| `Uni_Luminance` undeclared (volume branch) | `UniVolume.usf` | Fixed after the volume review: the function lives in `UniPhotometry.ush` |
| X3550 / X3511 (array addressing, FXC) | `UniVolume.ush` (volume branch) | Fixed: slices are written with static indices |

## 3. First run

| Symptom | Likely cause | What to do |
| --- | --- | --- |
| Black sky, `uu.Stats` "cells ready" 0, "failed" > 0 | Generation batches timing out | Check the log for "generation batch ... timed out". Usually a shader compile problem: see §2 |
| Cells ready, nothing drawn | The sprite pass or the hook | RenderDoc: look for the "UnifiedUniverse" event scope after MotionBlur. Check that `UniRenderHooks` logged "scene view extension registered" |
| Stars drawn over meshes | Depth-UV mapping after TSR | Try `r.ScreenPercentage 100` |
| Stars shimmer | The callback runs before TSR on your version | Tell me which version; the hook point moves |
| Cannot get anywhere / `uu.Cruise` does nothing | The default pawn is ~12 m/s; old `uu.Cruise` ignored speeds < 1e8 cm/s | `uu.Fly`, then the mouse wheel or `uu.Speed <cm/s>` |
| Free flight does not move | No key held (motion only while W/A/S/D/Q/E is down), or the console has focus | Close the console; hold W |
| Wildly over- or under-bright | Uncalibrated (lesson #36) | `uu.Exposure ±3`; disable auto-exposure in an empty level |
| Volume branch: NaN/white blow-up | Radiance overflow | Fixed (float32 targets). If it recurs: `uu.Volume 0` and report |
| Volume branch: GPU hang / TDR (driver timeout) | Dense scene, supersampling × 512 steps | `uu.VolumeDivisor 4`, `uu.BackgroundFacesPerFrame 1` |
| Volume branch: glow looks wrong (position, structure, brightness) | Varies | Debug views: `uu.VolumeDebug 1` (steps per ray; magenta = truncated), `2` (red = carriers hit, green = supersampling, blue = truncated), `3` (emission, log heat; shift it with `uu.VolumeDebugLogLo`), `4` (dust transmittance). `0` is off |

## 4. GPU parity (UE-4)

Run the new test `UnifiedUniverse.Render.GpuParity` **without** `-nullrhi`, from Session Frontend or:

```
UnrealEditor-Cmd.exe "D:\Projects\vxl\vxl.uproject" -ExecCmds="Automation RunTests UnifiedUniverse.Render;Quit" -unattended -log
```

It generates the solar-camera scene on the GPU and on the CPU side by side, then compares every ready cell object by object.

- **Pass:** counts equal in ≥ 99% of cells, and integer parts (offsets, archetype, flags, seed) identical in ≥ 99% of objects. Its info line gives the exact rates.
- **If it fails:**
  - Counts off everywhere: the probe pass or slice tables are wrong.
  - Counts right but offsets wrong: the place pass's scan.
  - Only float parts differ: expected (D-105).
- Run `uu.Disable` first: the test refuses to run while a universe is enabled, because it shares the GPU pool.

## 5. Star systems (volumetrics branch, D-142 to D-145)

| Symptom | Where | Fix |
| --- | --- | --- |
| `FCriticalSection` / `FScopeLock` undeclared | `UniRenderer.h` / `.cpp` | The includes `HAL/CriticalSection.h` and `Misc/ScopeLock.h` are added; if your version moved them, include `HAL/ThreadSafeCounter.h`, or replace with `FCriticalSection` from `CoreMinimal.h` |
| `AddClearUAVPass`: no overload for a structured buffer UAV | `RenderBackdrop_RenderThread` (activation index and record clears) | Delete those two clears; the count clear is the one that matters (only `Count` entries are read). If RDG then complains that the copy reads an unwritten buffer, keep them and clear with `AddClearUAVPass(GraphBuilder, UAV, 0u)` on a `PF_R32_UINT` typed view instead |
| Shader: `FUniObject` unknown in `UniProject.usf` | `RWStructuredBuffer<FUniObject> OutActivateRecord` | The struct comes from `Core/UniPlacement.ush` through `UniProject.ush`. Check the include order did not change |
| Planets never activate | `uu.Systems`: last readback 0 | `SystemArchetypeMask` 0 (the archetype lacks `bSystems`), or the readback ring never completes. Check the `Uni.ProjectAndCull` pass in RenderDoc for the `UniActivate` buffers |

## 6. M3 parallax and promotion (D-146, D-147)

| Symptom | Where | Fix |
| --- | --- | --- |
| `/Engine/BasicShapes/Sphere` not found at runtime (no sphere, log warning) | `AUniBodyActor` constructor | Engine content missing from a packaged build: add `/Engine/BasicShapes` to "Additional asset directories to cook". In the editor it always exists |
| `TObjectPtr` unknown | `UniBodyActor.h` | Engines before 5.0: use raw `UStaticMeshComponent*` with `UPROPERTY()` |
| `SpawnActorDeferred` / `FinishSpawning` signature | `UUniSubsystem::UpdateLightRig` | Use `World->SpawnActor<ADirectionalLight>()` and set mobility after; a warning about changing mobility is harmless here |
| Body actors destroyed right after spawning | World bounds check | Placements stay inside 7e10 cm (compressed) or 5e11 cm (real space). If your project shrank the world bounds, disable World Settings > Enable World Bounds Checks |
| Sphere is black | No light | `uu.LightRig 1` (default) needs an active system; `uu.LightRigLux 50` if auto-exposure darkens it |
| `UFloatingPawnMovement` unknown | `UniSubsystem.cpp` | Include `GameFramework/FloatingPawnMovement.h` (Engine module); it is present. With a custom pawn type it is simply not found and nothing is suppressed |
| `OnLocalRecentre` UHT error on the delegate | `UniSubsystem.h` | The `DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam` must sit above the `UCLASS`; it does. If UHT complains about the parameter name, rename `WorldShift` |
