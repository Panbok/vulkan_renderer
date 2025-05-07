#include "platform.h"

#if defined(PLATFORM_APPLE)
void *platform_mem_reserve(uint64_t size) {
  void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return result;
}

bool32_t platform_mem_commit(void *ptr, uint64_t size) {
  mprotect(ptr, size, PROT_READ | PROT_WRITE);
  return 1;
}

void platform_mem_decommit(void *ptr, uint64_t size) {
  madvise(ptr, size, MADV_DONTNEED);
  mprotect(ptr, size, PROT_NONE);
}

void platform_mem_release(void *ptr, uint64_t size) { munmap(ptr, size); }

uint64_t platform_get_page_size() { return getpagesize(); }

#endif