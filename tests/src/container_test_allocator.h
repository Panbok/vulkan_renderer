#pragma once

#include "memory/vkr_allocator.h"

// Deterministic allocation failure for container ownership/rollback tests.
typedef struct ContainerTestAllocator {
  bool8_t fail;
  uint64_t calls;
  uint64_t fail_at;
  uint64_t live_bytes;
} ContainerTestAllocator;

static inline void *container_test_alloc(void *ctx, uint64_t size,
                                         VkrAllocatorMemoryTag tag) {
  (void)tag;
  ContainerTestAllocator *state = ctx;
  state->calls++;
  if (state->fail || state->calls == state->fail_at) {
    return NULL;
  }
  void *data = malloc(size);
  assert(data);
  state->live_bytes += size;
  return data;
}

static inline void *container_test_realloc(void *ctx, void *data,
                                           uint64_t old_size, uint64_t size,
                                           VkrAllocatorMemoryTag tag) {
  (void)tag;
  ContainerTestAllocator *state = ctx;
  state->calls++;
  if (state->fail || state->calls == state->fail_at) {
    return NULL;
  }
  void *result = realloc(data, size);
  assert(result);
  state->live_bytes = state->live_bytes - old_size + size;
  return result;
}

static inline void container_test_free(void *ctx, void *data, uint64_t size,
                                       VkrAllocatorMemoryTag tag) {
  (void)tag;
  ContainerTestAllocator *state = ctx;
  assert(state->live_bytes >= size);
  state->live_bytes -= size;
  free(data);
}

/* Over-allocates and stores the malloc base just before the aligned block, so
 * alignments above malloc's guarantee are honoured. */
static inline void *container_test_alloc_aligned(void *ctx, uint64_t size,
                                                 uint64_t alignment,
                                                 VkrAllocatorMemoryTag tag) {
  (void)tag;
  ContainerTestAllocator *state = ctx;
  state->calls++;
  if (state->fail || state->calls == state->fail_at) {
    return NULL;
  }
  uint8_t *base = malloc(size + alignment + sizeof(void *));
  assert(base);
  const uintptr_t start = (uintptr_t)(base + sizeof(void *));
  const uintptr_t aligned =
      (start + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
  ((void **)aligned)[-1] = base;
  state->live_bytes += size;
  return (void *)aligned;
}

static inline void container_test_free_aligned(void *ctx, void *data,
                                               uint64_t size,
                                               uint64_t alignment,
                                               VkrAllocatorMemoryTag tag) {
  (void)alignment;
  (void)tag;
  ContainerTestAllocator *state = ctx;
  assert(state->live_bytes >= size);
  state->live_bytes -= size;
  free(((void **)data)[-1]);
}

static inline VkrAllocator
container_test_allocator(ContainerTestAllocator *state) {
  return (VkrAllocator){.ctx = state,
                        .alloc = container_test_alloc,
                        .alloc_aligned = container_test_alloc_aligned,
                        .realloc = container_test_realloc,
                        .free = container_test_free,
                        .free_aligned = container_test_free_aligned};
}
