---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Scene Management and Culling Design Specification

## Overview

This document specifies a lightweight scene system that manages renderables, performs visibility determination (frustum culling), and selects levels of detail (LOD). The system produces an ordered list of visible draws for the renderer, enabling batching and minimizing per-frame overhead.

Related: [Render Batching & State Caching](./render_batching_and_state_caching_design.md), [Render Flow](./render_flow_and_state_updates_design.md), [Material System](./material_system_design.md).

## Architecture

```
Scene
  ├─ Renderable storage (handles → objects)
  ├─ Spatial index (grid/octree/BVH; start with uniform grid)
  ├─ Culling pipeline (camera frustum → visible set)
  └─ LOD selection (distance-based thresholds)

Per-frame flow
  1) Update transforms and bounds
  2) Collect candidates from spatial index
  3) Frustum test AABBs (reject/accept)
  4) Pick LOD per visible object
  5) Build sort keys (pipeline, material, layout) → renderer
```

## API

```c
typedef struct VkrRenderable {
    VkrGeometryHandle geometry;
    VkrMaterialHandle material;
    Mat4 model;
    AABB bounds_ws;     // world-space AABB
    uint32_t flags;     // static/dynamic, visibility mask
} VkrRenderable;

typedef struct VkrScene {
    // renderables, spatial structures, temporary arrays for visible set
} VkrScene;

bool8_t vkr_scene_initialize(VkrScene *scene);
void    vkr_scene_shutdown(VkrScene *scene);

uint32_t vkr_scene_add_renderable(VkrScene *scene, const VkrRenderable *renderable);
void     vkr_scene_remove_renderable(VkrScene *scene, uint32_t renderable_id);
void     vkr_scene_update_renderable(VkrScene *scene, uint32_t renderable_id, const VkrRenderable *renderable);

// Culling and submission
typedef struct VkrCamera { Mat4 view; Mat4 proj; Frustum frustum; } VkrCamera;
uint32_t vkr_scene_collect_visible(VkrScene *scene, const VkrCamera *camera,
                                   VkrRenderable **out_visible_begin, VkrRenderable **out_visible_end);
```

Bounding types:
- Bounds authored as local-space AABB; transformed to world-space each frame for dynamic objects or cached for static ones.
- Frustum planes derived from `proj * view` matrix.

## Design Decisions

1) Uniform grid as initial spatial index
- Rationale: Simple to implement; good for evenly distributed small scenes.
- Implementation: Hash grid buckets by position; store renderable ids per cell.

2) AABB frustum testing with early-out
- Rationale: Fast broad-phase rejection; conservative acceptance.
- Implementation: Plane-AABB test; reject if outside any plane.

3) LOD selection based on camera distance
- Rationale: Reduce vertex/texture cost at distance.
- Implementation: Per-renderable thresholds; choose geometry/material variant accordingly.

4) Static vs dynamic classification
- Rationale: Avoid updating bounds/indices for static objects each frame.
- Implementation: Static objects pre-inserted; dynamic objects updated per-frame.

5) Output sorted list for batching
- Rationale: Enable render-time state reduction.
- Implementation: Build sort keys and order visible list before submission.

## Usage Examples

```c
// Per frame
camera.frustum = frustum_from_viewproj(mul_mat4(camera.proj, camera.view));
VkrRenderable *vis_begin = 0, *vis_end = 0;
vkr_scene_collect_visible(&scene, &camera, &vis_begin, &vis_end);

// Build sort keys and submit to renderer
build_and_sort_keys(vis_begin, vis_end);
for (VkrRenderable *r = vis_begin; r != vis_end; ++r) {
    VkrMaterial *m = vkr_material_system_get_by_handle(&app->material_system, r->material);
    vkr_material_apply(m);
    vkr_renderer_update_local_state(renderer, pipeline_for(m), &r->model);
    vkr_geometry_system_render(renderer, r->geometry);
}
```

## Performance Considerations

- Use SoA layouts for bounds and transforms to improve SIMD opportunities.
- Bucket culling work by grid cell; early-reject cells outside frustum.
- Limit per-frame dynamic updates by classifying objects.
- Consider hierarchical BVH/octree for large scenes.

## Testing

- `test_frustum_aabb_basic` – in/out/partial overlap cases.
- `test_spatial_index_grid_bucket` – renderables map to expected cells.
- `test_lod_thresholds` – correct LOD picked across distances.
- `test_static_dynamic_update_cost` – dynamic updates limited to moving objects.

## Revision History

- Version 1.0 (2025-10-11): Initial specification for scene storage, culling, and LOD.


