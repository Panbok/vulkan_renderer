---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Render Flow and State Updates Design Specification

## Overview

This document defines the per-frame render flow and the separation of global vs local state updates. Global state (e.g., view/projection) must be updated and bound once per frame, while local state (per-object model matrix, material uniforms, and textures) is updated per draw. This eliminates catastrophic redundancy and aligns with the current Vulkan descriptor layout.

Related: [Shader/Local State](./shader_state_and_local_state_design.md), [Material System](./material_system_design.md), [Pipeline Registry](./pipeline_registry_and_multi_pipeline_design.md).

## Architecture

```
Per Frame:
  begin_frame
    ├─ update_global_state()    // once
    └─ for each renderable:
         ├─ material_apply()    // fills local UBO + sets texture slot 0
         ├─ update_local_state()
         └─ draw_geometry()
  end_frame
```

## API

```c
// Update global uniforms (e.g., view, projection) once per frame
bool8_t vkr_renderer_update_global_state(RendererFrontend *renderer,
                                         PipelineHandle pipeline,
                                         const GlobalUniformObject *ubo);

// Update per-object local state (model matrix and material-driven uniforms)
bool8_t vkr_renderer_update_local_state(RendererFrontend *renderer,
                                        PipelineHandle pipeline,
                                        const Mat4 *model);
```

Backend policy:
- Global state binding/writes occur only when global data changes or once per frame.
- Local state writes are per draw; descriptor updates occur when material/texture generation changes.

## Design Decisions

1) Split global and local updates
- Rationale: Avoid repeated writes/binds of identical global data.
- Implementation: Two explicit frontend calls; backend caches bound global state.

2) Material-driven local uniforms
- Rationale: Materials own texture + scalar/vector parameters; local UBO maps 1:1 with `MaterialUniforms` (v1: diffuse_color).
- Implementation: `vkr_material_apply` writes local UBO data and sets texture slot 0.

3) Stable ordering guarantees
- Rationale: Avoid undefined behavior across frames.
- Implementation: Documented order: bind pipeline → update/bind global → for each renderable: apply material → update local → draw.

4) Idempotent backend state updates
- Rationale: Prevent unnecessary driver work.
- Implementation: Track currently bound pipeline/descriptors; skip redundant binds.

## Usage Examples

```c
vkr_renderer_begin_frame(renderer, delta);

// Global once per frame
vkr_renderer_update_global_state(renderer, pipeline, &globals);

// Per object
for (uint32_t i = 0; i < renderable_count; ++i) {
    VkrMaterial *m = vkr_material_system_get_by_handle(&app->material_system, r[i].material);
    vkr_material_apply(m);
    vkr_renderer_update_local_state(renderer, pipeline, &r[i].model);
    vkr_geometry_system_render(renderer, r[i].geometry);
}

vkr_renderer_end_frame(renderer, delta);
```

## Performance Considerations

- Updating global UBO once per frame removes O(N) redundant writes.
- Caching bound pipeline/descriptors can remove most redundant binds.
- Sorting by material reduces descriptor churn.

## Testing

- `test_global_updates_once` – verify only one global UBO write/bind per frame.
- `test_local_updates_per_object` – local data changes reflected per draw.
- `test_ordering_enforced` – pipeline → global → local → draw.

## Revision History

- Version 1.0 (2025-10-11): Initial separation of global/local updates and render flow definition.


