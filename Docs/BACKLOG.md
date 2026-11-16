# Backlog

*Ideas and deferred work not yet scheduled. Each item says where it would land and what it depends on. Decisions still go to `DECISIONS.md` once taken.*

## B-001: Warp streaks from late-loading stars (owner idea, 2026-11-01) — DONE (D-187 streaks and arrival as motion; D-190 optics; D-192 motes)

**Milestone:** sprite work (better stand-ins for star and galaxy sprites). Render-only (lesson #12): no content change, no reroll.

**Problem it turns around.** At high speed a cell can become ready after its objects should already have faded in: teleport fill latency, velocity-LOD resumes, or GPU generation simply lagging the camera. Those stars appear at full brightness, which is a pop. D-116/D-118 remove pops at normal speeds, and on the volumetrics branch D-128 carries the missing light as glow, but a late load still pops.

**Idea.** When an object spawns already visible, give it a short-lived ghost trail instead:
- The trail runs back to where its fade-in would have started: the point on the camera's recent path where the object crossed the visibility limit.
- Along the trail, brightness ramps from zero to the current value and size from the minimum footprint to the current one.
- The trail then collapses over a short duration (≈0.1–0.3 s), retracting into the current position.
- At warp speeds this gives the classic streaking stars, so a load-speed weakness becomes a visual gain.
- Objects that spawn just below visibility fade in normally, as now.

**Doppler tint.** Tint the streaks by line-of-sight velocity: blueshift ahead, redshift behind.
- Use the relativistic factor for camera speed v and the angle to the direction of travel, D = 1 / (γ (1 − β cos θ)).
- Shift the sprite's colour (its blackbody temperature, T' = D · T) and optionally its intensity (D³–D⁴ for bolometric beaming).
- At v ≪ c, scale the tint artistically from the camera's speed relative to the level's cell crossing rate, so it reads at any warp speed.
- Applying it to all sprites, not just streaks, gives aberration and beaming for free; decide per art direction.

**How it fits what exists.**
- *Detection:* the cell table can carry each cell's ready frame. Project-and-cull already computes every object's intensity; an object in a cell that became ready this frame, with intensity above the just-visible threshold, is a streak candidate. That is the same test as T3.flythrough's pop count.
- *Tail position:*
  1. Compute r_fade = 10 pc · 10^((m_lim − M) / 5) (the fade-band start).
  2. Find the camera position, from a short ring of recent exact camera coordinates, at which the object was r_fade away.
  3. Project the object from that position; that point is the tail.
  4. Exact deltas (D-104) keep the projection stable at any coordinate.
- *Drawing:* a tapered, oriented sprite quad from tail to head, with the intensity and size ramp along it and the collapse driven by age since ready. Additive like the other sprites, and depth-tested the same way (D-120).
- *Interplay:* with the volume on, a late cell's light is already carried as glow until its sprites arrive (D-128). The streak should take its energy from the same budget, so the sum stays conserved, or be explicitly decorative (decide).
- *Verification:* an offline T3 check. Streak candidates in the flythrough match the pop counter's would-be pops. Tails land on the camera path within a pixel. No streak at normal speeds (0 candidates with D-118).

## B-002: Star systems emitting planets (owner request, 2026-11-01) — DONE (D-142..D-145)

**Status (2026-11-01, 22:50):** v1 built on the volumetrics branch (D-142 to D-145). It covers activation, generation, Kepler orbits on rails, the clock, reflected-light photometry and drawing through the existing sprite path. It is verified offline (6 checks) and needs UE testing (REVIEW_TONIGHT §9). Still to do: lit discs and the promoted planet actor (M3, with B-003), moons, binaries (later, at the system level), and planets in the volume's light budget.

**Milestone:** the next content level after M2/M3 (it needs the promotion path, M3). **Reroll impact:** new content only: new archetypes and new seed channels (CLAUDE.md rule 6). Existing stars and galaxies are unchanged.

**What it adds:** a star archetype emits planets, and planets can later emit moons and rings the same way.

**Why it is not just one more density emitter:**
- **Planets move.** Orbits are a function of time, so a planet cannot sit in a fixed level cell the way stars do. Its position is the star's exact coordinate plus an orbit evaluated at the universe clock.
- **Stars never leave the GPU.** That's by design: only objects whose archetype has emitters are read back. Giving every star an emitter would read back millions of records.
- **Planets shine by reflected light.** Brightness depends on the star's luminosity, albedo, phase angle and both distances, not on a fixed absolute magnitude.

**Sketch:**
1. **System activation, like promotion.** Project-and-cull already tests every star against the camera. Stars inside a system radius (about 0.05 pc, in that star's own terms) go to a small activation buffer that is read back, as promotion candidates are. The CPU registers each one as a contributor with an orbital emitter. Only a handful are active at once.
2. **Orbital emitter (new emitter kind).** Rolled by named channels from the star's seed:
   - planet count, by stellar type;
   - per planet: semi-major axis (log-uniform, scaled by the star's luminosity and snow line), eccentricity, inclination, node, argument and mean anomaly at epoch;
   - radius and albedo, from a mass draw;
   - type: rocky, ice or gas giant.
   The system record holds these (one 32-byte record per planet suffices), not positions.
3. **Position at time t.** A Kepler solve in the shared core, used by the GPU project pass and the CPU resolve alike: star coordinate + orbit(t), in exact deltas (D-104).
   - Needs a universe clock in the subsystem, with time warp, and a decision on whether orbits are on rails (deterministic from the epoch: yes for v1).
4. **Photometry.** Reflected flux from the star's luminosity, a phase function (Lambert to start) and both distances. The same sprite and fade path as stars, with the same D-128 complement when the volume is on.
5. **Resolved and promoted planets.** These are M3: a lit sphere actor (a terminator, a phase), and the real-space bubble and local anchor for landing distances.
6. **Moons:** planets as contributors with their own orbital emitters, the same mechanism one level down.

**Verification (offline first):**
- Kepler solve: CPU vs the shared core vs a high-precision oracle.
- Orbits are periodic and closed, and positions are bit-stable across machines.
- Activation hysteresis: no flicker at the system radius.
- Reflected-light photometry matches the analytic phase curve.
- Systems are deterministic per star (re-entering gives the same planets), and stars' content is unchanged (no reroll).

**Owner decisions (2026-11-01):**
- **Orbits on rails** in v1: deterministic from the epoch, so a system is the same every time you return.
- **Clock:** real time by default, configurable (a time-scale setting). Relativistic motion scaling may come later.
- **Binary and multiple stars: not in v1.** When they come, they are generated at the star-system level, so that one galaxy-level star sprite stands in for the whole system and resolves into separate stars on approach, with the same conserved-light handoff as galaxy sprite → stars.

## B-003: Promote at the glare/physical-size crossover (owner question, 2026-11-01)

**Done for bodies (2026-11-02, D-148).** Bodies (extended objects without children, which today means planets) ease their sprite from the glare footprint to the true size between ¼ θ_promote and θ_promote, and draw as a soft disc (plus a fading glare halo) once wider than 1.5 px. At the hand-off the sprite is exactly the body's size and shape (T2.parallax.promotion, T1.sprite.profile).
- A correction: an earlier note here said a gas giant's sprite was 1.85× its body at the swap. That was measured on the wrong frame. At the swap it was already body-sized; a very bright body's was 1.39×.
- Still open: per-object promotion exactly at the crossover (the fixed 0.02 rad is kept, and the blend makes it seamless); stars (their glare stays as a corona); handing glare to engine bloom once body actors are emissive.

**Milestone:** M3 (promotion actor pool). Render and lifecycle only, no reroll.

**What exists:** `Uni_ProjectObject` (UniProject.ush) already draws a sprite at max(physical angular radius, glare footprint). The glare footprint is one pixel × (1 + `uu.GlareGain` · log2(1 + intensity)). Far away, a star shows as its glare; as you approach, the physical disc overtakes it and the sprite shows the true size. Today the change from one to the other is a kink in max(), not a blend. Promotion to an actor is a fixed θ_promote = 0.02 rad (D-119, about 19 px at 1920 px across 90°).

**Numbers** (1920 px, 90°, gain 0.35, m_lim 6.5):

| Star | Crossover distance | Radius at crossover |
| --- | --- | --- |
| Red dwarf (M = 10, 0.3 R☉) | 0.083 AU | 16 px |
| Sun | 0.26 AU | 17 px |
| Supergiant (M = −7, 500 R☉) | 135 AU | 16.5 px |

Glare grows only with log flux, so every star crosses over at about the same on-screen size, about 16–17 px. That is just under today's θ_promote. The owner's intuition holds: the crossover is the natural swap point, and it already nearly coincides.

**Proposal:**
- Promote per object exactly at the crossover (θ_phys ≥ θ_glare(I)), clamped to a minimum actor size, instead of the fixed 0.02 rad.
- Blend the sprite size from glare to physical over the last octave before the crossover, rather than using max().
- At handoff the actor shows its true disc and the engine's bloom takes over the glare. Its emissive output is matched to the sprite's flux, so total light is conserved and nothing pops.
- Planets (B-002) use the same rule with reflected flux.

**Verification:**
- Size is continuous through the crossover (offline).
- Sprite flux equals actor emissive flux at the swap (needs UE, UE-7).
- Hysteresis gives no flicker.

## B-004: Galaxy groups (owner, 2026-11-02) — DONE (D-174, option 2; revised by D-177: groups are labels, dwarfs come from the node matter)

**Owner decision (2026-11-03): option 2, groups from the node list** (next heaviest nodes after the clusters become group objects that emit only faint members; bright members stay web field galaxies; reach a few Mpc). **Status:** not started. Most real galaxies live in groups of a few bright galaxies plus dwarfs. D-151 makes rich and poor clusters (M ≤ −24.5) explicit and leaves groups as field galaxies.

**Why not the same way:** a group as a parent of galaxies must be resident wherever its brightest member can be seen (D-152): about 330 Mpc. Groups are 10–100× more numerous than clusters. At FaintMag −23 that is about 130,000 resident groups (measured: 1.16M before D-152), each a contributor the streamer measures every frame.

**Options:**
1. **Group clumping in the web field.** Give the field galaxies' web a node term like the clusters'. The galaxies themselves crowd into group-sized clumps at nodes, with no new objects. Cheap; rerolls field galaxies; no group objects for gameplay.
2. **Group parents for faint members only.** Groups place only galaxies fainter than about −20; their bright members come from the web. Group reach then drops to a few Mpc.
3. **Spatial index for contributors.** A coarse grid instead of per-frame measurement of every contributor, so 10⁵ contributors cost what 10⁴ do now. Also a performance-pass item.

## B-005: Sampling boxes for galaxies' coarse star levels (D-153 follow-up) — MEASURED (2026-11-03): L75 0.92x, L74 1.01x, L73 1.01x of expected with no envelope clipping (0.2%) or shortfall; boxes give 0.98 / 0.97 / 1.00 but reroll every galaxy's stars. CLOSED (owner, 2026-11-03)

The coarsest star levels (the brightest stars) have cells comparable to a galaxy's influence sphere, so the same probe miss can under-place them. Turning `bSampleBox` on for the galaxy emitter would fix it, but rerolls stars. Measure first (a T2 member census per galaxy, like T2.cluster.members), and decide before content.

## B-006: Cluster realism — DONE (D-172: morphology-density; D-191: brightest cluster galaxies and intracluster light for rich catalogue clusters)

Elliptical and lenticular galaxies dominate cluster cores (the morphology-density relation). Members currently roll the same spiral parameters as field galaxies.
- A galaxy archetype variant, or parameter ranges conditioned on the parent, would add it.
- So would intracluster light: a diffuse glow, about 10–20% of a rich cluster's light, which the volume could carry.
- So would a brightest cluster galaxy at the centre, as an anchor-like first member.

## B-007: Clusters from the adhesion node list (owner, 2026-11-03; D-165 follow-up) — DONE (D-167)
The noise project's `*_nodes.json` lists every node cluster (position in tile units, mass as a share of the tile's mass, heaviest first). Place one cluster per node above a mass cut, at its position, with richness (absolute magnitude, members) from its mass, instead of drawing clusters from the node-matter channel. Needs a catalogue emitter: objects from a list rather than a Poisson draw (per cell: the list entries inside it, on GPU and CPU alike). Field galaxies already exclude the node matter, so nothing is counted twice.

## B-008: March the textured web farther (D-165 follow-up) — CLOSED (D-170: measured, no visible gain)
The analytic tail past the march (0.5 periods) is ~99% of the background's light and uniform, so the adhesion web's structure barely shows in a full frame. With the texture, far samples can read the mip matching the step (not just the pixel) and take log steps, so marching several periods costs tens of steps. Measure the look and the cost; then the per-pixel background at speed.

## B-009: Spread cell submissions over frames (D-168 follow-up) — DONE (D-173: modest; remaining bursts are urgent work)
After D-168 the game thread's remaining spikes at speed are cells entering at once: a coarse window shift or a travel-sensitivity resize brings 500–1,000 cells in one frame, and task building (Submit) plus eviction (Window) cost 20–40 ms on the reference for them. A per-frame budget on cells submitted (nearest the camera first, never one inside the visible radius left waiting) would spread them; T2.stream.responsive bounds what the delay may cost.

## B-010: How visible should the web be in the sky? (owner, art direction; D-170 follow-up) — BUILT AS A SWITCH (D-189: uu.SkyWeb; the owner's A/B picks the default)
Physically, the integrated light of unresolved galaxies is isotropic to a few percent: D-170 marched the web to 3.2 Gpc and the image did not change. The structure that does show is the near ~100 Mpc over a uniform pedestal (the tail). Options: (a) leave it physical; (b) a near-web glow: the web's density within N Mpc added as a faint, non-physical emission with its own gain (render only, like uu.WebDebug but in the beauty pass); (c) dim the pedestal (scale the tail) so the near structure carries the contrast. (b) is the owner's "low-grade field contribution to the background".

## B-011: Galaxy impostors in cluster interiors (D-172 follow-up) — MEASURED, NOT NEEDED (2026-11-04, D-184)
Measured with the warp textures in: a cluster core costs ~11% more (textured galaxies +21%), because far galaxies skip their warps (D-181). Parked; reopen if engine numbers in a rich core say otherwise.
Inside a rich core the volume marches up to 12 galaxies per ray and 128 per frame (D-161 caps), ~36 steps per ray of which 31 are the cluster's own glow. Cheap now; when warp textures go into the galaxy density each galaxy sample costs more. Then: galaxies under N pixels drawn from a cached impostor (a small view-dependent image per galaxy, re-rendered when the view direction changes by a few degrees) instead of marched. Measure with the warp textures first.

## B-012: Port the mature spiral galaxy density generator from UltraLargeScale (owner, 2026-11-03) — INVESTIGATED (2026-11-04; plan in SPRITE_PASS_PLAN §3 A)
Our galaxy field is the basic one (exponential disc, n = 2 bulge, log-spiral arms, D-160/D-163 prefilters, D-172 early types). UltraLargeScale's galaxy density generator (`GalaxyDensityCore.ush`, `GalaxyDensityRaymarch.ush`, ULS_REFERENCE_INDEX) has concepts to keep: warp fields that make discs and arms organic (the noise project's warp textures), and disc-level deformations (warps, lopsidedness and similar) that add variance between galaxies. Read-only reference (CLAUDE.md rule 1): its folder needs to be connected for reading when this starts. Lands with the warp textures; rerolls galaxies' stars (their field changes), so it is a content decision per parameter. Couples with B-011 (a costlier galaxy sample is what makes impostors worth it).

## B-013: Members for procedural clusters, and member generation on the GPU (D-177 follow-up)
D-177 authors bright members only for catalogue clusters (node list), on the CPU, cached per copy (~15 µs per copy cold on the reference). Scenes without a node list keep the old emitter split (all members from the emitter, cluster reach ~330 Mpc). If a scene ever needs many procedural clusters, the same split could run from the cluster emitter's own placement (members of generated clusters drawn on the GPU in the place pass). Worth it only if such a scene matters.

## B-014: Peripheral glow at warp (WARP_PLAN E4 rest; D-192 follow-up)
WARP_PLAN E4 also proposed a slight brightening at the edges of the view at speed, to hide what pops in there (windows lead forward, so peripheral cells are the late ones). Not built: E2's arrival (D-187) already fades late cells in, and a vignette is a matter of taste. Render-only, a few lines in the composite; build it if the owner sees pop-in at the edges at speed.

## B-015: Resolve anywhere (design doc runtime API; D-193 follow-up)
`ResolveCell(FUniCellKey)` / `ResolveObject(FUniObjectId)`: generate a cell and its ancestry on demand, anywhere, for maps, navigation and lookups. A cell's contributors are parent objects whose influence reaches it, from neighbouring cells too, so "one cell per level" is really a minimal window per level. Two ways:
- **(A, recommended) A second, small streamer** whose camera is the target: minimal windows, its own slice of the GPU pool (a few hundred thousand slots) and the same GPU backend, plus a readback of the target cell's range. What it returns is exactly what the renderer would draw there. About 2 GPU round trips per level: well under a second.
- **(B) The CPU reference generator on a worker thread**, through the same shader cores. Simpler, but lesson #1: CPU and GPU float differences could flip a placement near a threshold, so a resolved object could differ from what is drawn (GPU parity, UE-4, is unverified in the engine).

## B-016: OnVisible for leaf objects (stars) (D-193 follow-up)
The visibility watch tests CPU-known objects only. Stars could be watched through the GPU: a small list of (cell index, slot) for watched objects in the frame's table, flagged per cell, tested in the project pass and read back. Build it if gameplay needs to know when a particular star comes into view.

## B-017: Volume march phase 2: accumulating resolve (D-195 follow-up) — PART DONE (D-208: interleave up to 4 x 4 when near still; disc clipping measured, not adopted; jitter + accumulation not built: it adds lag at speed)
Modelled on CloudAtmosphere (`AtmosphereTemporal.usf`, `MainResolveCS`; RenderReference.md):
- one jittered sample per log step (blue noise, R2 rank, golden-ratio shift per frame), so the step can grow further without banding;
- an age-weighted history (EMA) with a moments clamp (mean +/- gamma sigma, gamma 4 -> 1 with motion), replacing D-184/D-194's 2 x 2 slice and neighbour clamp;
- a larger interleave (3 x 3 or 4 x 4 at the same divisor, or full resolution with N x N): a further 2-4x on the march;
- possibly clipping each ray to the galaxy's disc slab (|z| < a few scale heights) and bulge sphere, skipping empty halo steps.
Measure with T2.vol.perf_inside and T2.vol.temporal / temporal_fast. Start once the owner's D-195 numbers are in.

## B-018: Warp effects dim the sky at intergalactic speed (D-196 follow-up)
Owner, with `uu.Warp 1`: at 425 kpc/s inside a cluster almost every galaxy (and the brighter ones' glow) vanished until the camera stopped; `uu.Warp 0` fixed it. Warp is now off by default; the effects were not convincing yet anyway. Suspects to check before turning it back on:
- the D-187 arrival fade (LevelFade x lerp(1, Arrival, warp) from cell ages);
- streaks spreading light over up to 64 px;
- the D-190 Doppler tint;
- something driving Unreal's auto-exposure down (the pawn's own glow dimmed too).
Reproduce offline with T1.warp.* at a cluster-core camera and 7 kpc a frame.

B-018 update (D-199): one cause found and fixed. The warp optics' motion overflowed in float past 6 pc a frame: zero motion with the optics on. That gave every sprite a tail whatever its distance (~55-64 px at full warp) and shortened the volume's rays. At 425 kpc/s that tail is only ~1 px, so check again with `uu.Warp 1`. If galaxies still dim at speed, try `ShowFlag.EyeAdaptation 0` (the pawn dimmed too) and `uu.Warp.Motes 0`.


## B-019: One texture fetch for the background web's two carriers (D-202 follow-up) — DONE (D-206)
At an intergalactic camera the glow is the two periodic web carriers alone (galaxies on walls + filaments + void, dwarfs on the node matter: the same texture, different channel weights), 2 active carriers a step, each sampling the same texel. Evaluating them as a pair (as D-181 pairs a galaxy's emission and dust) halves the texture reads and step hints per step on that path, bit for bit. Build once the owner's profile at speed shows the GPU (not the game thread) over budget with the volume march the bulk of it.

## B-020: One generation round trip (D-203 follow-up) — DONE for leaf cells (D-207; cells with authored objects or emitter children stay two-trip)
With cell glow (D-203) a cell that has not loaded is glow, so latency no longer causes holes, but it decides how much of what is near the camera is glow at a given speed. A cell takes two backend round trips (probe for counts, CPU allocation, then place: ~6 frames, D-122). A GPU prefix sum and allocation for leaf levels (stars: their records never come back) would make it one. Worth it if the owner finds nearby stars stay glow too long at speed.

## B-021: Clipmap density cache for the galaxy the camera is in (D-210 follow-up) — BUILT (D-211), SUPERSEDED by the galaxy map (D-212), REMOVED (D-213)
Owner: cache the galaxy as a density volume instead of evaluating the analytic field at every step. Inside the galaxy the march's per-step cost is the field (D-210 cut ~6% on the CPU reference; the rest is the spiral twice, three texture reads and ~55 transcendentals). A camera-centred clipmap: ~5 nested 96^3-128^3 levels each twice as coarse (voxel size ~ distance, as the march's footprint), holding emission, the palette components and dust (two RGBA16F textures a level, ~30 MB a level at 128^3), baked by a compute pass from the same shader core (one field source) as the camera moves a voxel at that level (a shell, not the whole level); the march samples it for the camera's own galaxy (1-2 filtered reads a step) and keeps the analytic field for every other carrier and for placement. To decide: what the finest level resolves (the warp and knot detail within ~a voxel of the camera), the light kept against the analytic march (T2.vol.perf_inside's bars), the bake's cost per frame while moving, and the background map's interplay. Start: offline prototype in UniVolumeRef against T2.vol.perf_inside / T3.vol.march, then the engine pass.

## B-022: Galaxy impostors from the 2D map (D-212 follow-up)
Owner: the map makes impostors (and top-down views for UI: scanning a galaxy, picking a warp target) nearly free. Its face-on column is analytic (the light's in-plane factor x 2 x the flared scale height; the dust's likewise); an inclined view is that image foreshortened with the dust's attenuation offset by the disc's height, or a short march through the projected thickness at low resolution for the edge-on look. Small maps (128^2-256^2, ~0.1-0.5 MB) baked at the size a galaxy covers on screen, from the same bake, would replace the volume march for nearby galaxies seen from outside (clusters and groups march many at once) and give the sprite -> impostor -> glow handoff one source. Start with a UI top-down render of the home galaxy's map.
- Raised by the paradigm review (2026-11-10, PARADIGMS.md §1): D-219's worst slicing error (1.5-4%) is a galaxy seen from outside at 60 kpc. An impostor drawn from the map has no march and no slicing there. Measure the impostor against the march first (inclinations 0-85°, 20-200 kpc: light, p95, the edge-on failure angle), then T2.vol.slide's 60 kpc flight with it.

## B-023: Fewer stars in dense regions (owner's option (c), D-214 follow-up)
Owner, 2026-11-09: not certain; could be its own isolated pass. A per-cell cap on stars where they are dense (a galaxy's core and inner disc), the rest folded into the galaxy's glow as uu.Star.Keep already does galaxy-wide: individual stars there blend at any distance where many are on screen. Rerolls only the dropped stars if done as a keep-by-rank (draw a keep per star from its own seed). Measure after D-214 in the engine first: if the dense-region stutter is gone at uu.Star.Keep 0.35, this may be fidelity-only.


## B-024: Read the bulge's warp_large only where the bulge is (D-217 side finding) — DONE (D-218)
Today a galaxy sample reads warp_large inside r < 0.5 field units (fading over [0.3, 0.5]), a quarter of the disc's area, though the bulge it moves is ~1% of the light at r = 0.3. Limiting it to r < 8 r_e (0.32 for spirals), fading over [0.6, 1] of that, changes the field 0.07% per point (bulge weight 1.5: 0.13%) and reads it over 10% of the disc's area. One line in Uni_GalaxyTexTerms (the field, the map and placement share it). Stars reroll only where an accept decision flips. Owner's call; measure with the march's texture-bound share first (uu.GalaxyTextures 0).

## B-025: Sliced galaxy map (D-217)
The warp baked into the map as vertical slices (one 3D read instead of warp_fine + the 2D map). Measured offline: with today's warp it needs 16-32 slices to stay within a few percent (4-8 slices soften the vertical wisps, 14-24% per point; 8 slices keep the layering). A warp 4-8x slower in height fits 4-8 slices within ~3% but looks columnar itself. Parked until the owner decides whether one texture fetch of four is worth 8 slices of today's warp (16 MB a galaxy at 512^2).
- Re-measured after D-220 (2026-11-10, PARADIGMS.md §5): the structural warp is coarser, so slices fit better: home 4 slices 10.0%, 8 slices 3.9% per point (views p95 3.4-4.6%); fast warp 8 slices 8.2%; thick disc 8 slices 25.5%. Still 64 MB a galaxy at 1024^2 (16 MB at 512^2) for one fetch, and thick discs fail. Still parked.

## B-026: Slice sliding in motion (D-219 follow-up)
Owner, 2026-11-09: substantial sliding of the temporal slicing in motion. Offline (T2.vol.slide) the march itself does not slide; the slicing's reconstruction is 1.5-4% off with matching crawl at 60 kpc in motion, and N > 2 at slow motion more than doubles it. CloudAtmosphere's jitter and accumulating resolve measured worse on our content (D-219). Next: localise in the engine (uu.VolumeTemporal 0, uu.VolumeInterleave 2), then target what it is: if the slow-motion blocks, tighten D-208's thresholds; if the reconstruction at speed, candidates are marching more than a quarter while moving (N = 1 above a motion, paying the rays), or reprojecting the previous frame's fresh samples rather than its reconstructed output (no repeated resampling).
- D-220 (2026-11-10): the owner's diagnosis of the remaining sliding: the disc warp's fine octaves aliasing in the march. The warp now moves the disc at a structural level and its fine band is a bounded multiplier; check B-026 again in the engine after D-220.

## B-027: Engine review notes, 2026-11-10 (D-221 polish pass; none seen in the engine yet)
Read through the generation backend (probe, place, one trip, poll, timeouts), RenderVolume, RenderBackdrop, the readback rings, the sky light and uu.Perf with nothing wrong found in them. Three edge cases, each small and none worth an untested engine change while the owner is away:
- Changing `uu.GalaxyMap.Slots` or `uu.GalaxyMap.Size` while flying recreates the map array; a placement batch already in flight then reads the fresh, empty map, so its cells get no stars until they are revisited. Fix if it ever matters: drop in-flight batches that read a map (bump the map epoch so they fail closed and resubmit).
- Two game views in one frame (split screen) share the volume's temporal history and background map: the second view marches unsliced (the "same frame rendered again" count in uu.Perf) and its history becomes the first view's next frame. Fix if split screen is ever wanted: key the history and the background map by view (FSceneViewState).
- RenderVolume uploads the carriers and the volume view once per pass (march, reconstruct, composite; the background map once per face): identical small buffers, a few per frame. Cache the uploads per frame if ProfileGPU ever shows them.

## B-028: Split the march by distance: far light cached in world-aligned direction maps (paradigm review, PARADIGMS.md §2) — BUILT, slow only (D-228; UE-59)
From the solar camera, 77-100% of the light is beyond 300 pc and costs 60-77% of the steps (explore.shells, archived). That part moves one march pixel per 0.7 pc of travel at 300 pc. Generalise the background map: the far part (from a split distance R that grows with speed) is marched into a world-aligned cube, its in-view texels refreshed on a parallax budget, and composited as B_near + T_near B_far. Fresh samples are resampled once, not reprojected frame after frame (D-219's open candidate), and turning is free. Open questions: the cube's in-view resolution, cell glow in the far map (VOLUMETRICS §13), and the split distance's curve. Measure first in T2.vol.slide: error and crawl against the fine reference and today's slicing, and steps a frame. After B-022.
- D-227 / D-228 (2026-11-11, owner: switched by speed): measured, then built as the slow-only far cache. Per-face anchors, in-view refreshes spread over 8 frames, R 2 kpc. At speed it pays only while the camera's parallax at R stays under ~0.06 texels a frame (a face's re-march ~3x a frame's march), so it is off there. Open: cell glow in the map (off while cells load); a speed-dependent R (4 kpc would widen the window at 0.46x at rest).

## B-029: Stars by compute splatting (paradigm review, PARADIGMS.md §3) — BUILT (D-223: scatter + quads; UE-56)
Sub-pixel stars added into a fixed-point light buffer with atomics (Schütz et al. 2022), with quads only for stars with halos, spikes or streaks. It costs per star, not per covered pixel. This would give room to raise uu.Star.Keep toward 1. First a ProfileGPU of the sprite pass in a dense core; not worth doing if it is around a millisecond.
- D-223 (2026-11-11, owner: scatter + quads): built as the 2x luminance + 1x colour splat with a shared gather; halo, disc, ellipse and streak sprites stay quads. Next, if UE-56 shows the quads' remainder still matters: tile-gather for the large ones (one pipeline, deterministic), as discussed.

## B-030: Long rays at some azimuths (D-224 side finding) — FOUND AND FIXED (D-225: faint cluster glows supersampled behind the core)
From mid-disc 20 degrees below the plane looking at the core, azimuths 45-105 degrees have rays with 2-3x the steps of the rest (max 206 vs ~100 steps; mean +0.5%). On the GPU a wave runs as long as its longest ray, so where these rays fall decides how many waves pay. Check after UE-57: if a slowdown at those angles remains with the background on its map, find what these rays cross (the bar end-on or an arm along the line of sight) and look at their step hints.

## B-031: Roadmap after the travel look (owner, 2026-11-11)
Once the overarching look and feel is in a good state, short diversions for entity variety, then the gameplay bridge. In order:
- **Nebulae:** emission and reflection nebulae and dark clouds in galaxies, as a field (no explicit entities: keep the cost flat). *Outside view done: D-234; the fly-into layer is B-034.*
- **A galactic star-cluster layer:** clustering at the galaxy level baked as a density field modulating star placement and the glow, not as explicit cluster entities like the universe's (a dense entity layer spiked the cost before). *Done: globulars D-238, open clusters D-239 (UE-69, UE-70). Follow-ups in B-035.*
- **Compact and exotic objects:** stellar and supermassive black holes, quasars, magnetars and the like.
- **The gravity ship:** a pass in the gravity project for a walkable, gravity-enabled ship that flies in real space and drives the speed/scale system at universe scale. It's the bridge from the debug traversal mode to gameplay traversal.

## B-032: Far cache follow-ups (D-228)
- A split distance that grows with speed: 4 kpc is 0.68x today's at 1 pc a frame instead of 0.92x at 2 kpc, but 0.46x at rest instead of 0.38x.
- Cell glow in the far map, so the cache does not switch off while cells load.

## B-033: Motion web follow-ups (D-230)
- Near structure is soft (the web texture's texels are 0.78 Mpc): a finer web texture or a detail octave would let the near fade come in closer.
- The node channel dominates as round blobs. A separate filament / node weight (one more view float) would let the filaments read as threads.

## B-034: Nebulae follow-ups (D-234)
- **The fly-into layer (owner's option 3, second half):** *BUILT (D-237; rebuilt cloud-style in D-241 with the jittered, accumulated march of D-240: structure eroded by two warped detail layers, ~42 pc walls and ~10 pc texture in the Milky Way, level of detail by footprint and, past the detail's reach, by distance; UE-71). Left: ionisation fronts proper (a gradient), and a nebula seen from inside is still dominated by the glow of its far side (the knot read's 125 pc texels).* Original note: inside the camera's galaxy, nebulae with structure of their own at pc scale: finer octaves of the same density in the galaxy's frame (the near fade, uu.Nebula.NearKpc, then shrinks to the new layer's reach), a step hint for the gas layer and the new octaves, and their own colour structure (an [O III] teal core in the brightest, ionisation fronts at the dark clouds' edges).
- *BUILT behind uu.Nebula.Refine, off (D-244): whole-field sub-samples, unbiased; ~2.5x the galaxy work along the disc at 4. Left: a cheaper source for the sub-samples (the young light without the full field), if the engine says the cost matters.* **Cloud-style coarse/fine stepping (owner 2026-11-14: the dust and nebulae are cloud rendering without the light pass, so the marches should match or say why not):** CloudAtmosphere steps coarsely through empty space and switches to fine steps once it meets density. Ours steps by log ratio only, and D-241's distance level of detail blends knots past ~1-2 kpc along the disc to their mean (one frame's noise 6.4% with it, 85% without). A switch driven by the structure field itself (X > 0 at the coarse sample: refine, step back) would keep far knots sharp at a bounded cost. The D-237 attempt eased the step ratio per ray from the last sample, which made patches; the cloud form refines within the step and returns. Remaining divergences, with reasons: log steps (one ray crosses pc to Mpc), no light march (emission + extinction: samples are cheap, so smaller steps are affordable where structure is).
- *BUILT (D-242, UE-72; uu.Nebula.Dark): each nebula's dark share, dark dust added in its structure up close. Left: stronger clouds if the engine shows them too faint; pillars (a 3D Worley through the texture project) for the dark clouds' shapes.* **Luminous vs dark dust per nebula (owner 2026-11-14: "nebula currently just have an emissive component ... in reality they would have an emissive component and a non emissive component similar to the galaxy dust lanes ... another variance handle"):** today the dark side is the galaxy's one dust field x the knots (D-234's dark clouds) and D-241's lanes, the same balance for every nebula. A per-nebula dust share from a new hash channel (as the brightness J) would range from glowing H II regions to dark clouds rimmed with glow (pillars), the light balanced on average, the dark dust on the same detail walls. Pairs with the offset dark clouds below. New textures (e.g. a 3D Worley for pillars), if needed, go through the owner's texture-authoring project (specs as in TEXTURE_SPECS).
- **Young stars in nebulae:** placement could weight O/B stars toward the nebulae's density (one field source: the same function), so the clusters lighting them are there when you arrive.
- *ADDRESSED (D-245): the nebulae fade in past the handoff over a 6x zoom (no step). Left: the sprite itself carrying a per-galaxy share (needs per-galaxy parameters in the sprite pass).* **The galaxy sprite lacks the nebulae's light** (a few to ~17% for star-forming spirals): the carrier brightens by that over the 4-8 px hand-off. A per-galaxy nebula share from the quick integral would close it.
- *BUILT (D-243): ionisation fronts inside each nebula (the dark share splits it, bright fronts between).* **Dark clouds offset from the H II regions:** today they share the knots' positions (the dust's arms sit inward, so the clouds fall on the arms' inner flank); a shifted read would put ionisation fronts at cloud edges.


- **Nebulae at range are soft (D-255; owner 2026-11-15: "fidelity is a bit low (chunky nebula at range)"):** the grid's cell is ~ distance / 48 (its box's layout): ~64 pc, ~13 pixels at 3 kpc. Options: 256 cells (x2 finer, ~171 MB); a cubic reconstruction (smoother, not sharper; 8 hardware reads a read); detail at the walk's read points (TRIED 2026-11-15, not adopted: the walls layer, one read per walk read, as a multiplier at 3 kpc added no visible sharpness, light -13%, error 27 -> 31%; the reference's edges are the field's warped erosion of the structure, not texture on top of a blob); ray-guided bricks (GigaVoxels: fine bricks only where rays need them, the real fix for memory against detail).
- **The starlight's step footprint is not mean-preserving (D-255: light -3.7% at 3 kpc at 0.5, -7.4% at 1):** the texture terms' nonlinear modifiers (max(1 + H.y Knot, 0), the fine band) lose light as they fade; their fade means would keep it.

## B-035: Star-cluster follow-ups (D-238, D-239)
- **Open clusters' mass spectrum is top-heavy (D-262: worse on D-256's barred Milky Way, p99 5.7 x 10^4 -> 1.4 x 10^5 stars, max 2.9 x 10^5; T2.galaxy.open_clusters' p99 limit 10^5 -> 2 x 10^5):** a cluster's mass follows the young light at it, so p90 is ~70x the median (real open clusters ~10x). A cluster count that follows the young light (more clusters in the arms, not just bigger ones) needs the young light at the hash: a cheap analytic arm term.
- **Clusters sit on the analytic mid-plane**, the young light on the texture-warped one (~100 pc apart on average): the gathered light is 0.94-0.98 of frac, not 1. D-259: on D-256's Milky Way 0.86 / 0.76 in T2.galaxy.open_clusters' two patches (its limit 15% -> 25%): this is now the item to fix there. A per-cluster height from one texture read at its cell would close it, at a read per cluster (four per volume sample).
- **The brightest stars (the Milky Way's L71+ in the disc, L71+ in the halo) are the smooth disc's and halo's:** the by-cell draw could go past 16 cells if the count's single-thread loop is cheap enough in the engine (UE-70 step 5).
- **Young stars in clusters:** a cluster's stars take the disc's luminosity function; an age per cluster (young: brighter, bluer) would make young clusters stand out.

## B-036: Ideas from the owner's papers (2026-11-15; Docs/iop_paper.pdf, Docs/hal_paper.pdf)
GAMER (Groeneboom & Dahle 2014, ApJ 783:138; offline, fixed steps: appearance, not the march):
- **Twirl:** each component's noise read in coordinates rotated by the arms' winding x tau (their Fig. 12). We have it: D-179's detail twist (Uni_GalaxyTexTerms: rotation by the winding x r/cut x H.z). Their tau differs per component (stars 0.1, dust and disc 0.5): ours is one per galaxy.
- **Arms: Ringermacher & Mead (2009), their Eq. 9-10 and Fig. 7 (owner, 2026-11-15: "particularly useful"; rerolls do not matter for now)** — BUILT (D-256; the parameterization: total winding and openness, see D-256): r(phi) = A / log(B tan(phi / 2N)), inverted theta(r) = 2N atan(e^(A/r) / B). Two winding numbers: N (how tight), B (how open; the bar). The winding is finite at both ends: toward the centre the arms straighten into a bar (theta -> N pi), outward they open (theta -> 2N atan(1/B)); one family spans grand-design, barred and flocculent shapes (their Fig. 7: B 0.05-1, N 1-10). Ours: a log spiral, (u + T u^2 / 2) / tan(p), u = -ln r, winding without limit inward (D-210's rings in the core, handled by starting the arms outside it). Candidate: the arms' centre line from RM (the per-arm pitch factor, jitter, strength, end radius kept), the step hint's |grad Phi| from its derivative (2N A e^(A/r) / B / (r^2 (1 + e^(2A/r) / B^2))), the detail twist from the same winding, a bar term along the straight inner arms; rolls of N and B for the morphology spread (Hubble type from B mostly).
- **Noise shapes per component** — BUILT (D-258: knots to the 8th, a structure read at a quarter of the detail's frequency, its ridged channel to the 4th for the dust and its fBm cubed for the light, each over its mean at the read's mip): dust as the inverse of an fBm sum (sharp dark filaments, soft gaps); star-forming clumps as the sum to the 8th (sparse bright knots); star-forming amplitude following the disc noise's. Not taken: per-component twirl (one per galaxy still).
- **Extinction curve:** Cardelli R_V = 3.1 against our fixed RGB reddening (0.8, 1.0, 1.3) (D-132). Dust on the arms' concave side we already have (D-132's -0.35 rad).
Inria report (Gérôme / Neyret 2026, hal-05713074; procedural star octree, Shadertoy):
- Ray from node boundary to node boundary, the leaf size max(d x pixel angle, s_min) (the D-255 walk's principle, for stars); cell integrals as a 2D mip x an analytic vertical integral (our D-212 map's structure); dust once per cell their open problem (ours: D-255).
- **Explicit stars to a continuum by count per cell** (N ~100-300; the same count drives both, so the light cannot jump at the switch). Ours hands over by angular size (D-129): check the hand-over conserves light along a flight (T3.vol.handoff covers carriers; the star sprites' to the volume glow not measured in motion).
- Early ray stop on sensor saturation (a bright star filling the pixel) as well as on opacity.

## B-037: The march paradigms (2026-11-15; PARADIGMS.md "Volume rendering paradigms"; owner: "lets build it then worry about optimization")
- *FIRST PART BUILT (D-262): the step's run read at up to uu.Volume.SegmentTaps points at the pixel's footprint (default 4). Left: the vertical profiles integrated analytically over the step, so steps need not resolve the gas layer (fewer steps, the taps carrying the detail); the other galaxies (field path) the same way.* **Paradigm C, the camera's galaxy's starlight and dust integrated over each step:** owner 2026-11-15, engine test of D-255 + D-258: "a fidelity LOSS when entering the galaxy proper - the knots are clearer outside before it swaps to the interior march"; `uu.Volume.StepFoot 0` "resolves it". D-255's StepFoot prefilters the in-plane terms (map and textures) isotropically to half the step: right along the ray, a blur across it (a knot 1 kpc away read over ~40 pc). Plan: across the ray the pixel's footprint only; along it the step's in-plane run sampled at the pixel's mip (taps by the texels it crosses, capped; past the cap the taps' footprint grows along the run only), the vertical profiles integrated analytically over the step (exp(-|z| / h) along a straight z(t)), extinction composited per tap. The vertical integral frees the step from the gas layer's thickness, so fewer steps carry more taps. Offline first (against the fine reference: error, frame change, taps a ray), then the engine.
- **Paradigm E, ray-guided nebula bricks:** the grid's bricks at the resolution rays need (requests by footprint, GigaVoxels-style), for the chunky nebulae at range (B-034's last item) and the grid's memory.

## B-038: Retire uu.Nebula.Refine (D-244)
- D-255's cell walk integrates the nebulae exactly within the grid's reach; the refinement (off by default) is biased on D-256's Milky Way (12%, slide 33%: T2.galaxy.nebula_slide reports it, D-259). Remove it with paradigm C, or keep it for outside the grid's reach if C does not cover that.

## B-039: The tuning surface (owner 2026-11-15: "put all the puzzle pieces in place")
- *BUILT (D-261): the page, live binding, auto re-enable, uu.Volume.DustGain. Presets BUILT (D-262: text files, uu.Tuning.SavePreset / LoadPreset). Left: a light gain for the galaxies' glow (the balance's other side, needs a view field), paradigm C as a switch.*
- "the ability to swap between stepwise and voxel marching paradigms; ability to swap volume textures (try different resolutions and fbm octaves/frequencies etc); global multiplier knobs for things like background dust, light/dust balances; steps counts/sizes/expansion rates or governing params, raymarching params basically; and then we want some way to expose this stuff editor side". Plan: one knob list; Project Settings > Plugins > Unified Universe Tuning (UDeveloperSettings, each property bound to its console variable: live in PIE, saved to config); knobs that need uu.Enable re-enable on change; texture names with a reload; presets as data assets for A/B. Then paradigm C as a third march option (B-037).

## B-040: D-256's barred Milky Way moved checks without a found cause (D-262)
- T3.vol.handoff: the blended stage (892 kpc, 24% sprite) at -2.2% (was within 0.9%); all-sprite and all-volume within 1.1%. Suspect the carrier's blend toward its mean (FeatureScale) against the RM arms. Limit 2% -> 2.5%.
- T2.vol.nebula_steps: D-251's gas-layer steps cost 2.1-2.3x D-250's (was 1.76x) from the new brightest nebula. Limit 2x -> 2.5x.
- (Found: T2.gen.fidelity's disc cell r 0.96 -> 0.894 is its Poisson limit, 0.893: the cell is smoother on the new arms; the check now allows the Poisson limit. T2.stream.quick_integral's worst 8.8%: 8192 samples, 5.3%.)

## B-041: The nebula layer under D-258's shaping (D-262)
- With the shaped knots the nebula grid's light falls short of the field's (T2.vol.nebgrid_grain from the brightest nebula: x0.99 / 0.86 / 0.69 at 250 pc / 1 / 3 kpc, error x2.3 D-251's; the knots alone x0.79 at 3 kpc, the structure alone x0.86): the grid holds the dust (and the reflection's multiplier) as cell averages while the march takes the knots' young light per sample, and the sharper knots correlate with the dark clouds (they gather where the knots are). T2.vol.far_split_nebulae (1 kpc above, map 1/4: -5.9%) and T2.galaxy.nebula_detail's jitter comparison moved too. The layer is off by default (D-260): its checks now run with the shaping off (UniVerify::FGalShapeOff) and measure its own machinery. To fix with paradigm E (the bricks), or the young share in the grid.
