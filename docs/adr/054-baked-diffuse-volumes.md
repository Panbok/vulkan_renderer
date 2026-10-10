---
status: partial
updated: 2026-10-09
authority: adr
---

# ADR-054: Baked diffuse volumes

## Status

Accepted (partial). The sparse brick volume replaced the room-proof grid on
2026-10-09. The desktop pipeline (Vulkan) bakes, loads, composes and samples
it, with native evidence on Bistro. The tiled pipeline's composition and lookup
are written but have not been compiled or run on Metal. Metal has no probe
gather, so a Mac bakes volumes with the CPU integrator. Light paths are not
yet shared across layers.

## Context

Global environment SH and local reflection probes describe diffuse irradiance
but cannot keep light from crossing walls. The renderer needs static,
scene-local, multi-bounce diffuse light without per-frame ray tracing.

Until 2026-10-09 a volume was one uniform grid of at most 256 probes. It
proved room membership per cell by voxelizing the scene and flood-filling the
outside. Open scenes such as Bistro found no valid cell. A 350 × 30 × 250 m
indoor level fitted cells larger than its rooms. The grid could not place
probes densely near surfaces and sparsely in open air.

## Decision

### Offline bake

A managed project bakes the scene the runtime loads
([`vkr_project_bake.c`](../../tools/bakery/project/vkr_project_bake.c)). It
takes the lowered document with its authored overrides, then the project
World's entities, then the overlay's editor-created entities, whose light
edit values become the scene's light blocks. A hidden created entity and its
subtree stay out, and so do dynamic lights, which no bake holds
([ADR-088](088-baked-lightmap-sets.md)).

The bake scene loader builds solid and visual brushes and blockout shapes as
the runtime does ([ADR-084](084-agent-channel-and-level-design-toolkit.md)).
It leaves out every brush and shape that a `mover` on the entity or an
ancestor moves, so a closed door does not stop baked light between rooms. It
reads the `atmosphere`, `environment` and `subsurface` blocks at the top level
of the document. When the scene has none of its own, it takes the component of
the scene's entities or else the World's, as the runtime does.

Every baked light casts shadows, whatever its `casts_shadow`: that flag prices
runtime shadow maps, while a baked light reaches the screen only through its
bake. A punctual light's shadow ray stops `min(5 cm, 1% of its range)` short of
the light. The runtime's local shadow maps clip that span as their near plane
([`vkr_local_shadow_system.c`](../../runtime/src/renderer/systems/vkr_local_shadow_system.c)),
so a bulb or socket mesh around an imported lamp does not shadow its own lamp.
Before 2026-10-09 every Bistro lamp baked fully occluded, and only noisy
bounce escaped its lantern.

**Placement** ([`vkr_bake_bricks.h`](../../tools/bake/vkr_bake_bricks.h)).
Probes sit in 4 × 4 × 4 bricks over up to three levels.
- Level 0 has spacing `s` (`--spacing`, 1 m by default); each level triples
  it.
- The grid starts half a spacing below the scene bounds. It is covered by
  indirection entries of `3s`, and its dimensions are whole top-level
  blocks.
- A block refines its 27 children wherever blocking geometry lies within
  `--margin` spans of a child. A child that is not refined shares one brick
  of the parent's level.
- Probes are deduplicated by level and lattice position, so face probes that
  meet a finer level are filled from the coarse brick's corners.

**Validity and relocation.** Each probe traces an octahedral 8 × 8 map with
16 stratified rays per texel.
- If more than a quarter of the hits are back faces, the probe is inside
  geometry. It moves up to 0.45 of its spacing in three steps: through the
  nearest back face plus 0.1 spacing, or away from a front face closer than
  0.1 spacing.
- A probe that stays inside is invalid.
- A brick whose probes are all invalid is dropped.
- The same rays write the probe's distance moments (mean and mean square,
  clamped to two spacings) into a 10 × 10 RG16F tile: the 8 × 8 interior and a
  border that repeats the opposite edge, so bilinear filtering wraps across
  the octahedron's seams.

**Light layers.** A volume holds one L1 SH set per probe for each light layer
([`vkr_bake_layers.h`](../../tools/bake/vkr_bake_layers.h)):
- in an atmosphere scene, one per sun key, baked under that key's atmosphere
  with the key light and the sky;
- then one per static light group, holding the group's bounce.

**Lamp direct bands.** No gathered path hits a punctual light. Each lamp group
therefore also gets a direct band: every light's analytic irradiance at the
probe, shadowed as above, projected into L1
(`vkr_bake_integrator_direct_l1`). A light of normal-incidence irradiance `E`
from direction `w` gives constant `E / 4π` and linear `E w / 2π`. The runtime
adds the band only for surfaces whose lightmap does not hold the lamps
([ADR-104](104-desktop-baked-lamps.md)).

**Transport.**
- The default is `vkr_bake_gpu` with Vulkan ray queries
  ([`vkr_bake_vulkan.cpp`](../../tools/bake/vkr_bake_vulkan.cpp)).
- `--cpu` selects the CPU integrator
  ([`vkr_bake_integrator.h`](../../tools/bake/vkr_bake_integrator.h)), and
  `--gpu-parity` compares the two.
- The CPU integrator keeps the bounded photon estimate for caustics after
  specular or thick glass chains. The GPU gather has none.
- In the CPU integrator, a path that exceeds the transparent-layer limit
  ends dark, and a shadow walk that exceeds it returns no light.
- Each probe's paths are seeded from its lattice position, so a volume is
  byte-identical for any thread count ([ADR-077](077-asset-build-system.md)).
- SH projection is L1 only, as `E/π` with the cosine lobe's band-1 transfer
  of 2/3 (`vkr_bake_sh_project_l1`).

### Portable artifact

`DVOL` v3 ([`vkr_diffuse_volume.h`](../../runtime/src/assets/vkr_diffuse_volume.h))
is explicit little-endian; native structs are never serialized. It holds:
- a 128-byte header, with the lamp direct band count at byte 116 and the SH
  scale at byte 112;
- the layer table of 64-byte light-layer records
  ([`vkr_light_layers.h`](../../runtime/src/assets/vkr_light_layers.h));
- indirection entries, each `(level << 30) | brick` or empty (`0xFFFFFFFF`);
- brick records;
- per probe, a 3-half relocation offset and a validity half;
- the moment tiles;
- the SH bands. A band is three half4 rows (red, green, blue: linear x, y, z,
  then constant), divided by a power-of-two `sh_scale`.

Sections are 16-byte aligned. A volume holds at most 13,104 bricks (the
runtime's moment atlas) and 4,194,304 entries. The decoder rejects a malformed
layout, CRC mismatches, an invalid layer table, entries that name a missing
brick or another level, misaligned bricks, offsets beyond half a spacing,
validity other than 0 or 1, non-finite halves, and a direct band count other
than 0 or the lamp-group count. Versions 1 and 2 are refused.

[`vkr_bakery bake diffuse`](../../tools/bakery/vkr_bakery_bake.c) writes an
inspect manifest, records the dependency closure, recipe and tool digest,
verifies the output with the runtime decoder and writes the `.vkdv.bake.json`
sidecar. `--check` rejects stale inputs or corrupt output. A placement without
bricks exits 3 without output.

### Runtime

A scene's `diffuse_volume.path` is prepared on a worker
([`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c))
into five scene-owned textures:
- R32_UINT indirection;
- RGBA16F probe records, 1,024 probes per row;
- the RG16F moment atlas, 512 tiles per row;
- an immutable RGBA16F texture of every SH band;
- a writable RGBA16F composed-SH texture with two row bands.

A volume is used only once all five publications are confirmed and their
uploads have completed; until then the frame lights without it.

The scene weighs the layers ([ADR-090](090-time-of-day.md)): the two sun keys
nearest the current sun share weight one, and each lamp group takes its light
group's factor. When a weight moves by more than one percent, at most every
0.25 s, the composition revision advances (`vkr_scene_update_diffuse_volume`).
`DiffuseVolume.Compose`, a compute pass in both render graphs, then writes the
weighted sum into the composed texture's first band. On a frame that samples
baked lamps, the active lamp groups and their direct bands go to the second
band instead.

**Lookup** ([`diffuse_volume_kernel.slangh`](../../renderer/src/shaders/shared/diffuse_volume_kernel.slangh),
`packet_diffuse_volume_response`):
- An empty entry, or a total weight below 0.001, keeps the environment path.
- The receiver moves `0.6 ×` its level's spacing along `0.2 n + 0.8 v`, where
  `n` is the normal and `v` points toward the viewer.
- The eight corners of its brick cell weigh trilinear × back-face
  (`facing²`) × Chebyshev visibility to the sixth power. Weights below 0.2 are
  crushed.
- The weighted L1 evaluated at the normal replaces only diffuse indirect
  light. Environment and probe specular stay, and so does ambient occlusion.
- SSGI does not add bounce on covered pixels.

**On the desktop pipeline** the lookup runs outside deferred lighting
([`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c)):
1. `DiffuseVolume.Sample` looks the volume up once per 2 × 2 block, at the
   block's first surface pixel. It stores that pixel's normal and camera
   distance as a guide; the distance is negative where its lightmap holds the
   baked lamps.
2. `DiffuseVolume.Upsample` weights the bilinear half-resolution texels by
   `saturate(n · n_t)⁸` and by a camera-distance tolerance of 5 %. It lists
   each pixel whose weight is below 0.05.
3. `DiffuseVolume.Fallback` looks those pixels up exactly in one indirect
   dispatch.

Deferred lighting reads the result per pixel. Transmission shading and SSGI
call the lookup inline. The tiled pipeline calls it inline in
`Tiled.Opaque`. Frame roots carry the textures, origin, spacing, `sh_scale`,
moment rows, lamp band offset and dimensions
([ADR-044](044-shader-cross-backend-contract.md) owns the ABI checks).

## Consequences

- Static geometry, material, light or layer changes need a rebake.
- The desktop lookup interpolates at half resolution. It is exact only at
  silhouettes and on thin geometry.
- The volume does not supply dynamic indirect light, sharp caustics or runtime
  glass transport.
- L1 loses the quadratic band's directional detail.
- Moments trade some light near thin occluders for leak control. At the
  previous 6 × 6 interior, a probe just past a thin roof saw its hits and
  escapes in one texel, and light leaked through.

## Alternatives considered

- **Keep the room proof as a hard mask.** It rejects open space and keeps a
  second representation.
- **Validity alone.** It leaks through thin facades.
- **L2 SH.** It needs seven reads per probe instead of three. At 1 m spacing,
  variation across space dominates angular detail.
- **Look the volume up inside deferred lighting.** Lighting rose 1.18 ms at
  1440p; one corner alone cost 0.53 ms.
- **A full-resolution pass.** It cost 0.83 ms.
- **A single upsample pass with an inline exact fallback.** The fallback's
  registers cost 0.24 ms.
- **The upsample inside lighting.** Lighting rose 0.47 ms.
- **Packed RGB9E5 images.** No gain over RGBA16F.

## Evidence and remaining checks

CPU tests (`run_lightmap_bake_tests`, 2026-10-09):
- the DVOL v3 round trip and its rejections, including a direct band count
  that does not match the lamp groups;
- brick placement and relocation on a room fixture with a 2 m column;
- the L1 Lambert furnace: emission 1 with albedo 0.5 gives exactly 1 at depth
  1 and 1.75 at depth 3;
- `test_point_lamp_direct_l1`: a point lamp 2 m above a probe projects to the
  analytic constant and linear terms. A 3 cm socket box around the lamp does
  not shadow it; a wall between them does.

Leak fixture: three rooms, room C sealed under a 0.2 m roof. An emulation of
the lookup over the decoded volume found light on 88 % of room C's lit floor
with 6 × 6 moments and bias 0.3. With the shipped 10 × 10 moments, bias 0.6
and the sixth power, it found light on 0.27 %. The Vulkan render of room C is
black: its brightest pixel is 6/255.

Bistro, Windows (Ryzen 5 2600, RX 6700 XT, Release), 2026-10-09:
- **Bake.** Spacing 1 m, three levels, margin 1:
  - Placement: 7,991 bricks (7,537 / 416 / 38 per level), 236,786 probes,
    18.7 s.
  - Visibility: 194,463 valid probes, 58,660 relocated, 7,581 bricks kept,
    87.6 s.
  - GPU gather: about 22 s per sun key and 59 s for the 72-lamp group.
  - Lamp direct band: 0.26 s, nonzero on 12.6 % of probes.
  - Output: 314.7 MB, with nine layers and one direct band.
  - GPU textures: about 340 MB, mostly the 194 MB moment atlas and the
    116 MB band texture.
- **Output.** `sparse_volume_bistro_street_{on,off}_mode9` (2560 × 1440
  indirect diffuse; the off scene has no volume). Mean difference 16.6/255,
  with 67 % of pixels differing by more than 8. Depth is identical.
  Half-resolution against full-resolution lookup: mean 0.99/255; 4.47 % of
  pixels differ by more than 4/255 and 0.45 % by more than 16/255.
- **Cost.** `sparse_volume_bistro_street_{on,off}_timing` at 2560 × 1440 with
  `local-offscreen-gpu-repeated` (5 repetitions, 1,500 samples). This is
  **non-authoritative**: local profile, dirty tree.
  - Sample 0.243 ms, Upsample 0.241 ms, Fallback 0.031 ms.
  - `Lighting.Deferred` 6.578 against 6.523 ms.
  - Transmission shading layer 0: 0.936 against 0.835 ms.
  - Frame wall: 19.90 against 19.20 ms.
  - Runs `20261009T134750.845Z-000e68` and `20261009T135807.377Z-001d74`.
  - The owner's budget was 0.5 ms; the volume adds 0.57 ms to the opaque
    path and 0.10 ms to transmission.

Unavailable on this host:
- the tiled pipeline's compose and lookup on Metal, and
  `tiled_bistro_baked_native` before and after;
- volume bake times on the M1 Pro;
- an authoritative clean-tree timing;
- a Bistro night capture across the café facade.

## Revisit when

- **Path sharing.** Sharing light paths across layers would cut bake time:
  each sun key costs about 22 s on Bistro.
- **Metal probe gather.** Mac bakes need one to stop falling back to the CPU.
- **Cost.** An authoritative timing exceeds the owner's budget, or the
  half-resolution softening shows at contact shadows in review.
- **Size.** A scene needs more than 13,104 bricks: the moment atlas limit,
  reached at about 0.5 m spacing on Bistro.

## Code evidence

- Bake: [bricks](../../tools/bake/vkr_bake_bricks.h), [diffuse baker](../../tools/vkr_diffuse_baker.cpp), [integrator](../../tools/bake/vkr_bake_integrator.h), [GPU gather](../../tools/bake/vkr_bake_lightmap.slang), [SH](../../tools/bake/vkr_bake_sh.h)
- Asset and scene: [DVOL codec](../../runtime/src/assets/vkr_diffuse_volume.h), [scene loader](../../runtime/src/renderer/resources/loaders/scene_loader.c), [scene binding](../../runtime/src/renderer/systems/vkr_scene_system.h)
- Renderer: [binding contract](../../renderer/src/vkr_render_resources.h), [Vulkan compose](../../renderer/src/vulkan/vkr_vulkan_diffuse_volume.c), [Vulkan sample passes](../../renderer/src/vulkan/vkr_vulkan_deferred.c), [shared kernel](../../renderer/src/shaders/shared/diffuse_volume_kernel.slangh), [compose shader](../../renderer/src/shaders/vulkan/slang/world/diffuse_volume.slang)
