---
status: investigation
updated: 2026-07-31
authority: investigation
---

> **Archived.** Superseded by [`../rendering/cascading-shadow-mapping-design.md`](../rendering/cascading-shadow-mapping-design.md). Retained for history; do not treat as current.
# CSM Straight Cutoff / Shadow Pop-In Investigation

## Status: RESOLVED

This document captures the investigation and resolution of cascaded shadow map (CSM) artifacts in the Sponza scene.

## Original Symptoms

- Shadows **pop in/out** when rotating or moving the camera.
- Large areas become **fully unshadowed** or **fully shadowed** depending on view angle.
- A very noticeable **perfectly straight cutoff line** appears on the floor, aligned with cascade transitions.
- Pillars sometimes stop casting shadows until moving closer or changing angle.

## Root Cause Analysis

The investigation revealed **two separate but related issues**:

### Issue 1: Cascade Selection vs Coverage Mismatch

**Symptom**: Straight cutoff lines at cascade boundaries, magenta in debug mode 8.

**Cause**: The shader selected cascades based on view-space depth (`select_cascade`), but the selected cascade's light-space projection didn't always cover the fragment. This happened because:
- Cascade projections were fitted tightly to camera frustum slices
- Texel stabilization snapping could shift the projection slightly
- The UV out-of-range check was too strict (1.5 texels epsilon)

### Issue 2: Shadow Pop-In When Camera Moves (THE FUNDAMENTAL ISSUE)

**Symptom**: Shadows appear/disappear dramatically when moving the camera, even without rotating.

**Cause**: Each cascade's light-space projection was computed **only from camera frustum corners**. This meant:
- The shadow map's depth range only covered geometry visible to the camera
- Shadow casters **outside the camera frustum** (but whose shadows should fall inside) were not included
- When the camera moved, different casters entered/exited the shadow maps, causing shadow pop-in

This is the classic CSM pitfall: fitting cascades purely to the camera frustum optimizes resolution but fails to capture all shadow casters.

## The Fix (Multi-Part)

### Fix 1: Cascade Fallback in Shader

**Files**: `assets/shaders/default.world.slang`

When the initially selected cascade doesn't cover a fragment, the shader now searches all cascades to find one that does:

```hlsl
CascadeLookup find_covering_cascade(float3 world_pos, uint initial_cascade)
{
    // Check all cascades and pick the one with best coverage quality
    for (uint i = 0u; i < 4u; ++i) {
        ShadowProjection p = project_shadow(world_pos, i);
        if (!p.uv_out && !p.z_out) {
            // Track cascade with best coverage_quality (UV most toward center)
            ...
        }
    }
}
```

This eliminates the seam lines at cascade boundaries.

### Fix 2: More Lenient UV Bounds

**Files**: `assets/shaders/default.world.slang`

Changed the UV out-of-range check from a tiny epsilon (1.5 texels) to a 10% margin:

```hlsl
float shadow_uv_margin() {
    return 0.1f;  // Allow up to 10% out-of-range
}
```

This prevents thin "seam" lines where no cascade would claim coverage.

### Fix 3: Scene AABB for Z Bounds (THE KEY FIX)

**Files**: `lib/src/renderer/systems/vkr_shadow_system.h`, `vkr_shadow_system.c`

Inspired by reference implementation analysis, we now extend the light-space **Z bounds** (depth range) to include the entire **scene bounding box**, not just the camera frustum:

```c
// New type for scene bounds
typedef struct VkrShadowSceneBounds {
  Vec3 min;
  Vec3 max;
  bool8_t use_scene_bounds;
} VkrShadowSceneBounds;
```

In `vkr_shadow_compute_cascade_matrix`:

```c
// Extend Z bounds using scene AABB
if (scene_bounds && scene_bounds->use_scene_bounds) {
    for (int i = 0; i < 8; ++i) {
        Vec3 corner = /* scene AABB corner */;
        Vec4 corner_ls = mat4_mul_vec4(light_view, vec3_to_vec4(corner, 1.0f));
        // Only extend Z, not XY (XY is fitted to frustum for resolution)
        min_z = vkr_min_f32(min_z, corner_ls.z);
        max_z = vkr_max_f32(max_z, corner_ls.z);
    }
}
```

**Key insight from reference implementation**:
- **XY bounds**: Fitted to camera frustum (optimizes shadow map resolution)
- **Z bounds**: Extended to scene AABB (ensures all shadow casters are included)

This is the fundamental fix that eliminates shadow pop-in when the camera moves.

## Configuration

Default scene bounds in `VKR_SHADOW_CONFIG_DEFAULT`:

```c
.scene_bounds = {
    .min = {-100.0f, -100.0f, -100.0f},
    .max = {100.0f, 100.0f, 100.0f},
    .use_scene_bounds = true_v,
}
```

Adjust these bounds to match your scene's actual extent for optimal results.

## Debug Views Reference

Mode legend (press `E` to cycle):

| Mode | Name | Description |
|------|------|-------------|
| 0 | off | Normal rendering |
| 1 | cascades | Color-code by cascade index |
| 2 | factor | Shadow factor (black=shadow, white=lit) |
| 3 | depth | Receiver depth vs shadow map depth |
| 4-7 | map0-3 | Raw shadow map visualization per cascade |
| 8 | frustum | Coverage diagnostics (magenta = fallback used) |
| 9 | camera | Camera clip-space + split contours |

## Files Modified

### Shadow System (CPU)
- `lib/src/renderer/systems/vkr_shadow_system.h`
  - Added `VkrShadowSceneBounds` type
  - Added `scene_bounds` to `VkrShadowConfig`
- `lib/src/renderer/systems/vkr_shadow_system.c`
  - `vkr_shadow_compute_cascade_matrix`: Extend Z bounds using scene AABB

### Shader (GPU)
- `assets/shaders/default.world.slang`
  - `shadow_uv_margin()`: Increased UV tolerance
  - `ShadowProjection`: Added `coverage_quality` field
  - `find_covering_cascade()`: Search all cascades for best coverage
  - `calculate_shadow_ex()`: Return effective cascade used

## Lessons Learned

1. **CSM cascade fitting must consider the scene, not just the camera**: Pure frustum-based fitting causes shadow pop-in because shadow casters outside the view aren't captured.

2. **XY and Z bounds have different requirements**: XY should be tight to the frustum for resolution; Z should encompass all potential shadow casters.

3. **Shader fallback is a good safety net**: Even with proper CPU-side fitting, having the shader search for covering cascades handles edge cases gracefully.

4. **Debug visualization is essential**: Mode 8 (frustum coverage) was crucial for diagnosing the cascade selection vs coverage mismatch.

## Reference Implementation

Analysis of a working CSM implementation (`vkmerc` project) revealed the key pattern:

```cpp
// From CascadedShadowmap.hpp
// fit z bounding to scene AABB
for (uint32_t i = 0; i < 2; i++) {
    for (uint32_t j = 0; j < 2; j++) {
        for (uint32_t k = 0; k < 2; k++) {
            glm::vec3 vert = glm::vec3(lightSpaceTransform * sceneAABB * glm::vec4(i, j, k, 1));
            boundingA.z = std::min(vert.z, boundingA.z);
            boundingB.z = std::max(vert.z, boundingB.z);
        }
    }
}
```

This pattern of extending only the Z bounds to the scene AABB while keeping XY fitted to the frustum was the key insight that resolved the shadow pop-in issue.

## Related Docs

- `docs/rendering/cascading-shadow-mapping-design.md`
- `docs/rendering/csm-implementation-analysis.md`
