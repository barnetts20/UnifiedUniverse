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
| 11:12 | P3 | note | Tier 2 caught: anchors' level never activated (activation fix), rejection cap shortfall z=−12 (D-114), normaliser 4% high (D-115) |
| 11:21 | P3 | decision D-101..D-103, D-111..D-115 | Influence term, radius scatter, cell integral, luminosity-scaled N, tile symmetries, regeneration, candidate budget, normaliser |
| 11:24 | P3 | done (gate green) | 29 checks green. Tier 2 on web_to_galaxy and galaxy_to_star: determinism 54,864 cells 0 differ; counts z=−1.05 over 12,735 tasks; slicing 1.0021; fidelity r=0.919 / 0.983; coverage 298k children 0 outside; baked path |
