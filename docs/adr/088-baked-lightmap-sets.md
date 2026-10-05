---
status: partial
updated: 2026-10-05
authority: adr
---

# ADR-088: Baked lightmap sets

## Status

Accepted (partial). Baking, storage, packaging and the editor controls are
implemented. The runtime does not sample lightmaps yet (the scene loader
ignores the `lightmaps` block), only one sun key and one lamp group are baked,
the bake needs Metal ray tracing, and no run has exercised textured materials
on the GPU baker.

## Context

[ADR-087](087-gpu-class-graphics-pipelines.md) gives tile-based GPUs their own
pipeline with a native 1440p60 budget on the M1 Pro. The
[tiled-pipeline proposal](../proposals/tiled-pipeline.md) measured that static
lamps cost about 23 ms at runtime, so static light must be baked. The world
has a day/night cycle, so sun light cannot be baked as one fixed term.

Owner decisions (2026-10-05): unwrap with vendored xatlas; bake sun bounce in
eight sun keys blended by the current sun position; group lamps into layers
the runtime scales; store layers as ASTC 4×4 HDR; bake on the GPU with Metal
ray tracing, with the CPU integrator of
[ADR-054](054-baked-diffuse-volumes.md) as the reference and the path for
hosts without Metal ray tracing.

## Decision

### Lightmap UVs

A mesh cooked with `lightmap_texels_per_unit` carries a second UV set and one
chart atlas per source mesh ([ADR-030](030-offline-mesh-optimization-and-cooking.md),
[ADR-031](031-versioned-packed-static-geometry-abi.md)). In managed projects
the setting is part of the mesh record's recipe
(`recipe.settings.lightmap_texels_per_unit`). An import applies the request's
`model_settings`; preparing a scene repeats the recorded setting; a rebuild or
reimport that the request asks for applies the request's `model_settings`, so
a model gains or drops lightmap UVs; texture finalization repeats the recorded
setting ([`vkr_project_import.c`](../../tools/bakery/project/vkr_project_import.c),
[`vkr_project_lower.c`](../../tools/bakery/project/vkr_project_lower.c)). The
editor's Bakery panel option "Lightmap UVs on model import and rebuild" sends
8 texels per world unit.

### Layout

[`vkr_bake_lightmap.h`](../../tools/bake/vkr_bake_lightmap.h) gives every
lightmapped instance one rectangle sized for a world density (atlas size ×
instance scale × density / atlas density) in whole 4×4 blocks, and
shelf-packs the rectangles onto square pages (4096 by default). When everything
fits on one smaller page without scaling, the smallest power-of-two page from
256 upward is used. A page's texels are rasterized from the instances' lightmap
UVs, one triangle per texel.

### Layers

A layer names its lights and whether it adds the sky, surface emission and the
lights' direct term at the texel:

| Layer | Lights | Sky | Emission | Direct at texel |
|---|---|---|---|---|
| Sun key | directional | yes | no | no; the runtime adds the sun |
| Lamp group | the others | only without a sun key | yes | yes |

Gather paths never hit a delta light, so a sun key holds sun bounce and sky
light only. Sun key plus lamp group, without the lamps' texel term, equals one
path of the full transport. Today a scene bakes sun key 0 (its first
directional light's direction recorded) and lamp group 0.

### GPU transport

[`vkr_bake_metal.h`](../../tools/bake/vkr_bake_metal.h) uploads the scene once
(an acceleration structure, corner attributes, materials, base color and
emission textures resampled to 128×128 RGBA16F, the lights, the renderer's DFG
table and a 512×256 equirectangular sky) and gathers cosine-weighted paths per
texel in 65,536-texel command buffers, which stay under the system watchdog.
It is a lightmap subset of the ADR-054 transport:

- a bounce scatters through one cosine lobe whose albedo is the CPU BSDF's
  split-sum specular reflectance R plus the diffuse residual
  (1 − R)(1 − metallic) base color: specular energy is kept but spread
  diffusely;
- a surface's glass fraction passes the path straight through, tinted by base
  color, instead of refracting it;
- cutout and blended surfaces, the shadow walk, light falloff, cones,
  rectangle lights, the shading-normal side rules and Russian roulette follow
  the CPU integrator;
- clearcoat, sheen, subsurface and anisotropy are not modeled.

### Encoding

Each layer page is composed in float RGBA: covered texels take their value,
up to four rings of empty texels take the mean of their filled 8-neighbors
inside the same rectangle, and the rest of the rectangle takes its mean, so
bilinear filtering and blocks at chart edges never read black or another
instance. Pages are encoded with astcenc to ASTC 4×4 in the HDR RGB, LDR alpha
profile at effort 10.

### VKLM file

[`vkr_lightmap_set.h`](../../runtime/src/assets/vkr_lightmap_set.h) defines
VKLM v1: a 128-byte header, the layer table (kind, number, sun direction), the
instance table and, aligned to 256 bytes, one page image per page and layer,
page-major. Every scalar is little-endian. An instance is keyed by its entity's
index in the scene document's entity array and its source-node index in the
entity's cooked model (zero without source nodes), and records its page
rectangle. Instances are sorted by that key. Producers stream the payload and
write the header last; the decoder checks header, table and payload CRCs,
sizes, layer meanings and that every rectangle lies on whole blocks within its
page, and returns the payload as a view into the caller's bytes.

### Bakery and projects

`vkr_bakery bake lightmap --scene <scene.json> --output <set.vklm>`
([`vkr_bakery_bake.c`](../../tools/bakery/vkr_bakery_bake.c)) follows
`bake diffuse`: an inspect pass records the source closure, the
`lightmap-baker` tool bakes into a temporary file, the result is decoded and
the closure re-checked before the file is renamed into place with a
`.bake.json` provenance sidecar; `--check` reports `current` or `stale`. It
accepts `--samples` (64), `--max-depth` (4), `--seed`, `--page-size` (4096) and
`--texels-per-unit` (8), and exits 4 without publishing when no model carries
lightmap UVs.

A managed `bake_scene` request with `bakes.lightmap` and optional
`lightmap_settings` bakes the effective runtime scene into
`builds/<uuid>/lightmaps.vklm`, records an asset of kind and role `lightmap`,
and sets the scene's `lightmaps` block to that asset
([`vkr_project_bake.c`](../../tools/bakery/project/vkr_project_bake.c)).
Lowering replaces the asset reference with a path. A bake without lightmapped
models drops the previous set and warns. Bundles store `.vklm` uncompressed
and mappable under pack loader `VKR_PACK_LOADER_LIGHTMAP`. The Bakery panel's
"Scene lightmaps" option adds the bake to Prepare and to Bake lighting.

## Consequences

- Bistro (2,909 lightmapped instances at 8 texels per meter, three 4096 pages,
  two layers) bakes in 46 s at 16 samples per texel on the M1 Pro and stores
  100.7 MB; each further layer adds 16 MiB per page. Cooking lightmap UVs
  costs about 25 times the cook time of the same model without them.
- Bakes need Metal ray tracing. Windows and Linux hosts cannot bake lightmaps
  until the CPU integrator gains the layer split and texel direct term.
- The GPU baker approximates the CPU BSDF; the measured bias is below 1% of
  sun bounce on Bistro and within sampling noise overall.
- Entity indices key the instance table, so any edit that reorders or inserts
  entities, or changes a model's source nodes, stales the set until the scene
  is baked again with lightmaps selected.

## Alternatives considered

- **CPU-only bake.** 9.4 s per sample per layer on Bistro: 2 to 8 hours for
  twelve layers. Kept as the reference only.
- **One KTX2 file per page or layer.** More files to reference and package;
  the runtime would still need the instance table.
- **BC6H or RGB9E5.** The M1 Pro creates both, as it does ASTC 4×4 HDR. The
  owner chose ASTC 4×4 HDR: 8 bits per texel like BC6H, a quarter of RGB9E5,
  and the format of the Apple and mobile GPUs the tiled pipeline targets.
- **Lightmap UVs for every import.** Cooks 25 times slower and grows meshes
  by about 40% for models the desktop pipeline never lightmaps.

## Evidence

Release build, M1 Pro, 2026-10-05, Bistro (`bistro-lights.gltf`) cooked at 8
texels per meter with deferred textures:

- Cook: 17.7 s and 1.27 GB peak memory with lightmap UVs against 0.7 s and
  0.68 GB without, byte-identical on a second run. All 646 source meshes get
  an atlas; seams raise the vertex count from 1,741,441 to 2,158,263 and the
  artifact from 30.8 to 42.9 MB. The scene packs 2,909 instances onto three
  4096 pages: 32.1 million rectangle texels, 4.28 million inside triangles.
- CPU reference: 8 samples per covered texel through the ADR-054 integrator
  on 8 threads took 74.9 s, 457,000 paths per second.
- Rays, `--gpu trace` at 8 samples: a 706 MB acceleration structure built in
  363 ms; 35.8 million closest-hit rays per second (20.9 to 55.4 million by
  page); mean hit fractions match the CPU BVH within 0.00001.

- Parity, `vkr_bakery tool lightmap-baker --scene <bistro> --gpu gather
  --pages 1 --check-texels 4096 --check-samples 1024`: mean luminance -0.9%
  (z = -0.71, seed 1) and -2.4% (z = -0.97, seed 7) against the CPU
  integrator, per-texel RMS 0.79 and 0.72 of the sampling noise. With
  `--check-transport`: sky and emission +0.1% (z = 0.09); sun -0.85%
  (z = -1.8), of which -0.17% remains with Lambert materials and no glass.
  Each layer on page 0 (1,060,457 texels, 16 samples) takes 6.2 to 7.1 s of
  GPU time, 2.4 to 2.7 million paths per second against 457,000 on the CPU.
- Encoding, `--astc-effort 10`: 1.0 to 1.3 s per 4096 page, RMS log2
  luminance error 0.022 to 0.033 and mean relative error about 1% on 16-sample
  layers; effort 60 took 50 to 60% longer for the same error.
- `vkr_bakery bake lightmap --scene <bistro> --output <set.vklm> --samples 16`:
  65.8 s including both inspections, 6.3 GB peak memory in the baker; the
  sidecar reports 3 pages, 2 layers and 2,909 instances; `--check` reported
  `current`, then `stale` after a scene byte changed.
- `run_lightmap_bake_tests` covers packing, fitted pages, texel rasterization,
  rectangle-bounded fill, the HDR round trip and VKLM round trip and
  rejection. A managed project made from a box import with
  `model_settings`, then `bake_scene` with `bakes.lightmap`, published a
  131 KB set keyed to entity 0 that survived workspace cleanup; a rebuild with
  `model_settings` added lightmap UVs to a model imported without them.

Unavailable: a textured scene through the GPU baker, any runtime use, and a
Windows or Vulkan host.

## Revisit when

The tiled runtime samples lightmaps, the time-of-day system defines sun keys
and lamp groups, or the CPU path gains the layer split.
