# Warp travel: speed effects that stay navigable and hide pop-in (SPRITE_PASS_PLAN §3 E)

*Draft, 2026-11-04, for the owner's review. **Built 2026-11-05 with the recommended defaults, each behind a switch:** E1 and E2 (D-187), E3 (D-190), E4 motes (D-192; the peripheral glow is not built, BACKLOG B-014). The questions in section 5 can still change any of them. Owner's brief: speed should read as speed while the view stays navigable; relativistic shifts only as far as they help; the effects should hide streaming artefacts such as pop-in.*

## 1. What it has to do

- **Read as speed.** At intergalactic speed today the sky slides past; nothing says "fast" except the travel sensitivity's brightening (D-154).
- **Stay navigable.** The direction of travel stays where you are looking, targets do not jump, and turning still works. A real 0.99c view (the whole sky crammed into a few degrees ahead, everything blue-white) is not navigable, so every effect is driven by a stylised speed, capped.
- **Hide pop-in.** At speed, cells arrive late: D-173 already knows which ones, and the outer shell of a widened window is always filling. Objects there should arrive as something that reads as motion (a streak sweeping in from ahead), not appear.
- **Stay cheap and render-only.** Nothing here changes what is generated; no new history per object.

## 2. One speed for every effect

A warp amount `w` in [0, 1] from the camera's speed, the same curve as travel sensitivity (D-154): 0 below 100 kpc/s, 1 at 10 Mpc/s and above, smoothstep in log speed. It is rate-limited the same way: up 2/s, down 1/s. Jumps don't count. Every effect below is a function of `w`, so stopping removes them over a second and the view settles back to physical.

An alternative is to key each effect to the speed relative to the level the camera is in (cells crossed per second). That reads better inside a galaxy, where "fast" is pc/s. **Owner question 1** (recommended: the travel curve, plus a second curve for in-galaxy speeds if that feels flat).

## 3. Effects, in build order

### E1. Streaks (sprites)

- Each sprite is stretched along its screen motion over the last frame, from its previous direction. That needs no per-object history: the object is static, so its previous direction is `normalize(RenderPos + CameraStep)`, seen through the previous frame's view basis. D-184 already carries both to the renderer.
- The quad becomes a capsule from the previous to the current position, with the sprite's profile convolved along it.
- **Energy:** conserved by default, spread over the tail; a gain is optional (owner question 4).
- **Length:** the motion times an exposure factor. It ramps with `w`, so at low speed there are no streaks, and it is capped, so a star crossing the view in a frame does not paint the whole screen.
- **Cost:** only moving bright sprites grow their quads.
- **Offline checks:** energy kept, the streak direction is the motion's, zero at rest, and a contact sheet of a scripted flight.

### E2. Arrival as motion (the pop-in camouflage)

- A cell that becomes Ready while objects in it are already inside the visible radius (a late cell; the streamer can flag this exactly, D-139/D-173) fades its objects in over 4–8 frames.
- While they fade in, their streaks are drawn long, starting from the direction of travel. A late object reads as streaming in from ahead.
- The same ramp covers level fades at speed (D-116).
- **Plumbing:** one byte per cell for the age since it became Ready, beside the level fade the project pass already reads.
- **Offline check:** over the flythrough at 100 Mpc/s, no object reaches full brightness on its first frame (today's T3 pop count, extended to late cells).

### E3. Stylised aberration and Doppler

- **Aberration:** apply the relativistic angle remap with an effective speed `beta = BetaMax * w`, capped (**owner question 2**; recommended BetaMax 0.5, which compresses the forward cone by about 1.7×).
  - The view direction is a fixed point, so what is ahead stays ahead and steering keeps working.
  - Applied consistently: sprite directions in the project pass, and the volume's and background's ray directions inverted per pixel. It is one formula either way.
- **Doppler colour:** blue ahead, red behind, as a colour-temperature scale on sprite colours and a per-pixel tint of the volume by direction.
  - Strength from `w`, capped at about a 1.5× temperature ratio.
  - Brightness beaming is optional and mild, D¹ rather than D⁴ (**owner question 3**).
  - The volume tint is luminance-kept, like the palettes (D-182).
- **Offline checks:**
  - remap and inverse agree (1e-6);
  - the view centre does not move;
  - total light is unchanged with beaming off;
  - stills at `w` = 0, 0.5 and 1.

### E4. Ambience in empty space (optional)

- In a void the sky barely moves even at 30 Mpc/s, so nothing says "fast". A decorative field of motes in camera-local space adds that: hashed points on log-spaced shells, drawn as short streaks with E1's code, faded by `w`.
- It is purely decorative and drawn in the sprite pass.
- A slight brightening at the edges of the view hides what pops in there (windows lead forward, so peripheral cells are the late ones).
- **Owner question 5:** motes yes/no, and the vignette.

## 4. Order and switches

1. E1 streaks, then E2 arrival: the pop-in camouflage the owner asked for, and both are sprite-pass only.
2. E3 aberration and Doppler: touches the project pass, the volume and the background.
3. E4 ambience, if wanted after E1–E3 are seen in the engine.

Each effect gets its own CVar, with `uu.Warp` for all of them (0 = physical) and `uu.Warp.<Effect>` for each. Each step ends as the others do: offline checks, stills, a REVIEW_TONIGHT section, then the owner's engine pass.

## 5. Owner questions

*Unanswered at build time; the defaults taken: 1 the travel curve only; 2 BetaMax 0.5 (`uu.Warp.Aberration`); 3 colour only (`uu.Warp.Beaming` 0); 4 light-conserving; 5 motes yes (`uu.Warp.Motes`), no peripheral glow.*

1. Speed curve: the travel-sensitivity curve (100 kpc/s → 10 Mpc/s) for everything, or also a per-level curve for in-galaxy speeds?
2. Aberration cap: BetaMax 0.5 (forward cone ~1.7× tighter at full warp), stronger, or none?
3. Doppler: colour only, or mild brightness beaming too?
4. Streaks: light-conserving (default), or extra glow at speed?
5. Void ambience: motes and a peripheral glow, or nothing decorative?
