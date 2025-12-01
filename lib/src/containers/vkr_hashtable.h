/**
 * @file vkr_hashtable.h
 * @brief Open-addressing hash table using the abstract allocator API.
 */

#pragma once

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

#define VKR_HASH_TABLE_LOAD_FACTOR 0.75
#define VKR_HASH_TABLE_MAX_PROBES 128
#define VKR_HASH_TABLE_INITIAL_CAPACITY 16
#define VKR_HASH_TABLE_FNV_OFFSET_BASIS 14695981039346656037ULL
#define VKR_HASH_TABLE_FNV_PRIME 1099511628211ULL

typedef enum VkrOccupancyState {
  VKR_OCCUPIED = 1,
  VKR_TOMBSTONE = 2,
  VKR_EMPTY = 0,
} VkrOccupancyState;

#define VkrHashTable(type) VkrHashTableConstructor(type, type)

#define VkrHashTableConstructor(type, name)                                    \
  typedef struct VkrHashEntry_##name {                                         \
    const char *key;                                                           \
    type value;                                                                \
    VkrOccupancyState occupied;                                                \
  } VkrHashEntry_##name;                                                       \
                                                                              \
  typedef struct VkrHashTable_##name {                                         \
    VkrAllocator *allocator;                                                   \
    uint64_t capacity;                                                         \
    uint64_t size;                                                             \
    VkrHashEntry_##name *entries;                                              \
  } VkrHashTable_##name;                                                       \
                                                                              \
  vkr_internal INLINE uint64_t vkr_hash_name_##name(const char *key,           \
                                                    uint64_t capacity) {       \
    assert_log(key != NULL, "Name must not be NULL");                          \
    uint64_t hash = VKR_HASH_TABLE_FNV_OFFSET_BASIS;                           \
    for (const char *p = key; *p; p++) {                                       \
      hash ^= (uint64_t)(unsigned char)*p;                                     \
      hash *= VKR_HASH_TABLE_FNV_PRIME;                                        \
    }                                                                          \
    return hash % capacity;                                                    \
  }                                                                            \
                                                                              \
  /* Internal insert without resizing; expects capacity headroom. */           \
  vkr_internal INLINE bool32_t vkr_hash_table_insert_internal_##name(          \
      VkrHashTable_##name *table, const char *key, type value) {               \
    uint64_t index = vkr_hash_name_##name(key, table->capacity);               \
    uint64_t probes = 0;                                                       \
    uint64_t first_tombstone = UINT64_MAX;                                     \
    while (table->entries[index].occupied != VKR_EMPTY) {                      \
      if (table->entries[index].occupied == VKR_OCCUPIED &&                    \
          string_equals(table->entries[index].key, key)) {                     \
        table->entries[index].value = value;                                   \
        return true_v;                                                         \
      }                                                                        \
      if (table->entries[index].occupied == VKR_TOMBSTONE &&                   \
          first_tombstone == UINT64_MAX) {                                     \
        first_tombstone = index;                                               \
      }                                                                        \
      index = (index + 1) % table->capacity;                                   \
      probes++;                                                                \
      if (probes >= VKR_HASH_TABLE_MAX_PROBES || probes >= table->capacity) {  \
        log_error("Hash table probe limit exceeded for key: %s", key);         \
        return false_v;                                                        \
      }                                                                        \
    }                                                                          \
                                                                              \
    if (first_tombstone != UINT64_MAX) {                                       \
      index = first_tombstone;                                                 \
    }                                                                          \
                                                                              \
    table->entries[index].key = key;                                           \
    table->entries[index].value = value;                                       \
    table->entries[index].occupied = VKR_OCCUPIED;                             \
    table->size++;                                                             \
    return true_v;                                                             \
  }                                                                            \
                                                                              \
  vkr_internal INLINE VkrHashTable_##name vkr_hash_table_create_##name(        \
      VkrAllocator *allocator, uint64_t capacity) {                            \
    assert_log(allocator != NULL, "Allocator must not be NULL");               \
    assert_log(capacity > 0, "Capacity must be greater than 0");               \
    VkrHashTable_##name table = {0};                                           \
    table.allocator = allocator;                                               \
    table.capacity = capacity;                                                 \
    table.size = 0;                                                            \
    table.entries = vkr_allocator_alloc(                                       \
        allocator, capacity * sizeof(VkrHashEntry_##name),                    \
        VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);                                  \
    assert_log(table.entries != NULL, "alloc failed for hash table entries");  \
    MemZero(table.entries, capacity * sizeof(VkrHashEntry_##name));            \
    return table;                                                              \
  }                                                                            \
                                                                              \
  vkr_internal INLINE void vkr_hash_table_destroy_##name(                      \
      VkrHashTable_##name *table) {                                            \
    if (!table) {                                                              \
      return;                                                                  \
    }                                                                          \
    if (table->allocator && table->entries) {                                  \
      vkr_allocator_free(table->allocator, table->entries,                     \
                         table->capacity * sizeof(VkrHashEntry_##name),        \
                         VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);                 \
    }                                                                          \
    table->entries = NULL;                                                     \
    table->allocator = NULL;                                                   \
    table->capacity = 0;                                                       \
    table->size = 0;                                                           \
  }                                                                            \
                                                                              \
  vkr_internal INLINE void vkr_hash_table_resize_##name(                       \
      VkrHashTable_##name *table, uint64_t new_capacity) {                     \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(table->allocator != NULL, "Allocator must not be NULL");        \
    assert_log(new_capacity > 0, "New capacity must be greater than 0");       \
                                                                              \
    VkrHashEntry_##name *old_entries = table->entries;                         \
    uint64_t old_capacity = table->capacity;                                   \
                                                                              \
    VkrHashEntry_##name *new_entries = vkr_allocator_alloc(                    \
        table->allocator, new_capacity * sizeof(VkrHashEntry_##name),         \
        VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);                                  \
    assert_log(new_entries != NULL, "alloc failed for resized hash table");    \
    MemZero(new_entries, new_capacity * sizeof(VkrHashEntry_##name));          \
                                                                              \
    table->entries = new_entries;                                              \
    table->capacity = new_capacity;                                            \
    table->size = 0;                                                           \
                                                                              \
    if (old_entries) {                                                         \
      for (uint64_t i = 0; i < old_capacity; ++i) {                            \
        if (old_entries[i].occupied == VKR_OCCUPIED) {                         \
          vkr_hash_table_insert_internal_##name(table,                         \
                                                 old_entries[i].key,           \
                                                 old_entries[i].value);        \
        }                                                                      \
      }                                                                        \
      vkr_allocator_free(table->allocator, old_entries,                        \
                         old_capacity * sizeof(VkrHashEntry_##name),           \
                         VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);                 \
    }                                                                          \
  }                                                                            \
                                                                              \
  vkr_internal INLINE void vkr_hash_table_reset_##name(                        \
      VkrHashTable_##name *table) {                                            \
    assert_log(table != NULL, "Table must not be NULL");                       \
    if (table->entries) {                                                      \
      MemZero(table->entries, table->capacity * sizeof(VkrHashEntry_##name));  \
    }                                                                          \
    table->size = 0;                                                           \
  }                                                                            \
                                                                              \
  vkr_internal INLINE bool32_t vkr_hash_table_insert_##name(                   \
      VkrHashTable_##name *table, const char *key, type value) {               \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(table->allocator != NULL, "Allocator must not be NULL");        \
    assert_log(key != NULL, "Key must not be NULL");                           \
                                                                              \
    if (table->size >=                                                         \
        (uint64_t)(table->capacity * VKR_HASH_TABLE_LOAD_FACTOR)) {            \
      uint64_t new_capacity = table->capacity > 0                              \
                                  ? table->capacity * 2                        \
                                  : VKR_HASH_TABLE_INITIAL_CAPACITY;           \
      vkr_hash_table_resize_##name(table, new_capacity);                       \
    }                                                                          \
                                                                              \
    return vkr_hash_table_insert_internal_##name(table, key, value);           \
  }                                                                            \
                                                                              \
  vkr_internal INLINE bool8_t vkr_hash_table_remove_##name(                    \
      VkrHashTable_##name *table, const char *key) {                           \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    if (!table->entries) {                                                     \
      return false_v;                                                          \
    }                                                                          \
                                                                              \
    uint64_t index = vkr_hash_name_##name(key, table->capacity);               \
    uint64_t probes = 0;                                                       \
    while (table->entries[index].occupied != VKR_EMPTY) {                      \
      if (table->entries[index].occupied == VKR_OCCUPIED &&                    \
          string_equals(table->entries[index].key, key)) {                     \
        table->entries[index].occupied = VKR_TOMBSTONE;                        \
        table->entries[index].key = NULL;                                      \
        table->size--;                                                         \
        return true_v;                                                         \
      }                                                                        \
      index = (index + 1) % table->capacity;                                   \
      probes++;                                                                \
      if (probes >= VKR_HASH_TABLE_MAX_PROBES || probes >= table->capacity) {  \
        return false_v;                                                        \
      }                                                                        \
    }                                                                          \
    return false_v;                                                            \
  }                                                                            \
                                                                              \
  vkr_internal INLINE type *vkr_hash_table_get_##name(                         \
      const VkrHashTable_##name *table, const char *key) {                     \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    if (!table->entries) {                                                     \
      return NULL;                                                             \
    }                                                                          \
                                                                              \
    uint64_t index = vkr_hash_name_##name(key, table->capacity);               \
    uint64_t probes = 0;                                                       \
    while (table->entries[index].occupied != VKR_EMPTY) {                      \
      if (table->entries[index].occupied == VKR_OCCUPIED &&                    \
          string_equals(table->entries[index].key, key)) {                     \
        return &table->entries[index].value;                                   \
      }                                                                        \
      index = (index + 1) % table->capacity;                                   \
      probes++;                                                                \
      if (probes >= VKR_HASH_TABLE_MAX_PROBES || probes >= table->capacity) {  \
        return NULL;                                                           \
      }                                                                        \
    }                                                                          \
    return NULL;                                                               \
  }                                                                            \
                                                                              \
  vkr_internal INLINE bool8_t vkr_hash_table_contains_##name(                  \
      const VkrHashTable_##name *table, const char *key) {                     \
    return vkr_hash_table_get_##name(table, key) != NULL ? true_v : false_v;   \
  }                                                                            \
                                                                              \
  vkr_internal INLINE bool8_t vkr_hash_table_is_empty_##name(                  \
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
