---
status: proposed
updated: 2026-07-31
authority: spec
---
# Stable CSM (Directional) Spec

**Legacy note:** This document references the deprecated view/layer system.
Render orchestration now uses the render graph; view modules are render helpers
invoked by pass executors.

## Status
Draft (Jan 24, 2026)

## Problem
The current CSM implementation computes a per-cascade light view matrix (light
position is derived from the cascade center). This defeats texel-grid snapping
and causes cascade shimmer/instability when the camera moves or rotates. The
current snapping is effectively a no-op because the cascade center is close to
(0,0) in that per-cascade light space.

## Goals
- Make cascades **stable** under camera translation and rotation.
- Preserve current features: guard band, scene AABB depth extension, constant
  cascade size option, and per-cascade resolutions.
- Keep shader sampling and pipeline binding unchanged in Phase 1.
- Keep the implementation local to the shadow system and shadow pass executor.

## Non-Goals
- Shadow map atlas or layered depth textures (descriptor layout changes).
- PCSS / contact hardening / new filtering kernels.
- Multi-directional lights or point/spot shadows.

## Constraints
- Must work with existing `VkrShadowConfig` presets and CPU data upload path.
- Must not change descriptor set layout in Phase 1.
- Must not depend on global samplers (instance-only sampler binding remains).

## High-Level Design
Replace per-cascade light-view construction with a **single frame-level light
view** (rotation aligned to the directional light) and compute per-cascade
orthographic bounds in that fixed light space. Snap cascade bounds to texel
increments derived from the cascade shadow-map size to remove sub-texel drift.

Key changes:
- `light_view` is computed once per frame and reused across cascades.
- Cascade snapping is applied to **bounds** (`minX/minY`), not the center.
- Optional radius quantization to reduce extent “breathing”.

## Data Flow (Per Frame)
1. Compute cascade splits (existing logic).
2. Build a **stable light view** from directional light + a stable anchor.
3. For each cascade:
   - Compute world-space frustum corners (slice).
   - Compute slice center + radius (optional quantization).
   - Transform center/corners into light space (using the frame light view).
   - Derive XY extents (AABB or constant radius extent).
   - Apply guard band in texels.
   - **Snap minX/minY to texel grid** and rebuild maxX/maxY.
   - Compute Z range using scene bounds or extended frustum range.
   - Build ortho projection and `view_projection`.

## Stable Light View
We use a fixed orientation per frame and a stable anchor in world space.

### Light orientation
- `dir = normalize(light_direction)`
- `up_ref = (abs(dir.y) > 0.99) ? (0,0,1) : (0,1,0)`
- `right = normalize(cross(up_ref, dir))`
- `up = cross(dir, right)`

### Light anchor
- If `scene_bounds.use_scene_bounds == true`:
  - `anchor = (scene_bounds.min + scene_bounds.max) * 0.5`
- Else:
  - `anchor = camera.position`

Optional stability improvement (Phase 1 optional, Phase 2 recommended):
- Project `anchor` into light space, snap its XY to a coarse grid (e.g.,
  4–16 texels of cascade 0), then reconstruct world-space anchor. This reduces
  long-range drift from floating origin-style movement.

### Light position
- `light_pos = anchor - dir * light_distance`
- `light_distance` must keep `anchor` and scene bounds within the Z range.
  A safe default is `scene_radius * 2` when scene bounds are enabled.

### Light view matrix
- `light_view = look_at(light_pos, anchor, up)`

This `light_view` is reused for all cascades in the frame.

## Cascade Bounds & Snapping

### XY extents
Option A (current default): AABB of the slice frustum corners in light space.
Option B (constant size): bounding sphere radius in world space, converted to a
square extent in light space.

If `use_constant_cascade_size`:
- `extent = radius * 2`
- `center_ls = light_view * float4(center_ws, 1)`
- `center_x = center_ls.x`, `center_y = center_ls.y`

Otherwise:
- Transform all 8 frustum corners into light space.
- Compute min/max for X/Y.
- `extent = max(max_x - min_x, max_y - min_y)`
- `center_x/y` derived from min/max.

### Guard band
- `extent += 2 * texel_size * guard_band_texels`
- Recompute `texel_size = extent / shadow_map_size`

### Snapping (stable CSM)
Snap the **minimum bound** to the texel grid:
- `min_x = floor((center_x - extent/2) / texel_size) * texel_size`
- `min_y = floor((center_y - extent/2) / texel_size) * texel_size`
- `max_x = min_x + extent`
- `max_y = min_y + extent`

This ensures that camera movement smaller than one texel does not shift the
shadow projection.

### Z extents
If `scene_bounds.use_scene_bounds`:
- Transform all 8 scene AABB corners into light space and extend `min_z/max_z`.

Else:
- Extend Z based on cascade radius:
  - `min_z -= radius * z_extension_factor`
  - `max_z += radius * z_extension_factor`

Add a small safety pad (existing behavior):
- `z_pad = max(0.5, z_range * 0.05)`

## Quantized Radius (Optional, Phase 1)
To reduce “breathing” under camera rotation, quantize the radius:
- `radius = ceil(radius * 16.0f) / 16.0f`

This mirrors the reference example and produces stable extents at the cost of
slightly larger cascades.

## Shader Changes (Phase 1)
None. Shadow sampling and cascades remain identical.

## Shader Changes (Phase 2, Optional)
Add slope-scaled receiver bias for acne reduction at grazing angles.
- New uniform: `shadow_slope_bias` in `GlobalUniformBufferObject`.
- Shader: `bias = base_bias + shadow_slope_bias * tan(acos(ndotl))` (clamped).

## File/Module Changes
- `lib/src/renderer/systems/vkr_shadow_system.c`
  - Add per-frame `light_view` computation.
  - Replace per-cascade `look_at` calls with the shared `light_view`.
  - Implement snapping on min bounds.
  - Optional radius quantization.
- `lib/src/renderer/systems/vkr_shadow_system.h`
  - If Phase 2: add `shadow_slope_bias` to `VkrShadowConfig` + frame data.
- `lib/src/renderer/vulkan/shaders/world/default.slang`
  - Phase 2 only: consume `shadow_slope_bias`.

## Debugging & Validation
Use existing debug modes:
- `shadow_debug_mode = 8`: cascade coverage diagnostics (seams).
- `shadow_debug_mode = 4..7`: per-cascade depth visualization.

Expected improvements:
- Reduced shimmer when rotating the camera.
- Reduced crawl on thin geometry (pillars, railings).
- Stable boundaries when moving laterally.

## Performance Impact
- CPU: negligible (reuse `light_view`, minor extra math).
- GPU: no changes in Phase 1.
- Memory: unchanged.

## Risks
- Incorrect light-space Z range can clip casters if the new light view places
  the scene behind the near plane. Mitigate by conservative `light_distance`
  and Z-range computation from scene bounds.

## Acceptance Criteria
- Moving the camera by less than 1 texel in world space does not move the
  shadow projection for cascade 0 (visible as stable contact shadows).
- Cascade transitions do not shimmer when rotating the camera.
- No new clipping of casters in the shadow pass.

## Test Plan
- Manual:
  - Enable debug mode 8 and sweep camera rotation; observe stable coverage.
  - Enable debug mode 4..7 to verify all cascades contain valid depth.
  - Place thin geometry (poles/rails) and translate camera sideways; confirm
    reduced shimmer.
- Instrumentation:
  - Log `world_units_per_texel` per cascade to verify stability.
