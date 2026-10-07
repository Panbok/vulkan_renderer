---
status: proposed
updated: 2026-10-07
authority: proposal
---

# Sparse diffuse volumes for open scenes

The baked diffuse volume of [ADR-054](../adr/054-baked-diffuse-volumes.md)
gives static multi-bounce indirect diffuse light only inside closed rooms, on
one grid of at most 256 probes. In open scenes such as Bistro the bake finds
no valid cell, so the desktop pipeline (Vulkan) takes indirect diffuse light
from the global environment and local probes alone. This proposal extends the
volume to open scenes: sparse bricks of probes, dense near surfaces, with
per-probe visibility in place of the room proof. It records the baseline, the
design, the open choices and the evidence needed to accept it. No part of it
is implemented.

A probe grid holds indirect light only. It does not reduce the cost of static
lamps: their shadows in `Shadow.LocalMask` and their shading in
`Lighting.Deferred` stay as they are. Baking lamp light on the desktop
pipeline is a separate question.

## Baseline

Desktop deferred lighting chooses one indirect diffuse source per pixel
([`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang),
`packet_diffuse_volume_terms` in
[`default.slang`](../../renderer/src/shaders/vulkan/slang/world/default.slang)):

| Source | Used when | Occlusion |
|---|---|---|
| Diffuse volume | The pixel's cell is valid | GTAO visibility |
| Global environment and local probe SH | Otherwise | GTAO visibility with multi-bounce |
| SSGI ([ADR-060](../adr/060-screen-space-diffuse-indirect-lighting.md)) | Optional, off by default; suppressed in valid cells | None |

Limits of the current volume:

- **Probe count.** `VKR_DIFFUSE_VOLUME_MAX_PROBES` and the bake's `--grid`
  check allow 256 probes on one uniform grid over the bounds.
- **Room proof.** The voxelizer flood-fills exterior air from the faces of
  the bounds as region zero. A cell is valid only when it is clear and its
  eight probes share one interior region
  ([`vkr_bake_voxels.h`](../../tools/bake/vkr_bake_voxels.h)). Open scenes
  have no interior region: Bistro inspects with no valid cell, and the bake
  exits 3 without output. Inside a room, the proof never leaks; an invalid
  cell falls back to the environment.
- **Texture layout.** The runtime texture has one row per probe of eight
  RGBA32F texels, 128 bytes. The 2D height limit, 16,384 on RDNA2 and Apple
  GPUs, therefore bounds the probe count even without the cap.
- **Layer composition.** `vkr_scene_update_diffuse_volume` composes the light
  layers on the CPU into a new texture when a weight moves by more than 1%, at
  most every 0.25 s. Each composition uploads the whole texture and creates a
  texture handle.
- **Lookup.** The shared kernel
  ([`diffuse_volume_kernel.slangh`](../../renderer/src/shaders/shared/diffuse_volume_kernel.slangh))
  reads one cell texel and seven texels of each of eight probes: 57 reads per
  pixel. Its consumers are desktop deferred and forward shading, the SSGI
  cell check
  ([`ssgi.slang`](../../renderer/src/shaders/vulkan/slang/post/ssgi.slang))
  and, on the tiled pipeline, draws without a lightmap
  (`vkr_metal_packet_diffuse_volume` in
  [`lighting.metalh`](../../renderer/src/shaders/metal/msl/world/lighting.metalh)).

Bistro's model bounds, from the glTF position accessors under their node
transforms, are about 111 × 32 × 119 m. A uniform grid over them holds about
59,000 probes at 2 m, 451,000 at 1 m and 3.5 million at 0.5 m. Much of that
volume is sky above the roofs or closed building mass, so a dense grid must
be sparse.

## Goals and scope

- Valid baked indirect diffuse light over the Bistro street, its facades and
  the café interior, with every light layer and the time-of-day weights of
  [ADR-090](../adr/090-time-of-day.md).
- Probe spacing of about 1 m within a few metres of surfaces, coarser
  elsewhere.
- No visible leaking through walls. The sealed room of the ADR-054 blockout
  stays black in every layer.
- Bakes on macOS and Windows. The CPU integrator stays the reference and the
  Windows path.
- A desktop lookup cost that the owner accepts (open choice 7).

Out of scope: the direct light and shadows of lamps, specular light and
runtime updates of the probes. The layout keeps runtime ray-traced updates
possible on the desktop pipeline later (see [Later work](#later-work)).

## Design

### Sparse bricks

- A brick holds 4 × 4 × 4 probes. Level 0 bricks have the finest spacing
  `s`, and each higher level triples it, so a brick of level k spans
  3 · 3^k · s. With three levels and `s` = 1 m, bricks span 3, 9 and 27 m.
- The bake places level 0 bricks where the conservative occupancy of the
  existing voxelizer finds a surface within one brick span, and level 1 and 2
  bricks elsewhere inside the bounds. A brick entirely inside closed solid
  is not stored.
- Bricks live in one 3D atlas. An indirection 3D texture over the bounds, with
  one entry per level 0 brick span, names the brick and level that cover the
  entry. For Bistro at `s` = 1 m it has about 38 × 11 × 40 entries, 67 KB at
  4 bytes each.
- Probes on a brick face repeat the neighbouring brick's values, so
  interpolation is continuous across bricks of one level. Where levels meet,
  the bake fills the face probes of the finer brick from the coarser brick's
  interpolation, so the boundary has no step.

### Probe validity and relocation

A probe whose gather sees back faces in more than a quarter of its directions
lies inside geometry. The bake moves it toward the free side, by at most half
its spacing, and bakes it there. A probe that stays inside is invalid. Each
probe stores its offset and its validity. The shader gives an invalid probe
zero weight and renormalizes the other weights.

### Visibility

Each probe stores the mean and the mean square of its first-hit distances in
an octahedral 8 × 8 RG16F map: 6 × 6 directions plus a one-texel border for
bilinear filtering, 256 bytes. The shader weights each corner probe by:

- the trilinear weight;
- a smooth back-face term from the receiver normal and the direction to the
  probe;
- a Chebyshev bound on the receiver's distance against the probe's distance
  moments in the receiver's direction, as in
  [DDGI](https://jcgt.org/published/0008/02/01/).

Before the test, the receiver point moves along its normal and toward the
viewer by a fraction of the spacing, so a surface does not shadow itself. A
pixel whose summed weight falls below a small threshold takes the environment
path, as an invalid cell does today.

This visibility test replaces the room proof and the region IDs, so rooms and
open space use one representation. The ADR-054 sealed-room check keeps
proving that rooms do not leak.

### Representation

- L1 SH in RGBA16F: the four coefficients of red, green and blue fill three
  texels. A fourth texel holds the relocation offset and the validity. The
  composed data of a probe is 32 bytes. L2 stays the representation of the
  environment and local probes ([ADR-038](../adr/038-sh-l2-diffuse-irradiance.md)).
- Per pixel, the lookup reads one indirection entry, four texels of each of
  eight probes and one filtered visibility sample per probe. That is 41 reads
  against 57 today, plus the Chebyshev arithmetic.

Memory per probe with Bistro's nine layers (eight sun keys and one lamp
group) is about 504 bytes: 32 composed, 256 of visibility and 9 × 24 of layer
SH. 100,000 probes take about 50 MB and 200,000 about 100 MB. Phase 1
measures the real probe count.

### Light layers

Every layer's SH is uploaded once. When a layer weight moves by more than 1%,
at most every 0.25 s as today, a compute pass writes the weighted sum into the
composed atlas in place. The graph orders the pass before the frame's
readers; the reads of earlier frames precede it in queue order. This removes
the CPU composition, the upload of the whole texture and the new texture
handle at each composition. Both backends run the same pass.

### Bake

- Placement, relocation and validity use the existing BVH and voxelizer.
- Transport keeps the CPU integrator of ADR-054. Each probe gathers over
  cube faces (`--face-size`), projects each layer's radiance to L1 SH and
  records first-hit distances for the moments. Caustic photons stay as today.
- The Metal gather of [ADR-088](../adr/088-baked-lightmap-sets.md) gains a
  probe kernel in a later phase. The CPU stays the reference and the Windows
  path.
- `DVOL` v3 holds the header, the layer table, the indirection, the bricks,
  the layer SH, the offsets and the moments. The loader refuses v2 and asks
  for a new bake, as it refuses v1 today. `vkr_bakery bake diffuse` replaces
  `--grid` with `--spacing` and `--levels`, and the managed `bakes.diffuse`
  request follows.

Bake time is an estimate until phase 2 measures it. On the M1 Pro the CPU
integrator traced 457,000 paths per second on Bistro with 8 threads, and the
Metal gather 0.79 million per second on textured Bistro (ADR-088).
100,000 probes × 1,024 paths × 9 layers is about 0.9 billion paths: about
34 minutes on the CPU and 19 on the GPU. Each layer traces its own paths
today. A path traced once could add every layer's lights and sky at its
vertices, which removes most of that factor of nine; lightmap bakes would
gain the same.

### Consumers

- Desktop deferred and forward shading keep today's composition. A covered
  pixel replaces the global and local probe diffuse light, GTAO visibility
  multiplies it, and environment specular is unchanged.
- The SSGI cell check becomes the same coverage test. Bistro becomes covered
  almost everywhere, so SSGI then adds nothing there (open choice 5).
- The tiled pipeline reads the volume only for draws without a lightmap,
  such as dynamic objects. Their per-pixel cost changes with the new lookup.

## Phases

1. Placement only: Bistro bricks at `s` = 1 m and 0.5 m, probe counts and
   memory, without transport.
2. `DVOL` v3 and the CPU bake: bricks, relocation, moments and L1 layers.
   CPU tests: the codec round trip and rejection, placement that is
   byte-identical for any thread count, and the ADR-054 Lambert furnace in
   L1.
3. Runtime: the shared kernel, desktop deferred and forward shading, the SSGI
   coverage test, the tiled consumer and the composition pass. The same
   change removes v2, the region IDs and the CPU composition.
4. The Metal probe gather and paths shared across layers.

## Later work

- SSGI from light the bake does not hold, such as dynamic lights and the
  emission of dynamic objects, so that it adds to covered pixels without
  counting baked light twice. ADR-060 names this revisit.
- Specular occlusion from the volume: scale environment specular by the ratio
  of volume irradiance to environment irradiance, so glossy surfaces inside
  the café stop reflecting the sky.
- Ray-traced runtime updates of the same bricks and moments on the desktop
  pipeline, for dynamic lights and time of day without baked layers.

## Open choices

| # | Choice | Options | Recommendation |
|---|---|---|---|
| 1 | Leak control | Distance moments and validity; validity only; the room proof kept as a hard mask | Moments and validity. Validity alone leaks through thin facades. The room proof rejects open space and keeps a second representation. |
| 2 | SH order | L1; L2 | L1: four reads per probe instead of seven. At 1 m spacing, variation across space dominates angular detail. Revisit if bounce under awnings shows directional error. |
| 3 | Finest spacing | 1 m; 0.5 m | 1 m, about eight times fewer level 0 probes. Decide after the phase 1 counts. |
| 4 | Moment map size | 8 × 8; 16 × 16 (1 KB) | 8 × 8, raised only if the thin-wall check leaks. |
| 5 | SSGI in covered pixels | Suppressed, as today; a source of unbaked light only | Suppressed first; the unbaked source as later work. |
| 6 | Layer composition | A GPU pass; the CPU into a persistent texture | The GPU pass: no full upload or handle per composition. |
| 7 | Lookup budget | Owner decision | Suggested: at most 0.5 ms added to `Lighting.Deferred` at 2560×1440 on the RX 6700 XT, against the environment path. |

## Evidence needed to accept

- **Bake.** Bistro publishes a volume that covers the street, the facades and
  the café. Record brick and probe counts, memory, bake time and peak memory
  on the M1 Pro (CPU and Metal) and on Windows (CPU).
- **Leaks.** The ADR-054 blockout's sealed room holds zero in every layer of
  the decoded volume and renders black at midnight in the editor. A Bistro
  night capture across the café facade shows no interior lamp bounce on the
  outer wall away from the openings.
- **Desktop output.** Vulkan captures of the Bistro street view of
  `local_shadow_bistro_vulkan_street` in render mode 9 (indirect diffuse) and
  in the final image, with and without the volume.
- **Desktop cost.** Matched capture-free Release profiles of the same view on
  the RX 6700 XT: `Lighting.Deferred` and the composition pass, with and
  without the volume.
- **Tiled cost.** `tiled_bistro_baked_native` before and after, with
  `Tiled.Opaque` within noise.
- **CPU tests** as listed in phase 2.

Unavailable from the macOS host: every Vulkan output and timing gate, which
needs the Windows host.
