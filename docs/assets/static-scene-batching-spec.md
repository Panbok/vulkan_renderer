---
status: proposed
updated: 2026-07-31
authority: design
---
# Static Scene Batching Spec (Phase 4)

## Purpose

Define a scene-level static batching pipeline that merges *multiple mesh
instances* into a small number of mega-buffers per (material, vertex format,
domain) group. This is separate from per-mesh mega-buffers (Phase 2) and is
targeted at large, mostly-static scenes like San Miguel.

## Goals

- Reduce world/shadow draw calls beyond per-mesh batching.
- Keep material bindings stable (one material per batch).
- Preserve correct bounds/culling for static geometry.
- Avoid per-reload memory growth (use reclaimable allocators).

## Non-Goals

- Dynamic object batching (moving objects remain unbatched).
- GPU-driven culling or meshlet-style clusters (can be added later).
- Cross-material batching (requires material indirection tables).

## Terminology

- **Static entity**: scene object that does not move for the entire scene.
- **Batch group**: collection of submeshes that share material + vertex format.
- **Batch range**: one original submesh range baked into a batch buffer.

## Batching Keys

Each static batch group is keyed by:

- `material_id` (or resolved material handle)
- `vertex_format` (currently `VkrVertex3d`)
- `pipeline_domain` (`WORLD` vs `WORLD_TRANSPARENT`)
- `shadow_mode` (opaque vs alpha-tested)

Alpha-tested casters are never mixed with opaque casters.

## Data Model (Proposed)

Introduce a new CPU-side resource owned by the scene system:

```
VkrStaticBatch {
  VkrGeometryHandle geometry;      // One V/I buffer pair
  VkrMaterialHandle material;      // Bound once for world
  VkrPipelineDomain domain;
  bool8_t alpha_tested;
  Array<VkrStaticBatchRange> ranges;
  Vec3 bounds_center;
  float32_t bounds_radius;
}

VkrStaticBatchRange {
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  Mat4 model;                      // Optional if transform baked
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
}
```

If transforms are baked, `model` can be omitted and `center/extents` are
pre-transformed.

## Build Pipeline (Scene Load)

1. Identify static entities:
   - entity has mesh component and a flag `static=true`
   - transform is not expected to change after load
2. Resolve each submesh into:
   - material handle
   - source geometry range (`first_index/index_count/vertex_offset`)
   - model matrix
3. Group submeshes by batching key.
4. For each group:
   - allocate merged vertex/index arrays (temp scope)
   - append each source range into the group buffers
   - compute per-range bounds in batch space
   - build one `VkrGeometryHandle` from merged buffers
   - store `VkrStaticBatch` + `VkrStaticBatchRange[]` in scene-owned storage
5. Release temp allocations and any intermediate handles.

## Rendering Integration

### World (opaque)

- For each static batch:
  - bind pipeline once
  - bind material once
  - bind merged V/I buffers once
  - emit one MDI call with `drawCount = range_count`
  - `firstInstance` points at per-range instance data (if not baked)
  - shaders should compute `instance_index = SV_InstanceID + SV_StartInstanceLocation`

### World (transparent)

- Only batch per material and keep back-to-front ordering *within* each batch.
- For now, transparent static objects should remain unbatched unless a
  stable per-batch sort strategy is added.

### Shadows

- Opaque shadows: no material bindings. Use `shader.shadow.opaque`.
- Alpha-tested shadows: batch per material + alpha texture.

## Culling Strategy

Minimum viable:

- Each `VkrStaticBatch` has a bounding sphere.
- If batch is out of frustum, skip the entire batch.

Better:

- Also store per-range bounds and CPU-cull before filling MDI command list.
- Optional GPU culling later (requires per-range data buffer).

## Memory + Lifetime

- Batch buffers are per-scene and must be freed on scene unload.
- Use `VkrDMemory` or `VkrPool` for batch metadata (ranges, batch structs).
- Use a scoped allocator for temporary merge buffers.
- Release global allocator accounting before destroying bulk allocations.

## Rebuild Triggers

Rebuild static batches when:

- scene is unloaded/reloaded
- materials or meshes used by a static entity are reloaded
- any "static" entity transform changes (should invalidate static flag)

## Debug + Metrics

Track:

- static batches created
- total ranges merged
- bytes uploaded to GPU
- world/shadow draw calls saved vs baseline

## Implementation Steps (Suggested)

1. Add a `static` flag to scene mesh components (if not already present).
2. Implement `vkr_scene_static_batch_build()` in `lib/src/renderer/systems/vkr_scene_system.c`.
3. Add a new render path in `vkr_pass_world.c` and `vkr_pass_shadow.c` for static batches.
4. Add debug stats to `VkrRendererFrameMetrics`.
5. Validate memory cleanup on scene unload.

## Risks / Watchouts

- MDI requires `multiDrawIndirect` and `drawIndirectFirstInstance`.
- Slang’s `SV_InstanceID` is `(InstanceIndex - BaseInstance)`; add
  `SV_StartInstanceLocation` to recover the true index.
- Avoid batching transparent geometry without a deterministic ordering plan.
- Keep batch creation out of arena allocators that persist across scene reloads.
