#include "memory/vkr_heap.h"

void *vkr_heap_alloc(uint64_t size) {
  if (size > (uint64_t)SIZE_MAX) {
    return NULL;
  }
  return malloc((size_t)size);
}

void vkr_heap_free(void *block) { free(block); }
