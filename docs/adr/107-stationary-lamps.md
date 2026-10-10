---
status: declined
updated: 2026-10-10
authority: adr
---

# ADR-107: Stationary lamps

## Status

Declined. Stationary lamps shipped in d4d922b1 and were removed the same
day after the owner's review; [ADR-108](108-baked-lamp-direct-pages.md)
replaces them. This record keeps the measured reasons, so the design is not
tried again without addressing them.

## Context

On the tiled pipeline every shadow of the Testbed project's "Level Design
Test" scene came from static lamps baked at 8 texels per metre, so lamp
shadows were blurred and stepped at every Shadow quality (user report,
2026-10-10). The owner first chose stationary lamps, as Unreal Engine has
them, over a sharper bake.

## Decision (removed)

A `stationary` point or spot lamp baked its bounce and a four-channel shadow
mask; the runtime shaded its direct light. Channels went to lamps by a
greedy colouring over lamps that see each other; at each point a channel
belonged to the candidate of that channel with the largest
`weight × window / d²`, and the other lamps of that channel baked there as
static lamps. The nearest 6 lamps (3 at Balanced) took runtime shadow maps,
the others the mask. VKLM v5 carried the lamp records, per-instance
candidates and the mask plane.

## Why it was removed

Owner review on the M1 Pro, Epic, 2026-10-10:

- **Rings and triangles.** Where a channel's owner changed, a lamp's light
  switched between the runtime path (full BRDF with specular, shadow map or
  mask) and its baked Lambertian light. The ownership contours are spheres
  of equal metric, so the switch showed as circles, arcs and straight
  diagonals on walls and floors. A debug view that coloured pixels by their
  owners reproduced the shapes of the owner's screenshots exactly.
- **Pixelation on Epic.** Only the 6 nearest lamps took shadow maps; the
  others still shaded through the 12.5 cm mask, with bilinear filtering only.
- **Cost.** In the headless editor's Scene view at Epic, `Tiled.Opaque` took
  2.74 ms with stationary lamps against 1.10 ms with the same frames'
  stationary shading left out in the lobby, and 1.98 against 0.82 ms in the
  cafeteria (`VKR_RG_GPU_TIMING=1`, median of 40 frames). The per-pixel
  candidate ranking, up to four BRDF evaluations and shadow taps doubled the
  pass; the owner saw the frame rate fall from about 120 to 55.

## Alternatives considered

The owner then chose a baked direct plane at a higher density
([ADR-108](108-baked-lamp-direct-pages.md)) over a baked distance-field mask
and over baking the whole lightmap at 16 texels per metre.

## Revisit when

A lamp must change at runtime while keeping baked bounce. A new design must
then avoid per-point switching between baked and runtime light, for example
by fixing each surface's runtime lamps for the whole surface, and must
measure its `Tiled.Opaque` cost against the baked frame.
