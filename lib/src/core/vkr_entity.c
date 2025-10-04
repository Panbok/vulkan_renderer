#include "core/vkr_entity.h"
#include "defines.h"
#include <string.h> // for strlen

// ----------------------
// Small helpers
// ----------------------

vkr_internal INLINE VkrEntityFlags vkr_entity_flags_create() {
  return bitset8_create();
}

vkr_internal INLINE VkrEntityFlags vkr_entity_flags_from_bits(uint8_t bits) {
  VkrEntityFlags flags = bitset8_create();
  if (bits & VKR_ENTITY_FLAG_VISIBLE)
    bitset8_set(&flags, VKR_ENTITY_FLAG_VISIBLE);
  if (bits & VKR_ENTITY_FLAG_STATIC)
    bitset8_set(&flags, VKR_ENTITY_FLAG_STATIC);
  if (bits & VKR_ENTITY_FLAG_DISABLED)
    bitset8_set(&flags, VKR_ENTITY_FLAG_DISABLED);
  if (bits & VKR_ENTITY_FLAG_PENDING_DESTROY)
    bitset8_set(&flags, VKR_ENTITY_FLAG_PENDING_DESTROY);
  if (bits & VKR_ENTITY_FLAG_DIRTY_XFORM)
    bitset8_set(&flags, VKR_ENTITY_FLAG_DIRTY_XFORM);
  return flags;
}

vkr_internal INLINE VkrEntityId vkr_entity_id_make(uint32_t index,
                                                   uint16_t generation,
                                                   uint16_t world) {
  VkrEntityId id;
  id.parts.index = index;
  id.parts.generation = generation;
  id.parts.world = world;
  return id;
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
  uint32_t word = typeId >> 6;          // /64
  uint32_t bit = typeId & 63u;          // %64
  signature->bits[word] |= 1ull << bit; // unsigned shift
}

vkr_internal INLINE bool32_t vkr_entity_sig_has(const VkrSignature *signature,
                                                VkrComponentTypeId typeId) {
  assert_log(typeId < VKR_ECS_MAX_COMPONENTS, "Component id out of range");
  uint32_t word = typeId >> 6;
  uint32_t bit = typeId & 63u;
  return (signature->bits[word] & (1ull << bit)) != 0ull ? true_v : false_v;
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

  uint32_t new_cap = (*cap == 0) ? 8u : (*cap * 2u);
  while (new_cap < need)
    new_cap *= 2u;

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

  uint32_t new_cap = (*cap == 0) ? 8u : (*cap * 2u);
  while (new_cap < need)
    new_cap *= 2u;

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

  uint32_t new_cap = (*cap == 0) ? 8u : (*cap * 2u);
  while (new_cap < need)
    new_cap *= 2u;

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
  dir->capacity = initial ? initial : 1024u;
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
  uint32_t new_cap = old_cap * 2u;

  dir->records = vkr_entity_realloc(
      world, dir->records, old_cap * sizeof(VkrEntityRecord),
      new_cap * sizeof(VkrEntityRecord), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  dir->generations = vkr_entity_realloc(
      world, dir->generations, old_cap * sizeof(uint16_t),
      new_cap * sizeof(uint16_t), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  MemZero(dir->records + old_cap,
          (new_cap - old_cap) * sizeof(VkrEntityRecord));
  MemZero(dir->generations + old_cap, (new_cap - old_cap) * sizeof(uint16_t));

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
  }

  return dir->living++;
}

vkr_internal INLINE void vkr_entity_dir_free_index(VkrWorld *world,
                                                   uint32_t idx) {
  assert_log(world, "World must not be NULL");
  assert_log(idx < world->dir.capacity, "Index out of bounds");

  VkrEntityDir *dir = &world->dir;
  vkr_entity_ensure_capacity_u32(&dir->free_indices, &dir->free_capacity,
                                 dir->free_count + 1, world);
  dir->free_indices[dir->free_count++] = idx;
}

vkr_internal INLINE bool32_t vkr_entity_comps_init(VkrWorld *world,
                                                   uint32_t initial) {
  assert_log(world, "World must not be NULL");

  world->comp_count = 0;
  world->comp_capacity = (initial ? initial : 64u);
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
    uint32_t neu = old * 2u;
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
  world->components[world->comp_count++] =
      (VkrComponentInfo){.name = name, .size = size, .align = align};

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

// Build canonical key string for archetype: "N: t0,t1,t2" or "0:" for empty
vkr_internal INLINE const char *
vkr_entity_arch_key_build(VkrWorld *world, const VkrComponentTypeId *types,
                          uint32_t n) {
  assert_log(world, "World must not be NULL");
  assert_log(n == 0 || types, "Types must not be NULL when n > 0");

  if (n == 0) {
    // "0:" (2 chars + null)
    char *s =
        (char *)vkr_entity_alloc(world, 3, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!s)
      return NULL;
    s[0] = '0';
    s[1] = ':';
    s[2] = '\0';
    return s;
  }

  uint32_t len = 0;
  uint32_t n_digits = vkr_dec_digits_u32(n);
  // "N: " prefix
  len += n_digits + 2; // ':' and ' '
  // components and commas
  for (uint32_t i = 0; i < n; ++i) {
    len += vkr_dec_digits_u32((uint32_t)types[i]);
    if (i + 1 < n)
      len += 1; // comma
  }
  len += 1; // null terminator

  char *s =
      (char *)vkr_entity_alloc(world, len, VKR_ALLOCATOR_MEMORY_TAG_STRING);
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
    used = vkr_ceil_div_u32((uint32_t)used, (uint32_t)AlignOf(VkrEntityId));
    used += (uint64_t)cap * sizeof(VkrEntityId);
    for (uint32_t comp = 0; comp < comp_count; ++comp) {
      used = vkr_ceil_div_u32((uint32_t)used, aligns[comp]);
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
        !archetype->col_offsets)
      return NULL;

    for (uint32_t typeIdx = 0; typeIdx < n; ++typeIdx) {
      VkrComponentTypeId typeId = types[typeIdx];
      archetype->types[typeIdx] = typeId;
      vkr_entity_sig_set(&archetype->sig, typeId);
      const VkrComponentInfo *ci = &world->components[typeId];
      archetype->sizes[typeIdx] = ci->size;
      archetype->aligns[typeIdx] = ci->align;
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
  off = vkr_ceil_div_u32(off, (uint32_t)AlignOf(VkrEntityId));
  archetype->ents_offset = off;
  off += cap * (uint32_t)sizeof(VkrEntityId);

  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp) {
    off = vkr_ceil_div_u32(off, archetype->aligns[comp]);
    archetype->col_offsets[comp] = off;
    off += cap * archetype->sizes[comp];
  }

  archetype->chunks = NULL;
  archetype->key = vkr_entity_arch_key_build(world, types, n);

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
  if (!chunk->data)
    return NULL;

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
    if (chunk->count < chunk->capacity)
      return chunk;
  }

  VkrChunk *new_chunk = vkr_entity_chunk_create(world, archetype);
  if (!new_chunk)
    return NULL;

  // push-front
  new_chunk->next = archetype->chunks;
  archetype->chunks = new_chunk;

  return new_chunk;
}

vkr_internal INLINE VkrArchetype *
vkr_entity_archetype_get_or_create(VkrWorld *world, VkrComponentTypeId *types,
                                   uint32_t n) {
  assert_log(world, "World must not be NULL");
  assert_log(n == 0 || types, "Types must not be NULL when n > 0");

  if (n > 1)
    vkr_entity_sort_types(types, n);

  // Build temporary key string for lookup; free it after lookup.
  const char *tmp_key = vkr_entity_arch_key_build(world, types, n);
  if (!tmp_key)
    return NULL;

  VkrArchetype *found =
      vkr_hash_table_get_VkrArchetype(&world->arch_table, tmp_key);
  if (found) {
    // Free temporary key string
    vkr_entity_free(world, (void *)tmp_key, (uint64_t)(strlen(tmp_key) + 1),
                    VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return found;
  }

  // Not found; free the temp key and create the archetype (which builds its own
  // key)
  vkr_entity_free(world, (void *)tmp_key, (uint64_t)(strlen(tmp_key) + 1),
                  VKR_ALLOCATOR_MEMORY_TAG_STRING);

  VkrArchetype *archetype = vkr_entity_archetype_create(world, types, n);
  if (!archetype)
    return NULL;

  vkr_hash_table_insert_VkrArchetype(&world->arch_table, archetype->key,
                                     *archetype);

  // Ensure in arch_list (pointers)
  if (world->arch_count >= world->arch_capacity) {
    uint32_t old = world->arch_capacity ? world->arch_capacity : 8u;
    uint32_t neu =
        (world->arch_capacity ? world->arch_capacity : 0u) ? old * 2u : old;
    if (world->arch_list == NULL) {
      world->arch_list = (VkrArchetype **)vkr_entity_alloc(
          world, neu * sizeof(VkrArchetype *), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!world->arch_list)
        return archetype;
      world->arch_capacity = neu;
    } else {
      VkrArchetype **new_list = (VkrArchetype **)vkr_entity_realloc(
          world, (void *)world->arch_list,
          world->arch_capacity * sizeof(VkrArchetype *),
          neu * sizeof(VkrArchetype *), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!new_list)
        return archetype;
      world->arch_list = new_list;
      world->arch_capacity = neu;
    }
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
  world->world_id = info->world_id;

  if (!vkr_entity_comps_init(world, info->initial_components))
    return NULL;
  if (!vkr_entity_dir_init(world, info->initial_entities))
    return NULL;

  // Archetype table (likely arena-backed via allocator->ctx)
  Arena *arena = (Arena *)world->alloc->ctx;
  world->arch_table = vkr_hash_table_create_VkrArchetype(
      arena, info->initial_archetypes ? info->initial_archetypes : 16u);

  world->arch_list = NULL;
  world->arch_capacity = 0u;
  world->arch_count = 0u;

  // Ensure EMPTY archetype exists
  (void)vkr_entity_archetype_get_or_create(world, NULL, 0);

  return world;
}

void vkr_entity_destroy_world(VkrWorld *world) {
  if (!world)
    return;

  // Free archetypes and chunks (if not using arena)
  for (uint32_t i = 0; i < world->arch_count; ++i) {
    VkrArchetype *arch = world->arch_list[i];
    if (!arch)
      continue;

    // Free chunks
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

    // Free archetype arrays
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
      vkr_entity_free(world, (void *)arch->key,
                      (uint64_t)(strlen(arch->key) + 1),
                      VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }

    vkr_entity_free(world, arch, sizeof(VkrArchetype),
                    VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }

  // Free arch_list
  if (world->arch_list && world->arch_capacity) {
    vkr_entity_free(world, (void *)world->arch_list,
                    world->arch_capacity * sizeof(VkrArchetype *),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  // Free component registry
  if (world->components && world->comp_capacity) {
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
  return vkr_entity_comps_add(world, name, size, align);
}

const VkrComponentInfo *vkr_entity_get_component_info(const VkrWorld *world,
                                                      VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  assert_log(type < VKR_ECS_MAX_COMPONENTS, "Component id out of range");

  if (type == VKR_COMPONENT_TYPE_INVALID || type >= world->comp_count)
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

  // Bump generation on create to avoid u64 == 0 and ensure uniqueness
  uint16_t gen = (uint16_t)(world->dir.generations[idx] + 1u);
  if (gen == 0) // skip 0 on wrap
    gen = 1u;
  world->dir.generations[idx] = gen;

  VkrEntityId id = vkr_entity_id_make(idx, gen, world->world_id);

  // Insert into EMPTY archetype
  VkrArchetype *empty = vkr_entity_archetype_get_or_create(world, NULL, 0);
  VkrChunk *chunk = vkr_entity_archetype_acquire_chunk(world, empty);
  assert_log(chunk != NULL, "Failed to acquire chunk");

  uint32_t slot = chunk->count++;
  chunk->ents[slot] = id;

  world->dir.records[idx] = (VkrEntityRecord){.chunk = chunk, .slot = slot};

  return id;
}

bool8_t vkr_entity_is_alive(const VkrWorld *world, VkrEntityId id) {
  assert_log(world, "World must not be NULL");
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");

  uint32_t idx = id.parts.index;
  if (idx >= world->dir.capacity)
    return false_v;

  return world->dir.generations[idx] == id.parts.generation ? true_v : false_v;
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
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");

  if (!vkr_entity_is_alive(world, id))
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
  assert_log(type < VKR_COMPONENT_TYPE_INVALID, "Type out of range");

  // types[] are sorted ascending; binary search optional.
  int32_t lo = 0, hi = (int32_t)archetype->comp_count - 1;
  while (lo <= hi) {
    int32_t mid = (lo + hi) / 2;
    if (archetype->types[mid] == type)
      return mid;
    if (archetype->types[mid] < type)
      lo = mid + 1;
    else
      hi = mid - 1;
  }

  return -1;
}

// Move entity from src archetype to dst archetype (add/remove component)
vkr_internal INLINE bool32_t vkr_entity_move_entity(
    VkrWorld *world, VkrEntityId id, VkrArchetype *dst,
    VkrComponentTypeId added_type, const void *added_init_or_null) {
  assert_log(world, "World must not be NULL");
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");
  assert_log(dst, "Destination archetype must not be NULL");
  assert_log(added_type == VKR_COMPONENT_TYPE_INVALID ||
                 added_type < VKR_COMPONENT_TYPE_INVALID,
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
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");
  assert_log(type < VKR_COMPONENT_TYPE_INVALID, "Type out of range");

  if (!vkr_entity_is_alive(world, id))
    return false_v;

  uint32_t idx = id.parts.index;
  VkrEntityRecord rec = world->dir.records[idx];
  VkrChunk *chunk = rec.chunk;
  VkrArchetype *archetype = chunk->arch;

  if (vkr_entity_arch_find_col(archetype, type) >= 0)
    return true_v; // already has it

  // Build dst types = src types + type
  uint32_t comp_count = archetype->comp_count + 1u;
  VkrComponentTypeId stack_types[64];
  VkrComponentTypeId *dst_types =
      (comp_count <= 64)
          ? stack_types
          : vkr_entity_alloc(world, comp_count * sizeof(VkrComponentTypeId),
                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp)
    dst_types[comp] = archetype->types[comp];
  dst_types[archetype->comp_count] = type;

  VkrArchetype *dst =
      vkr_entity_archetype_get_or_create(world, dst_types, comp_count);
  if (dst_types != stack_types)
    vkr_entity_free(world, dst_types, comp_count * sizeof(VkrComponentTypeId),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!dst)
    return false_v;

  return vkr_entity_move_entity(world, id, dst, type, init_data);
}

bool8_t vkr_entity_remove_component(VkrWorld *world, VkrEntityId id,
                                    VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");
  assert_log(type < VKR_COMPONENT_TYPE_INVALID, "Type out of range");

  if (!vkr_entity_is_alive(world, id))
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
  VkrComponentTypeId stack_types[64];
  VkrComponentTypeId *dst_types =
      (comp_count <= 64)
          ? stack_types
          : vkr_entity_alloc(world, comp_count * sizeof(VkrComponentTypeId),
                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t j = 0;
  for (uint32_t comp = 0; comp < archetype->comp_count; ++comp) {
    if (archetype->types[comp] != type)
      dst_types[j++] = archetype->types[comp];
  }

  VkrArchetype *dst =
      vkr_entity_archetype_get_or_create(world, dst_types, comp_count);
  if (dst_types != stack_types)
    vkr_entity_free(world, dst_types, comp_count * sizeof(VkrComponentTypeId),
                    VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!dst)
    return false_v;

  return vkr_entity_move_entity(world, id, dst, VKR_COMPONENT_TYPE_INVALID,
                                NULL);
}

void *vkr_entity_get_component_mut(VkrWorld *world, VkrEntityId id,
                                   VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");
  assert_log(type < VKR_COMPONENT_TYPE_INVALID, "Type out of range");

  if (!vkr_entity_is_alive(world, id))
    return NULL;

  VkrEntityRecord rec = world->dir.records[id.parts.index];
  VkrArchetype *archetype = rec.chunk->arch;
  int32_t col_i = vkr_entity_arch_find_col(archetype, type);
  if (col_i < 0)
    return NULL;

  uint8_t *col = (uint8_t *)rec.chunk->columns[col_i];
  return (void *)(col + (size_t)archetype->sizes[col_i] * rec.slot);
}

const void *vkr_entity_get_component(const VkrWorld *world, VkrEntityId id,
                                     VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");
  assert_log(type < VKR_COMPONENT_TYPE_INVALID, "Type out of range");
  return vkr_entity_get_component_mut((VkrWorld *)world, id, type);
}

bool8_t vkr_entity_has_component(const VkrWorld *world, VkrEntityId id,
                                 VkrComponentTypeId type) {
  assert_log(world, "World must not be NULL");
  assert_log(id.parts.index < world->dir.capacity, "Index out of range");
  assert_log(type < VKR_COMPONENT_TYPE_INVALID, "Type out of range");

  if (!vkr_entity_is_alive(world, id))
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
  for (uint32_t comp = 0; comp < include_count; ++comp)
    vkr_entity_sig_set(&out_query->include, include_types[comp]);
  for (uint32_t comp = 0; comp < exclude_count; ++comp)
    vkr_entity_sig_set(&out_query->exclude, exclude_types[comp]);
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
  assert_log(type < VKR_COMPONENT_TYPE_INVALID, "Type out of range");

  int32_t col_i = vkr_entity_arch_find_col(chunk->arch, type);
  if (col_i < 0)
    return NULL;
  return chunk->columns[col_i];
}

const void *vkr_entity_chunk_column_const(const VkrChunk *chunk,
                                          VkrComponentTypeId type) {
  assert_log(chunk, "Chunk must not be NULL");
  assert_log(type < VKR_COMPONENT_TYPE_INVALID, "Type out of range");

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