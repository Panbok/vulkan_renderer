#include "core/vkr_json.h"
#include "vkr_harness_json.h"
#include "vkr_harness_runtime.h"

#include <stb_image.h>

static uint32_t vkr_harness_read_u32_le(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
         ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint16_t vkr_harness_read_u16_le(const uint8_t *bytes) {
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static float32_t vkr_harness_half_to_float(uint16_t half) {
  const uint32_t sign = half >> 15u;
  const uint32_t exponent = (half >> 10u) & 0x1fu;
  const uint32_t mantissa = half & 0x3ffu;
  float32_t value = 0.0f;
  if (exponent == 0u) {
    value = (float32_t)mantissa * (1.0f / 16777216.0f);
  } else if (exponent < 31u) {
    value = (1.0f + (float32_t)mantissa * (1.0f / 1024.0f)) *
            vkr_pow_f32(2.0f, (float32_t)((int32_t)exponent - 15));
  } else {
    const uint32_t bits = (sign << 31u) | 0x7f800000u | (mantissa << 13u);
    MemCopy(&value, &bits, sizeof(value));
    return value;
  }
  return sign ? -value : value;
}

static VkrHarnessComparisonResult
vkr_harness_comparison_finish(VkrHarnessComparisonResult result,
                              const VkrHarnessCompareConfig *config,
                              bool8_t exact) {
  result.mean_absolute_error =
      result.value_count ? result.mean_absolute_error / result.value_count
                         : 0.0;
  result.failed_pixel_ratio =
      result.pixel_count
          ? (float64_t)result.failing_pixel_count / result.pixel_count
          : 0.0;
  const bool8_t pass =
      exact
          ? result.failing_value_count == 0u
          : result.max_absolute_error <= config->max_pixel_delta &&
                result.mean_absolute_error <= config->max_mean_absolute_error &&
                result.failed_pixel_ratio <= config->max_failed_pixel_ratio;
  result.outcome =
      pass ? VKR_HARNESS_COMPARISON_PASS : VKR_HARNESS_COMPARISON_FAIL;
  return result;
}

/** Error magnitude as grey; a pixel over threshold is pushed to red. */
static void vkr_harness_diff_pixel(uint8_t *diff, uint64_t pixel,
                                   float64_t error, bool8_t failed) {
  if (!diff) {
    return;
  }
  const uint8_t value = (uint8_t)(Clamp(error, 0.0, 1.0) * 255.0 + 0.5);
  diff[pixel * 4u + 0u] = failed ? 255u : value;
  diff[pixel * 4u + 1u] = value;
  diff[pixel * 4u + 2u] = value;
  diff[pixel * 4u + 3u] = 255u;
}

VkrHarnessComparisonResult vkr_harness_compare_rgba8(
    const uint8_t *actual, const uint8_t *baseline, uint64_t pixel_count,
    const VkrHarnessCompareConfig *config, uint8_t *diff_rgba) {
  VkrHarnessComparisonResult result = {
      .outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE,
      .value_count = pixel_count * 4u,
      .pixel_count = pixel_count,
  };
  if (!actual || !baseline || !config) {
    return result;
  }
  for (uint64_t pixel = 0; pixel < pixel_count; ++pixel) {
    bool8_t failed = false_v;
    float64_t pixel_error = 0.0;
    for (uint32_t component = 0; component < 4u; ++component) {
      const uint64_t index = pixel * 4u + component;
      const float64_t error =
          vkr_abs_f64((float64_t)actual[index] - baseline[index]) / 255.0;
      result.mean_absolute_error += error;
      result.max_absolute_error = Max(result.max_absolute_error, error);
      pixel_error = Max(pixel_error, error);
      if (error > config->max_pixel_delta) {
        result.failing_value_count++;
        failed = true_v;
      }
    }
    result.failing_pixel_count += failed ? 1u : 0u;
    vkr_harness_diff_pixel(diff_rgba, pixel, pixel_error, failed);
  }
  return vkr_harness_comparison_finish(result, config, false_v);
}

VkrHarnessComparisonResult vkr_harness_compare_f32_le(
    const uint8_t *actual, const uint8_t *baseline, uint64_t pixel_count,
    const VkrHarnessCompareConfig *config, uint8_t *diff_rgba) {
  VkrHarnessComparisonResult result = {
      .outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE,
      .value_count = pixel_count,
      .pixel_count = pixel_count,
  };
  if (!actual || !baseline || !config) {
    return result;
  }
  for (uint64_t pixel = 0; pixel < pixel_count; ++pixel) {
    const uint32_t actual_bits = vkr_harness_read_u32_le(actual + pixel * 4u);
    const uint32_t baseline_bits =
        vkr_harness_read_u32_le(baseline + pixel * 4u);
    float32_t actual_value = 0.0f;
    float32_t baseline_value = 0.0f;
    MemCopy(&actual_value, &actual_bits, sizeof(actual_value));
    MemCopy(&baseline_value, &baseline_bits, sizeof(baseline_value));
    /* A non-finite sample has no meaningful distance to any baseline, so the
       whole channel is reported incompatible rather than silently thresholded.
     */
    if (!vkr_is_finite_f64(actual_value) ||
        !vkr_is_finite_f64(baseline_value)) {
      return result;
    }
    const float64_t error =
        vkr_abs_f64((float64_t)actual_value - baseline_value);
    result.mean_absolute_error += error;
    result.max_absolute_error = Max(result.max_absolute_error, error);
    const bool8_t failed = error > config->max_pixel_delta;
    result.failing_value_count += failed ? 1u : 0u;
    result.failing_pixel_count += failed ? 1u : 0u;
    vkr_harness_diff_pixel(diff_rgba, pixel, error, failed);
  }
  return vkr_harness_comparison_finish(result, config, false_v);
}

static VkrHarnessComparisonResult
vkr_harness_compare_float16_le(const uint8_t *actual, const uint8_t *baseline,
                               uint64_t pixel_count, uint32_t component_count,
                               const VkrHarnessCompareConfig *config,
                               uint8_t *diff_rgba) {
  VkrHarnessComparisonResult result = {
      .outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE,
      .value_count = pixel_count * component_count,
      .pixel_count = pixel_count,
  };
  if (!actual || !baseline || !config) {
    return result;
  }
  for (uint64_t pixel = 0; pixel < pixel_count; ++pixel) {
    bool8_t failed = false_v;
    float64_t pixel_error = 0.0;
    for (uint32_t component = 0; component < component_count; ++component) {
      const uint64_t index = (pixel * component_count + component) * 2u;
      const float32_t actual_value =
          vkr_harness_half_to_float(vkr_harness_read_u16_le(actual + index));
      const float32_t baseline_value =
          vkr_harness_half_to_float(vkr_harness_read_u16_le(baseline + index));
      if (!vkr_is_finite_f64(actual_value) ||
          !vkr_is_finite_f64(baseline_value)) {
        return result;
      }
      const float64_t error =
          vkr_abs_f64((float64_t)actual_value - baseline_value);
      result.mean_absolute_error += error;
      result.max_absolute_error = Max(result.max_absolute_error, error);
      pixel_error = Max(pixel_error, error);
      if (error > config->max_pixel_delta) {
        result.failing_value_count++;
        failed = true_v;
      }
    }
    result.failing_pixel_count += failed ? 1u : 0u;
    vkr_harness_diff_pixel(diff_rgba, pixel, pixel_error, failed);
  }
  return vkr_harness_comparison_finish(result, config, false_v);
}

VkrHarnessComparisonResult vkr_harness_compare_rgba16f_le(
    const uint8_t *actual, const uint8_t *baseline, uint64_t pixel_count,
    const VkrHarnessCompareConfig *config, uint8_t *diff_rgba) {
  return vkr_harness_compare_float16_le(actual, baseline, pixel_count, 4u,
                                        config, diff_rgba);
}

VkrHarnessComparisonResult vkr_harness_compare_rg16f_le(
    const uint8_t *actual, const uint8_t *baseline, uint64_t pixel_count,
    const VkrHarnessCompareConfig *config, uint8_t *diff_rgba) {
  return vkr_harness_compare_float16_le(actual, baseline, pixel_count, 2u,
                                        config, diff_rgba);
}

VkrHarnessComparisonResult vkr_harness_compare_u32_le(const uint8_t *actual,
                                                      const uint8_t *baseline,
                                                      uint64_t pixel_count,
                                                      uint8_t *diff_rgba) {
  VkrHarnessComparisonResult result = {
      .outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE,
      .value_count = pixel_count,
      .pixel_count = pixel_count,
  };
  if (!actual || !baseline) {
    return result;
  }
  for (uint64_t pixel = 0; pixel < pixel_count; ++pixel) {
    const bool8_t failed = vkr_harness_read_u32_le(actual + pixel * 4u) !=
                           vkr_harness_read_u32_le(baseline + pixel * 4u);
    result.failing_value_count += failed ? 1u : 0u;
    result.failing_pixel_count += failed ? 1u : 0u;
    result.mean_absolute_error += failed ? 1.0 : 0.0;
    result.max_absolute_error = failed ? 1.0 : result.max_absolute_error;
    vkr_harness_diff_pixel(diff_rgba, pixel, failed ? 1.0 : 0.0, failed);
  }
  /* Identifiers admit no tolerance, so the verdict comes from the exact path
     and the thresholds are never consulted. */
  const VkrHarnessCompareConfig unused_thresholds = {0};
  return vkr_harness_comparison_finish(result, &unused_thresholds, true_v);
}

const char *
vkr_harness_comparison_outcome_name(VkrHarnessComparisonOutcome outcome) {
  switch (outcome) {
  case VKR_HARNESS_COMPARISON_PASS:
    return "pass";
  case VKR_HARNESS_COMPARISON_FAIL:
    return "fail";
  case VKR_HARNESS_COMPARISON_INCOMPATIBLE:
    return "incompatible";
  case VKR_HARNESS_COMPARISON_NOT_RUN:
  default:
    return "not_run";
  }
}

/**
 * Copies one source artifact into the compare run's own root, refusing any
 * payload whose digest no longer matches what the source report published.
 */
static bool8_t vkr_harness_compare_copy_artifact(
    const char *source_root, const char *destination_root,
    const VkrHarnessArtifact *artifact, Arena *arena, VkrHarnessError *error) {
  char source[VKR_HARNESS_PATH_MAX];
  char destination[VKR_HARNESS_PATH_MAX];
  char directory[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_path_is_safe_relative(artifact->path) ||
      string_format(source, sizeof(source), "%s/%s", source_root,
                    artifact->path) <= 0 ||
      string_format(destination, sizeof(destination), "%s/%s", destination_root,
                    artifact->path) <= 0 ||
      !vkr_harness_path_parent(destination, directory)) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t length = 0u;
  char digest[VKR_HARNESS_DIGEST_MAX];
  Scratch scratch = scratch_create(arena);
  const bool8_t ok =
      vkr_harness_make_directories(directory, error) &&
      vkr_harness_sha256_file(source, digest) &&
      string_equals(digest, artifact->sha256) &&
      vkr_harness_read_file(source, arena, &bytes, &length) &&
      vkr_harness_atomic_write(destination, bytes, length, error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return ok;
}

void vkr_harness_compare_publish_diffs(VkrHarnessReport *report,
                                       const char *run_root) {
  if (!report || !run_root) {
    return;
  }
  for (uint32_t i = 0; i < report->capture_count; ++i) {
    const char *relative = report->captures[i].diff_path;
    if (!relative[0]) {
      continue;
    }
    char absolute[VKR_HARNESS_PATH_MAX];
    string_format(absolute, sizeof(absolute), "%s/%s", run_root, relative);
    if (!vkr_harness_report_add_artifact(report, "comparison.diff", relative,
                                         "image/png", absolute)) {
      vkr_harness_report_mark_incomplete(report,
                                         "comparison.diff_digest_failed");
      return;
    }
  }
}

const char *
vkr_harness_baseline_incompatibility(const VkrHarnessCaptureSummary *actual,
                                     const VkrHarnessCaptureSummary *baseline,
                                     bool8_t cross_backend) {
  if (!actual || !baseline ||
      !string_equals(actual->workload_fingerprint,
                     baseline->workload_fingerprint) ||
      !string_equals(actual->policy_fingerprint,
                     baseline->policy_fingerprint)) {
    return "baseline.fingerprint_mismatch";
  }
  if (!cross_backend) {
    return string_equals(actual->environment_fingerprint,
                         baseline->environment_fingerprint)
               ? NULL
               : "baseline.fingerprint_mismatch";
  }
  if (actual->case_manifest.renderer.backend[0] ||
      baseline->case_manifest.renderer.backend[0]) {
    return "baseline.cross_backend_case_pinned";
  }
  if (string_equals(actual->environment_fingerprint,
                    baseline->environment_fingerprint)) {
    return "baseline.cross_backend_same_environment";
  }
  return NULL;
}

int vkr_harness_compare_run(const char *repo_root, const char *run_path,
                            bool8_t cross_backend) {
  if (!repo_root || !run_path || !vkr_harness_path_is_safe_relative(run_path)) {
    return VKR_HARNESS_EXIT_INVALID;
  }
  /* Report tables and the summaries they alias live on `arena` until the report
     is written; every decoded image and diff buffer is scoped to `transient`
     and released between captures. */
  Arena *arena = arena_create();
  Arena *transient = arena_create();
  VkrHarnessError error = {0};
  int result = VKR_HARNESS_EXIT_ERROR;
  char source_root[VKR_HARNESS_PATH_MAX];
  string_format(source_root, sizeof(source_root), "%s/%s", repo_root, run_path);
  vkr_harness_path_to_run_root(source_root);
  char snapshot_root[VKR_HARNESS_PATH_MAX];
  string_format(snapshot_root, sizeof(snapshot_root), "%s/%s", repo_root,
                "build/_artifacts/snapshot");
  if (!arena || !transient ||
      !vkr_harness_existing_path_is_below(snapshot_root, source_root)) {
    vkr_harness_stderr("compare --run must name a snapshot run\n");
    goto cleanup;
  }
  char source_summary_path[VKR_HARNESS_PATH_MAX];
  string_format(source_summary_path, sizeof(source_summary_path),
                "%s/capture-summary.bin", source_root);
  VkrHarnessCaptureSummary source = {0};
  if (!vkr_harness_capture_summary_read(source_summary_path, arena, &source) ||
      source.capture_count == 0u) {
    vkr_harness_stderr("Snapshot capture summary is missing or invalid\n");
    goto cleanup;
  }
  char artifact_candidate[VKR_HARNESS_PATH_MAX];
  char artifact_root[VKR_HARNESS_PATH_MAX];
  char compare_root[VKR_HARNESS_PATH_MAX];
  string_format(artifact_candidate, sizeof(artifact_candidate), "%s/%s",
                repo_root, "build/_artifacts/compare");
  if (!vkr_harness_make_directories(artifact_candidate, &error) ||
      !vkr_harness_realpath(artifact_candidate, artifact_root)) {
    goto cleanup;
  }
  VkrHarnessReport report = {
      .tool = VKR_HARNESS_TOOL_COMPARE,
      .authoritative = source.authoritative,
      .case_manifest = source.case_manifest,
      .profile = source.profile,
      .profile_compatible = source.profile_compatible,
      .provenance = source.provenance,
      .requested_repetitions = 1u,
      .completed_repetitions = 1u,
  };
  string_format(report.environment_fingerprint,
                sizeof(report.environment_fingerprint), "%s",
                source.environment_fingerprint);
  string_format(report.workload_fingerprint,
                sizeof(report.workload_fingerprint), "%s",
                source.workload_fingerprint);
  string_format(report.policy_fingerprint, sizeof(report.policy_fingerprint),
                "%s", source.policy_fingerprint);
  if (!vkr_harness_create_run_root(artifact_root, report.run_id,
                                   compare_root) ||
      !vkr_harness_report_init_storage(&report, arena, source.capture_count,
                                       source.artifact_count +
                                           source.capture_count + 1u)) {
    goto cleanup;
  }
  report.capture_count = source.capture_count;
  MemCopy(report.captures, source.captures,
          sizeof(*report.captures) * source.capture_count);
  for (uint32_t i = 0; i < source.artifact_count; ++i) {
    const VkrHarnessArtifact *artifact = &source.artifacts[i];
    if (!string_equals(artifact->role, "capture.color") &&
        !string_equals(artifact->role, "capture.raw") &&
        !string_equals(artifact->role, "capture.preview") &&
        !string_equals(artifact->role, "capture.metadata")) {
      continue;
    }
    if (!vkr_harness_compare_copy_artifact(source_root, compare_root, artifact,
                                           arena, &error) ||
        report.artifact_count >= report.artifact_capacity) {
      goto cleanup;
    }
    report.artifacts[report.artifact_count++] = *artifact;
  }
  char baseline_root[VKR_HARNESS_PATH_MAX];
  VkrHarnessCaptureSummary baseline = {0};
  if (!vkr_harness_baseline_current(repo_root, source.profile_id,
                                    source.case_id, arena, baseline_root,
                                    &baseline, &error)) {
    vkr_harness_report_set_status(&report, "missing_baseline",
                                  VKR_HARNESS_EXIT_MISSING_BASELINE);
    vkr_harness_report_add_authority_reason(&report, "baseline.missing");
  } else {
    const char *incompatibility =
        vkr_harness_baseline_incompatibility(&source, &baseline, cross_backend);
    if (incompatibility) {
      vkr_harness_report_set_status(&report, "missing_baseline",
                                    VKR_HARNESS_EXIT_MISSING_BASELINE);
      vkr_harness_report_add_incompatibility(&report, incompatibility);
    } else {
      VkrHarnessArenas arenas = {.persistent = arena, .transient = transient};
      const VkrHarnessExitCode comparison = vkr_harness_compare_capture_sets(
          compare_root, baseline_root, report.captures, report.capture_count,
          baseline.captures, baseline.capture_count, &arenas, &error);
      vkr_harness_report_set_status(
          &report, vkr_harness_exit_code_name(comparison), comparison);
      vkr_harness_compare_publish_diffs(&report, compare_root);
    }
  }
  char report_path[VKR_HARNESS_PATH_MAX];
  char digest[VKR_HARNESS_DIGEST_MAX];
  string_format(report_path, sizeof(report_path), "%s/report.json",
                compare_root);
  vkr_harness_timestamp_utc(report.provenance.ended_at);
  if (!vkr_harness_report_write(report_path, &report, &error) ||
      !vkr_harness_sha256_file(report_path, digest)) {
    goto cleanup;
  }
  vkr_harness_stdout(
      "{\"status\":\"%s\",\"exit_code\":%u,\"report\":"
      "\"build/_artifacts/compare/%s/report.json\",\"sha256\":\"%s\"}\n",
      report.status, report.exit_code, report.run_id, digest);
  result = report.exit_code;
cleanup:
  if (result == VKR_HARNESS_EXIT_ERROR && error.message[0]) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
  }
  arena_destroy(transient);
  arena_destroy(arena);
  return result;
}

static const VkrHarnessCaptureResult *
vkr_harness_baseline_capture_find(const VkrHarnessCaptureResult *captures,
                                  uint32_t count,
                                  const VkrHarnessCaptureResult *actual) {
  for (uint32_t i = 0; i < count; ++i) {
    if (captures[i].checkpoint_frame == actual->checkpoint_frame &&
        string_equals(captures[i].channel, actual->channel)) {
      return &captures[i];
    }
  }
  return NULL;
}

static bool8_t
vkr_harness_capture_compatible(const VkrHarnessCaptureResult *actual,
                               const VkrHarnessCaptureResult *baseline) {
  return baseline && actual->capture_version == baseline->capture_version &&
         actual->width == baseline->width &&
         actual->height == baseline->height && actual->mip == baseline->mip &&
         actual->layer == baseline->layer &&
         string_equals(actual->canonical_encoding,
                       baseline->canonical_encoding) &&
         string_equals(actual->value_kind, baseline->value_kind) &&
         string_equals(actual->color_space, baseline->color_space) &&
         string_equals(actual->origin, baseline->origin);
}

static bool8_t
vkr_harness_capture_file_verified(const char *root, const char *relative,
                                  const char *expected,
                                  char absolute[VKR_HARNESS_PATH_MAX]) {
  char digest[VKR_HARNESS_DIGEST_MAX];
  if (!vkr_harness_path_is_safe_relative(relative) ||
      string_format(absolute, VKR_HARNESS_PATH_MAX, "%s/%s", root, relative) <=
          0 ||
      !vkr_harness_sha256_file(absolute, digest)) {
    return false_v;
  }
  return string_equals(digest, expected);
}

/** Extended-linear raw values are in the producer's display units. The
 * metadata sidecar is part of the digest-protected capture contract, so it is
 * the durable source for the headroom and native-unit scale omitted from the
 * fixed-layout capture summary. */
static bool8_t vkr_harness_capture_extended_display_metadata(
    const char *root, const VkrHarnessCaptureResult *capture, Arena *arena,
    float64_t *out_headroom, float64_t *out_scale) {
  if (!root || !capture || !arena || !out_headroom || !out_scale ||
      !string_equals(capture->color_space, "extended_srgb_linear"))
    return false_v;
  char path[VKR_HARNESS_PATH_MAX];
  uint8_t *bytes = NULL;
  uint64_t length = 0u;
  float64_t headroom = 0.0;
  float64_t scale = 0.0;
  char color_space[32] = {0};
  VkrHarnessError ignored = {0};
  Scratch scratch = scratch_create(arena);
  VkrHarnessJsonDocument *document =
      arena_alloc(arena, sizeof(*document), ARENA_MEMORY_TAG_STRUCT);
  bool8_t duplicate = false_v;
  const bool8_t loaded =
      vkr_harness_capture_file_verified(root, capture->metadata_path,
                                        capture->metadata_sha256, path) &&
      document && vkr_harness_read_file(path, arena, &bytes, &length) &&
      vkr_harness_json_parse(document, (const char *)bytes, length, &ignored);
  const int32_t color_space_token =
      loaded
          ? vkr_harness_json_object_get(document, 0, "color_space", &duplicate)
          : -1;
  const bool8_t color_space_valid =
      loaded && !duplicate && color_space_token >= 0 &&
      vkr_harness_json_string(document, color_space_token, color_space,
                              sizeof(color_space), "color_space", &ignored) &&
      string_equals(color_space, "extended_srgb_linear");
  duplicate = false_v;
  const int32_t headroom_token =
      color_space_valid ? vkr_harness_json_object_get(
                              document, 0, "display_headroom", &duplicate)
                        : -1;
  const bool8_t headroom_valid =
      color_space_valid && !duplicate && headroom_token >= 0 &&
      vkr_harness_json_f64(document, headroom_token, &headroom,
                           "display_headroom", &ignored);
  duplicate = false_v;
  const int32_t scale_token =
      headroom_valid ? vkr_harness_json_object_get(
                           document, 0, "display_output_scale", &duplicate)
                     : -1;
  const bool8_t scale_valid =
      headroom_valid && !duplicate && scale_token >= 0 &&
      vkr_harness_json_f64(document, scale_token, &scale,
                           "display_output_scale", &ignored) &&
      vkr_is_finite_f64(headroom) && vkr_is_finite_f64(scale) &&
      headroom >= 1.0 && scale > 0.0 && headroom * scale <= 65504.0;
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRUCT);
  if (!scale_valid)
    return false_v;
  *out_headroom = headroom;
  *out_scale = scale;
  return true_v;
}

VkrHarnessExitCode vkr_harness_compare_capture_sets(
    const char *actual_root, const char *baseline_root,
    VkrHarnessCaptureResult *actual, uint32_t actual_count,
    const VkrHarnessCaptureResult *baseline, uint32_t baseline_count,
    const VkrHarnessArenas *arenas, VkrHarnessError *error) {
  if (!actual_root || !baseline_root || !actual || !baseline || !arenas) {
    return VKR_HARNESS_EXIT_ERROR;
  }
  VkrHarnessExitCode verdict = VKR_HARNESS_EXIT_PASS;
  for (uint32_t i = 0; i < actual_count; ++i) {
    VkrHarnessCaptureResult *row = &actual[i];
    const VkrHarnessCaptureResult *reference =
        vkr_harness_baseline_capture_find(baseline, baseline_count, row);
    if (!vkr_harness_capture_compatible(row, reference)) {
      row->comparison.outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE;
      string_format(row->comparison_status, sizeof(row->comparison_status),
                    "incompatible");
      verdict = VKR_HARNESS_EXIT_MISSING_BASELINE;
      continue;
    }
    if (string_equals(row->color_space, "extended_srgb_linear")) {
      float64_t actual_headroom = 0.0;
      float64_t actual_scale = 0.0;
      float64_t baseline_headroom = 0.0;
      float64_t baseline_scale = 0.0;
      if (!vkr_harness_capture_extended_display_metadata(
              actual_root, row, arenas->transient, &actual_headroom,
              &actual_scale) ||
          !vkr_harness_capture_extended_display_metadata(
              baseline_root, reference, arenas->transient, &baseline_headroom,
              &baseline_scale) ||
          actual_headroom != baseline_headroom ||
          actual_scale != baseline_scale) {
        row->comparison.outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE;
        string_format(row->comparison_status, sizeof(row->comparison_status),
                      "incompatible");
        verdict = VKR_HARNESS_EXIT_MISSING_BASELINE;
        continue;
      }
    }
    char actual_path[VKR_HARNESS_PATH_MAX];
    char baseline_path[VKR_HARNESS_PATH_MAX];
    if (!vkr_harness_capture_file_verified(actual_root, row->data_path,
                                           row->data_sha256, actual_path) ||
        !vkr_harness_capture_file_verified(baseline_root, reference->data_path,
                                           reference->data_sha256,
                                           baseline_path)) {
      vkr_harness_error_set(error, "comparison.digest", row->channel,
                            "Capture data changed after publication");
      return VKR_HARNESS_EXIT_ERROR;
    }
    string_format(row->baseline_data_path, sizeof(row->baseline_data_path),
                  "%s", reference->data_path);
    string_format(row->baseline_data_sha256, sizeof(row->baseline_data_sha256),
                  "%s", reference->data_sha256);

    Scratch scratch = scratch_create(arenas->transient);
    const uint64_t pixels = (uint64_t)row->width * row->height;
    uint8_t *diff = row->thresholds.emit_diff
                        ? arena_alloc(arenas->transient, pixels * 4u,
                                      ARENA_MEMORY_TAG_ARRAY)
                        : NULL;
    if (row->thresholds.emit_diff && !diff) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      return VKR_HARNESS_EXIT_ERROR;
    }
    const bool8_t rgba8_png =
        string_equals(row->canonical_encoding, "RGBA8_SRGB_PNG") ||
        string_equals(row->canonical_encoding, "RGBA8_UNORM") ||
        string_equals(row->canonical_encoding, "RGBA8_UNORM_PNG");
    const bool8_t rgba16f =
        string_equals(row->canonical_encoding, "RGBA16_FLOAT_LE");
    const bool8_t rg16f =
        string_equals(row->canonical_encoding, "RG16_FLOAT_LE");
    if (rgba8_png) {
      int actual_width = 0, actual_height = 0, actual_channels = 0;
      int baseline_width = 0, baseline_height = 0, baseline_channels = 0;
      uint8_t *actual_png = NULL;
      uint8_t *baseline_png = NULL;
      uint64_t actual_png_size = 0u;
      uint64_t baseline_png_size = 0u;
      if (!vkr_harness_read_file(actual_path, arenas->transient, &actual_png,
                                 &actual_png_size) ||
          !vkr_harness_read_file(baseline_path, arenas->transient,
                                 &baseline_png, &baseline_png_size) ||
          actual_png_size > INT32_MAX || baseline_png_size > INT32_MAX) {
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        return VKR_HARNESS_EXIT_ERROR;
      }
      uint8_t *actual_rgba =
          stbi_load_from_memory(actual_png, (int)actual_png_size, &actual_width,
                                &actual_height, &actual_channels, 4);
      uint8_t *baseline_rgba = stbi_load_from_memory(
          baseline_png, (int)baseline_png_size, &baseline_width,
          &baseline_height, &baseline_channels, 4);
      if (!actual_rgba || !baseline_rgba || actual_width != (int)row->width ||
          actual_height != (int)row->height || baseline_width != actual_width ||
          baseline_height != actual_height) {
        stbi_image_free(actual_rgba);
        stbi_image_free(baseline_rgba);
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        row->comparison.outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE;
        string_format(row->comparison_status, sizeof(row->comparison_status),
                      "incompatible");
        verdict = VKR_HARNESS_EXIT_MISSING_BASELINE;
        continue;
      }
      row->comparison = vkr_harness_compare_rgba8(
          actual_rgba, baseline_rgba, pixels, &row->thresholds, diff);
      stbi_image_free(actual_rgba);
      stbi_image_free(baseline_rgba);
    } else {
      uint8_t *actual_bytes = NULL;
      uint8_t *baseline_bytes = NULL;
      uint64_t actual_size = 0u;
      uint64_t baseline_size = 0u;
      if (!vkr_harness_read_file(actual_path, arenas->transient, &actual_bytes,
                                 &actual_size) ||
          !vkr_harness_read_file(baseline_path, arenas->transient,
                                 &baseline_bytes, &baseline_size) ||
          actual_size != pixels * (rgba16f ? 8u : 4u) ||
          baseline_size != actual_size) {
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        row->comparison.outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE;
        string_format(row->comparison_status, sizeof(row->comparison_status),
                      "incompatible");
        verdict = VKR_HARNESS_EXIT_MISSING_BASELINE;
        continue;
      }
      row->comparison =
          rgba16f
              ? vkr_harness_compare_rgba16f_le(actual_bytes, baseline_bytes,
                                               pixels, &row->thresholds, diff)
          : rg16f ? vkr_harness_compare_rg16f_le(actual_bytes, baseline_bytes,
                                                 pixels, &row->thresholds, diff)
          : string_equals(row->value_kind, "depth")
              ? vkr_harness_compare_f32_le(actual_bytes, baseline_bytes, pixels,
                                           &row->thresholds, diff)
              : vkr_harness_compare_u32_le(actual_bytes, baseline_bytes, pixels,
                                           diff);
    }
    string_format(row->comparison_status, sizeof(row->comparison_status), "%s",
                  vkr_harness_comparison_outcome_name(row->comparison.outcome));
    if (row->comparison.outcome == VKR_HARNESS_COMPARISON_INCOMPATIBLE) {
      verdict = VKR_HARNESS_EXIT_MISSING_BASELINE;
    } else if (row->comparison.outcome == VKR_HARNESS_COMPARISON_FAIL &&
               verdict == VKR_HARNESS_EXIT_PASS) {
      verdict = VKR_HARNESS_EXIT_FAIL;
    }
    if (diff && row->comparison.outcome != VKR_HARNESS_COMPARISON_PASS) {
      char diff_dir[VKR_HARNESS_PATH_MAX];
      char diff_path[VKR_HARNESS_PATH_MAX];
      string_format(diff_dir, sizeof(diff_dir), "%s/diffs", actual_root);
      string_format(row->diff_path, sizeof(row->diff_path),
                    "diffs/frame_%06u_%s.png", row->checkpoint_frame,
                    row->channel);
      string_format(diff_path, sizeof(diff_path), "%s/%s", actual_root,
                    row->diff_path);
      if (!vkr_harness_make_directories(diff_dir, error) ||
          !vkr_harness_capture_png_write(diff_path, diff, row->width,
                                         row->height, arenas, error) ||
          !vkr_harness_sha256_file(diff_path, row->diff_sha256)) {
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        return VKR_HARNESS_EXIT_ERROR;
      }
    }
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  }
  if (actual_count != baseline_count && verdict == VKR_HARNESS_EXIT_PASS) {
    verdict = VKR_HARNESS_EXIT_MISSING_BASELINE;
  }
  return verdict;
}

/*
 * perf-compare: a frame-time verdict between two profile reports.
 *
 * A published report holds tens of thousands of JSON tokens, far beyond the
 * harness token parser's budget, so these readers walk the report text with
 * the lib reader and take only the members they name. Every view below
 * borrows the report bytes, which live on the command's arena until it exits.
 */

#define VKR_HARNESS_PERF_MAX_ROWS (VKR_HARNESS_PERF_MAX_METRICS + 8u)
#define VKR_HARNESS_PERF_MAX_WARNINGS 48u
#define VKR_HARNESS_PERF_WARNING_MAX 192u
#define VKR_HARNESS_PERF_MAX_REASONS 8u
#define VKR_HARNESS_PERF_WORK_EXAMPLES 8u
#define VKR_HARNESS_PERF_OUTPUT_MAX MB(1)
/** Significant digits for the values an agent reads, not round-trip ones. */
#define VKR_HARNESS_PERF_DIGITS 6u
#define VKR_HARNESS_PERF_CONDITION_COUNT                                       \
  (VKR_HARNESS_CONDITION_METRIC_COUNT + VKR_HARNESS_DIAGNOSTIC_METRIC_COUNT +  \
   VKR_HARNESS_OUTPUT_CLASS_COUNT)

/** Rows every comparison reads when both reports carry valid samples. */
vkr_global const char *const vkr_harness_perf_default_metrics[] = {
    "frame.wall",        "gpu.submission", "cpu.frame_work",
    "cpu.render_submit", "cpu.update",     "cpu.render_wait",
};

typedef struct VkrHarnessPerfStats {
  char unit[16];
  uint64_t sample_count;
  float64_t mean;
  float64_t p50;
  float64_t p95;
  float64_t stddev;
} VkrHarnessPerfStats;

/** A system condition or diagnostic count; unavailable is not zero. */
typedef struct VkrHarnessPerfCondition {
  float64_t value;
  bool8_t available;
} VkrHarnessPerfCondition;

typedef struct VkrHarnessPerfReport {
  char path[VKR_HARNESS_PATH_MAX];
  char sha256[VKR_HARNESS_DIGEST_MAX];
  char status[24];
  char present_mode[24];
  char environment[VKR_HARNESS_DIGEST_MAX];
  char workload[VKR_HARNESS_DIGEST_MAX];
  char policy[VKR_HARNESS_DIGEST_MAX];
  bool8_t authoritative;
  /* Pure submission-timing profiles hold only current-frame work rows to
     determinism, as the parent's work-volume check does. */
  bool8_t current_frame_work_only;
  String8 root;
  String8 metrics;
  String8 passes;
  String8 authority_reasons;
  /* `aggregate.metrics` of each repetition whose report matched its digest. */
  String8 run_metrics[VKR_HARNESS_MAX_RUNS];
  uint32_t run_count;
  uint32_t run_reference_count;
  VkrHarnessPerfCondition conditions[VKR_HARNESS_PERF_CONDITION_COUNT];
} VkrHarnessPerfReport;

typedef struct VkrHarnessPerfMetricRow {
  char name[VKR_HARNESS_PERF_NAME_MAX];
  char unit[16];
  bool8_t duration;
  /* Waits and sleeps are idle time: reported, but not work that decides the
     verdict, since less CPU work can lengthen a wait for the GPU. */
  bool8_t idle;
  VkrHarnessPerfStats before;
  VkrHarnessPerfStats after;
  VkrHarnessPerfSpread before_spread;
  VkrHarnessPerfSpread after_spread;
  VkrHarnessPerfChange change;
} VkrHarnessPerfMetricRow;

typedef struct VkrHarnessPerfPassRow {
  char name[VKR_HARNESS_PERF_NAME_MAX];
  float64_t before_ms;
  float64_t after_ms;
  bool8_t in_before;
  bool8_t in_after;
  bool8_t gpu_before;
  bool8_t gpu_after;
} VkrHarnessPerfPassRow;

typedef struct VkrHarnessPerfWorkChange {
  char name[VKR_HARNESS_PERF_NAME_MAX];
  float64_t before_mean;
  float64_t after_mean;
  bool8_t in_before;
  bool8_t in_after;
} VkrHarnessPerfWorkChange;

typedef struct VkrHarnessPerfComparison {
  const VkrHarnessPerfOptions *options;
  VkrHarnessPerfReport before;
  VkrHarnessPerfReport after;
  VkrHarnessPerfMetricRow rows[VKR_HARNESS_PERF_MAX_ROWS];
  uint32_t row_count;
  VkrHarnessPerfPassRow passes[VKR_HARNESS_PERF_MAX_PASSES];
  uint32_t pass_count;
  uint32_t comparable_pass_count;
  uint32_t work_compared;
  uint32_t work_changed;
  VkrHarnessPerfWorkChange work_examples[VKR_HARNESS_PERF_WORK_EXAMPLES];
  uint32_t work_example_count;
  char reasons[VKR_HARNESS_PERF_MAX_REASONS][VKR_HARNESS_PERF_WARNING_MAX];
  uint32_t reason_count;
  char warnings[VKR_HARNESS_PERF_MAX_WARNINGS][VKR_HARNESS_PERF_WARNING_MAX];
  uint32_t warning_count;
  const char *primary;
  const char *primary_reason;
  const char *verdict;
  const char *evidence;
  bool8_t authoritative;
} VkrHarnessPerfComparison;

/** Bounded in-memory sink, so stdout never receives a partial document. */
typedef struct VkrHarnessPerfText {
  char *data;
  uint64_t length;
  uint64_t capacity;
} VkrHarnessPerfText;

VkrHarnessPerfChange
vkr_harness_perf_classify(const VkrHarnessPerfSpread *before,
                          const VkrHarnessPerfSpread *after,
                          float64_t threshold_pct) {
  if (!before || !after || !(before->p50 > 0.0)) {
    return VKR_HARNESS_PERF_NO_CHANGE;
  }
  const float64_t delta = after->p50 - before->p50;
  const float64_t delta_pct = 100.0 * delta / before->p50;
  if (!vkr_is_finite_f64(delta_pct) || vkr_abs_f64(delta_pct) < threshold_pct ||
      delta == 0.0) {
    return VKR_HARNESS_PERF_NO_CHANGE;
  }

  bool8_t separated = false_v;
  if (before->run_count >= 2u && after->run_count >= 2u) {
    separated = delta < 0.0 ? after->run_max < before->run_min
                            : after->run_min > before->run_max;
  } else {
    separated = vkr_abs_f64(delta) > 2.0 * Max(before->stddev, after->stddev);
  }
  if (!separated) {
    return VKR_HARNESS_PERF_NO_CHANGE;
  }
  return delta < 0.0 ? VKR_HARNESS_PERF_DECREASED : VKR_HARNESS_PERF_INCREASED;
}

vkr_internal void vkr_harness_perf_warn(VkrHarnessPerfComparison *comparison,
                                        const char *format, ...) {
  if (comparison->warning_count >= VKR_HARNESS_PERF_MAX_WARNINGS) {
    return;
  }
  char *warning = comparison->warnings[comparison->warning_count];
  va_list args;
  va_start(args, format);
  string_format_v(warning, VKR_HARNESS_PERF_WARNING_MAX, format, args);
  va_end(args);
  for (uint32_t i = 0u; i < comparison->warning_count; ++i) {
    if (string_equals(comparison->warnings[i], warning)) {
      return;
    }
  }
  comparison->warning_count++;
}

vkr_internal void vkr_harness_perf_reason(VkrHarnessPerfComparison *comparison,
                                          const char *side, const char *reason,
                                          const char *detail) {
  if (comparison->reason_count >= VKR_HARNESS_PERF_MAX_REASONS) {
    return;
  }
  char *out = comparison->reasons[comparison->reason_count++];
  if (side) {
    string_format(out, VKR_HARNESS_PERF_WARNING_MAX, "%s.%s:%s", side, reason,
                  detail);
  } else {
    string_format(out, VKR_HARNESS_PERF_WARNING_MAX, "%s", reason);
  }
}

/** Copies a JSON string view; false when it does not fit. */
vkr_internal bool8_t vkr_harness_perf_copy(String8 value, char *out,
                                           uint64_t capacity) {
  if (value.length >= capacity) {
    return false_v;
  }
  MemCopy(out, value.str, value.length);
  out[value.length] = '\0';
  return true_v;
}

/** Positions a reader at the value of a member of `object` itself. */
vkr_internal bool8_t vkr_harness_perf_member(String8 object, const char *name,
                                             VkrJsonReader *out_value) {
  VkrJsonReader reader = vkr_json_reader_from_string(object);
  if (!vkr_json_find_root_field(&reader, name)) {
    return false_v;
  }
  *out_value = reader;
  return true_v;
}

/** An object or array member; `null` and scalars are misses. */
vkr_internal bool8_t vkr_harness_perf_composite(String8 object,
                                                const char *name,
                                                String8 *out_value) {
  VkrJsonReader reader = {0};
  return vkr_harness_perf_member(object, name, &reader) &&
         vkr_json_capture_composite(&reader, out_value);
}

vkr_internal bool8_t vkr_harness_perf_number(String8 object, const char *name,
                                             float64_t *out_value) {
  VkrJsonReader reader = {0};
  return vkr_harness_perf_member(object, name, &reader) &&
         vkr_json_parse_double(&reader, out_value);
}

vkr_internal bool8_t vkr_harness_perf_flag(String8 object, const char *name,
                                           bool8_t *out_value) {
  VkrJsonReader reader = {0};
  return vkr_harness_perf_member(object, name, &reader) &&
         vkr_json_parse_bool(&reader, out_value);
}

/** A string member as raw text; report codes and digests carry no escapes. */
vkr_internal bool8_t vkr_harness_perf_text(String8 object, const char *name,
                                           char *out, uint64_t capacity) {
  VkrJsonReader reader = {0};
  String8 value = {0};
  return vkr_harness_perf_member(object, name, &reader) &&
         vkr_json_parse_string(&reader, &value) &&
         vkr_harness_perf_copy(value, out, capacity);
}

/** Positions `cursor` after the opening delimiter of an object or array. */
vkr_internal bool8_t vkr_harness_perf_begin(String8 composite, uint8_t open,
                                            VkrJsonReader *cursor) {
  *cursor = vkr_json_reader_from_string(composite);
  vkr_json_skip_whitespace(cursor);
  if (cursor->pos >= cursor->length || cursor->data[cursor->pos] != open) {
    return false_v;
  }
  cursor->pos++;
  return true_v;
}

/** The next object element, or a string element without its quotes. */
vkr_internal bool8_t vkr_harness_perf_array_next(VkrJsonReader *cursor,
                                                 String8 *out_element) {
  if (!vkr_json_next_array_element(cursor)) {
    return false_v;
  }
  if (cursor->data[cursor->pos] == '"') {
    return vkr_json_parse_string(cursor, out_element);
  }
  return vkr_json_capture_composite(cursor, out_element);
}

/** The next member of an object whose values are all objects. */
vkr_internal bool8_t vkr_harness_perf_object_next(VkrJsonReader *cursor,
                                                  String8 *out_key,
                                                  String8 *out_value) {
  vkr_json_skip_whitespace(cursor);
  if (cursor->pos < cursor->length && cursor->data[cursor->pos] == ',') {
    cursor->pos++;
  }
  if (!vkr_json_parse_string(cursor, out_key)) {
    return false_v;
  }
  vkr_json_skip_whitespace(cursor);
  if (cursor->pos >= cursor->length || cursor->data[cursor->pos] != ':') {
    return false_v;
  }
  cursor->pos++;
  return vkr_json_capture_composite(cursor, out_value);
}

vkr_internal bool8_t vkr_harness_perf_stats_parse(String8 object,
                                                  VkrHarnessPerfStats *out) {
  float64_t sample_count = 0.0;
  MemZero(out, sizeof(*out));
  if (!vkr_harness_perf_text(object, "unit", out->unit, sizeof(out->unit)) ||
      !vkr_harness_perf_number(object, "sample_count", &sample_count) ||
      !vkr_harness_perf_number(object, "mean", &out->mean) ||
      !vkr_harness_perf_number(object, "p50", &out->p50) ||
      !vkr_harness_perf_number(object, "p95", &out->p95) ||
      !vkr_harness_perf_number(object, "stddev", &out->stddev) ||
      !(sample_count >= 0.0)) {
    return false_v;
  }
  out->sample_count = (uint64_t)sample_count;
  return true_v;
}

/** A metric with at least one valid sample in `metrics`. */
vkr_internal bool8_t vkr_harness_perf_stats_find(String8 metrics,
                                                 const char *name,
                                                 VkrHarnessPerfStats *out) {
  String8 object = {0};
  return vkr_harness_perf_composite(metrics, name, &object) &&
         vkr_harness_perf_stats_parse(object, out) && out->sample_count > 0u;
}

/** Writes the flat name perf-compare reports a condition slot under. */
vkr_internal void vkr_harness_perf_condition_name(uint32_t index, char *out,
                                                  uint64_t capacity) {
  const uint32_t diagnostics_first = VKR_HARNESS_CONDITION_METRIC_COUNT;
  const uint32_t output_first =
      diagnostics_first + VKR_HARNESS_DIAGNOSTIC_METRIC_COUNT;
  if (index < diagnostics_first) {
    string_format(
        out, capacity, "%s",
        vkr_harness_condition_metric_name((VkrHarnessConditionMetric)index));
  } else if (index < output_first) {
    string_format(out, capacity, "%s",
                  vkr_harness_diagnostic_metric_name(
                      (VkrHarnessDiagnosticMetric)(index - diagnostics_first)));
  } else {
    string_format(out, capacity, "output.%s",
                  vkr_harness_output_class_name(
                      (VkrHarnessOutputClass)(index - output_first)));
  }
}

/**
 * Reads `system_conditions` and `diagnostic_counts`. A report without them, or
 * a gauge written as `null`, leaves the slot unavailable rather than zero.
 */
vkr_internal void vkr_harness_perf_conditions_read(String8 root,
                                                   VkrHarnessPerfReport *out) {
  String8 section = {0};
  String8 metrics = {0};
  String8 row = {0};
  if (vkr_harness_perf_composite(root, "system_conditions", &section) &&
      vkr_harness_perf_composite(section, "metrics", &metrics)) {
    for (uint32_t i = 0u; i < VKR_HARNESS_CONDITION_METRIC_COUNT; ++i) {
      /* Levels report their highest frame, flags the frames they were on. */
      const char *value_name =
          vkr_harness_condition_metric_is_level((VkrHarnessConditionMetric)i)
              ? "max"
              : "active_frames";
      VkrHarnessPerfCondition *slot = &out->conditions[i];
      slot->available =
          vkr_harness_perf_composite(
              metrics,
              vkr_harness_condition_metric_name((VkrHarnessConditionMetric)i),
              &row) &&
          vkr_harness_perf_number(row, value_name, &slot->value);
    }
  }
  if (!vkr_harness_perf_composite(root, "diagnostic_counts", &section)) {
    return;
  }

  const uint32_t diagnostics_first = VKR_HARNESS_CONDITION_METRIC_COUNT;
  if (vkr_harness_perf_composite(section, "metrics", &metrics)) {
    for (uint32_t i = 0u; i < VKR_HARNESS_DIAGNOSTIC_METRIC_COUNT; ++i) {
      VkrHarnessPerfCondition *slot = &out->conditions[diagnostics_first + i];
      slot->available =
          vkr_harness_perf_composite(
              metrics,
              vkr_harness_diagnostic_metric_name((VkrHarnessDiagnosticMetric)i),
              &row) &&
          vkr_harness_perf_number(row, "total", &slot->value);
    }
  }
  const uint32_t output_first =
      diagnostics_first + VKR_HARNESS_DIAGNOSTIC_METRIC_COUNT;
  if (vkr_harness_perf_composite(section, "output", &metrics)) {
    for (uint32_t i = 0u; i < VKR_HARNESS_OUTPUT_CLASS_COUNT; ++i) {
      VkrHarnessPerfCondition *slot = &out->conditions[output_first + i];
      slot->available = vkr_harness_perf_number(
          metrics, vkr_harness_output_class_name((VkrHarnessOutputClass)i),
          &slot->value);
    }
  }
}

/**
 * Loads each repetition's own report through the parent's `runs[]` rows. A
 * report whose bytes no longer match the recorded digest supplies nothing, so
 * a replaced or truncated child can never widen or narrow the spread.
 */
vkr_internal void vkr_harness_perf_runs_read(Arena *arena,
                                             VkrHarnessPerfReport *report) {
  String8 runs = {0};
  String8 run = {0};
  VkrJsonReader cursor = {0};
  char run_root[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_perf_composite(report->root, "runs", &runs) ||
      !vkr_harness_perf_begin(runs, '[', &cursor)) {
    return;
  }
  if (!vkr_harness_path_parent(report->path, run_root)) {
    string_format(run_root, sizeof(run_root), ".");
  }
  while (report->run_reference_count < VKR_HARNESS_MAX_RUNS &&
         vkr_harness_perf_array_next(&cursor, &run)) {
    report->run_reference_count++;
    char relative[VKR_HARNESS_RELATIVE_PATH_MAX];
    char recorded[VKR_HARNESS_DIGEST_MAX];
    char digest[VKR_HARNESS_DIGEST_MAX];
    char path[VKR_HARNESS_PATH_MAX];
    if (!vkr_harness_perf_text(run, "report", relative, sizeof(relative)) ||
        !vkr_harness_perf_text(run, "sha256", recorded, sizeof(recorded)) ||
        !vkr_harness_path_is_safe_relative(relative)) {
      continue;
    }
    const int32_t written =
        string_format(path, sizeof(path), "%s/%s", run_root, relative);
    uint8_t *data = NULL;
    uint64_t length = 0u;
    if (written <= 0 || (uint64_t)written >= sizeof(path) ||
        !vkr_harness_read_file(path, arena, &data, &length)) {
      continue;
    }
    vkr_harness_sha256_bytes(data, length, digest);
    const String8 child = {.str = data, .length = length};
    String8 aggregate = {0};
    String8 metrics = {0};
    if (string_equals(digest, recorded) &&
        vkr_harness_perf_composite(child, "aggregate", &aggregate) &&
        vkr_harness_perf_composite(aggregate, "metrics", &metrics)) {
      report->run_metrics[report->run_count++] = metrics;
    }
  }
}

/** Accepts a `.json` report path or a run directory holding `report.json`. */
vkr_internal bool8_t vkr_harness_perf_report_read(
    Arena *arena, const char *path, VkrHarnessPerfReport *report) {
  const uint64_t length = string_length(path);
  const bool8_t json_path =
      length > 5u && string_equals(path + length - 5u, ".json");
  const int32_t written =
      json_path ? string_format(report->path, sizeof(report->path), "%s", path)
                : string_format(report->path, sizeof(report->path),
                                "%s/report.json", path);
  uint8_t *data = NULL;
  uint64_t size = 0u;
  if (written <= 0 || (uint64_t)written >= sizeof(report->path) ||
      !vkr_harness_read_file(report->path, arena, &data, &size)) {
    vkr_harness_stderr("perf-compare: unable to read report '%s'\n",
                       report->path);
    return false_v;
  }
  vkr_harness_sha256_bytes(data, size, report->sha256);
  report->root = (String8){.str = data, .length = size};

  char kind[32];
  String8 comparison = {0};
  String8 aggregate = {0};
  String8 config = {0};
  bool8_t gpu_timing = false_v;
  bool8_t submission_gpu_timing = false_v;
  const String8 root = report->root;
  if (!vkr_harness_perf_text(root, "kind", kind, sizeof(kind)) ||
      !string_equals(kind, "vkr.harness.report") ||
      !vkr_harness_perf_text(root, "status", report->status,
                             sizeof(report->status)) ||
      !vkr_harness_perf_flag(root, "authoritative", &report->authoritative) ||
      !vkr_harness_perf_composite(root, "authority_reasons",
                                  &report->authority_reasons) ||
      !vkr_harness_perf_composite(root, "comparison", &comparison) ||
      !vkr_harness_perf_text(comparison, "environment_fingerprint",
                             report->environment,
                             sizeof(report->environment)) ||
      !vkr_harness_perf_text(comparison, "workload_fingerprint",
                             report->workload, sizeof(report->workload)) ||
      !vkr_harness_perf_text(comparison, "policy_fingerprint", report->policy,
                             sizeof(report->policy)) ||
      !vkr_harness_perf_composite(root, "aggregate", &aggregate) ||
      !vkr_harness_perf_composite(aggregate, "metrics", &report->metrics) ||
      !vkr_harness_perf_composite(aggregate, "passes", &report->passes) ||
      !vkr_harness_perf_composite(root, "effective_config", &config) ||
      !vkr_harness_perf_text(config, "present_mode", report->present_mode,
                             sizeof(report->present_mode))) {
    vkr_harness_stderr("perf-compare: '%s' is not a readable profile report\n",
                       report->path);
    return false_v;
  }
  vkr_harness_perf_flag(config, "gpu_timing", &gpu_timing);
  vkr_harness_perf_flag(config, "submission_gpu_timing",
                        &submission_gpu_timing);
  report->current_frame_work_only = submission_gpu_timing && !gpu_timing;
  vkr_harness_perf_conditions_read(root, report);
  vkr_harness_perf_runs_read(arena, report);
  return true_v;
}

/** Rule 1: both runs passed and measured the same workload under one policy. */
vkr_internal void
vkr_harness_perf_compatibility(VkrHarnessPerfComparison *comparison) {
  const VkrHarnessPerfReport *before = &comparison->before;
  const VkrHarnessPerfReport *after = &comparison->after;
  if (!string_equals(before->status, "pass")) {
    vkr_harness_perf_reason(comparison, "before", "status", before->status);
  }
  if (!string_equals(after->status, "pass")) {
    vkr_harness_perf_reason(comparison, "after", "status", after->status);
  }
  if (!string_equals(before->workload, after->workload)) {
    vkr_harness_perf_reason(comparison, NULL, "workload_fingerprint_differs",
                            NULL);
  }
  if (!string_equals(before->policy, after->policy)) {
    vkr_harness_perf_reason(comparison, NULL, "policy_fingerprint_differs",
                            NULL);
  }
  if (!string_equals(before->environment, after->environment) &&
      !comparison->options->cross_environment) {
    vkr_harness_perf_reason(comparison, NULL, "environment_fingerprint_differs",
                            NULL);
  }
}

/** Duration units convert to milliseconds; any other unit keeps its own. */
vkr_internal float64_t vkr_harness_perf_unit_scale(const char *unit,
                                                   bool8_t *out_duration) {
  *out_duration = true_v;
  if (string_equals(unit, "ns")) {
    return 1.0e-6;
  }
  if (string_equals(unit, "us")) {
    return 1.0e-3;
  }
  if (string_equals(unit, "ms")) {
    return 1.0;
  }
  *out_duration = false_v;
  return 1.0;
}

vkr_internal void vkr_harness_perf_stats_scale(VkrHarnessPerfStats *stats,
                                               float64_t scale) {
  stats->mean *= scale;
  stats->p50 *= scale;
  stats->p95 *= scale;
  stats->stddev *= scale;
}

/**
 * The range of per-repetition medians, used only when every referenced
 * repetition supplied one; a partial set would understate the spread.
 */
vkr_internal VkrHarnessPerfSpread
vkr_harness_perf_spread(const VkrHarnessPerfReport *report, const char *name,
                        const VkrHarnessPerfStats *pooled, float64_t scale) {
  VkrHarnessPerfSpread spread = {
      .p50 = pooled->p50,
      .stddev = pooled->stddev,
  };
  uint32_t found = 0u;
  for (uint32_t i = 0u; i < report->run_count; ++i) {
    VkrHarnessPerfStats run = {0};
    if (!vkr_harness_perf_stats_find(report->run_metrics[i], name, &run)) {
      continue;
    }
    const float64_t median = run.p50 * scale;
    spread.run_min = found == 0u ? median : Min(spread.run_min, median);
    spread.run_max = found == 0u ? median : Max(spread.run_max, median);
    found++;
  }
  if (found > 0u && found == report->run_reference_count) {
    spread.run_count = found;
  } else {
    spread.run_min = 0.0;
    spread.run_max = 0.0;
  }
  return spread;
}

/** Rule 3: one metric row; a default row both reports lack is skipped. */
vkr_internal void vkr_harness_perf_row_add(VkrHarnessPerfComparison *comparison,
                                           const char *name,
                                           bool8_t requested) {
  for (uint32_t i = 0u; i < comparison->row_count; ++i) {
    if (string_equals(comparison->rows[i].name, name)) {
      return;
    }
  }
  VkrHarnessPerfStats before = {0};
  VkrHarnessPerfStats after = {0};
  if (!vkr_harness_perf_stats_find(comparison->before.metrics, name, &before) ||
      !vkr_harness_perf_stats_find(comparison->after.metrics, name, &after)) {
    if (requested) {
      vkr_harness_perf_warn(comparison, "metric_unavailable:%s", name);
    }
    return;
  }
  if (!string_equals(before.unit, after.unit)) {
    vkr_harness_perf_warn(comparison, "metric_unit_differs:%s", name);
    return;
  }
  if (comparison->row_count >= VKR_HARNESS_PERF_MAX_ROWS) {
    return;
  }

  VkrHarnessPerfMetricRow *row = &comparison->rows[comparison->row_count++];
  *row = (VkrHarnessPerfMetricRow){0};
  string_format(row->name, sizeof(row->name), "%s", name);
  const float64_t scale =
      vkr_harness_perf_unit_scale(before.unit, &row->duration);
  string_format(row->unit, sizeof(row->unit), "%s",
                row->duration ? "ms" : before.unit);
  row->idle = row->duration && (string_find(name, "wait") != NULL ||
                                string_find(name, "sleep") != NULL);
  vkr_harness_perf_stats_scale(&before, scale);
  vkr_harness_perf_stats_scale(&after, scale);
  row->before = before;
  row->after = after;
  row->before_spread =
      vkr_harness_perf_spread(&comparison->before, name, &before, scale);
  row->after_spread =
      vkr_harness_perf_spread(&comparison->after, name, &after, scale);
  row->change =
      vkr_harness_perf_classify(&row->before_spread, &row->after_spread,
                                comparison->options->threshold_pct);
}

vkr_internal VkrHarnessPerfPassRow *
vkr_harness_perf_pass_row(VkrHarnessPerfComparison *comparison,
                          const char *name) {
  for (uint32_t i = 0u; i < comparison->pass_count; ++i) {
    if (string_equals(comparison->passes[i].name, name)) {
      return &comparison->passes[i];
    }
  }
  if (comparison->pass_count >= VKR_HARNESS_PERF_MAX_PASSES) {
    vkr_harness_perf_warn(comparison, "pass_table_full");
    return NULL;
  }
  VkrHarnessPerfPassRow *row = &comparison->passes[comparison->pass_count++];
  *row = (VkrHarnessPerfPassRow){0};
  string_format(row->name, sizeof(row->name), "%s", name);
  return row;
}

vkr_internal void
vkr_harness_perf_passes_read(VkrHarnessPerfComparison *comparison,
                             const VkrHarnessPerfReport *report,
                             bool8_t after) {
  VkrJsonReader cursor = {0};
  String8 pass = {0};
  if (!vkr_harness_perf_begin(report->passes, '[', &cursor)) {
    return;
  }
  while (vkr_harness_perf_array_next(&cursor, &pass)) {
    char name[VKR_HARNESS_PERF_NAME_MAX];
    if (!vkr_harness_perf_text(pass, "name", name, sizeof(name))) {
      continue;
    }
    VkrHarnessPerfPassRow *row = vkr_harness_perf_pass_row(comparison, name);
    if (!row) {
      continue;
    }
    String8 gpu = {0};
    VkrHarnessPerfStats stats = {0};
    const bool8_t gpu_valid =
        vkr_harness_perf_composite(pass, "gpu_ms", &gpu) &&
        vkr_harness_perf_stats_parse(gpu, &stats) && stats.sample_count > 0u;
    if (after) {
      row->in_after = true_v;
      row->gpu_after = gpu_valid;
      row->after_ms = stats.p50;
    } else {
      row->in_before = true_v;
      row->gpu_before = gpu_valid;
      row->before_ms = stats.p50;
    }
  }
}

vkr_internal bool8_t
vkr_harness_perf_pass_comparable(const VkrHarnessPerfPassRow *row) {
  return row->in_before && row->in_after && row->gpu_before && row->gpu_after;
}

/** Comparable rows first, largest |GPU p50 delta| first, then by name. */
vkr_internal int32_t vkr_harness_perf_pass_order(const void *lhs,
                                                 const void *rhs) {
  const VkrHarnessPerfPassRow *a = lhs;
  const VkrHarnessPerfPassRow *b = rhs;
  const bool8_t a_comparable = vkr_harness_perf_pass_comparable(a);
  const bool8_t b_comparable = vkr_harness_perf_pass_comparable(b);
  if (a_comparable != b_comparable) {
    return a_comparable ? -1 : 1;
  }
  if (a_comparable) {
    const float64_t a_delta = vkr_abs_f64(a->after_ms - a->before_ms);
    const float64_t b_delta = vkr_abs_f64(b->after_ms - b->before_ms);
    if (a_delta != b_delta) {
      return a_delta > b_delta ? -1 : 1;
    }
  }
  return string_compare(a->name, b->name);
}

/** Rule 4: pass rows matched by name, sorted for the top-N listing. */
vkr_internal void
vkr_harness_perf_passes(VkrHarnessPerfComparison *comparison) {
  vkr_harness_perf_passes_read(comparison, &comparison->before, false_v);
  vkr_harness_perf_passes_read(comparison, &comparison->after, true_v);
  vkr_sort(comparison->passes, comparison->pass_count,
           sizeof(comparison->passes[0]), vkr_harness_perf_pass_order);
  bool8_t shared = false_v;
  for (uint32_t i = 0u; i < comparison->pass_count; ++i) {
    const VkrHarnessPerfPassRow *row = &comparison->passes[i];
    shared = shared || (row->in_before && row->in_after);
    if (vkr_harness_perf_pass_comparable(row)) {
      comparison->comparable_pass_count++;
    }
  }
  if (shared && comparison->comparable_pass_count == 0u) {
    vkr_harness_perf_warn(comparison, "pass_gpu_timing_unavailable");
  }
}

vkr_internal void
vkr_harness_perf_work_example(VkrHarnessPerfComparison *comparison,
                              const char *name, String8 before, String8 after) {
  comparison->work_changed++;
  if (comparison->work_example_count >= VKR_HARNESS_PERF_WORK_EXAMPLES) {
    return;
  }
  VkrHarnessPerfWorkChange *example =
      &comparison->work_examples[comparison->work_example_count++];
  *example = (VkrHarnessPerfWorkChange){
      .in_before = before.length > 0u,
      .in_after = after.length > 0u,
  };
  string_format(example->name, sizeof(example->name), "%s", name);
  if (example->in_before) {
    vkr_harness_perf_number(before, "mean", &example->before_mean);
  }
  if (example->in_after) {
    vkr_harness_perf_number(after, "mean", &example->after_mean);
  }
}

/**
 * Rule 5: the deterministic work rows the parent holds repetitions to. Both
 * reports come from one writer, so equal work serializes to equal bytes and
 * any difference in a row's statistics object is a work difference.
 */
vkr_internal void
vkr_harness_perf_work_volume(VkrHarnessPerfComparison *comparison) {
  const bool8_t current_frame_only =
      comparison->before.current_frame_work_only ||
      comparison->after.current_frame_work_only;
  for (uint32_t side = 0u; side < 2u; ++side) {
    const String8 own =
        side == 0u ? comparison->before.metrics : comparison->after.metrics;
    const String8 other =
        side == 0u ? comparison->after.metrics : comparison->before.metrics;
    VkrJsonReader cursor = {0};
    String8 key = {0};
    String8 value = {0};
    if (!vkr_harness_perf_begin(own, '{', &cursor)) {
      continue;
    }
    while (vkr_harness_perf_object_next(&cursor, &key, &value)) {
      char name[VKR_HARNESS_PERF_NAME_MAX];
      if (!vkr_harness_perf_copy(key, name, sizeof(name)) ||
          !(current_frame_only ? vkr_harness_metric_is_current_frame_work(name)
                               : vkr_harness_metric_is_work_volume(name))) {
        continue;
      }
      String8 match = {0};
      const bool8_t found = vkr_harness_perf_composite(other, name, &match);
      if (side == 0u) {
        comparison->work_compared++;
        if (!found || !string8_equals(&value, &match)) {
          vkr_harness_perf_work_example(comparison, name, value, match);
        }
      } else if (!found) {
        /* A row only the after report has; the before pass saw no match. */
        vkr_harness_perf_work_example(comparison, name, (String8){0}, value);
      }
    }
  }
  if (comparison->work_changed > 0u) {
    vkr_harness_perf_warn(comparison, "work_volume_changed");
  }
}

/** Rule 6: the run conditions each side measured under. */
vkr_internal void
vkr_harness_perf_conditions(VkrHarnessPerfComparison *comparison) {
  for (uint32_t i = 0u; i < VKR_HARNESS_PERF_CONDITION_COUNT; ++i) {
    const VkrHarnessPerfCondition *before = &comparison->before.conditions[i];
    const VkrHarnessPerfCondition *after = &comparison->after.conditions[i];
    /* Other processes' GPU use varies by a few percent between any two runs,
       so it differs only when the runs fall on opposite sides of the
       authority threshold. */
    const bool8_t thresholded = i == VKR_HARNESS_CONDITION_GPU_PEER_UTIL;
    const float64_t threshold = (float64_t)VKR_HARNESS_GPU_PEER_LOAD_PERCENT;
    const bool8_t same_value =
        thresholded ? (before->value > threshold) == (after->value > threshold)
                    : before->value == after->value;
    if (before->available == after->available &&
        (!before->available || same_value)) {
      continue;
    }
    char name[64];
    vkr_harness_perf_condition_name(i, name, sizeof(name));
    vkr_harness_perf_warn(comparison,
                          i < VKR_HARNESS_CONDITION_METRIC_COUNT
                              ? "conditions_differ:%s"
                              : "diagnostics_differ:%s",
                          name);
  }
}

vkr_internal const VkrHarnessPerfMetricRow *
vkr_harness_perf_row_find(const VkrHarnessPerfComparison *comparison,
                          const char *name) {
  for (uint32_t i = 0u; i < comparison->row_count; ++i) {
    if (string_equals(comparison->rows[i].name, name)) {
      return &comparison->rows[i];
    }
  }
  return NULL;
}

vkr_internal const char *
vkr_harness_perf_change_name(VkrHarnessPerfChange change, bool8_t duration) {
  switch (change) {
  case VKR_HARNESS_PERF_DECREASED:
    return duration ? "faster" : "lower";
  case VKR_HARNESS_PERF_INCREASED:
    return duration ? "slower" : "higher";
  case VKR_HARNESS_PERF_NO_CHANGE:
  default:
    return "no_change";
  }
}

/**
 * Rule 7: the primary row is `frame.wall` unless presentation paces it. FIFO
 * or a frame limiter holds `frame.wall` at the display interval, so the GPU
 * submission time (or, without it, the CPU frame work) shows the change.
 */
vkr_internal void
vkr_harness_perf_primary(VkrHarnessPerfComparison *comparison) {
  VkrHarnessPerfStats before_sleep = {0};
  VkrHarnessPerfStats after_sleep = {0};
  const bool8_t fifo = string_equals(comparison->before.present_mode, "fifo") ||
                       string_equals(comparison->after.present_mode, "fifo");
  const bool8_t limiter =
      (vkr_harness_perf_stats_find(comparison->before.metrics,
                                   "frame.limiter_sleep", &before_sleep) &&
       before_sleep.p50 > 0.0) ||
      (vkr_harness_perf_stats_find(comparison->after.metrics,
                                   "frame.limiter_sleep", &after_sleep) &&
       after_sleep.p50 > 0.0);
  comparison->primary_reason = fifo      ? "present_mode_fifo_paces_frame_wall"
                               : limiter ? "frame_limiter_paces_frame_wall"
                                         : NULL;
  static const char *const paced[] = {"gpu.submission", "cpu.frame_work",
                                      "frame.wall"};
  static const char *const unpaced[] = {"frame.wall", "gpu.submission",
                                        "cpu.frame_work"};
  const char *const *order = comparison->primary_reason ? paced : unpaced;
  comparison->primary = NULL;
  for (uint32_t i = 0u; i < ArrayCount(paced) && !comparison->primary; ++i) {
    if (vkr_harness_perf_row_find(comparison, order[i])) {
      comparison->primary = order[i];
    }
  }
}

/** Disagreeing work rows make a mixed result; otherwise the primary decides. */
vkr_internal void
vkr_harness_perf_verdict(VkrHarnessPerfComparison *comparison) {
  const VkrHarnessPerfReport *before = &comparison->before;
  const VkrHarnessPerfReport *after = &comparison->after;
  const bool8_t environment_changed =
      !string_equals(before->environment, after->environment);
  if (environment_changed) {
    vkr_harness_perf_warn(comparison, "environment_changed");
  }
  /* A matched pair needs one environment; a cross-environment comparison
     is evidence about two machines, never a speed claim. */
  comparison->authoritative =
      before->authoritative && after->authoritative && !environment_changed;
  comparison->evidence = comparison->authoritative          ? "authoritative"
                         : comparison->options->allow_local ? "local"
                                                            : "none";
  if (before->run_count < before->run_reference_count) {
    vkr_harness_perf_warn(comparison, "repetition_reports_unavailable:before");
  }
  if (after->run_count < after->run_reference_count) {
    vkr_harness_perf_warn(comparison, "repetition_reports_unavailable:after");
  }

  vkr_harness_perf_primary(comparison);
  const VkrHarnessPerfMetricRow *primary =
      comparison->primary
          ? vkr_harness_perf_row_find(comparison, comparison->primary)
          : NULL;
  bool8_t any_faster = false_v;
  bool8_t any_slower = false_v;
  for (uint32_t i = 0u; i < comparison->row_count; ++i) {
    const VkrHarnessPerfMetricRow *row = &comparison->rows[i];
    if (!row->duration || row->idle) {
      continue;
    }
    any_faster = any_faster || row->change == VKR_HARNESS_PERF_DECREASED;
    any_slower = any_slower || row->change == VKR_HARNESS_PERF_INCREASED;
  }
  if (!primary) {
    vkr_harness_perf_warn(comparison, "primary_metric_unavailable");
    comparison->verdict = "inconclusive";
  } else if (any_faster && any_slower) {
    comparison->verdict = "mixed";
  } else {
    comparison->verdict = vkr_harness_perf_change_name(primary->change, true_v);
  }
  if (!comparison->authoritative && !comparison->options->allow_local) {
    comparison->verdict = "inconclusive";
  }
}

vkr_internal bool8_t vkr_harness_perf_text_sink(void *context,
                                                const uint8_t *data,
                                                uint64_t length) {
  VkrHarnessPerfText *text = context;
  /* Room is kept for the closing newline and terminator. */
  if (length + 2u > text->capacity - text->length) {
    return false_v;
  }
  MemCopy(text->data + text->length, data, length);
  text->length += length;
  return true_v;
}

/** The non-finite marker `vkr_harness_perf_emit_value` writes as `null`. */
vkr_internal float64_t vkr_harness_perf_no_value(void) {
  const uint64_t bits = 0x7ff8000000000000ull;
  float64_t value = 0.0;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

/** A value an agent reads; a non-finite one is written as `null`. */
vkr_internal bool8_t vkr_harness_perf_emit_value(VkrJsonWriter *writer,
                                                 const char *name,
                                                 float64_t value) {
  if (!vkr_harness_json_emit_name(writer, name)) {
    return false_v;
  }
  if (!vkr_is_finite_f64(value)) {
    return vkr_json_writer_null(writer);
  }
  return vkr_json_writer_f64_digits(writer, value, VKR_HARNESS_PERF_DIGITS);
}

vkr_internal bool8_t vkr_harness_perf_emit_view(VkrJsonWriter *writer,
                                                String8 value) {
  return vkr_json_writer_string(writer, value);
}

vkr_internal bool8_t vkr_harness_perf_emit_strings(
    VkrJsonWriter *writer, const char *name,
    const char (*values)[VKR_HARNESS_PERF_WARNING_MAX], uint32_t count) {
  bool8_t ok = vkr_harness_json_emit_name(writer, name) &&
               vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0u; ok && i < count; ++i) {
    ok = vkr_json_writer_string(
        writer, string8_create_from_cstr((const uint8_t *)values[i],
                                         string_length(values[i])));
  }
  return ok && vkr_json_writer_end_array(writer);
}

/** Identifies one report: its path, digest, authority and repetitions. */
vkr_internal bool8_t vkr_harness_perf_emit_report(
    VkrJsonWriter *writer, const char *name, const VkrHarnessPerfReport *report,
    bool8_t identity_only) {
  bool8_t ok = vkr_harness_json_emit_name(writer, name) &&
               vkr_json_writer_begin_object(writer) &&
               vkr_harness_json_emit_string(writer, "report", report->path) &&
               vkr_harness_json_emit_string(writer, "sha256", report->sha256) &&
               vkr_harness_json_emit_string(writer, "status", report->status);
  if (ok && !identity_only) {
    VkrJsonReader cursor = {0};
    String8 reason = {0};
    ok = vkr_harness_json_emit_bool(writer, "authoritative",
                                    report->authoritative) &&
         vkr_harness_json_emit_name(writer, "authority_reasons") &&
         vkr_json_writer_begin_array(writer);
    if (ok && vkr_harness_perf_begin(report->authority_reasons, '[', &cursor)) {
      while (ok && vkr_harness_perf_array_next(&cursor, &reason)) {
        ok = vkr_harness_perf_emit_view(writer, reason);
      }
    }
    ok = ok && vkr_json_writer_end_array(writer) &&
         vkr_harness_json_emit_string(writer, "present_mode",
                                      report->present_mode) &&
         vkr_harness_json_emit_u64(writer, "repetitions",
                                   report->run_reference_count) &&
         vkr_harness_json_emit_u64(writer, "repetition_reports_read",
                                   report->run_count);
  }
  return ok && vkr_json_writer_end_object(writer);
}

vkr_internal bool8_t vkr_harness_perf_emit_side(
    VkrJsonWriter *writer, const char *name, const VkrHarnessPerfStats *stats,
    const VkrHarnessPerfSpread *spread) {
  bool8_t ok =
      vkr_harness_json_emit_name(writer, name) &&
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_perf_emit_value(writer, "p50", stats->p50) &&
      vkr_harness_perf_emit_value(writer, "p95", stats->p95) &&
      vkr_harness_perf_emit_value(writer, "mean", stats->mean) &&
      vkr_harness_perf_emit_value(writer, "stddev", stats->stddev) &&
      vkr_harness_json_emit_u64(writer, "sample_count", stats->sample_count);
  if (ok && spread->run_count > 0u) {
    ok = vkr_harness_json_emit_u64(writer, "repetitions", spread->run_count) &&
         vkr_harness_perf_emit_value(writer, "repetition_p50_min",
                                     spread->run_min) &&
         vkr_harness_perf_emit_value(writer, "repetition_p50_max",
                                     spread->run_max);
  }
  return ok && vkr_json_writer_end_object(writer);
}

vkr_internal bool8_t vkr_harness_perf_emit_metric(
    VkrJsonWriter *writer, const VkrHarnessPerfComparison *comparison,
    const VkrHarnessPerfMetricRow *row) {
  const float64_t delta = row->after.p50 - row->before.p50;
  /* A zero before median has no percentage; the writer prints it as null. */
  const float64_t delta_pct = row->before.p50 > 0.0
                                  ? 100.0 * delta / row->before.p50
                                  : vkr_harness_perf_no_value();
  const bool8_t ranges =
      row->before_spread.run_count >= 2u && row->after_spread.run_count >= 2u;
  const char *observed =
      vkr_harness_perf_change_name(row->change, row->duration);
  const bool8_t gated =
      !comparison->authoritative && !comparison->options->allow_local;
  return vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_string(writer, "name", row->name) &&
         vkr_harness_json_emit_string(writer, "unit", row->unit) &&
         vkr_harness_json_emit_string(writer, "role",
                                      !row->duration ? "count"
                                      : row->idle    ? "idle"
                                                     : "work") &&
         vkr_harness_perf_emit_side(writer, "before", &row->before,
                                    &row->before_spread) &&
         vkr_harness_perf_emit_side(writer, "after", &row->after,
                                    &row->after_spread) &&
         vkr_harness_perf_emit_value(
             writer, row->duration ? "delta_ms" : "delta", delta) &&
         vkr_harness_perf_emit_value(writer, "delta_pct", delta_pct) &&
         vkr_harness_json_emit_string(writer, "spread_rule",
                                      ranges ? "repetition_ranges"
                                             : "pooled_stddev") &&
         vkr_harness_json_emit_string(writer, "observed", observed) &&
         vkr_harness_json_emit_string(writer, "verdict",
                                      gated ? "inconclusive" : observed) &&
         vkr_json_writer_end_object(writer);
}

vkr_internal bool8_t vkr_harness_perf_emit_passes(
    VkrJsonWriter *writer, const VkrHarnessPerfComparison *comparison) {
  const uint32_t listed =
      Min(comparison->options->pass_limit, comparison->comparable_pass_count);
  bool8_t ok = vkr_harness_json_emit_name(writer, "passes") &&
               vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0u; ok && i < listed; ++i) {
    const VkrHarnessPerfPassRow *row = &comparison->passes[i];
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_string(writer, "name", row->name) &&
         vkr_harness_perf_emit_value(writer, "before_gpu_p50_ms",
                                     row->before_ms) &&
         vkr_harness_perf_emit_value(writer, "after_gpu_p50_ms",
                                     row->after_ms) &&
         vkr_harness_perf_emit_value(writer, "delta_ms",
                                     row->after_ms - row->before_ms) &&
         vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer);
  for (uint32_t side = 0u; ok && side < 2u; ++side) {
    ok = vkr_harness_json_emit_name(writer, side == 0u ? "passes_only_before"
                                                       : "passes_only_after") &&
         vkr_json_writer_begin_array(writer);
    for (uint32_t i = 0u; ok && i < comparison->pass_count; ++i) {
      const VkrHarnessPerfPassRow *row = &comparison->passes[i];
      const bool8_t only = side == 0u ? row->in_before && !row->in_after
                                      : row->in_after && !row->in_before;
      if (only) {
        ok = vkr_json_writer_string(
            writer, string8_create_from_cstr((const uint8_t *)row->name,
                                             string_length(row->name)));
      }
    }
    ok = ok && vkr_json_writer_end_array(writer);
  }
  return ok;
}

vkr_internal bool8_t vkr_harness_perf_emit_work(
    VkrJsonWriter *writer, const VkrHarnessPerfComparison *comparison) {
  bool8_t ok =
      vkr_harness_json_emit_name(writer, "work_volume") &&
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_u64(writer, "compared",
                                comparison->work_compared) &&
      vkr_harness_json_emit_u64(writer, "changed", comparison->work_changed) &&
      vkr_harness_json_emit_name(writer, "examples") &&
      vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0u; ok && i < comparison->work_example_count; ++i) {
    const VkrHarnessPerfWorkChange *example = &comparison->work_examples[i];
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_string(writer, "name", example->name) &&
         vkr_harness_json_emit_name(writer, "before_mean") &&
         (example->in_before
              ? vkr_json_writer_f64_digits(writer, example->before_mean,
                                           VKR_HARNESS_PERF_DIGITS)
              : vkr_json_writer_null(writer)) &&
         vkr_harness_json_emit_name(writer, "after_mean") &&
         (example->in_after
              ? vkr_json_writer_f64_digits(writer, example->after_mean,
                                           VKR_HARNESS_PERF_DIGITS)
              : vkr_json_writer_null(writer)) &&
         vkr_json_writer_end_object(writer);
  }
  return ok && vkr_json_writer_end_array(writer) &&
         vkr_json_writer_end_object(writer);
}

vkr_internal bool8_t vkr_harness_perf_emit_conditions(
    VkrJsonWriter *writer, const VkrHarnessPerfComparison *comparison) {
  bool8_t ok = vkr_harness_json_emit_name(writer, "conditions") &&
               vkr_json_writer_begin_object(writer);
  for (uint32_t side = 0u; ok && side < 2u; ++side) {
    const VkrHarnessPerfReport *report =
        side == 0u ? &comparison->before : &comparison->after;
    ok = vkr_harness_json_emit_name(writer, side == 0u ? "before" : "after") &&
         vkr_json_writer_begin_object(writer);
    for (uint32_t i = 0u; ok && i < VKR_HARNESS_PERF_CONDITION_COUNT; ++i) {
      const VkrHarnessPerfCondition *slot = &report->conditions[i];
      char name[64];
      vkr_harness_perf_condition_name(i, name, sizeof(name));
      ok = vkr_harness_json_emit_name(writer, name) &&
           (slot->available ? vkr_json_writer_f64_digits(
                                  writer, slot->value, VKR_HARNESS_PERF_DIGITS)
                            : vkr_json_writer_null(writer));
    }
    ok = ok && vkr_json_writer_end_object(writer);
  }
  return ok && vkr_json_writer_end_object(writer);
}

/** A string member, or `null` when `value` is NULL. */
vkr_internal bool8_t vkr_harness_perf_emit_optional(VkrJsonWriter *writer,
                                                    const char *name,
                                                    const char *value) {
  if (!value) {
    return vkr_harness_json_emit_name(writer, name) &&
           vkr_json_writer_null(writer);
  }
  return vkr_harness_json_emit_string(writer, name, value);
}

vkr_internal bool8_t vkr_harness_perf_emit(
    VkrJsonWriter *writer, const VkrHarnessPerfComparison *comparison) {
  if (comparison->reason_count > 0u) {
    return vkr_json_writer_begin_object(writer) &&
           vkr_harness_json_emit_string(writer, "verdict", "incompatible") &&
           vkr_harness_perf_emit_strings(writer, "reasons", comparison->reasons,
                                         comparison->reason_count) &&
           vkr_harness_perf_emit_report(writer, "before", &comparison->before,
                                        true_v) &&
           vkr_harness_perf_emit_report(writer, "after", &comparison->after,
                                        true_v) &&
           vkr_json_writer_end_object(writer);
  }
  bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_string(writer, "verdict", comparison->verdict) &&
      vkr_harness_json_emit_bool(writer, "authoritative",
                                 comparison->authoritative) &&
      vkr_harness_json_emit_string(writer, "evidence", comparison->evidence) &&
      vkr_harness_perf_emit_optional(writer, "primary", comparison->primary) &&
      vkr_harness_perf_emit_optional(writer, "primary_reason",
                                     comparison->primary_reason) &&
      vkr_harness_perf_emit_value(writer, "threshold_pct",
                                  comparison->options->threshold_pct) &&
      vkr_harness_perf_emit_report(writer, "before", &comparison->before,
                                   false_v) &&
      vkr_harness_perf_emit_report(writer, "after", &comparison->after,
                                   false_v) &&
      vkr_harness_json_emit_name(writer, "metrics") &&
      vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0u; ok && i < comparison->row_count; ++i) {
    ok = vkr_harness_perf_emit_metric(writer, comparison, &comparison->rows[i]);
  }
  return ok && vkr_json_writer_end_array(writer) &&
         vkr_harness_perf_emit_passes(writer, comparison) &&
         vkr_harness_perf_emit_work(writer, comparison) &&
         vkr_harness_perf_emit_conditions(writer, comparison) &&
         vkr_harness_perf_emit_strings(writer, "warnings", comparison->warnings,
                                       comparison->warning_count) &&
         vkr_json_writer_end_object(writer);
}

/** Serializes the result into one buffer and prints it only when complete. */
vkr_internal bool8_t vkr_harness_perf_print(
    Arena *arena, const VkrHarnessPerfComparison *comparison) {
  VkrHarnessPerfText text = {
      .data = arena_alloc(arena, VKR_HARNESS_PERF_OUTPUT_MAX,
                          ARENA_MEMORY_TAG_STRING),
      .capacity = VKR_HARNESS_PERF_OUTPUT_MAX,
  };
  if (!text.data) {
    return false_v;
  }
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, vkr_harness_perf_text_sink, &text);
  if (!vkr_harness_perf_emit(&writer, comparison) ||
      !vkr_json_writer_complete(&writer)) {
    return false_v;
  }
  text.data[text.length++] = '\n';
  text.data[text.length] = '\0';
  vkr_platform_stdout_write(text.data);
  return true_v;
}

int vkr_harness_perf_compare_run(const char *before_path,
                                 const char *after_path,
                                 const VkrHarnessPerfOptions *options) {
  if (!before_path || !after_path || !options ||
      !vkr_is_finite_f64(options->threshold_pct) ||
      options->threshold_pct <= 0.0 ||
      options->pass_limit > VKR_HARNESS_PERF_MAX_PASSES ||
      options->metric_count > VKR_HARNESS_PERF_MAX_METRICS) {
    return VKR_HARNESS_EXIT_INVALID;
  }
  /* Both reports, every repetition report and the output buffer live on the
     arena; the comparison's views borrow them until the command returns. */
  Arena *arena = arena_create();
  int result = VKR_HARNESS_EXIT_INVALID;
  VkrHarnessPerfComparison *comparison =
      arena ? arena_alloc(arena, sizeof(*comparison), ARENA_MEMORY_TAG_STRUCT)
            : NULL;
  if (!comparison) {
    vkr_harness_stderr("perf-compare: unable to allocate the comparison\n");
    result = VKR_HARNESS_EXIT_ERROR;
    goto cleanup;
  }
  MemZero(comparison, sizeof(*comparison));
  comparison->options = options;
  if (!vkr_harness_perf_report_read(arena, before_path, &comparison->before) ||
      !vkr_harness_perf_report_read(arena, after_path, &comparison->after)) {
    goto cleanup;
  }

  vkr_harness_perf_compatibility(comparison);
  if (comparison->reason_count == 0u) {
    for (uint32_t i = 0u; i < ArrayCount(vkr_harness_perf_default_metrics);
         ++i) {
      vkr_harness_perf_row_add(comparison, vkr_harness_perf_default_metrics[i],
                               false_v);
    }
    for (uint32_t i = 0u; i < options->metric_count; ++i) {
      vkr_harness_perf_row_add(comparison, options->metrics[i], true_v);
    }
    vkr_harness_perf_passes(comparison);
    vkr_harness_perf_work_volume(comparison);
    vkr_harness_perf_conditions(comparison);
    vkr_harness_perf_verdict(comparison);
  }
  if (!vkr_harness_perf_print(arena, comparison)) {
    vkr_harness_stderr("perf-compare: unable to write the verdict\n");
    result = VKR_HARNESS_EXIT_ERROR;
    goto cleanup;
  }
  result = comparison->reason_count > 0u ? VKR_HARNESS_EXIT_MISSING_BASELINE
                                         : VKR_HARNESS_EXIT_PASS;

cleanup:
  if (arena) {
    arena_destroy(arena);
  }
  return result;
}
