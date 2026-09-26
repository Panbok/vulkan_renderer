---
status: proposed
updated: 2026-09-26
authority: proposal
---
# Lighting efficiency

## Current baseline

`Lighting.Deferred` evaluates the sky, image-based and probe lighting, the
directional cascades, and every punctual light in the pixel's world-grid cell
([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)). A base kernel
without the clearcoat, sheen and anisotropy paths shades every tile unless the
frame has material planes; then a layered kernel shades the tiles that use them
([ADR-062](../adr/062-layered-clearcoat.md)). That split was the first step of
this proposal: in the table's configuration it lowered `Lighting.Deferred`
from 9.71 ms to 7.09 ms and the frame from 20.92 ms to 18.30 ms. The second
step stopped Metal from resetting and encoding culling commands for cached
local-shadow faces ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)):
`Cull.Encode` fell from 0.96 ms to 0.16 ms and the GPU pass sum to 14.25 ms,
but the frame stayed at 18.24 ms. Two things explain the difference. Pass
timestamps cost about 1.5 ms: without them the same case runs at 16.76 ms.
A Metal System Trace of the steady case (328 frames, 17.2 ms each) shows the
GPU busy 87% of the time, about 15.0 ms per frame. The remaining 2.2 ms per
frame was idle time at about 86 encoder boundaries, each 25 to 50 µs, spread
evenly across passes, because the Metal backend then recorded every compute
graph pass in its own encoder. CPU work is about 2.5 ms of the frame; the rest
of `cpu.render_prepare` waits for a command slot, so the frame is GPU-bound. Point lights come
from a camera-independent world grid of at most 384 cells. Local shadows use a
nine-tap PCF that reprojects taps leaving the receiver's cube face, plus
transmission prefix lookups when refractive casters exist. Deferred lighting
reads that visibility from the full-resolution `Shadow.LocalMask` array, which
filters each shadowed light once per pixel ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)).

Measured on 2026-09-26, before the kernel split: Metal Release, Apple M1 Pro, case
`bistro_native_perf_audit_steady` (Bistro street view, 1280x720), profile
`local-offscreen-perf-audit-gpu`, five children of 300 frames each, run back to
back. These runs are local and non-authoritative. The ablations used temporary
diagnostic switches that are not in the tree.

| Configuration | Lighting.Deferred | Cull.Encode | GPU pass sum | Frame |
|---|---:|---:|---:|---:|
| High preset, three shadowed local lights | 9.71 ms | 0.97 ms | 17.70 ms | 20.92 ms |
| Local shadows off | 6.04 | 0.16 | 12.83 | 16.02 |
| Point lights off | 3.38 | 0.16 | 9.93 | 12.93 |
| World grid with 32,256 cells of at least 1 m | 9.58 | 0.97 | 17.55 | 20.81 |
| Clearcoat and sheen compiled out | 7.37 | 0.97 | 15.35 | 18.59 |

This shows the following:

- Local shadows cost about 4.9 ms of the frame. Of that, 3.7 ms is PCF
  sampling in deferred lighting, 0.8 ms is GPU culling and command encoding
  for shadow faces that are cached and not redrawn, and the rest is
  transmission shading.
- Unshadowed point lights cost 2.7 ms. Culling precision is not the limit: a
  grid 89 times finer saved 0.13 ms. Bistro's 7.5 to 15 m ranges overlap
  genuinely, with up to 28 lights reaching one 1 m cell.
- The uber-kernel's clearcoat and sheen paths cost 2.35 ms (24%) on pixels
  that use neither. With those paths compiled out, the 14 Bistro snapshot
  views matched same-code captures within run-to-run edge noise. The cost is
  register pressure, not work.

## Proposed change

In order of measured payoff:

0. **Fewer passes in multi-pass chains.** Metal now records consecutive
   compute passes in one encoder ([ADR-025](../adr/025-selected-renderer-implementation-strategy.md)),
   which removed most of the 2.2 ms of encoder-boundary idle time: the steady
   case fell from 16.54 ms to 15.31 ms per frame without timestamps. Chains
   such as the HZB mips and per-layer transmission compaction still pay a
   barrier and a dispatch per step and are candidates for single-pass kernels.
1. **Cheaper local-shadow sampling**, from the 3.7 ms pool. Two steps have
   shipped. Taps that stay in the centre face skip reprojection, which lowered
   `Lighting.Deferred` from 7.11 ms to 6.71 ms in a shorter steady case. The
   shadow mask pass then moved filtering out of the lighting kernels:
   lighting fell to 3.3 ms for a 2.3 ms mask pass, and the GPU pass sum from
   14.02 ms to 12.89 ms, with output equal within 8-bit quantization. Rejected
   with measurements: a five-tap early exit (no gain), skipping back-facing
   lookups (no gain), and a half-resolution mask with depth- and
   normal-aware upsampling. The half-resolution mask saved 0.7 ms but
   softened sharp shadows on thin geometry next to lights; the inline
   fallback that restores them made it slower than the full-resolution mask.
   A five-tap PCF would save about 1.3 ms of inline cost, with 1.5 to 6% of
   pixels changing; it needs an owner quality decision. Four rotated taps
   integrated by TAA remain an option with the same decision boundary.
2. **Contact shadows** have shipped in the mask pass: an eight-step
   depth-buffer march of up to 0.25 m per shadowed light, for about 0.85 ms
   ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)). They recover
   contacts that the 512-squared maps lose; whether they allow smaller maps
   is untested.
3. **Screen-space-sized shadow atlas.** The single-layer atlas has shipped
   ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)): faces of 128
   to 1024 texels follow each light's size on screen. In the Bistro street
   view it costs about 0.2 ms more `Shadow.LocalMask` sampling and matches
   the accepted snapshot. Separate static and dynamic layers remain: static
   casters would render once and only dynamic casters would redraw into a
   copy of the static square. Bistro has no dynamic shadow casters (all 2,909
   candidates are static), so that step needs a scene with moving casters to
   measure.
4. **Half-precision BRDF terms** in the per-light loop, keeping positions,
   depths and normalisation in 32-bit. This applies to both backends and
   needs a numeric comparison against the full-precision loop.
5. **Deferred for Bistro-class scenes: camera-space clustered culling.** It
   remains useful above the 128-light table or for scenes with short-range
   lights, but it does not pay off here.

Longer term, stationary-light shadowing (baked static visibility, with runtime
maps only for dynamic casters) or ray-traced many-light sampling would remove
the local shadow budget. Virtual shadow maps should be reconsidered only
together with a meshlet-rendering decision. Each needs a separate decision.

## Decision boundaries

- Tap reduction relies on TAA or an upscaler, so modes without temporal
  resolve need a defined fallback kernel.
- Contribution cutoffs and half precision change output within a tolerance
  that must be chosen before implementation.

## Evidence needed

For each step, use matched Release before and after runs of the steady and
motion Bistro cases, plus the Bistro Metal text snapshot. Output must match
within the existing tolerance, or the owner must accept a documented quality
change. Native Vulkan execution and parity remain required for the portable
claim and are unavailable on the development host.
