---
status: partial
updated: 2026-10-10
authority: adr
---

# ADR-054: Baked diffuse volumes

## Status

Accepted (partial). The sparse brick volume replaced the room-proof grid on
2026-10-09. The desktop pipeline (Vulkan) bakes, loads, composes and samples
it, with native evidence on Bistro. The tiled pipeline (Metal) composes and
samples it too, with native evidence on Bistro and the leak fixture since
2026-10-10, and a Mac bakes volumes on Metal ray tracing. Light paths are not
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

**Non-negative bands.** The runtime evaluates `E/π(n) = c + l · n` per channel
and clamps the result at zero. A band with `|l| > c` is negative behind its
dominant direction: every single-lamp direct band has `|l| = 2c`, and a
gathered band near a bright lamp can come close. There the warm channels clip
to zero and only the sky's blue survives, which showed as dark blue patches
beside Bistro's lantern brackets. The baker therefore shortens each stored
band's linear term per channel to at most its constant term
(`clamp_l1_nonnegative` in `store_band`), for gathered and direct bands
alike. Composition sums bands with non-negative weights, so every composition
stays non-negative. The cost is directionality: a lamp-dominated probe facing
its lamp evaluates `2c` instead of `3c`.

**Transport.**
- The default is `vkr_bake_gpu`: Vulkan ray queries
  ([`vkr_bake_vulkan.cpp`](../../tools/bake/vkr_bake_vulkan.cpp)) or Metal ray
  tracing ([`vkr_bake_metal.mm`](../../tools/bake/vkr_bake_metal.mm)), whose
  `probe_gather` kernels share the lightmap gather's `path_radiance`. Metal
  takes one sample of each pixel per command buffer and accumulates the runs
  in sample order, so the split does not change the result.
- `--cpu` selects the CPU integrator
  ([`vkr_bake_integrator.h`](../../tools/bake/vkr_bake_integrator.h)), and
  `--gpu-parity` compares the two.
- The CPU integrator keeps the bounded photon estimate for caustics after
  specular or thick glass chains. The GPU gather has none.
- In the CPU integrator, a path that exceeds the transparent-layer limit
  ends dark, and a shadow walk that exceeds it returns no light.
- Each probe's paths are seeded from its lattice position, so a CPU volume is
  byte-identical for any thread count ([ADR-077](077-asset-build-system.md)).
  A Metal volume is byte-identical for any batching on a scene without cutout
  or blended surfaces, but two Metal bakes of the Level Design Test differ:
  shadow rays accept any hit, so through such surfaces the order of hits, and
  with it the random draws, varies between runs.
- A scene whose bounds need more indirection entries or bricks than the
  limits allow at its spacing gets no volume: the diffuse baker exits with
  `VKR_DIFFUSE_VOLUME_OVER_BUDGET_EXIT`, `vkr_bakery bake diffuse` with
  `VKR_BAKERY_BAKE_VOLUME_TOO_LARGE`, and the project bake drops any previous
  volume and bakes the lightmaps with a warning, as for a scene without
  geometry. The Bake settings window's Probe spacing widens the lattice.
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
band instead. The tiled pipeline has no second band: it looks the volume up
only for draws without a lightmap, so while a lightmap is sampled its compose
adds each active lamp group's direct band into the single sum (owner decision,
2026-10-10). The compose waits at queue level for earlier frames' reads of the
composed texture before rewriting it in place.

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

Mac (Apple M1 Pro, 16 GB, Metal 4, Release), 2026-10-10, local
non-authoritative runs on a dirty tree:
- **Bake.** `vkr_bakery tool diffuse-baker --scene
  assets/scenes/fixtures/bistro_sparse_volume_local.scene.json --face-size 4
  --samples 1` on the CPU integrator: 194,463 baked probes, nine layers and
  one direct band (72 lamps), 463 s, 5.2 GB peak, 315 MB. The leak fixture
  (`--spacing 1 --levels 2 --margin 1 --face-size 4 --samples 1`): 6,266
  probes, 10.5 s.
- **Compose ABI.** Every Metal startup creates `DiffuseVolume.Compose` and
  checks its 128-byte root by reflection; the first run found the root checked
  at buffer 1 instead of 0, which had disabled the compose silently.
- **Tiled lookup.** `tiled_bistro_sparse_volume_{on,off}_capture` (lightmapped
  Bistro with the animated mannequin, the only draw without a lightmap): depth
  identical; every changed pixel (2,769) lies in the mannequin's box, which
  takes the street's warm bounce instead of the environment's blue.
- **Leak fixture.** `sparse_volume_leak_room_c{,_none}_metal_local`: room C's
  far wall, which no direct light reaches, is 0/255 over 48,100 pixels with
  the volume and 76/255 from the environment without it. Room C's floor and
  ceiling take `lamp_b`'s direct light through `wall_bc`, pixel-identical
  before this change; the tiled pipeline does not shadow that lamp.
- **Blue patches.** `sparse_volume_bistro_street_on_lighting_metal_local`
  with a volume baked before and after the non-negative clamp: 10.4 % of
  pixels change, in patches beside lamps; across them blue minus red moves
  from −9.7 to −12.3 and 59 % of them were bluer without the clamp.
- **Metal probe gather.** `--gpu-parity 256` on the leak fixture
  (`--face-size 4`): at 4 samples every sun key's mean is within 0.5 % of the
  CPU's and its per-probe RMS 0.23 to 0.87 of the RMS between two CPU seeds;
  the lamp group's 1.67 falls to 1.04 at 16 samples, where sun keys 2 and 7
  read 0.5 % and 0.4 % low (z ≈ −4), the diffusely spread specular lobe of the
  GPU subset ([ADR-088](088-baked-lightmap-sets.md)). Lifting the loop into
  `path_radiance` left the leak fixture's lightmap byte-identical.
- **GPU bakes.** Bistro at the default `--face-size 8 --samples 4`: 677 s,
  4.8 GB peak; each sun key 40 to 44 s of GPU time, the 72-lamp group 183 s.
  The longest command buffer was 111 ms in a sun key and 659 ms in the lamp
  group. The Level Design Test at `--spacing 1.5` (8,683 bricks, 262,700
  probes; 1 m exceeds the brick budget): 82 s with 51 s of GPU time, from the
  editor's Bake lighting.
- **Validation.** One `MTL_DEBUG_LAYER=1` snapshot each of
  `tiled_bistro_sparse_volume_on_capture` (compose) and
  `local_shadow_bistro_metal_street_moving_capture`
  (`local-metal-offscreen-validation-serial`): no messages.

Unavailable:
- the tiled pipeline's indirect diffuse alone: the harness renders only the
  default, unlit, detail lighting, lighting-only and wireframe modes there;
- a Windows rebake of Bistro's volume with the clamp and its render mode 9
  capture at the bracket camera;
- an authoritative clean-tree timing;
- a Bistro night capture across the café facade.

## Revisit when

- **Path sharing.** Sharing light paths across layers would cut bake time:
  each sun key costs about 22 s on Bistro.
- **Command buffer length.** A lamp group's longest Metal buffer (659 ms on
  Bistro) nears the second at which macOS ended lightmap buffers while
  another app drew; larger lamp counts need smaller buffers for lamp layers.
- **Directionality.** Lamp-lit moving objects look flat next to lightmapped
  surfaces: the non-negative clamp halves a lamp band's linear term.
- **Cost.** An authoritative timing exceeds the owner's budget, or the
  half-resolution softening shows at contact shadows in review.
- **Size.** A scene needs more than 13,104 bricks: the moment atlas limit,
  reached at about 0.5 m spacing on Bistro.

## Code evidence

- Bake: [bricks](../../tools/bake/vkr_bake_bricks.h), [diffuse baker](../../tools/vkr_diffuse_baker.cpp), [integrator](../../tools/bake/vkr_bake_integrator.h), [GPU gather](../../tools/bake/vkr_bake_lightmap.slang), [SH](../../tools/bake/vkr_bake_sh.h)
- Asset and scene: [DVOL codec](../../runtime/src/assets/vkr_diffuse_volume.h), [scene loader](../../runtime/src/renderer/resources/loaders/scene_loader.c), [scene binding](../../runtime/src/renderer/systems/vkr_scene_system.h)
- Renderer: [binding contract](../../renderer/src/vkr_render_resources.h), [Vulkan compose](../../renderer/src/vulkan/vkr_vulkan_diffuse_volume.c), [Vulkan sample passes](../../renderer/src/vulkan/vkr_vulkan_deferred.c), [shared kernel](../../renderer/src/shaders/shared/diffuse_volume_kernel.slangh), [compose shader](../../renderer/src/shaders/vulkan/slang/world/diffuse_volume.slang), [Metal compose](../../renderer/src/shaders/metal/msl/world/diffuse_volume.metal) and [its frame preparation](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc), [tiled lookup](../../renderer/src/shaders/metal/msl/world/lighting.metalh)
