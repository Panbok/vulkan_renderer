/**
 * @file vkr_scene_system.c
 * @brief Scene system implementation.
 */

#include "vkr_scene_system.h"

#include "core/logger.h"
#include "math/vkr_math.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/world/vkr_text_3d.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_picking_system.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_world_resources.h"

// ============================================================================
// Internal Constants
// ============================================================================

#define SCENE_DEFAULT_ENTITY_CAPACITY 1024
#define SCENE_DEFAULT_DIRTY_CAPACITY 256
#define SCENE_DEFAULT_MESH_CAPACITY 64
#define SCENE_DEFAULT_INSTANCE_CAPACITY 64

// ============================================================================
// Internal Types
// ============================================================================

/**
 * @brief Slot in the parent -> children index.
 *
 * Keyed by parent entity index. `parent_id` is a generation guard to prevent
 * mixing entries when entity indices are reused by the ECS.
 */
typedef struct SceneChildIndexSlot {
  VkrEntityId parent_id;
  VkrEntityId *children;
  uint32_t child_count;
  uint32_t child_capacity;
} SceneChildIndexSlot;

/**
 * @brief Internal render bridge for syncing scene state to the renderer.
 *
 * Edge case: entries are cleared to VKR_ENTITY_ID_INVALID when an entity
 * becomes invisible to prevent stale picking mappings.
 */
typedef struct VkrSceneRenderBridge {
  VkrAllocator *alloc;
  VkrEntityId *render_id_to_entity;
  uint32_t render_id_capacity;
} VkrSceneRenderBridge;

struct VkrSceneRuntime {
  VkrScene scene;
  VkrSceneRenderBridge bridge;

  // Per-scene arena for ECS/entity allocations. Destroyed with the scene to
  // reclaim all memory in bulk (arena frees are no-ops during scene lifetime).
  Arena *scene_arena;
  VkrAllocator scene_allocator;

  // Parent allocator used to allocate this runtime struct itself.
  VkrAllocator *parent_alloc;
};

vkr_internal bool8_t scene_render_bridge_init(VkrSceneRenderBridge *bridge,
                                              VkrAllocator *alloc,
                                              uint32_t initial_capacity);
vkr_internal void scene_render_bridge_shutdown(VkrSceneRenderBridge *bridge);
vkr_internal void scene_render_bridge_sync(VkrSceneRenderBridge *bridge,
                                           struct s_RendererFrontend *rf,
                                           VkrScene *scene);
vkr_internal void scene_render_bridge_full_sync(VkrSceneRenderBridge *bridge,
                                                struct s_RendererFrontend *rf,
                                                VkrScene *scene);
vkr_internal VkrEntityId scene_render_bridge_entity_from_picking_id(
    const VkrSceneRenderBridge *bridge, uint32_t object_id);

// ============================================================================
// Internal Helpers
// ============================================================================

vkr_internal uint32_t scene_next_capacity(uint32_t current, uint32_t needed,
                                          uint32_t default_capacity) {
  uint32_t capacity;

  // Handle initial capacity: if current is 0, use default_capacity (or 1 if
  // that's also 0)
  if (current == 0) {
    capacity = (default_capacity > 0) ? default_capacity : 1;
  } else {
    // Check if doubling current would overflow
    if (current > UINT32_MAX / 2) {
      // Already at or near max, return max or needed (whichever is smaller)
      return (needed <= UINT32_MAX) ? needed : UINT32_MAX;
    }
    capacity = current * 2;
  }

  // Double capacity until we meet the need, checking for overflow each time
  while (capacity < needed) {
    // Check if doubling would overflow
    if (capacity > UINT32_MAX / 2) {
      // Clamp to needed if it fits, otherwise max out
      capacity = (needed <= UINT32_MAX) ? needed : UINT32_MAX;
      break;
    }
    capacity *= 2;
  }

  return capacity;
}

vkr_internal bool8_t scene_grow_array(VkrAllocator *alloc, void **array,
                                      uint32_t *capacity, uint32_t count,
                                      uint32_t needed,
                                      uint32_t default_capacity,
                                      size_t element_size, size_t align,
                                      bool8_t zero_new) {
  if (needed <= *capacity) {
    return true_v;
  }

  uint32_t new_capacity =
      scene_next_capacity(*capacity, needed, default_capacity);
  void *new_array =
      vkr_allocator_alloc_aligned(alloc, new_capacity * element_size, align,
                                  VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!new_array) {
    return false_v;
  }

  if (zero_new) {
    MemZero(new_array, new_capacity * element_size);
  }

  if (*array && count > 0) {
    MemCopy(new_array, *array, count * element_size);
  }
  if (*array) {
    vkr_allocator_free_aligned(alloc, *array, (*capacity) * element_size, align,
                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  *array = new_array;
  *capacity = new_capacity;
  return true_v;
}

vkr_internal void scene_invalidate_queries(VkrScene *scene) {
  if (!scene) {
    return;
  }
  scene->queries_valid = false_v;
  scene->child_index_valid = false_v;
}

vkr_internal bool8_t scene_child_index_ensure_capacity(VkrScene *scene,
                                                       uint32_t needed_slots) {
  if (!scene || !scene->alloc) {
    return false_v;
  }
  if (needed_slots == 0) {
    return true_v;
  }

  uint32_t count = scene->child_index_capacity;
  return scene_grow_array(scene->alloc, (void **)&scene->child_index_slots,
                          &scene->child_index_capacity, count, needed_slots,
                          SCENE_DEFAULT_ENTITY_CAPACITY,
                          sizeof(SceneChildIndexSlot),
                          AlignOf(SceneChildIndexSlot), true_v);
}

vkr_internal SceneChildIndexSlot *
scene_child_index_get_slot(VkrScene *scene, VkrEntityId parent) {
  if (!scene || !scene->world) {
    return NULL;
  }
  if (parent.u64 == VKR_ENTITY_ID_INVALID.u64) {
    return NULL;
  }

  if (!scene_child_index_ensure_capacity(scene, scene->world->dir.capacity)) {
    return NULL;
  }
  if (parent.parts.index >= scene->child_index_capacity) {
    return NULL;
  }
  return &scene->child_index_slots[parent.parts.index];
}

vkr_internal void scene_child_index_slot_reset(SceneChildIndexSlot *slot,
                                               VkrEntityId parent) {
  if (!slot) {
    return;
  }
  if (slot->parent_id.u64 != parent.u64) {
    slot->parent_id = parent;
    slot->child_count = 0;
  }
}

vkr_internal bool8_t scene_child_index_slot_contains(
    const SceneChildIndexSlot *slot, VkrEntityId child) {
  if (!slot || !slot->children) {
    return false_v;
  }
  for (uint32_t i = 0; i < slot->child_count; i++) {
    if (slot->children[i].u64 == child.u64) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal bool8_t scene_child_index_add(VkrScene *scene, VkrEntityId parent,
                                           VkrEntityId child) {
  if (!scene || !scene->alloc) {
    return false_v;
  }
  if (parent.u64 == VKR_ENTITY_ID_INVALID.u64 ||
      child.u64 == VKR_ENTITY_ID_INVALID.u64) {
    return false_v;
  }

  SceneChildIndexSlot *slot = scene_child_index_get_slot(scene, parent);
  if (!slot) {
    return false_v;
  }
  scene_child_index_slot_reset(slot, parent);

  if (scene_child_index_slot_contains(slot, child)) {
    return true_v;
  }

  if (!scene_grow_array(scene->alloc, (void **)&slot->children,
                        &slot->child_capacity, slot->child_count,
                        slot->child_count + 1, 4, sizeof(VkrEntityId),
                        AlignOf(VkrEntityId), false_v)) {
    return false_v;
  }

  slot->children[slot->child_count++] = child;
  return true_v;
}

vkr_internal void scene_child_index_remove(VkrScene *scene, VkrEntityId parent,
                                           VkrEntityId child) {
  if (!scene || parent.u64 == VKR_ENTITY_ID_INVALID.u64 ||
      child.u64 == VKR_ENTITY_ID_INVALID.u64 || !scene->child_index_slots) {
    return;
  }
  if (parent.parts.index >= scene->child_index_capacity) {
    return;
  }

  SceneChildIndexSlot *slot = &scene->child_index_slots[parent.parts.index];
  if (slot->parent_id.u64 != parent.u64 || slot->child_count == 0 ||
      !slot->children) {
    return;
  }

  for (uint32_t i = 0; i < slot->child_count; i++) {
    if (slot->children[i].u64 == child.u64) {
      slot->children[i] = slot->children[slot->child_count - 1];
      slot->child_count--;
      return;
    }
  }
}

vkr_internal void scene_child_index_clear_parent_slot(VkrScene *scene,
                                                      VkrEntityId parent) {
  if (!scene || parent.u64 == VKR_ENTITY_ID_INVALID.u64 ||
      !scene->child_index_slots) {
    return;
  }
  if (parent.parts.index >= scene->child_index_capacity) {
    return;
  }
  SceneChildIndexSlot *slot = &scene->child_index_slots[parent.parts.index];
  if (slot->parent_id.u64 == parent.u64) {
    slot->parent_id = VKR_ENTITY_ID_INVALID;
    slot->child_count = 0;
  }
}

vkr_internal void scene_child_index_reset_all(VkrScene *scene) {
  if (!scene || !scene->child_index_slots) {
    return;
  }
  for (uint32_t i = 0; i < scene->child_index_capacity; i++) {
    scene->child_index_slots[i].parent_id = VKR_ENTITY_ID_INVALID;
    scene->child_index_slots[i].child_count = 0;
  }
}

typedef struct SceneChildIndexBuildCtx {
  VkrScene *scene;
} SceneChildIndexBuildCtx;

vkr_internal void scene_child_index_build_cb(const VkrArchetype *arch,
                                             VkrChunk *chunk, void *user) {
  (void)arch;
  SceneChildIndexBuildCtx *ctx = (SceneChildIndexBuildCtx *)user;
  VkrScene *scene = ctx->scene;
  VkrWorld *world = scene->world;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);

  for (uint32_t i = 0; i < count; i++) {
    VkrEntityId parent = transforms[i].parent;
    if (parent.u64 == VKR_ENTITY_ID_INVALID.u64) {
      continue;
    }
    // Use inline fast path for parent validation
    if (!vkr_entity_is_alive(world, parent)) {
      continue;
    }
    scene_child_index_add(scene, parent, entities[i]);
  }
}

vkr_internal bool8_t scene_child_index_rebuild(VkrScene *scene) {
  if (!scene || !scene->world || !scene->queries_valid) {
    return false_v;
  }

  if (!scene_child_index_ensure_capacity(scene, scene->world->dir.capacity)) {
    return false_v;
  }

  scene_child_index_reset_all(scene);

  SceneChildIndexBuildCtx ctx = {.scene = scene};
  vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
                                       scene_child_index_build_cb, &ctx);
  scene->child_index_valid = true_v;
  return true_v;
}

vkr_internal bool8_t scene_child_index_ensure_built(VkrScene *scene) {
  if (!scene || !scene->queries_valid) {
    return false_v;
  }
  if (scene->child_index_valid) {
    return true_v;
  }
  return scene_child_index_rebuild(scene);
}

vkr_internal void scene_child_index_shutdown(VkrScene *scene) {
  if (!scene || !scene->alloc) {
    return;
  }
  if (scene->child_index_slots) {
    for (uint32_t i = 0; i < scene->child_index_capacity; i++) {
      SceneChildIndexSlot *slot = &scene->child_index_slots[i];
      if (slot->children) {
        vkr_allocator_free_aligned(scene->alloc, slot->children,
                                   slot->child_capacity * sizeof(VkrEntityId),
                                   AlignOf(VkrEntityId),
                                   VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        slot->children = NULL;
        slot->child_capacity = 0;
        slot->child_count = 0;
      }
    }

    vkr_allocator_free_aligned(
        scene->alloc, scene->child_index_slots,
        scene->child_index_capacity * sizeof(SceneChildIndexSlot),
        AlignOf(SceneChildIndexSlot), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    scene->child_index_slots = NULL;
  }
  scene->child_index_capacity = 0;
  scene->child_index_valid = false_v;
}

/**
 * @brief Ensure topo_order array has sufficient capacity.
 */
vkr_internal bool8_t scene_ensure_topo_capacity(VkrScene *scene,
                                                uint32_t needed) {
  return scene_grow_array(scene->alloc, (void **)&scene->topo_order,
                          &scene->topo_capacity, scene->topo_count, needed, 64,
                          sizeof(VkrEntityId), AlignOf(VkrEntityId), false_v);
}

/**
 * @brief Ensure render dirty list has sufficient capacity.
 */
vkr_internal bool8_t scene_ensure_dirty_capacity(VkrScene *scene,
                                                 uint32_t needed) {
  return scene_grow_array(scene->alloc, (void **)&scene->render_dirty_entities,
                          &scene->render_dirty_capacity,
                          scene->render_dirty_count, needed,
                          SCENE_DEFAULT_DIRTY_CAPACITY, sizeof(VkrEntityId),
                          AlignOf(VkrEntityId), false_v);
}

/**
 * @brief Ensure owned meshes array has sufficient capacity.
 */
vkr_internal bool8_t scene_ensure_mesh_capacity(VkrScene *scene,
                                                uint32_t needed) {
  return scene_grow_array(scene->alloc, (void **)&scene->owned_meshes,
                          &scene->owned_mesh_capacity, scene->owned_mesh_count,
                          needed, SCENE_DEFAULT_MESH_CAPACITY, sizeof(uint32_t),
                          AlignOf(uint32_t), false_v);
}

/**
 * @brief Mark entity as needing render sync.
 * @note Entity is assumed to be already validated alive by caller.
 */
vkr_internal void scene_mark_render_dirty(VkrScene *scene, VkrEntityId entity) {
  // Fast path: check if entity has mesh renderer or shape via archetype lookup
  // Entity already validated in caller (vkr_scene_update), use unchecked access
  VkrEntityRecord rec = scene->world->dir.records[entity.parts.index];
  const VkrArchetype *arch = rec.chunk->arch;

  bool8_t has_renderable =
      (arch->type_to_col[scene->comp_mesh_renderer] !=
       VKR_ENTITY_TYPE_TO_COL_INVALID) ||
      (arch->type_to_col[scene->comp_shape] != VKR_ENTITY_TYPE_TO_COL_INVALID);
  if (!has_renderable) {
    return;
  }

  // Add to dirty list if capacity allows
  if (!scene_ensure_dirty_capacity(scene, scene->render_dirty_count + 1)) {
    // Overflow - trigger full sync next frame
    scene->render_full_sync_needed = true;
    return;
  }

  scene->render_dirty_entities[scene->render_dirty_count++] = entity;
}

/**
 * @brief Mark immediate children of a parent as world-dirty.
 */
typedef struct SceneChildDirtyContext {
  VkrScene *scene;
  VkrEntityId parent;
} SceneChildDirtyContext;

vkr_internal void scene_mark_children_dirty_cb(const VkrArchetype *arch,
                                               VkrChunk *chunk, void *user) {
  (void)arch;
  SceneChildDirtyContext *ctx = (SceneChildDirtyContext *)user;
  VkrScene *scene = ctx->scene;

  uint32_t count = vkr_entity_chunk_count(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);

  for (uint32_t i = 0; i < count; i++) {
    if (transforms[i].parent.u64 == ctx->parent.u64) {
      transforms[i].flags |= SCENE_TRANSFORM_DIRTY_WORLD;
    }
  }
}

vkr_internal void scene_mark_children_world_dirty(VkrScene *scene,
                                                  VkrEntityId parent) {
  if (!scene->queries_valid)
    return;
  if (parent.u64 == VKR_ENTITY_ID_INVALID.u64)
    return;

  if (scene_child_index_ensure_built(scene)) {
    SceneChildIndexSlot *slot = scene_child_index_get_slot(scene, parent);
    if (!slot || slot->parent_id.u64 != parent.u64 || !slot->children ||
        slot->child_count == 0) {
      return;
    }

    VkrWorld *world = scene->world;
    VkrComponentTypeId comp_transform = scene->comp_transform;

    uint32_t i = 0;
    while (i < slot->child_count) {
      VkrEntityId child = slot->children[i];
      // Combined is_alive + has_component + get_component in single call
      SceneTransform *child_t =
          (SceneTransform *)vkr_entity_get_component_if_alive(world, child,
                                                              comp_transform);
      if (!child_t) {
        // Entity dead or no transform - remove from index
        slot->children[i] = slot->children[slot->child_count - 1];
        slot->child_count--;
        continue;
      }
      child_t->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
      i++;
    }
    return;
  }

  // Fallback: query scan (e.g., OOM building the index).
  SceneChildDirtyContext ctx = {
      .scene = scene,
      .parent = parent,
  };
  vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
                                       scene_mark_children_dirty_cb, &ctx);
}

/**
 * @brief Compute local matrix from TRS.
 */
vkr_internal Mat4 scene_compute_local_matrix(Vec3 position, VkrQuat rotation,
                                             Vec3 scale) {
  Mat4 t = mat4_translate(position);
  Mat4 r = vkr_quat_to_mat4(rotation);
  Mat4 s = mat4_scale(scale);
  return mat4_mul(mat4_mul(t, r), s);
}

// ============================================================================
// Two-Pass Transform Update (Phase 3 Optimization)
// ============================================================================

/**
 * @brief Chunk callback for Pass 1: Update dirty local matrices.
 *
 * Iterates all transform chunks and updates local matrices for entities with
 * SCENE_TRANSFORM_DIRTY_LOCAL flag. This is cache-friendly because transforms
 * are stored contiguously in chunks.
 *
 * Also clears WORLD_UPDATED flag from previous frame to prepare for Pass 2's
 * deferred dirty propagation.
 *
 * Local matrix computation has no dependencies on other entities, so chunk
 * iteration order doesn't matter.
 */
vkr_internal void transform_local_update_cb(const VkrArchetype *arch,
                                            VkrChunk *chunk, void *user) {
  VkrScene *scene = (VkrScene *)user;

  uint32_t count = chunk->count;
  if (count == 0)
    return;

  // Direct column access via archetype - no per-entity lookup
  uint16_t transform_col = arch->type_to_col[scene->comp_transform];
  SceneTransform *transforms = (SceneTransform *)chunk->columns[transform_col];

  for (uint32_t i = 0; i < count; i++) {
    SceneTransform *t = &transforms[i];

    // Clear WORLD_UPDATED from previous frame (for deferred dirty propagation)
    t->flags &= ~SCENE_TRANSFORM_WORLD_UPDATED;

    if (t->flags & SCENE_TRANSFORM_DIRTY_LOCAL) {
      t->local = scene_compute_local_matrix(t->position, t->rotation, t->scale);
      t->flags &= ~SCENE_TRANSFORM_DIRTY_LOCAL;
      t->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
    }
  }
}

// ============================================================================
// Topo Sort Context
// ============================================================================

typedef struct TopoSortContext {
  VkrScene *scene;
  uint8_t *visited;   // Scratch: visited[entity_index] = 1 if processed
  VkrEntityId *queue; // Scratch: BFS queue
  uint32_t queue_head;
  uint32_t queue_tail;
  uint32_t max_index; // Max entity index for bounds checking
} TopoSortContext;

/**
 * @brief Chunk callback to count transform entities.
 */
vkr_internal void topo_count_cb(const VkrArchetype *arch, VkrChunk *chunk,
                                void *user) {
  (void)arch;
  uint32_t *count = (uint32_t *)user;
  *count += vkr_entity_chunk_count(chunk);
}

/**
 * @brief Chunk callback to find root entities (no parent or dead parent).
 */
vkr_internal void topo_find_roots_cb(const VkrArchetype *arch, VkrChunk *chunk,
                                     void *user) {
  (void)arch;
  TopoSortContext *ctx = (TopoSortContext *)user;
  VkrScene *scene = ctx->scene;
  VkrWorld *world = scene->world;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);

  for (uint32_t i = 0; i < count; i++) {
    transforms[i].flags &= ~SCENE_TRANSFORM_DIRTY_HIERARCHY;

    VkrEntityId parent = transforms[i].parent;
    // Use inline fast path for parent validation
    bool8_t is_root = (parent.u64 == VKR_ENTITY_ID_INVALID.u64) ||
                      !vkr_entity_is_alive(world, parent);

    if (is_root) {
      ctx->queue[ctx->queue_tail++] = entities[i];
    }
  }
}

/**
 * @brief Chunk callback to find children of a specific parent.
 */
typedef struct FindChildrenCtx {
  VkrScene *scene;
  VkrEntityId parent;
  VkrEntityId *queue;
  uint32_t *queue_tail;
  uint8_t *visited;
} FindChildrenCtx;

vkr_internal void topo_find_children_cb(const VkrArchetype *arch,
                                        VkrChunk *chunk, void *user) {
  (void)arch;
  FindChildrenCtx *ctx = (FindChildrenCtx *)user;
  VkrScene *scene = ctx->scene;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);

  for (uint32_t i = 0; i < count; i++) {
    if (transforms[i].parent.u64 == ctx->parent.u64) {
      uint32_t idx = entities[i].parts.index;
      if (!ctx->visited[idx]) {
        ctx->queue[(*ctx->queue_tail)++] = entities[i];
      }
    }
  }
}

/**
 * @brief Chunk callback to find unvisited entities (cycle detection).
 */
typedef struct FindUnvisitedCtx {
  VkrScene *scene;
  uint8_t *visited;
  VkrEntityId *topo_order;
  uint32_t *topo_count;
} FindUnvisitedCtx;

vkr_internal void topo_find_unvisited_cb(const VkrArchetype *arch,
                                         VkrChunk *chunk, void *user) {
  (void)arch;
  FindUnvisitedCtx *ctx = (FindUnvisitedCtx *)user;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);

  for (uint32_t i = 0; i < count; i++) {
    uint32_t idx = entities[i].parts.index;
    if (!ctx->visited[idx]) {
      log_warn("Cycle detected in transform hierarchy for entity %u, treating "
               "as root",
               idx);
      ctx->topo_order[(*ctx->topo_count)++] = entities[i];
      ctx->visited[idx] = 1;
    }
  }
}

/**
 * @brief Rebuild topological order of transform entities.
 * Uses BFS from roots, handling cycles gracefully.
 */
vkr_internal void scene_rebuild_topo_order(VkrScene *scene) {
  if (!scene->queries_valid)
    return;

  // Count entities
  uint32_t entity_count = 0;
  vkr_entity_query_compiled_each_chunk(&scene->query_transforms, topo_count_cb,
                                       &entity_count);

  if (entity_count == 0) {
    scene->topo_count = 0;
    scene->hierarchy_dirty = false;
    return;
  }

  // Ensure capacity
  if (!scene_ensure_topo_capacity(scene, entity_count)) {
    log_error("Failed to allocate topo order array");
    return;
  }

  // Allocate scratch arrays
  // Use a conservative max_index based on ECS directory capacity
  uint32_t max_index = scene->world->dir.capacity;
  uint32_t visited_size = max_index * sizeof(uint8_t);
  uint32_t aligned_offset =
      vkr_align_up_u32(visited_size, AlignOf(VkrEntityId));
  size_t scratch_size = aligned_offset + entity_count * sizeof(VkrEntityId);
  VkrAllocatorScope scratch_scope = vkr_allocator_begin_scope(scene->alloc);
  bool8_t scratch_scoped = vkr_allocator_scope_is_valid(&scratch_scope);
  uint8_t *scratch = (uint8_t *)vkr_allocator_alloc_aligned(
      scene->alloc, scratch_size, 8, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!scratch) {
    if (scratch_scoped) {
      vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    log_error("Failed to allocate topo sort scratch");
    return;
  }

  uint8_t *visited = scratch;
  VkrEntityId *queue = (VkrEntityId *)(scratch + aligned_offset);
  MemZero(visited, max_index);

  TopoSortContext ctx = {
      .scene = scene,
      .visited = visited,
      .queue = queue,
      .queue_head = 0,
      .queue_tail = 0,
      .max_index = max_index,
  };

  // Find roots (entities with no valid parent)
  vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
                                       topo_find_roots_cb, &ctx);

  bool8_t use_child_index = scene_child_index_ensure_built(scene);

  // BFS traversal
  scene->topo_count = 0;
  while (ctx.queue_head < ctx.queue_tail) {
    VkrEntityId entity = ctx.queue[ctx.queue_head++];
    uint32_t idx = entity.parts.index;

    if (idx >= max_index || visited[idx])
      continue;
    visited[idx] = 1;

    scene->topo_order[scene->topo_count++] = entity;

    // Find children
    if (use_child_index) {
      SceneChildIndexSlot *slot = scene_child_index_get_slot(scene, entity);
      if (slot && slot->parent_id.u64 == entity.u64 && slot->children) {
        uint32_t ci = 0;
        while (ci < slot->child_count) {
          VkrEntityId child = slot->children[ci];
          // Combined is_alive + has_component check via get_component
          if (!vkr_entity_has_component_if_alive(scene->world, child,
                                                 scene->comp_transform)) {
            slot->children[ci] = slot->children[slot->child_count - 1];
            slot->child_count--;
            continue;
          }

          uint32_t child_idx = child.parts.index;
          if (child_idx < max_index && !visited[child_idx]) {
            if (ctx.queue_tail < entity_count) {
              ctx.queue[ctx.queue_tail++] = child;
            } else {
              log_warn("Topo sort queue overflow (entity_count=%u)",
                       entity_count);
            }
          }
          ci++;
        }
      }
    } else {
      FindChildrenCtx child_ctx = {
          .scene = scene,
          .parent = entity,
          .queue = queue,
          .queue_tail = &ctx.queue_tail,
          .visited = visited,
      };
      vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
                                           topo_find_children_cb, &child_ctx);
    }
  }

  // Detect cycles: unvisited entities are in cycles
  FindUnvisitedCtx unvisited_ctx = {
      .scene = scene,
      .visited = visited,
      .topo_order = scene->topo_order,
      .topo_count = &scene->topo_count,
  };
  vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
                                       topo_find_unvisited_cb, &unvisited_ctx);

  if (scratch_scoped) {
    vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    vkr_allocator_free_aligned(scene->alloc, scratch, scratch_size, 8,
                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  scene->hierarchy_dirty = false;
}

/**
 * @brief Compile scene queries.
 */
vkr_internal bool8_t scene_compile_queries(VkrScene *scene) {
  if (scene->queries_valid)
    return true;

  // Build transform query
  VkrQuery q_transforms;
  vkr_entity_query_build(scene->world, &scene->comp_transform, 1, NULL, 0,
                         &q_transforms);

  if (!vkr_entity_query_compile(scene->world, &q_transforms, scene->alloc,
                                &scene->query_transforms)) {
    log_error("Failed to compile transform query");
    return false;
  }

  // Build renderables query (transform + mesh renderer + render id)
  VkrComponentTypeId renderable_types[3] = {
      scene->comp_transform,
      scene->comp_mesh_renderer,
      scene->comp_render_id,
  };
  VkrQuery q_renderables;
  vkr_entity_query_build(scene->world, renderable_types, 3, NULL, 0,
                         &q_renderables);

  if (!vkr_entity_query_compile(scene->world, &q_renderables, scene->alloc,
                                &scene->query_renderables)) {
    log_error("Failed to compile renderables query");
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_transforms);
    return false;
  }

  // Build directional light query
  VkrQuery q_dir_light;
  vkr_entity_query_build(scene->world, &scene->comp_directional_light, 1, NULL,
                         0, &q_dir_light);

  if (!vkr_entity_query_compile(scene->world, &q_dir_light, scene->alloc,
                                &scene->query_directional_light)) {
    log_error("Failed to compile directional light query");
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_transforms);
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_renderables);
    return false;
  }

  // Build point light query (transform + point light)
  VkrComponentTypeId point_light_types[2] = {
      scene->comp_transform,
      scene->comp_point_light,
  };
  VkrQuery q_point_lights;
  vkr_entity_query_build(scene->world, point_light_types, 2, NULL, 0,
                         &q_point_lights);

  if (!vkr_entity_query_compile(scene->world, &q_point_lights, scene->alloc,
                                &scene->query_point_lights)) {
    log_error("Failed to compile point lights query");
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_transforms);
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_renderables);
    vkr_entity_query_compiled_destroy(scene->alloc,
                                      &scene->query_directional_light);
    return false;
  }

  // Build shapes query (transform + shape + render id)
  VkrComponentTypeId shape_types[3] = {
      scene->comp_transform,
      scene->comp_shape,
      scene->comp_render_id,
  };
  VkrQuery q_shapes;
  vkr_entity_query_build(scene->world, shape_types, 3, NULL, 0, &q_shapes);

  if (!vkr_entity_query_compile(scene->world, &q_shapes, scene->alloc,
                                &scene->query_shapes)) {
    log_error("Failed to compile shapes query");
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_transforms);
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_renderables);
    vkr_entity_query_compiled_destroy(scene->alloc,
                                      &scene->query_directional_light);
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_point_lights);
    return false;
  }

  scene->queries_valid = true;
  scene->hierarchy_dirty = true; // Need to rebuild topo order

  scene->child_index_valid = false_v;
  if (!scene_child_index_rebuild(scene)) {
    log_warn("Scene: failed to rebuild parent->children index; falling back to "
             "transform scans");
    scene->child_index_valid = false_v;
  }
  return true;
}

// ============================================================================
// Scene Lifecycle
// ============================================================================

bool8_t vkr_scene_init(VkrScene *scene, VkrAllocator *alloc, uint16_t world_id,
                       uint32_t initial_entity_capacity,
                       VkrSceneError *out_error) {
  if (!scene || !alloc) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false;
  }

  MemZero(scene, sizeof(VkrScene));
  scene->alloc = alloc;
  scene->world_id = world_id;

  // Create ECS world
  // NOTE: scratch_alloc must be NULL or a SEPARATE allocator from alloc.
  // Using the same arena for both causes scope-based "frees" to corrupt
  // permanent allocations like archetypes and chunks.
  VkrWorldCreateInfo world_info = {
      .alloc = alloc,
      .scratch_alloc = NULL, // Don't use scopes - avoids memory corruption
      .world_id = world_id,
      .initial_entities = initial_entity_capacity
                              ? initial_entity_capacity
                              : SCENE_DEFAULT_ENTITY_CAPACITY,
      .initial_components = 16,
      .initial_archetypes = 16,
  };

  scene->world = vkr_entity_create_world(&world_info);
  if (!scene->world) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_WORLD_INIT_FAILED;
    return false;
  }

  // Register components
  scene->comp_name = vkr_entity_register_component_once(
      scene->world, "SceneName", sizeof(SceneName), AlignOf(SceneName));
  scene->comp_transform = vkr_entity_register_component_once(
      scene->world, "SceneTransform", sizeof(SceneTransform),
      AlignOf(SceneTransform));
  scene->comp_mesh_renderer = vkr_entity_register_component_once(
      scene->world, "SceneMeshRenderer", sizeof(SceneMeshRenderer),
      AlignOf(SceneMeshRenderer));
  scene->comp_visibility = vkr_entity_register_component_once(
      scene->world, "SceneVisibility", sizeof(SceneVisibility),
      AlignOf(SceneVisibility));
  scene->comp_render_id = vkr_entity_register_component_once(
      scene->world, "SceneRenderId", sizeof(SceneRenderId),
      AlignOf(SceneRenderId));
  scene->comp_text3d = vkr_entity_register_component_once(
      scene->world, "SceneText3D", sizeof(SceneText3D), AlignOf(SceneText3D));
  scene->comp_shape = vkr_entity_register_component_once(
      scene->world, "SceneShape", sizeof(SceneShape), AlignOf(SceneShape));
  scene->comp_directional_light = vkr_entity_register_component_once(
      scene->world, "SceneDirectionalLight", sizeof(SceneDirectionalLight),
      AlignOf(SceneDirectionalLight));
  scene->comp_point_light = vkr_entity_register_component_once(
      scene->world, "ScenePointLight", sizeof(ScenePointLight),
      AlignOf(ScenePointLight));

  if (scene->comp_name == VKR_COMPONENT_TYPE_INVALID ||
      scene->comp_transform == VKR_COMPONENT_TYPE_INVALID ||
      scene->comp_mesh_renderer == VKR_COMPONENT_TYPE_INVALID ||
      scene->comp_visibility == VKR_COMPONENT_TYPE_INVALID ||
      scene->comp_render_id == VKR_COMPONENT_TYPE_INVALID ||
      scene->comp_text3d == VKR_COMPONENT_TYPE_INVALID ||
      scene->comp_shape == VKR_COMPONENT_TYPE_INVALID ||
      scene->comp_directional_light == VKR_COMPONENT_TYPE_INVALID ||
      scene->comp_point_light == VKR_COMPONENT_TYPE_INVALID) {
    vkr_entity_destroy_world(scene->world);
    if (out_error)
      *out_error = VKR_SCENE_ERROR_COMPONENT_REGISTRATION_FAILED;
    return false;
  }

  // Initialize arrays
  scene->topo_order = NULL;
  scene->topo_count = 0;
  scene->topo_capacity = 0;
  scene->hierarchy_dirty = true;

  scene->owned_meshes = NULL;
  scene->owned_mesh_count = 0;
  scene->owned_mesh_capacity = 0;

  scene->render_dirty_entities = NULL;
  scene->render_dirty_count = 0;
  scene->render_dirty_capacity = 0;
  scene->render_full_sync_needed = true; // Full sync on first frame

  scene_invalidate_queries(scene);
  scene->next_render_id = 1;

  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return true;
}

/**
 * @brief Callback context for destroying text3d entities via layer messages.
 */
typedef struct DestroyText3DContext {
  VkrScene *scene;
  RendererFrontend *rf;
} DestroyText3DContext;

/**
 * @brief Chunk callback to send destroy messages for all text3d entities.
 */
vkr_internal void destroy_text3d_chunk_cb(const VkrArchetype *arch,
                                          VkrChunk *chunk, void *user) {
  (void)arch;
  DestroyText3DContext *ctx = (DestroyText3DContext *)user;
  VkrScene *scene = ctx->scene;
  RendererFrontend *rf = ctx->rf;

  uint32_t count = vkr_entity_chunk_count(chunk);
  SceneText3D *text3d_comps =
      (SceneText3D *)vkr_entity_chunk_column(chunk, scene->comp_text3d);

  if (!text3d_comps)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (rf->world_resources.initialized) {
      vkr_world_resources_text_destroy(rf, &rf->world_resources,
                                       text3d_comps[i].text_index);
    }
  }
}

void vkr_scene_shutdown(VkrScene *scene, struct s_RendererFrontend *rf) {
  if (!scene)
    return;

  // Wait for GPU idle before destroying any resources to avoid freeing
  // descriptor sets and buffers that are still referenced by in-flight frames.
  if (rf) {
    VkrRendererError wait_err = vkr_renderer_wait_idle(rf);
    if (wait_err != VKR_RENDERER_ERROR_NONE) {
      log_warn("Scene shutdown: renderer wait idle failed (%d)", wait_err);
    }

    // Invalidate picking instance states before scene textures are destroyed.
    // This ensures descriptor sets don't reference stale texture handles.
    vkr_picking_invalidate_instance_states(rf, &rf->picking);
  }

  // Send destroy messages for all text3d entities to world resources.
  // Must happen before ECS world destruction since we need to query components.
  if (rf && scene->rf && scene->world) {
    // Build text3d query
    VkrQuery q_text3d;
    vkr_entity_query_build(scene->world, &scene->comp_text3d, 1, NULL, 0,
                           &q_text3d);

    VkrQueryCompiled compiled;
    if (vkr_entity_query_compile(scene->world, &q_text3d, scene->alloc,
                                 &compiled)) {
      DestroyText3DContext ctx = {
          .scene = scene,
          .rf = (RendererFrontend *)rf,
      };
      vkr_entity_query_compiled_each_chunk(&compiled, destroy_text3d_chunk_cb,
                                           &ctx);
      vkr_entity_query_compiled_destroy(scene->alloc, &compiled);
    }
  }

  // Remove owned mesh instances from mesh manager
  if (rf && scene->owned_instances) {
    for (uint32_t i = 0; i < scene->owned_instance_count; i++) {
      vkr_mesh_manager_destroy_instance(&rf->mesh_manager,
                                        scene->owned_instances[i]);
    }
    scene->owned_instance_count = 0;
  }

  // Remove owned meshes from mesh manager
  if (rf && scene->owned_meshes) {
    for (uint32_t i = 0; i < scene->owned_mesh_count; i++) {
      vkr_mesh_manager_remove(&rf->mesh_manager, scene->owned_meshes[i]);
    }
  }

  // Destroy queries
  if (scene->queries_valid) {
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_transforms);
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_renderables);
    vkr_entity_query_compiled_destroy(scene->alloc,
                                      &scene->query_directional_light);
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_point_lights);
    vkr_entity_query_compiled_destroy(scene->alloc, &scene->query_shapes);
  }

  scene_child_index_shutdown(scene);

  // Free arrays
  if (scene->topo_order) {
    vkr_allocator_free_aligned(scene->alloc, scene->topo_order,
                               scene->topo_capacity * sizeof(VkrEntityId),
                               AlignOf(VkrEntityId),
                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (scene->owned_meshes) {
    vkr_allocator_free_aligned(scene->alloc, scene->owned_meshes,
                               scene->owned_mesh_capacity * sizeof(uint32_t),
                               AlignOf(uint32_t),
                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (scene->owned_instances) {
    vkr_allocator_free_aligned(
        scene->alloc, scene->owned_instances,
        scene->owned_instance_capacity * sizeof(VkrMeshInstanceHandle),
        AlignOf(VkrMeshInstanceHandle), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (scene->render_dirty_entities) {
    vkr_allocator_free_aligned(
        scene->alloc, scene->render_dirty_entities,
        scene->render_dirty_capacity * sizeof(VkrEntityId),
        AlignOf(VkrEntityId), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  // Destroy ECS world
  if (scene->world) {
    vkr_entity_destroy_world(scene->world);
  }

  MemZero(scene, sizeof(VkrScene));
}

void vkr_scene_update(VkrScene *scene, float64_t dt) {
  (void)dt; // Reserved for future animation

  if (!scene || !scene->world)
    return;

  // Compile queries if needed
  if (!scene->queries_valid) {
    if (!scene_compile_queries(scene)) {
      return;
    }
  }

  // Rebuild topo order if hierarchy changed
  if (scene->hierarchy_dirty) {
    scene_rebuild_topo_order(scene);
  }

  // ============================================================================
  // Two-Pass Transform Update (Phase 3 + Phase 4 Optimization)
  // ============================================================================
  //
  // Pass 1: Update all dirty local matrices (chunk-based, cache-friendly)
  // - Iterates chunks contiguously for better cache utilization
  // - Local matrix computation has no dependencies, order doesn't matter
  // - Clears WORLD_UPDATED flag from previous frame
  //
  // Pass 2: Propagate world matrices (topo-ordered, deferred dirty propagation)
  // - Must be in topological order so parents are updated before children
  // - Deferred dirty propagation: if parent has WORLD_UPDATED, child inherits
  // dirty
  // - Eliminates expensive scene_mark_children_world_dirty() lookups

  // Pass 1: Chunk-based local matrix update + clear WORLD_UPDATED flags
  vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
                                       transform_local_update_cb, scene);

  // Pass 2: Topo-ordered world matrix propagation with deferred dirty
  // propagation
  VkrWorld *world = scene->world;
  VkrComponentTypeId comp_transform = scene->comp_transform;

  for (uint32_t i = 0; i < scene->topo_count; i++) {
    // Full entity ID stored in topo_order - no reconstruction needed
    VkrEntityId entity = scene->topo_order[i];

    // Combined is_alive + get_component - single validation
    SceneTransform *transform =
        (SceneTransform *)vkr_entity_get_component_if_alive(world, entity,
                                                            comp_transform);
    if (!transform)
      continue;

    // Single parent lookup for both dirty propagation and matrix computation
    SceneTransform *parent_transform = NULL;
    if (transform->parent.u64 != VKR_ENTITY_ID_INVALID.u64) {
      parent_transform = (SceneTransform *)vkr_entity_get_component_if_alive(
          world, transform->parent, comp_transform);

      // Deferred dirty propagation: if parent was updated this frame, inherit
      // dirty This eliminates the expensive scene_mark_children_world_dirty()
      // call
      if (parent_transform &&
          (parent_transform->flags & SCENE_TRANSFORM_WORLD_UPDATED)) {
        transform->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
      }
    }

    // Only process if world matrix needs update
    if (!(transform->flags & SCENE_TRANSFORM_DIRTY_WORLD))
      continue;

    // Compute world matrix
    if (parent_transform) {
      transform->world = mat4_mul(parent_transform->world, transform->local);
    } else {
      transform->world = transform->local;
    }

    transform->flags &= ~SCENE_TRANSFORM_DIRTY_WORLD;
    transform->flags |=
        SCENE_TRANSFORM_WORLD_UPDATED; // Mark for child propagation

    // Mark for render sync
    scene_mark_render_dirty(scene, entity);
  }
}

// ============================================================================
// Entity Management
// ============================================================================

VkrEntityId vkr_scene_create_entity(VkrScene *scene, VkrSceneError *out_error) {
  if (!scene || !scene->world) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_INVALID_ENTITY;
    return VKR_ENTITY_ID_INVALID;
  }

  VkrEntityId entity = vkr_entity_create_entity(scene->world);
  if (entity.u64 == VKR_ENTITY_ID_INVALID.u64) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ENTITY_LIMIT_REACHED;
    return VKR_ENTITY_ID_INVALID;
  }

  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return entity;
}

void vkr_scene_destroy_entity(VkrScene *scene, VkrEntityId entity) {
  if (!scene || !scene->world)
    return;
  bool8_t had_mesh =
      vkr_entity_has_component(scene->world, entity, scene->comp_mesh_renderer);

  VkrEntityId old_parent = VKR_ENTITY_ID_INVALID;
  const SceneTransform *t = (const SceneTransform *)vkr_entity_get_component(
      scene->world, entity, scene->comp_transform);
  if (t) {
    old_parent = t->parent;
  }

  // Mark children world-dirty before destroy so they recompute world as root.
  if (scene->queries_valid && scene_child_index_ensure_built(scene)) {
    SceneChildIndexSlot *slot = scene_child_index_get_slot(scene, entity);
    if (slot && slot->parent_id.u64 == entity.u64 && slot->children) {
      uint32_t i = 0;
      while (i < slot->child_count) {
        VkrEntityId child = slot->children[i];
        if (!vkr_entity_is_alive(scene->world, child) ||
            !vkr_entity_has_component(scene->world, child,
                                      scene->comp_transform)) {
          slot->children[i] = slot->children[slot->child_count - 1];
          slot->child_count--;
          continue;
        }
        SceneTransform *child_t =
            (SceneTransform *)vkr_entity_get_component_mut(
                scene->world, child, scene->comp_transform);
        if (child_t) {
          child_t->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
        }
        i++;
      }
    }
  } else if (scene->queries_valid) {
    // Fallback to scan if the index couldn't be built (e.g., OOM).
    SceneChildDirtyContext ctx = {
        .scene = scene,
        .parent = entity,
    };
    vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
                                         scene_mark_children_dirty_cb, &ctx);
  }

  if (scene->child_index_valid) {
    scene_child_index_remove(scene, old_parent, entity);
    scene_child_index_clear_parent_slot(scene, entity);
  }

  vkr_entity_destroy_entity(scene->world, entity);

  // Note: hierarchy will be cleaned up on next topo rebuild
  scene->hierarchy_dirty = true;
  if (had_mesh) {
    scene->render_full_sync_needed = true;
  }
}

bool8_t vkr_scene_entity_alive(const VkrScene *scene, VkrEntityId entity) {
  if (!scene || !scene->world)
    return false;
  return vkr_entity_is_alive(scene->world, entity);
}

// ============================================================================
// Component Helpers
// ============================================================================

bool8_t vkr_scene_set_name(VkrScene *scene, VkrEntityId entity, String8 name) {
  if (!scene || !scene->world)
    return false;

  SceneName *existing = (SceneName *)vkr_entity_get_component_mut(
      scene->world, entity, scene->comp_name);
  if (existing) {
    if (existing->name.length == name.length &&
        MemCompare(existing->name.str, name.str, name.length) == 0) {
      return true_v;
    }
  }

  // Copy string
  char *name_copy = (char *)vkr_allocator_alloc(
      scene->alloc, name.length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!name_copy)
    return false;
  MemCopy(name_copy, name.str, name.length);
  name_copy[name.length] = '\0';

  SceneName comp = {
      .name = {.str = (uint8_t *)name_copy, .length = name.length}};

  if (existing) {
    // Free the old string before overwriting
    if (existing->name.str) {
      vkr_allocator_free(scene->alloc, existing->name.str,
                         existing->name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
    *existing = comp;
    return true_v;
  }

  bool8_t result =
      vkr_entity_add_component(scene->world, entity, scene->comp_name, &comp);
  if (result) {
    scene_invalidate_queries(scene);
  }
  return result;
}

String8 vkr_scene_get_name(const VkrScene *scene, VkrEntityId entity) {
  if (!scene || !scene->world)
    return (String8){0};

  const SceneName *comp = (const SceneName *)vkr_entity_get_component(
      scene->world, entity, scene->comp_name);
  if (!comp)
    return (String8){0};

  return comp->name;
}

bool8_t vkr_scene_set_transform(VkrScene *scene, VkrEntityId entity,
                                Vec3 position, VkrQuat rotation, Vec3 scale) {
  if (!scene || !scene->world)
    return false;

  SceneTransform comp = {
      .position = position,
      .rotation = rotation,
      .scale = scale,
      .parent = VKR_ENTITY_ID_INVALID,
      .local = scene_compute_local_matrix(position, rotation, scale),
      .world = mat4_identity(),
      .flags = SCENE_TRANSFORM_DIRTY_WORLD,
  };
  comp.world = comp.local; // Initial world = local (no parent)

  bool8_t result = vkr_entity_add_component(scene->world, entity,
                                            scene->comp_transform, &comp);
  if (!result) {
    log_error("Failed to add transform component (type %u) to entity",
              scene->comp_transform);
    return false;
  }
  scene->hierarchy_dirty = true;   // New entity in hierarchy
  scene_invalidate_queries(scene); // Query may need recompile for new archetype
  return true;
}

SceneTransform *vkr_scene_get_transform(VkrScene *scene, VkrEntityId entity) {
  if (!scene || !scene->world)
    return NULL;
  return (SceneTransform *)vkr_entity_get_component_mut(scene->world, entity,
                                                        scene->comp_transform);
}

void vkr_scene_set_position(VkrScene *scene, VkrEntityId entity,
                            Vec3 position) {
  SceneTransform *t = vkr_scene_get_transform(scene, entity);
  if (!t)
    return;
  t->position = position;
  t->flags |= SCENE_TRANSFORM_DIRTY_LOCAL | SCENE_TRANSFORM_DIRTY_WORLD;
}

void vkr_scene_set_rotation(VkrScene *scene, VkrEntityId entity,
                            VkrQuat rotation) {
  SceneTransform *t = vkr_scene_get_transform(scene, entity);
  if (!t)
    return;
  t->rotation = rotation;
  t->flags |= SCENE_TRANSFORM_DIRTY_LOCAL | SCENE_TRANSFORM_DIRTY_WORLD;
}

void vkr_scene_set_scale(VkrScene *scene, VkrEntityId entity, Vec3 scale) {
  SceneTransform *t = vkr_scene_get_transform(scene, entity);
  if (!t)
    return;
  t->scale = scale;
  t->flags |= SCENE_TRANSFORM_DIRTY_LOCAL | SCENE_TRANSFORM_DIRTY_WORLD;
}

void vkr_scene_set_parent(VkrScene *scene, VkrEntityId entity,
                          VkrEntityId parent) {
  SceneTransform *t = vkr_scene_get_transform(scene, entity);
  if (!t)
    return;

  VkrEntityId old_parent = t->parent;
  if (old_parent.u64 == parent.u64) {
    return;
  }

  if (scene->child_index_valid) {
    scene_child_index_remove(scene, old_parent, entity);
    if (parent.u64 != VKR_ENTITY_ID_INVALID.u64 &&
        vkr_entity_is_alive(scene->world, parent)) {
      scene_child_index_add(scene, parent, entity);
    }
  }

  t->parent = parent;
  t->flags |= SCENE_TRANSFORM_DIRTY_HIERARCHY | SCENE_TRANSFORM_DIRTY_WORLD;
  scene->hierarchy_dirty = true;
}

bool8_t vkr_scene_set_mesh_renderer(VkrScene *scene, VkrEntityId entity,
                                    VkrMeshInstanceHandle instance) {
  if (!scene || !scene->world)
    return false;

  SceneMeshRenderer comp = {.instance = instance};
  if (!vkr_scene_ensure_render_id(scene, entity, NULL)) {
    log_error("Failed to assign render id for entity (instance.id=%u)",
              instance.id);
    return false;
  }

  bool8_t result = vkr_entity_add_component(scene->world, entity,
                                            scene->comp_mesh_renderer, &comp);
  if (result) {
    scene_invalidate_queries(
        scene); // Query may need recompile for new archetype
    scene->render_full_sync_needed = true;
  }
  return result;
}

bool8_t vkr_scene_ensure_render_id(VkrScene *scene, VkrEntityId entity,
                                   uint32_t *out_render_id) {
  if (!scene || !scene->world)
    return false;

  SceneRenderId *existing = (SceneRenderId *)vkr_entity_get_component_mut(
      scene->world, entity, scene->comp_render_id);
  if (existing) {
    if (out_render_id)
      *out_render_id = existing->id;
    return true_v;
  }

  if (scene->next_render_id == 0 ||
      scene->next_render_id > VKR_PICKING_ID_MAX_VALUE) {
    log_error("Scene render id allocator exhausted");
    return false_v;
  }

  SceneRenderId comp = {.id = scene->next_render_id++};
  if (!vkr_entity_add_component(scene->world, entity, scene->comp_render_id,
                                &comp)) {
    return false_v;
  }

  scene_invalidate_queries(scene);
  scene->render_full_sync_needed = true;
  if (out_render_id)
    *out_render_id = comp.id;
  return true_v;
}

uint32_t vkr_scene_get_render_id(const VkrScene *scene, VkrEntityId entity) {
  if (!scene || !scene->world)
    return 0;
  const SceneRenderId *comp = (const SceneRenderId *)vkr_entity_get_component(
      scene->world, entity, scene->comp_render_id);
  return comp ? comp->id : 0;
}

void vkr_scene_set_visibility(VkrScene *scene, VkrEntityId entity,
                              bool8_t visible, bool8_t inherit_parent) {
  if (!scene || !scene->world)
    return;

  SceneVisibility comp = {
      .visible = visible,
      .inherit_parent = inherit_parent,
  };
  SceneVisibility *existing = (SceneVisibility *)vkr_entity_get_component_mut(
      scene->world, entity, scene->comp_visibility);
  if (existing) {
    *existing = comp;
  } else {
    if (vkr_entity_add_component(scene->world, entity, scene->comp_visibility,
                                 &comp)) {
      scene_invalidate_queries(scene);
    }
  }

  scene->render_full_sync_needed = true;
}

// ============================================================================
// Light Components
// ============================================================================

bool8_t vkr_scene_set_point_light(VkrScene *scene, VkrEntityId entity,
                                  const ScenePointLight *light) {
  if (!scene || !scene->world || !light)
    return false_v;

  // Ensure render ID for picking
  uint32_t prev_render_id = vkr_scene_get_render_id(scene, entity);
  if (!vkr_scene_ensure_render_id(scene, entity, NULL)) {
    log_error("Failed to assign render id for point light entity");
    return false_v;
  }
  bool8_t render_id_added = (prev_render_id == 0);

  ScenePointLight *existing = (ScenePointLight *)vkr_entity_get_component_mut(
      scene->world, entity, scene->comp_point_light);
  if (existing) {
    *existing = *light;
    if (render_id_added) {
      scene->render_full_sync_needed = true;
    }
    return true_v;
  }

  bool8_t result = vkr_entity_add_component(scene->world, entity,
                                            scene->comp_point_light, light);
  if (result) {
    scene_invalidate_queries(scene);
    scene->render_full_sync_needed = true;
  }
  return result;
}

ScenePointLight *vkr_scene_get_point_light(VkrScene *scene,
                                           VkrEntityId entity) {
  if (!scene || !scene->world)
    return NULL;
  return (ScenePointLight *)vkr_entity_get_component_mut(
      scene->world, entity, scene->comp_point_light);
}

bool8_t vkr_scene_set_directional_light(VkrScene *scene, VkrEntityId entity,
                                        const SceneDirectionalLight *light) {
  if (!scene || !scene->world || !light)
    return false_v;

  SceneDirectionalLight *existing =
      (SceneDirectionalLight *)vkr_entity_get_component_mut(
          scene->world, entity, scene->comp_directional_light);
  if (existing) {
    *existing = *light;
    return true_v;
  }

  bool8_t result = vkr_entity_add_component(
      scene->world, entity, scene->comp_directional_light, light);
  if (result) {
    scene_invalidate_queries(scene);
  }
  return result;
}

SceneDirectionalLight *vkr_scene_get_directional_light(VkrScene *scene,
                                                       VkrEntityId entity) {
  if (!scene || !scene->world)
    return NULL;
  return (SceneDirectionalLight *)vkr_entity_get_component_mut(
      scene->world, entity, scene->comp_directional_light);
}

// ============================================================================
// Mesh Ownership
// ============================================================================

bool8_t vkr_scene_spawn_mesh(VkrScene *scene, struct s_RendererFrontend *rf,
                             const VkrMeshLoadDesc *desc,
                             uint32_t *out_mesh_index,
                             VkrSceneError *out_error) {
  if (!scene || !rf || !desc || !out_mesh_index) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false;
  }

  uint32_t mesh_index;
  VkrRendererError load_error;

  if (!vkr_mesh_manager_load(&rf->mesh_manager, desc, &mesh_index, NULL,
                             &load_error)) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_MESH_LOAD_FAILED;
    return false;
  }

  if (!vkr_scene_track_mesh(scene, mesh_index, out_error)) {
    return false;
  }

  *out_mesh_index = mesh_index;
  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return true;
}

bool8_t vkr_scene_track_mesh(VkrScene *scene, uint32_t mesh_index,
                             VkrSceneError *out_error) {
  if (!scene) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false;
  }

  if (!scene_ensure_mesh_capacity(scene, scene->owned_mesh_count + 1)) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false;
  }

  scene->owned_meshes[scene->owned_mesh_count++] = mesh_index;
  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return true;
}

void vkr_scene_release_mesh(VkrScene *scene, uint32_t mesh_index) {
  if (!scene)
    return;

  // Find and remove from owned list
  for (uint32_t i = 0; i < scene->owned_mesh_count; i++) {
    if (scene->owned_meshes[i] == mesh_index) {
      // Swap with last and shrink
      scene->owned_meshes[i] = scene->owned_meshes[scene->owned_mesh_count - 1];
      scene->owned_mesh_count--;
      return;
    }
  }
}

/**
 * @brief Ensure owned instance handle array has sufficient capacity.
 */
vkr_internal bool8_t scene_ensure_instance_capacity(VkrScene *scene,
                                                    uint32_t needed) {
  return scene_grow_array(
      scene->alloc, (void **)&scene->owned_instances,
      &scene->owned_instance_capacity, scene->owned_instance_count, needed,
      SCENE_DEFAULT_INSTANCE_CAPACITY, sizeof(VkrMeshInstanceHandle),
      AlignOf(VkrMeshInstanceHandle), false_v);
}

bool8_t vkr_scene_track_instance(VkrScene *scene,
                                 VkrMeshInstanceHandle instance,
                                 VkrSceneError *out_error) {
  if (!scene) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

  if (!scene_ensure_instance_capacity(scene, scene->owned_instance_count + 1)) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

  scene->owned_instances[scene->owned_instance_count++] = instance;
  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return true_v;
}

void vkr_scene_release_instance(VkrScene *scene,
                                VkrMeshInstanceHandle instance) {
  if (!scene)
    return;

  // Find and remove from owned list
  for (uint32_t i = 0; i < scene->owned_instance_count; i++) {
    if (scene->owned_instances[i].id == instance.id &&
        scene->owned_instances[i].generation == instance.generation) {
      // Swap with last and shrink
      scene->owned_instances[i] =
          scene->owned_instances[scene->owned_instance_count - 1];
      scene->owned_instance_count--;
      return;
    }
  }
}

// ============================================================================
// Render Bridge
// ============================================================================

vkr_internal void scene_entity_id_fill_invalid(VkrEntityId *entities,
                                               uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    entities[i] = VKR_ENTITY_ID_INVALID;
  }
}

vkr_internal bool8_t scene_render_bridge_init(VkrSceneRenderBridge *bridge,
                                              VkrAllocator *alloc,
                                              uint32_t initial_capacity) {
  if (!bridge || !alloc)
    return false;

  MemZero(bridge, sizeof(VkrSceneRenderBridge));
  bridge->alloc = alloc;

  if (initial_capacity == 0)
    initial_capacity = 256;

  bridge->render_id_to_entity = (VkrEntityId *)vkr_allocator_alloc_aligned(
      alloc, initial_capacity * sizeof(VkrEntityId), AlignOf(VkrEntityId),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!bridge->render_id_to_entity)
    return false;

  scene_entity_id_fill_invalid(bridge->render_id_to_entity, initial_capacity);
  bridge->render_id_capacity = initial_capacity;

  return true;
}

vkr_internal void scene_render_bridge_shutdown(VkrSceneRenderBridge *bridge) {
  if (!bridge)
    return;

  if (bridge->render_id_to_entity) {
    vkr_allocator_free_aligned(bridge->alloc, bridge->render_id_to_entity,
                               bridge->render_id_capacity * sizeof(VkrEntityId),
                               AlignOf(VkrEntityId),
                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  MemZero(bridge, sizeof(VkrSceneRenderBridge));
}

vkr_internal bool8_t scene_render_bridge_ensure_render_id_capacity(
    VkrSceneRenderBridge *bridge, uint32_t needed) {
  if (needed <= bridge->render_id_capacity)
    return true_v;

  uint32_t new_capacity =
      scene_next_capacity(bridge->render_id_capacity, needed, 256);

  VkrEntityId *new_map = (VkrEntityId *)vkr_allocator_alloc_aligned(
      bridge->alloc, new_capacity * sizeof(VkrEntityId), AlignOf(VkrEntityId),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!new_map)
    return false_v;

  scene_entity_id_fill_invalid(new_map, new_capacity);

  if (bridge->render_id_to_entity && bridge->render_id_capacity > 0) {
    MemCopy(new_map, bridge->render_id_to_entity,
            bridge->render_id_capacity * sizeof(VkrEntityId));
    vkr_allocator_free_aligned(bridge->alloc, bridge->render_id_to_entity,
                               bridge->render_id_capacity * sizeof(VkrEntityId),
                               AlignOf(VkrEntityId),
                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  bridge->render_id_to_entity = new_map;
  bridge->render_id_capacity = new_capacity;
  return true_v;
}

vkr_internal void
scene_render_bridge_clear_mapping(VkrSceneRenderBridge *bridge) {
  scene_entity_id_fill_invalid(bridge->render_id_to_entity,
                               bridge->render_id_capacity);
}

vkr_internal bool8_t scene_entity_is_visible(VkrScene *scene,
                                             VkrEntityId entity) {
  VkrWorld *world = scene->world;
  VkrComponentTypeId comp_visibility = scene->comp_visibility;
  VkrComponentTypeId comp_transform = scene->comp_transform;
  uint32_t max_depth = world->dir.capacity;
  uint32_t depth = 0;
  VkrEntityId current = entity;

  while (current.u64 != VKR_ENTITY_ID_INVALID.u64 && depth < max_depth) {
    // Combined is_alive + get_component: returns NULL if dead or component
    // missing
    const SceneVisibility *vis =
        (const SceneVisibility *)vkr_entity_get_component_if_alive_const(
            world, current, comp_visibility);
    if (vis) {
      if (!vis->visible) {
        return false_v;
      }
      if (!vis->inherit_parent) {
        return true_v;
      }
    }

    // Get transform for parent traversal
    const SceneTransform *transform =
        (const SceneTransform *)vkr_entity_get_component_if_alive_const(
            world, current, comp_transform);
    if (!transform) {
      break;
    }

    VkrEntityId parent = transform->parent;
    if (parent.u64 == VKR_ENTITY_ID_INVALID.u64) {
      break;
    }

    current = parent;
    depth++;
  }

  return true_v;
}

/**
 * @brief Sync context for render bridge.
 */
typedef struct RenderSyncContext {
  VkrSceneRenderBridge *bridge;
  struct s_RendererFrontend *rf;
  VkrScene *scene;
} RenderSyncContext;

vkr_internal void
scene_render_bridge_update_mapping(VkrSceneRenderBridge *bridge,
                                   uint32_t render_id, VkrEntityId entity,
                                   bool8_t is_visible) {
  if (render_id == 0)
    return;
  if (scene_render_bridge_ensure_render_id_capacity(bridge, render_id + 1)) {
    bridge->render_id_to_entity[render_id] =
        is_visible ? entity : VKR_ENTITY_ID_INVALID;
  }
}

vkr_internal void scene_sync_renderable(RenderSyncContext *ctx,
                                        VkrEntityId entity,
                                        VkrMeshInstanceHandle instance,
                                        Mat4 world, uint32_t render_id,
                                        bool8_t is_visible) {
  if (!vkr_mesh_manager_instance_set_visible(&ctx->rf->mesh_manager, instance,
                                             is_visible)) {
    return;
  }

  if (!vkr_mesh_manager_instance_set_render_id(&ctx->rf->mesh_manager, instance,
                                               render_id)) {
    return;
  }

  if (!is_visible) {
    scene_render_bridge_update_mapping(ctx->bridge, render_id, entity, false_v);
    return;
  }

  if (!vkr_mesh_manager_instance_set_model(&ctx->rf->mesh_manager, instance,
                                           world)) {
    return;
  }

  scene_render_bridge_update_mapping(ctx->bridge, render_id, entity, true_v);
}

/**
 * @brief Chunk callback to sync renderables.
 */
vkr_internal void render_sync_chunk_cb(const VkrArchetype *arch,
                                       VkrChunk *chunk, void *user) {
  (void)arch;
  RenderSyncContext *ctx = (RenderSyncContext *)user;
  VkrScene *scene = ctx->scene;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  SceneMeshRenderer *mesh_renderers =
      (SceneMeshRenderer *)vkr_entity_chunk_column(chunk,
                                                   scene->comp_mesh_renderer);
  SceneRenderId *render_ids =
      (SceneRenderId *)vkr_entity_chunk_column(chunk, scene->comp_render_id);

  for (uint32_t i = 0; i < count; i++) {
    VkrMeshInstanceHandle instance = mesh_renderers[i].instance;
    bool8_t is_visible = scene_entity_is_visible(scene, entities[i]);
    uint32_t render_id = render_ids ? render_ids[i].id : 0;
    scene_sync_renderable(ctx, entities[i], instance, transforms[i].world,
                          render_id, is_visible);
  }
}

/**
 * @brief Chunk callback to sync point light render IDs for picking.
 */
vkr_internal void render_sync_point_light_cb(const VkrArchetype *arch,
                                             VkrChunk *chunk, void *user) {
  (void)arch;
  RenderSyncContext *ctx = (RenderSyncContext *)user;
  VkrScene *scene = ctx->scene;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  ScenePointLight *lights = (ScenePointLight *)vkr_entity_chunk_column(
      chunk, scene->comp_point_light);

  if (!lights)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (!lights[i].enabled)
      continue;

    uint32_t render_id = vkr_scene_get_render_id(scene, entities[i]);
    if (render_id == 0)
      continue;

    bool8_t is_visible = scene_entity_is_visible(scene, entities[i]);
    scene_render_bridge_update_mapping(ctx->bridge, render_id, entities[i],
                                       is_visible);
  }
}

/**
 * @brief Chunk callback to sync shapes (mesh-slot path).
 */
vkr_internal void render_sync_shape_cb(const VkrArchetype *arch,
                                       VkrChunk *chunk, void *user) {
  (void)arch;
  RenderSyncContext *ctx = (RenderSyncContext *)user;
  VkrScene *scene = ctx->scene;
  RendererFrontend *rf = ctx->rf;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  SceneShape *shapes =
      (SceneShape *)vkr_entity_chunk_column(chunk, scene->comp_shape);
  SceneRenderId *render_ids =
      (SceneRenderId *)vkr_entity_chunk_column(chunk, scene->comp_render_id);

  if (!transforms || !shapes || !render_ids)
    return;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t mesh_index = shapes[i].mesh_index;
    if (mesh_index == VKR_INVALID_ID)
      continue;

    uint32_t render_id = render_ids[i].id;
    bool8_t is_visible = scene_entity_is_visible(scene, entities[i]);

    // Sync mesh-slot state for shapes
    vkr_mesh_manager_set_model(&rf->mesh_manager, mesh_index,
                               transforms[i].world);
    vkr_mesh_manager_set_visible(&rf->mesh_manager, mesh_index, is_visible);
    vkr_mesh_manager_set_render_id(&rf->mesh_manager, mesh_index, render_id);

    // Update picking mapping
    scene_render_bridge_update_mapping(ctx->bridge, render_id, entities[i],
                                       is_visible);
  }
}

vkr_internal void scene_render_bridge_sync(VkrSceneRenderBridge *bridge,
                                           struct s_RendererFrontend *rf,
                                           VkrScene *scene) {
  if (!bridge || !rf || !scene)
    return;

  // If full sync needed or dirty overflow, do full sync
  if (scene->render_full_sync_needed) {
    scene_render_bridge_full_sync(bridge, rf, scene);
    return;
  }

  RenderSyncContext ctx = {
      .bridge = bridge,
      .rf = rf,
      .scene = scene,
  };

  // Process dirty entities - use inline functions for hot path
  VkrWorld *world = scene->world;
  for (uint32_t i = 0; i < scene->render_dirty_count; i++) {
    VkrEntityId entity = scene->render_dirty_entities[i];

    // Combined is_alive + get_component - single validation
    const SceneTransform *transform =
        (const SceneTransform *)vkr_entity_get_component_if_alive_const(
            world, entity, scene->comp_transform);
    if (!transform)
      continue;

    // Entity is alive (validated above), use unchecked for render_id
    // But render_id might not exist, so use safe accessor
    const SceneRenderId *render_id_comp =
        (const SceneRenderId *)vkr_entity_get_component_if_alive_const(
            world, entity, scene->comp_render_id);
    if (!render_id_comp)
      continue;

    bool8_t is_visible = scene_entity_is_visible(scene, entity);
    uint32_t render_id = render_id_comp->id;

    // Try mesh renderer (instance) first - entity validated, component optional
    const SceneMeshRenderer *mesh_renderer =
        (const SceneMeshRenderer *)vkr_entity_get_component_if_alive_const(
            world, entity, scene->comp_mesh_renderer);
    if (mesh_renderer) {
      VkrMeshInstanceHandle instance = mesh_renderer->instance;
      scene_sync_renderable(&ctx, entity, instance, transform->world, render_id,
                            is_visible);
      continue;
    }

    // Try shape (mesh-slot path) next
    const SceneShape *shape =
        (const SceneShape *)vkr_entity_get_component_if_alive_const(
            world, entity, scene->comp_shape);
    if (shape && shape->mesh_index != VKR_INVALID_ID) {
      vkr_mesh_manager_set_model(&rf->mesh_manager, shape->mesh_index,
                                 transform->world);
      vkr_mesh_manager_set_visible(&rf->mesh_manager, shape->mesh_index,
                                   is_visible);
      vkr_mesh_manager_set_render_id(&rf->mesh_manager, shape->mesh_index,
                                     render_id);
      scene_render_bridge_update_mapping(ctx.bridge, render_id, entity,
                                         is_visible);
    }
  }

  scene->render_dirty_count = 0;
}

vkr_internal void scene_render_bridge_full_sync(VkrSceneRenderBridge *bridge,
                                                struct s_RendererFrontend *rf,
                                                VkrScene *scene) {
  if (!bridge || !rf || !scene)
    return;

  // Compile queries if needed
  if (!scene->queries_valid) {
    if (!scene_compile_queries(scene)) {
      return;
    }
  }

  uint32_t render_id_capacity = scene->next_render_id + 1;
  if (!scene_render_bridge_ensure_render_id_capacity(bridge,
                                                     render_id_capacity)) {
    log_error("Scene render bridge: failed to resize render id mapping");
    return;
  }
  scene_render_bridge_clear_mapping(bridge);

  RenderSyncContext ctx = {
      .bridge = bridge,
      .rf = rf,
      .scene = scene,
  };

  vkr_entity_query_compiled_each_chunk(&scene->query_renderables,
                                       render_sync_chunk_cb, &ctx);
  vkr_entity_query_compiled_each_chunk(&scene->query_point_lights,
                                       render_sync_point_light_cb, &ctx);
  vkr_entity_query_compiled_each_chunk(&scene->query_shapes,
                                       render_sync_shape_cb, &ctx);

  scene->render_dirty_count = 0;
  scene->render_full_sync_needed = false;
}

vkr_internal VkrEntityId scene_render_bridge_entity_from_picking_id(
    const VkrSceneRenderBridge *bridge, uint32_t object_id) {
  if (!bridge || object_id == 0)
    return VKR_ENTITY_ID_INVALID;

  VkrPickingDecodedId decoded = vkr_picking_decode_id(object_id);
  if (!decoded.valid || decoded.kind != VKR_PICKING_ID_KIND_SCENE) {
    return VKR_ENTITY_ID_INVALID;
  }

  uint32_t render_id = decoded.value;
  if (render_id >= bridge->render_id_capacity)
    return VKR_ENTITY_ID_INVALID;

  return bridge->render_id_to_entity[render_id];
}

// ============================================================================
// Scene Runtime Handle API
// ============================================================================

VkrSceneHandle vkr_scene_handle_create(VkrAllocator *alloc, uint16_t world_id,
                                       uint32_t initial_entity_capacity,
                                       uint32_t initial_picking_capacity,
                                       VkrSceneError *out_error) {
  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  if (!alloc) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return VKR_SCENE_HANDLE_INVALID;
  }

  struct VkrSceneRuntime *runtime =
      (struct VkrSceneRuntime *)vkr_allocator_alloc(
          alloc, sizeof(*runtime), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!runtime) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return VKR_SCENE_HANDLE_INVALID;
  }

  MemZero(runtime, sizeof(*runtime));
  runtime->parent_alloc = alloc;

  // Create per-scene arena for ECS allocations. This allows bulk deallocation
  // on scene destroy and prevents arena high-water-mark growth across cycles.
  runtime->scene_arena = arena_create(MB(2), MB(2));
  if (!runtime->scene_arena) {
    vkr_allocator_free(alloc, runtime, sizeof(*runtime),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return VKR_SCENE_HANDLE_INVALID;
  }
  runtime->scene_allocator.ctx = runtime->scene_arena;
  if (!vkr_allocator_arena(&runtime->scene_allocator)) {
    arena_destroy(runtime->scene_arena);
    vkr_allocator_free(alloc, runtime, sizeof(*runtime),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return VKR_SCENE_HANDLE_INVALID;
  }

  VkrSceneError err = VKR_SCENE_ERROR_NONE;
  if (!vkr_scene_init(&runtime->scene, &runtime->scene_allocator, world_id,
                      initial_entity_capacity, &err)) {
    arena_destroy(runtime->scene_arena);
    vkr_allocator_free(alloc, runtime, sizeof(*runtime),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (out_error)
      *out_error = err;
    return VKR_SCENE_HANDLE_INVALID;
  }

  if (!scene_render_bridge_init(&runtime->bridge, &runtime->scene_allocator,
                                initial_picking_capacity)) {
    vkr_scene_shutdown(&runtime->scene, NULL);
    arena_destroy(runtime->scene_arena);
    vkr_allocator_free(alloc, runtime, sizeof(*runtime),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return VKR_SCENE_HANDLE_INVALID;
  }

  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return (VkrSceneHandle)runtime;
}

void vkr_scene_handle_destroy(VkrSceneHandle handle,
                              struct s_RendererFrontend *rf) {
  if (!handle)
    return;

  struct VkrSceneRuntime *runtime = (struct VkrSceneRuntime *)handle;
  VkrAllocator *parent_alloc = runtime->parent_alloc;

  scene_render_bridge_shutdown(&runtime->bridge);
  vkr_scene_shutdown(&runtime->scene, rf);
  if (rf && rf->render_graph_enabled && rf->render_graph) {
    vkr_rg_log_resource_stats(rf->render_graph, "RenderGraph (scene unload)");
  }

  // Release global accounting for the scene arena before destroying it.
  // This adjusts global memory stats for all allocations made from scene_alloc
  // since arena frees are no-ops and wouldn't decrement the counters otherwise.
  if (runtime->scene_arena) {
    vkr_allocator_release_global_accounting(&runtime->scene_allocator);
    arena_destroy(runtime->scene_arena);
    runtime->scene_arena = NULL;
  }

  // Free the runtime struct from the parent allocator.
  if (parent_alloc) {
    vkr_allocator_free(parent_alloc, runtime, sizeof(*runtime),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
}

VkrScene *vkr_scene_handle_get_scene(VkrSceneHandle handle) {
  if (!handle)
    return NULL;
  struct VkrSceneRuntime *runtime = (struct VkrSceneRuntime *)handle;
  return &runtime->scene;
}

void vkr_scene_handle_update(VkrSceneHandle handle, float64_t dt) {
  if (!handle)
    return;
  struct VkrSceneRuntime *runtime = (struct VkrSceneRuntime *)handle;
  vkr_scene_update(&runtime->scene, dt);
}

void vkr_scene_handle_sync(VkrSceneHandle handle,
                           struct s_RendererFrontend *rf) {
  if (!handle || !rf)
    return;
  struct VkrSceneRuntime *runtime = (struct VkrSceneRuntime *)handle;
  scene_render_bridge_sync(&runtime->bridge, rf, &runtime->scene);
}

void vkr_scene_handle_full_sync(VkrSceneHandle handle,
                                struct s_RendererFrontend *rf) {
  if (!handle || !rf)
    return;
  struct VkrSceneRuntime *runtime = (struct VkrSceneRuntime *)handle;
  scene_render_bridge_full_sync(&runtime->bridge, rf, &runtime->scene);
}

void vkr_scene_handle_update_and_sync(VkrSceneHandle handle,
                                      struct s_RendererFrontend *rf,
                                      float64_t dt) {
  if (!handle || !rf)
    return;
  struct VkrSceneRuntime *runtime = (struct VkrSceneRuntime *)handle;
  vkr_scene_update(&runtime->scene, dt);
  scene_render_bridge_sync(&runtime->bridge, rf, &runtime->scene);
}

VkrEntityId vkr_scene_handle_entity_from_picking_id(VkrSceneHandle handle,
                                                    uint32_t object_id) {
  if (!handle)
    return VKR_ENTITY_ID_INVALID;
  struct VkrSceneRuntime *runtime = (struct VkrSceneRuntime *)handle;
  return scene_render_bridge_entity_from_picking_id(&runtime->bridge,
                                                    object_id);
}

// ============================================================================
// Text3D Implementation
// ============================================================================

bool8_t vkr_scene_set_text3d(VkrScene *scene, VkrEntityId entity,
                             const VkrSceneText3DConfig *config,
                             VkrSceneError *out_error) {
  if (!scene || !scene->world || !config || !scene->rf) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)scene->rf;

  // Check if entity already has text3d
  SceneText3D *existing = (SceneText3D *)vkr_entity_get_component_mut(
      scene->world, entity, scene->comp_text3d);
  if (existing) {
    // Update existing text via layer message
    existing->dirty = true_v;
    if (out_error)
      *out_error = VKR_SCENE_ERROR_NONE;
    return true_v;
  }

  // Allocate world-resources text slot ID (use entity index as text_id)
  uint32_t text_id = entity.parts.index;

  // Get transform for text positioning
  const SceneTransform *transform =
      (const SceneTransform *)vkr_entity_get_component(scene->world, entity,
                                                       scene->comp_transform);
  VkrTransform text_transform = vkr_transform_identity();
  if (transform) {
    text_transform = vkr_transform_from_position_scale_rotation(
        transform->position, transform->scale, transform->rotation);
  }

  VkrText3DConfig text_config = {
      .font = config->font,
      .font_size = config->font_size,
      .color = config->color,
      .texture_width = config->texture_width,
      .texture_height = config->texture_height,
      .uv_inset_px = config->uv_inset_px,
  };

  VkrWorldTextCreateData payload = {
      .text_id = text_id,
      .content = config->text,
      .config = &text_config,
      .transform = text_transform,
  };

  if (rf->world_resources.initialized) {
    vkr_world_resources_text_create(rf, &rf->world_resources, &payload);
  } else {
    log_warn("Scene: world_resources not initialized, text3d will not render");
  }

  uint32_t tex_w = config->texture_width > 0 ? config->texture_width
                                             : VKR_TEXT_3D_DEFAULT_TEXTURE_SIZE;
  uint32_t tex_h = config->texture_height > 0
                       ? config->texture_height
                       : VKR_TEXT_3D_DEFAULT_TEXTURE_SIZE;

  float32_t world_width = 1.0f;
  float32_t world_height =
      (tex_w > 0) ? ((float32_t)tex_h / (float32_t)tex_w) : 1.0f;

  // Add component to entity
  SceneText3D comp = {
      .text_index = text_id,
      .dirty = false_v,
      .world_width = world_width,
      .world_height = world_height,
  };

  if (!vkr_entity_add_component(scene->world, entity, scene->comp_text3d,
                                &comp)) {
    if (rf->world_resources.initialized) {
      vkr_world_resources_text_destroy(rf, &rf->world_resources, text_id);
    }
    if (out_error)
      *out_error = VKR_SCENE_ERROR_COMPONENT_ADD_FAILED;
    return false_v;
  }

  scene_invalidate_queries(scene);

  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return true_v;
}

SceneText3D *vkr_scene_get_text3d(VkrScene *scene, VkrEntityId entity) {
  if (!scene || !scene->world)
    return NULL;
  return (SceneText3D *)vkr_entity_get_component_mut(scene->world, entity,
                                                     scene->comp_text3d);
}

bool8_t vkr_scene_update_text3d(VkrScene *scene, VkrEntityId entity,
                                String8 text) {
  if (!scene || !scene->world || !scene->rf)
    return false_v;

  SceneText3D *comp = (SceneText3D *)vkr_entity_get_component_mut(
      scene->world, entity, scene->comp_text3d);
  if (!comp)
    return false_v;

  RendererFrontend *rf = (RendererFrontend *)scene->rf;

  if (rf->world_resources.initialized) {
    if (!vkr_world_resources_text_update(rf, &rf->world_resources,
                                         comp->text_index, text)) {
      return false_v;
    }
  }

  comp->dirty = false_v;
  return true_v;
}

// ============================================================================
// Shape Implementation
// ============================================================================

bool8_t vkr_scene_set_shape(VkrScene *scene, struct s_RendererFrontend *rf,
                            VkrEntityId entity,
                            const VkrSceneShapeConfig *config,
                            VkrSceneError *out_error) {
  if (!scene || !scene->world || !rf || !config) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

  // For now, only support cube
  if (config->type != SCENE_SHAPE_TYPE_CUBE) {
    log_error("Scene: unsupported shape type %d", config->type);
    if (out_error)
      *out_error = VKR_SCENE_ERROR_INVALID_ENTITY;
    return false_v;
  }

  // Create cube geometry
  char shape_name[64];
  snprintf(shape_name, sizeof(shape_name), "scene_cube_%u_%u",
           entity.parts.index, entity.parts.generation);

  VkrRendererError geom_err = VKR_RENDERER_ERROR_NONE;
  VkrGeometryHandle geom = vkr_geometry_system_create_cube(
      &rf->geometry_system, config->dimensions.x, config->dimensions.y,
      config->dimensions.z, shape_name, &geom_err);

  if (geom.id == VKR_INVALID_ID) {
    String8 err_str = vkr_renderer_get_error_string(geom_err);
    log_error("Scene: failed to create cube geometry: %s",
              string8_cstr(&err_str));
    if (out_error)
      *out_error = VKR_SCENE_ERROR_MESH_LOAD_FAILED;
    return false_v;
  }

  // Get transform for mesh position
  const SceneTransform *transform =
      (const SceneTransform *)vkr_entity_get_component(scene->world, entity,
                                                       scene->comp_transform);
  VkrTransform mesh_transform = vkr_transform_identity();
  if (transform) {
    mesh_transform = vkr_transform_from_position_scale_rotation(
        transform->position, transform->scale, transform->rotation);
  }

  // Acquire or create material for shape
  VkrMaterialHandle mat = rf->material_system.default_material;
  bool8_t owns_material = false_v;

  if (config->material_name.length > 0) {
    // Material specified with name and path
    VkrAllocatorScope scope = vkr_allocator_begin_scope(&rf->scratch_allocator);
    String8 mat_name =
        string8_duplicate(&rf->scratch_allocator, &config->material_name);
    String8 mat_path =
        string8_duplicate(&rf->scratch_allocator, &config->material_path);

    // Try to acquire existing material by name
    VkrRendererError mat_err = VKR_RENDERER_ERROR_NONE;
    VkrMaterialHandle acquired_mat = vkr_material_system_acquire(
        &rf->material_system, mat_name, true_v, &mat_err);

    if (mat_err == VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED &&
        mat_path.length > 0) {
      // Material not loaded - try to load it via resource system
      VkrResourceHandleInfo handle_info = {0};
      VkrRendererError load_err = VKR_RENDERER_ERROR_NONE;

      if (vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL, mat_path,
                                   &rf->scratch_allocator, &handle_info,
                                   &load_err)) {
        // After loading, acquire with name to get proper ref count
        acquired_mat = vkr_material_system_acquire(&rf->material_system,
                                                   mat_name, true_v, &mat_err);
        if (mat_err == VKR_RENDERER_ERROR_NONE && acquired_mat.id != 0) {
          mat = acquired_mat;
          owns_material = true_v;
        } else {
          log_warn("Scene: failed to acquire shape material '%.*s' after load",
                   (int)mat_name.length, mat_name.str);
        }
      } else {
        log_warn("Scene: failed to load shape material '%.*s': %d",
                 (int)mat_path.length, mat_path.str, (int)load_err);
      }
    } else if (mat_err == VKR_RENDERER_ERROR_NONE && acquired_mat.id != 0) {
      mat = acquired_mat;
      owns_material = true_v;
    } else {
      log_warn("Scene: failed to acquire shape material '%.*s'",
               (int)mat_name.length, mat_name.str);
    }

    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    // No material specified - create a colored material for this shape
    char mat_name[64];
    snprintf(mat_name, sizeof(mat_name), "__shape_mat_%u_%u",
             entity.parts.index, entity.parts.generation);

    VkrRendererError mat_err = VKR_RENDERER_ERROR_NONE;
    VkrMaterialHandle colored_mat = vkr_material_system_create_colored(
        &rf->material_system, mat_name, config->color, &mat_err);
    if (mat_err == VKR_RENDERER_ERROR_NONE && colored_mat.id != 0) {
      mat = colored_mat;
      owns_material = true_v;
    } else {
      log_warn("Scene: failed to create colored material for shape, using "
               "default");
    }
  }

  // Create submesh descriptor
  VkrSubMeshDesc submesh_desc = {
      .geometry = geom,
      .material = mat,
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
      .owns_geometry = true_v,
      .owns_material = owns_material,
  };

  // Add to mesh manager
  VkrMeshDesc mesh_desc = {
      .transform = mesh_transform,
      .submeshes = &submesh_desc,
      .submesh_count = 1,
  };

  uint32_t mesh_index = VKR_INVALID_ID;
  VkrRendererError mesh_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_add(&rf->mesh_manager, &mesh_desc, &mesh_index,
                            &mesh_err)) {
    String8 err_str = vkr_renderer_get_error_string(mesh_err);
    log_error("Scene: failed to add shape to mesh manager: %s",
              string8_cstr(&err_str));
    if (owns_material) {
      vkr_material_system_release(&rf->material_system, mat);
    }
    vkr_geometry_system_release(&rf->geometry_system, geom);
    if (out_error)
      *out_error = VKR_SCENE_ERROR_MESH_LOAD_FAILED;
    return false_v;
  }

  // Track mesh ownership
  if (!vkr_scene_track_mesh(scene, mesh_index, out_error)) {
    // mesh_manager_remove handles geometry/material release via owns_* flags
    vkr_mesh_manager_remove(&rf->mesh_manager, mesh_index);
    return false_v;
  }

  // Add shape component
  SceneShape comp = {
      .type = config->type,
      .dimensions = config->dimensions,
      .color = config->color,
      .mesh_index = mesh_index,
  };

  if (!vkr_entity_add_component(scene->world, entity, scene->comp_shape,
                                &comp)) {
    vkr_scene_release_mesh(scene, mesh_index);
    // mesh_manager_remove handles geometry/material release via owns_* flags
    vkr_mesh_manager_remove(&rf->mesh_manager, mesh_index);
    if (out_error)
      *out_error = VKR_SCENE_ERROR_COMPONENT_ADD_FAILED;
    return false_v;
  }

  // Shapes use mesh-slot indices. Set up render_id, visibility, and model
  // on the mesh for picking to work. Transform sync requires a query for
  // shapes.
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

  scene_invalidate_queries(scene);

  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return true_v;
}

const SceneShape *vkr_scene_get_shape(const VkrScene *scene,
                                      VkrEntityId entity) {
  if (!scene || !scene->world)
    return NULL;
  return (const SceneShape *)vkr_entity_get_component(scene->world, entity,
                                                      scene->comp_shape);
}

// ============================================================================
// Entity Lookup
// ============================================================================

/**
 * @brief Context for finding entity by name.
 */
typedef struct FindEntityByNameCtx {
  VkrScene *scene;
  String8 target_name;
  VkrEntityId result;
} FindEntityByNameCtx;

vkr_internal void find_entity_by_name_cb(const VkrArchetype *arch,
                                         VkrChunk *chunk, void *user) {
  (void)arch;
  FindEntityByNameCtx *ctx = (FindEntityByNameCtx *)user;

  // Already found
  if (ctx->result.u64 != VKR_ENTITY_ID_INVALID.u64)
    return;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneName *names =
      (SceneName *)vkr_entity_chunk_column(chunk, ctx->scene->comp_name);

  if (!names)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (string8_equals(&names[i].name, &ctx->target_name)) {
      ctx->result = entities[i];
      return;
    }
  }
}

VkrEntityId vkr_scene_find_entity_by_name(const VkrScene *scene, String8 name) {
  if (!scene || !scene->world || name.length == 0)
    return VKR_ENTITY_ID_INVALID;

  // Build query for entities with name component
  VkrQuery q_names;
  vkr_entity_query_build(scene->world, &scene->comp_name, 1, NULL, 0, &q_names);

  VkrQueryCompiled compiled;
  // Note: cast away const for query compilation (doesn't modify scene state)
  if (!vkr_entity_query_compile(scene->world, &q_names,
                                (VkrAllocator *)scene->alloc, &compiled)) {
    return VKR_ENTITY_ID_INVALID;
  }

  FindEntityByNameCtx ctx = {
      .scene = (VkrScene *)scene,
      .target_name = name,
      .result = VKR_ENTITY_ID_INVALID,
  };

  vkr_entity_query_compiled_each_chunk(&compiled, find_entity_by_name_cb, &ctx);

  vkr_entity_query_compiled_destroy((VkrAllocator *)scene->alloc, &compiled);

  return ctx.result;
}
