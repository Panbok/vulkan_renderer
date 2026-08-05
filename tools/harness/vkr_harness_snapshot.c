#include "vkr_harness_runtime.h"

#define VKR_HARNESS_SNAPSHOT_ARTIFACT_ROOT "build/_artifacts/snapshot"

static int vkr_harness_snapshot_spawn(
    const char *executable, const char *repo_root, const char *case_path,
    const char *profile_path, const char *run_dir, uint32_t capture_index,
    const char *replay_mode, const char *cache_path, uint32_t timeout_ms,
    bool8_t prewarm, const char *scene_content_digest) {
  char stdout_path[VKR_HARNESS_PATH_MAX];
  char stderr_path[VKR_HARNESS_PATH_MAX];
  char index[16];
  string_format(stdout_path, sizeof(stdout_path), "%s/stdout.log", run_dir);
  string_format(stderr_path, sizeof(stderr_path), "%s/stderr.log", run_dir);
  string_format(index, sizeof(index), "%u", capture_index);
  const char *arguments[16] = {
      prewarm ? "--child-profile" : "--child-snapshot",
      "--repo-root",
      repo_root,
      "--case",
      case_path,
      "--profile",
      profile_path,
      "--run-dir",
      run_dir,
      "--scene-content-digest",
      scene_content_digest,
  };
  uint32_t argument_count = 11u;
  if (prewarm) {
    arguments[argument_count++] = "--prewarm";
  } else {
    arguments[argument_count++] = "--capture-index";
    arguments[argument_count++] = index;
    arguments[argument_count++] = "--capture-mode";
    arguments[argument_count++] = replay_mode;
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

static bool8_t vkr_harness_snapshot_is_capture_role(const char *role) {
  return string_n_equals(role, "capture.", 8u);
}

/**
 * Re-roots one child-relative path under this checkpoint's directory.
 * Truncation is a failure, not a silently shortened path that would name a
 * different file than the one whose digest was verified.
 */
static bool8_t vkr_harness_snapshot_reroot(char *destination, uint64_t capacity,
                                           const char *child_relative,
                                           const char *source) {
  if (!vkr_harness_path_is_safe_relative(source)) {
    return false_v;
  }
  const int32_t written =
      string_format(destination, capacity, "%s/%s", child_relative, source);
  return written > 0 && (uint64_t)written < capacity;
}

/**
 * Adopts one child's captures into the aggregate report, after proving every
 * artifact it claims still hashes to the digest the child recorded.
 */
static bool8_t vkr_harness_snapshot_merge_summary(
    VkrHarnessReport *report, const char *child_relative, const char *child_dir,
    const VkrHarnessCaptureSummary *summary) {
  if (!report || !child_relative || !child_dir || !summary) {
    return false_v;
  }
  uint32_t adopted_artifacts = 0u;
  for (uint32_t i = 0; i < summary->artifact_count; ++i) {
    adopted_artifacts +=
        vkr_harness_snapshot_is_capture_role(summary->artifacts[i].role) ? 1u
                                                                         : 0u;
  }
  if (report->capture_count + summary->capture_count >
          report->capture_capacity ||
      report->artifact_count + adopted_artifacts > report->artifact_capacity) {
    return false_v;
  }

  for (uint32_t i = 0; i < summary->artifact_count; ++i) {
    const VkrHarnessArtifact *artifact = &summary->artifacts[i];
    if (!vkr_harness_snapshot_is_capture_role(artifact->role)) {
      continue;
    }
    if (!vkr_harness_path_is_safe_relative(artifact->path)) {
      return false_v;
    }
    char absolute[VKR_HARNESS_PATH_MAX];
    char digest[VKR_HARNESS_DIGEST_MAX];
    string_format(absolute, sizeof(absolute), "%s/%s", child_dir,
                  artifact->path);
    if (!vkr_harness_sha256_file(absolute, digest) ||
        !string_equals(digest, artifact->sha256)) {
      return false_v;
    }
  }

  for (uint32_t i = 0; i < summary->capture_count; ++i) {
    const VkrHarnessCaptureResult *source = &summary->captures[i];
    VkrHarnessCaptureResult *capture = &report->captures[report->capture_count];
    *capture = *source;
    if (!vkr_harness_snapshot_reroot(capture->data_path,
                                     sizeof(capture->data_path), child_relative,
                                     source->data_path) ||
        !vkr_harness_snapshot_reroot(capture->preview_path,
                                     sizeof(capture->preview_path),
                                     child_relative, source->preview_path) ||
        !vkr_harness_snapshot_reroot(capture->metadata_path,
                                     sizeof(capture->metadata_path),
                                     child_relative, source->metadata_path)) {
      return false_v;
    }
    report->capture_count++;
  }

  for (uint32_t i = 0; i < summary->artifact_count; ++i) {
    const VkrHarnessArtifact *source = &summary->artifacts[i];
    if (!vkr_harness_snapshot_is_capture_role(source->role)) {
      continue;
    }
    VkrHarnessArtifact *artifact = &report->artifacts[report->artifact_count];
    *artifact = *source;
    if (!vkr_harness_snapshot_reroot(artifact->path, sizeof(artifact->path),
                                     child_relative, source->path)) {
      return false_v;
    }
    report->artifact_count++;
  }
  return true_v;
}

int vkr_harness_snapshot_run(const char *executable, const char *repo_root,
                             const char *case_path, const char *profile_path,
                             const char *artifact_root_override) {
  VkrHarnessError error = {0};
  const char *artifact_root_relative =
      artifact_root_override && artifact_root_override[0]
          ? artifact_root_override
          : VKR_HARNESS_SNAPSHOT_ARTIFACT_ROOT;
  if (!vkr_harness_path_is_safe_relative(artifact_root_relative)) {
    return VKR_HARNESS_EXIT_INVALID;
  }
  VkrHarnessCase case_manifest = {0};
  VkrHarnessProfile profile = {0};
  if (!vkr_harness_case_load(repo_root, case_path, &case_manifest, &error) ||
      !vkr_harness_profile_load(repo_root, profile_path, &profile, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    return VKR_HARNESS_EXIT_INVALID;
  }
  if (case_manifest.capture_count == 0u) {
    vkr_harness_stderr(
        "Snapshot requires at least one captures[] checkpoint\n");
    return VKR_HARNESS_EXIT_INVALID;
  }
  VkrSubsystemPlan subsystem_plan = {0};
  if (!vkr_harness_subsystem_plan(VKR_HARNESS_TOOL_SNAPSHOT, &case_manifest,
                                  &subsystem_plan, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    return VKR_HARNESS_EXIT_INVALID;
  }
  Arena *summary_arena = arena_create();
  if (!summary_arena) {
    return VKR_HARNESS_EXIT_ERROR;
  }
  VkrHarnessCaptureReplay *replays =
      arena_alloc(summary_arena, sizeof(*replays) * VKR_HARNESS_MAX_REPLAYS,
                  ARENA_MEMORY_TAG_ARRAY);
  uint32_t replay_count = 0u;
  if (!replays || !vkr_harness_capture_replays_build(&case_manifest, replays,
                                                     VKR_HARNESS_MAX_REPLAYS,
                                                     &replay_count, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    arena_destroy(summary_arena);
    return VKR_HARNESS_EXIT_INVALID;
  }

  char artifact_candidate[VKR_HARNESS_PATH_MAX];
  char artifact_root[VKR_HARNESS_PATH_MAX];
  char run_root[VKR_HARNESS_PATH_MAX];
  string_format(artifact_candidate, sizeof(artifact_candidate), "%s/%s",
                repo_root, artifact_root_relative);
  VkrHarnessReport report = {
      .tool = VKR_HARNESS_TOOL_SNAPSHOT,
      .authoritative = profile.authoritative,
      .case_manifest = case_manifest,
      .profile = profile,
      .profile_compatible = true_v,
      .subsystem_mask = subsystem_plan.effective_mask,
      .requested_repetitions = replay_count,
      .provenance = {.actual_target = case_manifest.target,
                     .actual_present = VKR_HARNESS_PRESENT_UNKNOWN},
  };
  if (!vkr_harness_make_directories(artifact_candidate, &error) ||
      !vkr_harness_resolve_existing_path(repo_root, artifact_root_relative,
                                         artifact_root, &error) ||
      !vkr_harness_create_run_root(artifact_root, report.run_id, run_root)) {
    vkr_harness_stderr("Unable to create snapshot run directory: %s\n",
                       error.message);
    arena_destroy(summary_arena);
    return VKR_HARNESS_EXIT_ERROR;
  }
  vkr_harness_timestamp_utc(report.provenance.started_at);
  vkr_harness_provenance_collect(executable, repo_root, &report.provenance);
  if (!profile.authoritative) {
    vkr_harness_report_add_authority_reason(&report, "profile.local_only");
  }
  if (report.provenance.dirty) {
    vkr_harness_report_add_authority_reason(&report, "provenance.dirty");
  }

  /* Holds the aggregate capture/artifact tables and each child summary it is
     built from, so both stay live until the final report is written. */
  uint32_t merged_captures = 0u;
  for (uint32_t i = 0; i < case_manifest.capture_count; ++i) {
    merged_captures += case_manifest.captures[i].channel_count;
  }
  if (!vkr_harness_report_init_storage(
          &report, summary_arena, merged_captures,
          (merged_captures * VKR_HARNESS_ARTIFACTS_PER_CAPTURE) + replay_count +
              merged_captures + 2u) ||
      !vkr_harness_report_init_auxiliary_runs(&report, summary_arena,
                                              replay_count)) {
    vkr_harness_stderr("Unable to size the snapshot report tables\n");
    arena_destroy(summary_arena);
    return VKR_HARNESS_EXIT_ERROR;
  }
  VkrHarnessSceneManifest scene_manifest = {0};
  char scene_manifest_path[VKR_HARNESS_PATH_MAX];
  string_format(scene_manifest_path, sizeof(scene_manifest_path),
                "%s/scene-content-manifest.json", run_root);
  if (!vkr_harness_scene_manifest_build(repo_root, case_manifest.scene,
                                        summary_arena, &scene_manifest,
                                        &error) ||
      !vkr_harness_scene_manifest_write(scene_manifest_path, &scene_manifest,
                                        &error) ||
      !vkr_harness_report_add_artifact(
          &report, "scene.content_manifest", "scene-content-manifest.json",
          "application/json", scene_manifest_path)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    arena_destroy(summary_arena);
    return VKR_HARNESS_EXIT_ERROR;
  }
  for (uint32_t i = 0; i < replay_count; ++i) {
    const VkrHarnessCaptureReplay *replay = &replays[i];
    char child_relative[VKR_HARNESS_PATH_MAX];
    char child_dir[VKR_HARNESS_PATH_MAX];
    char child_report[VKR_HARNESS_PATH_MAX];
    char child_summary[VKR_HARNESS_PATH_MAX];
    string_format(child_relative, sizeof(child_relative), "captures/%u", i);
    string_format(child_dir, sizeof(child_dir), "%s/%s", run_root,
                  child_relative);
    if (!vkr_harness_make_directories(child_dir, &error)) {
      break;
    }
    char cache_path[VKR_HARNESS_PATH_MAX] = {0};
    if (case_manifest.cache != VKR_HARNESS_CACHE_SHARED) {
      string_format(cache_path, sizeof(cache_path), "%s/pipeline.cache",
                    child_dir);
    }
    if (case_manifest.cache == VKR_HARNESS_CACHE_ISOLATED_WARM) {
      char prewarm_dir[VKR_HARNESS_PATH_MAX];
      string_format(prewarm_dir, sizeof(prewarm_dir), "%s/prewarm", child_dir);
      if (!vkr_harness_make_directories(prewarm_dir, &error) ||
          vkr_harness_snapshot_spawn(
              executable, repo_root, case_path, profile_path, prewarm_dir,
              replay->capture_index, replay->mode, cache_path,
              case_manifest.repetition_timeout_ms, true_v,
              scene_manifest.sha256) != VKR_HARNESS_EXIT_PASS) {
        break;
      }
    }
    const int child_exit = vkr_harness_snapshot_spawn(
        executable, repo_root, case_path, profile_path, child_dir,
        replay->capture_index, replay->mode, cache_path,
        case_manifest.repetition_timeout_ms, false_v, scene_manifest.sha256);
    VkrHarnessRunReference *reference =
        &report.auxiliary_runs[report.auxiliary_run_count++];
    reference->index = i;
    string_format(reference->status, sizeof(reference->status), "%s",
                  child_exit == VKR_HARNESS_EXIT_PASS ? "pass" : "incomplete");
    string_format(reference->report, sizeof(reference->report),
                  "%s/report.json", child_relative);
    string_format(child_report, sizeof(child_report), "%s/report.json",
                  child_dir);
    string_format(child_summary, sizeof(child_summary),
                  "%s/capture-summary.bin", child_dir);
    Scratch summary_scratch = scratch_create(summary_arena);
    VkrHarnessCaptureSummary summary = {0};
    if (child_exit != VKR_HARNESS_EXIT_PASS ||
        !vkr_harness_sha256_file(child_report, reference->sha256) ||
        !vkr_harness_capture_summary_read(child_summary, summary_arena,
                                          &summary) ||
        !vkr_harness_snapshot_merge_summary(&report, child_relative, child_dir,
                                            &summary) ||
        !vkr_harness_report_add_artifact(&report, "capture.auxiliary_report",
                                         reference->report, "application/json",
                                         child_report)) {
      scratch_destroy(summary_scratch, ARENA_MEMORY_TAG_ARRAY);
      break;
    }
    string_format(reference->environment_fingerprint,
                  sizeof(reference->environment_fingerprint), "%s",
                  summary.environment_fingerprint);
    string_format(reference->workload_fingerprint,
                  sizeof(reference->workload_fingerprint), "%s",
                  summary.workload_fingerprint);
    string_format(reference->policy_fingerprint,
                  sizeof(reference->policy_fingerprint), "%s",
                  summary.policy_fingerprint);
    if (report.completed_repetitions == 0u) {
      string_format(report.provenance.gpu, sizeof(report.provenance.gpu), "%s",
                    summary.provenance.gpu);
      string_format(report.provenance.driver, sizeof(report.provenance.driver),
                    "%s", summary.provenance.driver);
      string_format(report.provenance.color_format,
                    sizeof(report.provenance.color_format), "%s",
                    summary.provenance.color_format);
      string_format(report.provenance.depth_format,
                    sizeof(report.provenance.depth_format), "%s",
                    summary.provenance.depth_format);
      string_format(report.provenance.color_space,
                    sizeof(report.provenance.color_space), "%s",
                    summary.provenance.color_space);
      report.provenance.gpu_vendor_id = summary.provenance.gpu_vendor_id;
      report.provenance.gpu_device_id = summary.provenance.gpu_device_id;
      report.provenance.actual_present = summary.provenance.actual_present;
      report.provenance.actual_target = summary.provenance.actual_target;
      report.provenance.actual_target_image_count =
          summary.provenance.actual_target_image_count;
      report.provenance.actual_target_width =
          summary.provenance.actual_target_width;
      report.provenance.actual_target_height =
          summary.provenance.actual_target_height;
    }
    scratch_destroy(summary_scratch, ARENA_MEMORY_TAG_ARRAY);
    report.completed_repetitions++;
  }

  VkrHarnessFingerprintField environment[VKR_HARNESS_ENVIRONMENT_FIELD_COUNT];
  const uint32_t environment_count =
      vkr_harness_environment_fields(&report.provenance, false_v, environment);
  (void)vkr_harness_case_fingerprints_with_scene_digest(
      VKR_HARNESS_TOOL_SNAPSHOT, &case_manifest, &profile,
      report.subsystem_mask, environment, environment_count,
      scene_manifest.sha256, report.environment_fingerprint,
      report.workload_fingerprint, report.policy_fingerprint, &error);
  vkr_harness_timestamp_utc(report.provenance.ended_at);
  const bool8_t complete = report.completed_repetitions == replay_count;
  vkr_harness_report_set_status(&report, complete ? "pass" : "incomplete",
                                complete ? VKR_HARNESS_EXIT_PASS
                                         : VKR_HARNESS_EXIT_ERROR);
  if (complete) {
    char baseline_root[VKR_HARNESS_PATH_MAX];
    VkrHarnessCaptureSummary baseline = {0};
    VkrHarnessError baseline_error = {0};
    if (!vkr_harness_baseline_current(repo_root, profile.id, case_manifest.id,
                                      summary_arena, baseline_root, &baseline,
                                      &baseline_error)) {
      if (string_equals(baseline_error.code, "baseline.missing")) {
        vkr_harness_report_add_authority_reason(&report, "baseline.missing");
      } else {
        vkr_harness_report_mark_incomplete(&report, "baseline.load_failed");
      }
    } else if (!string_equals(report.environment_fingerprint,
                              baseline.environment_fingerprint) ||
               !string_equals(report.workload_fingerprint,
                              baseline.workload_fingerprint) ||
               !string_equals(report.policy_fingerprint,
                              baseline.policy_fingerprint)) {
      vkr_harness_report_set_status(&report, "missing_baseline",
                                    VKR_HARNESS_EXIT_MISSING_BASELINE);
      vkr_harness_report_add_incompatibility(&report,
                                             "baseline.fingerprint_mismatch");
    } else {
      /* Decoded images and diff buffers are scoped to their own arena so a
         wide capture set does not grow the arena the report tables alias. */
      Arena *comparison_transient = arena_create();
      VkrHarnessArenas comparison_arenas = {.persistent = summary_arena,
                                            .transient = comparison_transient};
      const VkrHarnessExitCode comparison =
          comparison_transient
              ? vkr_harness_compare_capture_sets(
                    run_root, baseline_root, report.captures,
                    report.capture_count, baseline.captures,
                    baseline.capture_count, &comparison_arenas, &error)
              : VKR_HARNESS_EXIT_ERROR;
      arena_destroy(comparison_transient);
      if (comparison == VKR_HARNESS_EXIT_FAIL) {
        vkr_harness_report_set_status(&report, "fail", comparison);
      } else if (comparison == VKR_HARNESS_EXIT_MISSING_BASELINE) {
        vkr_harness_report_set_status(&report, "missing_baseline", comparison);
        vkr_harness_report_add_incompatibility(&report,
                                               "baseline.capture_incompatible");
      } else if (comparison == VKR_HARNESS_EXIT_ERROR) {
        vkr_harness_report_mark_incomplete(&report, "baseline.compare_failed");
      }
      vkr_harness_compare_publish_diffs(&report, run_root);
    }
  }
  char aggregate_summary[VKR_HARNESS_PATH_MAX];
  string_format(aggregate_summary, sizeof(aggregate_summary),
                "%s/capture-summary.bin", run_root);
  if (report.exit_code != VKR_HARNESS_EXIT_ERROR &&
      (!vkr_harness_capture_summary_write(aggregate_summary, &report,
                                          summary_arena, &error) ||
       !vkr_harness_report_add_artifact(
           &report, "capture.summary", "capture-summary.bin",
           "application/vnd.vkr.harness-capture-summary", aggregate_summary))) {
    vkr_harness_report_mark_incomplete(&report, "capture.summary_failed");
  }
  char final_report[VKR_HARNESS_PATH_MAX];
  char digest[VKR_HARNESS_DIGEST_MAX];
  string_format(final_report, sizeof(final_report), "%s/report.json", run_root);
  const bool8_t published =
      vkr_harness_report_write(final_report, &report, &error) &&
      vkr_harness_sha256_file(final_report, digest);
  /* The report's capture and artifact tables alias this arena, so it outlives
     the write that reads them. */
  arena_destroy(summary_arena);
  if (!published) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    return VKR_HARNESS_EXIT_ERROR;
  }
  uint64_t repo_root_length = string_length(repo_root);
  while (repo_root_length > 0u && repo_root[repo_root_length - 1u] == '/') {
    repo_root_length--;
  }
  const char *relative = final_report + repo_root_length;
  if (*relative == '/') {
    relative++;
  }
  vkr_harness_stdout("{\"status\":\"%s\",\"exit_code\":%u,\"report\":\"%s\","
                     "\"sha256\":\"%s\"}\n",
                     report.status, report.exit_code, relative, digest);
  return report.exit_code;
}
