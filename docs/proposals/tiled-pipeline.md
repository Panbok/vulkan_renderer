---
status: proposed
updated: 2026-10-05
authority: proposal
---

# Tiled graphics pipeline

[ADR-087](../adr/087-gpu-class-graphics-pipelines.md) accepts a separate
pipeline class for tile-based GPUs and its M1 Pro budget: Bistro at
2560×1440, render scale 1.0, no upscaler, 16.7 ms p95. This proposal holds
the open design, the prototype that tests the budget, and the evidence needed
to accept the design.

## Current baseline

Apple M-series runs the desktop pipeline. At the budget's resolution it takes
56.12 ms mean per frame on the M1 Pro; ADR-087 records the configuration and
the passes. Three further measurements from the same day (Release, M1 Pro,
native 2560×1440, single local runs, not authoritative) bound what tuning the
desktop pipeline can recover:

- Register spills, attributed by giving each compute pass its own encoder in a
  temporary build and recording a Metal System Trace: `World.Blend` spills
  1.91 KiB per thread, `Transmission.Shade.Compact` layers 1 to 3 spill
  1.73 KiB and layer 0 816 B. `Lighting.Deferred` spills 64 B and
  `GBuffer.Resolve` 48 B; their low occupancy is the register limit that
  [ADR-019](../adr/019-bounded-forward-spatial-lighting.md) already tuned.
- A `max_total_threads_per_threadgroup` hint of 256 on the six transmission
  shade kernels lowered their summed time from 3.41 and 3.43 ms (two runs
  without a hint) to 2.69 ms; 192 and 128 gave 2.80 and 2.78 ms. The change is
  not landed.
- Keeping the current G-buffer in tile memory would save at most about 1 ms:
  `GBuffer.Resolve` and `Lighting.Deferred` move about 87 and 148 MB per frame
  at 4 to 13 GB/s.

Tuning the desktop pipeline can therefore recover a few milliseconds, not the
39 ms between the measured frame and the budget.

## Candidate design

Every item below is open until the prototype measures it.

| Stage | Desktop pipeline today | Tiled candidate |
|---|---|---|
| Culling and draw encoding | GPU classification, ICB or indirect-count draws | Shared |
| Opaque surfaces | Visibility buffer, compute G-buffer resolve, compute lighting | One MSAA render pass: depth pre-pass, then forward shading with clustered lists of the dynamic lights (decided) |
| Anti-aliasing | Portable TAA, MetalFX or FSR 3.1 | MSAA resolved in tile memory; no temporal history |
| Static light | Every static light evaluated per pixel each frame | Lightmaps baked by `vkr_bakery`: direct and bounced diffuse light from static lights, on a second UV set (owner decision, 2026-10-05) |
| Indirect light | IBL, baked diffuse volumes ([ADR-054](../adr/054-baked-diffuse-volumes.md)), optional SSGI | Lightmaps on static surfaces; IBL and baked volumes for dynamic objects; no SSGI |
| Ambient occlusion | GTAO in compute | Baked or probe occlusion |
| Reflections | IBL and optional SSR | IBL and local probes |
| Directional shadows | Retained cascades with PCF | Shared retained cascades; a tier may lower filtering |
| Local shadows | Mask pass, nine-tap PCF, contact march | A tier-bounded count of shadowed lights with PCF, drawn in the lighting pass |
| Transmission | Four peeled layers shaded in compute | Sorted forward blend with one refraction sample |
| Post-processing | Compute bloom, exposure, tonemap | Shared color pipeline; tonemap and bloom combine in the final render pass where possible |
| Arithmetic | 32-bit throughout | 32-bit; 16-bit only where a measured kernel is register-bound (no ALU gain on M1, see below) |

Mobile Vulkan implements the same structure with dynamic-rendering local read
for tile-memory reads and lazily allocated transient attachments.

## Open choices

- Day/night cycle (owner requirement, 2026-10-05): sun direct light and
  shadows stay at runtime through the retained cascades
  ([ADR-041](../adr/041-retained-cascaded-shadows.md)); sun bounce and sky
  light come from baked sun keys blended by the sun position, and static
  lamps from lamp-group layers the runtime scales
  ([ADR-088](../adr/088-baked-lightmap-sets.md)), eight keys on the sun's
  daily circle about the celestial pole, each with its own atmosphere. The
  time of day ([ADR-090](../adr/090-time-of-day.md)) turns the sun and moon
  and sets each light group's factor.
- Lightmap design: unwrapping, packing, density, the GPU baker and ASTC 4×4
  HDR storage are decided ([ADR-088](../adr/088-baked-lightmap-sets.md)).
  Open: a directional encoding (SH or a dominant direction) for normal maps
  and specular, and how dynamic objects and the specular highlights of static
  lights are lit.

- Forward shading or deferred shading in tile memory: decided, forward after
  a depth pre-pass ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md),
  measured below).
- MSAA sample count: two or four.
- The M1 Pro quality tier: which effects drop or lower first.
- Graph representation: a graph pass is one render pass today. Several
  subpasses that share tile memory need either a graph pass that owns
  sub-stages or merged pass groups in the graph compiler.

## Prototype gate

Build a Metal-only prototype outside production presets that renders Bistro
opaque surfaces at 2560×1440 on the M1 Pro: MSAA, directional light with the
retained cascades, IBL and baked volumes, unshadowed point lights and a
bounded set of shadowed local lights. Measure whole-frame GPU time with
`performance-windowed-gpu-submission` for each open shading choice. The
prototype passes when opaque shading, including the main pass and resolve,
fits within half of the 16.7 ms budget, leaving the rest for shadows,
transmission, blend and post-processing.

### First prototype measurements

A Metal prototype (2026-10-05, worktree only, not landed) replays the culled
`VBuffer.Opaque` draws after that pass into one render pass with memoryless
color and depth, shades them with the existing forward shader
(`vkr_metal_packet_opaque_fragment`) and resolves on chip. Its output feeds
nothing, so its cost is the change in whole-frame GPU time. Each row is one
process of a temporary native-resolution copy of `bistro_metal_production_040`
(2560×1440, render scale 1.0, portable TAA, 120 warmup and 180 measured
frames) under a temporary single-process copy of
`performance-windowed-gpu-submission` (Release, M1 Pro, non-authoritative).
The desktop baseline measured 50.14 to 50.30 ms `gpu.submission` median in
four runs.

| Prototype variant | Median | Added |
|---|---|---|
| Forward, no depth pre-pass, 1 / 2 / 4 samples | 110.15 / 124.15 / 128.49 ms | +60 to +78 ms |
| Forward with depth pre-pass, 1 / 4 samples | 81.56 / 86.71 ms | +31.4 / +36.6 ms |
| Same, local shadows faded out (baseline 45.96 ms) | 79.27 ms at 4 samples | +33.3 ms |
| Forward with pre-pass, no punctual or rectangle lights, 1 / 4 samples | 61.70 / 63.55 ms | +11.6 / +13.4 ms |

Findings:

- The forward shader can discard, so without a depth pre-pass hidden-surface
  removal does not apply and overdraw is shaded in full.
- Four-sample MSAA in tile memory costs about 2 to 5 ms over one sample.
- Shading each pixel once with the current lighting model costs about 31 ms,
  more than the desktop chain's resolve, lighting and mask passes. Bistro's
  72 static local lights cost about 23 ms of it; inline local shadows about
  3 ms. The remaining base shading costs 12 to 13 ms.
- The render-pass structure is not the limit; the lighting model is. The
  baked diffuse volumes hold only indirect diffuse light
  ([ADR-054](../adr/054-baked-diffuse-volumes.md)), so every static lamp's
  direct light is evaluated per pixel each frame.

A second series (same configuration, one sample with the depth pre-pass, no
punctual or rectangle lights) removed one part of the forward shader per run.
The desktop baseline measured 50.35 and 51.84 ms before and after the series;
added costs below are against their mean, 51.1 ms, with about ±1 ms drift:

| Forward base shading variant | Median | Added |
|---|---|---|
| Full base shading | 63.45 ms | +12.4 ms |
| Clearcoat, sheen and anisotropy compiled out | 58.25 ms | +7.2 ms |
| Sun shadow sampling removed | 59.78 ms | +8.7 ms |
| IBL probe loop removed (global environment only) | 60.68 ms | +9.6 ms |
| Baked-volume lookup removed | 61.68 ms | +10.6 ms |
| Cloud shadow removed | 62.21 ms | +11.1 ms |
| Layers, probes, volumes and clouds removed; sun shadow kept | 57.55 ms | +6.5 ms |

The savings do not add: compiling out the material layers frees registers,
and that occupancy gain carries most of the lean variant's saving. Per-material
shader variants, which the desktop deferred path already uses per tile
([ADR-062](../adr/062-layered-clearcoat.md)), are therefore the first base
shading change. Lightmaps replace the probe and volume diffuse lookups on
static surfaces.

A third series compared the lean variant with a compact forward shader
written for the prototype (`vkr_metal_tiled_minimal_shade`: base, normal,
ORM and emissive textures, alpha test, GGX sun light with the cascades,
global SH and prefiltered environment), compiled once in 32-bit and once in
16-bit arithmetic. The baseline measured 49.32 and 49.38 ms:

| Variant (depth pre-pass) | Median, 1 sample | Median, 4 samples |
|---|---|---|
| Lean über-shader | 55.65 ms (+6.3) | not run |
| Compact shader, 32-bit | 54.53 ms (+5.2) | 54.99 ms (+5.7) |
| Compact shader, 16-bit | 54.42 ms (+5.1) | 54.76 ms (+5.4) |

The whole opaque pass, including the depth pre-pass, the raster of every
Bistro opaque draw, shading and four-sample MSAA, costs 5.4 to 5.7 ms with a
compact shader, within the half-budget gate. 16-bit arithmetic gains about
0.1 to 0.3 ms: the M1 runs 16-bit and 32-bit arithmetic at the same rate, and
this pass is limited by texture fetches and raster, not ALU. Four-sample MSAA
adds 0.3 to 0.5 ms to the compact shader.

The gate therefore needs a cheaper lighting model as well as the tiled
structure: direct light from static lights baked offline, a bounded runtime
budget for dynamic lights, and base shading near half its current cost.

A fourth series (2026-10-05) compared the compact forward shader with
single-pass deferred shading in tile memory: G-buffer draws write emission
(RGBA16F), albedo and occlusion (RGBA8 sRGB), normal (RGB10A2), roughness,
metalness and dielectric reflectance (RGBA8) and depth (R32F) to memoryless
attachments, 24 bytes per sample, and one full-screen draw in the same render
pass reads them through programmable blending, reconstructs the position and
lights it with the same model; with multisampling it runs once per sample.
Both variants produced the same image: frame-mean colour within 0.1% and
identical reconstructed positions. Two series ran in opposite orders; the
baselines measured 51.71 and 50.98 ms, then 50.44 ms:

| Variant | Added, 1 sample | Added, 4 samples |
|---|---|---|
| Compact forward, depth pre-pass | +5.6 / +4.8 ms | +6.0 / +5.4 ms |
| Tile deferred, depth pre-pass | +5.6 ms | +7.7 / +7.3 ms |
| Tile deferred, no pre-pass | +4.9 / +4.8 ms | +9.0 / +8.8 ms |

Without multisampling the two structures cost the same. With four samples,
tile deferred costs 1.7 to 3.4 ms more: its G-buffer takes four times the
tile memory and its lighting runs per sample, while forward shading pays
0.4 to 0.6 ms for four samples. Lighting only edge pixels per sample would at
best bring tile deferred to the forward cost. Forward shading with a depth
pre-pass is therefore the recommended opaque structure; tile deferred would
pay off only with many dynamic lights per pixel, which the baked lamp groups
avoid.

## Lightmaps

[ADR-088](../adr/088-baked-lightmap-sets.md) records what is implemented:
lightmap UVs on cooked and managed models, the layout, sun-key and lamp-group
layers, the Metal GPU baker and its parity with the CPU integrator, ASTC 4×4
HDR pages in VKLM files, `vkr_bakery bake lightmap`, project storage,
packaging and the Bakery panel options, with the owner decisions and Bistro
measurements.

Open: irradiance only or directional SH per layer for normal-mapped surfaces,
and the texel density budget for the M1 memory floor (Bistro stores 16 MiB
per layer page: 453 MB for eight sun keys and one lamp group on three
pages).

Remaining phases:

1. Bake completion: the layer split and texel direct term in the CPU
   integrator for hosts without Metal ray tracing, and a faster path through
   stacked cutout foliage (textured Bistro bakes at a third of the untextured
   rate).
2. Tiled runtime: load VKLM sets (an ASTC 4×4 HDR texture format in both
   backends), map instances to their rectangles, and sample the blended sun
   keys and scaled lamp groups in the tiled pipeline's forward shader,
   measured against the 16.7 ms budget.

## Acceptance evidence

- Matched Release `gpu.submission` runs of the complete tiled pipeline on the
  M1 Pro meeting 16.7 ms p95 on Bistro, with the selected tier recorded.
- Captures showing that the art-level contract holds: equal exposure, color
  pipeline and material response on surfaces both pipelines light the same way.
- A Vulkan implementation plan for mobile GPUs, or an explicit deferral.
