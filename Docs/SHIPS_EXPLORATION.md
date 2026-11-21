# Scanning and exploration: design

*Design piece 1 of the ship travel design (SHIPS.md §7, expanded). 2026-11-19. Nothing here is built. §9 has the owner's answers (D-288). Follow-ups: 1b the bands, 1c a procedural naming scheme.*

**Why this piece comes first:** above tier 1 the flight computer flies to a destination the pilot chooses (D-285). So what a pilot can find, know and target decides what travel *is*. Navigation (piece 2), the HUD and maps (piece 3) and the rest build on it.

## 1. The premise: the universe is not charted, it is observed

Unified Universe generates only what is relevant around the camera: the level windows, sized by brightness (design spec, scale levels). So nothing about a place is known until something has looked at it, which is the owner's "we truly do not know what is out there".

Gameplay follows the same rule as the engine:

- **What the ship can know is what its sensors make resident and see.**
  - **The resident set:** a scanner's sensitivity *is* the engine's sensitivity (Δm, a limiting-magnitude offset, design spec "Sensitivity").
  - **The scans:** the original design's two scan shapes, the expanding ping shell and the cone, are scans.
  - **Determinism:** "a scan never changes which objects exist, only which are resident and visible" (design spec), so scanning is deterministic and cannot alter the universe.
- **What the ship has learned is kept in the chart.** It is keyed by the exact identities the engine already has (`FUniObjectId`), and is the original design's `UUniDeltaStore.MarkDiscovered` / `Rename`, never built until now.

## 2. Knowledge levels

Each object the ship has noticed has a knowledge level. The level decides what the HUD shows, what can be targeted, and how precisely.

| Level | Name | Known | How it is reached |
| --- | --- | --- | --- |
| 0 | Unknown | nothing | — |
| 1 | Detected | direction, apparent brightness in a band, a rough kind ("point source", "extended") | it is above the scanner's passive limit in some band |
| 2 | Identified | kind and class (a G2 star, a barred spiral, an X-ray binary); distance as an *estimate* with an interval | a focused look: the pick plus a short dwell, or a ping that reaches it |
| 3 | Surveyed | exact position and properties; its children listed (a star's planets and companion; a galaxy's nucleus and its notable regions) | a survey scan within the band's survey range, or parallax closing the interval (§4) |
| 4 | Visited | everything at level 3, plus "you were there" | inside its tier-1 well (a system), or its tier-3 well (a galaxy), or within a few radii (a planet) |

The chart stores each object's highest level, first and last seen, and the band it was found in.

## 3. Bands: different instruments see different things

The omni ship carries all four bands; real ships carry a subset (D-286). Each band ranks objects by its own brightness, against its own limit. Each draws on things the universe already computes:

| Band | Sees | Brightness it ranks by | Engine source |
| --- | --- | --- | --- |
| **Optical** | stars, planets, galaxies, groups, clusters (what the eye sees, fainter) | V magnitude (the rendering photometry) | resident objects (`QueryResident`, CPU-known parents) |
| **X-ray** | accreting black holes: galaxy nuclei (AGN, quasars) and stellar X-ray binaries in outburst | accretion luminosity (D-272, D-281: `LuminosityErgS`, the Eddington ratio) | galaxies' black hole records; resident stars' companions (`StellarCompanionOf` is a cheap function of a star's seed) |
| **Gravity** | masses as *wells*: stars, black holes (including quiet ones, invisible in every other band), galaxies | depth `r_g / r` (D-284's travel quantity) | masses from the records (stars: from luminosity; galaxies: dynamical) |
| **Infrared / radio** | dust, dark clouds, nebulae, star-forming regions: the *fields*, not objects | the field's column density or emission along a direction | the galaxy field core on the CPU (one field source), sampled along rays |

Why bands are worth having:
- **Each band is a reason to scan in a different way,** and a capability that ship classes can differ in. A cheap ship sees optical only; an explorer adds X-ray and gravity.
- **The gravity band is the navigator's band:** it draws the wells that gate the drives (D-284) as rings, so the pilot sees where a black hole will pull them out before it happens.
- **Quiet black holes** (an Eddington ratio of 10⁻⁸, dark in optical and X-ray) are found only by gravity. That makes the gravity band the black-hole hunter's instrument, and black holes are a fuel source in the resource design (SHIPS §6.6, §8.3).

### 3.1 Field features get identities

- Nebulae, dark clouds and star clusters are *fields* in this plugin, by design (D-234, D-238, D-239: no entities, flat cost), so they have no `FUniObjectId`.
- To be charted and targeted, a field feature found by the infrared / radio band gets a **feature id**: a hash of the parent galaxy's id, the feature kind and its position quantised at the field's own cell scale in the galaxy's frame.
- That id is deterministic, so the same nebula found again from elsewhere is the same chart entry. Its position is the field's local maximum near the scan's peak.

## 4. Distance is an estimate until it is not

A single look gives a direction and a brightness, not a distance.

- **Level 1 (detected):** a bearing only.
- **Level 2 (identified):** the class gives an expected luminosity, so the distance is an *interval* (for a star, about ±25%; for a galaxy, about ±40%).
- **The interval closes by parallax as you move.** Two looks from points a baseline b apart, at distance d, pin the distance to a relative precision of about d · σθ / b, with σθ the scanner's angular precision. The ship's own travel is the baseline, so flying toward or past something resolves it naturally. A survey scan also closes it outright within its range.
- **Level 3 is reached** when the interval is under 1% (or by a survey).

What this means for travel:
- The stop-in-time governor (D-284) uses the **near end** of the interval as the target's distance. You can always set course for something only detected or identified. You arrive slower than you could have, never too fast.
- As the interval tightens on the way in, the ETA sharpens on the HUD.
- "Heading into the unknown" becomes a real act: you fly toward a faint smudge, and it becomes a place as you approach.

The engine knows the true position all along (the object is resident). Only what the HUD, the chart and the governor use is the estimate. So nothing about generation or streaming changes.

## 5. The scans

### 5.1 Passive (always on, free)

- **What it is:** contacts above the scanner's passive limit in each band the ship carries.
  - In optical the limit is the eye's (the rendering limit) plus the scanner's grade.
  - A grade above the eye's means the scanner lists stars that are resident but too faint to draw.
  - Raising residency itself is what pings and cones do.
- **Source:**
  - one resident query a frame at most (D-193), rotating through bands and distance shells;
  - CPU-known parents (galaxies, groups, clusters) every frame, at no cost.
- **Shown:** HUD brackets for the most relevant few (the target, the brightest, anything new); the rest in the contact list (§7).

### 5.2 Ping (an expanding shell)

- **What happens:** an active pulse. A shell grows outward from the ship **logarithmically**: radius r(t) = r_min · (r_max / r_min)^(t / T), so it crosses each decade of distance in equal time.
  - Inside the shell, sensitivity is raised by the ping's boost (default +2.5 mag, the grade's).
  - Fainter objects become resident there, and everything the shell crosses is identified (level 2) and enters the chart.
- **This is the original design's ping:** generation streams ahead of the shell, so its latency is hidden. The log growth reveals near things first and far things later, matching how windows fill.
- **Range follows the scale you are at**, like the drives:
  - in a system: r_max is about its tier-2 well (≈100 AU);
  - in a galaxy: about 1 kpc;
  - between galaxies: about 10 Mpc.
  - T is about 3 s in every case.
- **Cost:**
  - power and heat (§8);
  - a cooldown;
  - a transient residency spike: k³ in the shell, bounded by the window cap N_max (D-139's pool guard applies);
  - **in the weapons era, a signature:** a ping announces you. It is a hook now and does nothing yet.
- **At warp:** none at tiers 2–3 (the drive's field), allowed at tier 1 with half range.

### 5.3 Cone (a telescope, integration time)

- **What happens:** aim, hold steady, and the depth grows with time: Δm(t) = 1.25 · log10(1 + t / 1 s), up to the grade's maximum (omni: +5).
  - That is the photon-counting law: signal-to-noise grows as √t.
  - 30 s gives +1.9 mag; 5 minutes gives +3.1 mag.
- **Shape:** the original design's cone. Windows grow only in cells that meet the cone, so the cost scales with the cone's share of the sky (default half-angle 2°).
- **What it is for:** the explorer's instrument. Look deep in one direction to find a destination beyond what pings reach: a faint dwarf galaxy, a far cluster, a quasar.
- **Holding steady:** the ship's turn rate must stay under a small limit. The flight computer has an attitude hold for this (SHIPS §5.2). Moving on resets the integration, and no cone scans at warp.

### 5.4 Survey (detail on one object)

- **What happens:** a focused scan of one identified object, within the band's survey range for that kind. It takes a few seconds and needs line of sight.
- **What it gives:** level 3 (exact position and properties) and the object's children.
  - **A star:** its planets and its stellar black hole companion. Both are pure functions of its seed (`BuildSystem`, `StellarCompanionOf`): instant, with no generation and no residency. So a surveyed star's system is known from afar.
  - **A galaxy:** its nucleus (mass, accretion: D-272), its notable regions (the bar, arm segments, nebula complexes as field features), its satellite dwarfs if resident.
  - **A planet:** type, radius, orbit; later its surface and resources.
- **Survey range by kind** (grade-scaled defaults):

| Kind | Range |
| --- | --- |
| Planet | 1 AU |
| Star | 10 pc |
| Field feature | 100 pc |
| Galaxy | 10 Mpc |
| Cluster | 100 Mpc |

### 5.5 Resolve (the chart's backstop)

- **When it is needed:** chart entries must stay valid when their objects are not resident. Positions are stored at survey time (exact `FUniCoord`).
- **What it re-derives:** the design's CPU `ResolveObject` (B-015) re-derives a charted object from its id anywhere, when needed:
  - after a save is loaded;
  - to confirm a target before a long plot.
- **Lesson #1's risk:** CPU and GPU placement could differ near a threshold (B-015 B). It is handled by checking the resolved object against the stored one and keeping the stored coordinate if they differ: the chart never moves a target.

## 6. The chart

- **Entry:**
  - the `FUniObjectId` (or a feature id);
  - kind and class;
  - the knowledge level;
  - a position: an exact coordinate, or a bearing and distance interval below level 3;
  - parents: the galaxy, group or cluster, by id;
  - the band it was found in;
  - first seen, last seen and visited times;
  - a name (§6.1);
  - pilot notes.
- **Storage:** a SaveGame with the original design's delta store (`MarkDiscovered`, `Rename`). Entries are small (about 100 bytes), so 10⁵ entries are about 10 MB.
- **Hierarchy:** entries nest by their parents. The chart is browsable as universe → cluster or group → galaxy → region → star → planet, the same tree the route planner walks (piece 2).
- **Summaries:** a surveyed galaxy shows how many of its stars, systems and features you have charted. Counts come from queries' `TotalMatches`, not by charting everything.

### 6.1 Names

- **Catalogue designations by default**, deterministic from the id: a short code by kind and level, readable and unique. For example "G 4F2A-91" for a galaxy and "G 4F2A-91 / S 19C3" for a star in it.
- **Proper names** generated from the seed for notable objects only: black holes, quasars, rich clusters, the brightest stars of a galaxy.
- **Renaming:** the pilot can rename anything, through the delta store's `Rename`.

## 7. Targets and the contact list

- **Anything with a chart entry or a live contact can be targeted.** The target's knowledge level sets how the computer can plot to it:
  - **Detected:** a bearing. The computer plots "toward", and the governor uses a cautious default distance until identification gives an interval.
  - **Identified:** a distance interval. The plot uses its near end and refines on the way.
  - **Surveyed and visited:** an exact plot.
- **Regions and bearings:**
  - a point on a map (a dense arm segment, a galaxy's core);
  - "this bearing, this far": the exploration destinations of D-285.
- **Contact list:**
  - grouped by reach: this system (tier 1), this galaxy (tier 2), beyond (tier 3), each row with its ETA and fuel from the warp law's closed form;
  - filters by band, kind and knowledge level;
  - sorting by distance, brightness or ETA.
  - This is the owner's "traversable target enumeration".

## 8. Costs and capabilities

| Scanner capability (D-286) | Grades |
| --- | --- |
| Bands carried | optical, X-ray, gravity, infrared / radio |
| Passive limit | Δm over the eye per band (omni +1) |
| Ping | boost Δm (omni +2.5), range factor, cooldown (omni 10 s) |
| Cone | maximum depth (omni +5), half-angle, steadiness tolerance |
| Survey | range factor per kind (omni 1×, the table in §5.4) |

| Scan | Spends |
| --- | --- |
| Passive | power |
| Ping | a burst of power and heat |
| Cone | power while held |
| Survey | power while held |

The omni ship carries every band at full grade, and its capability panel can turn each off to test a lesser ship.

## 9. Owner decisions (2026-11-19 11:53; D-288)

1. **Starting knowledge:** an empty chart except the start system, surveyed. A sandbox switch that charts everything remains a test option on the omni ship's panel.
2. **Distance as an estimate** closing by parallax (§4): yes.
3. **Bands:** "needs more design/planning". The four in §3 stand as the working set; follow-up piece 1b settles them.
4. **Names:** a procedural naming scheme for objects is needed (follow-up piece 1c), and renaming. §6.1's catalogue designations and proper names are the starting point.
5. **Ping as a signature** for the weapons era: yes (a hook; nothing yet).

## 10. What this needs from the engine (for when building starts)

- **Sensitivity shapes:** the ping shell and the cone, from the design spec, not yet built. Both are window changes inside the streamer: per-cell sensitivity, so a cell's level window grows only where the shape is. The guard and fade rules hold as they are.
- **Queries:**
  - an apparent-magnitude filter in the query, alongside today's absolute one;
  - a shell (min and max radius) form;
  - a cone form.
  - Today's form is a sphere with an absolute-magnitude cap, one a frame, 4,096 results.
- **CPU:**
  - companions and systems from seeds (exist);
  - galaxy black holes (exist);
  - field sampling along rays for the infrared / radio band (the field cores compile as C++ already);
  - Resolve (B-015).
- **Checks (offline):**
  - knowledge levels follow the rules;
  - parallax closes the interval at the rate the formula says;
  - the ping's shell growth against streaming;
  - field feature ids are stable from different viewpoints;
  - the chart round-trips through a save;
  - a scan never changes generation (determinism, as T3.sensitivity).
