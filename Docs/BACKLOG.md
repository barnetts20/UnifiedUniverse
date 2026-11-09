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
