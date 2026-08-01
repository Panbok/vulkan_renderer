/**
 * @file vkr_harness_parent.c
 * @brief `profile` orchestration: launch independent child repetitions, verify
 *        their artifacts, aggregate their evidence, and publish one atomic
 *        report.
 *
 * This side never links the renderer. Every measurement comes from a child
 * process, because in-process frame windows are samples, not independent
 * repetitions.
 */
#include "vkr_harness_runtime.h"

/**
 * Serializes authoritative runs against each other on one machine. Removing a
 * window does not remove GPU contention, so this is a policy the profile owns
 * rather than an implementation detail.
 */
typedef struct VkrHarnessGpuLane {
  VkrPlatformProcessLock lock;
} VkrHarnessGpuLane;

static void vkr_harness_gpu_lane_init(VkrHarnessGpuLane *lane) {
  MemZero(lane, sizeof(*lane));
}

static bool8_t vkr_harness_gpu_lane_acquire(VkrHarnessGpuLane *lane,
                                            const char *artifact_root) {
  return vkr_platform_process_lock_acquire("VkrHarnessGpuLane", artifact_root,
                                           &lane->lock);
}

static void vkr_harness_gpu_lane_release(VkrHarnessGpuLane *lane) {
  vkr_platform_process_lock_release(&lane->lock);
}

/** stdout carries exactly one machine-readable line, on every exit path. */
static void vkr_harness_emit_result(const char *status,
                                    VkrHarnessExitCode exit_code,
                                    const char *report_relative_path,
                                    const char *report_digest) {
  if (report_relative_path) {
    vkr_harness_stdout("{\"status\":\"%s\",\"exit_code\":%u,\"report\":\"%s\","
                       "\"sha256\":\"%s\"}\n",
                       status, exit_code, report_relative_path,
                       report_digest ? report_digest : "");
  } else {
    vkr_harness_stdout("{\"status\":\"%s\",\"exit_code\":%u,\"report\":null}\n",
                       status, exit_code);
  }
}

static int vkr_harness_spawn_child(const char *executable,
                                   const char *repo_root, const char *case_path,
                                   const char *profile_path,
                                   const char *run_dir, const char *cache_path,
                                   uint32_t timeout_ms, bool8_t prewarm) {
  char stdout_path[VKR_HARNESS_PATH_MAX];
  char stderr_path[VKR_HARNESS_PATH_MAX];
  string_format(stdout_path, sizeof(stdout_path), "%s/stdout.log", run_dir);
  string_format(stderr_path, sizeof(stderr_path), "%s/stderr.log", run_dir);
  const char *arguments[11] = {"--child-profile", "--repo-root", repo_root,
                               "--case",          case_path,     "--profile",
                               profile_path,      "--run-dir",   run_dir};
  uint32_t argument_count = 9u;
  if (prewarm) {
    arguments[argument_count++] = "--prewarm";
  }
  const VkrPlatformEnvironmentVariable environment = {
      .name = "VKR_PIPELINE_CACHE_PATH", .value = cache_path};
  const VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = argument_count,
      .working_directory = repo_root,
      .stdout_path = stdout_path,
      .stderr_path = stderr_path,
      .environment = cache_path && cache_path[0] ? &environment : NULL,
      .environment_count = cache_path && cache_path[0] ? 1u : 0u,
      .timeout_ms = timeout_ms,
      .termination_grace_ms = 100u,
      .hidden = true_v,
  };
  int32_t exit_code = VKR_HARNESS_EXIT_ERROR;
  bool8_t timed_out = false_v;
  if (!vkr_platform_process_run(&config, &exit_code, &timed_out) || timed_out) {
    return VKR_HARNESS_EXIT_ERROR;
  }
  return exit_code;
}

/**
 * Run IDs are generated, never accepted from the command line, and the
 * directory is created exclusively so two concurrent invocations cannot share
 * a run root.
 */
static bool8_t
vkr_harness_create_run_root(const char *artifact_root, char run_id[64],
                            char run_root[VKR_HARNESS_PATH_MAX]) {
  for (uint32_t attempt = 0; attempt < 16u; ++attempt) {
    if (!vkr_harness_generate_run_id(run_id)) {
      return false_v;
    }
    const int32_t written = string_format(run_root, VKR_HARNESS_PATH_MAX,
                                          "%s/%s", artifact_root, run_id);
    if (written < 0 || written >= (int32_t)VKR_HARNESS_PATH_MAX) {
      return false_v;
    }
    FilePath path = vkr_harness_file_path(run_root);
    const FileError result = file_create_directory_exclusive(&path);
    if (result == FILE_ERROR_NONE) {
      return true_v;
    }
    if (result != FILE_ERROR_ALREADY_EXISTS) {
      return false_v;
    }
  }
  return false_v;
}

/**
 * Confirms the child recorded the same samples digest the parent now measures,
 * so a stale or rewritten samples file cannot be aggregated under a report
 * that never covered it.
 *
 * The child report holds one statistics object per registered metric, which
 * exceeds the harness JSON parser's fixed token budget; this checks the
 * artifact entry textually instead.
 */
static bool8_t vkr_harness_child_sample_digest_matches(const char *report_path,
                                                       const char *samples_path,
                                                       Arena *transient) {
  char digest[VKR_HARNESS_DIGEST_MAX];
  uint8_t *report_data = NULL;
  uint64_t report_length = 0u;
  Scratch scratch = scratch_create(transient);
  bool8_t matches = false_v;
  if (vkr_harness_sha256_file(samples_path, digest) &&
      vkr_harness_read_file(report_path, transient, &report_data,
                            &report_length)) {
    /* The report is read as bytes; terminate a copy so the searches below are
       string operations. */
    char *text =
        arena_alloc(transient, report_length + 1u, ARENA_MEMORY_TAG_STRING);
    if (text) {
      MemCopy(text, report_data, (size_t)report_length);
      text[report_length] = '\0';
      const char *artifact = string_find(text, "\"role\":\"samples.raw\"");
      const char *recorded =
          artifact ? string_find(artifact, "\"sha256\":\"") : NULL;
      const char *end = artifact ? string_find_char(artifact, '}') : NULL;
      matches = recorded && end && recorded < end &&
                string_n_equals(recorded + 10u, digest, string_length(digest));
    }
  }
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return matches;
}

/**
 * Determinism rule 6: work volume must be bit-identical across independent
 * repetitions of the same case and build. A difference invalidates the timing
 * evidence regardless of its average.
 */
static bool8_t vkr_harness_work_metric(const char *name) {
  return string_n_equals(name, "draw.", 5u) ||
         string_n_equals(name, "visibility.", 11u) ||
         string_find(name, "overflow") || string_find(name, "capture");
}

static bool8_t
vkr_harness_work_volume_matches(const VkrHarnessCase *case_manifest,
                                const VkrHarnessSampleSet *runs,
                                uint32_t run_count) {
  if (run_count < 2u) {
    return true_v;
  }
  const uint32_t metric_count = runs[0].header.metric_count;
  bool8_t is_work[VKR_METRICS_MAX_SLOTS];
  for (uint32_t metric = 0; metric < metric_count; ++metric) {
    is_work[metric] = vkr_harness_work_metric(runs[0].metrics[metric].name);
  }
  const uint64_t first = (uint64_t)case_manifest->warmup_frames * metric_count;
  const uint64_t count = (uint64_t)case_manifest->measure_frames * metric_count;
  for (uint32_t run = 1; run < run_count; ++run) {
    for (uint64_t offset = 0; offset < count; ++offset) {
      if (!is_work[offset % metric_count]) {
        continue;
      }
      if (runs[0].availability[first + offset] !=
              runs[run].availability[first + offset] ||
          MemCompare(&runs[0].values[first + offset],
                     &runs[run].values[first + offset],
                     sizeof(float64_t)) != 0) {
        return false_v;
      }
    }
  }
  return true_v;
}

static bool8_t vkr_harness_profile_matches(const char *required,
                                           const char *actual) {
  return !required[0] || string_equals(required, actual);
}

static bool8_t
vkr_harness_environment_satisfies_profile(const VkrHarnessProfile *profile,
                                          const VkrHarnessProvenance *actual) {
  return vkr_harness_profile_matches(profile->required_os, actual->os) &&
         vkr_harness_profile_matches(profile->required_cpu, actual->cpu) &&
         vkr_harness_profile_matches(profile->required_gpu, actual->gpu) &&
         vkr_harness_profile_matches(profile->required_driver,
                                     actual->driver) &&
         vkr_harness_profile_matches(profile->required_power_mode,
                                     actual->power_mode) &&
         vkr_harness_profile_matches(profile->required_thermal_state,
                                     actual->thermal_state_start) &&
         (profile->required_gpu_vendor_id == 0u ||
          profile->required_gpu_vendor_id == actual->gpu_vendor_id) &&
         (profile->required_gpu_device_id == 0u ||
          profile->required_gpu_device_id == actual->gpu_device_id) &&
         (!profile->has_required_process_priority ||
          profile->required_process_priority == actual->process_priority);
}

/** Adopts the presentation/device configuration the first repetition observed.
 */
static void
vkr_harness_adopt_run_provenance(VkrHarnessProvenance *provenance,
                                 const VkrHarnessSampleFileHeader *header) {
  vkr_harness_provenance_set_text(provenance->gpu, sizeof(provenance->gpu),
                                  header->gpu);
  vkr_harness_provenance_set_text(provenance->driver,
                                  sizeof(provenance->driver), header->driver);
  provenance->gpu_vendor_id = header->gpu_vendor_id;
  provenance->gpu_device_id = header->gpu_device_id;
  provenance->actual_present = (VkrHarnessPresentMode)header->actual_present;
  provenance->actual_target_image_count = header->actual_image_count;
  vkr_harness_provenance_set_text(provenance->color_format,
                                  sizeof(provenance->color_format),
                                  header->color_format);
  vkr_harness_provenance_set_text(provenance->color_space,
                                  sizeof(provenance->color_space),
                                  header->color_space);
}

/**
 * A later repetition must describe the same catalog and device as the first,
 * or its samples belong to a different observation.
 */
static bool8_t
vkr_harness_run_is_compatible(const VkrHarnessSampleSet *first,
                              const VkrHarnessSampleSet *candidate) {
  return candidate->header.metric_count == first->header.metric_count &&
         candidate->header.pass_count == first->header.pass_count &&
         MemCompare(first->metrics, candidate->metrics,
                    sizeof(*first->metrics) * first->header.metric_count) ==
             0 &&
         MemCompare(first->passes, candidate->passes,
                    sizeof(*first->passes) * first->header.pass_count) == 0 &&
         candidate->header.gpu_vendor_id == first->header.gpu_vendor_id &&
         candidate->header.gpu_device_id == first->header.gpu_device_id &&
         candidate->header.actual_present == first->header.actual_present;
}

/**
 * Folds every completed repetition's bounded event stream, drop counts, and
 * stability flag into the aggregate.
 *
 * Runs once after the loop rather than per repetition: the total event count is
 * only known when every header has been read, which turns an incremental
 * `realloc` chain into a single sized allocation.
 */
static bool8_t vkr_harness_collect_run_evidence(VkrHarnessReport *report,
                                                const VkrHarnessSampleSet *runs,
                                                Arena *persistent,
                                                bool8_t *out_snapshot_dropped) {
  uint64_t total_events = 0;
  *out_snapshot_dropped = false_v;
  report->warmup_stable = true_v;
  for (uint32_t run = 0; run < report->completed_repetitions; ++run) {
    const VkrHarnessSampleFileHeader *header = &runs[run].header;
    total_events += header->event_count;
    report->events_dropped += header->events_dropped;
    report->event_subjects_truncated += header->event_subjects_truncated;
    *out_snapshot_dropped |= header->snapshot_publications_dropped > 0u;
    report->warmup_stable &=
        (header->flags & VKR_HARNESS_SAMPLE_FLAG_WARMUP_STABLE) != 0u;
  }
  if (total_events == 0u) {
    return true_v;
  }
  if (total_events > UINT32_MAX) {
    return false_v;
  }
  const uint64_t bytes = total_events * sizeof(*report->events);
  report->events = arena_alloc(persistent, bytes, ARENA_MEMORY_TAG_STRUCT);
  if (!report->events) {
    return false_v;
  }
  MemZero(report->events, bytes);
  for (uint32_t run = 0; run < report->completed_repetitions; ++run) {
    for (uint32_t i = 0; i < runs[run].header.event_count; ++i) {
      const VkrHarnessSampleEvent *source = &runs[run].events[i];
      VkrHarnessEvent *target = &report->events[report->event_count++];
      string_format(target->source, sizeof(target->source), "%s",
                    source->source);
      string_format(target->subject, sizeof(target->subject), "%s",
                    source->subject);
      target->start_ns = source->start_ns;
      target->duration_ns = source->duration_ns;
      target->bytes = source->bytes;
      target->thread_id = source->thread_id;
      target->repetition = run;
      target->success = source->status == VKR_METRIC_EVENT_STATUS_SUCCESS;
      target->subject_truncated = source->subject_truncated;
    }
  }
  return true_v;
}

/**
 * Concatenates the measured window of every completed repetition into one
 * series per metric and per pass. Warmup frames are excluded here rather than
 * inside the statistics, so the aggregate case describes exactly what it
 * summarizes.
 */
static bool8_t vkr_harness_aggregate_runs(const VkrHarnessArenas *arenas,
                                          VkrHarnessReport *report,
                                          const VkrHarnessCase *case_manifest,
                                          const VkrHarnessSampleSet *runs,
                                          VkrHarnessError *error) {
  const uint32_t run_count = report->completed_repetitions;
  const uint32_t metric_count = runs[0].header.metric_count;
  const uint32_t pass_count = runs[0].header.pass_count;
  const uint32_t measured = case_manifest->measure_frames;
  const uint64_t metric_values = (uint64_t)measured * metric_count;
  const uint64_t pass_values = (uint64_t)measured * pass_count;

  /* The concatenated series exist only for the statistics below, so they are
     scoped; the results they produce come from the persistent arena. */
  Scratch scratch = scratch_create(arenas->transient);
  float64_t *values = arena_alloc(arenas->transient,
                                  sizeof(float64_t) * metric_values * run_count,
                                  ARENA_MEMORY_TAG_ARRAY);
  uint8_t *availability = arena_alloc(
      arenas->transient, metric_values * run_count, ARENA_MEMORY_TAG_ARRAY);
  float64_t *pass_cpu = NULL;
  float64_t *pass_gpu = NULL;
  uint8_t *pass_flags = NULL;
  if (pass_values > 0u) {
    const uint64_t pass_bytes = sizeof(float64_t) * pass_values * run_count;
    pass_cpu =
        arena_alloc(arenas->transient, pass_bytes, ARENA_MEMORY_TAG_ARRAY);
    pass_gpu =
        arena_alloc(arenas->transient, pass_bytes, ARENA_MEMORY_TAG_ARRAY);
    pass_flags = arena_alloc(arenas->transient, pass_values * run_count,
                             ARENA_MEMORY_TAG_ARRAY);
  }
  bool8_t ok = values && availability &&
               (pass_values == 0u || (pass_cpu && pass_gpu && pass_flags));
  if (!ok) {
    vkr_harness_error_set(error, "aggregate.allocate", "$",
                          "Unable to allocate the aggregate sample buffers");
  }
  for (uint32_t run = 0; ok && run < run_count; ++run) {
    const uint64_t metric_source =
        (uint64_t)case_manifest->warmup_frames * metric_count;
    const uint64_t pass_source =
        (uint64_t)case_manifest->warmup_frames * pass_count;
    MemCopy(values + (uint64_t)run * metric_values,
            runs[run].values + metric_source,
            sizeof(float64_t) * metric_values);
    MemCopy(availability + (uint64_t)run * metric_values,
            runs[run].availability + metric_source, metric_values);
    if (pass_values > 0u) {
      MemCopy(pass_cpu + (uint64_t)run * pass_values,
              runs[run].pass_cpu_ms + pass_source,
              sizeof(float64_t) * pass_values);
      MemCopy(pass_gpu + (uint64_t)run * pass_values,
              runs[run].pass_gpu_ms + pass_source,
              sizeof(float64_t) * pass_values);
      MemCopy(pass_flags + (uint64_t)run * pass_values,
              runs[run].pass_flags + pass_source, pass_values);
    }
  }
  const uint32_t aggregate_frames = measured * run_count;
  if (ok && vkr_harness_compute_metric_results(
                arenas, 0u, aggregate_frames, metric_count, runs[0].metrics,
                values, availability, &report->metrics, error)) {
    report->metric_count = metric_count;
  } else {
    ok = false_v;
  }
  if (ok && vkr_harness_compute_pass_results(
                arenas, 0u, aggregate_frames, pass_count, runs[0].passes,
                pass_cpu, pass_gpu, pass_flags, &report->passes, error)) {
    report->pass_count = pass_count;
  } else {
    ok = false_v;
  }
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return ok;
}

/**
 * Runs one repetition — optional isolated-cache prewarm, then the timed child
 * — and validates its artifacts before its samples may be aggregated.
 */
static bool8_t vkr_harness_execute_repetition(
    const VkrHarnessArenas *arenas, const char *executable,
    const char *repo_root, const char *case_path, const char *profile_path,
    const char *run_root, uint32_t index, const VkrHarnessCase *case_manifest,
    VkrHarnessRunReference *reference, VkrHarnessSampleSet *out_samples,
    VkrHarnessError *error) {
  char run_dir[VKR_HARNESS_PATH_MAX];
  string_format(run_dir, sizeof(run_dir), "%s/runs/%u", run_root, index);
  reference->index = index;
  string_format(reference->report, sizeof(reference->report),
                "runs/%u/report.json", index);
  string_format(reference->status, sizeof(reference->status), "incomplete");
  if (!vkr_harness_make_directories(run_dir, error)) {
    return false_v;
  }
  char cache_path[VKR_HARNESS_PATH_MAX] = {0};
  if (case_manifest->cache != VKR_HARNESS_CACHE_SHARED) {
    string_format(cache_path, sizeof(cache_path), "%s/pipeline.cache", run_dir);
  }
  if (case_manifest->cache == VKR_HARNESS_CACHE_ISOLATED_WARM) {
    char prewarm_dir[VKR_HARNESS_PATH_MAX];
    string_format(prewarm_dir, sizeof(prewarm_dir), "%s/prewarm", run_dir);
    if (!vkr_harness_make_directories(prewarm_dir, error) ||
        vkr_harness_spawn_child(executable, repo_root, case_path, profile_path,
                                prewarm_dir, cache_path,
                                case_manifest->repetition_timeout_ms,
                                true_v) != VKR_HARNESS_EXIT_PASS) {
      return false_v;
    }
  }
  const int child_exit = vkr_harness_spawn_child(
      executable, repo_root, case_path, profile_path, run_dir, cache_path,
      case_manifest->repetition_timeout_ms, false_v);
  char child_report[VKR_HARNESS_PATH_MAX];
  char samples_path[VKR_HARNESS_PATH_MAX];
  string_format(child_report, sizeof(child_report), "%s/report.json", run_dir);
  string_format(samples_path, sizeof(samples_path), "%s/samples.bin", run_dir);
  if (child_exit != VKR_HARNESS_EXIT_PASS ||
      !vkr_harness_sha256_file(child_report, reference->sha256) ||
      !vkr_harness_child_sample_digest_matches(child_report, samples_path,
                                               arenas->transient) ||
      !vkr_harness_samples_read(samples_path, case_manifest, arenas->persistent,
                                out_samples)) {
    return false_v;
  }
  return true_v;
}

/** Records the run's artifact digests; every one must resolve. */
static bool8_t vkr_harness_record_run_artifacts(VkrHarnessReport *report,
                                                const char *run_root,
                                                uint32_t index) {
  static const struct {
    const char *file;
    const char *role_suffix;
    const char *media_type;
  } kArtifacts[] = {
      {"report.json", "report", "application/json"},
      {"samples.bin", "samples", "application/vnd.vkr.harness-samples"},
      {"stdout.log", "stdout", "text/plain"},
      {"stderr.log", "stderr", "text/plain"},
  };
  bool8_t complete = true_v;
  for (uint32_t i = 0; i < ArrayCount(kArtifacts); ++i) {
    char relative[VKR_HARNESS_PATH_MAX];
    char absolute[VKR_HARNESS_PATH_MAX];
    char role[96];
    string_format(relative, sizeof(relative), "runs/%u/%s", index,
                  kArtifacts[i].file);
    string_format(absolute, sizeof(absolute), "%s/%s", run_root, relative);
    string_format(role, sizeof(role), "run.%u.%s", index,
                  kArtifacts[i].role_suffix);
    complete &= vkr_harness_report_add_artifact(
        report, role, relative, kArtifacts[i].media_type, absolute);
  }
  return complete;
}

/**
 * Turns the collected evidence into a verdict. Missing or partial evidence
 * makes the run incomplete; only complete evidence that violates an assertion
 * is a failure.
 */
static void vkr_harness_apply_verdict(VkrHarnessReport *report,
                                      const VkrHarnessCase *case_manifest,
                                      const VkrHarnessProfile *profile,
                                      const VkrHarnessSampleSet *runs,
                                      bool8_t snapshot_dropped) {
  if (report->completed_repetitions != report->requested_repetitions) {
    vkr_harness_report_mark_incomplete(report,
                                       "execution.repetitions_incomplete");
  } else if (!report->warmup_stable && profile->require_warmup_stability) {
    vkr_harness_report_mark_incomplete(report, "execution.warmup_unstable");
  } else {
    vkr_harness_report_set_status(report, "pass", VKR_HARNESS_EXIT_PASS);
    if (!report->warmup_stable) {
      vkr_harness_report_add_authority_reason(report,
                                              "execution.warmup_unstable");
    }
  }
  if (snapshot_dropped) {
    vkr_harness_report_mark_incomplete(report,
                                       "metrics.snapshot_publication_dropped");
  }
  /* Events only gate completeness when the profile asked for their subjects. */
  if (profile->event_subjects && report->events_dropped > 0u) {
    vkr_harness_report_mark_incomplete(report, "events.dropped");
  }
  if (profile->event_subjects && report->event_subjects_truncated > 0u) {
    vkr_harness_report_mark_incomplete(report, "events.subjects_truncated");
  }
  if (report->completed_repetitions == report->requested_repetitions &&
      !vkr_harness_work_volume_matches(case_manifest, runs,
                                       report->completed_repetitions)) {
    vkr_harness_report_mark_incomplete(report,
                                       "determinism.work_volume_mismatch");
  }
  for (uint32_t i = 0; i < profile->required_metric_count; ++i) {
    const VkrHarnessMetricResult *required =
        vkr_harness_report_find_metric(report, profile->required_metrics[i]);
    if (!required || required->statistics.sample_count == 0u ||
        required->statistics.invalid_count > 0u) {
      vkr_harness_report_mark_incomplete(report, "metrics.required_incomplete");
      break;
    }
  }
  for (uint32_t i = 0; i < case_manifest->assertion_count; ++i) {
    const VkrHarnessAssertion *assertion = &case_manifest->assertions[i];
    const VkrHarnessAssertionOutcome outcome =
        vkr_harness_assertion_evaluate(report, assertion, NULL);
    if (outcome == VKR_HARNESS_ASSERTION_INCOMPLETE) {
      vkr_harness_report_mark_incomplete(report,
                                         "assertions.evidence_incomplete");
    } else if (outcome == VKR_HARNESS_ASSERTION_FAIL &&
               report->exit_code == VKR_HARNESS_EXIT_PASS) {
      vkr_harness_report_set_status(report, "fail", VKR_HARNESS_EXIT_FAIL);
    }
    /* A case may explore timing locally, but it may not gate on it. */
    const VkrHarnessMetricResult *metric =
        vkr_harness_report_find_metric(report, assertion->metric);
    if (metric && string_equals(metric->unit, "ns")) {
      vkr_harness_report_add_authority_reason(report,
                                              "policy.case_timing_assertion");
    }
  }
  if (profile->require_actual_present &&
      report->provenance.actual_present != profile->required_present) {
    vkr_harness_report_add_incompatibility(report, "environment.present_mode");
    vkr_harness_report_set_status(report, "unavailable",
                                  VKR_HARNESS_EXIT_UNAVAILABLE);
  }
  if (!vkr_harness_environment_satisfies_profile(profile,
                                                 &report->provenance)) {
    vkr_harness_report_add_incompatibility(report,
                                           "environment.profile_constraint");
    vkr_harness_report_set_status(report, "unavailable",
                                  VKR_HARNESS_EXIT_UNAVAILABLE);
  }
}

int vkr_harness_profile_run(const char *executable, const char *repo_root,
                            const char *case_path, const char *profile_path) {
#if !VKR_METRICS_ENABLED
  (void)executable;
  (void)repo_root;
  (void)case_path;
  (void)profile_path;
  vkr_harness_stderr("The harness requires VKR_METRICS_ENABLED=1\n");
  vkr_harness_emit_result("unavailable", VKR_HARNESS_EXIT_UNAVAILABLE, NULL,
                          NULL);
  return VKR_HARNESS_EXIT_UNAVAILABLE;
#else
  VkrHarnessError error = {0};
  VkrHarnessCase case_manifest = {0};
  VkrHarnessProfile profile = {0};
  if (!vkr_harness_case_load(repo_root, case_path, &case_manifest, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    vkr_harness_emit_result("invalid", VKR_HARNESS_EXIT_INVALID, NULL, NULL);
    return VKR_HARNESS_EXIT_INVALID;
  }
  if (!vkr_harness_profile_load(repo_root, profile_path, &profile, &error)) {
    /* An unreadable profile is a missing instrument, not a malformed one. */
    const bool8_t missing = string_n_equals(error.code, "path.", 5u) ||
                            string_equals(error.code, "manifest.read");
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    const VkrHarnessExitCode exit_code =
        missing ? VKR_HARNESS_EXIT_MISSING_BASELINE : VKR_HARNESS_EXIT_INVALID;
    vkr_harness_emit_result(missing ? "missing_baseline" : "invalid", exit_code,
                            NULL, NULL);
    return exit_code;
  }
  const char *unsupported =
      vkr_harness_phase2_unsupported(&case_manifest, &profile);
  if (unsupported) {
    vkr_harness_stderr("%s\n", unsupported);
    vkr_harness_emit_result("unavailable", VKR_HARNESS_EXIT_UNAVAILABLE, NULL,
                            NULL);
    return VKR_HARNESS_EXIT_UNAVAILABLE;
  }
  if (case_manifest.target != profile.target ||
      case_manifest.present != profile.required_present) {
    vkr_harness_stderr("Case and execution profile are incompatible\n");
    vkr_harness_emit_result("missing_baseline",
                            VKR_HARNESS_EXIT_MISSING_BASELINE, NULL, NULL);
    return VKR_HARNESS_EXIT_MISSING_BASELINE;
  }

  VkrHarnessReport report = {
      .tool = VKR_HARNESS_TOOL_PROFILE,
      .authoritative = profile.authoritative,
      .case_manifest = case_manifest,
      .profile = profile,
      .profile_compatible = true_v,
      /* A profile may raise the repetition count but never lower it. */
      .requested_repetitions =
          Max(case_manifest.repetitions, profile.minimum_repetitions),
      .gpu_lane_lock_acquired = !profile.require_exclusive_gpu_lane,
      /* Until a repetition reports one, no presentation configuration is
         claimed. */
      .provenance = {.actual_present = VKR_HARNESS_PRESENT_UNKNOWN},
  };
  if (report.requested_repetitions > VKR_HARNESS_MAX_RUNS) {
    vkr_harness_stderr("Requested repetitions exceed %u\n",
                       VKR_HARNESS_MAX_RUNS);
    vkr_harness_emit_result("invalid", VKR_HARNESS_EXIT_INVALID, NULL, NULL);
    return VKR_HARNESS_EXIT_INVALID;
  }
  vkr_harness_timestamp_utc(report.provenance.started_at);
  vkr_harness_provenance_collect(executable, repo_root, &report.provenance);
  if (!profile.authoritative) {
    vkr_harness_report_add_authority_reason(&report, "profile.local_only");
  }
  if (report.provenance.dirty) {
    if (!profile.allow_dirty) {
      vkr_harness_stderr("Execution profile requires a clean worktree\n");
      vkr_harness_emit_result("unavailable", VKR_HARNESS_EXIT_UNAVAILABLE, NULL,
                              NULL);
      return VKR_HARNESS_EXIT_UNAVAILABLE;
    }
    vkr_harness_report_add_authority_reason(&report, "provenance.dirty");
  }

  char artifact_candidate[VKR_HARNESS_PATH_MAX];
  char artifact_root[VKR_HARNESS_PATH_MAX];
  char run_root[VKR_HARNESS_PATH_MAX];
  string_format(artifact_candidate, sizeof(artifact_candidate), "%s/%s",
                repo_root, VKR_HARNESS_ARTIFACT_ROOT);
  if (!vkr_harness_make_directories(artifact_candidate, &error) ||
      !vkr_harness_resolve_existing_path(repo_root, VKR_HARNESS_ARTIFACT_ROOT,
                                         artifact_root, &error) ||
      !vkr_harness_create_run_root(artifact_root, report.run_id, run_root)) {
    vkr_harness_stderr("Unable to create a unique artifact run directory: %s\n",
                       error.message);
    vkr_harness_emit_result("error", VKR_HARNESS_EXIT_ERROR, NULL, NULL);
    return VKR_HARNESS_EXIT_ERROR;
  }
  /* Every repetition's samples stay live until the aggregate report is
     written, so they share one bump allocation released at exit. */
  VkrHarnessArenas arenas = {.persistent = arena_create(),
                             .transient = arena_create()};
  if (!arenas.persistent || !arenas.transient) {
    vkr_harness_stderr("Unable to create the aggregation arenas\n");
    arena_destroy(arenas.persistent);
    arena_destroy(arenas.transient);
    vkr_harness_emit_result("error", VKR_HARNESS_EXIT_ERROR, NULL, NULL);
    return VKR_HARNESS_EXIT_ERROR;
  }
  VkrHarnessGpuLane lane;
  vkr_harness_gpu_lane_init(&lane);
  if (profile.require_exclusive_gpu_lane) {
    report.gpu_lane_lock_acquired =
        vkr_harness_gpu_lane_acquire(&lane, artifact_root);
    if (!report.gpu_lane_lock_acquired) {
      vkr_harness_stderr("Exclusive GPU lane is busy\n");
      arena_destroy(arenas.transient);
      arena_destroy(arenas.persistent);
      vkr_harness_emit_result("unavailable", VKR_HARNESS_EXIT_UNAVAILABLE, NULL,
                              NULL);
      return VKR_HARNESS_EXIT_UNAVAILABLE;
    }
  }

  VkrHarnessSampleSet runs[VKR_HARNESS_MAX_RUNS] = {0};
  for (uint32_t run = 0; run < report.requested_repetitions; ++run) {
    VkrHarnessRunReference *reference = &report.runs[report.run_count++];
    if (!vkr_harness_execute_repetition(
            &arenas, executable, repo_root, case_path, profile_path, run_root,
            run, &case_manifest, reference, &runs[run], &error)) {
      break;
    }
    if (run > 0u && !vkr_harness_run_is_compatible(&runs[0], &runs[run])) {
      string_format(reference->status, sizeof(reference->status),
                    "incompatible");
      break;
    }
    if (run == 0u) {
      vkr_harness_adopt_run_provenance(&report.provenance, &runs[0].header);
    }
    string_format(reference->status, sizeof(reference->status), "pass");
    report.completed_repetitions++;
  }
  bool8_t snapshot_dropped = false_v;
  if (!vkr_harness_collect_run_evidence(&report, runs, arenas.persistent,
                                        &snapshot_dropped)) {
    vkr_harness_report_mark_incomplete(&report, "events.unavailable");
  }
  if (report.completed_repetitions > 0u &&
      !vkr_harness_aggregate_runs(&arenas, &report, &case_manifest, runs,
                                  &error)) {
    vkr_harness_report_mark_incomplete(&report, "aggregate.failed");
  }
  vkr_harness_apply_verdict(&report, &case_manifest, &profile, runs,
                            snapshot_dropped);

  VkrHarnessFingerprintField environment[VKR_HARNESS_ENVIRONMENT_FIELD_COUNT];
  const uint32_t environment_count = vkr_harness_environment_fields(
      &report.provenance, profile.require_exclusive_gpu_lane, environment);
  VkrHarnessCase effective_case = case_manifest;
  effective_case.target_image_count =
      report.provenance.actual_target_image_count;
  if (!vkr_harness_case_fingerprints(
          VKR_HARNESS_TOOL_PROFILE, &effective_case, &profile, environment,
          environment_count, report.environment_fingerprint,
          report.workload_fingerprint, report.policy_fingerprint, &error)) {
    vkr_harness_report_mark_incomplete(&report, "comparison.unavailable");
  }

  char final_report[VKR_HARNESS_PATH_MAX];
  char summary[VKR_HARNESS_PATH_MAX];
  string_format(final_report, sizeof(final_report), "%s/report.json", run_root);
  string_format(summary, sizeof(summary), "%s/summary.csv", run_root);
  vkr_harness_timestamp_utc(report.provenance.ended_at);
  bool8_t artifacts_complete = true_v;
  for (uint32_t run = 0; run < report.completed_repetitions; ++run) {
    artifacts_complete &=
        vkr_harness_record_run_artifacts(&report, run_root, run);
  }
  if (!vkr_harness_summary_csv_write(summary, &report, arenas.transient,
                                     &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    vkr_harness_report_mark_incomplete(&report, "artifacts.incomplete");
  } else {
    artifacts_complete &= vkr_harness_report_add_artifact(
        &report, "summary.metrics", "summary.csv", "text/csv", summary);
  }
  if (!artifacts_complete) {
    vkr_harness_report_mark_incomplete(&report, "artifacts.incomplete");
  }
  /* The report is the last write: a killed process leaves an incomplete run
     directory rather than a plausible partial report. */
  if (!vkr_harness_report_write(final_report, &report, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    vkr_harness_report_set_status(&report, "error", VKR_HARNESS_EXIT_ERROR);
    vkr_harness_emit_result("error", VKR_HARNESS_EXIT_ERROR, NULL, NULL);
  } else {
    char report_digest[VKR_HARNESS_DIGEST_MAX] = {0};
    char relative[VKR_HARNESS_PATH_MAX];
    (void)vkr_harness_sha256_file(final_report, report_digest);
    string_format(relative, sizeof(relative), "%s/%s/report.json",
                  VKR_HARNESS_ARTIFACT_ROOT, report.run_id);
    vkr_harness_emit_result(report.status, report.exit_code, relative,
                            report_digest);
  }

  vkr_harness_gpu_lane_release(&lane);
  arena_destroy(arenas.transient);
  arena_destroy(arenas.persistent);
  return report.exit_code;
#endif
}
