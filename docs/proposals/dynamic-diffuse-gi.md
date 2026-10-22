---
status: proposed
updated: 2026-10-22
authority: proposal
---

# Dynamic diffuse GI

The desktop pipeline (Vulkan) lights indirect diffuse from a bake and adds a
ray-traced layer for the light the bake lacks. This proposal makes the bake
optional there: on a GPU with ray queries, a scene with no lightmap set and
no baked volume gets final-quality diffuse light, not blockout light. It
builds on [ADR-119](../adr/119-dynamic-diffuse-volume-layer.md) and depends
on [Sparse diffuse volumes](sparse-diffuse-volumes.md) item 2 (the all-light
layer for time of day, a grid finer than 1024 bricks, the editor writing
`runtime_grid`), which it does not repeat. The tiled pipeline stays baked
([ADR-087](../adr/087-gpu-class-graphics-pipelines.md)); a Mac uses Metal
ray tracing only to bake.

## Current baseline

- **The bake is the authority.** Lightmap sets
  ([ADR-088](../adr/088-baked-lightmap-sets.md)) and the sparse brick volume
  ([ADR-054](../adr/054-baked-diffuse-volumes.md)) hold static diffuse light
  per sun key and lamp group. `DiffuseVolume.Compose` weighs the layers;
  `DiffuseVolume.Sample`, `DiffuseVolume.Upsample` and
  `DiffuseVolume.Fallback` look the volume up per 2 × 2 block with an exact
  fallback, and transmission shading looks it up inline
  ([`diffuse_volume_kernel.slangh`](../../renderer/src/shaders/shared/diffuse_volume_kernel.slangh),
  shared with the tiled pipeline). A Bistro volume bake takes minutes on an
  RX 6700 XT and 677 s on the M1 Pro (Sparse diffuse volumes, Baseline).
- **The dynamic layer** (ADR-119). With ray queries, `Volume.Build` builds
  the frame slot's BLASes and TLAS and `Volume.Trace` traces 128 rays per
  probe into a signed L1 layer that blends in at `VKR_DYNAMIC_VOLUME_BLEND`
  (0.25). It traces only the sources the bake lacks: dynamic lights,
  emissive movers and changed static lights. GPU timestamps set the next
  frame's probe count between 64 and 2048 within 1.0 ms
  (`vkr_renderer_dynamic_volume_probe_budget`); on the café case at
  2560 × 1440 the trace took 0.811 ms p50 and the build 0.045 ms.
- **The runtime grid** is the unbaked mode. `"runtime_grid": true` lays one
  uniform grid of at most 1024 bricks (`DYNAMIC_VOLUME_GRID_BRICKS_MAX` in
  [`vkr_dynamic_volume.c`](../../runtime/src/renderer/systems/vkr_dynamic_volume.c)),
  spacing at least 0.5 m, over the static candidates' bounds. Its layer
  traces the sun, the environment on a miss, every light and every emitter,
  and updates moments and relocation by the bake's rules. On Bistro the
  spacing is 4.88 m, which ADR-119 rates as blockout quality. A scene whose
  lamp lightmaps hold its static lamps gets no grid.
- **Reflection probes** are baked cubes. `vk_volume_probe_relight` scales a
  probe by (baked + layer light) ÷ baked light at its capture origin, and
  leaves the factor at 1 when the capture record holds no baked light
  ([`volume_trace.slang`](../../renderer/src/shaders/vulkan/slang/world/volume_trace.slang)).
  In an unbaked scene probes therefore keep their capture brightness.
- **Without ray queries** (`vkr_renderer_dynamic_volume_supported` false, or
  `VKR_RAY_QUERY_DISABLED` set) the layer and grid are off. A baked scene
  keeps its bake; an unbaked scene falls back to the environment SH and the
  probes.
- **Owner rules.** Screen-space GI was removed on 2026-10-12
  ([ADR-060](../adr/060-screen-space-diffuse-indirect-lighting.md)), and
  effects must resolve from the current frame plus at most one previous
  frame ([Short-history rendering](short-history-rendering.md), owner
  decisions). The trace has no async compute queue
  ([`ARCHITECTURE.md`](../ARCHITECTURE.md), "Graph and native execution").

## Reference: idTech 8

DOOM: The Dark Ages removed baking ([Sousa, SIGGRAPH 2025](https://www.advances.realtimerendering.com/s2025/content/SOUSA_SIGGRAPH_2025_Final.pdf)).
A Doom Eternal path-traced lightmap bake took 4 to 10 h per map and about
500 MB per level, and Dark Ages levels are 4 to 10 times larger. Its system:

- a world-space light grid of 16³ cells × 8 cascades, built on async compute;
- cascaded irradiance volumes of 16³ probes × 6 around the camera, plus
  hand-placed local 10³ volumes; each probe traces 64, 32 or 16 visibility
  rays by platform, and one cascade and one local volume update per frame;
- a spatially hashed world radiance cache: each ray hit marks a 25 cm cell
  (coarser with distance), about 20k cells are shaded per frame and reused
  for several frames, 14 MB; the previous frame's volumes give more bounces;
- an octahedral probe atlas in RGB9E5 with RG16F visibility, 64 MB;
- a final gather of 1 ray per pixel at reduced resolution, reading a
  screen-space cache, then the world radiance cache, then the volumes; it
  stores 2-band SH per pixel, then a separable bilateral denoise, a bilateral
  upscale and a temporal filter;
- reflection probes re-fit by frame irradiance ÷ probe irradiance;
- a froxel irradiance volume for transparents and particles, and
  directional occlusion for detail the BLASes lack;
- about 1.4 ms on Xbox Series X and 1.7 ms on Series S with async compute,
  on hardware where ray tracing is required.

## Proposed work

1. **Camera cascades.** Replace the scene-bounds runtime grid with nested
   grids around the camera that scroll as it moves, so probe density follows
   the view instead of the level's size. Each cascade keeps the brick
   layout, moments and relocation the bake's rules already define.
2. **A shading cache for ray hits.** Shade each probe ray's hit once into a
   hashed world-space cell and reuse it across probes and frames, so cost
   follows cache entries, not rays × lights. The light list must be
   world-space; [Clustered light and decal binning](clustered-light-and-decal-binning.md)
   decides whether that grid is shared.
3. **Lookup across cascades.** `DiffuseVolume.Sample` and `Fallback` pick
   the finest cascade that holds the receiver and blend at its border. The
   lookup kernel is shared with the tiled pipeline, so the cascade path
   needs a desktop-only entry or must keep one meaning for both
   (`AGENTS.md`).
4. **The bake becomes optional.** With ray queries and no bake, runtime GI
   is the scene's full diffuse path. With a bake, ADR-119's additive layer
   stays. Without ray queries the bake stays required, and the editor warns
   when a scene has none.
5. **Probes without a bake.** Record the irradiance a probe saw at capture,
   from whichever source lit it, and re-fit by the current runtime
   irradiance at its origin, as the baked ratio does today.
6. **Budget.** Keep the adaptive probe budget, extended to cascades and the
   cache, within a GPU share the owner grants from the desktop budget's
   reserve ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md#desktop-budget-lines)).

## Owner decisions (2026-10-20)

The owner accepted each recommendation below on 2026-10-20.

1. **Screen-space cache.** idTech 8's gather first reuses the lit frame
   where a ray stays on screen. That is screen-space GI, which the owner
   removed on 2026-10-12. Decision: world-space caches only (the
   radiance cache and the cascades); accept more rays or coarser detail.
2. **Per-pixel gather and its temporal filter.** A per-pixel ray restores
   contact bounce between probes, but its noise needs the temporal filter
   the short-history rule forbids. Decision: probe lookup only first,
   with GTAO for small-scale occlusion; add a world-space gather with a
   spatial-only denoise only if Bistro captures show missing bounce.
3. **Bake and runtime together.** Decision: the bake stays the
   authority when present (ADR-119's owner decision), and runtime GI is the
   full path only for unbaked scenes and editing. Revisit after the baked
   against runtime comparison below.
4. **GPU share.** Decision: measure at the current 1.0 ms and at
   2.0 ms, then set the share in the desktop budget.

## Evidence needed

All scene evidence uses Bistro.

- **Image.** The unbaked café and street views against their baked cases
  (`runtime_grid_bistro_cafe_capture`, `sparse_volume_bistro_street_on_mode9`):
  `indirect_diffuse` and `final_color` differences by region, at day and at
  night (`bistro_lamps_baked_volume_night_capture`).
- **Leaks.** A night capture across the café facade with no interior lamp
  bounce on the outer wall away from the openings.
- **Stability.** A still camera with no light change differs from a repeat
  run by no more than two baked runs differ (ADR-119 found 13 against 15
  pixels).
- **Response.** `dynamic_volume_bistro_cafe_capture`'s moving lamp: bounce
  rise by frame, against ADR-119's record.
- **Cost.** Matched, capture-free Release `profile` reports at 2560 × 1440 on
  the RX 6700 XT, before and after, with valid samples and spread, against
  the desktop budget; memory for acceleration structures, cascades and the
  cache.
- **Native Vulkan.** Debug with `VK_LAYER_KHRONOS_validation` and
  synchronization on the café and unbaked cases, one process at a time.
- **No ray queries.** `VKR_RAY_QUERY_DISABLED=1` keeps a baked scene baked
  and renders an unbaked one on the environment path without errors.

## Risks

- Coarse far cascades leak light through walls that fine cells resolve.
- Probes update in turns, so a switched light's bounce lags; the bake hides
  this today and runtime GI would show it everywhere.
- Unbaked scenes would look different on desktop GPUs with and without ray
  queries.
- The trace path has hit driver defects (ADR-119: a miscompiled index
  pointer hung the GPU, and a TLAS read before its build lost the device);
  crash runs can freeze the Windows host, so they need the owner's consent.
- Memory grows: ADR-119 measured `mesh` allocations rising from 201 MB to
  317 MB live on the café case.
