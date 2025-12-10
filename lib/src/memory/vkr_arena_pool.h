#pragma once

#include "core/vkr_threads.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_pool.h"

/**
 * @brief Thread-safe pool for arena memory chunks.
 *
 * Provides fixed-size memory chunks that can be used to create buffer-backed
 * arenas for parallel mesh loading. The pool avoids repeated arena
 * creation/destruction overhead by reusing chunks.
 */
typedef struct VkrArenaPool {
  VkrPool pool;        /**< Underlying fixed-size chunk pool */
  VkrMutex mutex;      /**< Mutex for thread-safe acquire/release */
  uint64_t chunk_size; /**< Size of each chunk */
  bool8_t initialized; /**< Whether the pool is initialized */
} VkrArenaPool;

/**
 * @brief Creates a thread-safe arena pool.
 * @param chunk_size Size of each memory chunk (should fit arena + mesh data).
 * @param chunk_count Number of chunks to allocate (typically worker_count).
 * @param allocator Allocator for mutex creation.
 * @param out_pool Output pool structure.
 * @return true on success, false on failure.
 */
bool8_t vkr_arena_pool_create(uint64_t chunk_size, uint32_t chunk_count,
                              VkrAllocator *allocator, VkrArenaPool *out_pool);

/**
 * @brief Destroys an arena pool and releases all memory.
 * @param allocator Allocator used to create the pool.
 * @param pool Pool to destroy.
 */
void vkr_arena_pool_destroy(VkrAllocator *allocator, VkrArenaPool *pool);

/**
 * @brief Acquires a memory chunk from the pool (thread-safe).
 * @param pool Pool to acquire from.
 * @return Pointer to the chunk, or NULL if pool is exhausted.
 */
void *vkr_arena_pool_acquire(VkrArenaPool *pool);

/**
 * @brief Releases a memory chunk back to the pool (thread-safe).
 * @param pool Pool to release to.
 * @param chunk Chunk pointer previously returned by vkr_arena_pool_acquire.
 */
void vkr_arena_pool_release(VkrArenaPool *pool, void *chunk);