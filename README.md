# Unified Universe

An Unreal Engine 5 plugin for a single procedural universe larger than the observable one. It has:
- 128-bit universal coordinates (`FUniCoord`, 1 cm floor, wrapping on a 2^128 cm torus);
- implicit octree scale levels with toroidal streaming windows;
- archetype / emitter / field generation on the GPU;
- a radially compressed backdrop renderer (sprites, plus a volume pass on the `volumetrics` branch).

**Status: prototype.** The engine-agnostic core is verified offline (`Verification/REPORT.md`). The Unreal side is being built and tested for the first time; see `Docs/REVIEW_TONIGHT.md`.

## Layout

| Path | What |
| --- | --- |
| `Source/UniCore/` | Engine-agnostic core, header-only C++ (no UE includes) |
| `Shaders/Private/Core/` | Shader cores shared by the GPU and the CPU reference (compiled as C++ through `UniHlslShim.h`) |
| `Source/UnifiedUniverseCore/`, `UnifiedUniverseRender/`, `UnifiedUniverse/`, `UnifiedUniverseEditor/` | The UE modules |
| `Tools/UniVerify/` | Offline verification harness: `python3 Tools/UniVerify/verify_all.py` builds it and writes `Verification/REPORT.md` |
| `Docs/` | Design (`UnifiedUniverse_Design.md`), decisions (`DECISIONS.md`), handover (`REVIEW_TONIGHT.md`), build triage (`BUILD_TRIAGE.md`), volumetrics design (on `volumetrics`) |
| `CLAUDE.md` | Working agreement for sessions on this plugin |

## Use

1. Clone into a project's `Plugins/UnifiedUniverse`.
2. Build the editor target.
3. In an empty level, press Play and run `uu.Enable galaxy`.
4. Move with `uu.Fly` (WASD/QE, mouse wheel for speed) and run `uu.Flythrough 60` for the scripted dive.
5. `uu.Stats` reports streaming and render state.

If the editor will not start because of a shader error, see `Docs/BUILD_TRIAGE.md` (`uu.Shaders=0`).

## Branches

- `main`: day-1 prototype (streaming, generation, sprites, test controls).
- `volumetrics`: `main` plus the volume pass (`Docs/VOLUMETRICS.md`): unresolved light, dust and the extragalactic background.
