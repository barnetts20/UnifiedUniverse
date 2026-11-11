# Paradigm review (2026-11-10)

*Owner: "we went multiple iterations to finally land on the 2D galaxy map, but it required us to break out of the design assumptions we had made at that point. Inspect the project and investigate other methodologies and ideas that represent paradigm shifts."*

The method: list the assumptions each part of the pipeline still rests on, ask what changes if each is dropped, and measure where that is cheap to do offline. The 2D map came from dropping "a galaxy is a 3D field sampled at every step". The candidates below drop a different assumption each. Ranked by what they would buy against the owner's traces (steady flight 23 ms, the march 12 ms, generation 0.2 ms; the visible defect is the slicing's sliding in motion).

## Summary

| # | Assumption today | Alternative | Evidence | Verdict |
|---|---|---|---|---|
| 1 | A galaxy seen from outside is marched like one seen from inside | Draw it from its 2D map as an impostor: the map projected with its analytic thickness, no march | D-219: the worst sliding (1.5-4%) is outside a galaxy (60 kpc), where the march resamples a sharp disc frame after frame | **Prototype next** (B-022, raised) |
| 2 | The march is split by carrier (background map vs per pixel) | Split it by distance: near marched every frame, far cached in world-aligned direction maps refreshed on a parallax budget | Measured (below): 77-100% of the light from inside the disc is beyond 300 pc and costs 60-77% of the steps | **Prototype after 1** (B-028) |
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
