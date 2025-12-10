#include "memory/vkr_arena_pool.h"
#include "core/logger.h"

bool8_t vkr_arena_pool_create(uint64_t chunk_size, uint32_t chunk_count,
                              VkrAllocator *allocator, VkrArenaPool *out_pool) {
  assert_log(out_pool != NULL, "out_pool must not be NULL");
  assert_log(allocator != NULL, "allocator must not be NULL");
  assert_log(chunk_size > 0, "chunk_size must be greater than 0");
  assert_log(chunk_count > 0, "chunk_count must be greater than 0");

  MemZero(out_pool, sizeof(VkrArenaPool));

  // Create the underlying pool
  if (!vkr_pool_create(chunk_size, chunk_count, &out_pool->pool)) {
    log_error("Failed to create arena pool with chunk_size=%llu, count=%u",
              (uint64_t)chunk_size, (uint32_t)chunk_count);
    return false_v;
  }

  // Create mutex for thread-safe access
  if (!vkr_mutex_create(allocator, &out_pool->mutex)) {
    log_error("Failed to create arena pool mutex");
    vkr_pool_destroy(&out_pool->pool);
    return false_v;
  }

  out_pool->chunk_size = chunk_size;
  out_pool->initialized = true_v;

  log_debug("Arena pool created: chunk_size=%llu, chunk_count=%u",
            (uint64_t)chunk_size, (uint32_t)chunk_count);
  return true_v;
}

void vkr_arena_pool_destroy(VkrAllocator *allocator, VkrArenaPool *pool) {
  if (pool == NULL || !pool->initialized) {
    return;
  }

  if (pool->mutex) {
    vkr_mutex_destroy(allocator, &pool->mutex);
  }

  vkr_pool_destroy(&pool->pool);

  MemZero(pool, sizeof(VkrArenaPool));
}

void *vkr_arena_pool_acquire(VkrArenaPool *pool) {
  assert_log(pool != NULL, "pool must not be NULL");
  assert_log(pool->initialized, "pool must be initialized");

  vkr_mutex_lock(pool->mutex);
  void *chunk = vkr_pool_alloc(&pool->pool);
  vkr_mutex_unlock(pool->mutex);

  return chunk;
}

void vkr_arena_pool_release(VkrArenaPool *pool, void *chunk) {
  assert_log(pool != NULL, "pool must not be NULL");
  assert_log(pool->initialized, "pool must be initialized");

  if (chunk == NULL) {
    return;
  }

  vkr_mutex_lock(pool->mutex);
  vkr_pool_free(&pool->pool, chunk);
  vkr_mutex_unlock(pool->mutex);
}