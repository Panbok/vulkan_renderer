---
status: partial
updated: 2026-10-06
authority: adr
---

# ADR-088: Baked lightmap sets

## Status

Accepted (partial). Baking, storage, packaging, light mobility and groups,
the editor controls, runtime loading and binding of a set, and sampling in
the tiled pipeline's forward shader
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 8) are
implemented. The toolkit test level and a lightmap-UV Bistro fixture render
with their sets on the tiled pipeline, in the editor too; Bistro bakes
without visible noise at 16 samples once outlier texels are rejected and
each layer is smoothed (see Evidence). The desktop pipeline lights static
and dynamic lights alike. The bake needs
Metal ray tracing.

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
hosts without Metal ray tracing; give each light a mobility and a named group
(static lights bake into their group's layer, dynamic lights never bake; at
most four groups).

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
lights' direct term at the texel. Lightmap and diffuse-volume bakes plan the
same layers ([`vkr_bake_layers.h`](../../tools/bake/vkr_bake_layers.h),
[ADR-054](054-baked-diffuse-volumes.md)), and both assets store them as the
64-byte records of
[`vkr_light_layers.h`](../../runtime/src/assets/vkr_light_layers.h):

| Layer | Lights | Sky | Emission | Direct at texel |
|---|---|---|---|---|
| Sun key | the atmosphere's key light | yes | no | no; the runtime adds the sun |
| Lamp group | the group's static lights | default group, only without a sun key | default group | yes |

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
light bakes none.

### Light mobility and groups

Point and rectangle lights carry `mobility`, `"static"` (the default) or
`"dynamic"`, and `light_group`, a name of at most 31 letters, digits, `_` or
`-`; an empty or absent name is the group `default`
([`vkr_scene_system.h`](../../runtime/src/renderer/systems/vkr_scene_system.h),
`vkr_light_group_name_valid` in
[`vkr_light_layers.h`](../../runtime/src/assets/vkr_light_layers.h)). Scene
documents, the editor overlay (`point_mobility`, `point_group`,
`rectangle_mobility`, `rectangle_group`; older overlays default them), the
Details panel's Baking heading and `vkr_component_set` carry them, and the
scene loader rejects an unknown mobility or an invalid name. A model's own
lights are static members of `default`.

Every bake leaves dynamic lights out, the diffuse volumes of ADR-054
included. Each distinct group among the enabled static lights bakes into one
lamp layer: lamp group 0 is `default`, which always exists and also holds
surface emission and, without a sun key, the sky; the other groups follow in
name order. A scene whose static lights name more than
`VKR_LIGHT_LAYER_MAX_LAMP_GROUPS` (4) groups, `default` included, fails to
bake. The runtime will scale each group's layer by the group's factor
([ADR-090](090-time-of-day.md)), which the desktop pipeline already applies
to the group's lights; a dynamic light is the choice for a light that moves or
changes color.

### GPU transport

[`vkr_bake_metal.h`](../../tools/bake/vkr_bake_metal.h) uploads the scene once
(an acceleration structure, corner attributes, materials, base color,
emission, metallic-roughness and transmission textures resampled to 128×128
RGBA16F, the lights, the renderer's DFG table and a 512×256 equirectangular
sky) and gathers cosine-weighted paths per texel in command buffers of
16,384 texels and 8 samples per texel, each adding its share of the texel's
mean, so a buffer stays near a quarter second of a textured Bistro layer and
under the system watchdog. Each sample's random sequence follows its texel
and sample index. Textured Bistro layers at 64 samples in one buffer per
65,536 texels ran 7 s each and were ended for impacting interactivity, and so
was a buffer of 65,536 texels at 8 samples while a browser drew on the same
GPU. The first layer's gather of each page also returns each texel's ambient
visibility from the same first-bounce rays: one minus the mean occlusion of
its samples, where a first hit within the occlusion radius occludes by its
opacity (one minus its glass fraction) times one minus its distance over the
radius. The radius is one world unit (`--ao-radius` of the lightmap baker,
0 turning it off). The acceleration structure
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
  the CPU integrator; directional lights, the atmosphere's key light among
  them, always cast shadows, as the runtime's sun does
  ([ADR-054](054-baked-diffuse-volumes.md));
- normal maps, clearcoat, sheen, subsurface and anisotropy are not modeled.

### Encoding

Before a page is composed, the bake rejects outlier texels
([`vkr_bake_lightmap_reject_outliers`](../../tools/bake/vkr_bake_lightmap.h)):
a texel with at least three same-surface neighbors, the covered texels among
the eight around it on the page that lie within four texels of it in world
space and face within about 37 degrees of its normal, whose luminance exceeds
twice the brightest of them takes their mean. Rare high-energy paths
otherwise remain as speckles at any practical sample count, and a rejected
texel's own light is lost, about 1% of a layer. One smoothing pass then
gives each texel the mean of itself and its same-surface neighbors, weighted
4 for the texel, 2 for edge and 1 for corner neighbors
(`vkr_bake_lightmap_smooth`), which blurs irradiance by about a texel and
never across a chart seam. `--outlier-ratio` (0 turns rejection off) and
`--smooth <passes>` set them; `--gpu gather` counts each layer's outliers
without replacing or smoothing anything, so its parity check still compares
raw transport.

Each layer page is composed in float RGBA: covered texels take their value,
up to four rings of empty texels take the mean of their filled 8-neighbors
inside the same rectangle, and the rest of the rectangle takes its mean, so
bilinear filtering and blocks at chart edges never read black or another
instance. Alpha is the page's ambient visibility, smoothed like irradiance
but without outlier rejection, in every layer, so whichever layers a frame
weighs carry it; texels outside every rectangle keep alpha one. Pages are
encoded with astcenc to ASTC 4×4 in the HDR RGB, LDR alpha profile at effort
10.

### VKLM file

[`vkr_lightmap_set.h`](../../runtime/src/assets/vkr_lightmap_set.h) defines
VKLM v3: a 128-byte header, the layer table (64-byte light-layer records:
kind, number, sun direction and, for a lamp group, its group name), the
instance table and,
aligned to 256 bytes, one page image per page and layer,
page-major. Every scalar is little-endian. An instance carries its entity's
document id (zero when the entity has none), the entity's index in the baked
entity array (the document's entities, then the World's, then editor-created
ones) and its source-node index in the entity's cooked model (zero without
source nodes and for brushes), and records its page rectangle. The runtime
matches an instance by document id and falls back to the index. Instances are
sorted by index and node. Page alpha is ambient visibility; sets baked
before it carry alpha one, which reads as unoccluded, so v3 is unchanged.
Versions 1 (keyed by index only) and 2 (without group names) were never read
by a runtime. Producers stream the payload and
write the header last; the decoder checks header, table and payload CRCs,
sizes, layer meanings, that lamp groups have distinct valid names and sun
keys none, that a set holds at most four lamp groups, and that every
rectangle lies on whole blocks within its page, and returns the payload as a
view into the caller's bytes.

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
"Scene lightmaps" option adds the bake to Prepare and to Bake lighting; the
Cmd statement `scene.bake lightmaps` adds it without the option and reports
the bake's outcome ([ADR-075](075-editor-cmd-bar-and-evaluator.md)).

### Runtime set

The scene loader reads a `lightmaps` block's `path`
([`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c)),
decodes the set and publishes every layer page as one
`VKR_TEXTURE_FORMAT_ASTC_4x4_HDR` 2D array whose slice
`page * layer_count + layer` is the file's page image, uploaded from the file
bytes in place. Metal uploads a texture larger than 64 MB in consecutive
submissions of whole slices or mips (`vkr_metal_packet_upload_texture`),
because Bistro's 453 MB set exceeds the largest publication slot the
upload ring can grow to. Metal 4 devices always sample ASTC HDR; Vulkan enables
`textureCompressionASTC_HDR` when the device has it and reports
`supports_texture_astc_hdr`, and a device without it keeps the set off.

Runtime brush meshes pack the same lightmap UVs the bake computes for them
(`vkr_brush_lightmap_layout`,
[`vkr_scene_brush.c`](../../runtime/src/renderer/systems/vkr_scene_brush.c));
cooked models carry theirs from the cook.

The scene keeps the layers and instances
([`vkr_scene_lightmaps.c`](../../runtime/src/renderer/systems/vkr_scene_lightmaps.c))
and gives each matched draw its lightmap slot, the instance's index plus one:
a mesh instance or a generated mesh such as a brush. An instance with a
document id matches the entity with that id, the entity itself for instance
zero, else the entity's source node; an instance without one matches the
source node of the document entity at its index. Binding runs after the set
loads and again whenever entities or meshes change. The slot travels in
`VkrInstanceDataGPU.lightmap_slot` and, as an exact float, in the prepared
instance row's `normal_column2.w`. Each frame `VkrFrameLighting.lightmap`
carries the texture, the rectangle table and the layers with nonzero weight:
the two sun keys nearest the current sun and every lamp group at its group's
factor ([ADR-090](090-time-of-day.md)), at most six. The Metal frame root
carries them in `VkrMetalPacketLightmap` with the rectangle table, which only
the tiled pipeline copies; its vertex stage maps the lightmap UVs into the
draw's rectangle and its forward shader sums the active layers.

## Consequences

- Bistro (2,909 lightmapped instances at 8 texels per meter, three 4096 pages)
  bakes nine layers, eight sun keys and a lamp group, in 180 s at 16 samples
  per texel on the M1 Pro and stores 453 MB; each layer takes 16 MiB per
  page, so lamp groups and the texel density set the memory floor. Cooking lightmap UVs
  costs about 25 times the cook time of the same model without them.
- Bakes need Metal ray tracing. Windows and Linux hosts cannot bake lightmaps
  until the CPU integrator gains the layer split and texel direct term.
- The GPU baker approximates the CPU BSDF. Spreading specular reflection
  diffusely ignores where a delta sun's specular lobe points, which darkens
  shadowed sun bounce by about 2% on the blockout; overall light stays within
  sampling noise.
- A level has at most four independently switchable baked light groups;
  further variation needs dynamic lights, which the tiled pipeline's runtime
  light budget bounds.
- Entity indices key the instance table, so any edit that reorders or inserts
  entities, or changes a model's source nodes, stales the set until the scene
  is baked again with lightmaps selected.
- The runtime binds instances only within the scene that owns the set; the
  project World's entities, which the bake includes, stay unbound.
- The texels store irradiance, so a surface's diffuse response is its
  diffuse albedo over π times the blended texel.
- On the toolkit test level (23 brushes, ten layers on one 1024 page), the
  tiled pipeline's view of a room matches the desktop pipeline's, run on the
  Metal desktop implementation since removed, in exposure and tone, with the
  bake's sampling noise
  (`sha256:91f74e06739cb7bcc50bb0a2641235d70437ab2a4d90739b59ab8def310ab80c`
  tiled, `sha256:c55355971b2cb89ba6181ef19285052ae42b1e8280a5170cfe0b051e2b9e8970`
  desktop, 2026-10-06, temporary local cases on a copy of its effective
  runtime scene).

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
  These sun figures, and the Bistro sun-key luminances below, predate sun
  shadows in bakes.
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
  samples before sun shadows: -0.12% overall, sky -0.02%, lamps +0.31%
  (z = 0.95), sun -0.80%. Rebaking produced the same file digest.

- Sun keys: the eight key atmospheres build in 6.8 s. On the blockout each
  key layer bakes in 0.14 s, with mean luminance from 0.16 to 0.35 around
  the circle (that sun never sets: it stays 10 degrees above the horizon at
  its lowest). Bistro with eight keys and lamp group 0 at 16 samples: 180 s,
  131 s of it GPU time and 31 s encoding, 453 MB, 6.4 GB peak memory; key
  luminance 0.28 to 0.59.

- Light groups, on the same blockout through `vkr_mcp`
  (`vkr_component_set`): an invalid group name was refused with the
  validator's message; Lamp B set to group `warm` and Lamp C to `dynamic`
  saved into the overlay. The managed `bake_scene` loaded 3 lights instead of
  4 (the atmosphere key light, Lamp A and Lamp B) and published ten layers:
  eight sun keys, lamp group `default` (Lamp A and emission, mean luminance
  0.073) and `warm` (Lamp B, 0.054), decoded with their names from the VKLM v3
  layer table. CPU tests cover the loader fields and name rejection, the
  overlay round trip of a dynamic light's group, and VKLM name rules.

- Sun shadows: the key light was baked unshadowed until model version 3, so
  the blockout's sealed room, which no opening reaches, held sun-key light of
  mean luminance 0.115 to 0.160 per key, and the eight keys' level means were
  0.20 to 0.41. With the sun shadowed the sealed room holds 0 in every layer
  and the key means fall 18 to 20% (0.16 to 0.35). Unshadowed, the sun-key
  layers also differed between runs (three digests in six); shadowed, three
  runs matched. Parity at 2,048 samples, seeds 1 and 7: overall -0.07% and
  -0.11% (z = -1.3 and -2.1), sky -0.02%, sun -1.94% and -1.65% (z = -7.6
  and -6.4, per-texel RMS 1.6 and 1.5 of the noise). Starting the GPU shadow
  ray as the CPU does did not change the sun difference; with every brush
  material's dielectric specular set to zero it fell to -0.13% and +0.20%
  (z = -0.5 and 0.8, RMS 1.0 and 0.97), so it comes from the diffusely spread
  specular lobe.

- Bistro on the tiled pipeline, 2026-10-06: the model cooked with lightmap
  UVs by `vkr_bakery tool mesh --input assets/models/bistro-lights.gltf
  --output assets/models/bistro-lights-lightmapped.vkb
  --lightmap-texels-per-unit 8` and the six string-light ranges of
  `assets/bakery.json` (26.0 s, 3.0 GB peak; the default texture tier keeps
  the shared materials, which `--texture-tier deferred` would strip of their
  textures), then `vkr_bakery bake lightmap --scene
  assets/scenes/fixtures/bistro_tiled_local.scene.json --output
  assets/scenes/bistro_tiled.vklm --samples 16`: 578.6 s, 408.6 s of it GPU
  time and 37.0 s encoding, 5.6 GB peak, 453 MB, eight sun keys and lamp
  group `default` on three pages
  (`sha256:3aebcd4a95e5f98ef25c5521f1df0b8e341776633758f90759dd71b919ca8909`).
  The set loads, binds all 2,909 instances and renders
  (`tiled_bistro_baked_capture`); its frame cost is in
  [ADR-087](087-gpu-class-graphics-pipelines.md#baked-bistro-measurement).
  At 16 samples the set shows bright single-texel speckles on walls and
  pavement; the desktop capture of the same view, on the Metal desktop
  implementation since removed, lights the lamps analytically and shows none.
  The same bake with ambient visibility in every layer's alpha, after the
  gather's command buffers shrank to 16,384 texels (2026-10-06, `--samples
  16`, a browser drawing on the same GPU): 734.3 s, 555.5 s of it GPU time
  and 36.6 s encoding, 5.6 GB peak, 453 MB
  (`sha256:b508f705f7deaad5b2152d0be8e7bfd6af20d33478ca94f177fad1f3f8ead3c7`).
  Rendered by the tiled pipeline against the previous set on the same build,
  `tiled_bistro_baked_capture` darkens environment specular in window
  frames, door insets and alcoves, 237,979 pixels by more than 2 of 255 with
  a mean of 99.02 against 99.37
  (`sha256:72215c6c1bdf16bfe51b019cfd646fcffd32fd73cc5191be3ed294e92dcf3ba3`,
  `sha256:48630f36075db11aca4a79c31bbfbcf792e621b0c10632737ac502c40f3740d3`),
  with no visible block artifacts.
  The same bake at 64 samples, after the gather
  split its samples into runs of eight (`--samples 64`: 1,819.4 s, 1,643.1 s
  of it GPU time and 37.7 s encoding, 6.7 GB peak,
  `sha256:ae552816713073148735a6ce8d6cc2b92a21650d48ecc4dbd2927293f9938056`),
  reproduces the 64-sample mean luminance of the first layer (0.038691) and
  turns the speckles into more numerous, dimmer blotches
  (`tiled_bistro_baked_capture`,
  `sha256:15a6bf11227b53f2cf9e7f168dab9bbd1cb06135f15db88dffb17c1446d3b37a`).
- Bistro speckle sources, `vkr_bakery tool lightmap-baker --scene
  assets/scenes/fixtures/bistro_tiled_local.scene.json --gpu gather --pages 1
  --check-texels 0 --samples 64`, first page: surface emission alone has mean
  luminance 0.0018 against 0.221 for the lamp lights alone, so the bulbs'
  emissive surfaces are not the source. Texels brighter than twice their
  brightest same-surface neighbor are 2.4% of the sun-key layer, carrying
  0.44% of its light, and 1.0% of the lamp layer, carrying 1.3%; at four
  times, 16 samples flag 19,939 sun-key texels and 64 samples 16,499, so the
  count falls far slower than the noise. With rejection the 64-sample set
  loses its bright dots but keeps faint ones
  (`sha256:d87e27d60edf00ebed67fd96fa06946d2d12dc4f5f264dfe5703099c4739c197`);
  16 samples with rejection and one smoothing pass (423.1 s GPU time, 35.4 s
  encoding; set
  `sha256:a276a3501d1b7aa676e27f436a2936f0c085b7432872aa7c295a345e6be959d0`)
  render without visible noise
  (`sha256:77a108b3ab6558f3d540f252939712011a301a6c642f3f0585cfb9e59a96ca5f`).
  The fixture uses that set.

Unavailable: a Windows or Vulkan host.

## Revisit when

Smoothing visibly softens light detail a level needs at its texel density,
outlier rejection removes real single-texel light such as a beam through a
small opening, a level needs more than four baked groups, the spread
specular lobe's sun bias shows against the art-level contract, or the CPU
path gains the layer split.
