#include "vkr_harness_runtime.h"

#define VKR_HARNESS_SNAPSHOT_ARTIFACT_ROOT "build/_artifacts/snapshot"

static bool8_t
vkr_harness_snapshot_create_root(const char *artifact_root, char run_id[64],
                                 char run_root[VKR_HARNESS_PATH_MAX]) {
  for (uint32_t attempt = 0; attempt < 16u; ++attempt) {
    if (!vkr_harness_generate_run_id(run_id)) {
      return false_v;
    }
    string_format(run_root, VKR_HARNESS_PATH_MAX, "%s/%s", artifact_root,
                  run_id);
    FilePath path = vkr_harness_file_path(run_root);
    FileError result = file_create_directory_exclusive(&path);
    if (result == FILE_ERROR_NONE) {
      return true_v;
    }
    if (result != FILE_ERROR_ALREADY_EXISTS) {
      return false_v;
    }
  }
  return false_v;
}

static int vkr_harness_snapshot_spawn(
    const char *executable, const char *repo_root, const char *case_path,
    const char *profile_path, const char *run_dir, uint32_t capture_index,
    const char *cache_path, uint32_t timeout_ms, bool8_t prewarm) {
  char stdout_path[VKR_HARNESS_PATH_MAX];
  char stderr_path[VKR_HARNESS_PATH_MAX];
  char index[16];
  string_format(stdout_path, sizeof(stdout_path), "%s/stdout.log", run_dir);
  string_format(stderr_path, sizeof(stderr_path), "%s/stderr.log", run_dir);
  string_format(index, sizeof(index), "%u", capture_index);
  const char *arguments[12] = {
      prewarm ? "--child-profile" : "--child-snapshot",
      "--repo-root",
      repo_root,
      "--case",
      case_path,
      "--profile",
      profile_path,
      "--run-dir",
      run_dir,
  };
  uint32_t argument_count = 9u;
  if (prewarm) {
    arguments[argument_count++] = "--prewarm";
  } else {
    arguments[argument_count++] = "--capture-index";
    arguments[argument_count++] = index;
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
                             const char *case_path, const char *profile_path) {
  VkrHarnessError error = {0};
  VkrHarnessCase case_manifest = {0};
  VkrHarnessProfile profile = {0};
  if (!vkr_harness_case_load(repo_root, case_path, &case_manifest, &error) ||
      !vkr_harness_profile_load(repo_root, profile_path, &profile, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    return VKR_HARNESS_EXIT_INVALID;
  }
  const char *unsupported = vkr_harness_unsupported(&case_manifest, &profile);
  if (unsupported) {
    vkr_harness_stderr("%s\n", unsupported);
    return VKR_HARNESS_EXIT_UNAVAILABLE;
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

  char artifact_candidate[VKR_HARNESS_PATH_MAX];
  char artifact_root[VKR_HARNESS_PATH_MAX];
  char run_root[VKR_HARNESS_PATH_MAX];
  string_format(artifact_candidate, sizeof(artifact_candidate), "%s/%s",
                repo_root, VKR_HARNESS_SNAPSHOT_ARTIFACT_ROOT);
  VkrHarnessReport report = {
      .tool = VKR_HARNESS_TOOL_SNAPSHOT,
      .case_manifest = case_manifest,
      .profile = profile,
      .profile_compatible = true_v,
      .subsystem_mask = subsystem_plan.effective_mask,
      .requested_repetitions = case_manifest.capture_count,
      .provenance = {.actual_present = VKR_HARNESS_PRESENT_UNKNOWN},
  };
  if (!vkr_harness_make_directories(artifact_candidate, &error) ||
      !vkr_harness_resolve_existing_path(repo_root,
                                         VKR_HARNESS_SNAPSHOT_ARTIFACT_ROOT,
                                         artifact_root, &error) ||
      !vkr_harness_snapshot_create_root(artifact_root, report.run_id,
                                        run_root)) {
    vkr_harness_stderr("Unable to create snapshot run directory: %s\n",
                       error.message);
    return VKR_HARNESS_EXIT_ERROR;
  }
  vkr_harness_timestamp_utc(report.provenance.started_at);
  vkr_harness_provenance_collect(executable, repo_root, &report.provenance);
  vkr_harness_report_add_authority_reason(&report,
                                          "execution.diagnostic_replay");
  if (report.provenance.dirty) {
    vkr_harness_report_add_authority_reason(&report, "provenance.dirty");
  }

  /* Holds the aggregate capture/artifact tables and each child summary it is
     built from, so both stay live until the final report is written. */
  Arena *summary_arena = arena_create();
  if (!summary_arena) {
    return VKR_HARNESS_EXIT_ERROR;
  }
  const uint32_t merged_captures =
      case_manifest.capture_count * VKR_HARNESS_MAX_CAPTURE_CHANNELS;
  if (!vkr_harness_report_init_storage(
          &report, summary_arena, merged_captures,
          (merged_captures * VKR_HARNESS_ARTIFACTS_PER_CAPTURE) +
              case_manifest.capture_count)) {
    vkr_harness_stderr("Unable to size the snapshot report tables\n");
    arena_destroy(summary_arena);
    return VKR_HARNESS_EXIT_ERROR;
  }
  for (uint32_t i = 0; i < case_manifest.capture_count; ++i) {
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
          vkr_harness_snapshot_spawn(executable, repo_root, case_path,
                                     profile_path, prewarm_dir, i, cache_path,
                                     case_manifest.repetition_timeout_ms,
                                     true_v) != VKR_HARNESS_EXIT_PASS) {
        break;
      }
    }
    const int child_exit = vkr_harness_snapshot_spawn(
        executable, repo_root, case_path, profile_path, child_dir, i,
        cache_path, case_manifest.repetition_timeout_ms, false_v);
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
    if (report.completed_repetitions == 0u) {
      string_format(report.provenance.gpu, sizeof(report.provenance.gpu), "%s",
                    summary.provenance.gpu);
      string_format(report.provenance.driver, sizeof(report.provenance.driver),
                    "%s", summary.provenance.driver);
      string_format(report.provenance.color_format,
                    sizeof(report.provenance.color_format), "%s",
                    summary.provenance.color_format);
      string_format(report.provenance.color_space,
                    sizeof(report.provenance.color_space), "%s",
                    summary.provenance.color_space);
      report.provenance.gpu_vendor_id = summary.provenance.gpu_vendor_id;
      report.provenance.gpu_device_id = summary.provenance.gpu_device_id;
      report.provenance.actual_present = summary.provenance.actual_present;
      report.provenance.actual_target_image_count =
          summary.provenance.actual_target_image_count;
    }
    scratch_destroy(summary_scratch, ARENA_MEMORY_TAG_ARRAY);
    report.completed_repetitions++;
  }

  VkrHarnessFingerprintField environment[VKR_HARNESS_ENVIRONMENT_FIELD_COUNT];
  const uint32_t environment_count =
      vkr_harness_environment_fields(&report.provenance, false_v, environment);
  (void)vkr_harness_case_fingerprints(
      VKR_HARNESS_TOOL_SNAPSHOT, &case_manifest, &profile,
      report.subsystem_mask, environment, environment_count,
      report.environment_fingerprint, report.workload_fingerprint,
      report.policy_fingerprint, &error);
  vkr_harness_timestamp_utc(report.provenance.ended_at);
  const bool8_t complete =
      report.completed_repetitions == case_manifest.capture_count;
  vkr_harness_report_set_status(&report, complete ? "pass" : "incomplete",
                                complete ? VKR_HARNESS_EXIT_PASS
                                         : VKR_HARNESS_EXIT_ERROR);
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
                     complete ? "pass" : "incomplete",
                     complete ? VKR_HARNESS_EXIT_PASS : VKR_HARNESS_EXIT_ERROR,
                     relative, digest);
  return complete ? VKR_HARNESS_EXIT_PASS : VKR_HARNESS_EXIT_ERROR;
}
