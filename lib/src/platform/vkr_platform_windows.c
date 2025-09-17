#include "vkr_platform.h"

#if defined(PLATFORM_WINDOWS)
static float64_t clock_frequency;
static bool32_t high_res_timer_enabled = false;
static UINT timer_resolution = 0;

bool8_t vkr_platform_init() {
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
      timer_resolution = target_resolution;
    }
  }

  return true_v;
}

void *vkr_platform_mem_reserve(uint64_t size) {
  return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool32_t vkr_platform_mem_commit(void *ptr, uint64_t size) {
  return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void vkr_platform_mem_decommit(void *ptr, uint64_t size) {
  VirtualFree(ptr, size, MEM_DECOMMIT);
}

void vkr_platform_mem_release(void *ptr, uint64_t _size) {
  VirtualFree(ptr, 0, MEM_RELEASE);
}

uint64_t vkr_platform_get_page_size() {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
}

uint64_t vkr_platform_get_large_page_size() { return GetLargePageMinimum(); }

void vkr_platform_sleep(uint64_t ms) {
  if (ms == 0) {
    return;
  }

  // For very short sleeps, use a hybrid approach
  if (ms <= 2) {
    // For delays <= 2ms, use busy-wait with occasional yields
    float64_t start_time = vkr_platform_get_absolute_time();
    float64_t target_time = start_time + (ms * 0.001);

    while (vkr_platform_get_absolute_time() < target_time) {
      // Yield thread every few iterations to be nice to other threads
      SwitchToThread();
    }
  } else {
    // For longer delays, sleep for most of the time, then busy-wait the
    // remainder
    if (ms > 1) {
      Sleep(ms - 1); // Sleep for most of the duration
    }

    // Busy-wait for the remainder for better precision
    float64_t start_time = vkr_platform_get_absolute_time();
    float64_t target_time = start_time + (1.0 * 0.001); // 1ms remainder

    while (vkr_platform_get_absolute_time() < target_time) {
      SwitchToThread();
    }
  }
}

float64_t vkr_platform_get_absolute_time() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (float64_t)now.QuadPart * clock_frequency;
}

void vkr_platform_console_write(const char *message, uint8_t colour) {
  HANDLE console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  // FATAL,ERROR,WARN,INFO,DEBUG,TRACE
  static uint8_t levels[6] = {64, 4, 6, 2, 1, 8};

  uint8_t safe_colour =
      (colour < 6) ? colour
                   : 3; // Default to INFO level (index 3) if out of bounds
  SetConsoleTextAttribute(console_handle, levels[safe_colour]);

  OutputDebugStringA(message);
  uint64_t length = strlen(message);
  DWORD number_written_var;
  WriteConsoleA(console_handle, message, (DWORD)length, &number_written_var, 0);
  SetConsoleTextAttribute(console_handle,
                          FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

void vkr_platform_shutdown() {
  if (high_res_timer_enabled) {
    timeEndPeriod(timer_resolution);
    high_res_timer_enabled = false;
  }
}
#endif