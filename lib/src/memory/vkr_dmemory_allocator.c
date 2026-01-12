#include "vkr_dmemory_allocator.h"
#include "core/logger.h"
#include "memory/vkr_dmemory.h"

vkr_internal INLINE void *dmemory_alloc_cb(void *ctx, uint64_t size,
                                           VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrDMemory *dmemory = (VkrDMemory *)ctx;
  return vkr_dmemory_alloc(dmemory, size);
}

vkr_internal INLINE void *dmemory_alloc_aligned_cb(void *ctx, uint64_t size,
                                                   uint64_t alignment,
                                                   VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");
  assert_log(alignment > 0, "Alignment must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  VkrDMemory *dmemory = (VkrDMemory *)ctx;
  return vkr_dmemory_alloc_aligned(dmemory, size, alignment);
}

vkr_internal INLINE void dmemory_free_aligned_cb(void *ctx, void *ptr,
                                                 uint64_t old_size,
                                                 uint64_t alignment,
                                                 VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(ptr != NULL, "Pointer must not be NULL");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");
  VkrDMemory *dmemory = (VkrDMemory *)ctx;
  if (vkr_dmemory_free_aligned(dmemory, ptr, old_size, alignment)) {
    return;
  }

  log_error("Failed to free aligned memory from dmemory allocator");
}

vkr_internal INLINE void dmemory_free_cb(void *ctx, void *ptr,
                                         uint64_t old_size,
                                         VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(ptr != NULL, "Pointer must not be NULL");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrDMemory *dmemory = (VkrDMemory *)ctx;
  if (vkr_dmemory_free(dmemory, ptr, old_size)) {
    return;
  }

  log_error("Failed to free memory from dmemory allocator");
}

vkr_internal INLINE void *dmemory_realloc_cb(void *ctx, void *ptr,
                                             uint64_t old_size,
                                             uint64_t new_size,
                                             VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(new_size > 0, "New size must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrDMemory *dmemory = (VkrDMemory *)ctx;
  return vkr_dmemory_realloc(dmemory, ptr, new_size, 0);
}

vkr_internal INLINE void *
dmemory_realloc_aligned_cb(void *ctx, void *ptr, uint64_t old_size,
                           uint64_t new_size, uint64_t alignment,
                           VkrAllocatorMemoryTag tag) {
  assert_log(ctx != NULL, "Context must not be NULL");
  assert_log(new_size > 0, "New size must be greater than 0");
  assert_log(alignment > 0, "Alignment must be greater than 0");
  assert_log(tag < VKR_ALLOCATOR_MEMORY_TAG_MAX,
             "Tag must be less than VKR_ALLOCATOR_MEMORY_TAG_MAX");

  VkrDMemory *dmemory = (VkrDMemory *)ctx;
  return vkr_dmemory_realloc(dmemory, ptr, new_size, alignment);
}

void vkr_dmemory_allocator_create(VkrAllocator *out_allocator) {
  assert_log(out_allocator != NULL, "Out allocator must not be NULL");

  if (out_allocator->ctx == NULL) {
    log_error("Allocator context (VkrDMemory) must not be NULL");
    return;
  }

  out_allocator->type = VKR_ALLOCATOR_TYPE_DMEMORY;
  out_allocator->stats = (VkrAllocatorStatistics){0};
  out_allocator->alloc = dmemory_alloc_cb;
  out_allocator->free = dmemory_free_cb;
  out_allocator->realloc = dmemory_realloc_cb;
  out_allocator->alloc_aligned = dmemory_alloc_aligned_cb;
  out_allocator->free_aligned = dmemory_free_aligned_cb;
  out_allocator->realloc_aligned = dmemory_realloc_aligned_cb;
  out_allocator->accounting_released = false_v;
}

void vkr_dmemory_allocator_destroy(VkrAllocator *allocator) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->ctx != NULL,
             "Allocator context (VkrDMemory) must not be NULL");

  vkr_dmemory_destroy(allocator->ctx);
}
