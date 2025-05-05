/**
 * @file vector.h
 * @brief Dynamic resizable array implementation
 *
 * This file provides a generic, type-safe dynamic array implementation using
 * the C preprocessor. Vectors are similar to arrays but can grow in size
 * automatically as elements are added, providing more flexibility than
 * fixed-size arrays.
 *
 * Memory Layout:
 * A vector consists of a metadata structure and a contiguous block of elements:
 *
 * +---------------------+ <-- Vector_TYPE structure
 * | Arena *arena        |     (Pointer to the arena allocator used for memory)
 * | uint64_t capacity     |     (Current allocated capacity of the vector)
 * | uint64_t length       |     (Current number of elements in the vector)
 * | TYPE *data          | --> Points to contiguous memory block of capacity *
 * | sizeof(TYPE)        |     (Size of each element in the vector)
 * +---------------------+
 *
 * Resizing Mechanism:
 * When the vector needs to grow (length == capacity), a new larger block of
 * memory is allocated with capacity * DEFAULT_VECTOR_RESIZE_FACTOR, and
 * existing elements are copied to the new location. This amortizes the cost of
 * resizing across many operations, providing efficient O(1) amortized time
 * complexity for push operations.
 *
 * This implementation is useful for scenarios where:
 * - The final size of the array is not known in advance
 * - The collection needs to grow dynamically
 * - Efficient append operations are required
 * - Random access to elements is needed
 *
 * Each vector operation includes bounds checking to ensure memory safety
 * and prevent buffer overflows.
 *
 * Usage Pattern:
 * 1. Create a vector using vector_create_TYPE()
 * 2. Add elements with vector_push_TYPE()
 * 3. Access elements with vector_get_TYPE()
 * 4. Modify elements with vector_set_TYPE()
 * 5. Remove the last element with vector_pop_TYPE()
 * 6. When done, optionally call vector_destroy_TYPE() to clean up metadata
 */

#pragma once

#include "arena.h"
#include "logger.h"
#include "pch.h"

#define DEFAULT_VECTOR_CAPACITY 16
#define DEFAULT_VECTOR_RESIZE_FACTOR 2

typedef struct VectorFindResult {
  uint64_t index;
  bool32_t found;
} VectorFindResult;

#define VectorConstructor(type, name)                                          \
  /**                                                                          \
   * @brief Vector structure for storing dynamic collections of elements of    \
   * type 'type' A dynamically resizable array of type 'name' with automatic   \
   * growth                                                                    \
   */                                                                          \
  struct Vector_##name;                                                        \
  typedef struct Vector_##name {                                               \
    Arena *arena;      /**< Arena allocator used for memory allocation */      \
    uint64_t capacity; /**< Current allocated capacity of the vector */        \
    uint64_t length;   /**< Current number of elements in the vector */        \
    type *data;        /**< Pointer to the contiguous vector storage */        \
  } Vector_##name;                                                             \
  /**                                                                          \
   * @brief Creates a new vector with default capacity                         \
   * @param arena Arena allocator to use for memory allocation                 \
   * @return Initialized Vector_##name structure with DEFAULT_VECTOR_CAPACITY  \
   */                                                                          \
  static inline Vector_##name vector_create_##name(Arena *arena) {             \
    assert_log(arena != NULL, "Arena is NULL");                                \
                                                                               \
    Vector_##name vector = {                                                   \
        arena, DEFAULT_VECTOR_CAPACITY, 0,                                     \
        arena_alloc(arena, DEFAULT_VECTOR_CAPACITY * sizeof(type))};           \
    return vector;                                                             \
  }                                                                            \
  /**                                                                          \
   * @brief Creates a new vector with specified capacity                       \
   * @param arena Arena allocator to use for memory allocation                 \
   * @param capacity Initial capacity of the vector                            \
   * @return Initialized Vector_##name structure with the specified capacity   \
   */                                                                          \
  static inline Vector_##name vector_create_##name##_with_capacity(            \
      Arena *arena, uint64_t capacity) {                                       \
    assert_log(arena != NULL, "Arena is NULL");                                \
    assert_log(capacity > 0, "Capacity is 0");                                 \
    Vector_##name vector = {arena, capacity, 0,                                \
                            arena_alloc(arena, capacity * sizeof(type))};      \
    return vector;                                                             \
  }                                                                            \
  /**                                                                          \
   * @brief Resizes the vector to increase its capacity                        \
   * @param vector Pointer to the vector                                       \
   * @return Pointer to the new data buffer after resizing                     \
   * @note This is called automatically by vector_push when needed             \
   * @note Capacity grows by DEFAULT_VECTOR_RESIZE_FACTOR each time            \
   */                                                                          \
  static inline type *vector_resize_##name(Vector_##name *vector) {            \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->arena != NULL, "Arena is NULL");                        \
                                                                               \
    uint64_t target_capacity =                                                 \
        vector->capacity * DEFAULT_VECTOR_RESIZE_FACTOR;                       \
    uint64_t allocation_size = target_capacity * sizeof(type);                 \
    type *new_data = arena_alloc(vector->arena, allocation_size);              \
    assert_log(new_data != NULL, "Failed to allocate memory");                 \
                                                                               \
    if (vector->data != NULL && vector->length > 0) {                          \
      MemCopy(new_data, vector->data, vector->length * sizeof(type));          \
    }                                                                          \
                                                                               \
    vector->data = new_data;                                                   \
    vector->capacity = target_capacity;                                        \
    return (type *)new_data;                                                   \
  }                                                                            \
  /**                                                                          \
   * @brief Adds an element to the end of the vector                           \
   * @param vector Pointer to the vector                                       \
   * @param value Value to add to the vector                                   \
   * @note Automatically resizes the vector if needed                          \
   */                                                                          \
  static inline void vector_push_##name(Vector_##name *vector, type value) {   \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->arena != NULL, "Arena is NULL");                        \
    if (vector->length == vector->capacity) {                                  \
      vector_resize_##name(vector);                                            \
    }                                                                          \
    vector->data[vector->length] = value;                                      \
    vector->length++;                                                          \
  }                                                                            \
  /**                                                                          \
   * @brief Removes and returns the last element from the vector               \
   * @param vector Pointer to the vector                                       \
   * @return The removed element from the end of the vector                    \
   * @note Asserts if the vector is empty                                      \
   */                                                                          \
  static inline type vector_pop_##name(Vector_##name *vector) {                \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->arena != NULL, "Arena is NULL");                        \
    assert_log(vector->length > 0, "Vector is empty");                         \
    return (type)(vector->data[--vector->length]);                             \
  }                                                                            \
  /**                                                                          \
   * @brief Removes and returns the element at the specified index             \
   * @param vector Pointer to the vector                                       \
   * @param index Zero-based index of the element to remove                    \
   * @param dest Pointer to a variable to store the removed element (optional) \
   * @return The removed element from the specified index                      \
   * @note Asserts if index is out of bounds                                   \
   */                                                                          \
  static inline type *vector_pop_at_##name(Vector_##name *vector,              \
                                           uint64_t index, type *dest) {       \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->arena != NULL, "Arena is NULL");                        \
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
  /**                                                                          \
   * @brief Finds the index of the first occurrence of a value in the vector   \
   * @param vector Pointer to the vector                                       \
   * @param value Value to find in the vector                                  \
   * @return Index of the first occurrence of the value, or UINT64_MAX if not  \
   * found                                                                     \
   */                                                                          \
  static inline VectorFindResult vector_find_##name(                           \
      const Vector_##name *vector, type *value) {                              \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->arena != NULL, "Arena is NULL");                        \
    for (uint64_t i = 0; i < vector->length; i++) {                            \
      if (vector->data[i] == *value) {                                         \
        return (VectorFindResult){i, true};                                    \
      }                                                                        \
    }                                                                          \
    return (VectorFindResult){0, false};                                       \
  }                                                                            \
  /**                                                                          \
   * @brief Removes all elements from the vector                               \
   * @param vector Pointer to the vector                                       \
   * @note This does not deallocate memory or change capacity, only resets     \
   * length                                                                    \
   */                                                                          \
  static inline void vector_clear_##name(Vector_##name *vector) {              \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->arena != NULL, "Arena is NULL");                        \
    vector->length = 0;                                                        \
  }                                                                            \
  /**                                                                          \
   * @brief Sets the value of an element at the specified index                \
   * @param vector Pointer to the vector                                       \
   * @param index Zero-based index of the element to set                       \
   * @param value Value to assign to the element                               \
   * @note Asserts if index is out of bounds                                   \
   */                                                                          \
  static inline void vector_set_##name(Vector_##name *vector, uint64_t index,  \
                                       type value) {                           \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->arena != NULL, "Arena is NULL");                        \
    assert_log(index < vector->length, "Index is out of bounds");              \
    vector->data[index] = value;                                               \
  }                                                                            \
  /**                                                                          \
   * @brief Retrieves a pointer to an element at the specified index           \
   * @param vector Pointer to the vector                                       \
   * @param index Zero-based index of the element to retrieve                  \
   * @return Pointer to the element at the specified index                     \
   * @note Asserts if index is out of bounds                                   \
   */                                                                          \
  static inline type *vector_get_##name(const Vector_##name *vector,           \
                                        uint64_t index) {                      \
    assert_log(vector != NULL, "Vector is NULL");                              \
    assert_log(vector->arena != NULL, "Arena is NULL");                        \
    assert_log(index < vector->length, "Index is out of bounds");              \
    return (type *)(vector->data + index);                                     \
  }                                                                            \
  /**                                                                          \
   * @brief Marks the vector as destroyed, sets all members to NULL/0          \
   * @param vector Pointer to the vector                                       \
   * @note This does not deallocate memory, as that's managed by the arena     \
   */                                                                          \
  static inline void vector_destroy_##name(Vector_##name *vector) {            \
    assert_log(vector != NULL, "Vector is NULL");                              \
    vector->data = NULL;                                                       \
    vector->arena = NULL;                                                      \
    vector->capacity = 0;                                                      \
    vector->length = 0;                                                        \
  }

#define Vector(type) VectorConstructor(type, type)

Vector(uint8_t);
Vector(uint32_t);
Vector(uint64_t);
Vector(float32_t);
Vector(float64_t);
