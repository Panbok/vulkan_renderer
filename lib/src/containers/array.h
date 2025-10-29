/**
 * @file array.h
 * @brief Fixed-size array implementation
 *
 * This file provides a generic, type-safe fixed-size array implementation using
 * the C preprocessor. Arrays are allocated from an arena allocator and provide
 * efficient O(1) access operations with bounds checking.
 *
 * Memory Layout:
 * An array consists of a metadata structure and a contiguous block of elements:
 *
 * +---------------------+ <-- Array_TYPE structure
 * | Arena *arena        |     (Pointer to the arena allocator used for memory)
 * | uint64_t length       |     (Number of elements in the array)
 * | TYPE *data          | --> Points to contiguous memory block of length *
 * | sizeof(TYPE)        |     (Size of each element in the array)
 * +---------------------+
 *
 * This implementation is useful for scenarios where:
 * - The size of the array is known at creation time
 * - Random access to elements is required
 * - The array size doesn't need to change during its lifetime
 *
 * Each array operation includes bounds checking to ensure memory safety
 * and prevent buffer overflows.
 *
 * Usage Pattern:
 * 1. Create an array using array_create_TYPE()
 * 2. Set elements with array_set_TYPE()
 * 3. Get elements with array_get_TYPE()
 * 4. When done, optionally call array_destroy_TYPE() to clean up metadata
 */

#pragma once

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/arena.h"

#define ArrayConstructor(type, name)                                           \
  /**                                                                          \
   * @brief Array structure for storing fixed-size collections of elements of  \
   * type 'type' A fixed-size array of type 'name' with bounds checking        \
   */                                                                          \
  struct Array_##name;                                                         \
  typedef struct Array_##name {                                                \
    Arena *arena;    /**< Arena allocator used for memory allocation */        \
    uint64_t length; /**< Number of elements in the array */                   \
    type *data;      /**< Pointer to the contiguous array storage */           \
  } Array_##name;                                                              \
                                                                               \
  /**                                                                          \
   * @brief Creates a new array with the specified length                      \
   * @param arena Arena allocator to use for memory allocation                 \
   * @param length Number of elements to allocate in the array                 \
   * @return Initialized Array_##name structure                                \
   */                                                                          \
  static inline Array_##name array_create_##name(Arena *arena,                 \
                                                 const uint64_t length) {      \
    assert_log(arena != NULL, "Arena is NULL");                                \
    assert_log(length > 0, "Length is 0");                                     \
                                                                               \
    type *buf =                                                                \
        arena_alloc(arena, length * sizeof(type), ARENA_MEMORY_TAG_ARRAY);     \
    assert_log(buf != NULL, "arena_alloc failed for array_create");            \
    Array_##name array = {arena, buf ? length : 0, buf};                       \
    return array;                                                              \
  }                                                                            \
                                                                               \
  /**                                                                          \
   * @brief Retrieves a pointer to an element at the specified index           \
   * @param array Pointer to the array                                         \
   * @param index Zero-based index of the element to retrieve                  \
   * @return Pointer to the element at the specified index                     \
   * @note Asserts if index is out of bounds                                   \
   */                                                                          \
  static inline type *array_get_##name(const Array_##name *array,              \
                                       const uint64_t index) {                 \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(index < array->length, "Index is out of bounds");               \
    return (type *)(array->data + index);                                      \
  }                                                                            \
                                                                               \
  /**                                                                          \
   * @brief Sets the value of an element at the specified index                \
   * @param array Pointer to the array                                         \
   * @param index Zero-based index of the element to set                       \
   * @param value Value to assign to the element                               \
   * @note Asserts if index is out of bounds                                   \
   */                                                                          \
  static inline void array_set_##name(Array_##name *array,                     \
                                      const uint64_t index, type value) {      \
    assert_log(array != NULL, "Array is NULL");                                \
    assert_log(index < array->length, "Index is out of bounds");               \
    array->data[index] = value;                                                \
  }                                                                            \
  /**                                                                          \
   * @brief Marks the array as destroyed, sets all members to NULL/0           \
   * @param array Pointer to the array                                         \
   * @note This does not deallocate memory, as that's managed by the arena     \
   */                                                                          \
  static inline void array_destroy_##name(Array_##name *array) {               \
    assert_log(array != NULL, "Array is NULL");                                \
    array->data = NULL;                                                        \
    array->arena = NULL;                                                       \
    array->length = 0;                                                         \
  }                                                                            \
  /**                                                                          \
   * @brief Checks if the array was never initialized or has been destroyed    \
   * @param array Pointer to the array                                         \
   * @return True if the array data pointer is NULL, false otherwise           \
   */                                                                          \
  static inline bool32_t array_is_null_##name(const Array_##name *array) {     \
    assert_log(array != NULL, "Array is NULL");                                \
    return array->data == NULL;                                                \
  }                                                                            \
  /**                                                                          \
   * @brief Checks if the array is empty (has zero length)                     \
   * @param array Pointer to the array                                         \
   * @return True if the array has zero length, false otherwise                \
   */                                                                          \
  static inline bool32_t array_is_empty_##name(const Array_##name *array) {    \
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