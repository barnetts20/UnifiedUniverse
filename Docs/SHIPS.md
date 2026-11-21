# Ships, gravity and navigation: the framework plan

*Design, 2026-11-19. Nothing here is built yet. The owner asked for planning and gameplay design of ship travel first, not an implementation (2026-11-19 00:19).*

*How it is laid out:*
- *Sections 2–5: the framework the design rests on.*
- *Section 6: the travel design.*
- *Section 8: ships as capability sets, starting with the omni ship.*
- *Section 14: the owner's answers to the open questions (00:42), recorded as D-282..D-286.*

**Design pieces, in order** (owner, 2026-11-19 00:50: piece by piece, order left to us). Each piece gets its own document, and this one keeps the summary:

1. Scanning and exploration: Docs/SHIPS_EXPLORATION.md (done; the owner's answers in its §9). Follow-ups:
   - 1b: the bands in detail (owner: "needs more design");
   - 1c: a procedural naming scheme for objects (owner: "we will need to come up with" one).
2. Navigation: targets, route planning, the computer's plots and the route follower.
3. The HUD and map modes.
4. Flight at scale 1: the feel, the flight computer and its assists.
5. The interior and the seat: interaction, possession, EVA, hatches and airlocks (boarding's rules are in §2.1).
6. The omni ship's capabilities and resources in detail: the panel, gauges, numbers.
7. Gates and carriers.
8. Planets (added 2026-11-19 11:53): Docs/SHIPS_PLANETS.md (designed 12:05, ahead of the rest at the owner's request: start on the home planet as the gravity character, the ship nearby). It is built first, as phase P1 before S0.
9. Persistence (added 11:53): save and load (ship state, frame, chart, resources); the ship as a universe object while you are away from it.

Multiplayer is not a piece: §2.2 notes what it would take, and the design rules that keep it possible.

Owner's brief (2026-11-19):
- A ship to travel the universe in, replacing the debug flight.
- A walkable interior. The ship has its own gravity, and it is pulled by larger fields.
- Windows, so the motion can be checked from inside.
- A pilot chair that hands over the controls and the HUD.
- A physics-based flight model at speed scale 1, and a model for the scales above it: interplanetary, interstellar and intergalactic, with fuels, modes, autopilots and warp ramps.
- Scanning, mapping and route planning over a universe nobody has charted.
- Weapons later, built so they fit.
- Modular ships: a frame of hardpoints carrying thrusters, drives, shields, scanners, computers, cooling, power, cloaking, armour and so on. This comes after the framework.
- Navigation display modes.
- Seamless boarding (added 08:51): walk or float aboard through a hatch or door, with no load screen and no teleport (§2.1).

Known hurdles:
- nested gravity fields;
- the player's weight pushing on the ship;
- momentum flinging the player off moving or spinning platforms;
- the same thing on an orbiting planet;
- animations that assume −Z is down.

## 1. What exists today

**GravPlugin** (`Plugins/GravPlugin`, 738 lines):

- `AGravityZone` (exported):
  - `Priority`: only the highest-priority zones an actor overlaps apply.
  - `ExcludeTags`.
  - `BaseVector`.
  - an overridable `GetGravityVector(WorldPos)`.
  - Overlaps are reported by the zone's Blueprint calling `OnZoneBeginOverlap` / `OnZoneEndOverlap`.
- `UGravityManager` (exported tickable world subsystem): every tick, for each actor in a zone, it sums the zones of the top priority.
  - A grounded character takes only the strongest of equal-priority zones.
  - Characters get `CMC::SetGravityDirection` and `GravityScale`.
  - Simulating primitives with engine gravity off get `AddForce`.
  - An actor in no zone keeps its last gravity.
- `AGravityController`: a gravity-relative view (pitch accumulator, eased gravity change). It is **not exported** (no `GRAVPLUGIN_API`), so another module cannot subclass it.

**VoxelPlanets:** `APlanetGravityZone` is already an `AGravityZone`: spherical, `BaseVector` × (R/d)^Falloff, with an influence of 2 radii. `APlanetActor` is a large compound actor (adaptive voxel terrain, ocean, CloudAtmosphere atmosphere, gravity zone) with child actors.

**Unified Universe:**
- **Motion modes (D-008, D-149).** In Local mode the pawn moves through the UE world: camera = anchor + view location, re-centred by whole centimetres past 1,000 km. In Cruise the pawn is pinned and the universal camera integrates.
- **Placement (D-146).** Promoted bodies (`AUniBodyActor`) and anything with a `UUniParallaxComponent` are placed every frame from their universal coordinate.
- **Orbits.** Planets orbit on the universe clock, so a promoted planet already moves through the UE world every frame.
- **Axes.** Universe axes are UE world axes everywhere: every render pass takes its view basis straight from the engine view.
- **Missing pieces.** There is no rotation between the two, no planet spin and no planet mass (radius and type only).
- **Generation and lookups.** A CPU reference generator exists (`FRefGenerator::GenerateCell`), so the design's `ResolveObject` (M4, not built) can be built on the CPU for navigation.

## 2. The central idea: the world is the frame you are standing in

Every hurdle on the owner's list comes from the same thing: the player's ground moves through the UE world. That ground can be a spinning platform, a manoeuvring ship or an orbiting planet. Physics then sees accelerations that gameplay does not want:
- the fling, from centrifugal force and the velocity left behind when the player leaves the floor;
- the push on the platform, from the character standing on a simulated body.

**The rule:** never move what the player stands in. The UE world is bound to one *reference frame* at a time, and the rest of the universe is placed around it.

| Frame | When | The UE world is | What moves |
| --- | --- | --- | --- |
| **Ship** | the player is aboard a ship (or near it on EVA) in open space | the ship's body frame: the ship sits at the origin, unrotated, for good | the universe: planets, stars, the backdrop, other ships |
| **Body** | the player (or the ship carrying them) is within a planet's frame radius: low orbit, atmosphere, the surface | the planet's frame: its centre at the origin, turning with its spin once spin exists | ships, which fly through it; the sun and sky turn overhead |
| **Inertial** | the player is on foot far from both | today's Local anchor (no rotation, re-centred) | everything as today |

**Consequences:**

- **Ship interior.** It is static in the UE world whatever the ship does. Walking, jumping and loose props are ordinary Chaos physics under a constant gravity. No fling, no lag and nothing to compensate.
- **Planet surface.** It is static too, so the planet is never moved under the player. A subdivided voxel planet, its ocean and its atmosphere stay put while you are near them. They are moved only while far away (compressed placement, as today), where collision does not matter.
- **The cost.** The renderer must accept a rotation between universe axes and UE world axes; today they are identical (§11.2). It is one 3×3 matrix in the view setup of each pass. It is mechanical, it can be checked offline, and it can be done exactly.
- **Switching frames.** A frame change (ship → body on entering a planet's frame radius, and back) applies one rigid transform to every actor in the world that the universe does not place, rotating velocities too. This is today's `OnLocalRecentre` with a rotation. It is rare, with hysteresis, and happens where little is going on (high above the planet).
- **A ship flying inside a Body frame** (atmosphere, landing) moves through the world. The player inside it then needs the framed character movement of §4.3, and loose props are *stowed* (asleep and carried with the ship) until it lands or the frame goes back to the ship. Both are standard. They are limited to atmospheric flight, which is gentle compared with a combat spin in space.

**Why not only "the ship moves and the character rides it"?** That is the usual moving-base approach, and §4.3 builds it anyway for atmospheric flight. On its own it fixes the player but not props, not other characters and not anything physics-simulated inside. It is also what the owner's spinning platform already does and where the fling came from. Pinning the frame removes the problem instead of compensating for it.

**Multiplayer note:** a UE world can be pinned to only one frame, and Unified Universe has one universal camera. So this is single-player, which matches the plugin today. Several players in different places would need per-player frames: §2.2 says what that would take.

### 2.1 Seamless boarding (owner, 2026-11-19 08:51; D-287)

*"Seamless entry exit of the ship, no load screen or teleporting - you walk or float on board through a hatch/door."*

**Which frame applies:**
- **The frame is the outermost moving thing the player is in or near.** Within a planet's frame radius that is the planet, even aboard a landed or low-flying ship. In open space it is the ship (or the carrier a docked shuttle sits in).
- **The ship's frame radius** (default 10 hull lengths, at least 1 km) covers EVA around it. A ship frame stays active whether the player is aboard or floating beside the hull.

**Boarding in the common cases needs no frame change:**
- **Landed:** the Body frame holds before, during and after you walk up the ramp. The ship is static in it, so boarding is walking into a building.
- **In space:** the Ship frame holds while you float to the airlock and through it.
- **A docked shuttle in a carrier's hangar:** the carrier's frame holds. The shuttle is static in it while docked.

**The ship is always fully there.** Its interior is part of the ship actor, with no level streaming and no load. Very large ships may stream interior *detail* by distance later, within the same world.

**Gravity at a threshold comes on gradually.**
- GravPlugin's priorities switch zones instantly, so each hatch or airlock gets a **transition zone**, a priority between outside and inside. Its gravity blends from the outside field to the ship's over its depth (about 1 m, smoothstep), in magnitude and direction.
- **Floating in:** gravity comes on over the threshold, and the character rights itself toward the ship's floor over about half a second (the gravity controller's easing).
- **Landed on a slope:** the planet's down turns into the ship's down over the ramp.

**Velocity at a threshold follows physics.**
- Inside the damped interior, movement is relative to the ship (§3.3, §4.3).
- **Leaving the interior volume**, the character keeps the ship's velocity plus its own relative velocity. Step out of a coasting ship and you coast beside it; jump from a ship flying through an atmosphere and you fall with its speed.
- **In the Ship frame this is automatic** (the ship *is* the frame). In a Body frame it is the hand-over at the volume's edge.

**Frame changes, when they happen, are invisible.**
- **When they happen:**
  - flying into or out of a planet's frame radius;
  - EVA drifting beyond the ship's frame radius;
  - a shuttle launching from a carrier.
- **What happens:** every actor moves by the same rigid transform, so the view of the scene does not change.
- **Two things make that true in practice:**
  - physics and movement velocities are rotated with the transform (Chaos teleport with velocities, the character movement's base and velocity);
  - the renderer's previous-frame view is moved by the same transform, so temporal antialiasing and motion blur reproject correctly.
  - A history reset on that frame is the fallback.
- **Today's 1,000 km re-centre** moves actors but not that history (UniSubsystem `RecentreLocalIfNeeded`), so it may show a one-frame smear. The frame work fixes both together. Worth watching for in the engine now.

**Doors are physical:**
- hatches, ramps and airlocks are animated components with collision;
- an airlock cycle (both doors, pressure) is optional gameplay;
- no fades, cuts or teleports anywhere in boarding.

**Warp and crew outside:** a drive does not engage with a hatch open or the player outside. The computer says why, rather than leaving anyone behind.


### 2.2 What multiplayer would take (owner, 2026-11-19 11:53: single-player now; "curious what it would take")

Not a requirement. This note keeps the door open and says what to avoid closing it.

**What is already in its favour: the universe is deterministic.**
- Every client can generate its own universe around its own camera, from the same seeds, with no network traffic for content.
- Only *changes* would travel: the delta store (renames, discoveries, removals), plus dynamic things (ships, players, props).
- This is the hard part of most space games, and here it is free.

**What is in the way: one world, one frame.**
- A UE world is pinned to one frame (§2), and the server has one physics world.
- Two players in different places (one in a ship between stars, one on a planet) cannot share one pinned world.

**How it would work: islands.**
- Each place where players are (a ship's interior, a patch of planet surface) is a physics *island* with its own local frame.
  - **On the server:** islands sit far apart in its one world, as separate pockets, and the server simulates each in its own frame.
  - **On each client:** the client pins its world to its own player's island (§2, unchanged). It places the other islands' contents around it from universal coordinates, as it already places the universe.
  - **Players sharing an island** (the same ship, the same landing site) share a frame, so ordinary UE replication and character prediction work inside it.
- **Between islands** only coarse state travels: a ship's universal position, attitude and velocity. They come from the deterministic, fixed-step flight core (`UniFlight.h`), so the server can step them and clients can predict them.
- **The server never renders**, and generates only what physics needs near islands: planet terrain where someone stands, the active system's bodies for gravity.

**The cost:** a milestone of its own. Islands on the server, replication of universal coordinates, per-client frames, per-island promotion and systems (the subsystem assumes one camera today), and the voxel planet on the server.

**What keeps it possible now, at little cost** (adopted as design rules, D-289):
1. Authoritative state lives in universal or container-local coordinates, never in UE world coordinates: ships in `FUniCoord`, characters aboard relative to their ship.
2. Ship simulation is fixed-step and deterministic in `UniCore`, testable and replayable.
3. The frame code is written in terms of an *island* (the thing the frame is pinned to), even though single-player only ever has one.
4. No new code assumes "the first player controller" where it could take a player or an island instead.

## 3. Gravity: one field, apparent gravity, damped interiors

### 3.1 One field source

Universe gravity comes from one engine-agnostic function, `UniCore::GravityAt(coord, time)`. It sums the active system's star, planets and stellar black hole at their positions on the universe clock (the same orbits that place them).
- Planets get a mass from radius and type: a density per type, about 5.5 / 1.3 / 2 g/cm³ for rocky / gas / ice, as a seed-free derived value.
- The ship's flight model integrates this field.
- Characters receive the same field through GravPlugin zones placed on promoted bodies (`AUniBodyGravityZone : AGravityZone`), whose `GetGravityVector` converts a world position to a universal one and calls the same function.
- The owner's `APlanetGravityZone`, once it replaces the placeholder, takes its surface gravity from the same mass, so the two never disagree.

### 3.2 Apparent gravity in a frame

In any frame, the gravity an object feels is the true field minus the frame's own acceleration (plus rotation terms in a spinning frame). This is the equivalence principle, and it is what makes EVA correct:
- **EVA next to a coasting ship:** ship and astronaut fall together, so the apparent gravity is about 0. The astronaut floats beside the ship in orbit.
- **The ship thrusts:** the astronaut is left behind at the thrust's acceleration, as they should be.
- **On a planet in its own frame:** the surface gravity, as everyone expects. The star's tidal pull (~10⁻⁷ g) is dropped.

### 3.3 Damped interiors (the owner's "move in the ship's frame")

A ship's interior zone (`AUniShipGravityZone : AGravityZone`):
- covers the walkable volume;
- has a high priority (1000), so planet zones (100s) never apply inside;
- excludes the ship itself through `ExcludeTags` (the owner's point);
- returns ship-down × g (default 1 g).

Inside it the frame's acceleration is **not** applied: the interior has "inertial dampeners". The hull still carries the player through every manoeuvre, and nothing inside feels it. A small felt fraction (camera shake, a lean of the view) can be added for feel, render-only.

### 3.4 Nothing inside can push the ship

The ship is **kinematic**: it is moved by its own integrator (§5), never by Chaos forces. A character standing in it, or a prop resting on its floor, cannot push or spin it. As belt and braces, the character's movement component has `bEnablePhysicsInteraction` off and `StandingDownwardForceScale` 0 against ship hulls, which removes the push that spun the owner's platform.

The ship's mass comes from its modules (§8) and enters only the flight model.

### 3.5 GravPlugin fit

- **What works unchanged:** zones, priorities and exclusions. The zones are C++ subclasses that bind their own overlap events and call the plugin's notifications, so no Blueprint is needed.
- **Gap 1, the controller:** `AGravityController` is not exported. Either it is exported (one word, in the owner's plugin), or Unified Universe ships its own frame-aware controller with the same view logic.
- **Gap 2, apparent gravity outside zones:** "in no zone" keeps the last gravity, while EVA needs "true field − frame acceleration". A small Unified Universe component on characters sets this after the manager, or the plugin gains a hook for an additive field.
- **The owner approved both changes (D-283):**
  - export `AGravityController`;
  - an additive-field hook for apparent gravity.

  Nothing else in GravPlugin changes without asking.

## 4. The ship as actors, and walking around in it

### 4.1 Actors and components (new runtime module `UnifiedUniverseShip`, depending on GravPlugin and EnhancedInput)

| Piece | Role |
| --- | --- |
| `AUniShip` (APawn) | Root: the hull's collision (kinematic, blocks characters and props). Owns the universal state (§5) and is the Ship frame's provider. Possessed while piloted. |
| `UUniShipFlightComponent` | Bridge to `UniCore` flight: input → commands → integration (fixed substeps) → the ship's universal pose. |
| `AUniShipGravityZone` | The interior's gravity (§3.3); spawned and attached by the ship, tagged to exclude it. |
| `UUniPilotSeatComponent` | Interaction volume, sit and stand points, eye point, the HUD class and the input context for piloting. |
| `UUniInteractableComponent` + interaction trace | A general "look at it and press E" for seats, doors, consoles and later scanner stations. |
| `AUniCharacter` + `UUniCharacterMovementComponent` | A CMC subclass: no physics push on ship hulls, persistent framed base (§4.3), gravity from GravPlugin. The owner's gravity character can be re-parented to it. |
| `AUniPlayerController` | Gravity-relative view (as GravityController), interaction, possession switching, HUD selection, frame-aware view rotation. |
| `AUniShipHUD` | Canvas HUD first (no assets needed); UMG later. |

### 4.2 Interior, windows and verification

The first ship is assembled in C++ from engine basic shapes:
- a floor, walls and a ceiling;
- a cockpit with the seat;
- window openings with an invisible blocking pane and thin frames;
- a few loose physics crates.

The owner's mesh can replace it with no change to any system: the zone, the seat and the windows are components with transforms.

Ways to check it from inside:
- through the windows, the starfield, sun and planets turn and slide as the ship manoeuvres;
- sunlight through the windows sweeps across the floor as the ship rotates (the light rig's direction is frame-rotated);
- the crates stay put through any manoeuvre in the Ship frame;
- `uu.Ship.Spin <deg/s>` and `uu.Ship.Thrust <g>` force manoeuvres while you walk.

### 4.3 Framed movement (the moving-base cases)

Inside a Body frame the ship moves through the world. `UUniCharacterMovementComponent` then:
- keeps the ship as its movement base in **every** movement mode, not only walking;
- keeps the character's velocity relative to the ship;
- carries the base's transform change each tick, rotation included;
- never adds the base's velocity when leaving the floor.

The stock CMC clears the base and adds the base's velocity on becoming airborne. That is exactly the owner's fling, and this subclass overrides it.

The view rotation is stored relative to the base, so it turns with the ship with no lag; the gravity controller's easing applies only to real gravity changes.

### 4.4 The pilot chair

1. Interacting with the seat attaches the character to the seat's socket (seated pose, collision off) and possesses `AUniShip`.
2. The ship's camera sits at the seat's eye point with free look, plus an optional chase view.
3. The ship's input context and HUD replace the character's.
4. Leaving re-possesses the character at the stand point.

Possession keeps the character and the ship as separate pawns, which is the clean way to give each its own input and HUD.

### 4.5 Animation and gravity "down" (owner's note, 2026-11-19)

- **Inside a ship.** In the Ship frame the floor's down *is* the world's −Z, so animations made for −Z work unchanged aboard.
- **On a planet.** Down varies across the sphere, so anything new must be gravity-relative:
  - a `UUniGravityAnimInstance` base that exposes velocity, ground speed, vertical speed and lean in gravity space (UE 5.4+'s CMC gravity-space helpers);
  - foot IK and ground traces along the gravity direction;
  - root motion converted from gravity space.
- Not needed for the first ship; listed in phase S6.

## 5. Flight at speed scale 1: Newtonian, with a flight computer

### 5.1 The model (engine-agnostic, `UniCore/UniFlight.h`)

**State, all in double:**
- position as a universal coordinate (with fraction);
- velocity;
- attitude quaternion;
- body angular velocity.

**Integration:**
- in the dominant body's non-rotating frame (precision);
- semi-implicit with fixed substeps (120 Hz), independent of frame rate.

**Forces:**
- **Thrust:** six axis groups (main forward much stronger than retro and lateral), from thruster modules, each with a maximum force and an exhaust velocity (Isp), consuming propellant.
- **Torque:** about three axes, from the thrusters' arms or from reaction wheels.
- **Gravity:** §3.1.
- **Drag:** in atmospheres, ½ρv²C_dA with an exponential density.

**On rails:**
- **When it applies:** no thrust, out of atmosphere, and no contact.
- **What happens:** the ship follows the exact Kepler orbit about the dominant body. There is no integration drift over any number of orbits.
- **Time compression:** `uu.TimeScale` works with the ship aboard: the ship coasts on its conic while orbits run fast. As in KSP, compression is refused under thrust.

**Collisions (physics-adjacent):**
- the hull is swept along its step in the current frame;
- a hit applies an impulse (restitution, friction) in our integrator;
- landing gear is a few sweep probes with spring-damper contact.

The step happens before the frame is updated, so in the Ship frame the sweep is the ship's motion relative to the world it is leaving. That is exact.

### 5.2 Flight assist (the flight computer)

| Mode | Stick | Throttle / strafe |
| --- | --- | --- |
| **Coupled** (default) | angular *rates*; a damped rate controller saturating at the available torque | a target *velocity* in ship axes, relative to the local frame of rest (the dominant body's, its surface turning with it), reached at the available thrust |
| **Decoupled** | torque | thrust (pure Newton) |

Holds:
- attitude (prograde, retrograde, radial, normal, at the target);
- velocity match (target, or the frame of rest);
- hover (cancel gravity at the current altitude);
- descent rate (landing);
- orbit (coast).

The limits are honest: an assist never commands more than the modules provide, and the HUD shows when it is saturated.

### 5.3 Checks (offline, `T1.flight.*`)

- **Orbit energy:** 10⁴ orbits on rails, drift 0; integrated, under 10⁻⁶ an orbit.
- **Assists:** rate and velocity hold settle within a stated time with at most 5% overshoot over a grid of masses and thrusts; saturation never exceeds the modules' limits.
- **Hover:** holds within 1 m over 60 s.
- **Determinism:** the same inputs give the same trajectory, bit for bit.

## 6. Above scale 1: travel rules with a logic behind them

*Reworked after the owner's note (2026-11-19): allowed speeds should mean something. For example: leave the atmosphere for interplanetary cruise, the star system for interstellar, the galaxy for intergalactic; a black hole pulls you out of warp; above interplanetary, the flight computer flies. This section is a design brainstorm with a recommended version, not a build list.*

### 6.1 The fiction in one sentence

A warp drive needs flat space. Each drive is built to tolerate a certain gravitational depth. Where space is deeper than that, its field collapses and you drop to the tier below, or to real space.

**Gravitational depth:**
- **Definition:** an object's depth at distance r is `Φ/c² = GM / (r c²) = r_g / r`, where `r_g = GM/c²` is half its Schwarzschild radius. A dimensionless number from the masses the universe already has.
- **Each tier n has a tolerance `D_n`.** An object's **well** for that tier is where its depth exceeds the tolerance: `r_well = r_g / D_n`, with a floor at a safe multiple of the object's own size.
- **One rule then gives the owner's gates, and black holes fall out of it unasked.**

| Tier | Owner's name | Tolerance D_n | Gated by | What that means in the universe |
| --- | --- | --- | --- | --- |
| 0 | Real space (scale 1) | any | nothing | thrusters work anywhere |
| 1 | Interplanetary | 10⁻⁷ | atmospheres (matter, not depth: "the field collapses in air"); a minimum altitude over airless bodies; stars and compact objects by depth | **leave the atmosphere** to engage. The Sun's well is 0.1 AU (21 solar radii): no cruising through a star's corona. A 10 M☉ stellar black hole's is 1 AU. Planets' depth wells lie inside the planets, so only their air or altitude gates. |
| 2 | Interstellar | 10⁻¹⁰ | stars and compact objects | **leave the star system**: the Sun's well is ≈100 AU, and scales with mass (a red dwarf ≈10 AU, an O star ≈6,000 AU), matching the systems' activation radii (30–10⁴ AU, D-150). A 10 M☉ black hole: 1,000 AU. **A galaxy's central black hole (4×10⁶ M☉): ~2 kpc. The interstellar drive dies inside the galactic core's region.** |
| 3 | Intergalactic | 10⁻⁶ | galaxies (their dynamical mass, about 10× the stars) and their central black holes | **leave the galaxy**: a Milky Way-sized galaxy's well is ≈50 kpc, its halo; a dwarf's is its own size (the floor of 1.5 radii). |

**Black holes as interdictors:**
- At every tier a black hole's well is far larger than a star's of the same mass would be, because nothing hides it under a surface.
- Inside its tier-1 well (2 pc for a galactic nucleus, 1 AU for a stellar one), even supercruise collapses. You are **pulled out into real space**, where its gravity is real (the lens pass is already drawn there).
- Close approaches become a deliberate, slow, dangerous act. The accretion disc's radiation as heat is a later hazard and a fuel source (§6.6).

**Clusters and groups** gate no tier: their depth would forbid intergalactic travel across tens of Mpc. They can lower the tier-3 cap instead (thick intracluster gas: a "headwind"), which is optional flavour.

**Nebulae** (optional flavour): dense gas lowers the interstellar cap, like an atmosphere does tier 1. Cruising through a nebula is slower than around it, which makes nebulae terrain for routes.

### 6.2 Why speeds are what they are: never faster than you can stop

**The speed rule:** at any tier, the drive may only go as fast as it can still stop before the nearest thing that would stop it. That thing is:
- the edge of a well ahead;
- the target's drop point;
- anything on the corridor ahead.

**Stopping:** the drive decelerates by one e-fold of speed every `tau` seconds (default 3 s), so the stopping distance from speed v is `v·tau`. Hence:

```
v ≤ min( Vcap(tier),  L / tau )
L = distance to the nearest well edge ahead, the target's drop point,
    or an obstacle's closest approach on the corridor
```

What follows:
- **Climbing out of a well** (from a planet, out of a system), L grows with distance from the well's centre, so speed grows exponentially. Every e-fold of distance costs `tau` seconds, at any scale.
- **Arriving**, speed falls exponentially onto the drop point. **You cannot overshoot or crash at warp by construction.**
- **Between wells**, the drive runs at its cap. The cap is the drive's grade: the module's power, a fitting choice.
- **Passing traffic** (a star a parsec to the side) does not slow you. Only what is on the corridor counts.
- **Trip time** is a closed form, `tau·ln(out) + cruise / Vcap + tau·ln(in)`, so the route planner and the ETA readout are exact predictions, not guesses.

**Default caps** (owner to tune):

| Tier | Cap | Feel |
| --- | --- | --- |
| 1 | 3,000 c | planet to planet in 1–2 minutes |
| 2 | 1 kpc/s | to a neighbour star in about a minute; across a galaxy in a minute or two |
| 3 | 100 Mpc/s | to the next galaxy in about a minute; across a cluster in a couple of minutes |

The plugin's streaming, travel sensitivity and warp optics (D-154, D-187, D-190) were built and verified for these speeds and this log-distance rhythm. The scripted flythrough moves the same way.

### 6.3 Handovers, step-downs, drops and pull-outs

- **Engage:** possible only outside every well of the drive's tier, after its spool (5–15 s; the drive charges, the view stretches).
- **Step-down:** cruising at tier 2 into a lower-tier well that was not the destination (a star on the corridor, say) hands over to tier 1 at the same speed: no stop, just the slower regime. Leaving the well hands back up.
- **Drop:** at the destination, speed reaches the drop point and the ship returns to real space. It takes the local frame of rest's velocity (a planet's orbital motion), so it arrives parked beside the target, not drifting off at orbital speed.
- **Pull-out (interdiction):** an *uncommanded* entry into a well whose depth the current tier cannot hold is a forced drop to the tier that can, or to real space.
  - With the stop-in-time rule this only happens when the governor was overridden, or when something entered the corridor unseen: an uncharted black hole is the classic case.
  - A pull-out costs heat and a moment of control loss (a gameplay hook, later damage).
- **Ramps:** log speed changes at most ×4.5 a second (up and down); drops ease out over about 1 s; the warp optics follow the speed.

### 6.4 Who flies: the flight computer's share grows with the tier

| Tier | Pilot | Flight computer |
| --- | --- | --- |
| 0 | flies (stick, throttle, strafe) | flight assist, holds, landing and docking assists (§5.2) |
| 1 | steers and sets the throttle | enforces the stop-in-time rule; drops at the target; warns on wells ahead |
| 2, 3 | picks the destination, approves the plot, can abort or nudge the heading | plots the legs, spools, aligns, cruises, steps down and up, avoids, drops |

**Destinations above tier 1:**
- a charted object;
- a region on a map;
- a bearing and a distance into the unknown ("2 kpc that way"): the exploration case.

**Manual control at tiers 2 and 3:** the pilot keeps a limited heading nudge and an abort. The computer flies at those tiers to start (owner, D-285). A manual override (free cruise, with the computer only as a governor) may come later; the governor already enforces the rules either way.

### 6.5 What carries the fiction on screen and in the cockpit

- **HUD:**
  - the tier, and the current depth against the tolerance as a bar ("field margin");
  - wells ahead marked on the corridor;
  - an ETA from the closed form.
- **Pull-out warnings** when a well edge is near and the governor is overridden.
- **Black holes on the scanner:**
  - a well-radius ring for every tier, so you can see where you will be pulled out;
  - the lens pass and the disc already make them unmistakable up close.

### 6.6 Fuels and power

Owner (2026-11-19): what each resource does matters, not its name.
- Each drive tier spends its own resource channel, by a law the warp model gives in closed form (per distance at the cap, per e-fold on the ramps). So range and route fuel are exact predictions.
- The model is in §8.3.
- The omni ship refills everything with a button for now. Refilling in the world (stations, skimming gas giants and star coronas, harvesting at black holes, gate tolls) comes with real ship classes.

### 6.7 Brainstorm: alternatives and additions (not in the recommended version)

- **Potential sum instead of per-object wells.** Rejected for the gates: a galaxy's own potential (~10⁻⁶ at the Sun) is deeper than the Sun's at 1 AU, so a summed rule makes nesting impossible. Per-object wells of the tier's own scale keep the hierarchy readable.
- **Speed tied to depth** (slower near any mass, as in Elite's supercruise) instead of stop-in-time. It reads well, but its trip times are less predictable, so the ETA and route planning get worse.
- **Range-limited jumps** at tier 2 (Elite's hyperspace) instead of cruise. It suits fuel and range gameplay but skips the space between stars, which is this plugin's strength. Not adopted. The owner's jump gates (§8.6) cover ships without long-range drives, and they keep the seamless leg by borrowing the gate's drive.
- **Interdiction by things other than mass:** a "deep field" artefact, or an enemy inhibitor (weapons era). Same mechanism: a well with a tolerance.
- **Felt acceleration at drops:** render-only shake and a lean of the view; the interior zone damps physics.
- **Time:** the universe clock and warp. Orbits run on universe time, so at tier 1, crossing a system in a minute changes little. If the owner ever wants relativistic flavour (time passing at the destination), it is a single clock offset per trip. Not recommended.

### 6.8 Checks when built (`T1.warp.*`)

- Wells from depth match the table (each row, 1%).
- No engage inside a tier's well.
- Arrival times against the closed form (1%).
- Never inside a drop point or a well at a tier that cannot hold it, over randomised scenes.
- Pull-outs only when the governor is overridden or something appears unseen.
- Speed continuous across step-downs.
- Ramps within their limits.
- At a drop, the frame of rest's velocity.

## 7. Scanning, the chart and navigation

*Expanded in Docs/SHIPS_EXPLORATION.md (design piece 1, 2026-11-19): knowledge levels, bands (optical, X-ray, gravity, infrared / radio), distance estimates closing by parallax, ping / cone / survey scans built on the design spec's sensitivity shapes, the chart, names, the contact list. Where the two differ, that document wins.*

### 7.1 What can be known

The universe is generated only around the camera (the windows). So knowledge has to be *gathered*, which fits "we do not know what is out there":

- **Passive sensors** (always on):
  - Contacts are resident objects brighter than the scanner's limit: the scanner's grade is a magnitude, the same photometry as rendering, so a better scanner literally sees fainter things.
  - They come from `QueryResident` over the windows and the CPU-known parents (galaxies, groups, clusters).
- **Active scan:**
  - a ping sphere grows over a few seconds;
  - it charts everything inside it, through batched resident queries (one a frame; queued);
  - a scan wave is drawn, render-only.
- **System scan:** a star's planets are deterministic from its seed (`BuildSystem`), so a scanned star reveals its system instantly, even from afar once the star itself is charted. Detailed planet scans need proximity.
- **Resolve (design M4):** a CPU `ResolveObject(FUniObjectId)` built on the reference generator re-derives a charted object's coordinate anywhere, so chart entries stay valid without storing generation state.

### 7.2 The chart (discovery database)

- **Entries:** `FUniObjectId` → kind, exact coordinate, parents (galaxy, group, cluster), scan level, first and last seen, and a procedural name.
- **Saved** in a SaveGame.
- **The source of every navigation target.** A target never needs its object resident: travel brings it into the windows.

### 7.3 Targets

Targets are:
- anything charted;
- any contact;
- "regions": a point on a map, for heading into the unknown toward a dense patch of a galaxy's map.

The HUD brackets the target and shows distance (`FormatDistance`), ETA, the tier needed and fuel.

### 7.4 Route planning

- **Hierarchical legs:** from the start to the target through their common ancestor in the object hierarchy.
  - For example: planet → its star's well → galaxy → (cluster) → the other galaxy → star → planet.
  - Each leg is assigned the tier that governs it.
- **Each leg's time** comes from the warp law in closed form, its fuel from §8.3, and its feasibility from the ship's capabilities.
- **Gates and carriers** (§8.6) are edges in the same plan, for ships whose drives do not reach.
  - Charted gates come from the chart; a route that needs one the ship has not found says so.

### 7.5 Navigation displays

- **Cockpit HUD:**
  - attitude and velocity vector;
  - the frame of rest ("rel. Planet 3");
  - the tier and speed as a speed scale;
  - target brackets with distance and ETA;
  - fuel, power and heat.
- **System map:** an orrery from the system's orbits, with the ship, targets and the route.
- **Galaxy map:** the galaxy's own map texture (D-212's galaxy maps) top-down or tilted, with charted stars, scan bubbles and the route.
- **Universe map:** charted galaxies, groups and clusters over the web texture.
- **Scanner view:** the contact list, filters and classification.

The maps are drawn from data (textures and charted points), not by a second universal camera. A second camera would mean a second set of windows and re-streaming the ship's surroundings on return.

## 8. Ships as capability sets: the omni ship first

*Owner, 2026-11-19 00:42: the first ship is "basically a composite of all ship capabilities", with a refuel button to start. Real ships will have a subset, with real refuelling. It is "the prototype for the ship systems we plan to use to build different ships from". Example: only specialised, very large ships fly intergalactic, and smaller ships rely on something like jump gates.*

### 8.1 The rule: systems read capabilities, never ship types

Every system asks the ship what it can do, how well, and at what cost, and nothing else:
- flight, warp, the flight computer and autopilots;
- scanners, navigation and the HUD;
- gravity, power and heat.

A ship class is a frame plus a capability set. No system ever asks "is this a freighter?".

The **omni ship** has every capability at full grade with free refills. It is the prototype and the test bench at once: every later ship is the same code with fewer or weaker capabilities.

### 8.2 The capabilities

| Capability | Grants | Grades (its parameters) | Spends | A ship without it |
| --- | --- | --- | --- | --- |
| Hull | walkable interior, mass, hardpoints | size class, hardpoint layout | — | (every ship has one) |
| Artificial gravity | the interior's gravity zone (§3.3) | g, zone shape | power | a zero-g interior: the frame still pins it, and characters float and push off walls |
| Thrust (tier 0) | forces on six axes, torque | force per axis, exhaust velocity, torque | propellant | (every ship has some) |
| Flight computer | assists and autopilots (§5.2, §6.4) | the feature set: holds, tier-1 governor, tier-2/3 plotting, route follower, landing, docking | power | manual flight only; no drive above tier 1 can be used |
| Drive, tier 1 / 2 / 3 | warp at that tier (§6) | cap speed, spool time; optionally a tolerance factor (a better drive holds deeper wells) | its drive resource | uses gates (§8.6) or a carrier, or does not go there |
| Gate transit | passage through gates | the largest gate class accepted (size) | a toll (later) | — |
| Scanner | contacts, pings, system and detail scans (§7.1) | passive limiting magnitude, ping radius and time, detail range | power | sees only what the pilot sees |
| Power plant | power for everything | output | reactor fuel | — |
| Cooling | heat removal | capacity | power | heat builds up: drives and scans throttle |
| Tanks | resource capacity | capacity per resource | — | — |
| Docking bay | carries smaller ships | size class, slots | — | — |
| *Reserved:* shields, armour, cloak, weapons, cargo | (§8.7) | | | |

### 8.3 Resources: functionality, not names

The owner's direction: names do not matter, function does. A resource is a channel with:
- a capacity, from the tanks;
- an amount;
- a consumption law, from what uses it:
  - **per impulse:** thrust, Δp / exhaust velocity;
  - **per second:** power draw, artificial gravity, life support;
  - **per distance:** cruising at a drive's cap;
  - **per e-fold of distance:** the log-distance legs of the warp law. Its closed form gives a route's fuel exactly (§7.4).
- refill sources, later: stations, skimming a gas giant or a star's corona, harvesting at black holes, and gate tolls.

**Defaults (placeholder names):**

| Spender | Resource | Law |
| --- | --- | --- |
| thrusters | propellant | per impulse |
| systems | power | per second, from the power plant, which burns reactor fuel per second of output |
| tier 1 drive | power (a charge) | per second while cruising |
| tier 2 drive | drive resource A | per distance at the cap, per e-fold on its ramps |
| tier 3 drive | drive resource B | the same |

Which resource each spender draws is data, so a ship class can share or split them.

**The omni ship:**
- large tanks;
- a **Refuel** button on its console (and `uu.Ship.Refuel`) that fills every channel;
- consumption still runs, so gauges, range estimates and the route planner's fuel are exercised from the first build.

### 8.4 The omni ship as a test bench

A **capability panel** (a console in the ship, plus `uu.Ship.Cap <name> <0|1|grade>`) switches any capability on or off, or changes its grade, live. So the omni ship can behave as any future class:
- tier 3 off: test gates and route planning without the long-range drive;
- artificial gravity off: test a zero-g interior;
- a low scanner grade: test exploration with poor sensors;
- the flight computer stripped: test manual-only flight.

Each switch is data, not code: the same path a module fitting will drive later.

### 8.5 From the omni ship to real ships

Real ships are assembled, not written:
- **A frame** (hull, interior, hardpoints with size classes).
- **Modules** fitted to the hardpoints; each grants or grades capabilities (§8.2) and adds mass, power draw and heat.
- **Hardpoint size enforces the owner's example:** a tier-3 drive module exists only in a large size class, so only large frames can fly intergalactic.

Example classes, for illustration only:

| Class | Tiers | Notable |
| --- | --- | --- |
| Shuttle | 0–1 | zero-g interior, small tanks; gates for anything farther |
| Explorer | 0–2 | strong scanner, big tanks |
| Freighter | 0–1 | large tanks and cargo; gate-dependent |
| Deep-space carrier | 0–3 | docking bays: ferries small ships between galaxies |

### 8.6 Jump gates (for ships without the long-range tiers)

- **What a gate is:** a fixed object linked to another gate (a pair or a network) beyond a small ship's own reach.
- **Transit borrows the gate's drive for one leg:** the ship is carried between the gates along the same warp law.
  - By default the trip is continuous, so the universe streams past and the leg is seen and seamless.
  - An instant jump is the alternative: the teleport's coarse-to-fine rebuild, hidden behind a transit effect.
- **Where gates are:** a gameplay content layer. Placed by story or factions, or procedurally from seeds at qualifying places: bright stars near a galaxy's hub, say, on new seed channels, with no reroll of anything that exists.
- **Navigation:** gate legs are edges in the route planner (§7.4), costed by time and toll. The planner mixes the ship's own drives, gates and carriers.
- **Carriers** are the other way across: a large tier-3 ship carries docked small ships, which ride in its frame like anything else aboard (§2).

### 8.7 Weapons-ready now (nothing built until there is something to fight)

- the Weapon category and its hardpoints;
- fire groups on the flight computer;
- `IUniTargetable` on contacts;
- module and hull integrity fields with a damage event;
- weapons drawing power, heat and ammunition through the same resource channels.

**Verification:** fit validation, resource balance and the derived capabilities are offline checks (`T1.ship.loadout`).

## 9. The owner's hurdles

| Hurdle | Answer |
| --- | --- |
| Nested gravity fields | Priorities as GravPlugin has them (ship interior 1000 > planets > stars), one field source behind every zone, and apparent gravity (true − frame acceleration) where no damped zone applies. |
| The player's weight pushes the ship | The ship is kinematic (its own integrator), and its interior zone excludes it. No standing or push forces on hulls. |
| Fling from momentum | In space the ship *is* the world frame: the interior never moves. In a planet's frame, framed movement keeps velocity ship-relative and never adds the base's velocity. |
| An orbiting planet you stand on | Near a planet the planet *is* the world frame: it never moves under you, and the sun and sky move instead. The voxel planet is only ever moved while far away. |
| The ship's zone acting on the ship | `ExcludeTags` with the ship's tag; and the ship is kinematic, so GravPlugin's forces would not reach it anyway. |
| Animations assume −Z | Unchanged aboard (ship floors stay on −Z); gravity-relative animation base for planets later (§4.5). |

## 10. Other pitfalls to plan for

1. **Tick order.**
   - The order is: ship step, then characters, then the universal camera and placement from the ship's new pose, then the renderer with the same frame rotation.
   - A one-frame mismatch shows as the universe shivering against the interior, so the order is enforced: the flight component ticks pre-physics, and the subsystem builds the camera after the pawns.
2. **The sky light** is marched in universe axes. In a rotating frame its cube is re-marched in world axes when the rotation changes by more than a few degrees. `SkyLightComponent` only rotates about Z.
3. **Frame switches** touch every non-universe actor: a hitch scaled by actor count. They need hysteresis and must be rare, and Chaos velocities and CMC velocities are rotated in the same step.
4. **Backdrop occlusion indoors:** the sprites, glow and lens must stay behind the hull and interior (scene depth), and show only through windows. UE-5 covered sprites; the volume and lens composites need the same check with an interior.
5. **The lens pass with an interior in front** (D-279's foreground rule) should hold, but needs an engine check.
6. **The universe clock** runs at `uu.TimeScale`, while physics runs in real time. Time compression is only allowed on rails.
7. **Planets do not spin yet:** the Body frame is translation-only until spin exists. Adding spin later rotates the frame, and the renderer already accepts that rotation.
8. **Precision.** The interior stays within metres of the origin (ideal). The universe is placed relative to the frame in double, as now. In the Body frame the ship's position is double relative to the planet's centre: sub-millimetre to 10¹³ cm.
9. **Overlap cost:** universe-placed zones move every frame and re-test overlaps. That is fine for tens of zones.
10. **GravPlugin's sticky gravity** outside zones would leave an EVA player with the last ship gravity. Unified Universe's character component sets zero or apparent gravity there (§3.5).
11. **Free flight (`uu.Fly`)** stays as a debug mode, but is off while a ship is the frame. The ship replaces it as the way to travel.
12. **Single-player only** (§2).
13. **Temporal history across frame changes and re-centres:** the renderer's previous-frame view must move with the world, or temporal antialiasing and motion blur smear for a frame (§2.1). Today's re-centre has the same gap.

## 11. Changes to existing systems

### 11.1 Unified Universe subsystem

- **Frame API:** a frame provider (ship, body or inertial) gives the universal pose of the UE world each tick.
- **Camera:** camera = origin + R·view location.
- **Placement:** parallax placement rotates offsets and actor orientations by Rᵀ.
- **Frame change:** `OnFrameChanged(FTransform)` generalises `OnLocalRecentre`.
- **Light rig:** its direction is rotated.
- **Motion modes:** Local/Cruise stay for the inertial frame; a ship frame moves continuously, with no threshold.

### 11.2 Renderer

- `FUniFrameData` gains the frame rotation.
- Each pass that takes the view basis (project, sprite, volume, background, lens, splat) uses the universe-space basis R·(engine basis).
- The sprite pass's translated-world-to-clip matrix gains Rᵀ.
- **Check:** render with frame rotation R and view V equals render with no frame and view R·V, to float rounding (`T1.frame.render`).

### 11.3 New core files (offline-tested)

- `UniFrame.h`: frames, a double quaternion, transforms, frame changes.
- `UniFlight.h`: rigid body, thrusters, flight computer, Kepler rails.
- `UniWarp.h`: the warp law, wells, tiers, ramps.
- Later: `UniNav.h` (legs, ETA, chart) and `UniShipLoadout.h` (fit, bus).

### 11.4 GravPlugin

Approved by the owner (D-283), and only these two changes:
- export `AGravityController`;
- an additive-field hook (apparent gravity outside zones, §3.2).

Anything more needs asking first.

### 11.5 Plugin descriptor

- Unified Universe declares GravPlugin and EnhancedInput as plugin dependencies.
- The new module `UnifiedUniverseShip` (Runtime) holds every gameplay class, so the universe core never depends on gameplay.

## 12. Verification

**Offline** (UniVerify, every phase):
- `T1.frame.*`: exact transforms; frame changes preserve every universal position (sub-cm) and velocity; the rotated render equivalence.
- `T1.flight.*` (§5.3).
- `T1.warp.*` (§6.8).
- `T1.nav.*`: route legs; ETA against simulated flight within 5%.
- `T1.ship.loadout`: fit and bus balance.
- Stills from the reference renderer of a rotated frame.

**In the engine** (REVIEW_TONIGHT sections with UE ids):
- walk, jump and drop crates while `uu.Ship.Spin 90` and `uu.Ship.Thrust 3`;
- sit, fly and leave the seat;
- the windows show the turning sky and sunlight sweeping the floor;
- EVA beside a coasting ship;
- frame switches at a planet;
- landing;
- warp tiers with handovers and drops.

## 13. Phases (for when building starts)

The current effort is design (owner, 2026-11-19). The order below is the proposal for when implementation is asked for; the travel design (§6, §7, §8) can keep evolving until phase S3.

| Phase | Builds | Ends with |
| --- | --- | --- |
| **S0 Frames** | `UniFrame.h`; the subsystem's frame API; renderer frame rotation; rotated placement, light rig and sky light; plugin dependencies; the `UnifiedUniverseShip` module skeleton; GravPlugin's two approved changes | `T1.frame.*` and stills; engine check that the sky turns with a test frame |
| **S1 The omni ship** | `AUniShip` (kinematic, test interior with windows and crates); its capability set as data (§8.2), the capability panel and Refuel (§8.3, §8.4); interior gravity zone; character and CMC subclass; controller; interaction; pilot seat and possession; Newtonian flight with the flight computer; resource channels with consumption; Canvas HUD; `uu.SpawnShip`, `uu.Ship.*` | `T1.flight.*`, `T1.ship.resources`; engine walk-and-spin checks |
| **S2 Planets** | `GravityAt` with planet masses; body gravity zones; the Body frame and frame switching; framed movement; prop stowing; atmosphere drag; landing contacts; Kepler rails and time compression | frame-switch checks; landing in the engine |
| **S3 Warp** | `UniWarp.h`: depth wells by tier, the stop-in-time governor, engage / step-down / drop / pull-out, ramps; the flight computer's tier-1 assist and tier-2/3 plotting; per-tier resource laws | `T1.warp.*`; a flight from a planet to the next star in the engine |
| **S4 Navigation** | scanners (passive, ping, system) graded by capability; the chart and SaveGame; CPU Resolve; targets; route legs and the route follower; HUD target brackets and well rings; system and galaxy maps | `T1.nav.*`; a charted trip across a galaxy |
| **S5 Ship classes** | frames and module assets; hardpoints with size classes; capability sets assembled from modules (the omni ship becomes one fitting); power, heat and mass; gates and carriers (§8.6); weapon hooks | `T1.ship.loadout`; a shuttle that needs a gate |
| **S6 Polish** | UMG HUD and maps; universe map; gravity-relative animation base (§4.5); docking and landing assists; felt-acceleration effects; refuelling in the world; the manual override at tiers 2–3 | engine passes |

## 14. Owner decisions (2026-11-19 00:42)

1. **Frames:** the world follows what the player stands in: the ship in space, the planet near it; the universe moves around them (D-282).
2. **GravPlugin:** yes to exporting `AGravityController` and the additive-field hook, and only those (D-283).
3. **Travel gates:** gravitational depth with one tolerance per tier, and the stop-in-time speed rule (D-284).
4. **Who flies above tier 1:** the computer, to start; a manual override later (D-285).
5. **Fuels:** function over names. The first ship is an omni ship with every capability and a refuel button; real ships are subsets with real refuelling. Small ships may rely on jump gates; only large, specialised ships fly intergalactic (D-286; §8).

### Owner decisions (2026-11-19 11:53)

- **Single-player for now.** Multiplayer is not a requirement; §2.2 says what it would take, and its four design rules are adopted (D-289).
- **The planet is the owner's voxel planet** (`APlanetActor`, which already ties into GravPlugin through `APlanetGravityZone`). Design piece 8.
- **Planet spin is necessary** (piece 8).
- **Atmosphere data comes from the planet actors** (piece 8).
- **The ship persists as a universe object** while the player is away from it, like a promoted body (piece 9).
- **Save and load need design** (piece 9).
