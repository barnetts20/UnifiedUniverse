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
