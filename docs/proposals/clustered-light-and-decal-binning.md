---
status: proposed
updated: 2026-10-20
authority: proposal
---
# Clustered light and decal binning

This proposal replaces the desktop pipeline's CPU-built world grids for
punctual lights and decals with clusters that the GPU builds each frame, and
raises the 128-light and 64-decal limits. It takes over item 4 of
[Lighting efficiency](lighting-efficiency.md), camera-space clustered
culling. What decals write stays in
[Decal channels and receivers](decal-channels-and-receivers.md).

## Current baseline

- **Light table.** The runtime keeps at most 128 point and spot lights
  (`VKR_MAX_SCENE_POINT_LIGHTS`,
  [`vkr_lighting.h`](../../renderer/src/vkr_lighting.h)), nearest the camera,
  with hysteresis and 0.25 s fades
  ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)). Grid
  membership is a 128-bit mask, so the mask width fixes the table size.
- **Light grid.** After light selection,
  `vkr_lighting_system_build_point_light_grid`
  ([`vkr_lighting_system.c`](../../runtime/src/renderer/systems/vkr_lighting_system.c))
  rebuilds a camera-independent world grid on the CPU over the range boxes
  of the finite lights. `vkr_world_grid_fit` sizes it to at most 384 cells
  of at least 4 m. Lights without a finite range go to a global mask that
  every cell reads.
- **Light consumers on Vulkan.** `packet_point_light_mask`
  ([`default.slang`](../../renderer/src/shaders/vulkan/slang/world/default.slang))
  looks up a pixel's cell. `packet_punctual_layered` uses it in deferred
  lighting, transmission shading and forward blended draws, and iterates the
  union of the wave's masks so light rows load as scalars. `Shadow.LocalMask`
  ands the cell mask with the shadowed lights that reach the view and fills
  eight RGBA8 overlap slots; lights past the eighth slot filter inline.
  Froxel injection does not read the grid: it scatters at most two selected
  local lights (`VKR_FROXEL_FOG_LOCAL_LIGHT_COUNT_MAX`,
  [`vkr_froxel_fog.h`](../../renderer/src/vkr_froxel_fog.h)).
- **Decals.** The runtime keeps the 64 decals nearest the camera
  (`VKR_MAX_FRAME_DECALS`, [`vkr_decal.h`](../../renderer/src/vkr_decal.h))
  and `vkr_decal_grid_build` places them each frame in a world grid of at
  most 256 cells of at least 2 m with 64-bit masks
  ([ADR-092](../adr/092-projected-decals.md)). On Vulkan,
  `vkr_vk_apply_decals` in `vk_gbuffer_resolve`
  ([`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang))
  iterates the wave's union of cell masks.
- **Tiled pipeline.** Metal reads the same light grid
  (`vkr_metal_packet_point_light_mask`) and decal grid, and draws the 16
  dynamic lights nearest the camera
  ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md), decision 11).
- **Shared contract.** Both grids reach the backends through the
  backend-neutral packet (`VkrPointLightGrid`, `VkrDecalGrid`). The shared
  kernels in `renderer/src/shaders/shared/` own the light row and its
  falloff (`point_light.slangh`, `punctual_light_kernel.slangh`), not the
  lookup.
- **Measured.** On Vulkan the punctual loop and BRDF take 0.83 ms of a
  3.69 ms `Lighting.Deferred` (Bistro street view, 1920x1080, RX 6700 XT,
  [Lighting efficiency](lighting-efficiency.md#vulkan-cost-split-2026-10-03)).
  On the removed Metal desktop implementation (M1 Pro, 1280x720, local), a
  world grid 89 times finer saved only 0.13 ms: Bistro's 7.5 to 15 m lights
  overlap genuinely, with up to 28 lights in one 1 m cell. Finer binning
  does not pay on Bistro as it is; the limits are the gap.
- **Cases.** The tracked Bistro cases light the street with its lamps
  (`bistro_lamps_*`) or six decals (`decals_bistro_street_*`). None has many
  short-range lights or more decals than the table holds.

## Reference: idTech

| Game | Binning | Scale |
|---|---|---|
| Doom (2016) | CPU, 16x8x24 = 3,072 frustum clusters with log depth slices; up to 256 lights, decals and probes per cluster | Tens of lights |
| Doom Eternal | Compute rasterizer: light and decal hexahedra into 256 px tiles, then 32 px tile bitfields and froxels; reverse float depth; under 500 us | Hundreds of lights, thousands of decals |
| The Dark Ages | Screen clusters for raster lighting with 32x32 tiles; a world grid of 16^3 cells x 8 cascades, 64 ids per cell, on async compute, about 0.2 ms on consoles, 30 MB, also read by ray tracing, particles and glass | 4,000 lights and 30,000 decals in retail builds |

Eternal moved binning to the GPU because large levels piled distant small
lights into large clusters and combat needed more dynamic lights and decals.
Sources: [Courrèges, Doom (2016)](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/),
[Geffroy et al., SIGGRAPH 2020](https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf),
[Coenen, Doom Eternal](https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy),
[Sousa, SIGGRAPH 2025](https://www.advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf),
[Lazarek and Hammer, GPC 2025](https://static.graphicsprogrammingconference.com/public/2025/talks/visibility-buffer-and-deferred-rendering-in-doom-the-dark-ages/Lazarek-Hammer-visibility-buffer-and-deferred-rendering-in-doom-the-dark-ages.pdf).

## Proposed work

1. **Index lists.** Clusters hold an offset and a count into a light index
   list and a decal index list, so the table size no longer follows a mask
   width. Raise the light and decal table limits (decision 4).
2. **GPU build.** A compute pass after `Cull.Encode` tests the frame's light
   range spheres and spot cones and the decal boxes against each cluster,
   conservatively, and writes the lists. Its dispatch size follows the
   cluster count, not the light count.
3. **Desktop consumers.** Deferred lighting, `Shadow.LocalMask`,
   transmission shading, forward blended draws and the decal resolve read
   the cluster of their pixel. They keep the wave-union iteration, so light
   and decal rows still load once per wave.
4. **Froxel injection.** Optionally read the same clusters to scatter more
   than two local lights, after its own measurement.
5. **Tiled pipeline.** It keeps the CPU grid and the 16-light policy.
   ADR-087 bounds its forward shader by register cost on Apple GPUs, and no
   tiled measurement asks for more lights.
6. **Tile classification.** Deferred lighting keeps its 8x8 classification
   tiles (decision of 2026-10-20). Revisit only if aligning it with the
   cluster tile measurably pays.

## Owner decisions (2026-10-20)

The owner accepted each recommendation below on 2026-10-20.

1. **Cluster space.** Frustum clusters or a world grid with cascades.
   Decision: frustum clusters. Every consumer above shades a pixel in
   the camera's view. The dynamic diffuse layer traces off-screen with its
   own list of at most 128 lights (`VKR_DYNAMIC_VOLUME_LIGHTS_MAX`,
   [ADR-119](../adr/119-dynamic-diffuse-volume-layer.md)); a world grid
   pays only when a ray-traced consumer needs many off-screen lights.
2. **Build method.** A per-cluster test in two levels (screen tile, then
   depth slice) or Eternal's compute rasterizer. Decision: the
   two-level test first; the rasterizer only if the build exceeds its
   budget at the new limits.
3. **Cluster grid.** Tile size and slice count. Decision: time two
   shapes, 32x32 and 64x64 px tiles with exponential slices, on the scoped
   case; keep the one with the lower total cost of build and consumers.
4. **Limits.** The new light and decal limits are an owner decision.
   Decision: size them from the scoped case and the list memory per
   frame slot, and keep the ranked table with hysteresis and fades above
   the limit, so lights past it fade instead of popping.
5. **Packet contract.** The tiled pipeline keeps the 128-bit CPU grid.
   Decision: the desktop build reads the packet's light table, and the
   packet keeps `VkrPointLightGrid` for Metal; a larger table needs the CPU
   grid to cover only the tiled class's 16 lights or the first 128 rows.

## Evidence needed

- A scoped Bistro case, added under `tools/cases/` with a fixture scene:
  the street view with many short-range moving lights and more decals than
  64, in timing and capture variants. Never Sponza (AGENTS.md).
- Matched, capture-free Release reports before and after on Vulkan, with
  equivalent work and output, the build pass's GPU time and the CPU time the
  removed grid builds took ([`vkr-performance`](../../.codex/skills/vkr-performance/SKILL.md)).
- Image comparison on the existing Bistro lamp and decal capture cases,
  where the cluster lists must light and decal every pixel as the grids do.
- One native Vulkan synchronization-validation run of the new pass.
- The light-contribution counters that rank local shadows (ADR-019) keep
  their results.

## Risks

- Lists sized for the worst case cost memory in every frame slot; a full
  cluster needs a deterministic overflow rule that drops the lowest-ranked
  lights, never the nearest.
- The GPU build runs every frame, while the CPU light grid rebuilds only
  when the selection changes.
- The desktop and tiled pipelines look lights up in different structures
  until the tiled pipeline changes; ADR-087 allows this class difference.
- Bistro alone shows little gain. Only the scoped case can show the benefit
  of the higher limits.
