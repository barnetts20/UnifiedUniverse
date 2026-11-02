# Backlog

*Ideas and deferred work not yet scheduled. Each item says where it would land and what it depends on. Decisions still go to `DECISIONS.md` once taken.*

## B-001: Warp streaks from late-loading stars (owner idea, 2026-11-01)

**Milestone:** sprite work (better stand-ins for star and galaxy sprites). Render-only (lesson #12): no content change, no reroll.

**Problem it turns around.** At high speed a cell can become ready after its objects should already have faded in: teleport fill latency, velocity-LOD resumes, or GPU generation simply lagging the camera. Those stars appear at full brightness, which is a pop. D-116/D-118 remove pops at normal speeds, and on the volumetrics branch D-128 carries the missing light as glow, but a late load still pops.

**Idea.** When an object spawns already visible, give it a short-lived ghost trail instead:
- The trail runs back to where its fade-in would have started: the point on the camera's recent path where the object crossed the visibility limit.
- Along the trail, brightness ramps from zero to the current value and size from the minimum footprint to the current one.
- The trail then collapses over a short duration (≈0.1–0.3 s), retracting into the current position.
- At warp speeds this gives the classic streaking stars, so a load-speed weakness becomes a visual gain.
- Objects that spawn just below visibility fade in normally, as now.

**Doppler tint.** Tint the streaks by line-of-sight velocity: blueshift ahead, redshift behind.
- Use the relativistic factor for camera speed v and the angle to the direction of travel, D = 1 / (γ (1 − β cos θ)).
- Shift the sprite's colour (its blackbody temperature, T' = D · T) and optionally its intensity (D³–D⁴ for bolometric beaming).
- At v ≪ c, scale the tint artistically from the camera's speed relative to the level's cell crossing rate, so it reads at any warp speed.
- Applying it to all sprites, not just streaks, gives aberration and beaming for free; decide per art direction.

**How it fits what exists.**
- *Detection:* the cell table can carry each cell's ready frame. Project-and-cull already computes every object's intensity; an object in a cell that became ready this frame, with intensity above the just-visible threshold, is a streak candidate. That is the same test as T3.flythrough's pop count.
- *Tail position:*
  1. Compute r_fade = 10 pc · 10^((m_lim − M) / 5) (the fade-band start).
  2. Find the camera position, from a short ring of recent exact camera coordinates, at which the object was r_fade away.
  3. Project the object from that position; that point is the tail.
  4. Exact deltas (D-104) keep the projection stable at any coordinate.
- *Drawing:* a tapered, oriented sprite quad from tail to head, with the intensity and size ramp along it and the collapse driven by age since ready. Additive like the other sprites, and depth-tested the same way (D-120).
- *Interplay:* with the volume on, a late cell's light is already carried as glow until its sprites arrive (D-128). The streak should take its energy from the same budget, so the sum stays conserved, or be explicitly decorative (decide).
- *Verification:* an offline T3 check. Streak candidates in the flythrough match the pop counter's would-be pops. Tails land on the camera path within a pixel. No streak at normal speeds (0 candidates with D-118).
