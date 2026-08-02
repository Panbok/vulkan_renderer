#include "vkr_harness_runtime.h"

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
  string_format(provenance->build_type, sizeof(provenance->build_type), "%s",
                VKR_HARNESS_BUILD_TYPE);
  string_format(provenance->compiler, sizeof(provenance->compiler), "%s",
                VKR_HARNESS_COMPILER);
  string_format(provenance->power_mode, sizeof(provenance->power_mode),
                "unknown");
  string_format(provenance->thermal_state_start,
                sizeof(provenance->thermal_state_start), "unknown");
  string_format(provenance->thermal_state_end,
                sizeof(provenance->thermal_state_end), "unknown");
  VkrPlatformSystemInfo system = {0};
  (void)vkr_platform_get_system_info(&system);
  vkr_harness_provenance_set_text(provenance->os, sizeof(provenance->os),
                                  system.os);
  vkr_harness_provenance_set_text(provenance->cpu, sizeof(provenance->cpu),
                                  system.cpu);
  provenance->process_priority = system.process_priority;
  /* Device rows stay unknown until a process actually opens a target: the
     parent never does, and a run that never started has no device to report. */
  string_format(provenance->gpu, sizeof(provenance->gpu), "unknown");
  string_format(provenance->driver, sizeof(provenance->driver), "unknown");
  string_format(provenance->color_format, sizeof(provenance->color_format),
                "unknown");
  string_format(provenance->depth_format, sizeof(provenance->depth_format),
                "unknown");
  string_format(provenance->color_space, sizeof(provenance->color_space),
                "unknown");
}

void vkr_harness_provenance_set_text(char *field, uint64_t capacity,
                                     const char *value) {
  string_format(field, capacity, "%s",
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
  const char *head_arguments[] = {"-C", repo_root, "rev-parse", "HEAD"};
  const char *status_arguments[] = {"-C", repo_root, "status", "--porcelain",
                                    "--untracked-files=normal"};
  const char *const *arguments =
      query == VKR_HARNESS_GIT_HEAD ? head_arguments : status_arguments;
  const uint32_t argument_count = query == VKR_HARNESS_GIT_HEAD
                                      ? ArrayCount(head_arguments)
                                      : ArrayCount(status_arguments);
  int32_t exit_code = -1;
  out[0] = '\0';
  if (!vkr_platform_process_capture("git", arguments, argument_count, NULL, out,
                                    capacity, &exit_code) ||
      exit_code != 0 || (query == VKR_HARNESS_GIT_HEAD && out[0] == '\0')) {
    return false_v;
  }
  for (uint32_t i = 0; out[i] != '\0'; ++i) {
    if (out[i] == '\r' || out[i] == '\n') {
      out[i] = '\0';
      break;
    }
  }
  return true_v;
}

void vkr_harness_provenance_collect(const char *executable,
                                    const char *repo_root,
                                    VkrHarnessProvenance *provenance) {
  vkr_harness_provenance_system(provenance);
  if (!vkr_harness_git_first_line(repo_root, VKR_HARNESS_GIT_HEAD,
                                  provenance->git_sha,
                                  sizeof(provenance->git_sha))) {
    string_format(provenance->git_sha, sizeof(provenance->git_sha), "unknown");
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
  string_format(fields[count].name, sizeof(fields[count].name), "%s", NAME);   \
  string_format(fields[count++].value, sizeof(fields[0].value), "%s", VALUE)

  VKR_HARNESS_ENVIRONMENT_FIELD("build_type", provenance->build_type);
  VKR_HARNESS_ENVIRONMENT_FIELD("compiler", provenance->compiler);
  VKR_HARNESS_ENVIRONMENT_FIELD("os", provenance->os);
  VKR_HARNESS_ENVIRONMENT_FIELD("cpu", provenance->cpu);
  VKR_HARNESS_ENVIRONMENT_FIELD("gpu", provenance->gpu);
  VKR_HARNESS_ENVIRONMENT_FIELD("driver", provenance->driver);
  string_format(composite, sizeof(composite), "%u:%u",
                provenance->gpu_vendor_id, provenance->gpu_device_id);
  VKR_HARNESS_ENVIRONMENT_FIELD("gpu_ids", composite);
  string_format(composite, sizeof(composite), "%s:%s:%u:%ux%u",
                vkr_harness_target_name(provenance->actual_target),
                vkr_harness_present_name(provenance->actual_present),
                provenance->actual_target_image_count,
                provenance->actual_target_width,
                provenance->actual_target_height);
  VKR_HARNESS_ENVIRONMENT_FIELD("target", composite);
  VKR_HARNESS_ENVIRONMENT_FIELD("color_format", provenance->color_format);
  VKR_HARNESS_ENVIRONMENT_FIELD("depth_format", provenance->depth_format);
  VKR_HARNESS_ENVIRONMENT_FIELD("color_space", provenance->color_space);
  VKR_HARNESS_ENVIRONMENT_FIELD("power_mode", provenance->power_mode);
  string_format(composite, sizeof(composite), "%d:%u",
                provenance->process_priority, exclusive_gpu_lane);
  VKR_HARNESS_ENVIRONMENT_FIELD("execution", composite);
#undef VKR_HARNESS_ENVIRONMENT_FIELD
  return count;
}
