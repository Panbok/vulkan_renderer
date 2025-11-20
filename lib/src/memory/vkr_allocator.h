/**
 * @file vkr_allocator.h
 * @brief Defines the abstract memory allocator system interface, that can be
 * used to allocate memory for the application.
 */

// todo: in the future we need to clean up arena memory from tags, formating,
// etc. since all of this will be handled by the allocator system

#include "defines.h"

#pragma once

typedef enum VkrAllocatorMemoryTag {
  VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN,
  VKR_ALLOCATOR_MEMORY_TAG_ARRAY,
  VKR_ALLOCATOR_MEMORY_TAG_STRING,
  VKR_ALLOCATOR_MEMORY_TAG_VECTOR,
  VKR_ALLOCATOR_MEMORY_TAG_QUEUE,
  VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
  VKR_ALLOCATOR_MEMORY_TAG_BUFFER,
  VKR_ALLOCATOR_MEMORY_TAG_RENDERER,
  VKR_ALLOCATOR_MEMORY_TAG_FILE,
  VKR_ALLOCATOR_MEMORY_TAG_TEXTURE,
  VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE,
  VKR_ALLOCATOR_MEMORY_TAG_FREELIST,

  VKR_ALLOCATOR_MEMORY_TAG_MAX,
} VkrAllocatorMemoryTag;

typedef enum VkrAllocatorType {
  VKR_ALLOCATOR_TYPE_ARENA,
  VKR_ALLOCATOR_TYPE_MMEMORY,
  VKR_ALLOCATOR_TYPE_DMEMORY,
  VKR_ALLOCATOR_TYPE_UNKNOWN,

  VKR_ALLOCATOR_TYPE_MAX,
} VkrAllocatorType;

/**
 * @brief Statistics for the allocator.
 */
typedef struct VkrAllocatorStatistics {
  uint64_t total_allocs;
  uint64_t total_frees;
  uint64_t total_reallocs;
  uint64_t total_zeros;
  uint64_t total_copies;
  uint64_t total_sets;

  uint64_t total_allocated;
  uint64_t tagged_allocs[VKR_ALLOCATOR_MEMORY_TAG_MAX];
} VkrAllocatorStatistics;

/**
 * @brief Abstract interface that every allocator must implement.
 */
typedef struct VkrAllocator {
  VkrAllocatorType type;
  VkrAllocatorStatistics stats;
  void *ctx; // allocator-specific state, e.g., Arena*

  // Allocate size bytes. Alignment can be handled inside if you prefer.
  void *(*alloc)(void *ctx, uint64_t size, VkrAllocatorMemoryTag tag);

  // Free with known old_size. For arenas, this can be a no-op.
  void (*free)(void *ctx, void *ptr, uint64_t old_size,
               VkrAllocatorMemoryTag tag);

  // Reallocate: returns new pointer. For arenas: alloc+copy+leave old as-is.
  void *(*realloc)(void *ctx, void *ptr, uint64_t old_size, uint64_t new_size,
                   VkrAllocatorMemoryTag tag);
} VkrAllocator;

/**
 * @brief Allocates memory from the allocator.
 * @param allocator The allocator to use.
 * @param size The size of the memory to allocate.
 * @param tag The tag to associate with the allocation.
 * @param alloc_line The line number of the allocation.
 * @param alloc_file The file name of the allocation.
 * @return Pointer to the allocated memory, or NULL on failure.
 */
void *_vkr_allocator_alloc(VkrAllocator *allocator, uint64_t size,
                           VkrAllocatorMemoryTag tag, uint32_t alloc_line,
                           const char *alloc_file);

#define vkr_allocator_alloc(allocator, size, tag)                              \
  _vkr_allocator_alloc(allocator, size, tag, __LINE__, __FILE__)

/**
 * @brief Frees memory from the allocator.
 * @param allocator The allocator to use.
 * @param ptr The pointer to the memory to free.
 * @param tag The tag to associate with the allocation.
 * @param old_size The size of the memory to free.
 * @note If you don't know old_size, call with 0; stats will not adjust
 * bytes_current for that call.
 */
void vkr_allocator_free(VkrAllocator *allocator, void *ptr, uint64_t old_size,
                        VkrAllocatorMemoryTag tag);

/**
 * @brief Reallocates memory from the allocator.
 * @param allocator The allocator to use.
 * @param ptr The pointer to the memory to reallocate.
 * @param old_size The size of the memory to reallocate.
 * @param new_size The new size of the memory to reallocate.
 * @param tag The tag to associate with the allocation.
 */
void *vkr_allocator_realloc(VkrAllocator *allocator, void *ptr,
                            uint64_t old_size, uint64_t new_size,
                            VkrAllocatorMemoryTag tag);

/**
 * @brief Sets the memory.
 * @param allocator The allocator to use. (Optional)
 * @param ptr The pointer to the memory to set.
 * @param value The value to set.
 * @param size The size of the memory to set.
 */
void vkr_allocator_set(VkrAllocator *allocator, void *ptr, uint32_t value,
                       uint64_t size);

/**
 * @brief Zeros the memory.
 * @param allocator The allocator to use. (Optional)
 * @param ptr The pointer to the memory to zero.
 * @param size The size of the memory to zero.
 */
void vkr_allocator_zero(VkrAllocator *allocator, void *ptr, uint64_t size);

/**
 * @brief Copies the memory.
 * @param allocator The allocator to use. (Optional)
 * @param dst The pointer to the memory to copy to.
 * @param src The pointer to the memory to copy from.
 * @param size The size of the memory to copy.
 */
void vkr_allocator_copy(VkrAllocator *allocator, void *dst, const void *src,
                        uint64_t size);

/**
 * @brief Gets the statistics from the allocator.
 * @param allocator The allocator to use.
 * @return The statistics from the allocator.
 */
VkrAllocatorStatistics
vkr_allocator_get_statistics(const VkrAllocator *allocator);

/**
 * @brief Prints the statistics from the allocator.
 * @param allocator The allocator to use.
 * @return Dynamically allocated string with statistics. Caller must free with
 * vkr_allocator_free. Returns NULL on allocation failure.
 */
char *vkr_allocator_print_statistics(VkrAllocator *allocator);

/**
 * @brief Gets the global statistics from the allocator.
 * @return The global statistics from the allocator.
 */
VkrAllocatorStatistics vkr_allocator_get_global_statistics();

/**
 * @brief Prints the global statistics from the allocator.
 * @param allocator The allocator to use.
 * @return Dynamically allocated string with global statistics. Caller must free
 * with vkr_allocator_free. Returns NULL on allocation failure.
 */
char *vkr_allocator_print_global_statistics(VkrAllocator *allocator);