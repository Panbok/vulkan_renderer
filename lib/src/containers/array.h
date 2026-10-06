/**
 * @file array.h
 * @brief Growable typed array
 *
 * Array(type) declares Array_type and its functions. A record holds an
 * allocator, a capacity, a length and contiguous storage aligned for the
 * element type.
 *
 * - array_create_TYPE(allocator, capacity) reserves capacity with length 0.
 * - array_create_filled_TYPE(allocator, length) allocates length zeroed
 *   elements and sets the length.
 * - A record set only with .allocator is a valid empty array; its first
 *   reserve or push allocates.
 *
 * Constructors return an all-zero record when the size overflows or the
 * allocation fails. Capacity 0 returns an empty record that keeps the
 * allocator. Growth preserves data, length and capacity on failure.
 *
 * A successful push, reserve or grow can move the storage and invalidates
 * borrowed element pointers. An owner that hands out element pointers, such
 * as a fixed slot table, allocates once and never grows the array. The
 * allocator owns the storage until destroy.
 */

#pragma once

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

#define VKR_ARRAY_GROWTH_FACTOR 2
#define VKR_ARRAY_FIRST_GROWTH_CAPACITY 16

typedef struct ArrayFindResult {
  uint64_t index;
  bool32_t found;
} ArrayFindResult;

#define ArrayConstructor(type, name)                                           \
  typedef struct Array_##name {                                                \
    VkrAllocator *allocator;                                                   \
    uint64_t capacity;                                                         \
    uint64_t length;                                                           \
    type *data;                                                                \
  } Array_##name;                                                              \
                                                                               \
  static inline VKR_MAYBE_UNUSED VKR_MUST_USE                                  \
      Array_##name array_create_##name(VkrAllocator *allocator,                \
                                       uint64_t capacity) {                    \
    assert_log(allocator != NULL, "Allocator is NULL");                        \
    if (capacity > SIZE_MAX / sizeof(type)) {                                  \
      return (Array_##name){0};                                                \
    }                                                                          \
    if (capacity == 0) {                                                       \
      return (Array_##name){.allocator = allocator};                           \
    }                                                                          \
    type *data = vkr_allocator_alloc_aligned(                                  \
        allocator, capacity * sizeof(type), AlignOf(type),                     \
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);                                       \
    if (!data) {                                                               \
      return (Array_##name){0};                                                \
    }                                                                          \
    return (Array_##name){                                                     \
        .allocator = allocator, .capacity = capacity, .data = data};           \
  }                                                                            \
                                                                               \
  static inline VKR_MAYBE_UNUSED VKR_MUST_USE                                  \
      Array_##name array_create_filled_##name(VkrAllocator *allocator,         \
                                              uint64_t length) {               \
    Array_##name array = array_create_##name(allocator, length);               \
    if (array.data) {                                                          \
      MemZero(array.data, length * sizeof(type));                              \
      array.length = length;                                                   \
    }                                                                          \
    return array;                                                              \
  }                                                                            \
                                                                               \
  static inline VKR_MAYBE_UNUSED VKR_MUST_USE bool8_t array_reserve_##name(    \
      Array_##name *array, uint64_t capacity) {                                \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(array->allocator != NULL, "Allocator is NULL");                 \
    if (capacity <= array->capacity) {                                         \
      return true_v;                                                           \
    }                                                                          \
    if (capacity > SIZE_MAX / sizeof(type)) {                                  \
      return false_v;                                                          \
    }                                                                          \
    type *data = vkr_allocator_alloc_aligned(                                  \
        array->allocator, capacity * sizeof(type), AlignOf(type),              \
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);                                       \
    if (!data) {                                                               \
      return false_v;                                                          \
    }                                                                          \
    /* Only live elements move; an arena keeps the old block, so copying       \
     * the unused capacity would only cost time. */                            \
    if (array->data) {                                                         \
      MemCopy(data, array->data, array->length * sizeof(type));                \
      vkr_allocator_free_aligned(                                              \
          array->allocator, array->data, array->capacity * sizeof(type),       \
          AlignOf(type), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);                      \
    }                                                                          \
    array->data = data;                                                        \
    array->capacity = capacity;                                                \
    return true_v;                                                             \
  }                                                                            \
                                                                               \
  /* Grows by VKR_ARRAY_GROWTH_FACTOR, or to VKR_ARRAY_FIRST_GROWTH_CAPACITY   \
   * from an empty record. */                                                  \
  static inline VKR_MAYBE_UNUSED VKR_MUST_USE bool8_t array_grow_##name(       \
      Array_##name *array) {                                                   \
    assert_log(array != NULL, "Array is NULL");                                \
    const uint64_t max_capacity = SIZE_MAX / sizeof(type);                     \
    if (array->capacity == max_capacity) {                                     \
      return false_v;                                                          \
    }                                                                          \
    uint64_t capacity =                                                        \
        array->capacity > max_capacity / VKR_ARRAY_GROWTH_FACTOR               \
            ? max_capacity                                                     \
            : array->capacity * VKR_ARRAY_GROWTH_FACTOR;                       \
    if (capacity == 0) {                                                       \
      capacity = VKR_ARRAY_FIRST_GROWTH_CAPACITY;                              \
    }                                                                          \
    return array_reserve_##name(array, capacity);                              \
  }                                                                            \
                                                                               \
  static inline VKR_MAYBE_UNUSED VKR_MUST_USE bool8_t array_push_##name(       \
      Array_##name *array, type value) {                                       \
    assert_log(array != NULL, "Array is NULL");                                \
    if (array->length == array->capacity && !array_grow_##name(array)) {       \
      return false_v;                                                          \
    }                                                                          \
    assert_log(array->data != NULL, "Array storage is NULL");                  \
    array->data[array->length++] = value;                                      \
    return true_v;                                                             \
  }                                                                            \
                                                                               \
  static inline VKR_MAYBE_UNUSED type array_pop_##name(Array_##name *array) {  \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(array->length > 0, "Array is empty");                           \
    assert_log(array->data != NULL, "Array storage is NULL");                  \
    return array->data[--array->length];                                       \
  }                                                                            \
                                                                               \
  /* Removes the element at `index`, keeps the order of the rest and copies    \
   * the removed element to `dest` when it is not NULL. */                     \
  static inline VKR_MAYBE_UNUSED type *array_pop_at_##name(                    \
      Array_##name *array, uint64_t index, type *dest) {                       \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(index < array->length, "Index is out of bounds");               \
    assert_log(array->data != NULL, "Array storage is NULL");                  \
    if (dest != NULL) {                                                        \
      MemCopy(dest, array->data + index, sizeof(type));                        \
    }                                                                          \
    const uint64_t elements_to_move = array->length - 1 - index;               \
    if (elements_to_move > 0) {                                                \
      MemCopy(array->data + index, array->data + index + 1,                    \
              elements_to_move * sizeof(type));                                \
    }                                                                          \
    array->length--;                                                           \
    return dest;                                                               \
  }                                                                            \
                                                                               \
  typedef bool8_t (*ArrayFindCallback_##name)(type * current_value,            \
                                              type * value);                   \
                                                                               \
  static inline VKR_MAYBE_UNUSED ArrayFindResult array_find_##name(            \
      const Array_##name *array, type *value,                                  \
      ArrayFindCallback_##name callback) {                                     \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(callback != NULL, "Callback is NULL");                          \
    for (uint64_t i = 0; i < array->length; i++) {                             \
      if (callback(&array->data[i], value)) {                                  \
        return (ArrayFindResult){i, true_v};                                   \
      }                                                                        \
    }                                                                          \
    return (ArrayFindResult){0, false_v};                                      \
  }                                                                            \
                                                                               \
  static inline VKR_MAYBE_UNUSED void array_clear_##name(                      \
      Array_##name *array) {                                                   \
    assert_log(array != NULL, "Array is NULL");                                \
    array->length = 0;                                                         \
  }                                                                            \
                                                                               \
  static inline VKR_MAYBE_UNUSED void array_set_##name(                        \
      Array_##name *array, uint64_t index, type value) {                       \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(index < array->length, "Index is out of bounds");               \
    assert_log(array->data != NULL, "Array storage is NULL");                  \
    array->data[index] = value;                                                \
  }                                                                            \
                                                                               \
  static inline VKR_MAYBE_UNUSED type *array_get_##name(                       \
      const Array_##name *array, uint64_t index) {                             \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(index < array->length, "Index is out of bounds");               \
    assert_log(array->data != NULL, "Array storage is NULL");                  \
    return array->data + index;                                                \
  }                                                                            \
                                                                               \
  /* Frees the storage and resets the record to all zeros. */                  \
  static inline VKR_MAYBE_UNUSED void array_destroy_##name(                    \
      Array_##name *array) {                                                   \
    assert_log(array != NULL, "Array is NULL");                                \
    if (array->allocator && array->data) {                                     \
      vkr_allocator_free_aligned(                                              \
          array->allocator, array->data, array->capacity * sizeof(type),       \
          AlignOf(type), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);                      \
    }                                                                          \
    *array = (Array_##name){0};                                                \
  }                                                                            \
                                                                               \
  /* True when the record has no storage: never allocated, empty or            \
   * destroyed. */                                                             \
  static inline VKR_MAYBE_UNUSED bool32_t array_is_null_##name(                \
      const Array_##name *array) {                                             \
    assert_log(array != NULL, "Array is NULL");                                \
    return array->data == NULL;                                                \
  }                                                                            \
                                                                               \
  static inline VKR_MAYBE_UNUSED bool32_t array_is_empty_##name(               \
      const Array_##name *array) {                                             \
    assert_log(array != NULL, "Array is NULL");                                \
    return array->length == 0;                                                 \
  }

#define Array(type) ArrayConstructor(type, type)

Array(uint8_t);
Array(uint16_t);
Array(uint32_t);
Array(uint64_t);
Array(float32_t);
Array(float64_t);
Array(String8);
Array(bool8_t);
