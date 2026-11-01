# Unified Universe — Decision Log

Every decision taken without the owner's sign-off is recorded here so it can be reviewed at integration time. Nothing in this file is final until the owner marks it **Accepted**.

**Status values**
- **Provisional** — chosen by Claude, in use, awaiting owner review.
- **Accepted** — owner reviewed and agreed.
- **Superseded by D-xxx** — replaced; keep the entry, never delete it.

**How to add an entry.** Append at the end, next free ID. Decisions made during implementation start at **D-100**. Use the same fields. If a decision changes generated content (seeds, placement keys, coordinate layout), say so under *Reroll impact* — those are expensive to reverse once content ships.

---

## Decisions taken at kickoff (answers to the design doc's open questions and implied choices)

### D-001 — Naming and module split
- **Question:** Is the `Uni` prefix and four-module split right for the vxl umbrella? *(Design doc → Risks and open questions)*
- **Decision:** Plugin `UnifiedUniverse`. Type prefix `Uni` (`FUniCoord`, `UUniArchetype`, `UUniSubsystem`). Modules: `UnifiedUniverseCore`, `UnifiedUniverseRender`, `UnifiedUniverse`, `UnifiedUniverseEditor`, plus an engine-agnostic source tree `Source/UniCore/` (see D-015). Shader virtual path `/Plugin/UnifiedUniverse`.
- **Rationale:** Distinct from UltraLargeScale's `ULS` and from engine types; short enough to read well.
- **Reroll impact:** None.
- **Revisit trigger:** Owner prefers a vxl-wide prefix.
- **Status:** Provisional

### D-002 — Coordinate width and floor
- **Question:** Is a 1 cm floor enough, or is a fifth, sub-cm limb needed?
- **Decision:** Four uint32 limbs per axis, 1 cm resolution, 2^128 cm span, root wraps as a 3-torus. Nominal origin at 2^127 on each axis. No fifth limb.
- **Rationale:** Sub-cm detail only matters inside the real-space bubble, where promoted actors already use `FVector` relative to a local anchor.
- **Reroll impact:** HIGH if changed later — limb count is part of every cell key and seed.
- **Revisit trigger:** Any system needs persistent absolute positions finer than 1 cm.
- **Status:** Provisional

### D-003 — Orbital motion
- **Question:** Static placement, or deterministic Kepler motion from seed plus time?
- **Decision:** Static placement for the prototype. When motion arrives, it lives inside an object's local frame as `f(seed, time)`. The parent object's identity and cell placement never move.
- **Rationale:** Keeps `(L, cell, index)` → position a pure function, which generation, persistence and resolve all depend on.
- **Reroll impact:** None now. Choosing frame-local motion later avoids any.
- **Revisit trigger:** Start of M3, or any gameplay need for visible orbits.
- **Status:** Provisional

### D-004 — Contributor gather strategy
- **Question:** Is a per-level contributor list enough, or is a spatial hash needed?
- **Decision:** Per-level compact list with a sphere-versus-cell test per entry. Log the maximum list length per level in every verification run.
- **Rationale:** The camera is inside only a handful of structures at any scale. Simplest correct approach first.
- **Reroll impact:** None (must not change results, only speed).
- **Revisit trigger:** Any level exceeds 256 contributors in a test scene.
- **Status:** Provisional

### D-005 — Object pool size and caps
- **Question:** Object pool size and per-level caps.
- **Decision:** Global pool of 2,097,152 objects (64 MB at 32 bytes). Soft cap 131,072 per level. Degradation order from the spec: pull Δm toward 0, suspend deepest levels, truncate per-cell counts (logged).
- **Rationale:** Comfortably above UltraLargeScale's resident counts; small enough for any target GPU.
- **Reroll impact:** None, as long as truncation keeps lowest candidate indices (deterministic).
- **Revisit trigger:** M2 tuning numbers.
- **Status:** Provisional

### D-006 — Target engine version and render hooks
- **Question:** Which engine version fixes the post-processing subscription point?
- **Decision:** Target whatever `EngineAssociation` the project's `.uproject` names. All scene view extension and post-process subscription code lives in one file (`UniRenderHooks.cpp`). Composite subscribes after motion blur (post-TSR, pre-bloom/tonemap); confirm against the engine source if available, otherwise flag in REVIEW_TONIGHT.
- **Rationale:** Isolates the most engine-version-sensitive code.
- **Reroll impact:** None.
- **Revisit trigger:** Hook signature mismatch on compile, or wrong pass order observed in UE.
- **Status:** Provisional

### D-007 — Noise baker format (interim)
- **Question:** What is the noise baker's export format, and can it bake periodic volumes?
- **Decision:** Until the baker's format is known, use an interim raw format, **UniVol**: little-endian header `{ char[4] "UVOL"; uint32 version=1; uint32 sizeX, sizeY, sizeZ; uint32 channels; uint32 format (0 = R16F, 1 = R32F); uint32 flags (bit 0 = periodic); uint32 mipCount }` followed by tightly packed mip 0..N data. The prototype's tooling generates periodic test volumes in this format; an editor importer for UniVol comes with M2. A native baker importer replaces or wraps it later.
- **Rationale:** Unblocks baked-field work without the baker; trivially convertible.
- **Reroll impact:** None (format only).
- **Revisit trigger:** Owner supplies the baker's export format.
- **Status:** Provisional

### D-008 — Motion modes
- **Question:** Keep Local + Cruise, or stay pinned everywhere like UltraLargeScale? *(Also an open comment on the design doc.)*
- **Decision:** Implement both modes as specced. Switch to Cruise above 1e8 cm/s (1,000 km/s, where the pawn would cross the 1e8 cm anchor zone in about a second), back to Local below 5e7 cm/s (hysteresis).
- **Rationale:** Conventional physics near objects; no precision limit when travelling.
- **Reroll impact:** None.
- **Revisit trigger:** Owner's answer on the design doc comment.
- **Status:** Provisional — owner question outstanding

### D-009 — r0 and the compression knee
- **Decision:** r0 = 1e9 cm (10,000 km). The radial compression knee equals r0.
- **Reroll impact:** None.
- **Revisit trigger:** Planet-approach testing in M3.
- **Status:** Provisional

### D-010 — Window and velocity LOD constants
- **Decision:** Window half-width N = 2 (125 cells). Sensitivity cap N_max = 6. Velocity LOD suspends a level whose cells are crossed faster than once every K = 4 frames.
- **Reroll impact:** Changing N changes level assignment (`L = ceil(log2(R/N))`) and therefore rerolls. **Treat N as frozen once content exists.**
- **Status:** Provisional

### D-011 — Photometric baselines
- **Decision:** V-band magnitudes throughout. Baseline limiting magnitude m_ref = 6.5 (dark-sky naked eye). θ_min = 8e-4 rad (about 1 px at 1920 px across a 90° FOV). Baseline surface-brightness limit μ_ref = 25 mag/arcsec². Δm clamped to [−5, +5]. Sprite intensity = 10^(−0.4 (m − m_exposure)), faded to zero across [m_lim − 0.5, m_lim].
- **Reroll impact:** HIGH for m_ref and θ_min — both feed level assignment. **Freeze before content ships.**
- **Status:** Provisional

### D-012 — Prototype fields
- **Decision:** The prototype uses analytic stand-in fields written in the shared C++/HLSL subset: a cellular cosmic-web field on a periodic hash lattice, and a galaxy field (exponential disc, Sérsic-like bulge, logarithmic spiral arms). Ports of UltraLargeScale's `UniverseDensityCore.ush` and `GalaxyDensityCore.ush` are scheduled as **M2b**, because they depend on `/UniverseNoisePack` textures that cannot be verified offline.
- **Rationale:** Everything the day produces must be verifiable without the owner.
- **Reroll impact:** Content changes when the ported fields land — expected.
- **Status:** Provisional

### D-013 — Placement method and probes
- **Decision:** Analytic fields place by rejection against a probed per-cell maximum; baked fields by mip-chain descent. Default probes per cell: 2 rounds of 64 (UltraLargeScale lesson #10). Envelope pad ×1.25.
- **Status:** Provisional

### D-014 — Luminosity function
- **Decision:** A placeholder tabulated V-band stellar luminosity function, baked to a LUT, drives luminosity slicing for stars. Galaxies use a placeholder Schechter-shaped function. Both are clearly marked placeholders.
- **Revisit trigger:** Art direction / realism pass.
- **Status:** Provisional

### D-015 — Engine-agnostic core
- **Decision:** All maths that can be tested without Unreal lives in `Source/UniCore/` as header-only C++17 with no UE includes (coordinates, seeds, cell keys, windows, level assignment, photometry, f(r), Poisson, placement reference). UE modules wrap it with thin adapters. Shader cores live in `Shaders/Private/Core/*.ush`, written in a subset that also compiles as C++ through `UniHlslShim.h`.
- **Rationale:** The owner is unavailable to test in UE during the prototype day. This makes the core verifiable with a plain C++ compiler and keeps CPU and GPU on the same source.
- **Status:** Provisional

### D-016 — Test harness
- **Decision:** Dependency-free test macros in `Tools/UniVerify/` plus a Python arbitrary-precision oracle for coordinate fuzzing. If an Unreal install is present, the same core is also exercised through UE Automation tests runnable headless with `-nullrhi`.
- **Status:** Provisional

### D-017 — Shader validation without the engine
- **Decision:** If DXC is obtainable, every shader entry point is compiled offline (SM6, UE-specific includes stubbed). Shared cores are additionally executed on the CPU through the shim. GPU execution parity is listed in REVIEW_TONIGHT as owner-verified.
- **Status:** Provisional

### D-018 — Reference renderer output
- **Decision:** `univerify` renders reference frames to PNG (vendored `stb_image_write.h` if fetchable, otherwise PPM converted by a Python script) with a text HUD of camera coordinates and level counts. Flythroughs produce contact sheets.
- **Status:** Provisional

### D-019 — Version control
- **Decision:** Work on branch `uu/prototype-day1`. Commit after each green verification step with the step ID in the message. Never modify UltraLargeScale or project-wide config beyond adding the plugin.
- **Status:** Provisional

---

## Decisions taken during implementation

*(Append D-100 onwards here.)*

> Numbering note: D-100 to D-103 were reserved while planning (window seam, influence term, slicing bound, cell integral) and are written when their code lands. Entries appear in the order written, not by number.

D-104 — Exact per-cell deltas on the GPU instead of hi/lo floats
Question: The design sends each resident cell's camera-relative vector as `ToHiLo(Sub(CellOrigin, Camera))` (48 bits) and adds `Offset · cellWidth / 2^32` in float. Is that precise enough?
Decision: No. The CPU uploads each cell's delta `CellOrigin − Camera` as an exact 128-bit two's-complement value per axis (3 × uint4 = 48 bytes per cell). The GPU adds the object's offset in integer arithmetic (`Uni_ObjectOffsetCm`, shifted by L − 32) and converts once, correctly rounded, to float (`Uni_ObjectRelative` in UniCoord.ush). The camera's sub-cm fraction is subtracted after conversion.
Rationale: With hi/lo the absolute error is about |cell delta| · 2^-48, which is the cell's size, not the object's distance. For a bright star at L = 75 (10 kpc cells), the error is about 1,300 km. At the r0 = 10,000 km promotion boundary that is about 0.1 rad, a visible jump at handoff. The exact form gives 2^-24 of the object's own distance at every level and distance, for roughly 100 integer ops per object. Upload size goes from 24 to 48 bytes per cell, about 0.5 MB for 10k cells.
Reroll impact: None (render-side only).
Revisit trigger: Project-and-cull GPU time over budget in M1. Then use hi/lo for cells whose delta exceeds 2^40 · cell width and the exact path only for near cells.
Status: Provisional

D-105 — What "bit-identical CPU and GPU" covers
Question: M0's exit gate asks for bit-identical CPU and GPU results. Which results can that apply to?
Decision: Integer-only cores (UniCoord, UniSeed, cell keys, window slots, the final float construction in `Uni_U128ToFloat`) must be bit-identical. That is verified on the CPU here, and on the GPU by UE-4 tonight. Anything computed after a transcendental (exp, log, pow, sin in fields, Poisson, photometry) is required to be statistically identical only: CPU libm and GPU hardware approximations differ by a few ULP, so counts can differ in rare boundary cases.
Rationale: There is no CPU generation path at runtime (lesson #1), so CPU/GPU float divergence cannot desynchronise anything. The CPU reference pipeline exists to verify GPU logic, not to reproduce GPU output bit for bit.
Reroll impact: None.
Revisit trigger: Any feature that needs CPU and GPU to agree on generated content (CPU resolve for persistence of GPU-generated objects). Then resolve must run on the GPU too, which the design already implies ("Resolve generates the ancestor chain", async).
Status: Provisional

D-106 — Engine version targeting without the .uproject
Question: D-006 says to target the .uproject's EngineAssociation. It was not readable here.
Decision: Target UE 5.5 or later APIs (folder names on the laptop suggest 5.7). Any signature known to have moved between 5.4 and 5.7 is wrapped in `ENGINE_MAJOR_VERSION` / `ENGINE_MINOR_VERSION` guards, and those guards live only in UniRenderHooks.cpp.
Reroll impact: None.
Revisit trigger: UE-1 build errors.
Status: Provisional

D-107 — Engine-free re-implementation of HashCombine and FRandomStream in UniSeed
Question: ProcSeed is to be ported exactly, but it calls UE's `HashCombine` and `FRandomStream`, and UniCore may not include UE headers (D-015).
Decision: UniSeed.ush carries its own `Uni_HashCombine` (the Bob Jenkins 96-bit mix that UE's legacy `HashCombine` uses) and an `FRandomStream`-compatible LCG (`Seed = Seed * 196314165 + 907633515`, fraction from the top 23 bits). They are written from the UE 5 source as I know it. An Automation test (UE-2) compares both against the engine's functions on fixed inputs.
Rationale: The GPU needs the same functions anyway, and the GPU cannot call engine C++.
Reroll impact: HIGH if the engine differs and the engine's version is chosen later. Every seed would change. Settle it with UE-2 before any content is authored.
Revisit trigger: UE-2 failure. In particular, ProcSeed's own comment ("HashCombine reduces to Channel ^ (Seed + K)") reads more like `HashCombineFast` than the Jenkins mix, so check which one your engine's `HashCombine` is.
Status: Provisional — needs UE-2

D-108 — UniCore type names and Blueprint wrappers
Question: The design names `FUniCoord` for both the engine-agnostic maths type and the Blueprint type. One C++ name cannot be both a plain struct and a USTRUCT.
Decision: UniCore types live in `namespace UniCore` (`FCoord`, `FDelta`, `FCellKey`, `FObjectId`). The USTRUCTs `FUniCoord`, `FUniCellKey` and `FUniObjectId` in UnifiedUniverseCore hold the same limbs as uint32 UPROPERTY arrays and convert with `ToCore()` / `Make()`. Blueprint reaches them through `UUniCoordLibrary`, with hex strings as the text form.
Reroll impact: None.
Status: Provisional

D-100 — Level windows at the root seam and on coarse axes
Question: FTierStreamingSystem's `SlotOf = PosMod(coord, Side)` assumed int32 coordinates in one frame. At level L, cell coordinates wrap over 2^(128−L), which is never a multiple of an odd Side. So what happens at the seam, and on levels with fewer cells per axis than the window is wide?
Decision: (1) If an axis has at most 4N + 2 cells (only the top few levels), the window covers the whole axis: the residue is the cell coordinate and the slot table is 2^(128−L) wide on that axis. (2) Otherwise, when the window straddles coordinate 0 on an axis ("seam mode"), cells within 2N of zero read as c + 2^(128−L), which makes the window one contiguous run. (3) Any change in the per-axis seam mask forces a full rebuild of that level.
Rationale: Slots stay a bijection everywhere (T1.window.bijection, 0 violations over 5,184 slides including seam-heavy centres). The seam is at the antipode of the nominal origin, 2^127 cm away, so rebuilds there cost nothing in practice. First attempt used a half-axis rule; the test caught it aliasing on 16-cell axes at N = 6, and the per-axis mask was also a test catch.
Reroll impact: None (slots never affect content, which is keyed by cell).
Status: Provisional

D-109 — Identity seeds use their own injective mixer, not MixSeed
Question: The design says object seed = `MixSeed(CellSeed, index)`. Is that good enough?
Decision: No. Cell seeds, object seeds and counter-based draws use `Uni_SeedMix(h, v) = Avalanche(h ^ Avalanche(v ^ 0x9E3779B9))`, chained over every input. Each step is a bijection of the varying input, so object seeds are injective in index. The ProcSeed port (`MixSeed`, `HashCombine`, the `FRandomStream` LCG) is kept exactly, for name-keyed rolled parameters only, where A/B parity with UltraLargeScale matters.
Rationale: Measured over 300 cells × 32,768 indices, `MixSeed(root, CH, index)` gave 373 collisions with the Jenkins `HashCombine` and 19,342 with the fast form, against a birthday expectation of 75. Collisions mean twin objects in a cell. `Uni_SeedMix` gives 0 (T1.seed.injective). This also makes generated content independent of which `HashCombine` the engine has (D-107), so D-107 now affects rolled archetype parameters only.
Reroll impact: HIGH once content exists: this defines every cell and object seed. Freeze before authoring.
Status: Provisional

D-110 — Float tolerances in Tier 1 photometry and f(r)
Question: VERIFICATION_TOOLING asks photometry to match closed form "to 1e-6". The shared cores are float, as on the GPU.
Decision: Judge each quantity against its float conditioning. Apparent magnitude: error ≤ 1e-6 of the terms' size (|M| + |5 log10(r/10pc)|), because the sum cancels. k = 10^(Δm/5): 1e-6 relative, as specified. Sprite intensity: 1e-4 relative, because 10^(−0.4 m) amplifies the magnitude's rounding. f⁻¹(f(r)): within 4 · 2^-24 · (f(r)/r0 + 1). Measured: m 3.7e-7, k 2.5e-7, intensity 6.7e-5, f⁻¹ at 0.67 of its bound.
Rationale: Visual effect is nil: 1e-5 mag, or 1e-5 of a radius. A flat 1e-6 would need double precision, which the GPU path does not use.
Reroll impact: None.
Status: Provisional

D-101 — Influence term of the relevance radius
Question: The design says an object's influence relevance is "archetype-defined, so a star system's contributor is live wherever its planets could be relevant". What is the rule?
Decision: R_influence = max over emitters of (InfluenceMul · R_phys,max + √3 · (N_max + 1) · 2^Lmax_child), where Lmax_child is the coarsest level of that emitter's children. An object is then resident wherever any of its children's windows (up to the sensitivity cap N_max) can touch its influence sphere.
Rationale: This is what makes a cell's contributor set content-determined, which determinism depends on. T2.gen.determinism (with eviction on exit) and T2.gen.coverage verify it. For galaxies the angular term dominates (19 Mpc against 0.2 Mpc), so it changes nothing today. It will matter for star systems with planets.
Reroll impact: MEDIUM. It feeds level assignment for archetypes with emitters.
Status: Provisional

D-102 — Physical radius scatter and level assignment
Question: Radius is drawn after the level is chosen, but it also feeds the angular relevance term.
Decision: The radius model is log10 R = a + b(M − M0) + s(2u − 1). Level assignment uses the top of the scatter (u = 1), so a drawn radius can only make an object coarser than needed, never too fine to be covered.
Reroll impact: MEDIUM (level assignment).
Status: Provisional

D-103 — Cell integral from the envelope probes
Question: The design's default integral uses 8 jittered probes, while the envelope uses 128 (D-013).
Decision: The cell integral is the mean of the same 128 envelope probes, a stratified 4×4×4 grid jittered twice.
Rationale: Strictly better than 8 at no extra cost, since the probes run anyway. Coarse cells (a contributor spanning ≤ 8 cells per axis) still miscount by 3–18% (T2.gen.slicing measured: 4 cells/axis 1.18, 8 → 0.97, ≥ 16 within 0.3%). That is lesson #10's under-probing showing up as count error at the coarsest star levels. M2 should add adaptive probes when a cell is larger than about 1/8 of its contributor.
Reroll impact: Content changes if the probe count changes.
Status: Provisional

D-111 — Children scale with parent luminosity
Question: A galaxy emitter's N_total ("2e11 stars per galaxy") is per parent, but parents vary by six magnitudes.
Decision: An emitter may set NTotalRefMag. Then N = NTotal · 10^(−0.4 (M_parent − NTotalRefMag)): luminosity-proportional, 2e11 stars at M* = −21.
Reroll impact: Content.
Status: Provisional

D-112 — Tile symmetry without integer offsets
Question: Tile hashing picks a symmetry and an integer offset per tile. Offsets that are not multiples of 2^mip break the exact mip-integral alignment at coarse mips.
Decision: Symmetries only (48 per tile, from `hash(level, tile, field id)`). All 48 are used across 2,000 tiles, and integrals are unchanged under each (T2.gen.baked).
Reroll impact: Content of baked fields when offsets are added.
Revisit trigger: Visible repetition in baked fields.
Status: Provisional

D-113 — Regenerate cells whose contributor set grows
Question: A contributor can be registered after cells it overlaps have already generated, for example when its parent cell enters a coarser window after the finer window was filled.
Decision: Each cell stores the sorted set of contributor ids it was generated with. If the gathered set for a resident cell gains a member, the cell is regenerated. A set that only shrinks (contributor left the window list but still exists) keeps its content.
Rationale: This is the determinism contract. Measured regenerations on the test paths: 0. With the D-101 influence term, a contributor first overlaps a level's window at the window edge, beyond relevance, so a regeneration is never visible.
Reroll impact: None.
Status: Provisional

D-114 — Candidate budget for rejection placement
Question: How many candidates may a task evaluate before it gives up on reaching its Poisson count?
Decision: 256 + 4n / acceptance, where acceptance = probe mean / envelope (floored at 1e-4). The GPU uses the same formula to size its candidate dispatch.
Rationale: The first version (256 + 64n) under-delivered in the bulge cusp: 8,546 objects short over 12,735 tasks, z = −12. After the change: 0 short, z = −1.05.
Reroll impact: None (it only adds objects that should exist).
Status: Provisional

D-115 — Normalising integral uses the cell estimator
Question: How is a contributor's ∫all F computed?
Decision: The 128-probe cell estimator over a 32³ tiling of the field's domain, skipping tiles outside the unit ball. That is about 2.4M field evaluations per contributor on first use, then cached.
Rationale: One jittered sample per stratum (64³) was measured 4% high on the galaxy field, against a 512³ midpoint reference. The disc is thinner than a stratum, and slicing totals missed the 1% bound. Using the same estimator as the cells makes them consistent: tilings at 16–64 cells per axis agree within 0.3%.
Reroll impact: Content (counts) changes with this estimator.
Status: Provisional
