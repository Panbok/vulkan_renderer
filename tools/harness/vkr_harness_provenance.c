#include "vkr_harness_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

#ifndef VKR_HARNESS_BUILD_TYPE
#define VKR_HARNESS_BUILD_TYPE "unknown"
#endif
#ifndef VKR_HARNESS_COMPILER
#define VKR_HARNESS_COMPILER "unknown"
#endif

typedef enum VkrHarnessGitQuery {
  VKR_HARNESS_GIT_HEAD,
  VKR_HARNESS_GIT_STATUS,
} VkrHarnessGitQuery;

void vkr_harness_provenance_system(VkrHarnessProvenance *provenance) {
  snprintf(provenance->build_type, sizeof(provenance->build_type), "%s",
           VKR_HARNESS_BUILD_TYPE);
  snprintf(provenance->compiler, sizeof(provenance->compiler), "%s",
           VKR_HARNESS_COMPILER);
  snprintf(provenance->power_mode, sizeof(provenance->power_mode), "unknown");
  snprintf(provenance->thermal_state_start,
           sizeof(provenance->thermal_state_start), "unknown");
  snprintf(provenance->thermal_state_end, sizeof(provenance->thermal_state_end),
           "unknown");
#if defined(_WIN32)
  const char *processor = getenv("PROCESSOR_IDENTIFIER");
  snprintf(provenance->os, sizeof(provenance->os), "Windows");
  snprintf(provenance->cpu, sizeof(provenance->cpu), "%s",
           processor ? processor : "unknown");
  provenance->process_priority = 0;
#else
  struct utsname info = {0};
  if (uname(&info) == 0) {
    snprintf(provenance->os, sizeof(provenance->os), "%s %s", info.sysname,
             info.release);
    snprintf(provenance->cpu, sizeof(provenance->cpu), "%s", info.machine);
  } else {
    snprintf(provenance->os, sizeof(provenance->os), "unknown");
    snprintf(provenance->cpu, sizeof(provenance->cpu), "unknown");
  }
#if defined(__APPLE__)
  size_t cpu_length = sizeof(provenance->cpu);
  if (sysctlbyname("machdep.cpu.brand_string", provenance->cpu, &cpu_length,
                   NULL, 0) != 0 ||
      cpu_length <= 1u) {
    cpu_length = sizeof(provenance->cpu);
    (void)sysctlbyname("hw.model", provenance->cpu, &cpu_length, NULL, 0);
  }
#endif
  errno = 0;
  provenance->process_priority = getpriority(PRIO_PROCESS, 0);
#endif
  /* Device rows stay unknown until a process actually opens a target: the
     parent never does, and a run that never started has no device to report. */
  snprintf(provenance->gpu, sizeof(provenance->gpu), "unknown");
  snprintf(provenance->driver, sizeof(provenance->driver), "unknown");
  snprintf(provenance->color_format, sizeof(provenance->color_format),
           "unknown");
  snprintf(provenance->color_space, sizeof(provenance->color_space), "unknown");
}

void vkr_harness_provenance_set_text(char *field, uint64_t capacity,
                                     const char *value) {
  snprintf(field, (size_t)capacity, "%s",
           value && value[0] != '\0' ? value : "unknown");
}

/**
 * Reads the first stdout line of a fixed git query. The argument vector is
 * identical on both platforms so worktree cleanliness is decided by the same
 * porcelain output everywhere.
 */
static bool8_t vkr_harness_git_first_line(const char *repo_root,
                                          VkrHarnessGitQuery query, char *out,
                                          uint32_t capacity) {
#if defined(_WIN32)
  if (strchr(repo_root, '"')) {
    return false_v;
  }
  char command[VKR_HARNESS_PATH_MAX + 96u];
  snprintf(command, sizeof(command), "git -C \"%s\" %s", repo_root,
           query == VKR_HARNESS_GIT_HEAD
               ? "rev-parse HEAD"
               : "status --porcelain --untracked-files=normal");
  FILE *pipe = _popen(command, "r");
#else
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
    dup2(descriptors[1], STDOUT_FILENO);
    close(descriptors[0]);
    close(descriptors[1]);
    if (query == VKR_HARNESS_GIT_HEAD) {
      execlp("git", "git", "-C", repo_root, "rev-parse", "HEAD", NULL);
    } else {
      execlp("git", "git", "-C", repo_root, "status", "--porcelain",
             "--untracked-files=normal", NULL);
    }
    _exit(127);
  }
  close(descriptors[1]);
  FILE *pipe = fdopen(descriptors[0], "r");
#endif
  if (!pipe) {
#if !defined(_WIN32)
    close(descriptors[0]);
    int status = 0;
    waitpid(pid, &status, 0);
#endif
    return false_v;
  }
  const bool8_t read = fgets(out, (int)capacity, pipe) != NULL;
  if (!read) {
    out[0] = '\0';
  }
#if defined(_WIN32)
  const int status = _pclose(pipe);
  const bool8_t command_succeeded = status == 0;
#else
  fclose(pipe);
  int status = 0;
  waitpid(pid, &status, 0);
  const bool8_t command_succeeded =
      WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
  if (!command_succeeded || (!read && query == VKR_HARNESS_GIT_HEAD)) {
    return false_v;
  }
  out[strcspn(out, "\r\n")] = '\0';
  return true_v;
}

void vkr_harness_provenance_collect(const char *executable,
                                    const char *repo_root,
                                    VkrHarnessProvenance *provenance) {
  vkr_harness_provenance_system(provenance);
  if (!vkr_harness_git_first_line(repo_root, VKR_HARNESS_GIT_HEAD,
                                  provenance->git_sha,
                                  sizeof(provenance->git_sha))) {
    snprintf(provenance->git_sha, sizeof(provenance->git_sha), "unknown");
  }
  /* Any porcelain output at all — tracked or untracked — is a dirty tree. An
     unavailable git is treated as dirty rather than silently authoritative. */
  char status[8] = {0};
  const bool8_t status_available = vkr_harness_git_first_line(
      repo_root, VKR_HARNESS_GIT_STATUS, status, sizeof(status));
  provenance->dirty = !status_available || status[0] != '\0';
  if (!vkr_harness_sha256_file(executable, provenance->binary_sha256)) {
    provenance->binary_sha256[0] = '\0';
  }
}

uint32_t vkr_harness_environment_fields(
    const VkrHarnessProvenance *provenance, bool8_t exclusive_gpu_lane,
    VkrHarnessFingerprintField fields[VKR_HARNESS_ENVIRONMENT_FIELD_COUNT]) {
  uint32_t count = 0;
  char composite[VKR_HARNESS_TEXT_MAX];
#define VKR_HARNESS_ENVIRONMENT_FIELD(NAME, VALUE)                             \
  snprintf(fields[count].name, sizeof(fields[count].name), "%s", NAME);        \
  snprintf(fields[count++].value, sizeof(fields[0].value), "%s", VALUE)

  VKR_HARNESS_ENVIRONMENT_FIELD("build_type", provenance->build_type);
  VKR_HARNESS_ENVIRONMENT_FIELD("compiler", provenance->compiler);
  VKR_HARNESS_ENVIRONMENT_FIELD("os", provenance->os);
  VKR_HARNESS_ENVIRONMENT_FIELD("cpu", provenance->cpu);
  VKR_HARNESS_ENVIRONMENT_FIELD("gpu", provenance->gpu);
  VKR_HARNESS_ENVIRONMENT_FIELD("driver", provenance->driver);
  snprintf(composite, sizeof(composite), "%u:%u", provenance->gpu_vendor_id,
           provenance->gpu_device_id);
  VKR_HARNESS_ENVIRONMENT_FIELD("gpu_ids", composite);
  snprintf(composite, sizeof(composite), "%s:%u",
           vkr_harness_present_name(provenance->actual_present),
           provenance->actual_target_image_count);
  VKR_HARNESS_ENVIRONMENT_FIELD("target", composite);
  VKR_HARNESS_ENVIRONMENT_FIELD("color_format", provenance->color_format);
  VKR_HARNESS_ENVIRONMENT_FIELD("color_space", provenance->color_space);
  VKR_HARNESS_ENVIRONMENT_FIELD("power_mode", provenance->power_mode);
  snprintf(composite, sizeof(composite), "%d:%u", provenance->process_priority,
           exclusive_gpu_lane);
  VKR_HARNESS_ENVIRONMENT_FIELD("execution", composite);
#undef VKR_HARNESS_ENVIRONMENT_FIELD
  return count;
}

const char *vkr_harness_phase2_unsupported(const VkrHarnessCase *case_manifest,
                                           const VkrHarnessProfile *profile) {
  if (case_manifest->target == VKR_HARNESS_TARGET_OFFSCREEN ||
      profile->target == VKR_HARNESS_TARGET_OFFSCREEN) {
    return "Offscreen targets ship in Phase 6";
  }
  if (case_manifest->present == VKR_HARNESS_PRESENT_NONE ||
      profile->required_present == VKR_HARNESS_PRESENT_NONE) {
    return "present=none requires an offscreen target (Phase 6)";
  }
  if (case_manifest->boot == VKR_HARNESS_BOOT_AUTOMATION) {
    return "The automation boot profile ships in Phase 3";
  }
  /* A case that also declares captures is still profileable: `profile` runs
     capture-free primary repetitions and excludes captures from its
     fingerprint. Only `snapshot`/`autotest` need Phase 4. */
  return NULL;
}
