#pragma once

#include "arena.h"

#define ArrayConstructor(type, name)                                           \
  struct Array_##name;                                                         \
  typedef struct Array_##name {                                                \
    Arena *arena;                                                              \
    size_t stride;                                                             \
    size_t length;                                                             \
    type *data;                                                                \
  } Array_##name;                                                              \
                                                                               \
  Array_##name array_create_##name(Arena *arena, const size_t length) {        \
    assert(arena != NULL);                                                     \
    assert(length > 0);                                                        \
                                                                               \
    Array_##name array = {arena, sizeof(type), length,                         \
                          arena_alloc(arena, length * sizeof(type))};          \
    return array;                                                              \
  }                                                                            \
                                                                               \
  type *array_get_##name(const Array_##name *array, const size_t index) {      \
    assert(array != NULL);                                                     \
    assert(index < array->length);                                             \
    return (type *)(array->data + index);                                      \
  }                                                                            \
                                                                               \
  void array_set_##name(Array_##name *array, const size_t index, type value) { \
    assert(array != NULL);                                                     \
    assert(index < array->length);                                             \
    array->data[index] = value;                                                \
  }                                                                            \
  void array_destroy_##name(Array_##name *array) {                             \
    assert(array != NULL);                                                     \
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
