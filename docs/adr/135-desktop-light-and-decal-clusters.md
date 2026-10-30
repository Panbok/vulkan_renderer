---
status: partial
updated: 2026-10-28
authority: adr
---

# ADR-135: Desktop light tiles and decal clusters

## Status

Accepted (partial). Since 2026-10-27 the desktop pipeline bins punctual
lights and decals on the GPU each frame, in place of the CPU world grids it
read before. Each 32 px screen tile holds a bitfield of the light rows that
reach it, with one bit per row of the table, so the desktop light loops
shade every row; deferred lighting, transmission shading, forward blended
draws and `Shadow.LocalMask` walk it. Each frustum cluster, a tile's depth
slice, lists the decal rows that reach it for the decal resolve. Since
2026-10-28 the runtime builds no CPU light or decal grid at all: the tiled
pipeline lists its lights and decals per screen tile on the GPU too
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 11). Native
evidence is Vulkan on an RX 6700 XT (Evidence). Since 2026-10-28 the light
table holds 1024 rows (`VKR_MAX_SCENE_POINT_LIGHTS`); the tiled pipeline
draws the 64 nearest. Depth bins for the light tiles, per-tile lists
past the bitfields and the cluster counters in metrics are open
([Clustered light and decal binning](../proposals/clustered-light-and-decal-binning.md)).

## Context

The light table held at most 128 rows and the frame's decals at most 64,
because the CPU grids name rows in 128-bit and 64-bit masks
([ADR-019](019-bounded-forward-spatial-lighting.md),
[ADR-092](092-projected-decals.md)). The owner accepted GPU-built frustum
clusters on 2026-10-20 and, on 2026-10-27, scaling targets of about 2,000
lights and 15,000 decals with bitfields rather than lists for lights.
Bistro's lights overlap genuinely, so finer binning alone does not pay
there; the gain is the limits.

Matched timing on the Bistro orbit (`Lighting.Deferred` p50) decided the
light representation. An index list per cluster for every light cost
0.17 ms per frame in dependent loads; a light loop that also walked a tail
list, even never executed, cost about 0.05 ms in registers. A bitfield that
the wave ORs and walks as scalars costs what the old grid mask cost within
0.07 ms, and dropping the depth slices for lights changed nothing (3.718
against 3.716 ms): on Bistro the shader's range test per light is cheaper
than depth culling saves.

## Decision

1. **Pass.** `Lighting.Clusters` (`pass.light_clusters`,
   [`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json))
   runs after `Cull.Encode` under `scene_rendering`. It writes four
   per-frame-slot graph buffers: `light_cluster_headers` (2 bytes per render
   pixel: the decal clusters, then the light tiles' words, 16-byte
   aligned), `light_cluster_indices` (4 bytes per render pixel),
   `light_cluster_boxes` (98304 bytes) and `light_cluster_state` (16 bytes
   of counters). Every pass that shades punctual lights or decals reads the
   headers and indices.
2. **Grid.** 32 px screen tiles; decals also use 32 slices of clip w,
   exponential from the near plane to the far plane or 2000 m
   (`VKR_VULKAN_LIGHT_CLUSTER_*`,
   [`vkr_vulkan_internal.h`](../../renderer/src/vulkan/vkr_vulkan_internal.h)).
   A view that does not project, or an extent whose tiles do not fit the
   header buffer, keeps one tile and cluster that hold every light and
   decal.
3. **Build.** `vkr_vk_prepare_light_clusters`
   ([`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c))
   fills the frame's `VkrVulkanLightClusters` record and queues two
   dispatches
   ([`light_clusters.slang`](../../renderer/src/shaders/vulkan/slang/world/light_clusters.slang)).
   The prepare dispatch, one thread per light and decal, bounds each by a
   world sphere (a light's range, or the sphere around a narrow spot cone;
   a decal's box) and a box of clusters. The bin dispatch gives each tile
   one workgroup: it marks the items whose box covers the tile and whose
   sphere reaches the tile's four side planes, writes the tile's
   `ceil(light count / 32)` light words, then lets one thread per slice
   keep the decals whose slice range holds it. A light without a finite
   range reaches every tile. One build marks at most 4096 lights and
   decals together. Since 2026-10-30 the desktop shades the light rows
   `[0, min(count, 4096 - decals))`
   (`vkr_vulkan_shaded_point_light_limit`). The light head, rows
   `[0, 128)`, and the 512 decals always fit. Rows past the limit stay
   unshaded and `lighting.point.unshaded` counts them. This holds until
   per-tile light lists replace the bitfields (proposal W4c), which lets
   the table grow past 1024 rows.
4. **Decal cluster.** An 8-byte `VkrVulkanLightCluster` names a run of the
   index pool: its decal rows in ascending order. A list that does not fit
   the pool is dropped whole and counted, so a pixel never reads another
   cluster's indices.
5. **Consumers.** `packet_punctual_layered`
   ([`default.slang`](../../renderer/src/shaders/vulkan/slang/world/default.slang))
   walks the tile's words in order, ORs each across the wave and loads each
   light row as a scalar. Deferred lighting and `Shadow.LocalMask` take the
   tile from their render pixel, and skip, once per wave, a ranged light
   whose range misses the box of the wave's world positions, which no pixel
   of the wave could pass the range test for (2026-10-28); forward and
   transmission shading project their world position. `vkr_vk_apply_decals`
   ([`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang))
   merges the lanes' decal lists with `WaveActiveMin`, so each decal row
   loads once per wave and decals blend in table order.
6. **Light head.** Every row shades, but only the head's rows (below
   `VKR_POINT_LIGHT_GRID_LIGHT_MAX`, 128) cast shadows, take a
   `Shadow.LocalMask` slot and add to the contribution counters; the host
   uploads `light_first_view` only for them, so later rows shade
   unshadowed.
7. **Table addresses.** The host always uploads at least one light row and
   one decal row, zero when the frame has none, so no shader reads through
   a null table address (the AMD driver may hoist such a load above its
   guard).
8. **Light table.** Since 2026-10-29 the light rows live in `light_table`,
   a retained graph buffer of 65536 bytes (1024 rows; ADR-029).
   `Lighting.Scatter` (`pass.light_scatter`) runs before
   `Lighting.Clusters` and writes the frame's updates into it, one thread
   per update (`vk_light_table_scatter`); every pass that reads light rows
   declares a read of the table, so the graph orders the scatter after the
   previous frame's reads and every read after the scatter. The updates
   are the rows the renderer lists as changed since the last submitted
   frame (`VkrLightTableFrame`, `vkr_light_table.h`), or every row when the
   renderer's list is full or the graph does not keep the table's
   contents. A frame without scene rendering declares no table and reads
   its uploaded rows.

## Consequences

- The desktop keeps the 512 decals nearest the camera
  (`VKR_MAX_FRAME_DECALS`) and the tiled pipeline the 64 nearest
  (`VKR_TILED_FRAME_DECALS`), both in compositing order
  ([ADR-092](092-projected-decals.md)). Neither desktop limit is a mask
  width any longer; the build's 4096 items, the header buffer's tile words
  and the index pool bound them.
- Vulkan no longer reads the CPU light and decal grids, and since
  2026-10-28 the runtime builds neither. The build costs about 0.014 ms
  of GPU time on the Bistro orbit and runs every frame.
- A tile's light loop walks every word of the table, so its cost grows
  with the light count even where no light reaches; depth bins that narrow
  the words a pixel walks are the planned remedy at thousands of lights.
- The desktop and tiled pipelines look lights and decals up in different
  structures, as ADR-087 allows between the classes.

## Alternatives considered

- **Index lists for every light.** Rejected by matched timing (Context).
- **A head mask with a tail-list loop.** Rejected: the unexecuted tail walk
  cost registers on every frame (3.93 ms against 3.73 to 3.75 ms for
  `Lighting.Deferred`).
- **Light bitfields per depth slice.** No gain on Bistro over the 2D tile,
  and about 29 MB at 2,000 lights; depth bins over depth-sorted lights
  (the proposal's W4a) keep the culling at a few kilobytes.
- **A world grid with cascades for raster consumers.** Rejected by owner
  decision 1: every raster consumer shades a pixel in the camera's view.
- **A compute rasterizer of light volumes.** Deferred until the per-tile
  build exceeds its budget at higher limits (owner decision 2).
- **Decal bitfields per tile in table order.** Measured on 2026-10-27 and
  kept off: they render identically and drop the index pool and slices,
  but on `bistro_clustered_1440` (176 decals, profile
  `local-offscreen-gpu-repeated`) the G-buffer resolve took 1.452 ms
  against 1.364 ms with the cluster lists, and 1.435 ms with 64 global
  depth bins of clip w added: a tile and a screen-wide depth bin cull far
  less than a tile's own slice. The owner kept the lists; the code stays
  on the local branch `w4b-decal-bits`.

## Revisit when

- The stress case shows the word loop dominating at its light count: add
  depth bins (W4a).
- The stress case's decal counts make the lists' pool or the per-cluster
  loads the bottleneck: measure decal bitfields again there.
- The pool overflows on a tracked case.

## Evidence

Vulkan Release, RX 6700 XT, 2560x1440 High, validation unset.

- **Per-wave range skip (2026-10-28).** A CPU estimate from the depth of
  `bistro_clustered_stress_capture` gives 106 lights tested per pixel from
  the 32 px tile bitfields, 48 with tile depth bounds and 18 in range. The
  skip, measured with the shader catalog against the same build under
  `local-offscreen-gpu-repeated`: `bistro_clustered_stress_1440`
  `Lighting.Deferred.Fullscreen` 10.185 to 9.976 ms (run
  `20261028T124433.084Z-000465`), `bistro_clustered_dense_1440` 15.026 to
  14.698 ms (`20261028T125526.425Z-0027a8`), and the Bistro orbit under
  `local-windowed-gpu` 3.668 against 3.662 ms. Rejecting an out-of-range
  light was already cheap, so the lights in range set the cost, and tile
  depth bounds or per-tile lists wait for tables past 1024 rows (proposal
  item 3). The lamp HDR capture is byte-identical to a run without the skip;
  one earlier run without it differed in 68,410 pixels, as two runs of the
  same shaders can, since the shadowed head follows contribution samples.
- **1024-row table (2026-10-28).** With `VKR_MAX_SCENE_POINT_LIGHTS` at
  1024, `bistro_clustered_1440` selects all 392 lights and drops none
  (`lighting.point.selected` 392); with the half-resolution local shadow
  mask (ADR-019), `Lighting.Deferred.Fullscreen` reads 6.395 ms against
  5.469 ms at 128 rows, about 3.5 us per extra light, and the Lighting
  group 10.357 against 9.321 ms (`local-offscreen-gpu-repeated`, run
  `20261028T113448.448Z-001779`, main checkout, non-authoritative). The
  `bistro_clustered_capture` frame shows the extra lamps lit with no tile
  or seam faults (`20261028T114442.196Z-002855`). The Debug CPU suite passes
  on Windows, including the head trade test that compiles only when the
  table is wider than the head; Metal has not run it.
- **Image parity.** The first clusters against the grid build: the decal
  G-buffer capture and the lamp HDR captures are byte-identical; two lamp
  runs differed only in foliage local-shadow state, which also varies
  between identical runs and is identical with local shadows off. The
  light tiles against those clusters, on one build with each shader
  catalog: `bistro_clustered_capture` depth and G-buffer identical, and the
  lamp HDR capture identical in all four cross pairs of runs matched by
  shadow state.
- **Without CPU grids** (54c10645, 2026-10-27): `bistro_clustered_capture`
  (`snapshot`, profile `local-offscreen`) passes with 0 GPU and log errors;
  it selects 128 lights and drops 264, keeps all 176 decals, and reports
  no light grid cells.
- **Timing of the first clusters**, 2026-10-27: before (ca993d2c) and after
  builds in clean worktrees, alternated ABBA, all eight reports
  authoritative (`vkr_harness profile --profile
  tools/profiles/performance-windowed-gpu-submission-vulkan.json`, cases
  `bistro_vulkan_high_orbit_1440` and `bistro_vulkan_high_street_walk_1440`).
  `gpu.submission` p50: orbit 12.92 and 12.80 ms before, 13.03 and 12.89 ms
  after (+0.8 %, `no_change` twice); walk 13.09 and 13.11 ms before, 13.42
  and 13.21 ms after (+1.7 %; one pair `slower` at +2.5 %, one
  `no_change`). The owner accepted the cost on 2026-10-27.
- **Timing of the light tiles**, shader-only A/B on one build
  (`VKR_SHADER_CATALOG`, orbit, profile `local-windowed-gpu`,
  `Lighting.Deferred` p50): the old grid 3.649 ms; the first clusters
  3.713 to 3.721 ms; the light tiles 3.714 to 3.717 ms, with
  `Lighting.Clusters` 0.014 ms against 0.025 ms. A second cluster lookup
  added 0.036 ms, and taking the tile from the pixel instead of a
  projection saved nothing measurable, so the remaining 0.07 ms against
  the grid is not the lookup's arithmetic.
- **Past the head** (table constant raised to 1024 in a local build only):
  `bistro_clustered_capture` selects all 392 lights with 0 GPU and log
  errors; its 64 moving lights change 436,501 pixels of the HDR image
  between measured frames 0 and 60, against 83 when only 128 rows shade.
  `bistro_clustered_1440` (profile `local-offscreen-gpu-repeated`):
  `Lighting.Deferred` 5.82 ms with 128 lights and 6.90 ms with 392,
  `Lighting.Clusters` 0.028 and 0.034 ms.
- **Validation:** one selective GPU-assisted validation run of the walk
  (`VKR_VULKAN_GPU_VALIDATION=1`, the resolve, local shadow mask, cluster,
  deferred lighting, transmission and world shaders instrumented): two
  repetitions and two prewarm children of 1,326 frames each, 0 validation
  errors and warnings. Before decision 7 about one prewarm child in twenty
  lost the device with a device fault at address 0; none of the 8 timing
  runs after it did. That run and the synchronization run below predate
  the light tiles.
- **Synchronization validation**, 2026-10-27: Release with
  `VK_LAYER_KHRONOS_validation` loaded through `VK_INSTANCE_LAYERS` and
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`, case
  `bistro_lamps_baked_night_moving_validation`, profile `local-offscreen`:
  no errors or warnings.
