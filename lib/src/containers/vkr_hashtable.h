#pragma once

#include "containers/str.h"
#include "core/logger.h"
#include "memory/arena.h"

// TODO: Use open addressing for collision resolution in the future
// version of the hash table

#define VKR_HASH_TABLE_MULTIPLIER 97

#define VkrHashTableConstructor(type, name)                                    \
  struct VkrHashTable_##name;                                                  \
  typedef struct VkrHashTable_##name {                                         \
    Arena *arena;                                                              \
    uint64_t capacity;                                                         \
    uint64_t size;                                                             \
    type *data;                                                                \
    uint8_t *occupied;                                                         \
    const char **keys;                                                         \
  } VkrHashTable_##name;                                                       \
  static inline uint64_t vkr_hash_name_##name(                                 \
      const VkrHashTable_##name *table, const char *name) {                    \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(name != NULL, "Name must not be NULL");                         \
    assert_log(table->capacity > 0, "Table capacity must be greater than 0");  \
    unsigned const char *us;                                                   \
    uint64_t hash = 0;                                                         \
    for (us = (unsigned const char *)name; *us; us++) {                        \
      hash = hash * VKR_HASH_TABLE_MULTIPLIER + *us;                           \
    }                                                                          \
    hash %= table->capacity;                                                   \
    return hash;                                                               \
  }                                                                            \
  static inline VkrHashTable_##name vkr_hash_table_create_##name(              \
      Arena *arena, uint64_t capacity) {                                       \
    assert_log(arena != NULL, "Arena must not be NULL");                       \
    assert_log(capacity > 0, "Capacity must be greater than 0");               \
    VkrHashTable_##name table = (VkrHashTable_##name){0};                      \
    table.arena = arena;                                                       \
    table.capacity = capacity;                                                 \
    table.size = 0;                                                            \
    table.data = (type *)arena_alloc(arena, capacity * sizeof(type),           \
                                     ARENA_MEMORY_TAG_HASH_TABLE);             \
    table.occupied = (uint8_t *)arena_alloc(arena, capacity * sizeof(uint8_t), \
                                            ARENA_MEMORY_TAG_HASH_TABLE);      \
    table.keys = (const char **)arena_alloc(                                   \
        arena, capacity * sizeof(const char *), ARENA_MEMORY_TAG_HASH_TABLE);  \
    assert_log(table.data != NULL && table.occupied != NULL &&                 \
                   table.keys != NULL,                                         \
               "arena_alloc failed for hash table create");                    \
    MemZero(table.occupied, capacity * sizeof(uint8_t));                       \
    MemZero((void *)table.keys, capacity * sizeof(const char *));              \
    return table;                                                              \
  }                                                                            \
  static inline void vkr_hash_table_reset_##name(VkrHashTable_##name *table) { \
    assert_log(table != NULL, "Table must not be NULL");                       \
    table->size = 0;                                                           \
    if (table->occupied) {                                                     \
      MemZero(table->occupied, table->capacity * sizeof(uint8_t));             \
    }                                                                          \
    if (table->keys) {                                                         \
      MemZero((void *)table->keys, table->capacity * sizeof(const char *));    \
    }                                                                          \
  }                                                                            \
  static inline bool8_t vkr_hash_table_insert_##name(                          \
      VkrHashTable_##name *table, const char *key, type value) {               \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    uint64_t hash = vkr_hash_name_##name(table, key);                          \
    if (table->occupied[hash]) {                                               \
      if (table->keys[hash] && string_equals(table->keys[hash], key)) {        \
        table->data[hash] = value;                                             \
        return true;                                                           \
      }                                                                        \
      return false;                                                            \
    }                                                                          \
    table->data[hash] = value;                                                 \
    table->occupied[hash] = 1;                                                 \
    table->keys[hash] = key;                                                   \
    table->size++;                                                             \
    return true;                                                               \
  }                                                                            \
  static inline bool8_t vkr_hash_table_remove_##name(                          \
      VkrHashTable_##name *table, const char *key) {                           \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    uint64_t hash = vkr_hash_name_##name(table, key);                          \
    if (!table->occupied[hash]) {                                              \
      return false;                                                            \
    }                                                                          \
    if (!(table->keys[hash] && string_equals(table->keys[hash], key))) {       \
      return false;                                                            \
    }                                                                          \
    table->occupied[hash] = 0;                                                 \
    table->keys[hash] = NULL;                                                  \
    table->size--;                                                             \
    return true;                                                               \
  }                                                                            \
  static inline type *vkr_hash_table_get_##name(                               \
      const VkrHashTable_##name *table, const char *key) {                     \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    uint64_t hash = vkr_hash_name_##name(table, key);                          \
    if (!table->occupied[hash]) {                                              \
      return NULL;                                                             \
    }                                                                          \
    if (!(table->keys[hash] && string_equals(table->keys[hash], key))) {       \
      return NULL;                                                             \
    }                                                                          \
    return (type *)(table->data + hash);                                       \
  }                                                                            \
  static inline bool8_t vkr_hash_table_contains_##name(                        \
      const VkrHashTable_##name *table, const char *key) {                     \
    assert_log(table != NULL, "Table must not be NULL");                       \
    assert_log(key != NULL, "Key must not be NULL");                           \
    uint64_t hash = vkr_hash_name_##name(table, key);                          \
    if (!table->occupied[hash]) {                                              \
      return false_v;                                                          \
    }                                                                          \
    return (table->keys[hash] && string_equals(table->keys[hash], key))        \
               ? true_v                                                        \
               : false_v;                                                      \
  }                                                                            \
  static inline bool8_t vkr_hash_table_is_empty_##name(                        \
      const VkrHashTable_##name *table) {                                      \
    assert_log(table != NULL, "Table must not be NULL");                       \
    return table->size == 0;                                                   \
  }

#define VkrHashTable(type) VkrHashTableConstructor(type, type)

VkrHashTable(uint8_t);