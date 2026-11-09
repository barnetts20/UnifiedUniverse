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
Status: Provisional (UE-38)

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
Status: Provisional (UE-48: shaders compile; Uni.Volume.March in the core and at the solar circle with uu.GalaxyCache 0 / 1; Uni.Volume.CacheBake cost while flying; no seams between levels; memory)

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
