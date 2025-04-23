#pragma once

#include "arena.h"

#define DEFAULT_VECTOR_CAPACITY 16
#define DEFAULT_VECTOR_RESIZE_FACTOR 2

#define VectorConstructor(type, name)                                          \
  struct Vector_##name;                                                        \
  typedef struct Vector_##name {                                               \
    Arena *arena;                                                              \
    size_t capacity;                                                           \
    size_t length;                                                             \
    size_t stride;                                                             \
    type *data;                                                                \
  } Vector_##name;                                                             \
  Vector_##name vector_create_##name(Arena *arena) {                           \
    assert(arena != NULL);                                                     \
                                                                               \
    Vector_##name vector = {                                                   \
        arena, DEFAULT_VECTOR_CAPACITY, 0, sizeof(type),                       \
        arena_alloc(arena, DEFAULT_VECTOR_CAPACITY * sizeof(type))};           \
    return vector;                                                             \
  }                                                                            \
  Vector_##name vector_create_##name##_with_capacity(Arena *arena,             \
                                                     size_t capacity) {        \
    assert(arena != NULL);                                                     \
    assert(capacity > 0);                                                      \
    Vector_##name vector = {arena, capacity, 0, sizeof(type),                  \
                            arena_alloc(arena, capacity * sizeof(type))};      \
    return vector;                                                             \
  }                                                                            \
  type *vector_resize_##name(Vector_##name *vector) {                          \
    assert(vector != NULL);                                                    \
    assert(vector->arena != NULL);                                             \
                                                                               \
    size_t target_capacity = vector->capacity * DEFAULT_VECTOR_RESIZE_FACTOR;  \
    size_t allocation_size = target_capacity * vector->stride;                 \
    type *new_data = arena_alloc(vector->arena, allocation_size);              \
    if (!new_data) {                                                           \
      return NULL;                                                             \
    }                                                                          \
                                                                               \
    if (vector->data != NULL && vector->length > 0) {                          \
      MemCopy(new_data, vector->data, vector->length * vector->stride);        \
    }                                                                          \
                                                                               \
    vector->data = new_data;                                                   \
    vector->capacity = target_capacity;                                        \
    return (type *)new_data;                                                   \
  }                                                                            \
  void vector_push_##name(Vector_##name *vector, type value) {                 \
    assert(vector != NULL);                                                    \
    assert(vector->arena != NULL);                                             \
    if (vector->length == vector->capacity) {                                  \
      vector_resize_##name(vector);                                            \
    }                                                                          \
    vector->data[vector->length] = value;                                      \
    vector->length++;                                                          \
  }                                                                            \
  type vector_pop_##name(Vector_##name *vector) {                              \
    assert(vector != NULL);                                                    \
    assert(vector->arena != NULL);                                             \
    assert(vector->length > 0);                                                \
    return (type)(vector->data[--vector->length]);                             \
  }                                                                            \
  void vector_clear_##name(Vector_##name *vector) {                            \
    assert(vector != NULL);                                                    \
    assert(vector->arena != NULL);                                             \
    vector->length = 0;                                                        \
  }                                                                            \
  void vector_set_##name(Vector_##name *vector, size_t index, type value) {    \
    assert(vector != NULL);                                                    \
    assert(vector->arena != NULL);                                             \
    assert(index < vector->length);                                            \
    vector->data[index] = value;                                               \
  }                                                                            \
  type *vector_get_##name(Vector_##name *vector, size_t index) {               \
    assert(vector != NULL);                                                    \
    assert(vector->arena != NULL);                                             \
    assert(index < vector->length);                                            \
    return (type *)(vector->data + index);                                     \
  }                                                                            \
  void vector_destroy_##name(Vector_##name *vector) {                          \
    assert(vector != NULL);                                                    \
    vector->data = NULL;                                                       \
    vector->arena = NULL;                                                      \
    vector->capacity = 0;                                                      \
    vector->length = 0;                                                        \
    vector->stride = 0;                                                        \
  }

#define Vector(type) VectorConstructor(type, type)

Vector(uint8_t);
Vector(uint32_t);
Vector(uint64_t);
Vector(size_t);
