# Visual inspection — reference renderer

Written by the session after looking at every image in `images/` (2026-11-01). These are judgements, not checks. The checks are in REPORT.md.

## still_galaxy_towards_centre.png
Camera 8 kpc from the anchored Milky-Way-like galaxy's centre, 20 pc above the disc, looking towards the centre. A clear horizontal band of dense, faint, bluish-white stars runs across the frame along the disc plane, with field stars above and below. It reads as the Milky Way seen from inside a disc: the structure is right. About 9,900 stars are visible. That is above the real naked-eye sky (about 9,000 over the whole sky), because the placeholder luminosity function and the 2e11-star normalisation are untuned (D-014). The spiral arms are not legible from inside, as expected.

## still_galaxy_out_of_plane.png
Same position, looking out of the plane. A sparse, uniform field of about 2,000 stars with a few bright ones. Correct contrast with the in-plane view.

## still_web_sensitive_dm5.png
Intergalactic camera at Δm = +5. Sixty-five galaxies show as faint points. The cosmic web's filaments are **not** visually legible from 65 points, so this image does not demonstrate structure. Web placement is verified numerically instead: T2.gen.fidelity correlates placed counts with the web field at r = 0.98. A wider field, or deeper sensitivity with more resident galaxies, would show it. Galaxies render as points because none is resolved (θ < θ_min) at these distances; extended sprites are exercised only by unit logic today.

## flythrough_contact_sheet.png (48 frames, 3 Gpc → 1 m from a star's surface)
- **Frames 00–09** (3 Gpc → 34 kpc) are nearly black, with 0–14 galaxies visible. Velocity LOD suspends every level whose cells the camera crosses faster than once per 4 frames, and at this speed (2.5% of the remaining distance per frame) that is almost all of them. This is the designed behaviour, but it means the far half of the dive shows little.
- **Frames 10–15** (9.6 kpc → 52 ly): the galaxy's stars fade in level by level as the camera slows relative to each level's cells. No frame shows a star appearing at full brightness; T3.flythrough counts 0 pops over 545,902 entering objects. The field gets denser and brighter towards the target, as expected.
- **Frames 16–25** (14 ly → 36 AU): the target star (M = −4.4) brightens and its sprite's glare grows until it covers about a fifth of the frame. Background stars stay put, with no jitter visible between frames.
- **Frame 25 → 26: a visible discontinuity at promotion.** At about 3 AU the star's angular radius passes θ_promote = 0.02 rad. Its sprite is replaced by the true-scale body, which in the reference renderer is a small flat disc without glare, so the large glare vanishes in one frame. This is a handoff appearance problem (lesson #31: a promoted actor must inherit its sprite's appearance), not a position or identity problem. In the engine, bloom on the promoted body should supply the glare. This **needs checking in UE** (REVIEW_TONIGHT UE-5).
- **Frames 26–29**: the promoted body grows at true scale. Frames 30–47 (from 2.5e6 km down to 1 m) are filled by the star's surface, which is correct for a camera facing it.
- **A reference-renderer artifact, now fixed:** the first version truncated very bright sprites' kernels at 48 px, which drew them as squares. Kernels now extend until the tail falls below visibility.

## Volume pass (branch uu/volumetrics, Docs/VOLUMETRICS.md)
The `still_vol_*` stills use a display-only auto exposure: the 99.5th-percentile pixel maps to 1.5 before the tonemap. The gain is in each HUD. The demo galaxy is 7.4× too bright for its magnitude (D-133), so fixed exposure saturates the interior.

### still_vol_galaxy_faceon_60kpc.png
The anchored galaxy seen face-on from 60 kpc, entirely as volume (one carrier, 16 steps per ray). The two-armed log spiral, the exponential disc and the bright bulge all read correctly. The arms are very regular, because the analytic stand-in field has no noise (D-012). The dust is invisible face-on at this resolution, which is expected with a face-on τ of 0.6 spread over a thin layer.

### still_vol_galaxy_edgeon_30kpc.png
Edge-on from 30 kpc, 1° above the plane: a thin, bright disc with a central bulge. The dust lane is **barely legible**: there is at most a faint mid-plane darkening near the bulge. The 150 pc dust layer is about 2 px here, and the bright bulge sits behind it. At 60 kpc it was narrower than a pixel, which is physically right. A stronger lane (τ, or a thicker or offset dust layer) is an art-direction choice for later. The interior view proves the extinction path works.

### still_vol_interior_towards_centre.png
The solar camera, towards the centre. The diffuse Milky Way band is the unresolved light of the stars beyond the sprite limit (96% of the light; the 9,918 sprites carry 4%). It brightens towards the bulge, and **a dark dust lane runs along the band's middle**: the transmittance works for the glow and for sprites behind it. This is the image the design was after.

### still_vol_interior_out_of_plane.png
Looking out of the plane: a smooth, faint glow with the resolved field stars over it. Sprites carry 30% of the light here, against 4% in-plane, which is the right contrast.

### flythrough_volume_contact_sheet.png
The T3.flythrough path with the volume, using the budget-corrected model.
- **Frames 08–10** (123 → 9.6 kpc): the galaxy's spiral is visible from above as glow. In the sprite-only sheet these frames are nearly black, because velocity LOD suspends the star levels there. That is D-128 working: suspended sprites hand their light to the volume.
- **Frames 11–15:** the camera enters the disc. The glow becomes the interior band and the resolved stars take over smoothly; the volume's share falls 98% → 37%.
- **Frames 15–25:** the target star's growing white disc is its sprite glare, as in the sprite-only sheet (UE-7), not the volume. Frames 30–47 are the star's surface.
- **Frames 00–06** (3 Gpc → 1.6 Mpc): still dark. The galaxy is a faint point or a sub-pixel carrier, and the extragalactic background (27.4 mag/arcsec²) is below the display range at this exposure, which is physically right.
