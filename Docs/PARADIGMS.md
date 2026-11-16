# Paradigm review (2026-11-10)

*Owner: "we went multiple iterations to finally land on the 2D galaxy map, but it required us to break out of the design assumptions we had made at that point. Inspect the project and investigate other methodologies and ideas that represent paradigm shifts."*

The method: list the assumptions each part of the pipeline still rests on, ask what changes if each is dropped, and measure where that is cheap to do offline. The 2D map came from dropping "a galaxy is a 3D field sampled at every step". The candidates below drop a different assumption each. Ranked by what they would buy against the owner's traces (steady flight 23 ms, the march 12 ms, generation 0.2 ms; the visible defect is the slicing's sliding in motion).

## Summary

| # | Assumption today | Alternative | Evidence | Verdict |
|---|---|---|---|---|
| 1 | A galaxy seen from outside is marched like one seen from inside | Draw it from its 2D map as an impostor: the map projected with its analytic thickness, no march | D-219: the worst sliding (1.5-4%) is outside a galaxy (60 kpc), where the march resamples a sharp disc frame after frame | **Prototype next** (B-022, raised) |
| 2 | The march is split by carrier (background map vs per pixel) | Split it by distance: near marched every frame, far cached in world-aligned direction maps refreshed on a parallax budget | Measured (below): 77-100% of the light from inside the disc is beyond 300 pc and costs 60-77% of the steps | **Built, slow only** (B-028, D-228) |
| 3 | Stars are instanced quads through the raster pipeline | Compute splatting of sub-pixel stars (atomic adds into a light buffer); quads only for stars with halos | Schütz et al. rasterise 1-2 billion points a frame in compute | **Built (D-223, scatter + quads; UE-56)** |
| 4 | Generation is a CPU-orchestrated GPU round trip | GPU work graphs (cell → emitter → stars on the GPU) | Generation is 0.2 ms a frame; Ready latency is hidden by cell glow (D-203); UE 5.7 work graph access for plugins unconfirmed | Not now |
| 5 | Galaxies are continuous fields | Particle/sprite galaxies (SpaceEngine, Gaia Sky), or splatted Gaussians | Those engines draw galaxies as many soft sprites; Gaussian splatting needs a sort or moment OIT for dust | Not recommended |
| 6 | The warp is 3D, so the map is 2D plus a per-sample warp read | Bake the warp into vertical slices (D-217, B-025) | Re-measured after D-220 (below): 3.5x better, still 64 MB a galaxy for one fetch | Still parked |

## 1. Galaxies from outside as impostors (B-022)

**Assumption dropped:** every galaxy in view is a carrier, marched per pixel with temporal slicing.

**Why it matters:** D-219 found the march itself does not slide (0.1% against a fine reference), but the slicing's reconstruction does: 0.23% inside the disc, and 1.5-4.4% at 60 kpc, outside it. A galaxy seen from outside is a thin sharp disc filling part of the screen, which is the worst case for reprojecting a reconstructed image. Clusters and groups also march many carriers at once (D-200: rich cluster cores are cheaper per pixel than a galaxy interior, but every carrier is a ray-sphere test and some steps).

**The idea:** the 2D map already holds the disc's in-plane light and dust (D-212), and the vertical profile is analytic. From outside, a galaxy's image is the map seen through its inclination: each pixel's light is the column through the disc, which for an exponential profile is the face-on light over |cos i| attenuated by the dust's column. The dust's column is offset by the disc's height along the view, a shift of the dust map. Edge-on views need either a short low-resolution march through the projected thickness or a few depth layers. The impostor is rasterised (no march, no slicing, so nothing to slide), and it is the same map placement already reads (one field source holds: the map is baked from the field).

**What it costs:** a map per galaxy drawn this way. B-022 suggested 128²-512² maps (0.1-2 MB) baked at the size the galaxy covers on screen, from the same bake shader. The handoff becomes sprite → impostor → marched carrier (when the camera enters, or the galaxy is large on screen), and conservation still holds if the impostor's light is the carrier's light: the handoff table (D-196) already does this between the sprite and the carrier.

**To measure offline first:** the impostor against the march, for inclinations 0-85° and distances 20-200 kpc. Measure light error, per-pixel p95 and the edge-on failure angle. Then measure T2.vol.slide's 60 kpc flight with the impostor in place of the carrier (its sliding should go to the raster's, near 0).

**Measured (D-226, 2026-11-11):**
- A single map read at the disc crossing loses light with inclination (-3.4% at 50°, -11% at 75°): the radial profile is convex across the ray's path through the slab.
- K = 8 reads a side, through the slab: light within 0.6% and L1 2.5-2.6% up to 65°; 75° -1.4% (p90 23%); it breaks down past ~75°.
- At about 20-25 evaluations a pixel, an every-frame impostor costs more than the sliced march. So the proposal is cached billboards per galaxy, re-rendered on a view-angle and distance budget and drawn each frame by the camera's 2D transform. That is no sliding, a cost that follows refreshes, and the machinery #2 needs.

## 2. Split the march by distance, not by carrier (B-028)

**Assumption dropped:** a carrier is marched whole, per pixel, every frame (or reconstructed). Only the background has a world-aligned cache (the background map, D-136/D-166).

**Measured** (`explore.shells`, archived in `Tools/UniVerify/experiments/paradigm_shells_explore.cpp.txt`): the solar camera in the flythrough scene at the defaults, 192 x 108. Each view was marched to R and from R:

| View | Full, steps/px | Beyond 300 pc: light, steps | Beyond 1 kpc: light, steps | Within 300 pc: steps |
|---|---|---|---|---|
| Toward the centre | 52.7 | 99.8%, 40.5 | 97.8%, 36.2 | 13.0 |
| Along the disc | 41.5 | 97.2%, 27.4 | 84.6%, 22.4 | 13.0 |
| Out of the plane | 30.7 | 77.1%, 18.4 | 22.8%, 14.0 | 13.0 |

(The far light is unattenuated by the near dust; near plus far steps exceed the full march by about one restart step.)

**Why it matters:** the log stepping puts about a quarter to two fifths of the steps within 300 pc, where almost none of the light is. The steps beyond it carry nearly all the light and move slowly on screen. At the engine's march resolution (about 683 x 384 over 90°, 2.3e-3 rad a march pixel), structure at 300 pc moves one march pixel per 0.7 pc of sideways travel, at 1 kpc per 2.3 pc, and at 60 kpc per 140 pc.

**The idea:** generalise the background map. Keep a world-aligned direction map (a cube) holding the far part's light and transmittance from a split distance R outward, as seen from the position it was marched at. Re-march its texels in view, a slice per frame, when the camera has moved more than a parallax budget (R x a fraction of a texel), with the near part marched every frame and composited as the background is: B = B_near + T_near B_far(dir) (D-136's rule). R follows speed: at cruising speeds inside a galaxy (0.2 pc a frame) R near 300 pc keeps the far map valid for several frames; at 17 pc a frame (1 kpc/s) nothing near is reusable and R grows until the far map is the background map of today.

**What it buys:**
- Quality: the far texels are marched fresh at fixed directions and resampled into the screen once. That is the "reproject the fresh samples, not the reconstructed output" candidate D-219 left open (B-026), for most of the light. Turning the camera costs nothing for cached directions.
- Cost: in a slow cruise the 60-77% of steps beyond R run at a fraction of the frame rate. Today's slicing already marches 1/4-1/16 of the rays at slow motion, so the cost gain over today is modest. The quality gain is the point.

**Open questions:**
- The cube's resolution for the in-view part. It needs about 683² a face to match the march, and the texels in view number about the march's pixel count.
- Dust in the near part over far light (exact with the composite rule).
- Cell glow in the far map: kept out of the background map today because it changes while cells load (VOLUMETRICS §13). The far map would need the same per-pixel exception, or a refresh when a level's load state changes.

**To measure offline first:** T2.vol.slide's flights with the far part from a cached direction map refreshed on a parallax budget. Measure error and crawl against the fine reference, compared with today's slicing, and the steps per frame.

**Measured (D-227, 2026-11-11):**
- Slow (0.3 pc a frame) the far cache from 1 kpc costs 4.2 steps/px a frame against today's 12.7, with the same crawl.
- At 15 pc a frame it removes the slicing's sliding (crawl 0.91% -> 0.01-0.09%) but must be re-marched every frame: 35-42 steps/px, ~3x today.
- A quality/cost trade, switched by speed if wanted.

**Built (D-228, 2026-11-11), slow only:**
- Costed at engine resolution first. With the cube as fine as the march, refreshes must be per face and in view only; refreshing the whole cube costs more than today as soon as one happens. A face's re-march costs ~3x a frame's march.
- Break-even is ~0.06 texels of parallax at R a frame. Steps against today's at R 2 kpc: 0.38x at rest, 0.52x at 0.3 pc/frame (192-texel faces), 0.92x at 1, 1.74x at 3.
- So it's a parked/drifting win; the quality-at-speed case (no sliding, ~3x the march) was not built.
- The rules, fills and history conversion are in UniCore/UniFarCache.h and VOLUMETRICS' controls table.

## 3. Stars by compute splatting (B-029)

**Assumption dropped:** every visible star is a quad through the raster pipeline (one instanced indirect draw, additive, depth-tested in the pixel shader).

**The idea:** most stars are sub-pixel. A compute pass that adds each star's light into a fixed-point light buffer with atomics (Schütz et al., "Software Rasterization of 2 Billion Points in Real Time", 2022) costs per star, not per covered pixel, and has no quad overdraw or small-triangle waste. Stars with halos, spikes or streaks (D-158, E1) stay on the quad path. Occlusion by promoted bodies is one scene-depth read per star, and extinction is already a per-star factor.

**What it would buy:** headroom to raise `uu.Star.Keep` back toward 1 (it is 0.35 for spawning and draw cost), if the draw is a real share of the frame.

**First:** a ProfileGPU capture of the sprite pass (Uni sprite draw and project) in a dense core. If the sprites cost a millisecond or less, this is not worth it yet.

## 4. Considered, not recommended now

- **GPU work graphs for generation.** They would remove the probe/alloc/place round trip, but generation is 0.2 ms a frame, and D-207's one trip plus D-203's cell glow already hide its latency. I could not confirm that UE 5.7 exposes work graphs to plugins (no official documentation found; a third-party article contradicts itself). Revisit if generation ever shows in a trace.
- **Particle or Gaussian galaxies** (SpaceEngine draws galaxies as procedurally generated sprites; Gaia Sky builds them from per-channel particle distributions in compute). This would break D-128's exact sprite + glow conservation and the sharp analytic structure. Dust needs ordered blending (a sort, or moment-based order-independent transmittance), and a disc seen from inside needs many small splats. Our map plus march already gives the structure at a known cost. The part worth taking from those engines is item 1: cheap far galaxies.
- **Stateless stars** (regenerated every frame from hashes, no pool or streaming). This trades a one-time spawn for a per-frame one, and placement rejects about 3.5 candidates a star (D-214). Not a win while spawning is what stuttered.
- **A screen-space layered history** (several depth layers per pixel, each reprojected at its own depth). D-219 found that reprojecting at the exact light depth changes nothing (2.73% → 2.73%): the error is repeated resampling of sharp structure, not depth. Layers don't address that; item 2's world-aligned cache does.

## 5. Premises changed by D-220

D-220 moved the disc's warp to mip 2 and up. Two earlier measurements rested on the old warp:
- **B-025 (sliced map), re-measured.** `univerify galslices` was updated to D-220: its rebuilt field now applies the fine band and the warp's mip floor, and matches UniField_Galaxy to 7e-6 (it was 5.3 before the fix). Home galaxy: 4 slices 10.0% L1 per point (D-217: 24%), 8 slices 3.9% (13.7%), view p95 3.4-4.6%. Fast warp, 8 slices: 8.2%. Thick disc, 8 slices: 25.5% (its warp still varies every 2 H in height). Better by about 3.5x, but 8 slices of 1024² are 64 MB a galaxy (16 MB at 512²) to save one texture fetch of four, and thick discs still fail. Still parked.
- **D-219's sliding numbers** were measured on an untextured galaxy (the flaw I noted in D-220). The engine localisation in REVIEW_TONIGHT §53 is the next step either way. If it points at the slicing, T2.vol.slide should be re-run with textures before tuning anything.

## Sources

- [SpaceEngine blog, 17 Feb 2019: galaxy and nebula sprite models](https://spaceengine.org/news/blog190217/)
- [Gaia Sky: procedural galaxy generation preview (2025)](https://gaiasky.space/news/2025/galaxy-generation/)
- [Schütz, Kerbl, Wimmer: Software Rasterization of 2 Billion Points in Real Time (2022)](https://www.cg.tuwien.ac.at/research/publications/2022/SCHUETZ-2022-PCC/)
- [compute_rasterizer source](https://github.com/m-schuetz/compute_rasterizer)
- [Moment-Based 3D Gaussian Splatting: order-independent transmittance](https://arxiv.org/html/2512.11800v1)
- [Microsoft: D3D12 work graphs](https://devblogs.microsoft.com/directx/d3d12-work-graphs/)
- [UE forum: DirectX 12 work graph support (no official answer)](https://forums.unrealengine.com/t/directx12-work-graph-support/2225357)

# Volume rendering paradigms (2026-11-15)

*Owner: "We also are not married to the voxel marching paradigm, we put it in place to solve the grain ... if there is another paradigm that can do an analytic march without the grain that would be fine too and would save us the caching memory overhead ... I am basically open to whatever is going to get us the best combo of appearance (semi-realistic galaxies with fantasy color palettes) and performance - 60 fps min with enough overhead for the rest of our gpu operations to function. It might be worth just composing a big list of different paradigms along with pros/cons of each. might also be worth crawling for other projects that do proc gen galaxies and their methodologies."*

## What the problem actually is

Measured this week (D-251 to D-255, `experiments/nebula_grid_d255`):

- **Grain is undersampling.** It comes from any layer whose structure is finer than the step, read at one jittered point per step. It was first the nebulae's light, then the dark nebulae's dust, then starlight at 3 kpc. Temporal accumulation turns it into flicker or smear, never into the right answer.
- **The fix is always the same rule: one sample must stand for its whole step.** There are two ways to obey it. Either integrate the layer exactly along the step (the cell walk), or blur the layer to the step's size before reading it (cone tracing; `uu.Volume.StepFoot` for starlight).
- **The budget.** At 60 fps a frame is 16.7 ms. The volume should take about 2-4 ms of it at 1080p-1440p. Guerrilla's clouds budget 2 ms. The march has measured 4-60 ms in the owner's past captures, depending on the scene. No measurement of D-255 in the engine yet.
- **The layers differ in kind.**
  - Starlight and large-scale dust are smooth, and separable: a 2D map times an analytic height profile (D-212).
  - Nebulae are sparse, sharp, high-contrast and 3D.
  - Stars are points, already splatted (D-223).
  
  One paradigm for all of them is not required. What's required is that each layer obeys the rule above at a known cost.

## The candidates

| # | Paradigm | Grain | Fidelity | Cost | Memory | Movement | Who uses it |
|---|---|---|---|---|---|---|---|
| A | Per-pixel analytic march, jittered + temporal accumulation (our D-251) | yes: undersampling | infinite detail, if sampled enough | steps x field cost (high) | none | smear / reprojection artifacts | Horizon clouds 2015, most Shadertoy galaxies, GAMER (offline, fixed steps) |
| B | Analytic march, cone-traced (each sample prefiltered to max(pixel, step)) | no | softer laterally at long steps | as A, fewer steps | none | none | GigaVoxels' cone tracing; Inria star octree (leaf = pixel footprint) |
| C | Separable analytic integration (2D map x analytic height, integrated per segment) | no | exact for separable layers; none for 3D-only structure | few samples a ray | the map (already paid: 1-4 MB a galaxy) | none | Inria report (mip x tanh integral); our D-212 map |
| D | Camera-centred sparse voxel clipmap + cell walk (our D-252..D-255) | no | cells ~ distance / 48: chunky at range | reads ~ cells crossed | 45-171 MB | rebake on fast moves | Nubis voxel clouds; UE heterogeneous volumes |
| E | Ray-guided sparse voxels (bricks baked on demand at the footprint's LOD) | no | resolution follows the screen | as D | bounded by the screen | a frame of latency on new views | GigaVoxels (Crassin 2009) |
| F | Froxels (frustum-aligned 3D texture, density injected, integrated once) | no | ~160x90x64: haze only, no sharp structure at range | ~0.5-1 ms fixed | ~10-50 MB | reprojected per froxel | UE Volumetric Fog, Frostbite 2015, The Last of Us Part II |
| G | World-aligned direction caches for far content (our far cache D-228; skybox cubemaps) | inherits its source | full at the texels | near zero per frame; refresh spikes | cube faces | parallax budget; useless at speed | our background map, Elite / galvan's procedural skyboxes |
| H | Impostors from the 2D map (galaxies from outside) | no | sharp to ~75° inclination | raster, cheap | map per galaxy | cached billboards | our D-226 measurement (B-022), SpaceEngine's LOD sprites |
| I | Particle / sprite galaxies (soft sprites, dust sprites) | no | artistic; weak from inside | raster; overdraw | particles | popping, sorting | SpaceEngine, Gaia Sky, beltoforion density-wave renderer |
| J | Gaussian splats baked from the field | no | high | raster + sort | per galaxy | sort each frame; OIT for dust | 3DGS literature; nobody for galaxies yet |
| K | Lower resolution + upscale (quarter-res, checkerboard) | orthogonal | lower | /4 | small | upscale artifacts | Star Citizen clouds (quarter-res, 2026), Horizon (1/16 a frame) |

### A. Per-pixel analytic march with temporal accumulation (D-251, today's fallback)
- **For:** no memory; detail limited only by the field; one code path.
- **Against:** grain whenever a layer is finer than the step, and the steps must be long at range or the cost explodes. Temporal accumulation trades grain for smear, and it fails under motion, which is this project's main use. Cost is steps times field cost, and our field cost per step is high (warps, detail, knots, map).
- **Verdict:** keep only as the reference and the fallback.

### B. Analytic march, cone-traced
- **For:** no cache. The grain is gone by construction, because every read is prefiltered to its step. Starlight already does this (D-255's `StepFoot`), and nebula detail did a variant (D-251's `FineStepLod`).
- **Against:** the prefilter blurs in all directions while the step is a line, so lateral detail is lost at long steps. Nonlinear terms don't prefilter linearly: the nebulae's erosion, the knots' multiplier and D-258's shaped noise all bias as they blur. That's measured: −3.7% light at 3 kpc with `StepFoot` 0.5 (D-255), and the D-251 fine step LOD.
- **Verdict:** right for the smooth layers. Wrong for nebulae, whose look is the nonlinear erosion.

### C. Separable analytic integration
- **For:** the galaxy's light and large-scale dust are separable: an in-plane map times a height profile (D-212). Along a segment the height integral is analytic (exponential, sech²). The in-plane part is a line integral through the map, done by a few reads along the segment's projection, each at the mip of its spacing. Rays out of the plane cross the disc in a few samples. Rays in the plane need the projection's length divided by the mip's texel, which is bounded by the map's mips. Fits the Inria report exactly. No new memory.
- **Against:** covers only what is separable. The 3D detail (warps, galaxy_detail, D-258's structure read), the nebulae and the bulge's noise are not. The warp moves the disc ~100 pc in height, so the height integral is along a warped mid-plane (the map already stores the warp's large part, M.z).
- **Verdict:** the strongest cost lever for starlight and the large dust. It replaces most of A's steps for the home galaxy. Worth a measured prototype.

### D. Sparse voxel clipmap + cell walk (today)
- **For:** solved the nebula grain, deterministic, empty-space skipping, cost decoupled from the field's complexity (baked once). Light and dust from one source.
- **Against:** 45 MB at 128 cells, 171 MB at 256. The cell is about distance / 48, so nebulae are chunky at range: 64 pc cells, ~13 pixels, at 3 kpc. D-257's detail-on-top did not help. The bake costs during fast flight, and there are two paths (grid inside its reach, field outside).
- **Verdict:** right for nebulae (sparse, sharp, 3D). Memory and range fidelity are its limits; E answers both.

### E. Ray-guided sparse voxels
- **For:** the march records which bricks at which LOD each pixel needed, and the planner bakes those next frame. Resolution follows the screen, not a box around the camera, so a nebula at 3 kpc in view gets pixel-sized cells and nothing behind the camera is baked. Memory is bounded by what is visible. It's GigaVoxels' design, and our pool, table and bake are most of it already.
- **Against:** one or two frames of latency on newly seen regions. Those show the coarser level meanwhile, a pop to manage with a blend. Needs a request buffer, its readback (or a GPU-side planner) and LRU eviction. Fast turns request many bricks at once.
- **Verdict:** the real fix for chunky nebulae at range without 171 MB. Next for nebulae after C, if range fidelity matters more than the pop.

### F. Froxels
- **For:** a fixed, small cost (UE's Volumetric Fog runs in about a millisecond). Every source is injected into one camera-aligned grid, integrated once and read per pixel. Temporal reprojection is per froxel. It plugs into UE's own volumetric fog.
- **Against:** resolution. A 160x90x64 grid with exponential depth slices can't hold a nebula's edge or a dust lane at kpc range; it's a haze. Light leaks across cells.
- **Verdict:** good for the near haze, the light the camera sits inside. Not for galaxy-scale structure.

### G. Far direction caches (built: D-228)
- **For:** far light marched once and reused while the parallax budget holds. The background map already does this.
- **Against:** useless at speed (re-marched every frame, ~3x cost; D-227), with refresh spikes.
- **Verdict:** keep as is.

### H. Impostors for galaxies seen from outside (B-022, measured in D-226)
- **For:** a galaxy from outside becomes its map projected through its inclination. That's cheap, sharp and can't slide.
- **Against:** past about 75° (edge-on) it needs a short march. One map per galaxy drawn this way.
- **Verdict:** the right answer for every galaxy except the one you're in. Its cost is what D-226 measured; build when the field of other galaxies shows in a trace.

### I. Particle / sprite galaxies
- **For:** SpaceEngine and Gaia Sky draw galaxies this way. The density-wave renderers place stars and dust as sprites on elliptical orbits. Very art-directable, cheap from outside.
- **Against:** from inside a galaxy it needs enormous counts; dust needs sorting; it breaks D-128's light conservation and the one-field-source rule.
- **Verdict:** no.

### J. Gaussian splats baked from the field
- **For:** state of the art for captured scenes.
- **Against:** a per-frame sort, order-independent transparency for dust, a bake per galaxy, memory per galaxy. No precedent for procedural galaxies.
- **Verdict:** no.

### K. Lower resolution + upscale
- **For:** a /4 cost cut, orthogonal to everything above. Star Citizen just added a quarter-res cloud mode without temporal accumulation, alongside half-res and temporal modes. Our march already runs below screen resolution (`uu.VolumeDivisor`).
- **Against:** softer.
- **Verdict:** a knob, not a paradigm; keep it in the budget.

## What other projects do

- **SpaceEngine:**
  - Galaxies are sprite models, procedurally distributed to approximate the dust band, with LOD switching for performance.
  - Real nebulae are sprites. Procedural nebulae are raymarched, from Shadertoy-derived shaders.
  - No published costs. ([blog 2019-02-17](https://spaceengine.org/news/blog190217/), [2016-10-08](https://spaceengine.org/news/blog161008/))
- **Gaia Sky:**
  - Procedural galaxies from per-channel particle distributions in compute (bulge, disc, arms, dust, H II regions).
  - Nebulae are raymarched in a full-screen post pass with fixed steps, no LOD.
  - Sources: [galaxy generation](https://gaiasky.space/news/2025/galaxy-generation/), [nebulae](https://tonisagrista.com/blog/2024/rendering-aurorae-nebulae/).
- **Inria (Gérôme / Neyret 2026, owner's PDF):**
  - Rays step from cell to cell of a virtual star octree, with the leaf at the pixel's footprint.
  - Cell integrals come from 2D mips times an analytic height integral.
  - Dust is sampled once per cell, which they list as an open problem.
  - 30-60 fps at 1200x675 on an RTX 4080 for a whole galaxy, about 15 fps in the core.
- **GAMER (Groeneboom & Dahle 2014, owner's PDF):**
  - Offline, fixed linear steps.
  - Its look comes from the model: Ringermacher & Mead arms (D-256), twirled noise, and nonlinear noise shaping per component (D-258).
- **Guerrilla:**
  - Horizon's clouds are procedural noise raymarched within 2 ms, with temporal reprojection ([2015](https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn)).
  - Nubis³ moved to voxel clouds with compressed signed distance fields for ray acceleration ([2023](https://www.guerrilla-games.com/read/nubis-cubed)).
- **Star Citizen:** volumetric clouds raymarched into a low-resolution buffer and upscaled; a quarter-res mode without temporal accumulation added in 2026 ([PCGH](https://www.pcgameshardware.de/Star-Citizen-Spiel-3481/News/Wolken-Rendering-Quarter-Res-Monatsbericht-September-2026-1556196/)).
- **Unreal:**
  - Volumetric Fog is froxels.
  - Heterogeneous Volumes (experimental) march sparse volume textures with a step factor relative to the voxel size and a lighting cache ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/heterogeneous-volumes-in-unreal-engine)).
- **Elite Dangerous:** a 400-billion-star procedural Milky Way (Stellar Forge). Its sky is generated from the galaxy model, but no rendering details are published.
- **Stars:**
  - Compute rasterisation of sub-pixel stars by solid angle: 22 million stars in about 8 ms at 4K on an RTX 4070 Super ([Schneegans et al. 2025](https://www.uni-weimar.de/de/medien/professuren/medieninformatik/vr/research/real-time-rendering/smaller-than-pixels-rendering-millions-of-stars-in-real-tim/)).
  - Our D-223 does the same.

Nobody publishes a real-time, fly-through, procedural galaxy at our fidelity with costs. The closest are the Inria report (whole galaxy, ~4080-class, 30-60 fps, no nebulae) and Guerrilla's clouds (2 ms, one layer).

## Recommendation: one rule, one paradigm per layer

| Layer | Paradigm | Status |
|---|---|---|
| Stars | compute splat (D-223) | built |
| Other galaxies | sprite, then cached impostor from the map (H), then march when inside | sprite + march built; impostor measured (D-226) |
| The home galaxy's starlight and large dust | **separable analytic integration from the map (C)**, cone-traced where not separable (B) | B for starlight built (D-255 `StepFoot`); C to prototype |
| Nebulae | sparse voxels + cell walk (D), **ray-guided (E)** for range fidelity and memory | D built (D-255); E to prototype |
| Near haze | the march's near steps (or froxels, F) | built |
| Far light at rest | far cache (G) | built (slow only) |
| Everything | resolution divisor + upscale (K) | built (`uu.VolumeDivisor`) |

Why: C removes most of the steps for the layer that covers the whole screen (starlight and dust, about 40-90 steps a pixel today) with no memory and no grain. D/E keeps the nebulae sharp and stable at a memory cost that follows the screen. The march that remains is short: near haze, the field outside the grid's reach, and the galaxy's 3D detail at small footprints.

**Before building, measure in the engine:** a ProfileGPU capture of `Uni.Volume.March` (with D-255) inside the Milky Way at 250 pc, 1 kpc and 3 kpc from a nebula, still and moving, at your target resolution. That says whether the steps (C), the grid reads (E), or the resolution (K) is the first lever.

**Prototype order, offline:**
1. C for the home galaxy: a ray's starlight and dust as the analytic segment integral from the map against today's march. Measure the error, steps a ray and the grain.
2. E for nebulae: requests by footprint, bake latency, memory against 128/256 cells, range error at 3 kpc.

## Sources (added)

- [Inria: Gérôme, Neyret, Massive Procedural Rendering of Stars on the GPU (2026), hal-05713074](https://inria.hal.science/hal-05713074)
- [Groeneboom & Dahle, GAMER, ApJ 783:138 (2014)](https://iopscience.iop.org/article/10.1088/0004-637X/783/2/138/pdf)
- [GigaVoxels (Crassin et al. 2009)](https://publications.graphics.tudelft.nl/papers/423)
- [Nubis Cubed (Guerrilla 2023)](https://www.guerrilla-games.com/read/nubis-cubed)
- [Horizon Zero Dawn cloudscapes (Guerrilla 2015)](https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn)
- [Physically Based and Unified Volumetric Rendering in Frostbite (Hillaire 2015)](https://www.slideshare.net/DICEStudio/physically-based-and-unified-volumetric-rendering-in-frostbite)
- [UE5 Volumetric Fog](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-fog-in-unreal-engine), [UE5 Heterogeneous Volumes](https://dev.epicgames.com/documentation/en-us/unreal-engine/heterogeneous-volumes-in-unreal-engine)
- [Volumetric Effects of The Last of Us Part II (Kovalovs)](https://history.siggraph.org/learning/volumetric-effects-of-the-last-of-us-part-two-by-kovalovs)
- [Star Citizen quarter-res clouds (PCGH, 2026)](https://www.pcgameshardware.de/Star-Citizen-Spiel-3481/News/Wolken-Rendering-Quarter-Res-Monatsbericht-September-2026-1556196/)
- [SpaceEngine blog 2019-02-17](https://spaceengine.org/news/blog190217/), [2016-10-08](https://spaceengine.org/news/blog161008/)
- [Gaia Sky galaxy generation](https://gaiasky.space/news/2025/galaxy-generation/), [Gaia Sky nebulae](https://tonisagrista.com/blog/2024/rendering-aurorae-nebulae/)
- [Smaller than Pixels (Schneegans et al. 2025)](https://www.uni-weimar.de/de/medien/professuren/medieninformatik/vr/research/real-time-rendering/smaller-than-pixels-rendering-millions-of-stars-in-real-tim/)
- [Elite Dangerous: Stellar Forge interview (space.com)](https://www.space.com/31366-elite-dangerous-stellar-forge-interview.html)
- [beltoforion spiral galaxy renderer (density waves)](https://beltoforion.de/en/spiral_galaxy_renderer/spiral-galaxy-renderer.html)
