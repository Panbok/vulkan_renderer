---
status: partial
updated: 2026-08-15
authority: spec
---
# Mesh System Refactor: Asset Dedup + Mesh Instances

**Legacy note:** This document includes historical names like `VkrViewWorldState`
and `VkrViewShadowDrawRange`. In the stateless renderer these map to packet-driven
pass executors and the helpers in
`lib/src/renderer/passes/internal/vkr_pass_draw_utils.*`. The asset/instance
refactor ships, but the historical names keep this specification partial.

## Status

- **Implemented feature, partially reconciled document** - All refactor phases
  completed; historical names remain in the design record.

## Problem Statement

Large scenes with many entities referencing the same mesh file (e.g. `100_000`
entities all referencing `assets/models/falcon.obj`) currently:

- Repeatedly invoke the mesh resource loader for the same `mesh_path`, which is
  slow and can exhaust memory (notably the mesh loader arena pool / scratch
  arenas).
- Create one `VkrMesh` entry per entity. `VkrMesh` currently contains both
  per-entity data (transform/model/render_id/visibility/bounds) and per-asset
  data (`submeshes` describing geometry/material draw ranges). This duplicates
  the per-asset portion `N` times.
- Hard-limit mesh count at init (default `max_mesh_count = 1024`), which was
  incompatible with scenes that contain tens/hundreds of thousands of meshes;
  the renderer frontend now configures `max_mesh_count = 100000`.

The end result is Out-Of-Memory failures and very slow load times for scenes
with heavy mesh reuse.

## Goals

1. **Load each unique mesh file once per load** (per unique key), then create
   many instances cheaply.
2. **Split mesh "asset" from mesh "instance"** so shared submesh data is stored
   once and per-entity state is stored per instance.
3. **Support large instance counts** (e.g. 100k+) without OOM under reasonable
   memory limits.
4. Preserve existing dedup where it already exists:
   - Geometry buffers are already deduped by stable geometry names derived from
     mesh path + subset index (or merged buffer key).
5. Keep lifetimes explicit and symmetric:
   - Instances can be created/destroyed frequently without unbounded memory
     growth.
   - Assets are released when no instances reference them.

## Non-Goals

- Redesigning the GPU instancing / MDI approach. This spec only restructures CPU
  mesh ownership and loading; it should be compatible with current instanced
  rendering infrastructure.
- Fully solving every "100k entities" performance issue (ECS query, culling,
  batching) beyond removing the biggest avoidable work: redundant mesh loads and
  redundant per-asset allocations.

---

# Implementation Details

## Phase 1: Loader-Level Dedup

**Files Modified:**
- `lib/src/renderer/systems/vkr_mesh_manager.c`

### MeshAssetKey Structure

```c
typedef struct VkrMeshAssetKey {
  String8 mesh_path;
  VkrPipelineDomain pipeline_domain;
  String8 shader_override;
} VkrMeshAssetKey;
```

### Equality/Lookup Functions

Added internal functions for comparing asset keys and scanning the unique list:

```c
vkr_internal bool8_t vkr_mesh_asset_key_equals(const VkrMeshAssetKey *a,
                                                const VkrMeshAssetKey *b) {
  if (a->pipeline_domain != b->pipeline_domain) return false_v;
  if (!string8_equals(&a->mesh_path, &b->mesh_path)) return false_v;
  if (!string8_equals(&a->shader_override, &b->shader_override)) return false_v;
  return true_v;
}

vkr_internal int32_t vkr_mesh_asset_key_find(
    const VkrMeshAssetKey *unique_keys, uint32_t unique_count,
    const VkrMeshAssetKey *key); // Linear scan, returns -1 if not found
```

`vkr_mesh_asset_key_from_desc` normalizes pipeline domains via
`vkr_mesh_manager_resolve_domain`, treating `0` as "unspecified" and defaulting
to `VKR_PIPELINE_DOMAIN_WORLD` (or the descriptor fallback).

### Batch Load Deduplication

Modified `vkr_mesh_manager_load_batch()` to:

1. Build `unique_keys[]` array from input descriptors
2. Track `desc_to_unique[]` mapping for each descriptor
3. Call the resource loader only once per unique key in the wave
4. Process each descriptor using the mapped handle info
5. Unload unique handles per wave to release loader arena chunks

Note: `unique_to_first_desc` is used in the instance batch path (Phase 3) to map
loaded assets back to the descriptor that defined the key.

---

## Phase 2: Mesh Asset Registry

**Files Modified:**
- `lib/src/renderer/resources/vkr_resources.h`
- `lib/src/renderer/systems/vkr_mesh_manager.h`
- `lib/src/renderer/systems/vkr_mesh_manager.c`

### New Types in vkr_resources.h

```c
typedef struct VkrMeshAssetHandle {
  uint32_t id;
  uint32_t generation;
} VkrMeshAssetHandle;

typedef struct VkrMeshAssetSubmesh {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  VkrPipelineDomain pipeline_domain;
  String8 shader_override;  // Owned copy

  uint32_t range_id;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t opaque_first_index;
  uint32_t opaque_index_count;
  int32_t opaque_vertex_offset;

  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;

  bool8_t owns_geometry;
  bool8_t owns_material;
} VkrMeshAssetSubmesh;
Array(VkrMeshAssetSubmesh);

typedef struct VkrMeshAsset {
  uint32_t id;
  uint32_t generation;

  String8 mesh_path;        // Owned (freeable allocator)
  VkrPipelineDomain domain;
  String8 shader_override;  // Owned (freeable allocator)
  char *key_string;         // Owned key for asset_by_key removal

  Array_VkrMeshAssetSubmesh submeshes;

  bool8_t bounds_valid;
  Vec3 bounds_local_center;
  float32_t bounds_local_radius;

  uint32_t ref_count;
} VkrMeshAsset;
Array(VkrMeshAsset);
```

`VkrMeshAsset.key_string` stores the owned hash-table key
(`mesh_path|domain|shader_override`) so removal can use the exact pointer; the
key is allocated from the asset allocator and freed after removal. Pipeline
domains are normalized (`0` -> fallback -> `VKR_PIPELINE_DOMAIN_WORLD`) before
building keys to keep cache lookups stable.

### VkrMeshManager Extensions

Added to `VkrMeshManager` struct:

```c
// Asset registry
VkrDMemory asset_dmemory;              // Freeable allocator for asset strings
VkrAllocator asset_allocator;
Array_VkrMeshAsset mesh_assets;
Array_uint32_t asset_free_indices;
uint32_t asset_free_count;
uint32_t asset_count;
uint32_t next_asset_index;
uint32_t asset_generation_counter;
VkrHashTable_VkrMeshAssetEntry asset_by_key;  // Hash table for key lookup
```

### Asset API

```c
VkrMeshAssetHandle vkr_mesh_manager_acquire_asset(
    VkrMeshManager *manager, String8 path,
    VkrPipelineDomain domain, String8 shader_override,
    VkrRendererError *out_error);

void vkr_mesh_manager_release_asset(VkrMeshManager *manager,
                                    VkrMeshAssetHandle handle);

VkrMeshAsset *vkr_mesh_manager_get_asset(VkrMeshManager *manager,
                                         VkrMeshAssetHandle handle);
```

`vkr_mesh_manager_acquire_asset` performs a synchronous load via the resource
system, registers the asset, then unloads the mesh resource to release loader
memory. Successful acquisitions increment `ref_count`.
Handle lookups use slot+generation for O(1) access; `asset_by_key` only handles
deduplication by `mesh_path|domain|shader_override`.

### Material Reference Counting Fix

**Critical Implementation Detail:** When creating assets from loaded mesh data,
materials must have their reference count incremented to keep them alive after
the loader unloads:

```c
// Add reference to keep material alive after loader unload
if (owns_material && material.id != 0) {
  vkr_material_system_add_ref(manager->material_system, material);
}
```

Without this, materials would be released when the mesh loader's temporary
resources were freed, causing white/black meshes with missing textures.

---

## Phase 3: Mesh Instances

**Files Modified:**
- `lib/src/renderer/resources/vkr_resources.h`
- `lib/src/renderer/systems/vkr_mesh_manager.h`
- `lib/src/renderer/systems/vkr_mesh_manager.c`
- `lib/src/renderer/systems/vkr_scene_system.h`
- `lib/src/renderer/systems/vkr_scene_system.c`
- `lib/src/renderer/resources/loaders/scene_loader.c`

### New Types in vkr_resources.h

```c
typedef struct VkrMeshInstanceHandle {
  uint32_t id;
  uint32_t generation;
} VkrMeshInstanceHandle;

typedef struct VkrMeshSubmeshInstanceState {
  VkrPipelineHandle pipeline;
  VkrRendererInstanceStateHandle instance_state;
  bool8_t pipeline_dirty;
  uint64_t last_render_frame;
} VkrMeshSubmeshInstanceState;
Array(VkrMeshSubmeshInstanceState);

typedef struct VkrMeshInstance {
  VkrMeshAssetHandle asset;
  Mat4 model;
  uint32_t render_id;
  bool8_t visible;
  VkrMeshLoadingState loading_state;

  bool8_t bounds_valid;
  Vec3 bounds_world_center;
  float32_t bounds_world_radius;

  Array_VkrMeshSubmeshInstanceState submesh_state;
} VkrMeshInstance;
Array(VkrMeshInstance);
```

### Instance API

```c
VkrMeshInstanceHandle vkr_mesh_manager_create_instance(
    VkrMeshManager *manager, VkrMeshAssetHandle asset,
    Mat4 model, uint32_t render_id, bool8_t visible,
    VkrRendererError *out_error);

uint32_t vkr_mesh_manager_create_instances_batch(
    VkrMeshManager *manager, const VkrMeshLoadDesc *descs,
    uint32_t count, VkrMeshInstanceHandle *out_handles,
    VkrRendererError *out_error);

bool8_t vkr_mesh_manager_destroy_instance(VkrMeshManager *manager,
                                          VkrMeshInstanceHandle handle);

VkrMeshInstance *vkr_mesh_manager_get_instance(VkrMeshManager *manager,
                                               VkrMeshInstanceHandle handle);

VkrMeshInstance *vkr_mesh_manager_get_instance_by_index(VkrMeshManager *manager,
                                                        uint32_t index);

VkrMeshInstance *vkr_mesh_manager_get_instance_by_live_index(
    VkrMeshManager *manager, uint32_t live_index, uint32_t *out_slot);

uint32_t vkr_mesh_manager_instance_count(const VkrMeshManager *manager);

uint32_t vkr_mesh_manager_instance_capacity(VkrMeshManager *manager);
```

### Handle Indexing

**Important:** Instance handles are 1-indexed (slot 0 = id 1). When constructing
handles from iteration indices:

```c
VkrMeshInstanceHandle handle = {
    .id = instance_index + 1,  // Handles are 1-indexed
    .generation = inst->generation
};
```

### Instance Memory

Per-instance `submesh_state` arrays are allocated from `instance_dmemory` via
`instance_allocator` so they can be freed per instance without growing arena
allocators.

### Live Index Iteration

`mesh_count` and `instance_count` track live entries only. Iteration uses
`vkr_mesh_manager_get_mesh_by_live_index` and
`vkr_mesh_manager_get_instance_by_live_index` with
`mesh_live_indices`/`instance_live_indices` to avoid scanning freed slots.

### Scene Component Update

Changed `SceneMeshRenderer` to store instance handle instead of mesh index:

```c
typedef struct SceneMeshRenderer {
  VkrMeshInstanceHandle instance;  // Was: uint32_t mesh_index
} SceneMeshRenderer;
```

### Pipeline Refresh Optimization

Added early-return check in `vkr_mesh_manager_instance_refresh_pipeline()` to
prevent descriptor set allocation every frame:

```c
bool8_t requires_update =
    state->pipeline_dirty || state->pipeline.id != desired_pipeline.id ||
    state->pipeline.generation != desired_pipeline.generation;
if (!requires_update) {
  return true_v;  // No update needed
}
```

Without this optimization, descriptor sets were being allocated every frame,
causing "Failed to allocate descriptor set" errors.

---

## Phase 4: Pass/Render Integration

**Files Modified:**
- `lib/src/renderer/vkr_draw_batch.h`
- `lib/src/renderer/passes/vkr_pass_world.c`

### VkrDrawCommand Extension

Added `is_instance` flag to distinguish between legacy meshes and instances:

```c
typedef struct VkrDrawCommand {
  VkrDrawKey key;
  uint32_t mesh_index;
  uint32_t submesh_index;
  Mat4 model;
  uint32_t object_id;
  float32_t camera_distance;
  bool8_t is_instance;  // When true, mesh_index refers to instance slot
} VkrDrawCommand;
```

### Instance Bind/Render Functions

Added helper functions in `vkr_pass_world.c`:

```c
vkr_internal VkrViewWorldDrawRange vkr_pass_world_resolve_instance_draw_range(
    RendererFrontend *rf, const VkrMeshAssetSubmesh *submesh, bool8_t allow_opaque);

vkr_internal bool8_t vkr_pass_world_bind_instance_submesh(
    RendererFrontend *rf, VkrViewWorldState *state, uint32_t instance_index,
    uint32_t submesh_index, VkrPipelineDomain domain,
    VkrPipelineHandle *globals_pipeline, VkrMeshAssetSubmesh **out_asset_submesh,
    VkrMeshSubmeshInstanceState **out_instance_state);

vkr_internal void vkr_pass_world_render_instance_submesh(
    RendererFrontend *rf, VkrViewWorldState *state, uint32_t instance_index,
    uint32_t submesh_index, VkrPipelineDomain domain, uint32_t instance_count,
    uint32_t first_instance, VkrPipelineHandle *globals_pipeline);
```

### Instance Iteration in World Pass

Added instance iteration loop in `vkr_pass_world_execute()`:

```c
// Instance iteration
uint32_t instance_count = vkr_mesh_manager_instance_count(&rf->mesh_manager);
for (uint32_t inst_i = 0; inst_i < instance_count; ++inst_i) {
  uint32_t instance_slot = 0;
  VkrMeshInstance *inst = vkr_mesh_manager_get_instance_by_live_index(
      &rf->mesh_manager, inst_i, &instance_slot);
  if (!inst->visible || inst->loading_state != VKR_MESH_LOADING_STATE_LOADED)
    continue;

  VkrMeshAsset *asset = vkr_mesh_manager_get_asset(&rf->mesh_manager, inst->asset);
  if (!asset) continue;

  // Frustum culling using instance bounds
  if (inst->bounds_valid && !vkr_frustum_test_sphere(&frustum, ...))
    continue;

  // Iterate asset submeshes and add draw commands with is_instance = true_v
  for (uint32_t s = 0; s < asset->submeshes.length; ++s) {
    VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
    // ... build draw command with range_id = use_mdi ? 0 : submesh->range_id
    cmd.is_instance = true_v;
    vkr_draw_batcher_add_opaque(&state->draw_batcher, &cmd);
  }
}
```

Legacy meshes iterate via `vkr_mesh_manager_count` and
`vkr_mesh_manager_get_mesh_by_live_index` to avoid scanning freed slots.

### Batch Key Range ID

**Critical Detail:** Instance draw commands can use `range_id = 0` when `use_mdi`
is enabled, because the indirect path emits per-command ranges. When `use_mdi`
is false (or MDI is unavailable), use the actual `submesh->range_id` and draw
per command if batching merged multiple ranges. The original code had:

```c
uint32_t range_id = use_mdi ? 0 : submesh->range_id;
```

Using `range_id = 0` without an MDI fallback caused all submeshes to be batched
together incorrectly, resulting in only parts of meshes rendering.

### Render Loop Updates

The batch rendering loop checks `cmd->is_instance` and calls the appropriate
bind/render function:

```c
if (cmd->is_instance) {
  vkr_pass_world_render_instance_submesh(rf, state, cmd->mesh_index, ...);
} else {
  vkr_pass_world_render_submesh(rf, state, cmd->mesh_index, ...);
}
```

---

## Phase 5: Picking System Integration

**Files Modified:**
- `lib/src/renderer/systems/vkr_picking_system.c`

### Transparent Entry Extension

Added `is_instance` flag for transparent picking entries:

```c
typedef struct VkrPickingTransparentSubmeshEntry {
  uint32_t mesh_index;
  uint32_t submesh_index;
  float32_t distance;
  bool8_t is_instance;  // True if mesh_index refers to instance slot
} VkrPickingTransparentSubmeshEntry;
```

### Instance Picking Helper

Added helper function for rendering instance submeshes during picking:

```c
vkr_internal bool8_t picking_render_instance_submesh(
    RendererFrontend *rf, VkrInstanceBufferPool *instance_pool,
    VkrMeshInstance *instance, VkrMeshAssetSubmesh *submesh,
    VkrTextureOpaqueHandle fallback_texture, bool8_t can_alpha_test,
    uint32_t *out_first_instance);
```

### Instance Iteration in Picking

Added instance iteration in `vkr_picking_render()` after the legacy mesh loop:

```c
// Instance iteration
uint32_t instance_count = vkr_mesh_manager_instance_count(mesh_manager);
for (uint32_t inst_idx = 0; inst_idx < instance_count; inst_idx++) {
  uint32_t instance_slot = 0;
  VkrMeshInstance *inst = vkr_mesh_manager_get_instance_by_live_index(
      mesh_manager, inst_idx, &instance_slot);
  if (!inst->visible || inst->loading_state != VKR_MESH_LOADING_STATE_LOADED)
    continue;

  VkrMeshAsset *asset = vkr_mesh_manager_get_asset(mesh_manager, inst->asset);
  if (!asset) continue;

  for (uint32_t submesh_idx = 0; submesh_idx < asset->submeshes.length; submesh_idx++) {
    VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[submesh_idx];
    // ... check for cutout materials, add to transparent queue or render directly
    // Use picking_render_instance_submesh() for rendering
  }
}
```

### Transparent Loop Update

The transparent rendering loop handles both legacy and instance entries:

```c
for (uint32_t t = 0; t < transparent_count; ++t) {
  VkrPickingTransparentSubmeshEntry *entry = &transparent_entries[t];

  if (entry->is_instance) {
    // Instance path - get instance and asset
    VkrMeshInstance *inst = vkr_mesh_manager_get_instance_by_index(...);
    VkrMeshAsset *asset = vkr_mesh_manager_get_asset(...);
    VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[entry->submesh_index];
    picking_render_instance_submesh(...);
  } else {
    // Legacy mesh path
    VkrMesh *mesh = vkr_mesh_manager_get(...);
    VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(...);
    picking_render_submesh(...);
  }
}
```

---

## Phase 6: Shape (Legacy Mesh) Picking Fix

**Files Modified:**
- `lib/src/renderer/systems/vkr_scene_system.h`
- `lib/src/renderer/systems/vkr_scene_system.c`

### Problem

Shapes (procedural geometry like cubes) use legacy meshes via `SceneShape.mesh_index`
but were not pickable because:

1. No `SceneRenderId` component was assigned to shape entities
2. No render_id was set on the legacy mesh
3. No transform sync occurred for shapes
4. Shapes weren't being marked dirty when transforms changed

### Solution

#### 1. Setup in vkr_scene_set_shape()

Added render_id assignment and initial mesh setup:

```c
// Assign render_id to entity
uint32_t render_id = 0;
if (!vkr_scene_ensure_render_id(scene, entity, &render_id)) {
  log_warn("Scene: failed to assign render id for shape entity");
}

// Set up mesh for picking and visibility
bool8_t is_visible = scene_entity_is_visible(scene, entity);
vkr_mesh_manager_set_render_id(&rf->mesh_manager, mesh_index, render_id);
vkr_mesh_manager_set_visible(&rf->mesh_manager, mesh_index, is_visible);

// Set model matrix from transform
if (transform) {
  vkr_mesh_manager_set_model(&rf->mesh_manager, mesh_index, transform->world);
}
```

#### 2. Query for Shapes

Added `query_shapes` to `VkrScene`:

```c
VkrQueryCompiled query_shapes;  // (SceneTransform, SceneShape, SceneRenderId)
```

Initialized in `scene_compile_queries()`:

```c
VkrComponentTypeId shape_types[3] = {
    scene->comp_transform,
    scene->comp_shape,
    scene->comp_render_id,
};
VkrQuery q_shapes;
vkr_entity_query_build(scene->world, shape_types, 3, NULL, 0, &q_shapes);
vkr_entity_query_compile(scene->world, &q_shapes, scene->alloc, &scene->query_shapes);
```

#### 3. Shape Sync Callback

Added `render_sync_shape_cb()` for full sync:

```c
vkr_internal void render_sync_shape_cb(const VkrArchetype *arch,
                                       VkrChunk *chunk, void *user) {
  // ... get transforms, shapes, render_ids from chunk
  for (uint32_t i = 0; i < count; i++) {
    uint32_t mesh_index = shapes[i].mesh_index;
    if (mesh_index == VKR_INVALID_ID) continue;

    uint32_t render_id = render_ids[i].id;
    bool8_t is_visible = scene_entity_is_visible(scene, entities[i]);

    // Sync legacy mesh state
    vkr_mesh_manager_set_model(&rf->mesh_manager, mesh_index, transforms[i].world);
    vkr_mesh_manager_set_visible(&rf->mesh_manager, mesh_index, is_visible);
    vkr_mesh_manager_set_render_id(&rf->mesh_manager, mesh_index, render_id);

    // Update picking mapping
    scene_render_bridge_update_mapping(ctx->bridge, render_id, entities[i], is_visible);
  }
}
```

Called in `scene_render_bridge_full_sync()`:

```c
vkr_entity_query_compiled_each_chunk(&scene->query_shapes, render_sync_shape_cb, &ctx);
```

#### 4. Mark Shapes as Dirty

Updated `scene_mark_render_dirty()` to include shapes:

```c
bool8_t has_renderable =
    vkr_entity_has_component(scene->world, entity, scene->comp_mesh_renderer) ||
    vkr_entity_has_component(scene->world, entity, scene->comp_shape);
if (!has_renderable) {
  return;
}
```

#### 5. Incremental Sync for Shapes

Updated `scene_render_bridge_sync()` to handle shapes in dirty entity loop:

```c
// Try mesh renderer (instance) first
const SceneMeshRenderer *mesh_renderer = ...;
if (mesh_renderer) {
  scene_sync_renderable(&ctx, entity, mesh_renderer->instance, ...);
  continue;
}

// Try shape (legacy mesh) next
const SceneShape *shape = ...;
if (shape && shape->mesh_index != VKR_INVALID_ID) {
  vkr_mesh_manager_set_model(&rf->mesh_manager, shape->mesh_index, transform->world);
  vkr_mesh_manager_set_visible(&rf->mesh_manager, shape->mesh_index, is_visible);
  vkr_mesh_manager_set_render_id(&rf->mesh_manager, shape->mesh_index, render_id);
  scene_render_bridge_update_mapping(ctx.bridge, render_id, entity, is_visible);
}
```

---

## Phase 7: Shadow System Integration

**Files Modified:**
- `lib/src/renderer/passes/vkr_pass_shadow.c`

### Problem

Shadows only worked for legacy meshes (shapes), not for mesh instances (loaded models).

### Solution

#### 1. Unified Submesh Info Struct

Created a struct to unify access to both legacy and instance submesh data:

```c
typedef struct VkrShadowSubmeshInfo {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t opaque_first_index;
  uint32_t opaque_index_count;
  int32_t opaque_vertex_offset;
  uint32_t range_id;
  bool8_t valid;
} VkrShadowSubmeshInfo;
```

#### 2. Helper to Get Submesh Info

```c
vkr_internal VkrShadowSubmeshInfo
vkr_pass_shadow_get_submesh_info(RendererFrontend *rf, const VkrDrawCommand *cmd) {
  VkrShadowSubmeshInfo info = {0};

  if (cmd->is_instance) {
    VkrMeshInstance *inst = vkr_mesh_manager_get_instance_by_index(...);
    VkrMeshAsset *asset = vkr_mesh_manager_get_asset(...);
    VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[cmd->submesh_index];
    // Copy fields to info
    info.valid = true_v;
  } else {
    const VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(...);
    // Copy fields to info
    info.valid = true_v;
  }
  return info;
}
```

#### 3. Resolve Draw Range Helper

```c
vkr_internal VkrViewShadowDrawRange vkr_pass_shadow_resolve_draw_range_info(
    RendererFrontend *rf, const VkrShadowSubmeshInfo *info, bool8_t allow_opaque) {
  // Same logic as vkr_pass_shadow_resolve_draw_range but uses info struct
}
```

#### 4. Instance Iteration in Shadow Rendering

Added instance iteration loop after legacy mesh loop:

```c
// Instance iteration
uint32_t instance_count = vkr_mesh_manager_instance_count(&rf->mesh_manager);
for (uint32_t i = 0; i < instance_count; ++i) {
  uint32_t instance_slot = 0;
  VkrMeshInstance *inst = vkr_mesh_manager_get_instance_by_live_index(
      &rf->mesh_manager, i, &instance_slot);
  if (!inst->visible || inst->loading_state != VKR_MESH_LOADING_STATE_LOADED)
    continue;

  // Frustum culling against shadow frustum
  if (inst->bounds_valid && !vkr_frustum_test_sphere(&shadow_frustum, ...))
    continue;

  VkrMeshAsset *asset = vkr_mesh_manager_get_asset(&rf->mesh_manager, inst->asset);
  if (!asset) continue;

  for (uint32_t s = 0; s < asset->submeshes.length; ++s) {
    VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
    // ... build draw command
    cmd.is_instance = true_v;
    cmd.key.range_id = use_mdi ? 0 : submesh->range_id;
    vkr_draw_batcher_add_opaque(&state->draw_batcher, &cmd);
  }
}
```

#### 5. Updated Batch Processing

All batch processing code was updated to use the unified helpers:

```c
// Get submesh info using helper
VkrShadowSubmeshInfo submesh_info = vkr_pass_shadow_get_submesh_info(rf, cmd);
if (!submesh_info.valid) continue;

// Resolve draw range using info
VkrViewShadowDrawRange batch_range =
    vkr_pass_shadow_resolve_draw_range_info(rf, &submesh_info, !needs_alpha_test);

// Use submesh_info.geometry, submesh_info.material, etc.
```

---

## Key Lessons Learned

### 1. Array Field Names

Arrays use `.length`, not `.count`:
```c
// Correct
asset->submeshes.length

// Wrong
asset->submeshes.count
```

### 2. Handle Indexing

Handles are 1-indexed (id = slot_index + 1):
```c
// Creating handle from iteration index
handle.id = instance_index + 1;

// Getting slot from handle
uint32_t slot = handle.id - 1;
```

### 3. Pipeline Refresh Optimization

Always check if update is needed before acquiring new descriptor sets:
```c
bool8_t requires_update = state->pipeline_dirty ||
    state->pipeline.id != desired_pipeline.id ||
    state->pipeline.generation != desired_pipeline.generation;
if (!requires_update) return true_v;
```

### 4. Material Reference Counting

Materials must be ref-counted when stored in assets to survive loader cleanup:
```c
if (owns_material && material.id != 0) {
  vkr_material_system_add_ref(manager->material_system, material);
}
```

### 5. Batch Key Range ID

When MDI is enabled, both legacy and instance draw commands may set
`range_id = 0` to maximize batching. When MDI is disabled or unavailable,
use the actual `range_id` and fall back to per-command draws if necessary.
```c
uint32_t range_id = use_mdi ? 0 : submesh->range_id;
```

### 6. Dirty Marking for All Renderables

When marking entities dirty for sync, check all renderable component types:
```c
bool8_t has_renderable =
    vkr_entity_has_component(scene->world, entity, scene->comp_mesh_renderer) ||
    vkr_entity_has_component(scene->world, entity, scene->comp_shape);
```

### 7. Live Index Iteration

Iterate with `vkr_mesh_manager_count`/`vkr_mesh_manager_instance_count` and
`vkr_mesh_manager_get_mesh_by_live_index`/
`vkr_mesh_manager_get_instance_by_live_index` to avoid scanning freed slots.

### 8. Alignment for Submesh Arrays

`VkrMeshAssetSubmesh` requires 16-byte alignment; generic arrays now allocate
aligned. For manual arrays, use `vkr_allocator_alloc_aligned` and free with
`vkr_allocator_free_aligned`.

---

## Acceptance Criteria (Verified)

### Functional

- [x] Loading a scene with `N` entities referencing the same mesh file results in:
  - `mesh_loader_calls == 1` (per unique `MeshAssetKey`)
  - `instances_created == N`
  - No Out-Of-Memory failures from mesh loader arena pool

### Correctness

- [x] Destroying/unloading the scene releases all instances
- [x] No double-free or missing release of geometry/material handles
- [x] No misaligned access when reading/writing `VkrMeshAssetSubmesh` arrays
- [x] Picking works for both instances and legacy meshes (shapes)
- [x] Shadows work for both instances and legacy meshes (shapes)
- [x] Transform gizmo correctly moves shapes (legacy meshes)
- [x] Materials persist correctly after loader cleanup

### Integration

- [x] World view iterates instances correctly
- [x] Picking system handles both legacy and instance paths
- [x] Shadow system handles both legacy and instance paths
- [x] Scene system syncs both mesh instances and shapes

---

## Files Modified Summary

| File | Changes |
|------|---------|
| `lib/src/containers/array.h` | Aligned array allocations to satisfy submesh alignment |
| `lib/src/renderer/renderer_frontend.c` | Mesh manager configured for 100000 max meshes |
| `lib/src/renderer/resources/vkr_resources.h` | Added VkrMeshAsset*, VkrMeshInstance* types |
| `lib/src/renderer/systems/vkr_mesh_manager.h` | Extended VkrMeshManager, declared new API |
| `lib/src/renderer/systems/vkr_mesh_manager.c` | Core implementation: dedup, asset registry, instances |
| `lib/src/renderer/vkr_draw_batch.h` | Added `is_instance` flag to VkrDrawCommand |
| `lib/src/renderer/passes/vkr_pass_world.c` | Instance bind/render functions, instance iteration |
| `lib/src/renderer/passes/vkr_pass_shadow.c` | Unified submesh helpers, instance iteration |
| `lib/src/renderer/systems/vkr_picking_system.c` | Instance picking support, transparent entry update |
| `lib/src/renderer/systems/vkr_scene_system.h` | Added query_shapes, updated SceneMeshRenderer |
| `lib/src/renderer/systems/vkr_scene_system.c` | Shape sync, dirty marking, incremental sync |
| `lib/src/renderer/resources/loaders/scene_loader.c` | Uses create_instances_batch API |
