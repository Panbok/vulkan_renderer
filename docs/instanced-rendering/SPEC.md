---
status: partial
updated: 2026-07-31
authority: spec
---
# Instanced Rendering Implementation Specification

> **Target**: Improve San Miguel scene performance from ~10 FPS (debug) / ~40 FPS (release) toward 60+ FPS on Apple M1 with CSM (4 cascades) and 4K textures.
>
> **Portability requirement (added)**: The design must run on:
> - **Ryzen 5 2600 + RX 6700 XT** (discrete GPU, Vulkan)
> - **Apple M1** (Vulkan via MoltenVK, or future native Metal backend)
>
> **Stateless note:** References to "world view" and "shadow view" map to the
> stateless pass executors (`pass.world`, `pass.shadow`) and packet-driven
> draw lists.

## Executive Summary

This specification defines a practical instancing + batching path for the
renderer **without relying on bindless** features. Direct instancing and bounded
MDI now ship; sections below retain parts of the original design for rationale,
so ADR-013 and the renderer architecture spec are authoritative for current
behavior.

The core idea is:
- keep the existing **material instance descriptor set** (textures/material params) binding model,
- move **per-object transform/object_id** out of push constants into a **streamed per-frame instance buffer**, and
- render opaque geometry using **instanced draws** (direct first; indirect optional).

This yields meaningful CPU overhead reduction on Ryzen 2600 and reduces Vulkan command recording pressure on MoltenVK/M1, while keeping a safe fallback path.

---

## Implementation Status (current repo)

Summary of what is implemented vs. remaining work.

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 0 (metrics + feature probes) | Implemented, partial | Command/call, visibility, merge, graph CPU/GPU timing, MDI and first-instance probes ship; per-stage extraction timing remains absent. |
| Phase 1 (draw sorting) | Implemented | Opaque draws are sorted/batched by key; transparent draws are sorted back-to-front and rendered per object. |
| Phase 2 (instance buffer + direct instancing) | Implemented | Instance buffer pool, SSBO binding (set 0, binding 1), shaders switched to instance data, and world/picking/shadow/gizmo paths use instance data. |
| Phase 3 (indirect draws) | Implemented | Indirect draw buffer system + backend API; compatible world/shadow groups use MDI with direct fallback. |
| Phase 4 (material SSBO/bindless) | Not implemented | No material SSBO or descriptor indexing path. |
| Phase 5 (shadow batching) | Implemented | pass.shadow batches instanced draws; opaque/alpha split with opaque using no set 1. |

### Current Implementation Details (session notes)

- Instance data is provided via a per-frame SSBO and indexed in shaders; per-object push constants were removed.
- Instance index portability uses `SV_InstanceID + SV_StartInstanceLocation` so Vulkan `firstInstance` is honored without relying on `SV_VulkanInstanceID`.
- `shaderDrawParameters` (Vulkan 1.1) remains enabled but is not required for correctness.
- Global descriptor set 0 now includes `g_instances` and is refreshed per frame when the instance buffer changes; instance UBO binding stage flags include vertex+fragment.
- Transparent rendering uses the transparent pipeline domain but still renders per object; global descriptors are re-applied on pipeline changes to avoid stale bindings.
- Instance buffer pool uses preferred/fallback memory flags and explicit flush when memory is non-coherent.
- Default material fallback is applied when submesh materials are missing to avoid null material paths in the world and gizmo renderers.
- Indirect draw system uses a frame-slot-indexed command stream and
  `vkCmdDrawIndexedIndirect` for compatible world/shadow groups when enabled.
- Shadow rendering keeps a light-visible instance list separate from the camera
  list, batches opaque commands by geometry/index-buffer state, and leaves
  alpha-tested draws direct; the opaque shadow pass uses only set 0.
- Camera culling tests conservative submesh spheres; shadow culling keeps the
  union of all cascade volumes rather than assuming cascades are nested.
- Local reflection probes are selected once per draw, so their position-
  dependent binding context prevents unsafe world instancing across positions.
- Per-frame batch metrics (world draws/batches, shadow draws/batches/set1 binds, batch avg/max) are collected and shown in UI.
- Mesh loading reuses geometry by a stable key derived from `mesh_path + subset_index`; material loader returns existing handles instead of failing on duplicates. Texture system already dedups by path.
- San Miguel content is heavily submesh-split (~1563 `usemtl` switches, 281 materials), so batching gains are limited unless geometry is reused across multiple scene instances.

---

## Current Architecture Analysis

### Original Draw Call Flow (Historical Bottleneck)

```
Per Frame:
  For each mesh in mesh_manager:
    For each submesh in mesh:
      1. vkCmdBindPipeline()           // if pipeline changed
      2. vkCmdBindDescriptorSets()     // per-object instance state
      3. vkCmdBindVertexBuffers()      // per-geometry
      4. vkCmdBindIndexBuffer()        // per-geometry
      5. vkCmdPushConstants()          // model matrix, object_id
      6. vkCmdDrawIndexed(count, 1, 0, 0, 0)  // instance_count=1 always
```

**With CSM 4 cascades**: Each object renders 5 times (1 main + 4 shadow passes).

### Key Data Structures

```c
// Current per-submesh state (vkr_mesh_manager.h)
typedef struct VkrSubMesh {
  VkrGeometryHandle geometry;      // Dedicated V/I buffers
  VkrMaterialHandle material;      // Material properties + textures
  VkrPipelineHandle pipeline;      // Graphics pipeline
  VkrRendererInstanceStateHandle instance_state;  // Descriptor set
} VkrSubMesh;

// Current instance state pool (vulkan_types.h)
#define VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT 8192
VulkanShaderObjectInstanceState instance_states[8192];
```

### Performance Bottlenecks

| Bottleneck | Impact | Current State |
|------------|--------|---------------|
| Draw calls | HIGH | 1 per submesh, no batching |
| Descriptor binds | HIGH | 1 per submesh (set 1) |
| Buffer binds | MEDIUM | 1 V/I bind per geometry |
| Pipeline binds | LOW | Cached, minimal switching |
| Push constants | LOW | 80 bytes per draw |

---

## Proposed Solution Overview

### Non-goals / constraints (important)

- **Transparent rendering**: do **not** instance multi-object transparent draws (blending order becomes incorrect). Transparent stays per-object back-to-front, but can still benefit from Phase 1 sorting for state changes within equal-depth buckets.
- **Bindless textures**: optional only (Phase 4). Must not be required for correctness or baseline performance.
- **One size fits all memory flags**: the current code often asks for `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`; that can fail on discrete GPUs without a matching memory type. This spec requires **preferred + fallback memory policies** to support RX6700XT.

### Phase 0: Measurements & feature detection (added)
- Implemented: per-frame counters for world draws/batches/indirect usage and per-cascade shadow draw/batch/set1 binds, surfaced as a UI HUD.
- Remaining: add CPU/GPU timing around draw collection, sort, instance write, and shadow passes.
- Remaining: runtime feature probes for optional features (descriptor indexing, indirect).
- **Portability fix prerequisite**: audit all “dynamic update” buffers (including existing global UBOs) to avoid requiring impossible memory flag combinations on discrete GPUs. Use a preferred+fallback policy (see Phase 2 memory policy).

### Phase 1: Draw sorting (CPU)
Collect draws into arrays, then sort opaque by `(pipeline, material, geometry)` to minimize state changes. Transparent remains back-to-front by distance.

### Phase 2: Instanced direct draws (core)
Stream per-instance transforms/object IDs into a per-frame buffer and replace per-object push constants with `SV_InstanceID` indexing. Render opaque batches using **`vkCmdDrawIndexed(..., instanceCount > 1, ..., firstInstance)`**.

### Phase 3: Indirect draws (optional, gated)
Optionally stream `VkDrawIndexedIndirectCommand` into an indirect buffer and issue **one `vkCmdDrawIndexedIndirect` per (pipeline, material, geometry) group**. This reduces CPU call overhead further, but should be feature-gated and benchmarked on MoltenVK (some drivers are slower with indirect).

### Phase 4: Material SSBO / bindless (optional)
Enable different materials inside the same geometry instanced group using a material buffer + texture indexing **only when supported**; otherwise keep batching within the same material.

### Phase 5: Shadow pass instancing (core)
Apply the same instancing infrastructure to shadow rendering. **Do not reuse the camera-visible instance list** for shadows; build a separate shadow-caster list (or per-cascade lists) to avoid incorrect omissions.

---

## Phase 1: Draw Call Sorting

**Status**: Implemented (world pass batching and transparent sorting).

### Goal
Reduce state changes by sorting opaque draws by (pipeline_id, material_id, geometry_id).

### Issues in the original spec (fixed)

- Sorting was presented as a standalone win, but the renderer already sorts transparents by distance; the spec must explicitly define **opaque vs transparent policy** and how keys differ.
- The original `VkrDrawCommand` stored `Mat4 model` by value; this is okay, but for CPU cost it’s often better to store a **transform reference/index** and compute the final matrix only when writing the instance buffer (Phase 2).

### New Files (proposed)

**`lib/src/renderer/vkr_draw_batch.h`**
```c
#pragma once

#include "defines.h"
#include "math/mat.h"
#include "containers/array.h"

// Sort key for draw batching
typedef struct VkrDrawKey {
  uint32_t pipeline_id;
  uint32_t material_id;
  uint32_t geometry_id;
} VkrDrawKey;

// Single draw command (collected during frustum cull)
typedef struct VkrDrawCommand {
  VkrDrawKey key;
  uint32_t mesh_index;
  uint32_t submesh_index;
  // Prefer storing a stable transform reference; material system already
  // knows how to build the world matrix. The by-value Mat4 variant is allowed
  // for the first implementation if it keeps integration simpler.
  Mat4 model;
  uint32_t object_id;           // For picking system
  float32_t camera_distance;    // For transparent sorting
} VkrDrawCommand;

Array(VkrDrawCommand);

// Batch of draws sharing same state
typedef struct VkrDrawBatch {
  VkrDrawKey key;
  uint32_t first_command;       // Index into command array
  uint32_t command_count;       // Draws in this batch
  // Populated in Phase 2+: baseInstance into the instance buffer stream.
  uint32_t first_instance;
} VkrDrawBatch;

Array(VkrDrawBatch);

// Draw batcher state (per-frame, reset each frame)
typedef struct VkrDrawBatcher {
  Array_VkrDrawCommand opaque_commands;
  Array_VkrDrawCommand transparent_commands;
  Array_VkrDrawBatch opaque_batches;
  Array_VkrDrawBatch transparent_batches;

  // Statistics
  uint32_t total_draws_collected;
  uint32_t batches_created;
  uint32_t draws_merged;
} VkrDrawBatcher;

// API
bool8_t vkr_draw_batcher_init(VkrDrawBatcher *batcher, VkrAllocator *allocator,
                               uint32_t initial_capacity);
void vkr_draw_batcher_shutdown(VkrDrawBatcher *batcher);
void vkr_draw_batcher_reset(VkrDrawBatcher *batcher);

void vkr_draw_batcher_add_opaque(VkrDrawBatcher *batcher,
                                  const VkrDrawCommand *cmd);
void vkr_draw_batcher_add_transparent(VkrDrawBatcher *batcher,
                                       const VkrDrawCommand *cmd);

// Sort and build batches (call after collecting all draws)
void vkr_draw_batcher_finalize(VkrDrawBatcher *batcher);

// Iteration
uint32_t vkr_draw_batcher_opaque_batch_count(const VkrDrawBatcher *batcher);
const VkrDrawBatch *vkr_draw_batcher_get_opaque_batch(
    const VkrDrawBatcher *batcher, uint32_t index);
const VkrDrawCommand *vkr_draw_batcher_get_command(
    const VkrDrawBatcher *batcher, uint32_t index);
```

### Integration Point

**`vkr_pass_world.c` modification:**
```c
// Before: Immediate render during iteration
for (mesh in meshes) {
  for (submesh in mesh) {
    render_submesh(submesh);  // Draw immediately
  }
}

// After: Collect, sort, then render
VkrDrawBatcher batcher;
for (mesh in meshes) {
  if (!frustum_cull(mesh)) continue;
  for (submesh in mesh) {
    VkrDrawCommand cmd = { ... };
    if (is_transparent(submesh)) {
      vkr_draw_batcher_add_transparent(&batcher, &cmd);
    } else {
      vkr_draw_batcher_add_opaque(&batcher, &cmd);
    }
  }
}

vkr_draw_batcher_finalize(&batcher);

// Render sorted batches
for (batch in opaque_batches) {
  bind_state_if_changed(batch.key);
  for (cmd in batch.commands) {
    push_constants(cmd.model, cmd.object_id);
    draw_indexed(...);
  }
}
```

### Expected Impact
- **State changes**: Reduced by 60-80% for scenes with material/geometry reuse
- **Draw calls**: Unchanged (still 1 per submesh)
- **Implementation effort**: Low (pure CPU sorting)

---

## Phase 2: Instance Data Buffer

**Status**: Implemented (instance buffer pool, shaders updated, world/picking/shadow/gizmo paths wired).

### Goal
Enable GPU instancing for opaque geometry by moving per-object data (model matrix, picking id) out of per-object push constants into a per-frame instance stream.

### Key correction (original spec inconsistency)

The original text claimed Phase 2 alone reduces draw calls to **1 per (pipeline, geometry)**. That is only true if you **also** eliminate per-material descriptor binding (Phase 4 / bindless). With the current renderer architecture (material = descriptor set), Phase 2 reduces opaque draws to approximately:

- **1 draw per (pipeline, material, geometry)**, with `instanceCount = number_of_objects_in_that_group`.

### New Files

**`lib/src/renderer/vkr_instance_buffer.h`**
```c
#pragma once

#include "defines.h"
#include "math/mat.h"
#include "renderer/vkr_renderer.h"

// GPU-side instance data (must match shader struct)
// Aligned to 16 bytes for std430 layout
typedef struct VkrInstanceDataGPU {
  Mat4 model;                   // 64 bytes - world transform
  uint32_t object_id;           // 4 bytes  - picking ID
  uint32_t material_index;      // 4 bytes  - index into material SSBO (Phase 4)
  uint32_t flags;               // 4 bytes  - per-instance flags
  uint32_t _padding;            // 4 bytes  - alignment
} VkrInstanceDataGPU;           // Total: 80 bytes

_Static_assert(sizeof(VkrInstanceDataGPU) == 80,
               "VkrInstanceDataGPU must be 80 bytes");
_Static_assert(sizeof(VkrInstanceDataGPU) % 16 == 0,
               "VkrInstanceDataGPU must be 16-byte aligned");

// Per-frame instance buffer
typedef struct VkrInstanceBuffer {
  VkrBufferHandle buffer;
  VkrInstanceDataGPU *mapped_ptr;  // Persistent map (M1 unified memory)
  uint32_t capacity;
  uint32_t write_offset;           // Current write position
} VkrInstanceBuffer;

// Triple-buffered instance buffer pool
typedef struct VkrInstanceBufferPool {
  VkrInstanceBuffer buffers[3];    // BUFFERING_FRAMES
  uint32_t current_frame;
  uint32_t max_instances;
  bool8_t initialized;
} VkrInstanceBufferPool;

// API
bool8_t vkr_instance_buffer_pool_init(VkrInstanceBufferPool *pool,
                                       VkrRendererFrontendHandle renderer,
                                       uint32_t max_instances);
void vkr_instance_buffer_pool_shutdown(VkrInstanceBufferPool *pool,
                                        VkrRendererFrontendHandle renderer);

// Call at frame start
void vkr_instance_buffer_begin_frame(VkrInstanceBufferPool *pool,
                                      uint32_t frame_index);

// Write instance data, returns base_instance for draw call
uint32_t vkr_instance_buffer_write(VkrInstanceBufferPool *pool,
                                    const VkrInstanceDataGPU *instances,
                                    uint32_t count);

// Get current buffer for binding
VkrBufferHandle vkr_instance_buffer_get_current(
    const VkrInstanceBufferPool *pool);
```

### API simplification (recommended)

To avoid per-instance function call overhead on Ryzen 2600, prefer an allocation-style API:

```c
// Allocates 'count' contiguous instances, returns base_instance and writable pointer.
bool8_t vkr_instance_buffer_alloc(VkrInstanceBufferPool *pool,
                                 uint32_t count,
                                 uint32_t *out_base_instance,
                                 VkrInstanceDataGPU **out_ptr);
```

This turns instance writes into a tight memcpy/struct-fill loop.

### Shader Modifications

**`lib/src/renderer/vulkan/shaders/world/default.slang`**
```slang
// Keep g_ubo at (set 0, binding 0) unchanged.

// Replace per-object push constants (model matrix) with per-frame instance data.
struct InstanceData {
    column_major float4x4 model;
    uint object_id;
    uint material_index;
    uint flags;
    uint _padding;
};

[[vk::binding(1, 0)]]  // Set 0, Binding 1
StructuredBuffer<InstanceData> g_instances;

// Before (today): push constant carries the model matrix
// struct PushConstantsObject { column_major float4x4 model; };
// [[vk::push_constant]] ConstantBuffer<PushConstantsObject> push_constants;

// Vertex shader input (existing)
struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 tex_coord : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
};

[shader("vertex")]
VertexDto vertexMain(VertexInput input, uint instance_id : SV_InstanceID,
                     uint base_instance : SV_StartInstanceLocation) {
    uint instance_index = instance_id + base_instance;
    InstanceData inst = g_instances[instance_index];

    // Replace push_constants.model with inst.model throughout.
    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    float4 viewPos = mul(g_ubo.view, worldPos);
    float4 clipPos = mul(g_ubo.projection, viewPos);

    VertexDto output;
    output.position = clipPos;
    output.frag_position = worldPos.xyz;
    output.normal = normalize(mul((float3x3)inst.model, input.normal));
    // If picking uses a separate pipeline, object_id can be written there.

    return output;
}
```

### Instance index correctness (portable)

Slang’s `SV_InstanceID` equals `InstanceIndex - BaseInstance`. To recover the
true Vulkan instance index, add `SV_StartInstanceLocation`:

```
uint instance_index = instance_id + base_instance;
```

This keeps direct and indirect instanced draws correct without requiring
`SV_VulkanInstanceID`.

### Descriptor Set Layout Changes

**`vulkan_shaders.c` modification:**
```c
// Global descriptor set layout (set 0) currently:
// - binding 0: g_ubo (optional; some shaders set global_ubo_stride=0)
//
// Required change:
// - add binding 1: g_instances (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertex stage)
//
// Practical approach (minimal churn):
// - Always create set 0 for every shader object.
// - When instancing is enabled, set 0 includes binding 1 for all shader objects
//   (even if a particular shader doesn't read it).
//
// Descriptor pool updates:
// - Add VK_DESCRIPTOR_TYPE_STORAGE_BUFFER with descriptorCount = frame_count.
//
// Descriptor writes:
// - Instance buffers are triple-buffered; update each per-frame global descriptor
//   set once at init to point at that frame's instance buffer. No per-frame
//   descriptor rewrite is required.
```

### Memory policy (portable)

The original spec suggested always using `HOST_VISIBLE|HOST_COHERENT|DEVICE_LOCAL`. That can fail on discrete GPUs.

Required memory selection policy:
- **Preferred** (unified / BAR-visible): `HOST_VISIBLE | HOST_COHERENT | DEVICE_LOCAL`
- **Fallback** (discrete GPUs like RX 6700 XT): `HOST_VISIBLE | HOST_COHERENT`
- **If not coherent**: allow `HOST_VISIBLE` and use `vkFlushMappedMemoryRanges` for written ranges.

The instance buffer is small enough (few MB) that host-visible memory is acceptable even on discrete GPUs.

### M1/Apple Silicon Memory Considerations

```c
// Prefer HOST_VISIBLE | HOST_COHERENT for unified memory efficiency.
// Add DEVICE_LOCAL only if the platform exposes a memory type that satisfies it.
VkrBufferDescription instance_buffer_desc = {
  .size = max_instances * sizeof(VkrInstanceDataGPU),
  .usage = vkr_buffer_usage_flags_from_bits(
      VKR_BUFFER_USAGE_STORAGE | VKR_BUFFER_USAGE_TRANSFER_DST),
  .memory_properties = vkr_memory_property_flags_from_bits(
      VKR_MEMORY_PROPERTY_HOST_VISIBLE |
      VKR_MEMORY_PROPERTY_HOST_COHERENT |
      VKR_MEMORY_PROPERTY_DEVICE_LOCAL),  // M1 unified memory
  .bind_on_create = true_v,
};

// Persistent mapping - no map/unmap per frame
vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped_ptr);
// Keep mapped for entire buffer lifetime
```

### Expected Impact
- **Draw calls**: Reduced to ~1 per unique **(pipeline, material, geometry)** for opaque objects
- **Descriptor binds**: Reduced (no per-object instance state bind; only per-material)
- **Implementation effort**: Medium

---

## Phase 3: Indirect Draw Buffer

**Status**: Implemented (opaque world path uses indirect draws; transparent remains direct).

### Goal
Reduce CPU call overhead further by using `vkCmdDrawIndexedIndirect` for each state-stable group, while keeping a direct draw fallback.

### Major correction (original spec bug)

You **cannot** “record bindings while building the indirect buffer and then execute all indirect draws later” and expect bindings to match. Pipeline/material/vertex/index binding state is consumed at command execution time, and `vkCmdDrawIndexedIndirect` executes with **the currently bound** state.

Therefore indirect execution must be:
- grouped by `(pipeline, material, geometry)` (or a superset),
- bind that state,
- then call `vkCmdDrawIndexedIndirect` for the commands for that group.

### New Files

**`lib/src/renderer/vkr_indirect_draw.h`**
```c
#pragma once

#include "defines.h"
#include "renderer/vkr_renderer.h"

// Matches VkDrawIndexedIndirectCommand exactly
typedef struct VkrIndirectDrawCommand {
  uint32_t index_count;
  uint32_t instance_count;
  uint32_t first_index;
  int32_t  vertex_offset;
  uint32_t first_instance;
} VkrIndirectDrawCommand;

_Static_assert(sizeof(VkrIndirectDrawCommand) == 20,
               "Must match VkDrawIndexedIndirectCommand");

typedef struct VkrIndirectDrawBuffer {
  VkrBufferHandle buffer;
  VkrIndirectDrawCommand *mapped_ptr;
  uint32_t capacity;
  uint32_t count;
} VkrIndirectDrawBuffer;

typedef struct VkrIndirectDrawSystem {
  VkrIndirectDrawBuffer buffers[3];  // Triple buffered
  uint32_t current_frame;
  bool8_t enabled;
  bool8_t initialized;
} VkrIndirectDrawSystem;

// API
bool8_t vkr_indirect_draw_init(VkrIndirectDrawSystem *sys,
                                VkrRendererFrontendHandle renderer,
                                uint32_t max_draws);
void vkr_indirect_draw_shutdown(VkrIndirectDrawSystem *sys,
                                 VkrRendererFrontendHandle renderer);

void vkr_indirect_draw_begin_frame(VkrIndirectDrawSystem *sys,
                                    uint32_t frame_index);
// Allocation-style API: the view fills commands directly.
bool8_t vkr_indirect_draw_alloc(VkrIndirectDrawSystem *sys,
                               uint32_t count,
                               uint64_t *out_offset_bytes,
                               VkrIndirectDrawCommand **out_ptr);

VkrBufferHandle vkr_indirect_draw_get_current_buffer(
    const VkrIndirectDrawSystem *sys);
```

### Vulkan usage requirement (implementation detail)

To support indirect draws through the existing `VkrBufferHandle` API, add an explicit buffer usage bit:

- `VKR_BUFFER_USAGE_INDIRECT` → maps to `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`

Without this, the backend may create buffers that cannot be bound as indirect buffers.

### API simplification (recommended)

Keep the indirect system “dumb” (just buffer management). Grouping stays in the view/batcher which already has the draw keys.

The view loop becomes:
- allocate N commands for the current `(pipeline, material, geometry)` group
- fill commands with `.first_instance` from the instance stream
- bind state for the group
- `vkr_renderer_draw_indexed_indirect(indirect_buffer, offset_bytes, N, sizeof(VkDrawIndexedIndirectCommand))`

### Backend API Addition

**`lib/src/renderer/vkr_renderer.h`**
```c
// Add to VkrRendererBackendInterface
void (*draw_indexed_indirect)(
    void *backend_state,
    VkrBackendResourceHandle indirect_buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride);

// Frontend wrapper
void vkr_renderer_draw_indexed_indirect(
    VkrRendererFrontendHandle renderer,
    VkrBufferHandle indirect_buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride);
```

**`lib/src/renderer/vulkan/vulkan_backend.c`**
```c
void renderer_vulkan_draw_indexed_indirect(
    void *backend_state,
    VkrBackendResourceHandle indirect_buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride) {

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VulkanCommandBuffer *cmd = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  struct s_BufferHandle *buffer = (struct s_BufferHandle *)indirect_buffer.ptr;

  vkCmdDrawIndexedIndirect(
      cmd->handle,
      buffer->handle,
      offset,
      draw_count,
      stride);
}
```

### Implementation Notes (current repo)

- `vkr_indirect_draw.c/h` manages a triple-buffered, persistently-mapped indirect command stream with preferred/fallback memory flags and explicit flush on non-coherent memory.
- `VKR_BUFFER_USAGE_INDIRECT` maps to `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` in the Vulkan backend.
- `vkr_renderer_draw_indexed_indirect` and `renderer_vulkan_draw_indexed_indirect` are wired end-to-end.
- World opaque batches use indirect draws when the indirect system is initialized and enabled; transparent draws remain direct.
- Indirect commands set `first_instance` per draw and shaders recover the index
  via `SV_StartInstanceLocation` as described above.

### Batched Draw Flow

```c
// Correct high-level flow (pseudo):
//
// 1) Build instance stream + indirect buffer ranges per key
// 2) For each recorded range:
//      bind pipeline/material/geometry
//      draw_indexed_indirect(indirect_buffer, range.offset, range.draw_count, stride)
```

### Expected Impact
- **Draw calls**: ~1 `vkCmdDrawIndexedIndirect` per unique (pipeline, material, geometry)
- **CPU overhead**: Dramatically reduced (no per-instance Vulkan calls)
- **Implementation effort**: Medium-High (and must be benchmarked per platform)

---

## Phase 4: Material SSBO (Optional)

**Status**: Not implemented.

### Goal
Allow different materials within same instanced batch.

### Data Structure

```c
// GPU-side material data
typedef struct VkrMaterialDataGPU {
  Vec4 diffuse_color;           // 16 bytes
  Vec4 specular_color;          // 16 bytes
  Vec4 emission_color_shininess; // xyz=emission, w=shininess, 16 bytes
  uint32_t texture_flags;       // 4 bytes
  uint32_t diffuse_tex_idx;     // 4 bytes (bindless index)
  uint32_t normal_tex_idx;      // 4 bytes
  uint32_t specular_tex_idx;    // 4 bytes
} VkrMaterialDataGPU;           // Total: 64 bytes
```

### Shader Integration

```slang
[[vk::binding(2, 0)]]
StructuredBuffer<MaterialData> g_materials;

// In fragment shader
MaterialData mat = g_materials[inst.material_index];
float4 diffuse = mat.diffuse_color;
if (mat.texture_flags & TEXTURE_FLAG_HAS_DIFFUSE) {
    diffuse *= SampleTexture(mat.diffuse_tex_idx, uv);
}
```

### Note on Bindless Textures
Full bindless requires `VK_EXT_descriptor_indexing`. Check device support:
```c
VkPhysicalDeviceDescriptorIndexingFeatures indexing_features = {
  .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
  .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
  .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
  .descriptorBindingPartiallyBound = VK_TRUE,
};
```

For initial implementation without bindless: batch only within same-material groups.

---

## Phase 5: Shadow Pass Optimization

**Status**: Implemented (instanced batching with opaque/alpha split; opaque uses set 0 only).

### Goal
Apply instancing to CSM shadow rendering.

### Current Shadow Flow (vkr_pass_shadow.c)
```c
for (cascade = 0; cascade < 4; cascade++) {
  begin_shadow_pass(cascade);
  for (mesh in visible_meshes) {
    for (submesh in mesh) {
      push_constants(light_vp[cascade], model);
      draw_indexed(...);  // 1 draw per submesh per cascade
    }
  }
  end_shadow_pass();
}
```

### Optimized Shadow Flow
```c
// IMPORTANT: camera-visible instance data cannot be reused for shadow culling.
// Build a separate list of shadow casters (and optionally per-cascade lists).
VkrBufferHandle instance_buffer = vkr_instance_buffer_get_current(&shadow_pool);

for (cascade = 0; cascade < 4; cascade++) {
  begin_shadow_pass(cascade);
  bind_instance_buffer(instance_buffer);

  // Push cascade-specific light matrix
  push_constants(light_vp[cascade]);

  // For each (geometry, shadow-material) group:
  //   bind geometry
  //   bind material instance state (alpha cutoff + diffuse texture)
  //   draw instanced (direct first; indirect optional)
  end_shadow_pass();
}
```

### Shadow Shader Modification

**`lib/src/renderer/vulkan/shaders/shadow/cutout.slang`**
```slang
struct ShadowPushConstants {
    column_major float4x4 light_view_projection;
    uint cascade_index;
};

[[vk::push_constant]]
ShadowPushConstants push;

// Keep set numbering stable with the world shader:
// g_instances should be (set 0, binding 1) if following the Phase 2 convention.
// If instancing is disabled, the shadow shader falls back to push_constants.model.
[[vk::binding(1, 0)]]
StructuredBuffer<InstanceData> g_instances;

[shader("vertex")]
float4 vertexMain(float3 position : POSITION, uint instance_id : SV_InstanceID) : SV_Position {
    InstanceData inst = g_instances[instance_id];
    float4 world_pos = mul(inst.model, float4(position, 1.0));
    return mul(push.light_view_projection, world_pos);
}
```

### Expected Impact
- **Shadow draw calls**: Reduced from N*4 to M*4 (M << N)
- **Shadow pass time**: 50-70% reduction
- **Implementation effort**: Medium (reuses Phase 2-3 infrastructure)

---

## Implementation Schedule

```
Phase 1: Draw Sorting         [2-3 days]
  - vkr_draw_batch.c/h
  - Integration in vkr_pass_world.c
  - Unit tests

Phase 2: Instance Buffer      [3-4 days]
  - vkr_instance_buffer.c/h
  - Shader modifications
  - Descriptor set layout changes
  - M1 memory optimization

Phase 3: Indirect Draw        [done]
  - vkr_indirect_draw.c/h
  - Backend API
  - Integration with Phase 1-2

Phase 4: Material SSBO        [2-3 days] (Optional)
  - Material buffer system
  - Bindless texture setup
  - Shader modifications

Phase 5: Shadow Optimization  [2-3 days]
  - Shadow shader changes
  - Instance buffer sharing
  - Cascade-specific optimizations

Total: 11-16 days (Phases 1-3 + 5)
       13-19 days (with Phase 4)
```

---

## Verification & Testing

### Performance Metrics
```c
// Add to renderer frontend
typedef struct VkrInstancingStats {
  uint32_t total_objects;
  uint32_t total_draw_calls;
  uint32_t batches_created;
  uint32_t instances_written;
  uint32_t indirect_draws_issued;
  float64_t batch_time_ms;
  float64_t draw_time_ms;
} VkrInstancingStats;
```

### Test Cases
1. **Single object**: Verify basic instancing works
2. **1000 identical objects**: Verify batching reduces draws to 1
3. **1000 unique materials**: Verify graceful degradation
4. **San Miguel scene**: Target metric (60+ FPS)
5. **Shadow cascades**: Verify 4x reduction per cascade
6. **Picking system**: Verify object IDs preserved
7. **Transparent sorting**: Verify correct back-to-front order

### Profiling Points
- RenderDoc frame capture for draw call analysis
- `VK_EXT_debug_utils` markers for GPU timing
- CPU profiling of batch building

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Shader complexity | Keep fallback non-instanced path |
| Memory growth | Fixed caps + graceful fallback to non-instanced; avoid arena growth on reload |
| Picking breaks | Explicit object_id in instance data |
| M1 compatibility | Prefer direct instanced draws first; keep indirect behind flag |
| RX6700XT compatibility | Use portable memory flags (no forced DEVICE_LOCAL+HOST_VISIBLE); flush if non-coherent |
| Debug performance | Profile debug build separately |

### Feature Flag
```c
// In vkr_renderer.h or config
typedef struct VkrInstancingConfig {
  bool8_t enabled;
  bool8_t indirect_draws_enabled;
  bool8_t material_ssbo_enabled;
  uint32_t max_instances;
  uint32_t max_indirect_draws;
} VkrInstancingConfig;

#define VKR_INSTANCING_CONFIG_DEFAULT \
  ((VkrInstancingConfig){ \
    .enabled = true_v, \
    .indirect_draws_enabled = false_v, \
    .material_ssbo_enabled = false_v, \
    .max_instances = 65536, \
    .max_indirect_draws = 4096, \
  })
```

---

## Files Summary

### New Files
| File | Purpose |
|------|---------|
| `lib/src/renderer/vkr_draw_batch.c/h` | Draw command collection and sorting |
| `lib/src/renderer/vkr_instance_buffer.c/h` | GPU instance data management |
| `lib/src/renderer/vkr_indirect_draw.c/h` | Indirect draw buffer system |

### Modified Files
| File | Changes |
|------|---------|
| `lib/src/renderer/vkr_renderer.h` | Add indirect draw API, instancing config |
| `lib/src/renderer/renderer_frontend.c` | Frontend wrappers for indirect draws |
| `lib/src/renderer/vulkan/vulkan_backend.c` | `vkCmdDrawIndexedIndirect` implementation |
| `lib/src/renderer/vulkan/vulkan_backend.h` | Backend API declaration for indirect draws |
| `lib/src/renderer/vulkan/vulkan_shaders.c` | Instance SSBO descriptor binding |
| `lib/src/renderer/vulkan/vulkan_types.h` | Indirect draw types |
| `lib/src/renderer/vulkan/vulkan_utils.c` | Buffer usage flag mapping for indirect draws |
| `lib/src/renderer/renderer_frontend.h` | Frontend state includes indirect draw system |
| `lib/src/renderer/passes/vkr_pass_world.c` | Batch collection and instanced rendering |
| `lib/src/renderer/passes/vkr_pass_shadow.c` | Shadow pass instancing |
| `lib/src/renderer/systems/vkr_mesh_manager.c` | Geometry reuse by stable mesh+subset key |
| `lib/src/renderer/resources/loaders/material_loader.c` | Reuse existing materials on duplicate loads |
| `app/src/main.c` | On-screen metrics HUD (world/shadow batching) |
| `lib/src/renderer/systems/vkr_geometry_system.c` | Geometry indirect draw helper |
| `lib/src/renderer/systems/vkr_geometry_system.h` | Geometry indirect draw helper |
| `lib/src/renderer/vulkan/shaders/world/default.slang` | Instance buffer sampling |
| `lib/src/renderer/vulkan/shaders/shadow/cutout.slang` | Shadow instancing support |
| `lib/src/renderer/vulkan/shaders/picking/world.slang` | Object ID from instance buffer |

---

## Appendix A: Quick Reference - Current vs Proposed

### Current Draw Loop
```
For each mesh:
  For each submesh:
    BindPipeline()
    BindDescriptorSet(instance_set)
    BindVertexBuffer()
    BindIndexBuffer()
    PushConstants(model, object_id)
    DrawIndexed(count, 1, ...)
```

### Proposed Draw Loop (Phase 3)
```
WriteInstanceBuffer(all_instances)
BindDescriptorSet(global_set_with_instance_ssbo)

For each unique (pipeline, material, geometry):
  BindPipeline()
  BindMaterialDescriptorSet()
  BindVertexBuffer()
  BindIndexBuffer()
  DrawIndexedIndirect(buffer, offset, count, stride)
```

### Draw Call Comparison (San Miguel estimate, tightened)

To make the estimate actionable and portable across scenes, define these measured counters (Phase 0 should log them):

- `W_opaque`: number of opaque world draws in the current renderer (≈ visible submeshes that are opaque)
- `W_trans`: number of transparent world draws (must remain per-object for correct blending)
- `S_casters`: number of shadow-caster draws for a single cascade in the current renderer
- `C`: cascade count (typically 4)

And these derived “group” counters:

- `G_world`: number of unique **(pipeline, material, geometry)** groups among `W_opaque`
- `G_shadow`: number of unique **(pipeline, material, geometry)** groups among `S_casters`

With those, the *expected* draw counts become:

- **Current**:
  - world: `W_opaque + W_trans`
  - shadow: `C * S_casters`
- **Phase 1 (sorting)**:
  - draw counts unchanged; only state changes reduced
- **Phase 2 (instanced direct)**:
  - world: `G_world + W_trans`
  - shadow: `C * G_shadow`
- **Phase 3 (indirect)**:
  - draw counts roughly the same as Phase 2 (still one call per group), but CPU overhead can drop further
- **Phase 5 (shadow instancing)**:
  - this is the point where shadow draw counts drop from `C * S_casters` to `C * G_shadow` (if Phase 2 wasn't already applied to shadows)

#### Concrete example (keeps the original ballpark, but makes assumptions explicit)

Assume San Miguel roughly matches the earlier “~2000 total draws” observation:

- `W_opaque ≈ 350`, `W_trans ≈ 50` → world ≈ 400
- `S_casters ≈ 400` per cascade, `C = 4` → shadow ≈ 1600
- `G_world ≈ 70` (moderate material/geometry reuse)
- `G_shadow ≈ 120` (more groups due to alpha-test material variants / fewer opportunities to merge)

Then:

| Scenario | Current | Phase 1 | Phase 2 | Phase 3 | Phase 5 |
|----------|---------|---------|---------|---------|---------|
| World pass | `~400` | `~400` | `~(70 + 50)=120` | `~120` | `~120` |
| Shadow (4 cascades) | `~1600` | `~1600` | `~1600`* | `~1600`* | `~(4*120)=480` |
| **Total** | **~2000** | **~2000** | **~1720** | **~1720** | **~600** |

`*` Phase 2/3 only reduce shadow draw calls if the shadow pass is migrated to the same instancing path (Phase 5).

> Note: if `W_trans` is small (typical for San Miguel), Phase 2’s win is dominated by how small `G_world` gets. If materials are highly unique, Phase 2 still helps (no per-object descriptor bind), but draw count reduction will be smaller.

---

## Appendix B: M1 Performance Notes

1. **Unified Memory**: Prefer `HOST_VISIBLE | HOST_COHERENT`; add `DEVICE_LOCAL` only if available as a combined type
2. **Persistent Mapping**: Map once, keep mapped (no map/unmap overhead)
3. **Coherency**: If memory is not coherent, flush written ranges explicitly
4. **Indirect draws**: Benchmark; keep a runtime flag to prefer direct instanced draws if indirect is slower
5. **Alignment**: Respect std430 rules (16-byte alignment). Use additional padding only if profiling proves a benefit.

---

## Appendix C: Platform / feature requirements (added)

### Baseline required features (must work everywhere)

- **Vulkan**: core `vkCmdDrawIndexed` instancing (Vulkan 1.0)
- **Storage buffers** in vertex stage (for SSBO-based instance data) OR a fallback instance-vertex-buffer path
- **`vkCmdDrawIndexedIndirect`** only for Phase 3, guarded by config + runtime check

### Optional features (may be absent or undesirable)

- **`VK_EXT_descriptor_indexing`**: only for Phase 4 (bindless); must not be required
- **Non-coherent memory**: must be supported by flushing ranges when needed

### Hardware-specific notes

- **Ryzen 5 2600 (CPU)**:
  - Avoid per-instance function calls in tight loops; prefer bulk allocation + struct fill.
  - Prefer radix sort on packed integer keys if qsort becomes a bottleneck.
  - Pre-reserve arrays to avoid per-frame allocations.

- **RX 6700 XT (Vulkan, discrete GPU)**:
  - Do not require `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT` to exist.
  - Host-visible instance buffer is acceptable; if profiling shows bandwidth issues, add a staged copy path later.

- **Mac M1 (MoltenVK)**:
  - Expect `VK_KHR_portability_subset` and different performance characteristics.
  - Prefer direct instanced draws as the baseline; keep indirect behind a feature flag and benchmark.
