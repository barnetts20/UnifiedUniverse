# Backlog

*Ideas and deferred work not yet scheduled. Each item says where it would land and what it depends on. Decisions still go to `DECISIONS.md` once taken.*

## B-001: Warp streaks from late-loading stars (owner idea, 2026-11-01)

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

## B-002: Star systems emitting planets (owner request, 2026-11-01)

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

**Questions for the owner before starting:**
- On-rails orbits for v1, or do you want n-body (it would not be deterministic)?
- How fast should the default clock run? Real time makes planets look static.
- Do binaries and multiple stars come in v1?

## B-003: Promote at the glare/physical-size crossover (owner question, 2026-11-01)

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
