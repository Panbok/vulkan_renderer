#pragma once

#include "arena.h"
#include "logger.h"

#define ArrayConstructor(type, name)                                           \
  struct Array_##name;                                                         \
  typedef struct Array_##name {                                                \
    Arena *arena;                                                              \
    size_t stride;                                                             \
    size_t length;                                                             \
    type *data;                                                                \
  } Array_##name;                                                              \
                                                                               \
  static inline Array_##name array_create_##name(Arena *arena,                 \
                                                 const size_t length) {        \
    assert_log(arena != NULL, "Arena is NULL");                                \
    assert_log(length > 0, "Length is 0");                                     \
                                                                               \
    Array_##name array = {arena, sizeof(type), length,                         \
                          arena_alloc(arena, length * sizeof(type))};          \
    return array;                                                              \
  }                                                                            \
                                                                               \
  static inline type *array_get_##name(const Array_##name *array,              \
                                       const size_t index) {                   \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(index < array->length, "Index is out of bounds");               \
    return (type *)(array->data + index);                                      \
  }                                                                            \
                                                                               \
  static inline void array_set_##name(Array_##name *array, const size_t index, \
                                      type value) {                            \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(index < array->length, "Index is out of bounds");               \
    array->data[index] = value;                                                \
  }                                                                            \
  static inline void array_destroy_##name(Array_##name *array) {               \
    assert_log(array != NULL, "Array is NULL");                                \
    array->data = NULL;                                                        \
    array->arena = NULL;                                                       \
    array->length = 0;                                                         \
    array->stride = 0;                                                         \
  }

#define Array(type) ArrayConstructor(type, type)

Array(uint8_t);

Array(uint32_t);

Array(uint64_t);

Array(size_t);
