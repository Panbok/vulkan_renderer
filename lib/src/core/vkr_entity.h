#pragma once

/**
 * @file vkr_entity.h
 * @brief Entity system header file.
 * @note ECS is archetype-oriented (SoA) with dense chunked storage.
 */

#include "containers/array.h"
#include "containers/vkr_hashtable.h"
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
  VkrAllocator *alloc;
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
  VkrAllocator *alloc; // allocator for all ECS allocations
  VkrAllocator *scratch_alloc; // optional scratch allocator
  uint16_t world_id;   // world id to embed in VkrEntityId (0 if single world)
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
 * @return Component type ID
 * @note Returns VKR_COMPONENT_TYPE_INVALID if name is already registered.
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
 * @return Component type ID
 * @note If name is registered with a different size/alignment, returns
 * VKR_COMPONENT_TYPE_INVALID.
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
 * @brief Check if an entity is alive
 * @param world World to check the entity in
 * @param id Entity ID
 * @return True if the entity is alive, false otherwise
 */
bool8_t vkr_entity_is_alive(const VkrWorld *world, VkrEntityId id);

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
 * @brief Get a mutable component
 * @param world World to get the component from
 * @param id Entity ID
 * @param type Component type ID
 * @return Mutable component
 */
void *vkr_entity_get_component_mut(VkrWorld *world, VkrEntityId id,
                                   VkrComponentTypeId type);

/**
 * @brief Get a component
 * @param world World to get the component from
 * @param id Entity ID
 * @param type Component type ID
 * @return Component
 */
const void *vkr_entity_get_component(const VkrWorld *world, VkrEntityId id,
                                     VkrComponentTypeId type);

/**
 * @brief Check if an entity has a component
 * @param world World to check the component in
 * @param id Entity ID
 * @param type Component type ID
 * @return True if the entity has the component, false otherwise
 */
bool8_t vkr_entity_has_component(const VkrWorld *world, VkrEntityId id,
                                 VkrComponentTypeId type);

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
 * @note Recompile after new archetypes are introduced.
 */
typedef struct VkrQueryCompiled {
  VkrArchetype **archetypes;
  uint32_t archetype_count;
} VkrQueryCompiled;

/**
 * @brief Build a query
 * @param world World to build the query in
 * @param include_types Types to include
 * @param include_count Number of include types
 * @param exclude_types Types to exclude
 * @param exclude_count Number of exclude types
 * @param out_query Query to build
 */
void vkr_entity_query_build(VkrWorld *world,
                            const VkrComponentTypeId *include_types,
                            uint32_t include_count,
                            const VkrComponentTypeId *exclude_types,
                            uint32_t exclude_count, VkrQuery *out_query);

/**
 * @brief Compile a query into a snapshot of matching archetypes.
 * @note Recompile after new archetypes are introduced.
 */
bool8_t vkr_entity_query_compile(VkrWorld *world, const VkrQuery *query,
                                 VkrAllocator *allocator,
                                 VkrQueryCompiled *out_query);

/**
 * @brief Destroy a compiled query and free its archetype list.
 */
void vkr_entity_query_compiled_destroy(VkrAllocator *allocator,
                                       VkrQueryCompiled *query);

/**
 * @brief Iterate over all chunks in a compiled query.
 */
void vkr_entity_query_compiled_each_chunk(const VkrQueryCompiled *query,
                                          VkrChunkFn fn, void *user);

/**
 * @brief Query each chunk
 * @param world World to query the chunks in
 * @param query Query to use
 * @param fn Function to call for each chunk
 * @param user User data
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
