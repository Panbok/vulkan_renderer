---
status: proposed
updated: 2026-10-02
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

### Cost split, 2026-10-02

Metal Release, M1 Pro, Bistro street view at 1280x720 without TAA (the street
camera of `bistro_native_perf_audit_steady`), `local-offscreen-gpu-single`,
after the occupancy hints (ADR-019). Each row is the mask or lighting time
removed by also compiling out that part, applied cumulatively in the listed
order, so a row's cost depends on the rows above it.

| `Shadow.LocalMask` (5.77 ms; 4.21 ms with TAA) | ms |
|---|---:|
| Contact march for the full-filter lights | 1.10 |
| Nine-tap PCF for the three full-filter lights (four rotated taps with TAA: 1.14) | 2.63 |
| Single-tap compare and transmission lookup for every shadowed light | 1.37 |
| Light traversal, punctual term, G-buffer reads and slot writes | 0.63 |

| `Lighting.Deferred` (3.76 ms) | ms |
|---|---:|
| Light-contribution counters | 0.19 |
| Inline filtering of lights past the eighth mask slot | 0.44 |
| Mask reads | 0.18 |
| Punctual BRDF loop over the cell's lights | 1.36 |
| Directional cascades and cloud shadow | 0.59 |
| Directional BRDF | 0.06 |
| Image-based, probe and volume lighting | 0.24 |
| GTAO | 0.07 |
| Remainder: G-buffer decode, sky, roughness filtering and writes | 0.63 |

The transmission chain took 3.25 ms in four peeled layers covering 61,898,
2,272, 1,483 and 1,258 pixels: four `VBuffer.Transmission` rasters 0.50 ms,
four full-screen `Transmission.Compact` scans 0.68 ms, the opaque pyramid
0.11 ms, culling 0.04 ms, the nearest layer's shading 1.44 ms (0.49 ms of it
inline nine-tap local-shadow filtering) and the three deeper layers' shading
0.48 ms, about 0.15 ms each whatever their pixel count.

## Proposed change

In order of measured payoff:

0. **Fewer passes in multi-pass chains.** Metal now records consecutive
   compute passes in one encoder ([ADR-025](../adr/025-selected-renderer-implementation-strategy.md)),
   which removed most of the 2.2 ms of encoder-boundary idle time: the steady
   case fell from 16.54 ms to 15.31 ms per frame without timestamps. Chains
   such as the HZB mips and per-layer transmission compaction still pay a
   barrier and a dispatch per step and are candidates for single-pass kernels.
   Measured on 2026-09-27 (Metal Release, M1 Pro, `bistro_native_perf_audit_steady`,
   `local-offscreen-perf-audit-gpu`, report
   `20260926T225225.918Z-005ae6`, GPU pass sum 14.81 ms p50): the eleven HZB
   passes take 0.15 ms together, the largest 0.05 ms, so a single-pass HZB
   kernel cannot repay a two-backend rewrite and is not pursued. The four
   transmission compaction passes take about 0.65 ms and remain the chain
   candidate.
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
   integrated by TAA have shipped in the mask pass, with the fixed nine taps
   kept without temporal reconstruction
   ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)): 1.83 ms less
   mask time in the street view with TAA. Inline filtering in forward and
   transmission shading still takes nine taps.
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

- Further tap reduction outside the mask relies on TAA or an upscaler; the
  mask's fallback without temporal resolve is the fixed nine-tap kernel.
- Contribution cutoffs and half precision change output within a tolerance
  that must be chosen before implementation.

## Evidence needed

For each step, use matched Release before and after runs of the steady and
motion Bistro cases, plus the Bistro Metal text snapshot. Output must match
within the existing tolerance, or the owner must accept a documented quality
change. Native Vulkan execution and parity remain required for the portable
claim and are unavailable on the development host.
