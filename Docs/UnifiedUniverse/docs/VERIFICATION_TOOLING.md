# Unified Universe — Verification Tooling Spec

The owner cannot test in Unreal until late in the day. This tooling lets the implementing session prove its own progress, and gives the owner a single report to read tonight instead of re-deriving what works.

**Rule:** nothing is reported as done unless a check in this tooling passes for it, or it is explicitly listed as *unverified — needs UE* in `REVIEW_TONIGHT.md`.

## Layout

```
Plugins/UnifiedUniverse/
  Source/UniCore/                 engine-agnostic header-only C++17 (D-015)
  Shaders/Private/Core/*.ush      shared HLSL/C++ subset
  Shaders/Private/Core/UniHlslShim.h   makes the .ush cores compile as C++
  Tools/UniVerify/
    CMakeLists.txt or build.sh    builds with any C++17 compiler, no UE
    tests/                        unit + property tests (in-house macros)
    oracle/                       Python reference implementations
    scenes/                       scene definitions (camera paths, archetype sets)
    univerify (binary)            CLI: test | gen | render | metrics
    verify_all.sh|.py             runs everything, writes the report
  Verification/
    REPORT.md                     generated; pass/fail table, numbers, image links
    images/                       generated PNGs and contact sheets
```

## Tier 1 — Core correctness (no engine, no GPU)

| Check | What it proves | Pass criterion |
| --- | --- | --- |
| Coordinate fuzz | `Sub`, `AddDisplacement`, shifts, `CellOf`, `Neighbour`, wrap all match a Python big-int oracle | 10^6 random cases plus targeted edge cases (borrow chains across all four limbs, root wrap, max magnitude, zero) — zero mismatches |
| Hi/lo conversion | `ToHiLo` precision | Relative error ≤ 2^-46 over the fuzz set |
| Seed quality | `Avalanche`/`MixSeed` spread | Chi-squared over 64 bins within the 99% band for sequential roots (UltraLargeScale lesson) |
| Window maths | Toroidal slot bijection; resident coords keep their slot as the window slides | Exhaustive over windows N = 1..6 and random slides |
| Level assignment | `L = ceil(log2(R/N))` gives guaranteed coverage | For random camera offsets inside the centre cell, every object with R ≥ distance is inside its level's window |
| f(r) | Monotonic, C1 at r0, inverse exists | Sampled across 0..2^128 cm in log steps |
| Photometry | Apparent magnitude, fade band, sensitivity scaling `k = 10^(Δm/5)` | Matches closed form to 1e-6 |
| Shared shader cores | Each `.ush` core compiled as C++ through the shim | Builds; its CPU tests pass. Same source as GPU, so CPU results stand for GPU logic |

## Tier 2 — Generation behaviour (CPU reference pipeline)

`univerify gen --scene <name>` runs the full reference generator: level windows, contributor gather, cell integrals, Poisson counts, placement, luminosity slicing.

| Check | What it proves | Pass criterion |
| --- | --- | --- |
| Determinism | A cell's contents depend only on its key | Per-cell content hashes identical across: different batch orders, different window histories, revisiting after 10 window slides |
| Count statistics | Counts follow the field integral | Aggregate z-score of observed vs λ within ±3 over ≥ 10^4 cells |
| Slicing totals | Luminosity slicing conserves totals | Sum over levels of expected children ≈ `N_total` within 1% |
| Field fidelity | Objects sit on structure | Correlation between placed-object density and field value per cell > 0.9 |
| Contributor coverage | No child placed outside its contributor's influence | Zero violations |
| Budget | Residency under D-005 caps | Peak per-level and total counts logged; under caps for all scenes |
| Gather size | D-004 trigger | Max contributor list length per level logged |

## Tier 3 — Rendering behaviour (CPU reference renderer)

`univerify render` projects resident objects exactly as the GPU project-and-cull pass will (same shared core), splats sprites into an image, and writes PNG with a HUD.

| Check | What it proves | Pass criterion |
| --- | --- | --- |
| Jitter | Exact camera-relative maths | Camera parked at 10^30, 10^35 and 2^127 cm offsets, moved by 0.001 cm per frame for 100 frames: max screen-space error of static objects vs a double-double reference < 0.01 px |
| Ordering | Monotonic f(r) preserves depth order | For 10^5 random object pairs, compressed render depth order equals true distance order. Also demonstrate that per-depth compression would fail on the same set (documents the design claim) |
| Occlusion | Scene-depth test logic | Synthetic promoted occluder: backdrop objects behind it are culled, objects in front are kept, for all test placements |
| Flythrough | Seamless scale transitions | Scripted path from 10^28 cm altitude down to 1 m from an object; contact sheet of 48 frames; no frame-to-frame pop (count of objects entering above fade threshold = 0) |
| Visual sanity | Looks like structure | The session inspects the contact sheets itself and records observations in the report |

## Tier 4 — Unreal integration (only what the environment allows)

| Environment has | Do | Otherwise |
| --- | --- | --- |
| Unreal install matching the project | Build the plugin (`RunUAT BuildPlugin` or the project's editor target). Run Automation tests headless: `UnrealEditor-Cmd <proj> -ExecCmds="Automation RunTests UnifiedUniverse;Quit" -unattended -nullrhi -nosplash` | List as *unverified — needs UE* |
| DXC | Compile every `.usf` entry point offline with UE includes stubbed | List shader compile as *unverified — needs UE* |
| Neither | Static review of UE-facing code against engine headers if present in the repo | Everything UE-facing goes in REVIEW_TONIGHT with exact steps |

## The report

`verify_all` writes `Verification/REPORT.md`:

1. Header: commit hash, date and time, environment tier reached.
2. Pass/fail table for every check above, with the measured numbers.
3. Images: latest reference frames and contact sheets, linked.
4. Regressions since the previous run (keep the previous report as `REPORT.prev.md`).
5. A list of everything not verified, which must match REVIEW_TONIGHT.

Run `verify_all` before every commit that claims a step is complete.
