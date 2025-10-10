/**
 * @file vkr_dmemory.h
 * @brief Defines the dynamic memory allocator using platform memory and
 * freelist tracking
 */

#pragma once

#include "containers/vkr_freelist.h"

/**
 * @brief Dynamic memory allocator using platform memory and freelist tracking
 *
 * This allocator reserves a large block of virtual memory from the platform,
 * commits pages as needed, and uses a freelist to track allocations.
 * It does NOT use malloc/free/realloc.
 */
typedef struct VkrDMemory {
  void *base_memory;       // Base address of reserved memory block
  uint64_t total_size;     // Total size of reserved memory
  uint64_t committed_size; // Currently committed memory
  uint64_t page_size;      // Platform page size

  void *freelist_memory;         // Memory block for freelist node storage
  uint64_t freelist_memory_size; // Size of freelist memory block
  VkrFreeList freelist;          // Freelist tracking free blocks
} VkrDMemory;

/**
 * @brief Creates a dynamic memory allocator
 * @param total_size Total size of memory to reserve (should be page-aligned)
 * @param out_dmemory Output dmemory structure
 * @return true if successful, false otherwise
 */
bool8_t vkr_dmemory_create(uint64_t total_size, VkrDMemory *out_dmemory);

/**
 * @brief Destroys a dynamic memory allocator
 * @param dmemory The dmemory to destroy
 */
void vkr_dmemory_destroy(VkrDMemory *dmemory);

/**
 * @brief Allocates memory from the dmemory allocator
 * @param dmemory The dmemory to allocate from
 * @param size Size of memory to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
void *vkr_dmemory_alloc(VkrDMemory *dmemory, uint64_t size);

/**
 * @brief Frees memory back to the dmemory allocator
 * @param dmemory The dmemory to free to
 * @param ptr Pointer to memory to free
 * @param size Size of memory to free
 * @return true if successful, false otherwise
 */
bool8_t vkr_dmemory_free(VkrDMemory *dmemory, void *ptr, uint64_t size);

/**
 * @brief Gets the total free space in the dmemory allocator
 * @param dmemory The dmemory to query
 * @return Total free space in bytes
 */
uint64_t vkr_dmemory_get_free_space(VkrDMemory *dmemory);