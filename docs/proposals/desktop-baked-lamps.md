---
status: proposed
updated: 2026-10-07
authority: proposal
---

# Baked static lamps on the desktop pipeline

The desktop pipeline (Vulkan) shades and shadows every static lamp at runtime.
The tiled pipeline bakes the same lamps into lightmaps
([ADR-088](../adr/088-baked-lightmap-sets.md)). This proposal bakes static
lamps on the desktop pipeline too, for performance: their direct light,
shadows and bounce come from the lamp layers of the scene's lightmap set, and
lamps with `dynamic` mobility stay at runtime. It records the owner's
decisions, the design, the remaining choices and the evidence needed to
accept it. No part of it is implemented.

## Owner decisions (2026-10-07)

1. **Full bake.** A static lamp's direct light, shadows and bounce come from
   the lightmap. Its light changes only through its light group's factor
   ([ADR-090](../adr/090-time-of-day.md)). A lamp that moves or changes on its
   own is `dynamic`.
2. **Dominant direction.** Each lamp layer gains a direction page beside its
   irradiance, so baked lamp light responds to normal maps and gives an
   approximate highlight.
3. **Moving objects take lamp light from the probe volume.** The lamp layers
   of the [sparse diffuse volume](sparse-diffuse-volumes.md) gain the lamps'
   direct light.
4. **Moving casters shadow the nearest lamps.** Moving objects cast shadows
   from the few nearest static lamps onto baked surfaces.

## Baseline and expected saving

Static lamps run through the point-light list, the world light grid, local
shadow selection and `Shadow.LocalMask`
([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)). The runtime
removes static lights from that list only on the tiled pipeline with a loaded
set (`vkr_standard_scene_runtime_static_lights_baked` in
[`vkr_standard_scene_runtime.c`](../../runtime/src/application/vkr_standard_scene_runtime.c),
read by
[`vkr_lighting_system.c`](../../runtime/src/renderer/systems/vkr_lighting_system.c)).

The [lighting efficiency](lighting-efficiency.md#vulkan-cost-split-2026-10-03)
cost split bounds the saving. It was measured with Vulkan Release on the
RX 6700 XT, in the Bistro street view at 1920×1080 with TAA and the High
preset, by compiling out each part. It is a local, non-authoritative
measurement.

| Lamp-related part | ms |
|---|---:|
| `Shadow.LocalMask`, all of it | 2.82 |
| Mask reads in `Lighting.Deferred` | 0.73 |
| Punctual light loop and BRDF | 0.83 |
| Light-contribution counters and inline filtering past eight slots | 0.25 |
| **Total** | **4.63** |

The GPU frame took about 11 ms, so the lamps were about 40% of it. The
per-pixel parts grow with resolution. The proposal adds work back: lightmap
sampling, the larger lightmapped model and the moving-caster shadows. None
of these is measured on Vulkan.

Constraints of the current set:

- **Encoding.** Pages are ASTC 4×4 HDR. RDNA2 and Ampere do not sample ASTC,
  so Vulkan keeps the set off on them (ADR-088). The repository has no BC6H
  encoder; `bc7enc_rdo` encodes BC7, not BC6H. Its BC7 and BC5 encoders build
  only on x86-64 hosts
  ([`vkr_bc7e.cmake`](../../cmake/vkr_bc7e.cmake)), while lightmaps bake only
  on the Mac, so the bake host cannot write BC pages today.
- **Bake host.** The lightmap bake needs Metal ray tracing. A Windows host
  cannot bake until the CPU integrator gains the layer split and the texel
  direct term (ADR-088).
- **Model.** Bistro must use the model cooked with lightmap UVs. Its seams
  raise the vertex count by 24%, from 1,741,441 to 2,158,263.

The desktop pipeline needs only the lamp-group layers. The sun stays at
runtime with its cascades, and the sparse volume holds its bounce. Bistro has
one lamp group, so its desktop lamp data is 3 pages of 16 MiB, 48 MiB, against
453 MB for the full tiled set.

## Design

### Bake and data

- The existing `vkr_bakery bake lightmap` keeps baking the lamp layers: a
  lamp group's static lights with their direct term at each texel, their
  bounce and, in the default group, surface emission.
- **Direction pages.** For each lamp layer, the bake also stores each texel's
  luminance-weighted mean incident direction. The length of that mean is the
  texel's directionality: 1 when one lamp dominates, near 0 for light from
  all sides. The direct term contributes its exact light directions, and
  bounce paths contribute their first directions. The pages are BC7 for
  Vulkan and ASTC 4×4 LDR for Metal, 1 byte per texel each: 48 MiB for
  Bistro.
- **Irradiance encoding.** Lamp-layer pages gain a BC6H encoding. One bake
  writes both encodings, with an encoder that runs on the bake host (open
  choice 2). Packaging keeps the encoding that the target platform samples
  (open choice 3).
- **VKLM v4** adds the direction pages and the BC6H lamp pages, and its
  layer table records which encodings each layer carries. The decoder checks
  their sizes, block alignment and CRCs as for v3 pages.

### Runtime light selection

`vkr_standard_scene_runtime_static_lights_baked` becomes true on the desktop
pipeline when the scene's set carries lamp pages in an encoding the device
samples. Static lamps then leave the point-light list, the world grid,
contribution ranking and local shadow selection, as they do on the tiled
pipeline; only the K lamps of the moving-caster shadows keep shadow faces. A
scene without a set, or with a set that the device cannot sample,
keeps runtime lamps, so lamps still show before a bake. Dynamic lamps are
unchanged.

### Static receivers

- **G-buffer resolve.** For a draw with a lightmap slot, `pass.gbuffer.resolve`
  decodes the lightmap UV (`vkr_decode_packed_lightmap_uv` in
  [`gpu_draw.slangh`](../../renderer/src/shaders/shared/gpu_draw.slangh)),
  maps it into the draw's rectangle, and sums the active lamp layers weighted
  by their group factors. It writes two images: the baked irradiance in
  `R16G16B16A16_SFLOAT` and the weighted direction and directionality in
  `R8G8B8A8_UNORM`. That is 12 bytes per pixel, 24.9 MB at 1920×1080 and
  44.2 MB at 2560×1440 for each instance of the images. A packed 11-11-10
  float format, which the graph's format table does not have yet, would cut
  it to 8 bytes.
- **Deferred lighting** shades the baked light as one light per pixel. The
  directional part acts as a light from the dominant direction, so normal maps
  modulate it and the material's BRDF gives its highlight. The rest is
  diffuse without direction. GTAO does not darken it, because the bake already
  holds the lamps' occlusion, as on the tiled pipeline (ADR-087, decision 8).
- **Forward and transmission shading** of a lightmapped draw sample the lamp
  layers themselves. Draws without a lightmap follow the next section.
- **SSGI** loses static lamp light from its direct source. The lightmap
  already holds that bounce.

### Moving receivers

The sparse volume's lamp layers add each static lamp's direct light at every
probe, which a shadow ray from the probe's position tests. The volume
composes its sun-key part and its lamp part separately. A lightmapped receiver
reads only the sun part, because its lightmap already holds the lamp bounce.
A receiver without a lightmap reads both. The second part adds 32 bytes per
probe and four reads per probe for those receivers only. The light is soft at
the probe spacing, and no lamp shadow falls on the moving object itself.

### Shadows of moving casters

For each frame, select at most K static lamps (open choice 5), nearest to
the camera, whose range holds a moving caster that is in view or casts into
view. For each selected lamp:

- one set of faces holds static casters only. It renders once and stays in
  the existing face cache;
- a second set holds moving casters only, redrawn each frame.

On a static receiver in the lamp's range, deferred lighting computes the
lamp's analytic light `L` and the two visibilities `v_static` and `v_moving`.
It then subtracts `L · v_static · (1 − v_moving)` from the baked light,
clamped at zero. The `v_static` term stops the subtraction from darkening a
point that the bake already shadows. When no moving caster is in a lamp's
range, the lamp is not selected and costs nothing. Item 3 of
[lighting efficiency](lighting-efficiency.md#proposed-changes), static and
dynamic atlas layers, provides the face split.

## Relation to other proposals

- [Local shadow architecture](local-shadow-architecture.md) still serves
  dynamic lamps and unbaked scenes. A baked scene keeps only the faces of the
  K selected lamps.
- [Sparse diffuse volumes](sparse-diffuse-volumes.md) supplies the moving
  receivers' lamp light. Its phases 2 and 3 come before phase 4 below.
- The tiled pipeline is unchanged. Adopting the direction pages there is a
  separate decision with its own budget (open choice 6).

## Phases

1. **Measure the saving first.** The bake also writes each lamp layer
   uncompressed as RGB9E5, a bit packing that needs no encoder: 192 MiB for
   Bistro. VKLM v4 starts here, with an encoding recorded per layer page.
   The runtime does not decode ASTC; it leaves out `astcenc` by design
   ([`vkr_ktx_read.cmake`](../../cmake/vkr_ktx_read.cmake)). The desktop
   runtime removes baked static lamps, the G-buffer resolve writes the
   irradiance image, and lighting adds it as diffuse light. Moving objects
   receive no static lamp light until phase 4. Profile on the Windows host.
2. **Direction pages:** the bake, the direction image and the
   dominant-direction shading. Metal pages are ASTC LDR. Until phase 3,
   Vulkan pages are uncompressed `R8G8B8A8_UNORM`: 192 MiB for Bistro.
3. **BC6H and BC7 pages** from an encoder on the bake host, and
   per-platform packaging. They replace the RGB9E5 lamp pages of phase 1
   and the uncompressed direction pages of phase 2.
4. **Volume lamp light** for moving receivers, after the sparse volume's
   runtime.
5. **Moving-caster shadows** of the K nearest lamps.

## Open choices

| # | Choice | Options | Recommendation |
|---|---|---|---|
| 1 | Where static receivers sample lamp layers | The G-buffer resolve writes two images; deferred lighting reconstructs the lightmap UV | The resolve: it already holds the triangle and barycentrics, and lighting would repeat the reconstruction. |
| 2 | Desktop page encoding | A BC6H and BC7 encoder that builds on arm64 and x86-64; the ISPC encoders also built for arm64; uncompressed RGB9E5 and `R8G8B8A8_UNORM` | An encoder that builds on both hosts: 1 byte per texel against 4, written where the bake runs. A new third-party encoder needs owner approval. Building ISPC for arm64 needs a check of ADR-077's byte-identical output across hosts. |
| 3 | Encodings in project files | One VKLM with both, stripped per platform at packaging; one set per pipeline class | One file: one bake and one staleness check. |
| 4 | Lamp texel density | 8 texels per metre, as today; 16 near lamps | 8 first. Lamp shadows are soft at 12.5 cm texels with one smoothing pass. Raise the density only where a capture shows the loss. |
| 5 | Moving-caster lamps K | 2; 4 | 2, as many lamps as ADR-019 gives the nine-tap filter. |
| 6 | Direction pages on the tiled pipeline | Adopt; keep irradiance only | Decide after phase 2, measured against the tiled budget. |

## Evidence needed to accept

- **Cost.** Matched capture-free Release Vulkan profiles of the Bistro
  street view of `local_shadow_bistro_vulkan_street` at 1920×1080 and
  2560×1440 on the RX 6700 XT, comparing three states:
  - the current model with runtime lamps;
  - the lightmapped model with runtime lamps, which isolates the vertex cost;
  - the lightmapped model with baked lamps.

  Record the GPU frame, `GBuffer.Resolve`, `Lighting.Deferred` and
  `Shadow.LocalMask`.
- **Output.** Vulkan night captures of the same view with runtime and with
  baked lamps, for owner review of lamp pools, shadow softness, normal-map
  response and highlights.
- **Moving objects.** The mannequin in Bistro at night: lit by the volume's
  lamp layers, and casting shadows from the nearest lamps onto the pavement.
  Record the cost with and without a moving caster in range.
- **Memory.** GPU memory of the lamp and direction pages and of the two
  G-buffer images on Vulkan.
- **CPU tests.** The VKLM v4 round trip and rejection, the baked-lamp
  condition for each pipeline class and encoding, and the projection of a
  point lamp's direct light into probe SH against its analytic irradiance.
- **Tiled.** `tiled_bistro_baked_native` unchanged, while its pipeline does
  not read the new pages.

Unavailable from the macOS host: every Vulkan output and timing gate. A
Windows bake stays unavailable until ADR-088's CPU path exists.
