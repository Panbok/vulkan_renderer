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
| Opaque surfaces | Visibility buffer, compute G-buffer resolve, compute lighting | One MSAA render pass: forward shading with clustered light lists, or a G-buffer kept in tile memory and lit in the same pass |
| Anti-aliasing | Portable TAA, MetalFX or FSR 3.1 | MSAA resolved in tile memory; no temporal history |
| Indirect light | IBL, baked diffuse volumes ([ADR-054](../adr/054-baked-diffuse-volumes.md)), optional SSGI | IBL and baked volumes; no SSGI |
| Ambient occlusion | GTAO in compute | Baked or probe occlusion |
| Reflections | IBL and optional SSR | IBL and local probes |
| Directional shadows | Retained cascades with PCF | Shared retained cascades; a tier may lower filtering |
| Local shadows | Mask pass, nine-tap PCF, contact march | A tier-bounded count of shadowed lights with PCF, drawn in the lighting pass |
| Transmission | Four peeled layers shaded in compute | Sorted forward blend with one refraction sample |
| Post-processing | Compute bloom, exposure, tonemap | Shared color pipeline; tonemap and bloom combine in the final render pass where possible |
| Arithmetic | 32-bit throughout | 16-bit where the art-level contract allows |

Mobile Vulkan implements the same structure with dynamic-rendering local read
for tile-memory reads and lazily allocated transient attachments.

## Open choices

- Forward shading with clustered lights or deferred shading in tile memory.
  Forward suits MSAA and the hardware's hidden-surface removal; tile-memory
  deferred keeps lighting cost independent of overdraw. The prototype measures
  both on Bistro opaque surfaces.
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

The gate therefore needs a cheaper lighting model as well as the tiled
structure: direct light from static lights baked offline, a bounded runtime
budget for dynamic lights, and base shading near half its current cost.

## Acceptance evidence

- Matched Release `gpu.submission` runs of the complete tiled pipeline on the
  M1 Pro meeting 16.7 ms p95 on Bistro, with the selected tier recorded.
- Captures showing that the art-level contract holds: equal exposure, color
  pipeline and material response on surfaces both pipelines light the same way.
- A Vulkan implementation plan for mobile GPUs, or an explicit deferral.
