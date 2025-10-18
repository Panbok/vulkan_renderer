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
 * This allocator uses sparse buffers: it reserves a large virtual address space
 * upfront but only commits physical memory as needed. This approach eliminates
 * pointer invalidation on resize since the base address never changes.
 */
typedef struct VkrDMemory {
  void *base_memory;       // Base address of reserved memory block
  uint64_t reserve_size;   // Total reserved virtual address space
  uint64_t total_size;     // Currently available size for allocations
  uint64_t committed_size; // Currently committed physical memory
  uint64_t page_size;      // Platform page size

  void *freelist_memory;         // Memory block for freelist node storage
  uint64_t freelist_memory_size; // Size of freelist memory block
  VkrFreeList freelist;          // Freelist tracking free blocks
} VkrDMemory;

/**
 * @brief Creates a dynamic memory allocator with sparse buffer support
 * @param total_size Initial available size for allocations
 * @param max_reserve_size Maximum virtual address space to reserve (must be >=
 * total_size)
 * @param out_dmemory Output dmemory structure
 * @return true if successful, false otherwise
 *
 * @note The allocator reserves max_reserve_size of virtual memory but only
 * commits total_size initially. This allows efficient resizing without pointer
 * invalidation.
 */
bool8_t vkr_dmemory_create(uint64_t total_size, uint64_t max_reserve_size,
                           VkrDMemory *out_dmemory);

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

/**
 * @brief Resizes a dmemory allocator to a larger size
 * @param dmemory The dmemory to resize
 * @param new_total_size New total size (must be <= reserve_size)
 * @return true if successful, false otherwise
 *
 * @note With sparse buffers, all existing pointers remain valid after resize
 *       since the base address never changes. Only additional pages are
 * committed.
 */
bool8_t vkr_dmemory_resize(VkrDMemory *dmemory, uint64_t new_total_size);