#include "vkr_platform.h"

#if defined(PLATFORM_APPLE)

static mach_timebase_info_data_t timebase_info;
static bool32_t timebase_initialized = false;

bool8_t vkr_platform_init() {
  kern_return_t kr = mach_timebase_info(&timebase_info);
  assert(kr == KERN_SUCCESS && "mach_timebase_info failed");
  timebase_initialized = true;
  return true_v;
}

void *vkr_platform_mem_reserve(uint64_t size) {
  void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return result;
}

bool32_t vkr_platform_mem_commit(void *ptr, uint64_t size) {
  uint32_t result = mprotect(ptr, size, PROT_READ | PROT_WRITE);
  return result == 0;
}

void vkr_platform_mem_decommit(void *ptr, uint64_t size) {
  madvise(ptr, size, MADV_DONTNEED);
  mprotect(ptr, size, PROT_NONE);
}

void vkr_platform_mem_release(void *ptr, uint64_t size) { munmap(ptr, size); }

uint64_t vkr_platform_get_page_size() { return getpagesize(); }

uint64_t vkr_platform_get_large_page_size() {
  uint64_t large_page_size = 0;
  size_t size_len = sizeof(large_page_size);
  uint64_t base_page_size = vkr_platform_get_page_size();

  // Check if we're on Apple Silicon or Intel
  uint32_t result = sysctlbyname("hw.optional.arm64", NULL, NULL, NULL, 0);
  bool is_apple_silicon = (result == 0);

  if (is_apple_silicon) {
    // On Apple Silicon:
    // - 16KB is the BASE page size
    // - 2MB is the actual large page size
    // - 32MB+ sizes may also be available but 2MB is most common
    large_page_size = 2 * 1024 * 1024; // 2MB large pages
  } else {
    // On Intel Macs, also use 2MB large pages
    large_page_size = 2 * 1024 * 1024; // 2MB
  }

  if (large_page_size < base_page_size ||
      (large_page_size % base_page_size) != 0) {
    return base_page_size;
  }

  return large_page_size;
}

uint32_t vkr_platform_get_logical_core_count(void) {
  uint32_t cores = 0;
  size_t size_len = sizeof(cores);

  if (sysctlbyname("hw.logicalcpu_max", &cores, &size_len, NULL, 0) != 0 ||
      cores == 0) {
    long active = sysconf(_SC_NPROCESSORS_ONLN);
    if (active > 0) {
      cores = (uint32_t)active;
    }
  }

  if (cores == 0) {
    cores = 1;
  }

  return cores;
}

void vkr_platform_sleep(uint64_t ms) {
  if (ms == 0) {
    return;
  }

  // For very short sleeps (<= 2ms), use spin-wait to avoid scheduler latency
  if (ms <= 2) {
    float64_t start_time = vkr_platform_get_absolute_time();
    float64_t target_time = start_time + (ms * 0.001);

    while (vkr_platform_get_absolute_time() < target_time) {
      // Yield to other threads occasionally
      sched_yield();
    }
    return;
  }

  // For longer sleeps, sleep for most of the duration then spin-wait the rest
  // This prevents oversleeping which causes missed vsync windows
  uint64_t sleep_ms = ms - 2; // Sleep for all but last 2ms

#if _POSIX_C_SOURCE >= 199309L
  struct timespec ts;
  ts.tv_sec = sleep_ms / 1000;
  ts.tv_nsec = (sleep_ms % 1000) * 1000 * 1000;
  nanosleep(&ts, 0);
#else
  if (sleep_ms >= 1000) {
    sleep(sleep_ms / 1000);
  }
  usleep((sleep_ms % 1000) * 1000);
#endif

  // Spin-wait for the remaining time to hit the exact target
  float64_t target_time =
      vkr_platform_get_absolute_time() + (2.0 * 0.001); // 2ms remaining
  while (vkr_platform_get_absolute_time() < target_time) {
    sched_yield();
  }
}

float64_t vkr_platform_get_absolute_time() {
  assert(timebase_initialized && "vkr_platform_init() must be called first");
  uint64_t mach_now = mach_absolute_time();
  return (float64_t)(mach_now * timebase_info.numer) /
         (timebase_info.denom * 1e9);
}

VkrTime vkr_platform_get_local_time() {
  time_t raw_time;
  time(&raw_time);
  struct tm *time_info = localtime(&raw_time);
  return (VkrTime){
      .seconds = time_info->tm_sec,
      .minutes = time_info->tm_min,
      .hours = time_info->tm_hour,
      .day = time_info->tm_mday,
      .month = time_info->tm_mon,
      .year = time_info->tm_year,
      .weekday = time_info->tm_wday,
      .year_day = time_info->tm_yday,
      .is_dst = time_info->tm_isdst,
      .gmtoff = time_info->tm_gmtoff,
      .timezone_name = time_info->tm_zone,
  };
}

void vkr_platform_console_write(const char *message, uint8_t colour) {
  const char *colour_strings[] = {"0;41", "1;31", "1;33",
                                  "1;32", "1;34", "1;30"};
  uint8_t safe_colour =
      (colour < 6) ? colour
                   : 3; // Default to INFO level (index 3) if out of bounds
  printf("\033[%sm%s\033[0m", colour_strings[safe_colour], message);
  fflush(stdout);
}

void vkr_platform_shutdown() { timebase_initialized = false; }
#endif
