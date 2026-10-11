---
status: partial
updated: 2026-10-11
authority: adr
---

# ADR-109: Reversed camera depth

## Status

Accepted (partial). Every camera depth buffer on both backends holds
reversed depth: the near plane at 1, the far plane at 0, cleared to 0 and
tested with greater-than. Shadow maps keep forward depth. The Scene and app
camera's default far plane is 20 km. Metal ran the tiled pipeline on Bistro
before and after the change; Vulkan compiles only, and no native Vulkan run
exists.

## Context

Camera projections used forward `[0,1]` depth (`mat4_perspective`, near 0.1
m, far 500 m). Float32 depth near 1 resolves about 6 mm at 100 m and about
0.6 m at 1 km, so faces a few millimetres apart z-fight in a large outdoor
level: 4 mm inset trim plates under a bridge deck flickered from 50 m
([ADR-084](084-agent-channel-and-level-design-toolkit.md) sizes its
`z_fight` lint gap from that case). The 500 m far plane also cut distant
terrain and partition proxies ([level toolkit
audit](../proposals/level-toolkit-audit.md) A7,
[ADR-086](086-world-partition.md)).

With reversed depth, device depth is about `near / distance`. Float32 keeps
its relative precision down to 0, so a 4 mm separation stays distinct at a
kilometre (the CPU suite `mat` checks this at near 0.1 m and far 20 km), and
a far plane kilometres away costs no precision.

## Decision

1. **Projections.** Cameras use `mat4_perspective_reversed` and
   `mat4_ortho_zo_yinv_reversed` ([mat.h](../../lib/src/math/mat.h)): the
   same X and Y mapping as the forward builders, depth 1 at the near plane
   and 0 at the far plane. The far plane stays finite: CPU code reads near
   and far back from the matrix (`mat4_perspective_reversed_near`,
   `mat4_perspective_reversed_far`), frustum planes need a nonzero far
   normal, and shaders place the sky at device depth 0. `vkr_camera.c` and
   the animation preview (`vkr_scene_animation.c`) build the camera
   projections.
2. **Shadow maps keep forward depth.** Cascades, local shadow faces and
   their transmission layers keep `mat4_ortho_zo_yinv` and
   `mat4_perspective`, clears of 1, less-than tests, the less-or-equal
   comparison samplers and positive raster depth bias. Their depth range is
   fitted to casters, so forward depth loses no useful precision there, and
   keeping them avoids changing every receiver kernel, bias and the shadow
   tests for no visible gain. Camera and shadow states are therefore split
   by pass: Vulkan picks the compare op from the pipeline
   (`vkr_vk_packet_pipeline_shadow_depth` in
   [vkr_vulkan_pipelines.c](../../renderer/src/vulkan/vkr_vulkan_pipelines.c)),
   Metal keeps `depth_write_state` (Greater) for cameras and
   `shadow_depth_write_state` (Less) for shadow passes
   ([vkr_metal_packet_record.inc](../../renderer/src/metal/internal/vkr_metal_packet_record.inc)).
3. **Formats, clears and tests.** Camera depth stays `D32_SFLOAT` /
   `Depth32Float` (it already was). The render graphs clear
   `opaque_vbuffer_depth`, `picking_depth` and `animation_preview_depth` to
   0. Camera depth writes test greater-than; depth-read blend and text,
   tiled shading and the tiled sky test greater-or-equal. The tiled MSAA
   depth resolve takes the maximum sample, the nearest surface.
4. **Shaders.** Every shader that reads camera depth uses the reversed
   meaning:
   - Sky tests become `depth <= 0` (tiled atmosphere, clouds on both
     backends, fog, froxel fog, depth of field) and the editor grid's surface
     test `depth > 0`. The tiled sky triangle sits at depth 0.
   - The HZB keeps each cell's farthest depth, now the minimum, and the
     occlusion test takes the nearest corner as the maximum, rejecting only
     when it is below the occluder minus the unchanged tolerance. Reversed
     and forward depth have the same slope magnitude, so absolute device
     tolerances (HZB 1e-3, transmission peel 1e-5, TAA 0.005) keep their
     distance meaning; only their signs follow the comparison.
   - Rays unproject the near plane at depth 1 (editor grid, cloud view
     direction, froxel fog in the shader and in its CPU light bounds, the
     runtime pick ray, label sight, the editor's orthographic ray).
   - Depth of field, motion blur and surface diffusion linearize with
     `n f / (n + d (f - n))`. GTAO and the SSR/SSGI history linearize from
     projection elements and need no change.
   - Transmission peeling keeps fragments with smaller depth; FSR's
     reactive pass takes the nearer of opaque and transmission depth as the
     maximum, and the FSR 3.1 context sets
     `FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED`.
   - Vulkan sky pixels reproject through the far plane at depth 0 and store
     previous depth 0 in the temporal validity image;
     `vkr_temporal_sky_reprojection` intersects orthographic rays with
     `q.z == 0`.
5. **CPU readers.** Occupied-depth feedback
   ([ADR-033](033-occupied-depth-sdsm-feedback.md)) stamps
   `VKR_SHADOW_DEPTH_SOURCE_REVERSED_Z` and takes the nearest occupied
   surface from the maximum device depth. The editor's grid fit reads a
   picked depth of 0 as sky. `vkr_frustum_from_matrix` is unchanged: it
   bounds `0 <= z <= w` under either convention; only its near and far
   labels swap for cameras, and no caller reads them.
6. **Far plane.** `VKR_SAMPLE_CAMERA_NEAR_METRES` (0.1) and
   `VKR_SAMPLE_CAMERA_FAR_METRES` (20,000) in
   [vkr_sample_runtime.h](../../runtime/src/vkr_sample_runtime.h) set the
   app and Scene camera. 20 km lies past the horizon of an 8 km world seen
   across its diagonal. A recalled view written with the old automatic 500 m
   plane reads at least the default, and `view.camera far` still sets up to
   100 km. Orthographic views frame their target at most 250 m from the eye
   (`sample_orthographic_distance`), as the 500 m plane did, so the
   atmosphere's observer and the grid's depth tolerance do not move 10 km
   with the far plane. Harness cases keep the near and far planes they name.

## Consequences

- Depth captures (`depth` channel) hold reversed device depth: sky reads 0
  and previews show near surfaces bright. A depth baseline from before this
  change no longer compares.
- Picking's readback depth is reversed; the Vulkan picking resolve and the
  Metal readback copy it unchanged.
- Shadow and camera depth now follow different conventions. Code that
  unprojects a shadow view must keep forward depth; code that unprojects a
  camera must use reversed depth.
- An infinite far plane is not supported: the frustum, the near/far readers
  and the far-plane sky reprojection would need another path.
- Raising the far plane draws more of a large world; culling, LOD and the
  shadow distance (`max_shadow_distance`) bound that work, and Bistro lies
  within 500 m, so its draw counts do not change.
- The ADR-084 `z_fight` gap (1 cm) was sized for forward depth and could
  shrink.

## Alternatives considered

- **Infinite reversed projection.** Removes the far plane entirely, but
  device depth 0 becomes a point at infinity: every unprojection at the far
  plane, the frustum's far plane and the matrix near/far readers would need
  separate handling for a negligible precision gain over a 20 km plane.
- **Reverse shadow maps too.** One convention everywhere, but every shadow
  receiver, blocker search, transmission layer, clear and bias would change
  for no gain on fitted ranges, with a much larger verification surface.
- **Keep forward depth and raise the near plane.** Precision grows with the
  near plane, but a 1 m near plane clips walls the camera brushes past and
  still resolves only centimetres at a kilometre.

## Revisit when

- A world needs views past 20 km: make the far plane follow the proxy
  radius ([world partition](../proposals/world-partition.md)).
- A Windows Vulkan run of the desktop pipeline is available: record the
  native evidence below.

## Evidence

Release, M1 Pro, Metal (tiled pipeline), 2026-10-11, validation variables
unset. "Before" is a build of `b988faf2` in a separate worktree; "after"
adds this change. `local-offscreen` snapshots compare colour per channel in
8-bit sRGB; depth is compared as linear distance after undoing each
convention (near 0.1 m, far 500 m, as the cases name).

| Case | Colour pixels differing by more than 1/255 | Depth |
| --- | --- | --- |
| `tiled_bistro_capture` (static street, MSAA, TAA, GTAO, clouds, fog) | final 297 of 921,600 (0.032 %), scene colour 388; isolated silhouette pixels | sky masks identical (21,721 pixels); linear distance within 6.3e-5 relative, mean 9.2e-6 |
| `bistro_vulkan_gate_a` (unpinned, Metal) | final 146 (0.016 %) | as above; `picking_ids` 31 of 921,600 change, each between two adjacent objects, none to or from the background |
| `tiled_bistro_decals_capture` | final 304 (0.033 %), scene colour 337 | as above |

Two after runs of `tiled_bistro_capture` differ by at most 1/255. Every
`draw.*` and `visibility.*` metric of the three snapshots matches, and over
the 300-frame `tiled_bistro_native` orbit (`local-windowed-gpu-single`) every
one matches the before build, including `visibility.hzb.rejected` (mean 3.67,
up to 220 a frame) and `visibility.gpu_visible.count` (mean 1,784.69). No
timing claim is made: the runs shared the GPU with another session's
editor.

In the headless Release editor on Bistro, `grid.fit` read 10.78 m from a
perspective view in both builds, and 16.84 m from the top view (ADR-087
recorded 16.8432 m). `view.capture` of a grid view, a street view and the
top view, at the old 500 m far plane, differ from the before build only in
the frame-time overlay, isolated silhouette pixels and grid lines lying on
the surface the grid was fitted to.

CPU suites (Release `vulkan_renderer_tester --suite`): 103 of 104 pass;
`run_local_socket_tests` fails at `test_local_socket_stale_and_live` with
the unchanged base binary too. `mat`, `visibility`, `temporal`, `gtao`,
`shadow_system`, `editor_label_sight` and `editor_project_store` check the
reversed builders, the forward and reversed frustum, the far-plane sky
reprojection, reversed occupied-depth feedback and the recalled far plane.

Vulkan: the 13 changed SPIR-V modules pass `spirv-val --target-env
vulkan1.4 --scalar-block-layout`. The FSR 3.1 source is not compiled on
macOS, and no native Vulkan run of any changed pass exists.
