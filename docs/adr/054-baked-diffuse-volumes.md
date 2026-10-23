---
status: partial
updated: 2026-10-23
authority: adr
---

# ADR-054: Baked diffuse volumes

## Status

Accepted (partial). The sparse brick volume replaced the room-proof grid on
2026-10-09. The desktop pipeline (Vulkan) bakes, loads, composes and samples
it, with native evidence on Bistro. The tiled pipeline (Metal) composes and
samples it too, with native evidence on Bistro and the leak fixture since
2026-10-10, and a Mac bakes volumes on Metal ray tracing. Since 2026-10-11
baked sky light scales environment reflections on both pipelines, with
native evidence on Metal only; since 2026-10-12 it scales only the global
environment's share, and local probes keep their captured radiance (the
tiled side not compiled or run on Metal). Also since 2026-10-11 the desktop pipeline
keeps lamp bounce apart from sky light on every frame with an active lamp
group, and its lightmapped pixels add their lamp light to the reflected
surroundings. Native Vulkan runs on Bistro (2026-10-12) find its indirect
diffuse unchanged within rounding against an unmatched older run and show
the extra lookups' cost in a local timing; the reflections it changes are
not yet checked on hardware. Light paths are not yet shared across layers.

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
bricks exits 3 without output. Since 2026-10-23 the bake writes the input
index `<name>.vkli` beside the volume
([ADR-088](088-baked-lightmap-sets.md), Rebakes); a bake whose previous volume
(`--previous`, the output itself by default) was made with the same recipe
and baker identity (`VKR_BAKERY_IDENTITY_DIFFUSE`) from inputs that match
its index within the tolerance keeps it, exits 6 and publishes nothing, and
`--full` bakes regardless. A project bake passes its previous volume and the
request's `full`, records the index as the volume record's `volume_index`
artifact, and on exit 6 keeps the record and its revision. The recorded
closure is re-hashed after the bake instead of a second scene load.

Since 2026-10-23 a changed scene bakes incrementally by default. When the
previous volume's sidecar names this bake's fingerprint (the same baker and
recipe), the bakery passes it with its index (`--previous-volume`,
`--previous-index`). The baker checks that the index names the volume's
checksum and size, that the globals, grid and layer table match, and then
keys probes by lattice position and level: a probe keeps a band's SH when
the previous volume stores its own SH (in a slot that is not a face fill,
valid, at the same relocated position) and the band's light there cannot
have changed by the lightmap plan's rules applied to the probe as a point
(`vkr_bake_plan_receivers`,
[ADR-088](088-baked-lightmap-sets.md) Rebakes): a sun key's sweep, a lamp
group's gathered band by its irradiance rule and its direct band by the
lamp-direct rule, with reach `max(--reach, spacing)`, 4 m by default. The
other probes gather; kept probes take the previous SH as stored, so their
slots stay bit-identical when the SH scale does, and face fills interpolate
the current probes. A bake gathers only the probes the file reads: a probe
whose every slot is a face fill and that no fill reads is skipped, which
left a full bake of the street fixture byte-identical with 15% fewer
probes. The sidecar's `production` records `kind`, the base volume and the
probes gathered per band; packaging warns for an incremental volume and a
baseline refuses one. `vkr_bakery tool diffuse-baker --compare <a> <b>`
reports per band the slots compared and identical, the luminance-weighted
constant-term error and the worst slots with their positions.

On a 144 m street fixture (12 bounded lamps, probe spacing 1, 33,709
probe-bands of each band; Release, M1 Pro, 2026-10-23) a full bake took
21.0 s. After a lamp's intensity doubled, the incremental bake gathered
4.3% of the probe-bands in 10.8 s; against the full bake the sun bands
differed by 0.001% (luminance-weighted) and the lamp group's gathered band
by 0.30%, worst slots 46% on dark probes behind a wall, 57 m from the lamp.
After a 1.5 m prop moved 1 m it gathered 3.5% in 10.9 s: sun bands 0.04%
(worst slot 1.7%), lamp band 0.09% (worst 15%). The rest of the time is
fixed cost: scene load, placement, visibility and the sun-key atmospheres.

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
uploads have completed; until then the frame lights without it. A missing or
invalid file logs an error, and the scene loads without a volume.

Bistro's volume, `assets/textures/bistro_sparse_volume_local.vkdv`, is
generated and untracked, about 315 MB, so only the `*_local` fixtures name it;
the tracked `assets/scenes/bistro.scene.json` does not, and a checkout without
the file still opens it. The café probe is captured from
`bistro_sparse_volume_local.scene.json`, so its reflections hold the volume's
bounce light ([ADR-019](019-bounded-forward-spatial-lighting.md)). The
2026-10-09 Windows bake below wrote the file from that fixture. Regenerate it
with

```sh
./build_release/tools/bakery/vkr_bakery bake diffuse \
  --scene assets/scenes/fixtures/bistro_sparse_volume_local.scene.json \
  --output assets/textures/bistro_sparse_volume_local.vkdv
```

whose defaults are that bake's recipe: 1 m spacing, three levels and a 1 m
margin ([`vkr_bakery_bake.c`](../../tools/bakery/vkr_bakery_bake.c)).

The scene weighs the layers ([ADR-090](090-time-of-day.md)): the two sun keys
nearest the current sun share weight one, and each lamp group takes its light
group's factor. When a weight moves by more than one percent, at most every
0.25 s, the composition revision advances (`vkr_scene_update_diffuse_volume`).
`DiffuseVolume.Compose`, a compute pass in both render graphs, then writes the
weighted sum into the composed texture's first band. The desktop pipeline
keeps the active lamp groups apart in the second band whenever one is
active, so the first band holds only the sun-key layers and every lookup can
tell baked sky light from lamp light. The lamp groups' direct bands join the
second band only on a frame that samples baked lamps, since a frame without
baked lamps lights the static lamps at runtime (owner decision, 2026-10-11;
`vkr_vk_diffuse_volume_lamp_rows` and `vkr_vk_diffuse_volume_lamps_baked` in
[`vkr_vulkan_diffuse_volume.c`](../../renderer/src/vulkan/vkr_vulkan_diffuse_volume.c)).
The tiled pipeline keeps no lamp band apart: it looks the volume
up only for draws without a lightmap, so while a lightmap is sampled its
compose adds each active lamp group's direct band into the single sum (owner
decision, 2026-10-10). Its second band holds, in each probe's first texel,
the Rec. 709 luminance of the sun-key layers' weighted L1, which the
reflection scale below reads. The compose waits at queue level for earlier frames' reads of the
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
  light. Ambient occlusion stays; environment and probe specular stay but
  are scaled by baked sky light (below).

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

Deferred lighting reads the result per pixel. Transmission shading calls the
lookup inline. The tiled pipeline calls it inline in
`Tiled.Opaque`. Frame roots carry the textures, origin, spacing, `sh_scale`,
moment rows, lamp band offset (zero when no lamp group is active) and
dimensions ([ADR-044](044-shader-cross-backend-contract.md) owns the ABI
checks).

### Baked sky in environment reflections

A surface that cannot see the sky must not reflect it. Without local probes
the environment specular of rooms, tunnels and a transit chasm showed sky and
clouds (Black Mesa test level, 2026-10-11). The global cube is captured from
one point; only baked light knows what a surface sees.

A surface with baked light, from its lightmap
([ADR-088](088-baked-lightmap-sets.md)) or the volume, reflects the global
environment's share as

`L = D + (E − D) · V`, with
`V = min(1, lum(D_sky) / lum(D_env))`
([`sh_l2_kernel.slangh`](../../renderer/src/shaders/shared/sh_l2_kernel.slangh),
`vkr_sh_l2_sky_visibility`, `vkr_sh_l2_occluded_radiance`):

- `E` is the global environment's prefiltered radiance along the
  reflection, the cloud layer included, pre-exposed.
- `D` is the surface's baked diffuse response E/π, the one its indirect
  diffuse uses. It stands for its surroundings' mean radiance. On the
  desktop pipeline a lightmap holds only the lamp groups, so a lightmapped
  surface's `D` is its volume light, which leaves the lamp groups out, plus
  its lightmap's lamp irradiance over π, less the light moving casters hide.
- `D_sky` is the part of `D` from sun-key layers: sky light and sun bounce,
  without the lamp groups ([`vkr_light_layers.h`](../../runtime/src/assets/vkr_light_layers.h)).
- `D_env` is the response the global environment's SH gives at the normal,
  with its radiance scale but without the authored diffuse and specular
  gains.

An open surface, whose baked sky light matches or exceeds the environment's,
keeps `V = 1` and reflects the environment unchanged. An enclosed one reflects
its lit surroundings at their mean radiance. The frame's IBL specular gain,
horizon fade, specular occlusion and, on the desktop pipeline, GTAO's cone
still apply after. A surface without baked light keeps the environment.

A local probe's share is not scaled: its capture already holds its room's
occlusion and lamps, which `D_sky` leaves out, so scaling it collapsed a
lamp-lit room's reflection to the flat colour `D` (audit R1 in
[Short-history rendering](../proposals/short-history-rendering.md), owner
decision 2026-10-12). The probes take their shares first
([ADR-019](019-bounded-forward-spatial-lighting.md)), and `V` applies to the
share they leave. A baked probe's radiance does follow the volume's layer
weights through one factor: the luminance the current composition gives
its capture origin over the luminance the capture's composition gave it
(`vkr_diffuse_volume_layer_luminance`, audit B4 in ADR-019).

Per backend:
- **Tiled (Metal).** A lightmapped draw sums its sun-key layers beside its
  irradiance; `VkrLightmapBinding::active_lamp_mask` marks the lamp-group
  layers and reaches the shader in `VkrMetalPacketLightmap`. A draw on the
  volume reads the composed sun-key luminance from the second band (row
  offset in `VkrMetalPacketDiffuseVolume::dimensions.w`). `D_env` is the
  global SH. A surface whose draw, or blended pixel, took a probe skips the
  blend
  ([`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal);
  not compiled or run on Metal since this change).
- **Desktop (Vulkan).** `packet_diffuse_volume_lookup` returns the sky
  share `lum(rows 0) / lum(total)`: the sun-key part's share of the
  response. The share is one where the response holds no lamp part: no lamp
  group is active, or the surface's lightmap holds the lamps and the lookup
  leaves them out. The volume images carry the share in alpha (at least
  1/1024, so a covered pixel stays nonzero); deferred lighting, forward and
  transmission shading pass it with `D` to
  `packet_environment_specular_radiance`, which scales only the global
  environment's share, against the global SH. A lightmapped surface the volume
  covers adds its lamps' light to `D` but not to `D_sky`
  (`packet_baked_light_with_lamps` in
  [`default.slang`](../../renderer/src/shaders/vulkan/slang/world/default.slang));
  one outside the volume keeps the environment, because nothing measures
  its sky light. Transmission shading adds its lightmap's lamp light without
  subtracting the light moving casters hide.

Cost per baked pixel on Metal: one L2 SH evaluation (seven `float4`
constant reads, about 30 FMA), one multiply-add per sun-key layer, two
luminance dots, a division and the blend; a volume draw adds one texel read
per corner. Measured on the baked Bistro orbit at 2560 × 1440 on this M1
(`tools/cases/local/tiled_bistro_baked_native.case.json`,
`tools/profiles/local-windowed-gpu.json`, local and dirty, so not
authoritative, alternated with a build of the parent commit): `Tiled.Opaque`
mean / p95 10.41 / 16.66, 10.41 / 16.68, 10.41 / 16.62, 10.41 / 16.62 and
10.42 / 16.64 ms before; 10.72 / 17.11 and 10.72 / 17.12 ms after, the second
run's warmup unstable
(`sha256:586273b70c105aef8bfbc4f2372f281e3357906ab6d0b7f5a2a4da3bd06463c1`
last before, `sha256:e2513bbd4804a46b44170e42af1c53a1a7fac0cde58c13994c80eb969c888598`
and `sha256:8b59e0e002f043f33a6c86c215815ba0fb13bf1f8ec8f9aa40a5de10b8add06f`
after). Frame wall time p50 rose 14.69 to 14.82 ms. The time is register pressure, not arithmetic: replacing the SH by a
constant or dropping the sun-key sums left it within 0.1 ms, and computing the
share before the cube sample cost 2.4 ms.

On the desktop pipeline a frame without baked lamps reads the lamp band too
while a lamp group is active: three more texel reads per valid corner in
`DiffuseVolume.Sample`, `DiffuseVolume.Fallback`, forward and transmission
lookups. A frame with baked lamps and no active lamp group reads three fewer.
These are source counts; the cost is not measured.

## Consequences

- Static geometry, material, light or layer changes need a rebake.
- The desktop lookup interpolates at half resolution. It is exact only at
  silhouettes and on thin geometry.
- The baked layers hold no dynamic light. On the desktop pipeline with ray
  queries, a dynamic layer adds the bounce of dynamic lights, emissive movers
  and changed static lights, and a runtime grid stands in for a volume not
  baked yet ([ADR-119](119-dynamic-diffuse-volume-layer.md)). Neither
  supplies sharp caustics or runtime glass transport.
- L1 loses the quadratic band's directional detail.
- Baked sky light scales a reflection by one number per pixel. A mirror in a
  half-open place loses sky detail in the directions that do see the sky, and
  an enclosed one reflects a uniform colour, not the shapes around it: local
  probes supply those, and planar reflections are proposed for water
  ([Short-history rendering](../proposals/short-history-rendering.md)).
- On the desktop pipeline a lightmapped surface outside the volume keeps
  the full environment reflection: its lightmap holds only the lamp groups,
  so it has no baked sky light to scale by. The tiled pipeline also scales
  environment specular by the lightmap's baked ambient visibility; the
  desktop pipeline has GTAO's cone only.
- On a desktop frame without a lightmap, a pixel's indirect diffuse is the
  sum of two RGBA16F texels, the sun-key band and the lamp band, added in
  fp32, where it was one texel. The result differs within fp16 rounding.
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
- **Reflection normalization by total baked light**, the common ratio of
  lightmap to capture irradiance. On the Black Mesa level the lamp-lit
  warehouse, terminal and chasm had a ratio of about one, so they kept
  reflecting the sky; only the sun-key part separates sky from lamps.
- **GTAO as specular occlusion.** It is screen-space and short-range, and the
  tiled pipeline has none.
- **A baked sky-visibility or bent-normal term.** Neither bake produces one;
  it would remove the runtime SH evaluation and the dependence on matching
  bake and runtime sky units.

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

Baked sky in reflections (2026-10-11, Release, validation unset, M1):
- Metal Bistro snapshots, `vkr_harness snapshot --case
  tools/cases/local/tiled_bistro_baked_capture.case.json` and
  `tiled_bistro_sparse_volume_on_capture.case.json` with
  `tools/profiles/local-offscreen.json`, before (parent-commit build) and
  after. Two before runs differ at 67.8 dB PSNR; before against after is
  28.3 dB on both cases. The change is reflection-only: bright sky rims on
  window frames, grilles and lanterns in the alley turn to the street's
  colour; mean luma 104.9 to 103.4. Reports
  `sha256:b4f078d659e521cda93d364f9fe54468bd744be09cfa4a2d1cd42e03cb908d29`,
  `sha256:77b7461ea003a6205583ed2a4605b3ec0476b6dcddb5c6da7d5227eb6663dc91`
  before and
  `sha256:34b989e0795c4ef791d80780eb735c2d665412fb314f93af1488e47766ec8695`,
  `sha256:5c615b060c09e5dda209f06186411f6a1a69e0d5a3113e4c132baf4e43028a1e`
  after.
- Black Mesa test level in the headless editor (`view.capture`): a debug map
  of `V` read zero on the warehouse, terminal and chasm surfaces, lightmapped
  and volume-lit alike, and one on the bridge deck by the lake; the chasm's
  pipes and walls lost their sky sheen and the lake views did not change
  beyond the clouds' motion.
- Vulkan: every SPIR-V module of the build passes `spirv-val --target-env
  vulkan1.4 --scalar-block-layout`; CPU suites pass. No native Vulkan run:
  the desktop pipeline's reflections, the volume images' alpha and the
  subtraction in the SSR composite, since removed (ADR-055), were unchecked on
  hardware.
- Vulkan lamp band on every frame and lamp light in lightmapped reflections
  (2026-10-11): a scratch compile of the Vulkan shader catalog
  (`vkr_bakery shaders --backend vulkan`) built every module, and the
  modules pass `spirv-val --target-env vulkan1.4 --scalar-block-layout`.
  Reflection of `packet.ssr_composite.comp.spv`, a module since removed with
  SSR, gave the baked lamp fields at bytes 424 and 428 of the 432-byte root.
  The changed host files pass `clang -fsyntax-only`.
- Native Vulkan after that change, 2026-10-12 (Windows 10 Pro 19045, Ryzen 5
  2600, RX 6700 XT, AMD 26.6.3, Release, graphics validation unset, dirty
  tree, not authoritative). Bistro's volume fixture has no lightmap, and its
  lamp group has no light group to scale it, so it is active and every
  lookup there now sums the sun-key and lamp bands.
  - **Indirect diffuse.** `vkr_harness snapshot --case
    tools/cases/local/sparse_volume_bistro_street_on_mode9.case.json
    --profile tools/profiles/local-offscreen.json` passed
    (`sha256:3a01412f0a0b06669b9a2568991528a4f017994fda275de2fcc57a6425d3590a`).
    Against the newest earlier run (2026-10-10, 98 commits older, not
    matched), depth agrees within 9.5e-7 and `indirect_diffuse` is identical
    on 95.6 % of pixels; 566 isolated pixels differ by more than 2 codes,
    and the mean stays 89.778.
  - **Cost.** `vkr_harness profile --case
    tools/cases/local/sparse_volume_bistro_street_on_timing.case.json
    --profile tools/profiles/local-offscreen-perf-audit-gpu.json` (2560 ×
    1440, TAA off, five children of 480 warmup and 300 measured frames),
    once before the 2026-10-11 change set and three times after it, in one
    session: `DiffuseVolume.Sample` 0.247 ms before against 0.286 to
    0.295 ms after (+16 to +19 %), `DiffuseVolume.Fallback` 0.034 against
    0.037 to 0.039 ms. `DiffuseVolume.Upsample`, `Lighting.Deferred` and
    frame wall time showed no change beyond the spread between runs, and the
    work counters were equal. In these passes the change set changes only
    the lamp band read. Reports
    `sha256:e56494b4eea04a609f75ad439a960cc9770d809d510abc397682277b31e7b0b9`
    (before),
    `sha256:f8b8daeb1c69382ee43d62ef65b8d315dffe5737bb72f5fe541dfc6e27766d70`,
    `sha256:90b1a5f4100c58dd0a1fe8e3798980384814e8282a933aaf99463f9ebe73cd7b`
    and
    `sha256:ca8f8f548a080895fb1ec44a5808309b6327a5783333c9ab2cf7b967a516d5d2`
    (after). This is a local observation on a dirty tree, not a speed claim.
  - **Unchecked on hardware:** a matched mode 9 capture before and after; a
    frame with baked lamps and no active lamp group, which reads three
    texels fewer; a sealed lamp-lit room's reflections, since no Bistro case
    views an enclosed room inside the volume yet; and the lightmapped
    reflections of `bistro_lamps_baked_volume_night_capture`, which ran with
    the change ([ADR-104](104-desktop-baked-lamps.md#evidence-and-remaining-checks))
    but was not compared with a capture without it.

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
- **Reflection detail.** A review asks enclosed glossy surfaces to show
  their surroundings, not their mean colour: author local probes, or bake
  sky visibility with a bent normal and drop the runtime SH evaluation.
- **Size.** A scene needs more than 13,104 bricks: the moment atlas limit,
  reached at about 0.5 m spacing on Bistro.

## Code evidence

- Bake: [bricks](../../tools/bake/vkr_bake_bricks.h), [diffuse baker](../../tools/vkr_diffuse_baker.cpp), [integrator](../../tools/bake/vkr_bake_integrator.h), [GPU gather](../../tools/bake/vkr_bake_lightmap.slang), [SH](../../tools/bake/vkr_bake_sh.h)
- Asset and scene: [DVOL codec](../../runtime/src/assets/vkr_diffuse_volume.h), [scene loader](../../runtime/src/renderer/resources/loaders/scene_loader.c), [scene binding](../../runtime/src/renderer/systems/vkr_scene_system.h)
- Renderer: [binding contract](../../renderer/src/vkr_render_resources.h), [Vulkan compose](../../renderer/src/vulkan/vkr_vulkan_diffuse_volume.c), [Vulkan sample passes](../../renderer/src/vulkan/vkr_vulkan_deferred.c), [shared kernel](../../renderer/src/shaders/shared/diffuse_volume_kernel.slangh), [compose shader](../../renderer/src/shaders/vulkan/slang/world/diffuse_volume.slang), [Metal compose](../../renderer/src/shaders/metal/msl/world/diffuse_volume.metal) and [its frame preparation](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc), [tiled lookup](../../renderer/src/shaders/metal/msl/world/lighting.metalh)
