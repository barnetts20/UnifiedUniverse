# Sprite and appearance pass: plan for review

*Draft, 2026-11-03. Status: for the owner's review; nothing here is built. It gathers the backlog items deferred to the "sprite work" milestone (B-001, B-003, B-006 rest, B-010, B-011, B-012), and proposes an order and the decisions needed before any code.*

## 1. What the milestone is

Everything so far made the universe correct, complete and fast: placement, streaming, conserved light (sprites + volume add up to every population's light, D-128), and travel at any speed. This milestone is about how it **looks**: what a star, a galaxy, a cluster and the web look like at every distance and speed, and the art direction choices that physical correctness leaves open.

Two kinds of change, kept apart as before:
- **Render-only** (no reroll): sprite shapes, glare, streaks, impostors, glows. Free to iterate.
- **Content** (rerolls what it touches): galaxy density fields (B-012), new objects (a brightest cluster galaxy). One decision each, recorded in DECISIONS with its reroll impact.

## 2. Where things stand

| Object | Far | Mid | Near |
| --- | --- | --- | --- |
| Star | point sprite, core + capped halo (D-156), glare grows with log flux | same | glare corona, disc once physically resolved (bodies only, D-148), promoted actor |
| Galaxy | round sprite (core + halo), colour from its temperature | volume march of its field (disc, bulge, arms; prefiltered, D-160/163), sprite fades out as the volume takes over (D-129) | its stars stream in as sprites; the volume carries the unresolved rest |
| Cluster | round extended sprite, faint | its members (galaxies) and its own β-model glow in the volume | members resolve; ellipticals in the core (D-172) |
| Group (D-174) | invisible (glow far below any limit) | its dwarfs and the web's bright galaxies | dwarfs resolve |
| Web | volume background, marched to 100 Mpc then a uniform tail (D-136/164/166) | same | n/a |

Known gaps: galaxies are round blobs until the volume takes over (no inclination, no shape at a few pixels); the galaxy field is the basic one (no warps, no organic irregularity); nothing special happens at warp speed except the travel-sensitivity boost; stars' glare never hands over to engine bloom.

## 3. Workstreams

### A. Galaxy look foundation (B-012, content) — investigated 2026-11-04
**Goal.** Galaxies that never look "samey": realistic physics (luminance, dust lanes) pulled towards a stylised palette, with variance from disc-level shape modes and texture warps rather than only from the spiral's parameters (owner's direction). Not a 1:1 port: our light and obscuration march stays (UltraLargeScale's march is "flat white: structure only", and it composes layers by max, which would break our light budget).

**What UltraLargeScale has (`GalaxyDensityCore.ush`, read-only, folder now connected) and what we take:**

| ULS feature | What it does | Take? | Cost | Reroll |
|---|---|---|---|---|
| Disc warp (m = 1 vertical) | z −= A rn² sin(θ − φ − twist·rn): the outer disc bends up on one side, down on the other, node line precessing | **Yes** | a sin per sample | stars |
| Lopsidedness (m = 1 in-plane) | density × (1 + a·rn·cos(θ − φ)): one side denser | **Yes** (azimuthal mean 1, so the light budget holds) | a cos | stars |
| Flare | scale height grows outward | **Yes** | none | stars |
| Per-arm asymmetry | each arm its own pitch factor, phase jitter, strength and length (end fade), from a hash | **Yes**: our arms are one m-fold cos wave, perfectly symmetric; this is the biggest "samey" fix | a loop over ≤ 4 arms (with ULS's angular early reject) | stars |
| Pitch tightening | arms wind tighter inward | **Yes** | none | stars |
| Domain warp from textures, in a frame that winds with the spiral | disc and arms displaced by a curl field; halo by a second one | **Yes**, our `warp_fine` (disc/arms) and `warp_large` (bulge/halo): divergence-free, so volume (and the normalising integral) is preserved | 1–2 texture reads | stars |
| Multiplicative modulation noise per layer | density × max(1 + a·n, 0) | **Yes**, `galaxy_detail` B (fBm) on the disc, G (Perlin-Worley clumps) as star-forming knots in the arms | 1 read, gated on density | stars |
| Central void | clears the very centre | No (looks artificial at our scales; a bar would be the better addition later) | | |
| Compact (1 − x²)^p profiles, max composition | | No (ours: exponential disc, n = 2 bulge, summed light) | | |

**Dust and colour (render-only, no reroll):** dust lanes become cloud-like: the dust field (D-132) × `galaxy_detail` R (ridged lanes), eroded by `ism_shape` / `ism_erode` (Nubis-style edges) and holed by `galaxy_detail` A (superbubbles). Colour: per-galaxy palette rolled from the seed (hue shift, saturation boost, arm/bulge contrast), arms bluer with pink knots (where the knot channel is high), bulge gold; the sprite's colour is the volume's integrated tint so the D-129 hand-off stays seamless. Physics stays: luminance from the budget, dust absorption and reddening as now.

**Rules kept:** one field source (placement, CPU integrals and the march read the same function; warp textures bound like the web texture, D-164, half-exact on the CPU); every new term prefiltered (the warp amplitude and modulation blend out as the pixel footprint passes their scale, D-160 pattern), so distant galaxies don't alias and the light is unchanged; Foot = 0 is the exact placement field.

**Steps:** A1 the analytic modes (warp, lopsidedness, flare, per-arm asymmetry, tightening): no textures, the cheapest big win. A2 load and bind `warp_fine`, `warp_large`, `galaxy_detail` (CPU loader generalised from FWebTexture; GPU bindings in the gen and volume passes). A3 texture warp and modulation in the field. A4 cloud dust. A5 palette. Each with offline checks (light against the budget per galaxy, T2.vol.equivalence / continuity, star census, stills before/after) and an engine pass.

**Reroll:** A1 and A3 move every galaxy's stars (their field changes; positions, counts per cell); A4 and A5 none. One decision entry each.

### B. Galaxies at a few pixels (new; render-only)
**Goal.** A distant galaxy reads as a disc at its inclination, not a round dot, before the volume takes over.
- The contributor's orientation is a pure function of the object's seed (RandomRotation on UNI_CH_ORIENT), so the project pass can compute it on the GPU from the record alone, and its axis ratio from the rolled disc/bulge weights (or a cheap proxy). The sprite becomes an oriented ellipse (axis ratio from inclination, position angle from the projected disc normal), with the same energy.
- **Hand-off:** the ellipse must match the volume's integrated shape at the D-129 hand-off size, so the swap is invisible; checked offline the way D-129 was.
- **Owner question:** worth it at all distances, or only above N px?

### C. Galaxy impostors (B-011; render-only; after A)
**Goal.** Keep the volume affordable when galaxy samples become costly (A) and many galaxies are a few to tens of pixels (cluster interiors).
- A small image per galaxy (e.g. 64² RGBA16F, view-dependent), rendered from its field by the same march, cached in an atlas, redrawn when the view direction to it changes by more than a few degrees or its size changes by 2×. Drawn as a sprite quad in place of marching it.
- **Band:** between the sprite (below ~8 px) and the full march (above ~64 px?). Light conserved by construction (the impostor is the march's own output).
- **Risks:** atlas memory and churn at speed (impostor renders per frame budget); popping when an impostor is refreshed (cross-fade).
- **Measure first:** with A's warp in, the march cost in a cluster core; build C only if it is over budget.

### D. Stars: glare, disc and bloom (B-003 stars; render-only)
**Goal.** Seamless from point to disc to promoted actor, and the glare handed to engine bloom once the star is an emissive actor.
- Today a star's glare stays as a corona through promotion (D-158). Options: (1) keep it (simple, consistent); (2) hand the glare to engine bloom at promotion with matched flux (UE-7 territory: emissive calibration); (3) per-star promotion at the glare/physical crossover (~16–17 px for every star, B-003's numbers) instead of the fixed 0.02 rad.
- **Art options to decide:** diffraction spikes (a camera artefact, very "space-art"), colour saturation of bright stars, limb darkening on resolved discs.

### E. Speed: warp streaks and Doppler (B-001; render-only)
**Goal.** Turn late-arriving objects at warp speed into streaks instead of pops, and make speed visible.
- Detection, tails from a ring of recent camera positions, collapse over 0.1–0.3 s, as B-001 describes; the ready-frame per cell is already known to the streamer (D-173 even knows which cells were late).
- Doppler and aberration: relativistic formula, or an artistic scale from speed relative to the level's crossing rate (at real v ≪ c nothing would show). Applying aberration to all sprites makes the starfield bunch ahead at speed (the classic look).
- **Owner questions:** decorative streaks (extra light) or conserved (borrowed from the volume's glow for those cells)? Aberration on everything or streaks only? At what speed should it start?

### F. Clusters: brightest cluster galaxy and intracluster light (B-006 rest; content + render)
- **BCG:** a giant elliptical at the centre of rich clusters, as an anchor-like first member (catalogue clusters know their centre exactly). Content (one new object per rich cluster; members' budget reduced by its light).
- **Intracluster light:** 10–20% of a rich cluster's light in a diffuse halo: an extra emission term on the cluster's own volume carrier, taken from the members' budget so the total is conserved. Render + budget.

### G. The web in the sky (B-010; render-only)
Options from D-170: (a) leave it physical (isotropic beyond ~100 Mpc); (b) a near-web glow (the web's density within N Mpc as a faint non-physical emission, own gain; uu.WebDebug's machinery in the beauty pass); (c) dim the uniform tail so the near structure carries contrast. A quick A/B in the engine decides it.

### H. Sky lighting (design spec item, not started)
The design calls for capturing the backdrop into the sky light's cubemap so promoted bodies are lit by and reflect the galaxy around them. Small and independent; fits here.

## 4. Proposed order

1. **A (galaxy look) with the warp textures**, since everything galaxy-shaped downstream (B, C, F) builds on it, and it is the only content change among the large items. Needs: the ULS folder connected, warp textures generated.
2. **B (inclined galaxy sprites)**, then measure; **C (impostors)** only if the cluster-core march is over budget with A in.
3. **D (stars)**: decide the glare/bloom option; small.
4. **E (streaks and Doppler)**: the most design-heavy; prototype offline (stills from a scripted flight), then the engine.
5. **F and G**: art tuning, best done with the owner in the engine (A/B toggles).
6. **H** whenever convenient.

Each step ends as now: offline checks (equivalence and continuity of light where it applies; stills), a REVIEW_TONIGHT section, then the owner's engine pass.

## 5. Decisions needed from the owner

1. ~~A: which UltraLargeScale features to keep; connecting it~~ (connected 2026-11-04; proposal in §3 A).
2. ~~A: the warp textures~~ (delivered: warp_fine, warp_large, galaxy_detail, ism_shape, ism_erode, curl_2d).
3. B: oriented galaxy sprites at all distances, or above a size?
4. D: glare kept as a corona, or handed to bloom; diffraction spikes yes/no.
5. E: streaks decorative or conserved; aberration and Doppler on all sprites or streaks only; onset speed.
6. G: physical sky, near-web glow, or dimmed tail.
7. ~~Art direction~~ (owner, 2026-11-04): realistic, pulled towards stylised/fantasy: more vibrant colours and more colour variation than real life, physics-based dust lanes and luminance, no two galaxies alike. Art references still welcome: a few images of the look you want (sky from inside a galaxy, a galaxy at a few degrees, warp travel) would anchor every one of these choices.
