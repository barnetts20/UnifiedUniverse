# Volumetric density raymarching — design

*v1, 2026-11-01. Extends "Spec: rendering → Volumetrics" in `UnifiedUniverse_Design.md`. Decisions are D-128..D-135 in `DECISIONS.md`. Numbers marked (measured) come from `Tools/UniVerify` (`T3.vol.*`, all green); the rest are targets. Built offline on branch `uu/volumetrics`: the shared core `Shaders/Private/Core/UniVolume.ush`, the CPU builder `Source/UniCore/UniVolume.h` and the reference renderer `Tools/UniVerify/src/UniVolumeRef.h`. The engine pass is not written yet (UE-9).*

## 1. The idea in one paragraph

The volume pass draws **the light of everything the sprite pass does not draw**. Each emitter's field already says where its children are, and its luminosity function already says how bright they are, so the expected emission at any point is known exactly. A child star at distance t is a visible sprite with weight `F_L · fade(m)`, where F_L is its level's fade and fade(m) is the magnitude fade band. The volume carries the remainder, `1 − F_L · fade(m)`. Sprites plus volume therefore always add up to the population's total light, whatever the distance, sensitivity, level fade or velocity LOD. That one rule (D-128) makes every handoff seamless by construction:
- galaxy sprite → galaxy glow → resolved stars;
- a level fading in or out;
- a teleport while cells fill;
- a sensitivity scan.

Dust is the same field machinery with absorption instead of emission (D-132).

## 2. What the pass draws

| Term | Source | Has a sprite counterpart? |
| --- | --- | --- |
| Unresolved emission | Every emitter: field shape × N_total × the per-frame unresolved-luminosity LUT | Yes, so conservation applies (§3) |
| Extinction (dust) | An archetype's volume profile: a dust field aligned with its emission field | Attenuates both the volume and the sprites (§7) |
| Background (extragalactic light) | The root emitters (cosmic web → galaxies), with horizon dimming | Yes (galaxy sprites) |
| Gas emission, nebulae, scattering | Later: authored volume profiles without children | No; plain light (v2) |

## 3. Conservation (D-128)

For an emitter e with expected child count N, the slices s of its child archetype carry fractions P_s, magnitudes drawn exactly as placement draws them (`Uni_DrawAbsMag` over the slice's inverse CDF) and level fades F_{L(s)}. The expected flux per child that is **not** drawn as a sprite, seen at distance t, is:

```latex
\ell_e(t) = \sum_s P_s \, \Big( \bar{L}_s - F_{L(s)} \, G_s\big(m_{lim} - \mu(t)\big) \Big), \qquad
\bar{L}_s = \langle 10^{-0.4M} \rangle_s, \quad
G_s(x) = \langle 10^{-0.4M} \, \mathrm{sat}\big((x - M)/0.5\big) \rangle_s, \quad
\mu(t) = 5 \log_{10}\frac{t}{10\,\mathrm{pc}}
```

- **The tables.** `L̄_s` and `G_s` are static per definition (a 1D table over x). The per-frame LUT `ℓ_e(ln t)` (256 entries, RGB) is 256 × slices lookups on the CPU, about 6k operations. The colour per slice comes from `Uni_DrawColour` over the same quadrature, so the glow reddens nearby, where only faint red stars are unresolved, and is the full population colour far away. That is physical.
- **Emission density** at field point p of a carrier with radius R and normalising integral I (D-115, the same number generation uses):

```latex
j(p, t) = \frac{N \, (10\,\mathrm{pc})^2}{I \, R^3} \, \rho(p) \, \ell_e(t) \quad\text{[flux units per sr per cm]}, \qquad
B = \int j \, T \, dt, \qquad \text{pixel energy} = B \, \Omega_{px} \, 10^{0.4 m_{exp}}
```

- **Units.** Sprite energy is already `10^(-0.4(m − m_exp))` summed over its pixels, so both terms are in one set of exposure units (lesson #36). `Ω_px = cos³θ / f²` is per pixel, exact for the gnomonic projection.
- **Colour (D-135).** Sprite radiance is `colour / Y(colour) × intensity`, with Y the Rec. 709 luminance, and the tables use the same normalisation, so luminance is conserved exactly whatever the hue. Colour used to be normalised to its brightest channel, under which no channel of a sum is conserved.
- **What F_L must mean.** For the identity to hold in the engine, F_L must be the fraction of that level's sprites actually drawn. A level whose window is not yet filled (teleport, rebuild) must hold F_L = 0 until its cells within relevance are ready, then fade in (an amendment to D-116). A suspended level (velocity LOD) fades out, so the volume takes over. **The flythrough's near-black first quarter becomes the galaxy's glow.**
- **Suppressors** cut their sphere out of the background emission of the archetype they suppress, so no light comes from galaxies that do not exist.

**Measured (T3.vol.conservation):** over the whole sphere at the solar camera, volume + realised sprites = all-unresolved within −0.16%, −0.14% and −0.10% at Δm = −1, 0 and +2 (z < 1). Sprites carry 4%, 6% and 12% of the light there; the unresolved galaxy carries the rest.

**Measured consequence (demo content, T3.vol.budget):** 2e11 stars drawn from the placeholder stellar luminosity function sum to M = −23.18. The anchored galaxy's drawn magnitude is −21.0, a 2.18 mag (7.4×) mismatch. Conservation is only as good as this budget, so `FUniverseModel::Validate` should check it per archetype (D-133, **owner decision**: fix N_total, the stellar function or `NTotalRefMag`; every option rerolls star content).

## 4. Carriers (D-129)

A **carrier** is one marched volume: a contributor (the object that owns an emitter, here a galaxy) plus its frame, or the root background.
- **Built on the CPU.** It already has every resident contributor, with its exact centre (FCoord), rotation, rolled field parameters and cached integral. Each frame, a carrier is emitted for each contributor whose angular radius θ is past the handoff ramp start.
- **Handoff weight.** w = smoothstep(θ_v0, θ_v1, θ), defaults 4 → 8 px. Project-and-cull computes the same w and multiplies the object's own sprite by (1 − w). The carrier also takes the sprite's visibility factor V (D-121), so `sprite (1 − w) V + carrier w V` is conserved. Past θ_v1 the galaxy is purely a volume plus its resolved stars.
- **Cap.** 64 carriers per frame, ranked by θ. If more qualify, θ_v0 and θ_v1 are raised for that frame and uploaded, so the CPU and the GPU agree on who is a carrier. Overflow degrades to sprites and never loses light.
- **No tiling in v1.** With ≤ 64 carriers, every pixel loops over all of them with a ray-sphere reject (about 10 flops each). Tiles come later, if nebulae add hundreds.
- **Background carrier.** One per root emitter, always active, periodic (§5).
- **Carrier record** (GPU, 112 B, a multiple of 16 B, `static_assert`ed):

| Bytes | Fields |
| --- | --- |
| 0–15 | `float3 CamLocal` (camera in carrier-normalised coordinates), `float InvRadiusCm` |
| 16–63 | rotation rows `Rot0..Rot2` (the contributor's, identical to placement, lesson #1), each with one packed uint: field id + flags, field-param index, emission-LUT index |
| 64–79 | `float Amplitude` (N (10 pc)² / (I R²)), `float Weight` (w V F_L), `uint Flags`, `uint DustParamIndex` (dust σ lives in that block's D.y) |
| 80–95 | `float MeanDensity`, `float FeatureScale` (prefilter), `float TStart`, `float TEnd` |
| 96–111 | `float ResolveScale` (supersampling), padding |

## 5. Rays and precision

- **True distance.** The march runs in **true distance t** along the exact view direction, so radial compression never enters: f(r) changes neither the direction nor the integral. Scene depth is converted to true distance as in the sprite pass, and the march stops there.
- **Carrier-local position.** `p = CamLocal + (t · InvRadius) · d`. CamLocal is `Rot · (Camera − Centre) / R`, computed on the CPU from the exact 128-bit delta (D-104), so precision is relative to the carrier. The angular error of a sample is about 1e-7 rad whatever the distance (0.07 mpx at 1080p). Camera motion moves CamLocal in steps of 1e-7 · |CamLocal| · R; seen from distance D ≈ |CamLocal| · R, that is the same 1e-7 rad.
- **Periodic background.** `p = frac(CamP + t · d / Period)`, where CamP = (camera mod period) / period is reduced exactly in integers (lesson #11). The march ends at 100 r_H (≈ 4,500 periods), where float frac precision is still 3e-4 of a period, against features of 0.2 period.

## 6. Stepping (D-130)

1. **Log-uniform steps.** Step `s = max(k · t, s_min)` with k = 0.08: uniform in ln t, so the count is logarithmic in range (lesson #35: chord-based, with growth).
2. **Field step hint.** `Uni_FieldStepHint(fieldId, p, d)` sits next to `Uni_FieldSample` in `UniFields.ush`, so it is one source. It returns the largest safe step for that field at that point, from the length scale of each layer:
   - disc vertical: 0.25 h / |d_z| inside |z| < 4h, so a crossing takes ≥ 32 samples (the mid-plane kink needs them); outside the slab, it stops at the slab surface;
   - disc radial: 0.3 R_d;
   - arms: 0.25 rad of arm phase, with R floored at R_d / 2, because the phase winds without limit at the axis;
   - bulge: 0.3 of its log-slope length inside 6 r_e.

   The step is the minimum over the carriers active at t.
3. **Union of intervals.** Each active carrier contributes its sphere chord. The march visits the union of the chords and the background front to back, summing emission and extinction of every carrier active at each sample. Nested content (camera inside the galaxy, inside the web) is exact, not composited.
4. **Prefiltered sampling.** When the pixel's transverse footprint `t · θ_px` exceeds a field's feature scale λ, the sample blends toward the field's mean (1 → 4 λ). Analytic fields use their mean; baked fields would read the matching mip, since mips are integrals. The integral stays unbiased and does not alias. It must be the pixel footprint, not the step: the step is capped by the hint, so a step-based footprint never reached the blend (measured: 500 steps per background ray).
4b. **Supersampling small carriers.** A pixel whose centre ray hits a carrier whose finest feature (`Uni_FieldResolveScale`: min(h, r_e / 2) for galaxies) projects under half a pixel is marched with up to 8 × 8 sub-rays (`Uni_CarrierSubsamples`). With one ray per pixel, a galaxy at the handoff size mis-integrated its light by 12–17% (measured). Small carriers cover few pixels, so the cost is about 1 / (4 s²) rays per carrier whatever its distance.
5. **Start and tail.**
   - Start: local carriers start at max(chord entry, 1 pc). The background starts at 0.1 Mpc: nearer galaxies are resolved, and log steps from 1 pc cost about 350 per ray for about 2e-5 of the light.
   - Tail: the background marches to 0.5 periods (50 Mpc for the demo web), then adds an analytic tail ∫ ℓ D dt with mean density, which is a per-frame cumulative LUT. Marching 2 periods instead changes the all-sky light by < 0.01% (measured).
6. **Budget.** An explicit step budget (256 for the near layer). A march that hits it sets a truncation flag that the debug view shows and the stats count. A truncated march must never look finished (lesson #35).
7. **No jitter in v1.** The pass runs after TSR (§8), so nothing would denoise it. Banding is measured offline, and if it is visible, v1.1 adds the volume's own temporal accumulation (the volumetric-cloud technique).

## 7. Extinction and sprites (D-132)

- **Dust field.** The archetype's volume profile holds a dust field. For galaxies it is the galaxy field with a thinner disc (h_d = 0.5 h), arms phase-shifted toward the inner edge, and no bulge.
- **Normalisation and colour.** Normalised by τ⊥, the face-on optical depth at one scale length. Reddening is σ_RGB = σ · (0.8, 1.0, 1.3).
- **Volume self-absorption** is in the march.
- **Sprites behind dust: transmittance slices** (the design's v2, proposed for v1). The march writes T at 8 slices uniform in ln t (half resolution, R16F, 8 MB at 1080p). The sprite pixel shader reads T at its true distance with bilinear and log interpolation. This gives the Milky Way's dark rift over its own stars, and galaxies hidden behind the disc (the zone of avoidance). Conservation holds because both terms see the same T.
- **Known gap:** internal extinction makes a dusty galaxy's carrier dimmer than its sprite, by up to about 30% edge-on (to be measured). v1.1 computes the carrier's mean transmittance on the CPU (16 rays through the same core) for carriers in the handoff ramp and applies it to the sprite.

## 8. Engine pass (written on `uu/volumetrics`, not compiled: UE-9)

Every hook stays in the existing post-MotionBlur callback (`UniRenderHooks.cpp`); `FUniRenderer::RenderVolume_RenderThread` runs before the sprite draw.

1. **Game thread, `UUniSubsystem::PushFrame`.**
   - `FVolumeBuilder::Build(*Streamer, ...)` produces the carriers (≤ 64), parameter blocks (through `FFieldParams::Pack`, lesson #2) and LUTs, in one by-value payload (lesson #25).
   - The LUT is cached and rebuilt only when m_lim, m_exp or a slice level's fade changes.
   - The carriers' ThetaPixel is the last rendered frame's (one frame late, only for handoff weights).
   - A background version bump requests a re-march of the direction map (D-136).
2. **`Uni.Volume.Background`** (compute). Re-marches the dirty faces of the persistent 6 × 256² RGBA32F direction map, `uu.BackgroundFacesPerFrame` (2) per frame.
3. **`Uni.Volume.March`** (compute). Marches the local carriers at 1 / `uu.VolumeDivisor` (2) of the output resolution.
   - Outputs: RGBA32F (radiance, luminance T) and an 8-layer R16F slice array (luminance T).
   - Pixels on sub-pixel carriers are supersampled up to 8 × 8.
4. **`Uni.Volume.Composite`** (full-screen triangle, additive into the scene-colour copy).
   - Depth-aware bilinear upsample (log-distance weights).
   - Adds T_near × the background map.
   - Multiplies by the pixel's solid angle and SceneColourScale × PreExposure.
5. **Sprites** read the slice array at their true distance (luminance transmittance). With the volume off, a 1 × 1 dummy slice texture of 1s is bound.
6. **Console:** `uu.Volume 0|1`, `uu.VolumeDivisor`, `uu.BackgroundSize`, `uu.BackgroundFacesPerFrame`. `uu.Stats` logs carriers, faces marched and LUT rebuilds.

**Memory at 1080p, half resolution:** volume 8 MB, slices 8 MB, background map 6 MB. The volume and background targets are float32: radiance per steradian overflows half floats, at ~7e4 per sr for a galaxy disc at the default exposure.

**Accepted GPU/CPU differences:**
- The engine stores luminance-only near transmittance (for the background) and luminance-only slices (for sprites). The reference uses RGB. They differ only in dust reddening of the background and of stars behind dust.

**Budget target:** ≤ 1.5 ms at 1080p for one galaxy carrier plus the background (UE-8).

**v1.1:**
- temporal accumulation and the near/far split for the local march;
- sprite internal-extinction correction;
- suppressors cutting the background;
- moving the LUT build to a worker thread.

## 9. Verification (Tools/UniVerify, Tier 3): measured

| Check | Claim | Measured |
| --- | --- | --- |
| `T3.vol.march` | The adaptive march matches a brute-force fine march (400k uniform + 0.002 log steps, no hints). Cases: outside face-on, edge-on, inside in-plane and out of plane, with dust | Worst emission 0.80%, τ 0.0013; 85 steps per ray mean, 300 max; 0 truncated |
| `T3.vol.conservation` | Volume + realised sprites = all-unresolved over the whole sphere at the solar camera, Δm = −1, 0, +2 | −0.16%, −0.14%, −0.10% (z −0.89, −0.74, −0.53) |
| `T3.vol.handoff` | Budget-consistent galaxy from 6 Mpc to 300 kpc (640 × 360): in-frame luminosity × d² constant | Within 0.84%; the sprite's share goes 100% → 0% |
| `T3.vol.nested` | Camera inside the galaxy, inside the web: union = galaxy + background | Exact (0.000%) |
| `T3.vol.precision` | The same scene at the origin and ~2^126 cm away; 0.001 cm camera steps | Bit-identical; 0 change |
| `T3.vol.background` | Finite (horizon dimming), converged, not truncated | 27.43 mag/arcsec² (analytic estimate 27.43); 0.5 → 2 periods +0.00%; 107 steps per ray |
| `T3.vol.budget` | Demo luminosity budget, reported | Children M = −23.18 vs drawn −21.00 |
| `T3.vol.frames` | Stills: face-on 60 kpc, edge-on 30 kpc, interior ×2 | 16 / 45 / 175 / 136 steps per ray; volume carries 100 / 100 / 96 / 70% of the light |
| `T3.vol.flythrough` | 48-shot contact sheet of the T3.flythrough path with the volume | 0 truncated; 124 steps per ray; the galaxy shows as glow in the shots where its star levels are suspended |
| `T3.vol.cube` | Background direction map (6 × 128²) + separable composite vs the union march (D-136) | Total 0.000%, 99th-percentile pixel ≤ 0.02%; 107 steps per texel |
| `T2.stream.volume_parity` | The streamer's volume scene equals the reference pipeline's; filling levels hold their fade at 0 (D-128) | Byte-identical carriers and parameters, identical LUTs; 0 violations |

## 10. Decisions (provisional unless marked)

| ID | Decision | Reroll |
| --- | --- | --- |
| D-128 | The volume draws the complement of the drawn sprites, per emitter; F_L means "fraction drawn" (amends D-116 for fills) | None |
| D-129 | CPU-built carriers from contributors, ≤ 64, handoff ramp 4 → 8 px with a shared w; no tiling in v1 | None |
| D-130 | Log-uniform steps (k = 0.08) with per-field step hints, union of intervals, pixel-footprint prefiltering, supersampling of sub-pixel carriers, explicit budget, no jitter in v1 | None |
| D-131 | Background horizon dimming D(t) = (1 + t / r_H)^-2, r_H = c / H₀ = 1.37e28 cm, applied to galaxy sprites too (render only). Without it the background diverges (Olbers): the torus is 10^10 times the mean free path to a galaxy | None (**owner**: value) |
| D-132 | Dust profile per archetype; transmittance slices in v1 so sprites are attenuated | None (**owner**: v1 or v2) |
| D-133 | `Validate` checks each archetype's luminosity budget: \|M_parent − M(Σ children)\| < 0.1 mag | **HIGH** to fix the demo (**owner**) |
| D-134 | The carrier normalising integral: a 64k-sample estimate until D-123's full integral is cached (about 1% flux noise at most) | None |
| D-135 | Sprite colour normalised by luminance (Rec. 709), not by its brightest channel, so light sums conserve | None |
| D-136 | Background marched into a direction map from 0.1 Mpc; composite B_near + T_near × B_map | None |

## 11. Build plan

1. **Offline core (done, `uu/volumetrics`).**
   - `Shaders/Private/Core/UniVolume.ush`: the carrier, LUT sampling, the march, subsampling.
   - Step hints in `UniFields.ush`.
   - `Source/UniCore/UniVolume.h`: the tables, the per-frame LUT, the carrier builder, the budget validator.
   - The reference renderer and the §9 checks and images.
2. **Engine (written, uncompiled):** `UniVolume.usf`, the passes above, the sprite transmittance lookup, `uu.Volume`. Still to do: a debug view (density / steps / truncation, lesson #37).
3. **v1.1:** temporal accumulation and the near/far split; sprite internal-extinction correction.
4. **v2:** gas emission, nebula profiles, single scattering, baked volumes with mips.

## 12. Performance controls as of D-200

The march's cost is rays x steps per ray x cost per step. These are the console settings that move each term, with their defaults and the decision behind each. The offline numbers are T2.vol.perf_inside (inside the Milky Way, steps per pixel toward the centre / along the disc / out of the plane), against 198.5 / 158.4 / 134.3 at the D-130 settings.

**Rays.**
| Setting | Default | What it does | Decision |
|---|---|---|---|
| `uu.VolumeDivisor` | 2 | the march at output / divisor | D-129 |
| `uu.VolumeMaxMarchPixels` | 262144 | at most this many march pixels; half the rays on a 1880 x 1120 viewport. 0: no limit | D-196 |
| `uu.VolumeTravelPixelScale` | 0.35 | the pixel budget at full travel speed (10 Mpc/s), in quarter steps from 1 at 100 kpc/s | D-198 |
| `uu.VolumeTargetFps` | 60 | the pixel budget steps down (7 levels, to x0.35) while the GPU frame is over this frame rate, and back up with headroom; `uu.VolumeAutoHold` (20) frames between changes. 0: off | D-200 |
| `uu.VolumeTemporal` | 1 | march one pixel of every 2 x 2 a frame and reconstruct the rest | D-184, D-194 |
| `uu.BackgroundMode` | 2 | the background from its direction map, marched per pixel only while travelling fast | D-166 |
| `uu.BackgroundPerPixelInterval` | 0 (automatic) | how often the background must change to be marched per pixel | D-196 |

**Steps per ray.**
| Setting | Default | What it does | Decision |
|---|---|---|---|
| `uu.VolumeStepRatio` | 0.32 | log step ratio. With the defaults below: 52.8 / 38.9 / 30.7 steps/px, p95 error 0.40-0.61% | D-195, D-199 |
| `uu.VolumeHintScale` | 3 | the field step hints x this, in carriers around the camera only (from outside, x 2 already costs 5% per ray) | D-195, D-198 |
| `uu.VolumeStartPc` | 10 | local carriers start this far out | D-195 |
| `uu.VolumeWebStepRatio` | 0.4 | the textured web's log step ratio (the background): 40 steps a texel | D-199 |
| `uu.VolumeWebTravelHint` | 2 | the background web's step hints x this at full travel speed (lerped by the travel amount): 40 -> 27 steps a texel. 1: off | D-202 |
| `uu.VolumeMaxSteps` | 512 | the step budget per ray | D-130 |

**Cost per step.** With one carrier active (inside a galaxy), the CPU reference spends 306 ns on the dusty galaxy sample (emission + dust, D-181), 90 ns on its step hint and 9 ns on the LUT. The sample is the term left to attack (B-017).

**Entities** (fewer sprites, cells and carriers; their light is kept as glow).
| Setting | Default | What it does | Decision |
|---|---|---|---|
| `uu.Star.Keep` | 0.35 | share of every galaxy's stars generated and drawn | D-197, D-198, D-205 |
| `uu.Cluster.MemberKeep` | 0.5 | share of cluster members and group dwarfs generated and drawn | D-197, D-198 |

**Loading and speed** (D-203: work per frame no longer grows with speed; what has not loaded is glow).
| Setting | Default | What it does | Decision |
|---|---|---|---|
| `uu.CellGlow` | 1 | a cell still loading (or fading in) is drawn as glow from the field; its sprites fade in once it is Ready (section 13). 0: a hole until it loads (before D-203) | D-203 |
| `uu.GalaxyMap` | 1 | the galaxy the camera is in is marched from its baked 2D map (arms and modes; warp, vertical profile, bulge and detail per sample; D-212). 0: its field at every step | D-212 |
| `uu.GalaxyMap.Size` | 1024 | the map's texels a side (power of two): 1024 ~11 MB (one frame to bake), 2048 ~43 MB (no visible difference in the engine) | D-212 |
| `uu.GalaxyMap.Budget` | 1048576 | texels of the map the GPU bakes a frame (1024^2 in one frame, 2048^2 in 4) | D-212 |
| `uu.GalaxyMap.Slots` | 4 | galaxy maps on the GPU at once (a texture array): every galaxy stars are placed in (D-214: placement reads the map too) and the camera's own; a galaxy past them waits as glow until a slot is unused for 64 frames | D-214 |
| `uu.CellFadeFrames` | 8 | frames a Ready cell's sprites take to fade in (its glow fades out by the same amount) | D-203 |
| `uu.StreamBuildBudget` | 96 | cells built per frame over every level, those needed soonest first; the rest wait as glow. 0: D-173's per-level rule | D-203 |
| `uu.VolumeInterleave` | 4 | most pixels per side of the temporal slicing's block: 4 x 4 below 0.1 march px of motion a frame, 3 x 3 below 0.5, else 2 x 2 | D-208 |
| `uu.TravelExposure` | 1.5 | magnitudes everything is brightened by at full travel speed (render only; windows unchanged) | D-205 |

**Look** (cost-neutral).
| Setting | Default | What it does | Decision |
|---|---|---|---|
| `uu.Galaxy.DustScale` | 2 | galaxies' dust density multiplier | D-198 |
| `uu.Galaxy.DustLanes` | 3.0 | textured lanes' contrast | D-198 |

**Diagnostics.**
- ProfileGPU shows Uni.Volume.March (or .March+Background), .Reconstruct, .Composite and .BackgroundMap.
- `uu.perf` prints the march size, rays, slicing and the frames with the background per pixel.

## 13. Cell glow (D-203)
A cell that is not Ready draws no sprites, and one that has just become Ready fades its sprites in over `uu.CellFadeFrames`. Before D-203 that light was missing until the cell loaded (pop-in), so windows stretched ahead at speed to load cells before they were seen (D-139, D-162), and a level's fade waited for its visible cells (D-128's hold).

Now the march draws what a cell does not: for each carrier, the LUT holds, after its emission and tail blocks, one block per child level q of the light drawn as sprites at that level, D_q(t) = sum over the level's slices of P keep F_L G (the term the emission block subtracts for it). Each frame the streamer lists the levels with a cell not fully drawn and their cells' fades over the window (`FUniStreamer::CellGlow`); the builder appends those grids to the LUT buffer (`FVolumeViewGPU::GlowMask`, `GlowBase`). A sample at distance t adds (1 - cell fade) D_q(t) for the cell of each loading level q that holds it (`Uni_CellGlow`). Sprites + glow stay the population's light whatever has loaded, and a loading cell reads as glow condensing into points.

- Exact: every cell of a level at fade 0 is the level's fade at 0, and per-cell fades add linearly (T2.vol.cell_glow_exact).
- Cost: nothing when no cell is loading (GlowLevels 0); otherwise, per step and active carrier, a grid read for each loading level of its children whose windows can reach the sample (D-205: levels with 2^L < t / 18 are skipped by a mask; in a galaxy core at speed +6% -> +0.5% on the march).
- Not in the background map: it is kept across frames while cells load, so their glow would stay after their sprites arrive. Per pixel (while travelling fast) it is in.
- Local carriers start at 10 pc (D-195): a loading cell's stars within 10 pc are still missing until it loads, as before.

