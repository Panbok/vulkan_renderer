---
status: proposed
updated: 2026-10-22
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
- **Load.** The texture system reads the whole file and decodes every level
  (`file_read_all`, then `ktxTexture2_CreateFromMemory` with
  `KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT` in
  [vkr_texture_system.c](../../runtime/src/renderer/systems/vkr_texture_system.c)).
  A load limit (`max_extent=N`) then drops the mips above the limit and keeps
  the smallest (`vkr_texture_limit_skip`). The limit is one value for all
  material textures: the tighter of the Graphics `texture_resolution` setting
  and the scene's `texture_max_extent` (ADR-012). A changed limit reloads the
  affected textures through the material stream queue, at most 32 queued or
  loading, recently drawn materials first. Only a material's first load of a
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
  Demand means "published to the world", not "seen by the camera": a texture
  behind the camera has the same demand epoch as one in front of it.
- **Eviction.** Under a residency budget the material system first lowers
  textures' top mips by halving a per-slot extent cap, oldest demand epoch
  first and largest first within an epoch, down to a 128 floor, through the
  load limit's reloads; it evicts whole shared textures, which show their
  neutral defaults, only when every texture is at the floor
  (`vkr_material_system_evict_to_fit` in
  [vkr_material_system.c](../../runtime/src/renderer/systems/vkr_material_system.c)).
  Caps grow back when the budget allows. A demanded eviction requeues after a
  real budget increase or a Scene memory-relief generation
  ([ADR-024](../adr/024-shared-bindless-gpu-cores.md)). The order follows
  demand epochs, not the mip a view needs, and each step reloads the whole
  file.
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

1. **Demand by footprint.** Replace world demand with a required top mip per
   texture. Take it from GPU feedback: the desktop resolve already computes
   gradients for every pixel, and the tiled fragment shader has derivatives.
   One atomic minimum per wave and texture writes into a small buffer that
   the CPU reads one or two frames later. Passes outside the camera view
   (shadows, probe captures, planar reflections, bakes) add their own demand
   or read the resident floor.
2. **Eviction by required mip.** Order the existing mip-level eviction
   (ADR-024) by how far each texture's resident mip exceeds its required mip
   from item 1, instead of demand epoch and size.
3. **Ranged loads.** Read only the levels needed, from the KTX2 level index,
   with `file_seek` and `file_read_into` or `file_map_readonly`
   ([filesystem.h](../../lib/src/filesystem/filesystem.h)). Load the floor
   first, so a new texture appears at once at low detail.
4. **Grow and shrink.** The backend adapter creates the new image, copies
   the kept levels on the GPU and uploads the new level. It publishes the new
   slot or resource ID after the copy completes and retires the old image
   after its last submission (ADR-024). Material rows change through the
   existing publication; per-draw work does not change.
5. **One priority for local and remote loads.** The network protocol's
   progressive phase requests remote mips smallest first by streaming source
   and screen size. It consumes the required mips from item 1 instead of a
   second estimate.

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
  today's load.
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
