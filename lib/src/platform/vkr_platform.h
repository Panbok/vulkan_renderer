#pragma once

#include "defines.h"
#include "vkr_pch.h"

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
  const char *working_directory;
  const char *stdout_path;
  const char *stderr_path;
  /** Borrowed overrides applied to the inherited environment for the child. */
  const VkrPlatformEnvironmentVariable *environment;
  uint32_t environment_count;
  uint32_t timeout_ms;           /**< Zero waits without a timeout. */
  uint32_t termination_grace_ms; /**< Grace before forced termination. */
  bool8_t hidden;
} VkrPlatformProcessConfig;

/** Opaque storage for one non-recursive cross-process lock. */
typedef struct VkrPlatformProcessLock {
  uintptr_t opaque[2];
  bool8_t acquired;
} VkrPlatformProcessLock;

void *vkr_platform_mem_reserve(uint64_t size);

bool32_t vkr_platform_mem_commit(void *ptr, uint64_t size);

void vkr_platform_mem_decommit(void *ptr, uint64_t size);

void vkr_platform_mem_release(void *ptr, uint64_t size);

uint64_t vkr_platform_get_page_size();

uint64_t vkr_platform_get_large_page_size();

uint32_t vkr_platform_get_logical_core_count(void);

void vkr_platform_sleep(uint64_t milliseconds);

float64_t vkr_platform_get_absolute_time();

VkrTime vkr_platform_get_local_time();

/** Returns wall-clock UTC with millisecond precision. */
bool8_t vkr_platform_get_utc_time(VkrTime *out_time);

uint32_t vkr_platform_get_process_id(void);

bool8_t vkr_platform_get_system_info(VkrPlatformSystemInfo *out_info);

/** Raw, uncoloured process output suitable for machine-readable tools. */
void vkr_platform_stdout_write(const char *message);
void vkr_platform_stderr_write(const char *message);

/**
 * Launches one process, redirects output, enforces timeout, and waits.
 *
 * All config storage is borrowed only for this blocking call. A true return
 * means the platform launch/wait operations succeeded; the child result is in
 * `out_exit_code`. On timeout, `out_timed_out` is true and the child is
 * terminated. POSIX first allows `termination_grace_ms`; Windows termination
 * is immediate. A null environment value removes that variable in the child.
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

void vkr_platform_console_write(const char *message, uint8_t colour);

bool8_t vkr_platform_init();

void vkr_platform_shutdown();
