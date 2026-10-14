#include "memory/vkr_heap.h"

void *vkr_heap_alloc(uint64_t size) {
  if (size > (uint64_t)SIZE_MAX) {
    return NULL;
  }
  return malloc((size_t)size);
}

void vkr_heap_free(void *block) { free(block); }

void *vkr_heap_alloc_zeroed(uint64_t count, uint64_t size) {
  if (count > (uint64_t)SIZE_MAX || size > (uint64_t)SIZE_MAX) {
    return NULL;
  }
  return calloc((size_t)count, (size_t)size);
}

void *vkr_heap_realloc(void *block, uint64_t size) {
  if (size > (uint64_t)SIZE_MAX) {
    return NULL;
  }
  return realloc(block, (size_t)size);
}
