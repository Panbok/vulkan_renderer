/**
 * @file vkr_hashtable.h
 * @brief Open-addressing hash table using the abstract allocator API.
 */

#pragma once

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

#define VKR_HASH_TABLE_INITIAL_CAPACITY 16
#define VKR_HASH_TABLE_FNV_OFFSET_BASIS 14695981039346656037ULL
#define VKR_HASH_TABLE_FNV_PRIME 1099511628211ULL
#define VKR_HASH_TABLE_NOT_FOUND UINT64_MAX

typedef enum VkrOccupancyState {
  VKR_OCCUPIED = 1,
  VKR_TOMBSTONE = 2,
  VKR_EMPTY = 0,
} VkrOccupancyState;

// Keys are borrowed: an entry keeps the pointer passed to the insert that
// created it until that key is removed, and a later insert of an equal key
// updates only the value. Capacity is zero or a power of two; creation and
// resize round a request up. Live keys plus tombstones stay at or below three
// quarters of capacity, so every probe sequence ends at an empty slot.
// Creation returns an all-zero record on failure. Resize and insert commit
// only on success; failure preserves the entries, capacity and mappings. An
// insert of a new key can rehash, which invalidates borrowed value pointers;
// remove never moves entries.
#define VkrHashTable(type) VkrHashTableConstructor(type, type)

/* A key view with its hash. C-string and String8 keys with the same bytes
 * have the same hash. */
typedef struct VkrHashTableKey {
  const uint8_t *bytes;
  uint64_t length;
  uint64_t hash;
} VkrHashTableKey;

/* The MurmurHash3 fmix64 finalizer. FNV-1a alone leaves the low bits of the
 * hash dependent only on the low bits of each byte, and the index mask reads
 * the low bits. */
vkr_internal INLINE VKR_MAYBE_UNUSED uint64_t
vkr_hash_table_finalize(uint64_t hash) {
  hash ^= hash >> 33u;
  hash *= 0xff51afd7ed558ccdULL;
  hash ^= hash >> 33u;
  hash *= 0xc4ceb9fe1a85ec53ULL;
  hash ^= hash >> 33u;
  return hash;
}

vkr_internal INLINE VKR_MAYBE_UNUSED VkrHashTableKey
vkr_hash_table_key_string8(String8 key) {
  uint64_t hash = VKR_HASH_TABLE_FNV_OFFSET_BASIS;
  for (uint64_t i = 0; i < key.length; ++i) {
    hash ^= (uint64_t)key.str[i];
    hash *= VKR_HASH_TABLE_FNV_PRIME;
  }
  return (VkrHashTableKey){.bytes = key.str,
                           .length = key.length,
                           .hash = vkr_hash_table_finalize(hash)};
}

vkr_internal INLINE VKR_MAYBE_UNUSED VkrHashTableKey
vkr_hash_table_key_cstr(const char *key) {
  assert_log(key != NULL, "Key must not be NULL");
  uint64_t hash = VKR_HASH_TABLE_FNV_OFFSET_BASIS;
  uint64_t length = 0;
  while (key[length] != '\0') {
    hash ^= (uint64_t)(unsigned char)key[length];
    hash *= VKR_HASH_TABLE_FNV_PRIME;
    length++;
  }
  return (VkrHashTableKey){.bytes = (const uint8_t *)key,
                           .length = length,
                           .hash = vkr_hash_table_finalize(hash)};
}

/* The hash an entry stores for a C-string key. */
vkr_internal INLINE VKR_MAYBE_UNUSED uint64_t
vkr_hash_table_hash_cstr(const char *key) {
  return vkr_hash_table_key_cstr(key).hash;
}

/* Compares a stored, null-terminated key with a view without measuring the
 * stored key first. */
vkr_internal INLINE VKR_MAYBE_UNUSED bool8_t
vkr_hash_table_key_equals(const char *stored, VkrHashTableKey key) {
  for (uint64_t i = 0; i < key.length; ++i) {
    if (stored[i] == '\0' || (uint8_t)stored[i] != key.bytes[i]) {
      return false_v;
    }
  }
  return stored[key.length] == '\0' ? true_v : false_v;
}

/* The most live keys plus tombstones a capacity holds: three quarters of it.
 * The result is below the capacity, so an empty slot always remains. */
vkr_internal INLINE VKR_MAYBE_UNUSED uint64_t
vkr_hash_table_max_used(uint64_t capacity) {
  return (capacity / 4u) * 3u + ((capacity % 4u) * 3u) / 4u;
}

/* The smallest power of two at or above `requested`, or 0 when `requested`
 * is 0 or the result exceeds `limit` entries. */
vkr_internal INLINE VKR_MAYBE_UNUSED uint64_t
vkr_hash_table_round_capacity(uint64_t requested, uint64_t limit) {
  if (requested == 0u || requested > limit) {
    return 0u;
  }
  uint64_t capacity = 1u;
  while (capacity < requested) {
    capacity <<= 1u;
  }
  return capacity <= limit ? capacity : 0u;
}

#define VkrHashTableConstructor(type, name)                                    \
  typedef struct VkrHashEntry_##name {                                         \
    uint64_t hash;                                                             \
    const char *key;                                                           \
    VkrOccupancyState occupied;                                                \
    type value;                                                                \
  } VkrHashEntry_##name;                                                       \
                                                                               \
  typedef struct VkrHashTable_##name {                                         \
    VkrAllocator *allocator;                                                   \
    uint64_t capacity;   /* Zero or a power of two. */                         \
    uint64_t size;       /* Live keys. */                                      \
    uint64_t tombstones; /* Removed slots inside probe sequences. */           \
    VkrHashEntry_##name *entries;                                              \
  } VkrHashTable_##name;                                                       \
                                                                               \
  /* Index of `key`, or VKR_HASH_TABLE_NOT_FOUND. A miss stores in `out_slot`  \
   * the slot an insert uses: the first tombstone on the probe path, else the  \
   * empty slot that ends it. Requires entries. */                             \
  vkr_internal INLINE VKR_MAYBE_UNUSED uint64_t vkr_hash_table_find_##name(    \
      const VkrHashTable_##name *table, VkrHashTableKey key,                   \
      uint64_t *out_slot) {                                                    \
    const uint64_t mask = table->capacity - 1u;                                \
    uint64_t index = key.hash & mask;                                          \
    uint64_t slot = VKR_HASH_TABLE_NOT_FOUND;                                  \
    for (uint64_t probe = 0; probe < table->capacity; ++probe) {               \
      const VkrHashEntry_##name *entry = &table->entries[index];               \
      if (entry->occupied == VKR_EMPTY) {                                      \
        if (slot == VKR_HASH_TABLE_NOT_FOUND) {                                \
          slot = index;                                                        \
        }                                                                      \
        break;                                                                 \
      }                                                                        \
      if (entry->occupied == VKR_TOMBSTONE) {                                  \
        if (slot == VKR_HASH_TABLE_NOT_FOUND) {                                \
          slot = index;                                                        \
        }                                                                      \
      } else if (entry->hash == key.hash &&                                    \
                 vkr_hash_table_key_equals(entry->key, key)) {                 \
        return index;                                                          \
      }                                                                        \
      index = (index + 1u) & mask;                                             \
    }                                                                          \
    if (out_slot) {                                                            \
      *out_slot = slot;                                                        \
    }                                                                          \
    return VKR_HASH_TABLE_NOT_FOUND;                                           \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED VKR_MUST_USE                            \
      VkrHashTable_##name vkr_hash_table_create_##name(                        \
          VkrAllocator *allocator, uint64_t capacity) {                        \
    assert_log(allocator != NULL, "Allocator must not be NULL");               \
    const uint64_t rounded = vkr_hash_table_round_capacity(                    \
        capacity, SIZE_MAX / sizeof(VkrHashEntry_##name));                     \
    if (rounded == 0u) {                                                       \
      return (VkrHashTable_##name){0};                                         \
    }                                                                          \
    VkrHashEntry_##name *entries =                                             \
        vkr_allocator_alloc(allocator, rounded * sizeof(VkrHashEntry_##name),  \
                            VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);              \
    if (!entries) {                                                            \
      return (VkrHashTable_##name){0};                                         \
    }                                                                          \
    MemZero(entries, rounded * sizeof(VkrHashEntry_##name));                   \
    return (VkrHashTable_##name){                                              \
        .allocator = allocator, .capacity = rounded, .entries = entries};      \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED void vkr_hash_table_destroy_##name(     \
      VkrHashTable_##name *table) {                                            \
    if (!table) {                                                              \
      return;                                                                  \
    }                                                                          \
    if (table->allocator && table->entries) {                                  \
      vkr_allocator_free(table->allocator, table->entries,                     \
                         table->capacity * sizeof(VkrHashEntry_##name),        \
                         VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);                 \
    }                                                                          \
    *table = (VkrHashTable_##name){0};                                         \
  }                                                                            \
                                                                               \
  /* Moves the live entries into new storage of `capacity`, a power of two     \
   * that holds them, and drops the tombstones. Stored hashes place the        \
   * entries without hashing or comparing keys. Failure changes nothing. */    \
  vkr_internal INLINE VKR_MAYBE_UNUSED VKR_MUST_USE bool32_t                   \
      vkr_hash_table_rehash_##name(VkrHashTable_##name *table,                 \
                                   uint64_t capacity) {                        \
    VkrHashEntry_##name *entries = vkr_allocator_alloc(                        \
        table->allocator, capacity * sizeof(VkrHashEntry_##name),              \
        VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);                                  \
    if (!entries) {                                                            \
      return false_v;                                                          \
    }                                                                          \
    MemZero(entries, capacity * sizeof(VkrHashEntry_##name));                  \
                                                                               \
    const uint64_t mask = capacity - 1u;                                       \
    for (uint64_t i = 0; i < table->capacity; ++i) {                           \
      const VkrHashEntry_##name *entry = &table->entries[i];                   \
      if (entry->occupied != VKR_OCCUPIED) {                                   \
        continue;                                                              \
      }                                                                        \
      uint64_t index = entry->hash & mask;                                     \
      while (entries[index].occupied != VKR_EMPTY) {                           \
        index = (index + 1u) & mask;                                           \
      }                                                                        \
      entries[index] = *entry;                                                 \
    }                                                                          \
                                                                               \
    if (table->entries) {                                                      \
      vkr_allocator_free(table->allocator, table->entries,                     \
                         table->capacity * sizeof(VkrHashEntry_##name),        \
                         VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);                 \
    }                                                                          \
    table->entries = entries;                                                  \
    table->capacity = capacity;                                                \
    table->tombstones = 0;                                                     \
    return true_v;                                                             \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED VKR_MUST_USE bool32_t                   \
      vkr_hash_table_resize_##name(VkrHashTable_##name *table,                 \
                                   uint64_t capacity) {                        \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(table->allocator != NULL, "Allocator must not be NULL");        \
    const uint64_t rounded = vkr_hash_table_round_capacity(                    \
        capacity, SIZE_MAX / sizeof(VkrHashEntry_##name));                     \
    if (rounded == 0u || table->size > vkr_hash_table_max_used(rounded)) {     \
      return false_v;                                                          \
    }                                                                          \
    return vkr_hash_table_rehash_##name(table, rounded);                       \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED void vkr_hash_table_reset_##name(       \
      VkrHashTable_##name *table) {                                            \
    assert_log(table != NULL, "Table must not be NULL");                       \
    if (table->entries) {                                                      \
      MemZero(table->entries, table->capacity * sizeof(VkrHashEntry_##name));  \
    }                                                                          \
    table->size = 0;                                                           \
    table->tombstones = 0;                                                     \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED bool8_t vkr_hash_table_remove_##name(   \
      VkrHashTable_##name *table, const char *key) {                           \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    if (!table->entries) {                                                     \
      return false_v;                                                          \
    }                                                                          \
                                                                               \
    uint64_t index =                                                           \
        vkr_hash_table_find_##name(table, vkr_hash_table_key_cstr(key), NULL); \
    if (index == VKR_HASH_TABLE_NOT_FOUND) {                                   \
      return false_v;                                                          \
    }                                                                          \
    table->entries[index].occupied = VKR_TOMBSTONE;                            \
    table->entries[index].key = NULL;                                          \
    table->size--;                                                             \
    table->tombstones++;                                                       \
                                                                               \
    /* No probe sequence crosses a tombstone that an empty slot follows, so    \
     * that tombstone and the run of tombstones before it become empty. */     \
    const uint64_t mask = table->capacity - 1u;                                \
    if (table->entries[(index + 1u) & mask].occupied == VKR_EMPTY) {           \
      while (table->entries[index].occupied == VKR_TOMBSTONE) {                \
        table->entries[index].occupied = VKR_EMPTY;                            \
        table->tombstones--;                                                   \
        index = (index - 1u) & mask;                                           \
      }                                                                        \
    }                                                                          \
    return true_v;                                                             \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED type *vkr_hash_table_get_##name(        \
      const VkrHashTable_##name *table, const char *key) {                     \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    if (!table->entries) {                                                     \
      return NULL;                                                             \
    }                                                                          \
                                                                               \
    const uint64_t index =                                                     \
        vkr_hash_table_find_##name(table, vkr_hash_table_key_cstr(key), NULL); \
    return index == VKR_HASH_TABLE_NOT_FOUND ? NULL                            \
                                             : &table->entries[index].value;   \
  }                                                                            \
                                                                               \
  /* The view need not be null-terminated; stored keys still are. */           \
  vkr_internal INLINE                                                          \
      VKR_MAYBE_UNUSED type *vkr_hash_table_get_string8_##name(                \
          const VkrHashTable_##name *table, String8 key) {                     \
    assert_log(table != NULL, "Table must not be NULL");                       \
    if (!table->entries || !key.str) {                                         \
      return NULL;                                                             \
    }                                                                          \
                                                                               \
    const uint64_t index = vkr_hash_table_find_##name(                         \
        table, vkr_hash_table_key_string8(key), NULL);                         \
    return index == VKR_HASH_TABLE_NOT_FOUND ? NULL                            \
                                             : &table->entries[index].value;   \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED VKR_MUST_USE bool32_t                   \
      vkr_hash_table_insert_##name(VkrHashTable_##name *table,                 \
                                   const char *key, type value) {              \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(table->allocator != NULL, "Allocator must not be NULL");        \
    assert_log(key != NULL, "Key must not be NULL");                           \
    const VkrHashTableKey hashed = vkr_hash_table_key_cstr(key);               \
    uint64_t slot = VKR_HASH_TABLE_NOT_FOUND;                                  \
    if (table->entries) {                                                      \
      const uint64_t found = vkr_hash_table_find_##name(table, hashed, &slot); \
      if (found != VKR_HASH_TABLE_NOT_FOUND) {                                 \
        table->entries[found].value = value;                                   \
        return true_v;                                                         \
      }                                                                        \
    }                                                                          \
                                                                               \
    /* A tombstone slot keeps the used count; an empty slot needs headroom.    \
     * Without headroom, rehash at the same capacity while live keys fill at   \
     * most half the limit, so a purge repays its cost; otherwise double. */   \
    const bool8_t has_room =                                                   \
        table->entries && (table->entries[slot].occupied == VKR_TOMBSTONE ||   \
                           table->size + table->tombstones <                   \
                               vkr_hash_table_max_used(table->capacity));      \
    if (!has_room) {                                                           \
      uint64_t capacity = table->capacity;                                     \
      if (capacity == 0u) {                                                    \
        capacity = VKR_HASH_TABLE_INITIAL_CAPACITY;                            \
      } else if ((table->size + 1u) * 2u >                                     \
                 vkr_hash_table_max_used(capacity)) {                          \
        if (capacity > SIZE_MAX / sizeof(VkrHashEntry_##name) / 2u) {          \
          return false_v;                                                      \
        }                                                                      \
        capacity *= 2u;                                                        \
      }                                                                        \
      if (!vkr_hash_table_rehash_##name(table, capacity)) {                    \
        return false_v;                                                        \
      }                                                                        \
      (void)vkr_hash_table_find_##name(table, hashed, &slot);                  \
    }                                                                          \
    assert_log(slot != VKR_HASH_TABLE_NOT_FOUND, "Probe found no free slot");  \
                                                                               \
    VkrHashEntry_##name *entry = &table->entries[slot];                        \
    if (entry->occupied == VKR_TOMBSTONE) {                                    \
      table->tombstones--;                                                     \
    }                                                                          \
    entry->hash = hashed.hash;                                                 \
    entry->key = key;                                                          \
    entry->occupied = VKR_OCCUPIED;                                            \
    entry->value = value;                                                      \
    table->size++;                                                             \
    return true_v;                                                             \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED bool8_t vkr_hash_table_contains_##name( \
      const VkrHashTable_##name *table, const char *key) {                     \
    return vkr_hash_table_get_##name(table, key) != NULL ? true_v : false_v;   \
  }                                                                            \
                                                                               \
  vkr_internal INLINE VKR_MAYBE_UNUSED bool8_t vkr_hash_table_is_empty_##name( \
      const VkrHashTable_##name *table) {                                      \
    assert_log(table != NULL, "Table must not be NULL");                       \
    return table->size == 0 ? true_v : false_v;                                \
  }

VkrHashTable(uint8_t);
VkrHashTable(uint16_t);
VkrHashTable(uint32_t);
VkrHashTable(uint64_t);
VkrHashTable(float32_t);
VkrHashTable(float64_t);
VkrHashTable(String8);
VkrHashTable(bool8_t);
