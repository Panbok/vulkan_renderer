---
status: proposed
updated: 2026-10-27
authority: proposal
---
# Clustered light and decal binning

This proposal scales the desktop pipeline's punctual lights and decals to
thousands: screen-space bitfields for raster consumers, and a world grid for
ray-traced consumers. Its first step shipped as
[ADR-135](../adr/135-desktop-light-and-decal-clusters.md). It takes over
item 4 of [Lighting efficiency](lighting-efficiency.md), camera-space
clustered culling. What decals write stays in
[Decal channels and receivers](decal-channels-and-receivers.md).

## Current baseline

- **Clusters (ADR-135).** On the desktop pipeline `Lighting.Clusters`
  builds frustum clusters each frame: 32 px tiles by 32 exponential slices.
  A cluster holds a 128-bit mask of the light head (the first 128 rows) and
  an index list of the other light rows and the decal rows. Deferred
  lighting, transmission, forward blend and `Shadow.LocalMask` walk the head
  mask; the decal resolve walks the decal list. The build costs about
  0.025 ms on the Bistro orbit (RX 6700 XT, 1440p).
- **Limits.** The light table holds at most 128 rows
  (`VKR_MAX_SCENE_POINT_LIGHTS`,
  [`vkr_lighting.h`](../../renderer/src/vkr_lighting.h)), ranked by camera
  distance with hysteresis and fades
  ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)); only these
  rows can be shadowed. The desktop light loops shade only the head, and a
  static assertion keeps the table there. Decals: see
  [ADR-092](../adr/092-projected-decals.md) for the frame selection.
- **Ray-traced consumers.** The diffuse volume's dynamic layer traces
  off-screen with its own list of at most 128 lights
  (`VKR_DYNAMIC_VOLUME_LIGHTS_MAX`,
  [`vkr_frame_input.h`](../../renderer/src/vkr_frame_input.h),
  [ADR-119](../adr/119-dynamic-diffuse-volume-layer.md)). Ray-traced
  reflections are planned to replace probes and planar reflections; they
  will shade hit points off-screen too.
- **Tiled pipeline.** Metal keeps the CPU light and decal grids and draws
  the 16 dynamic lights nearest the camera
  ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md), decision 11).
- **Measured on Vulkan** (orbit, `Lighting.Deferred` p50, shader-only A/B,
  2026-10-26): CPU grid 3.64 ms; an index list for every light 3.82 ms; the
  head mask with a tail-list loop, never executed on Bistro, 3.93 ms; the
  head mask alone 3.73 ms. Per-pixel index loads and even unexecuted list
  loops cost more than bitfield walks. The head-mask clusters remain
  0.1 ms behind the grid; the per-pixel cluster lookup (a projection and a
  `log2`) is the suspect.

## Reference: idTech and Call of Duty

| Game | Binning | Scale |
|---|---|---|
| Doom (2016) | CPU, 16x8x24 = 3,072 frustum clusters with log depth slices; up to 256 lights, decals and probes per cluster | Tens of lights |
| Doom Eternal | Compute rasterizer: light and decal hexahedra into 256 px tiles, then 32 px tile bitfields and froxels; reverse float depth; under 500 us | Hundreds of lights, thousands of decals |
| The Dark Ages | Screen clusters for raster lighting with 32x32 tiles; a world grid of 16^3 cells x 8 cascades, 64 ids per cell, on async compute, about 0.2 ms on consoles, 30 MB, also read by ray tracing, particles and glass | 4,000 lights and 30,000 decals in retail builds |
| Call of Duty: Infinite Warfare | Z-binning: lights sorted by view depth, a bitfield of lights per screen tile, and 1D depth bins that hold each bin's lowest and highest light index | Thousands of lights |

Eternal moved binning to the GPU because large levels piled distant small
lights into large clusters and combat needed more dynamic lights and decals.
Sources: [Courrèges, Doom (2016)](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/),
[Geffroy et al., SIGGRAPH 2020](https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf),
[Coenen, Doom Eternal](https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy),
[Sousa, SIGGRAPH 2025](https://www.advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf),
[Lazarek and Hammer, GPC 2025](https://static.graphicsprogrammingconference.com/public/2025/talks/visibility-buffer-and-deferred-rendering-in-doom-the-dark-ages/Lazarek-Hammer-visibility-buffer-and-deferred-rendering-in-doom-the-dark-ages.pdf),
Drobot, "Improved Culling for Tiled and Clustered Rendering", SIGGRAPH 2017
Advances in Real-Time Rendering.

## Targets

About 2,000 lights and 15,000 decals per frame on the desktop pipeline,
half of The Dark Ages' retail counts (owner, 2026-10-27). Shadows stay with
the 128-row light head and the shadow atlas, so most lights past the head
are unshadowed.

## Proposed work

1. **Z-binned light bitfields (W4a).** Sort the frame's lights by view
   depth. Each 32 px screen tile holds one bit per light (2,000 lights:
   250 bytes a tile, about 0.9 MB at 2560x1440); 1D depth bins, fine
   enough to keep ranges tight, hold the lowest and highest light index
   that reaches them. A pixel takes its tile from its pixel coordinate and
   its bin from linear depth, masks the tile's words to the bin's range,
   and walks them with W2's wave-uniform word loop: one loop shape at any
   light count, no index pool, no overflow. First measure it against the
   head-mask clusters at 128 lights; if it is cheaper there too, it
   replaces them. Then raise `VKR_MAX_SCENE_POINT_LIGHTS` and measure the
   stress case. Deferred lighting may also AND a tile depth-bounds mask
   later; glass and forward blend use the bins alone.
2. **Decal bitfields (W4b).** Decals blend in table order, which a depth
   sort would break. Each tile, possibly 64 px, holds one bit per decal in
   table order (15,000 decals on 64 px tiles: about 1.7 MB at 1440p); the
   resolve tests each listed decal's box, as it does now. Raise
   `VKR_MAX_FRAME_DECALS` to the target.
3. **World grid for ray-traced consumers (W5).** A camera-centred grid with
   cascades, built on the GPU from the same light bounds, with a fixed list
   of lights per cell and an overflow rule that keeps the strongest lights.
   The diffuse volume's dynamic layer reads it in place of its own 128-light
   list, and ray-traced reflections read it when they land; it may later
   feed stochastic many-light sampling (reservoirs per cell). It is the
   candidate for async compute.
4. **Build cost and async compute.** The screen build needs only the camera
   and the light table, so it can overlap G-buffer raster. Move a build to
   async compute only when its measured cost passes about 0.2 ms; Vulkan
   has one queue today, and [Asynchronous compute](async-compute.md) owns
   the compute queue and its rules.
5. **Froxel injection.** Optionally read the same structures to scatter
   more than two local lights, after its own measurement.
6. **Tiled pipeline.** It keeps the CPU grid and the 16-light policy
   (ADR-087).
7. **Tile classification.** Deferred lighting keeps its 8x8 classification
   tiles.

## Owner decisions

On 2026-10-20 the owner accepted these recommendations; the ones marked
revised were changed on 2026-10-27.

1. **Cluster space** (revised). Raster consumers bin in screen space; a
   world grid with cascades serves ray-traced consumers (W5), because the
   dynamic diffuse layer and future ray-traced reflections shade hit points
   off the screen.
2. **Build method.** A per-tile test first; a compute rasterizer of light
   volumes only if the build exceeds its budget at the targets.
3. **Depth** (revised). Z-bins over depth-sorted lights in place of 3D
   clusters: a bitfield per 3D cluster costs about 29 MB at the light
   target, and index lists cost per-pixel loads. Tile size stays 32 px for
   lights; decals time 32 and 64 px.
4. **Limits** (revised). Targets of about 2,000 lights and 15,000 decals,
   sized finally from the stress case; the ranked light table keeps its
   hysteresis and fades above the limit.
5. **Packet contract.** The tiled pipeline keeps the 128-bit CPU grid; the
   desktop build reads the packet's tables.

## Evidence needed

- A shader-only A/B at 128 lights on the ADR-087 orbit and walk:
  z-binned bitfields against the head-mask clusters, `Lighting.Deferred`
  p50.
- A Bistro stress case under `tools/cases/` with 1,000 to 2,000
  short-range lights and thousands of decals, in timing and capture
  variants; the existing `bistro_clustered_*` cases cover 264 lights and
  176 decals. Never Sponza (AGENTS.md).
- Matched, capture-free Release reports before and after each step on
  Vulkan, with the build's GPU time
  ([`vkr-performance`](../../.codex/skills/vkr-performance/SKILL.md)).
- Image comparison on the Bistro lamp, decal and clustered capture cases.
- One native Vulkan synchronization-validation run of each new pass.
- W5: the dynamic diffuse layer's image and cost with the world grid
  against its 128-light list.

## Risks

- Shading cost per pixel, not culling, bounds the light count: Bistro's
  lights overlap with up to 28 in one 1 m cell.
- Wide depth bins or lights spread across the view loosen the index ranges;
  the stress case must show the bins stay tight.
- The decal bitfield costs memory per tile in proportion to the decal
  count; above the target it needs a coarser tile or a hierarchy.
- A world grid cell that overflows drops light from ray-traced shading;
  its rule must keep the strongest lights, never the nearest bright one.
- The desktop and tiled pipelines look lights up in different structures,
  as ADR-087 allows.
