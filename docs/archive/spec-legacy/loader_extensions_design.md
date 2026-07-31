---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Loader Extensions Design Specification

## Overview

This document extends the resource loader framework with concrete loaders for geometry (e.g., .obj or custom binary) and shader modules (.spv). Loaders parse/validate source data into typed descriptions and then finalize them into runtime resources, returning stable handles.

Related: [Resource System & Loaders](./resource_system_and_loaders_design.md), [Pipeline Registry](./pipeline_registry_and_multi_pipeline_design.md).

## Architecture

```
Resource System
  ├─ registry: name → {type, handle, ref_count}
  └─ loaders: vtable per type

Loaders
  geometry_loader: .obj/.vbo → GeometryDesc → GeometryHandle
  shader_loader:   .spv      → ShaderDesc   → ShaderModuleHandle
```

## API

```c
// Geometry loader (shape)
typedef struct VkrGeometryDesc {
    // vertex layout, vertex data, index data, counts, topology
} VkrGeometryDesc;

bool8_t vkr_geometry_loader_can_load(String8 path);
bool8_t vkr_geometry_loader_load(Arena *scratch, String8 path, VkrGeometryDesc *out_desc);
bool8_t vkr_geometry_loader_finalize(void *renderer, const VkrGeometryDesc *desc, VkrGeometryHandle *out_handle);
void    vkr_geometry_loader_unload(VkrGeometryHandle *handle);

// Shader loader (shape)
typedef struct VkrShaderDesc {
    // stage, entry point, SPIR-V bytes
} VkrShaderDesc;

bool8_t vkr_shader_loader_can_load(String8 path);
bool8_t vkr_shader_loader_load(Arena *scratch, String8 path, VkrShaderDesc *out_desc);
bool8_t vkr_shader_loader_finalize(void *renderer, const VkrShaderDesc *desc, ShaderModuleHandle *out_handle);
void    vkr_shader_loader_unload(ShaderModuleHandle *handle);
```

## Design Decisions

1) Typed intermediate descriptions
- Rationale: Separate parsing from GPU creation; enables validation and reuse.
- Implementation: `*_load()` fills `*Desc`; `*_finalize()` creates GPU resources.

2) Format detection by extension with content sniffing fallback
- Rationale: Robust across inconsistent file naming.
- Implementation: `can_load()` checks extension; optional magic header checks.

3) Vertex layout awareness
- Rationale: Geometry must match pipeline vertex inputs.
- Implementation: Fill layout fields from file or defaults; validate against registry pipeline usage.

4) SPIR-V module validation
- Rationale: Catch stage mismatches and entry point issues early.
- Implementation: Ensure stage compatibility and presence of entry point.

5) Caching and deduplication
- Rationale: Avoid repeated parsing or SPIR-V creation.
- Implementation: Registry dedup by name; optional content hash cache.

## Usage Examples

```c
// Register the loaders
vkr_resource_system_register_loader(rsys, VKR_RESOURCE_TYPE_GEOMETRY, geometry_loader_vtable());
vkr_resource_system_register_loader(rsys, VKR_RESOURCE_TYPE_SHADER,   shader_loader_vtable());

// Acquire
VkrGeometryHandle geo; vkr_resource_system_acquire(rsys, string_lit("assets/cube.obj"), VKR_RESOURCE_TYPE_GEOMETRY, &geo);
ShaderModuleHandle vs; vkr_resource_system_acquire(rsys, string_lit("assets/world.vert.spv"), VKR_RESOURCE_TYPE_SHADER, &vs);
```

## Performance Considerations

- Parse on worker threads; finalize on render thread to centralize Vulkan access.
- Prefer binary mesh formats for faster loads; keep .obj as dev-time convenience.
- Cache SPIR-V module creation by path and timestamp.

## Testing

- `test_geometry_loader_obj_positions_uvs` – parse vertex attributes.
- `test_geometry_loader_finalize_handle` – creates valid handle, refcounts.
- `test_shader_loader_spirv_stage_entry` – validates stage and entry point.
- `test_registry_dedup` – second acquire increments refcount.

## Revision History

- Version 1.0 (2025-10-11): Initial specification for geometry and shader loaders.


