# Unified Universe — working agreement

Place this file at `Plugins/UnifiedUniverse/CLAUDE.md`. It applies to every session that works on this plugin.

## What this plugin is
A single-system procedural universe: 128-bit universal coordinates (`FUniCoord`), implicit octree scale levels, per-level toroidal streaming windows, archetype/emitter/field generation on the GPU, and a radially compressed backdrop renderer. The design lives in `Docs/UnifiedUniverse_Design.md`; open decisions live in `Docs/DECISIONS.md`.

## Hard rules
1. **Never modify UltraLargeScale** or anything outside this plugin without the owner's explicit approval. It is the A/B reference.
2. **Complete files, not snippets**, when changing project files.
3. **Absolute positions are never `FVector` or float.** Use `FUniCoord`, or an integer cell plus a fraction. `FVector` is only for positions inside the real-space bubble, relative to the local anchor.
4. **One field source.** Render and placement evaluate the same shader core. No CPU stand-in field, no silent fallback; generation fails closed and blanks the affected slots.
5. **GPU structs** are flat multiples of 16 bytes, with no float3 straddling a 16-byte boundary, and a `static_assert` on the C++ size.
6. **Seeds:** add channels, never reuse or repurpose them. Rolled parameters key their stream by name.
7. **Fades are render-only.** Bounds fades and sensitivity fades never change what is generated.
8. **Engine-agnostic core.** Anything testable without Unreal goes in `Source/UniCore/` (no UE includes). Shader cores in `Shaders/Private/Core/` must compile as C++ through `UniHlslShim.h`.
9. **Render hooks live in one file**, `UniRenderHooks.cpp`.
10. **Nothing is "done" without a passing check** in `Tools/UniVerify`, or an explicit entry in `Docs/REVIEW_TONIGHT.md` saying it needs UE testing.

## Before writing GPU, seed or field code
Read Appendix A of the design doc (inherited lessons). Lessons #19–#28 cover the dispatch and readback traps that cost UltraLargeScale the most time.

## Records
- Every decision not covered by the design: append to `Docs/DECISIONS.md` (D-100+), including reroll impact.
- Progress: `Docs/PROGRESS_LOG.md`, timestamped.
- Handover: keep `Docs/REVIEW_TONIGHT.md` current at all times.
- Run `Tools/UniVerify/verify_all` before any commit that claims a step is complete.
