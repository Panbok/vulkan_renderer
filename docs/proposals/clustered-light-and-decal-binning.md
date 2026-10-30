---
status: proposed
updated: 2026-10-28
authority: proposal
---
# Clustered light and decal binning

This proposal scales punctual lights and decals on both pipeline classes to
tens of thousands: up to 65,535 lights a frame, addressed by 16-bit
indices. The desktop pipeline bins lights on the GPU into screen tiles, the
tiled pipeline culls them in tile shaders, and a world grid serves
ray-traced consumers. Its first steps shipped as
[ADR-135](../adr/135-desktop-light-and-decal-clusters.md). It takes over
item 4 of [Lighting efficiency](lighting-efficiency.md), camera-space
clustered culling. What decals write stays in
[Decal channels and receivers](decal-channels-and-receivers.md).

## Current baseline

- **Light tiles and decal clusters (ADR-135).** On the desktop pipeline
  `Lighting.Clusters` builds, each frame, a bitfield of light rows per
  32 px screen tile, which deferred lighting, transmission, forward blend
  and `Shadow.LocalMask` walk, and decal lists per frustum cluster (the
  tile by 32 exponential slices), which the decal resolve walks. Every
  light row shades; only the 128-row head is shadowed. The build costs
  about 0.014 ms on the Bistro orbit (RX 6700 XT, 1440p). One build marks
  at most 4,096 lights and decals.
- **Limits.** The light table holds at most 1024 rows since 2026-10-28 (M4;
  `VKR_MAX_SCENE_POINT_LIGHTS`,
  [`vkr_lighting.h`](../../renderer/src/vkr_lighting.h)), ranked on the CPU
  by camera distance with hysteresis and fades and uploaded every frame
  ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)); the first
  128 rows (the head) can be shadowed. The desktop shades every row, so
  raising the constant was the whole change; at 1024 the scoped case
  shades all 392 of its lights, about 4 us of
  `Lighting.Deferred` per extra light at 1440p. The desktop keeps the 512
  nearest decals ([ADR-092](../adr/092-projected-decals.md)).
- **Ray-traced consumers.** The diffuse volume's dynamic layer traces
  off-screen with its own list of at most 128 lights
  (`VKR_DYNAMIC_VOLUME_LIGHTS_MAX`,
  [`vkr_frame_input.h`](../../renderer/src/vkr_frame_input.h),
  [ADR-119](../adr/119-dynamic-diffuse-volume-layer.md)). Ray-traced
  reflections are planned to replace probes and planar reflections; they
  will shade hit points off-screen too.
- **Tiled pipeline.** Since 2026-10-28 (M5 lights) a tile kernel at the
  start of `Tiled.Opaque` and `Tiled.Blend` lists each 32x16 tile's lights
  in threadgroup memory, and fragments walk their depth bin's part of it;
  the class draws the 64 dynamic lights nearest the camera, about 0.7 ms
  per extra light reaching each pixel at 1280x720 on the M1 Pro. The same
  kernel lists the 64 nearest decals per tile in table order
  ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md), decision 11).
- **Measured on Vulkan** (orbit, `Lighting.Deferred` p50, shader-only
  A/Bs, 2026-10-26 and 2026-10-27): the old CPU grid 3.65 ms; an index
  list for every light 3.82 ms; tile bitfields 3.71 to 3.72 ms. Per-pixel
  index loads cost more than bitfield walks at Bistro's counts, and
  dropping depth culling for lights changed nothing (3.718 against
  3.716 ms).

## Reference

| Game | Binning | Scale |
|---|---|---|
| Doom (2016) | CPU, 16x8x24 = 3,072 frustum clusters with log depth slices; up to 256 lights, decals and probes per cluster | Tens of lights |
| Doom Eternal | Compute rasterizer: light and decal hexahedra into 256 px tiles, then 32 px tile bitfields and froxels; reverse float depth; under 500 us | Hundreds of lights, thousands of decals |
| The Dark Ages | Screen clusters for raster lighting with 32x32 tiles; a world grid of 16^3 cells x 8 cascades, 64 ids per cell, on async compute, about 0.2 ms on consoles, 30 MB, also read by ray tracing, particles and glass | 4,000 lights and 30,000 decals in retail builds |
| Call of Duty: Infinite Warfare | Z-binning: lights sorted by view depth, a bitfield of lights per screen tile, and 1D depth bins that hold each bin's lowest and highest light index | Thousands of lights |
| PETAFAB (Aaltonen, 2026) | Metal 4 tile-shader local light culling, GPU-driven indirect rendering with task-shader meshlet culling, bindless with GPU pointers; distance-field ray tracing for primary rays, soft shadows and AO | Up to 65,535 lights; a locked 60 Hz on an iPhone 15 Pro |

Eternal moved binning to the GPU because large levels piled distant small
lights into large clusters and combat needed more dynamic lights and decals.
PETAFAB shows that tile-shader culling into 16-bit lists reaches tens of
thousands of lights on a phone GPU; the posts give no visible light count
or per-pass timings.
Sources: [Courrèges, Doom (2016)](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/),
[Geffroy et al., SIGGRAPH 2020](https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf),
[Coenen, Doom Eternal](https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy),
[Sousa, SIGGRAPH 2025](https://www.advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf),
[Lazarek and Hammer, GPC 2025](https://static.graphicsprogrammingconference.com/public/2025/talks/visibility-buffer-and-deferred-rendering-in-doom-the-dark-ages/Lazarek-Hammer-visibility-buffer-and-deferred-rendering-in-doom-the-dark-ages.pdf),
Drobot, "Improved Culling for Tiled and Clustered Rendering", SIGGRAPH 2017
Advances in Real-Time Rendering,
[Aaltonen, PETAFAB on iPhone 15 Pro](https://x.com/SebAaltonen/status/2114806977855623426),
[Aaltonen, PETAFAB renderer features](https://x.com/SebAaltonen/status/2114813074225983999).

## Targets

Up to 65,535 lights a frame on both pipeline classes, the reach of 16-bit
light indices (owner, 2026-10-28), and about 15,000 decals on the desktop
pipeline. On the way, the desktop's bitfields serve about 2,000 lights,
the target of 2026-10-27. Shadows stay with the 128-row light head and the
shadow atlas, so lights past the head are unshadowed. What a scene can
afford is set by how many lights reach each pixel, not by the table size.

## Proposed work

1. **GPU-resident light table (both classes).** One table of up to 65,535
   rows that both backends read through 16-bit indices. Static lights
   upload once and change only when edited; moving and changed lights
   stream as row updates. The CPU stops ranking every light each frame:
   it keeps ranking the candidates for the 128-row shadow head, with its
   hysteresis and fades, and the rows past the head need no CPU order. The
   packet carries the table and its updates in place of a per-frame row
   array; ADR-019 and the packet contract change with it.
2. **Tiled pipeline: tile-shader culling (Mac).** The tiled class has no
   depth prepass (ADR-087 decision 5), and the owner kept it that way on
   2026-10-28: a second raster of Bistro would cost about as much vertex
   work again (1.6 ms on the M1 Pro). A tile shader at the start of
   `Tiled.Opaque` tests the candidate lights against each tile's screen
   frustum and writes their 16-bit indices to threadgroup memory; the
   tile's fragment shading walks only that list, with each light's depth
   range tested per pixel (depth bins), since no tile depth bounds exist. A coarse GPU pass
   first bins the lights into large screen cells (for example 128 px), so
   a tile tests a few hundred candidates, not the whole table. Decals take
   the same path in table order. This replaces the CPU light and decal
   grids and the 16-light policy (ADR-087 decision 11), which the tiled
   class's ADR revises when it lands.
3. **Desktop lights past the bitfields (W4c).** The per-tile bitfields stay
   while the table is small: their loop walks every word of the table, so
   at tens of thousands of lights they cost about 8 KB a tile and a long
   word loop per pixel. Past a measured crossover the desktop uses the
   tiled class's structure in device memory: a coarse GPU pre-bin, then a
   fine per-tile cull into compact 16-bit lists, with depth bounds from the
   depth prepass for deferred lighting and depth bins for glass and
   forward blend. Measure lists against bitfields on the stress case to
   set the crossover. On 2026-10-28 a per-wave range skip, which drops what
   tile depth bounds would, saved only 0.2 to 0.3 ms of 10 to 15 ms on the
   stress cases at 1,024 rows (ADR-135, Evidence): below that size the
   lights in range set the cost, so the crossover lies past 1,024 rows and
   waits for M6's table.
4. **Z-binned light bitfields (W4a).** Stage 1 landed: per-tile light
   bitfields in table order with the tile taken from the pixel. Depth bins
   that narrow the words a pixel walks come only if the stress case shows
   the word loop dominating below the crossover.
5. **Decals (W4b).** Decals keep their frustum-cluster lists for now.
   Per-tile decal bitfields in table order, alone or with 64 global depth
   bins, made the G-buffer resolve 0.07 to 0.09 ms slower on the scoped
   case (ADR-135, Alternatives), so the owner kept the lists on
   2026-10-27. With the stress case, size the index pool and the item cap
   for the decal target, raise `VKR_MAX_FRAME_DECALS`, and measure again.
6. **World grid for ray-traced consumers (W5).** A camera-centred grid with
   cascades, built on the GPU from the same light table, with a fixed list
   of lights per cell and an overflow rule that keeps the strongest lights.
   The diffuse volume's dynamic layer reads it in place of its own 128-light
   list, and ray-traced reflections read it when they land; it may later
   feed stochastic many-light sampling (reservoirs per cell). It is the
   candidate for async compute.
7. **Build cost and async compute.** The screen build needs only the camera
   and the light table, so it can overlap G-buffer raster. Move a build to
   async compute only when its measured cost passes about 0.2 ms; Vulkan
   has one queue today, and [Asynchronous compute](async-compute.md) owns
   the compute queue and its rules.
8. **Froxel injection.** Optionally read the same structures to scatter
   more than two local lights, after its own measurement.
9. **Tile classification.** Deferred lighting keeps its 8x8 classification
   tiles.

## Design: GPU-resident light table (item 1)

Mac design for review by Windows (work split M6, 2026-10-28).

**Today.** Each frame `sync_point_lights_cb` reads every point-light entity,
`vkr_lighting_system_select_point_lights` ranks up to 2,048 candidates with
several sorts and copies up to 1,024 rows
([`vkr_lighting_system.c`](../../runtime/src/renderer/systems/vkr_lighting_system.c)),
and both backends pack every row into per-frame upload memory
(`vkr_metal_packet_write_upload_lights`, `vkr_vk_upload_point_lights`).
`vkr_point_light_pack` multiplies each row's intensity by the frame's
pre-exposure ([`vkr_lighting.c`](../../renderer/src/vkr_lighting.c)), so
automatic exposure changes every row every frame. Nothing keeps lights on
the GPU between frames.

1. **Rows.** Every enabled, visible point or spot light owns one row of a
   dense table `[0, count)`, with `count` at most 65,535 and `0xFFFF` as the
   empty 16-bit index. The row stays the 64-byte `VkrGpuPointLightRow`
   (4 MB at capacity). A light keeps its row while it stays in or out of
   the shadow head (item 5). Removing a light moves the last row into its
   hole, so the table stays dense and the desktop bitfields walk `count`
   rows, not the capacity. The lighting system maps `render_id` to row.
2. **Exposure leaves the rows.** Shaders multiply a row's intensity by the
   frame's pre-exposure where they read it. This changes shaders on both
   backends and lands first and alone, with image parity on both classes.
   The audit covers every reader of a row's colour and intensity (`p1.rgb`,
   `p2.x`): desktop deferred, transmission, forward blend, the
   `Shadow.LocalMask` cutoff and the contribution counters, the tiled
   forward and blend passes, and the diffuse volume's dynamic layer, which
   packs its own rows. `vkr_local_light_contribution`
   ([`local_shadow.slangh`](../../renderer/src/shaders/shared/local_shadow.slangh))
   multiplies by pre-exposure itself, so the cutoff and counters may apply
   it twice today; 2a checks that.
3. **Updates.** The runtime sends a frame's row updates as pairs of row
   index and row: new and edited lights, moved lights, the row a removal
   moves, and shadow-view changes (item 5). The packet carries the
   updates, the row count and a table generation. A new generation (scene
   load, capacity growth, device loss, or more updates than the frame's
   budget) sends every row. Packet validation checks each update (row
   below the count, finite values, valid kind); unchanged rows are not
   validated again.
4. **Change detection.** The lighting system keeps each light's last
   written world position and direction and its component revision. When
   the scene's `transform_revision` and the light component revisions have
   not changed, no row changes. Otherwise it compares each light's world
   position and direction with the cached ones: one read per light, no
   sort and no row copy. The scene's changed-entity list
   (`vkr_scene_changed_entities`) cannot replace the compare: it omits
   children that move with their parent, which is how movers carry lights,
   and it has one consumer. Edits overwrite `ScenePointLight` in place
   ([`vkr_scene_edit.c`](../../runtime/src/renderer/systems/vkr_scene_edit.c)),
   so the component needs a revision. The sync takes a spot light's
   direction from the entity's local rotation, not its world rotation; the
   change fixes that, which changes spot lights under rotated parents.
5. **Shadow head.** The CPU ranks only the lights that cast shadows, by
   camera distance with today's incumbent bonus and fades, and keeps at
   most 128. The head stays rows `[0, 128)`: on the desktop,
   `Shadow.LocalMask` reads only a tile's first four bitfield words, the
   mask's slot tags store row + 1 in 8 bits, the deferred slot lookup walks
   the tags in row order, and the contribution counters index rows below
   128. A light that enters the head swaps rows with the one that leaves
   (two row updates), and `light_first_view` stays per head row. A row's
   shadow view stays in `p3.w`, so a changed view sends one row update.
   Lights that cast no shadow need no ranking and no order.
6. **Capacity.** Past 65,535 lights the lights with the highest
   `render_id` are not resident, and the editor's `light_overflow` warns.
   A residency by camera distance would rank every light again whenever
   the camera moves.
7. **GPU table and last use.** Each backend owns one device-local table
   buffer, sized to the generation's count rounded up to a power of two;
   growth starts a new generation, and the old buffer retires after the
   last frame that read it, by its submit value. A graph pass at the start
   of the frame scatters the frame's updates from the frame's upload slice
   into the table, one thread per update, before `Lighting.Clusters` and
   every pass that shades lights. The upload slice is protected by the
   current per-slot retirement (Metal 2 slots, Vulkan 3). Only that pass
   writes the table, on the frame's queue; the graph declares the table as
   a persistent imported buffer, so it orders the write after the last
   frame's reads (write after read) and every read after the write. That
   order holds because one queue runs the frames; if light shading moves
   to async compute, the table needs two copies or a timeline wait. The
   table starts at a 16-byte aligned address (Vulkan device addresses).
8. **CPU rows.** The lighting system keeps the rows in a CPU array, the
   source of updates and of the CPU readers: the froxel fog's two-light
   pick, local-shadow candidates, metrics, and the tiled class's 16-light
   limit until M5. Until M5 the tiled packet names its 16 lights as row
   indices into the table, with the CPU grid over those entries.
9. **Unchanged consumers.** Contribution counters stay per head row with
   their `render_id` map (`vkr_vk_set_light_contribution_source`); the
   local shadow cache stays keyed by `render_id`; the diffuse volume's
   dynamic layer keeps its own light scan until the world grid (W5).
   Rectangle lights stay a per-frame array.
10. **Packet contract.** `VkrFrameLighting` replaces `point_lights` and
    `point_light_count` with the row count, table generation, updates and
    head ([`vkr_frame_input.h`](../../renderer/src/vkr_frame_input.h));
    ADR-019 and ADR-044 record it.

**Steps.** Each step keeps both backends working.

- **2a** Exposure out of the rows (item 2): Metal on the Mac, Vulkan on
  Windows, image parity on both classes (landed 2026-10-28: Vulkan
  ee41ebfd, Metal 745c50ec).
- **2b, phase 1** (landed 2026-10-28): the renderer packs each frame's rows
  and lists those that differ from the last submitted frame
  (`vkr_light_table.h`, ADR-019). Today's selection already keeps rows in
  place (render id order within the head and the rest, the head held by
  its incumbent bonus), so a still scene sends no rows and a moving light
  one; backends with a table between frames (2c, 2d) consume the list.
- **2b, phase 2** (with tables past 1024 rows): the runtime side, resident
  rows, change detection and ranking only the shadow head, which saves
  the CPU's per-frame ranking; until then it costs little at 1024 rows.
  CPU tests: a light keeps its row across frames, a removal moves one row,
  a new generation sends every row, and the head keeps its hysteresis.
- **2c** Metal table and scatter pass (Mac, landed 2026-10-30). **2d**
  Vulkan table and scatter pass (Windows, landed 2026-10-29).

**Status of 2a and 2d (2026-10-29).** Vulkan's half of 2a landed in
ee41ebfd. The Vulkan table and scatter pass build on 2b. The table
is not imported: it is `light_table`, a retained graph buffer (ADR-029,
which now covers buffers), so the graph itself carries the table's access
state across frames, and a buffer it recreates comes back with invalid
contents, which is item 7's new generation. Its size is fixed at 1024 rows,
the current `VKR_MAX_SCENE_POINT_LIGHTS`, rather than the generation's
count rounded up; a larger table changes that size or adds a count source.
Vulkan takes 2b's updates: it uploads the listed rows, and every row when
the list is `full` or the table's retained contents are invalid.

**Evidence.** CPU time of the lighting system and upload bytes per frame at
2,500 and 20,000 lights, before and after, on a still scene (no updates) and
on the mover cases; image parity on the clustered, stress and lamp captures
on both classes.

**Open choices** (recommendation first):

- Removal: move the last row into the hole, so loops walk a dense table;
  or a free list, which keeps every row in place but leaves holes that
  bitfield loops walk.
- Over capacity: drop by `render_id`, stable and cheap; or keep the
  nearest, which needs a residency pass on camera moves.
- Shadow view: in the row's `p3.w`, so rows stay the one source; or a
  separate per-row array, which a frame rewrites whole.

## Owner decisions

On 2026-10-20 the owner accepted these recommendations; the ones marked
revised were changed on 2026-10-27 or 2026-10-28.

1. **Cluster space** (revised). Raster consumers bin in screen space; a
   world grid with cascades serves ray-traced consumers (W5), because the
   dynamic diffuse layer and future ray-traced reflections shade hit points
   off the screen.
2. **Build method** (revised). Two levels: a coarse GPU pre-bin, then a
   fine per-tile test; on the tiled class the fine test is a tile shader.
   A compute rasterizer of light volumes only if the coarse pass exceeds
   its budget.
3. **Depth** (revised). No 3D cluster bitfields: one per cluster costs
   about 29 MB at 2,000 lights. Depth enters as tile depth bounds where the
   depth prepass exists, and as depth bins elsewhere.
4. **Limits** (revised 2026-10-28). Up to 65,535 lights through 16-bit
   indices, about 15,000 decals on the desktop; the shadow head stays at
   128 rows with its hysteresis and fades.
5. **Packet contract** (revised 2026-10-28). One GPU-resident light table
   with 16-bit indices for both classes; the tiled class's CPU grids retire
   with its tile-shader culling.
6. **Tiled pipeline** (2026-10-28). The tiled class culls lights in tile
   shaders and stops capping dynamic lights at 16.

## Evidence needed

- A Bistro stress case under `tools/cases/` with 1,000 to 2,000
  short-range lights and thousands of decals (M3), and a scaled variant
  with tens of thousands of small lights, in timing and capture variants
  for both classes; the existing `bistro_clustered_*` cases cover 392
  lights and 176 decals. Never Sponza (AGENTS.md).
- Tiled class: Metal timing of tile-shader culling against the 16-light
  path on the scoped and stress cases (M1 Pro), and its captures.
- Desktop: lists against bitfields on the stress cases, to set W4c's
  crossover; matched, capture-free Release reports before and after each
  step on Vulkan, with the build's GPU time
  ([`vkr-performance`](../../.codex/skills/vkr-performance/SKILL.md)).
- The light table: CPU time and upload bytes per frame at 2,000 and at
  tens of thousands of lights, before and after the GPU-resident table.
- Image comparison on the Bistro lamp, decal and clustered capture cases.
- One native validation run of each new pass on its backend.
- W5: the dynamic diffuse layer's image and cost with the world grid
  against its 128-light list.

## Risks

- Shading cost per pixel, not culling, bounds the light count: Bistro's
  lights overlap with up to 28 in one 1 m cell, and tens of thousands of
  lights only fit where each pixel sees a few dozen.
- A per-tile list has a fixed capacity in tile or device memory; a tile
  that overflows needs a rule that keeps the strongest lights.
- The GPU-resident table moves light selection off the CPU; editor tools,
  shadows and the contribution counters that read table rows today must
  follow it.
- A world grid cell that overflows drops light from ray-traced shading;
  its rule must keep the strongest lights, never the nearest bright one.
- The desktop and tiled pipelines cull with different passes over one
  table, as ADR-087 allows.
