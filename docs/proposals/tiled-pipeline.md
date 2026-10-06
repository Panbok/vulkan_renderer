---
status: proposed
updated: 2026-10-06
authority: proposal
---

# Tiled graphics pipeline

[ADR-087](../adr/087-gpu-class-graphics-pipelines.md) accepts a separate
pipeline class for tile-based GPUs and its M1 Pro budget: Bistro at
2560×1440, render scale 1.0, no upscaler, 16.7 ms p95. This proposal holds
the open design, the prototype measurements that chose its structure, the
remaining work and the evidence needed to accept the design.

## Current baseline

Every Metal renderer, including the editor, runs the tiled pipeline
(ADR-087, decisions 7 and 9); the desktop pipeline runs only on Vulkan. Its
graph, opaque pass, forward shader, lightmap sampling, glass, dynamic lights,
adaptive quality and measurements are in
[ADR-087](../adr/087-gpu-class-graphics-pipelines.md), decisions 5 to 12. On a
lightmap-baked Bistro with cooked mesh levels it takes 15.5 ms p95 at native
scale and 14.9 ms with adaptive quality; 16 dynamic lights reach 20.7 ms p95
at native scale. The desktop pipeline's removed Metal implementation took
56.12 ms mean on the same view (ADR-087, Context).

## Candidate design

Rows marked decided follow ADR-087 and rows marked queued are approved
remaining work; the others stay open until measured.

| Stage | Desktop pipeline (Vulkan) | Tiled pipeline |
|---|---|---|
| Culling and draw encoding | GPU classification, indirect-count draws | Shared classification, Metal ICB draws |
| Opaque surfaces | Visibility buffer, compute G-buffer resolve, compute lighting | One MSAA render pass: depth pre-pass, then forward shading with clustered lists of the dynamic lights (decided) |
| Anti-aliasing | Portable TAA or FSR 3.1 | 4× MSAA with alpha to coverage and a tone-mapped resolve in tile memory; no FXAA or temporal history (decided, ADR-087) |
| Static light | Every static light evaluated per pixel each frame | Lightmaps baked by `vkr_bakery`: direct and bounced diffuse light from static lights, on a second UV set (owner decision, 2026-10-05) |
| Indirect light | IBL, baked diffuse volumes ([ADR-054](../adr/054-baked-diffuse-volumes.md)), optional SSGI | Lightmaps on static surfaces; IBL and baked volumes for dynamic objects; no SSGI |
| Ambient occlusion | GTAO in compute | Baked AO in the lightmap alpha (queued) |
| Reflections | IBL and optional SSR | IBL, per-draw reflection probes (queued) and [planar reflections](planar-reflections.md) for mirror-like surfaces; no SSR (owner decision, 2026-10-06) |
| Directional shadows | Retained cascades with PCF | Shared retained cascades; a tier may lower filtering |
| Local shadows | Mask pass, nine-tap PCF, contact march | A tier-bounded count of shadowed lights filtered in the lighting pass (decided: 4 of the 16 dynamic lights, ADR-087); one bilinear comparison each today, a small gather PCF queued |
| Transmission | Four peeled layers shaded in compute | Sorted forward blend with dual-source transmittance (decided for thin glass, ADR-087); absorption, then refraction from a half-resolution copy, for thick and rough glass (queued) |
| Post-processing | Compute bloom, exposure, tonemap | Shared color pipeline; the tonemap pass samples the bloom chain itself (decided, ADR-087) |
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
- The M1 Pro quality tier: which effects drop or lower first.
- MSAA sample count and graph representation: decided, four samples and one
  graph pass that owns its sub-stages
  ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md), decision 6).

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

These series ran on the M1 Pro while Metal still ran the desktop pipeline;
their baselines are that removed implementation. A Metal prototype
(2026-10-05, worktree only, not landed) replays the culled
`VBuffer.Opaque` draws after that pass into one render pass with memoryless
color and depth, shades them with the existing forward shader
(`vkr_metal_packet_opaque_fragment`, since removed with the Metal desktop
pipeline) and resolves on chip. Its output feeds
nothing, so its cost is the change in whole-frame GPU time. Each row is one
process of a temporary native-resolution copy of `bistro_metal_production_040`,
a Metal desktop case since removed (2560×1440, render scale 1.0, portable
TAA, 120 warmup and 180 measured
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
2. Tiled runtime: the forward shader samples the blended sun keys and
   scaled lamp groups (ADR-087, decision 8); the editor runs it, and it
   draws thin glass and a bounded set of dynamic lights (decisions 9 to
   11). After the light shading variants, the anti-aliasing change and the
   bloom fold, the lightmap-baked Bistro takes about 12.6 ms median and
   16.7 to 17.0 ms p95 at native scale, and adaptive quality (decision 12)
   holds 15.8 ms p95
   ([measurements](../adr/087-gpu-class-graphics-pipelines.md#adaptive-quality-measurement)).
   The tiled pipeline is the only Metal pipeline (ADR-087, decision 7).
   [Remaining work](#remaining-work) lists what is left.
3. Cooked mesh LOD: implemented
   ([ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md#cooked-mesh-levels));
   the tiled native orbit takes 11.3 ms median and 15.5 ms p95 with it.
   Bistro's levels stop at true open borders; unlocking them would save
   at most 0.3 ms of `Tiled.Opaque` p95 and thin alpha-tested petals
   ([ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md#alternatives-considered)).
   Before the levels, a
   diagnostic that drew only the first half of every index range
   (2026-10-06, Release, M1 Pro, the tiled native cases alternating with
   the unchanged build twice, incomplete image) lowered `Tiled.Opaque` from
   8.57 / 12.45 to 7.42 / 8.58 ms median / p95, and from 10.29 / 17.76 to
   9.99 / 11.91 ms with the dynamic lights; cascade re-renders fell about
   42%, from 1.35, 2.15 and 1.63 ms to 0.78, 1.25 and 0.93 ms, and
   `gpu.submission` from 12.61 / 16.76 to 11.77 / 13.39 ms
   (`sha256:23159b0a148b7934729775a9a8f4572bec3b3698c7d145fe5ea3225ce295e498`,
   `sha256:9debfb308675f0c6b217bfb477abe93bf71240dba078e834dad073fd790f29a0`,
   `sha256:6d7c661b105ce1b05365c62d0ca8def16c9d78a86c783fb18d8046a819af8025`,
   `sha256:6a04af645bef9b83e8daef7d8ed8c19ba7956441d21fcf3bccb00e36399ad290`).
   The widest views, which set the p95, are bound by geometry: the
   pre-pass and forward pass both draw every triangle. Cooked levels are
   the largest remaining lever for the tiled p95; their saving is below
   this bound, since near geometry keeps its detail.

## Remaining work

The owner approved these tiled-pipeline features on 2026-10-06, to land in
this order as one measured change each against the 16.7 ms p95 budget:

1. Per-draw reflection probes: done, one probe per surface
   ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md#reflection-probe-measurement)).
2. Baked ambient occlusion: done, in every lightmap layer's alpha and applied
   to environment specular
   ([ADR-088](../adr/088-baked-lightmap-sets.md#encoding)).
3. A small fixed gather PCF for the four shadowed dynamic lights, in the
   shadowed shading variant only.
4. Thick glass: Beer-Lambert absorption in the blend shader first, then
   refraction and rough blur from a half-resolution copy of the scene, only
   on frames with thick or rough glass.

SSR is not planned for the tiled pipeline (owner decision, 2026-10-06).
Mirror-like surfaces get [planar reflections](planar-reflections.md) per
surface instead; other glossy surfaces use the probes of item 1.

Also open: the opaque pass on the widest views (its depth pre-pass is examined
in [tiled-depth-prepass.md](tiled-depth-prepass.md)), the dynamic-light tier at
its 0.65 floor (17.1 to 17.5 ms p95 before the cooked mesh levels), the
remaining material layers (clearcoat, sheen, anisotropy and diffuse
transmission), and the specular highlights of static lights.

## Acceptance evidence

- Matched Release `gpu.submission` runs of the complete tiled pipeline on the
  M1 Pro meeting 16.7 ms p95 on Bistro, with the selected tier recorded.
- Captures showing that the art-level contract holds: equal exposure, color
  pipeline and material response on surfaces both pipelines light the same
  way. The tiled pipeline runs on the Mac and the desktop pipeline on the
  Windows host, so this compares captures of one view from both machines;
  the harness cannot pair them (ADR-087, decision 7).
- A Vulkan implementation plan for mobile GPUs, or an explicit deferral.
