#include "platform.h"

#if defined(PLATFORM_WINDOWS)
static float64_t clock_frequency;
static bool32_t high_res_timer_enabled = false;

void platform_init() {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  clock_frequency = 1.0 / (float64_t)frequency.QuadPart;

  // Try to enable high resolution timer for better Sleep() precision
  TIMECAPS tc;
  if (timeGetDevCaps(&tc, sizeof(TIMECAPS)) == TIMERR_NOERROR) {
    // Request 1ms resolution if supported
    UINT target_resolution = min(max(tc.wPeriodMin, 1), tc.wPeriodMax);
    if (timeBeginPeriod(target_resolution) == TIMERR_NOERROR) {
      high_res_timer_enabled = true;
    }
  }
}

void *platform_mem_reserve(uint64_t size) {
  return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool32_t platform_mem_commit(void *ptr, uint64_t size) {
  return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void platform_mem_decommit(void *ptr, uint64_t size) {
  VirtualFree(ptr, size, MEM_DECOMMIT);
}

void platform_mem_release(void *ptr, uint64_t size) {
  VirtualFree(ptr, size, MEM_RELEASE);
}

uint64_t platform_get_page_size() {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
}

uint64_t platform_get_large_page_size() { return GetLargePageMinimum(); }

void platform_sleep(uint64_t ms) {
  if (ms == 0) {
    return;
  }

  // For very short sleeps, use a hybrid approach
  if (ms <= 2) {
    // For delays <= 2ms, use busy-wait with occasional yields
    float64_t start_time = platform_get_absolute_time();
    float64_t target_time = start_time + (ms * 0.001);

    while (platform_get_absolute_time() < target_time) {
      // Yield thread every few iterations to be nice to other threads
      SwitchToThread();
    }
  } else {
    // For longer delays, sleep for most of the time, then busy-wait the
    // remainder
    if (ms > 3) {
      Sleep(ms - 1); // Sleep for most of the duration
    }

    // Busy-wait for the remainder for better precision
    float64_t start_time = platform_get_absolute_time();
    float64_t target_time = start_time + (1.0 * 0.001); // 1ms remainder

    while (platform_get_absolute_time() < target_time) {
      SwitchToThread();
    }
  }
}

float64_t platform_get_absolute_time() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (float64_t)now.QuadPart * clock_frequency;
}

void platform_shutdown() {
  if (high_res_timer_enabled) {
    timeEndPeriod(1);
    high_res_timer_enabled = false;
  }
}
#endif