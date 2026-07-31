---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Material System Design Specification

## Overview

This document specifies the first version of the material system used by the renderer. Materials encapsulate shader-facing parameters and texture bindings and are referenced via stable handles (id + generation). The system integrates with the texture system and the render flow, providing a minimal but extensible foundation that maps cleanly to the current Vulkan descriptor layout (1 local UBO + 1 sampled image + 1 sampler).

Related: [Render Flow & State Updates](./render_flow_and_state_updates_design.md), [Shader/Local State](./shader_state_and_local_state_design.md), [Pipeline Registry](./pipeline_registry_and_multi_pipeline_design.md), [Resource System & Loaders](./resource_system_and_loaders_design.md).

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                 VkrMaterialSystem                        │
│  - Array<VkrMaterial>                                    │
│  - name → handle map (hashtable)                         │
│  - default material (index 0)                            │
│  - refcounts, generations                                │
└───────────────┬──────────────────────────────────────────┘
                │ uses
                ▼
┌──────────────────────────────────────────────────────────┐
│                    VkrTextureSystem                      │
│  - Acquire base_color textures                           │
│  - Handle validation, upload, lifetime                   │
└──────────────────────────────────────────────────────────┘

Material (v1):
  - handle {id,generation}
  - base_color: VkrTextureHandle (slot 0)
  - color_factor: Vec4 (maps to LocalUniformObject)
  - pipeline_family: VkrPipelineDomain (e.g., WORLD, UI)
```

## API

Note: Function/type names follow existing naming conventions. Exact header placement may vary; the design shape is normative.

```c
// Handle
typedef struct VkrMaterialHandle { uint32_t id; uint32_t generation; } VkrMaterialHandle;

// Material
typedef struct VkrMaterial {
    VkrMaterialHandle handle;
    Vec4 color_factor;               // maps to LocalUniformObject.diffuse_color
    VkrTextureHandle base_color;     // bound to texture slot 0 in current layout
    VkrPipelineDomain pipeline;      // world | ui (used by pipeline selection)
    uint32_t ref_count;
    uint32_t generation;             // for validation + descriptor updates
} VkrMaterial;

// System
typedef struct VkrMaterialSystem {
    // arrays/maps/arenas as per project conventions
} VkrMaterialSystem;

// Lifecycle
bool8_t vkr_material_system_initialize(VkrMaterialSystem *system);
void    vkr_material_system_shutdown(VkrMaterialSystem *system);

// Acquire/Release by name
VkrMaterialHandle vkr_material_system_acquire(VkrMaterialSystem *system, String8 name);
void              vkr_material_system_release(VkrMaterialSystem *system, VkrMaterialHandle handle);

// Lookup
VkrMaterial *vkr_material_system_get_by_handle(VkrMaterialSystem *system, VkrMaterialHandle handle);

// Setters (bump generation on change)
bool8_t vkr_material_set_base_color(VkrMaterialSystem *system, VkrMaterialHandle handle, VkrTextureHandle texture);
bool8_t vkr_material_set_color_factor(VkrMaterialSystem *system, VkrMaterialHandle handle, Vec4 color_factor);

// Bridge to render flow
// Copies color_factor to LocalUniformObject and sets texture slot 0 before local-state update
bool8_t vkr_material_apply(const VkrMaterial *material);
```

### `.mt` Material File Format (v1)

Simple key=value, 1 texture slot supported (base_color) and RGBA color factor:

```
pipeline=world|ui
base_color=assets/albedo.png
color_factor=1,1,1,1
```

## Design Decisions

1) Single-texture v1 aligning with current descriptor layout
- Rationale: Set 1 binds 1 sampled image + 1 sampler; keeps complexity low.
- Implementation: `base_color` bound to slot 0; future extensions add slots or arrays.

2) Handle-based identity with generation
- Rationale: Matches geometry/texture systems; prevents use-after-free; triggers descriptor updates on change.
- Implementation: `VkrMaterial.handle` is stable; each setter bumps `generation`.

3) Default material at index 0
- Rationale: Always-available fallback; simplifies error paths.
- Implementation: Created at init (white color, default texture if available).

4) Pipeline family on material
- Rationale: Directs pipeline/passes (e.g., WORLD vs UI) without per-call branching.
- Implementation: Stored on `VkrMaterial`; used by pipeline/pipeline registry selection.

5) Loader integration via Resource System
- Rationale: Unified acquisition path; dedupe by name; clear lifetime.
- Implementation: `.mt` files parsed by material loader; textures acquired via texture system.

## Usage Examples

```c
// Acquire material from .mt file
VkrMaterialHandle mat = vkr_material_system_acquire(&app->material_system, string_lit("assets/default.mt"));

// Build a renderable
Renderable r = {
    .geometry = geo_handle,
    .material = mat,
    .model = model_matrix,
};

// Per frame
vkr_renderer_begin_frame(renderer, delta);
vkr_renderer_update_global_state(renderer, pipeline, &globals);
for (uint32_t i = 0; i < renderable_count; ++i) {
    VkrMaterial *m = vkr_material_system_get_by_handle(&app->material_system, r[i].material);
    vkr_material_apply(m);                         // writes local uniforms + sets texture slot 0
    vkr_renderer_update_local_state(renderer, pipeline, &r[i].model);
    vkr_geometry_system_render(...);
}
vkr_renderer_end_frame(renderer, delta);
```

## Performance Considerations

- Keep LocalUniformObject aligned (e.g., 256B) and write once per object.
- Cache resolved pointers (`VkrMaterial*`, `VkrTexture*`) at scene setup; update on generation change.
- Batch by material to minimize descriptor writes and texture binds.
- Free CPU-side texture pixels after upload; materials store only handles.

## Testing

- `test_material_default_exists` – default material present at init.
- `test_material_parse_mt` – parse `.mt`, resolve texture via texture loader, create entry.
- `test_material_generation_bump` – setters bump generation and trigger descriptor refresh.
- `test_material_apply_local_uniforms` – color factor written to local UBO.

## Revision History

- Version 1.0 (2025-10-11): Initial specification aligned to current descriptor layout and loader framework.


