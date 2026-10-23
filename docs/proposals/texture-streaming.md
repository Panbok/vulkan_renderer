---
status: proposed
updated: 2026-10-23
authority: proposal
---
# Texture streaming

Keep only the texture mips the camera needs on the GPU, inside a texture
memory budget, on both pipeline classes
([ADR-087](../adr/087-gpu-class-graphics-pipelines.md)). This proposal owns
which mips are resident and when. The [network protocol](network-protocol.md)
owns fetching bytes from a remote depot and their transport order, and
[world partition](world-partition.md) owns which cells and assets load.

## Current baseline

- **Format.** Every `.vkt` is a KTX2 container of host-native blocks
  ([ADR-012](../adr/012-texture-compression-pipeline.md)). libktx writes the
  level data smallest mip first, with a byte offset and length for each level
  in the level index
  ([writer2.c](../../vendor/ktx-software/lib/writer2.c)). Bundles store
  texture chunks raw, not as zstd frames (`vkr_bundle_compress` in
  [vkr_bakery_bundle.c](../../tools/bakery/vkr_bakery_bundle.c)), so each
  level is a byte range of the file.
- **Load.** A load whose limit (`max_extent=N`) drops levels reads only the
  file prefix that holds the levels it keeps, up to the end of the largest
  (`vkr_texture_decode_ktx2_ranged` in
  [vkr_texture_system.c](../../runtime/src/renderer/systems/vkr_texture_system.c),
  ADR-012); a load that keeps every level reads the whole file. Either way
  the smallest mip remains (`vkr_texture_limit_skip`). The limit is one value for all
  material textures: the tighter of the Graphics `texture_resolution` setting
  and the scene's `texture_max_extent` (ADR-012). A changed limit reloads the
  affected textures through the material stream queue, at most 32 queued or
  loading, shrinks first, then drawn textures furthest below their measured
  need (ADR-012). Only a material's first load of a
  file larger than 1 MiB still arriving from a remote content directory
  reads less: the levels in its first 256 KiB, as a preview that this
  reload replaces once a frame draws it
  ([ADR-123](../adr/123-asset-streaming-from-a-depot.md)).
- **Streams.** The material system holds up to 4,096 texture streams with 8
  in flight
  ([vkr_material_system.h](../../runtime/src/renderer/systems/vkr_material_system.h)).
  A material publishes neutral defaults, and each ready texture replaces its
  default in the material row.
- **Demand.** Each frame, every loaded and visible world source touches its
  material (`vkr_material_system_touch_texture_residency` in
  [vkr_scene_frame.c](../../runtime/src/renderer/systems/vkr_scene_frame.c)).
  That epoch means "published to the world", not "seen by the camera": a
  texture behind the camera has the same epoch as one in front of it. The
  desktop G-buffer resolve also measures the extent each material's pixels
  need ([ADR-024](../adr/024-shared-bindless-gpu-cores.md)); the tiled
  pipeline measures nothing.
- **Eviction.** Under a residency budget the material system first lowers
  textures' top mips by halving a per-slot extent cap, oldest demand epoch
  first and largest first within an epoch, down to a 128 floor, through the
  load limit's reloads; it evicts whole shared textures, which show their
  neutral defaults, only when every texture is at the floor
  (`vkr_material_system_evict_to_fit` in
  [vkr_material_system.c](../../runtime/src/renderer/systems/vkr_material_system.c)).
  Caps grow back when the budget allows. A demanded eviction requeues after a
  real budget increase or a Scene memory-relief generation
  ([ADR-024](../adr/024-shared-bindless-gpu-cores.md)). On Vulkan the
  G-buffer resolve measures each material's required extent and the budget
  lowers mips beyond it first; Metal orders by demand epoch and size. On
  Vulkan a step's reload copies the levels the replaced texture holds on the
  GPU and reads and uploads only the levels it adds, none for a shrink
  (ADR-012); Metal reloads every kept level.
- **Budget owner.** The automatic policy derives the texture allowance from
  the heaps' usage and budget: pressure starts at 90% of the budget, ends below
  75% and targets 80%; under Metal's cap the allowance never drops below the
  capacity already charged to asset textures
  (`vkr_render_assets_texture_pressure_budget` in
  [vkr_render_assets.c](../../runtime/src/renderer/systems/vkr_render_assets.c)).
  Metal's budget is the managed allocation cap; Vulkan's is the device-local
  heaps' `VK_EXT_memory_budget` budget, with asset textures in blocks of their
  own, and Vulkan evicts whole textures to reach the target (ADR-024).
- **Publication.** Vulkan writes each image into a slot of the sampled-image
  descriptor buffer, and material rows hold the 32-bit slot
  (`VkrVulkanMaterialGpuRow`). Metal material rows hold a 64-bit resource ID.
  Slot reuse waits for the last submission that used it (ADR-024). Neither
  backend uses sparse residency or a min-LOD clamp; the Vulkan device enables
  neither `sparseResidencyImage2D` nor `shaderResourceMinLod`.
- **Sampling.** The desktop G-buffer resolve samples material textures with
  explicit gradients (`SampleGrad` in
  [deferred.slang](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)).
  The tiled pipeline samples them in fragment shaders.
- **Measured size.** Bistro's textures take 3.176 GB at full resolution and
  1.995 GB at the 2048 limit (ADR-012, ADR-083).

## Reference: idTech

- Doom (2016) used MegaTexture virtual texturing: 16k×8k virtual textures of
  128² tiles, streamed at frame start from a 160×120 feedback buffer that the
  forward pass wrote
  ([Courrèges](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/)).
- Doom Eternal removed MegaTexture
  ([Coenen](https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy));
  the sources do not describe what replaced it.
- DOOM: The Dark Ages streams its levels by sector, over levels 4 to 10
  times larger than Eternal's
  ([Khan and Stack, GPC 2025](https://static.graphicsprogrammingconference.com/public/2025/talks/a-beautiful-hell-path-tracing-in-doom-the-dark-ages/Khan-Stack-a-beautiful-hell-path-tracing-in-doom-the-dark-ages.pdf)).

id moved from page-based virtual texturing back to whole textures. This
proposal stays with whole textures and streams their mips.

## Options

| Option | Mechanism | Portability | Memory |
|---|---|---|---|
| A. Per-texture mip residency (recommended) | Each texture has a resident top mip that the streamer raises or lowers; a change reallocates the image and republishes its slot | Both backends, no new device feature | Saves memory; one texture doubles briefly while it grows |
| B. Sparse textures | Allocate the whole chain and commit or release tiles with feedback | Vulkan sparse residency is optional; Metal sparse textures need sparse heaps | Saves memory at tile size; sparse binding work on a queue |
| C. Virtual texturing | Page table, physical cache, feedback pass, an indirection in every material sample | Both backends, every material shader changes | Bounded cache; a per-sample cost |

Option A reuses the existing stream queue, reload path and slot publication.
Option B adds an optional device feature to every texture. Option C changes
every material sample for a workflow id itself abandoned.

## Proposed work

1. **Tiled demand.** The desktop G-buffer resolve measures each material's
   required texture extent and the budget lowers surplus mips first
   (ADR-024). Report the same `texture_demand_kernel.slangh` value from the
   tiled pipeline's fragment shaders, which have the derivatives, so Metal
   orders lowering by need instead of draw epoch and size. Passes outside the
   camera view (shadows, probe captures, planar reflections, bakes) still
   read what the camera's demand keeps resident.
2. **Trim without pressure.** Lower textures to their measured need with no
   budget pressure, so only the mips the camera needs stay resident
   (deferred by the owner on 2026-10-22 because each step republishes the
   texture). On Vulkan a step now copies the kept levels and reads only
   added ones (item 3), but still publishes a new texture.
3. **Grow and shrink on Metal.** Vulkan reloads copy the levels the replaced
   texture holds and read and upload only the added ones (ADR-012, ADR-024).
   Metal reports no `supports_texture_level_copy`; its adapter would copy
   the levels with a blit encoder, retain the source until that command
   buffer completes, and then report the capability.
4. **Floor first.** Load each new texture first at its 128-texel floor, so
   it appears at once at low detail. Done as a second full load it took
   Bistro's full detail from 27 to 35 s on the RX 6700 XT, and the owner
   deferred it on 2026-10-22 (ADR-012). The second step now uploads only the
   levels above the floor, but each texture still publishes twice, which
   that cost came from; it needs a measurement before it returns.
5. **One priority for local and remote loads.** Done for ordering: local
   reloads and remote whole-file downloads follow one order from the
   measured demand (ADR-012). A remote texture still downloads whole once
   drawn; fetching only the mips its need holds would trim without pressure
   (item 2), which the owner kept deferred on 2026-10-23. The tiled pipeline
   orders by epoch until item 1 lands. Not yet measured over a depot.

**Lifetime.** The texture system owns images, levels and slots; the material
system owns streams and demand; each backend adapter owns native images, copy
commands and retirement. A level's file bytes go to the existing upload
staging and return to it after the copy's GPU completion, not after a frame
count. An old image's last GPU use is the last submission that sampled its
slot. Demand aggregation runs once per frame over the feedback buffer; per-draw
loops gain no allocation, string, lock or wait.

## Owner decisions (2026-10-20)

The owner accepted each recommendation below on 2026-10-20.

- **Demand source.** GPU feedback or a CPU estimate from candidate bounds,
  distance and a cooked UV density per submesh. Decision: GPU feedback,
  because it sees occlusion and the shading passes already compute the
  derivatives; its per-frame cost needs a measurement.
- **Resident floor.** Decision: keep levels at or below 128² resident.
  For BC7 at one byte per texel that is about 21.4 KiB per texture, or about
  85 MiB for all 4,096 streams (computed, not measured).
- **Growth path.** Reallocate and copy, or allocate the full chain and only
  upload. Decision: reallocate, because saving memory is the goal.
- **Existing limits.** Decision: keep `texture_resolution` and the
  scene's `texture_max_extent` as caps on the top mip; streaming works below
  them.

## Evidence needed

- Bistro only. A texture memory report at a fixed camera path: resident bytes
  by texture and level, the budget, and the gap between resident and required
  mips, on the M1 Pro (tiled) and the RX 6700 XT (desktop).
- A hitch report while flying through Bistro on a recorded camera path at a
  fixed speed: frame time p95, p99 and maximum, and upload stalls, against
  today's load. First reading (2026-10-23, RX 6700 XT, Release,
  `bistro_vulkan_high_orbit_1440`, `performance-windowed-gpu.json`, 1,500
  frames, browser open, not authoritative): with the driver's budget the
  wall frame took 13.15 ms at p50, 14.49 ms at p95 and 22.0 ms at most; at
  `VKR_VULKAN_MEMORY_BUDGET_MB=10500`, 1.97 GB of Bistro's 3.16 GB resident,
  9 lowering steps and 4 level copies landing in the measured frames, it
  took 13.12, 14.45 and 23.7 ms, with up to 71 texture slots (41 on average)
  below their measured need. Open defect (2026-10-23): `bistro_vulkan_high_orbit_1440`
  at `VKR_VULKAN_MEMORY_BUDGET_MB=6000`, where the texture allowance falls
  to 0 and every texture keeps loading and being evicted, lost the device
  in every run with and without level copies, after 23,733 and 82,549
  submissions; an RGD dump placed the page fault in
  `Transmission.Compact.Fullscreen.0` at an address no live resource holds.
  The driver's budget passed. A later run with that pass's `visible_rows`
  and `materials` reads bounds-checked logged no out-of-range index and
  reached the texture-ready timeout without a device loss, as 2 of the 8
  runs at that budget did, so neither index is cleared or blamed. The cause
  is not found.
- G-buffer albedo captures along that path after streaming settles, against
  full residency, and a count of frames that sample below the required mip.
- A budget at half of Bistro's full-resolution texture bytes, with no
  material showing the neutral default after its first load.
- Matched, capture-free Release before and after reports for any speed claim.

## Risks

- Mips arrive one to two frames after feedback, so fast turns and
  disocclusion show blurred textures briefly.
- Growing and shrinking images doubles one texture's memory for a moment and
  republishes its slot; demand needs hysteresis to stop oscillation.
- Texture loads compete for disk and upload bandwidth with world partition
  cell loads and the 32-texture reload queue.
- Passes that sample outside the camera view read the floor unless they
  report demand.
