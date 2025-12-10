/**
 * @file vkr_pool.h
 * @brief Fixed-size pool allocator built on top of vkr_freelist.
 *
 * The pool hands out chunks of a fixed size and recycles them via a freelist.
 * Chunks are allocated from a single contiguous memory range reserved/committed
 * up front. All chunk offsets are tracked by vkr_freelist to avoid manual
 * bookkeeping.
 */

#pragma once

#include "containers/vkr_freelist.h"
#include "defines.h"

typedef struct VkrPool {
  void *memory;            // Base memory for chunks
  void *freelist_memory;   // Storage for freelist nodes

  uint64_t memory_size;          // Page-aligned size reserved for chunks
  uint64_t freelist_memory_size; // Page-aligned size reserved for freelist
  uint64_t pool_size;            // Usable bytes = chunk_size * chunk_count
  uint64_t chunk_size;           // Size of each chunk (aligned to MaxAlign)
  uint32_t chunk_count;          // Total number of chunks
  uint32_t allocated;            // Number of active chunks
  uint64_t page_size;            // Platform page size used for alignment

  VkrFreeList freelist; // Tracks free chunk offsets
} VkrPool;

/**
 * @brief Creates a fixed-size pool.
 * @param chunk_size Size of each chunk requested by the user.
 * @param chunk_count Number of chunks to allocate in the pool.
 * @param out_pool Output pool structure.
 * @return true if the pool was created successfully, false otherwise.
 */
bool8_t vkr_pool_create(uint64_t chunk_size, uint32_t chunk_count,
                        VkrPool *out_pool);

/**
 * @brief Destroys a pool and releases its memory back to the platform.
 * @param pool Pool to destroy.
 */
void vkr_pool_destroy(VkrPool *pool);

/**
 * @brief Allocates a chunk from the pool.
 * @param pool Pool to allocate from.
 * @return Pointer to the allocated chunk, or NULL if no space remains.
 */
void *vkr_pool_alloc(VkrPool *pool);

/**
 * @brief Allocates an aligned chunk from the pool.
 * @param pool Pool to allocate from.
 * @param alignment Power-of-two alignment requirement.
 * @return Pointer to the allocated chunk, or NULL on failure.
 */
void *vkr_pool_alloc_aligned(VkrPool *pool, uint64_t alignment);

/**
 * @brief Returns a chunk to the pool.
 * @param pool Pool to free to.
 * @param ptr Pointer previously returned by vkr_pool_alloc(_aligned).
 * @return true if the free succeeded, false otherwise.
 */
bool8_t vkr_pool_free(VkrPool *pool, void *ptr);

/**
 * @brief Returns how many free chunks remain in the pool.
 * @param pool Pool to inspect.
 * @return Number of free chunks left.
 */
uint64_t vkr_pool_free_chunks(VkrPool *pool);
