---
status: partial
updated: 2026-10-16
authority: adr
---

# ADR-114: Per-object draw distance

## Status

Accepted (partial). Both classify kernels cull by draw distance, screen size
and shadow distance, and the editor and the agent channel author it. The
dithered fade band of the [large-level rendering](../proposals/large-level-rendering.md)
proposal is not implemented. The tiled pipeline has native Metal evidence.
The desktop pipeline has compiled SPIR-V only; native Vulkan execution is not
recorded.

## Context

Nothing limited how far away an object drew. The 20 km far plane of
[ADR-109](109-reversed-camera-depth.md) put every scatter copy and terrain
of a large level in the camera frustum, and the HZB
([ADR-028](028-gpu-driven-deferred-visibility-buffer.md)) rejects nothing
while the camera moves. Every candidate was encoded and drawn in every view
it reached, even below a pixel. A CPU cull before candidate publication
would change the static candidate set as the camera moves and redraw
retained cascades ([ADR-041](041-retained-cascaded-shadows.md)) and cached
local faces ([ADR-019](019-bounded-forward-spatial-lighting.md)).

## Decision

1. **Authoring.** A `draw_distance` component (`SceneDrawDistance`,
   [vkr_scene_system.h](../../runtime/src/renderer/systems/vkr_scene_system.h))
   holds `max_distance` (metres, 0 no limit), `min_screen_pixels` (the
   smallest drawn diameter, 0 to 7.5, 0 none), `shadow_distance` (metres, 0
   every cascade) and `cast_shadows`. It applies to its entity and to
   descendants without their own; the nearest wins
   (`vkr_scene_draw_cull`). Without one, a draw takes its kind's defaults:
   objects (meshes, brushes, shapes) 1 px; scatter and spline-mesh copies
   2 px and no shadow distance; terrains no limit. The owner chose the 1 px
   object default on 2026-10-13 although it changes existing output (below).
   Until 2026-10-16 copies took a 50 m shadow distance, which the High
   preset's last cascade (starting about 54 m out) failed, so trees past it
   drew without a sun shadow and every copy's shadow ended there; the cut
   also moved with the camera's near clip, which sets the split. Copies
   now carry the copy bit instead (decision 4). A draw distance also
   bounds the shadow distance: `vkr_gpu_draw_cull_word` packs the smaller
   of the two codes, so a caster the camera stops drawing leaves the
   cascades that start past that distance.
2. **Resolution.** The render sync resolves the values when an entity syncs
   and stores a cull word on its `VkrMesh` or `VkrMeshInstance`
   ([vkr_resources.h](../../runtime/src/renderer/resources/vkr_resources.h));
   population applies its rule entity's word to every copy. A change to the
   component resyncs every draw. A change to what an object casts (its
   shadow distance or `cast_shadows`) is a topology change within its
   bounds, so retained shadows there redraw; camera-only changes need none.
3. **Candidate data.** The word rides in the candidate row's `state_flags`
   above the bucket and the four candidate flags, from bit 7
   (`vkr_gpu_draw_state_flags`,
   [vkr_gpu_abi.h](../../renderer/src/vkr_gpu_abi.h)): an 8-bit draw
   distance code, a 4-bit smallest diameter in half pixels and an 8-bit
   shadow distance code, then the hidden and sealed bits of
   [ADR-115](115-rooms-and-portals.md) and
   [ADR-116](116-scatter-proxy-cells.md) and the copy bit (bit 22,
   `VKR_GPU_DRAW_CULL_COPY_BIT`), which `vkr_scene_draw_cull` sets on every
   scatter and spline copy, under an authored component too. A code c > 0 stands for
   2^((c - 1) / 16) metres, 1 m to about 60 km in 4.4% steps, rounded up so
   a candidate never culls before its authored distance. The candidate row
   keeps 48 bytes and no binding is added. Encoding keeps only the low seven
   bits in the visible row (`VKR_GPU_DRAW_CANDIDATE_STATE_MASK`). Both
   backends keep static candidate rows resident across frames
   ([vkr_candidate_residency.h](../../renderer/src/vkr_candidate_residency.h)),
   so the payload carries `static_cull_generation`, a hash of the static
   candidates' cull words; a change repacks the resident rows without a
   static content change, which retained shadows would read.
4. **Views.** `VkrGpuLodView` keeps 48 bytes; two former reserved words
   carry `distance_scale` and `shadow_near`
   (`vkr_packet_write_lod_views`). The shared kernels in
   [lod_kernel.slangh](../../renderer/src/shaders/shared/lod_kernel.slangh)
   test, after the frustum:
   - the camera (`vkr_gpu_draw_distance_culled`): the nearest point of the
     bounding sphere past the draw distance times `distance_scale`, or a
     projected diameter times `distance_scale` below the smallest one,
     using the LOD view's pixels per metre;
   - each cascade, static and dynamic (`vkr_gpu_draw_shadow_culled`): the
     shadow distance shorter than the cascade's slice start, which reads no
     camera position, so a cascade's casters change only with the
     preset's splits; and (`vkr_gpu_cascade_caster_too_small`) a bounding
     sphere narrower than one texel of the cascade, or than
     `VKR_GPU_CASCADE_COPY_MIN_TEXELS` (4) for a copy, which bounds the
     cost of shadows from many small copies without a distance: grass and
     pebbles leave the far cascades and trees keep theirs. The texel size
     is the cascade's own, so this test reads no camera position either;
   - local faces and transmission views: nothing, since their
     `shadow_near` is zero.
   The camera view drives both opaque and transmission streams on the
   desktop pipeline. The CPU blend list tests the draw distance only
   (`vkr_scene_blend_visible`).
5. **Scale.** The `view_distance` graphics setting (near, medium, far, epic:
   0.5, 0.75, 1, 1.5; the presets Low to Epic) gives `distance_scale`. A
   reflection-probe capture, the editor Scene's "All" switch and the
   `VKR_DRAW_DISTANCE=0` diagnostic set it to zero, which draws every
   object at any distance and size. The scale never reaches the shadow
   test, so changing it leaves retained shadows valid.
6. **Editor and agents.** Details edits the component. The Scene toolbar's
   draw distance chip shows Game or All (Play always draws the game's
   distances). One selected object shows where the camera stops drawing it as
   a ground ring. `draw_distance.set` sets or removes the component on
   entities chosen by name, kind, tags, region or root as one journal group,
   and the Cmd command `draw_distance` and value `view.draw_distance` switch
   and read the chip ([ADR-075](075-editor-cmd-bar-and-evaluator.md)).
   `view.cull_report` has the runtime sort the next frame's camera
   candidates by the camera's tests on the CPU (`vkr_scene_cull_report`,
   through `vkr_gpu_draw_distance_reason`, the host mirror the kernels'
   test is split from) and answers with the counts per reason. It runs only
   on request, over the payload the frame built.
7. **Draw reach view.** Render mode `DRAW_REACH` (19), an artist data view
   of [ADR-099](099-artist-views-and-luminance-queries.md) on the tiled
   pipeline, colours each object by how many times farther it could be
   before the camera stops drawing it: magenta past a limit (shown only
   when the chip draws All), red at 1x through yellow at 4x to green at
   16x or more, blue without a limit, and grey for blended surfaces, which
   the GPU does not classify. The shared `vkr_gpu_draw_reach_code`
   ([lod_kernel.slangh](../../renderer/src/shaders/shared/lod_kernel.slangh))
   takes the tighter of the draw distance and the smallest size under the
   view's distance scale, or 1 when the chip draws All, as a 5-bit code;
   in this mode the camera's encode writes it into the visible row's probe
   bits, which a data view does not light with, and the inspection variant
   colours it (`vkr_editor_reach_color`).

## Consequences

- The camera drops sub-pixel draws by default. On
  `tools/cases/local/tiled_bistro_baked_capture.case.json` (Metal,
  `local-offscreen`, 2026-10-13) the final color moved by at most 7/255 on
  0.23% of the pixels against `VKR_DRAW_DISTANCE=0`, and depth matched. Strict
  Bistro snapshot gates fail until their baselines are accepted again.
- A culled object writes no depth, so it occludes nothing in the next
  frame's HZB; large occluders keep drawing under the screen-size default.
- On the large-level fixture's hill view
  (`large_level_bistro_metal_overview.case.json`, `local-offscreen-gpu-single`,
  one non-authoritative run each with another process building) the camera's
  visible rows fell from 8,481 to 5,224 and `Tiled.Opaque` from 5.42 to
  5.02 ms; on the glide from 6,818 to 4,490 and from 5.09 to 4.79 ms. These
  are indicative, not a timing claim.
- Matched Release timing on the glide (2026-10-13; M1 Pro, Metal 4, tiled
  pipeline, 1280x720 hidden window with immediate presentation;
  `large_level_bistro_metal_glide_windowed.case.json`,
  `performance-windowed-gpu.json`, five children each, authoritative with
  matching fingerprints, clean tree at `eb299c26`), against
  `VKR_DRAW_DISTANCE=0` in the same binary: `frame.wall` 14.901 to 14.834 ms
  (child means' standard deviation 0.022 and 0.034 ms), `Tiled.Opaque`
  9.865 to 9.745 ms (0.018 and 0.008 ms), the camera's visible rows a mean
  of 6,313 to 4,247 and `cpu.world_payload_build` 1.14 to 0.99 ms. The
  output differs by design, by the sub-pixel draws dropped. Reports
  `sha256:55be311f92c131cfa1511529718a521a22fd4d06ef9b11c1e0009df39a82fcd9`
  (on) and
  `sha256:a1c3fe26c7bec9ebbe7d2db359df7116df4fdc25d1c944cef9a290639e6ba5c6`
  (off). The fixture's distant draws are small, so the gain is too.
- The Draw reach view on the fixture (editor, Metal, 2026-10-13) showed the
  terrains blue, the near forest green and the far trees yellowing toward
  their size limit; with the chip on All, the far rocks past their limit
  showed magenta, and inside the café the walls showed green and the glasses
  grey. The same captures under Metal API validation (`MTL_DEBUG_LAYER=1`,
  one process) reported no error.
- No per-reason GPU counter exists; the compaction state keeps its 144
  bytes. `view.cull_report` gives the counts from the CPU mirror. On the
  fixture's hill view in the editor (2026-10-13) it sorted 15,701 camera
  candidates into 6,727 outside the view, 1,865 hidden, 1,768 below their
  size and 5,341 drawn, against 5,224 visible rows after occlusion in the
  harness's hill view; with the Scene's chip on All, 414 hidden (the
  merged proxy meshes) and 8,560 drawn, against 8,481.
- Since 2026-10-16 copies cast into every cascade above their size limit,
  so the last cascade draws far trees it skipped before. That change has
  no capture or timing yet: its cost on the tiled cascade redraw and the
  pixels it changes are unmeasured.

## Alternatives considered

- **CPU cull before publication.** Rejected: it changes the static candidate
  set with the camera and removes camera-culled objects' shadows.
- **A side array per candidate.** Rejected for the draw distance: the free
  bits of `state_flags` hold the 20-bit word without a binding or memory.
- **Metre defaults per kind.** Rejected: a screen-size default keeps a large
  cliff and drops a small crate at the same range.
- **Scaling the shadow distance with View distance.** Rejected: a setting
  change would leave retained cascades with the old caster set.

## Revisit when

A capture shows pops above the smallest size (then add the fade band:
alpha-tested bucket remap inside the band), or native Vulkan runs of the
fixture's twins disagree with the Metal evidence.
