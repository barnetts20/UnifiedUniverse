# Planets and the start: design

*Design piece 8 of the ship travel design (SHIPS.md). 2026-11-19. Nothing here is built. §11 has the owner's answers (12:15, D-290). The origin's placement is designed further in §5.1.*

**Why it moves to the front.** Owner, 2026-11-19 12:05: "it may be better to get our actual planet actors getting spawned... that will give us a place to spawn in and get in the ship. Id likely say we start in our home system, on our home planet, as a gravity 3rd person actor. Ship pass will then add our ship nearby, that gives us ground to space takeoff flow to start, gives us something we can use to get ship landing in place etc."

So this piece is the first one to be built. Its end state:
1. Press Play, and you stand on the home planet's surface as the gravity character, under its sky, with the universe overhead.
2. The ship pass then puts the ship nearby.

## 1. What exists on each side

**Unified Universe:**
- **Star systems** activate around stars near the camera (D-142..D-145): planets on Kepler rails at the universe clock, rolled from the star's seed (`BuildSystem`).
- **Planet types:** rocky (0.4–1.8 Earth radii), gas giant (6–12) and ice giant (3–4.5), each with a radius and an albedo, but no mass.
- **Promotion** (D-146, D-147) spawns a placeholder sphere (`AUniBodyActor`) when a body is close or large enough, and places it every frame from its universal coordinate.
- **The demo universe** has one authored galaxy, the Milky Way-like home galaxy (an anchor), and a "solar camera" 8 kpc out in its disc.
  - There is no home *star*: systems exist wherever the generator put stars.

**VoxelPlanets (`APlanetActor`; read only, outside this plugin):**
- **Children:**
  - a voxel terrain;
  - an ocean;
  - a CloudAtmosphere atmosphere (terrestrial clouds and weather, or air only);
  - an `APlanetGravityZone`, already a GravPlugin zone.
- **Radius and seeds:**
  - the radius is the actor's scale, in cm;
  - the seeds: `TerrestrialSettings.Seed`, and the ocean's and atmosphere's, which follow it at -1;
  - the generator: Noise or Terrestrial (baked: time-sliced, cached on disk; `GetBakeProgress`).
- **The driving contract** (from UltraLargeScale's `IPooledActor` / `IStarLit`, which VoxelPlanets implements):
  - a wrapping proxy positions the planet and calls `OnAcquired(WorldRadius)`, which sizes, builds and unhides it;
  - the star system pushes the star's world position every frame (`SetStarWorldPosition`; the atmosphere aims its light at it);
  - `SetDormant` parks it with no per-frame work;
  - `OnReturnToPool` tears it down.
- **The spawn timeline** (`FPlanetSpawnTimeline`) logs when the bake, atmosphere, climate, terrain's first bricks and terrain settled are reached.

**Unified Universe takes the proxy's place:** it positions the planet (in its frame), sizes it, seeds it, pushes its star, parks it and releases it.

## 2. The home system

The start needs a star with an Earth-like planet, at the same place every time.

**Recommended: an authored home star.**
- An anchor (like the home galaxy) placed near the solar camera.
- It has a G-type absolute magnitude and a seed **pinned offline** so that its rolled system has a rocky planet of 0.8–1.0 Earth radii (inside the size the owner has run without issues, §5.1) in the habitable zone (an insolation of 0.7–1.4 Earth's).
- Being authored, it exists on the GPU and the CPU alike by construction. No search at start, and no risk that a CPU search finds a star the GPU does not draw (lesson #1, B-015).
- The rest of its system is whatever its seed rolls: other rocky worlds, giants, possibly a stellar black hole companion (D-281 would be turned off for the home star).
- The owner can name it and later override any of its planets.

**Alternative: found at start.** The nearest qualifying star to the solar camera, by a CPU search through the reference generator. No authoring, but a start-up cost, and the parity risk above.

The home *planet* is the qualifying planet of the home system. Its identity is (home star id, planet ordinal), as every planet's is (D-193).

## 3. Which planets are voxel planets, and when

**By type:**
- **Rocky planets** become `APlanetActor`s with the Terrestrial generator. Their atmosphere is terrestrial, air only, or none (§4).
- **Gas giants** become `APlanetActor`s with the gas giant atmosphere model (`EPlanetAtmosphereType::GasGiant`). The owner (12:15): gas giants are already available through the planet actor. Whether they have a terrain core and an ocean is the planet actor's business; the default is none.
- **Ice giants** keep Unified Universe's placeholder sphere until the planet actor has a model for them (owner: not yet).

**A rocky planet's ladder, from far to near:**

| Range | What draws it | Cost |
| --- | --- | --- |
| far (below a few pixels) | its sprite (as today) | none |
| ~20 to ~60 radii | Unified Universe's placeholder sphere, lit, in the planet's tint (today's promotion) | one mesh |
| inside ~20 radii | the voxel planet, built from ~60 radii so it is ready on arrival | the full planet |

**Handovers:**
- Placeholder to voxel planet is a cross-fade over a few frames, once the planet's spawn timeline reports its atmosphere and first terrain. Until then the placeholder stays.
- Leaving reverses it: dormant at ~60 radii, released at ~100.

**At most two voxel planets live at once** (the one you are at, and the one you are flying to), from a pool of `APlanetActor` instances. The distances are tunables, to be measured against the spawn timeline's numbers for the Terrestrial generator (first bake and cached).

### 3.1 Fast, seamless spawn (owner, 2026-11-19 12:41)

*"We need planet spawn to be as fast and seamless as possible. There is currently around 1-2 seconds of spin up. Much of this can probably be cached at the system level instead of waiting on the planet to spawn."*

**Target:**
- A planet is never seen spinning up.
- Its build never costs a visible frame hitch.
- Returning to a planet you just left is instant.

**The lever is lead time.** The universe knows which planets will be needed long before they are:

| Signal | Lead time before the planet is needed (inside ~20 radii) |
| --- | --- |
| Its system activates (~100 AU for a Sun-like star, D-150) | minutes at interplanetary speed; far longer below it |
| The flight computer plots a course to it (D-285) | the leg's ETA, known in closed form (D-284) |
| Play starts on the home planet | none: the one case with no lead (§7's start-up wait) |

**Four layers, cheapest first.**

1. **Hide it behind the ladder** (no VoxelPlanets change).
   - The voxel planet is acquired at ~60 radii, where it is a few pixels and the placeholder sphere covers it.
   - Its 1–2 s of spin-up happens there, out of sight.
   - The placeholder cross-fades to it only once its spawn timeline reports the atmosphere and first terrain at the current distance.
   - **Navigation prefetch:** plotting a course to a planet acquires it early, at the far end of the trip's ETA.
2. **A warm pool** (no VoxelPlanets change).
   - A planet you leave is made dormant (`SetDormant`: no per-frame work, waking instant), not released.
   - The pool keeps the last few by memory; released oldest first.
   - Coming back is instant.
3. **System-level build kits** (VoxelPlanets change; the owner's suggestion).
   - When a system activates, a background job prepares each of its planets' heavy, deterministic data from seed, radius and settings, before any actor exists:
     - the land bake and its rivers;
     - the noise pack;
     - the climate base and field;
     - the ice shelf;
     - the density compositor's inputs;
     - the atmosphere's and ocean's generated settings.
   - Acquiring the planet hands it its kit, so `Initialize` only wires finished data. Kits are held by planet seed, within a memory budget, and dropped when the system deactivates (least recently used first).
4. **A deeper disk cache** (VoxelPlanets change).
   - The land bake is already cached on disk. The other deterministic stages could be too, keyed the same way.
   - Then the home planet at start and any planet visited before load instead of build.

**Game-thread hitches** are separate from the delay.
- The spawn timeline also logs the game thread's time in each synchronous step (bake gate, climate base, atmosphere config, field, shelf, compositor, terrain, ocean, `Initialize`).
- Layers 1 and 2 hide the delay but not those hitches.
- Layers 3 and 4 remove most of them, because the work happens on a worker or not at all.
- Whatever synchronous work remains gets time-sliced, under ~2 ms a frame.

**Who builds what:**
- **Unified Universe (layers 1–2, and the orchestration for 3–4):** when to prepare which planet, priorities (the home planet, the plotted destination, then nearest first), cancellation on deactivation, the memory budget, the pool.
- **VoxelPlanets (layers 3–4):**
  - an API to prepare a planet's kit without an actor;
  - acquiring with a kit;
  - readiness milestones as queryable state (today they are logged);
  - more stages on disk.
- **VoxelPlanets is outside this plugin** (rule 1), so those changes are the owner's, or need the owner's approval.

**First, measure.** One spawn timeline line from the planet's log, for a first build and for a cached one, says where the 1–2 s goes ("[Planet] <name> spawn timeline ..."). That orders the work: layer 3 is worth it for whichever stages dominate, and nothing else.

**Checks:**
- **Offline:** the orchestration's scheduling (priorities, cancellation, the budget) against a model of stage costs (`T2.planet.prewarm`).
- **In the engine:**
  - no visible spin-up flying in at speed;
  - the worst frame during a planet's build (`uu.Perf` with the spawn timeline);
  - returning to a planet is instant.

## 4. From a universe planet to an `APlanetActor`

Everything the planet actor needs comes from the universe planet's seed and its system, so the same planet is the same world every time and on every machine (one source).

| Planet actor input | From | Rule |
| --- | --- | --- |
| Radius (actor scale) | `FPlanet::RadiusCm` | as is: the planet handles real scale (owner, D-290) |
| `TerrestrialSettings.Seed` | the planet's seed | a new seed channel (`PLANET.SURFACE`); ocean and atmosphere seeds at -1 follow it |
| Generator | owner, D-290 | Terrestrial for every rocky planet (gas giants: the gas giant atmosphere model) |
| Atmosphere on/off, type | a new seeded property, gated by size | gas giants: `GasGiant`. Rocky, radius ≥ 0.5 Earth: `Terrestrial` if its equilibrium temperature allows water, else `AirOnly`. Smaller: none. Its look and extent are the planet actor's own (owner: "it would come from our planet actors") |
| Ocean on/off, sea level | equilibrium temperature (the star's luminosity and the orbit, both known) | oceans between about 250 and 330 K at the surface; frozen or dry outside |
| Gravity zone strength | a mass from radius and type density (SHIPS.md §3.1) | g = GM/R²: Earth density gives 9.8 m/s² × R/R_E, clamped to 0.3–2 g for play (tunable) |
| Star position (`IStarLit`) | the system's star, placed in the frame every frame | pushed each tick, as the ULS proxy did |
| Dormancy and release | §3's ladder | `SetDormant`, `OnReturnToPool` |

**Structure (rule 9's spirit: the core does not know the game):**
- The universe core gains a **body actor factory** interface: given a promoted body (type, radius, seed, system), make or reuse its actor, and update it each frame.
- The placeholder sphere is the default factory.
- A **game module** (the planned `UnifiedUniverseShip`, better named `UnifiedUniverseGame`) registers a VoxelPlanets factory for rocky planets.
- The universe plugin's core modules never depend on VoxelPlanets, or on ULS through it.

## 5. The planet's frame (first version: translation only)

On the planet, the UE world is the planet's frame (D-282, D-287): the planet stays still and the universe moves around it.

**Version 1 (needed for the start):**
- **The frame's origin** is the planet's centre, following its orbit continuously. Unified Universe's anchor becomes a moving one: the planet's centre at the universe clock, in exact coordinates with a sub-centimetre fraction.
- **The universal camera** is that centre plus the view's location.
- **Everything else** (the star, other planets, the backdrop, the light rig) is placed relative to it, as now.
- **The planet actor never moves** while you are near it: not even a re-centre, so there is no voxel-planet move to pay for.
- **No rotation yet** (planets do not spin, §6), so the renderer needs no change for this version: universe axes are still world axes.

**Precision:** with the planet's centre at the origin, a player on an Earth-sized surface is about 6.4 × 10⁸ cm from it. Whether that is fine, or the origin should follow the player along the surface instead, is §5.1's question, decided by an engine test at the start of P1.

**Frame radius:**
- The planet's frame holds within max(3 radii, 1.5 × the atmosphere's top), with 20% hysteresis.
- Beyond it, the frame hands to the ship's (in the ship) or to the inertial frame (on EVA).
- This radius is also where the planet's gravity zone and the tier-1 drive's gate meet (D-284: leave the atmosphere).

### 5.1 Where the world origin sits on a planet (owner, 12:15: "need to design this more")

The planet's frame fixes the axes to the planet and keeps it from moving continuously. That is settled (D-282, D-287). What is open is *where in that frame the UE origin sits*. There are two candidates, plus a test that decides between them.

**A. The planet's centre at the origin.**
- **For:** the planet never moves, not even once. Its terrain, ocean, atmosphere and gravity zone are static for as long as you are near it. Simplest code, and nothing to hitch.
- **Against:** the player stands about 6.4 × 10⁸ cm from the origin (Earth-sized; up to 1.15 × 10⁹ cm on the largest rocky planets, ~1.9 × 10⁹ cm at the frame's edge). Unreal 5 holds positions in double (large world coordinates), and rendering is camera-relative, so transforms and drawing are fine there. What may not be fine is anything that does arithmetic in float on *absolute* positions:
  - materials using absolute world position (a float's step at 6.4 × 10⁸ cm is ~64 cm, so triplanar or noise texturing would swim or block up) unless they use the LWC-aware nodes or planet-local positions;
  - any physics or movement maths done in float on world positions;
  - Niagara and decals without LWC handling;
  - CloudAtmosphere and the ocean, if they work in world rather than planet-local space.

**B. The origin near the player, re-centred along the surface.**
- **What it is:** the frame's axes stay fixed to the planet, but the origin is moved to the player's neighbourhood whenever they get more than a threshold from it (10–1,000 km, tunable): a translation within the planet's frame, like today's 1,000 km re-centre.
- **For:** everything near the player is within metres to kilometres of the origin, so float maths anywhere is safe.
- **Against:**
  - **The planet actor moves at each re-centre** (its terrain components, collision, ocean and atmosphere): a one-frame cost, unknown until measured.
  - **How often it happens:** walking, never; flying low at 300 m/s, every ~55 min at 1,000 km; a ship climbing to orbit, a few times.
  - **Smears:** the renderer's previous-frame view must move too, or a frame smears (pitfall 13, SHIPS.md §10).
- **Not the same as UltraLargeScale's approach,** where (as its parallax proxy reads) the planet was re-placed relative to the camera *every frame*, continuously. B moves it only at re-centres, which are rare and whole.

**The test that decides (first step of phase P1):** an engine scene with the home planet built at real scale and the player on its surface, run once with A and once with B, checking:

| Check | Pass |
| --- | --- |
| Character movement and camera | no jitter walking, jumping, standing still for a minute |
| Collision | no sinking, catching or sliding on terrain; physics props settle and stay |
| Terrain and ocean materials | no swimming, banding or blocky texture up close |
| Atmosphere and clouds | as at small scale; no shimmer near the player |
| Shadows, TSR and motion blur | clean while walking and turning |
| Ship-scale motion | a test pawn flying at 1 km/s low over the terrain: smooth |
| B only: a re-centre | its hitch in ms (`stat unit`), and no visible jump or smear |

**Evidence so far (owner, 12:20):** "around 5kkm radius there were no issues, will need more testing at larger sizes than that".
- Rocky planets roll 0.4–1.8 Earth radii (about 2,500–11,500 km), so the test runs at two sizes: the home planet's, and the largest rocky planet's (11,500 km).
- To stay near the known-good size, the home planet's pinned seed is chosen for a radius of 0.8–1.0 Earth (5,100–6,400 km) (§2).
- If the largest sizes show problems that neither origin candidate fixes, the fallback is a cap on the rocky radius roll. That changes those planets' sizes and brightness (a content change), so it waits for the test.
- Confirmed (owner, 12:24: "world was at origin in that test"): those runs had the planet at the world origin, so **candidate A is known good at ~5,000 km**.

- **Decision (D-291): A is the default.** The planet's centre sits at the origin and the planet never moves. The home planet (0.8–1.0 Earth radii) sits at about the proven size.
  - **The engine test narrows to larger planets:** 1.3 and 1.8 Earth radii (8,300 and 11,500 km), run when phase P1 has planets of those sizes to spawn.
  - **B (surface re-centring) is the fallback,** for sizes where A shows problems.
  - **Materials-only failures** are fixed in the materials first.

- **Original recommendation (before the owner's evidence):** try A first. It is simpler, and the planet would never move.
  - **If A fails,** the failures say where: if only materials fail, the fix may be in materials (LWC nodes, planet-local positions) rather than in the frame.
  - **If A fails broadly,** B, with the threshold set by the measured hitch.
- **Either way, the rest of this design is unchanged:** placement, the universe, the ship and the frame hand-overs all work from the frame's origin, wherever it is.
- **An offline check** (`T1.frame.recentre_body`) proves that B's re-centre preserves every universal position and velocity exactly, as `T1.frame.*` does for the others.

## 6. Spin (second version)

**The property:** planets spin (owner: "necessary"). It is a new seeded property of each planet, so nothing existing rerolls:

| Type | Rotation period | Axial tilt |
| --- | --- | --- |
| Rocky | 10–100 h, log-uniform | 0–40° mostly; rare large values |
| Giants | 9–20 h | — |

Plus a phase at the epoch. Tidally locked planets close to their star are an option (period = orbit, for very close planets).

**What it adds:**
- The planet's frame turns with the planet, so the sun rises and sets, and the stars wheel overhead.
- Launch speeds include the ground's speed (in the frame, the ground is still; in space, the ship carries the surface's velocity: D-287's hand-over).

**What it needs:** the frame rotation in the renderer and in placement (SHIPS.md S0). It is built as the second version, after the start works without it. Until then the home planet's day is fixed: the spawn point is on the day side (§7).

## 7. The start flow

1. **Play.** Unified Universe enables the home scene with the camera at the home planet's spawn point. The home system activates at once (its star is within reach), and the home planet is the body whose frame the world is in.
2. **The planet builds.** The planet actor is acquired at the frame's origin, sized, seeded and given its star. The bake (cached after the first run) and the terrain's first fill follow its spawn timeline.
   - Meanwhile the player sees the sky from the spawn point: the universe renders from the first frames.
   - The player's pawn is held in place, not simulated, so it cannot fall through terrain that does not exist yet.
3. **The ground is there.** When the terrain has settled at the spawn point (a downward trace hits), the pawn is placed on the surface and released.
   - **The pawn:** the project's gravity third-person character, through its game mode. The planet's gravity zone holds it.
4. **The ship** (ship pass): spawned on the flattest ground within ~50–150 m of the spawn point, resting on its landing gear.

**The spawn point** is deterministic from the home planet's seed: a land point (above sea level, gentle slope) at mid-latitude on the side facing the star. It is found by trying candidate directions from the seed against the planet's own height and water queries once its density is built, before the terrain is meshed.

**Start-up wait:** this is not a load screen. The sky and universe are live while the ground forms. First-run bake times are the owner's numbers (the spawn timeline logs them). Cached runs should be short.

## 8. Takeoff and landing (bridging to the ship)

**Takeoff:**
1. The ship lifts off in the planet's frame (it moves, the player inside rides on framed movement, props are stowed: SHIPS.md §4.3).
2. It climbs through the atmosphere under Newtonian flight with drag.
3. At the planet's frame radius the world hands over to the ship's frame: an invisible change (D-287). Interplanetary drive is allowed past the atmosphere (D-284).

**Landing:**
1. Reverse: the ship drops out of warp at the planet's well edge (D-284) and coasts in.
2. At the frame radius the world hands to the planet's frame.
3. The ship descends through the atmosphere and touches down on its landing gear's contact probes (SHIPS.md §5.1).
4. Once landed it is still in the planet's frame: boarding and leaving are walking (D-287).

## 9. What this piece builds, when building starts

**Phase P1, the start** (before SHIPS.md's S0). *Built 2026-11-19 (D-293, UE-104), except the origin test at large radii, navigation prefetch and layers 3–4. D-297: the ladder is sprite → voxel planet from promotion (~50 radii), the voxel planet at its true place and size outside the frame, no placeholder sphere.*
- The home star anchor and its pinned seed (offline search, `T1.home.system`).
- The body actor factory interface in the core, with the placeholder as the default.
- The game module with the VoxelPlanets factory (the dependency approved, D-290).
- First: the origin test of §5.1.
- The moving anchor: the planet's frame v1, translation only.
- Planet properties from seeds: surface seed, atmosphere and ocean rules, gravity.
- `IStarLit` pushing.
- The spawn point and the start flow (`uu.AutoStart home`).
- The ladder (placeholder ↔ voxel planet), with pooling and dormancy: §3.1's layers 1–2 (lead time, warm pool, navigation prefetch).
- After the owner's spawn timeline numbers: §3.1's layers 3–4 (system-level build kits, deeper disk cache), with the VoxelPlanets side agreed with the owner.

**Phase P2, spin:** with SHIPS.md S0 (the renderer's frame rotation).

**Then SHIPS.md S1:** the omni ship spawned beside the player.

**Offline checks:**
- `T1.home.system`: the home system is deterministic and qualifies.
- `T1.home.spawn`: spawn point rules (from a stand-in height function).
- `T1.frame.moving_anchor`: the planet's centre is exact at the origin over an orbit; universal positions are preserved.
- `T1.planet.properties`: deterministic properties, and the rules' ranges.

**Engine checks:**
- Play starts on the surface of a planet under a live sky.
- No fall-through.
- The sun's direction matches the universe's.
- Walking and the gravity zone.
- Flying away (free flight, before the ship exists) shows the hand to the placeholder and back.

## 10. Risks and unknowns

- **Real scale:** whether the voxel planet holds a 2,500–11,500 km radius well (terrain precision, collision, ocean, atmosphere scale).
- **Bake and build time:** on first run for the home planet, and for other planets on approach. This decides the ladder's distances.
- **Two voxel planets alive at once** (memory, the atmosphere's simulation budget): may need to be one, with the destination's build starting when the first is released.
- **Dependency chain:** VoxelPlanets depends on UltraLargeScale for `IPooledActor` / `IStarLit`, so the game module loads ULS with it.
- **CloudAtmosphere and the universe's sky:** the atmosphere composites over the universe (D-246's order). The sky seen from the ground through the atmosphere needs an engine look.

## 11. Owner decisions (2026-11-19 12:15; D-290)

1. **Real scale:** the voxel planets handle real scale. Planets keep the universe's radii, 0.4–1.8 Earth radii for rocky ones.
2. **The origin on a planet:** needs more design. §5.1 sets out the two candidates (the planet's centre at the origin; the origin near the player, re-centred along the surface) and the engine test that decides, first in phase P1.
3. **Generator:** Terrestrial.
4. **Home system:** an authored home star with a fixed seed.
5. **Giants:** gas giants are available through the planet actor (its gas giant atmosphere model) and become planet actors. Ice giants keep the placeholder until the planet actor has a model for them.
6. **Dependency:** yes. A Unified Universe game module depends on VoxelPlanets, and through it on UltraLargeScale.

## 12. The planet's look at real scale (D-294; owner, 2026-11-19 14:17)

The first run from orbit showed continental-scale streaks of cloud and a very thick layer. Part of that was the bare planet actor running on the atmosphere's class defaults (fixed: tuned sets by default). The rest is scale: CloudAtmosphere sizes everything as a fraction of the planet's radius (`HeightScale`), validated from 0.05 up, so on a real 5,000 km planet its thinnest atmosphere is ~250 km and its cloud layer about 20 times Earth's. The galaxy march met the same problems (a thin layer in a large volume; detail finer than a step), and these of its lessons carry over. Each is a CloudAtmosphere change (the owner's plugin, a frozen core: approval first), in order of payoff:

1. **A thin shell at real scale.** `HeightScale` ~0.003–0.015 (an Earth-like 15–75 km atmosphere, a 10–15 km weather layer). The cloud lattice's floor (band depth / `CloudSteps`) already shrinks with the band, so near steps get finer for free; what breaks is grazing chords (a 12 km slab at 5,000 km radius has horizontal chords of ~700 km) and the shadow bake's precision near the terminator. The galaxy's answer to its thin disc: steps capped inside the slab and long outside it (D-251, D-265), and the run of a long step read at a few cheap taps instead of more full steps (paradigm C, D-262: per-pixel error 3.1% → 0.7% for ×1.9 the march time).
2. **The detail read over the step, not just the pixel.** The march's detail footprint is the pixel cone alone; a step much longer than the detail's feature draws it at one jittered point, which is grain (D-251's measurement). Read over max(pixel footprint, k × the step's length) and the step's detail becomes its mean (blur, never grain; D-265's "the fade is the step's"). Alone it helped the nebulae little (6.1% → 5.9%); with taps it is what makes taps cheap.
3. **A bubble of detail with steps from its frequency** (D-265, D-266): detail layers read only within a range of the camera, the step capped by each layer's frequency inside it, so the far march pays nothing. CloudAtmosphere has the fade (`DetailFadeNear` / `Far`) but not the step cap.
4. **Strands: shape the noise, don't only advect it.** The flow carries and stretches the noise (`WarpStretch`), so a long-lived pattern becomes streaks along the wind. The galaxy's shaping (D-258, after GAMER: a noise sum to a power over its mean, ridged noise to the 4th for lanes) makes clumps and gaps at the same mean; erosion by warped detail (D-241) breaks a streak's edges. On the cloud side: lower `WarpStretch` and a shaped coverage noise, the tune first, then the field.
5. **Already shared:** blue noise per cell, the full-resolution resolve with a variance clamp, the composite before TSR (D-246 took these from CloudAtmosphere).

*Built 2026-11-19 (D-295, owner's approval): items 1 and 2 as CloudAtmosphere stage RS1 (`Sampling.StepTaps`, `Sampling.StepFootprint`; class defaults off, the universe's planets 4 and 0.5), and real-scale shells per planet through the seeded actor's locks (below). Items 3 and 4 remain.*

What the universe side can do without touching CloudAtmosphere: pick the sets and looks (done), and pass a per-planet `HeightScale` through the atmosphere's runtime API once the core supports thin shells (the planet's air from its gravity and temperature: a scale height `kT / (m g)`).

