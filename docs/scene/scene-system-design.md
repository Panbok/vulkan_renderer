---
status: partial
updated: 2026-09-04
authority: design
---
# Scene System Design (ECS + Renderer Integration)

**Note:** The view/layer system has been removed. Render orchestration now uses
the render graph and stateless packets; pass executors consume packet payloads.
The scene system ships, but this document retains historical “world view” and
planned-API material that has not been fully reconciled with current code.

## Purpose

Define a **Scene** abstraction for this engine that:

- Uses the existing ECS (`VkrWorld`) for entity/component data.
- Integrates with the current renderer architecture where the **World view renders from `RendererFrontend.mesh_manager`**, but the scene itself does not directly "own" or manipulate meshes.
- Supports editor workflows (hierarchy, selection/picking, and future serialization).

This document is written to be **LLM-consumable**: explicit file paths, data flow, and an implementation checklist.

---

## Table of Contents

1. [Current State (What Exists Today)](#1-current-state-what-exists-today)
2. [Goals and Non-goals](#2-goals-and-non-goals)
3. [Core Concept: `VkrScene`](#3-core-concept-vkrscene)
4. [Data Model](#4-data-model)
5. [Update and Rendering Flow](#5-update-and-rendering-flow)
6. [Picking and Editor Selection](#6-picking-and-editor-selection)
7. [Scene Serialization Format](#7-scene-serialization-format)
8. [Threading and Lifetime Rules](#8-threading-and-lifetime-rules)
9. [Proposed API Surface](#9-proposed-api-surface)
10. [Implementation Plan (Phased)](#10-implementation-plan-phased)
11. [File Changes Summary](#11-file-changes-summary)
12. [Open Questions / Follow-ups](#12-open-questions--follow-ups)
13. [Design Review: Issues and Solutions](#13-design-review-issues-and-solutions)

---

## Glossary

- **Scene**: A high-level container representing “what exists in the world” (entities, hierarchy, and renderable instances).
- **ECS / World**: Existing archetype-based storage (`lib/src/core/vkr_entity.h`) implemented as `VkrWorld`.
- **Renderable**: An entity that produces draw calls (currently via `VkrMeshManager` meshes/submeshes).
- **Mesh index**: The `uint32_t` index into `VkrMeshManager` slots (renderer-owned implementation detail).
- **Render ID**: A persistent per-entity 32-bit ID used for picking and editor selection (future-proof across renderer slot reuse).

---

## 1. Current State (What Exists Today)

### 1.1 Rendering source of truth is `VkrMeshManager`

- World rendering iterates the mesh manager:
  - `lib/src/renderer/passes/vkr_pass_world.c` → `vkr_pass_world_execute()`
  - Uses `mesh->model` for drawing and frustum culling.
- Frame entry point:
  - `lib/src/renderer/renderer_frontend.c` → `vkr_renderer_draw_frame()`
  - Executes the render graph (`vkr_rg_execute`) and optionally runs
    `vkr_picking_render()`.

### 1.2 Picking identifies objects by render id

- Picking renders `object_id = render_id + 1`:
  - `lib/src/renderer/systems/vkr_picking_system.c` → `.object_id = mesh->render_id + 1`
- The sample runtime maps picking results to entities via the scene handle:
  - `runtime/src/vkr_sample_runtime.c` uses
    `vkr_scene_handle_entity_from_picking_id()`.

### 1.3 ECS exists and is used by the scene system

- `lib/src/core/vkr_entity.h/.c` provides:
  - Stable `VkrEntityId` = `[world:16 | generation:16 | index:32]`
  - Archetype/chunk storage + a chunk query API.
- The scene system owns a `VkrWorld` and syncs render intent into the mesh manager.

### 1.4 JSON utilities exist (useful for scenes)

- `lib/src/core/vkr_json.h/.c` provides a lightweight JSON “field finder”.
- Important behavior: `vkr_json_parse_string()` returns a **view into the source buffer**, not an owned string.

---

## 2. Goals and Non-goals

### 2.1 Goals

1. **Entity-first authoring**:
   - Stable entity IDs and hierarchy (for editor and future gameplay logic).
2. **Renderer-compatible**:
   - Preserve the current render path that reads `RendererFrontend.mesh_manager`.
3. **Selection-ready**:
   - Convert picking results into entity IDs (not raw mesh indices), via a persistent per-entity Render ID.
4. **Serialization-ready**:
   - Define an on-disk scene format that can recreate the world deterministically.
5. **PBR-friendly structure**:
   - Keep materials/textures as explicit per-entity data so adding PBR later is mostly a matter of extending `VkrMaterial` and adding shader/pipeline variants.

### 2.2 Non-goals (initial version)

- Physics, animation, audio, navmesh, scripting.
- Live hot-reload of scenes (we can add later).
- Multi-scene rendering in a single frame (one active scene is enough initially).
- Picking non-mesh entities (UI, 3D text, lights) is explicitly deferred.

---

## 3. Core Concept: `VkrScene`

### 3.1 High-level idea

Introduce a Scene module that becomes the **ownership layer** for:

- ECS entities/components (`VkrWorld`), and
- a small set of “render intent” components consumed by renderer systems.

The renderer continues to draw the mesh manager as it does today; a renderer-side “scene bridge” is responsible for syncing renderer state from ECS component state.

### 3.2 One active scene (initial constraint)

To minimize renderer churn, start with:

- A **single active scene** per `RendererFrontend`.
- That scene’s ECS world is the authoritative source of “what exists”.

Later, this can evolve into per-scene mesh managers (or a renderable registry with scene tags), but that is explicitly out of scope for the first implementation.

---

## 4. Data Model

### 4.1 New runtime type

Proposed file: `lib/src/renderer/systems/vkr_scene_system.h`

```c
typedef struct VkrScene {
  VkrWorld *world;           // ECS storage (authoritative scene state)
  VkrAllocator *alloc;       // Scene-owned allocator for strings/temp data
  uint16_t world_id;         // Copied into entity IDs

  // Component type IDs (cached after registration)
  VkrComponentTypeId comp_name;
  VkrComponentTypeId comp_transform;
  VkrComponentTypeId comp_mesh_renderer;
  VkrComponentTypeId comp_visibility;

  // Compiled queries for efficient per-frame iteration (avoid recomputing each frame)
  VkrQueryCompiled query_transforms;       // All entities with SceneTransform
  VkrQueryCompiled query_renderables;      // (SceneTransform, SceneMeshRenderer)

  // Transform hierarchy support
  uint32_t *topo_order;      // Topologically sorted entity indices for hierarchy traversal
  uint32_t topo_count;       // Number of entities in topo_order
  uint32_t topo_capacity;    // Allocated size
  bool8_t hierarchy_dirty;   // Set when parent links change; triggers topo rebuild

  // Owned mesh indices (for cleanup on scene destroy)
  uint32_t *owned_meshes;
  uint32_t owned_mesh_count;
  uint32_t owned_mesh_capacity;

  // Render dirty tracking (entities needing sync to mesh manager)
  VkrEntityId *render_dirty_entities;
  uint32_t render_dirty_count;
  uint32_t render_dirty_capacity;
  bool8_t render_full_sync_needed;  // Set on scene load or dirty overflow
} VkrScene;
```

Renderer integration is handled by a separate module which consumes `scene->world`:

In the current implementation this is **not exposed as a standalone public module**.
Instead we expose a single runtime handle that owns both the ECS scene state and
the renderer sync state:

- `VkrSceneHandle` (opaque) is the runtime scene instance returned by the
  resource system for `VKR_RESOURCE_TYPE_SCENE`.
- `vkr_scene_handle_*()` APIs provide update, renderer sync, and picking lookup.

The internal bridge keeps renderer-owned mappings such as render-id →
`VkrEntityId` for picking.

### 4.2 Components (initial set)

These are ECS components registered in the scene’s `VkrWorld`.

#### `SceneName`

```c
typedef struct SceneName {
  String8 name; // stored in scene-owned allocator (not a JSON view)
} SceneName;
```

#### `SceneTransform`

Keep transform data ECS-safe (no parent pointers). Prefer TRS + cached matrices.

```c
typedef struct SceneTransform {
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;

  VkrEntityId parent;   // VKR_ENTITY_ID_INVALID means root
  Mat4 local;           // Cached local matrix (TRS composition)
  Mat4 world;           // Cached world matrix (parent.world * local)

  uint8_t flags;        // Bitmask: DIRTY_LOCAL | DIRTY_WORLD | DIRTY_HIERARCHY
} SceneTransform;

// Flag definitions
#define SCENE_TRANSFORM_DIRTY_LOCAL     0x01  // Local TRS changed, recompute local matrix
#define SCENE_TRANSFORM_DIRTY_WORLD     0x02  // World matrix needs recompute (parent changed or local changed)
#define SCENE_TRANSFORM_DIRTY_HIERARCHY 0x04  // Parent link changed, rebuild topo order
```

**Dirty flag semantics:**
- `DIRTY_LOCAL`: Set by transform setters. Cleared after `local` matrix is recomputed.
- `DIRTY_WORLD`: Set when `DIRTY_LOCAL` is set or when parent's world changes. Cleared after `world` matrix is recomputed.
- `DIRTY_HIERARCHY`: Set when `parent` changes. Triggers `scene->hierarchy_dirty = true` for topo rebuild. Cleared after topo rebuild.

#### `SceneMeshRenderer`

This is the entity’s “intent” to be rendered as a mesh. The renderer side owns the actual mesh slots/resources.

```c
typedef struct SceneMeshRenderer {
  uint32_t mesh_index;  // renderer-owned slot; managed by the render bridge
} SceneMeshRenderer;
```

#### `SceneVisibility` (v1 - required)

Visibility is fundamental for rendering. Include in v1.

```c
typedef struct SceneVisibility {
  bool8_t visible;          // If false, entity (and children) are not rendered
  bool8_t inherit_parent;   // If true, effective visibility = parent.visible && this.visible
} SceneVisibility;
```

**Default behavior:** If entity lacks `SceneVisibility` component, treat as visible.

#### `SceneRenderId` (picking)

Persistent per-entity ID for picking. This should not depend on mesh indices.

```c
typedef struct SceneRenderId {
  uint32_t id; // stable for entity lifetime
} SceneRenderId;
```

Notes:

- Assigned once per entity and never reused (monotonic allocator).
- Required for pickable entities (meshes today).

Optional (for later, but compatible with the initial model):

- `SceneCamera` (active camera entity, editor cameras)
- `SceneLight` (directional/point lights)

---

## 5. Update and Rendering Flow

### 5.1 Frame-level responsibilities

The scene update is split into responsibilities owned by different modules:

1. **Scene transform update** (`VkrScene`): compute `SceneTransform.world` for all dirty transforms (including hierarchy propagation).
2. **Render bridge sync** (renderer system): update renderer state (mesh manager transforms, visibility, picking IDs) for entities with render components.

### 5.2 Transform evaluation algorithm

**Challenge:** The ECS is archetype-based with SoA storage. Entities are stored by component signature, NOT by hierarchy. Parent-before-child ordering requires explicit topological sorting.

**Solution:** Maintain a topologically sorted order of transform entities. Rebuild when hierarchy changes.

#### 5.2.1 Topological Sort Algorithm (Kahn's Algorithm)

```c
// Called when scene->hierarchy_dirty is true
void vkr_scene_rebuild_topo_order(VkrScene *scene) {
  // 1. Count entities with SceneTransform
  uint32_t count = 0;
  vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
    count_entities_cb, &count);

  // 2. Allocate/resize topo arrays
  ensure_topo_capacity(scene, count);

  // 3. Build adjacency: for each entity, compute in-degree (number of children pointing to it as parent)
  // and collect entities with in-degree 0 (roots or orphaned parents)
  uint32_t *in_degree = scratch_alloc(count * sizeof(uint32_t));
  VkrEntityId *queue = scratch_alloc(count * sizeof(VkrEntityId));
  uint32_t queue_head = 0, queue_tail = 0;

  // First pass: compute in-degree for each entity
  // in_degree[i] = 0 means entity i has no valid parent (root)
  for_each_transform_entity(scene, entity, transform) {
    if (!vkr_entity_is_alive(scene->world, transform->parent)) {
      queue[queue_tail++] = entity;  // Root entity
    }
  }

  // 4. BFS: process roots, then their children
  scene->topo_count = 0;
  uint8_t *visited = scratch_calloc(max_entity_index, 1);

  while (queue_head < queue_tail) {
    VkrEntityId entity = queue[queue_head++];
    uint32_t idx = entity.parts.index;

    if (visited[idx]) continue;  // Cycle detected - skip
    visited[idx] = 1;

    scene->topo_order[scene->topo_count++] = idx;

    // Find all children of this entity and add to queue
    for_each_transform_entity(scene, child, child_transform) {
      if (child_transform->parent.u64 == entity.u64 && !visited[child.parts.index]) {
        queue[queue_tail++] = child;
      }
    }
  }

  // 5. Detect cycles: any unvisited entity with SceneTransform is in a cycle
  for_each_transform_entity(scene, entity, transform) {
    if (!visited[entity.parts.index]) {
      VKR_LOG_WARN("Cycle detected in transform hierarchy for entity %u, treating as root",
                   entity.parts.index);
      scene->topo_order[scene->topo_count++] = entity.parts.index;
    }
  }

  scene->hierarchy_dirty = false;
}
```

#### 5.2.2 Per-Frame Transform Update

```c
void vkr_scene_update_transforms(VkrScene *scene) {
  if (scene->hierarchy_dirty) {
    vkr_scene_rebuild_topo_order(scene);
  }

  // Process in topological order (parents before children)
  for (uint32_t i = 0; i < scene->topo_count; i++) {
    uint32_t entity_idx = scene->topo_order[i];
    VkrEntityId entity = vkr_entity_from_index(scene->world, entity_idx);

    SceneTransform *transform = vkr_entity_get(scene->world, entity, scene->comp_transform);
    if (!transform) continue;

    // Recompute local matrix if TRS changed
    if (transform->flags & SCENE_TRANSFORM_DIRTY_LOCAL) {
      transform->local = mat4_trs(transform->position, transform->rotation, transform->scale);
      transform->flags &= ~SCENE_TRANSFORM_DIRTY_LOCAL;
      transform->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
    }

    // Recompute world matrix
    if (transform->flags & SCENE_TRANSFORM_DIRTY_WORLD) {
      SceneTransform *parent_transform = NULL;
      if (vkr_entity_is_alive(scene->world, transform->parent)) {
        parent_transform = vkr_entity_get(scene->world, transform->parent, scene->comp_transform);
      }

      if (parent_transform) {
        transform->world = mat4_mul(parent_transform->world, transform->local);
      } else {
        transform->world = transform->local;
      }

      transform->flags &= ~SCENE_TRANSFORM_DIRTY_WORLD;

      // Mark this entity as needing render bridge sync
      vkr_scene_mark_render_dirty(scene, entity);
    }
  }
}
```

#### 5.2.3 Edge Cases

- **Parent missing/dead**: Treat entity as root (`world = local`).
- **Cycle in parent links**: Detected during topo rebuild. Cyclic entities are appended at end and treated as roots. Warning logged once per cycle.
- **Parent link change mid-frame**: Sets `DIRTY_HIERARCHY` flag → `scene->hierarchy_dirty = true` → topo rebuild next update.

### 5.3 Render sync

Currently, `VkrMeshManager` owns `VkrMesh.model` and frustum culling uses it:

- `lib/src/renderer/passes/vkr_pass_world.c` reads `mesh->model` and `mesh->bounds_world_*`.

To allow ECS-driven transforms without relying on `VkrMesh.transform` parenting pointers, the render bridge should call a mesh-manager helper:

```c
// New API proposal
void vkr_mesh_manager_set_model(VkrMeshManager *manager,
                                uint32_t mesh_index,
                                Mat4 model);
```

Behavior:

- Writes `mesh->model = model`
- Updates world bounds (`vkr_mesh_update_world_bounds(mesh)`)
- Resets `submesh->last_render_frame = 0` for correctness with instance caching

#### 5.3.1 Optimized Dirty-Only Sync

**Problem:** Iterating all renderable entities every frame is wasteful when most transforms don't change.

**Solution:** Track dirty entities and only sync those.

```c
void vkr_scene_handle_sync(VkrSceneHandle handle, struct s_RendererFrontend *rf) {
  VkrScene *scene = vkr_scene_handle_get_scene(handle);
  if (!scene) return;

  // Only process entities marked dirty during transform update.
  for (uint32_t i = 0; i < scene->render_dirty_count; i++) {
    VkrEntityId entity = scene->render_dirty_entities[i];
    // Fetch transform/mesh/visibility/render-id components.
    // Update mesh manager visibility/model and update the internal
    // render-id→entity mapping used by picking.
  }

  scene->render_dirty_count = 0;
}
```

**Full sync fallback:** On scene load or when dirty list overflows, perform a full sync of all renderables.

### 5.4 Who calls `vkr_scene_update`

Initial wiring (minimal invasive):

- `runtime/src/vkr_sample_runtime.c` calls
  `vkr_scene_handle_update_and_sync(scene_handle, &renderer, dt)` once per
  frame, before packet submission.

Later wiring option:

- Add `RendererFrontend.active_scene` and call `vkr_scene_update()` from `vkr_renderer_draw_frame()` (centralizes ordering).

---

## 6. Picking and Editor Selection

### 6.1 Required mapping

Current behavior (render-id picking):

- `object_id = render_id + 1`

The render bridge provides a lookup table:

```c
VkrEntityId vkr_scene_handle_entity_from_picking_id(VkrSceneHandle handle,
                                                    uint32_t object_id);
```

**Picking ID encoding:**
To support non-mesh picks, `object_id` is tagged with a 2-bit kind in the
high bits. Scene render IDs use kind `0`, which preserves `render_id + 1`
semantics. UI text and world text use their slot indices with kind tags.
This limits render IDs to 30 bits to keep encodings unambiguous.

### 6.2 Persistent per-entity Render ID

Picking uses `SceneRenderId.id`, and the render bridge maintains a
`render_id_to_entity` table during sync.

Important constraint:

- Today only mesh objects are written into the picking buffer. UI, 3D text, and lights are not pickable. Adding them requires extending the picking pass across the relevant views/pipelines and deciding per-domain ID encoding; this is deferred.

### 6.3 Specific edge case: async readback vs entity destruction

Picking readback completes asynchronously. If a renderable is destroyed before the readback completes, the `render_id` may no longer map to a live entity. The render bridge should return `VKR_ENTITY_ID_INVALID` in that case, and callers should treat it as no hit.

---

## 7. Scene Serialization Format

### 7.1 File format choice

Use JSON for v1:

- Easy to debug and compatible with `lib/src/core/vkr_json.h`.
- Recommended extension: `assets/scenes/<name>.scene.json`

### 7.2 Proposed schema (v1)

```json
{
  "version": 1,
  "entities": [
    {
      "name": "Sponza",
      "parent": null,
      "transform": {
        "pos": [0.0, 0.0, -15.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [0.0085, 0.0085, 0.0085]
      },
      "mesh": {
        "path": "assets/models/sponza.obj",
        "pipeline_domain": "world"
      }
    }
  ]
}
```

Notes:

- `pipeline_domain` maps to `VkrPipelineDomain` (e.g. `"world"`, `"ui"`, `"shadow"`, `"post"`).
- If a future PBR material model is introduced, a `"material"` object can be added per entity without changing the core scene wiring.

### 7.3 Parsing constraints (important)

Because `vkr_json_parse_string()` returns a view into the original JSON buffer:

- The loader must **copy** any strings that need to outlive the parse buffer (names, mesh paths).
- If escape sequences must be interpreted, a dedicated unescape helper is required (the JSON reader does not unescape).

---

## 8. Threading and Lifetime Rules

### 8.1 Threading

- Scene update runs on the main thread.
- Scene loading can call `vkr_mesh_manager_load_batch()`:
  - Internally uses job system for file I/O and decoding, but performs GPU uploads on the main thread (consistent with `docs/parallel-asset-loading.md`).

### 8.2 Ownership and shutdown

- Scene owns entity/component data (`VkrWorld`) and the set of meshes it spawned.
- On scene destroy:
  - Destroy ECS world (`vkr_entity_destroy_world()`).
  - Remove spawned meshes from `rf->mesh_manager` (or keep an owned list of mesh indices to remove).

---

## 9. Proposed API Surface

Suggested module name: `VkrSceneSystem` (even if v1 only supports one scene).

### 9.1 Scene Lifecycle

```c
// lib/src/renderer/systems/vkr_scene_system.h

typedef enum VkrSceneError {
  VKR_SCENE_ERROR_NONE = 0,
  VKR_SCENE_ERROR_ALLOC_FAILED,
  VKR_SCENE_ERROR_WORLD_INIT_FAILED,
  VKR_SCENE_ERROR_COMPONENT_REGISTRATION_FAILED,
  VKR_SCENE_ERROR_ENTITY_LIMIT_REACHED,
  VKR_SCENE_ERROR_INVALID_ENTITY,
  VKR_SCENE_ERROR_MESH_LOAD_FAILED,
} VkrSceneError;

bool8_t vkr_scene_init(VkrScene *scene,
                       VkrAllocator *alloc,
                       uint16_t world_id,
                       uint32_t initial_entity_capacity,
                       VkrSceneError *out_error);

void vkr_scene_shutdown(VkrScene *scene);

void vkr_scene_update(VkrScene *scene, float64_t dt);
```

### 9.2 Entity Management

```c
// Create entity with optional initial components
// Returns VKR_ENTITY_ID_INVALID on failure (check out_error)
VkrEntityId vkr_scene_create_entity(VkrScene *scene, VkrSceneError *out_error);

// Destroy entity and remove from scene
void vkr_scene_destroy_entity(VkrScene *scene, VkrEntityId entity);

// Check if entity exists and is alive
bool8_t vkr_scene_entity_alive(const VkrScene *scene, VkrEntityId entity);
```

### 9.3 Component Helpers (convenience wrappers)

```c
// Add SceneName to entity (copies string into scene allocator)
bool8_t vkr_scene_set_name(VkrScene *scene, VkrEntityId entity, String8 name);
String8 vkr_scene_get_name(const VkrScene *scene, VkrEntityId entity);

// Add/get SceneTransform
bool8_t vkr_scene_set_transform(VkrScene *scene, VkrEntityId entity,
                                 Vec3 position, VkrQuat rotation, Vec3 scale);
SceneTransform *vkr_scene_get_transform(VkrScene *scene, VkrEntityId entity);

// Transform setters that auto-mark dirty
void vkr_scene_set_position(VkrScene *scene, VkrEntityId entity, Vec3 position);
void vkr_scene_set_rotation(VkrScene *scene, VkrEntityId entity, VkrQuat rotation);
void vkr_scene_set_scale(VkrScene *scene, VkrEntityId entity, Vec3 scale);
void vkr_scene_set_parent(VkrScene *scene, VkrEntityId entity, VkrEntityId parent);

// Add SceneMeshRenderer (links entity to mesh_index from mesh manager)
bool8_t vkr_scene_set_mesh_renderer(VkrScene *scene, VkrEntityId entity,
                                     uint32_t mesh_index);

// Add SceneVisibility
void vkr_scene_set_visibility(VkrScene *scene, VkrEntityId entity,
                               bool8_t visible, bool8_t inherit_parent);
```

### 9.4 Mesh Ownership

```c
// Spawn a mesh via mesh manager and track ownership (scene cleans up on destroy)
bool8_t vkr_scene_spawn_mesh(VkrScene *scene,
                              struct s_RendererFrontend *rf,
                              const VkrMeshLoadDesc *desc,
                              uint32_t *out_mesh_index,
                              VkrSceneError *out_error);

// Release a mesh from scene ownership (scene won't destroy it on shutdown)
void vkr_scene_release_mesh(VkrScene *scene, uint32_t mesh_index);
```

### 9.5 Renderer Integration API

```c
// lib/src/renderer/systems/vkr_scene_system.h

// Runtime handle returned by `VkrResourceSystem` for VKR_RESOURCE_TYPE_SCENE.
VkrSceneHandle vkr_scene_handle_create(VkrAllocator *alloc, uint16_t world_id,
                                       uint32_t initial_entity_capacity,
                                       uint32_t initial_picking_capacity,
                                       VkrSceneError *out_error);
void vkr_scene_handle_destroy(VkrSceneHandle handle,
                              struct s_RendererFrontend *rf);
VkrScene *vkr_scene_handle_get_scene(VkrSceneHandle handle);

// Per-frame integration.
void vkr_scene_handle_update_and_sync(VkrSceneHandle handle,
                                      struct s_RendererFrontend *rf,
                                      float64_t dt);

// After load (or if incremental sync is insufficient).
void vkr_scene_handle_full_sync(VkrSceneHandle handle,
                                struct s_RendererFrontend *rf);

// Picking mapping.
VkrEntityId vkr_scene_handle_entity_from_picking_id(VkrSceneHandle handle,
                                                    uint32_t object_id);
```

---

## 10. Implementation Plan (Phased)

### Phase 1 (In scope): Scene owns ECS; renderer consumes ECS (no file I/O)

**Step 1.1:** Create scene system header/source

- Add `lib/src/renderer/systems/vkr_scene_system.h/.c`
- Define component types: `SceneName`, `SceneTransform`, `SceneMeshRenderer`, `SceneVisibility`
- Define `VkrScene` struct with compiled queries and topo order arrays
- Define `VkrSceneError` enum

**Step 1.2:** Implement scene lifecycle

- `vkr_scene_init()`: Create ECS world, register components, build initial queries
- `vkr_scene_shutdown()`: Destroy owned meshes, destroy queries, destroy ECS world
- `vkr_scene_update()`: Update transforms (topo sort if needed), mark dirty renderables

**Step 1.3:** Implement entity/component management

- `vkr_scene_create_entity()`, `vkr_scene_destroy_entity()`
- Component setters with auto dirty-marking: `vkr_scene_set_position()`, etc.
- `vkr_scene_set_parent()` sets `DIRTY_HIERARCHY` flag

**Step 1.4:** Implement transform hierarchy

- `vkr_scene_rebuild_topo_order()` using Kahn's algorithm with cycle detection
- `vkr_scene_update_transforms()` processes in topo order, recomputes local/world matrices

**Step 1.5:** Add mesh manager helper

- Implement `vkr_mesh_manager_set_model()` in `lib/src/renderer/systems/vkr_mesh_manager.h/.c`
- Updates `mesh->model`, world bounds, resets instance cache

**Step 1.6:** Implement renderer sync (internal) + runtime handle API

- Internal bridge syncs ECS state into the mesh manager and maintains the
  render-id → entity mapping used by picking.
- Public API is `VkrSceneHandle` + `vkr_scene_handle_*()`; the bridge is not
  exposed to callers.

**Step 1.7:** Wire in `runtime/src/vkr_sample_runtime.c` via the resource system

- Load the scene with `vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, ...)`
- Call `vkr_scene_handle_update_and_sync()` once per frame
- Use `vkr_scene_handle_entity_from_picking_id()` for picking results

### Phase 2: Scene loading/unloading via `VkrResourceSystem`

1. Add `VKR_RESOURCE_TYPE_SCENE` to `lib/src/renderer/systems/vkr_resource_system.h`.
2. Register `scene_loader` as a `VkrResourceLoader` from `lib/src/renderer/renderer_frontend.c`.
3. Implement `load()` to:
   - Parse `.scene.json` via `vkr_json_*`
   - Create a `VkrSceneHandle` and populate the ECS world
   - Batch load meshes via the mesh manager and track ownership for unload
4. Implement `unload()` to destroy the `VkrSceneHandle` (removes owned meshes + ECS world).

### Phase 3 (In scope): Editor and picking improvements

1. Introduce persistent per-entity `SceneRenderId` and write it in picking (mesh objects first).
2. Avoid renderer-slot reuse hazards during async readback by using persistent render ids (no reuse).
3. Extend picking beyond meshes (UI, text, lights).

---

## 11. File Changes Summary

**New (Phase 1)**

- `lib/src/renderer/systems/vkr_scene_system.h` - Scene types, components, API declarations
- `lib/src/renderer/systems/vkr_scene_system.c` - Scene implementation including:
  - Component type definitions (`SceneName`, `SceneTransform`, `SceneMeshRenderer`, `SceneVisibility`)
  - Scene lifecycle (`vkr_scene_init`, `vkr_scene_shutdown`, `vkr_scene_update`)
  - Entity management (`vkr_scene_create_entity`, `vkr_scene_destroy_entity`)
  - Component setters with auto-dirty marking
  - Transform hierarchy (topo sort, world matrix computation)
  - Internal renderer sync bridge + `VkrSceneHandle` runtime API

**New (Phase 2)**

- `lib/src/renderer/resources/loaders/scene_loader.h`
- `lib/src/renderer/resources/loaders/scene_loader.c`
- `lib/src/renderer/systems/vkr_resource_system.h` - add `VKR_RESOURCE_TYPE_SCENE`
- `lib/src/renderer/resources/vkr_resources.h` - add `VkrSceneHandle`

**Update (Phase 1)**

- `lib/src/renderer/systems/vkr_mesh_manager.h` - Add `vkr_mesh_manager_set_model()` declaration
- `lib/src/renderer/systems/vkr_mesh_manager.c` - Implement `vkr_mesh_manager_set_model()`:
  - Set `mesh->model`
  - Update world bounds
  - Reset instance cache (`last_render_frame = 0`)
- `runtime/src/vkr_sample_runtime.c` - Wire scene system:
  - Load/unload scene via `vkr_resource_system_load()` / `vkr_resource_system_unload()`
  - Tick scene via `vkr_scene_handle_update_and_sync()`
  - Map picking results via `vkr_scene_handle_entity_from_picking_id()`

---

## 12. Open Questions / Follow-ups

Resolved / Direction:

1. **Stable picking IDs**: implemented via persistent per-entity Render IDs; extending picking beyond meshes is deferred.
2. **Scene↔renderer boundary**: the scene should not directly work with meshes; it should expose intent via ECS, and renderer systems should consume ECS. This is in scope for v1.
3. **Instancing**: yes, `SceneMeshRenderer` should eventually evolve into an instancing-friendly representation; deferred.
4. **PBR migration**: extend `VkrMaterial`; deferred.

---

## 13. Design Review: Issues and Solutions

This section documents issues identified during design review and their solutions (now incorporated into the design above).

### 13.1 Hierarchy Traversal in Archetype-Based ECS

**Issue:** The original design said "maintain a per-frame traversal that computes world matrices in parent-before-child order" without specifying how. The ECS stores entities by component signature (archetype), NOT by hierarchy. Entities with identical components may have different parents scattered across chunks.

**Solution:** Explicit topological sort using Kahn's algorithm (Section 5.2.1). Rebuild only when `hierarchy_dirty` flag is set (parent links changed). Store sorted entity indices in `scene->topo_order`.

### 13.2 Transform Dirty Flag Semantics

**Issue:** Original design had a single `dirty` flag without specifying when it's cleared or how changes propagate to children.

**Solution:** Three-flag system (Section 4.2, SceneTransform):
- `DIRTY_LOCAL`: TRS changed → recompute local matrix
- `DIRTY_WORLD`: Local or parent changed → recompute world matrix
- `DIRTY_HIERARCHY`: Parent link changed → rebuild topo order

Flags are cleared after the corresponding computation completes.

### 13.3 Missing Compiled Queries

**Issue:** Original design didn't mention ECS compiled queries. The ECS supports `VkrQueryCompiled` for efficient repeated iteration—critical for per-frame systems to avoid rebuilding query each frame.

**Solution:** Cache compiled queries in `VkrScene` struct (Section 4.1): `query_transforms` and `query_renderables`. Rebuild queries only when component registrations change (never during normal operation).

### 13.4 No Render Bridge Sync Optimization

**Issue:** Original design would iterate ALL renderable entities every frame to sync transforms to mesh manager, even when most transforms don't change.

**Solution:** Dirty tracking (Section 5.3.1). Only entities whose world matrix changed are synced. `vkr_scene_mark_render_dirty()` called from transform update adds entities to dirty list. Full sync fallback for scene load or overflow.

### 13.5 SceneVisibility Missing from v1

**Issue:** Visibility was marked "optional for later" but it's fundamental for rendering. Mesh rendering already respects visibility.

**Solution:** Added `SceneVisibility` as a v1 component (Section 4.2). Includes `inherit_parent` flag for hierarchy-based visibility.

### 13.6 Missing Component Registration

**Issue:** Original API showed `vkr_scene_init()` but didn't mention that components need to be registered with the ECS before use.

**Solution:** `vkr_scene_init()` now explicitly registers all scene components and caches their type IDs in the `VkrScene` struct (Section 4.1).

### 13.7 No Error Handling in API

**Issue:** Original `vkr_scene_create_entity()` returned entity ID with no way to report allocation failure.

**Solution:** Added `VkrSceneError` enum and `out_error` parameters to fallible APIs (Section 9.1, 9.2). Functions return `VKR_ENTITY_ID_INVALID` or `false` on failure.

### 13.8 Entity Creation API Too Specific

**Issue:** Original `vkr_scene_create_entity(scene, xform_opt, name_opt)` hardcoded specific component parameters, limiting flexibility.

**Solution:** Simplified to `vkr_scene_create_entity(scene, out_error)` which creates a bare entity. Use separate setter functions to add components (Section 9.2, 9.3). More flexible and follows ECS philosophy.

### 13.9 Missing Mesh Ownership

**Issue:** Original design said "load meshes via existing renderer APIs" but scene should track which meshes it spawned for cleanup on shutdown.

**Solution:** Added `owned_meshes` array to `VkrScene` struct (Section 4.1) and `vkr_scene_spawn_mesh()` / `vkr_scene_release_mesh()` APIs (Section 9.4). Scene destroys owned meshes on shutdown.

### 13.10 Transform Setters Missing Auto-Dirty

**Issue:** No API to set transform properties (position/rotation/scale) and automatically mark dirty. Users would need to manually set flags.

**Solution:** Added setter functions that auto-mark dirty (Section 9.3): `vkr_scene_set_position()`, `vkr_scene_set_rotation()`, `vkr_scene_set_scale()`, `vkr_scene_set_parent()`. Parent setter additionally sets `DIRTY_HIERARCHY`.

### 13.11 Cycle Detection Algorithm Vague

**Issue:** Original design said "detect with a small per-update stack/mark array" without concrete algorithm.

**Solution:** Explicit cycle detection in Kahn's algorithm (Section 5.2.1). After BFS completes, any unvisited entity with `SceneTransform` is part of a cycle. These entities are appended to topo order and treated as roots. Warning logged once per frame for cycles.

### 13.12 Simplified Architecture (Merged vs Separate Bridge)

**Issue:** Original design proposed separate `VkrScene` and `VkrSceneRenderBridge` modules, but for v1 this adds unnecessary complexity.

**Solution:** The renderer sync bridge is internal to the scene runtime handle (`VkrSceneHandle`). Callers use `vkr_scene_handle_*()` for update/sync/picking without having to initialize or manage a separate bridge object.
