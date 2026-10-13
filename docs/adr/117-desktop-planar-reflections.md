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
tiled pipeline (Metal) shades a reflector's draws with its baked plane-bound
probe, the first step the owner chose for it on 2026-10-13 (decision 6). Its
real-time near field is not built
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
6. **Baked plane probe (tiled).** The component's `probe` names one of the
   scene's reflection probes by 1-based index (zero is none), baked at the
   plane by `vkr_bakery bake probe` with its box set to the surrounding
   walls and `box_projection` on. Each frame
   `vkr_standard_scene_runtime_probe_owners` gives that probe the enabled,
   visible reflector's draw `object_id` as `VkrFrameIblProbe.owner_object_id`,
   and `vkr_standard_scene_runtime_order_probes` ranks it ahead of the
   distance order, so the probe cap never drops it (a cap of zero, as in a
   probe capture, still keeps none). The tiled encode kernel
   (`vkr_metal_packet_draw_probe`) gives a draw the probe its `object_id`
   owns before the first-probe-past-one-half rule, and that rule and the
   per-pixel glass choice skip owned probes, so the plane probe shades its
   reflector only. In a reflection-probe capture (`static_capture`) the
   runtime marks every planar reflector's draws with the camera's hidden bit
   through `VkrSceneDrawGate.hidden_render_ids`, so a plane's probe never
   sees the plane. The desktop pipeline ignores `owner_object_id` and blends
   the plane probe like any other.

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

On the tiled pipeline the plane probe shows the static scene from its
capture point, corrected by its box: parallax grows for content far from
the box walls, and dynamic objects and the player do not reflect. Content
wholly below the plane is not filtered from the capture; for a viewer above
the plane, reflected rays point upward and rarely reach it.

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

Metal Release, M1 Pro, 2026-10-13, local and non-authoritative: the
fixture gains a plane probe (`assets/textures/bistro_water_probe.vkt`, box
x −40.2 to −28.3, y 0 to 12, z −13.5 to 3.3 m, fitted from depth captures
around the plane) baked with
`vkr_bakery bake probe --scene assets/scenes/fixtures/bistro_planar_water_local.scene.json --output assets/textures/bistro_water_probe.vkt --position -34 0.5 -5 --size 256`,
which `--check` reports current. Its downward face shows the cobbles, not
the water. With a temporary perfect-mirror water material,
`planar_water_bistro_street_tiled_capture` showed the sky before the
binding and the surrounding façade, windows, door and lit wall lamp after
it; with the greybox water the change is a paler, flatter reflection under
Fresnel, and depth stays byte-identical. Metal API validation of the case
reported no message (`sha256:d1245c68…`), and `build_test.sh` passed
(809 tests). Not run: the Vulkan build of the shared changes, and the
large-level proposal's error against a real-time reference image, which
only the desktop pipeline draws.

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
