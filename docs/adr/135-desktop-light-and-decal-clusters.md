---
status: partial
updated: 2026-10-27
authority: adr
---

# ADR-135: Desktop light and decal frustum clusters

## Status

Accepted (partial). Since 2026-10-27 the desktop pipeline bins punctual
lights and decals into frustum clusters that the GPU builds each frame, in
place of the CPU world grids it read before. Deferred lighting,
transmission shading, forward blended draws and `Shadow.LocalMask` walk a
cluster's mask of the 128-row light head; the decal resolve walks the
cluster's decal list, so the desktop decal table is no longer bounded by a
mask width. The tiled pipeline keeps the CPU grids. Native evidence is
Vulkan on an RX 6700 XT (Evidence). Shading light rows past the head and
the cluster counters in metrics are open; the owner chose z-binned
bitfields and a world grid for ray-traced consumers to scale past them
([Clustered light and decal binning](../proposals/clustered-light-and-decal-binning.md)).

## Context

The light table held at most 128 rows and the frame's decals at most 64,
because the CPU grids name rows in 128-bit and 64-bit masks
([ADR-019](019-bounded-forward-spatial-lighting.md),
[ADR-092](092-projected-decals.md)). The owner accepted frustum clusters
built on the GPU with a two-level test on 2026-10-20 (the proposal's owner
decisions 1 to 5). Bistro's lights overlap genuinely, so finer binning
alone does not pay there; the gain is the limits.

The first implementation replaced the masks with index lists in every
consumer. Matched timing rejected it: the dependent index load per light
and pixel made `Lighting.Deferred` 0.17 ms slower and the frame 2.6 %
slower on the orbit. A light loop that also walked a tail list, even never
executed, cost about 0.05 ms in registers.

## Decision

1. **Pass.** `Lighting.Clusters` (`pass.light_clusters`,
   [`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json))
   runs after `Cull.Encode` under `scene_rendering`. It writes four
   per-frame-slot graph buffers: `light_cluster_headers` (2 bytes per render
   pixel), `light_cluster_indices` (4 bytes per render pixel),
   `light_cluster_boxes` (49152 bytes) and `light_cluster_state` (16 bytes
   of counters). Every pass that shades punctual lights or decals reads the
   headers and indices.
2. **Grid.** 32 px screen tiles by 32 slices of clip w, exponential from the
   near plane to the far plane or 2000 m
   (`VKR_VULKAN_LIGHT_CLUSTER_*`,
   [`vkr_vulkan_internal.h`](../../renderer/src/vulkan/vkr_vulkan_internal.h)).
   A position's cluster is the tile of its projection by the jittered view
   projection and the slice of its clip w
   (`vkr_vk_light_cluster_header`). A view that does not project, or an
   extent whose tiles do not fit the header buffer, keeps one cluster that
   holds every light and decal.
3. **Build.** `vkr_vk_prepare_light_clusters`
   ([`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c))
   fills the frame's `VkrVulkanLightClusters` record and queues two
   dispatches
   ([`light_clusters.slang`](../../renderer/src/shaders/vulkan/slang/world/light_clusters.slang)).
   The prepare dispatch, one thread per light and decal, bounds each by a
   world sphere (a light's range, or the sphere around a narrow spot cone;
   a decal's box) and a box of clusters. The bin dispatch gives each tile
   one workgroup: it marks the items whose box covers the tile and whose
   sphere reaches the tile's four side planes, then one thread per slice
   keeps the items whose slice range holds it. A light without a finite
   range reaches every cluster. One build marks at most 2048 lights and
   decals together.
4. **Cluster.** A 32-byte `VkrVulkanLightCluster` holds a 128-bit mask of
   the light head's rows (rows below `VKR_POINT_LIGHT_GRID_LIGHT_MAX`) and a
   run of the index pool: the light rows past the head, then the decal
   rows, each ascending. A list that does not fit the pool is dropped whole
   and counted, so a pixel never reads another cluster's indices.
5. **Consumers.** `packet_punctual_layered`
   ([`default.slang`](../../renderer/src/shaders/vulkan/slang/world/default.slang))
   ORs the wave's head masks and loads each light row as a scalar, as the
   grid mask did; `Shadow.LocalMask` ands the head mask with its shadowed
   lights. `vkr_vk_apply_decals`
   ([`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang))
   merges the lanes' decal lists with `WaveActiveMin`, so each decal row
   loads once per wave and decals blend in table order.
6. **Light head.** The desktop light loops walk only the head mask. Rows
   past the head would need a kernel variant with the tail walk, selected
   only for frames with more lights, so `VKR_MAX_SCENE_POINT_LIGHTS` stays
   at the head's 128 rows; a static assertion in `vkr_vulkan_internal.h`
   stops a larger table from building without that variant. Shadows,
   contribution counters and `light_first_view` cover the head only.
7. **Table addresses.** The host always uploads at least one light row and
   one decal row, zero when the frame has none, so no shader reads through
   a null table address (the AMD driver may hoist such a load above its
   guard).

## Consequences

- The desktop decal limit is no longer a mask width: it is bounded by
  `VKR_MAX_FRAME_DECALS`, the build's 2048 items and the index pool.
- Vulkan no longer uploads the CPU light and decal grids. The build costs
  about 0.025 ms of GPU time on the Bistro orbit and runs every frame.
- The desktop and tiled pipelines look lights and decals up in different
  structures, as ADR-087 allows between the classes.
- Light rows past 128 are listed in the clusters but not shaded until the
  tail-light variants land.

## Alternatives considered

- **Index lists for every light.** Rejected by matched timing (Context).
- **A head mask with a tail loop in the same kernel.** Rejected: the
  unexecuted tail walk cost registers on every frame (3.93 ms against
  3.73 to 3.75 ms for `Lighting.Deferred`).
- **A world grid with cascades.** Rejected by owner decision 1: every
  consumer shades a pixel in the camera's view.
- **A compute rasterizer of light volumes.** Deferred until the two-level
  build exceeds its budget at higher limits (owner decision 2).

## Revisit when

- Z-binned light bitfields measure cheaper than the head mask at 128
  lights, or a scene needs more than 128 shaded lights: they replace these
  clusters for lights (the proposal's W4a, owner decision of 2026-10-27).
- Decal bitfields in table order replace the decal lists (W4b).
- The pool overflows on a tracked case before then.

## Evidence

Vulkan Release, RX 6700 XT, 2560x1440 High, validation unset.

- **Image parity** against the grid build: the decal G-buffer capture and
  the lamp HDR captures are byte-identical; two lamp runs differed only in
  foliage local-shadow state, which also varies between identical runs and
  is identical with local shadows off. `bistro_clustered_capture` keeps
  128 of 264 lights and 64 of 112 decals.
- **Timing**, 2026-10-27: before (ca993d2c) and after builds in clean
  worktrees, alternated ABBA, all eight reports authoritative
  (`vkr_harness profile --profile
  tools/profiles/performance-windowed-gpu-submission-vulkan.json`, cases
  `bistro_vulkan_high_orbit_1440` and `bistro_vulkan_high_street_walk_1440`).
  `gpu.submission` p50: orbit 12.92 and 12.80 ms before, 13.03 and 12.89 ms
  after (+0.8 %, `no_change` twice); walk 13.09 and 13.11 ms before, 13.42
  and 13.21 ms after (+1.7 %; one pair `slower` at +2.5 %, one
  `no_change`). Per pass on the orbit (2026-10-26): `Lighting.Clusters`
  0.025 ms, `Lighting.Deferred` about 0.1 ms over the grid, which the
  per-pixel cluster lookup is suspected of. The owner accepted the cost on
  2026-10-27; z-binning (the proposal's W4a) is the planned fix.
- **Validation:** one selective GPU-assisted validation run of the walk
  (`VKR_VULKAN_GPU_VALIDATION=1`, the resolve, local shadow mask, cluster,
  deferred lighting, transmission and world shaders instrumented): two
  repetitions and two prewarm children of 1,326 frames each, 0 validation
  errors and warnings. Before decision 7 about one prewarm child in twenty
  lost the device with a device fault at address 0; none of the 8 timing
  runs after it did.
- **Pending:** the synchronization-validation run of the pass and the
  moving-light check on the scoped case.
