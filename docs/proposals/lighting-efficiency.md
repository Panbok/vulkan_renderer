---
status: proposed
updated: 2026-10-09
authority: proposal
---
# Lighting efficiency

This proposal covers deferred lighting, local shadows and transmission on the
desktop pipeline, which runs only on Vulkan since 2026-10-06
([ADR-087](../adr/087-gpu-class-graphics-pipelines.md)). The tiled pipeline's
lighting work is in [Tiled graphics pipeline](tiled-pipeline.md).

## Current baseline

`Lighting.Deferred` evaluates the sky, image-based and probe lighting, the
directional cascades, and every punctual light in the pixel's world-grid cell
([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)). A base kernel
without the clearcoat, sheen and anisotropy paths shades every tile unless the
frame has material planes; then a layered kernel shades the tiles that use them
([ADR-062](../adr/062-layered-clearcoat.md)). Point lights come from a
camera-independent world grid of at most 384 cells. Deferred lighting reads
local-shadow visibility from the full-resolution `Shadow.LocalMask` array,
which filters each shadowed light once per pixel with a contact-shadow march
and, under temporal reconstruction, four rotated taps; transmission prefix
lookups apply when refractive casters exist (ADR-019).

### Vulkan cost split, 2026-10-03

Vulkan Release, RX 6700 XT, AMD driver 26.6.3, the Bistro street view at
1920x1080 with TAA and the High preset, one child of 240 frames under
`local-offscreen-gpu-single`, three full-filter lights. Rows are costs removed
by compiling out each part; the lighting rows are cumulative in the listed
order.

| `Shadow.LocalMask` (2.82 ms) | ms |
|---|---:|
| Four rotated taps for the three full-filter lights | 1.10 |
| One tap and setup for every shadowed light | 1.24 |
| Light traversal, punctual term and slot writes | 0.44 |
| Transmission-map lookups, inside both filter rows | 0.83 |

| `Lighting.Deferred` (3.69 ms) | ms |
|---|---:|
| Light-contribution counters (they also change the ranking) | 0.20 |
| Inline filtering of lights past the eighth mask slot | 0.05 |
| Mask reads | 0.73 |
| Punctual light loop and BRDF | 0.83 |
| Directional cascades, cloud shadow and BRDF | 0.68 |
| Image-based, probe and volume lighting | 0.29 |
| Remainder | 0.91 |

A second series, after counting light contribution every fourth frame,
removed one part at a time from the 3.50 ms median of `Lighting.Deferred`:
directional shadow sampling 0.53 ms, of which the PCSS blocker search is
0.08 ms, the cascade blend band 0.05 ms and the cloud shadow 0.09 ms; the
GTAO visibility sample and its cone and multibounce terms 0.15 ms; the
neighbour-normal roughness filter 0.09 ms.

The transmission chain took 1.50 ms over three children. Layer 0 covered
138,890 pixels for 0.73 ms; layers 1 to 3 covered 4,976, 3,208 and 2,710 pixels
for about 0.24 ms each, mostly full-screen compaction scans, glass rasters and
depth seeds. Each `Transmission.DepthSeed` copies opaque depth in about
0.03 ms, at memory bandwidth, so merging the four seeds saves nothing.

The AMD driver compiles these kernels as wave64. `vk_deferred_lighting` uses
222 VGPRs and 9.5 KB of scratch for the rectangle-light polygon clip; the
layered lighting and transmission kernels use 253 to 256 VGPRs and spill. None
of these limits the time: compiling out the rectangle-light path lowered
`vk_deferred_lighting` to 170 VGPRs without scratch and saved 0.02 ms;
requiring wave32 slowed the mask by 0.14 ms; loading shadow view rows per wave
did not change the mask; issuing the transmission depth loads before the
opaque test slowed it by 0.27 ms. Every Bistro light has refractive casters in
range, mostly its own lantern glass, so a per-light glass-overlap test removes
no transmission lookups in Bistro. Peeling two transmission layers instead of
four would save 0.47 ms but darkens the lantern glass and its glow visibly.

### Measured on the removed Metal implementation

The shipped steps were chosen from Metal Release measurements on the M1 Pro
(2026-09-26 to 2026-10-02, Bistro street view at 1280x720, local and
non-authoritative), when Metal still ran the desktop pipeline. They do not
describe the Vulkan implementation's costs; these findings still bound the
options below:

- Local shadows cost about 4.9 ms of a 20.9 ms frame, 3.7 ms of it PCF
  sampling in deferred lighting. Unshadowed point lights cost 2.7 ms, and a
  world grid 89 times finer saved only 0.13 ms: Bistro's 7.5 to 15 m ranges
  overlap genuinely, with up to 28 lights reaching one 1 m cell.
- The clearcoat and sheen paths cost 24% of `Lighting.Deferred` on pixels that
  use neither, through register pressure; this led to the base and layered
  kernel split (ADR-062).
- Rejected with measurements: a five-tap early exit and skipping back-facing
  lookups (no gain); a half-resolution mask (0.7 ms saved, but it softened
  sharp shadows on thin geometry, and the inline fallback that restores them
  was slower than the full-resolution mask); skipping the glass lookup of
  single-tap lights (it removes tinted shadows behind glass and shifts
  exposure); a single-pass HZB kernel (the eleven HZB passes took 0.15 ms);
  two GTAO slices or steps (6.3% to 25% of pixels changed by more than 2 of
  255).
- Temporal reuse of `Shadow.LocalMask` slots for a static camera lowered the
  mask from 4.63 to 2.09 ms without TAA but only from 3.23 to 2.50 ms with it,
  because of the per-light history read; a production version with
  reprojection and validation was estimated to save 0.4 to 0.6 ms under TAA
  and was not built.
- Directional PCF sample count is not the directional cost: 4 samples matched
  16 within 0.03 ms.

## Proposed changes

In order of measured payoff on Vulkan:

1. **Fewer passes in the transmission chain.** Compact transmission layers 1
   to 3 from layer 0's pixel list instead of full-screen scans; the scans
   cost about 0.2 ms at 1080p. Chains such as the HZB mips pay a barrier and
   a dispatch per step, but their total is small.
2. **Cheaper local-shadow data.** Store untinted `Shadow.LocalMask` slots in
   fewer channels; mask reads cost 0.73 ms of deferred lighting. Pack the
   local-shadow transmission depths into one texture; transmission lookups
   cost 0.83 ms of the mask, because every Bistro lamp is inside its own
   lantern glass. A five-tap PCF in the inline fallback needs an owner
   quality decision.
3. **Half-precision BRDF terms** in the per-light loop, keeping positions,
   depths and normalisation in 32-bit. It needs a numeric comparison against
   the full-precision loop on RDNA 2 and Ampere; on the M1, 16-bit arithmetic
   gave no ALU gain in the tiled prototype
   ([Tiled graphics pipeline](tiled-pipeline.md#first-prototype-measurements)).
4. **Deferred for Bistro-class scenes: camera-space clustered culling.** It
   remains useful above the 128-light table or for scenes with short-range
   lights, but it does not pay off here.

Compute tile lists that skip sky tiles, the compute form of a sky stencil, are
not worth timing on Bistro. The twelve distinct views of the Vulkan Bistro
snapshot camera at 1920x1080 average 3.5% sky-only 8x8 tiles and 0.4% mixed
tiles. The most open view has 19.4% sky-only tiles, and five interior views
have none. `GBuffer.Resolve` writes only defaults and sky motion in those
tiles, and `Lighting.Deferred` must still draw the sky there. Bistro has no
clearcoat, sheen or anisotropy material, so the layered lighting kernel never
runs on it. The counts come from `visibility_ids` captures on 2026-10-07:
RX 6700 XT, driver 26.6.3, dirty tree, `local-offscreen` profile, snapshot
report
`sha256:a03084e7d8cbef8f60f7bf35f36411d774f201257c36fe64f03da453ed48dcd7`.
The temporary case used the camera keys of `smoke.bistro.vulkan.text.snapshot`.

Longer term, stationary-light shadowing (baked static visibility, with runtime
maps only for dynamic casters) or ray-traced many-light sampling would remove
the local shadow budget. Virtual shadow maps should be reconsidered only
together with a meshlet-rendering decision. Each needs a separate decision.

## Decision boundaries

- Further tap reduction outside the mask relies on TAA or an upscaler; the
  mask's fallback without temporal resolve is the fixed nine-tap kernel.
- Half precision changes output within a tolerance that must be chosen
  before implementation. The contribution cutoff shipped at 0.001 of
  pre-exposed luminance (ADR-019).

## Evidence needed

For each step, use matched Release before and after runs on the Windows
Vulkan host of the Bistro street-view cases
(`local_shadow_bistro_vulkan_street` and its variants) and a moving case,
plus the Vulkan Bistro text snapshot (`smoke.bistro.vulkan.text.snapshot`).
Output must match within the existing tolerance, or the owner must accept a
documented quality change. Three identical runs of that snapshot differ in
captures 5, 8, 9, 12 and 13
([Windows/Vulkan checklist](windows-vulkan-verification.md)), so that
variation needs a fix or an exclusion first. Step 3 also needs the tiled
pipeline's Bistro cases on the M1 Pro.
