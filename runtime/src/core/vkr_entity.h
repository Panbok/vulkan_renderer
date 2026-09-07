#pragma once

/**
 * @file vkr_entity.h
 * @brief Entity system header file.
 * @note ECS is archetype-oriented (SoA) with dense chunked storage.
 */

#include "containers/array.h"
#include "containers/vkr_hashtable.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

/**
 * @brief Entity update class
 * @note This is used to determine the update frequency of the entity.
 */
typedef enum VkrEntityUpdateClass {
  VKR_ENTITY_UPDATE_CLASS_FROZEN = 0, // never updates (baked or disabled)
  VKR_ENTITY_UPDATE_CLASS_COLD = 1,   // infrequent updates
  VKR_ENTITY_UPDATE_CLASS_HOT = 2,    // frequent updates (per frame)
} VkrEntityUpdateClass;

/**
 * @brief Entity flags
 * @note This is used to determine the state of the entity.
 */
typedef enum VkrEntityBits {
  VKR_ENTITY_FLAG_NONE = 0u,
  VKR_ENTITY_FLAG_VISIBLE = 1u << 0,         // renderable and not hidden
  VKR_ENTITY_FLAG_STATIC = 1u << 1,          // rarely/never moves
  VKR_ENTITY_FLAG_DISABLED = 1u << 2,        // excluded from systems
  VKR_ENTITY_FLAG_PENDING_DESTROY = 1u << 3, // scheduled for removal
  VKR_ENTITY_FLAG_DIRTY_XFORM = 1u << 4,     // transform needs update
} VkrEntityBits;
typedef Bitset8 VkrEntityFlags;

/**
 * @brief Entity ID, 64-bit stable ID = [world:16 | generation:16 | index:32]
 */
typedef union VkrEntityId {
  uint64_t u64;
  struct {
    uint32_t index;
    uint16_t generation;
    uint16_t world; // 0 if you have a single world/scene
  } parts;
} VkrEntityId;
Array(VkrEntityId);

#define VKR_ENTITY_ID_INVALID (VkrEntityId){.u64 = 0}

/**
 * @brief Tunables
 */
#ifndef VKR_ECS_MAX_COMPONENTS
#define VKR_ECS_MAX_COMPONENTS 256u
#endif

#ifndef VKR_ECS_CHUNK_SIZE
#define VKR_ECS_CHUNK_SIZE KB(16) // 16 KB chunks by default
#endif

typedef uint16_t VkrComponentTypeId;
#define VKR_COMPONENT_TYPE_INVALID ((VkrComponentTypeId)0xFFFFu)
#define VKR_ENTITY_TYPE_TO_COL_INVALID ((uint16_t)0xFFFFu)

#define VKR_SIG_WORDS 4

/**
 * @brief Signature = 256-bit bitset for component presence
 */
typedef struct VkrSignature {
  uint64_t bits[VKR_SIG_WORDS]; // VKR_SIG_WORDS * 64 = 256
} VkrSignature;

/**
 * @brief Component info
 */
typedef struct VkrComponentInfo {
  const char *name;
  uint32_t size;
  uint32_t align;
} VkrComponentInfo;

/**
 * @brief Chunk (SoA block for an archetype)
 */
typedef struct VkrChunk {
  struct VkrArchetype *arch;
  uint8_t *data;     // chunk memory block
  VkrEntityId *ents; // pointer to entity column
  uint32_t count;    // rows used
  uint32_t capacity; // rows capacity
  void **columns;    // [arch->comp_count] base pointers
  struct VkrChunk *next;
} VkrChunk;

/**
 * @brief Entity record (directory entry)
 */
typedef struct VkrEntityRecord {
  struct VkrChunk *chunk;
  uint32_t slot;
} VkrEntityRecord;

/**
 * @brief Entity directory
 */
typedef struct VkrEntityDir {
  VkrEntityRecord *records; // index -> {chunk, slot}
  uint16_t *generations;    // index -> generation
  uint32_t capacity;
  uint32_t living; // count of allocated indices (high-water mark)
  uint32_t *free_indices;
  uint32_t free_count;
  uint32_t free_capacity;
} VkrEntityDir;

/**
 * @brief Archetype
 */
typedef struct VkrArchetype {
  struct VkrWorld *world;
  VkrSignature sig;

  uint32_t comp_count;
  VkrComponentTypeId *types; // [comp_count], sorted ascending
  uint32_t *sizes;           // cached from registry
  uint32_t *aligns;          // cached from registry
  uint32_t *col_offsets;     // [comp_count] offset within chunk->data
  uint16_t type_to_col[VKR_ECS_MAX_COMPONENTS]; // 0xFFFF = not present

  uint32_t ents_offset; // offset to entity-id column
  uint32_t chunk_capacity;

  VkrChunk *chunks; // singly-linked list
  const char *key;  // canonical string (lives in allocator)
} VkrArchetype;
VkrHashTableConstructor(struct VkrArchetype *, VkrArchetypePtr);

/**
 * @brief World
 */
typedef struct VkrWorld {
  VkrAllocator *alloc; // Main allocator for all persistent ECS allocations
  /**
   * @brief Optional scratch allocator for fast, short-lived temporary
   * allocations.
   *
   * Set this when you need a fast allocator optimized for transient or
   * per-frame allocations (e.g., temporary CPU/GPU upload buffers, staging
   * memory, temporary strings during archetype lookups, per-operation component
   * type arrays).
   *
   * If NULL, the system gracefully falls back to using `alloc` for all
   * allocations. The fallback behavior automatically handles both cases:
   * - When `scratch_alloc` is provided and differs from `alloc`, temporary
   * allocations use scoped allocations for automatic cleanup
   * - When `scratch_alloc` is NULL or matches `alloc`, temporary allocations
   * use manual free calls
   *
   * Use `scratch_alloc` for allocations with short lifetimes (single operation
   * or frame) that benefit from arena-style allocation or scope-based cleanup.
   * Use `alloc` for allocations that persist for the lifetime of the world
   * (archetypes, entity directory, component registry).
   */
  VkrAllocator *scratch_alloc;
  uint16_t world_id;

  // Component registry
  VkrComponentInfo *components;
  uint32_t comp_count;
  uint32_t comp_capacity;
  VkrHashTable_uint16_t component_name_to_id;

  // Archetype registry (string key -> archetype)
  VkrHashTable_VkrArchetypePtr arch_table;
  VkrArchetype **arch_list; // pointers to archetypes
  uint32_t arch_count;
  uint32_t arch_capacity;

  // Entity directory
  VkrEntityDir dir;
} VkrWorld;

/**
 * @brief World create info
 */
typedef struct VkrWorldCreateInfo {
  VkrAllocator *alloc; // allocator for all persistent ECS allocations
  /**
   * @brief Optional scratch allocator for fast, short-lived temporary
   * allocations.
   *
   * Set this when you need a fast allocator optimized for transient or
   * per-frame allocations (e.g., temporary CPU/GPU upload buffers, staging
   * memory, temporary strings during archetype lookups, per-operation component
   * type arrays).
   *
   * If NULL, the system gracefully falls back to using `alloc` for all
   * allocations. The fallback behavior automatically handles both cases:
   * - When `scratch_alloc` is provided and differs from `alloc`, temporary
   * allocations use scoped allocations for automatic cleanup
   * - When `scratch_alloc` is NULL or matches `alloc`, temporary allocations
   * use manual free calls
   *
   * Use `scratch_alloc` for allocations with short lifetimes (single operation
   * or frame) that benefit from arena-style allocation or scope-based cleanup.
   * Use `alloc` for allocations that persist for the lifetime of the world
   * (archetypes, entity directory, component registry).
   */
  VkrAllocator *scratch_alloc;
  uint16_t world_id; // world id to embed in VkrEntityId (0 if single world)
  // Optional initial capacities
  uint32_t initial_entities;   // e.g. 1024 (0 -> default)
  uint32_t initial_components; // e.g. 64 (0 -> default)
  uint32_t initial_archetypes; // e.g. 64 (0 -> default)
} VkrWorldCreateInfo;

// ================================
// World lifecycle
// ================================

/**
 * @brief Create a new world
 * @param info World create info
 * @return New world
 */
VkrWorld *vkr_entity_create_world(const VkrWorldCreateInfo *info);

/**
 * @brief Destroy a world
 * @param world World to destroy
 */
void vkr_entity_destroy_world(VkrWorld *world);

// ================================
// Components
// ================================

/**
 * @brief Register a new component
 * @param world World to register the component in
 * @param name Component name
 * @param size Component size
 * @param align Component alignment
 * @return Component type ID, or VKR_COMPONENT_TYPE_INVALID if name is already
 * registered
 * @note Returns VKR_COMPONENT_TYPE_INVALID when the name is already registered.
 *       For get-or-create behavior (register if missing, return existing if
 * present), use vkr_entity_register_component_once instead.
 * @see vkr_entity_register_component_once, vkr_entity_find_component
 */
VkrComponentTypeId vkr_entity_register_component(VkrWorld *world,
                                                 const char *name,
                                                 uint32_t size, uint32_t align);
/**
 * @brief Register component if missing and return its id.
 * @param world World to register the component in
 * @param name Component name
 * @param size Component size
 * @param align Component alignment
 * @return Component type ID, or VKR_COMPONENT_TYPE_INVALID if size/align
 * mismatch
 * @note Returns an existing component ID when the name is already registered
 * with matching size and alignment. Returns VKR_COMPONENT_TYPE_INVALID only
 * when the name exists but size/align differ (indicating a conflict).
 * @see vkr_entity_register_component, vkr_entity_find_component
 */
VkrComponentTypeId vkr_entity_register_component_once(VkrWorld *world,
                                                      const char *name,
                                                      uint32_t size,
                                                      uint32_t align);
/**
 * @brief Find a component id by name.
 * @param world World to search in
 * @param name Component name
 * @return Component id or VKR_COMPONENT_TYPE_INVALID if not found.
 */
VkrComponentTypeId vkr_entity_find_component(const VkrWorld *world,
                                             const char *name);

/**
 * @brief Get component info
 * @param world World to get the component info from
 * @param type Component type ID
 * @return Component info
 */
const VkrComponentInfo *vkr_entity_get_component_info(const VkrWorld *world,
                                                      VkrComponentTypeId type);

// ================================
// Entities
// ================================

/**
 * @brief Create a new entity
 * @param world World to create the entity in
 * @return Entity ID
 */
VkrEntityId vkr_entity_create_entity(VkrWorld *world);
/**
 * @brief Create a new entity with a component set.
 * @param world World to create the entity in
 * @param types Component type IDs
 * @param init_data Optional per-component init data (can be NULL)
 * @param count Number of component types
 * @return Entity ID
 * @note Duplicate component types are coalesced; the first non-NULL init data
 * is kept.
 */
VkrEntityId vkr_entity_create_entity_with_components(
    VkrWorld *world, const VkrComponentTypeId *types,
    const void *const *init_data, uint32_t count);

/**
 * @brief Destroy an entity
 * @param world World to destroy the entity in
 * @param id Entity ID
 * @return True if the entity was destroyed, false otherwise
 */
bool8_t vkr_entity_destroy_entity(VkrWorld *world, VkrEntityId id);

/**
 * @brief Check if an entity is alive (inline for hot paths).
 * @param world World to check the entity in (must not be NULL)
 * @param id Entity ID
 * @return True if the entity is alive, false otherwise
 */
vkr_internal INLINE bool8_t vkr_entity_is_alive(const VkrWorld *world,
                                                VkrEntityId id) {
  if (id.u64 == 0) {
    return false_v;
  }
  if (id.parts.world != world->world_id) {
    return false_v;
  }
  if (id.parts.index >= world->dir.capacity) {
    return false_v;
  }
  return world->dir.generations[id.parts.index] == id.parts.generation;
}

/**
 * @brief Check if entity ID matches a known-valid entity's current generation.
 *
 * Even faster than vkr_entity_is_alive when you know:
 * - The world is valid
 * - The entity index is within bounds
 * - The world ID matches
 *
 * Only checks generation. Use when iterating topo_order with pre-validated
 * indices.
 *
 * @param world World (must not be NULL)
 * @param id Entity ID to check
 * @return true if generation matches, false otherwise
 */
vkr_internal INLINE bool8_t vkr_entity_generation_valid(const VkrWorld *world,
                                                        VkrEntityId id) {
  return world->dir.generations[id.parts.index] == id.parts.generation;
}

// ================================
// Component Operations
// ================================

/**
 * @brief Add a component to an entity
 * @param world World to add the component to
 * @param id Entity ID
 * @param type Component type ID
 * @param init_data Initial data for the component
 * @return True if the component was added, false otherwise
 */
bool8_t vkr_entity_add_component(VkrWorld *world, VkrEntityId id,
                                 VkrComponentTypeId type,
                                 const void *init_data);

/**
 * @brief Remove a component from an entity
 * @param world World to remove the component from
 * @param id Entity ID
 * @param type Component type ID
 * @return True if the component was removed, false otherwise
 */
bool8_t vkr_entity_remove_component(VkrWorld *world, VkrEntityId id,
                                    VkrComponentTypeId type);

/**
 * @brief Unchecked component access for pre-validated entities.
 *
 * @pre Entity MUST be validated alive before calling.
 * @pre Component type MUST be valid and present in the entity's archetype.
 *
 * Skips all validation for maximum performance. Use only when:
 * - Entity was validated with vkr_entity_is_alive earlier in the same code path
 * - Component presence is guaranteed (e.g., from a compiled query that
 *   requires the component)
 *
 * @param world World (must not be NULL)
 * @param id Entity ID (must be alive)
 * @param type Component type (must be present)
 * @return Pointer to component data (never NULL if preconditions met)
 */
vkr_internal INLINE void *
vkr_entity_get_component_unchecked(VkrWorld *world, VkrEntityId id,
                                   VkrComponentTypeId type) {
  VkrEntityRecord rec = world->dir.records[id.parts.index];
  VkrArchetype *arch = rec.chunk->arch;
  uint16_t col_i = arch->type_to_col[type];
  uint8_t *col = (uint8_t *)rec.chunk->columns[col_i];
  return col + (size_t)arch->sizes[col_i] * rec.slot;
}

/**
 * @brief Unchecked const component access for pre-validated entities.
 *
 * Same as vkr_entity_get_component_unchecked but returns const pointer.
 *
 * @param world World (must not be NULL)
 * @param id Entity ID (must be alive)
 * @param type Component type (must be present)
 * @return Const pointer to component data
 */
vkr_internal INLINE const void *
vkr_entity_get_component_unchecked_const(const VkrWorld *world, VkrEntityId id,
                                         VkrComponentTypeId type) {
  VkrEntityRecord rec = world->dir.records[id.parts.index];
  const VkrArchetype *arch = rec.chunk->arch;
  uint16_t col_i = arch->type_to_col[type];
  const uint8_t *col = (const uint8_t *)rec.chunk->columns[col_i];
  return col + (size_t)arch->sizes[col_i] * rec.slot;
}

/**
 * @brief Combined is_alive + get_component in single validation pass (inline).
 *
 * Validates entity once and retrieves component, avoiding the common pattern:
 *   if (!vkr_entity_is_alive(world, id)) return;
 *   T* comp = vkr_entity_get_component_mut(world, id, type);  // re-validates!
 *
 * Returns NULL if entity is dead or component is not present.
 *
 * @param world World (must not be NULL)
 * @param id Entity ID
 * @param type Component type ID
 * @return Pointer to component, or NULL if entity dead or component missing
 */
vkr_internal INLINE void *
vkr_entity_get_component_if_alive(VkrWorld *world, VkrEntityId id,
                                  VkrComponentTypeId type) {
  // Single validation pass
  if (id.u64 == 0 || id.parts.world != world->world_id ||
      id.parts.index >= world->dir.capacity ||
      world->dir.generations[id.parts.index] != id.parts.generation) {
    return NULL;
  }

  VkrEntityRecord rec = world->dir.records[id.parts.index];
  if (!rec.chunk) {
    return NULL;
  }

  VkrArchetype *arch = rec.chunk->arch;
  uint16_t col_i = arch->type_to_col[type];
  if (col_i == VKR_ENTITY_TYPE_TO_COL_INVALID) {
    return NULL;
  }

  uint8_t *col = (uint8_t *)rec.chunk->columns[col_i];
  return col + (size_t)arch->sizes[col_i] * rec.slot;
}

/**
 * @brief Combined is_alive + get_component (const version, inline).
 *
 * @param world World (must not be NULL)
 * @param id Entity ID
 * @param type Component type ID
 * @return Const pointer to component, or NULL if entity dead or component
 * missing
 */
vkr_internal INLINE const void *
vkr_entity_get_component_if_alive_const(const VkrWorld *world, VkrEntityId id,
                                        VkrComponentTypeId type) {
  if (id.u64 == 0 || id.parts.world != world->world_id ||
      id.parts.index >= world->dir.capacity ||
      world->dir.generations[id.parts.index] != id.parts.generation) {
    return NULL;
  }

  VkrEntityRecord rec = world->dir.records[id.parts.index];
  if (!rec.chunk) {
    return NULL;
  }

  const VkrArchetype *arch = rec.chunk->arch;
  uint16_t col_i = arch->type_to_col[type];
  if (col_i == VKR_ENTITY_TYPE_TO_COL_INVALID) {
    return NULL;
  }

  const uint8_t *col = (const uint8_t *)rec.chunk->columns[col_i];
  return col + (size_t)arch->sizes[col_i] * rec.slot;
}

/**
 * @brief Check if entity is alive and has a specific component (inline).
 *
 * Combines validation and component presence check without retrieving data.
 *
 * @param world World (must not be NULL)
 * @param id Entity ID
 * @param type Component type ID
 * @return true if entity is alive AND has the component
 */
vkr_internal INLINE bool8_t vkr_entity_has_component_if_alive(
    const VkrWorld *world, VkrEntityId id, VkrComponentTypeId type) {
  if (id.u64 == 0 || id.parts.world != world->world_id ||
      id.parts.index >= world->dir.capacity ||
      world->dir.generations[id.parts.index] != id.parts.generation) {
    return false_v;
  }

  VkrEntityRecord rec = world->dir.records[id.parts.index];
  if (!rec.chunk) {
    return false_v;
  }

  const VkrArchetype *arch = rec.chunk->arch;
  return arch->type_to_col[type] != VKR_ENTITY_TYPE_TO_COL_INVALID;
}

/**
 * @brief Construct entity ID from index using world's current generation.
 *
 * Use when iterating indices (e.g., topo_order) and need full entity IDs.
 * The constructed ID is valid only if the generation matches.
 *
 * @param world World (must not be NULL)
 * @param index Entity index (must be < world->dir.capacity)
 * @return Entity ID with current generation from directory
 */
vkr_internal INLINE VkrEntityId vkr_entity_id_from_index(const VkrWorld *world,
                                                         uint32_t index) {
  VkrEntityId id;
  id.parts.index = index;
  id.parts.generation = world->dir.generations[index];
  id.parts.world = world->world_id;
  return id;
}

/**
 * @brief Get a mutable component (inline, validates entity).
 * @param world World to get the component from
 * @param id Entity ID
 * @param type Component type ID
 * @return Mutable component, or NULL if entity dead or component missing
 */
vkr_internal INLINE void *
vkr_entity_get_component_mut(VkrWorld *world, VkrEntityId id,
                             VkrComponentTypeId type) {
  return vkr_entity_get_component_if_alive(world, id, type);
}

/**
 * @brief Get a component (inline, validates entity).
 * @param world World to get the component from
 * @param id Entity ID
 * @param type Component type ID
 * @return Component, or NULL if entity dead or component missing
 */
vkr_internal INLINE const void *
vkr_entity_get_component(const VkrWorld *world, VkrEntityId id,
                         VkrComponentTypeId type) {
  return vkr_entity_get_component_if_alive_const(world, id, type);
}

/**
 * @brief Check if an entity has a component (inline, validates entity).
 * @param world World to check the component in
 * @param id Entity ID
 * @param type Component type ID
 * @return True if the entity is alive and has the component, false otherwise
 */
vkr_internal INLINE bool8_t vkr_entity_has_component(const VkrWorld *world,
                                                     VkrEntityId id,
                                                     VkrComponentTypeId type) {
  return vkr_entity_has_component_if_alive(world, id, type);
}

// ================================
// Query API
// ================================

/**
 * @brief Chunk function
 * @param arch Archetype
 * @param chunk Chunk
 * @param user User data
 */
typedef void (*VkrChunkFn)(const VkrArchetype *arch, VkrChunk *chunk,
                           void *user);

typedef struct VkrQuery {
  VkrSignature include;
  VkrSignature exclude;
} VkrQuery;

/**
 * @brief Compiled query snapshot of matching archetypes.
 *
 * Compiled queries cache a snapshot of archetypes that match the query criteria
 * at compile time. This provides fast iteration but becomes stale when new
 * archetypes are created in the world.
 *
 * **Invalidation:** A compiled query becomes invalid (stale) when:
 * - New archetypes are created via `vkr_entity_archetype_get_or_create()` or
 *   when entities are created with new component combinations
 * - The world's archetype count increases
 *
 * **When to recompile:**
 * - After creating entities with new component combinations
 * - After adding components to entities that create new archetypes
 * - Before using a compiled query if archetypes may have been created since
 *   the last compilation
 *
 * **When to use compiled vs non-compiled queries:**
 * - Use compiled queries (`vkr_entity_query_compile` +
 * `vkr_entity_query_compiled_each_chunk`) when the query is executed frequently
 * (e.g., every frame) and archetypes are stable (e.g., after initial scene
 * load).
 * - Use non-compiled queries (`vkr_entity_query_each_chunk`) when:
 *   - Archetypes are frequently created/destroyed
 *   - The query is executed infrequently
 *   - You want to avoid manual recompilation tracking
 *
 * **Lifecycle:**
 * - Compile with `vkr_entity_query_compile()` after building the query
 * - Use `vkr_entity_query_compiled_each_chunk()` to iterate
 * - Destroy with `vkr_entity_query_compiled_destroy()` when done
 * - Recompile if archetypes may have changed
 *
 * **Example:**
 * ```c
 * // Build query
 * VkrQuery query;
 * VkrComponentTypeId include[] = {TRANSFORM_TYPE, RENDER_TYPE};
 * vkr_entity_query_build(world, include, 2, NULL, 0, &query);
 *
 * // Compile once after scene load (when archetypes are stable)
 * VkrQueryCompiled compiled;
 * if (!vkr_entity_query_compile(world, &query, allocator, &compiled)) {
 *   // handle error
 * }
 *
 * // Use compiled query in hot path (e.g., render loop)
 * vkr_entity_query_compiled_each_chunk(&compiled, render_chunk_fn, user);
 *
 * // If new archetypes are created, recompile:
 * vkr_entity_query_compiled_destroy(allocator, &compiled);
 * vkr_entity_query_compile(world, &query, allocator, &compiled);
 *
 * // Cleanup
 * vkr_entity_query_compiled_destroy(allocator, &compiled);
 * ```
 *
 * @note In debug builds, `vkr_entity_query_compiled_each_chunk()` asserts if
 * the query appears stale (world's archetype count increased since
 * compilation).
 */
typedef struct VkrQueryCompiled {
  VkrArchetype **archetypes;
  uint32_t archetype_count;
#ifdef VKR_DEBUG
  uint32_t world_arch_count_at_compile; ///< Debug: world's archetype count at
                                        ///< compile time
#endif
} VkrQueryCompiled;

/**
 * @brief Build a query from component type signatures.
 *
 * Constructs a query that matches entities with the specified include/exclude
 * component signatures. The query can be used directly with
 * `vkr_entity_query_each_chunk()` or compiled for repeated use with
 * `vkr_entity_query_compile()`.
 *
 * **Query invalidation:** Built queries (`VkrQuery`) are not invalidated by
 * archetype changes; they are evaluated dynamically. However, compiled queries
 * (`VkrQueryCompiled`) become stale when new archetypes are created and must
 * be recompiled.
 *
 * @param world World to build the query in
 * @param include_types Component types that entities must have (all required)
 * @param include_count Number of include types
 * @param exclude_types Component types that entities must not have (any
 * excluded)
 * @param exclude_count Number of exclude types
 * @param out_query Output query structure
 *
 * @example
 * ```c
 * // Query entities with Transform and Render components, but not Hidden
 * VkrQuery query;
 * VkrComponentTypeId include[] = {TRANSFORM_TYPE, RENDER_TYPE};
 * VkrComponentTypeId exclude[] = {HIDDEN_TYPE};
 * vkr_entity_query_build(world, include, 2, exclude, 1, &query);
 * ```
 */
void vkr_entity_query_build(VkrWorld *world,
                            const VkrComponentTypeId *include_types,
                            uint32_t include_count,
                            const VkrComponentTypeId *exclude_types,
                            uint32_t exclude_count, VkrQuery *out_query);

/**
 * @brief Compile a query into a snapshot of matching archetypes.
 *
 * Creates a compiled query that caches the list of archetypes matching the
 * query criteria at the time of compilation. This snapshot becomes stale when
 * new archetypes are created in the world.
 *
 * **Invalidation requirements:**
 * - The compiled query becomes invalid when `world->arch_count` increases
 *   (new archetypes are created).
 * - You must recompile after:
 *   - Creating entities with new component combinations
 *   - Adding components that create new archetypes
 *   - Any operation that calls `vkr_entity_archetype_get_or_create()` with
 *     a new signature
 *
 * **When to compile:**
 * - After initial scene/entity setup when archetypes are stable
 * - After batch entity creation operations
 * - Before using in hot paths (e.g., render loops) where archetypes won't
 * change
 *
 * **Memory management:**
 * - The compiled query allocates memory for the archetype pointer array
 * - Use `vkr_entity_query_compiled_destroy()` to free the memory
 * - The allocator must outlive the compiled query's usage
 *
 * @param world World to compile the query against
 * @param query Query to compile (built with `vkr_entity_query_build()`)
 * @param allocator Allocator for the archetype array (must persist until
 * destroy)
 * @param out_query Output compiled query (must be destroyed when done)
 * @return `true_v` on success, `false_v` on allocation failure
 *
 * @example
 * ```c
 * VkrQuery query;
 * vkr_entity_query_build(world, include_types, include_count, NULL, 0, &query);
 *
 * VkrQueryCompiled compiled;
 * if (!vkr_entity_query_compile(world, &query, allocator, &compiled)) {
 *   log_error("Failed to compile query");
 *   return;
 * }
 *
 * // Use compiled query...
 * vkr_entity_query_compiled_each_chunk(&compiled, fn, user);
 *
 * // Recompile if archetypes changed:
 * if (archetypes_may_have_changed) {
 *   vkr_entity_query_compiled_destroy(allocator, &compiled);
 *   vkr_entity_query_compile(world, &query, allocator, &compiled);
 * }
 *
 * // Cleanup
 * vkr_entity_query_compiled_destroy(allocator, &compiled);
 * ```
 */
bool8_t vkr_entity_query_compile(VkrWorld *world, const VkrQuery *query,
                                 VkrAllocator *allocator,
                                 VkrQueryCompiled *out_query);

/**
 * @brief Destroy a compiled query and free its archetype list.
 *
 * Frees the memory allocated by `vkr_entity_query_compile()` for the archetype
 * pointer array. The query structure is zeroed after destruction.
 *
 * **Lifecycle:** Always pair `vkr_entity_query_compile()` with a matching
 * `vkr_entity_query_compiled_destroy()` call to avoid memory leaks.
 *
 * @param allocator Allocator used to create the compiled query
 * @param query Compiled query to destroy (can be NULL, safe to call multiple
 * times)
 *
 * @example
 * ```c
 * VkrQueryCompiled compiled;
 * vkr_entity_query_compile(world, &query, allocator, &compiled);
 * // ... use compiled query ...
 * vkr_entity_query_compiled_destroy(allocator, &compiled);
 * ```
 */
void vkr_entity_query_compiled_destroy(VkrAllocator *allocator,
                                       VkrQueryCompiled *query);

/**
 * @brief Iterate over all chunks in a compiled query.
 *
 * Iterates through all chunks in the archetypes cached by the compiled query.
 * This is faster than `vkr_entity_query_each_chunk()` because it avoids
 * re-evaluating the query against all archetypes each time.
 *
 * **Staleness detection:** In debug builds, this function asserts if the
 * compiled query appears stale (world's archetype count increased since
 * compilation). In release builds, stale queries may silently miss new
 * archetypes.
 *
 * **When to use:** Use compiled queries when:
 * - The query is executed frequently (e.g., every frame in render loops)
 * - Archetypes are stable (e.g., after initial scene load)
 * - You can track when to recompile (after entity creation operations)
 *
 * **When to prefer non-compiled:** Use `vkr_entity_query_each_chunk()` when:
 * - Archetypes change frequently
 * - The query is executed infrequently
 * - You want automatic up-to-date results without manual recompilation
 *
 * @param query Compiled query to iterate (must be valid and not stale)
 * @param fn Callback function called for each matching chunk
 * @param user User data passed to the callback
 *
 * @example
 * ```c
 * void render_chunk(const VkrArchetype *arch, VkrChunk *chunk, void *user) {
 *   Transform *transforms = vkr_entity_chunk_column(chunk, TRANSFORM_TYPE);
 *   Render *renders = vkr_entity_chunk_column(chunk, RENDER_TYPE);
 *   uint32_t count = vkr_entity_chunk_count(chunk);
 *   for (uint32_t i = 0; i < count; ++i) {
 *     // render entity...
 *   }
 * }
 *
 * VkrQueryCompiled compiled;
 * vkr_entity_query_compile(world, &query, allocator, &compiled);
 * vkr_entity_query_compiled_each_chunk(&compiled, render_chunk, NULL);
 * ```
 */
void vkr_entity_query_compiled_each_chunk(const VkrQueryCompiled *query,
                                          VkrChunkFn fn, void *user);

/**
 * @brief Query each chunk matching the query criteria (non-compiled path).
 *
 * Iterates through all chunks in the world that match the query, evaluating
 * the query dynamically against all archetypes. This always returns up-to-date
 * results but is slower than compiled queries for repeated iterations.
 *
 * **Advantages over compiled queries:**
 * - Always up-to-date (no staleness concerns)
 * - No manual recompilation needed
 * - Simpler lifecycle (no compile/destroy calls)
 *
 * **When to use:**
 * - Archetypes change frequently
 * - Query is executed infrequently
 * - You want to avoid tracking when to recompile
 * - One-off queries or queries in non-hot paths
 *
 * **When to prefer compiled queries:** Use `vkr_entity_query_compile()` +
 * `vkr_entity_query_compiled_each_chunk()` when:
 * - Query is executed every frame or in hot paths
 * - Archetypes are stable after initial setup
 * - Performance is critical
 *
 * @param world World to query the chunks in
 * @param query Query to evaluate (built with `vkr_entity_query_build()`)
 * @param fn Function to call for each matching chunk
 * @param user User data passed to the callback
 *
 * @example
 * ```c
 * VkrQuery query;
 * VkrComponentTypeId include[] = {TRANSFORM_TYPE, RENDER_TYPE};
 * vkr_entity_query_build(world, include, 2, NULL, 0, &query);
 *
 * void process_chunk(const VkrArchetype *arch, VkrChunk *chunk, void *user) {
 *   // process entities in chunk...
 * }
 *
 * // Always up-to-date, no recompilation needed
 * vkr_entity_query_each_chunk(world, &query, process_chunk, NULL);
 * ```
 */
void vkr_entity_query_each_chunk(VkrWorld *world, const VkrQuery *query,
                                 VkrChunkFn fn, void *user);

// ================================
// Chunk accessors
// ================================

/**
 * @brief Get the count of entities in a chunk
 * @param chunk Chunk
 * @return Count of entities
 */
uint32_t vkr_entity_chunk_count(const VkrChunk *chunk);

/**
 * @brief Get the entities in a chunk
 * @param chunk Chunk
 * @return Entities
 */
VkrEntityId *vkr_entity_chunk_entities(VkrChunk *chunk);

/**
 * @brief Get a column from a chunk
 * @param chunk Chunk
 * @param type Component type ID
 * @return Column
 */
void *vkr_entity_chunk_column(VkrChunk *chunk, VkrComponentTypeId type);

/**
 * @brief Get a column from a chunk
 * @param chunk Chunk
 * @param type Component type ID
 * @return Column
 */
const void *vkr_entity_chunk_column_const(const VkrChunk *chunk,
                                          VkrComponentTypeId type);

/**
 * @brief Get the archetype of a chunk
 * @param chunk Chunk
 * @return Archetype
 */
const VkrArchetype *vkr_entity_chunk_archetype(const VkrChunk *chunk);

/**
 * @brief Get the signature of an archetype
 * @param arch Archetype
 * @return Signature
 */
const VkrSignature *vkr_entity_archetype_signature(const VkrArchetype *arch);

/**
 * @brief Get the component count of an archetype
 * @param arch Archetype
 * @return Component count
 */
uint32_t vkr_entity_archetype_component_count(const VkrArchetype *arch);

/**
 * @brief Get a component at an index of an archetype
 * @param arch Archetype
 * @param idx Index
 * @return Component
 */
VkrComponentTypeId vkr_entity_archetype_component_at(const VkrArchetype *arch,
                                                     uint32_t idx);
