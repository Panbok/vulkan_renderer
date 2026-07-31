---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Shader State and Local State Design Specification

## Overview

This document specifies the per-object local state model and the cleanup of the historical ShaderStateObject. The goal is to expose a lean per-draw interface (model matrix) while hiding local UBO slice management and descriptor set selection behind a handle-based local state allocator.

Related: [Render Flow](./render_flow_and_state_updates_design.md), [Material System](./material_system_design.md), [Pipeline Registry](./pipeline_registry_and_multi_pipeline_design.md).

## Architecture

```
┌────────────────────────────────────────────────────────┐
│           RendererLocalStateAllocator                  │
│  - fixed/growable capacity of local UBO slices         │
│  - descriptor sets per object                          │
│  - handle → index mapping                              │
└───────────────┬────────────────────────────────────────┘
                │
                ▼
         Per-object draw call
            ├─ set model matrix via push constants
            ├─ material_apply() fills local UBO slice
            └─ bind descriptor set for this handle
```

## API

```c
typedef struct RendererLocalStateHandle { uint32_t id; uint32_t generation; } RendererLocalStateHandle;

// Acquire/release per-object local state slot
bool8_t renderer_local_state_acquire(RendererFrontend *renderer, PipelineHandle pipeline,
                                     RendererLocalStateHandle *out_handle);
void    renderer_local_state_release(RendererFrontend *renderer, PipelineHandle pipeline,
                                     RendererLocalStateHandle handle);

// Query uniform buffer offset for the handle (for debugging/introspection)
uint64_t renderer_local_state_offset(RendererFrontend *renderer, PipelineHandle pipeline,
                                     RendererLocalStateHandle handle);

// Minimal per-draw input (replaces large ShaderStateObject)
typedef struct PerDrawState {
    Mat4 model; // required
} PerDrawState;
```

Notes:
- Local uniforms are authored via the material system (`vkr_material_apply`).
- Textures are set by materials; the local state handle selects the correct descriptor set and UBO slice.

## Design Decisions

1) Hide indices behind `RendererLocalStateHandle`
- Rationale: Prevent accidental misuse; decouple app from descriptor set counts.
- Implementation: Handle → slot index mapping with generation validation.

2) Fixed capacity with future growth
- Rationale: Simplicity and deterministic memory; resize later if needed.
- Implementation: Allocate local UBO and descriptor sets up-front; consider grow-on-demand via reallocation.

3) Minimal per-draw structure
- Rationale: Avoid redundant or backend-specific fields.
- Implementation: `PerDrawState { Mat4 model }`; material data comes from material system.

4) 256B alignment of local UBO slices
- Rationale: Matches Vulkan uniform buffer alignment requirements.
- Implementation: Compute offsets by `slice_index * aligned_size`.

## Usage Examples

```c
// Scene setup: acquire a persistent local state handle per renderable (optional)
RendererLocalStateHandle lsh;
renderer_local_state_acquire(renderer, pipeline, &lsh);

// Each frame
PerDrawState draw = { .model = model_matrix };
VkrMaterial *m = vkr_material_system_get_by_handle(&app->material_system, renderable.material);
vkr_material_apply(m);                       // fills local UBO slice
vkr_renderer_update_local_state(renderer, pipeline, &draw.model);
vkr_geometry_system_render(renderer, renderable.geometry);

// On destruction
renderer_local_state_release(renderer, pipeline, lsh);
```

## Performance Considerations

- Reuse local state handles across frames to avoid descriptor set thrash.
- Batch draws by local state adjacency to improve cache locality.
- Grow-on-demand should preserve offsets when resizing (preserve-on-resize strategy).

## Testing

- `test_local_state_acquire_release` – obtain and free multiple handles without leaks.
- `test_local_state_generation_guard` – invalid handle rejected.
- `test_local_state_offsets` – offsets computed with correct alignment.

## Revision History

- Version 1.0 (2025-10-11): Initial specification for local state handle API and minimal per-draw input.


