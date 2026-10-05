---
status: partial
updated: 2026-10-05
authority: adr
---

# ADR-088: Baked lightmap sets

## Status

Accepted (partial). Baking, storage, packaging and the editor controls are
implemented. The runtime does not sample lightmaps yet (the scene loader
ignores the `lightmaps` block), lights have no groups yet so every lamp bakes
into lamp group 0,
and the bake needs Metal ray tracing.

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

### Brushes

A solid or visual brush is lightmapped from its geometry alone
([`vkr_brush.h`](../../runtime/src/level/vkr_brush.h),
`vkr_brush_lightmap_layout`): every face is a chart projected on an
orthonormal basis of its plane at 8 texels per brush unit with two texels of
padding, shelf-packed tallest first; a brush whose atlas would pass 1,024
texels halves its density. The bake and the runtime compute the same UVs
from the same planes, so brushes need no cooked lightmap data.

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
| Sun key | the atmosphere's key light | yes | no | no; the runtime adds the sun |
| Lamp group | every other light | only without a sun key | yes | yes |

Gather paths never hit a delta light, so a sun key holds sun bounce and sky
light only. Sun key plus lamp group, without the lamps' texel term, equals one
path of the full transport.

A scene with an atmosphere bakes eight sun keys on the sun's daily circle
(owner decision 2026-10-05): key k turns the authored sun by k × 45 degrees,
right-handed, about the atmosphere's `celestial_pole`, so the sun keeps its
angle to the pole. Each key rebuilds the atmosphere for its sun
(`vkr_bake_scene_build_sun_atmosphere`): the sky the key's paths escape to
and the key light, the sun while it lights the observer, else the moon
(ADR-081). A key records its sun direction; the runtime blends the two keys
nearest the current sun along the circle. A scene without an atmosphere bakes
one sun key from its directional lights, and a scene without a directional
light bakes none. Lamp group 0 holds every light that is not the
atmosphere's key light.

### GPU transport

[`vkr_bake_metal.h`](../../tools/bake/vkr_bake_metal.h) uploads the scene once
(an acceleration structure, corner attributes, materials, base color,
emission, metallic-roughness and transmission textures resampled to 128×128
RGBA16F, the lights, the renderer's DFG table and a 512×256 equirectangular
sky) and gathers cosine-weighted paths per texel in 65,536-texel command
buffers, which stay under the system watchdog. The acceleration structure
holds two geometries: opaque triangles, and cutout triangles whose lowest
alpha can fall below their cutoff. Rays test those as candidates in one
traversal; blended and tinting surfaces stop the traversal and are resolved
after it, because their pass must be drawn once per surface. A shadow ray
first accepts any stop, and walks its segment in order only when that stop
tints. It is a lightmap subset of the ADR-054 transport:

- a bounce scatters through one cosine lobe whose albedo is the CPU BSDF's
  split-sum specular reflectance R plus the diffuse residual
  (1 − R)(1 − metallic) base color: specular energy is kept but spread
  diffusely;
- a surface's glass fraction passes the path straight through, tinted by base
  color, instead of refracting it;
- cutout and blended surfaces, the shadow walk, light falloff, cones,
  rectangle lights, the shading-normal side rules and Russian roulette follow
  the CPU integrator;
- normal maps, clearcoat, sheen, subsurface and anisotropy are not modeled.

### Encoding

Each layer page is composed in float RGBA: covered texels take their value,
up to four rings of empty texels take the mean of their filled 8-neighbors
inside the same rectangle, and the rest of the rectangle takes its mean, so
bilinear filtering and blocks at chart edges never read black or another
instance. Pages are encoded with astcenc to ASTC 4×4 in the HDR RGB, LDR alpha
profile at effort 10.

### VKLM file

[`vkr_lightmap_set.h`](../../runtime/src/assets/vkr_lightmap_set.h) defines
VKLM v2: a 128-byte header, the layer table (kind, number, sun direction), the
instance table and, aligned to 256 bytes, one page image per page and layer,
page-major. Every scalar is little-endian. An instance carries its entity's
document id (zero when the entity has none), the entity's index in the baked
entity array (the document's entities, then the World's, then editor-created
ones) and its source-node index in the entity's cooked model (zero without
source nodes and for brushes), and records its page rectangle. The runtime
matches an instance by document id and falls back to the index. Instances are
sorted by index and node. Version 1, keyed by index only, was never read by a
runtime. Producers stream the payload and
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

- Bistro (2,909 lightmapped instances at 8 texels per meter, three 4096 pages)
  bakes nine layers, eight sun keys and a lamp group, in 180 s at 16 samples
  per texel on the M1 Pro and stores 453 MB; each layer takes 16 MiB per
  page, so lamp groups and the texel density set the memory floor. Cooking lightmap UVs
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

- Textured Bistro, imported through a managed `create_scene` with
  `texture_tier` `preview` and `model_settings` at 8 texels per unit (39 s):
  389 texture layers and 951,845 alpha-tested triangles. Parity of every lamp
  and emission (the scene has no sun) on page 0 at 1,024 samples: +0.34%
  (z = 0.19); emission alone -0.03%. Before the metallic-roughness texture was
  sampled, lamp light was 18% dark, because Bistro's materials keep glTF's
  metallic factor of one and take metalness from the texture. One layer page
  takes 21.4 s at 16 samples (0.79 million paths per second, against 2.17
  million untextured): rays pass through stacked foliage cards, and before
  cutouts were tested in one traversal it was 5.6 times slower than with every
  cutout forced opaque. `vkr_bakery bake lightmap --samples 16` published the
  set in 116.7 s, 65.6 s of it GPU time and 4.9 s encoding.

- A blockout built in the editor through `vkr_mcp` (`vkr_blockout_room` and
  `vkr_blockout_doorway`: two rooms joined by a doorway, a sealed room and a
  ground slab, three point lights, the World sun) and baked with a managed
  `bake_scene` (`bakes.diffuse` and `bakes.lightmap`, 64 samples): 276
  triangles, 23 lightmapped brushes on one 1,024 page, keyed by their
  document ids; the lightmaps published in 3.4 s and the volume (39 valid
  probes, 7 valid cells, two regions) in 5.2 s. Parity on that level at 2,048
  samples: -0.12% overall, sky -0.02%, lamps +0.31% (z = 0.95), sun -0.80%.
  Rebaking produced the same file digest.

- Sun keys: the eight key atmospheres build in 6.8 s. On the blockout each
  key layer bakes in 0.14 s, with mean luminance from 0.20 to 0.41 around
  the circle (that sun never sets: it stays 10 degrees above the horizon at
  its lowest). Bistro with eight keys and lamp group 0 at 16 samples: 180 s,
  131 s of it GPU time and 31 s encoding, 453 MB, 6.4 GB peak memory; key
  luminance 0.28 to 0.59.

Unavailable: any runtime use, and a Windows or Vulkan host.

## Revisit when

The tiled runtime samples lightmaps, the time-of-day system defines sun keys
and lamp groups, or the CPU path gains the layer split.
