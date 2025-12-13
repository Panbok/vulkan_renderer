#include "vkr_arena_allocator.h"
#include "core/logger.h"
#include "defines.h"

_Static_assert((uint32_t)VKR_ALLOCATOR_MEMORY_TAG_MAX ==
                   (uint32_t)ARENA_MEMORY_TAG_MAX,
               "Memory tag enums must have the same size");

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

vkr_internal INLINE void *arena_alloc_aligned_cb(void *ctx, uint64_t size,
                                                 uint64_t alignment,
                                                 VkrAllocatorMemoryTag tag) {
  Arena *arena = (Arena *)ctx;
  return arena_alloc_aligned(arena, size, alignment, to_arena_tag(tag));
}

vkr_internal INLINE void arena_free_aligned_cb(void *ctx, void *ptr,
                                               uint64_t old_size,
                                               uint64_t alignment,
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

vkr_internal INLINE void *arena_realloc_aligned_cb(void *ctx, void *ptr,
                                                   uint64_t old_size,
                                                   uint64_t new_size,
                                                   uint64_t alignment,
                                                   VkrAllocatorMemoryTag tag) {
  Arena *arena = (Arena *)ctx;

  if (ptr && new_size <= old_size) {
    return ptr;
  }

  void *p =
      arena_alloc_aligned(arena, new_size, alignment, to_arena_tag(tag));
  if (!p)
    return NULL;

  if (ptr && old_size) {
    uint64_t n = vkr_min_u64(old_size, new_size);
    MemCopy(p, ptr, n);
  }

  return p;
}

vkr_internal VkrAllocatorScope arena_begin_scope_cb(VkrAllocator *allocator) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->ctx != NULL,
             "Allocator context (Arena) must not be NULL");

  Arena *arena = (Arena *)allocator->ctx;
  Scratch scratch = scratch_create(arena);

  allocator->scope_depth++;
  allocator->stats.total_scopes_created++;

  VkrAllocatorScope scope = {
      .allocator = allocator,
      .scope_data = NULL,
      .bytes_at_start = scratch.pos,
      .total_allocated_at_start = allocator->stats.total_allocated,
      .tags_snapshot_valid = true_v,
  };

  for (uint32_t i = 0; i < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++i) {
    scope.tagged_allocs_at_start[i] = allocator->stats.tagged_allocs[i];
  }

  return scope;
}

vkr_internal void arena_end_scope_cb(VkrAllocator *allocator,
                                     VkrAllocatorScope *scope,
                                     VkrAllocatorMemoryTag tag) {
  assert_log(allocator != NULL, "Allocator must not be NULL");
  assert_log(allocator->ctx != NULL,
             "Allocator context (Arena) must not be NULL");
  assert_log(scope != NULL, "Scope must not be NULL");

  if (allocator->scope_depth == 0) {
    log_error("arena_end_scope_cb called without matching begin_scope");
    return;
  }

  Arena *arena = (Arena *)allocator->ctx;

  // Compute bytes released as (current_bytes - scope_start_bytes)
  uint64_t bytes_released = 0;
  if (allocator->stats.total_allocated > scope->total_allocated_at_start) {
    bytes_released =
        allocator->stats.total_allocated - scope->total_allocated_at_start;
  }

  // Add released bytes to scope_bytes_allocated (tracks cumulative scope
  // allocations)
  allocator->scope_bytes_allocated += bytes_released;

  Scratch scratch = {.arena = arena, .pos = scope->bytes_at_start};
  scratch_destroy(scratch, to_arena_tag(tag));

  allocator->scope_depth--;

  allocator->stats.total_scopes_destroyed++;
}

bool8_t vkr_allocator_arena(VkrAllocator *out_allocator) {
  assert_log(out_allocator != NULL, "out_allocator must not be NULL");

  if (out_allocator->ctx == NULL) {
    log_error("Allocator context (Arena) must not be NULL");
    return false_v;
  }

  out_allocator->type = VKR_ALLOCATOR_TYPE_ARENA;
  out_allocator->stats = (VkrAllocatorStatistics){0};
  out_allocator->alloc = arena_alloc_cb;
  out_allocator->free = arena_free_cb;
  out_allocator->realloc = arena_realloc_cb;
  out_allocator->alloc_aligned = arena_alloc_aligned_cb;
  out_allocator->free_aligned = arena_free_aligned_cb;
  out_allocator->realloc_aligned = arena_realloc_aligned_cb;
  out_allocator->scope_depth = 0;
  out_allocator->scope_bytes_allocated = 0;
  out_allocator->begin_scope = arena_begin_scope_cb;
  out_allocator->end_scope = arena_end_scope_cb;
  out_allocator->supports_scopes = true_v;

  return true_v;
}
