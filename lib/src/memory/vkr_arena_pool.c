#include "memory/vkr_arena_pool.h"
#include "core/logger.h"
#include "memory/vkr_arena_allocator.h"

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

  // Create condition variable for blocking acquire
  if (!vkr_cond_create(allocator, &out_pool->cond)) {
    log_error("Failed to create arena pool condition variable");
    vkr_mutex_destroy(allocator, &out_pool->mutex);
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

  if (pool->cond) {
    vkr_cond_destroy(allocator, &pool->cond);
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
  while (vkr_pool_free_chunks(&pool->pool) == 0) {
    if (!vkr_cond_wait(pool->cond, pool->mutex)) {
      vkr_mutex_unlock(pool->mutex);
      return NULL;
    }
  }

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
  bool8_t freed = vkr_pool_free(&pool->pool, chunk);
  if (freed) {
    vkr_cond_signal(pool->cond);
  }
  vkr_mutex_unlock(pool->mutex);
}

bool8_t vkr_arena_pool_acquire_arena(VkrArenaPool *pool, void **out_chunk,
                                     Arena **out_arena,
                                     VkrAllocator *out_allocator) {
  assert_log(out_chunk != NULL, "out_chunk must not be NULL");
  assert_log(out_arena != NULL, "out_arena must not be NULL");
  assert_log(out_allocator != NULL, "out_allocator must not be NULL");

  *out_chunk = NULL;
  *out_arena = NULL;
  MemZero(out_allocator, sizeof(*out_allocator));
  if (!pool || !pool->initialized) {
    return false_v;
  }

  void *chunk = vkr_arena_pool_acquire(pool);
  if (!chunk) {
    return false_v;
  }
  Arena *arena = arena_create_from_buffer(chunk, pool->chunk_size);
  if (!arena) {
    vkr_arena_pool_release(pool, chunk);
    return false_v;
  }
  VkrAllocator allocator = {.ctx = arena};
  if (!vkr_allocator_arena(&allocator)) {
    arena_destroy(arena);
    vkr_arena_pool_release(pool, chunk);
    return false_v;
  }

  *out_chunk = chunk;
  *out_arena = arena;
  *out_allocator = allocator;
  return true_v;
}

void vkr_arena_pool_release_arena(VkrArenaPool *pool, void *chunk, Arena *arena,
                                  VkrAllocator *allocator) {
  if (allocator && arena) {
    vkr_allocator_release_global_accounting(allocator);
  }
  if (arena) {
    arena_destroy(arena);
  }
  if (pool && chunk) {
    vkr_arena_pool_release(pool, chunk);
  }
}
