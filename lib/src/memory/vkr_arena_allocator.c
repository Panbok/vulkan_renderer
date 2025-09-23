#include "vkr_arena_allocator.h"
#include "core/logger.h"
#include "defines.h"

vkr_internal INLINE ArenaMemoryTag to_arena_tag(VkrAllocatorMemoryTag t) {
  return (ArenaMemoryTag)t;
}

vkr_internal INLINE void *arena_alloc_cb(void *ctx, uint64_t size,
                                         VkrAllocatorMemoryTag tag) {
  Arena *arena = (Arena *)ctx;
  return arena_alloc(arena, size, to_arena_tag(tag));
}

vkr_internal INLINE void arena_free_cb(void *ctx, void *ptr, uint64_t old_size,
                                       VkrAllocatorMemoryTag tag) {
  return;
}

vkr_internal INLINE void *arena_realloc_cb(void *ctx, void *ptr,
                                           uint64_t old_size, uint64_t new_size,
                                           VkrAllocatorMemoryTag tag) {
  Arena *arena = (Arena *)ctx;

  if (ptr && new_size <= old_size) {
    return ptr;
  }

  void *p = arena_alloc(arena, new_size, to_arena_tag(tag));
  if (!p)
    return NULL;

  if (ptr && old_size) {
    uint64_t n = vkr_min_u64(old_size, new_size);
    MemCopy(p, ptr, n);
  }

  return p;
}

void vkr_allocator_arena(VkrAllocator *out_allocator) {
  assert_log(out_allocator != NULL, "Arena and out_allocator must not be NULL");

  if (out_allocator->ctx == NULL) {
    log_error("Arena must not be NULL");
    return;
  }

  out_allocator->type = VKR_ALLOCATOR_TYPE_ARENA;
  out_allocator->stats = (VkrAllocatorStatistics){0};
  out_allocator->alloc = &arena_alloc_cb;
  out_allocator->free = &arena_free_cb;
  out_allocator->realloc = &arena_realloc_cb;
}