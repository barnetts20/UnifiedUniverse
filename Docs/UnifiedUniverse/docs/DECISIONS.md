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
