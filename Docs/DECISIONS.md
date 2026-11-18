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

D-116 — Fades across velocity-LOD resumes and window jumps
Question: A suspended level keeps its contents and fades out. When it resumes, its window jumps to the camera in one step, and the new cells can lie inside relevance.
Decision: (1) A level that resumes before it has fully faded out keeps fading out first. (2) Any re-centre that moves a window by more than one cell (resume, teleport, rebuild) starts that level's fade at zero, so it fades in over 8 frames. The fade is render-only (lesson #12).
Rationale: T3.flythrough's first run counted 102,618 pops. The diagnosis attributed them to resumed levels whose fade had already ticked to 0.125 when jumped cells entered at r/w ≈ 1.
Reroll impact: None.
Status: Provisional

D-117 — Per-slice magnitude tables, and the slice clamp
Question: Slices drew magnitudes from the archetype's global inverse-CDF LUT, restricted to the slice's CDF interval.
Decision: Each slice has its own 64-entry inverse CDF over its magnitude interval. This is FSliceGPU's LutBase / LutCount, as the GPU layout already allowed. Draws are also clamped to [BrightM + 0.016, FaintM]: the margin is one half-float step, because the record stores M as half.
Rationale: This was a real bug. The bright slices' CDF intervals (L69: 0.0002–0.0014) are narrower than one step of a 512-entry global table, so interpolation drew their stars up to about 5 mag too bright. That inflated visible counts about 3× (74k → 25k → 10k visible towards the centre) and broke level coverage, causing most of the flythrough pops. A new check, T2.gen.slice_draws, now verifies every slice's draws stay inside it and match its conditional mean. The clamp is what makes coverage hold whatever the table's resolution.
Reroll impact: Content (every magnitude draw).
Status: Provisional

D-118 — Level assignment uses N_eff = N − 1/K (deviation from the design)
Question: The design claims objects "stream in and out exactly where they are already invisible". Does that hold?
Decision: Not with L = ceil(log2(R / N)). A window lags the camera by one frame, and an active level's camera moves up to w/K per frame, so an entering object can be up to w/K closer than N cells: up to 0.6 mag inside the fade band for N = 2, K = 4. Levels are therefore assigned with N_eff = N − 1/K = 1.75, and sensitivity windows are N_L = min(ceil(N_eff · k + 1/K), N_max), which is still 2 at Δm = 0.
Rationale: After D-116 and D-117, the flythrough still showed boundary pops at r/w ≈ 2.0. With N_eff it shows 0 pops and 0 entering objects with any non-zero intensity, over 2,400 frames and 545,902 entering objects. Cost: objects sit about 0.19 octave finer, which means slightly more cells per object, and residency rises about 1.5× (peak L69 55k → 83k).
Reroll impact: **HIGH**. It changes level assignment, as D-010 warns. No content exists yet, so this is the moment to decide. The alternative is to keep N = 2 and accept fade-band pops. Owner decision (REVIEW_TONIGHT §4).
Status: Decided (owner, 2026-11-01): accept N_eff = 1.75. Frozen with D-010; D-139 builds on it.
Amended by D-139: the window lags by the generation latency (about 6 frames on the GPU), not one frame. Windows now widen with speed to keep the no-pop claim; N_eff and level assignment are unchanged.

D-119 — θ_promote = 0.02 rad
Question: The design names θ_promote without a value.
Decision: 0.02 rad, about 25 px at 1920 px across 90°, with hysteresis at 1.25× (demote below 0.016).
Reroll impact: None (render and lifecycle only).
Revisit trigger: M3 handoff tuning. The flythrough shows the sprite's glare far larger than the body at handoff (OBSERVATIONS.md).
Status: Provisional

D-120 — Scene-depth test in true distances
Question: How does the sprite pixel shader compare against the main scene?
Decision: The main pass depth is converted to true distance along the pixel's ray and compared with the object's true (uncompressed) distance r. The sprite is kept if r is less than the scene distance.
Rationale: Promoted bodies are at true scale in the main pass, so true distances are directly comparable, and compression never enters the test. Verified on 20,000 placements (T3.occlusion).
Reroll impact: None.
Status: Provisional

D-121 — Extended-source photometry blends in over [θ_min, 2θ_min]
Question: A galaxy crossing θ_min switches from point-source (total magnitude) to surface-brightness visibility, which can pop.
Decision: Visibility = max(point fade from m, surface-brightness fade from μ × ramp), where the ramp goes from 0 at θ_min to 1 at 2θ_min.
Reroll impact: None.
Status: Provisional

D-122 — GPU generation runs in two phases with a CPU allocation step between them
Question: The design's GPU passes are gather → prefix-sum and allocate → place → register contributors. Where do pool ranges and object indices come from?
Decision: PASS 0 (probe) writes each task's Poisson count n. The counts are read back, and the CPU allocates one contiguous pool range per cell (task sub-ranges in contributor order) and sets IndexBase = prefix sum of n, truncated at 2^15 with the lowest indices kept. PASS 1 (place) then writes exactly that range, blanking unfilled slots. Object indices advance by the drawn n, not the accepted count, so they are known after the probe phase alone. Object records come back to the CPU only for tasks whose children own emitters (galaxies), to register contributors. Star records never leave the GPU. This is in UniGenTask.ush, UniStreamer.h and UniGen.usf.
Rationale: It keeps the pool and window bookkeeping on the CPU, where the design puts it, and makes the whole protocol testable offline: T2.stream.parity, 5,375 cells identical. Cost: two GPU round trips per level, so a teleport fills about 18 levels × 2 ≈ 37 frames, coarse to fine.
Reroll impact: Index assignment by n (identity). The CPU reference uses the same rule.
Revisit trigger: Teleport latency. A GPU-side prefix sum (one round trip) or starting finer levels as soon as their own emitting parents are ready (not all coarser levels) would cut it.
Status: Provisional

D-123 — The normalising integral is computed on the CPU, on first use
Question: Where does a new contributor's ∫all F (D-115) get computed at runtime?
Decision: On the game thread, inside BuildTask, through the shared field core: about 2.4M field evaluations, roughly 0.2 s, the first time a contributor's children are generated. It is cached on the contributor.
Rationale: Simplest correct option today. It is the same field source as the GPU (no texture stand-in, lesson #1), and it only sizes counts.
Known cost: a one-time hitch when the camera first enters a galaxy (REVIEW_TONIGHT §6). Move it to a worker task or a GPU reduction in M2.
Amended by D-140: now computed on worker threads at runtime and cached by contributor id, so the hitch is gone.
Reroll impact: None.
Status: Provisional

D-124 — Backdrop and composite merged into one post-motion-blur draw (prototype)
Question: The design renders the backdrop after the base pass and composites after TSR.
Decision: One additive sprite draw in the post-processing callback after MotionBlur, directly into scene colour. It tests scene depth itself (D-120). There is no separate backdrop target until volumetrics need one.
Rationale: Fewest hooks and passes to prove first. Stars still get bloom and auto-exposure and are not smeared by TSR, if the engine's pass order is as expected (UE-5).
Reroll impact: None.
Status: Provisional

D-125 — Anchors reach the GPU pool through a scatter pass
Question: Authored anchors are not generated, so how do they get into the object pool?
Decision: The CPU knows anchors. Their records take the slots after the generated objects in their cell's pool range and are uploaded with that cell's place batch through a small scatter compute pass (UNI_GEN_PASS 2). Their contributors are registered from the CPU copy.
Reroll impact: None.
Status: Provisional

D-126 — Sensitivity resizes keep resident cells
Question: Changing Δm changes each level's window half-width. The first implementation rebuilt the window, reset the level's fade and, in the streamer, evicted and regenerated every cell.
Decision: A resize keeps every cell still inside the new window (content, pool range, state) and keeps the level fade. Only the new outer shell is generated. It lies beyond relevance at the new k by construction of N_L = ceil(N_eff · k + 1/K). Cells outside a shrunk window are released.
Rationale: Measured by a new check before the fix (T3.sensitivity, a Δm 0 → +2.5 → 0 pulse): 152 level blinks, and the sky black after every change. After: 0 blinks and 0 pops, with visible stars going 9,918 → 259,980 → 9,918. The streamer keeps 2,375 ready cells through a resize and settles to the reference exactly (T2.stream.sensitivity_resize).
Reroll impact: None.
Status: Provisional

D-127 — UE review fixes (render path and GPU protocol)
Question: An independent review of the uncompiled UE code found build breakers. It also found two ways for the backdrop to be invisible: writing into a non-render-targetable input, and NaN depth on cleared sky. There was a race between the probe and place request flags, and `UniGen.usf` had barriers after an early return.
Decision: Draw into a copy of scene colour (`OverrideOutput` or a new target). Use the unjittered projection, pre-exposed sprite radiance, and game views only. Keep per-phase request state, with timeouts that fail closed and are measured from render-thread execution. Release resources at engine pre-exit. The generator has no early return, and its loops are bounded and group-uniform. The full table is in REVIEW_TONIGHT §6.
Rationale: Each item is a likely first-build or first-frame failure. The streamer's fail-closed path is verified offline by the new check T2.stream.fail_closed. The rest needs UE-1, UE-3 and UE-5.
Reroll impact: None. Every change is render-path or protocol only. The generator emits the same objects (writes are gated by rank, and padding groups write nothing).
Status: Provisional

D-128 — The volume draws the complement of the drawn sprites
Question: What does the volume pass draw, and how does it hand off to sprites without double counting or gaps?
Decision: Per emitter, the volume carries each child's expected flux not drawn as a sprite: l(t) = Σ_s P_s (L̄_s − F_L(s) G_s(m_lim − μ(t))). That is integrated over each slice with the same Uni_SliceMagAt and Uni_ColourAt that placement draws with, times the field shape, N_total and the normalising integral generation uses. F_L means "the fraction of level L's sprites drawn". In the streamer a level must hold F_L = 0 until its window is filled (an amendment to D-116; not yet implemented in UniStreamer).
Rationale: Sprites plus volume equal the total light at any distance, sensitivity, level fade or velocity LOD. T3.vol.conservation measured −0.16% / −0.14% / −0.10% at Δm = −1 / 0 / +2 over the whole sphere (z < 1).
Reroll impact: None (render only).
Status: Provisional

D-129 — Carriers, and the sprite-to-carrier handoff
Question: Which objects become marched volumes, and how does an object's own sprite hand over?
Decision: A carrier is built on the CPU from each resident contributor, using its exact centre, rotation, rolled parameters and integral. The camera-local position comes from the exact 128-bit delta. One periodic carrier is added per root emitter. The cap is 64 carriers, ranked by angular size. The handoff weight w = smoothstep over angular radius [T1/2, T1], T1 = min(8 px, θ_promote), is shared: the object's sprite gets 1 − w and the carrier w · V · F_L. No screen tiling in v1. The carrier record is 112 B.
Rationale: T3.vol.handoff: in-frame luminosity × d² is constant within 0.84% from 6 Mpc to 300 kpc, while the sprite's share goes from 100% to 0%.
Reroll impact: None.
Status: Provisional

D-130 — Stepping: log-uniform with field step hints, interval union, prefiltering, supersampling
Question: How is the march stepped so that it is accurate at every scale and bounded in cost?
Decision:
- s = clamp(0.08 t, ...), capped by each active carrier's Uni_FieldStepHint (galaxy: disc, arms, bulge length scales) and by interval boundaries. Nested carriers are marched over the union of their intervals.
- Periodic fields blend to their mean once the pixel's transverse footprint is several feature scales wide. It must be the pixel footprint, not the step: the step is capped by the hint, so a step-based footprint never engaged.
- Pixels whose centre ray hits a carrier whose finest feature (Uni_FieldResolveScale) is under half a pixel are supersampled, up to 8×8. Cost per carrier is about 1/(4 s²) rays.
- Budget: 512 steps, reporting truncation. No jitter in v1.
Rationale:
- T3.vol.march: 0.8% worst against brute force, 85 steps per ray on average.
- One ray per pixel mis-integrated small carriers by 12–17% (measured); with supersampling it is 0.84%.
- Background: 500 → 107 steps per ray.
Reroll impact: None.
Status: Provisional

D-131 — Horizon dimming
Question: An endless toroidal universe has an infinite background (Olbers): the torus is about 10^10 mean free paths to a galaxy.
Decision: Every flux, sprite and volume alike, is multiplied by D(r) = (1 + r / r_H)^−2 with r_H = c / H₀ = 1.37e28 cm. Visibility, relevance and level assignment use undimmed magnitudes. The background marches 0.1 Mpc to 0.5 periods (50 Mpc), then adds an analytic mean tail.
Rationale: T3.vol.background: 27.43 mag/arcsec², matching the analytic estimate; changing the march extent from 0.5 to 2 periods changes it by < 0.01%.
Reroll impact: None.
Status: Decided (owner, 2026-11-01): r_H = c / H0 = 1.37e28 cm.

D-132 — Dust and transmittance slices
Question: How do dust lanes attenuate the glow and the sprites behind them?
Decision: The archetype has a render-only FVolumeProfile. The dust field is the galaxy field with a thinner disc (0.5 h), no bulge, arms shifted −0.35 rad, normalised to a face-on τ (0.6 for the demo), with reddening (0.8, 1.0, 1.3). The march writes transmittance at 8 slices uniform in ln t over 1 pc – 100 kpc. Sprites read it at their true distance.
Rationale: The interior still shows the dark lane across the band. T3.vol.march checks τ within 0.0013. Known gap: a dusty galaxy's sprite does not carry its internal extinction (v1.1).
Reroll impact: None.
Status: Decided (owner, 2026-11-01): keep the transmittance slices in v1.

D-133 — Luminosity budget validation
Question: The volume makes a parent's own magnitude and its children's summed light visible side by side. Are they consistent?
Decision: LuminosityBudget(model, archetype, M) computes the magnitude a parent's children add up to, and Validate should require agreement within 0.1 mag. Measured for the demo: 2e11 stars sum to M = −23.18, against the drawn −21.00 (7.4×). Fixing it is the owner's call: N_total = 2.69e10, a fainter stellar function, or NTotalRefMag. The checks use a corrected copy.
Reroll impact: HIGH for the fix (star content). The check itself has no reroll impact.
Status: Decided (owner, 2026-11-01): option B. The demo's N_total is 2.69e10, so its stars sum to M = -21.00 (T3.vol.budget). Rerolls star counts in every galaxy (x 0.134). Re-derive N_total from LuminosityBudget whenever the stellar function changes (D-014).

D-134 — The carrier's normalising integral
Question: A carrier needs the contributor's ∫ρ before any child cell exists.
Decision: The prototype calls the same IntegralAll (D-115/D-123), so it shares D-123's one-time cost. In the engine, use a 64k-sample estimate until the full integral is cached (≤ 1% flux noise).
Reroll impact: None.
Status: Provisional

D-135 — Sprite colour normalised by luminance
Question: Sprite colour was normalised so the brightest channel is 1, so no channel of a sum of many coloured stars is conserved when they hand light to the volume.
Decision: Radiance = colour / Y(colour) × intensity, where Y is Rec. 709 luminance. The intensity (from the V magnitude) is then the luminance, and sums conserve it exactly. The volume tables use the same normalisation.
Rationale: Exact luminance conservation regardless of hue (T3.vol.handoff, T3.vol.conservation). The packed object colour is unchanged.
Reroll impact: None. The colour of red stars brightens by 1/Y on screen (brightness is uncalibrated, lesson #36).
Status: Provisional

D-136 — Background direction map and separable composite
Question: Marching the background (the web's unresolved galaxies) per pixel costs ~107 steps of the expensive web field, every frame, for light that is faint and has almost no parallax.
Decision: The periodic carriers start at 0.1 Mpc, beyond every local carrier's chord. They are marched into a 6-layer direction map (256² per face, this core's own face convention so the CPU reference and the shader agree). Per pixel, B = B_near + T_near × B_map(dir). The map is re-marched, a few faces per frame, when the camera has moved a quarter texel's parallax at 0.1 Mpc (~3e20 cm) or the background's LUT has changed by more than 1%. In the engine T_near is luminance only.
Rationale: T3.vol.cube: total 0.000% and 99th-percentile pixel ≤ 0.02% against the union march, inside the dusty galaxy and in intergalactic space, at 107 steps per texel once rather than per pixel per frame.
Reroll impact: None.
Status: Provisional

D-128 (amendment, implemented) — In FUniStreamer a level's fade starts at 0 and cannot rise while its window is unbuilt or any of its cells is not Ready, so a filling level is drawn as glow and then fades in whole. T2.stream.volume_parity: 0 level-frames filling with a fade above 0, and the streamer's volume scene is byte-identical to the reference pipeline's.

D-137 — A level waits for its parent levels only (amends the design's coarse-to-fine ordering)
Question: Owner, first engine run: "galaxy movement level slow responsiveness at high speeds"; star pop-in inside the galaxy.
Decision: A level builds its cells once the windows of its PARENT levels are Ready. A parent is a level that holds an archetype whose emitters place objects at this level. Levels fed only by root emitters (the web's galaxy levels) wait for nothing. Before, every level waited for every coarser level, so latency cascaded down the ladder.
Rationale: Measured offline with a backend that takes 3 frames per phase, the GPU's order. Worst entering-to-ready latency went from 36 frames (web), 108 (galaxy) and 132 (flythrough) to 6 in all three. That is one probe plus one place round trip. The finest galaxy window during a 10 Mpc/s flight went from 58% to 83% Ready. Contributor completeness is unchanged, because contributors only come from parent levels. T2.stream.parity is unchanged (0 differences).
Reroll impact: None. Order only; the settled content is identical.
Status: Provisional

D-138 — Contributor distances measured once per frame
Question: The same run showed frame lag at speed.
Decision: The streamer measured every registered contributor's distance from every level's window, every frame: about 2,500 galaxies × 24 levels in the web scene. It now measures each contributor's offset from the camera once per frame, sorted by distance. Each level's gather is a cut-off scan of that list, plus a conservative double-precision box test; the exact overlap test still decides tasks. A level re-checks its Ready cells for new contributors only when its gathered set changed.
Rationale: Streamer bookkeeping in the web scene went from about 11 ms to about 1 ms per frame on the game thread (offline, -O2), with identical results.
Reroll impact: None.
Status: Provisional

D-139 — Windows widen with speed to cover the generation latency (amends D-118)
Question: D-118's no-pop rule assumes a window trails the camera by one frame. Does it hold on the GPU?
Decision: No. A GPU fill takes about 6 frames, so at the velocity-LOD limit a cell entering at the window edge became Ready up to 1.5 cells closer, inside the visible radius N_eff. With velocity LOD on, each level's window half-width is now N ≥ N_eff·k + speed × (latency + 1), in its own cells, capped at 4 (9³ cells).
- The latency is measured from probe submit to Ready, with fast attack and slow decay, clamped at 16 frames.
- A level is suspended only when even N = 4 cannot cover the lead (slack 2.25 cells).
- Widening goes through the D-126 resize path, which keeps cells, and stops above 70% pool use.
- A window shrinks back one step per 30 slow frames.
- Level assignment still uses N_eff, so nothing rerolls.
Rationale: New check T2.stream.responsive: in steady flight, 0 visible cells not Ready over 5 runs (galaxy at 1 and 10 kpc/s, web at 10 and 30 Mpc/s, flythrough at 10 kpc/s). The only gaps are the first 8 frames after going from standstill to full speed in a single frame, which is the case B-001's warp streaks would use. Pool peak 1.29M of 2.1M, with 0 allocation failures. The suspension limit rises from 0.25 to about 0.32 cells per frame at 6 frames of latency.
Cost: wider windows at speed. A level at N = 4 holds 5.8× the cells of N = 2, only while it moves that fast.
Reroll impact: None.
Status: Provisional

D-140 — Normalising integrals: cached by contributor id, and computed on worker threads at runtime (amends D-123)
Question: D-123 computes a contributor's field integral (about 2.4M field evaluations, about 0.2 s) on the game thread the first time its children are needed. Contributors are re-created whenever their parent's cell reloads, and that discarded the cached value.
Decision: The generator also caches the integral by contributor id; the id fixes the parameters, so the value is the same. The runtime streamer computes missing integrals on worker threads, at most 4 at a time. The UE module supplies its thread pool (AsyncTask); offline builds use a std::thread. Cells that need a missing integral stay Waiting until it arrives, and nothing else waits. Offline checks that count frames compute it inline, as before.
Rationale: Flying past galaxies hitched about 0.2 s for each new galaxy and again for each reloaded one, which plausibly explains the "chunky" movement between galaxies. New check T2.stream.async_integrals: 0 integrals computed on the calling thread, and the settled content is identical to the reference in all 3,000 cells.
Reroll impact: None. Same function, same values.
Status: Provisional

D-141 — Volumetrics: the fade hold counts visible cells only, and carriers wait for worker integrals (amends D-128 and D-140 on this branch)
Question: After merging D-137–D-140, two volumetrics rules worked against them.
- The D-128 fade hold kept a level's fade from rising while ANY cell in its window was generating. In a window widened for speed, the outer shell is almost always filling, so a level that had faded (after a teleport or a suspension) would stay as glow for as long as the camera kept moving.
- The volume builder computed normalising integrals inline, on the game thread, for every carrier. A galaxy needs a carrier from its 4 px handoff onwards, Mpc away and long before its stars are requested, so the D-123 hitch would have come back.
Decision:
- The hold counts only cells within ceil(N_eff·k) of the window centre (Chebyshev), the only ones that can hold visible objects. At rest (N = 2) that is the whole window, the same as before.
- The builder asks the streamer (FVolumeSource::IntegralReady). A missing integral starts the worker job and that carrier is skipped for those frames. The request comes at the start of the handoff ramp, where the carrier's weight is about 0. The reference pipeline still computes inline, so offline renders are unchanged.
Rationale: T2.stream.volume_parity still holds: 0 hold violations, identical carriers and LUTs (at rest the rule is unchanged). The flight cases are covered by T2.stream.responsive.
Reroll impact: None.
Status: Provisional

D-142 — Star systems: activated near the camera, generated on the CPU from the star's record
Question: B-002 (owner request). How do planets come into being, given that star records never leave the GPU?
Decision:
- **Activation.** An archetype can have systems (`FArchetype::bSystems`; stars in the demo). The project pass tests every resident object of such an archetype against its activation radius: 100 AU × √(L/L☉), clamped to 30 AU–10⁴ AU, shared core `UniSystem.ush`. The test uses true distance, before culling. Matches are appended, with their records, to a 32-entry readback ring.
- **Mapping back.** The game thread maps each readback to the star's cell through the last 16 frames' cell tables, then activates the system. A system is deactivated, with exact CPU distance, beyond 1.25× the radius.
- **Generation (`UniSystem.h`).** The system is generated from the star's record alone:
  - planet count: Poisson with a mean of 2.5–4 by mass, at most 10;
  - semi-major axes: log-uniform over 0.04–60 AU × √L, sorted, with a minimum spacing ratio of 1.3;
  - type: rocky inside the snow line (2.7 AU × √L); the first two beyond it are gas giants, then ice giants;
  - radius, albedo, eccentricity and inclination are rolled by name; node, argument and phase are uniform.
- **Seeds.** A new seed channel, SYSTEM. Every planet quantity keys its own named stream.
- **Owner decisions (2026-11-01).**
  - Orbits are on rails.
  - The clock runs at real time by default and is configurable.
  - Binaries are not in v1. When they come, they are generated at the system level, with one galaxy-level sprite resolving into its stars.
Rationale:
- Only a handful of systems are active at once, so CPU generation costs nothing.
- The star record already carries everything a system needs.
- Re-entering a system gives the same planets, because the system is a pure function of the record.
- Checks: T2.system.determinism (20,000 real stars: identical on rebuild, distinct per star, all planets within limits, 3.2 planets per system, 75% with a gas giant) and T2.system.activation (200 paths, grazing to central, with camera wobble: exactly one activation and one deactivation each).
Reroll impact: None for existing content. New content only.
Status: Provisional

D-143 — Planets are drawn through the existing project pass: dynamic pseudo-cells and a reserved pool tail
Question: Planets move every frame. How do they reach the sprite path without a new renderer?
Decision:
- **Pool.** The last 256 pool slots are reserved for planet records and rewritten every frame by the scatter pass. The generation pool ends below them, at 2,096,896 slots.
- **Pseudo-cell.** Each active system becomes one entry in the cell table. Its level is Lp + 1, where 2^Lp ≥ 4× the system's span. Its origin is half a level-Lp cell below the star, so every planet's integer offset fits the record's u32 for its whole orbit.
- **Records.** Each frame the CPU writes each planet's exact offset: star coordinate plus the double-precision Kepler position, then an integer subtraction.
- **Precision.** The camera-relative vector stays exact (D-104).
Rationale: T2.system.pipeline runs 300 real systems with the camera inside, sampling 24 times over each outer orbit (27,624 records) through the GPU's own projection core:
- worst direction error 6e-8 rad;
- distance error 6e-8;
- apparent magnitude within 0.016 of exact;
- 0 saturated offsets.
Reroll impact: None. Generation capacity drops by 256 of 2,097,152 slots.
Status: Provisional

D-144 — Planet photometry: reflected light, written as an effective absolute magnitude
Question: The sprite path takes an absolute magnitude and applies the distance modulus itself. Planets shine by reflected light, which depends on three bodies.
Decision:
- **Brightness.** m = M★ − 2.5 log10(A_g · Φ(α) · R² · (10 pc)² / (r² Δ²)), with Φ the Lambert-sphere phase function.
- **What the record carries.** The CPU writes M_eff = m − 5 log10(Δ / 10 pc) into each record every frame, so the GPU's distance modulus restores m exactly.
- **Hiding.** A planet with M_eff > 60 is hidden; F16 keeps 0.03-mag steps up to there.
- **Colour.** The star's colour × the planet type's tint.
- **Appearance.** Planets are flagged extended, so they get the resolved-disc path when large.
Rationale: T1.system.photometry:
- Φ agrees with a numerical integral over a lit sphere to 6e-7.
- The formula gives Jupiter −2.72 and Mars −2.08 at mean opposition. Literature values are about −2.7 and −2.0.
Limitations: The disc is drawn flat. The lit crescent comes with the M3 sphere actor (B-003).
Reroll impact: None.
Status: Provisional

D-145 — Universe clock
Question: Orbits need a time.
Decision: The subsystem has a double-precision clock in seconds since the epoch, starting at 0 when a universe is enabled.
- It advances at the time scale, 1 by default: real time, as the owner decided.
- `uu.TimeScale <x>` changes the rate (3.15e7 = one year per second); `uu.Time <s>` sets the clock.
- Relativistic scaling may come later.
- Positions are a pure function of t (on rails), so changing the time scale or jumping in time never accumulates error.
Rationale: T1.system.kepler checks the solver, closure and speed:
- worst Kepler residual 4e-16 rad over 200,000 solves;
- closure after one period within 2e-10 of the semi-major axis;
- vis-viva speed within 4e-5;
- a 1 AU orbit takes 365.257 days.
Reroll impact: None.
Status: Provisional

D-146 — Scene actors follow the parallax rule; the depth test reads render space back as true distance (M3)
Question: How do UE actors (promoted bodies, the owner's planets, anything) coexist with the compressed backdrop? Owner: real space at unit scale, consistent movement at any speed, a generic parallax wrapper.
Decision:
- **The wrapper.** `UUniParallaxComponent` can be added to any actor. Given a universal coordinate, it places its owner every frame, right after the universal camera moves:
  - while the owner's surface is inside r0 (and its centre within 5e11 cm, the engine's world bounds), at unit scale, at its true offset (real space);
  - otherwise, scaled about the eye by s = f(d)/d, at s × its offset. That preserves direction, angular size and parallax exactly, so the actor and the backdrop sprites agree at any distance.
- **Consistency.** The switch between the two is invisible on screen and has 1.25× hysteresis. Nothing placed this way moves by its own physics, so it moves in step with the backdrop at any camera speed.
- **Depth.** The sprite and volume passes read scene depth as render space and convert it to true distance with f⁻¹ (`Uni_SceneTrueDistance`):
  - exact inside r0;
  - for a compressed body, nothing behind it shows, and what is in front shows unless it lies within (1 + ln(d / r0)) R of the near surface. That is the bound for a uniformly scaled body; only a moon close in front of a distant planet could fall inside it.
- **Reference renderers.** Both now write render-space depth for their promoted spheres.
Rationale:
- T2.parallax.equivalence: 200,000 objects from 10 m to 10²⁷ cm, direction within 1e-7 rad of the sprite path, render distance within 2e-7, angular size exact to 4e-16.
- T2.parallax.realspace: approach and departure from a Jupiter with wobble at the edge, 2 switches, no size jump.
- T3.occlusion: exact for a real-space occluder, and the stated bound for a compressed one (0 wrong outside it).
Reroll impact: None.
Status: Provisional

D-147 — Promotion of nearby stars and planets to placeholder actors (M3)
Question: Which bodies become actors, when, and how does the sprite hand over?
Decision:
- **Which bodies.** The stars and planets of active systems (D-142) are promoted by the GPU's own test (`Uni_PromoteTest`: surface within r0 or angular radius above 0.02 rad, 1.25× hysteresis). Any body that can pass it is already inside an active system, so no new readback is needed.
- **The actor.** A placeholder `AUniBodyActor`: the engine sphere at the body's true radius, engine materials only, collision off, placed by D-146.
- **Hand-off.**
  - A promoted planet's record carries UNI_OBJ_FLAG_PROMOTED on the same frame, so exactly one of sprite and actor is drawn.
  - A star keeps its sprite. The sphere hides the sprite's centre by depth, and the glare outside reads as a corona until B-003.
- **Light rig.** A movable directional light from the nearest active star, in its colour (`uu.LightRig`, `uu.LightRigLux`).
- **Drawing change (D-143 amendment).** Each planet is drawn from its own level-34 cell, so its record is exact to 4 cm. A shared per-system cell quantised to about 21 km, which disagreed with the exact actor by 7e-6 rad at the swap.
Rationale: T2.parallax.promotion, a gas giant from 300 radii to 1,000 km and back while it orbits:
- 1 swap each way;
- 0 frames with both representations and 0 with neither;
- the actor within 2e-8 rad of where the sprite was.
Known, corrected by D-148: the 1.85× sprite-to-body ratio first reported here was measured on the wrong frame (300 radii out on departure, not at the swap). At the swap, this gas giant's sprite was already body-sized; a very bright body's was 1.39×. The visible mismatch was mostly the profile, a Gaussian blob becoming a hard-edged sphere. D-148 removes both.
Reroll impact: None.
Status: Provisional (placeholder actor; the owner's planet replaces it)

D-148 — Body sprites become their body before the hand-off (B-003, render-only)
Question: B-003 (owner). Far away a sprite is drawn at its glare footprint; near promotion it should come back to the body's true size, and match the actor at the swap.
Decision: Bodies are extended objects without children; today that means planets.
- **Size.** A body's sprite size eases from max(glare, true size) to the true size between ¼ θ_promote and θ_promote: lerp by smoothstep, never below the true size. At the swap it is exactly the body.
- **Shape.** A body wider than 1.5 px draws as a soft-edged uniform disc of its true radius. A Gaussian glare halo carries the rest of the energy and fades out as the size blend completes. The profile (`Uni_SpriteProfile`, shared by the pixel shader and the reference) sums to 1 over the pixels at any size, so flux is unchanged.
- **Unchanged.** Stars keep their glare (point sources; the corona around a promoted star stays). Galaxies keep their Gaussian; they hand off to the volume at 4–8 px (D-129).
Rationale:
- T1.sprite.profile: energy within 1.84% over 295 profiles from 0.8 to 80 px, and the size exact at θ_promote. Without the blend, a bright body was 39% too large there.
- T2.parallax.promotion: the sprite is the body's size (1.000) on the frame before the swap.
Reroll impact: None.
Status: Provisional. Its size rule is superseded by D-150; the disc profile stands.

D-149 — Real space: flight drives the pawn locally below cruise speed; integer-cm re-centring; physics-driven actors (M3)
Question: The M3 criterion is "physics stable in Local mode". The owner asked for consistent movement at every speed scale.
Decision:
- **One control scheme.** `uu.Fly` has the same controls at every speed. Below 5e7 cm/s (and until 1e8 cm/s, the D-008 hysteresis) it moves the pawn through the UE world in Local mode: camera = local anchor + view location. Promoted bodies are then fixed in the world, so physics near them is stable. Above that it is Cruise as before: the pawn is pinned and the universe moves. The pawn's own movement component is suppressed while free flight drives it, and restored when free flight ends.
- **Re-centring.** When the pawn is more than 1,000 km from the anchor, the anchor moves by the pawn's whole-centimetre position (`UniLocalFrame.h`) and the pawn moves back by the same amount. So anchor + position, the universal camera and every universal position are unchanged to the bit.
  - Simulated parallax actors shift with the pawn.
  - `OnLocalRecentre(WorldShift)` tells everything else.
  - The camera cache is refreshed in the same frame.
- **Physics-driven actors.** `UUniParallaxComponent::Mode = Simulated`: while in real space in Local mode, physics owns the actor and its universal `Coord` is written back from its world position. Otherwise the universe places it.
Rationale: T2.local.recentre, 200,000 frames of a wandering pawn with 164 re-centres: the camera stays within 1.5e-5 cm of the exactly accumulated path, moves 0 cm across a re-centre, and bodies shift by exactly the integer shift.
Reroll impact: None.
Status: Provisional (physics stability itself needs UE: UE-11)

D-150 — Every brightness-boosted sprite arrives at its true size (owner, generalises D-148)
Question: Owner: a star's sprite is enlarged with its brightness so it stays visible all the way in. By the time you are close enough to spawn its planets it should be back at its real perceived size, and the same goes for every sprite whose angular size is boosted for brightness.
Decision:
- **Arrival distance.** Each object has an arrival distance (`Uni_ArrivalDistance`):
  - a star whose archetype has systems: its system activation radius (100 AU × √L, 30 AU–10⁴ AU), where its planets spawn;
  - anything else: its promotion distance, R / θ_promote.
- **The ramp.** Over the last 4× in distance before arrival, the sprite's footprint eases (smoothstep in log distance) from the brightness-boosted glare to the true perceived size: the physical angular radius, never below one pixel.
- **Inside the ramp** the boost is frozen at its value at the ramp's start, so the footprint only shrinks on the way in.
- **Unchanged.** Flux is unchanged (the profile is normalised), so a star at its system radius is a single very bright pixel, and the engine's bloom provides its glare. Bodies keep D-148's disc profile.
- **Plumbing.** The view carries the system-archetype mask (`FUniView::SystemArchetypeMask`, which replaces its padding).
Rationale: T1.sprite.arrival, a red dwarf to a supergiant approached from 10⁴× their arrival distance:
- exactly the true size at arrival;
- monotone over the ramp;
- boosted 1.3–2.9× far out.

The other checks are unchanged: T2.parallax.promotion still finds the sprite at the body's size at the swap, T3.flythrough 0 pops, and T3.vol.conservation unchanged (energy is not affected).
Known: a promoted star's sprite is now a pixel inside its sphere, so the "corona" of D-147 is gone. With no emissive engine material, the placeholder star sphere is lit from behind and looks dark. It needs an emissive material (owner's asset) or B-003's bloom hand-off.
Reroll impact: None.
Status: Provisional

D-151 — Galaxy clusters: a Cluster archetype on the web, with budget-consistent members and tuning handles
Question: Owner: "the universe level field needs a bit more population so we aren't swimming through large black voids". Start from a realistic distribution, with handles to tune it.
Decision:
- **Archetype.** A Cluster archetype (id 3), placed by a second root emitter on the galaxies' own web lattice (same G, widths, seed), sharpened with a power (BiasPower 2) and a node term (NodeWeight 6, where four lattice sites are equidistant), so clusters sit where filaments meet.
- **Members.** Each cluster's emitter places galaxies with a new field (UNI_FIELD_CLUSTER): a triaxial isothermal beta model, ellipsoidal radius, core 0.06–0.14 of the influence radius. Up to 3 Plummer sub-clumps sit at seeded positions (new seed channel CLUMP), and the body tapers smoothly over the outer 35%. Its normaliser is analytic, so there are no sampled integrals and nothing to wait for.
- **Counts.** Budget-consistent: a cluster's galaxies add up to its own magnitude (D-133).
- **Light.** Clusters are Extended, so their light hands over to a volume carrier as they grow (D-129), like galaxies.
- **Fraction.** Field galaxies scale by (1 − Fraction), so the total galaxy density is unchanged.
- **Realistic defaults (FClusterTuning):**
  - 15% of all galaxies are cluster members, 8.6% in rich clusters (M < −25, 300+ galaxies).
  - Cluster luminosity function: Schechter, M* = −25.3, α = −2, over −27.5 to −24.5.
  - About 214 members on average, density 1.4e-5 per Mpc³ (rich: 5.6e-6).
- **Groups stay field galaxies.** Groups (most real galaxies) as explicit parents cost 10–100× more resident clusters (D-152). They are B-004.
- **Handles.** Enable, Fraction, MStar, Alpha, BrightMag, FaintMag, BiasPower, NodeWeight, RadiusScale, CoreMin/Max, MaxClumps, ClumpWeight. In UE these are `uu.Cluster.*`, applied on `uu.Enable`.
- **Commands.** `uu.Clusters` and `uu.GotoCluster [rich]`.
- **Resolve threshold.** A cluster's resolve threshold is 5e-3 rad (about 6 px): smaller, it counts as unresolved for residency. Its light is then the volume's, which models every child by point flux, so this is consistent with it. At 1 px they would be resident out to 1.6 Gpc.
Rationale (offline):
- **T2.cluster.census:** 14,063 resident from intergalactic space; density 1.39e-5 against 1.40e-5. The web density at clusters is 3.0× the mean, against 2.0× at field galaxies. Members are exactly 15.0%.
- **T2.cluster.members:** a rich cluster has 534 members against 493 expected (+1.8σ). Its summed light is M = −25.88 against −26.00 (1.5σ). Its radial profile matches the field (KS 0.053, 99% limit 0.071).
- **T1.field.cluster:** the analytic normaliser is unbiased against sampling (mean −0.02%).
- **T2.cluster.no_reroll:** with clusters off, the web scene is the old one record for record.
- **Stills:** `still_cluster_*`.
Reroll impact: HIGH for web content (field galaxy counts scale by 0.85; clusters are new). Owner-approved direction; tune before authoring. With Enable = 0 there is no change.
Status: Provisional

D-152 — Parents are resident where their children can be visible, not where their windows can reach (amends D-101)
Question: Making clusters parents of galaxies kept 1.16 million clusters resident. D-101 required a parent within the farthest corner of its children's widest window: √3 (N_max + 1) 2^L, 600 Mpc for galaxies' 50 Mpc level.
Decision: The reach is, per child level L, min(the largest child relevance at L × K(Δm cover), (N_max + 1) 2^L) + √3 2^L, plus the influence radius.
- Δm cover = +2.5 (`kParentCoverDeltaMag`).
- The reasoning: a child cell generated without a parent is regenerated when the parent arrives. That is invisible as long as every object in it is still beyond the children's visible radius then.
- Above +2.5, at the coarsest child level only, a bright member can appear when its parent arrives.
Rationale:
- For clusters, about 330 Mpc instead of 600–840 Mpc; with D-151's threshold, 14,000 resident clusters instead of 1.16 million.
- Every existing parent's relevance was already set by its own visibility. The star and galaxy slice tables and relevances are bit-identical, and the content is too (T2.cluster.no_reroll).
Reroll impact: None for existing content (checked); it sets cluster levels.
Status: Provisional

D-153 — Sampling boxes and analytic envelope bounds for small contributors in large cells
Question: A cluster's 2–5 Mpc influence sphere sits inside galaxy cells of up to 50 Mpc. A cell's 128 probes usually all missed it, so the cluster got 380 of its 493 members and a third of its light. Inside smaller cells, the probes missed its 0.2 Mpc core, so the rejection envelope capped the core at half its density.
Decision:
- **Sampling box.** An emitter can ask for one (`bSampleBox`). Its tasks probe and place only inside the part of the cell its influence sphere overlaps: in-cell offsets per axis, with lambda scaled by the box's volume.
- **The mapping.** It is exact integer arithmetic: Lo + mulhi(offset, Span), with a 32-bit `Uni_MulHi32` because SM5 has no 64-bit integers. Span 0 is the identity, so tasks without a box place bit-for-bit as before.
- **Envelope bound.** The cluster field also bounds itself over the box (`Uni_FieldBoundBeyond`, from the box's nearest point to the centre). The envelope is max(probe max × 1.25, bound); other fields return 0 and are unchanged.
- **Task size.** FGenTaskGPU grows from 176 to 208 bytes (BoxLo, BoxSpan).
- **Users.** Only the cluster emitter uses it. Galaxies' coarse star levels might benefit too, but turning it on for them would reroll stars (B-005).
Rationale:
- T2.cluster.members: from 380 members and M = −24.82 to 534 and −25.88.
- Core fraction (within 0.1 of the influence radius): from 6.0% to 12.0% (field: 12.9%).
- 0.00% of candidates above the envelope.
- T1.box.map: 2M products exact, every offset inside its box, span 0 the identity.
Reroll impact: None for existing content (no box, no bound).
Status: Provisional

D-154 — Travel sensitivity
Question: Owner: travelling between galaxies is "hunting specks". At the naked-eye limit a bright galaxy fades beyond about 3 Mpc, and a rich cluster's mean surface brightness (~25.4 mag/arcsec²) sits just past the limit.
Decision: While the camera moves fast, sensitivity rises by itself (`UniTravel.h`, FTravelSensitivity).
- It is added to the owner's own `uu.Sensitivity`, driven by the camera's actual speed.
- 0 below 100 kpc/s, +2.5 mag at 10 Mpc/s and above, smoothstep in log speed between.
- Rate-limited: up 2 mag/s, down 1 mag/s.
- It goes through the existing sensitivity path: windows widen with it (T3.sensitivity: 0 pops on a sweep at ~2.5 mag/s), the D-139 pool guard applies, and nothing generated changes.
- +2.5 is D-152's cover, so cluster members stay complete.
- Teleports and jumps don't count as speed.
- `uu.TravelSensitivity <mag>` (0 turns it off), `uu.TravelStart`, `uu.TravelFull`. The HUD shows "dm +x (travel +y)".
Rationale: T1.travel.sensitivity, the curve and a scripted trip: peak +2.50, worst rate 2.00 mag/s, back to 0 within 2.6 s of stopping.
Reroll impact: None (render and windows only).
Status: Provisional

Note (D-151 follow-up, verification tooling): the reference pipeline (FRefPipeline, used by every offline check) scanned every registered contributor for every level every frame, the cost the runtime streamer removed in D-138. With 17,500 contributors, the 2,400-frame flythrough check ran for over 40 minutes. It now measures contributors once per frame (refreshed whenever the registry changes, so same-frame registration still works), tests boxes in doubles with a margin (superset; the exact per-cell test still decides), and skips the D-113 regeneration pass when a level's gathered set is unchanged (exactly equivalent). Old and new pipelines produce the same hash over every resident record, fade and generation statistic, on the web and galaxy scenes over 60 moving frames.
Note (D-151, volume): a rich cluster's background LUT tail (flux per child ~1e10 × distance ~1e28 cm) overflowed float and made the background NaN (T3.vol.background, T3.vol.nested). Each child table's LUT is now scaled by a power of two near 1 / (its mean flux), undone in its carriers' amplitudes (`LutScale`). The march only uses LUT × amplitude, and power-of-two scaling is exact, so galaxy results are bit-for-bit unchanged. The background with clusters is 27.43 mag/arcsec², the same as without them: clusters move light, they don't add it.

D-155 — Streamer cost and parent gating with clusters (amends D-137, D-138)
Question: With 14,000 resident clusters (17,500 contributors), T2.stream.responsive measured the streamer at 13–16 ms per frame in the web scene, against about 1 ms before. At 30 Mpc/s, 1.7% of frames had a visible galaxy cell not Ready.
Decision:
- **Measurement (D-138).** It is now incremental. Each contributor's offset from a measure anchor near the camera is computed exactly once, at registration; per frame it is a double subtraction. The anchor re-bases (one exact pass) every 30 Mpc. Its rounding bound, 4 eps (|offset| + |camera offset|), is added to the gather margins, so the gather is still a superset and the exact overlap test decides. The sort is an insertion sort from last frame's order (nearly sorted, so linear), with a budget and a fallback to std::sort.
- **Gating (D-137).** A level waits only for the parent cells that could hold a contributor reaching its widest window. That is bounded by the largest influence any object at the parent level can have (`LevelMaxInfluence`, from the archetypes and anchors placed there). Before, every galaxy level stalled while a new slab of 200 Mpc cluster cells, 200–300 Mpc away, filled.
- **Latency bound.** T2.stream.responsive's bound is now 6 frames per depth of the parent chain (two deep with clusters): a coarse galaxy cell whose cluster cell fills at the same time waits for it, beyond the visible radius.
Rationale:
- Profiled offline at 10 Mpc/s with 17,500 contributors: streamer Update about 3 ms including the CPU stand-in's generation (the GPU's work in the engine), of which bookkeeping is about 1.1 ms, back to the pre-cluster level.
- Steady-flight holes: 0 at every speed (from 100).
- T2.stream.parity is unchanged: 114,456 objects, 0 differ.
Reroll impact: None.
Status: Provisional

D-156 — Sprite style: pinpoint core plus a capped halo (replaces the D-150 glare footprint, keeps its arrival rule)
Question: Owner, in the engine: inside a dense galaxy the sky is "snowflakes". Far, bright stars are several pixels across, and size no longer says anything about distance. The D-150 footprint grew as 1 + 0.35 log2(1 + I) pixels of Gaussian sigma: a star 5 mag over exposure was ~3.3 px sigma (a 20 px quad), one at I = 5e7 nearly 10 px. Owner chose option A of three (A: core + capped halo; B: magnitude-to-size table with a cap; C: pinpoints and the engine's bloom).
Decision (`UniProject.ush`, Uni_ProjectObject):
- **Core:** a Gaussian of sigma max(true angular radius, CorePx). Brightness does not enlarge it. Its light is the intensity up to a saturation level, with a sharp knee (I / (1 + (I/Sat)^4)^(1/4): untouched below Sat/8, never above Sat). A resolved source saturates per area (Sat × (core / CorePx)^2), so a disc keeps its surface brightness.
- **Halo:** only above saturation. Energy HaloGain × Sat × (doublings over Sat); sigma CorePx + HaloGrowth × doublings, capped at HaloMax. Faint and wide: it carries "this is very bright" without turning the star into a blob.
- **Arrival (D-150 kept):** inside the last 4× before arrival the halo's driving intensity is frozen and the halo fades out; at arrival only the core, at its true size, is left (T1.sprite.arrival, now on the footprint and halo share).
- **Physical mode:** `uu.Sprite.Saturation 0` puts all light in the core with no halo (option C: the engine's bloom does the glare).
- **Handles** (live, render thread): `uu.Sprite.CorePx` 1, `.Saturation` 16 (3 mag over exposure), `.HaloGain` 0.25, `.HaloGrowth` 0.5 px per doubling, `.HaloMax` 4 px. `uu.GlareGain` is gone.
- FUniSprite grows from 48 to 64 bytes (HaloRadius, HaloRatio, two pads); the quad covers the halo when there is one, the core otherwise.
Rationale: T1.sprite.style: a Sun-like star from 3 Mpc to its arrival ramp (intensity up to 4.8e7): core size exact at every distance, core light never over saturation and within 0.005% below Sat/8, no halo below saturation, halo never over the cap and never shrinking as brightness rises, none in physical mode. Its brightest sprite is 4.0 px sigma (the D-150 glare made it 9.9 px). Most stars now draw a ~6×6 quad instead of up to 60×60, which also cuts sprite overdraw.
Light: the total drawn for a bright star is no longer its intensity (core capped, halo logarithmic). This is a display choice, render-only; the volume and every photometric budget are unaffected (D-135 holds for the core below saturation and in physical mode).
Reroll impact: None (render only).
Status: Provisional (owner tuning)

D-157 — Performance instrumentation and volume march cost
Question: Owner, in the engine: 10 FPS in a dense galaxy, 3–7 FPS near a cluster, worse at intergalactic speed. `stat unit` and ProfileGPU: when facing a cluster the GPU is 123 ms, 42.7 of 45.6 ms of one frame in `Uni.Volume.March` (30 carriers); in flight, spikes of 223 ms on the game thread. The supersampling debug view showed 1 ray per pixel almost everywhere: supersampling is not the cost.
Decision:
- **Instrumentation.** The streamer times its own phases (`UniProfile.h`, FStreamProfile: integrals, batches, batch-cell search, registration, measurement, levels, parent gate, gather, window, growth checks, task building, cell table) and counts its work (cells and tasks submitted, overlap tests, growth checks, objects read back, contributors added/removed, gathered, batch-cell scans). Engine-agnostic: steady clock, optional hooks. The subsystem times its own phases (streamer, promotion, parallax, light rig, push frame with systems and volume build) and adds Insights scopes (`Uni_Tick`, `Uni_StreamerUpdate`, `Uni_PushFrame`, `Uni_VolumeBuild`, `Uni_RenderBackdrop`, `Uni_RenderVolume`, `Uni_Gen*`). `uu.Perf` logs averages, peaks since the last call and the worst frame's streamer breakdown; `uu.PerfHud 1` shows it live; `uu.PerfHitchMs` (50) logs any slower frame's breakdown. The sprite count is read back (no stall, a ring) for both.
- **Cluster volume steps.** The cluster field's step hint capped every step inside 0.7 of the centre at 0.3 clump radii (0.008–0.018 field units): 80–190 steps per ray across the whole cluster disc. It now uses, per clump, a quarter of max(distance to its centre, its radius), so steps shrink only near a clump. Render only (the step hint is used by the volume march alone).
- **Volume quality handles:** `uu.VolumeMaxSteps` (512), `uu.VolumeStepRatio` (0.08), `uu.VolumeMaxSubsamples` (8; FUniVolumeView's pad becomes MaxSubsamples, 0 meaning 8). With `uu.VolumeDivisor` (2) these trade glow quality for GPU time.
- The frame payload moves to the render thread instead of being copied.
- **Batch cells by key.** Advancing in-flight generation batches found each batch's cells by scanning every cell of every level, once per batch per frame (O(batches × resident cells), worst at speed when windows widen and many batches are in flight). Each batch now records its cells' keys at submission and looks them up; evicted or re-submitted cells are skipped exactly as the scan skipped them. T2.stream.* (parity 114,456 objects, 0 differ; responsive, fail-closed, resize, async integrals) pass unchanged.
Rationale: gate 75/75, plus the stream group rerun with the batch lookup.
Reroll impact: None.
Status: Provisional (the game-thread spike's cause is to be read from uu.Perf in the engine)

D-158 — Seamless approach: nothing about a point of light shrinks or dims on the way in (supersedes D-150; revises D-156)
Question: Owner, in the engine after D-156: "stars grow as we approach and then shrink as we get close to them spawning". Owner's goal, in their words: "a seamless experience at all distance levels allowing us to approach a star and seamlessly resolve the star system and eventually the actual star actor without ever having any kind of unexpected behavior in the way the point of light reads". Their first instinct: peg it at a minimum angular size, let it grow once its perceived size exceeds that.
Decision (`UniProject.ush`):
- **No arrival ramp.** The D-150 rule (shrink a brightness-boosted sprite to its true size over the last 4× before arrival) is removed; it is what read as "grows, then shrinks". Uni_ArrivalDistance stays (systems and tests use it).
- **Core:** pegged at CorePx while unresolved, the true size once larger (as D-156). Light saturates per area.
- **Glare (the D-156 halo):** point sources only (stars). Not galaxies, clusters or planets: their light already resolves through size, the volume and the actor. It is driven by the current intensity, so it only grows on the way in. Its width is the core's × (1 + HaloGrowth × doublings / CorePx), capped at HaloMax / CorePx × the core: identical to D-156 while unresolved, and once the disc resolves the whole sprite scales with it (the doublings over the area-scaled saturation stop changing) instead of the glare being left behind at 4 px.
- **Swap to the actor:** a promoted star keeps its sprite's glare (UNI_SPRITE_HALO_ONLY: core weight 0); the actor replaces only the disc. Promoted planets are drawn by their actor alone, as before. The reference renderer does the same.
Rationale:
- T1.sprite.arrival, rewritten: red dwarf, Sun, giant, supergiant, each from 10^4 × its system radius to its promotion distance: core size, footprint and total light never decrease (0 of 1,600+ samples, 0.01% tolerance); all four have glare; at the swap the glare's width and light are identical with and without the actor (< 1e-5). Footprint: a pinpoint far out (1–1.7 px) to ~77 px of glare around a 19 px disc at promotion.
- T1.sprite.style (D-156's properties) holds from 3 Mpc to promotion.
- T2.parallax.promotion: planets unchanged (size at the swap exact, never drawn twice or not at all).
Reroll impact: None (render only).
Status: Provisional (owner tuning; glare strength near a star: `uu.Sprite.HaloGain`, `uu.Sprite.HaloMax`)

D-159 — Background map: whole, consistent refreshes (amends D-136)
Question: Owner: at speed one face of the background map is brighter than its neighbours, an "inside a cube" read. Cause: the map was refreshed a few faces per frame (2), and a refresh restarted from face 0 whenever the background changed. At intergalactic speed it changes every frame (camera parallax at 0.1 Mpc, and the D-154 travel sensitivity changing its LUT), so faces 0 and 1 were re-marched forever and faces 2–5 kept old light.
Decision: three maps (previous, current, back). A refresh snapshots the background's carriers, field parameters, LUT and view, marches all six faces of the back map from that snapshot (`uu.BackgroundFacesPerFrame` per frame; the first map at once), then rotates it to current. A change arriving mid-refresh waits for the next one. The composite cross-fades from previous to current over one refresh's length, so faces always agree and a refresh never pops.
Why baked at all (owner question): the map carries the unresolved light of everything from 0.1 Mpc to the horizon (effectively billions of galaxies; the resident galaxies are already drawn as sprites). Marched per pixel instead, every ray runs from 0.1 Mpc to the horizon: at half resolution ~615k long rays per frame against 6 × 256² = 393k per refresh (~131k per frame while moving at 2 faces per frame), and nothing at rest. The cost: a refresh lags the camera by ~3 frames. If the near background visibly drifts at intergalactic speed, the next step is to march the near shell (0.1 Mpc to a few tens of Mpc) per pixel with the local carriers and bake only the far shell, where parallax is negligible.
Rationale: engine only (the reference renders the map whole); needs UE (UE-13).
Reroll impact: None.
Status: Provisional

D-160 — Volume cost near and inside clusters
Question: Owner, in the engine after D-157: facing a cluster the GPU is still ~70 ms (`stat unit`: Game 16 ms, GPU 70 ms). Reproduced on the CPU reference (T2.vol.cost: the same core, 484 × 318, one thread): from 8 Mpc and from uu.GotoCluster's landing the cluster costs ~15 steps per ray over the 8–15% of pixels it covers; from inside the core (1 cluster + 63 galaxy carriers, every ray in the cluster) 22.5 s, 547 steps per ray. Three costs multiplied:
- every local carrier marched from 1 pc with log-uniform steps (s = 0.08 t): ~190 steps per ray before reaching anything, from inside a cluster;
- galaxies 4–50 px across were supersampled over their whole footprint (up to 8 × 8 rays per pixel, D-130), to resolve their 0.0125-thick disc and their arms, and each sub-ray re-marched the cluster's long chord;
- the cluster's clump centres (three hashes and a sine/cosine each) were recomputed at every sample and step hint.
Decision (volume only; placement is untouched: T2.cluster.no_reroll):
- **LUT start.** An emission-only carrier starts where its LUT is significant (the entry before the first above 1e-6 of the block's peak; nearer, every child is resolved and drawn as a sprite). Dusty carriers keep the 1 pc start (dust absorbs at any distance). `uu.VolumeLutStart`.
- **Prefiltered galaxies.** `UniField_GalaxyFiltered(p, footprint)`: the disc's vertical profile is broadened to He = sqrt(H² + (F/2)²) with its column kept, and the arms blend to their azimuthal mean past the local arm scale; Foot = 0 is UniField_Galaxy exactly. The march passes the pixel's transverse footprint (t θ_px / R). Galaxy carriers are then supersampled only for rays within 6 bulge radii of the bulge (`Uni_CarrierSubsamplesRay`). `uu.VolumePrefilter`.
- **Clump centres once per ray** for the first cluster on the ray (`UniField_ClusterC`, `Uni_ClusterStepHintC`; UniField_Cluster calls the same code with the centres it computes, so the field is one source).
Rationale:
- T2.vol.cost, inside the core: 22.5 s → 1.14 s; cluster alone 6.7 s → 0.59 s (191 → 31 steps per ray); galaxies 4.1 s → 0.53 s (11,756 → 704 supersampled pixels). From the landing: 74 → 50 ms.
- T2.vol.equivalence (inside the core, D-157 march vs D-160, 242 × 159): cluster light +0.001%, total +3.7%, galaxies +4.0%, 21× fewer steps. Against higher resolution both converge to the same galaxy light (2× and 4×: old 858 → 859 → 866, new 893 → 877 → 870): at this resolution the old march is 1.2% low and the new 2.9% high. The new error is the prefiltered dust absorbing a little less than resolved lanes (without dust it is 1.8% low). Individual pixels of small galaxies change (averaged lanes instead of supersampled ones); totals hold.
Reroll impact: None (render only).
Status: Provisional (engine timing to confirm: ProfileGPU `Uni.Volume.March`)

D-161 — Game-thread cost at speed, and carrier flicker in cluster cores
Question: Owner's engine logs (uu.PerfHitchMs): a steady ~40–45 ms per frame of "volume build" on the game thread with ~17,500 contributors, and 50–240 ms spikes when a coarse window shifted in a dense region: Submit and Growth ~170 ms (2,700 cells × 1,700 gathered contributors = 1.25M exact overlap tests), Register ~45 ms and Measure ~40 ms (56,000 galaxies registering at once), Window ~50 ms (85,000 evicted). Separately, in a cluster core "the density-marched galaxies flip around frame to frame", and a frame showed 62 carriers (cap 64).
Decision:
- **Cell binning.** Gather records, per gathered contributor, the box of window cells its sphere can reach; growth checks and task building test each cell only against its own bin plus the few wide contributors. A superset of the exact answer; the exact overlap test still decides, so results are identical (T2.stream.parity, 0 objects differ).
- **Carrier build.** The build rejects contributors whose influence radius subtends less than half the handoff start before any per-object work, using the streamer's per-frame measured distances (`NearContributors`, sorted by id: the map's order, so the carriers are identical). T2.vol.build_cost: 9.3 ms → 1.2 ms on the reference with 17,521 contributors, identical carriers.
- **Carrier caps.** Per ray: 8 → 12 carriers; past the cap the least significant (weight × angular size) is replaced, and every kept carrier fades out as its significance nears the best one left out, so carriers trade places continuously. Per frame: 64 → 128 carriers, with the same fade at the cap.
- **Bulge supersampling reach.** A ray is supersampled for a galaxy's bulge when it passes within 6 r_e plus 1.5 pixel footprints (was 6 r_e): a bulge smaller than the pixel falls inside it wherever the centre ray passes, and the switch doubled a pixel's light between frames. *Superseded by D-163 (the bulge is prefiltered instead).*
Rationale: T2.vol.continuity (new): inside a rich cluster's core, moving 0.25 kpc per frame, no visible 10 × 10 pixel block of the volume changes by more than 10% (6.9%; it was 36% with the old bulge reach). The bulk register/evict cost (56k contributors at once) is not addressed here; D-162 makes each shift smaller.
Reroll impact: None.
Status: Provisional

D-162 — Window boxes with a forward stretch; 5³ at rest after measuring 4³ (amends D-010, D-118, D-139)
Question: Owner: would a fixed 3³ or 4³ neighbourhood per level, sized with a margin beyond visibility, outperform a dynamically widened scan? D-139 widened windows symmetrically at speed (5³ → 9³, ×5.8 cells, and a 9 × 9 slab entering per shift), which is what the hitch logs showed. Owner chose to try 4³ (recalling UltraLargeScale, where each extra ring cost dearly) with forward-only stretching, and asked for the measured cost of each.
Decision:
- **Window boxes** (`UniWindow.h`): a level's window is any box of cell offsets [Lo, Hi] per axis around the centre cell, not only (2N+1)³. Slot addressing generalises per axis (Side = Hi − Lo + 1; covered axes at ≤ 2(Side + N) cells; seam unwrapping within Side − 1 + N of zero).
- **Reach** (`Uni_WindowReach`): N_eff k + 1/K cells, placed by the camera's position in its cell (`PlanWindow`, `PlaceWindowAxis`): ceil(2 · reach) + 1 cells per axis.
- **N_eff stays 1.75: 5³ at rest** (reach 2 cells). 4³ (N_eff 1.25) was built and measured, and costs more (below). The window box code takes either; it is one constant.
- **Forward stretch** (replaces D-139's symmetric widening): per axis, the window extends ahead by the lead (speed × (latency + 1), in the level's cells) along the direction of travel only, up to 3 cells; beyond that the level is suspended (velocity LOD). Sides grow at once and shrink one cell per 30 frames. Placement keeps the previous position while it still covers the reach, so a window shifts only when it must.
- The reference pipeline plans its windows with the same function (at rest), so parity holds.
Measurement (same scene, same flight, reference pipeline; `Flythrough` scene, solar camera fill, then 200 frames from 3 Gpc to 20 Mpc):

| | 5³ (N_eff 1.75) | 4³ (N_eff 1.25) |
|---|---|---|
| cells per level | 125 | 64 |
| resident objects, solar fill | 67,444 | 130,457 (1.93×) |
| of which stars (galaxy interior levels) | 49,581 | 68,071 (1.37×) |
| of which galaxies + clusters | 17,863 | 62,386 (3.5×) |
| longest gather list | 244 | 986 (D-004 trigger is 256) |
| 200 frames of flight (wall time) | 12.3 s | 52.4 s (4.3×) |
| cluster census volume | 1.01e9 Mpc³ | 4.13e9 Mpc³ (4.1×) |

Why smaller is dearer here: levels are octaves and a slice goes to the smallest level whose reach covers its relevance radius (N_eff · 2^L ≥ R). Lowering N_eff pushes slices to cells twice as wide; a 4³ window of those cells is (8/5)³ = 4.1× the volume of the 5³ window it replaces. Averaged over radii the penalty is (4/1.25)³ / (5/1.75)³ ≈ 1.4× (the stars above); slices that sit just below an octave boundary pay the full 4.1× (the galaxies and clusters above). UltraLargeScale's layers had fixed cell sizes, so there fewer cells meant less volume; here they mean bigger cells. Larger windows go the other way (7³ ≈ 0.7× the volume of 5³) at more cells per level and more per shift; not pursued.
Rationale: T1.window.boxes (new): bijection, slot stability and diffs over even, asymmetric and stretched boxes on plain, seam and covered levels; placement always covers the reach and never moves without cause. T2.stream.parity, resize, fail-closed, volume parity, responsive and async integrals pass. With N_eff 1.75 the reference reproduces the D-160 residency exactly (same objects, same gather lengths).
Reroll impact: None (N_eff unchanged from D-118; the 4³ trial was never delivered).
Status: Provisional (engine measurement: uu.Perf at speed, the forward stretch)

D-163 — Prefiltered bulges; no supersampling for prefiltered galaxies (amends D-160, supersedes D-161's bulge reach)
Question: The full gate on D-161 showed the cluster-core march at 7.8e6 steps (D-160: 1.9e6; old march 5.7e7) and 5x slower than D-160. Cause: D-161's wider bulge reach supersampled every pixel near a sub-pixel bulge, and a pixel's supersampling is the maximum over all its carriers, so each such pixel marched every carrier (the cluster included) up to 8 × 8 times.
Decision: `UniField_GalaxyFiltered` widens the bulge to the footprint: r_e' = sqrt(r_e² + foot²), amplitude × (r_e / r_e')³ (the profile's integral goes as r_e³, so its light is kept). The step hint uses r_e'. `Uni_CarrierSubsamplesRay` returns 1 for every prefiltered carrier (signature now (carrier, θ_pixel)). Placement and the unfiltered field are unchanged.
Rationale (cluster core, reference, 242 × 159): steps 7.8e6 → 1.39e6 (old march 5.7e7: 41×), galaxy light the same as with the supersampling (1022.5 vs 1023.3), T2.vol.continuity 7.1% (limit 10%). T2.vol.equivalence now separates emission from dust: galaxies without dust match the old march within 3.4% (0.6% at 2× resolution); with dust they are 6.7% brighter, because a widened lane mixes with the widened stars instead of screening the far half of the disc (10% of the light absorbed against 19% resolved). Limits: emission 5%, with dust 8%. A prefilter that keeps the screen (e.g. dust widened less than the stars) is a possible later refinement.
Reroll impact: None (render only).
Status: Provisional (engine: ProfileGPU Uni.Volume.March in a cluster core)

D-164 — The cosmic web from a baked 3D texture (amends D-012, D-130, D-136; first step of the texture plan in TEXTURE_SPECS.md)
Question: Owner: move the density fields to baked volume textures (their texture project bakes them, with full mips) so the volume stops paying for procedural fields at every sample; the background march cost 11–15 ms per face. What should the runtime read, and how do placement, integrals and the volume stay one source?
Decision:
- **Channels.** The web texture holds R walls and G filaments (each mixed over the two lattices with weight2), B nodes, A unused: `Uni_WebChannels`, the procedural web's own terms regrouped. The runtime rebuilds the density as Floor + WallW R + FilW G + NodeW B, then the bias power (`UniField_WebTex`). Linear in the channels, so mips average the density exactly (only the clusters' power acts after filtering). Width, lattices and seed are baked (the bake key); weights, floor and power stay live.
- **One source.** Cores sample it only through `UNI_WEB_TEX_SAMPLE(P, Mip)`: in HLSL a SampleLevel on the global in `Shaders/Private/UniWebTexture.ush` (wrap, trilinear, linear mips), included by every .usf; in C++ the shim's `UniWebTexSample`, the same filter over the CPU copy of the same half-exact texels (`UniCore::FWebTexture`, bound with `BindWebTexture`). Unbound it returns NaN (fail closed). `UniField_Web` with C.z = 1 (C.w = texels per period) reads mip 0, so GPU placement, CPU normalising integrals and the reference all read the texture.
- **Volume.** A textured web carrier is prefiltered by its mips: it samples the mip whose texels match the pixel footprint (no blend to the mean), steps one texel of that mip (`UNI_WEB_TEX_STEP`), and has its own log step ratio 0.2 (new per-carrier `StepRatio`, replacing a pad; 0 = the view's; a step takes the smallest active ratio, so every other carrier marches exactly as before).
- **Stand-in.** Until the owner's textures arrive, `FWebTexture::Bake` bakes today's procedural web: 128³ per period (0.78 Mpc texels for the demo's 100 Mpc), each mip-0 texel the mean of 2³ samples, mips exact 2×2×2 means, all rounded to half. The engine bakes it at `uu.Enable` (std::thread) and caches it in `Saved/UnifiedUniverse/`; it is uploaded through a compute pass from a structured buffer (`UniWebTexUpload.usf`), so the renderer stays on RDG resources.
- **Handles.** `uu.WebSource` (1 texture, 0 procedural), `uu.WebTexSize` (128), `uu.WebTexSupersample` (2); apply at `uu.Enable`.
Rationale: T1.webtex.format (half rounding, bake = channels at texel centres, mips = exact means, sampler at centres / midpoints / wrap / across mips, no seam at the tile edge, cache round trip, channels = procedural web to 4e-7, unbound = NaN). T2.webtex.field: textured vs procedural web, correlation 0.997 and mean −0.04% (galaxy web), 0.995 / −5.7% (cluster web: power after filtering). T2.webtex.census: cluster density 1.39e-5 per Mpc³ (emitter 1.4e-5), field galaxies at their slice's density, clusters on 2.87× the mean web density vs 1.88× for field galaxies. T3.webtex.background: the marched part of the background map (0.1–50 Mpc) within 0.12% of the procedural light, texel correlation 0.999, 107 → 78 steps per texel and 12× faster on the reference. The analytic tail beyond 50 Mpc is ~99% of the background's light and unchanged. Every procedural check is unchanged (the per-carrier ratio is exact when unused).
Not yet: warp textures (none delivered; the stand-in has the procedural web's own shape), normalising integrals from coarse mips (they sample the texture now, already cheap), a longer march extent (now affordable), per-pixel background at speed (next).
Reroll impact: Yes, for the web's galaxies and clusters when textured (placement reads the texel-averaged web). None with uu.WebSource 0.
Status: Provisional (UE-14)

D-165 — The noise project's adhesion web in the runtime (amends D-151, D-164)
Question: The texture project delivered its textures (Data/Noise: raw RGBA16F mip 0 and mip chains with sidecars, plus VolumeTexture assets in Content/Noise). How does the runtime use them, and do clusters stay a discrete type (owner's question)?
Decision:
- **Clusters stay discrete, placed by the field.** The texture holds the web at 1.56 Mpc texels; a cluster's core (0.1–0.3 Mpc) and clumps are far below that, its members stream in only near it, and from afar it is one sprite and one glow. So the texture decides where clusters are, and the cluster object draws the sub-texel part. Now: clusters follow the node matter channel; next: one cluster per heavy node of the node list (BACKLOG).
- **Texture.** `web_adhesion_density_mid` (uu.WebTexture; young / mid / old, or standin): R walls, G filaments, B nodes, A void (uncollapsed) matter, each a mass density in units of the tile's mean (their sum's mean is 1). Loaded from the raw files (`FWebTexture::LoadDelivered`: mip 0 and the delivered mips, as delivered; no rebuild), not the .uasset; uploaded like the stand-in. Data/Noise and Content/Noise are not tracked (owner: the noise project regenerates them).
- **Scene** (`UseDeliveredWeb`). The tile is 2^89 cm (200 Mpc: periods are powers of two; the spec's 160 Mpc is not one). Field galaxies follow R + G + A (new void weight in D.x, `UniField_WebTex`); clusters follow B; members are the node matter's share of all galaxies (9.4% for mid, from the texture's mean), replacing uu.Cluster.Fraction for this web.
- **Exact counts on spiky fields.** A linear textured web takes its period mean from the texture's last mip (normalising integral), and a power-of-two cell of at least one texel takes its mean from the mip whose texels are its size, read at its centre (`Uni_TaskCount`). The node matter peaks at ~1250× its mean: 128 probes of a cell holding a whole tile put its mean out by ~100% either way, and clusters came out 9% over their emitter over 125 tiles. Probes still set the rejection envelope, and webs with a bias power keep probing (a power of the mean is not the mean of the power).
- Missing texture: the web's emitters are removed (no web galaxies, no glow) with an error naming the file and how to regenerate it. Never the procedural web in its place.
Rationale: T1.webtex.delivered (128³, 8 mips; channel means match the sidecar to 2e-9; 299,593 mip texels within rounding of their children's mean; no seam). T2.webtex.adhesion_census: 8,944 clusters at 8.86e-6 per Mpc³ (emitter 8.8e-6), node matter at clusters 49× its mean; field galaxies at their slice's density (0.00092 vs 0.000944 per Mpc³) on matter 3.2× its mean. T3.webtex.adhesion_stills: background 83 steps per ray. The stand-in checks are unchanged.
Seen in the stills: the marched background (0.1–100 Mpc) shows the adhesion web's knots and filaments; the full frame is dominated by the analytic tail beyond the march (uniform, ~99% of the light), so the structure barely shows. Marching farther with the texture is the fix (next).
Reroll impact: Yes, the web scene's galaxies and clusters (new web, new period). uu.WebTexture standin or uu.WebSource 0 give the earlier ones.
Status: Provisional (UE-14)

D-166 — Per-pixel background while travelling (amends D-136, D-159)
Question: Owner: bring the top end down so any speed works; the cached background map may stay as a mode below a travel speed. With the web a texture (D-164: ~12× cheaper per ray), is per-pixel marching affordable?
Decision: `uu.BackgroundMode` (renderer): 0 the direction map (D-136/D-159); 1 per pixel, every frame; **2 (default) per pixel while the background is changing** (its version bumps whenever the camera moves a quarter map texel's parallax, i.e. any travel), the map once it has been still for `uu.BackgroundStillFrames` (30). Per pixel, the background carriers are appended to the local ones and marched in the one volume pass at the march resolution (uu.VolumeDivisor), so no refresh lags the camera and no face is stale. Switching to per pixel drops the map; when the map comes back it is marched whole before it is shown (no cross-fade from stale light).
Rationale: the union march and the map + composite agree to 0.5% of the light (T3.vol.cube). Cost per frame: a half-resolution 1080p march is ~0.5M rays, about four times a two-face map refresh (2 × 256² rays), and each textured web ray costs ~1/12 of a procedural one on the reference. Needs the engine measurement: ProfileGPU `Uni.Volume.March … (with the background)` at speed.
Reroll impact: None (render only).
Status: Provisional (UE-14)

D-167 — Clusters from the adhesion node list; half-precision web texture on the CPU (amends D-151, D-165)
Question: Owner chose (of: rank-map to the LF, light traces mass, all nodes as groups) to rank-map: one cluster per heavy node, magnitudes from the tuned cluster luminosity function by rank. And the density textures are moving to 256³: what does that cost on the CPU side?
Decision:
- **Catalogues** (`FCatalogue` in the universe definition): authored objects at fixed tile positions, repeated every 2^PeriodBits cm. The generator emits a cell's entries like anchors (CPU-authored, reserved indices from 2^20, `UNI_OBJ_FLAG_ANCHOR`); each copy gets its own seed (cell seed with a catalogue "contributor", then the copy's index), so radius, colour, shape and members differ per tile while position and magnitude are the entry's. A catalogue keeps its levels active (it repeats every tile); the streamer sends its cells through the anchor path.
- **Rank-map** (`UseDeliveredWeb` with the node list): K = cluster density (uu.Cluster.Fraction, the LF) × tile volume = 113 per 200 Mpc tile; the K heaviest nodes, the i-th at the LF quantile (i + 1/2) / K (−26.5 to −24.5 for mid). The cluster web emitter is removed. Field galaxies follow walls, filaments, void matter and the node matter the clusters do not take (the lighter nodes: groups), scaled uniformly by 1 − (top-K node mass) / (all node mass); members are uu.Cluster.Fraction again. Without a node list the node-channel placement (D-165) remains.
- **Half on the CPU.** `FWebTexture` keeps every mip as RGBA16F halves (the delivered bytes; the stand-in rounds to them), so a 256³ tile is 128 MB on the CPU as on the GPU (was float32, twice that); the CPU sampler decodes through a 64K table. The upload sends the halves packed two per uint and the upload shader decodes them (lossless).
Rationale: T2.webtex.nodes: 113 of 4,125 nodes are clusters, heaviest brightest; 14,125 clusters resident over 125 tiles, every one at its entry's exact position and magnitude, exactly 113 per tile; a regenerated cell is identical; field galaxies at their slice's density (0.000862 vs 0.000886 per Mpc³) on matter 4.25× the mean. All earlier web texture checks unchanged with half storage (the CPU reference samples ~2× slower; the GPU is unaffected).
Reroll impact: Yes, the web scene's clusters (now on the node list) and field galaxies (weights).
Status: Provisional (UE-14)


D-168 — Contributors woken by distance; streamer bookkeeping at intergalactic speed (amends D-138, D-155, D-161)
Question: Owner: under 10 fps at intergalactic speeds (game thread 50–700 ms per frame, GPU 10 ms), back to 60 once stopped. The log: Measure ~155 ms every frame, Register up to 294 ms (+356k contributors in one frame), Window 195 ms (−332k), Submit up to 100 ms.
Diagnosis: every emitter object of every Ready cell was registered as a contributor at once, ~330k in the flythrough scene once travel sensitivity (D-154, +2.5 mag) widens the galaxy windows. Almost none can matter: a galaxy only feeds its stars' windows, which are kpc across around the camera, and the volume only asks for those subtending its reject angle. And Measure's insertion sort had a budget of 16 moves per entry before falling back to a full sort; at speed the order changes enough to spend it every frame (16 × 330k moves of 88-byte entries).
Decision:
- **Lazy wake.** A Ready cell notes its emitter objects; each is registered (contributors made, measured, gathered) only while it could matter: distance − influence ≤ the reach of any window of its coarsest child level ((N_max + lead_max + 1)·√3 cells, so every Gather sees exactly what it saw with everything registered), or it subtends WakeTheta or more (the engine sets it to half the volume's reject angle each frame). It sleeps past 1.5× both. Cells are culled by their bound (the level's largest influence) before any object is looked at, and each scan records how far the camera may move before anything in the cell can change state: until then the cell costs one distance per frame. Object distances are cell-local doubles (no 128-bit subtraction per object).
- **Measure.** Sort budget 2 moves per entry, then std::sort. The anchor re-bases in place, keeping the list's order (was a rebuild from the map in id order, then a full sort), every 300 Mpc (was 30: the two-step rounding at 300 Mpc is ~1e12 cm, inside every margin, and MeasureErr already carries it).
- **Growth checks** test only contributors the cell does not already have, and a Ready cell checked before is tested only against ids gathered since (exact overlap test either way). The gathered-ids baseline now advances only when the check runs (a frame whose parents were not ready used to advance it without checking).
- **Parent gate:** each parent level's not-yet-Ready cells and their camera offsets are listed once per frame, not walked per child level.
- New profile phase Wake (and counters woken / slept / awake / asleep in uu.StreamBreakdown and the hitch line), and Backend (the backend's submits, nested in Batches or Submit: on the CPU reference all of generation, in the engine the RDG enqueue).
Rationale: T2.webtex.travel: the flythrough scene with the adhesion web and node list, from rest in the Milky Way accelerating to 100 Mpc/s with travel sensitivity, 3-frame backend latency. Ready cells and their objects at four points of the flight identical with everything registered (27,572 cells, 958,766 objects, 0 differ). Bookkeeping per frame (the update less the backend's generation) on the reference: 42.7 ms mean / 292 worst with everything registered (317k contributors; with this decision's other changes) → 11.5 ms mean / 55 worst woken by distance (44k awake of 317k emitter objects). Before this decision (everything registered, old sort, old growth and gate): 82 ms mean / 470 worst. T2.stream.* unchanged (parity 0 differ, volume parity identical).
Not yet: Submit and Window still scale with the cells entering a frame (a coarse shift or a sensitivity resize brings 500–1,000 cells at once: 20–40 ms on the reference); a per-frame submission budget would spread them.
Reroll impact: None (the same cells, the same objects).
Status: Provisional (UE-15)

D-169 — Web debug views (uu.WebDebug)
Question: Owner: going only by cluster and galaxy placement makes the overall shape of the web hard to verify; are there debug modes for its components?
Decision: `uu.WebDebug` (renderer, render-only): the volume pass draws the textured web in false colour instead of its light (sprites still drawn on top), marching the background carriers per pixel. Core `Uni_WebDebugMarch` (UniVolume.ush, shared with the CPU reference): the first textured periodic web carrier, front to back, one texel of the footprint's mip per step, out to `uu.WebDebugDepth` (150 Mpc), with an emission-absorption transfer: a sample shows only above `uu.WebDebugThreshold` (2) times its mean, full strength two decades above, opacity `uu.WebDebugOpacity` (0.3) per texel at full strength, and a mild fade with distance so near structure reads in front. Modes: 1 channels (walls blue, filaments green, nodes red, void grey, mixed by overdensity), 2 walls, 3 filaments, 4 nodes, 5 void (each over its own tile mean, heat ramp), 6 placement (the galaxy web's own field, weights and floor included, over its mean: what galaxy placement reads). No texture bound: dark magenta (never an empty-looking web). Volume debug mode 5 internally; uu.VolumeDebug keeps 1–4.
Rationale: T2.webtex.debug on the adhesion web with its node list, from intergalactic space: resident clusters in view sit at 2.2× the node view's mean brightness, field galaxies at 1.34× the placement view's; every ray finite, 192 steps per ray (budget 1,024); unbound flagged. Stills: still_webdebug_channels / _nodes / _placement.
Reroll impact: None (render only).
Status: Provisional (UE-15)

D-170 — Marching the textured web farther: measured, not adopted (closes B-008)
Question: B-008: the analytic tail past the march (0.5 periods) is ~98% of the background's light and uniform, so would marching the textured web much farther (cheap with mips) make its structure show?
Measured (reference, adhesion web, intergalactic space, full background with the tail): marching to 16 periods (3.2 Gpc), with log steps of 0.05 past 0.5 periods each sampling the mip matching the step: total light +0.04% (same integral), the march carries 42% of the light instead of 2%, 144 → 215 steps per ray. The image is unchanged to the eye and in contrast (std/mean of 4 px and 20 px blocks 0.301 → 0.300, 0.298 → 0.297; that contrast is the pixels' solid-angle falloff, the same in both). Far shells add structure at ever smaller angles and average out within a pixel; the integrated background is isotropic, as the real extragalactic background is.
Decision: not adopted (no code kept). The web's visible structure in the background is the near ~100 Mpc over a nearly uniform pedestal; showing it more is an art choice (a near-web glow, or a dimmer pedestal), now B-010 for the owner.
Reroll impact: None.
Status: Measured

D-171 — Textured web step: 2 texels (amends D-164)
Question: With the 256³ adhesion web the background march went from 83 to 144 steps per ray (texels half the size). Can the textured web step 2 texels of its mip instead of 1?
Decision: UNI_WEB_TEX_STEP = 2. Measured on the adhesion background from intergalactic space (marched part, against 1 texel): light −0.01%, pixel correlation 0.99996, median pixel 0.1%, 99th percentile 1.4%; 144 → 83 steps per ray, 40% less time. 4 texels: 55 steps, but 99th percentile 9.7% (correlation 0.997), not taken.
Rationale: T3.webtex.background (cube light −0.13%, correlation 0.999, 107 → 50 steps per texel against the procedural web, 11× faster), T3.webtex.adhesion_stills (83 steps per ray).
Reroll impact: None (render only).
Status: Provisional (UE-15)

D-172 — Morphology-density relation for cluster members (amends D-151, D-163)
Question: Owner: inside a cluster many galaxies show as more than a dot; is that realistic, and what about its cost? Rich-cluster interiors really do show dozens of resolved galaxies (the Milky Way's sparse sky is a poor group's near a void). What was unrealistic: members rolled the same spirals as the field, while cluster cores are dominated by ellipticals and lenticulars (Dressler 1980).
Decision:
- **Placement decides** (shared core, `Uni_TaskEarlyType` in UniGenTask.ush): a member placed by a cluster's field is an early type with probability Outer + (Core − Outer) exp(−r / Scale), r its distance from the cluster centre in influence radii (~4 cluster radii; virial ~0.25), drawn on a new channel MORPH. Defaults Core 0.8, Outer 0.2, Scale 0.25 (~67% inside 0.1, ~50% to 0.25, ~35% to 0.5, ~25% beyond): carried by the cluster's field parameters C (unused before), so anchors and catalogue clusters have them too; `uu.Cluster.EarlyCore` (0 = off), `EarlyOuter`, `EarlyScale`, `EarlyColour`. The object gets `UNI_OBJ_FLAG_EARLY` (32) and a redder colour (its colour model −0.07 in log10 K, ~5600 → ~4800 K).
- **Shape** (CPU contributor, the emitter's new early shape): a spheroid of weight 1.5, r_e 0.06–0.12 influence radii (~3 kpc at M*), axis 0.55–0.95, disc weight 0–0.6 skewed to 0 (elliptical to lenticular), no arms, rolled on their own stream names. Its stars follow it. No dust carrier for early types.
- **Bulge prefilter keeps the light inside the cut:** D-163's widened bulge is rescaled by the ratio of its in-cut share before and after widening, P(6, 3.67 √(Cut / r_e)) (closed form). Render only. Without it a spheroid-dominated galaxy a few pixels across lost ~10% of its light (spirals ~3%).
Rationale: T2.cluster.morphology (anchored rich cluster, 534 members, 39% early; per radial bin within 2.4 sigma of the rule; galaxies identical to the rule off except early types' colour and flag; early types redder (R/B 1.16 vs 1.03); no arms; dust-free; late types' parameters unchanged; 0 of 3,028 web-scene field galaxies early; galaxy volume steps in the core −8%). T2.vol.equivalence: galaxies' emission +2.3% against the exact march (was −3.4% before D-172 with spirals, −10% with early types before the in-cut rescale). T2.vol.cost: galaxies inside the core 448 → 294 ms on the reference. T2.cluster.members unchanged (positions and magnitudes are).
Not done (cost): per-ray carriers stay capped at 12 and per-frame at 128 (D-161); inside a rich core the march is ~36 steps per ray, 31 of them the cluster's own glow. Impostors for small galaxies wait for the warp textures that would make galaxy samples costly (BACKLOG B-011).
Reroll impact: Yes, cluster members only: early types' colour, shape and stars (their positions and magnitudes, and every late type, unchanged). uu.Cluster.EarlyCore 0 restores them.
Status: Provisional (UE-15)

D-173 — Spreading entering cells over frames (B-009)
Question: After D-168 the remaining game-thread spikes at speed came from cells entering in bursts: a coarse window shift, the stretch growing, or a travel-sensitivity resize brings hundreds of cells per level, and several levels shift on the same frame (their cell boundaries nest), so up to ~1,500 cells were built in one frame.
Decision: `FUniStreamer::DeferSubmissions`. A waiting cell's slack is the frames the camera needs, at this frame's speed, to bring it inside the visible radius (its Chebyshev gap beyond the radius over the fastest axis's cells per frame, so never an overestimate). Built now: cells within the measured latency + SubmitSlackFrames (4), cells a finer level's parent gate would wait on (D-155's reach test, factored out as `ParentReachOf`; deferring those stalled their child levels: +1 frame of latency in T2.stream.responsive), and regenerated cells (they were drawn). Of the rest, at most `SubmitDeferPerLevel` (48; `uu.SubmitDeferPerLevel`, −1 off) per level per frame, least slack first; the others stay Waiting (undrawn: never Ready). Frames a cell was deferred are not counted as its latency. Profile: CellsDeferred (hitch line "cells deferred").
Rationale: T2.webtex.travel (flythrough, 0 → 100 Mpc/s): peak cells built in one frame 1,493 → 1,167; cells Ready in both runs identical (27,056 cells, 0 differ; 516 Ready in one run only at the sampled frames). T2.stream.responsive: latency bounds unchanged (6 / 12 frames), 0 holes. On the 2-core reference the frame-time percentiles did not move beyond run-to-run noise (p95 40–57 ms either way): most of a burst is cells needed now (the sensitivity ramp grows the visible radius itself, so its whole new shell is urgent), so spreading the rest is a modest gain. Kept: it lowers the worst burst deterministically and costs nothing measurable.
Not done: the bursts left are urgent; the levers are cheaper task building per cell, or staggering the sensitivity resize across levels (each level taking the new radius a frame or two apart). Worth it only if engine hitch lines still show Submit spikes while accelerating.
Reroll impact: None (when a cell is built, never what it holds).
Status: Provisional (UE-15)

D-174 — Galaxy groups from the node list; dwarf galaxies (B-004, owner's option 2)
Question: Most galaxies live in groups. As explicit parents of all their galaxies, groups would have to stay resident wherever their brightest member can be seen (~330 Mpc: 100k+ objects, D-152). Owner chose option 2: groups from the web's node list that place only faint members.
Decision:
- **Dwarf archetype** (kDwarf = 4): M −16..−12, below the galaxy function's faint end (−16), so it is new content, never a second copy of a field galaxy: Schechter faint end α −1.3; 4 kpc at −16 to ~1.3 kpc at −12 (R ~ L^0.3); bluer; thick discs, weak or no arms, small bulges; no dust; stars by the galaxy budget rule (D-133). Placed only by groups.
- **Group archetype** (kGroup = 5): a catalogue object (D-167 machinery) at every node after the clusters (4,012 per 200 Mpc tile). Its light is its dwarfs' (budget-consistent, as cluster members are): the median-mass group holds `uu.Group.Dwarfs` (20), each group in proportion to its node's mass (heaviest ~140), magnitude −2.5 log10(N × mean dwarf flux), −19.6 .. −14.6. Field: the cluster β-model (core 0.10–0.25, axes rolled, no clumps, no morphology rule), 0.25 Mpc at M −17 (R ~ L^(1/3)), dwarfs out to 3 radii, sampling boxes on. ThetaMin 0.02: its glow is far below any surface-brightness limit, so it is resident only within ~20 Mpc (its dwarfs are visible to a few Mpc). The group's bright galaxies are the web's field galaxies as before (they already follow the lighter nodes: D-167).
- `uu.Group.Enable` (1), `uu.Group.Dwarfs`; `uu.GotoGroup [rich]` (rich: M < −18); uu.Clusters also lists groups. Flythrough: no dwarf within 50 kpc of the anchor galaxy.
- **D-168 follow-up:** uu.Clusters and uu.GotoCluster counted registered contributors, which since D-168 are only the awake ones; they (and the group commands) now walk resident emitter objects (`FUniStreamer::ForEachEmitterObject`).
Rationale: T2.webtex.groups: 4,012 groups per tile after 113 clusters (heaviest brightest); from intergalactic space 984 resident, every one at its entry's exact position and magnitude; every field galaxy and cluster identical to the scene without groups (580 cells); dwarfs all within M −16..−12; the heaviest group's dwarfs with the camera at its centre 139 for a budget of 142.1 (0.98×, −0.3 σ). T2.stream.* and T2.cluster.no_reroll unchanged (new archetypes change nothing else; one more volume LUT block).
Reroll impact: None for existing content (new archetypes only; field galaxies, clusters and stars of existing galaxies identical). New: groups and their dwarfs (and the dwarfs' stars). uu.Group.Enable 0 removes them.
Status: Provisional (UE-16)

D-175 — Catalogue lookup by tile cell (D-174 follow-up)
Question: Owner's engine log after D-174: game-thread spikes of 50–450 ms at speed in Submit (40–250 ms of its own) and Batches (30–450 ms), with the backend calls under 1 ms. Offline the travel check did not show it (its flight holds the full-sensitivity window only briefly).
Cause: a cell's catalogue objects were found by walking every entry at its level, about 100 µs per cell for the 4,012 groups at L86, twice per cell (HasAuthored at submission, the authored content at allocation). At travel sensitivity the L86 window is up to 13³ cells and a shift brings hundreds.
Decision: `FRefGenerator` indexes catalogue entries per level by their cell within the tile (hash of the tile-local cell index; cells larger than a tile, or a tile deeper than 21 levels below it, keep the walk); a cell looks up its bucket, sorts the refs into the walk's order and tests each exactly as before.
Rationale: T2.webtex.catalogue_lookup: 8,788 cells over 13³ per catalogue level around two cameras, 530,871 catalogue objects, identical to the walk (entries, offsets, order); 6.1 µs per cell against the walk's 106 µs. T2.webtex.nodes, T2.webtex.groups, T2.stream.parity unchanged.
Reroll impact: None.
Status: Provisional (UE-16)

D-176 — Adaptive wake angle (D-168 follow-up)
Question: Owner's hitch log after D-175: at intergalactic speed with travel sensitivity the awake set still held tens of thousands of clusters and groups. Most were awake only because they subtended more than WakeTheta (half the volume's reject angle), not because their influence reached a window; the volume keeps at most MaxCarriers = 128 of them (the largest, D-161).
Decision: the angle threshold adapts: Th = WakeTheta x 2^(WakeStep / 4). It rises a quarter octave while more than 2 x `WakeAngleTarget` (512) objects are awake for their angle alone, falls below half the target, holds 4 frames between steps, and never goes below WakeTheta. Each scan counts its cell's angle-only objects (FCell::AngleAwake). The window-reach criterion (what generation needs) is untouched. Profile: AwakeByAngle, WakeStep.
Rationale: the angle wake only feeds the volume builder, which keeps the 128 largest; 512 leaves 4x headroom. Generation is unchanged (T2.webtex.travel: cells Ready in both runs identical).
Reroll impact: None.
Status: Provisional (UE-17)

D-177 — Hybrid cluster split; groups as labels; dwarfs from the map (owner's choices)
Question: Owner's hitch logs after D-175/D-176: game-thread cost at intergalactic speed still too high, with clusters (44k awake) and groups (15k awake) the bulk of the contributors, because a cluster's bright members can be seen from ~330 Mpc (D-152), so it had to stay awake that far. The owner asked whether galaxies should simply be spawned against the density maps. They wanted to keep the look of the clusters and named clusters and groups. Options offered: a pure density field; a hybrid split; groups as labels. Owner chose **"Hybrid split"** and **"Yes, map dwarfs"**.
Decision:
- **Bright members authored (catalogue clusters).** A catalogue cluster's members at galaxy levels from L86 (M brighter than −20.82; ~5.3% of members) are generated on the CPU with the catalogue, around each copy, from the copy's own contributor (`FRefGenerator::CopyMembers`). The copy's object and contributor are built exactly as the streamer builds them (shape, orientation, influence, D-172 early shape).
  - Count: Poisson(NTotal x BrightRatio), at most 64.
  - Magnitude: a bright slice by its share, then the slice's inverse CDF.
  - Position: drawn from the contributor's field (UniField_ClusterC), rotated to world.
  - Morphology, radius and colour: D-172's rule; radius and colour as generated objects draw them.
  - Seeds: all draws keyed by the copy's object seed on the new channel UNI_CH_MEMBER and the member's rank.
  - Indices: kFirstMemberIndex (2^21) + entry x 64 + rank.
  - Members are cached per copy, sorted by level.
- **Lookup.** Each entry is indexed at every member level under every tile cell its members' reach can overlap, with the copy's cell offset. The reach bound is the radius model's largest draw x the influence multiplier x the largest cut, plus 1%. A cell looks up one bucket, rejects copies whose reach misses it, then visits only that level's members. HasAuthored stops at the first member.
- **Field sampler (`SampleClusterField`).** It draws the cluster field exactly. The proposal is the body without its taper (an ellipsoidal β model, radius by safeguarded Newton on its CDF) plus the clumps without their edge (Plummer spheres, closed-form radius), each chosen by its mass. Since f ≤ h, accepting with f / h gives f, and about 85% of draws are accepted. The first version used uniform rejection in the sphere, capped at 256 tries. That needs ~1,400–25,000 tries per point for core radii 0.12–0.02, so it silently dropped most members of compact clusters; never committed.
- **The emitter's share.** The cluster emitter now places kMember (6), the galaxy archetype with its LF cut at −20.82. Its NTotal is scaled by (1 − PBright), and BrightRatio = PBright / (1 − PBright), so the total is unchanged. Its children are at finer levels, so a cluster is woken only as far as those windows reach.
- **Groups and dwarfs.** Groups are labels: catalogue entries with no emitter (no contributor, no bookkeeping). They keep their names, positions and magnitudes (their light is the dwarfs' worth, D-174), and uu.GotoGroup / uu.Clusters find them as resident objects (`FUniStreamer::ForEachObjectOf`). Dwarfs come from one root emitter on the web's node matter (textured), with NTotal = the groups' dwarfs per tile volume.
- **Engine.** GoToHost takes 3 radii as a group's extent. A cluster's member estimate includes its authored members.
Rationale:
- Offline flight (T2.webtex.travel, 0 → 100 Mpc/s, reference container):

  | | p50 | p95 | Worst | Frames over 33 ms |
  |---|---|---|---|---|
  | Lazy | 6.8 ms | 19.5 ms | 24.0 ms | 0 |
  | Eager | 10.5 ms | 48.6 ms | 69.0 ms | 28 |

  - Lazy run: awake contributors peak at 2.9k of 101k emitter objects. Before the split: ~60k awake, and the worst frames had 35–57 ms of Submit.
  - Cells Ready in both runs identical (27,163 cells, 553k objects).
- Member cost per cell (HasAuthored + content, 11³ cells around the void camera, cold / warm): L86 6.4 / 2.9 µs; L87 28.6 / 3.8 µs. The 27-bucket version measured 282 / 40 µs at L87.
- T2.webtex.members: members inside their cluster's cut (worst at 0.71 of it); 65 for 62.1 expected over the 3 clusters within 30 Mpc (0.4σ); authored bright, emitter members faint; early types 0.80 in cores and 0.23 outside; member cells identical regenerated and from a fresh generator; the sampler's radial distribution equals plain rejection's (KS 0.009 against a 1% bar of 0.027).
- T2.webtex.groups: groups are labels (0 contributors), all at their entries exactly; galaxies and clusters identical with groups off; the dwarf emitter's total equals the groups' dwarfs exactly; dwarfs on node matter (54x its mean).
Reroll impact:
- Yes, cluster members of catalogue clusters: the bright ones move to new positions and seeds, and the faint ones reroll, since their emitter's count and child archetype changed. Field galaxies, clusters themselves and stars of non-member galaxies are unchanged.
- Dwarfs reroll: they now come from the map, not per group. Groups keep their positions, names and magnitudes.
- Scenes with procedural clusters (no node list) are unchanged.
Status: Provisional (UE-17)

D-178 — Galaxy disc-level modes and per-arm asymmetry (SPRITE_PASS_PLAN §3 A1; B-012)
Question: Owner's art direction: realistic pulled towards stylised, and no two galaxies alike. Every galaxy gets a seeded config; the home galaxy may keep one fixed plausible set. UltraLargeScale's galaxy generator (read-only, connected 2026-11-04) varies shape beyond the spiral's parameters with modes that come almost free with the spiral math. Our arms were one m-fold cos wave, perfectly symmetric.
Decision:
- **Field parameters grow to eight float4** (FUniFieldParams A..H, FFieldParams V[32], 128 bytes; FGenTaskGPU 208 → 272). E..H are zero for every existing field, and every field treats zero as off.
- **Galaxy field** (`UniField_GalaxyFiltered`):
  - E = (warp amplitude, warp twist, lopsidedness, flare), F = (arm asymmetry, pitch tightening, shape seed).
  - Warp: the disc and arms move vertically by A rn² sin(θ − φ − twist·rn), rn = r / cut. The node line precesses with radius; the bulge is not warped.
  - Lopsidedness: the disc's density × max(1 + a·rn·cos(θ − φ), 0). Its azimuthal mean is 1, so the disc's light is unchanged.
  - Flare: the scale height × (1 + f·rn), with the face-on surface density kept.
  - Per-arm asymmetry (`Uni_GalaxyArmsAsym`), all from one hash of the shape seed per arm:
    - each arm has its own pitch factor (1 ± 0.3a), phase jitter (± a quarter spacing), strength (1 ± 0.6a) and end radius;
    - each arm's profile is the symmetric wave's own lobe, so at zero asymmetry this is the old wave;
    - arms merge by max;
    - pitch tightening winds arms tighter inward: (u + T u²/2) / tan p.
  - Seed channel DISCMODE (new) derives the phases and per-arm values.
  - Prefilter (D-160):
    - the blend uses the local winding rate;
    - asymmetric arms blend to their own mean (strength and end fade averaged over the arms);
    - Foot = 0 is the placement field, as before.
- **Ranges.**
  - Galaxies: warp 0–0.14 (skewed small), twist ±1.2 rad, lopsidedness 0–0.35, flare 0–1.2, asymmetry 0.1–0.7, tightening 0–0.5, shape seed. A warp of 0.14 is ~1.2 kpc at the visible edge of a 15 kpc galaxy; the Milky Way's is ~1 kpc.
  - Dwarfs: more irregular (lopsidedness 0.1–0.6, asymmetry 0.3–0.9).
  - Early types keep none of these.
  - The Milky Way anchor has one fixed plausible set (owner): warp 0.09, twist 0.4, lopsidedness 0.08, flare 0.5, asymmetry 0.2, tightening 0.15.
- **Owner, for A5 (palette):** each galaxy gets its own colour or palette, plausible but stylised.
Rationale: T2.galaxy.shape:
- With every new parameter at zero the field is the D-172 field bit for bit (0 of 48,000 points over 24 rolled galaxies, filtered and not).
- Arms at asymmetry 1e-7 are within 8.5e-6 of the symmetric wave.
- The warp moves the density-weighted mid-plane by its formula (correlation 1.000, gain 0.994).
- Light with lopsidedness 0.35: 1.0000 of the plain disc; with flare 1.2: 1.0004.
- Prefiltered light within 1.08% for rolled galaxies at a 0.03 footprint (0.53% with their modes off). It was 2.8% before the asymmetric mean.
- Contact sheets: still_galaxy_modes_off / _on.
- T2.gen.slicing at one probe seed went from 0.9975 to 0.9711 at 16 cells per axis. Over 16 seeds the 128-probe estimator is unbiased there (mean 1.001, std 0.027; 1.016 / 0.033 with the modes off), so the old pass was one lucky draw (D-103's coarse-cell noise). The check now tests the 16-seed mean (within 1% or 3 standard errors).
- The volume march's galaxy step hint widens the disc slab by the flare and by the warp reachable within one step (it stepped over warped discs: T3.vol.march face-on 8.0% → 0.46%, 85 → 91 steps per ray).
Reroll impact: Yes, every galaxy's and dwarf's stars (their field changes; the Milky Way's too). Counts are normalised by each contributor's integral, so totals hold. Positions and magnitudes of the galaxies themselves are unchanged, and so is everything outside galaxies.
Status: Provisional

D-179 — Galaxy textures: warps, disc noise and star-forming knots (SPRITE_PASS_PLAN §3 A2/A3; amends D-178)
Question: Owner: galaxies should not look samey; the delivered warp volumes can carry the local variance, with disc-level modes (D-178) for the overall shape.
Decision:
- **Textures.** The galaxy field may sample three of the noise project's textures (`Data/Noise`, not tracked in git):
  - `warp_fine` (curl displacement RGB, RMS 1, 4 periods a tile; A fBm) displaces discs and arms;
  - `warp_large` (2 periods a tile) displaces bulges;
  - `galaxy_detail` (R ridged lanes, G star-forming knots, B fBm, A superbubbles) modulates discs (B) and arms (G).
- **Loader.** `FWebTexture::LoadDelivered` also reads RGBA8 and RGB8 UNORM (as halves of k / 255). The size tells the format apart. Every texture is RGBA16F on the GPU and halves on the CPU, so both read the same values.
- **Binding.** CPU: `BindGalaxyTexture(slot)`, `UNI_GAL_WARP_FINE` / `_WARP_LARGE` / `_DETAIL` in the shim. HLSL: three Texture3D in UniWebTexture.ush, sharing its wrap / trilinear sampler. Engine:
  - `FUniRenderer::SetGalaxyTexture` / `RegisterGalaxyTexture`, uploaded by the web texture's upload pass;
  - bound in the generation probe and place passes and the volume pass;
  - unbound: the 1³ NaN dummy (fail closed);
  - `UUniSubsystem::SetupGalaxyTextures` at uu.Enable;
  - `uu.GalaxyTextures` (1).
- **Field** (G, H slots):
  - G = (disc warp, bulge warp, warp frequency, detail frequency); H = (disc fBm, arm knots, frame twist share, texels per period).
  - The disc family reads its textures in a frame rotated with the spiral's winding × rn × twist share (UltraLargeScale's noise frame), with z at twice the lateral rate (features read as sheets).
  - Texture offsets come from the shape seed.
  - Each read takes the mip whose texels match the footprint: mips are means, so this is the prefilter. Placement reads mip 0.
  - The bulge warp fades out between 0.3 and 0.5.
  - Disc reads happen only within 12 flared scale heights of the plane, plus the warps' reach. Beyond that the disc term is dropped (below e^-12 of its peak). This keeps a textured galaxy's normalising integral at ~0.7 s on the reference (2.2 s ungated); it runs on worker threads in the engine.
  - The march's step hint widens for the texture warp.
- **Amounts.**
  - The disc warp is about a tenth of its own wavelength (amount 0.008–0.022 at frequency 0.8–1.6). Larger amounts fold the structure, compressing and stretching the disc and its light.
  - Bulge warp 0–0.012; detail frequency 2–4; disc noise 0.2–0.6; knots 0.3–0.9; twist share 0.5–1.
  - Dwarfs are stronger (warp 0.012–0.03, noise 0.4–0.8, knots 0.5–1).
  - Early types stay smooth. The Milky Way has one fixed set.
  - `UniScenes::UseGalaxyTextures` adds the ranges; it is called only with the textures loaded.
- **Amends D-178:**
  - Asymmetric arm lobes now add instead of merging by max. Each lobe still ends smoothly at zero, and jittered arms brighten where they overlap. The prefilter's arm mean is now exact: a max had under-counted it by up to 3% for strongly asymmetric galaxies (prefiltered light 1.4% → 0.48% over T2.galaxy.shape's galaxies).
  - The disc warp range is widened at the owner's request (amplitude 0–0.22, bias 1.4: up to ~2 kpc at the visible edge).
Rationale: T2.galaxy.textures:
- Formats: textures 128³ with 8 mips; 2,048 RGBA8 texels equal k / 255 as halves; warp_fine identical to its raw bytes.
- A textured galaxy reads NaN with the textures unbound.
- Bound, over 6 rolled galaxies: prefiltered light within 1.00%; light with versus without the textures within 3.2%; the disc-plane correlation with the untextured field is 0.84, so the shape changes.
- Normalising integral 669 ms.
- The textured Milky Way from the solar camera places 46,737 stars against 47,051 untextured (0.993×), none non-finite.
- Still: still_galaxy_textured (patchy discs, clumpy arms, wiggled lanes).
T2.galaxy.shape still passes, prefiltered light within 0.48%.
Reroll impact:
- Yes: every galaxy's and dwarf's stars where textures are on (the engine by default; offline scenes only when UseGalaxyTextures is called).
- D-178's asymmetric arms reroll stars a second time (sum instead of max).
- uu.GalaxyTextures 0 gives the D-178 field (with the additive lobes).
Status: Provisional (UE-19)

D-180 — Cloud-like dust lanes (SPRITE_PASS_PLAN §3 A4; render only)
Question: Owner: cloud-like textures could shape the obscuring of light and the dust lanes, with physically based absorption kept.
Decision: The dust carrier of a textured galaxy (D-179) reads galaxy_detail's ridged channel (R) and superbubble channel (A) through two dust-only parameters, D.z and D.w, set by `DustFrom` from the archetype's volume profile:
- Lanes: dust density × max(1 + Lanes (2R − 1), 0). Linear, so a mip's mean is the texels' mean.
- Holes: dust density × max(1 − Bubbles (2A − 1)⁺, 0).
- The dust gets no star-forming knots. It follows the emission's warps and fBm as before.
- Defaults: Lanes 2.2, Bubbles 0.8 (`FVolumeProfile::DustLanes`, `DustBubbles`).
- Absorption and reddening are unchanged (D-132), and so is the emission field.
Rationale: T2.galaxy.dust, 4 rolled galaxies:
- Emission parameters carry no dust terms.
- The face-on dust column's mean is within 22% of the smooth dust's. Clipping the lanes at zero raises it.
- The cloud column over the smooth one varies by 34% rms or more across the disc.
- Still: still_galaxy_dust (smooth above, cloud below).
- Lower amplitudes (0.9) gave 19% rms and were hard to see.
Reroll impact: None (render only).
Status: Provisional (UE-19; the owner may want stronger or weaker lanes: the profile values)

D-181 — Galaxy march cost after D-178..D-180: one evaluation for emission and dust
Question: Owner, in the engine after D-179: GPU at 63 ms inside a galaxy, the march much dearer. Reproduced on the CPU reference (T2.galaxy.march_cost, Milky Way, solar camera along the disc, 242 × 159):
- D-177: 2.4 s, 170 steps per ray;
- with the D-178 modes: 3.5–4.0 s, 188 steps;
- with the D-179 textures and D-180 cloud dust: 11.7 s, 212 steps.
So the steps grew 25% and the per-sample cost grew ~4×. A dusty galaxy was evaluated twice per step, emission then dust, each with its own warp and detail reads, angle, hashes and mode terms.
Decision:
- **Shared state.** The galaxy field is split into `Uni_GalaxyShared` and `Uni_GalaxyFromShared`. The shared part holds the texture warps and detail texel, the cylindrical radius, the angle, the per-arm hashes, and the mode warp, lopsidedness and flare. All of it depends only on parameters that emission and dust share (DustFrom).
- **Pair.** `UniField_GalaxyPair` evaluates both from one shared state. The march's dusty galaxy carriers use it. It is bit for bit the two separate evaluations.
- **Coarse mips.** The texture warps fade out between mips 4 and 5 of their footprint (the curl field's mean there is ~0), so distant galaxies skip those reads. Placement (footprint 0) always reads.
- **Step hint.** The disc slab is centred on the mode-warped plane at the sample, with a margin for how far that plane moves within one step, instead of being widened on both sides. The texture-warp margin is 2.5 RMS (was 3).
- **Arms.** At most 4 arms; every archetype rolls 0–4.
Rationale:
- T2.galaxy.dust: pair = separate, 0 of 12,000 samples differ.
- T2.galaxy.shape: modes-off field still the D-172 field bit for bit.
- T2.galaxy.march_cost (reference, quiet machine), textured Milky Way along the disc: 11.7 → 8.9 s. From 60 kpc: 1.0 → 0.53 s, 76 → 65 steps per ray.
- T3.vol.march worst 0.67% (bar 1%). T2.vol.equivalence galaxies +7.45% with dust (bar 8%), +2.02% without. T2.vol.continuity 9.2%.
- The CPU reference emulates the trilinear texture reads, so it overstates their share. On the GPU the halved reads should matter more.
Not done: temporal slicing of the march (a quarter of the volume pixels per frame, reprojected), the larger lever, pending the owner's engine numbers.
Reroll impact: None for placement (footprint 0 is unchanged). Rendering: distant textured galaxies lose their sub-pixel warp (which averaged out anyway).
Status: Provisional (UE-19)

D-182 — Galaxy colour palettes (SPRITE_PASS_PLAN §3 A5; owner: stars too, per component)
Question: Owner: each galaxy its own colour or palette, plausible but stylised, with more vibrant colour and more variation than real life. Asked: should a galaxy's stars take its palette too, or only its glow and sprite (glow-only shifts back to physical star colours as stars resolve)? Owner: **stars too**, **per component**.
Decision:
- **Palette.** Four colours per galaxy: bulge, disc, arms, knots (`Uni_GalaxyPalette`, UniFields.ush, one source for CPU and GPU). They are rolled from the palette seed, which is the galaxy's object seed on the new channel PALETTE, within families:
  - bulges gold to amber (hue 28–52°);
  - discs teal-azure (50%), warm-neutral (30%) or violet (20%);
  - arms blue to violet (80%) or teal (20%);
  - knots pink-magenta (70%) or HII red-orange (30%).
  - Saturations are scaled by a per-galaxy vibrance (0.9–1.3).
  - Each colour is mixed with white by 0.9 and normalised to luminance 1, so a tint changes chroma only. Every sprite's and the volume's light is unchanged (D-135).
- **Whole-body tint.** Fixed shares 0.3 bulge / 0.45 disc / 0.2 arms / 0.05 knots. The galaxy's record colour carries it, so its sprite and the volume agree at the hand-off (D-129).
- **Where it applies** (all luminance-kept):
  - The galaxy field reports its components (bulge, disc, arms, knots; `Uni_GalaxyFromSharedC`, `UniField_GalaxyFilteredC`, `UniField_GalaxyPairC`).
  - A star placed by a galaxy with a palette (contributor F.w = palette seed) takes the tint of the components at its position (`Uni_TaskMakeObject`).
  - A galaxy record (slice flag `UNI_SLICE_FLAG_PALETTE`) takes the whole-body tint. So do the authored Milky Way anchor and the authored bright cluster members.
  - The volume tints each galaxy carrier's light by the components at the sample. It blends toward the whole-body tint as the footprint passes the disc's scale length.
  - The palette's colours are computed once per carrier on the CPU, into a parameter block (`FCarrierGPU::PaletteIndex`, flag `UNI_CARRIER_PALETTE`), not per sample.
- **Scope and switch.** `UniScenes::UseGalaxyPalettes` turns it on for galaxies, cluster members and dwarfs. `uu.GalaxyPalettes` (1) applies it at uu.Enable. Offline scenes use it only when asked.
Rationale: T2.galaxy.palette:
- 2,000 palettes: every colour at luminance 1 and in its family; consecutive galaxies' whole-body tints 0.33 apart (RGB at luminance 1).
- Milky Way from the solar camera: all 47,051 stars tinted, none differing in position, seed, magnitude or radius.
- The volume inside it: luminance 0.997× of the untinted, chroma shift 0.37.
- Still: still_galaxy_palettes.
- T2.stream.parity, volume_parity and T3.vol.march unchanged.
Reroll impact: Colours only: star and galaxy record colours with palettes on (the engine by default). Positions, magnitudes and counts are unchanged. uu.GalaxyPalettes 0 gives physical colours.
Status: Provisional (UE-20; tuning the families and strength is art direction, in Uni_GalaxyPalette)

D-183 — Inclined galaxy sprites (SPRITE_PASS_PLAN §3 B; render-only)
Question: A distant galaxy is drawn as a round Gaussian until the volume takes it over (D-129), whatever its inclination. Plan step B: draw it as a disc at its inclination, energy unchanged, matching the volume at the hand-off. Owner question (B: "worth it at all distances, or only above N px?") answered by construction: orientation shows only once the sprite is a few pixels across.
Decision:
- **Disc normal.** A galaxy contributor's orientation is a pure function of its object seed (UniGenRef `RandomRotation`, UNI_CH_ORIENT). `Uni_DiscNormal` (UniProject.ush) computes its third row (the field's z axis in world) in float, on the GPU, from the record alone. Fixed-orientation anchors (the Milky Way) keep identity: their seeds travel in `FUniView::FixedDiscSeeds` (up to four) and take world z.
- **Which objects.** Archetypes with a galaxy-field emitter (`FUniView::DiscArchetypeMask`, built at uu.Enable from the definition): galaxies, cluster members, dwarfs. Records are unchanged (no new object flag): render-only.
- **Shape.** The sprite's core is an elliptical Gaussian whose major sigma is the round sprite's and whose axis ratio is that of an oblate Gaussian of thickness q0 seen at inclination i, sqrt(cos² i + q0² sin² i), minor axis along the projected normal. q0 = 0.2 for discs and 0.65 for early types: the rolled fields' light-weighted thickness, measured at 0.11–0.31 for the disc galaxies of the web scene. The angle and ratio are computed in the project pass in the sprite quad's own basis (Side, Up), into the two free sprite slots (`FUniSprite::EllipseAngle`, `EllipseAxis`; the struct stays 64 B).
- **Energy.** The pixel shader evaluates the round profile at the squashed radius and divides by the effective ratio, so the energy is unchanged; the D-156 halo (glare) stays round. The effective ratio is never thinner than 0.7 px along the minor axis (`Uni_SpriteEllipseQ`), so sprites of a pixel or two stay round and the Gaussian is never undersampled.
- **Switch.** `uu.SpriteEllipses` (1), live. The reference renderer's camera has the mask off by default (stills and render tests unchanged bit for bit).
Rationale: T2.galaxy.sprites, 160 galaxies of the web scene, each looked at head on from 300 radii in a random direction:
- normal against the contributor's: worst 5.7e-7;
- sprite axis ratio and angle against the projected oblate disc: 3e-7, 8e-7 rad;
- splatted at 16 px: second-moment axis ratio within 0.007, angle within 0.011 rad, energy 3e-8; at 0.8 px the ratio stays above 0.87;
- hand-off: against the field's own projected light (light-weighted moments of the rolled field along the same view), axis ratio mean error 0.042, worst 0.125 (the thickest and thinnest discs edge-on);
- mask off: every other sprite field bit-identical, sprites round; 14,063 other objects never elliptical.
- Still: still_galaxy_sprites (32 galaxies at 16 px).
Known limits: per-galaxy thickness is not carried (worst edge-on mismatch at the hand-off ~0.1 in axis ratio); a per-galaxy q0 from the rolled parameters would need the emitter ranges on the GPU. Bulge-dominated galaxies read slightly too thin edge-on.
Reroll impact: None (render-only; no record, seed or placement change).
Status: Provisional (UE-21)

D-184 — Temporal slicing of the volume march (owner's option; SPRITE_PASS_PLAN step C measured)
Question: Owner, in the engine after D-179: the GPU march is the frame's cost inside a galaxy (63 ms) and offered temporal slicing like the engine's cloud renderer, upsampling, adaptive steps or less noise. D-181 cut the per-sample cost; temporal slicing is the larger lever. Plan step C (impostors) was to be built only if the cluster-core march was over budget with the galaxy textures in.
Measured first (reference, 242 × 159, anchored rich cluster): from inside the core textured galaxies cost 154 ms against 127 ms untextured (+21%; the whole frame +11%), from 2 Mpc no change. Far galaxies already skip the warps (D-181), so impostors (C) are not needed; not built.
Decision (render-only):
- **Slicing.** Each frame marches one pixel of every 2 × 2 block of the march grid; the phase cycles through the four in a Bayer order (`Uni_TemporalOffset`), so every pixel is fresh every fourth frame. The march dispatches a quarter of the threads.
- **Reconstruction** (new compute pass `Uni.Volume.Reconstruct`, `UNI_VOL_PASS 3`; rules in the shared core, `Uni_Temporal*` in UniVolume.ush): a pixel not marched this frame takes the previous frame's output reprojected through the camera's rotation and its move (`FUniFrameData::CameraStepCm`, from the subsystem's exact camera delta), at the depth its light comes from (new `FUniVolumeResult::LnDepth`: the march's luminance-weighted ln t, from the fresh neighbours). The history is resampled by Catmull-Rom where all 16 texels are on the grid and on this pixel's scene surface (bilinear otherwise: bilinear alone blurs a moving image a little more every frame it is reprojected), accepted while its texels lie on the same scene surface (promoted bodies) and its luminance is within a factor of 4 of the fresh neighbours'. Otherwise the fresh neighbours, bilinear and depth-aware like the composite.
- **History.** The frame's reconstructed volume, slices and depth are kept (extracted render targets) with the view. It is dropped on a jump (teleport, goto, uu.Enable), a new march size, or a non-consecutive frame; that frame marches every pixel.
- `uu.VolumeTemporal` (1), live; the debug views always march every pixel.
- The CPU reference runs the same (`FTemporalVolume`, UniVolumeRef.h); the per-pixel march is factored out of RenderVolume (`MarchPixel`, unchanged results).
Rationale: T2.vol.temporal, 16-frame paths against a full march of every frame (the first four not compared):
- inside the Milky Way at rest: identical (p95 0.0%);
- turning 1° a frame (~1.8 px): light 0.03%, per-pixel p95 0.5%;
- flying 3 pc a frame and turning: light 0.09%, p95 0.6%;
- from 60 kpc drifting 0.4 kpc a frame (~24 Mpc/s at 60 fps), 320 × 200: light 0.23%, p95 1.3%; drifting and turning 0.3° a frame: 0.39%, p95 2.8%, worst 8 × 8 block 2.0%;
- 25% of the pixels marched; on the reference 3.5–4× less time for the volume.
- Bilinear history instead of Catmull-Rom: p95 ~10% on the far galaxy turning; at 160 × 100 (arms a pixel or two wide) the reprojection's resampling dominates (7% mean error, 1% at 320 × 200).
- T3.vol.march, conservation, handoff and cube unchanged.
Known: up to three frames of latency in the glow of a pixel (light that changes while the camera is still, such as the travel-sensitivity ramp, refreshes over four frames, a faint 2 × 2 pattern while it changes); scene-surface edges (promoted bodies) are handled by the same-surface test but not checked offline (no surfaces in the volume checks).
Reroll impact: None (render only).
Status: Provisional (UE-22: ProfileGPU Uni.Volume.March and Uni.Volume.Reconstruct, look while flying and turning)

D-185 — Diffraction spikes and halo colour on bright stars (SPRITE_PASS_PLAN §3 D; owner: better star sprites, spikes yes)
Question: Owner: better star sprite work, with diffraction spikes. The D-156/D-158 sprite is a pinpoint core plus a capped round halo above saturation.
Decision (render-only, sprite style):
- **Spikes** (`Uni_SpriteSpikes`, UniProject.ush, shared by the sprite pixel shader and the reference splat): on every sprite with a halo (point sources above saturation, D-158), so they appear and grow with it and never shrink on the way in. Lines through the centre at Angle + k π / lines in the quad's (Side, Up) basis (about screen-aligned). Along each, (1 − |u| / length)³; across, a Gaussian of 0.7 px. Their light is Gain × the halo's, exact over the pixels (the profile ends at its length, which the quad covers). Length = 10 halo sigmas, at most 48 px.
- **Halo chroma** (`Uni_SpriteChroma`): the halo and spikes take the star's colour with its chroma × 1.4, luminance kept. The core stays the star's colour (it reads white once it saturates). This is the art direction's "more vibrant" for bright stars.
- **Defaults and switches:** 4 spikes (a cross), gain 0.5. `uu.Sprite.Spikes` (0 off; 6 or 8 more arms), `.SpikeGain`, `.SpikeLength`, `.SpikeMax`, `.SpikeAngle` (degrees), `.SpikeWidth`, `.HaloChroma` (1: the star's colour); live, render thread.
- The glare (halo) is kept through promotion as before (D-158): a promoted star keeps its halo and spikes around the actor. Handing it to the engine's bloom stays possible later (physical mode, uu.Sprite.Saturation 0, draws neither).
- The reference camera's style is off by default (stills and render checks unchanged bit for bit).
Rationale: T1.sprite.spikes:
- 400 random styles (2–8 spikes, lengths 2–48 px, widths 0.5–1.5 px, any angle and sub-pixel centre): spike light within 1.28% of gain × halo light;
- a line, not a disc (on-spike against off-spike far above 50);
- length and light never shrink as the halo grows;
- chroma keeps luminance (2e-7), makes colours more vivid, identity at 1;
- the splat deposits radiance × (core + halo × (1 + gain)) within 1e-7;
- style off draws every sprite as before, bit for bit.
- T1.sprite.style, arrival and profile, T2.parallax.promotion and T3.frames unchanged. Still: still_star_spikes (4, 6, and 4 at 45°; warm, white and blue stars).
Reroll impact: None (render only).
Status: Provisional (UE-23; tuning is art direction)

D-186 — Sky light from the backdrop (design spec item; SPRITE_PASS_PLAN §3 H)
Question: The design calls for promoted bodies lit by, and reflecting, the galaxy around them. Our backdrop draws after the scene, so the engine's sky capture never sees it.
Decision:
- **Cube** (`UniCore::BuildSkyCube`, UniSky.h): 16 texels a face, each the mean of 2 × 2 rays marched through a copy of the frame's volume scene (local and background carriers, to the horizon, the texel's footprint as the prefilter). Direct3D / Unreal face order and orientation (`SkyTexelDir`). Radiance × one screen pixel's solid angle, so a texel holds what a screen pixel of that sky shows.
- Sprites are not in it (a few percent of a galaxy's light). The nearest star lights through the light rig (D-147).
- **Engine** (`UUniSubsystem::StartSkyJob` / `UpdateSkyLight`):
  - a worker job every `uu.SkyLightInterval` (1 s), started only when the last one is done;
  - the result is uploaded as a transient float cubemap (`UTextureCube::CreateTransient`);
  - it is blended in over `uu.SkyLightBlend` (1 s) on a movable sky light with a specified cubemap (SetCubemapBlend, then SetCubemap), lower hemisphere not black;
  - `uu.SkyLight` (1), `uu.SkyLightSize` (16), `uu.SkyLightIntensity` (1);
  - disabling waits for the worker (it reads the CPU textures).
Rationale: T2.sky.cube:
- Direct3D's face selection of 20,000 directions comes back through SkyTexelDir (3.5e-16);
- the texels tile the sphere (4π to 1e-10);
- inside the Milky Way the brightest 5% of texels lie along the disc (mean |z| 0.097) and the brightest is towards the centre (cos 0.981);
- 16 and 48 texels a face hold the same light (1.026; one ray per texel was 1.08);
- the 16-texel cube takes 0.4 s on one reference thread, untextured. Textured, expect ~1–2 s on a worker; it refreshes at most once a second.
Known: the sky light is only as bright as the glow on screen, so next to the 10-lux light rig (uncalibrated, D-147) it may need uu.SkyLightIntensity. A level's own sky light adds to it. If the engine samples the cube rotated or mirrored, the face table is the place to fix (the check pins Direct3D's convention).
Reroll impact: None.
Status: Provisional (UE-24)

D-185 (amended, owner 2026-11-04) — Spikes withdrawn. Owner, after seeing them: star glare should come from the scene's post-processing (bloom and lens flares) instead of drawn spikes. The spike profile, its style and its console variables are removed (uu.Sprite.Spikes … SpikeWidth gone); the halo chroma stays (uu.Sprite.HaloChroma). The sprites are drawn after motion blur and before bloom and tonemapping (D-006, D-124), so they already feed the engine's bloom and lens flares. A bloom kernel with spikes (Convolution Bloom) gives engine-side spikes if wanted. uu.Sprite.Saturation 0 (physical mode, D-156) hands all of a star's light to them (no drawn halo).

D-188 — Sprite quads reach until their light falls below a floor (owner: bright sprites "bleed to the edge and clip")
Question: Owner's screenshot: a bright star near promotion, its halo cut into a square. The sprite quad was a fixed 3 sigmas of its footprint, where a Gaussian is still 1.1% of its peak. For a halo hundreds of times over the display's black, that is a visible edge.
Decision (render-only):
- **Reach** (`Uni_SpriteReachPx`, UniProject.ush): the quad reaches until the core and the halo each fall below half of `uu.Sprite.Floor` (2e-4 in pre-exposed scene colour, below one 8-bit step after tonemapping), so together they stay under it. It is never under SpriteExtent (3) sigmas and at most 8 sigmas (`UNI_SPRITE_REACH_MAX`); a resolved disc reaches past its soft edge. It is computed in the sprite vertex shader from the sprite's luminance.
- **Soft edge** (`Uni_SpriteEdgeWindow`): the profile fades to zero over the quad's last tenth, so a profile cut at the 8-sigma cap still shows no square.
- The reference splat already sized its kernel by brightness (unchanged).
Rationale: T1.sprite.reach, 2,000 sprites with cores 0.5–6.5 px, halos up to 5×, luminance 1e-3 to 1e8:
- 0 above the floor at their reach (worst exactly the floor), 0 under 3 sigmas;
- the light beyond the soft edge and outside the quad is at most 1.6% (the fixed 3-sigma quad lost 1.1%; most sprites now reach much farther);
- halo chroma: luminance kept to 2.4e-7.
- T1.sprite.style and profile, T3.frames and T2.galaxy.sprites unchanged.
Cost: the brightest stars' quads grow, about 2.3× the radius for a very bright one, so ~5× the area for a handful of sprites; faint ones are unchanged at 3 sigmas.
Reroll impact: None (render only).
Status: Provisional (UE-23)

D-187 — Warp streaks and arrival as motion (WARP_PLAN E1, E2; render-only)
Question: Owner: speed should read as speed while staying navigable, and the effects should hide pop-in. WARP_PLAN proposed streaks (E1) and arrival as motion (E2) first, both in the sprite pass. Its five owner questions mostly concern E3/E4; E1/E2 are built with the plan's recommended defaults (the travel curve; light-conserving streaks).
Decision:
- **Warp amount.** The travel-sensitivity curve (D-154) normalised to 0..1: 0 below 100 kpc/s, 1 at 10 Mpc/s, smoothstep in log speed, rising 2/s and falling 1/s. Jumps don't count. `uu.Warp` (1) scales it, 0 is physical. `FUniFrameData::WarpAmount`.
- **Streaks (E1)** (`Uni_SpriteStreak`, `Uni_SpriteProfileStreak`, UniProject.ush):
  - A point source's core and halo are smeared evenly along its motion over the last frame. The object is static, so its previous direction is normalize(Dir + CamStep / distance), the camera's move only. Turning does not streak, so steering stays readable.
  - Flying forward, tails point back to the vanishing point.
  - Length: the motion × `uu.Warp.StreakFrames` (1) × the warp amount, at most `uu.Warp.StreakMax` (64 px).
  - The profile is a Gaussian convolved with the segment (erf, Abramowitz–Stegun 7.1.26, the same on CPU and GPU). It sums to the sprite's light at any length, so the streak keeps the star's light.
  - The quad becomes the box from the sprite to its tail's end; D-188's reach and soft edge apply around the segment.
  - Bodies (resolved discs) and inclined galaxy ellipses don't streak.
- **Arrival (E2):**
  - The streamer records the frame a cell first became Ready (a regeneration keeps it). The cell table carries its age in Flags bits 16..23; planets' dynamic cells are always settled.
  - At warp, the project pass fades a new cell's objects in over `uu.Warp.Arrival` (8) frames: level fade × lerp(1, arrival, warp). It stores the arrival in the sprite's Flags bits 24..31.
  - The sprite pass stretches an arriving sprite's tail by 1 + `uu.Warp.ArrivalStretch` (6) × (1 − arrival). An object that arrives late streams in from ahead and settles, instead of popping.
  - At rest nothing changes: arrival is off, and the age bits are ignored.
Rationale: T1.warp.streaks:
- erf within 4.1e-7;
- over 300 random streaks (sigma 0.6–4 px, 0–64 px long, any angle and sub-pixel centre): light within 0.03%, centroid at half the length along the tail (0.01 of the bar), on its axis, continuous at the short-tail switch (1.4e-4);
- 474 forward tails all point to the view centre; sideways moves streak along the move; none at rest or without warp; none over the cap; arriving tails exactly 7× as long;
- arrival 0 → 1 over its frames;
- 59,194 cell-frames: every cell's age is 0 on its first Ready frame and one more each frame after.
- Still: still_warp_streaks (warp 0, 0.5, 1). Fixed while building it: |Dir × distance|² overflows a float past ~1e19 cm, so the direction is divided through first.
Known: the streak follows the last frame's move, so a sudden stop shortens it at once (the warp amount itself falls over a second). Galaxy sprites large enough to be ellipses don't streak.
Reroll impact: None (render only; the cell age is bookkeeping).
Status: Provisional (UE-25; the remaining WARP_PLAN questions decide E3/E4)

D-189 — The web in the sky: a near-web glow and a dimmed tail, as a switch (SPRITE_PASS_PLAN G, B-010; render-only)
Question: Physically the unresolved galaxies' light is isotropic to a few percent past ~100 Mpc (D-170), so the web barely shows in the sky. B-010 offered (a) physical, (b) a near-web glow, (c) a dimmed tail. Owner: "not sure", so an engine A/B decides.
Decision:
- `uu.SkyWeb`: 0 physical (the default), 1 near-web glow, 2 dimmed tail, 3 both. Live; the background map is re-marched when the settings change.
- **Glow (1):** on the periodic web carrier only, within `uu.SkyWeb.Depth` (50 Mpc, fading over its second half), the sampled density is lifted by `uu.SkyWeb.Glow` (2) × its overdensity (rho / mean − 1, never below 0). Where the web is denser it is brighter; voids are unchanged.
- **Dimmed tail (2):** the analytic tail past the march (the uniform pedestal) is kept at `uu.SkyWeb.Tail` (0.1) of its light, so the marched near structure carries the contrast.
- FVolumeViewGPU grew from 48 to 64 bytes: TailDim, WebGlowGain, WebGlowDepthCm and a pad. FVolumeSettings carries the three values.
Rationale: T3.webtex.skyweb, over the delivered web from the void camera:
- the dimmed tail is exactly the marched light + 0.1 × the tail;
- sky contrast (the radiance's spread over its mean, per solid angle) is 0.010 physical, 0.236 with the glow and 0.084 with the dimmed tail;
- stills: still_skyweb_physical, _glow and _dimtail.
Known: both modes are non-physical (the light budget no longer holds for the background) by design; that is why physical is the default. The glow is per pixel, so it costs nothing extra; the tail dim costs nothing.
Reroll impact: None (render only).
Status: Provisional (UE-26: the owner's A/B)

D-190 — Warp optics: stylised aberration and Doppler colour (WARP_PLAN E3; render-only)
Question: WARP_PLAN E3 and owner questions 2 and 3 (aberration cap; Doppler colour only or beaming too). Unanswered, so the plan's recommended defaults are built behind switches.
Decision:
- **Speed.** Effective speed beta = `uu.Warp.Aberration` (0.5, at most 0.9) × the warp amount (D-187). The motion is the camera's move over the last frame. Nothing applies across a jump or at rest.
- **Aberration** (`Uni_Aberrate`, UniPhotometry.ush): the relativistic angle remap; at −beta it is its own inverse. The direction of motion is a fixed point, so what is ahead stays ahead and steering keeps working. At 0.5 the forward cone is sqrt(3) ≈ 1.73× tighter. It is applied consistently:
  - sprites: the project pass places every object at its aberrated direction;
  - volume and background: each pixel's ray is un-aberrated to its true direction (the march, sub-rays and reconstruct pass, the background cube lookup);
  - temporal reprojection (D-184): through the previous frame's optics;
  - streaks (D-187): un-aberrate, step by the camera's move, re-aberrate.
- **Doppler colour** (`Uni_DopplerFactor` = gamma (1 + beta cos theta), `Uni_DopplerTint`): blue ahead, red behind, as a colour-temperature scale of `uu.Warp.Doppler` (1), luminance kept, on sprites, the volume and the background.
- **Beaming:** brightness × D^`uu.Warp.Beaming`, default 0 (colour only).
Rationale: T1.warp.optics:
- the remap matches the formula (2.8e-7) and its inverse (9.3e-6); nothing moves away from the motion; the motion's direction is fixed; the cone is ×1.7321 at 0.5;
- Doppler 1.7321 ahead and 0.5774 behind; the tint keeps luminance (6e-8), blue ahead and red behind;
- streaks are unchanged without the optics and point to the vanishing point with them; the reprojection puts every pixel back on itself at rest (3.8e-4 px);
- still: still_warp_optics (beta 0, 0.25, 0.5).
Known: the aberration crowds the sky ahead at speed, so targets near the edge of the view move inward while warping. That is the intended look; `uu.Warp.Aberration 0` removes it.
Reroll impact: None (render only).
Status: Provisional (UE-27)

D-191 — Brightest cluster galaxies and intracluster light (SPRITE_PASS_PLAN F, B-006 rest; content + render)
Question: B-006's remaining realism. Rich clusters have a giant elliptical at their centre and a diffuse intracluster glow (about 10–20% of their light). The light budget has to hold.
Decision:
- **Which clusters.** Rich catalogue clusters: node-list entries brighter than `BcgMaxMag` (−25); the delivered web's 113 node clusters run from M −26.5 to −24.5. Procedural clusters have no authored members (D-177), so they have neither.
- **BCG share** (`FCatalogue::BcgShareAt`): 6% of the cluster's light at M −26.5, with its light going as the cluster's^0.5 (observed: a poorer cluster's BCG takes a larger share), at most 30%. At M −25 it is 12%.
  - With a flat 6% the BCG of a cluster near the cut came out fainter than a drawn member (−22.00 against −22.39). With the scaling both rich clusters' BCGs are the brightest, with gaps of at least 0.33 mag.
- **BCG** (UniGenRef CopyMembers): authored with the copy's bright members (D-177):
  - at the cluster's centre, with rank 63 (index kFirstMemberIndex + 64 × entry + 63), so drawn members stop at 63;
  - seed: the copy's member seed mixed with 0xBC6;
  - an early type with the early types' colour shift (D-172);
  - radius: the radius model's × `BcgRadiusScale` (1.5), an extended envelope;
  - at its own magnitude's member level, which the member index now includes. Its magnitude is tested as the record stores it, a half float, on both sides.
- **Members.** A rich cluster's contributor count is × (1 − BCG share − `IclShare`), so both its bright and its emitter-placed members shrink. Members + ICL + BCG = the cluster's light exactly.
- **ICL** (UniVolume.h, UniVolume.ush): on the cluster's own carrier, not a second one, so it costs no carrier slot or field sample.
  - A flag, `UNI_CARRIER_ICL`; the carrier's unused PaletteIndex holds a total-light LUT block (every child's light, none resolved as sprites, built once per scene and child); its pad holds IclRatio = IclShare / keep.
  - The march adds LUT_total × IclRatio on the same density.
  - Such a carrier marches from the local start (1 pc), not the members' significance start, because the glow is there at any distance.
- Tuning: `FClusterTuning::BcgShare` (0.06), `IclShare` (0.12) and `BcgMaxMag` (−25).
Rationale: T2.webtex.bcg, from the heaviest cluster's copy, over the 3 clusters within 30 Mpc (2 rich):
- counts × keep for rich clusters only; light off by 0 at worst;
- exactly one BCG per rich cluster and none elsewhere, at the centre (offset 0), at its share's magnitude (0.0001 off), early, the brightest (smallest gap 0.33 mag);
- the ICL carrier's ratio and start are right; its block is never below the members' unresolved light, and equals it with no sprites drawn.
- T2.webtex.members (the BCG among the bright members: 57 for 52.5 expected, 0.6 sigma), nodes, groups, catalogue_lookup and travel, and the cluster group all pass unchanged.
Known:
- The ICL follows the members' distribution (the cluster's beta model and clumps); real ICL is a little more concentrated, around the BCG.
- Only catalogue clusters have BCGs (as with D-177's bright members).
- The test's sample is small (2 rich clusters near the camera).
Reroll impact: the members of rich catalogue clusters: bright and emitter-placed counts × 0.82 (more at the cut), plus one BCG each. Field galaxies, groups, poorer clusters, the web and stars elsewhere are unchanged.
Status: Provisional (UE-28)

D-192 — Warp ambience motes (WARP_PLAN E4; render-only decoration)
Question: WARP_PLAN E4 and owner question 5. In a void the sky barely moves even at 30 Mpc/s, so nothing says "fast". Unanswered; built as a switch (on), without the peripheral glow.
Decision:
- **Motes** (`Uni_MoteSprite`, UniProject.ush; `UniMotes.h`):
  - points fixed in the world, 16 hashed into every cell of a cubic lattice;
  - two lattice levels at once, with cell 2^n cm. Their scale follows the speed: log2 of the cell tracks |camera step| × `uu.Warp.MoteFrames` (30), smoothed over 0.3 s, held while still and reset on a jump. Level n weighs 1 − |x − n|, and level n always lives in slot n & 1, so slots change hands only at zero weight;
  - per level, the 4 × 4 × 4 cells around the camera's cell (shifted by its half cell). A mote fades out from 0.75 to 1.5 cells away, so the block's edge never shows, and fades in near the camera (from 0.35 to 0.1 cells). That makes about 20 in a 90 degree view.
- **Brightness:** `uu.Warp.MoteMag` (1.5 magnitudes below the exposure magnitude), spread over two magnitudes by a hash, × the warp amount × `uu.Warp.Motes` (1). At rest there are none.
- **Drawn** as a second draw of the sprite shaders (`MoteMode` 1): UNI_MOTE_COUNT = 2,048 procedural instances, no buffer. The streaks (E1), aberration and Doppler (E3), depth test and dust transmittance are the stars'.
- Seed channel MOTE (0xF6008302) is new.
- **Not built:** WARP_PLAN's peripheral glow (vignette) for pop-in at the view's edges. E2's arrival already covers late cells (BACKLOG B-014).
Rationale: T1.warp.motes, over a scripted flight (1 Mpc/s, then 3 s accelerating to 30 Mpc/s, 300 frames, full warp):
- 64,197 mote-frames found again the next frame in the same place (1e-5 of a cell); 3,431 faded out;
- nothing appears or vanishes in view above 1.9% of the brightest mote's light;
- the scale is 0.09 octaves off once settled; 20.4 motes in view on average; median streak 13 px at 30 Mpc/s; none behind the camera, none at rest;
- still: still_warp_motes.
- While building, the first fades were too steep (1–1.5 cells out, 0.03–0.15 near): a mote passing the camera vanished from 7.5% in two frames.
Known: motes are pure decoration. They are not in any light budget and not in the sky light, and they read as dust rather than stars.
Reroll impact: None (render only; a new seed channel).
Status: Provisional (UE-29)

D-193 — Runtime API, first block: Pick, QueryResident, FindNearest and events (design doc, Spec: runtime API; M4)
Question: Owner: start the gameplay-facing API (after the sprite pass). The design lists Resolve, Query, Pick, Events, Sensitivity shapes, Persistence and Readouts. This block is the part that works on what is resident; Resolve (generate anywhere) needs its own plan (BACKLOG B-015).
Decision:
- **Naming objects from the GPU.** A readback names an object by (cell index in a frame's cell table, slot in the cell's pool range), as the activation readback (D-142) already did.
  - The game thread keeps the last 32 frames' tables (`FFrameCells`: keys, cell generations, and the planets' dynamic cells after them).
  - `FUniStreamer::IdentityAt` turns a slot into the identity: generated objects fill a cell's range first in identity order (each task's IndexBase is the running sum of the capacities before it, so slot = index), anchors follow.
  - A cell regenerated since the readback's frame (its new `Generation`) or evicted names nothing: the result is reported not found, never a wrong object.
  - A planet's dynamic cell names its star and its ordinal: an activated system now keeps its star's identity (`FActiveSystem::StarId`, from the activation readback).
- **Pick** (`UUniSubsystem::Pick`; `Uni_PickScore`, Core/UniQuery.ush): the project pass writes each sprite's (cell, slot) beside it on a frame with a pick. Three small passes (UniQuery.usf) then find the best score and the lowest sprite index at that score (as the max of ~index, so 0-cleared buffers suffice), and read back the source and record.
  - The rule: a sprite is under the cursor when its centre is within its reach (`RadiusPx`, default 4 px; twice its core sigma; its true radius, whichever is largest). The winner has the highest luminance / (1 + (distance / RadiusPx)²): the brightest, but a much fainter star exactly under the cursor beats a slightly brighter one a few pixels away.
- **QueryResident / FindNearest** (`Uni_QueryRelative`, `Uni_QueryCellMayHit`, `Uni_QueryMatch`): one group of 64 per cell over the cell table and pool, like the project pass.
  - Distances are exact: the centre goes in as its 128-bit delta from the camera, and an object's offset from it is formed in integers before the float conversion (D-104's rule). Whole cells outside reach are skipped.
  - Filters: archetype ids (0..31), maximum absolute magnitude, at most 4,096 results (nearest first).
  - FindNearest is two passes (the smallest distance, then the matches at it).
- **Events**, batched once per frame on the game thread:
  - `OnObjectResident` / `OnObjectEvicted` for the objects the CPU knows (every emitter object read back, i.e. galaxies, clusters, groups; and anchors), by archetype (`SetResidentEventArchetypes`, off by default). They come from the streamer's cells that became Ready or left each frame (`ReadiedLastUpdate` / `LeftLastUpdate`), diffed per cell (`FResidentEvents`).
  - `OnObjectVisible` / `OnObjectHidden` for watched objects (`WatchObject`), the CPU running the shared `Uni_ObjectSeen`: in view (with its size as margin) and bright enough by the project pass's photometry, level faded in. It stays true while a galaxy's light is handed to its volume and while a promoted body is drawn by its actor: the object, not its sprite.
  - `OnBodyPromoted` / `OnBodyDemoted` from the promotion manager (M3).
- **Requests** queue: at most one pick and one query a frame go to the renderer, two of each in flight (its rings hold three). A request no view answered within 90 frames fails (not found). Disabling or re-enabling the universe fails everything pending, without events.
- **Blueprint**: `FUniObjectInfo` (id, planet ordinal, exact coordinate, archetype id and name, magnitude, radius, distance, seed) and `FUniQueryFilter` (UniApiTypes.h); dynamic delegates for results and events; `GetArchetypeId`, `GetKnownObject` (immediate, CPU-known objects). C++ has `...Native` variants taking TFunction.
- Console: `uu.Pick [x y]`, `uu.Query <radius pc> [archetype]`, `uu.Nearest <archetype>`, `uu.Events [archetype ...|off]`.
Rationale: T2.query.*:
- identity: 64,907 objects in 1,652 Ready cells (the flythrough scene after a track) named through the frame's table are the reference pipeline's objects of those identities, bit for bit; none unnamed; a newer generation names nothing; 4 planets name their star and ordinal, within 3.2 cm of their orbit position;
- pick: over 1,221 visible sprites, a cursor on each of the 957 that no other sprite reaches picks it; no winner out of reach; every winner names its own object; constructed pairs follow the rule;
- resident: four centres (camera, a star 0.4 pc away, a point 3 kpc away) and radii from 1 pc to 30 kpc with archetype and magnitude filters match exact 128-bit distances (47,052 hits in the largest; distances within 2.2e-7; nearest exact);
- events: 42 frames of flight, the wanted archetypes changed twice: 21,605 resident and 3,742 evicted events, and after every frame the reported set equals the Ready cells' wanted objects (no doubles, no ghosts);
- watch: 64 galaxies over a 360 degree turn: 53 events, all consistent, agreeing with an independent double-precision frustum and visibility test.
- T2.stream.* unchanged.
Known:
- Pick and queries see what is resident; Resolve (anywhere) is not built.
- OnObjectVisible works for CPU-known objects only: a watched star is never seen (it would need a GPU watch list).
- Resident events for every galaxy can be thousands per frame at speed: they are off by default.
- The watch ignores the warp optics' aberration (true directions).
- One pick and one query per frame; a burst of requests queues.
Reroll impact: None.
Status: Provisional (UE-30: compile; uu.Pick, uu.Query, uu.Nearest, uu.Events in the engine)

D-194 — Volume ghosting at speed: history clamped into its fresh neighbours, harmonic light depth (render-only; amends D-184)
Question: Owner: the glow's march ghosts at high speed; is that the trade-off for temporal slicing (D-184), or can it be reduced? Measured first (T2.vol.temporal_fast, new): passing the Milky Way at warp speeds, D-184 lost or misplaced up to 12% of a frame's light (turning at 3 kpc a frame from 60 kpc; per-pixel p95 39%), because:
- the history was accepted while its luminance stayed within 4x of the fresh neighbours', which has no floor when one neighbour is dark (stale dark history accepted where the galaxy has arrived, and the reverse);
- the reprojection depth was the light-weighted mean of ln distance: a pixel mixing a near galaxy with the far background reprojected as if all of it were megaparsecs away, so its predicted motion was nearly zero.
Decision:
- **Clamp, not reject** (`Uni_TemporalClamp`, `Uni_TemporalClampK`, `Uni_TemporalMotionPx`, UniVolume.ush). The reprojected history is clamped into its four fresh neighbours' per-channel range (radiance and transmittance), widened by K. K is 4 at rest (fine detail between the fresh pixels survives, as D-184's band allowed) and falls to 1 at `uu.VolumeTemporal.TightPx` (2) march pixels of motion: a moving image can't leave trails or holes outside its neighbours' range.
- **Too fast, no history**: beyond `uu.VolumeTemporal.MaxMotionPx` (8) the fresh neighbours are used (blur instead of trails).
- **Harmonic light depth** (`Uni_LightInvDepth`, `Uni_LightLnDepthFromInv`): the depth the reprojection uses is the light-weighted mean of 1/t, the mean parallax, carried scaled by e^60 so it never underflows. The march, its sub-rays (GPU and reference) and the fresh neighbours' combination all use it.
- `uu.VolumeTemporal.Clamp 0` restores the D-184 band for an A/B.
Rationale: T2.vol.temporal_fast, 320 x 200, 16 frames, against a full march every frame (D-184 band, then this):
- 3 kpc a frame toward and across from 60 kpc: light 4.10% -> 0.62%, per-pixel p95 10.0% -> 4.7%;
- 3 kpc a frame turning 0.5 degree a frame: light 12.28% -> 2.15%, p95 38.8% -> 8.1%;
- 12 kpc a frame sideways at 150 kpc (~13 px a frame): light 3.65% -> 2.60%, p95 46.9% -> 60.7%: the history is dropped there and the fresh neighbours' quarter-resolution image is blurrier (excess light 0.43% -> 4.43% is that blur). `uu.VolumeTemporal.MaxMotionPx 16` keeps the history there instead (p95 35.7%, light 4.35%) at the cost of the close passes (light 1.50% and 6.01%).
- Sweep of the fallback threshold, the close passes' light error: 16 px 1.5% / 6.0%; 12 px 1.0% / 4.8%; 8 px 0.6% / 2.2%; 4 px (measured before the harmonic depth) 0.2% / 0.7%, but half the pixels from the fresh neighbours. The tightness (1 to 4 px) changed little. 8 keeps most of the history while removing most of the error.
- T2.vol.temporal (D-184's paths: at rest, turning, flying inside the galaxy, drifting at 60 kpc) unchanged to the printed precision.
Known: temporal slicing still can't follow structure crossing many pixels a frame (the last case): there the image is a quarter-resolution blur for a few frames. A full-rate (or half-resolution, every pixel) march at warp would remove it at up to 4x the volume's cost: a later optimisation pass, with the owner's engine numbers.
Reroll impact: None (render only).
Status: Provisional (UE-31)

D-195 — Volume march from inside a galaxy: step ratio 0.24, looser hints around the camera, local start 10 pc (render-only; amends D-129, D-160)
Question: Owner's capture, inside a galaxy: about 30-40 fps, 15-20 moving through dense parts. The target is 60 fps with the ocean, atmosphere and meshing pipelines also on the GPU. Uni.Volume is 20-31 ms of a 24-36 ms GPU frame (85-90%). Where do the steps go, and what can be cut without visible change?
Findings (T2.vol.perf_inside, new: flythrough scene, solar camera, 240 x 135, three views, against a fine reference of step ratio 0.02 at 20000 steps):
- The march is emission plus dust absorption, with no light march. The CloudAtmosphere plugin's shadow-map trick (n^2 -> 2n for the light march) doesn't apply. Its steps already grow with distance (log steps), like CloudAtmosphere's lattice.
- Inside the Milky Way there is about one active carrier per pixel, and almost no supersampling (0-0.6% of pixels). The cost is the step count: 134-198 steps per pixel at 0.08, and every step evaluates the field's step hint as well as the field sample itself.
- The steps go to three things: the log ratio (0.08: ~29 steps per decade of distance), the galaxy's own step hints toward the centre, and the decades between 1 pc and 10 pc (light there is ~1e-4 of the column).
- Sweep (steps per pixel toward the centre / along the disc / out of the plane; p95 per-pixel error):
  - 0.08: 198.5 / 158.4 / 134.3 (p95 under 0.1%).
  - 0.24: 136.9 / 87.4 / 67.7 (0.1%).
  - 0.24 with hints x 2: 85.5 / 63.2 / 51.8 (0.2-0.3%).
  - With hints x 4: 61.8 / 53.9 / 46.9 (0.4-0.5%).
  - With no hints: 48.5 toward the centre, p95 6.4% (the hints matter there).
  - From 10 pc: 126.2 / 75.7 / 57.0 (0.1%).
  - From 100 pc: p95 2.7-3.0% (near dust and stars lost).
Decision:
- **`FVolumeSettings::StepRatio` 0.08 -> 0.24** (`uu.VolumeStepRatio`). The textured web keeps its own 0.2 (D-164).
- **`HintScale` 2** (`uu.VolumeHintScale`, the former `SkyPad` slot of `FUniVolumeView` / `FVolumeViewGPU`). It multiplies the step hint of every non-periodic carrier the camera is inside (|CamLocal| < 1). From outside a carrier the hints stay as written: a ray crossing a disc steeply loses 5% per ray at x 2 (T3.vol.march, face-on at 60 kpc, unprefiltered), against 0.5% from inside.
- **`LocalStartCm` 1 pc -> 10 pc** (`uu.VolumeStartPc`). Carriers whose LUT starts later (D-160) are unaffected.
Rationale: T2.vol.perf_inside, D-195 defaults against the old ones:
- 198.5 -> 74.8 steps/px toward the centre (2.65x fewer); 158.4 -> 52.2 along the disc (3.0x); 134.3 -> 41.1 out of the plane (3.3x).
- Total light within 0.25%, p95 per-pixel error at most 0.33% (old: 0.05%), no truncation. CPU reference march time 3347 -> 1141 ms.
- T3.vol.march (adaptive vs brute force, per ray, unprefiltered) stays under 1%: worst 0.67% (face-on 60 kpc, unchanged), inside 0.53%.
- Inside a rich cluster's core (T2.vol.cluster_d195, new; camera inside the cluster's carrier, 1 cluster + galaxy carriers): 2.15x fewer steps, total light -0.03%, the cluster's light -0.40%, visible pixels' p95 0.60%, against the D-160 march at the old step quality.
- T2.vol.equivalence (D-160's speed-ups) now pins both of its marches to the old step quality: at 0.24 the two step grids alone differ by 0.17% in the cluster's light, against its 0.1% bound. Its numbers are unchanged.
- The GPU march costs about the same per step, so the volume pass should drop by about 2.5-3x where the owner measured 20-31 ms. Engine numbers: UE-32.
Next (phase 2, B-017): a CloudAtmosphere-style accumulating resolve (a jittered sample within each step, an age-weighted history, a moments clamp, a larger interleave), and possibly clipping each ray to the galaxy's disc slab.
Reroll impact: None (render only).
Status: Provisional (UE-32)

D-196 — Volume cost on large viewports and in motion; sprites hand over only what their carrier takes; warp effects off by default (render-only)
Question: Owner, after D-195:
- still under 60 fps in dense galaxy cores; the worst case is 35-40 fps and needs to be about 50% faster;
- a galaxy's sprite (or the backdrop) can fade before its glow fades in;
- inside a cluster, moving at intergalactic speed (425 kpc/s), nearly every galaxy vanished until the camera stopped.
Findings:
- The owner's capture (RTX 4090 laptop, 1880 x 1120 viewport): Uni.Volume 17.3 ms of a 20.9 ms GPU frame, two dispatches and a draw (march, reconstruct, composite). ProfileGPU shows scopes only, so the march's own time and mode were not visible.
- Moving in vanishes: `uu.Warp 0` fixed it (owner). D-187/D-190/D-192's warp effects dim nearly everything at that speed; the cause is not found yet (B-018).
- Per-pixel background (D-166): any background change (a LUT change over 1%, or 200 pc of travel) put the background carriers into every pixel's march for the next 30 frames. Offline, from inside the Milky Way, that doubles the steps per pixel (70 -> 142 toward the centre; 47 -> 120 along the disc) and triples the carriers each step evaluates (1 -> 3). The uu.perf log counted 10 refreshes in 40 s of flying inside the galaxy, so about a quarter of the frames had it.
- The march ran at output / 2 whatever the viewport: 940 x 560 (0.53 Mpx) on the owner's viewport, about 130k rays a frame sliced.
- Sprite handoff: the sprite handed off by the curve alone (Uni_CarrierHandoff), while the carrier exists only if the builder kept it. Galaxies over the frame's carrier cap (128; a rich cluster's core has more), a carrier whose integral is not ready (D-140), or one whose contributor sleeps (D-168/D-176) lost their light: sprite faded, no glow.
Decision:
- **Background per pixel only when it pays** (`uu.BackgroundMode 2`). The background goes per pixel only while it changes at least every `uu.BackgroundPerPixelInterval` frames; 0 (automatic) is where a six-face map refresh per change costs more than marching it per pixel (6 x 256^2 rays against a quarter of the march). It leaves per pixel after `uu.BackgroundStillFrames` still frames, as before. A change now and then refreshes the map, cross-faded (D-159).
- **March pixel budget** (`uu.VolumeMaxMarchPixels`, 262144). The march is output / `uu.VolumeDivisor`, scaled down (aspect kept) to at most this many pixels (`UniCore::VolumeMarchSize`). On the owner's viewport, 663 x 395 instead of 940 x 560: about half the rays.
- **GPU scopes** for ProfileGPU: Uni.Volume.March (or .March+Background), .Reconstruct, .Composite, .BackgroundMap. `uu.perf` prints the march's size, rays, slicing, and how many frames had the background per pixel.
- **Carrier handoff table.** The builder lists each local carrier's parent and its share (`FCarrierHandoff`: the curve x the cap fade). The subsystem names them by this frame's (cell index, slot) (`CarrierHandoffTable`, `FUniStreamer::SlotOf`), sorted. The project pass binary-searches them for objects with a carrier (`Uni_ObjectIsCarrierParent`) and draws `1 - share` (`Uni_ProjectObjectH`). Without an entry (no carrier this frame, or the volume off), the sprite is whole. The curve stays for the offline sprite-only paths (`Uni_ProjectObject`).
- **Warp effects off by default** (`uu.Warp` 0). Owner: they are not convincing yet; revisit after the optimisation passes (B-018).
Rationale:
- T2.vol.march_budget (new): on the solar camera, output 627 x 373 (a third of the owner's), half resolution 313 x 186 against the budget's 221 x 131 (2.0x fewer rays). Upsampled to the output against a full-resolution march: light +0.36% / +0.32% (half +0.11% / +0.10%); per-pixel p95 1.23% / 0.88% (half 0.41% / 0.30%). Steps per pixel unchanged.
- T2.vol.handoff_cap (new): inside the rich cluster's core (over 128 candidates), with the carrier cap halved to 64, the light over the whole sphere is within -1.48% of the default cap's with the table, against -10.28% with the curve. At the default cap the curve is already 0.77% short there.
  - Caps measured (table / curve): 100: +0.07% / -3.47%; 64: -1.48% / -10.28%; 32: -11.4% / -21.1%.
  - Below about 40 the cap drops galaxies over UNI_THETA_PROMOTE (0.02 rad). Those draw no sprite at any share (extended promotion candidates), which is why the cap ranks by angular size.
- Background rule: engine-only (UE-33). Offline it costs 2x the steps and 3x the carriers per step (above).
Expected (UE-33): the march about 2x cheaper on the owner's viewport, more while moving. That is the 50% asked for if the march is still ~80% of the frame.
Known: the per-ray carrier cap (UNI_VOL_MAX_ACTIVE 12, D-161) still fades the least significant carriers on a ray crossing more than 12 (a cluster core); their sprites have handed off.
Reroll impact: None (render only).
Status: Provisional (UE-33)

D-197 — Fewer stars per galaxy and fewer galaxies per cluster or group: generated children thinned, their light kept as glow (amends D-133 counts; reroll: thinned objects removed, the kept unchanged)
Question: Owner, after D-196: reduce the galaxies per cluster or group, and the stars (quantity or view distance) within a galaxy, by ~30% each.
Findings:
- From 1 kpc from the Milky Way's centre (the dense core), 1.78 M stars are resident against ~31k drawn in an 1880 x 1120 frame. The project pass walks every resident object each frame, and the streamer generates them all.
- Shrinking star windows would not help in practice: windows are whole cells (5^3 at dm 0), so a 30% shorter reach changes nothing until it removes a whole shell (-78%). Stars past a window would also lose their light, since the LUT assumes they are drawn.
- Thinning the draw is cleaner. Counts come from Uni_PoissonCount (inversion on one uniform), so a lower lambda keeps the first draws: the kept children keep their identities.
Decision:
- **FUniEmitter::SpriteKeep** (default 1). Counts (LambdaScale, and the authored bright members' Poisson draw) use NTotal x Keep; the light stays NTotal's.
- **The carrier's LUT gives the thinned share to the glow**: unresolved = Lbar - Keep F_L G (UnresolvedAt, BuildEmissionLut). Blocks are per (child, keep), cached like the full ones (KeepBlockOf, KeepLutCache), with their own significance start (the glow is significant from the local start, 10 pc). Root emitters too (group dwarfs from the map); field galaxies are not thinned.
- **UniScenes::UseSpriteKeep(Def, StarKeep, MemberKeep)**:
  - every kStar emitter gets StarKeep;
  - every kCluster emitter (members, catalogue clusters' included) and every kDwarf emitter (group dwarfs) gets MemberKeep.
- **Engine defaults 0.7 and 0.7** (`uu.Star.Keep`, `uu.Cluster.MemberKeep`; FClusterTuning; applied at uu.Enable after the web). The offline scenes keep 1 unless a test applies it.
Rationale:
- T2.vol.sprite_keep (new), stars at 0.7:
  - 1 kpc from the centre: 1,784,195 -> 1,248,898 resident (0.700), all of them full-draw stars, sphere light +1.17%;
  - solar circle: 47,051 -> 33,022 (0.702), 99.94% full-draw, +0.23%.
- T2.vol.member_keep (new), members at 0.7 in a rich cluster's core:
  - 534 -> 390 resident (0.730), all full-draw; their own light 0.723;
  - the cluster's glow is linear in keep (47 -> 526 -> 1562 at 1, 0.7, 0.05; 0.01% off the line).
  - The sphere's total moves with which galaxies were thinned. One large galaxy near the camera can hold most of the light (1.2 Mpc out, its thinning took 93% of the member carriers' light), and the per-ray carrier cap (12) drops more of a full core's light than of a thinned one's, so the thinned core reads +6%. In expectation the glow is exact.
Reroll impact: the thinned stars, members and dwarfs are gone; every kept object is unchanged (identity, position, properties). Saved references to a thinned object fail to resolve (the API reports not found).
Status: Provisional (UE-34)

D-198 — Half the stars and cluster members, darker dust, looser hints around the camera, a march budget that falls with speed (amends D-195, D-196, D-197; reroll: as D-197)
Question: Owner, after D-197: still too slow at intergalactic speeds and in dense parts of galaxies. Specific asks:
- thin stars and cluster or group members to ~50% (less pop-in of nearby stars in dense regions, less to load and march in clusters);
- darker dust lanes;
- any further raymarch ideas.
Decision:
- **Thinning defaults 0.7 -> 0.5** (`uu.Star.Keep`, `uu.Cluster.MemberKeep`; D-197's mechanism).
- **Dust x 2 and lane contrast 2.2 -> 3.0** (`uu.Galaxy.DustScale`, `uu.Galaxy.DustLanes`; `UniScenes::UseDustTuning`, applied at uu.Enable; the offline scenes keep the D-132 dust unless a test applies it).
- **`uu.VolumeHintScale` 2 -> 3** (D-195: carriers around the camera only).
- **March budget falls with speed** (`uu.VolumeTravelPixelScale`, 0.35). `uu.VolumeMaxMarchPixels` x lerp(1, 0.35, the travel curve), the curve going 0 at 100 kpc/s to 1 at 10 Mpc/s (FUniFrameData::SpeedAmount, Warp.Current whatever uu.Warp). It is quantised to quarters of the curve, so the march size (and its history) changes at most four times on the way up. Past ~8 march pixels of motion a frame the history is dropped anyway (D-194), so the glow is the fresh quarter alone and detail is not seen.
Rationale:
- T2.vol.sprite_keep at 0.5: 1,784,195 -> 892,015 resident stars 1 kpc from the Milky Way's centre (0.500), identities kept, sphere light +1.93%; solar circle 0.503, +0.43%.
- T2.vol.member_keep at 0.5: 534 -> 280 members (0.524), their light 0.482; glow on the line (0.00%).
- T2.vol.dust_tuning (new): the dust takes 38.6% -> 51.5% of the Milky Way's glow edge-on from 40 kpc, and 46.9% -> 59.5% toward the centre from the solar camera. Steps unchanged (74.9 -> 74.0; not opaque enough to stop rays early).
- T2.vol.perf_inside at hints x 3: 58.6 / 45.8 / 37.4 steps/px toward the centre / along the disc / out of the plane (D-195's x 2: 74.8 / 52.2 / 41.1). p95 0.27-0.41%; T3.vol.march worst 0.67% (inside 0.43%).
- Speed budget: at full travel speed 0.35 of the rays: on the owner's viewport about 0.09 Mpx (~400 x 235), against D-196's 0.26 and the old 0.53.
Next if still short (B-017 and the owner's capture decide):
- the march's per-step cost (the textured galaxy sample and its hint, now the bulk with ~1 carrier active);
- CloudAtmosphere's accumulating resolve with a 3 x 3 or 4 x 4 interleave (2.25-4x fewer rays a frame);
- empty-space skipping outside the disc.
Reroll impact: as D-197 (thinned objects gone, the kept unchanged); dust and the march are render-only.
Status: Provisional (UE-35)

D-199 — Polish: coarser log steps, a coarser textured-web march, the warp optics' motion in double (B-018), the D-198 dust still (render-only)
Question: Owner, away for a few hours: keep polishing the stack, performance (especially at speed) first.
Findings:
- **Per-step cost** (CPU reference, inside the Milky Way, untextured): a dusty galaxy sample (emission + dust, D-181) 306 ns; the step hint 90 ns; the LUT 9 ns. The pair is twice an emission sample: the dust evaluates its own disc and arms. No safe cut there (D-181's pair test is bit-exact), so this round cuts steps.
- **Step ratio, with hints x 3** (T2.vol.perf_inside, steps/px toward the centre / along the disc / out of the plane; p95):
  - 0.24: 58.6 / 45.8 / 37.4, p95 0.27-0.41%;
  - 0.32: 52.8 / 38.9 / 30.7, p95 0.40-0.61%, T3.vol.march 0.72%;
  - 0.40: 49.5 / 34.9 / 26.9, p95 0.78%;
  - 0.32 with hints x 4: 44.8 / 35.8 / 28.9, but T3.vol.march 2.2% per ray toward the centre.
- **Textured web** (T3.webtex.background, cube light against the procedural web / correlation / median texel / steps per texel):
  - 0.2: -0.14% / 0.999 / 1.3% / 50;
  - 0.3: the same / 43;
  - 0.4: -0.13% / 0.999 / 1.3% / 40.
  - The texture's own hint bounds the step past 0.4. This is what the background costs per pixel while travelling fast (D-196).
- **B-018: the warp optics' motion overflowed.** WarpVelBetaOf took FVector3f::Size() of the camera's step, and a float's square overflows past 1.8e19 cm (6 pc a frame). At any warp speed the motion came out zero with the effective speed non-zero. Uni_Aberrate with a zero motion shortened every direction by sqrt(1 - b^2). So every sprite took a tail of that shortening whatever its distance: ~55 px at 20 degrees off centre and the 64 px cap at 40 at full warp (light spread that far: gone). The volume marched shortened rays and reprojected with them.
  - At the owner's 425 kpc/s (warp ~0.23, b ~0.12) that tail is ~1 px. So it is surely part of what the owner saw at speed with warp on, but maybe not all of it (the pawn dimmed too: auto-exposure is the other suspect).
- **Dust look:** a dust arm sharpness of 6 (with dust arm share 0.9) was tried for crisper lanes: it aliased into rings at the centre. Rejected, not kept.
Decision:
- **`FVolumeSettings::StepRatio` and `uu.VolumeStepRatio` 0.24 -> 0.32.**
- **`TexturedWebStepRatio` 0.2 -> 0.4** (`uu.VolumeWebStepRatio`, new).
- **`UniCore::WarpMotion`** (UniTravel.h): the camera step normalised in double; WarpVelBetaOf uses it. Uni_Aberrate and Uni_DopplerFactor leave a direction and its colour alone when the motion is not a unit vector (|v|^2 <= 0.25). `uu.Warp` stays 0 by default until the owner has looked again.
- **still_galaxy_dust_d198** (T2.galaxy.dust): the D-180 cloud dust over the engine's D-198 dust, one textured galaxy at 25 and 68 degrees.
Rationale: T2.vol.perf_inside and T3.vol.march as above. T3.webtex.background as above. T1.warp.speed (new): a 7 kpc step gives a unit motion (0 error; the float length was inf); a zero motion changes nothing (0, Doppler 1); a galaxy 20 degrees off centre streaks 1.84 px at 1 Mpc (its parallax x the warp amount, 1.97) and 0 at 50 Mpc.
Reroll impact: none (render only).
Status: Provisional (UE-36)

D-200 — The glow's march resolution follows the GPU frame time (dynamic resolution for the volume alone; render-only)
Question: Polish round 2, the owner away. The target is 60 fps with the ocean, atmosphere and meshing pipelines also on the GPU. Fixed budgets (D-196, D-198) cannot know what else the frame holds.
Findings:
- Inside a rich cluster's core (offline, sliced grid at 331 x 197): 16.1 steps/px and no supersampled pixel. But the wave maximum is 37.4 (2.3x the mean: rays crossing galaxies take more steps than their neighbours). So clusters cost less per pixel than a galaxy's interior (50+), which stays the worst case.
- Cutting the per-step cost needs the GPU profile (D-199). The one lever every scene shares is the march's pixel count.
Decision:
- **FVolumeAutoBudget** (UniVolume.h): seven fixed levels of the march pixel budget, 1, 0.84, 0.71, 0.6, 0.5, 0.42, 0.35 (~0.92x the march's linear size per level).
  - Smoothed GPU frame time (EMA 0.15): down a level when it is 3% over the target, up when 15% under.
  - At most one change per `uu.VolumeAutoHold` (20) frames, since each change restarts the glow's temporal history.
- **Engine:** `uu.VolumeTargetFps` (60; 0 off). The GPU frame time is RHIGetGPUFrameCycles(0) (DynamicRHI.h). F.VolumeBudgetScale reaches the renderer, and the smaller of it and the travel scale (D-198) wins.
- `uu.perf` prints the level, the scale, the smoothed GPU time and the changes so far.
Rationale: T2.vol.auto_budget (new, a model frame with 5% noise at 60 fps):
- with 6 ms of other work and the glow 17 ms at full budget, it settles on level 3 (x0.6, 16.2 ms) after 3 changes;
- it holds for 1000 frames without a change;
- it climbs back to full when the glow drops to 6 ms;
- it stays full with no target.
Known:
- If the rest of the frame alone is over the target, the glow drops to its floor (x0.35) and stays: it cannot help, and it costs little there.
- The GPU time comes from the RHI's own frame timing (what `stat unit` shows as GPU).
Reroll impact: none (render only).
Status: Provisional (UE-37)

D-201 — Travel sensitivity default 2.5 -> 0.5 (amends D-154; render and streaming only)
Question: Owner, after D-200: intergalactic speeds still drop to ~30 fps. D-200 only scales the glow.
Findings: T2.vol.travel_load (new). Flythrough, thinned as the engine's (D-198), camera 100 Mpc out; resident objects / cells by the travel boost (dm):

| dm | Objects | Cells | x rest |
|---|---|---|---|
| 0 | 17,349 | 584 | 1 |
| +0.5 | 30,641 | 1,027 | 1.77 |
| +1 | 71,870 | 2,435 | 4.14 |
| +1.5 | 102,092 | 3,471 | 5.9 |
| +2.5 | 307,803 | 10,490 | 17.7 |

D-154 adds up to `uu.TravelSensitivity` (2.5) mag while flying fast, widening every window. All of that is generated, projected and drawn while the camera moves through it.
Decision: `uu.TravelSensitivity` 2.5 -> 0.5. The boost and warp curves are otherwise unchanged (the warp amount and D-198's travel pixel scale have their own curve).
Rationale: at +0.5 the view at speed holds 1.8x rest's objects, not 17.7x. Galaxies still brighten a little into view while travelling. Owner: 0 turns it off.
Reroll impact: none (what is resident changes; what any cell generates does not).
Status: Superseded by D-213 (travel sensitivity removed; UE-38 withdrawn)

D-202 — The background web's step hints loosen with travel speed (render only)
Question: Owner: intergalactic speeds are still heavy; should distant clusters come from the density field rather than being spawned and marched?
Findings: clusters are one per web-texture node (the 113 heaviest a 200 Mpc tile; the other 4,012 nodes are groups, labels only, D-174/D-177), and far ones are already sprites only: a cluster's contributors wake lazily (D-176/D-177), so nothing is marched for them. The engine-like textured scene at the void camera: 2 periodic background carriers + 1 cluster carrier with no contribution in view; 73 steps a pixel, all of them the background web, 2 active carriers a step. While travelling fast (D-196) that background is marched per pixel, so at speed the glow's cost is the web, not the clusters. Its step hints x k (T3.webtex.background cube, 6 x 96^2):

| x | Steps/texel | Median texel | Correlation |
|---|---|---|---|
| 1 | 40 | 1.3% | - |
| 2 | 27 | 1.9% | 0.997 |
| 3 | 23 | 6.1% | 0.973 |

Decision: `FVolumeViewGPU::WebHintScale` (was Pad1) multiplies the step hint in periodic carriers that do not contain the camera; the engine sets it to lerp(1, `uu.VolumeWebTravelHint` (2), travel amount). The direction map is always marched at x 1 (it is reused at rest).
Rationale: x 2 is 32% fewer background steps for -0.11% light and a 1.9% median texel, and only while moving fast, when the march is also at reduced resolution (D-198) and the eye is least critical.
Reroll impact: none.
Status: Provisional (UE-39). T2.webtex.travel_hint.

D-203 — Cell glow: what has not loaded is glow, windows keep their rest size, a fixed build budget (amends D-128, D-139, D-162, D-173, D-154; render and streaming only)
Question: Owner, after D-202: "not have to use individual entities at that scale for raymarching, and to be able to transit at any speed scale without slowdown. Ideally our loading times would be fast enough that we don't need to stretch the windows and can just have a fade out implicit in the march." Owner chose to try it.
Findings:
- The glow already carries every child not drawn as a sprite (D-128), and distant clusters and galaxies are never marched one by one (D-129, D-177). But the drawn share was per level: a cell still loading inside the visible radius was a hole (pop-in). Windows stretched ahead at speed to load cells before they were seen (D-139, D-162), travel sensitivity widened them further (D-154), a level's fade waited for its visible cells (D-128's hold), and D-173 had to let "urgent" cells through unspread. So the work per frame grew with speed.
Decision:
- **Per-cell fade.** A cell draws its sprites x its own fade: 0 until it is Ready, then rising to 1 over `uu.CellFadeFrames` (8) frames from the frame it became Ready (`FCell::ReadyAt`; a regeneration fades in again). It goes to the GPU in the cell table's fade bits (the level's fade x the cell's). A level's fade no longer waits for its visible cells.
- **Cell glow in the march.** Per carrier block the LUT holds, after its emission and tail blocks, one block per child level q of the light drawn as sprites at that level, D_q(t) = sum over the level's slices of P keep F_L G (what the emission block subtracts for it; `BuildEmissionLut(..., bDrawn)`; the emission block's first entry's w holds first level + 256 x count). Each frame the streamer lists the levels with a cell not fully drawn and their cells' fades over the window box (`FUniStreamer::CellGlow`, `FCellGlowLevel`); the builder appends those grids to the LUT buffer after every block (`FVolumeViewGPU` grows 64 -> 96 bytes: `GlowMask` over levels 0..127, `GlowBase`, `GlowLevels`). At each step the march adds (1 - cell fade) D_q(t) for the cell of each loading level q of the carrier's children that holds the sample (`Uni_CellGlow`, `Uni_CellGlowCarrier`, `Uni_CellGlowFade`). Nothing loading: no cost.
- **Windows keep their rest size** (`FUniStreamer::WindowLeadMax`, `uu.WindowStretch` 0; D-162: 3). Suspension (velocity LOD) keeps D-162's threshold (`SuspendLeadCells` 3): a level whose cells would be crossed before they load is suspended into the glow, as before.
- **A fixed build budget** (`FUniStreamer::BuildBudget`, `uu.StreamBuildBudget` 96 cells a frame over every level). Chosen across levels after the level loop (`SubmitBudgeted`): cells a finer level's parent gate waits on first, then the least slack (D-173's frames until visible at this speed), then the nearest, coarser levels first. Regenerated cells always go and count against it. The rest stay Waiting: glow. 0: D-173's per-level rule.
- **Travel sensitivity default 0.5 -> 0** (`uu.TravelSensitivity`): it widened every window at speed.
- The background map is marched without cell glow (it is kept while cells load and fade, so their glow would stay baked in); per pixel, while travelling fast, it has it. The project pass no longer multiplies the warp arrival into the fade (the cell's own fade is the arrival now; the streak stretch stays).
- Engine: `uu.CellGlow` (1) switches the cell fade, the LUT's drawn blocks and the grids together. Offline the streamer's and builder's defaults stay off (the earlier checks keep testing the earlier rules).
Rationale:
- T2.vol.cell_glow_exact (new): solar camera, 18 star levels; every cell of every star level at fade 0 through the grids equals the levels' fades at 0 (total -0.0000%, worst pixel 0.000%); a hashed half of the cells plus its complement equals all plus none (-0.0000%, worst pixel 0.000%).
- T2.stream.cell_glow (new): the Milky Way at 10 pc a frame (600 pc/s), 3-frame phases, no stretch, 96 cells a frame. Light over the sphere against the settled reference, over its sprite light (frames 16 / 32 / 48): -1.20 / -0.59 / -0.24% with the glow, -2.68 / -1.91 / -1.39% without. With local carriers from 1 pc instead of D-195's 10 pc: -0.58 / -0.09 / +0.20%: most of what is left is stars within 10 pc of suspended or loading levels, lost with or without the glow (as before D-203). At 2 pc a frame cells load in time either way (0.04%). At most 96 cells built a frame; windows 125 cells.
- T2.webtex.cell_glow (new): the owner's scene (adhesion web, node list, galaxies, clusters, stars) from rest in intergalactic space to 30 Mpc/s, D-202 (stretch, D-173, travel sensitivity +0.5) against D-203: streamer bookkeeping 1.14 -> 0.35 ms a frame, generation (the CPU backend: the GPU's work in the engine) 2.54 -> 0.60 ms a frame, worst frame 32-37 -> 5 ms (two runs), cells submitted at most 336 -> 96 a frame, tasks over the flight 6,883 -> 2,793, largest window 441 -> 125 cells. Light within 0.3% of the reference's sprite light (glow or not: at intergalactic speed the levels that cannot keep up are suspended into the glow already; D-202 +0.9%, its travel sensitivity drawing more).
Known:
- A cell's stars within the local carriers' 10 pc start (D-195) are missing while it loads.
- The first fill after uu.Enable or a teleport is glow condensing over ~(cells / 96) frames instead of a level fading in whole.
- Not built: the generation round trips (probe, then place: ~6 frames) are unchanged; one round trip would put less of what is near the camera in glow at a given speed.
Reroll impact: none (when a cell is built and how it is drawn; never what it holds).
Status: Provisional (UE-40)

D-204 — Objects placed from their cell's float origin away from the camera (amends D-104; render only)
Question: Owner: on an implicit octree, an object should not redo the digits its cell (or parent) already fixes, all the way down the chain. Generation, the march and the CPU bookkeeping already work that way (objects store 32-bit in-cell offsets; fields are evaluated in the parent's float frame; carriers march in their own frame; contributors are measured from a nearby anchor). The project pass did not: every resident object, every frame, added its offset into its cell's exact 128-bit camera delta and converted to float (D-104), ~100 integer operations an object (~900k stars in a dense core). Owner: build it.
Decision:
- `Uni_CellRel` (UniCoord.ush) builds a cell's context once: the exact deltas, the origin as hi + lo floats (`Uni_S128ToHiLo`), and bFar: the camera is at least one cell width from the cell on some axis (so every object in it is at least that far), level 32 or above.
- `Uni_ObjectRelativeC`: a far cell's object is Hi + ((Lo + offset x 2^(L-32)) - camera fraction), float adds; otherwise the exact integer path (D-104). The inner sum is about a cell width, so it rounds by 2^-24 of that; with the outer add, at most twice the exact path's 2^-24 of the object's distance.
- The project pass builds the context once per thread of the cell's group, before its loop over the cell's objects (`Uni_ProjectObjectC`); `Uni_ProjectObjectH` builds it per call (offline, unchanged results within the bound). Pick and query stay exact.
Rationale: T2.parallax.cell_rel (new): every resident object of the flythrough scene at four cameras (solar, the anchor's centre with 4.8M objects resident, just inside a level-66 cell's corner, intergalactic): direction within 8.8e-8 rad of exact double precision and distance within 1.2e-7 (the exact path alone 5.7e-8); 66% of 4,986,405 objects on the float path (the 27 cells around the camera's cell, per level, keep the exact one). On the CPU shim 98 -> 43 ns an object (a proxy: the GPU's 128-bit work is 32-bit integer lanes; the float path is ~12 adds and multiplies against ~100 integer operations).
Reroll impact: none (render only; positions change below a few parts in 10^7 of the distance).
Status: Provisional (UE-41: ProfileGPU's project pass in a dense core, before and after)

D-205 — Clusters visible at speed again (render-only exposure); speed inside galaxy cores: cheaper cell glow, fewer stars, project pass guard (amends D-197/D-198, D-203, D-204)
Question: Owner, after D-203/D-204 in the engine: intergalactic flight is faster, but nearby clusters are less visible at speed ("boost the density/opacity of the effect or something"); travel at mid speed inside a galaxy, worst around the core, became a pain point that lags and eventually resolves; how many warp octaves; fewer stars if it helps.
Findings:
- Sensitivity raises the exposure with the limiting magnitude (ExposureMag = LimitingMag - 1 + bias). Travel sensitivity brightened everything at speed by 10^(0.4 dm): x10 at D-154's +2.5, x1.6 at D-201's +0.5. D-203 set it to 0 (it widened every window), which took that brightening away.
- Offline core flight (the Milky Way 1 kpc from its centre, stars at keep 0.5, 3-frame phases, 40 frames at 20 pc a frame), D-202 rules against D-203: streamer bookkeeping 11.2 -> 8.0 ms a frame, generation (the CPU backend: the GPU's work in the engine) 40.2 -> 22.3 ms (worst 661 -> 135 ms). So the offline model does not show D-203 as the regression; the engine's numbers are needed (UE-42).
- The cell glow's march cost there (one carrier, 4-5 loading levels): +6% at 5 pc a frame, +2% at 20. Every loading level was tested at every step, though a sample far out lies in none of the fine levels' windows.
- D-204's per-cell context was built by all 64 threads of a cell's group, including those with no object (most cells hold fewer than 64).
- "Warp octaves": none at runtime. The galaxy field reads baked textures (D-179): warp_fine (discs and arms), warp_large (bulges, faded out past 0.5 of the radius), galaxy_detail; one read each per sample, at the footprint's mip, and the warps are skipped past mip 4-5 (D-181). Their octaves are baked into the textures. The warp travel effect (uu.Warp) is off.
Decision:
- **`uu.TravelExposure` (1.5 mag)**: render-only brightening at speed, x the travel curve (0 at 100 kpc/s, 1 at 10 Mpc/s, rising 2/s, falling 1/s), quantised to 0.05 mag; added to the exposure bias for sprites, glow and background (`UUniSubsystem::TravelExposureMag`). Windows, the limiting magnitude and everything generated are unchanged. 1.5 mag is x4.
- **Cell glow pruned by distance** (`Uni_CellGlow`): a level's window reaches at most ~17.4 of its cells from the camera, so levels with 2^L < t / 18 are masked off at the step (they would read fade 1). The carrier's first level travels with its grid entry (bits 8..15). Exact (T2.vol.cell_glow_exact unchanged).
- **Project pass**: only threads with an object build the cell's context.
- **Stars per galaxy 0.5 -> 0.35** (`uu.Star.Keep`, FClusterTuning::StarKeep; owner's suggestion). Cluster members and dwarfs stay 0.5.
Rationale:
- Core flight, keep 0.5 -> 0.35: generation 22.3 -> 16.1 ms a frame (worst 135 -> 89 ms), resident objects 819k -> 579k.
- Cell glow's march cost in the core at 5 pc a frame: +6% -> +0.5%.
- T2.vol.sprite_keep at 0.35: 0.350 / 0.353 of the full draw resident, 100% of the kept in the full draw, light over the sphere +2.50% at 1 kpc from the centre (+1.93% at 0.5, +1.17% at 0.7: the excess grows with the thinning) and +0.48% at the solar circle; its bar is now 3%.
Known: why the engine's mid-speed core flight got slower is not reproduced offline. The A/B to run there: uu.CellGlow 0, uu.StreamBuildBudget 0 / 192, uu.WindowStretch 3, with stat unit and uu.perf.
Reroll impact: stars per galaxy thinned further (0.35); the kept stars are unchanged (D-197). Everything else render only.
Status: Provisional (UE-42)

D-206 — One web texel read for the background's two carriers (B-019; render only)
Question: Owner: build B-019. At an intergalactic camera the glow is two periodic textured web carriers (field galaxies on walls, filaments and void matter; group dwarfs on the node matter), the same texture in the same frame, so every step read the same texel twice.
Decision: At gather, a periodic textured web carrier in the same frame (camera position, period, rotation, prefilter) and texture resolution as an earlier one is paired with it (`Uni_CarrierSameWebTexel`); at each step the texel is read once and each carrier weights the channels itself (`UniField_WebTexCh`, which `UniField_WebTex` now calls). `FVolumeViewGPU::MarchFlags` (was a pad) bit 0 turns it off for checks.
Rationale: T2.webtex.shared_texel (new): the owner's scene from the void camera, background map 6 x 64^2: 2 textured web carriers, 1 pair; 0 of 24,576 texels differ, same steps and truncation; the CPU reference 620 -> 384 ms (its trilinear emulation; on the GPU the saving is one trilinear fetch of two per step).
Reroll impact: none (render only; bit for bit).
Status: Provisional (UE-43)

D-207 — One generation round trip for leaf cells (B-020; streaming only)
Question: Owner: build B-020. With cell glow (D-203) latency decides how long what is near the camera stays glow at speed. A cell took two backend round trips (probe for counts, CPU allocation, then place: ~6 frames, D-122).
Decision:
- A cell whose objects never come back (every task's child archetype has no emitters: stars) and that holds no authored objects is generated in one trip: probe, allocation and place back to back on the backend, in a pool arena the streamer reserves; only the counts come back.
- **Allocation rule** (`FuseAllocate`, UniStreamer.h, and UniGen.usf's new ALLOC pass, line for line): cells are runs of tasks with the same ordinal (`FGenTaskGPU::BoxLo[3]`, unused before) in order; a task's capacity is its count truncated so the cell holds at most 2^15 (lowest indices kept), its index base the running sum before it (as D-122); a cell whose total fits in the arena's remaining room is packed at the cursor, otherwise its tasks get capacity 0 and the next cells still try. The place pass reads the ranges from the ALLOC output (`UseAlloc`) instead of the task.
- **Arena**: per cell 1.5 x its level's largest recent cell (decaying 1% a frame) + 64, or 2048 while the level has none yet; at most a quarter of the pool per batch (the rest go two-trip). On completion the streamer runs the same rule on the read-back counts: cells that fit are Ready; a cell that did not wrote nothing and goes two-trip from then on; the arena's unused tail and the ranges of cells evicted meanwhile are released; a batch whose cells were all evicted releases its whole arena.
- `IGenBackend::SupportsFused / SubmitFused / PollFused` (CPU backend and the GPU backend: `FUniGenAllocCS`, one graph of probe, alloc and place); `FUniStreamer::bOneTrip` (engine `uu.GenOneTrip` 1; offline default off).
Rationale: T2.stream.one_trip (new): the D-203 flight along the Milky Way's disc (10 pc a frame, 3 frames a trip), two-trip against one-trip against one-trip with a first arena of 1 slot a cell (every first try overflows): every cell Ready in both runs holds the same records in the same order (1,649 cells, 0 differ, both comparisons); identities resolve; the pool holds exactly the Ready cells' ranges (64,097 slots in all three: nothing leaked); 4,013 cells took one trip (0 overflowed; forced: 200 overflowed and fell back); frames from submission to Ready 6.00 -> 3.54 on average.
Reroll impact: none (the same objects, sooner).
Status: Provisional (UE-44: shaders compile, `Uni.Gen.Alloc` in ProfileGPU, stars arrive sooner at speed)

D-208 — The volume's temporal slicing grows to 3 x 3 or 4 x 4 when the image is near still (B-017, part; render only)
Question: Owner: build B-017 (the volume march's phase 2: a larger interleave, per-step jitter with an accumulating history, clipping rays to the galaxy's disc).
Findings (each against a full march, offline):
- **Larger interleave.** The D-184 slicing marches one pixel of each 2 x 2 block a frame and reprojects the rest; with N x N, one in N^2. Errors grow with the motion between a pixel's refreshes (it is resampled N^2 times between), so N may only grow while the image is near still. From 60 kpc turning 0.1 deg a frame (0.17 march px): 4 x 4 p95 3.0%, 3 x 3 2.0%, 2 x 2 0.8%.
- **Clipping to the disc slab and bulge sphere** (12 footprint-widened flared scale heights plus the warps' reach; 18 widened effective radii; 99.9% of the light): 30.7 -> 29.3 steps per pixel over five views inside and outside the Milky Way, light within 0.005%, but the reference march no faster (the halo's steps are few and cheap already: the hints relax and the textures are gated off the disc). Not adopted, no code kept.
- **Per-step jitter** only pays with a history that accumulates fresh samples (an EMA), which adds lag and ghosting exactly where the owner sees problems (at speed). Not built.
Decision:
- **Interleave N by motion** (`Uni_TemporalInterleave`): the renderer estimates how far the image moves a frame in march pixels (the view's turn, plus the camera's step over the nearest marched structure, `FVolumeScene::NearCm`: each carrier's march start, or its sphere from outside); 4 x 4 below 0.1 px, 3 x 3 below 0.5, the D-184 2 x 2 beyond. `uu.VolumeInterleave` (4; 2: always 2 x 2). N can change every frame (the history is the full grid either way).
- **Phases** (`Uni_TemporalPack`: step | N << 8): 2 x 2 exactly as D-184 (its checks unchanged); 4 x 4 in the Bayer order; 3 x 3 a diagonal walk. Fresh pixels, their lattice taps and the march's dispatch (`bSliceMarch` now carries N) generalise to N.
Rationale: T2.vol.interleave (new), 24-frame paths at 200 x 125 against a full march every frame (adaptive / 2 x 2):
- inside the Milky Way at rest: N 4, 6% marched; light 0.00% / 0.00%, p95 0.00% / 0.00%;
- turning 0.02 deg a frame: N 4; p95 0.12% / 0.03%;
- turning 0.2 deg: N 3, 11% marched; light 0.02% / 0.02%, p95 0.37% / 0.19%;
- drifting 0.002 pc a frame: N 4; 0.00%;
- turning 1 deg: N 2 (unchanged), p95 0.44%;
- from 60 kpc turning 0.1 deg: N 3; light 0.39% / 0.41%, p95 2.01% / 0.77%.
T2.vol.temporal and temporal_fast unchanged (2 x 2).
Known: it saves 2.3-4x of the march's rays only while the view is near still (inside a galaxy core at rest or drifting); in motion it is the D-184 2 x 2 as before. The march at speed is unchanged by this decision.
Reroll impact: none (render only).
Status: Provisional (UE-45)


D-209 — The background map's re-march test ignores the cell-glow grids; uu.Perf says why the glow was or was not sliced and why the background was re-marched (render only)
Question: Owner profiling (engine, at rest): the volume march is the cost (uu.Volume 0: 60 fps everywhere); some views chug and others do not; temporal slicing on; uu.CellGlow makes a trivial difference. uu.Perf in a costly view: "volume march 967x636, 615012 rays last frame, with the background", auto budget at its lowest level (x0.35) with GPU 31 ms; the background was marched per pixel in nearly every frame between the two logs, at rest.
Findings:
- **The background is re-marched at rest.** The subsystem re-marches the background map (and under uu.BackgroundMode 2 marches the background per pixel while that keeps happening) when the camera moves, its LUT blocks change, the sky-web settings change, or the LUT buffer's size changes. Since D-203 the cell-glow grids follow the LUT blocks in the same buffer and change size whenever any level starts or finishes loading (cells streaming in or fading, also at rest), so the size test fired every few frames: D-196's hysteresis then kept the whole periodic web in every pixel's march, the most expensive configuration (the uu.VolumeDebug 1 view in the costly spot shows uniform mid-heat over the whole sky: the web marched per pixel).
- **The march was not capped.** 615,012 rays = the whole half-resolution grid of the owner's 1934 x 1272 output: uu.VolumeMaxMarchPixels was 0 (no limit). My profiling note had said to restore it with 0 after testing 65536; its default is 262144. With 0 the auto budget (a fraction of the cap) can do nothing either. Not a code change: the owner restores 262144.
- **Slicing was not in either logged frame** (no "sliced" in the line), though the owner sees it apply. The log's text predates D-208 and does not say why a frame was not sliced.
Decision:
- `UniCore::LutBlocksEnd(GlowLevels, GlowBase, LutSize)`: the end of the LUT's blocks (the grids' start, or the size without cell glow). The re-march test compares it (and keeps only the blocks), not the buffer's size.
- uu.Perf: the slicing actually used (1/N^2) and, since the last uu.Perf, frames sliced 1/16, 1/9, 1/4 and not sliced by reason (temporal off or a debug view, no history, march size changed, camera jump, the same frame rendered again, frames skipped); the march cap; background per pixel in how many of the frames since the last uu.Perf; background map re-marches by reason (camera moved, LUT layout, its LUT changed, sky web).
Rationale: T2.vol.bg_lut_layout (new): the Flythrough scene at the solar camera with no grids, one level's and three levels': blocks end 9216 in all three while the buffer is 9216 / 9401 / 9769 entries, blocks identical; without cell glow the end is the buffer's size (2048).
Reroll impact: none (render only).
Status: Provisional (UE-46: uu.Perf at rest shows the background per pixel in ~0 frames and re-marches only when moving; fps at the owner's costly spot with uu.VolumeMaxMarchPixels 262144)

D-210 — A galaxy march step does less work (constants once a frame, shared terms, the next step's limit in the sample pass); arms start outside the core and the disc warp is scale-free there
Question: Owner, after the per-step breakdown: do (1) per-galaxy constants once, (2) share the spiral's terms between light and dust, (3) fold the step limit into the sample, (5) coarser or weaker warp in the core (the owner's core screenshot shows contour-like swirls); not (4) dust every other step. Then the clipmap (B-021).
Findings:
- **Per step, per dusty galaxy** (the camera's own galaxy): ~10 seed hashes (2 avalanches each) that are the same for the whole galaxy (texture offsets, mode phases, four arm hashes; two more in the step limit); the spiral evaluated twice (light and dust, up to 4 lobes each: pow and cos per lobe); up to 3 texture reads; ~60 transcendentals; the carrier and its parameters (15 float4) loaded twice (step limit, then sample).
- **The core's rings and swirls** are the field, not the march: a log spiral winds without limit toward the centre (one more turn every ~1.5 e-folds of radius at the demo's pitch), so inside the bulge the arms (and the dust's, shifted in phase) were tight rings; the disc warp (a fixed displacement, ~225 pc for the home galaxy) bent them into swirls. Offline with the warp off the swirls go and the rings stay; with the bulge's warp off nothing changes.
Decision:
- **Galaxy constants** (`FUniGalaxyConst`, `Uni_GalaxyConstOf`, `Uni_GalaxyConstPack/Unpack`: 16-bit halves, exact in floats): the builder appends one block per parameter block after them (`FUniVolumeView::GalaxyConstBase`, the former pad; `FVolumeSettings::bGalaxyConst`, engine `uu.VolumeGalaxyConst` 1); the march reads them (`Uni_GalaxySharedK`, `UniField_GalaxyPairCK`, `UniField_GalaxyFilteredCK`, `Uni_GalaxyStepHintFilteredK`). Placement hashes inline as before (`Uni_GalaxyShared` wraps the K form).
- **Shared terms**: the shared state carries -log(r) and the disc's radial exponential; an evaluation uses them when its parameters match (the dust's scale length, pitch and arm count are the emission's), else computes its own. Bit for bit.
- **Step limit in the sample pass** (`Uni_CarrierStepLimit`): each carrier's limit at the next step's start is computed while its carrier and parameters are loaded for the sample (same point, same arithmetic); the step start uses it, and computes it only for a carrier not sampled in the previous step (its first step). Bit for bit.
- **Core** (`Uni_GalaxyArmInner`, `Uni_GalaxyWarpInner`; galaxy C.w = V[11], the arms' inner radius in disc scale lengths; 0: off, every older field exactly; exemplar 0.25, so every galaxy and the home galaxy): the arms go to their azimuthal mean (light kept) between half the radius and the radius, and the disc warp's displacement grows linearly with radius up to it. The step limit sets no arm limit inside half the radius.
- Not done: dust every other step (owner unsure).
Rationale:
- T2.vol.galaxy_const (new): bit for bit with and without the constant blocks in the core and from the solar camera.
- Development checks (scratch, 5 views: the core toward the centre / along the disc / up, 0.13 kpc from the centre, the solar camera; 320 x 180, textured, palettes): constants + shared terms bit for bit against the D-209 march; the folded step limit bit for bit against the march before it.
- T2.galaxy.core (new): outside the inner radius plus the warp's reach (20,000 points, filtered and not) the field is the old one bit for bit; ring means inside within 0.05% of the old field's (untextured, modes on); the arms' relative spread inside half the radius 0.00% (was 115%); stills still_galaxy_core_d209 / _d210 from the owner's spot (0.54 kpc from the centre toward it), the same display gain: the swirls are gone; 136.3 -> 133.2 steps/ray there.
- T2.galaxy.shape: the D-172 field with every new parameter at zero, bit for bit (ModesOff now also zeroes C.w).
- Cost (CPU reference, 5 views, 2 runs each, idle machine): D-209 870-888 ns/step; the exact cuts alone 833-880 (-4%); with the core 844-847 and 1.7% fewer steps (-6% in all). On the CPU the software texture reads and libm dominate; on the GPU hashes and buffer loads weigh more, so expect more there, but not a different order: the clipmap (B-021) is the lever. GPU unmeasured here. Per dusty galaxy step: ~10 hashes (~20 avalanches) and two transcendentals (a log and an exp) fewer, the carrier and parameters loaded once instead of twice; inside a quarter disc scale length (~0.5 kpc for the home galaxy) no arm lobes (up to 8 pow + 8 cos a step fewer) and steps 2% fewer there.
Reroll impact: **stars inside a quarter of a disc scale length of every galaxy's centre** (~0.5 kpc for the home galaxy) (the arms' and the disc warp's change there) are placed from the new field; elsewhere nothing.
Status: Provisional (UE-47: ProfileGPU Uni.Volume.March before/after (uu.VolumeGalaxyConst 0/1 isolates the constants); the core without swirls)

D-211 — The galaxy the camera is in is marched from a camera-centred clipmap (the galaxy cache; B-021)
Question: Owner: cache the galaxy as a density volume instead of evaluating its field at every step (after D-210 showed the per-step evaluation is the cost: the spiral twice, three texture reads, ~55 transcendentals).
Findings:
- **Resolution.** A camera-centred clipmap level reaches ~N/2 voxels, so a voxel subtends ~2/N rad at any distance (13 march pixels at the owner's resolution with N = 128). Emulated first by widening the analytic prefilter to 2/N: the core views did not change (p95 0.0%), the solar circle 0.9% at 128. The galaxy's structure is all >= ~40 pc.
- **Built and measured** (T2.vol.galaxy_cache, six views inside the home galaxy, 160 x 90, against the analytic march): cubic voxels gave p95 6.0% (96) / 3.8% (128) at the solar circle: the thin disc and its dust blurred vertically by coarse voxels (without detail or warp textures the error stayed; without dust it grew). Anisotropic levels (a z voxel capped at 2 x 0.3 / N field units for every level but the last, so they reach +-4.5 kpc in z; the last level isotropic for the halo) -> 4.4% (96) / 2.3% (128). Level 0 reaching 128 or 512 pc: the same light (512: one level fewer).
Decision:
- **UniGalaxyCache.h** (UniCore): FGalaxyCache picks the camera's own galaxy (FVolumeScene::Home: the most weighted local prefiltered galaxy carrier whose cut sphere holds the camera, with the camera's field position in double), keeps K levels (level 0 reaches FinestHalfPc, each twice the last, until the last reaches 2 field units) of N^3 voxels in its field units, toroidal: each frame it emits the boxes the camera's move uncovered (at most three slabs a level; the whole level after a jump of a side), coarse levels first, within a voxel budget (a level that does not fit keeps its last window and coarser levels cover it). Publish writes the header (N, K, scale, 1/scale; per level its baked window in field units and its voxels) after the LUT and the view's CacheBase / CacheCarrier / CacheN / CacheLevels (FVolumeViewGPU 96 -> 112 bytes). Resets when the galaxy changes.
- **Shader core** (UniVolume.ush): Uni_GalCacheBakeVoxel (light and dust densities prefiltered over the voxel, x a power-of-two scale for half floats; the palette tint over its largest channel), Uni_GalCacheSample (the finest level holding the point with 1.5 voxels to spare, faded into the next coarser over 4 voxels; false outside every level: the field then). The march uses it for the home carrier (light, dust, tint) where the prefilter has not already gone to the mean. Textures: A RG16F densities, B RGBA8 tint, N x N x K (N + 2): x, y wrap in the sampler, each level padded with copies of its last and first z slices so the hardware's z filter wraps within it.
- **Engine**: UniVolume.usf CacheBakeCS (UNI_VOL_PASS 4, FUniGalCacheBakeCS), persistent textures, boxes queued on the render thread in order from every frame's data (a frame not rendered loses nothing; more than two full bakes waiting drops the queue and the game thread resets the cache), the march binding (UniGalCacheA/B in the common parameters), the background map never uses it. CVars uu.GalaxyCache 1, uu.GalaxyCache.Size 128 (~120 MB; 96: ~50 MB), uu.GalaxyCache.Budget 1048576 voxels a frame, uu.GalaxyCache.FinestPc 512. uu.Perf reports it.
- The step limits stay the field's (correct, and cheap next to the sample); the cache does not change the steps.
Rationale: T2.vol.galaxy_cache (new): core toward the centre / along the disc / out of the plane / 0.13 kpc from the centre / solar toward the centre / solar along the disc: light within 0.05%, p95 0.35 / 0.58 / 0.22 / 0.51 / 2.26 / 2.17%, the same steps; CPU reference march 2.4-3x faster. A 40-frame flight (7 pc a frame) baked incrementally equals a full bake texel for texel (2.0M voxels over 39 frames; a full bake is 16.8M). A 300 pc jump at a budget of N^3/20: 4 levels behind, light -0.01%, p95 0.43%.
Reroll impact: none (render only; placement keeps the field).
Status: Superseded by D-212 and removed by D-213 (UE-48 withdrawn: the clipmap no longer exists)

D-212 — The galaxy the camera is in is marched from its baked 2D map (supersedes D-211's clipmap by default)
Question: Owner: instead of forcing the galaxy's density into a volume texture, go native 2D: nearly everything costly lives in the disc's plane; bake a top-down map once per galaxy and pack the vertical information into its channels. Also: easy impostors (B-022), and top-down views for UI (scanning, warp targets).
Findings:
- **What is planar.** The arms (two lobe sums of pow and cos a sample, light and dust), the winding, the D-178 modes (lopsidedness, flare, the mode warp's height), the disc's radial fall-off: functions of the warped point's x, y only.
- **What is not.** The texture warp (warp_fine, 3D curl sampled at (x, y, 2z)): its displacement changes over ~1.5 kpc in z, against a ~0.3 kpc disc. Baked at the mid-plane (first try) the arms became vertical columns: per-point error 46% of the disc term, per-pixel p95 54% at the solar circle and visible streaks looking down through the disc. The bulge's warp_large is 3D too (39% per point near the centre if dropped).
- **So the map is unwarped and the warp is read per sample**: the warp fetch moves the point (as the field's), the map is read there. Per point the disc then matches the field to 0.9% (mean |error|; bilinear and half floats) and the bulge exactly.
Decision:
- **Map** (UniGalaxyMap.h, `Uni_GalaxyMapTexel`): RGBA16F over the field's x, y in [-1, 1], mips by 2 x 2 means: R the light's in-plane factor (DiscW exp(-R/h_R) (lop/flare) ((1-a) + a W (1+s)), with D-210's inner arms), G the dust's (its weight, phase, arms), B the mode warp's disc centre, A the light's arm part (the knots multiply it). Thickness is analytic (the flared scale height), not baked.
- **Sample** (`Uni_GalaxyMapEval`): the twist, warp_fine (moves the point), galaxy_detail (disc noise, knots, dust lanes and holes), the map at the warped point (mip from the footprint), the vertical profile exp(-|z - B| / H_e) (H/H_e) for light and dust, the bulge displaced by warp_large: the field's terms but the arms and modes. Components as the field's (bulge, disc, arms, knots) for the palette.
- **March**: the home carrier (FVolumeScene::Home) uses it where `FUniVolumeView::MapCarrier` names it (view 112 -> 128 bytes: MapCarrier, MapSize); it supersedes the cache.
- **Engine**: baked on the GPU (owner: 0.8 s on the CPU is too long; bakes should be seamless and fast): when the camera's galaxy or the size changes, the game thread raises a request (an epoch, the size, the light's and dust's parameters) that rides every frame; the renderer bakes `uu.GalaxyMap.Budget` texels of rows a frame (UniVolume.usf MapBakeCS, the same Uni_GalaxyMapTexel; 1M: 2048^2 in 4 frames, 1024^2 in 1), then the mips (MapDownCS, 2 x 2 means), and reports the epoch; the frames after publish it. Until then the glow uses the field (no gap). Leaving the galaxy keeps the map for coming back. The background map never uses it. `uu.GalaxyMap` 1, `uu.GalaxyMap.Size` 2048 (~43 MB with mips; 1024 ~11 MB), `uu.GalaxyMap.Budget` 1048576. `uu.GalaxyCache` now 0 (used only where no map is published). uu.Perf reports both (the bake's frames). UniCore::FGalaxyMap::Bake (CPU) is the reference's (and the same function).
- Placement keeps the field (the map is render only; it differs from the field by the filter's error).
Rationale: T2.vol.galaxy_map (new; the six views of T2.vol.galaxy_cache, 160 x 90): at 2048^2 light within 0.02%, p95 0.07-0.32%; at 1024^2 light within 0.03%, p95 0.08-0.96%. CPU bake 0.8 s at 2048^2 (0.2 s at 1024^2) on two threads: ~4M evaluations of the planar terms (no textures), a few ms of GPU spread over 4 frames. CPU reference march ~20% faster than the field: on the CPU the three software texture fetches dominate; on the GPU the removed work is the arms' lobes (up to 8 pow + 8 cos), atan2, the modes' sin / cos and ~10 hashes, the fetches are hardware (expect much more there; measure).
Reroll impact: none (render only).
Status: Provisional (UE-49: ProfileGPU Uni.Volume.March with uu.GalaxyMap 0 / 1 in the core and at the solar circle; Uni.GalaxyMap.Bake per frame on entering a galaxy, at 1024 and 2048; no seams or blockiness near the camera)
Amendment (owner, engine): no fidelity difference seen between uu.GalaxyMap.Size 2048 and 1024, so the default is 1024 (~11 MB, baked in one frame at the default budget; offline p95 <= 0.96% against the field, 2048: 0.32%).

D-213 — Cleanup after the D-203..D-212 iterations: superseded paths removed, galaxy terms shared between the field and the map, dead code
Question: Owner: a clean-up and review pass before moving further. Remove (owner's choice): the galaxy clipmap (D-211), the window stretch (pre-D-203), travel sensitivity (D-205 replaced it), the A/B toggle of the exact cuts (uu.VolumeGalaxyConst).
Findings (audit): every CVar is read; no member field unused; no SCRATCH / TODO markers; the offline build has no warnings (-Wall -Wextra -Werror). Unused: shader functions Uni_ArrivalWeight (D-205 removed its use) and Uni_CellOriginAxis, UniCore's FLevelWindow::AllGenerated, FMagnitudeTable::CdfAt, FluxSliceForLevel, FUniStreamer::ForEachEmitterObject, the constant UNI_ARRIVAL_RANGE. Duplicated: Uni_GalaxyMapEval (D-212) carried its own copies of the field's texture terms (twist, warps, detail read), detail decoding and bulge.
Decision:
- **Clipmap removed** (D-211): UniGalaxyCache.h, Uni_GalCache*, CacheBakeCS (UNI_VOL_PASS 4), the cache textures and queue, uu.GalaxyCache*, the view's Cache* fields (FVolumeViewGPU 128 -> 112 bytes), LutBlocksEnd's cache argument, T2.vol.galaxy_cache. FGalaxyCache::HomeCarrier -> UniCore::HomeCarrierOf. The galaxy map's check moves to test_galaxymap.cpp.
- **Window stretch removed**: FUniStreamer::WindowLeadMax and uu.WindowStretch; windows keep their rest size at any speed (the engine's default since D-203); the lead still decides suspension. T2.stream.responsive no longer requires every visible cell Ready in steady flight (that was the stretch's guarantee; what cannot load is glow, T2.stream.cell_glow); it reports the holes. T2.webtex.cell_glow's D-202 baseline runs without a stretch.
- **Travel sensitivity removed**: uu.TravelSensitivity and the subsystem's Travel curve; the warp curve (D-187) reads uu.TravelStart / uu.TravelFull itself; the HUD shows the travel exposure instead. FTravelSensitivity stays (the warp curve).
- **uu.VolumeGalaxyConst removed**: the blocks are always written in the engine; FVolumeSettings::bGalaxyConst stays for T2.vol.galaxy_const.
- **Shared galaxy terms** (UniFields.ush): Uni_GalaxyTexTerms (warp_fine, warp_large, detail read), Uni_GalaxyDetailTerms (fBm, knots, dust lanes and holes), Uni_GalaxyBulgeAt (the widened Sersic bulge), used by the field (Uni_GalaxySharedK, Uni_GalaxyFromSharedC) and the map (Uni_GalaxyMapEval). The field is bit for bit the same (five views of the home galaxy against D-212, every pixel; T2.galaxy.shape, T2.vol.galaxy_const).
- **Dead code removed**: the functions above.
- Kept: the 128-bit helpers (Uni_U128, Uni_U128Equal, Uni_S128Abs: unused but part of the coordinate math's API); test-only helpers (Uni_ArrivalDistance, Uni_CellAxis ...).
Reroll impact: none.
Status: Provisional (UE-50: the engine builds with the removals; the look and fps unchanged)

D-214 — Generation load: galaxy stars placed from the galaxy's map, envelopes per octant, dense tasks split over many GPU groups, a hard cap on a frame's generation work, cells in view built first
Question: Owner: with uu.Star.Keep 0.05 the stutter in dense regions at speed is gone, so the cost is the number of stars generated in a frame. Asked for: (a) cheaper star spawning (all three ideas: placement from the same logic as the volume, a tighter rejection envelope, dense tasks off the serial path), (b) a hard per-frame cap (ideally balanced so it rarely staggers), and loading by visibility / angular size. (c) fewer stars in dense regions: not now (its own pass if needed).
Findings: a star cost 6.2 rejection candidates (solar camera, star levels), each a full analytic galaxy field evaluation, plus 128 probe evaluations per task; the GPU's place pass ran one 64-thread group per task, looping a dense cell's tens of thousands of candidates serially with ~14 barriers per 64; uu.StreamBuildBudget counted cells (96), not stars, so 96 core cells were a burst; the build order ignored the view.
Decision:
- **Placement from the map (a2)**: a galaxy task's density (probes, candidates, the component tint) is Uni_GalaxyMapLight: the D-212 map's light at mip 0 with the per-sample warp, vertical profile, bulge and detail, i.e. what the volume marches. Its slot in the task's BoxSpan.w (slot + 1). The maps live in the slots of one Texture2DArray (FGalaxyMapCache, UniGalaxyMapCache.h): keyed by the hash of the galaxy's light parameters (a map is a function of them, so a slot baked again holds the same texels), requested by generation (AcquireLight: R, B, A) and the volume (AcquireFull: dust too, rebaked in place when first asked with dust), evicted only after 64 ticks unused (longer than any batch in flight). A cell whose galaxy's map is not baked waits (glow). uu.GalaxyMap.Slots (4). The renderer bakes the pending slots every frame in its own graph (the frame's texel budget, uu.GalaxyMap.Budget) and reports each slot's epoch. Measured: 1.0002x the field's star count; the map's light within 0.29% of the field's (light-weighted, 1024^2).
- **Envelopes per octant (a3)**, galaxy tasks only: each octant of the sampling box has its 16 probes' max x 1.75 (UNI_OCTANT_PAD); a candidate picks its octant in proportion to the envelopes (new channel OCTANT) and its offset is halved into it. Candidates a star 6.16 -> 3.52; over their envelope 0.19% -> 0.36% (pad 1.5: 3.0 / 0.55%, 2.0: 4.0 / 0.24%). FUniTaskCount 16 -> 48 bytes. Other fields: the same draws and envelope bit for bit.
- **Split place pass (a1)**: UniGen.usf PLACE becomes EVAL (one group per chunk of 256 candidates: accept masks), SCAN (prefix per task) and WRITE (ranks from the prefix and the mask: the serial loop's order exactly; a task's last chunk continues serially from its plan to its budget if the plan fell short, then blanks). Uni_PlanCandidates: the expected candidates to fill the task + 3 sigma (2.6x what the serial loop used on average; short in 3 of 594 tasks).
- **Frame cap (b)**: the frame's one-trip cells go in one batch, in build order; ALLOC (and FuseAllocate, the CPU's mirror) packs cells while the batch's planned candidates stay under uu.GenFrameCandidates (1,000,000; the first cell always goes), the rest are deferred (Waiting, retried next frame, never two-trip). A cell must also fit the split pass's chunk grid (cap / 256 + 8192 chunks), else it goes two-trip.
- **Build order (b)**: classes parent-gated / needed within the latency / later, each in view first (uu.GenViewPriority 1; the view cone with the cell's bounding sphere, the FOV diagonal + 10 degrees); within a class as D-203 (slack, nearest, coarser first).
Reroll impact: galaxy stars (positions and every per-star draw that follows from the new candidate order); nothing else (web galaxies, cluster members, systems: unchanged, T2.cluster.no_reroll, T2.gen.determinism).
Status: Provisional (UE-51: shaders compile; stars appear in every galaxy entered and the dense-region stutter at speed is gone at uu.Star.Keep 0.35; uu.Perf's map and generation lines)

D-215 — The volume's temporal history survives a resize of the march (the auto budget's level changes were 100+ ms frames)
Question: Owner: after D-214, frame drops to 30-40 fps still while a dense region streams in (uu.GenFrameCandidates 100000 does not remove them; 250000 is the best balance). Trace: the GPU's volume march up to 115 ms in a frame (median 14); generation passes not visible. uu.Perf: 225 frames "not sliced: march size changed", the volume auto budget (D-200) at 225 level changes.
Finding: a new march size dropped the temporal history (D-184 required the same size), so the frame the auto budget changed level marched every pixel: 16x the rays at 1/16 slicing. Streaming load pushes the GPU over the auto budget's target, the budget changes level, the level change spikes the frame, the spike pushes the smoothed time further: the drops follow the dense regions, and fewer stars (uu.Star.Keep 0.05) kept the budget from moving.
Decision: the reconstruct pass reprojects the history at its own size (HistSize: Uni_TemporalPrevPixel, the taps and the Catmull-Rom bounds on the history's grid; the motion measured in this frame's pixels, Uni_TemporalToGrid), and the renderer keeps slicing across a resize within 2x either way. The CPU reference (FTemporalVolume) does the same. T2.vol.temporal_resize: the size changing level every 4 frames inside the Milky Way, moving and turning: the most marched in a frame 25% (as at a fixed size; dropping the history: 100%), worst-frame light 3.31% against the fixed size's 3.25%, pixel p95 17.3% against 19.1%.
Reroll impact: none (render only).
Amendment (owner, after the fix: better, a hitch remains; candidates may have to come down if no single cause shows): generation was invisible in every trace (its graphs run outside the scene and had only pass names; ProfileGPU and Insights show scopes). Its graphs now have named scopes (Uni.Gen.Probe / Place / OneTrip, Uni.GalaxyMap.Bake), and uu.Perf's generation line gives the candidates the one-trip batches planned (average and peak a batch since the last uu.Perf).
Status: Provisional (UE-52: no "march size changed" frames in uu.Perf's slicing line; no 100 ms frames in a trace while streaming)

D-216 — Play starts in the flythrough universe in free flight; performance defaults
Question: Owner: default to the galaxy map; begin play in flythrough mode, flying, without console commands; every expected flag at its performance default.
Decision:
- The galaxy map is already the default (uu.GalaxyMap 1 for the volume; placement always reads it, D-214).
- uu.AutoStart (flythrough) enables that scene when the world begins play (UUniSubsystem::OnWorldBeginPlay; none: wait for uu.Enable); uu.AutoFly (1) turns free flight on once the player's pawn exists (it is possessed after the world's begin play).
- Defaults audited against the decisions: every uu.* default is the decided value (uu.GenFrameCandidates stays 1,000,000: generation measured at ~0.12 ms a frame in the owner's trace, D-215). Changed: uu.SkyLightBlend 1 -> 0 and uu.SkyLightInterval 1 -> 2 s. Each blend step set a new sky light cubemap, and a movable sky light recaptures (refilters, averages its brightness) on every change, waiting on the GPU: the owner's uu.Perf light-rig peaks of 18-28 ms and the trace's FlushRenderingCommands / ComputeSingleAverageBrightnessFromCubemap outliers (130 / 60 ms). Now one recapture every 2 s; promoted bodies' sky lighting changes in a step instead of a 1 s blend. Amendment (owner: better, occasional spikes left; uu.Perf light rig peak 12.8 ms): refreshes only while the camera is slower than uu.SkyLightMaxSpeed (1 pc/s; the warp amount starts at 100 kpc/s, far above in-galaxy flight): faster, the sky changes every frame and lights nothing (bodies are promoted near rest). The owner's two spike selections were the engine's frame limiter sleeping (FEngineLoop_UpdateTimeAndHandleMaxTickRate 324 of a 334 ms frame: the editor's 3 fps background throttle), not universe work (~3 ms).
Reroll impact: none.
Status: Provisional (UE-53: play starts flying in flythrough; uu.Perf's light rig peak small; uu.SkyLight 0 shows whether any hitch is left from it)

D-217 — Sliced galaxy map: prototype measured offline, not adopted (owner decision pending)
Question: Owner, 2026-11-09 (away; "prototype the sliced bake and get some comparisons"): bake the disc warp into the galaxy map as vertical slices (top, top-mid, bottom-mid, bottom), interpolated, so a galaxy sample skips its warp_fine read without the columns of the first D-212 bake (warp at the mid-plane only); possibly jitter the slices or a z-only warp.
Decision: Measured, nothing in the engine or the shader cores changed. `univerify galslices` (Tools/UniVerify/src/galslices.cpp; images by Tools/UniVerify/galslices_report.py) bakes N slices of RGBA (light's in-plane factor and arm part at each slice's own warped point; the disc's centre as seen from the slice, which folds the warp's vertical displacement and the mode warp into one channel) and evaluates them with one trilinear read, no warp_fine read, detail and bulge as today. Slices relative to the disc's centre and flared scale height (centre and flare computed analytically before the read); absolute slices need the mode warp's span (8 absolute: 28.7% against 13.7% relative). Against UniField_Galaxy, light-weighted per-point error (importance-sampled over the disc, 300k points) and two emission renders inside / just above the home disc (per-pixel p95):
- Today's 2D map + warp_fine read: 0.27%; renders p95 0.03-0.04%.
- 1 slice (the columns): 34.8%; renders p95 27-28%.
- 4 slices (32 MB at 1024^2): 24.1%; renders p95 17-22%, +3.5% light in the in-disc view.
- 8 slices (64 MB): 13.7%; renders p95 10-11%. 16: 6.5%; 32: 3.7%. 512^2 gives the same (8 slices 14.0%, 16 MB): height is the limit, not in-plane resolution.
- The slices must span the camera's height: 16 slices at +-3H gave p95 10% from 4H above the disc, at +-4H 4.3%.
- Jitter by the detail fBm (0.5 slice) breaks the interpolation's regularity but adds error (8 slices 14.9%).
- Fast warp (top of the rolled range): 8 slices 24.4%. Thick disc (H 0.04, dwarf-like): 8 slices 41.5% (warp_fine's z period is 2 H there).
Why: warp_fine has ~30% of its energy at 8-32 cycles a tile (its manifest's bands), z periods of 1-4 H in a home-like disc (its frame doubles z), so linear interpolation between slices converges only about linearly with the slice count. A smoother or slower-in-height warp makes slicing accurate, but is a look change: warp_fine at mip 2 (top octaves dropped): 8 slices 3.9%, the look changes 23.6% per point; the warp 4x slower in height (in-plane pattern kept): 8 slices 2.8%; 8x: 4 slices 2.6%, 8 slices 1.6% (its per-point change, 32-34%, is a different realisation at each height, not a loss of detail; its own 1-slice error, 17% / 10% against the field's 35%, is how much less it varies with height).
Cost: the twist frame stays (galaxy_detail is read in it), so slicing saves the warp_fine fetch and turns the 2D map fetch into a 3D one: about one fetch of four for a disc sample inside 0.5 field units, plus memory (8 slices: 16 MB a galaxy at 512^2, no z mips). Not the 50% the owner is after even when exact.
Side finding: the bulge's warp_large is read inside r < 0.5 (a quarter of the disc's area, ~92% of its light) though the bulge it moves is ~1% of the light at r = 0.3. Read inside r < 8 r_e (0.32 for spirals) fading over [0.6, 1]: the field changes 0.07% per point (p95 0.25%; bulge weight 1.5: 0.13%, p95 0.62%), and the read covers 10% of the disc's area instead of 25%. Measured only; adopting it changes placement density slightly (stars reroll where an accept decision flips).
Reroll impact: none (nothing adopted).
Looks (vertical cross-sections, the report's panels): 4 slices break the columns into soft vertical bands; 8 slices keep the field's layering with its thin wisps softened; 16 are hard to tell from the field. The warps 4x and 8x slower in height look much like the 1-slice columns themselves (one wavy ridge, vertical streaks): they fit their slices because they hardly vary through the disc, which is the look the slices were meant to remove.
Status: Measured; owner to choose: (a) leave the warp as is (slicing not worth one fetch of four), (b) 8 slices of today's warp at 512^2 (16 MB a galaxy; softer wisps, 14% per point; stars reroll; D-212's map array becomes 3D), (c) the bulge read limit (B-024). The owner's uu.GalaxyTextures 0 test bounds what any texture saving can give.
Amendment (owner, 2026-11-09 23:23): (a) and (c): the warp stays, slicing is parked (B-025), the bulge read limit is D-218.

D-218 — The bulge's warp is read only where the bulge is (B-024; owner chose D-217's options a and c)
Question: Owner, 2026-11-09: of D-217's options, (a) keep the warp and park slicing (B-025), and (c) limit the bulge's warp_large read.
Decision: Uni_GalaxyTexTerms (the field's, the map's and placement's shared texture terms) reads warp_large inside Rb = min(0.5, 8 sqrt(r_e^2 + footprint^2)) instead of 0.5, fading over [0.6, 1] Rb instead of [0.3, 0.5]. The footprint widens Rb as it widens the bulge's profile (D-163), so a distant galaxy's widened bulge keeps its warp until the warp's own mip fade (D-179). Rb = 0.5 (an early type's r_e >= 0.0625, or a footprint >= 0.0625) is D-179's read to rounding. A spiral (r_e 0.04): Rb 0.32, the read covering 10% of the disc's area (was 25%). T2.galaxy.bulge_read against D-179's read rebuilt in the test: home bulge weight 0.6: 0.072% per point (light-weighted), p95 0.25%, light +0.001%; weight 1.5 (rolled maximum): 0.13%, p95 0.62%, light +0.006%; early type: 9e-7 at most.
Reroll impact: placement reads the map, whose bulge term moves with this: stars reroll only where an accept decision flips (density changes ~0.1% inside 0.32 to 0.5 field units). No seed or channel change.
Status: Provisional (UE-54: galaxies look unchanged; a small gain inside galaxies' inner discs)

D-219 — Slice sliding: CloudAtmosphere's march jitter and accumulating resolve measured, not adopted; the sliding is the slicing's reconstruction
Question: Owner, 2026-11-09: "substantial slice sliding in motion, may be worth looking at CloudAtmosphere for that" (and its cloud rendering); then "port our wins from cloud atmosphere that have relevance here".
Decision: Read CloudAtmosphere's render reference, AtmosphereTemporal.usf (MainResolveCS), its camera lattice (AtmospherePlan.ush) and draws (Atmo_TextureJitter). Ported to our march and reconstruct as a prototype, both sides (shader core + UniVolume.usf + the CPU reference), and measured with a new harness, T2.vol.slide: flights inside and outside the Milky Way, every frame against a fine reference (every step an eighth: MarchFlags bit 1, check-only), each frame's light-weighted error and its crawl (how much the error pattern changes frame to frame). Results (160 x 100 unless said):
- A full march every frame (step midpoints) is within 0.10-0.12% of the reference with 0.01-0.08% crawl: the march's sample shells do not slide. The sliding is the temporal slicing's reconstruction: inside the disc at 3 pc a frame 0.23% (crawl 0.17%); at 60 kpc, drifting 0.1 kpc a frame 3.9% (crawl 1.7%), 0.36 kpc a frame and turning 4.4% (crawl 4.3%); at 320-480 wide 1.5-2.7% with matching crawl.
- Per-step jitter (interleaved gradient noise, R2-moved lookups, golden-ratio shift a frame, a phase on each ray's first step) with the accumulating resolve (age, FreshWeight 0.15, fill share 0.1 -> 0.5 with motion, tent moments, gamma 4 -> 1): worse on every path (at rest 2.0% from noise against 0.10%; at 60 kpc 8.4% against 3.9%). Our content is deterministic and sharp: jitter only adds noise; the cloud's fill blending blurs arms 1-2 px wide.
- The resolve without jitter, a marched pixel taking its own sample (FreshWeight 1): worse (160: 5.2% / 4.9% against 3.9% / 4.4%; 480: 3.3% against 2.0%, crawl 4.6% against 2.9%). Its moment clamp alone changes nothing at these motions. Reprojecting at the exact per-pixel light depth (a full march's) changes nothing (2.73% -> 2.73%): the light depth estimate is not the error. A tighter D-194 clamp (TightPx 0.5 / 0.1) or dropping history above 0.5 px: worse. D-194's settings are a local optimum; what is left is the reconstruction's resampling of sharp bright structure.
- The one lever found: D-208's larger blocks at slow motion. At 60 kpc drifting 0.1 kpc a frame, N up to 4 gives 3.9% against 2 x 2's 1.67% (a pixel reprojected up to 15 times between its own samples). At the engine's march width the same drift is ~6x more march pixels, so N > 2 only engages much slower: whether this is the sliding the owner sees needs the engine (uu.VolumeInterleave 2).
Kept: T2.vol.slide (the harness; full march under 0.5% error and 0.2% crawl, slicing bounded at 6%) and MarchFlags bit 1 (the fine reference). The prototype (march jitter, ResolveAccumulate on both sides, an Age history) is archived in Tools/UniVerify/experiments/d219_jitter_accumulate.patch with its exploration test; nothing of it is in the engine.
Reroll impact: none.
Status: Measured; owner to localise in the engine (REVIEW_TONIGHT §53): uu.VolumeTemporal 0 (does the sliding go: it is the slicing), uu.VolumeInterleave 2 (does it go: D-208's slow-motion blocks), a description or capture of where and at what speed.

D-220 — The disc warp at a structural level; its fine band a bounded multiplier
Question: Owner, 2026-11-10 (with screenshots: arcs about the camera in a galaxy; rings moving out from the centre flying top-down): "a portion of the warp too fine to resolve in the available voxels, so when you move the camera it slides around... use a coarse warp at a structural level for macro scale organic variance, and use the finer level as an erode or composite rather than a warp term (a multiplier, e.g. 0.5 to 1.5, or an add)... importantly it will need to stay perceptually consistent."
Decision: Uni_GalaxyTexTerms (the field's, the map's and placement's shared texture terms) reads warp_fine for the disc's displacement at mip 2 at least (UNI_GAL_WARP_MIP_FLOOR); the footprint fade is as before. The fine band returns as Uni_GalaxyFineMul = clamp(1 + 15.2 x G.x x n, 0.5, 1.5) on the disc's light and dust (field, map evaluation, placement), n = warp_fine's A channel (an fBm, mean 0, std 1, unread before) at 3.3 x the warp's frequency, its height stretched 4 x (varying 4 x slower vertically than the warp frame), offset from the warp's read, faded with the footprint and the warp's inner fade (D-210). The gain follows each galaxy's rolled warp amount (the home galaxy's 0.015: 1 + 0.23 n). Constants in UniFields.ush; the same values as C++ globals in UniHlslShim.h (the galwarp tool varies them; T2.galaxy.warp_band checks they match).
Measured (univerify galwarp, Tools/UniVerify/galwarp_report.py; home galaxy, textures bound):
- Aliasing, the march against a fine reference of the same field (every step an eighth), flying top-down toward the centre and in the disc toward it (192 x 120): before error 1.62% / crawl 2.30% and 2.19% / 1.18%; warp at mip 1: 1.08 / 1.53, 1.44 / 0.79; mip 2: 0.80 / 1.13, 1.17 / 0.61; mip 3: 0.75 / 1.05, 1.19 / 0.60; no disc warp at all: 0.79 / 0.98, 1.13 / 0.56. Mip 2 removes the warp's share; what remains is the rest of the field.
- Look, the face-on structure (light over its azimuthal mean, annulus 0.1-0.7), octave band powers against before: mip 2 alone keeps 2-16 cycles a unit but 16-32 x0.47, 32-64 x0.15. Restoring them: warp_fine's fBm at the warp's own frequency adds coarse power (x1.2 at 2-16) and little fine; the detail fBm x1.5 / x2, its ridged channel as an erode, or the detail frame stretched in height: 16-32 at most x0.61 (the detail varies fast in height and averages out through the disc). warp_fine's fBm at 3.3 x the frequency: unstretched 1.08-1.21 x but the in-disc march 1.89% / 0.93%; stretched x2 (gain 0.28) 1.01-1.22 x, 1.59 / 0.84; stretched x4 (gain 0.23, adopted) 1.11-1.25 x, contrast 1.03 x, march 0.82 / 1.15 top-down and 1.45 / 0.78 in the disc.
- Adopted against before: top-down error -49% and crawl -50%; in the disc -34% / -34% (the fine band's in-plane detail along long horizontal steps is what is left there). T2.galaxy.warp_band (new): the constants match; band powers 8-128 within 0.8-1.4 x (1.08-1.22) and contrast within 10% (1.024); top-down crawl down at least 30% (2.22% -> 0.99%).
Cost: one more warp_fine read (the fine band) with the displacement's read at mip 2 (a quarter of the cache footprint); D-218 removed a warp_large read from most of the disc.
Reroll impact: every textured galaxy's density changes (the warp's fine octaves gone, the multiplier added): stars reroll in textured galaxies. No seed or channel change.
Status: Provisional (UE-55: the rings and arcs in motion; galaxies' look).

D-221 — Polish pass: shader parameter cross-check, record checks, engine review
Owner, 2026-11-10: away from home; polish, review and miscellaneous fixes (offline only).
- New gate check T4.static.shader_params (Tools/UniVerify/shader_params.py): for every global shader in UniShaders.h it flattens the C++ parameter struct (nested structs, macro-declared classes, `using FParameters`), preprocesses the .usf with the shader's defines (gcc cpp), keeps only what is reachable from the entry point through the call graph, and checks each parameter the code reads is declared with a matching HLSL type, and each declared one is read. Exceptions are listed with their reason (FUniGalMapBakeCS: the texture reads behind InTex = false compile out). Today 21 shaders, 0 problems. It catches the unbound or mistyped parameter that otherwise shows only as a UE shader compile error or a silent zero.
- New gate check T0.report.review_ids: every UE ID in a REVIEW_TONIGHT heading is open in unverified.json or marked withdrawn (the reverse was already checked). UE-38 (D-201) and UE-48 (D-211) are withdrawn: D-213 removed what they tested. D-201 and D-211 are marked superseded. REVIEW_TONIGHT §3 starts with the pending checks in test order.
- UniCore::StreamPhaseShortName replaces five `StreamPhaseName(...) + 11` prefix skips (uu.Perf, the HUD, the hitch log, two tests); T2.stream.phase_names checks every name carries the prefix and the short names are distinct. Text only.
- Engine-only code read through (the generation backend's submit, poll and timeout paths, RenderVolume, RenderBackdrop and its readback rings, auto start and auto fly, the sky light, uu.Perf): nothing wrong found. Three small edge cases noted in BACKLOG B-027 and left unchanged (untestable while the owner is away, and none seen).
Reroll impact: none.
Status: Done offline (the gate); the only engine-side change is the uu.Perf / HUD / hitch-log phase names (UniSubsystem.cpp), checked at the next build (REVIEW_TONIGHT §55).

D-222 — Paradigm review: assumptions audited, two prototypes proposed, galslices brought up to D-220
Owner, 2026-11-10: the 2D galaxy map came from breaking a design assumption; inspect the project and look for other paradigm shifts we may have missed. Written up in Docs/PARADIGMS.md (project doc claude/PARADIGMS.md).
Measured:
- explore.shells (archived, Tools/UniVerify/experiments/paradigm_shells_explore.cpp.txt): the solar camera at the defaults, each view marched to R and from R. Beyond 300 pc: toward the centre 99.8% of the light in 40.5 of 52.7 steps/px, along the disc 97.2% in 27.4 of 41.5, out of the plane 77.1% in 18.4 of 30.7. Within 300 pc 13 steps/px everywhere.
- galslices updated to D-220 (the rebuilt field applies the fine band and the warp's mip floor; it matches UniField_Galaxy to 7.4e-6, from 5.3). Sliced map: home 8 slices 3.9% per point (D-217: 13.7%), thick disc 25.5%. B-025 stays parked.
Proposed (owner to choose; nothing in the engine changed):
- Galaxies seen from outside as impostors drawn from their 2D map (B-022, raised: D-219's worst sliding is that case).
- The march split by distance, the far part in world-aligned direction maps refreshed on a parallax budget (B-028).
- Compute splatting of sub-pixel stars, after a ProfileGPU of the sprite pass (B-029).
Not recommended now, with reasons in PARADIGMS.md §4: work graphs (generation is 0.2 ms; plugin access in UE 5.7 unconfirmed), particle or Gaussian galaxies, stateless stars, a screen-space layered history (D-219: depth is not the error).
Reroll impact: none.
Status: Done (analysis). Prototypes wait for the owner's choice.

D-223 — Plain point sprites by a splat path (scatter + quads)
Owner, 2026-11-11: after PARADIGMS.md §3 and the follow-up on a unified pipeline: "lets give the scatter + quads approach a try".
Decision: a sprite that is the core alone (no halo, core weight 1, no resolved disc, round, at the core sigma, no warp: Uni_SpriteSplattable) is not drawn as a quad. The project pass sorts each visible sprite's slot into a quad list (draw arguments {6, n, 0, 0}) or a splat list (OutSprites, OutDrawArgs, pick and uu.Perf unchanged). After the volume pass:
- SplatCS: one thread per listed sprite (strided). The scene-depth test and the dust's transmittance at its centre pixel (the quad tests every pixel of a footprint a few pixels wide). Its luminance is added bilinearly into a 2x supersampled uint buffer, and its colour at the nearest 1x texel (fixed point 2^20 units per scene-colour unit, 2^26 cap per add: the floor 2e-4 is 210 units).
- SplatGatherCS: the 2x rows gathered to 1x columns by a Gaussian of sigma sqrt(core^2 - 1/24) (the 2x tent's variance taken out), normalised so each subtexel's light is handed out whole; colour rows blurred at the core sigma.
- SplatResolveVS/PS: the columns, then colour / its luminance x the luminance's shape, additive into scene colour. Light is kept off scene surfaces nearer than r0 (no sprite can be there), so the gather's tails do not touch a near body's edge.
Why this shape (measured, one star at 11 x 11 sub-pixel phases, against the quad's per-pixel Gaussian at a 1 px core):
- bilinear at 1x + blur: within 20% of the peak, its peak swinging 67% over phases (the quad's 28%: more twinkle in motion);
- Lagrange 3x3 at 1x: 15%; Catmull-Rom 4x4 at 1x: 5.6% but 36% swing;
- the exact 5x5 renormalised: 5%, but 75 atomics a star;
- 2x supersampled bilinear: 2.2%, swing 27.6% (adopted for luminance; 4x: 0.5%).
Full RGB on the 2x grid would be ~100 MB at 1880 x 1120. Luminance there and colour at 1x is 7 atomics a star and ~60 MB of transient buffers (RDG pooled), with an isolated star's colour exact.
Offline (T2.sprite.splat, new; UniSplatRef.h mirrors the passes with the shared core, and the engine quad's VS/PS for a round core):
- One star: core 1 px within 2.24% of the quad's peak, swing 27.6% vs 28.4%, light 1.0026x (the quad cuts its edge); 0.75 px 4.21%; 1.5 px 0.96%.
- The solar star field, three views: 98.9-99.9% of the sprites splatted, carrying 81-99% of the light (the rest are haloed stars). Light +0.24..+0.27%, per-pixel L1 1.33-1.51%, colour 0.01-0.07%.
- The buffers are bit-identical in reverse order; no overflow.
T4.static.shader_params: the four new shaders and the changed project and sprite passes bound, 25 shaders clean.
Engine: uu.Sprite.Splat (1; 0 is the quads as before). RDG scope Uni.Sprites.Splat (Deposit, Gather, Resolve). Not compiled here (DXC not runnable): UE-56.
Reroll impact: none (render only).
Status: Provisional (UE-56: shaders compile; the look against uu.Sprite.Splat 0; ProfileGPU in a dense core).

D-223 amendment (2026-11-11) — The splat path skips the screen away from its stars
Owner's engine profile of D-223: compiles, looks the same or slightly crisper. The quads cost 0.02-0.06 ms at today's density (in motion too, volume off); the splat path cost ~0.43 ms, almost all fixed: buffer clears 0.09, gather 0.22-0.26, resolve 0.10-0.13, deposit 0.006-0.011. Owner chose to keep it and cut the fixed cost (headroom for more stars).
Decision: tiles of 8 px (UNI_SPLAT_TILE). SplatCS marks the tiles it writes. SplatTilesCS gives each tile a level from the marks within two tiles: 2 (within one: gathered and resolved), 1 (within two: its gathered rows written as zeros, since a level-2 pixel's columns read them), 0 (skipped). The gather reaches at most UNI_SPLAT_TAPS + 0.75 px, so UNI_SPLAT_TAPS is 7, below a tile. The deposit buffers and marks persist between frames. SplatZeroCS zeroes the level-2 tiles after the gather and resets the marks, so the full-screen clears are gone. They are created cleared, and again when the view's size changes. SplatResolvePS returns at once outside level 2.
Offline (T2.sprite.splat, extended; UniSplatRef.h mirrors the passes): at 1920 x 1080 the tiled image equals the untiled one bit for bit (rows the gather skips hold NaN in the reference, so a read of one would show), and every buffer is zero after the zero pass. The solar star field's three views: 10-28% of the tiles gathered and resolved, 16-24% zero rows, 1-5% marked. Expected engine cost in views like the owner's: ~0.1-0.15 ms (from ~0.43).
Reroll impact: none. Status: Provisional (UE-56).

D-224 — Inside a galaxy the background stays on its direction map while travelling (the hitch at some angles)
Owner, 2026-11-11: travelling through the galaxy, some view and approach angles drop the frame rate. Mid-disc, below the plane looking up at the core ~20 degrees, orbiting: at some azimuths it drops, and moving in and out at speed it chugs. Captures: frames of 35-55 ms, all in one pass, Uni.Volume.March+Background, 31-51 ms. In the owner's static profile Uni.Volume.March alone was ~4 ms.
Measured:
- uu.Perf (default settings): the background was marched per pixel in 7909 of 39689 frames. Its map was re-marched 2044 times, 2035 because the camera moved. The auto budget was at its lowest level (373 x 245) with the GPU at 22.4 ms smoothed.
- With uu.BackgroundPerPixelInterval 2 for 31 s of the same flying: no per-pixel frame in 1635, the map re-marched every ~6 frames, GPU 4.4 ms smoothed.
- explore.hitch (archived, Tools/UniVerify/experiments/hitch_explore.cpp.txt): the owner's geometry, 8 and 11 kpc out, 2.9 and 4.0 kpc below the plane, looking at the core, every 15 degrees of azimuth. The local carriers alone take 45-50 steps/px; with the background per pixel 114-119 steps/px and 6.3-10.1x the time (web steps are far dearer than the galaxy's, which reads its 2D map). At azimuths 45-105 degrees some rays take 2-3x the steps of the others: max 206 vs ~100 locally, 470 vs ~170 with the background, the mean barely moving. A GPU wave runs as long as its longest ray, so this is a candidate for the angle dependence on top (B-030).
- The trigger: uu.BackgroundMode 2 marches the background per pixel when its map would be re-marched at least every Interval frames, with Interval = map rays / (march pixels / 4). The march's pixels were taken after the auto budget (D-200) had shrunk them. A slow GPU shrank the march, which raised Interval (6 -> 17 frames), which put the background per pixel, which slowed the GPU further: a feedback loop. Moving in and out at speed re-marks the map every few frames.
Decision:
- While the camera is inside a local carrier (UniCore::CameraInsideLocalCarrier: in a galaxy, or a cluster's or group's glow), the background is never marched per pixel. Its map refreshes (two faces a frame, cross-faded) as at rest. uu.BackgroundPerPixelInside 1 restores the old behaviour.
- The per-pixel interval is computed from the march's pixels before the auto budget (the travel scale only), so the auto budget no longer feeds into it.
- Outside any carrier (intergalactic travel, what D-166 made per-pixel background for) nothing changes.
T2.vol.bg_per_pixel_inside (new): the rule holds at the solar camera and at the owner's spot, and not at 60 kpc or in a void; per pixel at the owner's spot costs 8.7x the march.
Cost: the map lags the camera by up to a refresh (3 frames at two faces a frame, then a cross-fade). Inside a galaxy the web is the faint, distant part of the view.
Reroll impact: none (render only). Status: Provisional (UE-57).

D-225 — Supersampling scales with the light a carrier draws (the rest of the hitch)
Owner, 2026-11-11: the hitch at some angles remained after D-224. Capture: Uni.Volume.March alone ~16 ms with the auto budget at its smallest march (373 x 245, 5828 rays, 1/16 sliced): ~2.7 us a ray against ~0.25 us in a normal frame. uu.Perf: the background per pixel in 33 of 5489 frames (D-224 working). uu.VolumeMaxSubsamples 1 removed the hitch.
Measured (explore.hitch2, archived): from the owner's spot (8 kpc out, 20 degrees below the plane looking at the core) at azimuths 60-120 degrees, two distant clusters' glows (field 5, ~1 Mpc radius, 16-18 Mpc away, weight 0.03-0.05: the rest of their light still sprites) sit behind the core. Not prefiltered (D-160 is galaxies), each pixel they cover was supersampled 3x3 to 8x8 (Uni_CarrierSubsamples: their cores sub-pixel). 0.7% of the pixels at up to ~600 steps against ~100. On the GPU a wave runs as long as its longest ray, and with 1/16 slicing a wave spans ~1000 pixels. The auto budget's smaller march makes the cores smaller in pixels, so more rays: another feedback.
Decision: rays per axis x sqrt(saturate(Weight)) in Uni_CarrierSubsamples. The sampling error of a sub-pixel feature scales with the light it carries, so a carrier drawing a few percent of its light is not supersampled; one at full weight is supersampled exactly as before. (Rules tried: none below weight 0.25, x weight, x sqrt(weight). All three gave the same here.)
T2.vol.super_weight (new): at azimuths 60 and 90 (93 px wide, the smallest march scaled), against the old rule: supersampled pixels 0.73% -> 0, the longest ray 596-611 -> 101-102 steps, steps/px 51 -> 48. The image changes by at most 1.37% in a pixel, 0.0000% overall. At full weight 0 of 12 cases differ from the old rule.
Render only; the CPU reference and the shader share Uni_CarrierSubsamples.
Reroll impact: none. Status: Provisional (UE-58: no hitch at the owner's angles with uu.VolumeMaxSubsamples back at 8).

D-226 — Galaxy impostors from the 2D map: accuracy measured; build as cached billboards (proposal)
Owner, 2026-11-11: do PARADIGMS #1 (galaxies seen from outside as impostors) and #2 (the far cache).
Measured (explore.impostor, archived: Tools/UniVerify/experiments/impostor_explore.cpp.txt): the Milky Way from outside (40-120 kpc, azimuth 30 deg), 128 x 96, against today's march of it (its carrier alone).
- One map read per ray at the warped midplane crossing, the disc's vertical profile analytic, dust as the slab, the bulge 3D: face-on light +0.4%, L1 5.6%. The light falls with inclination: -3.4% at 50 deg, -6% at 65, -11% at 75 (dust off: the same). The disc's light varies across the ray's path through the slab (about 2 H tan i in the plane: +-0.05 radii at 75 deg), and its radial profile is convex: the crossing's value underestimates the path's mean.
- K map reads a side, each an equal share of the light's vertical profile at its own point on the ray (the vertical profile integrated analytically between them, light and dust apart):
  - K = 4: light within -0.6% to 65 deg, L1 2.7-3.8%;
  - K = 8: -0.1% at 50 deg (L1 2.5%, p50 0.7%), -0.55% at 65 (L1 2.6%), -1.4% at 75 (L1 4.0%, p90 23%). It breaks down past ~75 deg (82: -4.6%, p90 48%), where the march must take over.
- The face-on L1 of ~5.6% stays at any K: the march from outside samples the analytic field prefiltered over the footprint (D-160), the impostor the map's mips; the sharpest pixels differ (p99 ~17%, p50 0.8%). The bulge at its warp_large point (D-218) changes nothing.
- The march's own fine reference (MarchFlags bit 1) truncates at the step cap from outside (light 29-127% off), so it is not used here.
Proposal (owner to choose): about 20-25 evaluations a pixel per frame, unsliced, would cost 2-8x today's sliced march. So build the impostor as a cached billboard per galaxy seen from outside. It is drawn with the K-node method (or the march, unsliced) into a texture at the galaxy's screen size, and re-rendered when the view direction or distance to it moves past a budget. Each frame it is drawn by the exact 2D transform of the current camera, resampled once (no sliding). Past ~65 deg it hands back to the march, cross-faded. It is composited against the march with its transmittance through the volume's log-distance slices, as sprites are. Cost follows the refreshes, not the frames. PARADIGMS #2 (the far cache for the camera's own galaxy) uses the same refresh-on-a-budget machinery.
Reroll impact: none. Status: measured; proposal pending the owner.

D-227 — The march split by distance (PARADIGMS #2): measured, a quality/cost trade
Owner, 2026-11-11: #2 before #1.
Prototype (explore.farcache, archived: Tools/UniVerify/experiments/farcache_explore.cpp.txt), in the D-219 harness (the solar camera, GalaxyToStar, 160 x 100, against the fine reference every frame):
- The near part (carriers clipped to R) is marched every frame with today's slicing.
- The far part (TStart raised to R) is marched into a world-aligned 160^2-a-face cube from an anchor, re-marched when the camera has moved half a texel's parallax at R.
- Composite: B_near + T_near B_far(dir).
- Steps per pixel a frame: near rays x steps, plus the in-view share of each refresh.
Results (error / crawl / steps per px a frame):
- 0.3 pc a frame: today 0.12% / 0.036% / 12.7; from 1 kpc 0.32% / 0.024% / 4.2 (no refresh in 10 frames); from 300 pc 0.30% / 0.029% / 11.0.
- 3 pc a frame: today 0.23% / 0.17% / 12.7; from 1 kpc 0.30% / 0.11% / 19.8 (5 refreshes in 10); from 300 pc 0.30% / 0.04% / 42.2 (every frame).
- 15 pc a frame: today 1.05% / 0.91% / 12.8 (the slicing slides); from 1 kpc 0.30% / 0.085% / 35.5; from 300 pc 0.30% / 0.013% / 42.3.
Reading:
- Slow, the far cache is ~3x cheaper with the same crawl. Its ~0.3% floor is the cube's resampling (one texel a pixel): a finer cube lowers it.
- At speed, the far content's parallax is real motion. The cube must be re-marched every frame (2.8-3.3x today's steps), and a partial, rolling refresh would leave texels many texels out of place. In exchange the crawl falls 10-70x (0.9% -> 0.01-0.09%).
- Inside the disc at modest speed today's slicing is already good (D-219).
Options for the owner:
- (a) The far cache only while slow: a saving, with today's quality.
- (b) The far cache at speed too: no sliding, ~3x the march.
- (c) Both, switched by speed.
- (d) Leave it, and do #1 (D-226's billboards), where the outside-galaxy sliding is.
Reroll impact: none. Status: measured; owner to choose.

D-228 — The far cache, switched by speed (PARADIGMS #2, B-028): built
Owner, 2026-11-11: "Switch by speed (Recommended)".
Costed again first (explore.farcost, archived: Tools/UniVerify/experiments/farcost_explore.cpp.txt). D-227 had no refresh in its window, and its cube was as coarse as its view. Here the cube matches the march's angular resolution (192 x 108, N 192, 40 frames). Steps per pixel a frame against today's slicing, faces refreshed per face, in view only:
- R 1 kpc: 0.32x at rest; 0.63x at 0.3 pc/frame; 1.24x at 1.
- R 2 kpc: 0.38x; 0.52x; 0.92x; 1.74x at 3.
- R 4 kpc: 0.46x; 0.46x; 0.68x; 1.01x at 3.
- Refreshing the whole cube costs more than today once any refresh happens (2.42x at 1 pc/frame, R 1 kpc).
- A face's re-march costs ~40 steps per view pixel, about 3x a frame's march. Break-even is ~0.06 texels of parallax at R a frame, for any R. At the engine's default march (~680 px wide, 90 degrees) and R 2 kpc that is roughly 15-20 pc/s at 60 fps.
- Error 0.16-0.25% (today 0.10-0.20%); crawl at or under today's.
Built:
- UniCore/UniFarCache.h (engine-agnostic): FFarCacheControl (the rules), CubeFacesInView, FFarSceneKey.
  - On below uu.VolumeFarOnMotion (0.02 texels of parallax at R a frame) held uu.VolumeFarOnFrames (30) frames, with every face marched and the faces in view fresh. Off above uu.VolumeFarOffMotion (0.04), and while outside a local carrier, while cells load (their glow is per frame), with the background per pixel, or in a debug view.
  - Per-face anchors. A face in view is re-marched past uu.VolumeFarDueTexels (0.3) of parallax since its anchor; a face out of view past 1.2. The fill takes the scene's snapshot and marches uu.VolumeFarFillFrames (8) frames' rows into the face's spare layer, then swaps it in. No frame marches more than a face's share: no spike.
  - A change of more than 1% in the local carriers, their LUT or parameter blocks, or skipped game frames: every face re-marched, in view first, the map still used meanwhile. A jump: everything invalid.
- Shaders:
  - UniVolume.ush: Uni_FarComposite, Uni_FarHistoryConvert, Uni_FarSliceT.
  - UniVolume.usf:
    - FarCS fills the map: 12 layers, (rgb, luminance T), Float32; 12 x 8 slice layers.
    - MarchCS: past R the slices take the map's transmittance (a product).
    - ReconstructCS: at a switch the history is converted once (HistFarSign: whole <-> near part).
    - CompositePS: near + T_near far, then the background through both.
- Renderer: the march's local carriers stop at R (TEnd) while on. uu.VolumeFar (1), uu.VolumeFarStartPc (2000), uu.VolumeFarSize (0: as fine as the march at the view's centre, in 64s). ProfileGPU scopes: Uni.Volume.FarMap, Uni.Volume.March (near: far cache on). uu.Perf has a far-cache line.
- Found on the way: the march records a slice after the step that crosses it, so the far march's first step (from R) stamped every boundary nearer than R. The map's slices before R are now exactly 1 (Uni_FarSliceT); the sprites' slices went from 2.3% to 0.02% off at R 2 kpc.
Verified offline:
- T1.vol.far_controller: the rules on synthetic frames.
- T2.vol.far_split: still camera, near + far = the whole march. Light 0.28-0.29%, transmittance 0.0006, slices 0.0002-0.0011 mean abs, at R 1 and 2 kpc.
- T2.vol.far_flight: the solar camera drifting, travelling, drifting again with the renderer's rules.
  - On 77 of 122 frames at 0.46x today's steps (march plus fills); within 0.32% of the whole march.
  - Three switches, the light changing 0.05% at them (0.06% on other frames). Without the history conversion: 55%.
  - At most a face's share of rows a frame; the scene key steady.
Not verified: the engine side (UE-59).
Reroll impact: none (render only).
Status: built; engine test pending (UE-59).

D-229 — Galaxy detail in dense clusters: a quick integral for the volume; the galaxy map at 512^2
Owner, 2026-11-11: galaxies should load by perceived scale; flying into dense clusters the pop-in is "galaxy detail late"; try a 512^2 galaxy map.
Measured (explore.integrals, explore.mapsize; archived in Tools/UniVerify/experiments/):
- A galaxy becomes a volume carrier only once its normalising integral is known (D-140). The exact one is 32^3 x 128 probes, ~2.2M field samples (275 ms here, ~0.2 s in the engine), on a worker, four at a time, first come.
- Flying into a rich cluster (M -26) at 0.05 Mpc a frame, up to 239 galaxies are in the sprite-to-volume handoff at once.
  - Their on-screen area waiting as plain sprites: 89% first come, 81% largest first.
  - The longest wait of a galaxy past twice the handoff: ~40 frames in the simulation, 107 in the streamer with 12-frame jobs.
- Carriers are already ranked by angular size (D-161), so the queue was the cause, not the cap.
- Coarse grids are poor estimators (thin discs fall between samples): 16^3 x 32 is 2.2% mean, 12.5% worst.
- Importance sampling (a third uniform in the ball, a third in the thin disc with z Laplace at the field's scale height, a third toward the centre) with 4096 samples: 2.0% mean, 5.6% worst, 0.8 ms (357x faster). The exact itself is 0.24% from a 2M-sample reference.
Built (UniCore; no engine-specific code):
- FRefGenerator::ComputeIntegralQuick (galaxy fields only, deterministic per contributor).
- FUniStreamer::VolumeIntegral: the exact integral once known; until then the quick one, computed in batches of 32 on the job pool (largest on screen first, two batches at once; RunQuickJob, default RunJob); after a quick one was used, eased to the exact over 30 frames (log space, smoothstep).
- FVolumeSource::VolumeIntegral: the carrier build uses it (it was IntegralReady + the exact integral).
- Generation never reads the quick value: its cells still wait for the exact one. Star counts unchanged.
- uu.Perf: a volume integrals line.
Galaxy map: uu.GalaxyMap.Size 1024 -> 512, uu.GalaxyMap.Slots 4 -> 8.
- 512^2 is within 0.3% in light of 1024^2 in nine views (inside, above, edge-on). Per-pixel p95 is up to 1.7% within ~0.5 kpc of a core (1024^2: 0.8%), elsewhere the same.
- Bake 41 vs 133 ms (CPU), 2.7 vs 10.7 MB with mips.
- Placement from it: star count 1.0006x the field's, map light 1.11% light-weighted (the check allows 3%).
- Eight slots at 512^2 are half four at 1024^2's memory: twice the galaxies with maps at once.
Verified offline:
- T2.stream.quick_integral: mean 2.03%, worst 5.60%, 357x faster, deterministic.
- T2.stream.quick_flyin: the streamer with delayed jobs, flying into the rich cluster.
  - Area left as sprites 90.2% -> 3.8%; longest wait 107 -> 0 frames.
  - Largest amplitude step a frame 0.53%; at rest every carrier on the exact value.
- T2.vol.galaxy_map and T2.gen.map_placement now at 512^2.
Reroll impact:
- The integrals: none (render only).
- The map size: stars placed in galaxies change where an accept decision flips on the map's value (placement reads the map, D-214). Seeds are unchanged.
Status: built; engine test UE-60.

D-230 — The travel look: the motion web and the travel shift
Owner, 2026-11-11: intergalactic travel is mostly black with a few galaxies drifting by (galaxies are sparse, the volume subtle). Proposed: a speed-ramped view of the web's density, faded to zero before any edge, under the excuse of red/blue shift; distant galaxy and cluster sprites in the same shift system. Render only, art choices.
Built:
- The travel curve (Warp.Current: 0 at 100 kpc/s, 1 at 10 Mpc/s) ramps both, smoothstep from uu.TravelLook.Start (0.15, ~200 kpc/s) to uu.TravelLook.Full (0.75, ~3 Mpc/s). UniCore::TravelLookRamp.
- The motion web (Uni_MotionWebEmission, in the march's textured-web branch, once a step though two web carriers read the texel):
  - Emission from the web texture's filament + node channels over uu.MotionWeb.Threshold (3) x their mean, log over two decades.
  - Faded in over uu.MotionWeb.Near (4 Mpc: nearer structure is soft blobs, the texture's texels being 0.78 Mpc, 200 Mpc / 256 (wording corrected in D-232's commit: it said cells of ~1.6 Mpc)), out over the second half of uu.MotionWeb.Depth (50 Mpc, inside the 100 Mpc march and the 200 Mpc tile: no edge).
  - A cool white the shift colours. Strength uu.MotionWeb (0.01) in a galaxy disc's surface brightness (~7e4 per sr) per Mpc.
  - FVolumeViewGPU grows by one row (112 -> 128 bytes): MotionWebGain, Near, Depth, InvRef (the threshold folded into the mean).
- The travel shift (Uni_TravelShift, UniPhotometry.ush): Uni_DopplerTint by exp(uu.Shift x cos(angle to the motion)); ahead blue, behind red, luminance kept (hue only).
  - Applied to the volume's output (DopplerOf: galaxies, the web, the background map, the far map) and to every sprite in projection (splats included).
  - uu.Shift 0.5 (x1.65 ahead at full travel), uu.Shift.Colour 1.0.
  - Independent of the warp optics (uu.Warp, still off): its own parameters (FUniView ShiftVel/ShiftAmount/ShiftK; the volume's ShiftVelAmount/ShiftK); the direction from the camera's step (UniCore::ShiftMotion, in double).
- Previews (explore.travellook, archived): the flythrough universe with the delivered web from the void camera, at exposure gain 1 (the engine's near-black sky).
  - Motion web 0.002 / 0.006 / 0.02: barely / subtly / clearly visible.
  - Threshold 2 shows walls and voids, 5-8 only clusters.
  - Shift 0.5 / colour 1 reads; 1.0 / 1.5 is saturated.
  - A 90-frame flight at 10 Mpc/s went to the owner.
- Found on the way: the march's Du is the step in the carrier's field units (for the web, fractions of its 200 Mpc tile); the emission is per cm, so it is multiplied by the step in cm.
Verified offline:
- T1.look.travel_shift: blue/red x2.7 ahead and x0.37 behind at 0.5, the side unchanged, luminance kept; the ramp; ShiftMotion's edge cases.
- T2.look.motion_web:
  - Off is bit for bit today's march; the added light is exactly linear in the gain.
  - With two web carriers it is exactly one carrier's (without the once-a-step guard: 2.000, caught).
  - Nothing from within 2 Mpc; less at depth 25.
  - The shift keeps every pixel's luminance (4e-7).
Not verified: the engine side (UE-61).
Reroll impact: none (render only).
Status: built; defaults chosen from the previews; engine test UE-61.

D-231 — The motion web: organic detail, and an earlier, wider speed ramp
Owner, 2026-11-11: "that actually looks quite nice". Add a single noise texture read to warp or multiply the web for a more organic feel. The web should show at a substantially lower speed and reach full over a larger range: seen while crossing a galaxy's worth of distance in 1-2 seconds.
Built:
- Detail: one galaxy_detail read (its R channel: ridged multifractal filaments, mean 0.567) multiplies the motion web.
  - Read at the web carrier's point, tiled a whole number of times per 200 Mpc web tile (uu.MotionWeb.DetailMpc 20: ten tiles), so it stays periodic.
  - Mip from the pixel footprint, so distant detail is averaged, never aliased.
  - The pattern is (R / mean)^3, blended in by uu.MotionWeb.Detail (0.8).
  - Multiply rather than warp: a warp would need a second read of the web texture itself.
  - Off (0) or without the galaxy textures: D-230's web exactly.
  - FVolumeViewGPU 128 -> 144 bytes (MotionWebDetailCm, MotionWebDetailGain, two pads).
- The ramp: the motion web now has its own, from the camera's speed (the travel curve is 0 below 100 kpc/s). UniCore::FMotionWebRamp:
  - Ease-out on log speed from uu.MotionWeb.StartKpcS (5) to uu.MotionWeb.FullKpcS (5000).
  - Eased in time (0.3 s up, 0.6 s down) so speed jitter cannot flicker it.
  - 0.36 at 20 kpc/s and 0.56 at 50 kpc/s (a 30 kpc galaxy in 1.5 s and 0.6 s); 0.95 at 1 Mpc/s.
  - The travel shift keeps the travel-curve ramp (uu.TravelLook.*).
- Previews (explore.travellook, D-231 version archived): a 20 Mpc period at 0.8 reads as wisps and tendrils over D-230's soft blobs; 10 Mpc is finer (cobweb); 40 adds little. A flight went to the owner.
Verified offline:
- T1.look.web_ramp: off at 2 kpc/s, 0.36 at 20, 0.56 at 50, 0.95 at 1 Mpc/s, 1 at 10. Monotone; 96% of a step within 1 s, no overshoot; +-50% frame jitter moves it 0.5% a frame.
- T2.look.motion_web (extended): detail 0.8 keeps the added light within x1.16 and raises its pixel-to-pixel contrast x1.4 at 128 x 72 (prefiltered); off is D-230's exactly.
Amendment (owner, the same day: "your demo video has texture march sliding"). Measured (explore.webslide, archived):
- Method: flying at 10 Mpc/s, the frame-to-frame change of the march's image against a fine reference's (steps an eighth). Real motion is in both, swimming only in the march (D-219's crawl confounds the two at speed).
- Physical web 0.17% crawl. Plain motion web: change 0.25% of the light above the reference's (6%). With the detail: 2.27% above (37%).
- The detail's ridges (0.6-5 Mpc) are finer than the background's steps (~0.1 t, 0.5-5 Mpc). The steps sit at fixed distances from the camera, so the ridges swam through them.
- Prefiltering the detail over the step as well as the pixel removed the swim (0.32%) but most of the detail. Finer steps everywhere barely helped (ratio 0.05, 157 steps: 1.44%).
Fixed:
- Within the motion web's band (half its near distance to its depth), with the detail on, the step is capped at uu.MotionWeb.StepMpc (0.5) and the detail blurred over it.
  - Excess 0.03-0.05% (below the plain web's), the detail kept (uu.MotionWeb.Detail now 1.0).
  - The background march's steps ~68 -> ~133 a ray while it is on. uu.MotionWeb.StepMpc 0: no cap, free, the detail then blurs away.
- FVolumeViewGPU's pad becomes MotionWebStepCm.
- MarchFlags bit 2 (checks only): the fine reference blurs the detail over the march's own step.
- T2.look.web_swim: excess under 0.3% (0.05%).
Not verified: the engine side (UE-62).
Reroll impact: none (render only).
Status: built; engine test UE-62.

D-232 — Early types' spheroids: noise modulation, each its own mix
Owner, 2026-11-11 (with a screenshot of a smooth elliptical): "globular galaxies are currently a simple falloff - they should also get noise based modulation so that they get some organic variance... should randomly pick channel weights to apply from a packed noise probably so that we have variance between globs."
Found: early types (D-172) rolled no texture parameters (UseGalaxyTextures added "Tex.*" ranges to the late-type ranges only) and all shared shape seed 0, so even a texture read would have been the same pattern at the same offset in every elliptical.
Built:
- The spheroid's multiplier, in the shared texture terms (Uni_GalaxyTexTerms: field, galaxy map and placement alike, one field source): one galaxy_detail read at the bulge's (warp_large-displaced) point, its height divided by the axis ratio so the structure follows the isophotes, at G.w periods a field unit.
  - Mix (Uni_GalaxySpheroidMul): each channel standardised (galaxy_detail.json's mean and std), weighted by w = q^2 (q 6 bits of the galaxy's hash, so one or two channels usually lead) with a random sign each (ridges as filaments or lanes, Worley cells as clumps or holes), normalised to unit variance (the channels are uncorrelated: |r| < 0.035); 1 + amount x that, clamped to [0.3, 1.8].
  - The hash: a new per-galaxy constant, Hn = SeedMix(Hs, 12) on the shape seed's DISCMODE stream (a new sub-index; 0, 1, 8-11 were in use), carried in the march's constant block (D.z, D.w were free).
  - Faded with the footprint as the other reads are (mip 4 to 5), and inside the core: none within 0.2 r_e, all by r_e (the core stays smooth and bright).
  - Mip floor 3 (UNI_GAL_SPH_MIP_FLOOR; knob GKnobSphMipFloor): see the measurement below.
- Parameters (early types only, textured scenes only; new names, so new streams):
  - EarlyTex.ShapeSeed (V[22]): early types now have their own seed (texture offsets and the weights' hash).
  - EarlyTex.Noise (V[13] = D.y, 0.25-0.6): the amount. D.y was unused on emission parameter sets; on a dust set it is sigma, and a dust set has no bulge, so Uni_GalaxySpheroidNoise reads it only with B.y > 0.
  - EarlyTex.NoiseFreq (V[27] = G.w, 1.0-2.5 periods a field unit).
  - EarlyTex.BulgeWarp (V[25] = G.y, 0.004-0.02) and EarlyTex.WarpFreq (V[26] = G.z, 0.8-1.6): warp_large's existing bend of the spheroid, now on for early types too.
  - TexN (V[31]) set on the early exemplar.
  - Their discs (lenticulars) stay smooth: no disc warp, noise or knots.
- Measured (experiments/ellipnoise_explore_d232): the march's slide across step phases, close views (pixel 0.002 of the radius), 20 x 20 px: smooth spheroid 0.013%; textured spirals 0.09-0.13%; ellipticals by mip floor 0 / 1 / 2 / 3: 0.24-1.18% / 0.21-0.82% / 0.14-0.45% / 0.06-0.21%. The finest octaves are finer than the spheroid's steps (0.3 of the profile's log-slope length; 0.25 beyond 6 r_e). Floor 3 keeps lumps of about r_e (the 4- and part of the 8-cycle octave), which also reads better than grain on a spheroid (contact sheets at 1, 2, 3).
Verified offline (T2.galaxy.spheroid_noise, new):
- The shaders' floor equals the C++ knob; 256 rolls in range, 256 distinct shape seeds; late types, dwarfs, dust sets, the home galaxy and untextured scenes have none; the constant block round-trips Hn.
- The core inside 0.2 r_e (warped radius) is the smooth profile exactly (1762 points).
- 8 ellipticals: light within 3.9% (a low-frequency noise over one spheroid does not average to its mean; the normalising integral keeps the star count); face-on residual RMS 4.6% minimum, 10.2% mean in 1-4 r_e; mean |correlation| between galaxies' residuals 0.10.
- The march: slide 0.12% (smooth 0.013%), error against fine steps 0.052% (smooth 0.044%).
- Contact sheet still_galaxy_ellipticals (with and without).
Not verified: the engine side (UE-63).
Reroll impact: textured scenes' early types (cluster members drawn as such) change shape and light distribution; their stars move. Nothing else changes (spirals, dwarfs, the home galaxy, untextured scenes: bit for bit).
Status: built; engine test UE-63.

D-233 — The motion web's detail by region
Owner, 2026-11-11: "it would be nice for the universe web noise to get balanced in different ways based on regions so that different areas of the web have different noise modulations."
Built:
- The motion web's detail texel (D-231's one galaxy_detail read) now uses all four channels, mixed by region:
  - Each channel's pattern is (value / mean)^p with p set so every channel has the ridged one's contrast (p cv = 3 x 0.313: R 3, G 4.62, B 3.80, A 3.38), scaled to the ridged one's mean light (E[(R/m)^3] = 1.285; x 1, 0.950, 0.966, 0.978). Filaments (R), knots (G), cloud (B), cells (A).
  - The weights: a second read of galaxy_detail at the regions' scale (uu.MotionWeb.RegionMpc 200: one region period a 200 Mpc web tile, so it stays periodic; regions ~50 Mpc across) at mip 4 (smooth), each channel standardised at that mip and exponentiated (x 2; x 1 / 1.5 / 3 measured on the texels: median leading share 0.49 / 0.59 / 0.79).
  - The ridged-only weights give D-231's (R/m)^3 exactly; uu.MotionWeb.RegionMpc 0 is D-231's path.
- FVolumeViewGPU's last pad becomes MotionWebRegionCm (still 144 bytes).
- Cost: one more texture read per background step while the motion web is on (in its band only).
Verified offline:
- T2.look.web_regions (new): weights sum to 1 (2e-7); each channel leads in 22-28% of a tile; median leading share 0.54 (interpolated between texels); each channel alone has the ridged one's light within 1.1% and contrast within 16%; ridged weights = R^3 bit for bit; the frame's added light x0.98 of the ridged detail's, its per-pixel change 27% of it; slice still_motion_web_regions (regions vs ridged everywhere).
- T2.look.web_swim now runs with the regions on: excess 0.03% (under 0.3%).
Not verified: the engine side (UE-64).
Reroll impact: none (render only).
Status: built; engine test UE-64.

D-234 — Nebulae: the outside view (emission, reflection, dark clouds)
Owner, 2026-11-12 (option 3 of three): nebulae seen from outside first, the field built so a fly-into layer can grow from it later. Constraint (B-031): a field, no explicit entities (keep the cost flat).
Built:
- Emission nebulae (H II regions), in the shared galaxy core (Uni_GalaxyFromSharedCN; the field, the galaxy map's path Uni_GalaxyMapEvalN and the march; one field source):
  - Density in the field's own units from terms the light is already made of (no texture read of its own): the young light (the arms' knot-free share, or the disc's for a galaxy without arms) x a gas layer half the stars' scale height (as the dust; Uni_GalaxyNebulaVert, footprint-widened keeping columns) x the knot shape x the galaxy's knot amount H.y (star formation: no knots, no nebulae; early types, dust sets and knot-less galaxies have none).
  - Knot shape: galaxy_detail's knot term over 0.3 (about 11% of texels), full by 0.7, squared, x 60 (UNI_NEBULA_SCALE: uu.Nebula 1 is the look).
  - Receding: a threshold of a filtered noise loses its peaks (measured: the share fell 8x by mip 3.4, to 0 by 4.5). The knot term is stretched about its mean by a factor measured per half mip (0-5) that keeps the shape's mean, so the light holds: within 20% typically and 53% at worst at 6-12x the footprint (T2.galaxy.nebulae). Trilinear sampling lowers the variance faster than the mips' own texels (mip 3: std 0.099 sampled, 0.146 texels), so the table is measured from samples (experiments/nebula_knotstretch_d234).
  - Light: plain light, not a population's (no sprite counterpart; D-128's v2 row): the march adds it at the galaxy's full light per star, stored by the LUT builder in its emission block's second entry's w (Uni_VolumeLutFull, horizon-dimmed), so the nebulae stay as the stars resolve. Colour: H II pink (luminance 1), or the galaxy palette's knot colour where it has one.
- Reflection nebulae (march only): the dust lit by the young clusters, its density x the knots' share of the sample's light (Comp.w / sum), blue, x uu.Nebula.Reflect.
- Dark clouds: the textured dust x max(1 + 1.2 x (knot - its mean 0.0457), 0) (FVolumeProfile::DustClumps, carried in the dust set's G.y, which no bulge reads on a dust set; read in Uni_GalaxyDetailTerms with the lanes and holes). Linear about the mean, so the dust's mean is kept (T2.galaxy.dust: D-180's column within 25%; first built about 0, the mean rose 5% and that check failed at 29.5%). The dust's arms sit inward of the light's, so the clouds fall on the arms' inner flank beside the pink knots.
- Near fade (render only): the nebulae fade in from uu.Nebula.NearKpc (2) to 3x: up close they are galaxy-scale blobs until the fly-into layer exists.
- FVolumeViewGPU 160 bytes (NebulaGain, NebulaReflect, NebulaNearCm, pad); CVars uu.Nebula (1), uu.Nebula.Reflect (6), uu.Nebula.NearKpc (2).
- Measured: nine rolled spirals' nebulae carry 3.9-17% (mean 9%) of the stars' light face-on, the brightest tenth of their pixels 96%+ of it; the Milky Way +17.3% from 60 kpc, +5.4% from the solar camera (near fade on).
Verified offline:
- T2.galaxy.nebulae (new): never negative, zero without knots and outside the cut (19,200 points); shares, knottiness and receding as above; dark clouds keep the dust's mean within 5%; the LUT's full light equals the unresolved light where none is resolved (4e-8) and is never below it; the Milky Way's march adds light within bounds, and with the near fade at infinity is the frame without, bit for bit (the off path is clean). Stills: still_galaxy_nebulae, still_nebula_milkyway.
- T2.vol.galaxy_map_nebulae (new): the camera galaxy's map path against its field with nebulae, knots and cloud dust, three views at 512^2: light within 0.03%, p95 1.2%, the nebulae's added light within 0.7%.
Not verified: the engine side (UE-65).
Reroll impact: none for content (render only). The dark clouds change textured galaxies' dust (render only).
Status: built; engine test UE-65. Follow-ups: B-034 (the fly-into layer, young stars in nebulae, the sprite's share, offset clouds).

D-235 — Nebulae: no slide, fixed positions, seen up close, two colours a galaxy
Owner, 2026-11-12, after D-234 in the engine: "substantial raymarch sliding in the nebula field"; "they also change location when the view swaps after being stationary"; "they seem to disappear up close"; "they should get their colors randomized along with the rest of the galaxy palette... a couple nebula colors per galaxy".
Causes:
- D-234 thresholded the knot term read at the footprint's mip, stretched by mip. The footprint changes as the camera moves and when the march's resolution changes (the view swap at rest), so the pattern itself changed: it morphed, and nebulae appeared elsewhere.
- The march's steps (0.32 x distance, hints x 3 inside a galaxy) are far longer than a nebula: measured slide 48% of the nebulae's light inside a spiral (the stars 2-3%, the gas layer without knots 4-5%).
- The near fade (2 to 6 kpc) hid them up close.
Built:
- One fixed-mip read for the nebulae (UNI_NEBULA_MIP 1, the same point as the detail read; Uni_GalaxyTexTerms' OutNebK: knot term and fBm), so their pattern never depends on the view; stretched by 1.0694 (mip 1's measured factor) so the light is D-234's. The field returns the nebulae's terms (source, knot, mix: Uni_GalaxyFromSharedCN / Uni_GalaxyMapEvalN, float4); Uni_GalaxyNebulaDensity makes the density.
- Prefilter by blending to the mean, not by changing the pattern (Uni_GalaxyNebulaBlend): from 3 read texels per pixel (a knot's width) and from a quarter texel per half step along the disc plane (features the steps skip would slide), each full at twice. The light is kept and the pattern fades rather than moves.
- The gas layer's step cap (Uni_GalaxyNebulaStepCap, in Uni_CarrierStepLimit, not loosened by HintScale): quarter-texel steps along the plane inside +-4 gas scale heights, where the ray's path through the layer takes at most 48 of them (a ray crossing the disc: the outside view keeps its knots); none along the plane (inside a galaxy, the far nebulae blend to a smooth glow instead). Only with uu.Nebula on, only for galaxies with knots.
- The near fade off by default (uu.Nebula.NearKpc 0).
- Colours (Uni_GalaxyNebulaColour): two a galaxy from its palette seed (F.w) or, without a palette, the shape seed's hash (SeedMix 13, a new sub-index), on the palette channel's draws 12..17: H-alpha pink-red 55%, magenta-violet 20%, [O III] teal 15%, [S II] orange-red 10%, saturation 0.55-0.85, luminance 1; mixed nebula to nebula by the fixed-mip read's fBm channel, toward their average where blended. Replaces D-234's fixed pink and the palette's knot colour.
Measured (T2.galaxy.nebula_slide, new; experiments/nebula_slide_d235): inside a textured spiral as the engine steps, 160 rays: slide 6.0% (the gas layer without knots 5.5%, the stars 2.8%); from 60 kpc at 45 degrees: 0.9% (D-234: 3.3%). Sweep: mip 1-2 x blend 0.25-2 texels x cap 0.25-1 texel.
Verified offline: T2.galaxy.nebula_slide; T2.galaxy.nebulae (the pattern identical at two footprints; receding within 20%, was 53%; the near fade at infinity within 0.03% of the frame without nebulae, the step cap aside); T2.vol.galaxy_map_nebulae.
Known cost: the gas layer's cap adds up to 48 steps to rays crossing a large-on-screen galaxy's disc while uu.Nebula is on (ProfileGPU in UE-66).
Known limit: inside a galaxy, nebulae farther than about 1 kpc along the plane are a smooth glow (their structure cannot be sampled at the march's steps without sliding); the fly-into layer (B-034) is where near structure grows.
Reroll impact: none (render only).
Second engine test (owner, with screenshots): a nebula cut off along a stepped curve ("straddling bands"). The gas layer's cap only took effect at a step starting inside +-4 gas scale heights of z = 0, so (1) where the last uncapped step landed in the layer moved from pixel to pixel: its sample stood for a long step and blended to the mean, the next were capped and showed the pattern (a hard, stepped edge), and (2) the disc's warp lifts the layer off z = 0 (at the Sun's radius the Milky Way's sits ~0.016 field units up), so part of it was never capped. Fixed: the cap follows the warp and flare (the disc's own D-178 terms, the mode phases' hash passed in), a ray heading into the layer stops at its surface, and the cap lengthens smoothly with the ray's angle up to 4x (the blend is full there) instead of switching off at a threshold. T2.galaxy.nebula_edges (new): from 1 kpc above the Milky Way's disc looking down 60 degrees, where the cap samples at its finest, the engine stepping against fine steps: p95 error 9.5% (the old cap: 77%). Also: the cube passes (the far cache's FarCS, BackgroundCS) set NebulaThetaPixel to the screen's, so the nebulae blend at the screen's footprint there too, not their coarser texels' (FUniVolumeView's last pad; T2.vol.far_split_nebulae, new: the nebulae's light across the far split within 5% with the map at 1/2 and 1/4 of the view's resolution; in that scene the far side was already blended by its steps, so this alone did not show the cut).
First engine build: 'use of undeclared identifier UNI_NEBULA_MIP' (FUniGalMapBakeCS): the nebula macros' defaults were defined after Uni_GalaxyTexTerms, which reads at UNI_NEBULA_MIP; the C++ shim defines its knobs before including the cores, so the harness compiled. Moved to the top of UniFields.ush (Uni_GalaxyNebulaBlend defined before its use too), and a new static check, T4.static.shader_decl_order (shader_decl_order.py: each .usf with its includes in the compiler's order, every macro, static const, UNI_FN function and struct declared before its first use; it reports the broken version's 5 shaders).
Status: built; engine test UE-66 (UE-65 superseded: its look changed).


D-236 — Nebula colour variety; ellipticals: nebula filaments and tidal shells
Owner, 2026-11-12, after D-235 in the engine ("ok thats much closer"): "more variance in possible nebula colors and a pass to make the globular galaxies more interesting - they could also get nebulas, potentially different colors, etc".
Built:
- Nebula colours (Uni_GalaxyNebulaColour, Uni_NebulaHue): three a galaxy (D-235: two), on the palette channel's draws 12..20 (new draws; 12..17 change meaning, render only), each from a family table, saturation 0.4-0.95 (was 0.55-0.85), luminance 1; nebula to nebula the colour runs from the first through the second to the third by the mix term, toward their average where blended.
  - Spirals (H II regions): H-alpha red 30%, pink 18%, magenta-violet 14%, blue 8%, [O III] teal 14%, green 4%, [S II]/[N II] orange-gold 12%.
  - Early types (cooling-flow and shock-lit filaments): H-alpha red 35%, orange-gold 22%, teal 13%, violet-blue 18%, magenta 12%.
  - Each nebula its own brightness: x clamp(1 + 2 (A - 0.493), 0.2, 1.8) from the fixed-mip read's A channel (mean about 1), toward 1 with the blend.
- Early types' nebula filaments (Uni_GalaxyEarlyNebula: amount F.x, rolled "EarlyTex.Nebula" -0.8..1.2, so about 60% of textured early types have them; F.x is the arms' asymmetry on other sets, read only with arms):
  - Source: the amount x the spheroid's light (bulge, with its D-232 noise and the shells) x an envelope (Uni_GalaxyEarlyNebulaEnv: all inside 0.15, none past UNI_EARLY_NEB_RADIUS 3 r_e, isophote-rounded).
  - Shape: galaxy_detail's ridged channel read at the spheroid's point at UNI_EARLY_NEB_MIP 2 and UNI_EARLY_NEB_FREQ 0.125 x G.w periods an effective radius (one period 8 r_e at G.w 1: they scale with the galaxy), over its mean (0.567) past 1.19, full by 1.42 (about the 75th and 99.3th percentiles at that mip), squared; mean 0.0471 (sampled, trilinear); x UNI_EARLY_NEB_SCALE 7. Light at amount 1: 9.5-23% of the stars' (mean 15%) over 3 r_e.
  - Colour: the mix term runs with the radius (core to the sphere's edge), moved a quarter either way by the fBm channel: one colour inside, another outside (a first version mixed by fBm alone read as confetti).
  - Sampling: the blend (Uni_GalaxyNebulaBlend) uses the early read's texel and the whole step (a volume, not a layer). The march's cap (Uni_GalaxyNebulaStepCap, early branch): inside the sphere (3 r_e plus twice the bulge warp's reach), steps of UNI_EARLY_NEB_STEP 0.25 read texels, lengthened to the ray's chord over UNI_EARLY_NEB_MAX_STEPS 192, up to 4x; a ray outside heading in stops at its surface; none once the screen footprint is past the blend (2 x 3 texels; Uni_CarrierStepLimit passes the nebulae's footprint, the screen's in the cube passes). First built at mip 3 with the galaxy's G.w frequency and the spheroid's own steps: blobs of trilinear facets, and blended out almost everywhere by the spheroid's long steps; then half-texel steps and 96 a chord: slide 6.9% from inside.
- Tidal shells (Uni_GalaxyShells, in the spheroid's multiplier: field, map and placement): amount C.y, rolled "EarlyTex.Shells" -1.2..0.6 (about a third of textured early types). Five shells at 1.6 r_e x 1.55^k, sharp outside (0.06 of the radius), a long tail inside (0.3), on alternate sides of a random axis (SeedMix(Hs, 14), a new sub-index; sides fade over -0.2..0.4 of the axis cosine). The stars are moved, not added: the profile's mean over each sphere (0.45 of a side-masked shell) is taken off, so each radius keeps its light (a first version added up to 40%); x (1 + 3 C.y x that), never below 0; none inside 0.5 r_e, all by r_e; footprint-widened keeping each shell's integral.
Measured (T2.galaxy.early_nebulae; experiments/earlyneb_explore_d236): marched as the engine steps, from outside the steps start at the sphere's surface (slide 0.00%); from a camera in the core (64 rays, 4 step phases) 1.96% (source 0.02%); the stars with shells 0.034% (0.024% without) outside, 0.018% (0.016%) inside. Cost: the cap's steps x 2.45 over the spheroid's own in close views of the cores, only while the pattern is resolved and uu.Nebula is on. One galaxy's core holds only a few of the texture's features, so its pattern's mean differs from the global one (x 0.86-1.97 on the same samples, x 1.17 geometric mean over eight).
Verified offline:
- T2.galaxy.early_nebulae (new): the shape mean is the sampled one (0.04706 vs 0.0471); 256 rolls: 151 with filaments, 83 with shells; spirals, untextured early types, dust sets and the home galaxy have neither; light, blend, slide as above; colours: three a galaxy at luminance 1, none nearly flat, 10 of 12 hue bins for spirals and 9 for early types. Stills: still_galaxy_early_nebulae (nine early types whole and their cores, as rolled and with filaments and shells forced on), still_nebula_colours (24 spirals' and 24 early types' three colours).
- T2.galaxy.spheroid_noise: still passes with shells and filaments rolled (light within 3.1%, core exact).
- T2.galaxy.nebulae: early types without filaments still have none (the early set now has F.x 0).
- T2.galaxy.nebula_slide, nebula_edges, T2.vol.galaxy_map_nebulae, far_split_nebulae, T4.static.shader_decl_order: unchanged.
Not verified: the engine side (UE-67).
Reroll impact: textured scenes' early types: the shells move their stars (placement follows the field; each radius keeps its count), so their stars reroll. Filaments and colours are render only. Spirals, dwarfs, the home galaxy and untextured scenes: unchanged except the nebula colours (render only).
Status: built; engine test UE-67.

D-237 — Nebulae: the fly-into layer (B-034)
Owner, 2026-11-13: "lets do the nebula follow up layer first, followed by galactic star clusters".
Built (render only; the march and Uni_GalaxyNebulaFine in UniFields.ush):
- Two fine reads of galaxy_detail in the galaxy's frame at a fixed mip 2 (UNI_NEB_FINE_MIP: the pattern never depends on the view; mip 2 keeps the reads' fBm cycles and drops the texel grain the steps could not sample), at UNI_NEB_FINE_FREQ 16 x the disc's detail frequency G.w and 8x that. Milky Way (24 kpc a field unit): texels 15.6 pc and 2 pc under the knot read's 125 pc.
  - The first, sparse: the ridged channel (filaments) or the inverted Worley one (cell walls: bubble shells), each over its mean past its 70th percentile, full at its 99th, squared, over that's mean (mean 1, 0 in 70% of the volume), mixed by the nebula's colour mix term.
  - The second, texture: the same channels to powers of equal contrast (cv 0.717; ridged ^4, A ^3.133), mean 1.
  - Their product multiplies the nebula's density; the dust takes the first's complement ((1 - ramp) over its mean: dark lanes between the bright walls, cleared where they glow), weighted by how far the sample is inside a knot.
  - Each octave blends to 1 from an eighth of its texel per half step (UNI_NEB_FINE_RES; the knots' quarter slid: 5.2% vs 2.8%) and from the footprint as the knots'.
- The march's finer steps inside a nebula: the step ratio eases from the view's to uu.Nebula.FineRatio (0.04) by how far the last sample was inside a nebula (its knot shape x 3 and its density over a fifth of the stars', 0..1), within uu.Nebula.FineReachKpc (1). With it, the first octave shows within ~100 pc (gone by 200), the second within 12-25 pc (the march starts 10 pc out: the real-space bubble).
- Teal cores: every spiral nebula's colour toward [O III] teal (hue 175, saturation 0.8) by up to 45% where its knot shape is past 0.55 (to 1), with its blend. Visible at any distance (a change to D-236's colours where the knots peak).
- FVolumeViewGPU 176 bytes (NebulaFineGain, NebulaFineRatio, NebulaFineReachCm, pad); CVars uu.Nebula.Fine (1), uu.Nebula.FineRatio (0.04), uu.Nebula.FineReachKpc (1).
- Exploration (experiments/nebula_fine_d237): a first version read mip 0 at 4x G.w with a moderate (pow) pattern: invisible inside the haze; at 32x with the sparse pattern, the texel-faceted lattice and step shells showed; smoothstep-weighted trilinear made it blockier; mip 2 at 16x with an eighth-texel blend is the version above.
Measured (T2.galaxy.nebula_fine, new; the real march inside the Milky Way's brightest disc nebula, 64 x 36, four step phases): slide 2.88% (its finer steps alone 2.50%, which is the base march's); steps 64 -> 106 a pixel there (x 1.66; only within 1 kpc and inside nebulae); light x 1.013 of its steps alone. The finer steps themselves resolve the camera's own knot instead of blending it to the galaxy's mean: inside one, the frame's nebula light rises (here 1.13 -> 1.30 of the stars').
Verified offline: T2.galaxy.nebula_fine (pattern means 0.998-1.019, the dust's 0.999-1.000; exactly 1 blended or for early types; slide, steps, light as above; 3 kpc above the disc the frame without it bit for bit); still_nebula_fine. Unchanged: T2.galaxy.nebulae, nebula_slide, nebula_edges, early_nebulae, T2.vol.galaxy_map_nebulae, far_split_nebulae.
Known limit: from inside, the nebula's far side (beyond ~200 pc, where the layer has blended to its mean) is a glow that dominates the near walls; the offline stills show the structure as soft darker gaps and brighter walls, not crisp filaments. Stronger options if the engine look is too subtle: a sparser first octave, a hollow knot near the camera (bubble walls), or a smaller FineRatio (cost).
Not verified: the engine side (UE-68).
Reroll impact: none (render only).
Status: built; engine test UE-68.

D-238 — Globular clusters, as a field, placed exactly (B-031: the galactic star-cluster layer, part 1)
Owner, 2026-11-13: the star-cluster layer after the nebulae; asked which kinds: "Both" (open clusters along the arms and globulars in the halo, with new placement support for the globulars). Constraint (B-031): a field, no cluster entities (an entity layer spiked the cost before).
Built:
- The field (UniFields.ush, Uni_GalaxyGc*; one source: the field, the galaxy map's path, placement):
  - Textured galaxies with a bulge (no dust set has one) have a halo population on a lattice in their frame, cells UNI_GC_LATTICE 0.4 r_e across (the Milky Way ~400 pc). A cell holds one globular with probability N pdf(cell centre) L^3 (at most 1); N = 150 x the bulge weight (x 2 for early types) x 0.7-1.3 (Milky Way 163; rolled spirals 106 on average, early types 447); pdf a cored r^-3.5 halo (core 1.5 r_e) normalised inside the cut.
  - Each at a hashed point of its cell's inner part (0.13 of the cell from each face), mass 0.004 of the bulge's integral over N x a log-normal draw (sigma 0.6, mean 1, at most 5.06); profile two Gaussians: core sigma 0.002 r_e x 0.6-1.6 (the Milky Way ~2 pc), a halo 4x wider holding 35%. Everything from the galaxy's hash (SeedMix(Hs, 15), a new sub-index) and the cell's index on a new channel, GLOBULAR. Its light is the bulge's component (old stars: the palette's bulge tint).
  - The volume widens each over the footprint and the march's step (FUniGalaxyConst::Step, set by the march; the integral kept) and blends it to the smooth halo it averages to once the widened halo nears the cell's margin: in practice the glow shows a globular only within ~100 pc; farther, its stars (sprites) carry it.
  - The CPU integrals (ComputeIntegral, the quick one) sample the smooth halo in place of the peaks (Uni_GalaxyGcIntegrandFix): a sample landing in one would stand for a whole tile.
- Placement (UniGenTask.ush), galaxy tasks with globulars:
  - A box at most a lattice cell (the Milky Way: star levels up to L70, M > -2.6): the probes see the field without its globulars; the count adds their mass inside the box exactly (lattice cells reaching the box's bounds, each Gaussian's box share by erf; up to UNI_GC_LIST 8 kept, the most in the box); a share of the candidates (their in-box mass over it plus the octants' mean envelope) is drawn from them: a globular in proportion to its mass in the box, a Gaussian in proportion to its, the point from it truncated to the box (inverse CDF per box axis: Uni_ErfInv, Giles 2010); every candidate is tested against its octant's envelope plus the globulars' density there, which is what the mixture is drawn from, so the accepted ones follow the density exactly.
  - A larger box samples the globulars as the smooth halo (its probes could not find them, nor its candidates fill them): the brightest stars (M < -2.6 for the Milky Way) are the halo's, not the clusters'.
  - FUniTaskCount 48 -> 128 bytes (GcHead, two index and two mass float4s). Without globulars (untextured scenes, dust, non-galaxy fields) nothing is drawn on GLOBULAR: placement bit for bit.
  - Uni_Erf moved to UniCoreDefs.ush (D-187's, shared); Uni_ErfInv added there.
Measured (T2.galaxy.globulars, new): the real CPU generator around picked Milky Way globulars at L64-L70 (2742 tasks): stars within 3 halo sigmas less the field's expected there, against each globular's expected count: L64 19106 / 19290, L66 29463 / 29582, L68 5970 / 5924, L70 217 / 201: x 0.996, z -1.03; radial profile within 0.2 points of the two Gaussians; no candidate clipped; 1.01 candidates a star in those tasks. A first version drew from the whole Gaussians and rejected what fell outside the box: 224 candidates a star. Census: 168 globulars (N 163), mass x 1.105 of the system's, 16% inside the core radius (the halo's 13%), 59% inside 4 (62%). Widening keeps the light (1.000 at 0, 0.01, 0.03 of a cell); the smooth halo is the lattice's mass within 2.2% in a shell.
Verified offline: T2.galaxy.globulars; T1.seed.channels (GLOBULAR); T2.gen.* (untextured: bit for bit). T2.galaxy.spheroid_noise now measures the spheroid without its globulars (the noise-off reference also halves an early type's count, so they differed in the core), and the emulated marches in T2.galaxy.nebula_slide and early_nebulae set the step as the engine does.
Cost: a galaxy task with globulars (textured) pays one lattice lookup per probe and candidate (a hash and a pow where the cell is empty), and its count up to 27 lattice cells with erf; the march one lookup per galaxy sample near the camera, a pow (the smooth halo) elsewhere.
Not verified: the engine side (UE-69).
Reroll impact: textured scenes' galaxy stars (positions, and every draw that follows from the candidate order) reroll where globulars reach a small box; the rest of each galaxy's tasks keep their candidates (the GLOBULAR draw only where listed), but their counts change slightly (the integral now holds the globulars' mass). Untextured scenes: bit for bit.
Status: built; engine test UE-69. Next: open clusters (D-239), on the same machinery.

D-239 — Open clusters, as a field, placed exactly (B-031: the galactic star-cluster layer, part 2)
Owner, 2026-11-13: "Both" (open clusters along the arms and globulars in the halo, with new placement support); globulars were D-238. Constraint (B-031): a field, no cluster entities.
Built:
- The field (UniFields.ush, Uni_GalaxyOc*; one source: the field, the galaxy map's path, placement):
  - Textured spirals with arms and knots (Uni_GalaxyOcOn) have a 2D lattice in their disc plane, cells UNI_OC_LATTICE 0.025 disc scale lengths across (the Milky Way 93 pc). A cell holds a cluster with probability 0.6, at a hashed point of the cell, on the disc's analytic warped mid-plane (D-178) with a height scatter of 0.25 h (a sum of three uniforms); two isotropic Gaussians, core sigma 0.0005 disc scale lengths x 0.6-1.6 (the Milky Way 1.1-3 pc), a halo 2.5x wider holding 40%. Hash SeedMix(Hs, 16) (a new sub-index) and the cell's index on a new channel, OPENCLUSTER.
  - In the field's own frame, not the textures' warped frame: measured (experiments/oc_size_d239.cpp.txt) the warps move the Milky Way's disc 149 pc in-plane and 108 pc vertically on average and shear it ~0.36 (0.7 pc over 2 pc), so a cluster defined there is no Gaussian in space and placement could not draw it (a first version did: centroids 54 noise sigmas off).
  - Light moved, not added: the clusters gather UNI_OC_FRAC 0.06 of the saturated young light Ys = Ycap (1 - e^(-young / Ycap)), young the arms' and knots' light, Ycap UNI_OC_CAP 2 x the arms' light without the textures' detail. The disc becomes ... + Ys (C - frac), C the clusters' profiles x frac / 0.6 x a log-normal draw (sigma 0.4, mean 1) x the cell's area x the column the cluster's height leaves (2 h e^(|dz| / h)): C's mean over the disc is frac. Measured 0.976 and 0.936 of frac x Ys in two patches (the textures move the young light's mid-plane off the analytic one the clusters sit on).
  - Sizes (Milky Way, two patches, all star levels): median 249 stars a cluster, p90 17k, p99 57k, max 150k; together 2.6% of the disc's light. The mass follows the young light at the cluster, so the arms' knots hold the largest (Westerlund-1 scale) and the inter-arm ones are near empty. A first version (frac 0.4 of the unsaturated young light, 130 pc cells) put 23% of the disc's light in clusters of up to 15 million stars.
  - The volume widens each over the footprint and the march's step and blends C to its mean as the widened halo nears an eighth of a cell (the 2 x 2 nearest cells are evaluated; a cluster beyond them is at least half a cell away).
  - The CPU integrals sample Uni_GalaxyIntegrand: the open clusters' mean and the globulars' smooth halo in place of their peaks. Checks that sample the disc light at points coarser than a cluster use it too (T2.galaxy.textures' footprint-0 integral and correlation, T2.galaxy.warp_band's face-on bands: both back to their pre-cluster numbers).
- Placement (UniGenTask.ush), by the box's size against the lattice cell (OcMode):
  - At most half a cell (the Milky Way: L66 and finer): listed with the globulars in the same 8 slots (packed negative), their in-box mass (erf share x weight x Ys at the centre) added to the count, candidates drawn from them truncated to the box (D-238's machinery, now Uni_TaskClusterPoint).
  - Up to UNI_OC_CELLS 16 cells across (its extent x sqrt 3 within 14 cells; the Milky Way L67-L70): the count sums the clusters' in-box masses over the cells it reaches (no field read for a cluster out of the box's reach: the box share is from the hash); OcHead keeps the two bounds (weight x in-box share; Ys) and OcRange the cells. A candidate's open-cluster branch picks a cell uniformly, keeps it with probability its weight x share over the first bound (no field read: most stop here), then its Ys over the second, then draws the point from its Gaussians truncated to the box: each cluster in proportion to its mass in the box.
  - Larger (the Milky Way L71+): the clusters' mean (frac x Ys) in the density, no draws. The brightest stars are the disc's, not the clusters'.
  - The probes and candidates read the field once each (Uni_TaskDensityParts / Uni_TaskDensityFrom); probes take out the clusters where the count adds them.
  - FUniTaskCount 128 -> 160 bytes (OcHead, OcRange). Without open clusters (untextured scenes, early types, dust sets, non-galaxy fields) nothing is drawn on OPENCLUSTER and the GLOBULAR pick keeps D-238's test: placement bit for bit with D-238.
  - Shim: int2 and a float2 dot (UniHlslShim.h).
Measured (T2.galaxy.open_clusters, new): the real CPU generator around the six brightest Milky Way clusters of a patch at L64, L66 (listed) and L67 (by cell), and every cluster of an 8 x 8 cell patch about the brightest at L68 and L70 (by cell), 2290 tasks: stars within 3 halo sigmas less the field's expected there, against the clusters' expected counts: L64 13107 / 13218, L66 14618 / 14661, L67 7577 / 7664, L68 12798 / 12945, L70 401 / 438; listed x 0.994 (z -0.90), by cell x 0.987 (z -1.81); profile within 2.7 points; centroid within 1.65 noise sigmas; nothing clipped or truncated; candidates a star 1.12 / 1.34 listed, 2.2 / 2.2 / 3.0 by cell (the scene's placement averages 3.5). Lattice: occupancy 0.602, draw mean 1.007, sigmas in range, heights within 0.70 h. Widening keeps the light (1.000 at 0, 0.02, 0.06 of a cell; the mean past an eighth). T2.galaxy.globulars unchanged (1.01 candidates a star around globulars).
Verified offline: T2.galaxy.open_clusters; T1.seed.channels (OPENCLUSTER); T2.gen.* (untextured: bit for bit); T2.galaxy.* (textures and warp_band sample the clusters' mean, as above).
Cost: a textured spiral's tasks pay a 2 x 2 lattice lookup per probe and candidate; a by-cell box's count up to 16 x 16 hashes and a field read per in-reach cluster, in the count's single thread (UniGen.usf); the march a 2 x 2 lookup per galaxy disc sample.
Not verified: the engine side (UE-70).
Reroll impact: textured scenes' spiral stars reroll (the disc's light now moves into clusters; counts change by the integral, positions by the field and the candidates' draws). Early types and untextured scenes: bit for bit with D-238.
Status: built; engine test UE-70. B-031's star-cluster layer is done (D-238, D-239); follow-ups B-035.

D-240 — The volume march jittered and accumulated (B-034; the cloud's march)
Owner, 2026-11-14: "the march has slices and inconsistencies in the detail layers of the nebulae, and in general looks pretty poor. I would suggest taking a look at the way the cloud atmosphere layer handles its warp, structure, detail layers and its march." Chose: jitter the whole march (with temporal accumulation). Later: "the galaxy dust is similar to cloud rendering anyway ... you would kind of expect a unified or at least similar march paradigm between the 2" (divergences kept, with reasons, in B-034).
Built (render only; UniVolume.ush, UniVolume.usf, UniRenderer.cpp):
- Uni_VolumeMarchJ: each ray's first step (and the first after a gap between carriers) x (1 - a draw): the log lattice shifts per pixel, so neighbouring rays share no step boundary; each sample at a drawn point of its step instead of the middle. The draws (Uni_VolumeDraw): the phase interleaved gradient noise of the ray's pixel; a step's a hash of the pixel and the step; both shifted by frac(k x 0.618) with k the pixel's sample count (TemporalStep / N^2 when sliced). MarchFlags bit 3 (UNI_VOL_JITTER_FLAG), off under the checks' fine reference (bit 1); the cube passes (background, far cache) march unjittered (the Uni_VolumeMarch wrapper). FUniVolumeView 176 bytes: JitterShift and a pad replace D-237's fields.
- ReconstructCS: a fresh pixel blends into its reprojected history, clamped (D-194) into the box of it and its four fresh neighbours N away, with weight max(1 / (age (1 - reject) + 1), 1 / MaxSamples); reject = how far the clamp moved the history, as a share of the widened box. Age in a new R16F target (TemporalAge; 1 on unsliced frames). Reconstructed pixels carry their age x (1 - reject). Slices accumulate as the volume; depth stays fresh.
- CVars uu.VolumeJitter (1), uu.VolumeAccumulate (16).
- A first draw moved the gradient noise's lookup per step: that only adds a constant to its value, so consecutive steps were tied (r -0.43) and the coherent error stayed (4 x 4 blocks 1.42% vs unjittered 1.31%); hashed: 0.65%.
- Tried: shortening the accumulation with the pixel's motion (16 at rest to 1 at uu.VolumeTemporal.TightPx): no check moved by more than 0.15 points; not adopted (the clamp and its reject keep trails out; a moving view keeps its averaging).
Measured: T2.vol.jitter (new): draws uniform (worst of 16 bins 1.3%), consecutive r +0.002, neighbouring phases 0.49 apart; off: D-130 bit for bit; inside the Milky Way's textured disc against the march at an eighth of the steps, 4 x 4 blocks (per pixel): unjittered 1.31% (1.98%), mean of 16 jittered 0.65% (2.25%), temporal 2 x 2 over 64 still frames 0.66% (2.37%). T2.vol.accumulate (new): D-194's passes against a fine march: at rest light 0.07%, turning 0.09%, 3 kpc/frame toward the Milky Way 0.81% (unjittered, unaccumulated 4.37%: its steps were the error there), turning 2.11% (2.19%), 12 kpc/frame sideways 2.57% (2.56%); excess light as D-194's within 0.5 points. T2.galaxy.nebula_detail from 250 pc in a nebula: unjittered 5.17%, mean of 12 jittered 4.61%. Exploration (experiments/jitter_explore_d240): inside the knot, per pixel, one jittered frame 5.2%, the mean of 32 3.0%, unjittered 3.1% (the rest is shared by every coarse march: extinction over long steps); from 250 pc 7.5%, 4.6%, 5.2%.
Cost: the march's steps unchanged (+1 hash a step); the reconstruct +4 fresh loads and an R16F read and write per fresh pixel.
Not verified: the engine side (UE-71).
Reroll impact: none (render only).
Status: built; engine test UE-71.

D-241 — The nebulae rebuilt cloud-style: structure eroded by warped detail layers (B-034)
Owner, 2026-11-14: as D-240 (chose: rebuild the nebula field); after the first stills: "the detail layer is too fine".
Built (render only; UniFields.ush's nebula block, the march's nebula samples):
- One field, as CloudAtmosphere builds its clouds: the knots' structure X (the knot read at its fixed mip 1, D-235) x UNI_NEB_CORE 0.8, eroded by the walls layer: a galaxy_detail read whose ridged/Worley mix (per nebula, D-237's) gives the erosion e = m x UNI_NEB_EROSION1 0.75, D1 = saturate((X - e) / (1 - e)); then by the texture layer (EROSION2 0.5) on D1. Each detail read displaced by the standardised channels of the read above it, x UNI_NEB_WARP1 1.5 / WARP2 1.0 periods of its own. Walls ~42 pc, texture ~10 pc in the Milky Way (UNI_NEB_FINE_FREQ 6, octave 4; first stills at 16 / 8 were too fine).
- Level of detail: each detail read at the mip its footprint needs (>= 2), full to lod 4 and out by 5 (Uni_GalaxyNebulaFineStrength), x the distance fade over uu.Nebula.FineReachKpc's second half at the sample's own distance; faded layers take their fade means (UNI_NEB_FM1, FM2: the light is held through the fade). The knots blend to their mean by footprint (D-235, from 3 read texels a pixel) and, past the detail's reach, by distance: from UNI_NEBULA_LOD 2 read texels per half of the march's nominal step along the disc plane (its log ratio x the distance x the ray's in-plane share: smooth across pixels, unlike the step plan; the Milky Way's 78 pc texels: from ~1 kpc along the disc, full by ~2 kpc). Without the distance term, jittered kpc steps through the knots were 85% noise a frame in their light.
- Shape = D2^2 x J x SHAPE_MEAN / SHAPE_MEAN2 (the light D-234's), blended to the mean by the knots' blend. Dust lanes: the dust x max(1 + 0.8 S1 ((X CORE - D1) / LANE_MEAN - 1), 0): dark where the structure is eroded, mean 1. Teal cores from X (D-237).
- Removed: D-237's finer steps inside nebulae (uu.Nebula.FineRatio, per ray: patches), the step blends (UNI_NEBULA_RES; the detail's step terms), NebNear. Kept: the gas layer's step cap for rays crossing it.
- Shim knobs for every constant; T2.galaxy.nebula_detail checks the shaders' values equal them.
Measured: T2.galaxy.nebula_detail (new): the shape's mean with both layers, the texture faded, both faded, and blended within 0.01% of D-234's; the dust multiplier's mean 1.0000 inside, exactly 1 outside and faded; LOD and early types (D-236 bit for bit) as specified. T2.galaxy.nebula_slide (reworked for D-240): inside the Milky Way along and across the disc, the jittered mean against an eighth of the steps: bias 0.81% (its noise 0.80%), spread over first-step phases 3.29% (the gas layer's own 5.05%; one unjittered march 7.09%), one frame's noise 6.4%; outside 0.00%. T2.galaxy.nebula_edges (the jittered mean): p95 15.9%, light +0.1%. T2.vol.far_split_nebulae, T2.vol.galaxy_map_nebulae, T2.galaxy.early_nebulae, T2.galaxy.nebulae unchanged in passing.
Known limit: knots along the disc beyond ~2 kpc are their mean glow; cloud-style coarse/fine stepping inside the structure would keep them (B-034).
Not verified: the engine side (UE-71; UE-68 withdrawn).
Reroll impact: none (render only).
Status: built; engine test UE-71.

D-242 — Nebulae: each one's balance of glowing and dark dust (B-034)
Owner, 2026-11-14: "nebula currently just have an emissive component right? But in reality they would have an emissive component and a non emissive component similar to the galaxy dust lanes right? So if we have both i think our nebula will look better and have another variance handle, basically the balance of luminous dust to non luminous dust?" Built while the owner was away (reading: a per-nebula balance around a mean the owner controls; the dark side as dust gathered into the nebula's structure).
Built (render only; UniFields.ush's nebula block, the march):
- Each spiral nebula's dark share a from its knot read's colour and brightness terms: v = t / sqrt(1 + t^2), t = 1.5 z w (standardised; no new read; linearly uncorrelated with either: r +0.15 with the brightness, -0.14 with filaments-or-shells); a = m + v (1 - m) above, m + v m below (in [0, 1] for any mean m; its mean linear in m). m: uu.Nebula.Dark (0.35; below 0: off, D-241 bit for bit). At the default 23% of the nebulae (structure-weighted) are dark-dominated (a > 0.6), 22% glowing (a < 0.15).
- Emission x (1 - 0.85 a), over its light-weighted mean at the default (VPS, VMS: v's measured positive and negative means): D-241's light at the default; the mean moves with uu.Nebula.Dark (0.1: x1.14, 0.6: x0.86), blended exactly.
- Dust in the structure x (1 + 100 a (X CORE)^2): a dark nebula is an opaque cloud (x ~60 at its core; the galaxy's dust is ~0.04 optical depths per 100 pc in the plane), its glow at its rims. Added, not redistributed: holding the mean would starve the rest (the cores are ~1% of the gas layer). It goes as the knots blend: unresolved, the clouds cover little of a pixel (a clumpy medium is far more transparent than its mean), so from afar the dust is D-241's.
- The reflection nebulae (D-234) take the emission's factor over their own dust-weighted mean (VPR, VMR), not the dark dust: the reflection model has no shadowing, and a cloud's gathered dust lit through its whole depth made the dark clouds brighter (first attempt).
- FUniVolumeView 176 bytes: NebulaDark replaces D-241's pad. Uni_GalaxyNebulaDensityAt takes the share and returns the reflection's multiplier.
- Tried: dust redistributed to hold the mean (gain 3.5, then 40): invisible (the clouds' extra optical depth ~0.02 across the frame).
Measured (T2.galaxy.nebula_dark, new): over the Milky Way's gas layer at the default, light x1.0000, reflection x1.0000, dust x1.45 over the source region (x5.1 where the structure is); shares 0.1 and 0.6 the model's light within 0.01%, blended exactly, the blended dust D-241's exactly; off bit for bit; early types unchanged. The march in the mid-plane 400 pc from the brightest nebula, looking along the arm: x1.12 the light at share 0, x0.88 at 0.8 (still_nebula_dark). T2.galaxy.nebulae: the nebulae add 19.5% from the solar camera (D-241: 22.2%; the default's dark clouds absorb some), 16.2% from 60 kpc (unchanged); the near fade now fades the nebulae's dust with their light (render only; the frame without them within 0.04%).
Known limit: the effect is subtle offline; the knots' young stars, not the nebulae, are the brightest feature there, and from above the disc the stars above the gas layer outshine its clouds. Stronger: UNI_NEB_DARK_DUST (shader constant) or a higher uu.Nebula.Dark.
Not verified: the engine side (UE-72).
Reroll impact: none (render only).
Status: built; engine test UE-72.

D-243 — Nebulae: ionisation fronts (B-034)
Owner, 2026-11-14: "Yes let's address all of those follow ups" (B-034: dark clouds offset from the H II regions, ionisation fronts at their edges).
Built (render only; UniFields.ush):
- Where the walls are read (D-241), a nebula's dark share no longer dims it evenly: the walls read's fBm (its third channel, apart from the erosion's), standardised and squashed to 0..1 (s, ~40 pc patches in the Milky Way), splits it. Glowing gas where s is above the share (G = smoothstep(a - 0.08, a + 0.08, s)), dark dust below (the dust x (1 + 100 (1 - G) (X CORE)^2): D-242's density at a = 1, gathered on the dark side), and the front between them brighter (x 1 + a gaussian of 0.05 about s = a).
- Emission and the reflection's dust x (G + rim), over their means at the default share (LSPLIT 0.74917 light-weighted, RSPLIT 0.66538 dust-weighted), blended with D-242's even dimming by the walls' strength: the light holds as the detail fades. D-242's normalisations re-measured at detail 0, where the even dimming applies (VPS 0.03937, VMS 0.43490, VPR 0.27492, VMR 0.23992).
- Uni_GalaxyNebulaShapeSplitAt returns the split; Uni_GalaxyNebulaShapeAt keeps its signature.
Measured (T2.galaxy.nebula_fronts, new): at the default share the light x1.0000 at full detail (D-241's), x1.0171 half faded (each blend end normalised over its own detail's shapes), reflection x0.9963; 0.418 of the structure dark (the nebulae's mean share 0.389); dark-side dust x7.85 the glowing side's; fronts 8.4% of the structure. T2.galaxy.nebula_dark (detail 0) as D-242's.
Not verified: the engine side (UE-73).
Reroll impact: none (render only).
Status: built; engine test UE-73.

D-244 — Nebulae: cloud-style fine steps inside the gas layer, behind uu.Nebula.Refine (B-034)
Owner, 2026-11-14: as D-243; the coarse/fine stepping. Offered three options (on by default, behind a CVar off by default, drop); no answer before the build, so option 1 as announced: behind a CVar, off.
Built (render only; the march):
- uu.Nebula.Refine N (1: off): inside a spiral's gas layer, a step longer than a knot texel along the disc plane is split into up to N sub-samples (UNI_NEB_SUB_TEXELS 1 texel apart), stratified and jittered (the step's draw rotated by the golden ratio), each evaluating the whole galaxy field (UniField_GalaxyFilteredCKN) and the nebulae there; the step's nebula emission, dust and reflection multipliers are their mean. The distance level of detail moves out N times (its nominal step over N): far knots stay sharp to ~4-8 kpc along the disc at N = 4 instead of ~1-2.
- FUniVolumeView 176 bytes: NebulaRefine takes MapPad1.
- First attempt (archived, experiments/nebula_refine_d244.cpp.txt): sub-samples sharing the step's source with one knot read each: 8.4% biased (the source, young light, carries the same knots).
Measured (T2.galaxy.nebula_slide, extended): at N = 4 against its own reference, bias 0.76% (noise 1.51%), slide 3.33%, one frame's noise 12.1% (N = 1: 6.4%, with the knots blended past ~2 kpc), 79.7 whole-field evaluations a ray along and across the disc (the march takes ~50 steps there: ~2.5x the galaxy work). T2.galaxy.nebula_fronts: the march 250 pc from a knot at N = 4 x0.9814 the light of N = 1.
Not verified: the engine side, its cost (UE-73).
Reroll impact: none (render only).
Status: built, off; engine test UE-73 (ProfileGPU with uu.Nebula.Refine 1 vs 4).

D-245 — Nebulae fade in past the galaxy's handoff (B-034)
Owner, 2026-11-14: as D-243 (the galaxy sprite lacks the nebulae's light; the carrier brightened by their share over the 4-8 px handoff).
Built (render only; UniProject.ush, the carrier builder, the march):
- A galaxy carrier's nebulae (emission and reflection) are hidden through the handoff and fade in from its end (T1 = min(8 px, theta_promote)) to 6x it, smoothly in the log of the size (Uni_CarrierNebulaHide): the nebulae's share (~10% for the Milky Way) arrives over a 6x zoom where the handoff's 2x took it at once. Unresolved there anyway (below 3 knot texels a pixel).
- FUniCarrier / FCarrierGPU 112 -> 128 bytes (NebulaHide and three pads; 0: shown, the default for every other carrier). FVolumeSettings::bNebulaHandoffFade (checks).
- Not done: the sprite carrying a per-galaxy nebula share (the backlog's suggestion). The sprite pass has no per-galaxy parameters, and from afar the nebulae are part of the integrated light only up to ~10%; the fade removes the visible step instead.
Measured (T3.vol.nebula_handoff, new): the textured Milky Way from 6 Mpc to 80 kpc (no dust): through the handoff the nebulae add 0.03% at worst (unfaded: 3.1% at its middle); the largest change between steps 1.33x apart 2.19% (unfaded 5.70%); their share at 80 kpc +9.57% either way.
Not verified: the engine side (UE-73).
Reroll impact: none (render only).
Status: built; engine test UE-73.

D-246 — The volume's grain: CloudAtmosphere's blue noise, resolve and placement before TSR
Owner, 2026-11-14 (testing D-243..D-245 in the engine): "quite grainy compared to the cloud atmosphere shader... how does that shader smooth out the grain? composite before temporal aa?"; then "we want to use the same blue noise as the atmosphere... our resolve should follow the clouds template - full output resolution 2x2, 3x3, or 4x4, variance clamp, pixel averaging etc, composite before tsr ... this should render before the atmosphere shader does i think right?"
Read first (CloudAtmosphere, reference only): its draws are FastBlueNoise_scalar_128x128x8 texels at the pixel plus an R2 shift per draw index (Atmo_TextureJitter); MainResolveCS runs at full resolution with one marched pixel per CellSize^2 and blends every pixel each frame; it composites at BeforeDOF, before TSR, which accumulates further. Its extension keeps the default priority 0.
Built:
- Draws (UniVolume.ush Uni_VolumeDraw): with a tile bound (UNI_VOL_NOISE, its size), the draw is the tile's texel at the pixel moved by frac(index x (0.75487766, 0.56984029)) x size, plus the sample's golden-ratio shift; no tile: D-240's draws bit for bit. The renderer binds the engine's texture at its full size (the subsystem loads and holds it; uu.VolumeBlueNoise). Offline a void-and-cluster tile stands in (Tools/UniVerify/src/UniBlueNoise.h).
- Resolve (ReconstructCS CloudResolve; UniVolumeRef.h bCloud; the Uni_Resolve* helpers): every pixel takes the 4 x 4 fresh samples about it (K N + the phase's offset), weighted by scene-depth agreement (+1e-3); a bilinear fill and tent moments (two cells) on compressed radiance (rgb x exposure / (1 + max), the knee at the displayed brightness: SceneColourScale x PreExposure / FocalPx^2); the history clamped to mean +- gamma sigma (4 at rest, 1 by 2 px of motion), the clamp's move over gamma sigma + 0.01 taking its samples; a marched pixel blends its sample at max(1 / (age + 1), 1 / uu.VolumeAccumulate); the others blend the fill at lerp(0.1 (1 - own age), 0.5, motion), counted into their age; without history the fill. Ages R16F -> G16R16F (all, own). uu.VolumeResolve (1); 0: D-240's reconstruct.
- Resolution: with the resolve the march runs at uu.VolumeResolveDivisor (1: full resolution) with 4x uu.VolumeMaxMarchPixels (the rays a frame, sliced 1/4 to 1/16, as D-240's half resolution at 2 x 2 and below). The far cache's map keeps D-240's size (uu.VolumeDivisor): its light is smooth, and a finer map would refresh twice as often.
- Placement (UniRenderHooks.cpp): with uu.VolumeBeforeTSR (1) the volume (march, resolve, background, composite) runs in the BeforeDOF callback into a copy of scene colour at the internal resolution; the march's rays take the view's TAA jitter (RayJitterPx: the ray of pixel p is the unjittered one at p - J, in march pixels) while pixels and history stay at their centres (CloudAtmosphere's rule). The stars stay after MotionBlur, reading the slices kept from the earlier callback (same graph, view and render frame; otherwise the volume is drawn there as before). The extension's priority is 100: the engine calls extensions in descending priority, so the volume is in scene colour before CloudAtmosphere (0) composites the atmosphere over it. Yes, it should render first: the atmosphere attenuates and in-scatters over what is behind it.
- A history from the other placement or the old age format is dropped (one full march).
Measured:
- T2.vol.blue_noise (new): the stand-in tile uniform (bins within 0.00%), neighbours 0.411 apart (white 0.333), power below 1/8 cycle 0.04% of white noise's; draws uncorrelated (r +0.003); one jittered frame inside the disc per pixel as noisy as D-240's (3.96% vs 3.94%), its 4 x 4 blocks' grain over the floor 0.73x D-240's (1.02% -> 0.85%, floor 0.41%). Modest because D-240's phase draw was already interleaved gradient noise; the grain's fix is the resolve and TSR.
- T2.vol.resolve (new): on D-194's passes and inside the Milky Way, every frame's light within 2.69% (at rest 0.06%, D-194's unjittered 0.01%); a still camera in the textured disc (4 x 4, 64 frames) per pixel 2.46% from an eighth of the steps against D-240's 2.57%, 4 x 4 blocks 0.75% against 0.61% (the fill's share before a pixel's first own sample stays in its history: CloudAtmosphere's trade, quicker fill for a little blur).
Not verified: shaders compile, the hook order with CloudAtmosphere, TSR on the volume, the grain against the clouds, the cost of the full-resolution march, the texture in a cooked build (UE-74).
Reroll impact: none (render only).
Status: built; engine test UE-74.

D-247 — The sliced march's blue noise per cell (CloudAtmosphere's NoiseOffset)
Owner, 2026-11-14 (testing D-246): "it helped but there is still quite a bit of grain"; blotchy and crawling at rest and in motion.
Found: CloudAtmosphere's MainMarchCS draws its noise at the cell (DispatchThreadId: one per CellSize^2) plus an R2 offset per rank (the cell's pixel this frame); D-246 drew it at the pixel, so a sliced frame's fresh samples read the tile every N texels, and a blue tile decimated by N is not blue.
Built (render only): Uni_TemporalRank, Uni_VolumeNoiseCell (cell + frac(0.5 + rank (0.7548776662, 0.5698402910)) x 4096); MarchCS uses it when sliced with the tile bound (TemporalPhaseMarch), the pixel otherwise; UniVolumeRef.h bCellNoise.
Measured (T2.vol.resolve, still camera in the textured disc, 4 x 4, 64 frames): per pixel 2.46% -> 2.24% (D-240 2.57%), 4 x 4 blocks 0.75% -> 0.64% (D-240 0.61%), crawl a frame ~0.1% for all three.
Engine (owner 2026-11-15, uu.Perf): 1/16 4625, 1/9 13, 1/4 3551 frames sliced, 2 not: the history is used; uu.VolumeJitter 0 removes the speckle (and the slices return); uu.VolumeResolveDivisor 2 only changes the speckle's grid. So the grain is the samples' own noise through the nebulae; the owner found a much finer uu.VolumeStepRatio with a larger uu.VolumeResolveDivisor better.
Reroll impact: none (render only).
Status: built; engine test UE-74.

D-248 — Handles for the nebulae and their detail; the detail out to 7 kpc
Owner, 2026-11-15: "we want handles for all of those [the detail's frequencies and fade] as well as the frequency/scale/prevalence of the nebulas themselves, and we want the detail layer to default further out... something like a 7".
Built (render only; UniFields.ush UNI_NEB_TUNE / UNI_NEB_TUNE2, bound by UniVolume.usf (not the map bake, which holds no nebula light), the C++ shim's GNebTune / GNebTune2; UniCore::NebulaTune; frame data NebTune):
- uu.Nebula.Scale (1; 0.25..4): the knot read's frequency x 1 / scale; the knots' blend, the gas-layer step cap and D-244's sub-steps follow (their texel x the scale). Away from 1 the nebulae leave the stars' knots.
- uu.Nebula.Prevalence (1; x0.05..x5.6 of the default's ~11% of the gas layer): the knot thresholds shift by d where the measured coverage is that multiple (kNebulaPrevalence: 25 shifts over the Milky Way's gas layer, the coverage, E[X^2 J] and the eroded shape's mean, each over d = 0's); the shape means scale with them, so near detail and far blend keep one light; each nebula keeps its brightness (x2: light x2.35, x4: x6.95).
- The eroded shape's mean also follows the scale (kNebulaErodeByScale, 1/4..4 in half octaves: x0.84..x1.15): the detail reads the same texture as the knots at a fixed size, so their overlap changes with the nebulae's size.
- uu.Nebula.FineScale (1): the detail layers' size; uu.Nebula.FineOctave (4): the texture layer over the walls; uu.Nebula.FineLodBias (0): mips added to the detail reads by footprint.
- uu.Nebula.FineReachKpc 1 -> 7 (the engine's default; FVolumeSettings and the checks keep 1). More of the march reads the detail (cost to see in ProfileGPU).
- The sky light's worker march reads the shim's globals: the subsystem writes them only when the tuning changes.
- Defaults are D-247's bit for bit (x 1.0, + 0.0, the same constants).
Measured (T2.galaxy.nebula_tune, new): defaults equal everywhere (NebulaTune, FNebulaTune, the shim, the HLSL macros); prevalence x0.25/0.5/2/4 covers x0.246/0.493/1.996/4.016 with the near/far light x1.043/1.020/0.999/1.008 of the default's; scale x0.5/x2 coverage x1.052/1.031, the erosion's share within 6% (two sample sets differ by 3-5%); fine scale x0.5/x2 and octave 2/8 near/far within 2.2%; the mip bias moves both reads by exactly 1.
Not verified: the engine side (UE-75).
Reroll impact: none (render only; placement reads no nebula term).
Status: built; engine test UE-75.

D-249 — The volume textures by console variable; the nebulae's own detail texture
Owner, 2026-11-15: "we are also going to need to be able to configure the volume textures used for the various components because i will want to test different versions"; chose the three galaxy textures and a separate nebula-detail texture; "Textures are already normalized from the bake side" (no stats measured at load).
Built:
- uu.GalaxyTexture.WarpFine / .WarpLarge / .Detail (defaults warp_fine, warp_large, galaxy_detail): each a file in Data/Noise. They feed placement too: a change rerolls galaxies' stars.
- uu.GalaxyTexture.NebulaDetail (empty: Detail's): a fourth galaxy texture slot (UNI_GAL_TEX_COUNT 4, GalaxyTextureSlots 4) read only by the nebulae's two detail layers (UNI_NEB_DETAIL; UniVolume.usf's UniNebDetail, not in the map bake); any size; render only. The knots stay on galaxy_detail.
- uu.ReloadTextures: re-enables the current scene (textures load before the model bakes the definition) and puts the camera, its motion and free flight back. uu.WebTexture reloads with it.
Measured (T2.galaxy.nebula_tune, extended): the slot unbound or bound to galaxy_detail, the nebulae bit for bit; bound to another texture, 62 of 62 nebula shapes change, the knots and the field's light (placement's) unchanged at 4000 of 4000 points.
Not verified: the engine side (UE-76).
Reroll impact: the three galaxy textures, when changed (as before, at uu.Enable); the nebula slot none.
Status: built; engine test UE-76.

D-250 — Diffuse dust in galaxies, its reach rolled per galaxy
Owner, 2026-11-15: "some degree of background dust in the galaxy that makes it less likely for you to be able to view out to the distance of the entire galactic span from within it - not a heavily dense cloud like in the dust lanes or nebula, but a low grade background of dust"; "It should vary galaxy by galaxy ... lets say we randomize it from .75 kpc to 10kpc".
Built (render only; Uni_GalaxyDiffuseDust, the march's dust term):
- Each dusty galaxy carrier adds extinction tau = (gain / reach) x profile along the ray: an exponential disc on the stars' scale length, 1 at 2.5 scale lengths (UNI_DIFFUSE_DUST_RREF, ~the solar circle), held to 4x inside (UNI_DIFFUSE_DUST_CORE), and an exponential layer about the gas layer's warped, flared mid-plane (Uni_GalaxyLayerMid, factored out of D-235's step cap, bit for bit) with the dust set's scale height x 1.5.
- The reach (tau 1 along the mid-plane at the reference radius) is log-uniform in [uu.Galaxy.DustReachMinKpc 0.75, uu.Galaxy.DustReachMaxKpc 10] by the galaxy's seed on a new channel DIFFUSEDUST (0x6C94819D); uu.Galaxy.DiffuseDust (engine 1; FVolumeSettings 0, so the checks keep D-249's march); uu.Galaxy.DiffuseDustHeight 1.5.
- It fades in with the nebulae past the sprite handoff (D-245's NebulaHide): the sprite has no internal dust, so the volume must not step darker there.
- FUniVolumeView / FVolumeViewGPU 176 -> 192 bytes (four floats).
- The sprites dim through the slices as with any dust; the far cache, background and sky light march the same core.
Measured (T2.vol.diffuse_dust, new): 4000 seeds in range, 0.507 below the geometric middle, log quarters within 1.1%; the profile 1, e^-1 one scale height up, 4 at the centre; off is bit for bit; from the solar camera with a 2 kpc reach the march's added depth on rays along and near the disc within 0.43% of a fine integral (rays under tau 6; the tangent through the inner disc reaches tau 9.4: its far side gone), straight up tau 0.095.
Not verified: the engine side (UE-76).
Reroll impact: none (render only).
Status: built; engine test UE-76.

D-251 — The grain: finer steps through the gas layer (uu.Nebula.StepRatio); denoise and step level of detail as handles, off
Owner, 2026-11-15: "Dust is very nice ... the effect overall is really nice if we could come up with a solid fix for the grain noise wed be close to done"; earlier "my findings support a much finer step ratio against a larger volume resolve divisor".
Measured first (experiments/grain_d251; the engine's path offline: cloud resolve, blue noise, 4 x 4 slicing, 64 still frames, the detail to 7 kpc; per-pixel error against an eighth of the steps), 250 pc from the Milky Way's brightest disc nebula and along the disc from the solar camera:
- The grain is each step drawing the nebulae's 10-40 pc detail at one jittered point of a long step. A finer global step ratio removes it in the nebula (0.32 -> 0.16 -> 0.08: 6.1% -> 3.6% -> 2.6%) but changes the disc's content (its knots' distance level of detail follows the carrier's nominal ratio).
- An a-trous denoise (SVGF) with each pixel's own samples' variance: in the nebula 6.1% -> 4.6% (one pass), along the disc no better (3.89% -> 3.90%); with the fresh neighbourhood's spread as the early variance it blurred the disc (+8% at a tenth of it); more passes or a wider edge stop blur more.
- Prefiltering the detail over the step (x0.25): 6.1% -> 5.9%; with the denoise, worse.
Built (render only):
- uu.Nebula.StepRatio (engine 0.08; FVolumeSettings::NebulaStepRatio 0 for the checks): through a spiral's gas layer (Uni_GalaxyGasLayerInside: within 4 scale heights of the warped, flared mid-plane) within the detail's reach the step is at most this x the distance (Uni_CarrierStepLimit; UNI_NEB_TUNE3.x). The step plan only: the content (the knots' level of detail) stays the carrier's.
- uu.VolumeDenoise (0: off) and uu.VolumeDenoiseSigma (4): one or more a-trous passes (UNI_VOL_PASS 6 DenoiseCS; Uni_Denoise*; UniVolumeRef.h DenoiseVolume) over the resolve for the composite, the history unfiltered; the resolve keeps each pixel's own samples' compressed luminance moments (G32R32F history, started by a full march: MarchCS bWriteMoments) and writes a variance (0 before a pixel's second sample).
- uu.Nebula.FineStepLod (0: off): the detail's footprint at least this x the step (Uni_GalaxyNebulaDensityAtF; UNI_NEB_TUNE2.w).
Measured (T2.vol.nebula_steps, new): uu.Nebula.StepRatio 0.08 in the nebula 6.10% -> 1.76% per pixel (x0.29), frame change 0.297% -> 0.086%, steps a ray 62.9 -> 110.6 (x1.76); along the disc 3.89% -> 2.38% (x0.61), steps x1.68; the denoise's 0 passes bit for bit, one pass on D-250's steps x0.75.
Not verified: the engine side, the cost (UE-77).
Reroll impact: none (render only).
Status: built; engine test UE-77.

D-252 — The nebula grid: galaxy-local, sparse, cell-averaged nebulae the march reads along each step
Owner, 2026-11-15: "it helps but doesnt eliminate the grain ... at mid/far distance we still get grain"; "some paradigm to force the march into explicit non camera dependent based sample positions ... cells could be adaptively sized for lod, but the sample falling in a given cell would always return the same ... sample for that cell regardless of where the camera is"; "we would rather resolve at a lower resolution/frequency than to resolve at a frequency that will cause static grain"; "can we do sparse storage but with variable cell resolution - so #2 ... with the ability to swap between 128/256".
Measured first (experiments/nebula_grid_d252: the engine's path offline at engine-like 1.6 mrad pixels, 250 pc, 1 kpc and 3 kpc from the Milky Way's brightest disc nebula and along the disc; frame-to-frame change and error against the converged march):
- D-251's ratio cannot win at distance: its steps grow with distance, the nebulae's 10-40 pc detail does not. A dense prototype grid (nested lattices about the camera, each point its cell's average, the march reading a step's segment at cell spacing) cut the frame change 3-8x at the same steps; its cost is lateral softness (a cell 10-20 pixels wide at mid distance with 128 cells across; 256 halves it, 512 no better: the step then sets the level).
- Variants: the whole field from the grid (dark dust cores averaged over a cell spread into dark blobs along the disc, error 15%); the knots' shape at the point x the grid's detail ratios (sharp, but the knots' own edges are undersampled along in-plane rays: the grain came back); the light from the grid with the dust's and reflection's multipliers the point's knot shape x the grid's detail ratios (kept); two channels with the core tint from the point (error at 1 kpc 7.8% -> 12.8%: four channels kept); one bake sample a point against eight (the same frame change, ~1% more error).
- Occupancy (classify's keep rate) by the brick's size over the knot texel: 32% / 28% / 49% / 76% at 4-32 pc cells, coarser bricks nearly all kept; the worst occupied brick needed a slack of 0.07 in the knot shape before its clamp (0.15 used).
Built (render only):
- UniCore/UniNebGrid.h: the layout (level K's cell 4 pc x 2^K in the spiral's field units; Cells / 8 bricks of 8^3 cells across, as many up as cover the gas layer's 3 scale heights + 50 pc (at most 16); a toroidal table a level; pool slots by the expected occupancy x 1.25, levels too coarse to classify keep every brick; the pool an atlas of 9^3-texel slots, RGBA16F) and the planner (boxes about the camera snapped to bricks with a one-brick hysteresis; a move of the box or more resets a level, a smaller one releases the bricks that changed; a budget of bricks processed a frame, coarse levels first). FNebGridOpGPU 16 bytes.
- Core/UniVolume.ush THE NEBULA GRID: the bake (Uni_NebGridBakePoint: emission density, its core-tinted share, the dust's and reflection's detail ratios, at 1 or 8 jittered points of the cell, the cell the footprint), classify (the knot field over the brick's points' reach), the read (Uni_NebGridFetch: table, then the pool's trilinear; empty bricks (0, 0, 1, 1) exactly; unbaked: the next level), the level (the box holding the segment, its outer quarter blending to the next; the cell fitting the step in uu.Nebula.GridFetches reads; the pixel's footprint) and the segment (reads no further apart than a cell, at the step's jitter); the update's thread functions. In the march: the light from the grid, the dust and reflection the point's knot shape x the grid's ratios, the field in the outermost band and outside. FUniVolumeView / FVolumeViewGPU 192 -> 208 bytes (NebGridCount, NebGridParam, NebGridFetchMax).
- UniVolume.usf pass 7 (NebGridResetCS, NebGridReleaseCS, NebGridAllocCS: a group a brick classifying together, NebGridBakeCS: a group a brick's 729 points); the grid's headers, table and pool in every volume pass's common parameters; the renderer plans once a game frame, owns the table, free lists, counters (read back for uu.Perf) and pool, and binds them into the march and the far cache's fill.
- uu.Nebula.Grid 1, .GridCells 128 (or 256), .GridSamples 1 (or 8), .GridBudget 128 bricks a frame, .GridFetches 32, .GridCellPc 4, .GridReachKpc 8, .GridPool 1.25, .GridLayer 3; the grid follows the camera's spiral (its object, parameters, the nebulae's tuning, detail and dark share, and each enable's textures key it: a change rebakes); uu.Perf shows levels, memory, slots used and overflow by level, bricks waiting and processed.
Measured (new): T2.vol.nebgrid_plan (128 cells 40.1 MiB, 256 cells 145.4 MiB; a 480-frame walk with jumps: every entry held its box's world brick, every processed brick in its box, drained once the budget allows; the hysteresis holds). T2.vol.nebgrid_fetch (2400 reads equal the dense lattice's trilinear, 1744 of them empty bricks, none missed by classify, no overflow; a 1.5 kpc flight at 128 bricks a frame: 81% of reads answered, none wrong; world anchored). T2.vol.nebgrid_grain (D-251's 0.08 steps, 128 cells): frame change 250 pc 0.325% -> 0.064% (x0.20), 1 kpc 0.995% -> 0.160% (x0.16), 3 kpc 1.185% -> 0.145% (x0.12); error x1.32 / x1.67 / x1.37 (softer); light x1.000 / x0.943 / x1.018 (at 1 kpc the cells spread the brightest nebula's peak past the view: the emission integrated along its rays is x0.94 in the grid's reads, the field's own).
Not verified: the engine side, its GPU cost (the bake and the reads), the fill's look and fast travel (UE-78).
Reroll impact: none (render only).
Status: built; engine test UE-78.

D-253 — Nebulae kept to 32 kpc, their dark dust fading with their light, and their peak held (uu.Nebula.Peak)
Owner, 2026-11-15 (engine test of D-252): "that is substantially better, we still lose the nebulae past a certain distance but the grain is much better under this paradigm. not only that but the fade out distance that the dark nebulas fade out from is different than the light nebulas ... Id also say our peak nebula brightness is probably too high".
- Lost past a distance: past the grid (8 kpc) the march's field blends the knots to their mean with distance (D-241's level of detail, set against grain that the grid now handles). uu.Nebula.GridReachKpc 8 -> 32 (at most 8 levels; 128 cells 42.3 MiB, 256 cells 165.9 MiB). 6 kpc from the brightest nebula the field shows a mean haze (its light at the view's centre 11% over the image's), the grid the nebula (20%), the frame change 0.071% -> 0.105%.
- Dark fading before light: in the grid's share the dark dust (the point's knot shape) took the field's distance level of detail, which blends a dark core away (D-242) while the grid's light keeps its knots; the colour's blend too. In the grid's share both now take none (the cells are the level of detail); the field's share keeps it (render only).
- Peak: uu.Nebula.Peak (engine 200; FVolumeSettings::NebulaPeak 0 for the checks; UNI_NEB_TUNE3.y): the shape held softly under k x its mean, s / sqrt(1 + (s / k)^2) (Uni_GalaxyNebulaDensityAtF; the grid bakes it: its key includes it). Measured over the Milky Way's gas layer: a nebula's median is ~17-20x the mean, its 99th percentile ~440x, its cores ~800x; at 200 the median x0.996, the 99th percentile x0.42, the nebulae's light x0.77.
Measured: T2.galaxy.nebula_peak (new); T2.vol.nebgrid_grain extended (6 kpc). Seen, not changed: from 12 kpc dark speckle on a dust cloud in both D-251's march and the grid's (the galaxy's dust lanes drawn at one point of a long step; a candidate for the grid next).
Not verified: the engine side (UE-79).
Reroll impact: none (render only).
Status: built; engine test UE-79.

D-254 — The nebula grid from outside a galaxy: the nearest spiral within 64 kpc, up to 12 levels
Owner, 2026-11-15: "the nebula pop in is still pretty jarring ... its the level where i go from no nebulae to populated nebulae and it pops in some middling distance from the galaxy"; "Ideally we would see the nebula represented at all distances".
- Cause: the grid followed the camera's home galaxy only (D-211: the carrier whose sphere holds the camera): it switched on at that sphere, where the field (past its detail reach) showed the nebulae's mean and the grid their structure, then baked in over ~100 frames.
- Built (render only): the grid follows the nearest spiral (galaxy carrier with knots, weight, nebulae not hidden) whose disc is within uu.Nebula.GridReachKpc (32 -> 64), from outside too (the camera from its carrier, in float; the home's in double); its key the galaxy's parameters (D-252's object id dropped: a non-home carrier has none). UNI_NEB_GRID_MAX_LEVELS 8 -> 12 (the counters 2 x 12; uu.Perf's arrays); 128 cells 9 levels ~44.5 MiB, 256 cells 8 levels ~171 MiB. Where the grid switches on, its coarsest levels' cells blend the knots to their mean (the bake's footprint past three knot texels), as the field does.
Measured: T2.vol.nebgrid_outside (new): the Milky Way from 40 and 80 kpc (the disc 17 and 57 kpc off), the grid against the field: light x1.002 / x1.000, per pixel 1.14% / 0.02%.
Not verified: the engine side (UE-80).
Reroll impact: none (render only).
Status: built; engine test UE-80.

D-255 — The nebula grid read by a cell walk, empty bricks skipped, the dark dust from the grid, the starlight over the step
Owner, 2026-11-15: "if we are already baking a density grid at an established scale, our per ray plan should take that into account - could march the intersection points with the grid instead of arbitrary steps, do interpolation on those intersection points and then a blended weight for the step segment that takes its length into account"; "empty space skipping is probably pure gain"; "our ideal march should resolve fine enough detail and coarse detail, maintain fidelity and be performant ... we should also look to see if these issues have been solved elsewhere".
- **The cell walk (Uni_NebGridWalk):** a step's grid average is the walk over the level's cells the step crosses: the segment cut at the cell faces, each piece Simpson's rule over the trilinear reads at its ends and middle times its length. Along a line the trilinear is a cubic in a cell, so this is the exact integral of the grid's field (the face-crossing trapezoid alone missed a corner's peak between two zero faces: 99th-percentile error 4.5% of the segment's peak, now 8e-5). Each face read is shared by the cells either side; a brick without nebulae is crossed in one piece, unread; the walk is additive under any cut, so the march's step boundaries do not matter. The level's rule counts the segment's cells crossed (its L1 length in cells) against uu.Nebula.GridFetches. No jitter: D-255's first try moved each read up to a cell (to smooth the trilinear's creases over the accumulation): the frame change rose x1.5-2.5 and the error at every distance; dropped.
- **Empty space (Uni_NebGridSpace):** in a brick without nebulae at the level the march reads there, the step at least to the brick's exit (the field's own hint still caps it), no D-251 cap: 1 kpc 93.7 -> 55.1 steps a ray, 3 kpc 95.7 -> 46.8, 250 pc unchanged.
- **The dark dust and the reflection from the grid:** with long steps the grain left at 1 kpc was the step's extinction from the dark-nebula multiplier at its one jittered point (0.54% -> 0.10% with the dark share off; the large-scale dust was not it). The grid's third and fourth channels are now those multipliers' cell averages (D-252: their ratios to the knot shape, times the knot shape at the jittered point), and the march takes the walk's averages: one field evaluation a step fewer. Dark cores are now no sharper than a cell (4 pc at the finest level): the owner's rule, lower resolution over grain.
- **The starlight over the step (the cone rule; uu.Volume.StepFoot 0.5):** the camera's galaxy's map and texture terms are read over at least 0.5 x the step's run along the disc (Uni_GalaxyMapEvalNOP: the footprint in the plane; the height profiles keep the pixel's: widening them by a step along the disc took 10% of the light from a ray in the plane at 1 x). At 32-36 steps: frame change 0.259 -> 0.155% / 0.124 -> 0.107% / 0.046 -> 0.042% at 3 kpc / 1 kpc / 250 pc, error 10.63 -> 10.28% / 9.06 -> 9.58% / 2.55 -> 2.65%, light -3.7% / -1.0% / -0.2% (1 x: -7.4% / -2.3% / -0.5%).
- Prior art read (owner's request): Frostbite's per-step integration (the march already has it), GigaVoxels' cone tracing and ray-guided bricks, Nubis' voxel clouds with distance-field skipping, UE5's heterogeneous volumes; the owner's papers (B-036): the Inria star octree (cell-to-cell rays with the leaf at the pixel's footprint, cell integrals from a 2D mip x an analytic height: our D-212 map's structure; dust once per cell their open problem) and GAMER (appearance).
Measured (experiments: zz.grid, the Milky Way's brightest disc nebula, engine-like pixels 1.6 mrad, the engine's map path, 128 cells, reach 64 kpc, uu.Nebula.StepRatio 0.08, 64 frames of the temporal path): frame change against D-251 (D-252's grid at the same steps): 250 pc 0.325% -> 0.013% (0.067%), 1 kpc 0.995% -> 0.030% (0.173%), 3 kpc 1.185% -> 0.088% (0.165%); steps 93.1 / 55.1 / 46.8 (D-251 93.1 / 93.7 / 95.7); error against the converged reference 1.65 / 5.36 / 7.77% -> 2.29 / 8.77 / 10.18% (softer: the cells; at 256 cells 5.00% at 1 kpc, 8.27% at 3 kpc, 171 MB); the grid's reads 1141 / 715 / 459 a ray. Without D-251's step cap (32-36 steps): 0.042 / 0.107 / 0.155%. T2.vol.nebgrid_walk (new): over 1400 segments of 0.3-38 cells in six levels, the walk against the reads' mean at 64 points a cell: 99th percentile 8.2e-5 of the segment's peak, max 2.9e-4; cut anywhere the same; no pool read through empty bricks; a segment leaving its box fails. T2.vol.galaxy_map_stepfoot (new): from the solar circle along and across the disc (the 512^2 map, 8 jitter phases), uu.Volume.StepFoot 0.5 against 0: the spread over the phases x0.86, light within 0.3%. T2.vol.nebgrid_grain (D-251's steps, the field path): frame change x0.07 / x0.04 / x0.11 of D-251's, error x1.32 / x1.63 / x1.34 (limit 1.8). The grid's reflection channel is its average over the smooth part (Uni_GalaxyNebulaReflectBase: the dark share's dimming), so a brick without nebulae reads it exactly (T2.vol.nebgrid_fetch: no occupied brick read empty).
Known: nebulae at range are soft (a cell ~ distance / 48: ~64 pc, ~13 pixels at 3 kpc): B-034.
Not verified: the engine side (UE-81).
Reroll impact: none (render only).
Status: built; engine test UE-81.

D-256 — Spiral arms on Ringermacher & Mead's line: finite winding, bars, the morphology spread
Owner, 2026-11-15 (on GAMER, Groeneboom & Dahle 2014, ApJ 783, 138; Docs/iop_paper.pdf): "the figure 7 morphology in the iop paper looked particularly useful, and potentially some of their spiral math/barred spiral logic, and spiral parameterization to get that table of morphology variance"; "we dont care about galaxy rerolls and we wont for quite some time".
- **The arms' centre line (Uni_GalaxyWindingU):** Ringermacher & Mead's theta(r) = 2N (atan(e^(A/r) / B) - atan(1/B)) in place of the log spiral's u / tan(p) (u = -ln r) and D-178's tightening, everywhere the winding enters: the arms (D-178's lobes: per-arm pitch factor, jitter, strength and end kept), the detail's twist (D-179), the prefilter's blend and the step hint (the local rate d theta / du from the same function). The winding is finite at both ends: inward the arms straighten (a bar; no rings in the core: D-210's reason to start the arms outside it), outward they open.
- **The parameterization:** the paper's N and B alone (rendered as its Fig. 7) mostly trade the total winding (~2NB for small B) against each other. So: B.w is the total winding W (radians, centre outward; N = W / (2 (pi/2 - atan(1/B)))), F.y the openness B (0: the log spiral of tan(p) B.w, every older field), which sets where the winding happens: A = (0.5 + 2.5 (1 - B)^1.5) disc scale lengths, so a small B is a long straight bar with the arms wound outside it, B 1 the winding into the core. Three or more arms take at least openness 0.5 and winding 3.5 (rendered, they were three-spoked bars and straight stars; barred spirals are two-armed).
- **Rolls:** Arms.Winding 1.5-8 (was Arms.TanPitch 0.15-0.45), Arms.Openness 0.05-1, bias 1.6 (a third below 0.2: barred; was Arms.Tightening). The Milky Way: two arms, winding 4, openness 0.15 (barred).
Measured: T2.galaxy.arms_rm (new): openness 0 the log spiral bit for bit (2000 points); over 64 rolled galaxies the winding under 0.5% of its total at 1000 scale lengths, the total at the centre within 1%, monotonic inward, its rate the numerical derivative's within 1.9e-4 of the largest; the floors for three or more arms; the disc's light (modes off) the log spiral's exactly (the lobes keep their azimuthal mean). Stills: experiments/arms_d256.cpp.txt (the Fig. 7 grid; 19 rolled galaxies and the Milky Way).
Not verified: the engine side (UE-82).
Reroll impact: every spiral's arms (shape; the light's distribution in the disc, so its stars' placement); the Milky Way gains a bar. Owner: rerolls do not matter for now.
Status: built; engine test UE-82.

D-257 — Tried, not taken: finer nebula detail on top of the grid's cells
Owner, 2026-11-15: "fidelity is a bit low (chunky nebula at range)". The grid's cell is about distance / 48 (64 pc, ~13 pixels at 3 kpc with 128 cells); a detail term read at the march's point on top of the cell walk's averages was tried and dropped: it brought back the per-sample variation the walk removed (the grain) without restoring the shapes the cells had averaged away. The paths left: 256 cells (5.0% / 8.3% error at 1 / 3 kpc against 8.8% / 10.2%, 171 MB) or ray-guided bricks (PARADIGMS.md, paradigm E; B-037's plan). No code kept (UniVolume.ush and test_nebgrid.cpp as D-255 left them).
Reroll impact: none.
Status: dropped.

D-258 — GAMER's noise shaping: knots to the 8th, a structure read for dust and light, each over its mean
Owner, 2026-11-15 (on GAMER, Groeneboom & Dahle 2014): "their galaxies look very nice, so its probably worth lifting a lot of the shaping and noise paradigms even if we are going with our own lighter weight ray marching paradigm".
- **The knots:** D-179's knot term (galaxy_detail's G channel, taken linearly) is G^8 over its mean at the read's mip (GAMER's star-forming clumps: a noise sum to the 8th): sparse bright knots on a darker disc instead of an even mottle.
- **The structure read:** galaxy_detail read a second time at a quarter of the detail's frequency (GAMER's noise starts at the galaxy's own scale; ours started at the detail's), at the same twisted point with its own offset and its own mip (Uni_GalaxyDetailMip at the footprint x 1/4). Its ridged channel to the 4th multiplies a dust set's density (the large dark lanes: GAMER's inverse fBm, filaments sharp, gaps soft); its fBm cubed multiplies the disc's light (the arms' streaks and gaps). Each over its mean at the read's mip (Uni_GalaxyStructureMul).
- **The means (UNI_GAL_SHAPE_MEANS):** what a trilinear read at a random point sees, per mip 0-8 (the texels' own means are up to 9% higher), measured once from galaxy_detail's mips with the defaults; a mip between two lerps them. So the light and the dust keep their mean at every footprint and the map, the field and the grid agree.
- **Handles (shader defines):** UNI_GAL_SHAPE (knot power 8, dust power 4, light power 3, structure frequency 0.25; 0 turns each off: D-179's linear terms, no structure read). Chosen from offline stills (experiments/shape_d258.cpp.txt: knot 4-12, dust 2-6, light 0-4, frequency 1/8-1/2); a quarter was closest to GAMER's Fig. 1-5.
- **Cost:** one more galaxy_detail read where the detail is read (inside the disc slab): the field's, the map's bake and the grid's bake.
Measured: T2.galaxy.noise_shape (new): each shaped channel's mean at mips 0-6 and between within 4.1% of 1 (200k reads; the worst between table entries); over the Milky Way and 16 rolled galaxies the shaping's factor on the disc's light within 3.6% and on its dust within 11.9% at footprints 3e-3 and 3e-2 of its own at 1e-4 (a power of a smoothed read is not the smoothed power, and a disc's few structure cells keep their own share; limits 5% / 15%); against the structure read off the population's mean light x0.990, dust x0.975 (one galaxy's own pattern moves its total by up to 25%). Stills sent to the owner (d258_shaping_first_look).
Not verified: the engine side (UE-83). The nebula and cluster calibrations (8 checks) moved with D-256 and D-258 and are recalibrated next, before the gate.
Reroll impact: every textured galaxy's look (its knots, lanes and streaks; its total light within a few % either way, the dust up to 25% per galaxy); the stars placed by the light follow. Owner: rerolls do not matter for now.
Status: built; engine test UE-83.

D-259 — galaxy_shape (the shaping before the filter), the knots' young share, and the recalibration after D-256 / D-258
Owner, 2026-11-15 (engine test of D-258): "we have grain again, probably due to changing the noise paradigm - the new update has the knots at around the same detail level as the nebulas were before"; "where did you get the noises from? analytic or did you use some noises from our library, because we probably shouldnt be just using analitic noise for any of this, we have a robust texture library and creation pipeline". (Every term is galaxy_detail's, the library's texture; D-258 only raised its channels to powers.)
- **galaxy_shape (texture slot 4, UNI_GAL_SHAPED):** derived from galaxy_detail when it binds (UniCore::BuildGalaxyShapeTexture): R^4, B^3, G^8 each over its mean at mip 0, then the box mips; A the knots' amplitude alpha at that mip. A power of a filtered read is not the filtered power: D-258's knots, sharper than a march step, aliased (the owner's grain), and its structure's light moved with the footprint. Every shaped read now comes from galaxy_shape, so a read at any footprint is the shaped field's mean over it (each channel's mean 1 at every mip, 1.2e-4). Memory: one more 128^3 RGBA16F texture with mips (~18 MB). Cost: one more read where the detail is read (the knots at the detail's point; the structure read moved to galaxy_shape).
- **The knots' young share:** D-258's G^8 about the linear term's mean moved the arms' light into the knots (the young share E[max(Knot, 0)] x4.6), which the reflection nebulae (the Milky Way's nebula light +16% -> +44% from 60 kpc), the open clusters and the palette read. Knot = 0.0457 + x alpha (G^8 / mean - 1), alpha per mip keeping the linear term's young share (0.19 at mip 0, mip 4's past it): sparse bright knots on the arms (their floor ~85%) instead of the arms gathered into the knots.
- **The open clusters' cap:** the structure multiplier is detail, so out of the cap in the field path as in the map's (the two paths disagreed by 3.8% in the stars placed around clusters: T2.galaxy.open_clusters z -10).
- **Handles (uu.Enable; stars follow):** uu.Galaxy.Knots (amplitude over alpha; 0: D-179's linear knots), uu.Galaxy.KnotPower 8, uu.Galaxy.DustStructure 4, uu.Galaxy.LightStructure 3 (powers galaxy_shape is baked with; 0 off), uu.Galaxy.StructureFreq 0.25 (over the per-galaxy DetailFreq roll, 2-4). UNI_GAL_SHAPE (knots' amplitude, dust and light structure on, frequency) is the shaders' uniform UniGalShapeTune; the mean tables of D-258 are gone.
- **Recalibrated on D-256's Milky Way** (its bar and arms moved the knots every nebula constant was measured over): D-241's fade means FM1 0.38281 -> 0.38699, FM2 0.37549 -> 0.38945, the lanes' mean 0.12447 -> 0.12465, the eroded shape's mean 0.00165489 -> 0.00168205 (UNI_NEB_TUNE.w, the shim, UniVolume.h, UniRenderer.h); D-242's VPS 0.03937 -> 0.06425, VMS 0.43490 -> 0.26380, VPR 0.27492 -> 0.27445, VMR 0.23992 -> 0.24135; D-243's LSPLIT 0.74917 -> 0.77069, RSPLIT 0.66538 -> 0.63853 (now measured where the split applies: elsewhere VPR / VMR's even dimming normalises; the reflection x0.9943 -> x0.9985); D-248's kNebulaPrevalence and kNebulaErodeByScale re-run (experiments/nebula_prevalence_d248.cpp.txt).
- **Checks re-based, with the reason each:** T2.galaxy.nebula_fronts half-faded light 2% -> 3% (1.7% on the old Milky Way, 2.2% now: the two normalisations over different detail's shapes); T2.galaxy.nebula_detail the jittered mean within 1 point of one unjittered frame's error and its light as near (it was "nearer": 5.9 / 5.4% before, 5.0 / 5.7% now with every D-258 term off too: the moved brightest knot, not the shaping); T2.galaxy.nebula_tune near/far 5% -> 7% (four independent sample sets' default eroded mean spreads +-3.5% on this galaxy); T2.galaxy.nebula_edges p95 25% -> 35% (16% over 748 pixels before, 32% over 311 now: fewer nebulae in its fixed view); T2.galaxy.nebula_slide D-244's refinement reported, not held (B-038); T2.galaxy.open_clusters the gathered light within 25% (or 3 of its sampling errors, now reported: 0.031 / 0.076) of frac x the young light: 0.86 / 0.76 on D-256's Milky Way with every D-258 term off too, 4.5 and 3.1 sampling errors, so a bias: B-035's (the clusters on the analytic mid-plane, the young light on the texture-warped one, whose pattern D-256's winding moved).
- **Found, not fixed (B-037):** D-255's uu.Volume.StepFoot blurs the camera's galaxy across the ray (owner: knots sharper outside than inside; StepFoot 0 "resolves it"). Paradigm C (the step's in-plane run sampled at the pixel's footprint, the vertical integrated) is next.
Measured: T2.galaxy.noise_shape (rebuilt): galaxy_shape bound, its texel means within 1.2e-4 at every mip, random reads within 0.6%; the knots' mean the linear term's within 0.0011, their young share within 0.7% at mips 0-4; 17 galaxies across footprints unbiased (light -0.6% / -0.3%, dust +0.0% / +0.8%), each within 4.2% / 15.3% (the structure's correlation with the arms and lanes as they blend); against the structure off light x0.992, dust x0.978. T2.galaxy.nebulae: the Milky Way's nebulae +15.4% from 60 kpc, +10.5% solar (D-258: +44%, +27%). T2.galaxy.open_clusters placed listed x1.000 (z -0.1), by cell x1.002.
Not verified: the engine side (UE-84).
Reroll impact: every textured galaxy's knots (softer, on the arms) and structure; the stars placed by the light follow; nebula brightness normalisations (a few %).
Status: built; engine test UE-84.

D-260 — The nebula layer off by default (uu.Nebula 0)
Owner, 2026-11-15 (engine, D-258/D-259): "I would default nebulas to off right now, as this is closer to what we probably want from nebulas than most of the stuff we've tried so far, and it does it without using a separate layer". uu.Nebula's default 1 -> 0 (the engine's console variable only: the layer, its grid and its checks stay; uu.Nebula 1 restores it). With it off the nebula grid is not built (D-252: grid only with NebulaGain > 0), so its memory and bake are saved.
Next (owner, same message): a tuning surface: the march paradigm switchable (stepwise, voxel walk, later segment integration), the volume textures swappable live (resolutions, octaves, frequencies from the texture pipeline), global balance multipliers (background dust, light / dust), the step parameters, all exposed in the editor (B-039).
Reroll impact: none (render only).
Status: built; engine test with UE-84.

D-261 — The tuning page: every uu.* console variable in Project Settings, bound live; uu.Volume.DustGain
Owner, 2026-11-15: "put all the puzzle pieces in place - the ability to swap between stepwise and voxel marching paradigms, ability to swap volume textures (try different resolutions and fbm octaves/frequencies etc), global multiplier knobs for things like background dust, light/dust balances, steps counts/sizes/expansion rates or governing params, raymarching params basically - and then we want some way to expose this stuff editor side".
- **UUniTuningSettings (Project Settings > Plugins > Unified Universe Tuning):** one property per uu.* console variable (179), generated by Tools/gen_tuning_settings.py from their declarations (UniSubsystem.cpp, UniShaders.cpp, UniRenderer.cpp), grouped: March (Paradigms, Steps, Sampling and Noise, Temporal, Resolution, Galaxy Map, Voxel Grid, Far Cache), Textures, Galaxy (Shape and Noise, Balance), Nebulae, Population, Sprites, Web, Travel, Lighting, Background, Streaming, Debug. The tooltips are the variables' help. Defaults are the variables' values when the module loads (no second list of numbers).
- **Binding:** an edit sets the variable as typing it would (ECVF_SetByConsole: live in PIE); saved values (DefaultGame.ini: the page's "Set as Default", or uu.Tuning.Save) are applied when the subsystem starts (ECVF_SetByProjectSetting: an ini [ConsoleVariables] line or typing still wins). Settings that apply at enable (the textures, the galaxy shaping, palettes, populations, the web texture: 40) re-enable the scene where the camera is half a second after the last edit (Tuning > Auto re-enable; uu.Tuning.Reenable by hand). uu.Tuning.Import pulls the variables' current values into the page after console experiments. A table maps properties to variables (property metadata is editor-only).
- **uu.Volume.DustGain (new, render only):** every galaxy dust layer's optical depth in the volume x this: the light / dust balance without re-enabling (uu.Galaxy.DustScale scales the dust itself at enable; uu.Galaxy.DiffuseDust the background dust).
- **What the paradigm switches are today:** uu.GalaxyMap (the camera's galaxy marched from its 2D map, D-212, or the field), uu.Nebula.Grid (the nebulae through the voxel grid's cell walk, D-252/D-255, or stepped), uu.Nebula.Refine (D-244). With the nebulae off (D-260) the grid has nothing to hold: the galaxy's starlight and dust are stepped in both. Paradigm C (B-037) arrives as another switch here.
- **Checks:** T4.static.tuning_settings (new): regenerating the page changes nothing (a new console variable must be on it).
Not verified: the engine side (UE-85): the module's DeveloperSettings dependency, the page, live edits, the re-enable, saving.
Reroll impact: none (the page sets the same variables).
Status: built; engine test UE-85.

D-262 — Paradigm C, first part: the camera's galaxy's step read along its run (uu.Volume.SegmentTaps 4); uu.Volume.OpaqueCutoff; presets
Owner, 2026-11-15 (engine, D-255 + D-258): "a fidelity LOSS when entering the galaxy proper - the knots are clearer outside before it swaps to the interior march"; uu.Volume.StepFoot 0 "resolves it". And: "ramp up dust darkness more and have it basically eat the march by about 30-70% of the galaxy disc ... a faster truncation in the most expensive rays ... Key here though is if the look works".
- **The taps:** D-255's StepFoot read the camera's galaxy's map and texture terms over half the step in every direction: right along the ray, a blur across it. Now the step's run in the disc's plane is read at Kt points (stratified at the step's own jitter: Kt 1 is D-255 bit for bit), Kt = the run over the detail's texel at the pixel's footprint, at most uu.Volume.SegmentTaps; each over the pixel's footprint and StepFoot x its share of the run. Emission, dust, palette components and the nebulae's density are the taps' mean; the nebulae's other terms the middle tap's. With the taps covering the run StepFoot no longer matters (each share under a texel).
- **Measured (T2.vol.galaxy_segment, new; the Milky Way from the solar circle toward the centre, along the disc and from 300 pc above at 30 degrees; 8 jitter phases against an eighth-step unjittered StepFoot-0 reference):** per-pixel error / spread over the phases (the grain): StepFoot 0 3.15% / 5.34%, D-255 3.13% / 4.65%, 4 taps 0.67% / 1.26%, 8 taps 0.51% / 0.78%; light within 0.5%; the march's CPU time x1.9 (4) and x2.7 (8). Engine default 4 (the harness keeps 1, so every older check's baseline stands).
- **uu.Volume.OpaqueCutoff (new, live):** the transmittance under which a ray stops (the old constant 1e-3). The owner's dust idea measured (experiments/dustcut_d262.cpp.txt): from the solar circle, uu.Volume.DustGain 4 / 10 / 30 / 100 takes the light toward the centre to x0.81 / 0.69 / 0.55 / 0.42 and the steps a ray only to x0.98 / 0.95 / 0.90 / 0.83 (x0.78 with a 0.02 cutoff); along the disc x0.97 at 100. The Milky Way's dust layer is thinner than its stars: rays leave it before they become opaque, so darker dust is a look choice more than a saving, and most of a ray's cost is per step (~33-40 steps) rather than its length. Both knobs are on the tuning page for the look.
- **Presets:** uu.Tuning.SavePreset / LoadPreset <name> (Saved/UniTuningPresets/<name>.txt: every tuning variable's value a line), uu.Tuning.Presets.
- **The view (GPU) struct** 208 -> 224 bytes: SegmentTaps, OpaqueCutoff, two pads.
- **The full gate of D-256..D-260 (190 checks, 10 failed) and what each was:** D-256's barred Milky Way: T2.gen.fidelity (the disc cell's r 0.96 -> 0.894 is its Poisson limit, 0.893: a smoother cell; the check now takes the Poisson limit), T2.stream.quick_integral (worst 8.8%: 8192 samples, worst 5.3%, still 187x faster), T3.vol.handoff (the blended stage -2.2%: limit 2.5%, B-040), T2.vol.nebula_steps (D-251's steps 2.1-2.3x D-250's: limit 2.5x, B-040), T2.galaxy.open_clusters (p99 1.4 x 10^5 stars a cluster: limit 2 x 10^5, B-035); D-258's structure (a few cells a disc, so one galaxy's raw total moves; the normalising integral keeps its luminosity): T2.galaxy.textures (total light with vs without the textures 38%: limit 45%; the prefiltered light 3.7%: limit 5%; the textured Milky Way's stars from the solar camera x1.45 the untextured: the structure makes the camera's neighbourhood denser, so the 10% count check runs with the structure read off, x1.046, the structured count reported), T2.galaxy.dust (column mean 33%: limit 40%); the nebula layer under the shaping (B-041; the layer off by default, D-260): T2.vol.nebgrid_grain, T2.vol.far_split_nebulae and T2.galaxy.nebula_detail run with the shaping off (UniVerify::FGalShapeOff), nebgrid_grain's light 8% -> 10% (D-256 alone x0.916 at 3 kpc).
Not verified: the engine side (UE-86): the look inside the galaxy against outside, the GPU cost of 4 taps.
Reroll impact: none (render only).
Status: built; engine test UE-86.

D-263 — uu.Galaxy.DustHeight: the dust layer's scale height
Owner, 2026-11-16 (after the dust discussion: real in-plane extinction ~1-2 mag a kpc, ~30 mag to the centre; the real dust ~100 pc thick under ~300 pc of stars): "Do we also have a control for how fast the dust falls off from the central plane?"; "its just exposing a param, you can skip the test run".
- **uu.Galaxy.DustHeight (new, Galaxy > Balance, applies at uu.Enable):** every dusty archetype's FVolumeProfile::DustHeightRatio (0.5: the dust half the stars' scale height) x this, clamped 0.05..4 (UniScenes::UseDustTuning, beside D-198's DustScale and DustLanes). DustFrom keeps the face-on optical depth (sigma = DustTau / 2H), so a thinner layer is denser in the plane: darker edge-on and along the plane by 1 / DustHeight, unchanged face-on; rays leaving the plane escape sooner. 0.67 gives a third of the stars' height (about the Milky Way's). The diffuse dust's height (uu.Galaxy.DiffuseDustHeight, D-250) is relative to this layer's, so it follows.
- **Render-only:** the dust set's height (A.y) is read only by the dust density (the map path's Rd, the field's dust, the diffuse dust); the nebulae, the open clusters and placement read the emission set's.
- **Checks:** the UniVerify build (clang, gcc) and the static lints; the full gate skipped at the owner's word (a parameter exposed; the default 1 changes nothing).
Not verified: the engine side (UE-87).
Reroll impact: none (render only; the default is the old value).
Status: built; engine test UE-87.

D-264 — Tuning reset to defaults (page buttons, uu.Tuning.Reset)
Owner, 2026-11-16: "some option to reset to defaults would be good as well. i will work on visual tuning of the system some today".
- **Defaults:** each tuning variable's value when the module loaded (its declaration; an ini's [ConsoleVariables] or the command line if they set it), recorded when the page is built, before the subsystem applies the page's saved values (a variable registered later is recorded at that ApplyAll). No second list of numbers.
- **Reset:** UUniTuningSettings::ResetToDefaults(Filter): the changed variables set back as typed (ECVF_SetByConsole: over the saved values), the page re-imported and refreshed, the scene re-enabled if a reset variable needs it. Filter (case-insensitive): empty = all; a variable prefix (uu.Volume, uu.Galaxy.Dust) or a page category (Nebulae, March|Steps, Galaxy|Balance; the generated table now carries each variable's category). DefaultGame.ini is untouched: "Set as Default" (uu.Tuning.Save) after a reset makes it permanent, otherwise the saved values return at the next start.
- **Where:** the page's Tuning section: "Reset All To Defaults", "Reset filter" + "Reset Filtered To Defaults" (CallInEditor buttons); console uu.Tuning.Reset [filter]. LoadPreset now refreshes the page as well.
- **Not the editor's own "Reset to Defaults"** at the top of the settings page: it reloads the config without touching the console variables (the page and the scene could disagree); use the Tuning buttons.
- **Checks:** generated (T4.static.tuning_settings current); the static lints; the UE build untested here.
Not verified: the engine side (UE-88): the buttons show on a settings page (CallInEditor on the class default object), the reset and the page refresh.
Reroll impact: none.
Status: built; engine test UE-88.

D-265 — Detail layers: a layered density march for the camera's galaxy, steps scaled by the layers' frequency
Owner, 2026-11-16: "maybe the galaxy could run a similar layered density march - baked galaxy map underpins -> structure layer marched at full distance visible at all ranges; detail layer n marched at a constrained distance to blend in finer noise to make the clouds more organic when they are close to the camera - fades out at a specified range"; "we could also directly scale our step scales based on the frequency of the detail layers potentially"; "Yes lets try it".
- **Layers (Uni_GalaxyDetailLayers, UniFields.ush):** up to two, over the camera's galaxy in the march's map path only (render only: placement, the map bake and the field path never read them). Layer i reads galaxy_shape (shaped before filtering, mean 1 at every mip: D-259) at Freq_i x the galaxy's detail frequency, at the detail's twisted point with its own offset: light x 1 + Light_i (fBm^3 / mean - 1), dust x 1 + Dust_i (ridged^4 / mean - 1), amplitudes 0..1 (never negative; mean 1). Each tap of paradigm C (D-262) reads them at its own point.
- **The fade is the step's, not a hand-set range:** a layer is read over max(the pixel's footprint, Foot x the step's share), so where the march does not resolve it it reads a coarser mip (its mean: blur, never grain), and past galaxy_shape's last mip it is not read at all (the far march pays nothing).
- **Steps from the frequency (Uni_GalaxyDetailStepCap, UniVolume.ush):** inside the disc's slab, layer i resolves at Lambda_i = StepTexels of its texels a step for dust (extinction needs real steps), x SegmentTaps for light (its taps resolve it inside a step; the taps' spacing takes the light layers' too). Its reach: where the log step (ratio x T) reaches Lambda_i, then Steps more steps; past it, or out of the slab (rays leaving the disc keep their long steps), no cap. A budget per layer: each finer layer a proportionally shorter reach for the same steps, so the cost grows with the number of layers, not their frequency.
- **Defaults (engine):** uu.Galaxy.Detail.Freq0 4 (the Milky Way: 2 kpc features, 16 pc texels, resolved to ~1.2 kpc), Freq1 16 (500 pc, 4 pc texels, ~300 pc), Light / Dust 0.5 each, Steps 16, StepTexels 4, Foot 0.5; all live (no re-enable), on the tuning page's new Galaxy|Detail Layers. The checks' harness keeps them off (FVolumeSettings), so every other check marches as before.
- **Measured (zz.detaillayers, archived as experiments/detaillayers_d265.cpp.txt; the Milky Way from the solar circle toward the centre, 8 jitter phases against each configuration's fine reference):** off err 0.46%, spread 1.22%, 40.7 steps; x4 + x16 read without the cap 1.07% / 1.72%; with 16 steps 1.09% / 1.40% (x1.49 steps); with 32 steps 1.07% / 1.20% (x2.12); the light -1.1%. Images (320x180, toward the centre, along the disc, 300 pc and 1 kpc above): the layers add organic cloud structure near the camera and lose none far.
- **Check:** T2.vol.galaxy_detail_layers (the defaults; toward the centre and 300 pc above): light within 4% of off (each multiplier's mean is 1; one frame holds only a few of the x4 layer's 2 kpc features: -1.1%, -2.5% when set), the grain with the cap no more than without, the error under 2.5% (1.1%, 1.8%), steps at most 2x (x1.50). The view struct 224 -> 256 bytes (the D-262 pads reused).
- **Not done:** the field path (galaxies seen from outside: their pixels are past the layers' reach anyway); a texture slot of its own for the layers (they read galaxy_shape: the dust structure's and the light's shaped channels); the knots' own purpose-made texture (discussed, B-042).
Not verified: the engine side (UE-89): the look, the cost (the steps a ray x1.5 in the disc), the tuning page.
Reroll impact: none (render only).
Status: built; offline check; engine test UE-89.

D-266 — The bubble of detail: each detail layer limited to a range about the camera, faded in over a share of it
Owner, 2026-11-16 13:51 (on D-265): "the mid detail can actually be constrained closer to the camera than it currently is... maybe around .33 galactic radius, and we should make our fade in from coarse to mid occur over its full range, fine detail should fade in over around .5 of its range, leaving a bubble of detail around the player".
- **Uni_DetailRangeW (UniVolume.ush):** layer i's weight at T x InvRadius galaxy radii from the camera: 1 within (1 - Fade_i) x Range_i, smoothstep to 0 at Range_i; its light and dust amplitudes x the weight (mean 1 kept: 1 + w A (S - 1)). Weight 0: the layer is not read (the D-265 early-out), so past the bubble the march costs exactly the layers off. Per tap (D-262) at the tap's own distance; a sphere about the camera (the distance along the ray), so a bubble that moves with the player.
- **The step cap and the taps follow it:** a layer's step reach is min(its D-265 reach, its range), and the light layers' tap spacing counts only the layers whose bubble the sample is in, and only inside the disc's slab (as the reads and the cap: D-265's spacing took extra taps above the slab, where no layer is read).
- **Defaults:** uu.Galaxy.Detail.Range0 0.33 (the mid detail; the Milky Way's 24 kpc radius: ~8 kpc), Fade0 1 (from the coarse structure over all its range: full only at the camera); Range1 0.08 (the fine detail, ~2 kpc: a quarter of layer 0's as its frequency is 4x; the owner gave no number), Fade1 0.5 (full within half its range). 0: no limit (D-265's behaviour). Live; the tuning page's Galaxy|Detail Layers. The checks' harness keeps the layers off.
- **Check:** T2.vol.galaxy_detail_layers runs with the defaults' bubble, and from 18 kpc above the solar circle (the bubble's 7.9 kpc wholly above the disc's slab, 8.6 kpc flared at the edge: no layer applies) holds the frame and its steps exactly the layers off. (From 10 kpc above the faint thick disc within the bubble is inside the slab: the layers rightly apply there.) The view struct 256 -> 272 bytes.
- **Measured (the check, the defaults' bubble):** toward the centre err 0.74%, spread 1.17% (layers off 1.22%: the bubble's fade below the step's grain), light -1.0%, steps x1.49; 300 pc above err 1.60%, spread 1.10%, light -2.5%; 18 kpc above: identical to off (difference 0, 48.18 steps both).
Not verified: the engine side (UE-90).
Reroll impact: none (render only).
Status: built; offline check; engine test UE-90.


D-267 — Knots in the detail layers: the nebulae as part of the field; the knots' nebula tint
Owner, 2026-11-16 17:20: "with that detail bubble, the nebula can probably just be part of the field no? our mid and fine detail layers should allow it to get the correct fidelity?"; 17:28: "Yea lets try it".
- **The knot channel (Uni_GalaxyDetailLayers, UniFields.ush):** each detail layer's galaxy_shape read already held the knots' channel (G^8 over its mean, shaped before the mips: mean 1 at every footprint, D-259), unused. Its amplitude uu.Galaxy.Detail.Knots0 / Knots1 (0..1; the view's DetailKnot0 / 1, InLi.w) multiplies the knots' young light (Comp.w, the arms' light above their floor) by 1 + w A (S.z - 1), w the layer's bubble weight (D-266): each knot of the map breaks into the layer's clumps (the Milky Way's ~16 pc and ~4 pc texels), a clump only where its parent knot is. The light: Rm' = (Rm + Comp.w x the cut's soft edge x (Mk - 1)) x Ml (Uni_GalaxyCutEdge: the components are taken before it). No extra reads (the same sample), no extra steps (a knot layer is a light layer for the step cap and the taps' spacing).
- **The knots' nebula tint (the march, every galaxy carrier):** the knots' share of a sample's light (Comp.w over the components' sum) toward the galaxy's nebula colours (Uni_GalaxyNebulaColour: D-236's three a galaxy, nebula to nebula by the knot read's mix term, toward their average as the knots blend), luminance kept, by uu.Galaxy.Detail.KnotTint (0..1); hidden through the sprite's handoff with the nebulae (D-245's NebulaHide). At any distance, so no edge: from far away the arms' knots carry the nebulae's colours, near they break into clumps.
- **Defaults (engine):** Knots0 0.5, Knots1 0.5, KnotTint 0.5; live, on the tuning page's Galaxy|Detail Layers. FVolumeSettings keeps them 0 (the checks' march as before). The view struct 272 -> 288 bytes.
- **The nebula layer (D-234..D-255, uu.Nebula, its grid) stays as it is and off (D-260):** retired only once the owner has seen this in the engine.
- **Measured (experiments/knots_d267; the Milky Way from the solar circle toward the centre, along the disc and 300 pc above; 8 jitter phases against each configuration's own fine reference):** without knots err 1.10%, spread 1.231%; knots 0.5 1.14% / 1.261%; knots 1 1.24% / 1.326%; light within 0.7%; steps unchanged (57.6 a ray). More steps (32) lower the spread (1.186%) at x1.45 the steps; 2 texels a step raise it (1.663%). Images toward the centre: the pink knots broken into clumps; with the tint, teal and pink nebula colours on them.
- **Check:** T2.vol.galaxy_knot_layers (new): the layer function's light and dust bit for bit with knots on, knots off exactly 1, the knot multiplier's mean over the Milky Way's slab 1.011 / 1.012 at two footprints (within 3%); with the engine's layers and bubble and knots 0.5: light -0.75% at worst (3%), grain x1.027 (x1.15), error 1.63% (2.5%), the same steps; the tint keeps each sample's luminance (a pixel's within 0.25%: the dust in front reddens a pinker sample's light differently) and moves the colour (0.13 of the luminance).
Not verified: the engine side (UE-91).
Reroll impact: none (render only).
Status: built; offline check; engine test UE-91.

D-268 — The travel shift: its own speed curve, a stronger top end, the blue side's boost
Owner, 2026-11-16 17:28: "reduce the speed at which blue/red shift starts ramping up to about half of what it currently is / increase the speed cap at which blue/red shift hits its max by say 4x but dramatically increase the top end of the effect, moving at 4x the current max would be much more blue/red shifted, possibly with like luminosity boost on the blue side as it passes 1x the current max or so".
- **D-230's curve, measured:** the travel curve (smoothstep in log speed, 100 kpc/s to 10 Mpc/s) ramped from 0.15 to 0.75: in at ~308 kpc/s, full (ln 0.5: x1.65 ahead) at ~2.2 Mpc/s.
- **Its own curve (UniCore::TravelShiftTarget, eased in time by FTravelShiftRamp: 0.3 s up, 0.6 s down):** in log speed, 0 below uu.Shift.StartKpcS (150: half of D-230's start); to uu.Shift (0.5, D-230's top) at uu.Shift.RefKpcS (2200: D-230's full speed) as (u / U1)^1.5; from there to uu.Shift.Top (2: x7.4) at uu.Shift.FullKpcS (8900: 4x), its slope carried through the reference and accelerating (a x + (1 - a) x^2). x1.07 at 300 kpc/s (D-230 x1.00), x1.20 at 600 (x1.12), x1.35 at 1 Mpc/s (x1.37), x1.65 at 2.2, x2.63 at 4.4, x7.39 at 8.9 Mpc/s and above.
- **The tint (Uni_TravelShift):** the factor's log to +-3 (Uni_DopplerTint's x4 clamp was the old top end), up to uu.Shift D-230's tint exactly; past it the green falls with the excess (exp(-0.5 x excess): deeper red behind, a deeper blue ahead instead of the cyan a green held at 1 gives) and, on the blue side only, the light x exp(uu.Shift.Boost x excess) (1: x4.5 straight ahead at the top). Ahead stays blue-white at high brightness (as a hotter source would); behind goes deep orange-red.
- **Removed:** uu.TravelLook.Start / Full (only the shift used them since D-231) and UniCore::TravelLookRamp. uu.Shift now 0..3 (no negative).
- **Plumbing:** the frame's ShiftBoostFrom (uu.Shift) and ShiftBoost; the passes' ShiftKBoost float4 (was ShiftK); FUniView's ShiftBoostFrom / ShiftBoost (UniProject.ush); the harness's camera.
- **Check:** T1.look.travel_shift extended: up to 0.5 the tint is D-230's within 1.2e-7; the curve 0 below the start, no later than D-230's below 700 kpc/s, uu.Shift at the reference, Top at full, monotone, its slope continuous at the reference (0.279 / 0.279); no boost up to the reference's shift, x4.4817 ahead at the top, luminance kept behind and where the shift is 0.5; capped at e^3; eased (1.929 of 2 after 1 s up, 0.364 left 1 s after the stop).
Not verified: the engine side (UE-92).
Reroll impact: none (render only).
Status: built; offline check; engine test UE-92.

D-269 — Dark dust in the cosmic web
Owner, 2026-11-16 17:28: "we want to incorporate some dark matter dust type absorption in the universe cosmic web field as well... like dark knots or fillaments, it might benefit from the other lessons learned so far on the galaxy march". (Real intergalactic dust is far thinner; an art choice, render only.)
- **Uni_WebDustTau (UniVolume.ush), once a step in the march's textured-web branch (two web carriers read one texel: bWebDustDone, as the motion web):** optical depth (V) per cm = uu.WebDust / 100 Mpc x the filament + node matter over its mean (the web texture's y + z channels; held softly under uu.WebDust.Peak 30 x its mean: nodes are hundreds of times the mean) x a dark pattern x the fades, x the step (cm) x (1 - the carrier's blend).
- **The galaxy march's lessons:** linear in the matter, no threshold (a threshold of a filtered field loses its peaks with distance: D-234); the pattern from galaxy_shape, shaped before its mips (mean 1 at every footprint: D-259): its ridged channel^4 (dark filaments) toward its knots^8 (dark knots) by uu.WebDust.Knots, amplitude uu.WebDust.Detail, tiled a whole number of times a 200 Mpc web tile; both the web's channels and the pattern read over the cone rule's footprint, the larger of the pixel's and the step's (D-255: extinction wants the step's mean density, not a jittered point's; the channels re-read only where that is a coarser mip than the light's), so an unresolved pattern is its mean (blur, never grain: D-265); faded in over uu.WebDust.NearMpc (2: the home group not fogged) and out over the second half of uu.WebDust.DepthMpc (50: the far sky not dimmed as a whole; no edge). Its colour: grey toward the galaxies' dust's reddening by uu.WebDust.Redden (0.5).
- **Defaults (engine):** uu.WebDust 3, Detail 1, Knots 0.3, DetailMpc 30 (28.6 a tile: ~5 Mpc features, 220 kpc texels), Redden 0.5, Peak 30, NearMpc 2, DepthMpc 50; live; the tuning page's Web. Off without the web texture; the pattern off without the galaxy textures. FVolumeSettings keeps it off. The view struct 288 -> 320 bytes.
- **Exploration (experiments/webdust_d269; the void camera, the delivered web, with and without the motion web's glow):** the texture's filament + node matter over its mean: median 0.05, p90 2.5, p99 14, max 191. A 10 Mpc pattern made specks 10-50 Mpc away (a brownish haze); at 30 Mpc and optical depth 5 dark filament threads cross the glow. Half the step as the footprint left grain with the motion web (spread 2.48% against 2.26% without dust); the whole step 2.22%.
- **Check:** T2.look.web_dust (new; 128 x 72): off bit for bit; light x0.868 at 1 per 100 Mpc, x0.779 at 2 (ln ratio x1.77); one web carrier x0.9996 of two's dimming; blue/red 0.8974 without, 0.8974 grey, 0.8441 reddened; the pattern's frame within 0.7% of the matter alone's; a 0.5 Mpc depth x0.99985; the grain over 8 jitter phases 0.081% -> 0.686% (a smooth background: the step's window moves with the jitter; within 2 points).
Not verified: the engine side (UE-93): the look against the motion web and the background, the cost (one or two more texture reads a web step), sprites behind dark filaments (the march's transmittance slices).
Reroll impact: none (render only).
Status: built; offline check; engine test UE-93.

D-270 — Backlog pass: the dust's extinction curve as a knob; the step footprint's light closed; the motion web's node weight measured, not adopted
Owner, 2026-11-16: a tuning pass later, in a block of time; meanwhile improvements and backlog items.
- **The dust curve (B-036; uu.Volume.DustCurve, default 0):** Cardelli, Clayton & Mathis (1989) A/A_V for R_V 3.1 (UniCore::CardelliAlav, their Eq. 2-3: within 0.93% of their Table 3 from U to K) at the linear sRGB primaries' dominant wavelengths (612, 549, 465 nm) gives (0.886, 1, 1.233) of the green's optical depth against D-132's (0.8, 1, 1.3): the real curve is a little less red (behind optical depth 4, yellow rather than orange). DustCurveRgb blends D-132's toward it (0 bit for bit D-132's and FVolumeSettings' default); the engine sets DustSigmaRgb from it x uu.Volume.DustGain, so the web dust's reddening (D-269: DustSigmaRgb / its green) follows. Default 0: the look is the owner's call in the tuning pass (the tuning page's Galaxy|Balance; 210 variables).
- **The starlight over the step (B-034's item, D-255's -3.7% at 3 kpc at StepFoot 0.5, -7.4% at 1): closed.** Re-measured on today's march (experiments/zz_foot_d270; light against the fine reference, an eighth of the steps, StepFoot 0, one tap; 4 jitter phases): 1 kpc above the disc down 30 deg, 3 kpc above down 45 deg, along the disc at the solar circle: StepFoot 0.5 with 4 taps -0.11 / -0.08 / -0.16%; 1 with 4 taps -0.08 / -0.08 / -0.09%; 2 with 4 taps -0.67 / -0.37 / -0.32%; one tap at 0.5 -0.52 / -0.02 / -0.17%. The loss went with D-258/D-259 (the texture terms shaped before their mips: mean 1 at every footprint) and D-262 (the taps). T2.vol.galaxy_segment now also holds the defaults' light within 1% of its reference (metric light_taps4).
- **The motion web's node weight (B-033's second item): measured, not adopted.** (filaments + w x nodes) over its mean (experiments/webnodes_d270): the texture's channel means walls 0.377, filaments 0.415, nodes 0.094, uncollapsed 0.114; at the default threshold (3) the glow from node-dominated texels is 17.5% at w 1, 10.3% at 0; the lit volume 8.3% -> 10.0%; the void camera's frame within 0.7% and the images nearly the same. The round blobs are the web texture's 0.78 Mpc texels and the glow's log curve (saturate(0.5 log10 Fn)), not the nodes: the levers are uu.MotionWeb.Threshold (higher: thinner structure) and a finer web texture or detail octave (B-033's first item). The view struct change was reverted (still 320 bytes).
- **Design proposal (B-031's next item):** Docs/COMPACT_OBJECTS.md, supermassive black holes and AGN, quasars as an active-galaxy archetype, stellar remnants, lensing up close; four owner questions. Nothing built.
Measured: T1.look.dust_curve (new): the formula within 0.93% of Table 3; 0 D-132's and the harness default exactly; 1 (0.8863, 1, 1.2332), Cardelli's ratios within 1e-6; red < green < blue and each channel monotone across the blend; clamped outside [0, 1]. Swatches: white starlight through optical depth 0-4 under each curve.
Not verified: the engine side (UE-94: the dust's colour at DustCurve 0 / 1).
Reroll impact: none (render only; measurements).
Status: built; offline checks; engine test UE-94.

D-271 — Galaxy stack review: the sprites' transmittance past 100 kpc (the slices' far segment), slices at their boundary; findings
Owner, 2026-11-17 00:33: "we should review the galaxy stack we have been working on to look for any issues that may have crept in first".
- **How:** the view and carrier structs' fields written against read (all wired), the CVars against their reads, every check's measurement across the reports since D-255 (80 changed; the moves traced), the recent march code (D-265..D-270) read through, and experiments where a number moved.
- **Found and fixed: sprites past 100 kpc were never dimmed.** The transmittance slices covered 1 pc..100 kpc (8, uniform in ln t) and a sprite beyond read the last one, so since D-269 every galaxy sprite behind the web's dark dust shone through it (the void camera with the engine's web dust: the true transmittance 0.70 at 50 Mpc on average; the sprites' 1; REVIEW §94 claimed they dimmed). Now UNI_VOL_SLICES 12: the first UNI_VOL_SLICES_NEAR 8 as before, then 4 evenly in t to the view's SliceFarCm (Uni_SlicePosT; even in t because dust spread through the web is, and linear in t between them). The engine sets 50 Mpc, or the web dust's depth if farther (FVolumeSettings' default: none, every far slice the near segment's last: the harness's checks see D-130's slices). Measured (experiments/slicefar_d271, exact boundaries): 4 evenly in t mean error 0.0098 a distance over 0.5..50 Mpc, against 0.0746 without and 0.0181 for 4 in ln t; 6 or 8 would halve it again (rejected: the slice memory).
- **Slices at their boundary:** a slice recorded the transmittance at the end of the step crossing its boundary (past 10 Mpc a step is several Mpc); now at the boundary within that step (its extinction even along it, the step's share from the slice positions). Worst error a distance 0.0388 -> 0.0347 with the far segment; the near slices move by less than a step's extinction (sprites in a dusty disc just past a boundary were slightly over-dimmed).
- **Plumbing:** FUniVolumeView / FVolumeViewGPU 320 -> 336 bytes (SliceFarCm, 3 pads); the sprite and splat passes' SliceFarCm; UniGpuLayout::VolumeSlices 12; the far cache's slice rule (Uni_FarSliceT) through Uni_SlicePosT. Memory: the slice array 8 -> 12 layers (12 MB at 1080p, half resolution), the far cache's 96 -> 144.
- **Measured, not regressions (B-043):** T2.galaxy.sprites' hand-off axis ratio error 0.042 -> 0.059 at D-256..D-262: the web scene's discs' light-weighted thickness has mean 0.200 exactly (the sprite's q0), spread 0.12..0.31 galaxy to galaxy, so the error is per-galaxy thickness the sprite does not carry (experiments/handoffq_d271). T3.vol.march's optical depth error 0.0043 -> 0.0169 (limit 0.02) at D-262: its darker dust doubled the depths compared (to 6.7), the relative error is unchanged (0.2-2%; experiments/marchtau_d271). The detail layers end at the cut sphere at once (structure only). Sprites dim in luminance only (the slices).
- **Check:** T2.look.slice_reach (new): the near slices bit for bit with and without the far segment; without it every sprite past 100 kpc reads the 100 kpc slice exactly; with it the error against the true transmittance (a march stopped at each of 20 distances, 0.5..50 Mpc, 1296 rays) mean 0.0090, worst 0.0347 (without 0.0699, 0.3036); the far cache's slice rule. The volume and look groups pass (48 and 8 checks).
Not verified: the engine side (UE-95).
Reroll impact: none (render only).
Status: built; offline check; engine test UE-95.

D-272 — Supermassive black holes: a property of every galaxy; its nucleus a second sprite (COMPACT_OBJECTS.md phase A)
Owner, 2026-11-17 00:33 (COMPACT_OBJECTS.md §6): visibility "probably depends on their accretion discs, we would probably want some sort of gravitational lensing"; quasars "relatively rare, we may need to ramp their frequency up to find instances to test"; optical jets "yes"; "probably do supermassives first since every spiral galaxy will have one, and placement is always in the center of the bulge... the implementation should be robust enough to use for stellar or supermassive". "if you wanted to start on supermassive galaxy center black holes that would be a good place to start".
- **The core (Shaders/Private/Core/UniCompact.ush):** a black hole of any mass, FUniBlackHole from log10 mass and log10 Eddington ratio (Uni_BlackHoleOf: bolometric luminosity 1.26e38 x M x lambda, its V light 10% of it, Uni_BhSchwarzschildCm, Uni_NucleusColour: 5000 K at 1e-3 of Eddington and below to 25000 K at 1e-1 and above): what a stellar black hole (phase C) or the lensing pass (phase D) will use with its own mass source.
- **A galaxy's (Uni_GalaxyBlackHole, from the record alone: seed, V magnitude, early-type flag):** mass 0.0014 x the bulge (Haring & Rix 2004), 0.3 dex scatter (a normal from BH.MASS draws 0, 1, held within 3 sigma); the bulge = the host's V light x a bulge share (BH.MASS draw 2: discs log-uniform 0.08..0.45, early types uniform 0.6..1) x a mass-to-light ratio (2, 4). Eddington ratio (BH.EDDINGTON draw 0): an accreting share (uu.Bh.ActiveFraction 0.4) with p(log lambda) ~ lambda^-0.65 over 1e-4..1 (Aird et al. 2012), the rest quiet, log-uniform 1e-9..1e-5. New channels BH.MASS 0x1AF45697, BH.EDDINGTON 0xEB93DBAF (T1.seed.channels). A Milky Way-like disc (M_V -20.9): median 1.06e7 M_sun (the real 4.3e6 is below the relation; anchors keep the rolled value for now).
- **The nucleus:** Uni_NucleusAbsMag: the V light x uu.Bh.NucleusGain (1), at most uu.Bh.NucleusCap (0.5) x the host's (the level is the host's), none under 1e-6 of it. Drawn by the project pass as a second sprite of the galaxy's record (Uni_ProjectNucleusC: a point record at the galaxy's position, magnitude and colour of its own, through Uni_ProjectObjectC with no carrier share: a point source's glare, never handed to the volume, culled as any point, the slices' dust in front; hidden with a deleted galaxy, never a promotion candidate). The galaxy's own sprite, its carrier, placement and generation untouched. UniProject.usf's AppendSprite; the reference renderers draw it the same way. Statistics (10^4 records): 1.92% of galaxies above 1e-2 of Eddington; nuclei at 1% of the host or more 5.25%, at 10% 1.17%, capped 0.31%.
- **Engine:** uu.Bh.Nucleus (1), uu.Bh.ActiveFraction, uu.Bh.NucleusCap, uu.Bh.NucleusGain (live; the tuning page's Compact Objects; 214 variables); the frame's NucleusArchetypeMask (the galaxy-field archetypes) and NucleusParams. Gameplay: UUniSubsystem::GetGalaxyBlackHole (FUniBlackHoleInfo: position, mass, Eddington ratio, luminosity, Schwarzschild radius, nucleus magnitude) from UniCore::GalaxyBlackHole (UniCompact.h), the core's numbers.
- **Checks:** T1.bh.mass_relation (residual mean +0.004 sigma, spread 0.995; bulge shares in range, discs' median 0.1877 against 0.1897; units), T1.bh.eddington (accreting 0.4022, decile KS 0.0017, quiet median -7.01), T1.bh.nucleus (0 over the cap, the black hole's light below it within 1e-3 mag, gain, the CPU lookup identical), T2.bh.nucleus_sprite (3458 galaxies: own sprites bit for bit with the nuclei on; 59 visible to m 20, 318 with every black hole accreting x30; points at the galaxy's place; light within 2.6e-7 of a point of the nucleus's half-float magnitude; still render/still_bh_nuclei: the four brightest at 12 radii without and with).
Not verified: the engine side (UE-96): the look, a Blueprint lookup, the sprite count.
Reroll impact: none (new channels; render only; the API reads).
Status: built (phase A); offline checks; engine test UE-96.

D-273 — Black holes up close: gravitational lensing and the accretion disc (COMPACT_OBJECTS.md phase D, first part)
Owner, 2026-11-17 08:11: "Gravitational sensing ans accretion discs, there are quite a few shader references online for black holes as well".
- **References used:** R. Antonelli's "starless" (the flat-coordinate photon integrator), E. Bruneton 2020 "Real-time high-quality rendering of non-rotating black holes" (tables for the disc's light; a cube map for the background: not taken, see below), Luminet 1979 (the thin disc's image and its shift g), James et al. 2015 (Interstellar's DNGR). Schwarzschild only (no spin).
- **The lens (UniCompact.ush, Uni_LensTrace; units of R_s):** a camera ray traced back: inside a sphere of 200 R_s (or 1.25 x the disc) by RK4 on the acceleration -1.5 h^2 x / r^5 (its orbits are exactly u'' + u = 1.5 u^2), steps 0.06 r within 20 R_s (half inside 3), 0.15 r beyond; outside it in closed form along the straight line (that acceleration falls as r^-5: the line's remaining bend K(sigma), 2 R_s / b in all). The camera is a static observer (its measured direction's radial part x sqrt(1 - 1/r) to the coordinate one). Captured (inside the photon sphere going in, or past 400 steps: the edge of the shadow) or escaped: the scene sampled where the ray came from. Surface brightness is conserved: the lens moves light and never adds any.
- **The disc:** thin, optically thick, from 3 R_s to where it has cooled to 3000 K (12..1000 R_s), opaque to 0.6 of that then fading. Temperature (x^-3 (1 - sqrt(3/x)))^1/4 scaled to the physical thin disc's peak (T*^4 = 9 L / 4 pi sigma R_s^2: 8e4 K for 1e8 M_sun at 0.1 of Eddington, 1e7 K for a stellar hole), at least 4000 K (a slow accretor is a hot thin flow, its light warm). Each crossing (up to 4, front to back): g = sqrt(1 - 1.5/x) / (1 - Omega lambda) (Keplerian gas, the photon's angular momentum about the axis), the light the blackbody at g T in V (Planck weight x its colour at luminance 1). The axis: the host's disc normal tilted within 60 degrees (new channel BH.SPIN 0x564A2FF7).
- **Light kept through the hand-off:** the disc's light by direction (lensing, shifts and beaming and all) tabulated offline (`univerify lenstable`: 36 peak temperatures ln-even 4000 K..1e7 K x 17 inclinations, UNI_BH_DISC_BEAM / LOGNORM): the disc's radiance scale from it makes its light averaged over directions the nucleus's, and the nucleus sprite now shows the disc's light towards the camera (face-on brighter, edge-on dimmer; a small disc lifted by its own lensing: 0.67..1.26), its mean the record's magnitude. The nucleus's colour is the disc's (Uni_NucleusColour: 12 taps over its radii, within 0.4% chromaticity of the shifted average): D-272's 5000..25000 K model is superseded (render only).
- **The pass (UniLens.usf; UniRenderer: the end of the backdrop, after the sprites and volume, before bloom):** a copy of scene colour read, every pixel of the view written: outside the black hole's region (the integrated sphere and where the bend is >= 0.1 px) copied; scene surfaces nearer than the hole (a ship) copied; else traced, the scene sampled bilinear at the ray's origin (folded back into the view by mirroring when it left it: no cube map yet), the disc's light x the dust's transmittance to the hole (the volume's slices), and the nucleus as a point. **The nucleus while the pass runs:** its sprite is held back (UniProject's NucleusHandSeed / Share) and the pass draws it at the hole's own direction in the sprites' style (Uni_PointStyle: the project pass's core and glare for that intensity) x (1 - the disc's share): a sprite left in scene colour would be lensed as if behind the hole (a ring 20 px wide for a small disc). The disc takes the light over as it resolves (outer radius 2 -> 6 px).
- **Which black hole:** the subsystem's UpdateLensTarget: of the volume's home galaxy and its local carriers, the one whose Einstein ring (>= 0.25 px) or disc covers the most pixels (UniCore::LensTargetOf); none without the volume. One at a time.
- **Engine:** uu.Bh.Lens, uu.Bh.Disc, uu.Bh.LensRingMinPx (0.25), uu.Bh.DiscFromPx (2), uu.Bh.DiscToPx (6), uu.Bh.LensBendMinPx (0.1), uu.Bh.LensMaxSteps (400) (tuning page: Compact Objects; 221 variables); uu.Perf's lens line. GetGalaxyBlackHole adds the disc's axis, outer radius and peak temperature.
- **Checks:** T1.lens.deflection (125 rays, cameras 6..1e6 R_s, against the exact orbit in double: worst 3.3e-5 rad from b 3, 4.9e-4 just outside the shadow; the reference against the weak-field series 2.7e-4), T1.lens.shadow (the edge 3 sqrt(3)/2 within 3.6e-4, Synge's angle), T1.lens.redshift (face-on g = sqrt(1 - 1.5/x) to 4e-8; inclined against special relativity's Doppler with the gravitational shift to 8.7e-4), T1.lens.brightness (a uniform scene: every pixel 1 to 1e-6 or shadow; the shadow's solid angle Synge's within 0.2%), T1.lens.disc_light (fresh traces between the table's nodes within 2.3%, their mean within 0.6%, colour within 0.004), T1.lens.cost (mean 15-38 steps a traced ray, at most 331, none capped), T2.lens.handoff (a real galaxy's nucleus from 2e7 R_s to the resolved disc: the light shown within 0.33% of the sprite's at every distance; the point's style the sprite's to 1.8e-6), T2.lens.stills (render/still_bh_lens), T2.bh.nucleus_sprite updated (the beam), T1.seed.channels (BH.SPIN).
Not done (B-044): a cube map for rays that leave the view (Bruneton's; the fold mirrors the screen), spin (Kerr), the disc's turbulence and rotation in time, several black holes at once, the camera's own blueshift deep in the well, the pass before TSR (the disc's edge is not temporally antialiased), stellar black holes (phase C) through the same pass.
Not verified: the engine side (UE-97): the shader's compile, the look, the cost (Uni.Lens in ProfileGPU), the hand-off at the pass's start.
Reroll impact: none (a new channel; render only; the API reads).
Status: built (phase D's lensing and disc); offline checks; engine test UE-97.

D-274 — The accretion disc's brightness up close: a soft knee (uu.Bh.DiscKnee)
Owner, 2026-11-17 10:03 (engine, D-273): "the brightness of the black hole up close is much too bright for me to inspect the effect, it is masked by the bloom and flare".
- **Why:** the disc carries the nucleus's physical light: an active nucleus a few hundred R_s away is ~1e8-1e10 x the exposure per pixel (scene colour clamped at 6e4), so bloom and the lens flare swamp the lensing and the disc's structure.
- **What:** the lens pass softens the disc's light per pixel above uu.Bh.DiscKnee (exposure units; default 2): unchanged below it, then Knee (1 + log10(L / Knee)), colour kept (Uni_BhDiscSoften): 10x reads 2x, 1e8x 9x (comparable to a saturated sprite core, 16). Monotone, so the Doppler side, the lifted far side and the photon ring stay readable. 0: physical. Render only, live (render thread; tuning page Compact Objects, 222 variables). The point the pass draws keeps the sprites' own saturation and glare (D-156).
- **Hand-off:** with the knee the light no longer matches the sprite's through the hand-off (the sprite's core saturates on its own curve); both are bounded, so a resolving disc can look dimmer than the point it replaces rather than flash. T2.lens.handoff measures the physical light (knee 0).
- **Check:** T1.lens.disc_knee (new): bit for bit below the knee and at 0; monotone over 1e-3..1e12 x; chromaticity to 1e-6; 10x -> 2x, 1e8x -> 9x; still render/still_bh_knee (physical, 2, 0.5 at the scene's own exposure).
Not verified: the engine side (UE-98).
Reroll impact: none (render only).
Status: built; offline check; engine test UE-98.

D-275 — The lens never copies what is in front of the black hole
Owner, 2026-11-17 10:25 (engine, D-273; screenshot): "the gravity distortion is also effecting the objects in front of the black hole like the player... maybe that is physically accurate IDK but probably not".
- **Not physical:** a body between the camera and the hole is not lensed by it (its light never passes the hole). The pixels of the player were already kept (the depth test); but sky pixels near the hole sample the screen where their ray came from, and where that spot was covered by the player the pass drew a stretched copy of the player (the screen holds nothing of the sky behind it).
- **Fix (UniLens.usf, BehindHole; UniLensRef.h the same):** the sample point steps from the ray's origin back towards the pixel's own direction (a quarter of the way at a time, at most 4 steps; the pixel's own direction is sky: it passed the depth test) until its bilinear footprint (4 depth taps) is behind the hole. The lensed sky there is a little less bent next to the figure (its outline's neighbourhood shows nearer sky) instead of a copy of the figure. A true fix needs the sky without the foreground (B-044's capture).
- **Check:** T1.lens.foreground (new): a green figure before the hole (camera 30 R_s): 0 pixels elsewhere with its green (6209 without the fix), its own pixels untouched.
Not verified: the engine side (UE-99).
Reroll impact: none (render only).
Status: built; offline check; engine test UE-99.

D-276 — The accretion disc's look: brightness relative to the exposure; a volumetric, cloudy, rotating disc
Owner, 2026-11-17 10:23 (engine, D-273/D-274): "the overall disc and black hole effect look correct from a morphology standpoint. its still too bright - we want something that looks like what we think of a black hole rather than the blinding brightness of what it would really look like up close"; (1) "disc clouds rather than a static disc ... some form of noise ... potentially with like spiral distortion ... look around for how this is done in other implementations"; (2) "rotation ... bands of differing light intensity and fusion colors"; (3) "the disc itself ... would have their own volume that can be transited through ... if cost allows ... if cost prohibits ... billboard effect + danger/kill zones".
- **References:** S. Anderson's Unity "Black-Hole-Simulation" (a volumetric disc from a 3D noise texture sampled in radial coordinates, a screen-space pass; its radial seams), the three.js "Gargantua" Kerr tracer (a thin disc; Novikov-Thorne temperatures; razor-thin slabs alias), Vlachos 2010 "Water flow in Portal 2" (two flow phases cross-faded: the standard answer to differential rotation winding a pattern without end).
- **(3) answered: a volume, in the same trace.** The disc was never a billboard: every pixel's ray is integrated through the curved space; the volume is samples along the ray's chords where they pass through a flared slab (H = 0.05 rho), dI = (S - I) alpha ds front to back (each sample's transmittance exact), half a thickness apart, at most 24 a step, a budget of 160 a ray (p99 of the views measured 68-167) then one a step. Cost: ~50 samples a ray through the disc on average (p99 157), each two 3-octave value-noise fBms; only in pixels that see the disc of the one black hole drawn. A camera inside it works (the trace starts there). A kill zone stays a gameplay choice (GetGalaxyBlackHole has the radii), not a need.
- **The volume (Uni_BhDiscSample):** absorption Opacity / (sqrt(2 pi) H) x exp(-z^2 / 2H^2) x the clouds' clump x the D-273 edges (thinning in the plunging region 2.6..3.2 R_s); its source the thin disc's light at the local temperature shifted by g (so its thin, opaque limit is the thin disc: 1.0046 of its light). Clouds: value noise (an integer hash: GPU and CPU alike) fBm in the co-rotating cylinder (cos, sin of the azimuth wound by Spiral x ln rho, ln rho, z / H): no seam. The clump lerp(1, 2 smoothstep(0.40, 0.66, n), Clouds): gaps the sky shows through. Rings: 1 + Bands sin(2 pi 7 ln rho / ln(outer / 3) + 3 n). Heat: the temperature x (1 + Heat (2n - 1)) (hot blue-white knots among orange). Falloff: the light (Planck_V(gT) / Planck_V(peak))^Falloff x Planck_V(peak), colour from gT: 0.35 lets the cooler outer disc glow (1 physical: dark).
- **Rotation:** Keplerian about the axis, the ISCO's orbit uu.Bh.DiscPeriod seconds (8; physically hours to days); two flow phases each advecting the clouds for uu.Bh.DiscCycle orbits then reborn with a fresh seed (from the galaxy's seed and the cycle), weights 1 - |2 frac - 1| (UniCore::ApplyDiscLook, double time: the game world's seconds, pausing with it).
- **Brightness:** uu.Bh.DiscBrightness (1): the disc's peak temperature shows this many exposure units a pixel (DiscArtScale), whatever its physical light; 0: physical (D-273's scale, with D-274's knee). The knee still softens the Doppler-boosted side.
- **Knobs (render thread, live; Compact Objects, 234 variables):** uu.Bh.DiscBrightness 1, DiscVolume 1, DiscThickness 0.05, DiscOpacity 2.5, DiscClouds 0.85, DiscBands 0.3, DiscHeat 0.35, DiscSpiral 1.5, DiscPeriod 8, DiscCycle 1, DiscFalloff 0.35, DiscSamples 160.
- **Not kept:** with the exposure-relative brightness the light no longer matches the nucleus point through the hand-off (D-273's table is the physical mode's); both are bounded (the point's sprite style saturates), so it may step a little either way while the disc resolves.
- **Check:** T1.lens.disc_volume (new): thin limit 1.0046; 0.02 s across a phase restart 0.00101 of the image against 0.00157 mid-cycle; the clouds at 10 R_s turned 4 / 720 in 0.25 s against Keplerian 3.7; a camera inside (8 R_s) 1080 rays finite, all hidden along the plane; 49.9 samples a ray, p99 157, 0.72% past the budget; still render/still_bh_disc. Offline preview: a 40-frame rotation (Docs not kept; sent to the owner). The other lens checks unchanged (the thin disc is FUniLensDisc's default).
Not verified: the engine side (UE-100): the look, the cost (Uni.Lens in ProfileGPU, the hole filling the view), flying through.
Reroll impact: none (render only).
Status: built; offline checks; engine test UE-100.

D-277 — The disc's defaults after the owner's first look: thinner, tighter spirals, a larger hole (uu.Bh.LensScale)
Owner, 2026-11-17 11:32 (the D-276 preview): "that looks quite nice actually. disc thickness could come down by around half or so, and the central hole size could come up by say... 2x that of course includes the lensing to scale as well, spiral itself could be tighter with more winding so maybe 2-3x tighter".
- **Thickness** 0.05 -> 0.025 (uu.Bh.DiscThickness; samples a ray through the disc 49.9 -> 59.9 on average, p99 152, 0.55% past the budget). **Spiral** 1.5 -> 4 radians per e-fold (uu.Bh.DiscSpiral).
- **The hole 2x (uu.Bh.LensScale 2, game thread, live):** the black hole lenses as if this x heavier: the lens's R_s is k x the physical one, so the shadow, the photon ring and all the bending scale k x; the disc keeps its physical size (outer radius OuterX / k in the lens's units: the same pixels) and starts at the heavier hole's innermost stable orbit (nothing orbits inside the bigger shadow). Its temperature profile, brightness (exposure-relative) and the nucleus point's size are unchanged; the physical brightness mode is exact only at 1 (the table's disc). UniCore::FLensConfig.LensScale (1 there: the checks' physics).
- **Check:** T1.lens.lens_scale (new): at 2 the lens's R_s 2x, the outer radius in its units half, the disc's pixels the same, the ring sqrt 2 x, the point's size the same, the traced shadow 1.9999x the angle. The still render/still_bh_disc at the engine's defaults.
Not verified: the engine side (folded into UE-100).
Reroll impact: none (render only).
Status: built; offline checks; engine test UE-100.

D-278 — The disc: tighter still, a bigger hole, erosion detail
Owner, 2026-11-17 11:47 (D-277's preview): "looks great, spiral could go tighter still, lets try 3x, hole could go bigger still lets try 2x, and some erosion detail noise or something to break up the clean geometric nature of the accretion disc".
- **Spiral** 4 -> 12 (uu.Bh.DiscSpiral). **Hole** uu.Bh.LensScale 2 -> 4 (the shadow and its lensing 4x the physical against the disc).
- **Erosion (uu.Bh.DiscErosion 0.8; 0 = D-276's clouds exactly):** the volumetric-cloud way (Schneider 2015, Horizon Zero Dawn's clouds): a finer noise (2 octaves at 5x the clouds' frequency, 3x through the height), moving with the gas through the same flow phases, eats the clouds' thin edges and leaves their cores (n - 0.4 E d (1 - smoothstep(0.45, 0.85, n))); it scales the local height (a ragged top surface) and the outer rim's radius (a torn edge). Its winding is a quarter of the clouds' so it cuts across their streaks instead of following them (with the same winding the result still read as concentric rings). FUniLensDisc.Erosion (was LookPad0); the shader's DiscFlow.w.
- **Cost:** samples a ray through the disc 68 -> 74 on average (more gaps: rays go deeper), the budget 160 -> 192 (UNI_LENS_VOL_SAMPLES; uu.Bh.DiscSamples' default) to keep under 2% past it; each sample one more 2-octave noise per flow phase.
- **Check:** T1.lens.disc_volume at the new defaults (thin limit 1.0046 with erosion 0; restart 0.00167 vs mid-cycle 0.00294; rotation 4 / 720 vs 3.7; inside finite; 73.6 samples, p99 195, 1.98% past); the still at the engine's defaults.
Not verified: the engine side (folded into UE-100).
Reroll impact: none (render only).
Status: built; offline checks; engine test UE-100.

D-279 — No shells beside the player: the lens's fill behind the foreground
Owner, 2026-11-17 12:50 (engine, screenshots): "its not longer generating a distorted refraction of the player ... however there are still refraction glitches around the player ... a repeating series of silhouettes of refraction shells - they propagate from the player model towards the edge of the screen along a trajectory from the black hole center"; the hole "looks pretty great - could use a lighting/color pass".
- **Cause:** D-275 stepped a covered sample back towards the pixel's own direction in quarters, testing depth. The player's colour reaches a pixel or two past its depth (antialiasing; the depth at the internal resolution, the colour upscaled by TSR), so each quarter step that landed on that fringe copied its outline: three shells a pixel, marching outwards along the bend. Reproduced offline once the fringe is modelled (1908 pixels of the figure's colour copied, the shells).
- **Fix (UniLens.usf; UniLensRef.h the same):** a sky-only pyramid each frame the pass runs (SkyMaskCS: mip 0 the scene behind the hole premultiplied by a mask grown 2 px over the colour fringe, a 5 x 5 window; SkyDownCS: each mip the 2 x 2 average; RGBA16F, the view rect's size). A ray whose origin is covered finds the covered stretch of its line towards the pixel (7 bisections on mip 0's coverage) and samples the sky mirrored out of it by its depth (SkyBehind): continuous in the position (no train of contours) and starry (the pyramid alone filled with a starless blur, tried and dropped); where the mirror is covered too, the pyramid's fill at the edge (SkySample: mip 0's covered share, the coarser mips front to back). Cost: the mask pass (25 depth taps a pixel) and ~11 mip passes, only while the lens runs.
- **Check:** T1.lens.foreground: the figure's colour now reaches 2 px past its depth: 0 pixels of it copied elsewhere (D-275's: 1908); over a smooth gradient sky the steps beside it +493 against the figure-less image (D-275's +2246: ~9 outlines; bound 2 outlines). Offline comparison images (the shells reproduced, then gone) sent to the owner.
- **Owner's next (not done):** a lighting / colour pass on the hole and disc.
Not verified: the engine side (UE-101).
Reroll impact: none (render only).
Status: built; offline check; engine test UE-101.

