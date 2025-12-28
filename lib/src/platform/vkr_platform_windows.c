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

uint32_t vkr_platform_get_logical_core_count(void) {
  DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (count == 0) {
    count = GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS);
  }

  if (count == 0) {
    count = 1;
  }

  return (uint32_t)count;
}

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

VkrTime vkr_platform_get_local_time() {
  VkrTime result = {0};

  time_t raw_time;
  time(&raw_time);

  struct tm time_info;
  if (localtime_s(&time_info, &raw_time) != 0) {
    return result;
  }

  long timezone_sec = 0;
  _get_timezone(&timezone_sec);

  int32_t gmtoff = -(int32_t)timezone_sec;

  if (time_info.tm_isdst > 0) {
    long dst_bias_sec = 0;
    _get_dstbias(&dst_bias_sec);
    gmtoff -= (int32_t)dst_bias_sec;
  }

  int tz_index = (time_info.tm_isdst > 0) ? 1 : 0;
  result.timezone_name = _tzname[tz_index];

  result.seconds = time_info.tm_sec;
  result.minutes = time_info.tm_min;
  result.hours = time_info.tm_hour;
  result.day = time_info.tm_mday;
  result.month = time_info.tm_mon;
  result.year = time_info.tm_year;
  result.weekday = time_info.tm_wday;
  result.year_day = time_info.tm_yday;
  result.is_dst = time_info.tm_isdst;
  result.gmtoff = gmtoff;

  return result;
}

void vkr_platform_console_write(const char *message, uint8_t colour) {
  HANDLE console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (console_handle == INVALID_HANDLE_VALUE) {
    OutputDebugStringA(message);
    return;
  }
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
