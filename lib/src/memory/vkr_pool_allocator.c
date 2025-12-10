#include "vkr_pool_allocator.h"
#include "core/logger.h"
#include "memory/vkr_pool.h"

vkr_internal INLINE bool8_t vkr_pool_allocator_contains(VkrPool *pool,
                                                        void *ptr) {
  assert_log(pool != NULL, "pool must not be NULL");
  assert_log(ptr != NULL, "ptr must not be NULL");
  assert_log(pool->chunk_size > 0, "pool not initialized");

  uintptr_t base = (uintptr_t)pool->memory;
  uintptr_t end = base + (uintptr_t)pool->pool_size;
  uintptr_t target = (uintptr_t)ptr;
  if (target < base || target >= end) {
    return false_v;
  }

  uint64_t offset = (uint64_t)(target - base);
  return (offset % pool->chunk_size) == 0;
}

vkr_internal INLINE void *vkr_pool_alloc_cb(void *ctx, uint64_t size,
                                            VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrPool *pool = (VkrPool *)ctx;
  if (size > pool->chunk_size) {
    log_error("Pool allocation size (%llu) exceeds chunk size (%llu)",
              (uint64_t)size, (uint64_t)pool->chunk_size);
    return NULL;
  }

  return vkr_pool_alloc(pool);
}

vkr_internal INLINE void *vkr_pool_alloc_aligned_cb(void *ctx, uint64_t size,
                                                    uint64_t alignment,
                                                    VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(alignment > 0, "Alignment must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrPool *pool = (VkrPool *)ctx;
  if (size > pool->chunk_size) {
    log_error("Pool allocation size (%llu) exceeds chunk size (%llu)",
              (uint64_t)size, (uint64_t)pool->chunk_size);
    return NULL;
  }

  return vkr_pool_alloc_aligned(pool, alignment);
}

vkr_internal INLINE void vkr_pool_free_cb(void *ctx, void *ptr,
                                          uint64_t old_size,
                                          VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(ptr != NULL, "Pointer must not be NULL");
  assert_log(old_size > 0, "Old size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrPool *pool = (VkrPool *)ctx;
  if (old_size > pool->chunk_size) {
    log_error("Free size (%llu) exceeds pool chunk size (%llu)",
              (uint64_t)old_size, (uint64_t)pool->chunk_size);
    return;
  }

  if (!vkr_pool_free(pool, ptr)) {
    log_error("Failed to free pointer %p back to pool", ptr);
  }
}

vkr_internal INLINE void vkr_pool_free_aligned_cb(void *ctx, void *ptr,
                                                  uint64_t old_size,
                                                  uint64_t alignment,
                                                  VkrAllocatorMemoryTag tag) {
  (void)alignment;
  vkr_pool_free_cb(ctx, ptr, old_size, tag);
}

vkr_internal INLINE void *vkr_pool_realloc_cb(void *ctx, void *ptr,
                                              uint64_t old_size,
                                              uint64_t new_size,
                                              VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(new_size > 0, "New size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrPool *pool = (VkrPool *)ctx;
  if (new_size > pool->chunk_size) {
    log_error("Realloc size (%llu) exceeds pool chunk size (%llu)",
              (uint64_t)new_size, (uint64_t)pool->chunk_size);
    return NULL;
  }

  if (ptr == NULL) {
    return vkr_pool_alloc(pool);
  }

  if (!vkr_pool_allocator_contains(pool, ptr)) {
    log_error("Pointer %p does not belong to pool during realloc", ptr);
    return NULL;
  }

  (void)old_size;
  return ptr; // Size fits within the existing fixed-size chunk.
}

vkr_internal INLINE void *
vkr_pool_realloc_aligned_cb(void *ctx, void *ptr, uint64_t old_size,
                            uint64_t new_size, uint64_t alignment,
                            VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(new_size > 0, "New size must be greater than 0");
  assert_log(alignment > 0, "Alignment must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrPool *pool = (VkrPool *)ctx;
  if (new_size > pool->chunk_size) {
    log_error("Realloc size (%llu) exceeds pool chunk size (%llu)",
              (uint64_t)new_size, (uint64_t)pool->chunk_size);
    return NULL;
  }

  if (ptr == NULL) {
    return vkr_pool_alloc_aligned(pool, alignment);
  }

  if (!vkr_pool_allocator_contains(pool, ptr)) {
    log_error("Pointer %p does not belong to pool during realloc", ptr);
    return NULL;
  }

  if (((uintptr_t)ptr % alignment) != 0) {
    log_error("Existing pool pointer %p does not satisfy alignment %llu", ptr,
              (unsigned long long)alignment);
    return NULL;
  }

  (void)old_size;
  return ptr;
}

void vkr_pool_allocator_create(VkrAllocator *out_allocator) {
  assert_log(out_allocator != NULL, "out_allocator must not be NULL");
  assert_log(out_allocator->ctx != NULL, "Allocator context (VkrPool) is NULL");

  VkrPool *pool = (VkrPool *)out_allocator->ctx;
  assert_log(pool->chunk_size > 0, "Pool must be initialized before use");

  out_allocator->type = VKR_ALLOCATOR_TYPE_POOL;
  out_allocator->stats = (VkrAllocatorStatistics){0};
  out_allocator->alloc = vkr_pool_alloc_cb;
  out_allocator->alloc_aligned = vkr_pool_alloc_aligned_cb;
  out_allocator->free = vkr_pool_free_cb;
  out_allocator->free_aligned = vkr_pool_free_aligned_cb;
  out_allocator->realloc = vkr_pool_realloc_cb;
  out_allocator->realloc_aligned = vkr_pool_realloc_aligned_cb;
  out_allocator->scope_depth = 0;
  out_allocator->scope_bytes_allocated = 0;
  out_allocator->begin_scope = NULL;
  out_allocator->end_scope = NULL;
  out_allocator->supports_scopes = false_v;
}

void vkr_pool_allocator_destroy(VkrAllocator *allocator) {
  assert_log(allocator != NULL, "allocator must not be NULL");
  assert_log(allocator->ctx != NULL, "allocator ctx must not be NULL");

  vkr_pool_destroy((VkrPool *)allocator->ctx);
  allocator->ctx = NULL;
}
