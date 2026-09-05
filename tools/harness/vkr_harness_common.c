#include "core/vkr_subsystem_plan.h"
#include "vkr_harness.h"

bool8_t vkr_harness_metric_is_current_frame_work(const char *name) {
  if (!name)
    return false_v;
  if (string_n_equals(name, "frame.render_", sizeof("frame.render_") - 1u) ||
      string_equals(name, "frame.dynamic_resolution_transitions"))
    return true_v;
  if (string_equals(name, "instance_buffer.overflows"))
    return true_v;
  if (string_n_equals(name, "visibility.objects_",
                      sizeof("visibility.objects_") - 1u) ||
      string_n_equals(name, "visibility.gpu_candidates.",
                      sizeof("visibility.gpu_candidates.") - 1u) ||
      string_n_equals(name, "visibility.transmission.gpu_candidates.",
                      sizeof("visibility.transmission.gpu_candidates.") - 1u)) {
    return true_v;
  }
  return string_n_equals(name, "draw.shadow.", sizeof("draw.shadow.") - 1u) &&
         !string_find(name, ".indirect_");
}

bool8_t
vkr_harness_renderer_backend_resolve(const VkrHarnessRendererConfig *renderer,
                                     const char *environment_request,
                                     VkrRendererBackendType *out_backend) {
  if (!renderer || !out_backend) {
    return false_v;
  }
  const char *pinned = renderer->backend;
  if (pinned[0] != '\0' && environment_request &&
      environment_request[0] != '\0' &&
      !string_equals(pinned, environment_request)) {
    return false_v;
  }
  const char *requested = pinned[0] != '\0' ? pinned : environment_request;
  if (!requested || requested[0] == '\0') {
#if defined(_WIN32)
    *out_backend = VKR_RENDERER_BACKEND_TYPE_VULKAN;
#else
    *out_backend = VKR_RENDERER_BACKEND_TYPE_METAL;
#endif
    return true_v;
  }
  if (string_equals(requested, "vulkan")) {
    *out_backend = VKR_RENDERER_BACKEND_TYPE_VULKAN;
    return true_v;
  }
  if (string_equals(requested, "metal")) {
    *out_backend = VKR_RENDERER_BACKEND_TYPE_METAL;
    return true_v;
  }
  return false_v;
}

/**
 * @return True when `metric` belongs to `subsystem`'s own family, spelled
 *         either `<subsystem>.*` or `draw.<subsystem>.*`.
 */
static bool8_t vkr_harness_metric_is_owned_by(const char *metric,
                                              const char *subsystem) {
  static const char draw_prefix[] = "draw.";
  static const uint64_t draw_length = sizeof(draw_prefix) - 1u;
  const uint64_t length = string_length(subsystem);
  if (string_n_equals(metric, draw_prefix, draw_length)) {
    metric += draw_length;
  }
  /* `string_n_equals` stops at the terminator, so the trailing separator is
     only read once the name itself matched. */
  return string_n_equals(metric, subsystem, length) && metric[length] == '.';
}

/**
 * An assertion over a subsystem's own metrics cannot be answered by a boot that
 * omitted it, so naming one requests it. No such metric family is registered
 * today; this is the seam `autotest` assertions arrive through in Phase 5.
 */
static VkrSubsystemMask
vkr_harness_assertion_subsystems(const VkrHarnessCase *case_manifest) {
  static const struct {
    const char *name;
    VkrRendererSubsystem subsystem;
  } owners[] = {
      {"ui", VKR_RENDERER_SUBSYSTEM_UI},
      {"skybox", VKR_RENDERER_SUBSYSTEM_SKYBOX},
      {"editor", VKR_RENDERER_SUBSYSTEM_EDITOR},
      {"gizmo", VKR_RENDERER_SUBSYSTEM_GIZMO},
      {"picking", VKR_RENDERER_SUBSYSTEM_PICKING},
  };
  VkrSubsystemMask mask = 0u;
  for (uint32_t assertion = 0u; assertion < case_manifest->assertion_count;
       ++assertion) {
    const char *metric = case_manifest->assertions[assertion].metric;
    for (uint32_t owner = 0u; owner < ArrayCount(owners); ++owner) {
      if (vkr_harness_metric_is_owned_by(metric, owners[owner].name)) {
        mask |= VKR_RENDERER_SUBSYSTEM_BIT(owners[owner].subsystem);
      }
    }
  }
  return mask;
}

bool8_t vkr_harness_subsystem_plan(VkrHarnessTool tool,
                                   const VkrHarnessCase *case_manifest,
                                   VkrSubsystemPlan *out_plan,
                                   VkrHarnessError *out_error) {
  if (!case_manifest || !out_plan) {
    vkr_harness_error_set(out_error, "boot.plan", "$.boot",
                          "Subsystem plan inputs are invalid");
    return false_v;
  }

  VkrSubsystemMask requested = vkr_harness_assertion_subsystems(case_manifest);
  if (case_manifest->renderer.skybox) {
    requested |= VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SKYBOX);
  }
  if (case_manifest->renderer.editor) {
    requested |= VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_EDITOR);
  }
  if (case_manifest->renderer.text_fixture) {
    requested |= VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_UI);
  }
  if (tool != VKR_HARNESS_TOOL_PROFILE) {
    for (uint32_t capture = 0u; capture < case_manifest->capture_count;
         ++capture) {
      for (uint32_t channel = 0u;
           channel < case_manifest->captures[capture].channel_count;
           ++channel) {
        if (string_equals(case_manifest->captures[capture].channels[channel],
                          "picking_ids")) {
          requested |=
              VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_PICKING);
        }
      }
    }
  }

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  const VkrBootProfile profile =
      case_manifest->boot == VKR_HARNESS_BOOT_AUTOMATION
          ? VKR_BOOT_PROFILE_AUTOMATION
          : VKR_BOOT_PROFILE_FULL;
  /* Automation excludes every optional unit the workload did not ask for, so
     the exclusion set tracks the renderer's own definition of optional rather
     than a second list here that could drift as units are added. */
  const VkrSubsystemMask excluded =
      profile == VKR_BOOT_PROFILE_AUTOMATION
          ? VKR_RENDERER_SUBSYSTEM_OPTIONAL & ~requested
          : 0u;
  if (!vkr_subsystem_plan_build(profile, requested, excluded, out_plan,
                                &renderer_error)) {
    vkr_harness_error_set(out_error, "boot.plan", "$.boot",
                          "Workload requirements do not form a valid "
                          "renderer subsystem plan (%u)",
                          renderer_error);
    return false_v;
  }
  return true_v;
}

void vkr_harness_format_subsystem_mask(
    char out_text[VKR_HARNESS_SUBSYSTEM_MASK_MAX], VkrSubsystemMask mask) {
  string_format(out_text, VKR_HARNESS_SUBSYSTEM_MASK_MAX, "0x%016llx",
                (unsigned long long)mask);
}

const char *
vkr_harness_case_profile_mismatch(const VkrHarnessCase *case_manifest,
                                  const VkrHarnessProfile *profile) {
  if (case_manifest->target != profile->target) {
    return "Case and execution profile targets differ";
  }
  if (case_manifest->present != profile->required_present) {
    return "Case and execution profile presentation modes differ";
  }
  /* The stability gate compares the two halves of the last `window` warmup
     frames, so a window wider than the warmup can never be answered and the
     run is unconditionally incomplete. Rejected here as an incompatible
     pairing rather than after every repetition has already executed. */
  if (profile->require_warmup_stability &&
      case_manifest->warmup_frames < profile->warmup_stability_window) {
    return "Case warmup is shorter than the profile's warmup stability window";
  }
  return NULL;
}

void vkr_harness_error_clear(VkrHarnessError *error) {
  if (error) {
    MemZero(error, sizeof(*error));
  }
}

void vkr_harness_error_set(VkrHarnessError *error, const char *code,
                           const char *field, const char *format, ...) {
  if (!error) {
    return;
  }
  string_format(error->code, sizeof(error->code), "%s", code ? code : "error");
  string_format(error->field, sizeof(error->field), "%s", field ? field : "$");
  va_list args;
  va_start(args, format);
  string_format_v(error->message, sizeof(error->message), format, args);
  va_end(args);
}

static void vkr_harness_stream_write(bool8_t error_stream, const char *format,
                                     va_list args) {
  char message[4096];
  string_format_v(message, sizeof(message), format, args);
  if (error_stream) {
    vkr_platform_stderr_write(message);
  } else {
    vkr_platform_stdout_write(message);
  }
}

void vkr_harness_stdout(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vkr_harness_stream_write(false_v, format, args);
  va_end(args);
}

void vkr_harness_stderr(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vkr_harness_stream_write(true_v, format, args);
  va_end(args);
}

FilePath vkr_harness_file_path(const char *path) {
  const uint64_t length = path ? string_length(path) : 0u;
  return (FilePath){
      .path = string8_create_from_cstr((const uint8_t *)path, length),
      .type = path && (path[0] == '/' || (path[0] && path[1] == ':'))
                  ? FILE_PATH_TYPE_ABSOLUTE
                  : FILE_PATH_TYPE_RELATIVE,
  };
}

bool8_t vkr_harness_read_file(const char *path, Arena *arena,
                              uint8_t **out_data, uint64_t *out_size) {
  if (!path || !arena || !out_data || !out_size) {
    return false_v;
  }
  FilePath file_path = vkr_harness_file_path(path);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&file_path, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  const FileError result = file_read_all(&file, &allocator, out_data, out_size);
  file_close(&file);
  vkr_allocator_release_global_accounting(&allocator);
  return result == FILE_ERROR_NONE;
}

const char *vkr_harness_tool_name(VkrHarnessTool tool) {
  switch (tool) {
  case VKR_HARNESS_TOOL_PROFILE:
    return "profile";
  case VKR_HARNESS_TOOL_SNAPSHOT:
    return "snapshot";
  case VKR_HARNESS_TOOL_AUTOTEST:
    return "autotest";
  case VKR_HARNESS_TOOL_COMPARE:
    return "compare";
  default:
    return "unknown";
  }
}

const char *vkr_harness_exit_code_name(VkrHarnessExitCode exit_code) {
  switch (exit_code) {
  case VKR_HARNESS_EXIT_PASS:
    return "pass";
  case VKR_HARNESS_EXIT_FAIL:
    return "fail";
  case VKR_HARNESS_EXIT_INVALID:
    return "invalid";
  case VKR_HARNESS_EXIT_UNAVAILABLE:
    return "unavailable";
  case VKR_HARNESS_EXIT_MISSING_BASELINE:
    return "missing_baseline";
  case VKR_HARNESS_EXIT_ERROR:
  default:
    return "incomplete";
  }
}

const char *vkr_harness_target_name(VkrHarnessTarget target) {
  switch (target) {
  case VKR_HARNESS_TARGET_WINDOWED_VISIBLE:
    return "windowed_visible";
  case VKR_HARNESS_TARGET_WINDOWED_HIDDEN:
    return "windowed_hidden";
  case VKR_HARNESS_TARGET_OFFSCREEN:
    return "offscreen";
  default:
    return "unknown";
  }
}

const char *vkr_harness_present_name(VkrHarnessPresentMode present) {
  switch (present) {
  case VKR_HARNESS_PRESENT_IMMEDIATE:
    return "immediate";
  case VKR_HARNESS_PRESENT_FIFO:
    return "fifo";
  case VKR_HARNESS_PRESENT_NONE:
    return "none";
  case VKR_HARNESS_PRESENT_MAILBOX:
    return "mailbox";
  case VKR_HARNESS_PRESENT_UNKNOWN:
  default:
    return "unknown";
  }
}

const char *vkr_harness_cache_name(VkrHarnessCacheMode cache) {
  switch (cache) {
  case VKR_HARNESS_CACHE_ISOLATED_COLD:
    return "isolated_cold";
  case VKR_HARNESS_CACHE_ISOLATED_WARM:
    return "isolated_warm";
  case VKR_HARNESS_CACHE_SHARED:
    return "shared";
  default:
    return "unknown";
  }
}

const char *vkr_harness_boot_name(VkrHarnessBootProfile boot) {
  switch (boot) {
  case VKR_HARNESS_BOOT_FULL:
    return "full";
  case VKR_HARNESS_BOOT_AUTOMATION:
    return "automation";
  default:
    return "unknown";
  }
}

/** Index-aligned with VkrHarnessStatisticKind; also the report/CSV wire names.
 */
static const char *const vkr_harness_statistic_names[VKR_HARNESS_STAT_COUNT] = {
    "mean", "p50", "p95", "min", "max", "stddev", "total"};

const char *vkr_harness_statistic_name(VkrHarnessStatisticKind statistic) {
  return statistic < VKR_HARNESS_STAT_COUNT
             ? vkr_harness_statistic_names[statistic]
             : "unknown";
}

bool8_t
vkr_harness_statistic_from_name(const char *name,
                                VkrHarnessStatisticKind *out_statistic) {
  if (!name || !out_statistic) {
    return false_v;
  }
  for (uint32_t i = 0; i < VKR_HARNESS_STAT_COUNT; ++i) {
    if (string_equals(name, vkr_harness_statistic_names[i])) {
      *out_statistic = (VkrHarnessStatisticKind)i;
      return true_v;
    }
  }
  return false_v;
}

const char *vkr_harness_operator_name(VkrHarnessAssertionOperator operation) {
  switch (operation) {
  case VKR_HARNESS_ASSERT_MAX:
    return "max";
  case VKR_HARNESS_ASSERT_MIN:
    return "min";
  case VKR_HARNESS_ASSERT_EQUALS:
    return "equals";
  default:
    return "unknown";
  }
}

float64_t vkr_harness_statistic_value(const VkrHarnessStatistics *statistics,
                                      VkrHarnessStatisticKind statistic) {
  switch (statistic) {
  case VKR_HARNESS_STAT_P50:
    return statistics->p50;
  case VKR_HARNESS_STAT_P95:
    return statistics->p95;
  case VKR_HARNESS_STAT_MIN:
    return statistics->min;
  case VKR_HARNESS_STAT_MAX:
    return statistics->max;
  case VKR_HARNESS_STAT_STDDEV:
    return statistics->stddev;
  case VKR_HARNESS_STAT_TOTAL:
    return statistics->total;
  case VKR_HARNESS_STAT_MEAN:
  default:
    return statistics->mean;
  }
}

const VkrHarnessMetricResult *
vkr_harness_report_find_metric(const VkrHarnessReport *report,
                               const char *name) {
  for (uint32_t i = 0; i < report->metric_count; ++i) {
    if (string_equals(report->metrics[i].name, name)) {
      return &report->metrics[i];
    }
  }
  return NULL;
}

VkrHarnessAssertionOutcome
vkr_harness_assertion_evaluate(const VkrHarnessReport *report,
                               const VkrHarnessAssertion *assertion,
                               float64_t *out_actual) {
  if (out_actual) {
    *out_actual = 0.0;
  }
  const VkrHarnessMetricResult *metric =
      vkr_harness_report_find_metric(report, assertion->metric);
  if (!metric || metric->statistics.sample_count == 0u ||
      metric->statistics.invalid_count > 0u) {
    return VKR_HARNESS_ASSERTION_INCOMPLETE;
  }
  const float64_t actual =
      vkr_harness_statistic_value(&metric->statistics, assertion->statistic);
  if (out_actual) {
    *out_actual = actual;
  }
  bool8_t passed = false_v;
  switch (assertion->operation) {
  case VKR_HARNESS_ASSERT_MAX:
    passed = actual <= assertion->limit;
    break;
  case VKR_HARNESS_ASSERT_MIN:
    passed = actual >= assertion->limit;
    break;
  case VKR_HARNESS_ASSERT_EQUALS:
  default:
    passed = vkr_abs_f64(actual - assertion->limit) <= assertion->tolerance;
    break;
  }
  return passed ? VKR_HARNESS_ASSERTION_PASS : VKR_HARNESS_ASSERTION_FAIL;
}

const char *
vkr_harness_assertion_outcome_name(VkrHarnessAssertionOutcome outcome) {
  switch (outcome) {
  case VKR_HARNESS_ASSERTION_PASS:
    return "pass";
  case VKR_HARNESS_ASSERTION_FAIL:
    return "fail";
  case VKR_HARNESS_ASSERTION_INCOMPLETE:
  default:
    return "incomplete";
  }
}
