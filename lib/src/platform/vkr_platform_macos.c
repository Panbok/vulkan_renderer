#include "vkr_platform.h"

#if defined(PLATFORM_APPLE)

#include "containers/str.h"

#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>

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
  if (config->working_directory && chdir(config->working_directory) != 0) {
    _exit(127);
  }
  const char *const paths[2] = {config->stdout_path, config->stderr_path};
  const int streams[2] = {STDOUT_FILENO, STDERR_FILENO};
  for (uint32_t i = 0; i < ArrayCount(paths); ++i) {
    if (!paths[i]) {
      continue;
    }
    const int descriptor = open(paths[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
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

  int status = 0;
  if (config->timeout_ms == 0u) {
    while (waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) {
        return false_v;
      }
    }
  } else {
    const float64_t started = vkr_platform_monotonic_seconds();
    for (;;) {
      const pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        break;
      }
      if (waited < 0 && errno != EINTR) {
        return false_v;
      }
      if ((vkr_platform_monotonic_seconds() - started) * 1000.0 >=
          config->timeout_ms) {
        *out_timed_out = true_v;
        (void)kill(pid, SIGTERM);
        const float64_t grace_started = vkr_platform_monotonic_seconds();
        while (waitpid(pid, &status, WNOHANG) == 0 &&
               (vkr_platform_monotonic_seconds() - grace_started) * 1000.0 <
                   config->termination_grace_ms) {
          vkr_platform_sleep(10u);
        }
        if (waitpid(pid, &status, WNOHANG) == 0) {
          (void)kill(pid, SIGKILL);
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return true_v;
      }
      vkr_platform_sleep(10u);
    }
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
  const int descriptor = open(path, O_CREAT | O_RDWR, 0644);
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
#endif
