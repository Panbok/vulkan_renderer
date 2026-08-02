#include "vkr_harness_runtime.h"

#include <stb_image.h>

static uint32_t vkr_harness_read_u32_le(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
         ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
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

int vkr_harness_compare_run(const char *repo_root, const char *run_path) {
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
  } else if (!string_equals(source.environment_fingerprint,
                            baseline.environment_fingerprint) ||
             !string_equals(source.workload_fingerprint,
                            baseline.workload_fingerprint) ||
             !string_equals(source.policy_fingerprint,
                            baseline.policy_fingerprint)) {
    vkr_harness_report_set_status(&report, "missing_baseline",
                                  VKR_HARNESS_EXIT_MISSING_BASELINE);
    vkr_harness_report_add_incompatibility(&report,
                                           "baseline.fingerprint_mismatch");
  } else {
    VkrHarnessArenas arenas = {.persistent = arena, .transient = transient};
    const VkrHarnessExitCode comparison = vkr_harness_compare_capture_sets(
        compare_root, baseline_root, report.captures, report.capture_count,
        baseline.captures, baseline.capture_count, &arenas, &error);
    vkr_harness_report_set_status(
        &report, vkr_harness_exit_code_name(comparison), comparison);
    vkr_harness_compare_publish_diffs(&report, compare_root);
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
    if (string_equals(row->value_kind, "color")) {
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
          actual_size != pixels * 4u || baseline_size != actual_size) {
        scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
        row->comparison.outcome = VKR_HARNESS_COMPARISON_INCOMPATIBLE;
        string_format(row->comparison_status, sizeof(row->comparison_status),
                      "incompatible");
        verdict = VKR_HARNESS_EXIT_MISSING_BASELINE;
        continue;
      }
      row->comparison =
          string_equals(row->value_kind, "depth")
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
