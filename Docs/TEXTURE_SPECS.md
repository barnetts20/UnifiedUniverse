# Unified Universe — noise texture specs (for the texture authoring project)

Version 2.1, 2026-11-03 (v2: UE volume textures with full mips from your baking pipeline instead of raw files; `web_density` added. v2.1: the runtime now reads a web texture, D-164; see "Runtime status" below). Owner: Unified Universe plugin. Decision records: D-161, D-164 (Docs/DECISIONS.md).

## Why these textures

Today every density field is computed from scratch at every sample. The cosmic web, for example, is a Worley lattice: a 27-cell search with hashing, up to five of them per volume step with clusters on. The volume pass takes ~107 steps per background ray, and at intergalactic speed that costs 11–15 ms per background map face. The fix is to sample baked, tileable 3D noise and combine layers of it at runtime. Everything the runtime needs to stay tunable, it does itself:

- weights, widths and sharpening (the `uu.Cluster.*` handles keep working);
- warping, with warp layers whose world periods are deliberately non-commensurate with the base tile, so the combined pattern effectively never repeats;
- per-universe offsets and rotations.

The textures are **content, not art**. Galaxy and cluster placement (generation) will sample the same textures as rendering, so a texture change is a new universe (fine while the project is in construction). They must stay linear and uncompressed, because the values are data: thin walls and distance fields are destroyed by block compression.

## Runtime status (v2.2, D-165)

**In use:** `web_adhesion_density_{young,mid,old}` (default mid), read from the raw `.bin` / `_mips.bin` in `Data/Noise` (the `.uasset`s are not used by the runtime). Tile = 200 Mpc (2^89 cm; periods must be powers of two). Clusters are the heaviest nodes of `*_nodes.json` (rank-mapped to the cluster LF, D-167); field galaxies follow R + G + A and the rest of B. Any power-of-two size loads (256³ planned). **Next:** the warps, `galaxy_detail` and the ISM set.

## Fidelity notes (owner: up to 512³ if there's a reason; keep memory and cost in mind)

Costs per texture (RGBA16F): 128³ = 16 MB on the GPU, 256³ = 128 MB, 512³ = 1 GB, plus 14% for mips. The runtime also keeps a CPU copy, because placement's normalising integrals and the offline checks sample it. The copy is half precision (D-167), the same size as the GPU's.

| Texture | Now | Verdict |
|---|---|---|
| `web_adhesion_density_*` | 128³ (1.56 Mpc texels at the 200 Mpc tile) | **The one worth trying at 256³** (0.78 Mpc texels). Filaments are 1–3 Mpc wide, so at 128³ they are one or two texels across. Within ~20 Mpc of the camera, where galaxies resolve and the glow is near, they would be visibly sharper. Cost: 128 MB GPU + 128 MB CPU (as half). Not 512³ (1 GB). Same seed and recipe, so the two can be A/B'd. |
| `web_adhesion_*` (distances, 256³) | not used yet | Keep for now. An alternative to the 256³ density: rebuild the near web from distances with live widths (the spec's original T1 plan). Decide after seeing the 256³ density. |
| node list | used next (B-007) | No resolution question: clusters are discrete objects. |
| `warp_large` 128³ | not used yet | Fine: smooth by design. |
| `warp_fine` 64³ | not used yet | **128³** (owner, D-167): its top octave is 32 periods per tile, 2 texels per period at 64³. 16 MB. |
| `galaxy_detail` 128³ RGBA8 | not used yet | Maybe 256³ later, if flying inside galaxies shows repetition. Decide once it is in use. |
| `ism_shape` 128³ | not used yet | Fine. |
| `ism_erode` 32³ | not used yet | **128³** (owner, D-167): the spec asked for 32 periods per tile on 32 texels (one per period: aliased away; my spec error). 8 MB. |

## Runtime status (v2.1)

The runtime side of the web is in (D-164), running on a stand-in baked from today's procedural web. What it reads today is the **T1b layout**: an RGBA16F volume with full mips, where R = walls, G = filaments, B = nodes as **densities** in [0, 1] (A unused). It rebuilds the web as floor + w_wall·R + w_fil·G + w_node·B, with live weights. So the quickest way to try the adhesion-model web is a texture in that layout:

- R, G, B: the adhesion web's walls, filaments and nodes as separate density channels (or density in R alone with G = B = 0 to start), linear, mean-preserving mips.
- One tile = one period of the web (100 Mpc in the demo today; tell me the physical size your tile represents and I'll set the period to match).
- The node list (positions and masses) is wanted as well: cluster placement should move onto it.

The distance channels (T1 `web_cells`) remain useful for live widths close up, but are no longer needed to start.

## Common requirements (every texture)

| Item | Requirement |
|---|---|
| Tiling | Seamless and periodic on **all three axes**: texel (x, y, z) wraps to (x mod N, …). Generate on a periodic domain. Mirroring or cross-fading doesn't count. |
| Isotropy | No axis-aligned structure. The power spectrum should agree along x, y, z and the diagonals within a few %. Grid-aligned artefacts show up as visible lattices on a cube map or at a 90° view. |
| Stationarity | The same statistics everywhere in the tile: no regions that are denser, smoother or busier than the rest. |
| Asset | A UE **Volume Texture** from your baking pipeline, **with the full mip chain**. |
| Colour space | Linear data: sRGB off. |
| Compression | **Uncompressed** (e.g. `TC_HDR` / RGBA16F, RGBA16, G16, or the per-texture format below). No BC/ASTC, no dithering. |
| Mips | Full chain, **wrap-aware**: every mip texel averages across the tile edges (a clamped mip filter shows the tile seams at distance). Plain box or Kaiser averaging is fine; it must be an average (not min, max or point). |
| Address mode | Wrap on all three axes. |
| Sidecar | A JSON file next to each asset. Fields: `name`, `size` [N,N,N], `format`, per-channel `semantic`, `range` [min,max] of the encoded meaning, `mean`, `std`, `min`, `max` of mip 0; the generator name, parameters and seed (to regenerate it); `period_check` (max abs difference across each wrap face, ~0 for a periodic generator). |
| Raw mip 0 (if cheap) | Also `<name>_mip0.bin`: mip 0 as raw little-endian, x fastest, then y, then z, channels interleaved, in the asset's format. Only for my offline checks, which run the shader code on the CPU and can't read a `.uasset`. If your pipeline can't emit it, skip it and I'll test textured fields on the GPU only. |
| Preview (optional) | A slice atlas PNG for eyeballing. |

Delivery: `Content/UnifiedUniverse/Noise/<name>` (asset) + `<name>.json` (+ `<name>_mip0.bin`) in the plugin.

---

## T1. `web_cells` — cosmic web cellular distances  *(highest priority)*

The cosmic web as Voronoi distances, so the runtime can rebuild walls, filaments and nodes with live widths and weights.

| Item | Value |
|---|---|
| Size | 128³ |
| Format | `RGBA16_UNORM` (16 MB) |
| Sites | **8 × 8 × 8 = 512** Voronoi sites per tile (16 texels per cell), periodic. Placement: a jittered grid with jitter 0.85 of the cell, or blue-noise / Poisson-disc with the same density. Avoid near-coincident sites (minimum separation ≥ 0.25 cell). |
| R | F1: distance to the nearest site |
| G | F2 − F1 (walls are where this is ~0) |
| B | F3 − F1 (filaments: where three cells meet) |
| A | F4 − F1 (nodes: where four cells meet) |
| Units | All four in **cell units** (1.0 = the mean site spacing), clamped to [0, 1] and stored as UNORM. Distances must be true Euclidean, computed with periodic wrap. |
| Notes | These are distances, not densities: the runtime applies exp(−(d / width)²) itself, so the widths stay tunable and nothing is baked in. Distances trilinear-filter cleanly; that's why distances and not ridges. Its mips average distances, which is not the mean density, so for distant samples the runtime uses `web_density` (T1b) instead. |

World mapping (runtime): one tile = 160 Mpc → 20 Mpc cells (today: 100 Mpc, 5 cells). An optional second instance of this texture is sampled at a non-commensurate scale and rotation for the finer lattice.

**Deliverable extras:**
- The site list (`web_cells_sites.json`: 512 × xyz in [0, 1)).
- A second variant with a different seed (`web_cells_b`), so we can compare, or use both as two lattices.

## T1b. `web_density` — the web's mean density, for distance

Mips of T1 average distances, and exp(−(mean d)²) ≠ mean of exp(−d²): a distant pixel that covers whole cells needs the true average density. This texture is that density, baked from T1 at the default widths and weights, so its mip chain does the averaging correctly.

| Item | Value |
|---|---|
| Source | Derived from T1 (same sites, same tile), texel for texel |
| Size | 128³, full mips |
| Format | `RGBA16F` (16 MB + mips) |
| R | Walls: exp(−(G / 0.12)²) |
| G | Filaments: exp(−(B / 0.12)²) |
| B | Nodes: exp(−(A / 0.12)²) |
| A | Unused (0), or mean of R, G, B for debugging |
| Notes | 0.12 = today's web width in cell units. Supersample the exp at 4×4×4 per texel at mip 0 (the ridge is ~2 texels wide), so mip 0 is already the texel's average. The runtime weights R, G, B (walls, filaments, nodes) with the live handles; only the width is fixed at bake time, and only for distant samples. |

## T2. `warp_large` — large-scale divergence-free warp

Bends filaments and walls into organic curves without piling up or thinning out density. Divergence-free warps preserve volume, so normalisation and mean density stay exact.

| Item | Value |
|---|---|
| Size | 128³ |
| Format | `RGBA16F` (half float, 16 MB) |
| RGB | A **curl-noise** displacement vector: the curl of a smooth periodic vector potential (3 octaves of periodic gradient or simplex noise, base frequency **2 periods per tile**, lacunarity 2, gain 0.5). Scale the whole field so its **RMS magnitude = 1.0**; the runtime sets the amplitude. |
| A | A scalar periodic fBm, independent of the potential: 4 octaves, base frequency 2 per tile, zero mean, std 1 (range ~[−4, 4]). Used for density variation (richness modulation). |
| Checks | Report the mean |divergence| relative to the mean |gradient| (should be ≲ 1%), and confirm the RGB means are ≈ 0. |

World mapping (runtime): period ≈ 1.618 × the web tile, rotated, so warp and base never line up.

## T3. `warp_fine` — small-scale divergence-free warp

| Item | Value |
|---|---|
| Size | 64³ |
| Format | `RGBA16F` (2 MB) |
| Content | As T2 but base frequency **4 periods per tile**, 4 octaves; RMS 1.0. A = fBm at base frequency 4, zero mean, std 1. |

World mapping: period ≈ 0.382 × the web tile (the golden-ratio partner of T2). It's also used, at galaxy scale, for flocculent arm perturbation.

## T4. `galaxy_detail` — structure inside galaxy discs

Multiplies the analytic disc, arm and bulge model to give flocculent arms, star-forming knots, dust filaments and superbubbles. It's sampled in each galaxy's own frame, with a per-galaxy offset and rotation, so every galaxy differs.

| Item | Value |
|---|---|
| Size | 128³ |
| Format | `RGBA8_UNORM` (8 MB) |
| R | **Ridged multifractal** (1 − abs(noise), sharpened): 5 octaves, base frequency 4 per tile. Thin connected ridges, for dust lanes and filaments. |
| G | **Billow or Perlin–Worley** clumps: 4 octaves, base frequency 6 per tile. Blobby knots, for star-forming regions. |
| B | Plain **fBm**: 6 octaves, base frequency 3 per tile. General modulation. |
| A | **Inverted Worley F1** (1 − F1 normalised), base frequency 5 per tile, 2 octaves. Rounded cavities, for superbubbles and voids in the ISM. |
| Values | Each channel normalised to [0, 1], with its mean and std reported. |

## T5. `ism_shape` + `ism_erode` — close-range volumetric detail (nebulae / ISM)

For flying *inside* a galaxy, near enough that volumetric clouds and nebulae resolve. This is the standard real-time cloud set (Nubis / Horizon Zero Dawn style).

**`ism_shape`**

| Item | Value |
|---|---|
| Size | 128³ |
| Format | `RGBA8_UNORM` (8 MB) |
| R | Perlin–Worley: Perlin fBm remapped by inverted Worley. Base frequency 4 per tile. |
| G | Inverted Worley fBm, base frequency 4 (3 octaves) |
| B | Inverted Worley fBm, base frequency 8 (3 octaves) |
| A | Inverted Worley fBm, base frequency 16 (3 octaves) |

**`ism_erode`**

| Item | Value |
|---|---|
| Size | 32³ |
| Format | `RGB8_UNORM` (or RGBA8 with A unused) |
| RGB | Inverted Worley fBm at base frequencies 8 / 16 / 32 per tile, for edge erosion. |

## T6 (optional). `curl_2d` — 2D curl for in-plane swirl

| Item | Value |
|---|---|
| Size | 512² |
| Format | `RG16F` |
| Content | 2D curl noise (divergence-free in the plane), periodic, RMS 1, base frequency 4. Used to swirl disc structure in the galaxy plane (cheaper than 3D for face-on views). |

---

## Budget

| Texture | Size | MB |
|---|---|---|
| web_cells (+ variant b) | 128³ RGBA16 ×2 | 32 |
| web_density | 128³ RGBA16F | 16 |
| warp_large | 128³ RGBA16F | 16 |
| warp_fine | 128³ RGBA16F (was 64³) | 16 |
| galaxy_detail | 128³ RGBA8 | 8 |
| ism_shape / ism_erode | 128³ + 128³ RGBA8 (erode was 32³) | 16 |
| curl_2d (optional) | 512² RG16F | 1 |
| **Total** | | **~83 MB** + ~14% for mips (51 MB without web_cells_b) |

## How the runtime will use them (for context; no action needed)

- **Web density** at world point p: q = R·(p + offset_seed)/P_web + a₁·warp_large(R₁·p/P₁) + a₂·warp_fine(R₂·p/P₂). Then density = floor + w_wall·exp(−(G/width)²) + w_fil·exp(−(B/width)²) + w_node·exp(−(A/width)²), and finally bias^power. That's three texture reads instead of up to five 27-cell searches.
- **Prefiltering:** near samples rebuild the web from `web_cells` (live widths); once the footprint covers a good part of a cell, they switch smoothly to `web_density` at the mip the footprint picks. That replaces today's blend to the mean.
- **Placement and rendering read the same textures.** Placement uses mip 0.
- **Periods** P₁ = 1.618·P_web and P₂ = 0.382·P_web, plus per-layer rotations, make the composite effectively aperiodic over any distance anyone can fly.

## Acceptance checks I'll run on delivery

1. Wrap continuity: the max difference across opposite faces is ≈ 0, at mip 0 and at a coarse mip (catches clamped mip filtering).
2. Isotropy: the axis power spectra agree within 5%.
3. Curl fields: divergence ≲ 1% of the gradient.
4. Sidecar stats match the data.
5. Regeneration from the sidecar seed and parameters is byte-identical.
