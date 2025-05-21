#include "platform.h"

#if defined(PLATFORM_APPLE)

static mach_timebase_info_data_t timebase_info;
static bool32_t timebase_initialized = false;

void *platform_mem_reserve(uint64_t size) {
  void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return result;
}

bool32_t platform_mem_commit(void *ptr, uint64_t size) {
  uint32_t result = mprotect(ptr, size, PROT_READ | PROT_WRITE);
  return result == 0;
}

void platform_mem_decommit(void *ptr, uint64_t size) {
  madvise(ptr, size, MADV_DONTNEED);
  mprotect(ptr, size, PROT_NONE);
}

void platform_mem_release(void *ptr, uint64_t size) { munmap(ptr, size); }

uint64_t platform_get_page_size() { return getpagesize(); }

void platform_sleep(uint64_t ms) {
#if _POSIX_C_SOURCE >= 199309L
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000 * 1000;
  nanosleep(&ts, 0);
#else
  if (ms >= 1000) {
    sleep(ms / 1000);
  }
  usleep((ms % 1000) * 1000);
#endif
}

float64_t platform_get_absolute_time() {
  if (!timebase_initialized) {
    kern_return_t kr = mach_timebase_info(&timebase_info);
    if (kr != KERN_SUCCESS) {
      assert(kr == KERN_SUCCESS && "mach_timebase_info failed");
    }
    timebase_initialized = true;
  }
  uint64_t mach_now = mach_absolute_time();
  return (float64_t)(mach_now * timebase_info.numer) /
         (timebase_info.denom * 1e9);
}
#endif