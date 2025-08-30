#pragma once

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/arena.h"

#define VKR_HASH_TABLE_LOAD_FACTOR 0.75
#define VKR_HASH_TABLE_MAX_PROBES                                              \
  128 // Prevent infinite loops in worst-case scenarios
#define VKR_HASH_TABLE_INITIAL_CAPACITY                                        \
  16 // Optional: Default initial capacity if not specified
#define VKR_HASH_TABLE_FNV_OFFSET_BASIS 14695981039346656037ULL
#define VKR_HASH_TABLE_FNV_PRIME 1099511628211ULL

typedef enum VkrOccupancyState {
  VKR_OCCUPIED = 1,
  VKR_TOMBSTONE = 2,
  VKR_EMPTY = 0,
} VkrOccupancyState;

#define VkrHashTableConstructor(type, name)                                    \
  typedef struct VkrHashEntry_##name {                                         \
    const char *key;                                                           \
    type value;                                                                \
    VkrOccupancyState occupied; /* 0: empty, 1: occupied, 2: tombstone */      \
  } VkrHashEntry_##name;                                                       \
  typedef struct VkrHashTable_##name {                                         \
    Arena *arena;                                                              \
    uint64_t capacity;                                                         \
    uint64_t size;                                                             \
    VkrHashEntry_##name *entries;                                              \
  } VkrHashTable_##name;                                                       \
  vkr_internal INLINE uint64_t vkr_hash_name_##name(const char *key,           \
                                                    uint64_t capacity) {       \
    assert_log(key != NULL, "Name must not be NULL");                          \
    uint64_t hash = 14695981039346656037ULL;                                   \
    for (const char *p = key; *p; p++) {                                       \
      hash ^= (uint64_t)(unsigned char)*p;                                     \
      hash *= 1099511628211ULL;                                                \
    }                                                                          \
    return hash % capacity;                                                    \
  }                                                                            \
  vkr_internal INLINE VkrHashTable_##name vkr_hash_table_create_##name(        \
      Arena *arena, uint64_t capacity) {                                       \
    assert_log(arena != NULL, "Arena must not be NULL");                       \
    assert_log(capacity > 0, "Capacity must be greater than 0");               \
    VkrHashTable_##name table = {0};                                           \
    table.arena = arena;                                                       \
    table.capacity = capacity;                                                 \
    table.size = 0;                                                            \
    table.entries = arena_alloc(arena, capacity * sizeof(VkrHashEntry_##name), \
                                ARENA_MEMORY_TAG_HASH_TABLE);                  \
    assert_log(table.entries != NULL, "arena_alloc failed for entries");       \
    MemZero(table.entries, capacity * sizeof(VkrHashEntry_##name));            \
    return table;                                                              \
  }                                                                            \
  /* Forward declarations to avoid implicit non-static declarations */         \
  vkr_internal INLINE bool32_t vkr_hash_table_insert_##name(                   \
      VkrHashTable_##name *table, const char *key, type value);                \
  vkr_internal INLINE void vkr_hash_table_resize_##name(                       \
      VkrHashTable_##name *table, uint64_t new_capacity);                      \
                                                                               \
  vkr_internal INLINE void vkr_hash_table_resize_##name(                       \
      VkrHashTable_##name *table, uint64_t new_capacity) {                     \
    assert_log(table != NULL, "Table must not be NULL");                       \
    VkrHashTable_##name new_table =                                            \
        vkr_hash_table_create_##name(table->arena, new_capacity);              \
    for (uint64_t i = 0; i < table->capacity; i++) {                           \
      if (table->entries[i].occupied == VKR_OCCUPIED) {                        \
        vkr_hash_table_insert_##name(&new_table, table->entries[i].key,        \
                                     table->entries[i].value);                 \
      }                                                                        \
    }                                                                          \
    table->entries = new_table.entries;                                        \
    table->capacity = new_table.capacity;                                      \
    table->size = new_table.size;                                              \
  }                                                                            \
  vkr_internal INLINE void vkr_hash_table_reset_##name(                        \
      VkrHashTable_##name *table) {                                            \
    assert_log(table != NULL, "Table must not be NULL");                       \
    if (table->entries) {                                                      \
      MemZero(table->entries, table->capacity * sizeof(VkrHashEntry_##name));  \
    }                                                                          \
    table->size = 0;                                                           \
  }                                                                            \
  vkr_internal INLINE bool32_t vkr_hash_table_insert_##name(                   \
      VkrHashTable_##name *table, const char *key, type value) {               \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    if (table->size >=                                                         \
        (uint64_t)(table->capacity * VKR_HASH_TABLE_LOAD_FACTOR)) {            \
      uint64_t new_capacity = table->capacity > 0                              \
                                  ? table->capacity * 2                        \
                                  : VKR_HASH_TABLE_INITIAL_CAPACITY;           \
      vkr_hash_table_resize_##name(table, new_capacity);                       \
    }                                                                          \
    uint64_t hash = vkr_hash_name_##name(key, table->capacity);                \
    uint64_t index = hash;                                                     \
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
        log_debug("Hash table probe limit exceeded");                          \
        return false_v;                                                        \
      }                                                                        \
    }                                                                          \
    if (first_tombstone != UINT64_MAX) {                                       \
      index = first_tombstone;                                                 \
    }                                                                          \
    table->entries[index].key = key;                                           \
    table->entries[index].value = value;                                       \
    table->entries[index].occupied = VKR_OCCUPIED;                             \
    table->size++;                                                             \
    return true_v;                                                             \
  }                                                                            \
  vkr_internal INLINE bool8_t vkr_hash_table_remove_##name(                    \
      VkrHashTable_##name *table, const char *key) {                           \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    uint64_t hash = vkr_hash_name_##name(key, table->capacity);                \
    uint64_t index = hash;                                                     \
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
  vkr_internal INLINE type *vkr_hash_table_get_##name(                         \
      const VkrHashTable_##name *table, const char *key) {                     \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    uint64_t hash = vkr_hash_name_##name(key, table->capacity);                \
    uint64_t index = hash;                                                     \
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
  vkr_internal INLINE bool8_t vkr_hash_table_contains_##name(                  \
      const VkrHashTable_##name *table, const char *key) {                     \
    return vkr_hash_table_get_##name(table, key) != NULL ? true_v : false_v;   \
  }                                                                            \
  vkr_internal INLINE bool8_t vkr_hash_table_is_empty_##name(                  \
      const VkrHashTable_##name *table) {                                      \
    assert_log(table != NULL, "Table must not be NULL");                       \
    return table->size == 0 ? true_v : false_v;                                \
  }

#define VkrHashTable(type) VkrHashTableConstructor(type, type)

VkrHashTable(uint8_t);