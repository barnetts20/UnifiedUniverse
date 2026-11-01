# Environment — 2026-11-01

| Item | Found | Notes |
| --- | --- | --- |
| Project root | **No** | The vxl project is on the owner's laptop at `D:\Projects\vxl` (seen as a folder name only). Folder access was declined, and the owner asked for this run to stay in the cloud workspace until they are home. Everything is built in a standalone workspace, `/home/claude/uu`, laid out as the project root (`Plugins/UnifiedUniverse/…`). |
| `.uproject` / EngineAssociation | **No** | Not readable. Folder names on the laptop (`UltraLargeScale 5.7`, `Universe 5.4 - Copy 5.5`) suggest UE 5.7. UE-facing code targets 5.5+ APIs, with `ENGINE_*_VERSION` guards where signatures moved (D-106). |
| UltraLargeScale present | Reference only | Cloned read-only from `github.com/barnetts20/UltraLargeScale` (commit `cb2ccc5`, 2026-08-30) to `/home/claude/ref/UltraLargeScale`, outside the workspace. The laptop's copy may be newer. It is never modified. |
| Unreal install usable headless | **No** | No engine here. `D:\Program Files\Epic Games` exists on the laptop, but access was declined. No UBT, RunUAT or UnrealEditor-Cmd. |
| C++17 compiler | Yes | g++ 13.3.0 (primary) and clang 18.1.3 (secondary, used to catch portability issues). |
| CMake | Yes | 3.28.3 |
| Python 3 | Yes | 3.13.18, with numpy 2.5 and Pillow 12.3 |
| DXC | **No** | The Linux release tarball downloads, but running a downloaded binary was refused by this session's policy. No workaround was attempted. Shader compilation goes to REVIEW_TONIGHT. |
| stb_image_write.h | Not vendored | It downloads, but under the same policy no external code is vendored. Images are written as PPM and converted to PNG with Pillow (D-018 fallback). |
| git | Yes | New repository in the workspace, branch `uu/prototype-day1` (D-019). This is not the vxl repo: the handover is a zip plus a patch that applies at the vxl root. |
| OS / shell | Linux x86-64, bash | 2 cores, 7 GB RAM. The owner's machine is Windows (MSVC), so the UE-facing code was never compiled here. The UniCore and shader-core headers build under both g++ and clang with `-Wall -Wextra -Werror`, but MSVC itself was not available. |

**Verification tier reached:** Tiers 1, 2 and 3 run fully offline. Tier 4 is static only: UE code is reviewed against the UltraLargeScale sources and engine API knowledge, but not against engine headers.

**Consequences for the plan**

- The UE build, Automation tests, shader compilation, the GPU dispatch, CPU/GPU parity and the test map are all *unverified — needs UE*, with exact steps in REVIEW_TONIGHT.
- The shared `.ush` cores are executed on the CPU through `UniHlslShim.h` and tested. So the logic is verified, while HLSL syntax acceptance by DXC/FXC is not.
- `HashCombine` and `FRandomStream` are re-implemented engine-free in the UniSeed port (D-015 forbids UE includes in UniCore). They are written from the UE 5 sources as I know them, and an Automation test compares them against the engine's own functions. That test is the first thing to run tonight (D-107).
- Test content (`.uasset` map, Blueprints) cannot be authored without the editor. The test map step is a console command on any empty level instead (REVIEW_TONIGHT §3).
