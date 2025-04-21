#include "core.h"

void *mem_reserve(size_t size) {
  void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return result;
}

bool mem_commit(void *ptr, size_t size) {
  mprotect(ptr, size, PROT_READ | PROT_WRITE);
  return 1;
}

void mem_decommit(void *ptr, size_t size) {
  madvise(ptr, size, MADV_DONTNEED);
  mprotect(ptr, size, PROT_NONE);
}

void mem_release(void *ptr, size_t size) { munmap(ptr, size); }

size_t get_page_size() { return getpagesize(); }