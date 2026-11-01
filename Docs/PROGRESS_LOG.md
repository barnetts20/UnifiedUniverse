# Progress log

Append only. One entry per step start, finish, block, or commit. Times in the owner's timezone (America/New_York).

| Time | Phase/step | Event | Detail |
| --- | --- | --- | --- |
| 09:56 | P0 | start | Kickoff received. Read the design doc, DECISIONS, VERIFICATION_TOOLING, Appendix A and the CLAUDE.md seed |
| 09:58 | P0 | blocked | Laptop folder access (`D:\Projects\vxl`, Epic Games) declined. Owner: work in the cloud space until home. Standalone workspace per the kickoff fallback |
| 10:02 | P0 | note | UltraLargeScale cloned read-only from GitHub (cb2ccc5) as reference |
| 10:03 | P0 | blocked | DXC binary download OK, execution refused by session policy. Shader compile → REVIEW_TONIGHT. stb not vendored, PPM→PNG via Pillow |
| 10:08 | P0 | note | Workspace `/home/claude/uu`, branch `uu/prototype-day1`, plugin skeleton and records |
| 10:10 | P1 | start | Shim, UniCoord.ush, UniCoord.h, harness, oracle, UE scaffold |
| 10:24 | P1 | decision D-104..D-108 | Exact per-cell deltas, parity scope, engine targeting, HashCombine re-impl, type names |
| 10:26 | P1 | done (gate green) | Tier 1 coordinate checks: fuzz 10^6 (3 implementations) 0 mismatches; Python oracle 10^6 0 mismatches (mutation-tested); hi/lo worst 2^-49; g++ and clang build. UE scaffold written, unbuilt (UE-1) |
| 10:27 | P2 | start | UniSeed, windows, level assignment, photometry, f(r) |
| 10:31 | P2 | decision D-109 | Measured MixSeed(CellSeed, index) collisions (5x / 260x birthday) → injective identity mixer; ProcSeed port kept for rolled params |
| 10:38 | P2 | note | Window test caught two real bugs (half-axis seam rule aliasing at 16-cell axes; seam mode tracked globally not per axis). Fixed: D-100 |
| 10:41 | P2 | decision D-100, D-110 | Window seam/coarse axes; condition-aware float tolerances |
| 10:44 | P2 | done (gate green) | 21 checks green: seeds (chi² in band, 0 object-seed collisions, Python oracle 2007 cases), windows (0 violations / 5184 slides), coverage (0 / 240k), f(r), photometry, clang parity |
| 10:44 | P2 | note | Log times for P2 corrected to the clock (earlier entries were estimated ahead of time) |
| 10:45 | P3 | start | Fields, placement core, archetype/slicing model, UniVol, reference generator + pipeline, scenes, Tier 2 |
| 10:56 | P3 | note | Tier 2 caught: anchors' level never activated (activation fix), rejection cap shortfall z=−12 (D-114), normaliser 4% high (D-115) |
| 11:00 | P3 | decision D-101..D-103, D-111..D-115 | Influence term, radius scatter, cell integral, luminosity-scaled N, tile symmetries, regeneration, candidate budget, normaliser |
| 11:03 | P3 | done (gate green) | 29 checks green. Tier 2 on web_to_galaxy and galaxy_to_star: determinism 54,864 cells 0 differ; counts z=−1.05 over 12,735 tasks; slicing 1.0021; fidelity r=0.919 / 0.983; coverage 298k children 0 outside; baked path |
| 11:03 | P3 | note | P3 log times corrected to the clock. From here every entry is stamped from the clock at the time of writing |
| 11:04 | P4 | start | Project-and-cull core (UniProject.ush), reference renderer, Tier 3 |
| 11:14 | P4 | note | First flythrough: 102,618 pops. Diagnosed and fixed in three steps: resume fades (D-116), slice LUT bug drawing bright stars up to 5 mag too bright (D-117, new check T2.gen.slice_draws), window lag needs N_eff = N − 1/K (D-118, HIGH reroll, owner decision) |
| 11:28 | P4 | note | Visual inspection: Milky-Way band correct; web filaments not legible at 65 points; renderer square-kernel artifact fixed; sprite→body handoff glare discontinuity → UE-5 |
| 11:35 | P4 | decision D-116..D-121 | Resume fades, per-slice LUT + clamp, N_eff, θ_promote, true-distance depth test, extended blend |
| 11:38 | P4 | done (gate green) | 36 checks green. Tier 3: jitter 2.6e-3 px worst (limit 0.01), ordering 0 reversed / per-depth 2,284 wrong, occlusion 0 wrong / 20k, flythrough 0 pops / 545,902 entering |
| 11:40 | P5 | start | Unreal integration. First: task decomposition core (UniGenTask.ush) and runtime streamer (UniStreamer.h) so the GPU protocol is verifiable offline |
| 11:48 | P5 | note | CPU reference refactored onto the GPU's probe → allocate → place phases; Tier 2 unchanged. T2.stream.parity: streamer + CPU backend = reference, 5,375 cells / 719,687 objects (caught anchor-only cells skipping allocation) |
| 11:51 | P5 | commit | Checkpoint (37 checks green); UE shaders and renderer written, unbuilt |
| 12:01 | P5 | commit | UE game module, render hooks, Automation tests, strict C++20 header check; handover drafted. 39 checks green. Independent review of UE code running |
| 12:09 | P6 | done (stretch 1) | Global sensitivity: new checks T3.sensitivity, T2.stream.sensitivity_resize caught blink/regenerate-on-resize; fixed (D-126). Ping shell not started |
| 12:19 | P5 | decision D-127 | UE review fixes applied: B1, B2, B4, B5, R1, R2/W5, R3, R4, W1, W2, W3, W4, W6, S1. New check T2.stream.fail_closed. Uncompiled (UE-1/3/5) |
| 12:24 | END | done (gate green) | Final verify_all: 42 checks, 0 failed (new: T2.stream.fail_closed). Committed; zip and patches packaged for D:\Projects\vxl |
