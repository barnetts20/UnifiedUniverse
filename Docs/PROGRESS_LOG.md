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
| 14:46 | VOL | start | Owner (on phone): "devise our density raymarching scheme". Branch uu/volumetrics on top of day 1 |
| 15:00 | VOL | note | Design draft Docs/VOLUMETRICS.md (also in the claude.ai project). Found the demo luminosity budget mismatch: 2e11 stars = M −23.18 vs drawn −21.00 (D-133) |
| 15:20 | VOL | note | UniVolume.ush core, UniVolume.h builder, reference renderer. T3.vol.march green after tightening galaxy step hints (2.5% → 0.8%); T3.vol.conservation green first time (≤ 0.17%) |
| 15:30 | VOL | note | Handoff check caught one-ray-per-pixel mis-integration of sub-pixel carriers (12–17%) → supersampling by ResolveScale (0.84%). Luminance-normalised colour (D-135) |
| 15:45 | VOL | note | Flythrough with volume caught 93k truncated rays: background prefilter keyed on the capped step never engaged; background started at 1 pc. Fixed (D-130, D-131): 0 truncated, 124 steps/ray |
| 15:50 | VOL | decision D-128..D-135 | Volume scheme decisions recorded |
| 16:46 | VOL | start | Owner: "Sure, and any tips on universe-level background density marching?" Tips sent; engine pass started |
| 17:05 | VOL | note | Builder reads FVolumeSource (pipeline or streamer). Streamer fade-hold rule (D-128 amendment): T2.stream.volume_parity caught new levels ticking to 0.125 before their window existed; fixed |
| 17:15 | VOL | note | Background direction map + separable composite (D-136): T3.vol.cube 0.000% total, p99 ≤ 0.02% |
| 17:30 | VOL | note | Engine pass written (UniVolume.usf, sprite slices, RenderVolume_RenderThread, PushFrame, uu.Volume); independent review requested. Uncompiled (UE-9) |
| 17:38 | VOL | done (gate green) | Review fixes applied (Uni_Luminance visibility, float32 radiance targets, FLinearColor clears, FXC-safe slices, LUT cost); gate 53/53; committed and packaged |
| 17:45 | SAFE | start | Owner: stage things while away. Escape hatch, GPU parity test, triage sheet first (day-1 branch), then volume debug views, then a decision brief |
| 17:48 | SAFE | note | uu.Shaders read-only cvar gates every shader's ShouldCompilePermutation; passes and batches skip and fail closed. UnifiedUniverse.Render.GpuParity (UE-4): GPU vs CPU streamer, cell by cell. Docs/BUILD_TRIAGE.md. GRHIGlobals guarded for < 5.3. Uncompiled |
| 17:50 | SAFE | note | Log times for 17:30–17:48 corrected to the clock (two entries had been stamped ahead) |
| 17:58 | SAFE | note | Decision brief published (D-133, D-132, D-131, D-118 with offline renders) |
| 19:42 | CTRL | note | Owner: speed control is needed to test. Added uu.Fly (steerable free flight, WASD/QE, wheel x2/x0.5, any speed), uu.Speed, latched uu.Cruise (was ignored below 1e8 cm/s), HUD readout; InputCore dependency. Uncompiled |
| 20:10 | UE57 | done (UE-1 compiles, shaders compile) | First engine build on UE 5.7: EVisibility/FEmitter clashed with engine globals (renamed EUniVisibility/FUniEmitter), fopen C4996 (UniFOpen), RHI/RenderCore link deps for the GPU parity test, DXC rejects ?: on structs (UniGen.usf). Owner confirms the galaxy renders |
