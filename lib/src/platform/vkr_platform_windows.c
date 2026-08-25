#include "vkr_platform.h"

#if defined(PLATFORM_WINDOWS)
#include "containers/str.h"

static float64_t clock_frequency;
static bool32_t high_res_timer_enabled = false;
static UINT timer_resolution = 0;

bool8_t vkr_platform_init() {
  if (!SetProcessDpiAwarenessContext(
          DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    const DWORD error = GetLastError();
    fprintf(stderr,
            "Failed to establish Per-Monitor V2 DPI awareness "
            "(Win32 error %lu)\n",
            (unsigned long)error);
    return false_v;
  }
  if (!AreDpiAwarenessContextsEqual(
          GetThreadDpiAwarenessContext(),
          DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    fprintf(stderr,
            "Per-Monitor V2 DPI awareness request did not become effective\n");
    return false_v;
  }
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
  result.milliseconds = 0;

  return result;
}

bool8_t vkr_platform_get_utc_time(VkrTime *out_time) {
  if (!out_time) {
    return false_v;
  }
  SYSTEMTIME utc = {0};
  GetSystemTime(&utc);
  *out_time = (VkrTime){
      .seconds = utc.wSecond,
      .minutes = utc.wMinute,
      .hours = utc.wHour,
      .day = utc.wDay,
      .month = (int32_t)utc.wMonth - 1,
      .year = (int32_t)utc.wYear - 1900,
      .weekday = utc.wDayOfWeek,
      .year_day = 0,
      .is_dst = 0,
      .gmtoff = 0,
      .milliseconds = utc.wMilliseconds,
      .timezone_name = "UTC",
  };
  return true_v;
}

uint32_t vkr_platform_get_process_id(void) {
  return (uint32_t)GetCurrentProcessId();
}

bool8_t vkr_platform_get_system_info(VkrPlatformSystemInfo *out_info) {
  if (!out_info) {
    return false_v;
  }
  MemZero(out_info, sizeof(*out_info));
  string_format(out_info->os, sizeof(out_info->os), "Windows");
  if (GetEnvironmentVariableA("PROCESSOR_IDENTIFIER", out_info->cpu,
                              sizeof(out_info->cpu)) == 0u) {
    string_format(out_info->cpu, sizeof(out_info->cpu), "unknown");
  }
  switch (GetPriorityClass(GetCurrentProcess())) {
  case IDLE_PRIORITY_CLASS:
    out_info->process_priority = 19;
    break;
  case BELOW_NORMAL_PRIORITY_CLASS:
    out_info->process_priority = 10;
    break;
  case ABOVE_NORMAL_PRIORITY_CLASS:
    out_info->process_priority = -5;
    break;
  case HIGH_PRIORITY_CLASS:
    out_info->process_priority = -10;
    break;
  case REALTIME_PRIORITY_CLASS:
    out_info->process_priority = -20;
    break;
  default:
    out_info->process_priority = 0;
    break;
  }
  return true_v;
}

vkr_internal void vkr_platform_stream_write(DWORD stream, const char *message) {
  if (!message) {
    return;
  }
  HANDLE handle = GetStdHandle(stream);
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    OutputDebugStringA(message);
    return;
  }
  const uint64_t length = string_length(message);
  uint64_t offset = 0u;
  while (offset < length) {
    const DWORD chunk = (DWORD)Min(length - offset, (uint64_t)UINT32_MAX);
    DWORD written = 0u;
    if (!WriteFile(handle, message + offset, chunk, &written, NULL) ||
        written == 0u) {
      break;
    }
    offset += written;
  }
}

void vkr_platform_stdout_write(const char *message) {
  vkr_platform_stream_write(STD_OUTPUT_HANDLE, message);
}

void vkr_platform_stderr_write(const char *message) {
  vkr_platform_stream_write(STD_ERROR_HANDLE, message);
}

vkr_internal bool8_t vkr_platform_command_append(char *command,
                                                 uint64_t capacity,
                                                 uint64_t *cursor, char c) {
  if (*cursor + 1u >= capacity) {
    return false_v;
  }
  command[(*cursor)++] = c;
  command[*cursor] = '\0';
  return true_v;
}

vkr_internal bool8_t vkr_platform_command_append_argument(
    char *command, uint64_t capacity, uint64_t *cursor, const char *argument) {
  if (!argument ||
      (*cursor > 0u &&
       !vkr_platform_command_append(command, capacity, cursor, ' ')) ||
      !vkr_platform_command_append(command, capacity, cursor, '"')) {
    return false_v;
  }
  uint64_t backslashes = 0u;
  for (const char *at = argument;; ++at) {
    if (*at == '\\') {
      backslashes++;
      continue;
    }
    const uint64_t slash_count =
        *at == '"' || *at == '\0' ? backslashes * 2u : backslashes;
    for (uint64_t i = 0; i < slash_count; ++i) {
      if (!vkr_platform_command_append(command, capacity, cursor, '\\')) {
        return false_v;
      }
    }
    backslashes = 0u;
    if (*at == '\0') {
      break;
    }
    if (*at == '"' &&
        !vkr_platform_command_append(command, capacity, cursor, '\\')) {
      return false_v;
    }
    if (!vkr_platform_command_append(command, capacity, cursor, *at)) {
      return false_v;
    }
  }
  return vkr_platform_command_append(command, capacity, cursor, '"');
}

vkr_internal bool8_t vkr_platform_build_command(const char *executable,
                                                const char *const *arguments,
                                                uint32_t argument_count,
                                                char *command,
                                                uint64_t capacity) {
  if (!executable || argument_count > 63u || !command || capacity == 0u) {
    return false_v;
  }
  command[0] = '\0';
  uint64_t cursor = 0u;
  if (!vkr_platform_command_append_argument(command, capacity, &cursor,
                                            executable)) {
    return false_v;
  }
  for (uint32_t i = 0; i < argument_count; ++i) {
    if (!vkr_platform_command_append_argument(command, capacity, &cursor,
                                              arguments[i])) {
      return false_v;
    }
  }
  return true_v;
}

typedef struct VkrPlatformSavedEnvironment {
  char name[128];
  char value[4096];
  bool8_t existed;
} VkrPlatformSavedEnvironment;

vkr_internal bool8_t vkr_platform_environment_apply(
    const VkrPlatformEnvironmentVariable *variables, uint32_t count,
    VkrPlatformSavedEnvironment *saved) {
  if (count > 16u) {
    return false_v;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!variables[i].name ||
        string_length(variables[i].name) >= sizeof(saved[i].name)) {
      return false_v;
    }
    string_format(saved[i].name, sizeof(saved[i].name), "%s",
                  variables[i].name);
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA(
        variables[i].name, saved[i].value, sizeof(saved[i].value));
    if (length >= sizeof(saved[i].value)) {
      return false_v;
    }
    saved[i].existed = length > 0u || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!SetEnvironmentVariableA(variables[i].name, variables[i].value)) {
      for (uint32_t rollback = 0; rollback < i; ++rollback) {
        SetEnvironmentVariableA(saved[rollback].name,
                                saved[rollback].existed ? saved[rollback].value
                                                        : NULL);
      }
      return false_v;
    }
  }
  return true_v;
}

vkr_internal void
vkr_platform_environment_restore(const VkrPlatformSavedEnvironment *saved,
                                 uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    SetEnvironmentVariableA(saved[i].name,
                            saved[i].existed ? saved[i].value : NULL);
  }
}

bool8_t vkr_platform_process_run(const VkrPlatformProcessConfig *config,
                                 int32_t *out_exit_code,
                                 bool8_t *out_timed_out) {
  if (!config || !config->executable || !out_exit_code || !out_timed_out ||
      (config->environment_count > 0u && !config->environment) ||
      (config->argument_count > 0u && !config->arguments) ||
      config->environment_count > 16u) {
    return false_v;
  }
  char command[8192];
  if (!vkr_platform_build_command(config->executable, config->arguments,
                                  config->argument_count, command,
                                  sizeof(command))) {
    return false_v;
  }
  SECURITY_ATTRIBUTES security = {.nLength = sizeof(security),
                                  .bInheritHandle = TRUE};
  HANDLE output[2] = {GetStdHandle(STD_OUTPUT_HANDLE),
                      GetStdHandle(STD_ERROR_HANDLE)};
  const char *paths[2] = {config->stdout_path, config->stderr_path};
  for (uint32_t i = 0; i < ArrayCount(paths); ++i) {
    if (paths[i]) {
      output[i] =
          CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ, &security,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (output[i] == INVALID_HANDLE_VALUE) {
        for (uint32_t close_index = 0; close_index < i; ++close_index) {
          if (paths[close_index]) {
            CloseHandle(output[close_index]);
          }
        }
        return false_v;
      }
    }
  }
  STARTUPINFOA startup = {
      .cb = sizeof(startup),
      .dwFlags = STARTF_USESTDHANDLES,
      .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
      .hStdOutput = output[0],
      .hStdError = output[1],
  };
  PROCESS_INFORMATION process = {0};
  VkrPlatformSavedEnvironment saved[16] = {0};
  const bool8_t environment_applied = vkr_platform_environment_apply(
      config->environment, config->environment_count, saved);
  BOOL created = FALSE;
  if (environment_applied) {
    created = CreateProcessA(NULL, command, NULL, NULL, TRUE,
                             config->hidden ? CREATE_NO_WINDOW : 0u, NULL,
                             config->working_directory, &startup, &process);
    vkr_platform_environment_restore(saved, config->environment_count);
  }
  for (uint32_t i = 0; i < ArrayCount(paths); ++i) {
    if (paths[i]) {
      CloseHandle(output[i]);
    }
  }
  if (!created) {
    return false_v;
  }
  *out_exit_code = -1;
  *out_timed_out = false_v;
  const DWORD wait = WaitForSingleObject(
      process.hProcess, config->timeout_ms ? config->timeout_ms : INFINITE);
  if (wait == WAIT_TIMEOUT) {
    *out_timed_out = true_v;
    (void)TerminateProcess(process.hProcess, 1u);
    (void)WaitForSingleObject(process.hProcess, INFINITE);
  } else if (wait == WAIT_OBJECT_0) {
    DWORD exit_code = 0u;
    if (GetExitCodeProcess(process.hProcess, &exit_code)) {
      *out_exit_code = (int32_t)exit_code;
    }
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT;
}

bool8_t vkr_platform_process_capture(const char *executable,
                                     const char *const *arguments,
                                     uint32_t argument_count,
                                     const char *working_directory,
                                     char *out_output, uint64_t output_capacity,
                                     int32_t *out_exit_code) {
  if (!executable || !out_output || output_capacity == 0u || !out_exit_code ||
      (argument_count > 0u && !arguments)) {
    return false_v;
  }
  char command[8192];
  if (!vkr_platform_build_command(executable, arguments, argument_count,
                                  command, sizeof(command))) {
    return false_v;
  }
  SECURITY_ATTRIBUTES security = {.nLength = sizeof(security),
                                  .bInheritHandle = TRUE};
  HANDLE read_handle = NULL;
  HANDLE write_handle = NULL;
  if (!CreatePipe(&read_handle, &write_handle, &security, 0u) ||
      !SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0u)) {
    if (read_handle) {
      CloseHandle(read_handle);
    }
    if (write_handle) {
      CloseHandle(write_handle);
    }
    return false_v;
  }
  STARTUPINFOA startup = {
      .cb = sizeof(startup),
      .dwFlags = STARTF_USESTDHANDLES,
      .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
      .hStdOutput = write_handle,
      .hStdError = GetStdHandle(STD_ERROR_HANDLE),
  };
  PROCESS_INFORMATION process = {0};
  const BOOL created =
      CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                     working_directory, &startup, &process);
  CloseHandle(write_handle);
  if (!created) {
    CloseHandle(read_handle);
    return false_v;
  }
  uint64_t written = 0u;
  char buffer[1024];
  for (;;) {
    DWORD count = 0u;
    if (!ReadFile(read_handle, buffer, sizeof(buffer), &count, NULL) ||
        count == 0u) {
      break;
    }
    const uint64_t available = output_capacity - 1u - written;
    const uint64_t copy = Min((uint64_t)count, available);
    if (copy > 0u) {
      MemCopy(out_output + written, buffer, copy);
      written += copy;
    }
  }
  CloseHandle(read_handle);
  out_output[written] = '\0';
  (void)WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0u;
  const bool8_t exited = GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  *out_exit_code = exited ? (int32_t)exit_code : -1;
  return exited;
}

bool8_t vkr_platform_process_lock_acquire(const char *name,
                                          const char *lock_directory,
                                          VkrPlatformProcessLock *out_lock) {
  (void)lock_directory;
  if (!name || !out_lock) {
    return false_v;
  }
  MemZero(out_lock, sizeof(*out_lock));
  char mutex_name[256];
  const int32_t written =
      string_format(mutex_name, sizeof(mutex_name), "Local\\%s", name);
  if (written < 0 || (uint64_t)written >= sizeof(mutex_name)) {
    return false_v;
  }
  HANDLE mutex = CreateMutexA(NULL, TRUE, mutex_name);
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (mutex) {
      CloseHandle(mutex);
    }
    return false_v;
  }
  out_lock->opaque[0] = (uintptr_t)mutex;
  out_lock->acquired = true_v;
  return true_v;
}

void vkr_platform_process_lock_release(VkrPlatformProcessLock *lock) {
  if (!lock || !lock->acquired) {
    return;
  }
  HANDLE mutex = (HANDLE)lock->opaque[0];
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  MemZero(lock, sizeof(*lock));
}

void vkr_platform_console_write(const char *message, uint8_t colour) {
  HANDLE console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!console_handle || console_handle == INVALID_HANDLE_VALUE) {
    OutputDebugStringA(message);
    return;
  }
  // FATAL,ERROR,WARN,INFO,DEBUG,TRACE
  static uint8_t levels[6] = {64, 4, 6, 2, 1, 8};

  uint8_t safe_colour =
      (colour < 6) ? colour
                   : 3; // Default to INFO level (index 3) if out of bounds
  OutputDebugStringA(message);
  uint64_t length = strlen(message);
  DWORD number_written_var;
  DWORD console_mode;
  if (GetConsoleMode(console_handle, &console_mode)) {
    SetConsoleTextAttribute(console_handle, levels[safe_colour]);
    WriteConsoleA(console_handle, message, (DWORD)length, &number_written_var,
                  0);
    SetConsoleTextAttribute(console_handle, FOREGROUND_RED | FOREGROUND_GREEN |
                                                FOREGROUND_BLUE);
    return;
  }

  // WriteConsole fails when stdout is redirected. Keep diagnostics available
  // to automation and ordinary shell redirection without changing logger API.
  WriteFile(console_handle, message, (DWORD)length, &number_written_var, NULL);
}

void vkr_platform_shutdown() {
  if (high_res_timer_enabled) {
    timeEndPeriod(timer_resolution);
    high_res_timer_enabled = false;
  }
}
#endif
