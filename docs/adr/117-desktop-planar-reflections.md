---
status: partial
updated: 2026-10-13
authority: adr
---

# ADR-117: Desktop planar reflections

## Status

Accepted (partial). The desktop pipeline (Vulkan) renders one planar
reflector a frame: its mirrored view, shaded forward with the frame's lights
and shadows, replaces the probes' reflection on the reflector's surface. The
tiled pipeline's baked plane-bound probe, which the owner chose on
2026-10-13 to come first there, is not built
([Large-level rendering](../proposals/large-level-rendering.md), section 3;
[Planar reflections](../proposals/planar-reflections.md)).

## Context

Water reflects through planar reflections on both pipelines (owner decision,
2026-10-12, [Short-history rendering](../proposals/short-history-rendering.md),
section 5): after screen-space reflections were removed
([ADR-055](055-screen-space-reflections.md)), water reflected only the
probes and the global environment, which hold neither what is near the
plane nor any moving object. The owner chose full lighting for the mirrored
view, local lights and their shadows included (2026-10-13).

The visibility buffer, G-buffer resolve and deferred lighting serve one
camera view. A second deferred chain would need its own G-buffer, motion and
validity targets and resolve variants; compute shading cannot reuse the
forward path's implicit-derivative texture sampling. The forward world
shader already lights a surface with every light, the shadow cascades, the
local shadow atlas, the diffuse volume and the probes from any frame root.

## Decision

1. **Authoring.** A `planar_reflector` component
   (`ScenePlanarReflector` in
   [vkr_scene_system.h](../../runtime/src/renderer/systems/vkr_scene_system.h),
   category Rendering) places a mirror plane through its entity's origin,
   facing along the entity's Y axis. It holds `enabled`, `resolution`
   (`half` or `quarter`) and `max_distance` in metres (zero is unlimited).
   Only the entity's own draws reflect, and only where they lie on the
   plane.
2. **Selection.** Each frame, the scene runtime
   (`vkr_standard_scene_runtime_prepare_planar_reflection` in
   [vkr_standard_scene_runtime.c](../../runtime/src/application/vkr_standard_scene_runtime.c))
   takes the enabled, visible reflector with the camera above its plane,
   its bounds in the camera frustum and within its distance, whose bounding
   sphere covers the most of the view (radius² over distance²). It composes
   the camera view with the reflection about the plane and sends the
   mirrored view, the plane, the mirrored eye, the reflector's bounds
   projected to a screen rectangle (the whole screen when a corner lies
   behind the camera), the reflector draws' `object_id` and the divisor as
   `VkrFrameInput.planar_reflection`
   ([vkr_frame_input.h](../../renderer/src/vkr_frame_input.h)), validated at
   submission. The tiled pipeline sends none.
3. **Culling.** The mirrored view is the last GPU culling view, after the
   transmission views (`vkr_render_graph_reflection_view` in
   [vkr_render_graph.h](../../renderer/src/vkr_render_graph.h)). It
   follows the camera's candidate flags, draw distances and LOD rules from
   the mirrored eye, culls against its frustum cropped to the screen
   rectangle, takes no HZB test, and leaves out the reflector's own
   draws and candidates whose bounding sphere lies wholly below the plane
   (`vk_gpu_draw_classify` in
   [deferred.slang](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)).
4. **Raster.** `Reflection.Raster`
   ([main.rendergraph.json](../../assets/render_graphs/main.rendergraph.json),
   condition `planar_reflection_active`) draws the view's indirect buckets
   into `planar_reflection_color` (RGBA16F) and `planar_reflection_depth`,
   both half the viewport; the quarter tier draws their top-left quarter,
   and a scissor limits either to the screen rectangle.
   `vk_reflection_vertex` decodes the visible rows as the visibility raster
   does, and `vk_reflection_fragment` shades through
   `packet_world_shade_surface`, the forward shading body the blend path
   also uses
   ([default.slang](../../renderer/src/shaders/vulkan/slang/world/default.slang)),
   and writes alpha one. Its frame root keeps the camera's view matrix, so
   the sun cascades are chosen by camera depth, and takes the mirrored
   view-projection and eye. Every bucket's front face is flipped. The pass
   reads the cascade map, the local shadow atlas, the transmission layers
   and the cloud shadow.
5. **Lookup.** Deferred lighting
   (`vk_planar_reflection_specular`) gives a pixel the reflection when its
   draw carries the reflector's `object_id`, it lies within 5 cm of the
   plane and its geometric normal faces the plane normal within cos 0.9.
   The mirrored view shares the camera's projection, so the pixel samples
   its own screen position, shifted by 0.05 per unit of the shading
   normal's tilt off the plane in view space. The sample, weighted as the
   probes' radiance is, replaces the environment specular in proportion to
   its alpha; off the image, outside the reflector or over sky, the probes
   and environment remain.

## Consequences

Water and other planar surfaces reflect nearby buildings, lamps and moving
objects with the scene's lighting, at the cost of one forward-shaded,
geometry-bound view. The reflection is sharp: no roughness blur or mip
chain exists yet, so a rough reflector shows a sharp image under its GGX
weight. The mirrored view leaves out what the camera's per-frame state
covers only for itself: baked-lamp lightmaps, contact shadows, GTAO,
aerial perspective, froxel and analytic fog, transmission and blended
surfaces. Its sky is the probes' or environment's, through the empty
pixels. Room gates and draw distances follow the camera's; a room the
camera cannot see stays hidden in the reflection.

## Evidence

Vulkan Release unless stated, RX 6700 XT, 2026-10-13, local and
non-authoritative:

- `planar_water_bistro_street_capture` (`local-offscreen`, 1920 × 1080,
  `bistro_planar_water_local`: a 5 m greybox water plane in the square
  below the street camera). With a temporary perfect mirror material
  (metallic 1, roughness 0) the plane showed the façade, its door, windows
  and lit wall lamps mirrored about the plane, and nothing below it. With
  the greybox water material (roughness 0.2) the lamp and façade reflections
  appear under Fresnel.
- Vulkan validation (Debug, `VK_LAYER_KHRONOS_validation` with
  synchronization, two children): the capture case reported no validation
  message and no error (`sha256:58e4b639…`).
- Timing, `profile` with `local-offscreen-gpu-repeated` at 2560 × 1440,
  5 × 300 measured frames each, warmup stable, p50:
  `planar_water_bistro_street_timing` against
  `planar_water_bistro_street_off_timing`, the same scene with the reflector
  disabled. Drawing the whole mirrored view cost `Reflection.Raster`
  3.30 ms (p95 3.56) and the frame wall 23.89 ms against 20.54 ms;
  limited to the reflector's screen rectangle, `Reflection.Raster` takes
  1.19 ms (p95 1.31), `Lighting.Deferred.Fullscreen` 7.13 ms against
  7.00 ms, and the frame wall 21.75 ms (p95 22.44) against 20.54 ms
  (p95 21.40). The capture with the rectangle differs from the one without
  on 0.003 % of pixels, at the rectangle's edge (largest 33/255). Reports
  `sha256:497c64de…` (off), `sha256:08fb6bb0…` (whole view),
  `sha256:dc443618…` (rectangle).

The first run crashed: the culling-view loop computed a local shadow face
index modulo the frame's face count before choosing the reflection view,
and a frame that drew no face divided by zero.

## Alternatives considered

A second deferred chain for the mirrored view would reuse the visibility
buffer but duplicate the G-buffer, resolve and lighting targets and their
temporal writes. An oblique near plane would clip below the plane in the
projection, but lighting reconstructs depth in places that assume a
standard projection; culling and the wholly-below test bound the work
instead, and the reflector's own draws are left out. Rendering the sky into
the reflection was not needed while the probes and environment hold it.

## Revisit when

Rough or rippled water needs a blurred reflection (a mip chain or a
roughness-weighted filter); several reflectors are visible at once; the
reflection's missing effects (fog, aerial perspective, blended surfaces)
show; or the tiled pipeline's plane probe and near field need a shared
contract.
