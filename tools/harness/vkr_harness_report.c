#include "vkr_harness.h"

static String8 vkr_harness_string(const char *value) {
  const char *safe = value ? value : "";
  return string8_create_from_cstr((const uint8_t *)safe, string_length(safe));
}

bool8_t vkr_harness_json_emit_name(VkrJsonWriter *writer, const char *name) {
  return vkr_json_writer_name(writer, vkr_harness_string(name));
}

bool8_t vkr_harness_json_emit_string(VkrJsonWriter *writer, const char *name,
                                     const char *value) {
  return vkr_harness_json_emit_name(writer, name) &&
         vkr_json_writer_string(writer, vkr_harness_string(value));
}

bool8_t vkr_harness_json_emit_u64(VkrJsonWriter *writer, const char *name,
                                  uint64_t value) {
  return vkr_harness_json_emit_name(writer, name) &&
         vkr_json_writer_u64(writer, value);
}

bool8_t vkr_harness_json_emit_i64(VkrJsonWriter *writer, const char *name,
                                  int64_t value) {
  return vkr_harness_json_emit_name(writer, name) &&
         vkr_json_writer_i64(writer, value);
}

bool8_t vkr_harness_json_emit_f64(VkrJsonWriter *writer, const char *name,
                                  float64_t value) {
  return vkr_harness_json_emit_name(writer, name) &&
         vkr_json_writer_f64(writer, value);
}

bool8_t vkr_harness_json_emit_bool(VkrJsonWriter *writer, const char *name,
                                   bool8_t value) {
  return vkr_harness_json_emit_name(writer, name) &&
         vkr_json_writer_bool(writer, value);
}

static bool8_t
vkr_harness_report_write_statistics_reason(VkrJsonWriter *writer,
                                           const VkrHarnessMetricResult *metric,
                                           const char *unavailable_reason) {
  const VkrHarnessStatistics *stats = &metric->statistics;
  bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_string(writer, "unit", metric->unit) &&
      vkr_harness_json_emit_u64(writer, "sample_count", stats->sample_count) &&
      vkr_harness_json_emit_u64(writer, "invalid_count",
                                stats->invalid_count) &&
      vkr_harness_json_emit_f64(writer, "mean", stats->mean) &&
      vkr_harness_json_emit_f64(writer, "p50", stats->p50) &&
      vkr_harness_json_emit_f64(writer, "p95", stats->p95) &&
      vkr_harness_json_emit_f64(writer, "min", stats->min) &&
      vkr_harness_json_emit_f64(writer, "max", stats->max) &&
      vkr_harness_json_emit_f64(writer, "stddev", stats->stddev) &&
      vkr_harness_json_emit_f64(writer, "total", stats->total);
  if (ok && unavailable_reason) {
    ok = vkr_harness_json_emit_string(writer, "unavailable_reason",
                                      unavailable_reason);
  }
  return ok && vkr_json_writer_end_object(writer);
}

static bool8_t
vkr_harness_report_write_statistics(VkrJsonWriter *writer,
                                    const VkrHarnessMetricResult *metric) {
  return vkr_harness_report_write_statistics_reason(writer, metric, NULL);
}

bool8_t vkr_harness_report_init_storage(VkrHarnessReport *report, Arena *arena,
                                        uint32_t capture_capacity,
                                        uint32_t artifact_capacity) {
  if (!report || !arena) {
    return false_v;
  }
  capture_capacity = Min(capture_capacity, VKR_HARNESS_MAX_CAPTURE_RESULTS);
  artifact_capacity = Min(artifact_capacity, VKR_HARNESS_MAX_ARTIFACTS);
  const uint64_t capture_bytes =
      (uint64_t)capture_capacity * sizeof(VkrHarnessCaptureResult);
  const uint64_t artifact_bytes =
      (uint64_t)artifact_capacity * sizeof(VkrHarnessArtifact);
  report->captures = capture_capacity ? arena_alloc(arena, capture_bytes,
                                                    ARENA_MEMORY_TAG_ARRAY)
                                      : NULL;
  report->artifacts = artifact_capacity ? arena_alloc(arena, artifact_bytes,
                                                      ARENA_MEMORY_TAG_ARRAY)
                                        : NULL;
  if ((capture_capacity && !report->captures) ||
      (artifact_capacity && !report->artifacts)) {
    return false_v;
  }
  /* Arenas bump rather than zero, and the report writer emits every field of
     each row it is told exists. */
  if (report->captures) {
    MemZero(report->captures, capture_bytes);
  }
  if (report->artifacts) {
    MemZero(report->artifacts, artifact_bytes);
  }
  report->capture_capacity = capture_capacity;
  report->artifact_capacity = artifact_capacity;
  report->capture_count = 0u;
  report->artifact_count = 0u;
  return true_v;
}

bool8_t vkr_harness_report_init_auxiliary_runs(VkrHarnessReport *report,
                                               Arena *arena,
                                               uint32_t capacity) {
  if (!report || !arena || capacity > VKR_HARNESS_MAX_REPLAYS) {
    return false_v;
  }
  const uint64_t bytes = (uint64_t)capacity * sizeof(*report->auxiliary_runs);
  report->auxiliary_runs =
      capacity ? arena_alloc(arena, bytes, ARENA_MEMORY_TAG_ARRAY) : NULL;
  if (capacity && !report->auxiliary_runs) {
    return false_v;
  }
  if (bytes) {
    MemZero(report->auxiliary_runs, bytes);
  }
  report->auxiliary_run_count = 0u;
  report->auxiliary_run_capacity = capacity;
  return true_v;
}

/** `"<name>":"sha256:<64 hex>"` within `object`; any other shape is a miss. */
static bool8_t
vkr_harness_report_scan_digest(const char *object, const char *name,
                               char out_digest[VKR_HARNESS_DIGEST_MAX]) {
  const uint64_t digest_length = VKR_HARNESS_DIGEST_MAX - 1u;
  char needle[128];
  if (string_format(needle, sizeof(needle), "\"%s\":\"", name) <= 0) {
    return false_v;
  }
  const char *found = string_find(object, needle);
  if (!found) {
    return false_v;
  }
  found += string_length(needle);
  if (!string_n_equals(found, "sha256:", 7u) ||
      string_length(found) <= digest_length || found[digest_length] != '"') {
    return false_v;
  }
  MemCopy(out_digest, found, digest_length);
  out_digest[digest_length] = '\0';
  return true_v;
}

bool8_t
vkr_harness_report_read_fingerprints(const char *report_path, Arena *transient,
                                     VkrHarnessRunReference *out_reference) {
  if (!report_path || !transient || !out_reference) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t length = 0u;
  char *text = NULL;
  Scratch scratch = scratch_create(transient);
  bool8_t ok = vkr_harness_read_file(report_path, transient, &bytes, &length);
  if (ok) {
    text = arena_alloc(transient, length + 1u, ARENA_MEMORY_TAG_STRING);
    ok = text != NULL;
  }
  if (ok) {
    static const char *const names[] = {"environment_fingerprint",
                                        "workload_fingerprint",
                                        "policy_fingerprint"};
    char *const values[] = {out_reference->environment_fingerprint,
                            out_reference->workload_fingerprint,
                            out_reference->policy_fingerprint};
    MemCopy(text, bytes, length);
    text[length] = '\0';
    ok = false_v;
    /* A report names `comparison` more than once — the run's own identity and
       one per capture row — so the match is the first such object that actually
       carries all three fingerprints, not the first that appears. Each holds
       only scalars, so its first closing brace bounds the search. */
    for (char *object = (char *)string_find(text, "\"comparison\":{");
         object && !ok;
         object = (char *)string_find(object + 1u, "\"comparison\":{")) {
      char *object_end = (char *)string_find_char(object, '}');
      if (!object_end) {
        break;
      }
      *object_end = '\0';
      ok = true_v;
      for (uint32_t i = 0; ok && i < ArrayCount(names); ++i) {
        ok = vkr_harness_report_scan_digest(object, names[i], values[i]);
      }
      *object_end = '}';
    }
  }
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return ok;
}

void vkr_harness_report_add_authority_reason(VkrHarnessReport *report,
                                             const char *reason) {
  report->authoritative = false_v;
  for (uint32_t i = 0; i < report->authority_reason_count; ++i) {
    if (string_equals(report->authority_reasons[i], reason)) {
      return;
    }
  }
  if (report->authority_reason_count >= VKR_HARNESS_MAX_AUTHORITY_REASONS) {
    return;
  }
  string_format(report->authority_reasons[report->authority_reason_count++],
                sizeof(report->authority_reasons[0]), "%s", reason);
}

void vkr_harness_report_add_incompatibility(VkrHarnessReport *report,
                                            const char *reason) {
  report->profile_compatible = false_v;
  vkr_harness_report_add_authority_reason(report, reason);
  for (uint32_t i = 0; i < report->incompatibility_reason_count; ++i) {
    if (string_equals(report->incompatibility_reasons[i], reason)) {
      return;
    }
  }
  if (report->incompatibility_reason_count <
      ArrayCount(report->incompatibility_reasons)) {
    string_format(
        report->incompatibility_reasons[report->incompatibility_reason_count++],
        sizeof(report->incompatibility_reasons[0]), "%s", reason);
  }
}

bool8_t vkr_harness_report_add_artifact(VkrHarnessReport *report,
                                        const char *role,
                                        const char *relative_path,
                                        const char *media_type,
                                        const char *absolute_path) {
  if (!report->artifacts ||
      report->artifact_count >= report->artifact_capacity ||
      string_length(relative_path) >= VKR_HARNESS_RELATIVE_PATH_MAX ||
      !vkr_harness_path_is_safe_relative(relative_path)) {
    return false_v;
  }
  VkrHarnessArtifact *artifact = &report->artifacts[report->artifact_count];
  if (!vkr_harness_sha256_file(absolute_path, artifact->sha256)) {
    return false_v;
  }
  string_format(artifact->role, sizeof(artifact->role), "%s", role);
  string_format(artifact->path, sizeof(artifact->path), "%s", relative_path);
  string_format(artifact->media_type, sizeof(artifact->media_type), "%s",
                media_type);
  string_format(artifact->status, sizeof(artifact->status), "complete");
  report->artifact_count++;
  return true_v;
}

void vkr_harness_report_set_status(VkrHarnessReport *report, const char *status,
                                   VkrHarnessExitCode exit_code) {
  string_format(report->status, sizeof(report->status), "%s", status);
  report->exit_code = exit_code;
}

void vkr_harness_report_mark_incomplete(VkrHarnessReport *report,
                                        const char *reason) {
  vkr_harness_report_set_status(report, "incomplete", VKR_HARNESS_EXIT_ERROR);
  if (reason) {
    vkr_harness_report_add_authority_reason(report, reason);
  }
}

void vkr_harness_report_mark_unavailable(VkrHarnessReport *report,
                                         const char *reason) {
  vkr_harness_report_add_incompatibility(report, reason);
  vkr_harness_report_set_status(report, "unavailable",
                                VKR_HARNESS_EXIT_UNAVAILABLE);
}

bool8_t vkr_harness_report_write(const char *path,
                                 const VkrHarnessReport *report,
                                 VkrHarnessError *out_error) {
  if (!path || !report) {
    return false_v;
  }
  VkrJsonFileWriter file_writer = {0};
  if (!vkr_json_file_writer_begin(&file_writer, vkr_harness_string(path))) {
    vkr_harness_error_set(out_error, "report.open", "$",
                          "Unable to begin report '%s'", path);
    return false_v;
  }
  VkrJsonWriter *writer = &file_writer.writer;
  char subsystem_mask[VKR_HARNESS_SUBSYSTEM_MASK_MAX];
  vkr_harness_format_subsystem_mask(subsystem_mask, report->subsystem_mask);
  bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_u64(writer, "schema_version",
                                VKR_HARNESS_SCHEMA_VERSION) &&
      vkr_harness_json_emit_string(writer, "kind", "vkr.harness.report") &&
      vkr_harness_json_emit_string(writer, "tool",
                                   vkr_harness_tool_name(report->tool)) &&
      vkr_harness_json_emit_string(writer, "tool_version", "1") &&
      vkr_harness_json_emit_string(writer, "run_id", report->run_id) &&
      vkr_harness_json_emit_string(writer, "status", report->status) &&
      vkr_harness_json_emit_u64(writer, "exit_code", report->exit_code) &&
      vkr_harness_json_emit_bool(writer, "authoritative",
                                 report->authoritative) &&
      vkr_harness_json_emit_name(writer, "authority_reasons") &&
      vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->authority_reason_count; ++i) {
    ok = vkr_json_writer_string(
        writer, vkr_harness_string(report->authority_reasons[i]));
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_harness_json_emit_name(writer, "case") &&
       vkr_json_writer_begin_object(writer) &&
       vkr_harness_json_emit_string(writer, "id", report->case_manifest.id) &&
       vkr_harness_json_emit_string(writer, "suite",
                                    report->case_manifest.suite) &&
       vkr_harness_json_emit_string(writer, "manifest_sha256",
                                    report->case_manifest.manifest_sha256) &&
       vkr_json_writer_end_object(writer) &&
       vkr_harness_json_emit_name(writer, "profile") &&
       vkr_json_writer_begin_object(writer) &&
       vkr_harness_json_emit_string(writer, "id", report->profile.id) &&
       vkr_harness_json_emit_string(writer, "manifest_sha256",
                                    report->profile.manifest_sha256) &&
       vkr_harness_json_emit_bool(writer, "compatible",
                                  report->profile_compatible) &&
       vkr_harness_json_emit_name(writer, "incompatibility_reasons") &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->incompatibility_reason_count; ++i) {
    ok = vkr_json_writer_string(
        writer, vkr_harness_string(report->incompatibility_reasons[i]));
  }
  ok =
      ok && vkr_json_writer_end_array(writer) &&
      vkr_json_writer_end_object(writer) &&
      vkr_harness_json_emit_name(writer, "provenance") &&
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_string(writer, "started_at",
                                   report->provenance.started_at) &&
      vkr_harness_json_emit_string(writer, "ended_at",
                                   report->provenance.ended_at) &&
      vkr_harness_json_emit_string(writer, "git_sha",
                                   report->provenance.git_sha) &&
      vkr_harness_json_emit_bool(writer, "dirty", report->provenance.dirty) &&
      vkr_harness_json_emit_string(writer, "binary_sha256",
                                   report->provenance.binary_sha256) &&
      vkr_harness_json_emit_string(writer, "build_type",
                                   report->provenance.build_type) &&
      vkr_harness_json_emit_string(writer, "compiler",
                                   report->provenance.compiler) &&
      vkr_harness_json_emit_string(writer, "os", report->provenance.os) &&
      vkr_harness_json_emit_string(writer, "cpu", report->provenance.cpu) &&
      vkr_harness_json_emit_string(writer, "gpu", report->provenance.gpu) &&
      vkr_harness_json_emit_u64(writer, "gpu_vendor_id",
                                report->provenance.gpu_vendor_id) &&
      vkr_harness_json_emit_u64(writer, "gpu_device_id",
                                report->provenance.gpu_device_id) &&
      vkr_harness_json_emit_string(writer, "driver",
                                   report->provenance.driver) &&
      vkr_harness_json_emit_string(writer, "world_renderer",
                                   report->provenance.world_renderer) &&
      vkr_harness_json_emit_string(writer, "power_mode",
                                   report->provenance.power_mode) &&
      vkr_harness_json_emit_string(writer, "thermal_state_start",
                                   report->provenance.thermal_state_start) &&
      vkr_harness_json_emit_string(writer, "thermal_state_end",
                                   report->provenance.thermal_state_end) &&
      vkr_harness_json_emit_i64(writer, "process_priority",
                                report->provenance.process_priority) &&
      vkr_json_writer_end_object(writer) &&
      vkr_harness_json_emit_name(writer, "comparison") &&
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_string(writer, "environment_fingerprint",
                                   report->environment_fingerprint) &&
      vkr_harness_json_emit_string(writer, "workload_fingerprint",
                                   report->workload_fingerprint) &&
      vkr_harness_json_emit_string(writer, "policy_fingerprint",
                                   report->policy_fingerprint) &&
      vkr_json_writer_end_object(writer) &&
      vkr_harness_json_emit_name(writer, "effective_config") &&
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_name(writer, "resolution") &&
      vkr_json_writer_begin_array(writer) &&
      vkr_json_writer_u64(writer, report->provenance.actual_target_width
                                      ? report->provenance.actual_target_width
                                      : report->case_manifest.width) &&
      vkr_json_writer_u64(writer, report->provenance.actual_target_height
                                      ? report->provenance.actual_target_height
                                      : report->case_manifest.height) &&
      vkr_json_writer_end_array(writer) &&
      (!report->case_manifest.resize_round_trip ||
       (vkr_harness_json_emit_name(writer, "resize_round_trip") &&
        vkr_json_writer_begin_array(writer) &&
        vkr_json_writer_u64(writer, report->case_manifest.resize_width) &&
        vkr_json_writer_u64(writer, report->case_manifest.resize_height) &&
        vkr_json_writer_end_array(writer))) &&
      vkr_harness_json_emit_string(writer, "scene",
                                   report->case_manifest.scene) &&
      vkr_harness_json_emit_string(
          writer, "target",
          vkr_harness_target_name(report->provenance.actual_target)) &&
      vkr_harness_json_emit_u64(writer, "target_image_count",
                                report->provenance.actual_target_image_count) &&
      vkr_harness_json_emit_string(writer, "color_format",
                                   report->provenance.color_format) &&
      vkr_harness_json_emit_string(writer, "depth_format",
                                   report->provenance.depth_format) &&
      vkr_harness_json_emit_string(writer, "color_space",
                                   report->provenance.color_space) &&
      vkr_harness_json_emit_string(writer, "world_renderer",
                                   report->provenance.world_renderer) &&
      vkr_harness_json_emit_string(
          writer, "present_mode",
          vkr_harness_present_name(report->provenance.actual_present)) &&
      vkr_harness_json_emit_string(
          writer, "boot_profile",
          vkr_harness_boot_name(report->case_manifest.boot)) &&
      vkr_harness_json_emit_string(writer, "subsystem_mask", subsystem_mask) &&
      vkr_harness_json_emit_bool(writer, "editor",
                                 report->case_manifest.renderer.editor) &&
      vkr_harness_json_emit_bool(writer, "text_fixture",
                                 report->case_manifest.renderer.text_fixture) &&
      vkr_harness_json_emit_string(writer, "renderer_backend",
                                   report->case_manifest.renderer.backend[0]
                                       ? report->case_manifest.renderer.backend
                                       : "external") &&
      vkr_harness_json_emit_u64(
          writer, "cascades", report->case_manifest.renderer.shadow_cascades) &&
      vkr_harness_json_emit_u64(
          writer, "pcf_samples",
          report->case_manifest.renderer.shadow_pcf_samples) &&
      vkr_harness_json_emit_string(
          writer, "cache",
          vkr_harness_cache_name(report->case_manifest.cache)) &&
      vkr_harness_json_emit_u64(writer, "camera_script_version",
                                VKR_HARNESS_CAMERA_SCRIPT_VERSION) &&
      vkr_harness_json_emit_bool(writer, "gpu_timing",
                                 report->profile.gpu_timing) &&
      vkr_harness_json_emit_bool(writer, "submission_gpu_timing",
                                 report->profile.submission_gpu_timing) &&
      vkr_harness_json_emit_bool(writer, "metrics_compile_enabled",
                                 VKR_METRICS_ENABLED ? true_v : false_v) &&
      vkr_harness_json_emit_bool(writer, "events_enabled",
                                 report->profile.event_subjects) &&
      vkr_json_writer_end_object(writer) &&
      vkr_harness_json_emit_name(writer, "execution") &&
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_u64(writer, "requested_repetitions",
                                report->requested_repetitions) &&
      vkr_harness_json_emit_u64(writer, "completed_repetitions",
                                report->completed_repetitions) &&
      vkr_harness_json_emit_u64(writer, "warmup_frames",
                                report->case_manifest.warmup_frames) &&
      vkr_harness_json_emit_u64(writer, "measured_frames",
                                report->case_manifest.measure_frames) &&
      vkr_harness_json_emit_string(writer, "warmup_stability_metric",
                                   report->profile.warmup_stability_metric) &&
      vkr_harness_json_emit_bool(writer, "warmup_stable",
                                 report->warmup_stable) &&
      vkr_harness_json_emit_bool(writer, "gpu_lane_lock_acquired",
                                 report->gpu_lane_lock_acquired) &&
      vkr_json_writer_end_object(writer) &&
      vkr_harness_json_emit_name(writer, "runs") &&
      vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->run_count; ++i) {
    const VkrHarnessRunReference *run = &report->runs[i];
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_u64(writer, "index", run->index) &&
         vkr_harness_json_emit_string(writer, "status", run->status) &&
         vkr_harness_json_emit_string(writer, "report", run->report) &&
         vkr_harness_json_emit_string(writer, "sha256", run->sha256) &&
         vkr_harness_json_emit_string(writer, "environment_fingerprint",
                                      run->environment_fingerprint) &&
         vkr_harness_json_emit_string(writer, "workload_fingerprint",
                                      run->workload_fingerprint) &&
         vkr_harness_json_emit_string(writer, "policy_fingerprint",
                                      run->policy_fingerprint) &&
         vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_harness_json_emit_name(writer, "auxiliary_runs") &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->auxiliary_run_count; ++i) {
    const VkrHarnessRunReference *run = &report->auxiliary_runs[i];
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_u64(writer, "index", run->index) &&
         vkr_harness_json_emit_string(writer, "status", run->status) &&
         vkr_harness_json_emit_string(writer, "report", run->report) &&
         vkr_harness_json_emit_string(writer, "sha256", run->sha256) &&
         vkr_harness_json_emit_string(writer, "environment_fingerprint",
                                      run->environment_fingerprint) &&
         vkr_harness_json_emit_string(writer, "workload_fingerprint",
                                      run->workload_fingerprint) &&
         vkr_harness_json_emit_string(writer, "policy_fingerprint",
                                      run->policy_fingerprint) &&
         vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_harness_json_emit_name(writer, "aggregate") &&
       vkr_json_writer_begin_object(writer) &&
       vkr_harness_json_emit_name(writer, "metrics") &&
       vkr_json_writer_begin_object(writer);
  for (uint32_t i = 0; ok && i < report->metric_count; ++i) {
    ok = vkr_harness_json_emit_name(writer, report->metrics[i].name) &&
         vkr_harness_report_write_statistics(writer, &report->metrics[i]);
  }
  ok = ok && vkr_json_writer_end_object(writer) &&
       vkr_harness_json_emit_name(writer, "passes") &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->pass_count; ++i) {
    const VkrHarnessPassResult *pass = &report->passes[i];
    VkrHarnessMetricResult cpu = {.statistics = pass->cpu_ms};
    VkrHarnessMetricResult gpu = {.statistics = pass->gpu_ms};
    string_format(cpu.unit, sizeof(cpu.unit), "ms");
    string_format(gpu.unit, sizeof(gpu.unit), "ms");
    ok =
        vkr_json_writer_begin_object(writer) &&
        vkr_harness_json_emit_string(writer, "name", pass->name) &&
        vkr_harness_json_emit_name(writer, "cpu_ms") &&
        vkr_harness_report_write_statistics(writer, &cpu) &&
        vkr_harness_json_emit_name(writer, "gpu_ms") &&
        vkr_harness_report_write_statistics_reason(
            writer, &gpu,
            gpu.statistics.sample_count == 0u
                ? (pass->gpu_unsupported_scope_count > 0u
                       ? "unsupported_timestamp_scope"
                       : (report->profile.gpu_timing ? "no_valid_samples"
                                                     : "disabled"))
                : NULL) &&
        vkr_harness_json_emit_u64(writer, "culled_count", pass->culled_count) &&
        vkr_harness_json_emit_u64(writer, "disabled_count",
                                  pass->disabled_count) &&
        vkr_harness_json_emit_u64(writer, "omitted_count",
                                  pass->omitted_count) &&
        vkr_harness_json_emit_u64(writer, "gpu_unsupported_scope_count",
                                  pass->gpu_unsupported_scope_count) &&
        vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_json_writer_end_object(writer) &&
       vkr_harness_json_emit_name(writer, "events") &&
       vkr_json_writer_begin_object(writer) &&
       vkr_harness_json_emit_name(writer, "items") &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->event_count; ++i) {
    const VkrHarnessEvent *event = &report->events[i];
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_string(writer, "source", event->source) &&
         vkr_harness_json_emit_string(writer, "subject", event->subject) &&
         vkr_harness_json_emit_u64(writer, "start_ns", event->start_ns) &&
         vkr_harness_json_emit_u64(writer, "duration_ns", event->duration_ns) &&
         vkr_harness_json_emit_u64(writer, "bytes", event->bytes) &&
         vkr_harness_json_emit_u64(writer, "thread_id", event->thread_id) &&
         vkr_harness_json_emit_u64(writer, "repetition", event->repetition) &&
         vkr_harness_json_emit_string(writer, "status",
                                      event->success ? "success" : "failure") &&
         vkr_harness_json_emit_bool(writer, "subject_truncated",
                                    event->subject_truncated) &&
         vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_harness_json_emit_u64(writer, "dropped", report->events_dropped) &&
       vkr_harness_json_emit_u64(writer, "subjects_truncated",
                                 report->event_subjects_truncated) &&
       vkr_json_writer_end_object(writer) &&
       vkr_harness_json_emit_name(writer, "captures") &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->capture_count; ++i) {
    const VkrHarnessCaptureResult *capture = &report->captures[i];
    ok =
        vkr_json_writer_begin_object(writer) &&
        vkr_harness_json_emit_u64(writer, "checkpoint_frame",
                                  capture->checkpoint_frame) &&
        vkr_harness_json_emit_u64(writer, "capture_version",
                                  capture->capture_version) &&
        vkr_harness_json_emit_string(writer, "channel", capture->channel) &&
        vkr_harness_json_emit_string(writer, "producer_resource",
                                     capture->producer_resource) &&
        vkr_harness_json_emit_string(writer, "source_format",
                                     capture->source_format) &&
        vkr_harness_json_emit_string(writer, "canonical_encoding",
                                     capture->canonical_encoding) &&
        vkr_harness_json_emit_string(writer, "value_kind",
                                     capture->value_kind) &&
        vkr_harness_json_emit_string(writer, "color_space",
                                     capture->color_space) &&
        vkr_harness_json_emit_string(writer, "origin", capture->origin) &&
        vkr_harness_json_emit_u64(writer, "width", capture->width) &&
        vkr_harness_json_emit_u64(writer, "height", capture->height) &&
        vkr_harness_json_emit_u64(writer, "source_row_pitch",
                                  capture->source_row_pitch) &&
        vkr_harness_json_emit_u64(writer, "mip", capture->mip) &&
        vkr_harness_json_emit_u64(writer, "layer", capture->layer) &&
        vkr_harness_json_emit_u64(writer, "source_frame_index",
                                  capture->source_frame_index) &&
        vkr_harness_json_emit_u64(writer, "submit_serial",
                                  capture->submit_serial) &&
        vkr_harness_json_emit_string(writer, "data_path", capture->data_path) &&
        vkr_harness_json_emit_string(writer, "data_sha256",
                                     capture->data_sha256) &&
        vkr_harness_json_emit_string(writer, "preview_path",
                                     capture->preview_path) &&
        vkr_harness_json_emit_string(writer, "preview_sha256",
                                     capture->preview_sha256) &&
        vkr_harness_json_emit_string(writer, "metadata_path",
                                     capture->metadata_path) &&
        vkr_harness_json_emit_string(writer, "metadata_sha256",
                                     capture->metadata_sha256) &&
        vkr_harness_json_emit_string(writer, "comparison_status",
                                     capture->comparison_status) &&
        vkr_harness_json_emit_string(writer, "baseline_data_path",
                                     capture->baseline_data_path) &&
        vkr_harness_json_emit_string(writer, "baseline_data_sha256",
                                     capture->baseline_data_sha256) &&
        vkr_harness_json_emit_string(writer, "diff_path", capture->diff_path) &&
        vkr_harness_json_emit_string(writer, "diff_sha256",
                                     capture->diff_sha256) &&
        vkr_harness_json_emit_name(writer, "thresholds") &&
        vkr_json_writer_begin_object(writer) &&
        vkr_harness_json_emit_f64(writer, "max_pixel_delta",
                                  capture->thresholds.max_pixel_delta) &&
        vkr_harness_json_emit_f64(
            writer, "max_mean_absolute_error",
            capture->thresholds.max_mean_absolute_error) &&
        vkr_harness_json_emit_f64(writer, "max_failed_pixel_ratio",
                                  capture->thresholds.max_failed_pixel_ratio) &&
        vkr_harness_json_emit_bool(writer, "emit_diff",
                                   capture->thresholds.emit_diff) &&
        vkr_json_writer_end_object(writer) &&
        vkr_harness_json_emit_name(writer, "comparison") &&
        vkr_json_writer_begin_object(writer) &&
        vkr_harness_json_emit_f64(writer, "mean_absolute_error",
                                  capture->comparison.mean_absolute_error) &&
        vkr_harness_json_emit_f64(writer, "max_absolute_error",
                                  capture->comparison.max_absolute_error) &&
        vkr_harness_json_emit_u64(writer, "failing_value_count",
                                  capture->comparison.failing_value_count) &&
        vkr_harness_json_emit_u64(writer, "failing_pixel_count",
                                  capture->comparison.failing_pixel_count) &&
        vkr_harness_json_emit_u64(writer, "value_count",
                                  capture->comparison.value_count) &&
        vkr_harness_json_emit_u64(writer, "pixel_count",
                                  capture->comparison.pixel_count) &&
        vkr_harness_json_emit_f64(writer, "failed_pixel_ratio",
                                  capture->comparison.failed_pixel_ratio) &&
        vkr_json_writer_end_object(writer) &&
        vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_harness_json_emit_name(writer, "assertions") &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->case_manifest.assertion_count; ++i) {
    const VkrHarnessAssertion *assertion = &report->case_manifest.assertions[i];
    float64_t actual = 0.0;
    const VkrHarnessAssertionOutcome outcome =
        vkr_harness_assertion_evaluate(report, assertion, &actual);
    ok =
        vkr_json_writer_begin_object(writer) &&
        vkr_harness_json_emit_string(writer, "metric", assertion->metric) &&
        vkr_harness_json_emit_string(
            writer, "stat", vkr_harness_statistic_name(assertion->statistic)) &&
        vkr_harness_json_emit_string(
            writer, "operator",
            vkr_harness_operator_name(assertion->operation)) &&
        vkr_harness_json_emit_string(
            writer, "status", vkr_harness_assertion_outcome_name(outcome)) &&
        vkr_harness_json_emit_f64(writer, "actual", actual) &&
        vkr_harness_json_emit_f64(writer, "limit", assertion->limit) &&
        vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_harness_json_emit_name(writer, "diagnostics") &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->authority_reason_count; ++i) {
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_string(writer, "code",
                                      report->authority_reasons[i]) &&
         vkr_harness_json_emit_string(writer, "severity", "warning") &&
         vkr_harness_json_emit_string(
             writer, "message", "Run is not authoritative for this reason") &&
         vkr_json_writer_end_object(writer);
  }
  if (ok && !report->profile.gpu_timing &&
      !report->profile.submission_gpu_timing) {
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_string(writer, "code", "gpu_timing.disabled") &&
         vkr_harness_json_emit_string(writer, "severity", "info") &&
         vkr_harness_json_emit_string(writer, "message",
                                      "GPU timing was not requested") &&
         vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_harness_json_emit_name(writer, "artifacts") &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < report->artifact_count; ++i) {
    const VkrHarnessArtifact *artifact = &report->artifacts[i];
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_string(writer, "role", artifact->role) &&
         vkr_harness_json_emit_string(writer, "path", artifact->path) &&
         vkr_harness_json_emit_string(writer, "media_type",
                                      artifact->media_type) &&
         vkr_harness_json_emit_string(writer, "sha256", artifact->sha256) &&
         vkr_harness_json_emit_string(writer, "status", artifact->status) &&
         vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_json_writer_end_object(writer);
  if (!ok || !vkr_json_file_writer_commit(&file_writer)) {
    vkr_json_file_writer_abort(&file_writer);
    vkr_harness_error_set(out_error, "report.write", "$",
                          "Unable to publish report '%s'", path);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_harness_summary_csv_write(const char *path,
                                      const VkrHarnessReport *report,
                                      Arena *transient,
                                      VkrHarnessError *out_error) {
  if (!path || !report || !transient) {
    return false_v;
  }
  const uint64_t row_capacity = 512u;
  const uint64_t capacity = 96u + (uint64_t)report->metric_count *
                                      VKR_HARNESS_STAT_COUNT * row_capacity;
  Scratch scratch = scratch_create(transient);
  char *csv = arena_alloc(transient, capacity, ARENA_MEMORY_TAG_STRING);
  if (!csv) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    vkr_harness_error_set(out_error, "summary.allocate", "$",
                          "Unable to allocate the summary buffer");
    return false_v;
  }
  uint64_t used = (uint64_t)string_format(
      csv, capacity, "run_index,metric,unit,stat,value,sample_count,status\n");
  for (uint32_t metric_index = 0; metric_index < report->metric_count;
       ++metric_index) {
    const VkrHarnessMetricResult *metric = &report->metrics[metric_index];
    for (uint32_t stat = 0; stat < VKR_HARNESS_STAT_COUNT; ++stat) {
      const int32_t written = string_format(
          csv + used, capacity - used, "aggregate,%s,%s,%s,%.17g,%llu,%s\n",
          metric->name, metric->unit,
          vkr_harness_statistic_name((VkrHarnessStatisticKind)stat),
          vkr_harness_statistic_value(&metric->statistics,
                                      (VkrHarnessStatisticKind)stat),
          (unsigned long long)metric->statistics.sample_count,
          metric->statistics.invalid_count == 0u ? "valid" : "incomplete");
      if (written < 0 || (uint64_t)written >= capacity - used) {
        scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
        vkr_harness_error_set(out_error, "summary.row", "$",
                              "Summary row for '%s' exceeds its capacity",
                              metric->name);
        return false_v;
      }
      used += (uint64_t)written;
    }
  }
  const bool8_t result = vkr_harness_atomic_write(path, csv, used, out_error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return result;
}
