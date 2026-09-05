#pragma once

#include "containers/str.h"
#include "core/logger.h"
#include "memory/vkr_allocator.h"

#define DEFAULT_VECTOR_CAPACITY 16
#define DEFAULT_VECTOR_RESIZE_FACTOR 2

// Constructors return an all-zero record on failure. An empty lazy vector is
// initialized explicitly with .allocator; its first reserve or push allocates.
// Growth preserves data, length and capacity on failure. Successful growth may
// invalidate borrowed pointers. The allocator owns storage until destroy.
typedef struct VectorFindResult {
  uint64_t index;
  bool32_t found;
} VectorFindResult;

#define VectorConstructor(type, name)                                          \
  typedef struct Vector_##name {                                               \
    VkrAllocator *allocator;                                                   \
    uint64_t capacity;                                                         \
    uint64_t length;                                                           \
    type *data;                                                                \
  } Vector_##name;                                                             \
  static inline VKR_MUST_USE Vector_##name                                     \
      vector_create_##name##_with_capacity(VkrAllocator *allocator,            \
                                           uint64_t capacity) {                \
    assert_log(allocator != NULL, "Allocator is NULL");                        \
    if (capacity == 0 || capacity > SIZE_MAX / sizeof(type)) {                 \
      return (Vector_##name){0};                                               \
    }                                                                          \
    type *data = vkr_allocator_alloc(allocator, capacity * sizeof(type),       \
                                     VKR_ALLOCATOR_MEMORY_TAG_VECTOR);         \
    if (!data) {                                                               \
      return (Vector_##name){0};                                               \
    }                                                                          \
    return (Vector_##name){allocator, capacity, 0, data};                      \
  }                                                                            \
  static inline VKR_MUST_USE Vector_##name vector_create_##name(               \
      VkrAllocator *allocator) {                                               \
    return vector_create_##name##_with_capacity(allocator,                     \
                                                DEFAULT_VECTOR_CAPACITY);      \
  }                                                                            \
  static inline VKR_MUST_USE bool8_t vector_reserve_##name(                    \
      Vector_##name *vector, uint64_t capacity) {                              \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->allocator != NULL, "Allocator is NULL");                \
    if (capacity <= vector->capacity) {                                        \
      return true;                                                             \
    }                                                                          \
    if (capacity > SIZE_MAX / sizeof(type)) {                                  \
      return false;                                                            \
    }                                                                          \
    type *data =                                                               \
        vector->data                                                           \
            ? vkr_allocator_realloc(vector->allocator, vector->data,           \
                                    vector->capacity * sizeof(type),           \
                                    capacity * sizeof(type),                   \
                                    VKR_ALLOCATOR_MEMORY_TAG_VECTOR)           \
            : vkr_allocator_alloc(vector->allocator, capacity * sizeof(type),  \
                                  VKR_ALLOCATOR_MEMORY_TAG_VECTOR);            \
    if (!data) {                                                               \
      return false;                                                            \
    }                                                                          \
    vector->data = data;                                                       \
    vector->capacity = capacity;                                               \
    return true;                                                               \
  }                                                                            \
  static inline VKR_MUST_USE bool8_t vector_resize_##name(                     \
      Vector_##name *vector) {                                                 \
    const uint64_t max_capacity = SIZE_MAX / sizeof(type);                     \
    if (vector->capacity == max_capacity) {                                    \
      return false;                                                            \
    }                                                                          \
    uint64_t capacity =                                                        \
        vector->capacity > max_capacity / DEFAULT_VECTOR_RESIZE_FACTOR         \
            ? max_capacity                                                     \
            : vector->capacity * DEFAULT_VECTOR_RESIZE_FACTOR;                 \
    if (capacity == 0) {                                                       \
      capacity = DEFAULT_VECTOR_CAPACITY;                                      \
    }                                                                          \
    return vector_reserve_##name(vector, capacity);                            \
  }                                                                            \
  static inline VKR_MUST_USE bool8_t vector_push_##name(Vector_##name *vector, \
                                                        type value) {          \
    if (vector->length == vector->capacity && !vector_resize_##name(vector)) { \
      return false;                                                            \
    }                                                                          \
    vector->data[vector->length++] = value;                                    \
    return true;                                                               \
  }                                                                            \
  static inline type vector_pop_##name(Vector_##name *vector) {                \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->allocator != NULL, "Allocator is NULL");                \
    assert_log(vector->length > 0, "Vector is empty");                         \
    return (vector->data[--vector->length]);                                   \
  }                                                                            \
  static inline type *vector_pop_at_##name(Vector_##name *vector,              \
                                           uint64_t index, type *dest) {       \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->allocator != NULL, "Allocator is NULL");                \
    assert_log(index < vector->length, "Index is out of bounds");              \
    uint64_t length = vector->length;                                          \
    uint64_t stride = sizeof(type);                                            \
    if (dest != NULL) {                                                        \
      MemCopy(dest, vector->data + index, stride);                             \
    }                                                                          \
    if (index != length - 1) {                                                 \
      uint64_t elements_to_move = length - 1 - index;                          \
      MemCopy(vector->data + index, vector->data + index + 1,                  \
              elements_to_move * stride);                                      \
    }                                                                          \
    vector->length--;                                                          \
    return dest;                                                               \
  }                                                                            \
  typedef bool8_t (*VectorFindCallback_##name)(type * current_value,           \
                                               type * value);                  \
  static inline VectorFindResult vector_find_##name(                           \
      const Vector_##name *vector, type *value,                                \
      VectorFindCallback_##name callback) {                                    \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->allocator != NULL, "Allocator is NULL");                \
    assert_log(callback != NULL, "Callback is NULL");                          \
    for (uint64_t i = 0; i < vector->length; i++) {                            \
      if (callback(&vector->data[i], value)) {                                 \
        return (VectorFindResult){i, true};                                    \
      }                                                                        \
    }                                                                          \
    return (VectorFindResult){0, false};                                       \
  }                                                                            \
  static inline void vector_clear_##name(Vector_##name *vector) {              \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->allocator != NULL, "Allocator is NULL");                \
    vector->length = 0;                                                        \
  }                                                                            \
  static inline void vector_set_##name(Vector_##name *vector, uint64_t index,  \
                                       type value) {                           \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->allocator != NULL, "Allocator is NULL");                \
    assert_log(index < vector->length, "Index is out of bounds");              \
    vector->data[index] = value;                                               \
  }                                                                            \
  static inline type *vector_get_##name(const Vector_##name *vector,           \
                                        uint64_t index) {                      \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->allocator != NULL, "Allocator is NULL");                \
    assert_log(index < vector->length, "Index is out of bounds");              \
    return (type *)(vector->data + index);                                     \
  }                                                                            \
  static inline void vector_destroy_##name(Vector_##name *vector) {            \
    assert_log(vector != NULL, "Vector is NULL");                              \
    if (vector->data) {                                                        \
      vkr_allocator_free(vector->allocator, vector->data,                      \
                         vector->capacity * sizeof(type),                      \
                         VKR_ALLOCATOR_MEMORY_TAG_VECTOR);                     \
    }                                                                          \
    vector->data = NULL;                                                       \
    vector->allocator = NULL;                                                  \
    vector->capacity = 0;                                                      \
    vector->length = 0;                                                        \
  }

#define Vector(type) VectorConstructor(type, type)

Vector(uint8_t);
Vector(uint32_t);
Vector(uint64_t);
Vector(float32_t);
Vector(float64_t);
Vector(bool8_t);
Vector(String8);
