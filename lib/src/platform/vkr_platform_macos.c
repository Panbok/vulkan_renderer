#include "vkr_platform.h"

#if defined(PLATFORM_APPLE)

#include "containers/str.h"

#include "core/vkr_atomic.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <dlfcn.h>
#include <libkern/OSThermalNotification.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>

static mach_timebase_info_data_t timebase_info;

bool8_t vkr_platform_executable_path(char *path, uint32_t capacity) {
  if (!path || capacity == 0u) {
    return false_v;
  }
  path[0] = '\0';
  char executable[PATH_MAX];
  uint32_t size = sizeof(executable);
  char canonical[PATH_MAX];
  if (_NSGetExecutablePath(executable, &size) != 0 ||
      !realpath(executable, canonical) || strlen(canonical) >= capacity) {
    return false_v;
  }
  MemCopy(path, canonical, strlen(canonical) + 1u);
  return true_v;
}

bool8_t vkr_platform_user_directory(VkrPlatformUserDirectory kind, char *path,
                                    uint32_t capacity) {
  if (!path || capacity == 0u) {
    return false_v;
  }
  path[0] = '\0';
  if (kind == VKR_PLATFORM_USER_TEMP) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) {
      tmp = "/tmp";
    }
    uint64_t length = strlen(tmp);
    while (length > 1u && tmp[length - 1u] == '/') {
      length--;
    }
    if (length >= capacity) {
      return false_v;
    }
    MemCopy(path, tmp, length);
    path[length] = '\0';
    return true_v;
  }
  const char *home = getenv("HOME");
  if (!home || !home[0]) {
    return false_v;
  }
  if (kind == VKR_PLATFORM_USER_HOME) {
    const uint64_t length = strlen(home);
    if (length >= capacity) {
      return false_v;
    }
    MemCopy(path, home, length + 1u);
    return true_v;
  }
  const char *leaf = kind == VKR_PLATFORM_USER_CACHE
                         ? "Library/Caches"
                         : "Library/Application Support";
  const int written = snprintf(path, capacity, "%s/%s", home, leaf);
  if (written < 0 || (uint32_t)written >= capacity) {
    path[0] = '\0';
    return false_v;
  }
  return true_v;
}

bool8_t vkr_platform_environment_variable(const char *name, char *value,
                                          uint32_t capacity) {
  if (!value || capacity == 0u) {
    return false_v;
  }
  value[0] = '\0';
  if (!name) {
    return false_v;
  }
  const char *variable = getenv(name);
  if (!variable) {
    return true_v;
  }
  const uint64_t length = strlen(variable);
  if (length >= capacity) {
    return false_v;
  }
  MemCopy(value, variable, length + 1u);
  return true_v;
}

bool8_t vkr_platform_environment_is_set(const char *name) {
  return name && getenv(name) ? true_v : false_v;
}

bool8_t vkr_platform_random_bytes(void *out, uint64_t size) {
  if (!out && size) {
    return false_v;
  }
  arc4random_buf(out, (size_t)size);
  return true_v;
}

bool8_t vkr_platform_file_manager_command(const char *path, bool8_t select,
                                          VkrPlatformFileManagerCommand *out) {
  if (!path || !out) {
    return false_v;
  }
  *out = (VkrPlatformFileManagerCommand){.executable = "/usr/bin/open"};
  const uint64_t length = strlen(path);
  if (length >= sizeof(out->argument_storage)) {
    return false_v;
  }
  MemCopy(out->argument_storage, path, length + 1u);
  if (select) {
    out->arguments[out->argument_count++] = "-R";
  }
  out->arguments[out->argument_count++] = out->argument_storage;
  return true_v;
}

static bool32_t timebase_initialized = false;

bool8_t vkr_platform_init() {
  // Every clock conversion divides by the timebase denominator.
  if (mach_timebase_info(&timebase_info) != KERN_SUCCESS ||
      timebase_info.denom == 0) {
    return false_v;
  }
  timebase_initialized = true;
  return true_v;
}

void *vkr_platform_mem_reserve(uint64_t size) {
  void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return result == MAP_FAILED ? NULL : result;
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
  const uint64_t large_page_size = 2 * 1024 * 1024;
  const uint64_t base_page_size = vkr_platform_get_page_size();
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

  if (ms <= 2) {
    float64_t start_time = vkr_platform_get_absolute_time();
    float64_t target_time = start_time + (ms * 0.001);

    while (vkr_platform_get_absolute_time() < target_time) {
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

  float64_t target_time = vkr_platform_get_absolute_time() + (2.0 * 0.001);
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
      .milliseconds = 0,
      .timezone_name = time_info->tm_zone,
  };
}

bool8_t vkr_platform_get_utc_time(VkrTime *out_time) {
  if (!out_time) {
    return false_v;
  }
  struct timespec now = {0};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return false_v;
  }
  struct tm utc = {0};
  if (!gmtime_r(&now.tv_sec, &utc)) {
    return false_v;
  }
  *out_time = (VkrTime){
      .seconds = utc.tm_sec,
      .minutes = utc.tm_min,
      .hours = utc.tm_hour,
      .day = utc.tm_mday,
      .month = utc.tm_mon,
      .year = utc.tm_year,
      .weekday = utc.tm_wday,
      .year_day = utc.tm_yday,
      .is_dst = utc.tm_isdst,
      .gmtoff = 0,
      .milliseconds = (int32_t)(now.tv_nsec / 1000000L),
      .timezone_name = "UTC",
  };
  return true_v;
}

uint32_t vkr_platform_get_process_id(void) { return (uint32_t)getpid(); }

bool8_t vkr_platform_get_system_info(VkrPlatformSystemInfo *out_info) {
  if (!out_info) {
    return false_v;
  }
  MemZero(out_info, sizeof(*out_info));
  struct utsname info = {0};
  if (uname(&info) == 0) {
    string_format(out_info->os, sizeof(out_info->os), "%s %s", info.sysname,
                  info.release);
    string_format(out_info->cpu, sizeof(out_info->cpu), "%s", info.machine);
  }
  size_t cpu_length = sizeof(out_info->cpu);
  if (sysctlbyname("machdep.cpu.brand_string", out_info->cpu, &cpu_length, NULL,
                   0) != 0 ||
      cpu_length <= 1u) {
    cpu_length = sizeof(out_info->cpu);
    (void)sysctlbyname("hw.model", out_info->cpu, &cpu_length, NULL, 0);
  }
  out_info->cpu[sizeof(out_info->cpu) - 1u] = '\0';
  out_info->process_priority = getpriority(PRIO_PROCESS, 0);
  return true_v;
}

bool8_t vkr_platform_get_process_resident_memory(uint64_t *out_bytes) {
  if (!out_bytes) {
    return false_v;
  }
  *out_bytes = 0u;
  mach_task_basic_info_data_t info = {0};
  mach_msg_type_number_t info_count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &info_count) != KERN_SUCCESS) {
    return false_v;
  }
  *out_bytes = info.resident_size;
  return true_v;
}

/* The notify(3) token of the thermal pressure level, registered by the first
   caller, and the last level read through it; -1 until then. Reading the
   level is an IPC (about 13 us on an M1 Pro), so a call reads it only after
   notify_check (shared memory, under 0.1 us) reports a change. */
vkr_global VkrAtomicInt32 g_thermal_token = -1;
vkr_global VkrAtomicInt32 g_thermal_level = -1;

/* The power source and low-power mode, read at most once a second because
   the power source query is an IPC to powerd. `g_power_bits` packs the four
   VKR_POWER_* flags; `g_power_sampled_ms` is when they were read. */
vkr_global VkrAtomicUint32 g_power_bits = 0u;
vkr_global VkrAtomicUint64 g_power_sampled_ms = 0u;

enum {
  VKR_POWER_SOURCE_VALID = 1u << 0,
  VKR_POWER_ON_BATTERY = 1u << 1,
  VKR_POWER_LOW_POWER_VALID = 1u << 2,
  VKR_POWER_LOW_POWER = 1u << 3,
};

vkr_internal bool8_t
vkr_platform_macos_thermal_level(VkrPlatformThermalLevel *out_level) {
  int32_t token =
      vkr_atomic_int32_load(&g_thermal_token, VKR_MEMORY_ORDER_ACQUIRE);
  if (token < 0) {
    int registered = -1;
    if (notify_register_check(kOSThermalNotificationPressureLevelName,
                              &registered) != NOTIFY_STATUS_OK) {
      return false_v;
    }
    int32_t expected = -1;
    if (vkr_atomic_int32_compare_exchange(
            &g_thermal_token, &expected, (int32_t)registered,
            VKR_MEMORY_ORDER_ACQ_REL, VKR_MEMORY_ORDER_ACQUIRE)) {
      token = (int32_t)registered;
    } else {
      /* Another thread registered first; keep its token. */
      notify_cancel(registered);
      token = expected;
    }
  }
  int changed = 0;
  int32_t level =
      vkr_atomic_int32_load(&g_thermal_level, VKR_MEMORY_ORDER_ACQUIRE);
  if (notify_check((int)token, &changed) != NOTIFY_STATUS_OK) {
    return false_v;
  }
  if (changed || level < 0) {
    uint64_t state = 0u;
    if (notify_get_state((int)token, &state) != NOTIFY_STATUS_OK) {
      return false_v;
    }
    switch (state) {
    case kOSThermalPressureLevelNominal:
      level = VKR_PLATFORM_THERMAL_NOMINAL;
      break;
    case kOSThermalPressureLevelModerate:
      level = VKR_PLATFORM_THERMAL_FAIR;
      break;
    case kOSThermalPressureLevelHeavy:
      level = VKR_PLATFORM_THERMAL_SERIOUS;
      break;
    default:
      /* Trapping and sleeping, and any later, hotter level. */
      level = VKR_PLATFORM_THERMAL_CRITICAL;
      break;
    }
    vkr_atomic_int32_store(&g_thermal_level, level, VKR_MEMORY_ORDER_RELEASE);
  }
  *out_level = (VkrPlatformThermalLevel)level;
  return true_v;
}

vkr_internal bool8_t
vkr_platform_macos_memory_pressure(VkrPlatformMemoryPressure *out_pressure) {
  int32_t level = 0;
  size_t length = sizeof(level);
  if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &length, NULL,
                   0) != 0) {
    return false_v;
  }
  /* The kernel reports 1 normal, 2 warn and 4 critical. */
  *out_pressure = level >= 4   ? VKR_PLATFORM_MEMORY_PRESSURE_CRITICAL
                  : level >= 2 ? VKR_PLATFORM_MEMORY_PRESSURE_WARN
                               : VKR_PLATFORM_MEMORY_PRESSURE_NORMAL;
  return true_v;
}

/* Reads the power source and low-power mode now. Low-power mode has only a
   Foundation API, called through the Objective-C runtime; a process that
   loaded no Foundation finds no class and reports it unavailable. */
vkr_internal uint32_t vkr_platform_macos_power_bits(void) {
  uint32_t bits = 0u;
  CFTypeRef info = IOPSCopyPowerSourcesInfo();
  if (info) {
    CFStringRef source = IOPSGetProvidingPowerSourceType(info);
    if (source) {
      bits |= VKR_POWER_SOURCE_VALID;
      if (CFStringCompare(source, CFSTR(kIOPSBatteryPowerValue), 0) ==
          kCFCompareEqualTo) {
        bits |= VKR_POWER_ON_BATTERY;
      }
    }
    CFRelease(info);
  }
  Class process_info_class = objc_getClass("NSProcessInfo");
  SEL low_power = sel_registerName("isLowPowerModeEnabled");
  if (process_info_class &&
      class_getInstanceMethod(process_info_class, low_power)) {
    id process_info = ((id(*)(Class, SEL))objc_msgSend)(
        process_info_class, sel_registerName("processInfo"));
    if (process_info) {
      bits |= VKR_POWER_LOW_POWER_VALID;
      if (((BOOL(*)(id, SEL))objc_msgSend)(process_info, low_power)) {
        bits |= VKR_POWER_LOW_POWER;
      }
    }
  }
  return bits;
}

bool8_t vkr_platform_get_telemetry(VkrPlatformTelemetry *out_telemetry) {
  if (!out_telemetry) {
    return false_v;
  }
  VkrPlatformTelemetry telemetry = {0};

  task_vm_info_data_t vm_info = {0};
  mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm_info,
                &vm_count) == KERN_SUCCESS) {
    telemetry.footprint_bytes = vm_info.phys_footprint;
    telemetry.footprint_valid = true_v;
  }

  struct rusage usage = {0};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    telemetry.cpu_user_ns = (uint64_t)usage.ru_utime.tv_sec * 1000000000ull +
                            (uint64_t)usage.ru_utime.tv_usec * 1000ull;
    telemetry.cpu_system_ns = (uint64_t)usage.ru_stime.tv_sec * 1000000000ull +
                              (uint64_t)usage.ru_stime.tv_usec * 1000ull;
    telemetry.cpu_time_valid = true_v;
    telemetry.major_page_faults = (uint64_t)usage.ru_majflt;
    telemetry.major_page_faults_valid = true_v;
  }

  telemetry.thermal_valid =
      vkr_platform_macos_thermal_level(&telemetry.thermal_level);
  telemetry.memory_pressure_valid =
      vkr_platform_macos_memory_pressure(&telemetry.memory_pressure);

  const uint64_t now_ms = (uint64_t)(vkr_platform_get_absolute_time() * 1000.0);
  const uint64_t sampled_ms =
      vkr_atomic_uint64_load(&g_power_sampled_ms, VKR_MEMORY_ORDER_ACQUIRE);
  uint32_t power =
      vkr_atomic_uint32_load(&g_power_bits, VKR_MEMORY_ORDER_ACQUIRE);
  if (sampled_ms == 0u || now_ms - sampled_ms >= 1000u) {
    /* Two threads may both read; either result is current. */
    power = vkr_platform_macos_power_bits();
    vkr_atomic_uint32_store(&g_power_bits, power, VKR_MEMORY_ORDER_RELEASE);
    vkr_atomic_uint64_store(&g_power_sampled_ms, Max(now_ms, 1u),
                            VKR_MEMORY_ORDER_RELEASE);
  }
  telemetry.power_valid = (power & VKR_POWER_SOURCE_VALID) != 0u;
  telemetry.on_battery = (power & VKR_POWER_ON_BATTERY) != 0u;
  telemetry.low_power_valid = (power & VKR_POWER_LOW_POWER_VALID) != 0u;
  telemetry.low_power = (power & VKR_POWER_LOW_POWER) != 0u;

  *out_telemetry = telemetry;
  return telemetry.footprint_valid || telemetry.cpu_time_valid ||
         telemetry.thermal_valid || telemetry.memory_pressure_valid ||
         telemetry.power_valid || telemetry.low_power_valid;
}

void vkr_platform_exit_immediate(int32_t exit_code) { _Exit(exit_code); }

void vkr_platform_stdout_write(const char *message) {
  if (message) {
    fputs(message, stdout);
    fflush(stdout);
  }
}

void vkr_platform_stderr_write(const char *message) {
  if (message) {
    fputs(message, stderr);
    fflush(stderr);
  }
}

vkr_internal float64_t vkr_platform_monotonic_seconds(void) {
  struct timespec now = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0.0;
  }
  return (float64_t)now.tv_sec + (float64_t)now.tv_nsec / 1000000000.0;
}

vkr_internal void
vkr_platform_process_child_setup(const VkrPlatformProcessConfig *config) {
  if (config->terminate_process_tree && setpgid(0, 0) != 0) {
    _exit(127);
  }
  if (config->working_directory && chdir(config->working_directory) != 0) {
    _exit(127);
  }
  const char *const paths[2] = {config->stdout_path, config->stderr_path};
  const int streams[2] = {STDOUT_FILENO, STDERR_FILENO};
  for (uint32_t i = 0; i < ArrayCount(paths); ++i) {
    if (!paths[i]) {
      continue;
    }
    const int descriptor =
        open(paths[i],
             O_CREAT | O_WRONLY | (config->append_output ? O_APPEND : O_TRUNC),
             0644);
    if (descriptor < 0 || dup2(descriptor, streams[i]) < 0) {
      if (descriptor >= 0) {
        close(descriptor);
      }
      _exit(127);
    }
    close(descriptor);
  }
  for (uint32_t i = 0; i < config->environment_count; ++i) {
    const VkrPlatformEnvironmentVariable *variable = &config->environment[i];
    if (!variable->name ||
        (variable->value ? setenv(variable->name, variable->value, 1)
                         : unsetenv(variable->name)) != 0) {
      _exit(127);
    }
  }
}

vkr_internal bool8_t vkr_platform_process_wait_nonblocking(
    pid_t pid, int *status, bool8_t *out_reaped) {
  if (*out_reaped) {
    return true_v;
  }
  for (;;) {
    const pid_t waited = waitpid(pid, status, WNOHANG);
    if (waited == pid) {
      *out_reaped = true_v;
      return true_v;
    }
    if (waited == 0) {
      return true_v;
    }
    if (errno != EINTR) {
      return false_v;
    }
  }
}

vkr_internal bool8_t vkr_platform_process_wait_blocking(pid_t pid, int *status,
                                                        bool8_t *out_reaped) {
  if (*out_reaped) {
    return true_v;
  }
  while (waitpid(pid, status, 0) < 0) {
    if (errno != EINTR) {
      return false_v;
    }
  }
  *out_reaped = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_platform_process_group_exists(pid_t group) {
  return kill(-group, 0) == 0 || errno == EPERM;
}

vkr_internal bool8_t vkr_platform_process_signal(pid_t pid,
                                                 bool8_t process_tree,
                                                 int signal) {
  if (kill(process_tree ? -pid : pid, signal) == 0 || errno == ESRCH) {
    return true_v;
  }
  return false_v;
}

/* Process supervision polls need no wake-up precision; the trailing spin of
   vkr_platform_sleep would hold a core for 2 ms of every 10 ms poll for as
   long as the child runs. */
vkr_internal void vkr_platform_poll_sleep(uint64_t ms) {
  struct timespec duration = {
      .tv_sec = (time_t)(ms / 1000u),
      .tv_nsec = (long)((ms % 1000u) * 1000000u),
  };
  nanosleep(&duration, NULL);
}

vkr_internal bool8_t vkr_platform_process_stop(pid_t pid, bool8_t process_tree,
                                               uint32_t grace_ms, int *status,
                                               bool8_t *out_reaped) {
  if (!vkr_platform_process_signal(pid, process_tree, SIGTERM)) {
    return false_v;
  }
  const float64_t deadline =
      vkr_platform_monotonic_seconds() + (float64_t)grace_ms / 1000.0;
  for (;;) {
    if (!vkr_platform_process_wait_nonblocking(pid, status, out_reaped)) {
      return false_v;
    }
    if ((process_tree && !vkr_platform_process_group_exists(pid)) ||
        (!process_tree && *out_reaped)) {
      return true_v;
    }
    if (vkr_platform_monotonic_seconds() >= deadline) {
      break;
    }
    vkr_platform_poll_sleep(10u);
  }
  if (!vkr_platform_process_signal(pid, process_tree, SIGKILL)) {
    return false_v;
  }
  return vkr_platform_process_wait_blocking(pid, status, out_reaped);
}

bool8_t vkr_platform_process_run(const VkrPlatformProcessConfig *config,
                                 int32_t *out_exit_code,
                                 bool8_t *out_timed_out) {
  if (!config || !config->executable || !out_exit_code || !out_timed_out ||
      config->argument_count > 63u ||
      (config->argument_count > 0u && !config->arguments) ||
      (config->environment_count > 0u && !config->environment)) {
    return false_v;
  }
  *out_exit_code = -1;
  *out_timed_out = false_v;
  const pid_t pid = fork();
  if (pid < 0) {
    return false_v;
  }
  if (pid == 0) {
    vkr_platform_process_child_setup(config);
    char *arguments[65];
    arguments[0] = (char *)config->executable;
    for (uint32_t i = 0; i < config->argument_count; ++i) {
      arguments[i + 1u] = (char *)config->arguments[i];
    }
    arguments[config->argument_count + 1u] = NULL;
    execvp(config->executable, arguments);
    _exit(127);
  }

  if (config->terminate_process_tree && setpgid(pid, pid) != 0 &&
      getpgid(pid) != pid) {
    (void)vkr_platform_process_signal(pid, false_v, SIGKILL);
    int failed_status = 0;
    bool8_t failed_reaped = false_v;
    (void)vkr_platform_process_wait_blocking(pid, &failed_status,
                                             &failed_reaped);
    return false_v;
  }

  int status = 0;
  bool8_t reaped = false_v;
  if (config->timeout_ms == 0u && !config->is_cancelled) {
    if (!vkr_platform_process_wait_blocking(pid, &status, &reaped)) {
      return false_v;
    }
  } else {
    const float64_t started = vkr_platform_monotonic_seconds();
    for (;;) {
      if (!vkr_platform_process_wait_nonblocking(pid, &status, &reaped)) {
        return false_v;
      }
      if (reaped) {
        break;
      }
      const bool8_t cancelled =
          config->is_cancelled && config->is_cancelled(config->cancel_context);
      const bool8_t timed_out =
          config->timeout_ms > 0u &&
          (vkr_platform_monotonic_seconds() - started) * 1000.0 >=
              config->timeout_ms;
      if (cancelled || timed_out) {
        *out_timed_out = timed_out;
        if (!vkr_platform_process_stop(pid, config->terminate_process_tree,
                                       config->termination_grace_ms, &status,
                                       &reaped)) {
          return false_v;
        }
        break;
      }
      vkr_platform_poll_sleep(10u);
    }
  }
  if (config->terminate_process_tree &&
      vkr_platform_process_group_exists(pid) &&
      !vkr_platform_process_stop(pid, true_v, config->termination_grace_ms,
                                 &status, &reaped)) {
    return false_v;
  }
  if (WIFEXITED(status)) {
    *out_exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    *out_exit_code = 128 + WTERMSIG(status);
  }
  return true_v;
}

bool8_t vkr_platform_process_capture(const char *executable,
                                     const char *const *arguments,
                                     uint32_t argument_count,
                                     const char *working_directory,
                                     char *out_output, uint64_t output_capacity,
                                     int32_t *out_exit_code) {
  if (!executable || argument_count > 63u || !out_output ||
      output_capacity == 0u || !out_exit_code ||
      (argument_count > 0u && !arguments)) {
    return false_v;
  }
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    return false_v;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return false_v;
  }
  if (pid == 0) {
    if ((working_directory && chdir(working_directory) != 0) ||
        dup2(descriptors[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    close(descriptors[0]);
    close(descriptors[1]);
    char *process_arguments[65];
    process_arguments[0] = (char *)executable;
    for (uint32_t i = 0; i < argument_count; ++i) {
      process_arguments[i + 1u] = (char *)arguments[i];
    }
    process_arguments[argument_count + 1u] = NULL;
    execvp(executable, process_arguments);
    _exit(127);
  }
  close(descriptors[1]);
  uint64_t written = 0u;
  char buffer[1024];
  for (;;) {
    const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    const uint64_t available = output_capacity - 1u - written;
    const uint64_t copy = Min((uint64_t)count, available);
    if (copy > 0u) {
      MemCopy(out_output + written, buffer, copy);
      written += copy;
    }
  }
  close(descriptors[0]);
  out_output[written] = '\0';
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return false_v;
    }
  }
  *out_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return true_v;
}

bool8_t vkr_platform_process_lock_acquire(const char *name,
                                          const char *lock_directory,
                                          VkrPlatformProcessLock *out_lock) {
  if (!name || !lock_directory || !out_lock) {
    return false_v;
  }
  MemZero(out_lock, sizeof(*out_lock));
  char path[4096];
  const int32_t written =
      string_format(path, sizeof(path), "%s/%s.lock", lock_directory, name);
  if (written < 0 || (uint64_t)written >= sizeof(path)) {
    return false_v;
  }
  /* Close-on-exec: a launched process must not hold the lock after its
     owner exits. */
  const int descriptor = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (descriptor < 0 || flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    if (descriptor >= 0) {
      close(descriptor);
    }
    return false_v;
  }
  out_lock->opaque[0] = (uintptr_t)(descriptor + 1);
  out_lock->acquired = true_v;
  return true_v;
}

void vkr_platform_process_lock_release(VkrPlatformProcessLock *lock) {
  if (!lock || !lock->acquired) {
    return;
  }
  const int descriptor = (int)lock->opaque[0] - 1;
  (void)flock(descriptor, LOCK_UN);
  close(descriptor);
  MemZero(lock, sizeof(*lock));
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

bool8_t vkr_platform_library_open(const char *path, VkrPlatformLibrary *out,
                                  char *error, uint32_t error_capacity) {
  *out = (VkrPlatformLibrary){0};
  out->handle = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
  if (!out->handle) {
    const char *message = path ? dlerror() : "no library path";
    if (error && error_capacity) {
      snprintf(error, error_capacity, "%s",
               message ? message : "dlopen failed");
    }
    return false_v;
  }
  return true_v;
}

void *vkr_platform_library_symbol(const VkrPlatformLibrary *library,
                                  const char *name) {
  return library && library->handle ? dlsym(library->handle, name) : NULL;
}

void vkr_platform_library_close(VkrPlatformLibrary *library) {
  if (library && library->handle) {
    dlclose(library->handle);
    library->handle = NULL;
  }
}

#endif
