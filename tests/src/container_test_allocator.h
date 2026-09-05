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

static inline VkrAllocator
container_test_allocator(ContainerTestAllocator *state) {
  return (VkrAllocator){.ctx = state,
                        .alloc = container_test_alloc,
                        .realloc = container_test_realloc,
                        .free = container_test_free};
}
