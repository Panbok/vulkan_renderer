#pragma once

#include "defines.h"

typedef struct VkrTime {
  int32_t seconds;  // seconds after the minute [0-60]
  int32_t minutes;  // minutes after the hour [0-59]
  int32_t hours;    // hours since midnight [0-23]
  int32_t day;      // day of the month [1-31]
  int32_t month;    // months since January [0-11]
  int32_t year;     // years since 1900
  int32_t weekday;  // days since Sunday [0-6]
  int32_t year_day; // days since January 1 [0-365]
  int32_t is_dst;   // Daylight Savings Time flag
  int32_t gmtoff;   // offset from UTC in seconds
  int32_t milliseconds;
  char *timezone_name;
} VkrTime;

typedef struct VkrPlatformSystemInfo {
  char os[128];
  char cpu[128];
  int32_t process_priority;
} VkrPlatformSystemInfo;

typedef struct VkrPlatformEnvironmentVariable {
  const char *name;
  const char *value;
} VkrPlatformEnvironmentVariable;

typedef struct VkrPlatformProcessConfig {
  const char *executable;
  /** Borrowed arguments excluding argv[0]; `executable` becomes argv[0]. */
  const char *const *arguments;
  uint32_t argument_count;
  /** Borrowed optional paths; redirected output files are truncated. */
  /* Windows cwd must fit MAX_PATH, directly or via an existing same-directory
   * short name. Otherwise launch fails with ERROR_FILENAME_EXCED_RANGE; long
   * executable and log paths remain supported independently. */
  const char *working_directory;
  const char *stdout_path;
  const char *stderr_path;
  /** Appends to redirected output files instead of truncating them, so one
   * log can collect several consecutive processes while it is being read. */
  bool8_t append_output;
  /** Borrowed overrides applied to the inherited environment for the child. */
  const VkrPlatformEnvironmentVariable *environment;
  uint32_t environment_count;
  uint32_t timeout_ms;           /**< Zero waits without a timeout. */
  uint32_t termination_grace_ms; /**< Grace before forced termination. */
  bool8_t hidden;
  /** Makes this child the root of an owned process tree. The caller stops any
   * remaining descendants before process_run returns. Nested default calls
   * inherit the root's ownership without creating another tree. */
  bool8_t terminate_process_tree;
  /** Optional cancellation query on the waiting thread. Context is borrowed
   * through process_run; the callback synchronizes its own state. */
  bool8_t (*is_cancelled)(void *context);
  void *cancel_context;
} VkrPlatformProcessConfig;

/** Opaque storage for one non-recursive cross-process lock. */
typedef struct VkrPlatformProcessLock {
  uintptr_t opaque[2];
  bool8_t acquired;
} VkrPlatformProcessLock;

/** Reserve address space; returns NULL on either platform if reservation fails.
 */
void *vkr_platform_mem_reserve(uint64_t size);

bool32_t vkr_platform_mem_commit(void *ptr, uint64_t size);

void vkr_platform_mem_decommit(void *ptr, uint64_t size);

void vkr_platform_mem_release(void *ptr, uint64_t size);

uint64_t vkr_platform_get_page_size();

uint64_t vkr_platform_get_large_page_size();

uint32_t vkr_platform_get_logical_core_count(void);

void vkr_platform_sleep(uint64_t milliseconds);

float64_t vkr_platform_get_absolute_time();
/** The same monotonic clock as vkr_platform_get_absolute_time, in integer
 * nanoseconds; all threads share it. */
uint64_t vkr_platform_get_monotonic_ns(void);
/** Converts a raw reading of the counter behind vkr_platform_get_monotonic_ns
 * (QueryPerformanceCounter ticks on Windows, mach_absolute_time on macOS) to
 * that function's nanoseconds, such as a GPU driver's calibrated host stamp. */
uint64_t vkr_platform_monotonic_ns_from_counter(uint64_t counter);

VkrTime vkr_platform_get_local_time();

/** Returns wall-clock UTC with millisecond precision. */
bool8_t vkr_platform_get_utc_time(VkrTime *out_time);

/** Whole seconds since 1970-01-01 UTC. */
int64_t vkr_platform_get_unix_time(void);

uint32_t vkr_platform_get_process_id(void);
/** Canonical UTF-8 executable path, including a terminator, independent of CWD.
 */
bool8_t vkr_platform_executable_path(char *path, uint32_t capacity);

/** Per-user base directories an installed program keeps its state below. */
typedef enum VkrPlatformUserDirectory {
  /** %APPDATA% on Windows; ~/Library/Application Support on macOS. */
  VKR_PLATFORM_USER_SETTINGS = 0,
  /** %LOCALAPPDATA% on Windows; ~/Library/Caches on macOS. */
  VKR_PLATFORM_USER_CACHE,
  /** GetTempPathW (%TMP% or %TEMP%) on Windows; $TMPDIR or /tmp on macOS. */
  VKR_PLATFORM_USER_TEMP,
  /** %USERPROFILE% on Windows; $HOME on macOS. */
  VKR_PLATFORM_USER_HOME,
} VkrPlatformUserDirectory;

/** UTF-8 path of a per-user base directory with '/' separators and no
 * trailing separator; false when the environment does not name one or it
 * does not fit. The directory is not created. */
bool8_t vkr_platform_user_directory(VkrPlatformUserDirectory kind, char *path,
                                    uint32_t capacity);

/** Copies the UTF-8 value of the environment variable `name`, such as a
 * path, into `value`; an unset variable leaves `value` empty. False, with
 * `value` empty, when the value does not fit `capacity`. Windows reads the
 * wide environment and also rejects a value that is not valid UTF-16: the
 * narrow CRT copy (getenv) is in the active code page, which turns a
 * non-ASCII path into another one. POSIX values are copied as bytes. */
bool8_t vkr_platform_environment_variable(const char *name, char *value,
                                          uint32_t capacity);

/** True when the variable `name` is set, to any value including the empty
 * string; a caller that gives "set but empty" its own meaning asks this. */
bool8_t vkr_platform_environment_is_set(const char *name);

/** Sets the variable `name` to `value` (UTF-8) in this process's environment,
 * which libraries loaded afterwards, such as a graphics loader reading a
 * layer switch, see. Not thread safe against concurrent readers. */
bool8_t vkr_platform_environment_set(const char *name, const char *value);

/** True when the variable `name` is set to a value other than empty or "0".
 * A value longer than 63 bytes is not a flag and reads as false. */
vkr_internal INLINE bool8_t vkr_platform_environment_flag(const char *name) {
  char value[64];
  if (!name || !vkr_platform_environment_variable(name, value, sizeof(value))) {
    return false_v;
  }
  return value[0] != '\0' && !(value[0] == '0' && value[1] == '\0') ? true_v
                                                                    : false_v;
}

bool8_t vkr_platform_get_system_info(VkrPlatformSystemInfo *out_info);

/**
 * Returns resident process memory in bytes: macOS resident pages or the Windows
 * working set. This includes shared mappings; it is not allocator accounting
 * or dedicated GPU usage. Clears `out_bytes` and returns false on failure.
 */
bool8_t vkr_platform_get_process_resident_memory(uint64_t *out_bytes);

/** System thermal pressure, normalized across platforms. */
typedef enum VkrPlatformThermalLevel {
  VKR_PLATFORM_THERMAL_NOMINAL = 0,
  VKR_PLATFORM_THERMAL_FAIR = 1,
  /** The OS throttles the CPU and GPU; timings are not comparable. */
  VKR_PLATFORM_THERMAL_SERIOUS = 2,
  VKR_PLATFORM_THERMAL_CRITICAL = 3,
} VkrPlatformThermalLevel;

/** System memory pressure, normalized across platforms. */
typedef enum VkrPlatformMemoryPressure {
  VKR_PLATFORM_MEMORY_PRESSURE_NORMAL = 0,
  /** The OS compresses or pages memory out. */
  VKR_PLATFORM_MEMORY_PRESSURE_WARN = 1,
  VKR_PLATFORM_MEMORY_PRESSURE_CRITICAL = 2,
} VkrPlatformMemoryPressure;

/**
 * Process and system signals that explain a frame's time. Each value has a
 * validity flag: a platform without a source leaves it false, so a consumer
 * reports the value unavailable instead of zero.
 */
typedef struct VkrPlatformTelemetry {
  /** Memory the OS charges the process and limits it by: macOS
   * `phys_footprint`, Windows private bytes. */
  uint64_t footprint_bytes;
  /** CPU time of every thread of the process since it started. */
  uint64_t cpu_user_ns;
  uint64_t cpu_system_ns;
  /** Page faults that read from disk since the process started. */
  uint64_t major_page_faults;
  VkrPlatformThermalLevel thermal_level;
  VkrPlatformMemoryPressure memory_pressure;
  bool8_t on_battery;
  /** The OS's low-power mode, which lowers CPU and GPU clocks. */
  bool8_t low_power;
  bool8_t footprint_valid;
  bool8_t cpu_time_valid;
  bool8_t major_page_faults_valid;
  bool8_t thermal_valid;
  bool8_t memory_pressure_valid;
  bool8_t power_valid;
  bool8_t low_power_valid;
} VkrPlatformTelemetry;

/**
 * Samples every telemetry signal the platform has. Safe from any thread. On
 * an M1 Pro a call costs about 2.5 us; the thermal level is re-read only when
 * it changes, and the power source (about 90 us) at most once a second.
 * Returns false when no signal is available.
 */
bool8_t vkr_platform_get_telemetry(VkrPlatformTelemetry *out_telemetry);

/** Bytes of a GPU adapter's locally unique identifier: Windows `LUID`, the
 * layout of Vulkan's `deviceLUID`. */
#define VKR_PLATFORM_ADAPTER_LUID_SIZE 8u

/**
 * The OS's view of one GPU adapter, beside the graphics API's own figures.
 * Each value has a validity flag: a platform or adapter without a source
 * leaves it false, so a consumer reports the value unavailable instead of
 * zero.
 */
typedef struct VkrPlatformGpuTelemetry {
  /** Windows DXGI local segment (`QueryVideoMemoryInfo`): the budget the OS
   * gives this process on the adapter and the process's usage of it. They
   * count the same memory as VK_EXT_memory_budget's device-local heaps, as
   * the OS's video memory manager sees it. */
  uint64_t memory_budget_bytes;
  uint64_t memory_usage_bytes;
  /** The busiest GPU engine type's utilization by every other process on the
   * adapter, 0 to 100 percent, sampled about once a second. */
  uint32_t other_process_util_percent;
  bool8_t memory_valid;
  bool8_t other_process_util_valid;
} VkrPlatformGpuTelemetry;

/** Samples one GPU adapter for VkrPlatformGpuTelemetry; opaque. */
typedef struct s_VkrPlatformGpuMonitor *VkrPlatformGpuMonitor;
typedef struct VkrAllocator VkrAllocator;

/**
 * Opens a monitor of the adapter `adapter_luid` names
 * (VKR_PLATFORM_ADAPTER_LUID_SIZE bytes), or of every adapter when it is
 * NULL; without an adapter the memory values stay unavailable. The monitor
 * and, at the first sample, its sampler thread come from `allocator`.
 * Returns false where the platform has no source (macOS) or on failure.
 */
bool8_t vkr_platform_gpu_monitor_create(VkrAllocator *allocator,
                                        const uint8_t *adapter_luid,
                                        VkrPlatformGpuMonitor *out_monitor);

/**
 * Reads the adapter's memory now and the other processes' GPU use from the
 * last background sample, an atomic load. The first call starts a sampler
 * thread at about 1 Hz, because one collection of the per-process engine
 * counters takes milliseconds; call from the thread that may allocate from
 * the monitor's allocator. A NULL monitor reports nothing. Returns false
 * when no value is available.
 */
bool8_t vkr_platform_gpu_monitor_sample(VkrPlatformGpuMonitor monitor,
                                        VkrPlatformGpuTelemetry *out_telemetry);

/** Stops and joins the sampler thread and releases the monitor; NULL-safe. */
void vkr_platform_gpu_monitor_destroy(VkrAllocator *allocator,
                                      VkrPlatformGpuMonitor *monitor);

/** True when the variable `name` is set to exactly `expected`, such as an
 * off switch spelled "0". A value longer than 63 bytes never matches. */
vkr_internal INLINE bool8_t
vkr_platform_environment_equals(const char *name, const char *expected) {
  char value[64];
  if (!name || !expected ||
      !vkr_platform_environment_variable(name, value, sizeof(value))) {
    return false_v;
  }
  uint64_t i = 0u;
  while (value[i] != '\0' && value[i] == expected[i]) {
    ++i;
  }
  return value[i] == expected[i] ? true_v : false_v;
}

/** Ends the process at once with `exit_code`: no exit handlers run and no
 * buffered C streams are flushed. For a state where normal teardown is unsafe,
 * such as GPU work whose completion cannot be proven. */
void vkr_platform_exit_immediate(int32_t exit_code);

/** Raw, uncoloured process output suitable for machine-readable tools. */
void vkr_platform_stdout_write(const char *message);
void vkr_platform_stderr_write(const char *message);

/* printf-formatted forms of the writers above. A message longer than 4 KiB
 * is truncated there. */
#if defined(__clang__) || defined(__GNUC__)
#define VKR_PLATFORM_PRINTF(format_index, args_index)                          \
  __attribute__((format(printf, format_index, args_index)))
#else
#define VKR_PLATFORM_PRINTF(format_index, args_index)
#endif
void vkr_platform_stdout_writef(const char *format, ...)
    VKR_PLATFORM_PRINTF(1, 2);
void vkr_platform_stderr_writef(const char *format, ...)
    VKR_PLATFORM_PRINTF(1, 2);

/**
 * Launches one process, redirects output, enforces timeout, and waits.
 *
 * All config storage is borrowed only for this blocking call. A true return
 * means the platform launch/wait operations succeeded; the child result is in
 * `out_exit_code`. On timeout, `out_timed_out` is true and the child is
 * terminated. When `terminate_process_tree` is true, process_run owns the
 * launched process and descendants that remain in its process group or Job;
 * it stops remaining descendants before returning. POSIX first allows
 * `termination_grace_ms`; Windows termination is immediate. A null environment
 * value removes that variable in the child.
 */
bool8_t vkr_platform_process_run(const VkrPlatformProcessConfig *config,
                                 int32_t *out_exit_code,
                                 bool8_t *out_timed_out);

/**
 * Runs one process without a timeout and captures stdout into caller storage.
 * The output is always null-terminated; excess bytes are drained and omitted.
 */
bool8_t vkr_platform_process_capture(const char *executable,
                                     const char *const *arguments,
                                     uint32_t argument_count,
                                     const char *working_directory,
                                     char *out_output, uint64_t output_capacity,
                                     int32_t *out_exit_code);

/**
 * Attempts one non-blocking named lock shared by processes on this machine.
 * `lock_directory` owns the lock file on POSIX and is ignored on Windows.
 * Release `out_lock` exactly once only after a successful acquisition.
 */
bool8_t vkr_platform_process_lock_acquire(const char *name,
                                          const char *lock_directory,
                                          VkrPlatformProcessLock *out_lock);
void vkr_platform_process_lock_release(VkrPlatformProcessLock *lock);

/** Fills `out` with `size` bytes from the system's cryptographic random
 * source; false when the source fails. */
bool8_t vkr_platform_random_bytes(void *out, uint64_t size);

/** The system file manager's command for `path`: it opens a folder or file,
 * or with `select` shows the item selected in its folder. Run it with
 * vkr_platform_process_run; the arguments point into this record. */
typedef struct VkrPlatformFileManagerCommand {
  const char *executable;
  const char *arguments[2];
  uint32_t argument_count;
  char argument_storage[4112];
} VkrPlatformFileManagerCommand;

/** False when `path` does not fit the record. */
bool8_t vkr_platform_file_manager_command(const char *path, bool8_t select,
                                          VkrPlatformFileManagerCommand *out);

/** Clipboard UTF-8 access on the UI thread, only for an explicit user action.
 * Read returns a complete-codepoint prefix fitting capacity, including NUL.
 * No clipboard-owned pointer escapes either call. */
bool8_t vkr_platform_clipboard_read_text(uint8_t *buffer, uint32_t capacity,
                                         uint32_t *out_length);
bool8_t vkr_platform_clipboard_write_text(const uint8_t *text, uint32_t length);

/** A shared library loaded into the process (dlopen, LoadLibraryW). Open
 * binds every symbol immediately; `error` receives the loader's message on
 * failure. Symbols stay valid until close. */
typedef struct VkrPlatformLibrary {
  void *handle;
} VkrPlatformLibrary;
bool8_t vkr_platform_library_open(const char *path, VkrPlatformLibrary *out,
                                  char *error, uint32_t error_capacity);
void *vkr_platform_library_symbol(const VkrPlatformLibrary *library,
                                  const char *name);
void vkr_platform_library_close(VkrPlatformLibrary *library);

void vkr_platform_console_write(const char *message, uint8_t colour);

bool8_t vkr_platform_init();

void vkr_platform_shutdown();
