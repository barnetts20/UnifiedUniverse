# Compact and exotic objects: design proposal

Status: the third item of B-031's roadmap after nebulae (D-234 on) and star clusters (D-238, D-239). Proposed 2026-11-16; the owner answered §6 on 2026-11-17 (§7). Phase A (supermassive black holes and galaxies' nuclei) is built: D-272. Phase D's lensing and accretion disc are built ahead of B and C at the owner's word (2026-11-17 08:11): D-273 (§8). Each hole's own look and stellar black holes in binaries: D-280, D-281 (§9).

## 1. What is out there, and how much of it

Real numbers per Milky Way–like galaxy, or per volume of the universe. Our scenes are present-day, with no look-back time.

| Class | How many | Optical brightness | What makes it visible |
| --- | --- | --- | --- |
| Supermassive black hole (SMBH) | One per galaxy with a bulge; mass ~0.0014 × the bulge's (Häring & Rix 2004), scatter 0.3 dex; the Milky Way's is 4.3 × 10⁶ M☉ | A quiet one: nothing (Sgr A*) | Its accretion (§1.1); lensing up close |
| Low-luminosity AGN (Seyfert, LINER) | ~30–40% of nearby galaxies host a low-level nucleus; bright Seyferts ~1–5% of spirals | Nucleus 0.01–1 × the host | A bright point at the galaxy centre, bluish (Seyfert) or warm (LINER) |
| Quasar / bright AGN | Bright ones (M_B < −23) ~10⁻⁷–10⁻⁶ per Mpc³ today (about 1 per 100–200 Mpc on a side); ~100× more at cosmic noon (z ~ 2) | Nucleus 10–1000 × the host: outshines it | A star-like point visible across the web; the host as faint fuzz |
| Relativistic jets | ~10% of AGN are radio-loud; optical jets are rare (M87's is ~1.5 kpc); radio lobes reach up to Mpc | Jets: knots at a few % of the host; lobes: radio only | Thin beams along the spin axis, knotty |
| White dwarfs | ~10¹⁰ (~10% of the stars) | M_V +10 to +16 | Faint blue-white points, nearby only |
| Neutron stars | ~10⁹; pulsars ~10⁵ active, magnetars ~30–100 | Essentially none in the optical | Pulses (radio/X-ray), bursts (magnetars): art |
| Stellar black holes | ~10⁸ | None when isolated | Lensing up close; accretion in binaries |
| X-ray binaries / microquasars | A few hundred bright ones | The companion star plus a disc (some to M_V ~ −5 when in outburst) | A hot disc and, in microquasars, small jets (SS 433) |

### 1.1 How bright an AGN is

- Eddington: L_Edd = 1.26 × 10³⁸ (M / M☉) erg/s.
- The Eddington ratio λ is broad: active galaxies follow p(λ) ∝ λ^−0.65 over 10⁻⁴–1 (Aird et al. 2012), and most galaxies sit far below that.
- Optical light is ~10% of bolometric.
- Example: 10⁸ M☉ at λ = 0.1 gives ~10⁴⁴ erg/s optical, the luminosity of a whole Milky Way. At λ = 1 with 10⁹ M☉ it is 100× that: a quasar.

## 2. How it maps onto what we have

The engine already has the pieces: archetypes and emitters with luminosity slicing, per-object seed channels, the sprite pass with glare, render-only field terms in the volume, system activation near the camera (planets, D-142), the universe clock (D-145), and promotion (B-003).

### 2.1 SMBH and low-luminosity nucleus: a property of every galaxy, no new records

- **New seed channels:** `Bh.Mass` and `Bh.Eddington` (D-rule: add channels, never reuse).
  - The mass follows the bulge (the galaxy's bulge parameters already exist) with 0.3 dex scatter.
  - The Eddington ratio comes from a broad log distribution truncated at the low end.
- **Nucleus cap:** its luminosity is capped at 0.5 × the host. The galaxy's level was assigned from its own magnitude, and an uncapped nucleus could push the total past what its level's window covers, so it would pop in late.
- **Rendering:**
  - The galaxy sprite gains a point core. It uses the existing point-source glare (stars have it; galaxies don't yet).
  - In the volume, an unresolved point can't be marched. The core stays a sprite drawn on top at every distance; the glow hand-off (D-129) carries only the host's light.
- **Gameplay:** the SMBH is at the galaxy's centre, so a lookup resolves it from the galaxy record. Its mass and Eddington ratio are read from its channels.
- **Reroll impact:** none. These are new channels, and render only.

### 2.2 Bright AGN and quasars: an "active galaxy" archetype emitted by the web

- **Why a separate archetype:** a quasar's nucleus outshines its host 10–1000×, so it has to be sliced by its total luminosity.
- **Its own luminosity function:** the quasar LF, a double power law in total magnitude. Present-day density scaled by an art factor (§6, question 2).
- **Placement:** the web field (one field source), weighted toward nodes and filaments as massive hosts are.
- **The host:**
  - It is a galaxy: the same field, archetype parameters and star emitter. Up close it is a galaxy you can fly into, with a blinding core.
  - Its stars come from the normal star emitter keyed to this archetype. Normal galaxies' seeds are unaffected: a new emitter index means a new stream.
- **Jets (optional):** a render-only emission term in the host's field.
  - Two cones along the disc's spin axis (with a small misalignment rolled by name), lengths of a few to tens of kpc, knotty (galaxy_shape's knot channel along the axis: the D-267 lessons, mean 1 at every mip).
  - Blue-white, Doppler-boosted on the approaching side, using the D-268 shift function at the jet's bulk speed.
  - Render only, so they never place stars.
- **Reroll impact:** none for existing content. This is a new archetype and a new emitter.

### 2.3 Stellar remnants: archetypes emitted by each galaxy beside its stars

- **White dwarfs:** the star emitter's luminosity function gains a white-dwarf branch.
  - About 10% of the count, M_V +10 to +16, blue-white to red as they cool.
  - At m_ref they land only on the finest levels, so near the camera only, and the cost is small.
  - Rerolls only the finest star levels' content. A separate emitter would avoid even that: preferred.
- **Neutron stars and pulsars:** their own archetype.
  - Field shape: an older, thicker population: the thick disc and halo terms.
  - Optically they're invisible. Their relevance radius is a *beacon reach* (an influence rule, §6 question 1), so they are generated where gameplay should see them.
  - Pulsing is render-only: period (ms to s) and duty cycle from the seed, phase from the universe clock, modulating the sprite's intensity in the project pass. Fades are render-only, so this is allowed.
  - Magnetars are a rare variant with occasional bursts, a deterministic event schedule from seed and clock.
- **Stellar black holes:** their own archetype; isolated ones have no light.
  - Relevance is the lensing reach (≈ the distance at which their Einstein ring exceeds a pixel), so they generate only near the camera, like star systems.
  - The visual is lensing (§2.4).
- **X-ray binaries / microquasars:** an archetype with a companion star's photometry plus a hot disc (blue) and, for microquasars, small jets.
  - These are "systems" in the D-142 sense: activated near the camera, generated on the CPU from the record (disc size, inclination, companion type).

### 2.4 Up close: lensing and accretion discs (with promotion, B-003, and the gravity ship)

- **Lensing:** a screen-space pass in UniRenderHooks.cpp that deflects the scene behind each nearby black hole.
  - Thin lens on a background at infinity: deflection α = 2 R_s / b.
  - The shadow (~2.6 R_s), the photon ring, and an Einstein ring of θ_E = √(2 R_s / D).
  - Surface brightness is conserved, so lensing moves light and never adds any; a check can hold that.
  - Sgr A*'s ring from 8 kpc is ~2 arcsec. Lensing is a fly-close effect: a few hundred AU from a stellar BH, a few light-days from an SMBH.
- **Accretion disc:** a thin emitting disc around the promoted black hole.
  - Temperature profile T ∝ r^−3/4, coloured through the existing blackbody palette.
  - Doppler beaming using D-268's shift function, with the lens applied to the disc too (the classic image: the far side of the disc lifted over the top).
  - This is a promoted actor's material, not the backdrop: it belongs with B-003's promotion work and the gravity ship pass.

## 3. Proposed order

| Phase | Content | New records? | Rerolls | Size |
| --- | --- | --- | --- | --- |
| A | SMBH mass + low-luminosity nucleus on every galaxy; point core with glare in the galaxy sprite | No | None | ~1 session |
| B | Active-galaxy archetype (Seyfert to quasar LF) from the web; jets as a render-only term | Yes (new archetype) | None | ~2 sessions |
| C | Remnants: white dwarfs, pulsars (pulse from the clock), magnetars, stellar BHs, X-ray binaries as systems | Yes | None (separate emitters) | ~2–3 sessions |
| D | Lensing pass + accretion disc on promoted black holes | No (promotion) | None | ~2 sessions; with B-003 / gravity ship |

## 4. Checks (UniVerify) per phase

- **A:**
  - The mass relation's median and scatter over 10⁴ galaxies.
  - The Eddington distribution's quantiles.
  - The nucleus never above 0.5 × its host.
  - The galaxy's level unchanged (relevance from the host alone).
  - Core plus host light equal to the record's photometry.
- **B:**
  - The active galaxy LF reproduced by the slicing (T2.gen.slicing's method).
  - Space density at the configured value within Poisson error.
  - Hosts on structure (T2.gen.fidelity's method).
  - Normal galaxies' content bit for bit unchanged.
  - Jets render-only (no placement change) and mean-preserving over footprints (the D-267/D-269 method).
- **C:**
  - Counts per galaxy against the configured fractions.
  - Pulses periodic, deterministic across revisits and frame rates, and render-only.
  - Systems activate and deactivate (the D-142 checks).
- **D:**
  - Deflection against the analytic point lens.
  - Surface brightness conserved through the lens.
  - The shadow radius.
  - The disc's Doppler ratio against D-268's function.

## 5. Costs

- **A:** a few ALU per galaxy sprite.
- **B:** at quasar densities, a handful of extra galaxies in a 200 Mpc tile. Jets cost one texture read a step inside the jet's bounds only.
- **C:** white dwarfs on the finest levels only. Pulsars and black holes are sparse (generated by reach, not by count).
- **D:** one full-screen pass while a black hole is promoted, otherwise nothing.

## 6. Decisions for the owner

1. **Visibility: physical or beacons?**
   - Physically, neutron stars and isolated black holes are invisible in the optical, and pulsars are radio sources.
   - Proposal: physical counts, with an art "beacon" reach and brightness per archetype (render-only), so pulsars and magnetars read as navigational lights; uu.* knobs, default on.
2. **Quasar abundance.**
   - Present-day is about one bright quasar per 100–200 Mpc on a side: rare, a destination.
   - Cosmic-noon densities (~100×) would put several in every view of the web.
   - Proposal: present-day × an art factor (uu knob), default ~10.
3. **Jets in the optical?** Real optical jets are rare and faint; radio lobes (up to Mpc) are invisible to the eye. Proposal: optical jets on radio-loud AGN at physical brightness, and radio lobes as an optional faint art layer (off by default).
4. **Order:** A → B → C → D as above, or lensing (D) earlier if the gravity ship pass is coming soon.

## 7. The owner's answers (2026-11-17) and what follows

1. **Visibility:** "Probably depends on their accretion discs, we would probably want some sort of gravitational lensing." A black hole shows by its accretion (the nucleus, phase A; the disc, phase D) and by lensing (phase D). No art beacons for black holes; pulsars and magnetars stay a question for phase C.
2. **Quasar abundance:** "Keep them relatively rare, we may need to ramp their frequency up to find instances to test though." Present-day density by default, with a knob (phase B's `uu.Bh.QuasarDensity` x). Phase A's equivalent: `uu.Bh.ActiveFraction` (the accreting share, 0.4) and `uu.Bh.NucleusGain` (art x, 1) raise the nuclei for testing.
3. **Optical jets:** "yes." Phase B: render-only jets on radio-loud active galaxies.
4. **Order:** "probably do supermassives first since every spiral galaxy will have one, and placement is always in the center of the bulge... the implementation should be robust enough to use for stellar or supermassive." Phase A first (built, D-272). The black hole core (UniCompact.ush: a black hole of any mass from its mass and Eddington ratio: luminosity, V light, Schwarzschild radius, colour) is separate from the galaxy relation that sets a supermassive one's mass, so stellar black holes (phase C) and their lensing (phase D) use the same code with their own mass source.

### Phase A as built (D-272)

- **Mass:** 0.0014 x the bulge (Haring & Rix), 0.3 dex scatter (held within 3 sigma). The bulge from the host's V light x a bulge share (discs log-uniform 0.08..0.45, early types 0.6..1) x a mass-to-light ratio (2 discs, 4 early types). A Milky Way-like disc: median ~1e7 M_sun (the real 4.3e6 sits below the relation). Seed channels BH.MASS (draws 0, 1: the scatter; 2: the bulge share) and BH.EDDINGTON (draw 0).
- **Accretion:** 40% accreting (p(log lambda) ~ lambda^-0.65, 1e-4..1, Aird et al. 2012), the rest quiet (1e-9..1e-5 log-uniform). V light 10% of bolometric.
- **The nucleus:** a second sprite of the galaxy's record from the project pass (a point with a point source's glare at the galaxy's centre, never handed to the volume, dimmed by the dust in front through the slices); at most 0.5 x the host (the galaxy's level comes from its own light); warm (LINER-like) at low accretion to blue-white (Seyfert) at high. The galaxy's own sprite and generation are untouched.
- **Gameplay:** UUniSubsystem::GetGalaxyBlackHole (mass, Eddington ratio, luminosity, Schwarzschild radius, nucleus magnitude, position), from the same core function.
- **Not yet:** the anchors' real black holes (the Milky Way's 4.3e6 M_sun is rolled like any galaxy's); a nucleus's light inside the volume's own glow at the hand-off (it is a sprite on top); quasars (phase B).

## 8. Phase D as built (D-273): lensing and the accretion disc

Owner, 2026-11-17 08:11: "Gravitational sensing ans accretion discs, there are quite a few shader references online for black holes as well". Built ahead of phases B and C (the gravity ship and promotion were not needed: a screen pass suffices).

- **Lensing:** each pixel's ray traced back through the Schwarzschild metric (the core's Uni_LensTrace, any mass, units of R_s): RK4 on starless's flat-coordinate acceleration inside 200 R_s, closed form beyond; the scene sampled where the ray came from (surface brightness conserved). Within 3.3e-5 rad of the exact orbit; the shadow's edge at 3 sqrt(3)/2 R_s.
- **The disc:** thin and opaque from 3 R_s to where it cools to 3000 K; the thin disc's temperature profile at its physical peak (floor 4000 K); Keplerian gas, every crossing shifted by Luminet's g (gravity, Doppler, beaming), seen in V. Axis: the host's disc normal tilted up to 60 degrees (BH.SPIN).
- **One light from far to near:** the disc's light by direction (lensing included) is tabulated offline (`univerify lenstable`): the nucleus sprite shows that light towards the camera, and the disc's scale makes its average the nucleus's. While the lens pass runs it draws the nucleus point itself (a sprite would be lensed as if behind the hole) and hands the light to the disc as it resolves (2 -> 6 px). Offline: the light shown stays within 0.33% of the sprite's from 2e7 R_s to the resolved disc.
- **The pass:** UniLens.usf at the end of the backdrop (after stars and volume, before bloom); one black hole a frame (the camera's galaxies, the most visible); scene geometry in front is not lensed. uu.Bh.Lens, uu.Bh.Disc, uu.Bh.Lens*/Disc* thresholds.
- **Not yet (B-044):** a cube map for rays that leave the view (mirrored now), spin (Kerr), the disc's turbulence and rotation, several holes at once, the pass before TSR, stellar black holes (phase C) through it.
- **Checks (§4 D):** deflection against the exact orbit, the shadow, the disc's shift (face-on exact, inclined against special relativity), surface brightness conserved, the disc's light against the nucleus's, the cost, the hand-off, stills.
- **Brightness up close (D-274):** the disc's light per pixel is softened above uu.Bh.DiscKnee (2 exposure units; logarithmic, colour kept; 0 physical): the owner found the physical disc masked by bloom and flare.
- **The look (D-276, D-277):** brightness relative to the exposure (uu.Bh.DiscBrightness 1); a volumetric disc in the same trace (a flared slab H = 0.025 rho of value-noise clouds in co-rotating log-spiral coordinates, wound 4 radians per e-fold, with gaps, rings and hot knots), Keplerian rotation (the ISCO's orbit 8 s, art) with two flow phases re-forming the clouds each orbit; the hole lenses as if 2x heavier (uu.Bh.LensScale 2: shadow and lensing 2x against the disc). A camera can fly through the disc; a kill zone is a gameplay choice, not a cost one.
- **D-278:** spiral 12, the hole 4x (uu.Bh.LensScale), erosion detail (uu.Bh.DiscErosion 0.8: torn cloud edges, a ragged surface and rim).

## 9. Each black hole's look (D-280) and stellar black holes (D-281)

Owner, 2026-11-17 22:35: "proceduralize the black hole within a range given the size of its parent galaxy, give it some procedural color palettes, ranges of twists, accretion disc amount based on the galaxy that hosts it. We should be able to scatter some stellar mass black holes around as well."

- **The look (D-280):** Uni_BhStyleOf (channel BH.LOOK): multipliers about the engine's uu.Bh.Disc* (the centre), ^uu.Bh.DiscVariety. Thin, dense, brighter discs on fast accretors; puffy, optically thin tori on slow ones (the art's boundary 10^-4.3..10^-3.5 of Eddington: ~22% of accreting holes); an early type's disc smaller and its palette warmer; a disc galaxy's spiral following its arms' winding; bands, heat, erosion, period (with the mass), brightness (with the rate) and extent jittered. Size from the host: its mass (D-272).
- **Palettes:** blackbody, gold, quasar blue, ember, violet, teal, rose, ice and fire (three stops placed by temperature against the peak, luminance kept); the nucleus sprite in the same palette (the hand-off's colour within 0.007). uu.Bh.DiscPalette.
- **Stellar black holes (D-281):** X-ray binaries as §2.3 proposed: a star's companion (channel BH.STELLAR; uu.Bh.StellarFraction 1e-3 of stars), 5..25 M_sun, in outburst (uu.Bh.StellarActive 0.3) or quiescent, on a close circular orbit; found when the star's system activates; its light a dynamic record (capped at half its star's, faded in); the lens pass at uu.Bh.StellarScale (30, art); uu.GotoStellarBlackHole. Isolated ones (lensing only) are still to come (B-044).
