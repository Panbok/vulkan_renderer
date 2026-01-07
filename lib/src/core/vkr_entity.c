#include "core/vkr_entity.h"
#include "containers/str.h"
#include "defines.h"
#include "math/vkr_math.h"

#define VKR_ENTITY_DIR_INITIAL_CAPACITY 1024u
#define VKR_ENTITY_DIR_GROW_FACTOR 2u
#define VKR_ENTITY_DIR_GROW_MIN_CAPACITY 8u
#define VKR_ENTITY_SIGNATURE_TYPE_WORD(typeId) (typeId >> 6) // /64
#define VKR_ENTITY_SIGNATURE_TYPE_SHIFT(typeId)                                \
  (1ull << VKR_ENTITY_SIGNATURE_TYPE_BIT(typeId))            // unsigned shift
#define VKR_ENTITY_SIGNATURE_TYPE_BIT(typeId) (typeId & 63u) // %64
#define VKR_ENTITY_COMP_INITIAL_CAPACITY 64u
#define VKR_ENTITY_ARCH_KEY_SIZE 3
#define VKR_ENTITY_TYPE_TO_COL_INVALID ((uint16_t)0xFFFFu)
#define VKR_ENTITY_ARCH_INITIAL_CAPACITY 16u

// ----------------------
// Small helpers
// ----------------------

vkr_internal INLINE VkrEntityId vkr_entity_id_make(uint32_t index,
                                                   uint16_t generation,
                                                   uint16_t world) {
  VkrEntityId id;
  id.parts.index = index;
  id.parts.generation = generation;
  id.parts.world = world;
  return id;
}

vkr_internal INLINE uint64_t vkr_entity_align_up_u64(uint64_t value,
                                                     uint32_t alignment) {
  assert_log(alignment != 0u && (alignment & (alignment - 1u)) == 0u,
             "Alignment must be power-of-two");
  uint64_t mask = (uint64_t)alignment - 1u;
  return (value + mask) & ~mask;
}

vkr_internal INLINE bool8_t vkr_entity_validate_id(const VkrWorld *world,
                                                   VkrEntityId id) {
  if (!world || id.u64 == 0) {
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

vkr_internal INLINE bool8_t vkr_entity_validate_type(const VkrWorld *world,
                                                     VkrComponentTypeId type) {
  return world && type != VKR_COMPONENT_TYPE_INVALID &&
         type < world->comp_count;
}

vkr_internal INLINE void vkr_entity_sig_clear(VkrSignature *signature) {
  assert_log(signature, "Signature must not be NULL");
  signature->bits[0] = signature->bits[1] = signature->bits[2] =
      signature->bits[3] = 0ull;
}

vkr_internal INLINE void vkr_entity_sig_set(VkrSignature *signature,
                                            VkrComponentTypeId typeId) {
  assert_log(signature, "Signature must not be NULL");
  assert_log(typeId < VKR_ECS_MAX_COMPONENTS, "Component id out of range");
  signature->bits[VKR_ENTITY_SIGNATURE_TYPE_WORD(typeId)] |=
      VKR_ENTITY_SIGNATURE_TYPE_SHIFT(typeId);
}

vkr_internal INLINE bool32_t vkr_entity_sig_has(const VkrSignature *signature,
                                                VkrComponentTypeId typeId) {
  assert_log(typeId < VKR_ECS_MAX_COMPONENTS, "Component id out of range");
  return (signature->bits[VKR_ENTITY_SIGNATURE_TYPE_WORD(typeId)] &
          VKR_ENTITY_SIGNATURE_TYPE_SHIFT(typeId)) != 0ull
             ? true_v
             : false_v;
}

vkr_internal INLINE bool32_t vkr_entity_sig_contains(const VkrSignature *sigA,
                                                     const VkrSignature *sigB) {
  assert_log(sigA, "Signature A must not be NULL");
  assert_log(sigB, "Signature B must not be NULL");
  // sigA contains sigB if (sigA & sigB) == sigB
  for (int word = 0; word < VKR_SIG_WORDS; ++word) {
    if ((sigA->bits[word] & sigB->bits[word]) != sigB->bits[word])
      return false_v;
  }
  return true_v;
}

vkr_internal INLINE bool32_t
vkr_entity_sig_intersects(const VkrSignature *sigA, const VkrSignature *sigB) {
  assert_log(sigA, "Signature A must not be NULL");
  assert_log(sigB, "Signature B must not be NULL");
  for (int word = 0; word < VKR_SIG_WORDS; ++word) {
    if ((sigA->bits[word] & sigB->bits[word]) != 0ull)
      return true_v;
  }
  return false_v;
}

vkr_internal INLINE void *vkr_entity_alloc(VkrWorld *world, uint64_t size,
                                           VkrAllocatorMemoryTag tag) {
  assert_log(world, "World must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  return vkr_allocator_alloc(world->alloc, size, tag);
}

vkr_internal INLINE void *vkr_entity_realloc(VkrWorld *world, void *p,
                                             uint64_t old_size,
                                             uint64_t new_size,
                                             VkrAllocatorMemoryTag tag) {
  assert_log(world, "World must not be NULL");
  assert_log(p, "Pointer must not be NULL");
  assert_log(old_size > 0, "Old size must be greater than 0");
  assert_log(new_size > 0, "New size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  return vkr_allocator_realloc(world->alloc, p, old_size, new_size, tag);
}

vkr_internal INLINE void vkr_entity_free(VkrWorld *world, void *p,
                                         uint64_t size,
                                         VkrAllocatorMemoryTag tag) {
  assert_log(world, "World must not be NULL");
  assert_log(p, "Pointer must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  vkr_allocator_free(world->alloc, p, size, tag);
}

vkr_internal INLINE bool32_t vkr_entity_ensure_capacity_ptr(void ***arr,
                                                            uint32_t *cap,
                                                            uint32_t need,
                                                            VkrWorld *world) {
  assert_log(arr && *arr != (void **)0x1, "Array must be valid");
  assert_log(cap, "Capacity must not be NULL");
  assert_log(need > 0, "Need must be greater than 0");
  assert_log(world, "World must not be NULL");

  if (*cap >= need)
    return true_v;

  uint32_t new_cap = (*cap == 0) ? VKR_ENTITY_DIR_GROW_MIN_CAPACITY
                                 : (*cap * VKR_ENTITY_DIR_GROW_FACTOR);
  while (new_cap < need)
    new_cap *= VKR_ENTITY_DIR_GROW_FACTOR;

  void **new_data = NULL;
  if (*cap == 0 || *arr == NULL) {
    new_data = (void **)vkr_entity_alloc(world, new_cap * sizeof(void *),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    new_data = (void **)vkr_entity_realloc(
        world, (void *)*arr, (*cap) * sizeof(void *), new_cap * sizeof(void *),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (!new_data)
    return false_v;

  *arr = new_data;
  *cap = new_cap;
  return true_v;
}

vkr_internal INLINE bool32_t vkr_entity_ensure_capacity_u32(uint32_t **arr,
                                                            uint32_t *cap,
                                                            uint32_t need,
                                                            VkrWorld *world) {
  assert_log(arr, "Array must not be NULL");
  assert_log(cap, "Capacity must not be NULL");
  assert_log(need > 0, "Need must be greater than 0");
  assert_log(world, "World must not be NULL");

  if (*cap >= need)
    return true_v;

  uint32_t new_cap = (*cap == 0) ? VKR_ENTITY_DIR_GROW_MIN_CAPACITY
                                 : (*cap * VKR_ENTITY_DIR_GROW_FACTOR);
  while (new_cap < need)
    new_cap *= VKR_ENTITY_DIR_GROW_FACTOR;

  uint32_t *new_data = NULL;
  if (*cap == 0 || *arr == NULL) {
    new_data = vkr_entity_alloc(world, new_cap * sizeof(uint32_t),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    new_data = vkr_entity_realloc(world, *arr, (*cap) * sizeof(uint32_t),
                                  new_cap * sizeof(uint32_t),
                                  VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (!new_data)
    return false_v;

  *arr = new_data;
  *cap = new_cap;
  return true_v;
}

vkr_internal INLINE bool32_t vkr_entity_ensure_capacity_u16(uint16_t **arr,
                                                            uint32_t *cap,
                                                            uint32_t need,
                                                            VkrWorld *world) {
  assert_log(arr, "Array must not be NULL");
  assert_log(cap, "Capacity must not be NULL");
  assert_log(need > 0, "Need must be greater than 0");
  assert_log(world, "World must not be NULL");

  if (*cap >= need)
    return true_v;

  uint32_t new_cap = (*cap == 0) ? VKR_ENTITY_DIR_GROW_MIN_CAPACITY
                                 : (*cap * VKR_ENTITY_DIR_GROW_FACTOR);
  while (new_cap < need)
    new_cap *= VKR_ENTITY_DIR_GROW_FACTOR;

  uint16_t *new_data = NULL;
  if (*cap == 0 || *arr == NULL) {
    new_data = vkr_entity_alloc(world, new_cap * sizeof(uint16_t),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    new_data = vkr_entity_realloc(world, *arr, (*cap) * sizeof(uint16_t),
                                  new_cap * sizeof(uint16_t),
                                  VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (!new_data)
    return false_v;

  *arr = new_data;
  *cap = new_cap;
  return true_v;
}

// ----------------------
// Directory & registry
// ----------------------

vkr_internal INLINE bool32_t vkr_entity_dir_init(VkrWorld *world,
                                                 uint32_t initial) {
  assert_log(world, "World must not be NULL");

  VkrEntityDir *dir = &world->dir;
  dir->capacity = initial ? initial : VKR_ENTITY_DIR_INITIAL_CAPACITY;
  dir->living = 0;
  dir->free_count = 0;
  dir->free_capacity = 0;

  dir->records =
      vkr_entity_alloc(world, dir->capacity * sizeof(VkrEntityRecord),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  dir->generations = vkr_entity_alloc(world, dir->capacity * sizeof(uint16_t),
                                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  dir->free_indices = NULL;
  if (!dir->records || !dir->generations)
    return false_v;

  MemZero(dir->records, dir->capacity * sizeof(VkrEntityRecord));
  MemZero(dir->generations, dir->capacity * sizeof(uint16_t));

  return true_v;
}

vkr_internal INLINE void vkr_entity_dir_grow(VkrWorld *world) {
  assert_log(world, "World must not be NULL");

  VkrEntityDir *dir = &world->dir;
  uint32_t old_cap = dir->capacity;
  uint32_t new_cap = old_cap * VKR_ENTITY_DIR_GROW_FACTOR;

  VkrEntityRecord *new_records = vkr_entity_alloc(
      world, new_cap * sizeof(VkrEntityRecord), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint16_t *new_generations = vkr_entity_alloc(
      world, new_cap * sizeof(uint16_t), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!new_records || !new_generations) {
    if (new_records) {
      vkr_entity_free(world, new_records, new_cap * sizeof(VkrEntityRecord),
                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    if (new_generations) {
      vkr_entity_free(world, new_generations, new_cap * sizeof(uint16_t),
                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    log_error("Failed to grow entity directory");
    return;
  }

  MemCopy(new_records, dir->records, old_cap * sizeof(VkrEntityRecord));
  MemCopy(new_generations, dir->generations, old_cap * sizeof(uint16_t));
  MemZero(new_records + old_cap, (new_cap - old_cap) * sizeof(VkrEntityRecord));
  MemZero(new_generations + old_cap, (new_cap - old_cap) * sizeof(uint16_t));

  vkr_entity_free(world, dir->records, old_cap * sizeof(VkrEntityRecord),
                  VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_entity_free(world, dir->generations, old_cap * sizeof(uint16_t),
                  VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  dir->records = new_records;
  dir->generations = new_generations;
  dir->capacity = new_cap;
}

vkr_internal INLINE uint32_t vkr_entity_dir_alloc_index(VkrWorld *world) {
  assert_log(world, "World must not be NULL");

  VkrEntityDir *dir = &world->dir;
  if (dir->free_count > 0) {
    uint32_t idx = dir->free_indices[--dir->free_count];
    return idx;
  }

  if (dir->living >= dir->capacity) {
    vkr_entity_dir_grow(world);
    if (dir->living >= dir->capacity) {
      log_error("Entity directory full");
      return VKR_INVALID_ID;
    }
  }

  return dir->living++;
}

vkr_internal INLINE void vkr_entity_dir_free_index(VkrWorld *world,
                                                   uint32_t idx) {
  assert_log(world, "World must not be NULL");
  assert_log(idx < world->dir.capacity, "Index out of bounds");

  VkrEntityDir *dir = &world->dir;
  if (!vkr_entity_ensure_capacity_u32(&dir->free_indices, &dir->free_capacity,
                                      dir->free_count + 1, world)) {
    log_error("Failed to grow entity free list");
    return;
  }

  dir->free_indices[dir->free_count++] = idx;
}

vkr_internal INLINE bool32_t vkr_entity_comps_init(VkrWorld *world,
                                                   uint32_t initial) {
  assert_log(world, "World must not be NULL");

  world->comp_count = 0;
  world->comp_capacity = (initial ? initial : VKR_ENTITY_COMP_INITIAL_CAPACITY);
  world->components =
      vkr_entity_alloc(world, world->comp_capacity * sizeof(VkrComponentInfo),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!world->components)
    return false_v;

  MemZero(world->components, world->comp_capacity * sizeof(VkrComponentInfo));
  return true_v;
}

vkr_internal INLINE VkrComponentTypeId vkr_entity_comps_add(VkrWorld *world,
                                                            const char *name,
                                                            uint32_t size,
                                                            uint32_t align) {
  assert_log(size > 0, "Component size must be > 0");
  assert_log((uint32_t)align > 0 && (align & (align - 1)) == 0,
             "Align must be power-of-two");

  if (world->comp_count >= VKR_ECS_MAX_COMPONENTS) {
    log_error("Max components reached (%u)", VKR_ECS_MAX_COMPONENTS);
    return VKR_COMPONENT_TYPE_INVALID;
  }

  if (world->comp_count >= world->comp_capacity) {
    uint32_t old = world->comp_capacity;
    uint32_t neu = old * VKR_ENTITY_DIR_GROW_FACTOR;
    VkrComponentInfo *new_components = vkr_entity_realloc(
        world, world->components, old * sizeof(VkrComponentInfo),
        neu * sizeof(VkrComponentInfo), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!new_components)
      return VKR_COMPONENT_TYPE_INVALID;

    world->components = new_components;
    MemZero(world->components + old, (neu - old) * sizeof(VkrComponentInfo));
    world->comp_capacity = neu;
  }

  VkrComponentTypeId id = (VkrComponentTypeId)world->comp_count;
  uint64_t name_len = string_length(name) + 1;
  char *name_copy = (char *)vkr_entity_alloc(world, name_len,
                                             VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!name_copy)
    return VKR_COMPONENT_TYPE_INVALID;
  MemCopy(name_copy, name, name_len);

  if (!vkr_hash_table_insert_uint16_t(&world->component_name_to_id, name_copy,
                                      id)) {
    vkr_entity_free(world, name_copy, name_len,
                    VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return VKR_COMPONENT_TYPE_INVALID;
  }

  world->components[id] =
      (VkrComponentInfo){.name = name_copy, .size = size, .align = align};
  world->comp_count++;

  return id;
}

// ----------------------
// Sorting & keys
// ----------------------

// Sort component type ids (ascending) - insertion sort (small N)
vkr_internal INLINE void vkr_entity_sort_types(VkrComponentTypeId *types,
                                               uint32_t n) {
  if (n <= 1)
    return;
  assert_log(types, "Types must not be NULL");
  for (uint32_t i = 1; i < n; ++i) {
    VkrComponentTypeId key = types[i];
    int32_t j = (int32_t)i - 1;
    while (j >= 0 && types[j] > key) {
      types[j + 1] = types[j];
      --j;
    }
    types[j + 1] = key;
  }
}

typedef struct VkrEntityComponentInit {
  VkrComponentTypeId type;
  const void *data;
} VkrEntityComponentInit;

vkr_internal INLINE void
vkr_entity_sort_component_inits(VkrEntityComponentInit *inits, uint32_t n) {
  if (n <= 1)
    return;
  assert_log(inits, "Inits must not be NULL");
  for (uint32_t i = 1; i < n; ++i) {
    VkrEntityComponentInit key = inits[i];
    int32_t j = (int32_t)i - 1;
    while (j >= 0 && inits[j].type > key.type) {
      inits[j + 1] = inits[j];
      --j;
    }
    inits[j + 1] = key;
  }
}

// Build canonical key string for archetype: "N: t0,t1,t2" or "0:" for empty
vkr_internal INLINE const char *
vkr_entity_arch_key_build_alloc(VkrAllocator *allocator,
                                const VkrComponentTypeId *types, uint32_t n) {
  assert_log(allocator, "Allocator must not be NULL");
  assert_log(n == 0 || types, "Types must not be NULL when n > 0");

  if (n == 0) {
    char *s = (char *)vkr_allocator_alloc(allocator, VKR_ENTITY_ARCH_KEY_SIZE,
                                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!s)
      return NULL;
    s[0] = '0';
    s[1] = ':';
    s[2] = '\0';
    return s;
  }

  uint32_t len = 0;
  uint32_t n_digits = vkr_dec_digits_u32(n);
  len += n_digits + 2; // ':' and ' '
  for (uint32_t i = 0; i < n; ++i) {
    len += vkr_dec_digits_u32((uint32_t)types[i]);
    if (i + 1 < n)
      len += 1;
  }
  len += 1;

  char *s = (char *)vkr_allocator_alloc(allocator, len,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!s)
    return NULL;

  char *p = s;
  p = vkr_write_u32_dec(p, n);
  *p++ = ':';
  *p++ = ' ';
  for (uint32_t i = 0; i < n; ++i) {
    p = vkr_write_u32_dec(p, (uint32_t)types[i]);
    if (i + 1 < n)
      *p++ = ',';
  }

  *p = '\0';
  return s;
}

vkr_internal INLINE const char *
vkr_entity_arch_key_build(VkrWorld *world, const VkrComponentTypeId *types,
                          uint32_t n) {
  assert_log(world, "World must not be NULL");
  return vkr_entity_arch_key_build_alloc(world->alloc, types, n);
}

/**
 * @brief RAII-style helper for scratch-allocated archetype keys.
 * Manages scope creation and cleanup for temporary key allocations.
 */
typedef struct VkrScratchKey {
  const char *key;
  VkrAllocatorScope scope;
  bool8_t is_scoped;
  VkrAllocator *allocator;
} VkrScratchKey;

/**
 * @brief Acquires a scratch-allocated archetype key with proper scope
 * management.
 * @param world The world to get allocators from.
 * @param types Component type IDs array.
 * @param n Number of component types.
 * @return VkrScratchKey struct with key and cleanup state. key is NULL on
 * failure.
 */
vkr_internal INLINE VkrScratchKey vkr_scratch_key_acquire(
    VkrWorld *world, const VkrComponentTypeId *types, uint32_t n) {
  VkrScratchKey result = {0};
  assert_log(world, "World must not be NULL");

  VkrAllocator *scratch_alloc =
      world->scratch_alloc ? world->scratch_alloc : world->alloc;
  // Never open a scope on world->alloc (persistent allocator). If callers use
  // an outer scope on the same allocator, any persistent allocations created
  // while the scope is active can be reclaimed by scope reset.
  if (world->scratch_alloc && world->scratch_alloc != world->alloc) {
    result.scope = vkr_allocator_begin_scope(scratch_alloc);
    result.is_scoped = vkr_allocator_scope_is_valid(&result.scope);
  }

  result.allocator = scratch_alloc;
  result.key = vkr_entity_arch_key_build_alloc(scratch_alloc, types, n);
  if (!result.key && result.is_scoped) {
    vkr_allocator_end_scope(&result.scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    result.is_scoped = false_v;
  }

  return result;
}

/**
 * @brief Releases a scratch-allocated archetype key, cleaning up scope or
 * freeing memory.
 * @param key The scratch key to release.
 */
vkr_internal INLINE void vkr_scratch_key_release(VkrScratchKey *key) {
  if (!key || !key->key)
    return;

  if (key->is_scoped) {
    vkr_allocator_end_scope(&key->scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  } else {
    vkr_allocator_free(key->allocator, (void *)key->key,
                       string_length(key->key) + 1ull,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  key->key = NULL;
  key->is_scoped = false_v;
}

// ----------------------
// Chunk layout & archetypes
// ----------------------

vkr_internal INLINE uint32_t vkr_entity_compute_chunk_capacity(
    VkrWorld *world, uint32_t comp_count, const uint32_t *sizes,
    const uint32_t *aligns) {
  assert_log(world, "World must not be NULL");

  if (comp_count == 0) {
    uint32_t cap =
        (uint32_t)(VKR_ECS_CHUNK_SIZE / (uint64_t)sizeof(VkrEntityId));
    return cap > 0 ? cap : 1u;
  }

  assert_log(sizes && aligns,
             "Sizes/aligns must not be NULL when comp_count > 0");

  // Initial naive estimate ignoring per-column alignment
  uint64_t sum = sizeof(VkrEntityId);
  for (uint32_t comp = 0; comp < comp_count; ++comp)
    sum += sizes[comp];
  uint32_t cap = (sum > 0) ? (uint32_t)(VKR_ECS_CHUNK_SIZE / sum) : 0u;
  if (cap == 0)
    cap = 1;

  // Refine until fits
  for (;;) {
    uint64_t used = 0;
    // entity column (align to 8)
    used = vkr_entity_align_up_u64(used, (uint32_t)AlignOf(VkrEntityId));
    used += (uint64_t)cap * sizeof(VkrEntityId);
    for (uint32_t comp = 0; comp < comp_count; ++comp) {
      used = vkr_entity_align_up_u64(used, aligns[comp]);
      used += (uint64_t)cap * sizes[comp];
    }
    if (used <= VKR_ECS_CHUNK_SIZE)
      break;
    if (cap == 1)
      break;
    cap -= 1u;
  }

  return cap;
}

vkr_internal INLINE void
vkr_entity_validate_archetype_layout(const VkrArchetype *archetype) {
  assert_log(archetype, "Archetype must not be NULL");

  uint32_t cap = archetype->chunk_capacity;
  uint32_t end = 0;
  assert_log((archetype->ents_offset % AlignOf(VkrEntityId)) == 0,
             "Entity column misaligned");

  end = archetype->ents_offset + cap * (uint32_t)sizeof(VkrEntityId);
  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp) {
    uint32_t offset = archetype->col_offsets[comp];
    uint32_t align = archetype->aligns[comp];
    assert_log((offset % align) == 0, "Component column misaligned");
    assert_log(offset >= end, "Component column overlaps previous");
    end = offset + cap * archetype->sizes[comp];
  }

  assert_log(end <= VKR_ECS_CHUNK_SIZE, "Chunk layout exceeds chunk size");
}

vkr_internal INLINE VkrArchetype *
vkr_entity_archetype_create(VkrWorld *world, const VkrComponentTypeId *types,
                            uint32_t n) {
  assert_log(world, "World must not be NULL");
  assert_log(n == 0 || types, "Types must not be NULL when n > 0");

  VkrArchetype *archetype = vkr_entity_alloc(world, sizeof(VkrArchetype),
                                             VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!archetype)
    return NULL;

  MemZero(archetype, sizeof(*archetype));
  MemSet(archetype->type_to_col, VKR_ENTITY_TYPE_TO_COL_INVALID,
         sizeof(archetype->type_to_col));
  archetype->world = world;
  vkr_entity_sig_clear(&archetype->sig);
  archetype->comp_count = n;

  if (n > 0) {
    archetype->types = vkr_entity_alloc(world, n * sizeof(VkrComponentTypeId),
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    archetype->sizes = vkr_entity_alloc(world, n * sizeof(uint32_t),
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    archetype->aligns = vkr_entity_alloc(world, n * sizeof(uint32_t),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    archetype->col_offsets = vkr_entity_alloc(world, n * sizeof(uint32_t),
                                              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!archetype->types || !archetype->sizes || !archetype->aligns ||
        !archetype->col_offsets) {
      if (archetype->types) {
        vkr_entity_free(world, archetype->types, n * sizeof(VkrComponentTypeId),
                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }

      if (archetype->sizes) {
        vkr_entity_free(world, archetype->sizes, n * sizeof(uint32_t),
                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }

      if (archetype->aligns) {
        vkr_entity_free(world, archetype->aligns, n * sizeof(uint32_t),
                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }

      if (archetype->col_offsets) {
        vkr_entity_free(world, archetype->col_offsets, n * sizeof(uint32_t),
                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }

      vkr_entity_free(world, archetype, sizeof(VkrArchetype),
                      VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
      return NULL;
    }

    for (uint32_t typeIdx = 0; typeIdx < n; ++typeIdx) {
      VkrComponentTypeId typeId = types[typeIdx];
      assert_log(typeId < world->comp_count, "Component type out of range");
      archetype->types[typeIdx] = typeId;
      vkr_entity_sig_set(&archetype->sig, typeId);
      const VkrComponentInfo *ci = &world->components[typeId];
      archetype->sizes[typeIdx] = ci->size;
      archetype->aligns[typeIdx] = ci->align;
      archetype->type_to_col[typeId] = (uint16_t)typeIdx;
    }
  } else {
    archetype->types = NULL;
    archetype->sizes = NULL;
    archetype->aligns = NULL;
    archetype->col_offsets = NULL;
  }

  // Compute layout within chunk
  uint32_t cap = vkr_entity_compute_chunk_capacity(
      world, archetype->comp_count, archetype->sizes, archetype->aligns);
  archetype->chunk_capacity = cap;

  uint32_t off = 0;
  off = (uint32_t)vkr_entity_align_up_u64(off, (uint32_t)AlignOf(VkrEntityId));
  archetype->ents_offset = off;
  off += cap * (uint32_t)sizeof(VkrEntityId);

  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp) {
    off = (uint32_t)vkr_entity_align_up_u64(off, archetype->aligns[comp]);
    archetype->col_offsets[comp] = off;
    off += cap * archetype->sizes[comp];
  }

  archetype->chunks = NULL;
  archetype->key = vkr_entity_arch_key_build(world, types, n);
  if (!archetype->key) {
    if (archetype->types) {
      vkr_entity_free(world, archetype->types, n * sizeof(VkrComponentTypeId),
                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    if (archetype->sizes) {
      vkr_entity_free(world, archetype->sizes, n * sizeof(uint32_t),
                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    if (archetype->aligns) {
      vkr_entity_free(world, archetype->aligns, n * sizeof(uint32_t),
                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    if (archetype->col_offsets) {
      vkr_entity_free(world, archetype->col_offsets, n * sizeof(uint32_t),
                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    vkr_entity_free(world, archetype, sizeof(VkrArchetype),
                    VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return NULL;
  }

  vkr_entity_validate_archetype_layout(archetype);
  return archetype;
}

vkr_internal INLINE VkrChunk *vkr_entity_chunk_create(VkrWorld *world,
                                                      VkrArchetype *archetype) {
  assert_log(world, "World must not be NULL");
  assert_log(archetype, "Archetype must not be NULL");

  // Chunk struct + columns pointer array
  uint64_t ptrs_sz = (uint64_t)archetype->comp_count * sizeof(void *);
  uint64_t chunk_struct_sz = sizeof(VkrChunk) + ptrs_sz;

  VkrChunk *chunk =
      vkr_entity_alloc(world, chunk_struct_sz, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!chunk)
    return NULL;

  MemZero(chunk, chunk_struct_sz);
  chunk->arch = archetype;
  chunk->data = vkr_entity_alloc(world, VKR_ECS_CHUNK_SIZE,
                                 VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!chunk->data) {
    vkr_entity_free(world, chunk, chunk_struct_sz,
                    VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return NULL;
  }

  chunk->capacity = archetype->chunk_capacity;
  chunk->count = 0;
  chunk->columns = (void **)((uint8_t *)chunk + sizeof(VkrChunk));

  // Setup pointers
  chunk->ents = (VkrEntityId *)(chunk->data + archetype->ents_offset);
  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp) {
    chunk->columns[comp] = (void *)(chunk->data + archetype->col_offsets[comp]);
  }

  chunk->next = NULL;
  return chunk;
}

vkr_internal INLINE VkrChunk *
vkr_entity_archetype_acquire_chunk(VkrWorld *world, VkrArchetype *archetype) {
  assert_log(world, "World must not be NULL");
  assert_log(archetype, "Archetype must not be NULL");

  for (VkrChunk *chunk = archetype->chunks; chunk; chunk = chunk->next) {
    if (chunk->count < chunk->capacity) {
      return chunk;
    }
  }

  VkrChunk *new_chunk = vkr_entity_chunk_create(world, archetype);
  if (!new_chunk)
    return NULL;

  // push-front
  new_chunk->next = archetype->chunks;
  archetype->chunks = new_chunk;

  return new_chunk;
}

vkr_internal INLINE void vkr_entity_archetype_destroy(VkrWorld *world,
                                                      VkrArchetype *arch) {
  if (!world || !arch)
    return;

  VkrChunk *chunk = arch->chunks;
  while (chunk) {
    VkrChunk *next = chunk->next;
    if (chunk->data) {
      vkr_entity_free(world, chunk->data, VKR_ECS_CHUNK_SIZE,
                      VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    }
    uint64_t chunk_struct_sz =
        sizeof(VkrChunk) + (uint64_t)arch->comp_count * sizeof(void *);
    vkr_entity_free(world, chunk, chunk_struct_sz,
                    VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    chunk = next;
  }

  if (arch->types && arch->comp_count > 0) {
    vkr_entity_free(world, arch->types,
                    arch->comp_count * sizeof(VkrComponentTypeId),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (arch->sizes && arch->comp_count > 0) {
    vkr_entity_free(world, arch->sizes, arch->comp_count * sizeof(uint32_t),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (arch->aligns && arch->comp_count > 0) {
    vkr_entity_free(world, arch->aligns, arch->comp_count * sizeof(uint32_t),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (arch->col_offsets && arch->comp_count > 0) {
    vkr_entity_free(world, arch->col_offsets,
                    arch->comp_count * sizeof(uint32_t),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (arch->key) {
    vkr_entity_free(world, (void *)arch->key, (string_length(arch->key) + 1ull),
                    VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  vkr_entity_free(world, arch, sizeof(VkrArchetype),
                  VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

vkr_internal INLINE VkrArchetype *
vkr_entity_archetype_get_or_create(VkrWorld *world, VkrComponentTypeId *types,
                                   uint32_t n) {
  assert_log(world, "World must not be NULL");
  assert_log(n == 0 || types, "Types must not be NULL when n > 0");

  if (n > 1)
    vkr_entity_sort_types(types, n);

  // Build temporary key string for lookup.
  VkrScratchKey scratch_key = vkr_scratch_key_acquire(world, types, n);
  if (!scratch_key.key) {
    return NULL;
  }

  VkrArchetype **found =
      vkr_hash_table_get_VkrArchetypePtr(&world->arch_table, scratch_key.key);
  if (found) {
    vkr_scratch_key_release(&scratch_key);
    return *found;
  }

  vkr_scratch_key_release(&scratch_key);

  VkrArchetype *archetype = vkr_entity_archetype_create(world, types, n);
  if (!archetype)
    return NULL;

  // Ensure in arch_list (pointers)
  if (world->arch_count >= world->arch_capacity) {
    uint32_t old = world->arch_capacity ? world->arch_capacity : 8u;
    uint32_t neu =
        (world->arch_capacity ? world->arch_capacity : 0u) ? old * 2u : old;
    if (world->arch_list == NULL) {
      world->arch_list = (VkrArchetype **)vkr_entity_alloc(
          world, neu * sizeof(VkrArchetype *), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!world->arch_list) {
        vkr_entity_archetype_destroy(world, archetype);
        return NULL;
      }
      world->arch_capacity = neu;
    } else {
      VkrArchetype **new_list = (VkrArchetype **)vkr_entity_realloc(
          world, (void *)world->arch_list,
          world->arch_capacity * sizeof(VkrArchetype *),
          neu * sizeof(VkrArchetype *), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!new_list) {
        vkr_entity_archetype_destroy(world, archetype);
        return NULL;
      }
      world->arch_list = new_list;
      world->arch_capacity = neu;
    }
  }

  if (!vkr_hash_table_insert_VkrArchetypePtr(&world->arch_table, archetype->key,
                                             archetype)) {
    log_error("Failed to insert archetype into hash table");
    vkr_entity_archetype_destroy(world, archetype);
    return NULL;
  }

  world->arch_list[world->arch_count++] = archetype;
  return archetype;
}

// ----------------------
// World lifecycle
// ----------------------

VkrWorld *vkr_entity_create_world(const VkrWorldCreateInfo *info) {
  assert_log(info && info->alloc, "WorldCreateInfo/alloc must not be NULL");

  VkrWorld *world = vkr_allocator_alloc(info->alloc, sizeof(VkrWorld),
                                        VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!world)
    return NULL;

  MemZero(world, sizeof(*world));
  world->alloc = info->alloc;
  world->scratch_alloc = info->scratch_alloc;
  world->world_id = info->world_id;

  if (!vkr_entity_comps_init(world, info->initial_components))
    goto world_fail;
  if (!vkr_entity_dir_init(world, info->initial_entities))
    goto world_fail;

  uint32_t comp_cap = info->initial_components
                          ? info->initial_components
                          : VKR_ENTITY_COMP_INITIAL_CAPACITY;
  world->component_name_to_id =
      vkr_hash_table_create_uint16_t(world->alloc, comp_cap);

  world->arch_table = vkr_hash_table_create_VkrArchetypePtr(
      world->alloc, info->initial_archetypes
                        ? info->initial_archetypes
                        : VKR_ENTITY_ARCH_INITIAL_CAPACITY);

  world->arch_list = NULL;
  world->arch_capacity = 0u;
  world->arch_count = 0u;

  // Ensure EMPTY archetype exists
  if (!vkr_entity_archetype_get_or_create(world, NULL, 0)) {
    goto world_fail;
  }

  return world;

world_fail:
  vkr_entity_destroy_world(world);
  return NULL;
}

void vkr_entity_destroy_world(VkrWorld *world) {
  if (!world)
    return;

  // Free archetypes and chunks (if not using arena)
  for (uint32_t i = 0; i < world->arch_count; ++i) {
    VkrArchetype *arch = world->arch_list[i];
    if (!arch)
      continue;
    vkr_entity_archetype_destroy(world, arch);
  }

  // Free arch_list
  if (world->arch_list && world->arch_capacity) {
    vkr_entity_free(world, (void *)world->arch_list,
                    world->arch_capacity * sizeof(VkrArchetype *),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  // Free component registry
  if (world->components && world->comp_capacity) {
    for (uint32_t i = 0; i < world->comp_count; ++i) {
      if (world->components[i].name) {
        vkr_entity_free(world, (void *)world->components[i].name,
                        string_length(world->components[i].name) + 1ull,
                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
      }
    }
    vkr_entity_free(world, world->components,
                    world->comp_capacity * sizeof(VkrComponentInfo),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  // Free entity directory
  if (world->dir.records && world->dir.capacity) {
    vkr_entity_free(world, world->dir.records,
                    world->dir.capacity * sizeof(VkrEntityRecord),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (world->dir.generations && world->dir.capacity) {
    vkr_entity_free(world, world->dir.generations,
                    world->dir.capacity * sizeof(uint16_t),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (world->dir.free_indices && world->dir.free_capacity) {
    vkr_entity_free(world, world->dir.free_indices,
                    world->dir.free_capacity * sizeof(uint32_t),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  vkr_hash_table_destroy_uint16_t(&world->component_name_to_id);
  vkr_hash_table_destroy_VkrArchetypePtr(&world->arch_table);

  // If using arena, frees above are no-ops. Finally free the world struct.
  vkr_entity_free(world, world, sizeof(VkrWorld),
                  VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

// ----------------------
// Components
// ----------------------

VkrComponentTypeId vkr_entity_register_component(VkrWorld *world,
                                                 const char *name,
                                                 uint32_t size,
                                                 uint32_t align) {
  assert_log(world, "World must not be NULL");
  assert_log(name, "Name must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(align > 0 && (align & (align - 1)) == 0,
             "Align must be power-of-two");
  if (vkr_hash_table_get_uint16_t(&world->component_name_to_id, name)) {
    log_error("Component '%s' already registered", name);
    return VKR_COMPONENT_TYPE_INVALID;
  }

  return vkr_entity_comps_add(world, name, size, align);
}

VkrComponentTypeId vkr_entity_register_component_once(VkrWorld *world,
                                                      const char *name,
                                                      uint32_t size,
                                                      uint32_t align) {
  assert_log(world, "World must not be NULL");
  assert_log(name, "Name must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(align > 0 && (align & (align - 1)) == 0,
             "Align must be power-of-two");

  uint16_t *found =
      vkr_hash_table_get_uint16_t(&world->component_name_to_id, name);
  if (found) {
    VkrComponentTypeId id = (VkrComponentTypeId)*found;
    if (id < world->comp_count) {
      const VkrComponentInfo *info = &world->components[id];
      if (info->size != size || info->align != align) {
        log_error("Component '%s' registered with mismatched layout", name);
        return VKR_COMPONENT_TYPE_INVALID;
      }
    }

    return id;
  }

  return vkr_entity_comps_add(world, name, size, align);
}

VkrComponentTypeId vkr_entity_find_component(const VkrWorld *world,
                                             const char *name) {
  assert_log(world, "World must not be NULL");
  assert_log(name, "Name must not be NULL");

  uint16_t *found =
      vkr_hash_table_get_uint16_t(&world->component_name_to_id, name);
  return found ? (VkrComponentTypeId)*found : VKR_COMPONENT_TYPE_INVALID;
}

const VkrComponentInfo *vkr_entity_get_component_info(const VkrWorld *world,
                                                      VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  assert_log(type < VKR_ECS_MAX_COMPONENTS, "Component id out of range");

  if (!vkr_entity_validate_type(world, type))
    return NULL;

  return &world->components[type];
}

// ----------------------
// Entities
// ----------------------

VkrEntityId vkr_entity_create_entity(VkrWorld *world) {
  assert_log(world, "World must not be NULL");

  // Allocate index
  uint32_t idx = vkr_entity_dir_alloc_index(world);
  if (idx == VKR_INVALID_ID) {
    return VKR_ENTITY_ID_INVALID;
  }

  // Bump generation on create to avoid u64 == 0 and ensure uniqueness
  uint16_t gen = (uint16_t)(world->dir.generations[idx] + 1u);
  if (gen == 0) // skip 0 on wrap
    gen = 1u;
  world->dir.generations[idx] = gen;

  VkrEntityId id = vkr_entity_id_make(idx, gen, world->world_id);

  // Insert into EMPTY archetype
  VkrArchetype *empty = vkr_entity_archetype_get_or_create(world, NULL, 0);
  VkrChunk *chunk = vkr_entity_archetype_acquire_chunk(world, empty);
  if (!chunk) {
    vkr_entity_dir_free_index(world, idx);
    return VKR_ENTITY_ID_INVALID;
  }

  uint32_t slot = chunk->count++;
  chunk->ents[slot] = id;

  world->dir.records[idx] = (VkrEntityRecord){.chunk = chunk, .slot = slot};
  return id;
}

VkrEntityId vkr_entity_create_entity_with_components(
    VkrWorld *world, const VkrComponentTypeId *types,
    const void *const *init_data, uint32_t count) {
  assert_log(world, "World must not be NULL");
  assert_log(types || count == 0, "Types must not be NULL when count > 0");

  if (count == 0 || !types) {
    return vkr_entity_create_entity(world);
  }

  VkrAllocator *scratch_alloc =
      world->scratch_alloc ? world->scratch_alloc : world->alloc;
  // Scope must not run on world->alloc; otherwise it can reclaim
  // archetypes/chunks.
  VkrAllocatorScope scratch_scope = (VkrAllocatorScope){0};
  bool8_t scratch_scoped = false_v;
  if (world->scratch_alloc && world->scratch_alloc != world->alloc) {
    scratch_scope = vkr_allocator_begin_scope(scratch_alloc);
    scratch_scoped = vkr_allocator_scope_is_valid(&scratch_scope);
  }

  VkrEntityComponentInit stack_inits[64];
  VkrEntityComponentInit *inits = stack_inits;
  if (count > ArrayCount(stack_inits)) {
    inits = vkr_allocator_alloc(scratch_alloc,
                                count * sizeof(VkrEntityComponentInit),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!inits) {
      if (scratch_scoped) {
        vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return VKR_ENTITY_ID_INVALID;
    }
  }

  for (uint32_t i = 0; i < count; ++i) {
    if (!vkr_entity_validate_type(world, types[i])) {
      if (!scratch_scoped && inits != stack_inits) {
        vkr_allocator_free(scratch_alloc, inits,
                           count * sizeof(VkrEntityComponentInit),
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      } else if (scratch_scoped) {
        vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return VKR_ENTITY_ID_INVALID;
    }
    inits[i].type = types[i];
    inits[i].data = init_data ? init_data[i] : NULL;
  }

  // Debug-only check: detect duplicate component types before coalescing
#if ASSERT_LOG
  // Track which types we've already reported to avoid duplicate messages
  bool8_t reported[VKR_ECS_MAX_COMPONENTS] = {0};
  for (uint32_t i = 0; i < count; ++i) {
    VkrComponentTypeId type = types[i];
    if (type >= VKR_ECS_MAX_COMPONENTS || reported[type])
      continue;

    // Find all occurrences of this type
    uint32_t dup_indices[32];
    uint32_t dup_count = 0;
    for (uint32_t j = 0; j < count && dup_count < ArrayCount(dup_indices);
         ++j) {
      if (types[j] == type) {
        dup_indices[dup_count++] = j;
      }
    }

    // If found more than once, report it
    if (dup_count > 1) {
      if (dup_count == 2) {
        log_warn("vkr_entity_create_entity_with_components: duplicate "
                 "VkrComponentTypeId %u found at indices %u and %u (total "
                 "count=%u). "
                 "The first non-NULL init_data will be kept.",
                 (uint32_t)type, dup_indices[0], dup_indices[1], count);
        assert_log(false, "Duplicate component type detected in "
                          "vkr_entity_create_entity_with_components");
      } else {
        // Multiple duplicates - build indices string
        char indices_str[256] = {0};
        char *p = indices_str;
        for (uint32_t k = 0; k < dup_count && k < 10 && p < indices_str + 240;
             ++k) {
          if (k > 0)
            *p++ = ',';
          p = vkr_write_u32_dec(p, dup_indices[k]);
        }
        if (dup_count > 10) {
          *p++ = '.';
          *p++ = '.';
          *p++ = '.';
        }
        *p = '\0';
        log_warn(
            "vkr_entity_create_entity_with_components: duplicate "
            "VkrComponentTypeId %u found at indices [%s] (total count=%u, %u "
            "occurrences). The first non-NULL init_data will be kept.",
            (uint32_t)type, indices_str, count, dup_count);
        assert_log(false, "Duplicate component type detected in "
                          "vkr_entity_create_entity_with_components");
      }
      reported[type] = true_v;
    }
  }
#endif

  vkr_entity_sort_component_inits(inits, count);

  uint32_t unique_count = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (unique_count == 0 || inits[i].type != inits[unique_count - 1].type) {
      inits[unique_count++] = inits[i];
    } else if (!inits[unique_count - 1].data && inits[i].data) {
      inits[unique_count - 1].data = inits[i].data;
    }
  }

  VkrComponentTypeId stack_types[64];
  VkrComponentTypeId *sorted_types = stack_types;
  if (unique_count > ArrayCount(stack_types)) {
    sorted_types = vkr_allocator_alloc(
        scratch_alloc, unique_count * sizeof(VkrComponentTypeId),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!sorted_types) {
      if (!scratch_scoped && inits != stack_inits) {
        vkr_allocator_free(scratch_alloc, inits,
                           count * sizeof(VkrEntityComponentInit),
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      } else if (scratch_scoped) {
        vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return VKR_ENTITY_ID_INVALID;
    }
  }

  for (uint32_t i = 0; i < unique_count; ++i) {
    sorted_types[i] = inits[i].type;
  }

  VkrArchetype *archetype =
      vkr_entity_archetype_get_or_create(world, sorted_types, unique_count);
  if (!archetype) {
    if (!scratch_scoped) {
      if (inits != stack_inits) {
        vkr_allocator_free(scratch_alloc, inits,
                           count * sizeof(VkrEntityComponentInit),
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      if (sorted_types != stack_types) {
        vkr_allocator_free(scratch_alloc, sorted_types,
                           unique_count * sizeof(VkrComponentTypeId),
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
    } else {
      vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    return VKR_ENTITY_ID_INVALID;
  }

  uint32_t idx = vkr_entity_dir_alloc_index(world);
  if (idx == VKR_INVALID_ID) {
    if (scratch_scoped) {
      vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    } else {
      if (inits != stack_inits) {
        vkr_allocator_free(scratch_alloc, inits,
                           count * sizeof(VkrEntityComponentInit),
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      if (sorted_types != stack_types) {
        vkr_allocator_free(scratch_alloc, sorted_types,
                           unique_count * sizeof(VkrComponentTypeId),
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
    }

    return VKR_ENTITY_ID_INVALID;
  }

  uint16_t gen = (uint16_t)(world->dir.generations[idx] + 1u);
  if (gen == 0)
    gen = 1u;
  world->dir.generations[idx] = gen;
  VkrEntityId id = vkr_entity_id_make(idx, gen, world->world_id);

  VkrChunk *chunk = vkr_entity_archetype_acquire_chunk(world, archetype);
  if (!chunk) {
    vkr_entity_dir_free_index(world, idx);
    if (!scratch_scoped) {
      if (inits != stack_inits) {
        vkr_allocator_free(scratch_alloc, inits,
                           count * sizeof(VkrEntityComponentInit),
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }

      if (sorted_types != stack_types) {
        vkr_allocator_free(scratch_alloc, sorted_types,
                           unique_count * sizeof(VkrComponentTypeId),
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
    } else {
      vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    return VKR_ENTITY_ID_INVALID;
  }

  uint32_t slot = chunk->count++;
  chunk->ents[slot] = id;
  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp) {
    uint8_t *dst_col = (uint8_t *)chunk->columns[comp];
    uint32_t size = archetype->sizes[comp];
    const void *src = inits[comp].data;
    if (src) {
      MemCopy(dst_col + (size_t)size * slot, src, size);
    } else {
      MemZero(dst_col + (size_t)size * slot, size);
    }
  }

  world->dir.records[idx] = (VkrEntityRecord){.chunk = chunk, .slot = slot};

  if (scratch_scoped) {
    vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    if (inits != stack_inits) {
      vkr_allocator_free(scratch_alloc, inits,
                         count * sizeof(VkrEntityComponentInit),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    if (sorted_types != stack_types) {
      vkr_allocator_free(scratch_alloc, sorted_types,
                         unique_count * sizeof(VkrComponentTypeId),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
  }

  return id;
}

bool8_t vkr_entity_is_alive(const VkrWorld *world, VkrEntityId id) {
  assert_log(world, "World must not be NULL");
  return vkr_entity_validate_id(world, id);
}

vkr_internal INLINE void
vkr_entity_chunk_swap_remove(VkrWorld *world, VkrChunk *chunk, uint32_t slot) {
  assert_log(world, "World must not be NULL");
  assert_log(chunk, "Chunk must not be NULL");
  assert_log(slot < chunk->count, "Slot out of range");

  uint32_t last = chunk->count - 1u;
  if (slot != last) {
    VkrEntityId moved = chunk->ents[last];
    chunk->ents[slot] = moved;
    // move each component column
    VkrArchetype *archetype = chunk->arch;
    for (uint32_t comp = 0; comp < archetype->comp_count; ++comp) {
      uint8_t *col = (uint8_t *)chunk->columns[comp];
      uint32_t size = archetype->sizes[comp];
      MemCopy(col + (size_t)size * slot, col + (size_t)size * last, size);
    }
    // update directory for moved entity
    world->dir.records[moved.parts.index].chunk = chunk;
    world->dir.records[moved.parts.index].slot = slot;
  }

  chunk->count--;
}

bool8_t vkr_entity_destroy_entity(VkrWorld *world, VkrEntityId id) {
  assert_log(world, "World must not be NULL");
  if (!vkr_entity_validate_id(world, id))
    return false_v;

  uint32_t idx = id.parts.index;
  VkrEntityRecord rec = world->dir.records[idx];
  VkrChunk *chunk = rec.chunk;
  if (!chunk)
    return false_v;

  vkr_entity_chunk_swap_remove(world, chunk, rec.slot);

  // invalidate
  uint16_t gen = (uint16_t)(world->dir.generations[idx] + 1u);
  if (gen == 0)
    gen = 1u; // skip 0 on wrap
  world->dir.generations[idx] = gen;

  world->dir.records[idx] = (VkrEntityRecord){0};
  vkr_entity_dir_free_index(world, idx);

  return true_v;
}

// Find column index of type in archetype, or -1
vkr_internal INLINE int32_t vkr_entity_arch_find_col(
    const VkrArchetype *archetype, VkrComponentTypeId type) {
  assert_log(archetype, "Archetype must not be NULL");
  assert_log(type < VKR_ECS_MAX_COMPONENTS, "Type out of range");

  uint16_t col = archetype->type_to_col[type];
  int32_t result = (col == VKR_ENTITY_TYPE_TO_COL_INVALID) ? -1 : (int32_t)col;
  // Debug: suspicious if archetype has 0 components but we found a column
  if (archetype->comp_count == 0 && result >= 0) {
    log_error("BUG: arch_find_col returned %d for type %u in archetype %p with "
              "0 components (type_to_col[%u]=%u)",
              result, type, (void *)archetype, type, col);
  }
  return result;
}

// Move entity from src archetype to dst archetype (add/remove component)
vkr_internal INLINE bool32_t vkr_entity_move_entity(
    VkrWorld *world, VkrEntityId id, VkrArchetype *dst,
    VkrComponentTypeId added_type, const void *added_init_or_null) {
  assert_log(world, "World must not be NULL");
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");
  assert_log(dst, "Destination archetype must not be NULL");
  assert_log(added_type == VKR_COMPONENT_TYPE_INVALID ||
                 added_type < world->comp_count,
             "Added type out of range");

  uint32_t idx = id.parts.index;
  VkrEntityRecord rec = world->dir.records[idx];
  VkrChunk *src_chunk = rec.chunk;
  VkrArchetype *src_arch = src_chunk->arch;
  uint32_t src_slot = rec.slot;

  VkrChunk *dst_chunk = vkr_entity_archetype_acquire_chunk(world, dst);
  if (!dst_chunk)
    return false_v;

  uint32_t dst_slot = dst_chunk->count++;

  dst_chunk->ents[dst_slot] = id;

  // Copy shared/new components
  for (uint32_t comp = 0; comp < dst->comp_count; ++comp) {
    VkrComponentTypeId typeId = dst->types[comp];
    int32_t src_i = vkr_entity_arch_find_col(src_arch, typeId);
    uint8_t *dst_col = (uint8_t *)dst_chunk->columns[comp];
    if (src_i >= 0) {
      uint8_t *src_col = (uint8_t *)src_chunk->columns[src_i];
      MemCopy(dst_col + (size_t)dst->sizes[comp] * dst_slot,
              src_col + (size_t)src_arch->sizes[src_i] * src_slot,
              dst->sizes[comp]);
    } else if (typeId == added_type) {
      // new component
      if (added_init_or_null) {
        MemCopy(dst_col + (size_t)dst->sizes[comp] * dst_slot,
                added_init_or_null, dst->sizes[comp]);
      } else {
        MemZero(dst_col + (size_t)dst->sizes[comp] * dst_slot,
                dst->sizes[comp]);
      }
    } else {
      // present in dst, not in src, not the explicitly added one
      MemZero(dst_col + (size_t)dst->sizes[comp] * dst_slot, dst->sizes[comp]);
    }
  }

  // Remove from source chunk via swap-remove
  vkr_entity_chunk_swap_remove(world, src_chunk, src_slot);

  // Update directory
  world->dir.records[idx] =
      (VkrEntityRecord){.chunk = dst_chunk, .slot = dst_slot};

  return true_v;
}

bool8_t vkr_entity_add_component(VkrWorld *world, VkrEntityId id,
                                 VkrComponentTypeId type,
                                 const void *init_data) {
  assert_log(world, "World must not be NULL");

  if (!vkr_entity_validate_id(world, id)) {
    log_error("add_component: invalid entity id (index=%u, gen=%u)",
              id.parts.index, id.parts.generation);
    return false_v;
  }

  if (!vkr_entity_validate_type(world, type)) {
    log_error("add_component: invalid type %u (world comp_count=%u)", type,
              world->comp_count);
    return false_v;
  }

  uint32_t idx = id.parts.index;
  VkrEntityRecord rec = world->dir.records[idx];
  VkrChunk *chunk = rec.chunk;
  VkrArchetype *archetype = chunk->arch;

  int32_t existing_col = vkr_entity_arch_find_col(archetype, type);
  if (existing_col >= 0) {
    log_warn("add_component: entity already has type %u at col %d (archetype "
             "has %u components)",
             type, existing_col, archetype->comp_count);
    return true_v; // already has it
  }

  // Build dst types = src types + type
  uint32_t comp_count = archetype->comp_count + 1u;
  VkrAllocator *scratch_alloc =
      world->scratch_alloc ? world->scratch_alloc : world->alloc;
  // Scope must not run on world->alloc; otherwise it can reclaim
  // archetypes/chunks.
  VkrAllocatorScope scratch_scope = (VkrAllocatorScope){0};
  bool8_t scratch_scoped = false_v;
  if (world->scratch_alloc && world->scratch_alloc != world->alloc) {
    scratch_scope = vkr_allocator_begin_scope(scratch_alloc);
    scratch_scoped = vkr_allocator_scope_is_valid(&scratch_scope);
  }
  VkrComponentTypeId stack_types[64];
  VkrComponentTypeId *dst_types =
      (comp_count <= 64)
          ? stack_types
          : vkr_allocator_alloc(scratch_alloc,
                                comp_count * sizeof(VkrComponentTypeId),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!dst_types) {
    if (scratch_scoped) {
      vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }

    return false_v;
  }

  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp)
    dst_types[comp] = archetype->types[comp];
  dst_types[archetype->comp_count] = type;

  VkrArchetype *dst =
      vkr_entity_archetype_get_or_create(world, dst_types, comp_count);
  if (scratch_scoped) {
    vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else if (dst_types != stack_types) {
    vkr_allocator_free(scratch_alloc, dst_types,
                       comp_count * sizeof(VkrComponentTypeId),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (!dst)
    return false_v;

  return vkr_entity_move_entity(world, id, dst, type, init_data);
}

bool8_t vkr_entity_remove_component(VkrWorld *world, VkrEntityId id,
                                    VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  if (!vkr_entity_validate_id(world, id))
    return false_v;
  if (!vkr_entity_validate_type(world, type))
    return false_v;

  uint32_t idx = id.parts.index;
  VkrEntityRecord rec = world->dir.records[idx];
  VkrChunk *chunk = rec.chunk;
  VkrArchetype *archetype = chunk->arch;

  int32_t rm_i = vkr_entity_arch_find_col(archetype, type);
  if (rm_i < 0)
    return true_v; // not present

  // Build dst types = src types without 'type'
  uint32_t comp_count = archetype->comp_count - 1u;
  VkrAllocator *scratch_alloc =
      world->scratch_alloc ? world->scratch_alloc : world->alloc;
  // Scope must not run on world->alloc; otherwise it can reclaim
  // archetypes/chunks.
  VkrAllocatorScope scratch_scope = (VkrAllocatorScope){0};
  bool8_t scratch_scoped = false_v;
  if (world->scratch_alloc && world->scratch_alloc != world->alloc) {
    scratch_scope = vkr_allocator_begin_scope(scratch_alloc);
    scratch_scoped = vkr_allocator_scope_is_valid(&scratch_scope);
  }
  VkrComponentTypeId stack_types[64];
  VkrComponentTypeId *dst_types =
      (comp_count <= 64)
          ? stack_types
          : vkr_allocator_alloc(scratch_alloc,
                                comp_count * sizeof(VkrComponentTypeId),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!dst_types) {
    if (scratch_scoped) {
      vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    return false_v;
  }
  uint32_t j = 0;
  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp) {
    if (archetype->types[comp] != type)
      dst_types[j++] = archetype->types[comp];
  }

  VkrArchetype *dst =
      vkr_entity_archetype_get_or_create(world, dst_types, comp_count);
  if (scratch_scoped) {
    vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else if (dst_types != stack_types) {
    vkr_allocator_free(scratch_alloc, dst_types,
                       comp_count * sizeof(VkrComponentTypeId),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (!dst)
    return false_v;

  return vkr_entity_move_entity(world, id, dst, VKR_COMPONENT_TYPE_INVALID,
                                NULL);
}

void *vkr_entity_get_component_mut(VkrWorld *world, VkrEntityId id,
                                   VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  if (!vkr_entity_validate_id(world, id))
    return NULL;
  if (!vkr_entity_validate_type(world, type))
    return NULL;

  VkrEntityRecord rec = world->dir.records[id.parts.index];
  VkrArchetype *archetype = rec.chunk->arch;
  int32_t col_i = vkr_entity_arch_find_col(archetype, type);
  if (col_i < 0)
    return NULL;

  uint8_t *col = (uint8_t *)rec.chunk->columns[col_i];
  if (!col) {
    log_error(
        "NULL column at index %d for type %u in archetype with %u components",
        col_i, type, archetype->comp_count);
    return NULL;
  }
  return (void *)(col + (size_t)archetype->sizes[col_i] * rec.slot);
}

const void *vkr_entity_get_component(const VkrWorld *world, VkrEntityId id,
                                     VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  return vkr_entity_get_component_mut((VkrWorld *)world, id, type);
}

bool8_t vkr_entity_has_component(const VkrWorld *world, VkrEntityId id,
                                 VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");

  if (!vkr_entity_validate_id(world, id))
    return false_v;
  if (!vkr_entity_validate_type(world, type))
    return false_v;

  const VkrEntityRecord record = world->dir.records[id.parts.index];
  const VkrArchetype *archetype = record.chunk->arch;

  return vkr_entity_arch_find_col(archetype, type) >= 0 ? true_v : false_v;
}

// ----------------------
// Query
// ----------------------

void vkr_entity_query_build(VkrWorld *world,
                            const VkrComponentTypeId *include_types,
                            uint32_t include_count,
                            const VkrComponentTypeId *exclude_types,
                            uint32_t exclude_count, VkrQuery *out_query) {
  assert_log(world, "World must not be NULL");
  assert_log(out_query, "Output query must not be NULL");
  assert_log(include_count == 0 || include_types,
             "Include types must not be NULL when include_count > 0");
  assert_log(exclude_count == 0 || exclude_types,
             "Exclude types must not be NULL when exclude_count > 0");

  vkr_entity_sig_clear(&out_query->include);
  vkr_entity_sig_clear(&out_query->exclude);
  for (uint32_t comp = 0; comp < include_count; ++comp) {
    if (!vkr_entity_validate_type(world, include_types[comp])) {
      log_error("Invalid include component type: %u", include_types[comp]);
      continue;
    }
    vkr_entity_sig_set(&out_query->include, include_types[comp]);
  }

  for (uint32_t comp = 0; comp < exclude_count; ++comp) {
    if (!vkr_entity_validate_type(world, exclude_types[comp])) {
      log_error("Invalid exclude component type: %u", exclude_types[comp]);
      continue;
    }
    vkr_entity_sig_set(&out_query->exclude, exclude_types[comp]);
  }
}

void vkr_entity_query_each_chunk(VkrWorld *world, const VkrQuery *query,
                                 VkrChunkFn fn, void *user) {
  assert_log(world, "World must not be NULL");
  assert_log(query, "Query must not be NULL");
  assert_log(fn, "Callback must not be NULL");

  for (uint32_t ai = 0; ai < world->arch_count; ++ai) {
    VkrArchetype *archetype = world->arch_list[ai];
    if (!vkr_entity_sig_contains(&archetype->sig, &query->include))
      continue;
    if (vkr_entity_sig_intersects(&archetype->sig, &query->exclude))
      continue;
    for (VkrChunk *chunk = archetype->chunks; chunk; chunk = chunk->next) {
      if (chunk->count == 0)
        continue;
      fn(archetype, chunk, user);
    }
  }
}

bool8_t vkr_entity_query_compile(VkrWorld *world, const VkrQuery *query,
                                 VkrAllocator *allocator,
                                 VkrQueryCompiled *out_query) {
  assert_log(world, "World must not be NULL");
  assert_log(query, "Query must not be NULL");
  assert_log(allocator, "Allocator must not be NULL");
  assert_log(out_query, "Output query must not be NULL");

  out_query->archetypes = NULL;
  out_query->archetype_count = 0;
#ifdef VKR_DEBUG
  out_query->world_arch_count_at_compile = 0;
#endif

  uint32_t match_count = 0;
  for (uint32_t ai = 0; ai < world->arch_count; ++ai) {
    VkrArchetype *archetype = world->arch_list[ai];
    if (!vkr_entity_sig_contains(&archetype->sig, &query->include))
      continue;
    if (vkr_entity_sig_intersects(&archetype->sig, &query->exclude))
      continue;
    match_count++;
  }

  if (match_count == 0) {
#ifdef VKR_DEBUG
    out_query->world_arch_count_at_compile = world->arch_count;
#endif
    return true_v;
  }

  VkrArchetype **matches = (VkrArchetype **)vkr_allocator_alloc(
      allocator, match_count * sizeof(VkrArchetype *),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!matches)
    return false_v;

  uint32_t idx = 0;
  for (uint32_t ai = 0; ai < world->arch_count; ++ai) {
    VkrArchetype *archetype = world->arch_list[ai];
    if (!vkr_entity_sig_contains(&archetype->sig, &query->include))
      continue;
    if (vkr_entity_sig_intersects(&archetype->sig, &query->exclude))
      continue;
    matches[idx++] = archetype;
  }

  out_query->archetypes = matches;
  out_query->archetype_count = match_count;
#ifdef VKR_DEBUG
  out_query->world_arch_count_at_compile = world->arch_count;
#endif
  return true_v;
}

void vkr_entity_query_compiled_destroy(VkrAllocator *allocator,
                                       VkrQueryCompiled *query) {
  if (!query || !allocator)
    return;
  if (query->archetypes) {
    vkr_allocator_free(allocator, (void *)query->archetypes,
                       query->archetype_count * sizeof(VkrArchetype *),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  query->archetypes = NULL;
  query->archetype_count = 0;
#ifdef VKR_DEBUG
  query->world_arch_count_at_compile = 0;
#endif
}

void vkr_entity_query_compiled_each_chunk(const VkrQueryCompiled *query,
                                          VkrChunkFn fn, void *user) {
  assert_log(query, "Query must not be NULL");
  assert_log(fn, "Callback must not be NULL");

#ifdef VKR_DEBUG
  // Debug check: detect stale compiled queries
  // A query is stale if the world's archetype count increased since compilation
  if (query->archetype_count > 0 && query->archetypes[0]) {
    const VkrWorld *world = query->archetypes[0]->world;
    if (world && world->arch_count > query->world_arch_count_at_compile) {
      assert_log(
          false_v,
          "Compiled query is stale: world archetype count increased "
          "from %u to %u since compilation. Call vkr_entity_query_compile() "
          "to update the query.",
          query->world_arch_count_at_compile, world->arch_count);
    }
  }
#endif

  for (uint32_t ai = 0; ai < query->archetype_count; ++ai) {
    VkrArchetype *archetype = query->archetypes[ai];
    if (!archetype)
      continue;
    for (VkrChunk *chunk = archetype->chunks; chunk; chunk = chunk->next) {
      if (chunk->count == 0)
        continue;
      fn(archetype, chunk, user);
    }
  }
}

// ----------------------
// Chunk accessors
// ----------------------

uint32_t vkr_entity_chunk_count(const VkrChunk *chunk) {
  assert_log(chunk, "Chunk must not be NULL");
  return chunk->count;
}

VkrEntityId *vkr_entity_chunk_entities(VkrChunk *chunk) {
  assert_log(chunk, "Chunk must not be NULL");
  return chunk->ents;
}

void *vkr_entity_chunk_column(VkrChunk *chunk, VkrComponentTypeId type) {
  assert_log(chunk, "Chunk must not be NULL");
  if (!vkr_entity_validate_type(chunk->arch->world, type))
    return NULL;

  int32_t col_i = vkr_entity_arch_find_col(chunk->arch, type);
  if (col_i < 0)
    return NULL;
  return chunk->columns[col_i];
}

const void *vkr_entity_chunk_column_const(const VkrChunk *chunk,
                                          VkrComponentTypeId type) {
  assert_log(chunk, "Chunk must not be NULL");
  if (!vkr_entity_validate_type(chunk->arch->world, type))
    return NULL;

  int32_t col_i = vkr_entity_arch_find_col(chunk->arch, type);
  if (col_i < 0)
    return NULL;
  return chunk->columns[col_i];
}

const VkrArchetype *vkr_entity_chunk_archetype(const VkrChunk *chunk) {
  assert_log(chunk, "Chunk must not be NULL");
  return chunk->arch;
}

const VkrSignature *vkr_entity_archetype_signature(const VkrArchetype *arch) {
  assert_log(arch, "Archetype must not be NULL");
  return &arch->sig;
}

uint32_t vkr_entity_archetype_component_count(const VkrArchetype *arch) {
  assert_log(arch, "Archetype must not be NULL");
  return arch->comp_count;
}

VkrComponentTypeId vkr_entity_archetype_component_at(const VkrArchetype *arch,
                                                     uint32_t idx) {
  assert_log(arch, "Archetype must not be NULL");
  assert_log(idx < arch->comp_count, "Index out of range");
  return (idx < arch->comp_count) ? arch->types[idx]
                                  : VKR_COMPONENT_TYPE_INVALID;
}
