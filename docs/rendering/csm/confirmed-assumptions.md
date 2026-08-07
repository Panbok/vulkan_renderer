---
status: investigation
updated: 2026-07-31
authority: investigation
---
# CSM Confirmed Assumptions

This document records assumptions that are confirmed by the current code. It is
intended to make later CSM reviews and refactors explicit about what is already
true in the implementation.

## Scope
- CSM CPU pipeline: `lib/src/renderer/systems/vkr_shadow_system.c`
- Shadow view orchestration: `lib/src/renderer/passes/vkr_pass_shadow.c`
- Camera math and matrices: `lib/src/renderer/systems/vkr_camera.c`,
  `lib/src/renderer/systems/vkr_camera_controller.c`, `lib/src/math/mat.h`
- Shader usage context: `lib/src/renderer/vulkan/shaders/world/default.slang`

## Confirmed Assumptions
- The camera basis used for frustum corner generation is orthonormalized when
  yaw/pitch are updated. `forward`, `right`, and `up` are recomputed from
  `world_up` and normalized each time the camera rotates.
  - Source: `lib/src/renderer/systems/vkr_camera.c:34`

- Camera pitch is clamped to ±89°, avoiding the forward≈world_up singularity
  that would destabilize `right` and `up`.
  - Source: `lib/src/renderer/systems/vkr_camera.h:21`
  - Source: `lib/src/renderer/systems/vkr_camera.c:155`

- The view matrix is right-handed and points the camera down -Z. The projection
  matrices follow Vulkan conventions (Z in [0,1], Y inverted).
  - Source: `lib/src/math/mat.h:363`
  - Source: `lib/src/math/mat.h:403`
  - Source: `lib/src/math/mat.h:445`

- The shader’s view-depth usage (`view_depth = -view_pos.z`) matches the
  right-handed view convention and Vulkan clip-space setup in the math library.
  - Source: `lib/src/renderer/vulkan/shaders/world/default.slang:780`
  - Source: `lib/src/math/mat.h:445`
  - Source: `lib/src/math/mat.h:403`

- All cascades share a single light-view matrix; per-cascade variation comes
  from the orthographic projection fitted to that cascade slice.
  - Source: `lib/src/renderer/systems/vkr_shadow_system.c:135`
  - Source: `lib/src/renderer/systems/vkr_shadow_system.c:201`

- The light-view basis is constructed from the directional light using the same
  reference-up heuristic the shader uses (switching to Z-up when light is near
  vertical). This means CPU and shader basis generation are consistent as long
  as the light direction is consistent.
  - Source: `lib/src/renderer/systems/vkr_shadow_system.c:135`
  - Source: `lib/src/renderer/vulkan/shaders/world/default.slang:374`

- The shadow system update reads the active camera and directional light once
  per frame; all cascades for that frame are computed from those values.
  - Source: `lib/src/renderer/passes/vkr_pass_shadow.c:507`
  - Source: `lib/src/renderer/passes/vkr_pass_shadow.c:513`

- Cascade splits are computed from the camera near/far with optional max shadow
  distance clamping and a lambda blend of log/linear splits.
  - Source: `lib/src/renderer/systems/vkr_shadow_system.c:40`
