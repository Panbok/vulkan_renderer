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

- Day/night cycle (owner requirement, 2026-10-05): the sun moves and lamps
  switch with time of day, so lightmaps cannot hold sun light or fixed lamp
  light. Candidates: sun direct light and shadows stay at runtime through the
  retained cascades ([ADR-041](../adr/041-retained-cascaded-shadows.md)); sky
  and sun bounce come from a relightable baked transfer, such as per-texel
  sky visibility or SH transfer; static lamps bake into per-group lightmaps
  that the runtime scales by each group's current intensity.
- Lightmap design: UV unwrapping and packing in `vkr_bakery`, texel density,
  encoding (for example SH or a dominant direction for specular), the baker's
  reuse of the ADR-054 BVH and photon pass, and how dynamic objects and
  specular highlights of static lights are lit.

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

## Lightmap baseline and design

What exists (2026-10-05):

- The ADR-054 baker in [`tools/bake/`](../../tools/bake/) flattens the scene,
  builds a CPU BVH, traces paths with a photon pass and projects to SH. One
  trace sums sun, sky, every lamp and emission
  ([`vkr_bake_integrator.cpp`](../../tools/bake/vkr_bake_integrator.cpp)), so
  its output cannot be split by light or group.
- `VkrPackedStaticVertex` has one UV set; word 7 is always zero and validated
  as zero ([`vkr_packed_geometry.c`](../../renderer/src/vkr_packed_geometry.c)).
  The glTF importer reads only `TEXCOORD_0`. No UV unwrapping or chart packing
  code exists in the tree or in `vendor/`.
- Scene lights carry only `enabled`; there is no light group, mobility or
  intensity animation, and no time-of-day system. The sun is whichever
  directional light is resolved each frame.
- Scene bakes run as explicit `vkr_bakery bake` commands with project-level
  storage under `builds/<uuid>/` ([`vkr_project_bake.c`](../../tools/bakery/project/vkr_project_bake.c)),
  and the editor's Bake panel triggers them.

Proposed design:

1. **Second UV set.** The mesh producer unwraps static meshes into charts and
   stores UV2 as two unorm16 values in word 7. Per-instance atlas scale and
   offset live in a side table indexed by instance, leaving the prepared
   instance row unchanged.
2. **Separable bake.** A `vkr_bakery bake lightmap` scene bake reuses the
   ADR-054 BVH, scene flattening and integrator, but traces from lightmap
   texels and writes separate layers: one irradiance layer per lamp group,
   which the runtime scales by the group's current intensity, and a sky
   transfer layer, such as SH sky visibility, that relights with the current
   sky. Sun direct light stays at runtime through the retained cascades.
3. **Scene and time of day.** Lights gain a group and a baked or dynamic
   mobility. A time-of-day system drives the sun, moon, sky and lamp-group
   intensities; it is shared by both pipeline classes.
4. **Runtime.** Static surfaces in the tiled pipeline sample the lightmap
   layers; dynamic objects use the baked volumes and probes.

Owner decisions (2026-10-05): `vkr_bakery` unwraps with vendored
[xatlas](https://github.com/jpcy/xatlas) (MIT). Sun bounce light follows the
day/night cycle through baked sun keys: four to eight sun positions baked as
separate layers and blended at runtime by the current sun position.

Owner decisions (2026-10-05): eight sun keys, and lightmap layers stored as
ASTC 4×4 HDR, one byte per texel. A native probe on the M1 Pro (Apple7)
created `MTLPixelFormatASTC_4x4_HDR`, `BC6H_RGBUfloat` and `RGB9E5Float`
textures. At 8 texels per unit Bistro needs an estimated 30 million texels
once instances are counted (12.7 million for one instance per mesh times the
2.4 instance-to-unique triangle ratio): about 30 MB per layer and 360 MB for
eight sun keys, three lamp groups and one sky layer.

Open: irradiance only or directional SH per layer for normal-mapped surfaces,
the sun path the eight keys sample before a time-of-day model exists, and the
texel density budget once instances are packed.

Phases:

1. UV2: vendor xatlas; an opt-in mesh recipe field unwraps and packs each mesh
   into one normalized chart atlas with a recorded lightmap size; UV2 packs
   into word 7 under a new packed-geometry version; both native decoders read
   it.
   Status: implemented on the CPU and in both shader roots: the recipe field,
   the cooked flag and atlas block
   ([ADR-030](../adr/030-offline-mesh-optimization-and-cooking.md)), word 7
   ([ADR-031](../adr/031-versioned-packed-static-geometry-abi.md)) and the
   decoders, covered by `run_mesh_lightmap_uv_tests` and
   `test_mesh_cooked_lightmap_uv_round_trip`. The baker's mesh decode does
   not read UV2 yet. Bistro (`bistro-lights.gltf`, Release mesh tool, managed
   output in a temporary bundle, deferred textures, 8 texels per unit,
   2026-10-05) cooks in 17.7 s at 1.27 GB peak memory, against 0.7 s and
   0.68 GB without lightmap UVs, and byte-identically on a second run. All
   646 cooked source meshes get an atlas at full density; seams raise the
   vertex count from 1,741,441 to 2,158,263 and the artifact from 30.8 to
   42.9 MB. The largest atlas is 1,368 × 1,364 texels and the median 28 ×
   28. One instance of each mesh needs 12.7 million texels, about 97 MiB per
   uncompressed RGBA16F layer, before the 2,909 instances are counted
   separately, so lamp-group, sky and sun-key layers need block compression,
   a compact encoding or a lower density to fit the M1 memory floor.
2. Separable bake: texel tracing in the ADR-054 baker with lamp-group,
   sky and sun-key layers; project storage and the editor Bake panel.
3. Time of day: light groups and mobility in scene data; a system driving
   sun, moon, sky and group intensities.
4. Tiled runtime: lightmap sampling in the tiled pipeline's forward shader,
   measured against the 16.7 ms budget.

## Acceptance evidence

- Matched Release `gpu.submission` runs of the complete tiled pipeline on the
  M1 Pro meeting 16.7 ms p95 on Bistro, with the selected tier recorded.
- Captures showing that the art-level contract holds: equal exposure, color
  pipeline and material response on surfaces both pipelines light the same way.
- A Vulkan implementation plan for mobile GPUs, or an explicit deferral.
