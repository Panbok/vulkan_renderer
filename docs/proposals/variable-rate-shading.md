---
status: proposed
updated: 2026-10-22
authority: proposal
---

# Variable-rate shading

An optional setting that shades smooth, dark or low-contrast screen regions
at a coarser rate in the passes that dominate per-pixel cost. The owner
decided on 2026-10-20 that the setting stays optional: it is off by default,
and no preset turns it on until the evidence below exists. Work starts only
after the [desktop frame budget](desktop-frame-budget.md) shows a Bistro frame
limited by per-pixel shading.

## Current baseline

- The desktop pipeline shades opaque surfaces in compute. `GBuffer.Resolve`
  (`vk_gbuffer_resolve`) and `Lighting.Deferred` (`vk_deferred_lighting`) run
  8x8 groups over the whole extent
  ([`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang),
  [`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json)).
  Hardware fragment shading rate cannot reach these passes.
- Lighting splits its tiles between a base kernel, a layered kernel and a
  fallback kernel for local shadow mask overflows. Each kernel dispatches
  every 8x8 group. Each group votes in group-shared memory
  (`vk_deferred_tile_class`), and a kernel that does not own the tile
  returns early. The host sets `layered_tiles`
  ([`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c)).
  No pass builds an indirect list of lighting tiles or pixels.
- Some terms already run at fixed reduced rates: `Shadow.SunContact` and, by
  default, `Shadow.SunRay` once per 2x2 block, and the diffuse volume at half
  resolution with an exact fallback
  ([architecture](../ARCHITECTURE.md#rendering-pipeline)). Indirect pixel lists
  exist for SMAA 4x's second samples (`Lighting.Deferred.Sample1`) and for
  compacted transmission pixels (`Transmission.Shade.Compact.*`).
- The desktop raster passes are cheap per pixel or need exact per-pixel
  output: `VBuffer.Opaque` writes identities and the shadow passes write depth.
  `World.Blend` forward-shades ordinary blended surfaces, and
  `Reflection.Raster` already renders at half or quarter resolution.
- FSR 3.1 native antialiasing is the default, with a 16-phase Halton jitter and
  the SDK's own history. SMAA T2x alternates two jitters and keeps no scene
  colour history ([ADR-052](../adr/052-vulkan-fsr31-upscaling.md),
  [ADR-037](../adr/037-portable-same-resolution-temporal-antialiasing.md)).
- The tiled pipeline shades forward in `Tiled.Opaque`: four samples resolved
  on chip, or one sample at 0.6 of each axis under MetalFX. Its adaptive
  quality lowers the render scale under budget pressure
  ([ADR-039](../adr/039-metal-internal-render-scale.md),
  [ADR-121](../adr/121-tiled-temporal-upscaling.md)).
- No code requests `VK_KHR_fragment_shading_rate` or Metal rasterization rate
  maps. The Vulkan profile enables extra features only where it says they are
  optional, as for sampler anisotropy
  ([ADR-023](../adr/023-vulkan-1-4-bindless-capability-profile.md)).
- `VkrGraphicsSettings` has no shading-rate field
  ([`vkr_graphics_settings.h`](../../runtime/src/vkr_graphics_settings.h)).

## Reference: idTech

Doom Eternal shaded forward and used hardware VRS in its opaque and
transparent passes. DOOM: The Dark Ages moved opaque shading to compute and
lost hardware VRS there, so id wrote variable-rate compute shaders (VRCS):

- A 2x2 shading-rate image from scene luminance, with rates 1x1, 2x1, 1x2
  and 2x2.
- 16x16 screen tiles. Each tile lists its primary pixels, packed into waves
  so that whole waves retire early. A duplicate pixel copies its primary's
  result.
- VRCS covers deferred texturing, the G-buffer update and deferred lighting.
  id reports about 10% of GPU time (1 to 2 ms) at comparable image quality,
  depending on platform and resolution. Hardware VRS stays for transparents
  and Forward+ opaque.
- For 2x1 and 1x2, id flips the checkerboard each frame. A 2x2 block needs
  four frames to converge through the temporal filter, which id found too
  slow.

Sources: [Fuller, Hammer, GPC 2025](https://static.graphicsprogrammingconference.com/public/2025/talks/variable-rate-compute-shaders-in-doom-the-dark-ages/Fuller-Hammer-variable-rate-compute-shaders-in-doom-the-dark-ages.pdf),
[Lazarek, Hammer, GPC 2025](https://static.graphicsprogrammingconference.com/public/2025/talks/visibility-buffer-and-deferred-rendering-in-doom-the-dark-ages/Lazarek-Hammer-visibility-buffer-and-deferred-rendering-in-doom-the-dark-ages.pdf).

## Proposed work

1. **Tile lists first.** Replace the in-kernel layered vote with a
   classification pass that lists each kernel's 8x8 tiles for an indirect
   dispatch. This stops both kernels from launching every group, and VRCS
   needs the same list builder. Measure it alone before adding rates.
2. **Rate image.** One rate per 2x2 block, from the previous frame's
   luminance contrast reprojected by motion. A block takes a coarse rate only
   when its four pixels share one draw identity and none is a layered pixel,
   an SMAA 4x second sample or a pixel with transmission over it.
3. **Primary-pixel lists.** The classification pass also writes each tile's
   primary pixels, packed into waves. `Lighting.Deferred` shades the primaries
   and writes each duplicate from its primary. Lighting comes first because it
   writes one HDR colour; a coarse `GBuffer.Resolve` would copy normals and
   materials across pixels and waits for lighting evidence.
4. **Temporal.** Flip the primary of each 2x1 and 1x2 block every frame, in
   step with T2x's two-phase cycle. Allow 2x2 only if an image comparison in
   motion under FSR 3.1 accepts it.
5. **Hardware VRS.** No desktop raster pass is a first-phase target.
   `World.Blend` is the one forward-shaded candidate for
   `VK_KHR_fragment_shading_rate`, as an optional device feature, if a profile
   shows its cost matters.
6. **Tiled pipeline.** Not in the first phase. Metal rasterization rate maps
   set rates per screen region and need a later pass that maps the result back
   to screen space; their fit with the on-chip MSAA resolve and the MetalFX
   input is unknown. M1 support is a fact to verify on the device, not an
   assumption. Until then the tiled pipeline's rate control is its adaptive
   render scale.
7. **Setting.** An optional `variable_rate_shading` graphics setting, off by
   default and in no preset. The tiled pipeline ignores it, as it ignores
   other desktop-only gates. The `*.Editor` passes stay full rate.

## Owner decisions (2026-10-20)

The owner accepted each recommendation below on 2026-10-20.

- **Rate source.** Luminance contrast as id uses, or also normal and albedo
  variance from the G-buffer. Decision: luminance, with the identity
  guard of item 2; add G-buffer terms only if comparisons show artifacts.
- **Tile size.** 8x8 to match the lighting groups, or id's 16x16.
  Decision: 8x8, so that one classification pass serves both the
  layered split and VRCS.
- **Layered tiles.** Clearcoat, sheen and anisotropy pixels are the most
  expensive and the most specular. Decision: full rate in the first
  phase; revisit with a motion comparison.
- **2x2 rate.** Decision: leave it out of the first phase, as id's
  convergence finding suggests; T2x cannot converge it in two phases.

## Evidence needed

- The [desktop frame budget](desktop-frame-budget.md) report shows
  `Lighting.Deferred` or `GBuffer.Resolve` as the largest per-pixel cost on
  Bistro at the target resolution.
- Matched capture-free Release before/after reports on Windows Vulkan for
  [`bistro_shadow_orbit`](../../tools/cases/performance/bistro_shadow_orbit.case.json)
  with valid samples and spread, compared by `vkr_harness perf-compare`, for
  item 1 alone and then for the rates. Never a Sponza case.
- Image comparisons still and in motion for FSR 3.1, SMAA T2x and SMAA 4x
  against full rate. `final_color` varies between identical runs, so judge the
  difference against the spread of repeated full-rate runs.
- A focused Vulkan synchronization-validation run over the new passes.
- For the tiled pipeline: a device check of rate-map support on M1 before any
  design work.

## Risks

- Coarse blocks shimmer on glossy specular and thin geometry in motion, and
  temporal jitter can make it worse.
- The rate and list passes cost GPU time; a frame that is not limited by
  per-pixel shading can lose time overall. id called VRCS a win but not a
  silver bullet, with corner cases that were hard to catch.
- Copying across a block at a silhouette smears edges unless the identity
  guard holds.
- Snapshots and baselines must run with the setting off, or their final
  colour changes.
