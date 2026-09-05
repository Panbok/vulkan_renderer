#include "core/vkr_subsystem_plan.h"
#include "vkr_harness_json.h"
#include "vkr_harness_runtime.h"

#define VKR_HARNESS_AUTOTEST_ARTIFACT_ROOT "build/_artifacts/autotest"

/** The single JSON line a `profile` or `snapshot` invocation prints. */
typedef struct VkrHarnessCommandResult {
  char status[24];
  VkrHarnessExitCode exit_code;
  char report[VKR_HARNESS_PATH_MAX];
  char sha256[VKR_HARNESS_DIGEST_MAX];
} VkrHarnessCommandResult;

vkr_internal bool8_t vkr_harness_autotest_result_parse(
    const char *path, Arena *arena, VkrHarnessCommandResult *result,
    VkrHarnessError *error) {
  uint8_t *bytes = NULL;
  uint64_t length = 0u;
  VkrHarnessJsonDocument *document =
      arena_alloc(arena, sizeof(*document), ARENA_MEMORY_TAG_STRUCT);
  if (!document || !vkr_harness_read_file(path, arena, &bytes, &length) ||
      !vkr_harness_json_parse(document, (const char *)bytes, length, error)) {
    return false_v;
  }
  /* `vkr_harness_json_object_get` clears the flag per lookup, so each key needs
     its own duplicate check or only the last one would ever be answered. */
  bool8_t duplicate = false_v;
  bool8_t any_duplicate = false_v;
  const int32_t status =
      vkr_harness_json_object_get(document, 0, "status", &duplicate);
  any_duplicate |= duplicate;
  const int32_t exit_code =
      vkr_harness_json_object_get(document, 0, "exit_code", &duplicate);
  any_duplicate |= duplicate;
  const int32_t report =
      vkr_harness_json_object_get(document, 0, "report", &duplicate);
  any_duplicate |= duplicate;
  const int32_t digest =
      vkr_harness_json_object_get(document, 0, "sha256", &duplicate);
  any_duplicate |= duplicate;
  uint64_t code = 0u;
  if (status < 0 || exit_code < 0 || report < 0 || digest < 0 ||
      any_duplicate ||
      !vkr_harness_json_string(document, status, result->status,
                               sizeof(result->status), "status", error) ||
      !vkr_harness_json_u64(document, exit_code, &code, "exit_code", error) ||
      code > VKR_HARNESS_EXIT_ERROR ||
      !vkr_harness_json_string(document, report, result->report,
                               sizeof(result->report), "report", error) ||
      !vkr_harness_json_string(document, digest, result->sha256,
                               sizeof(result->sha256), "sha256", error)) {
    return false_v;
  }
  result->exit_code = (VkrHarnessExitCode)code;
  return true_v;
}

vkr_internal int vkr_harness_autotest_spawn(
    const char *executable, const char *repo_root, const char *command,
    const char *case_path, const char *profile_path,
    const char *artifact_root_relative, const char *stdout_path,
    const char *stderr_path, uint32_t timeout_ms) {
  const char *arguments[] = {
      command,      "--repo-root",     repo_root,
      "--case",     case_path,         "--profile",
      profile_path, "--artifact-root", artifact_root_relative};
  const VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .working_directory = repo_root,
      .stdout_path = stdout_path,
      .stderr_path = stderr_path,
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
 * Verifies that a child's self-reported report really is the artifact under
 * this run's root, then records it as a digest- and fingerprint-carrying
 * reference. A child that named a report outside the run root is rejected
 * rather than quoted.
 */
vkr_internal bool8_t vkr_harness_autotest_reference(
    const char *repo_root, const char *run_relative,
    const VkrHarnessCommandResult *source, const char *absolute_root,
    Arena *arena, VkrHarnessRunReference *reference) {
  const uint64_t prefix = string_length(run_relative);
  if (!string_n_equals(source->report, run_relative, prefix) ||
      source->report[prefix] != '/') {
    return false_v;
  }
  char absolute[VKR_HARNESS_PATH_MAX];
  char digest[VKR_HARNESS_DIGEST_MAX];
  string_format(absolute, sizeof(absolute), "%s/%s", repo_root, source->report);
  if (!vkr_harness_existing_path_is_below(absolute_root, absolute) ||
      !vkr_harness_sha256_file(absolute, digest) ||
      !string_equals(digest, source->sha256)) {
    return false_v;
  }
  string_format(reference->status, sizeof(reference->status), "%s",
                source->status);
  string_format(reference->report, sizeof(reference->report), "%s",
                source->report + prefix + 1u);
  string_format(reference->sha256, sizeof(reference->sha256), "%s",
                source->sha256);
  return vkr_harness_report_read_fingerprints(absolute, arena, reference);
}

/**
 * The combined run makes no observations of its own, so its comparison
 * identity is exactly the pair of child identities it references.
 */
vkr_internal void vkr_harness_autotest_combine_fingerprint(
    const char *primary, const char *snapshot,
    char out_digest[VKR_HARNESS_DIGEST_MAX], VkrHarnessError *error) {
  VkrHarnessFingerprintField fields[2] = {0};
  string_format(fields[0].name, sizeof(fields[0].name), "primary");
  string_format(fields[0].value, sizeof(fields[0].value), "%s", primary);
  string_format(fields[1].name, sizeof(fields[1].name), "snapshot");
  string_format(fields[1].value, sizeof(fields[1].value), "%s", snapshot);
  (void)vkr_harness_fingerprint(fields, ArrayCount(fields), out_digest, error);
}

/**
 * The worst outcome of the pair wins, ordered by how little the run proves:
 * an errored or invalid child says nothing about the other's verdict, and a
 * pass cannot be claimed without an accepted baseline to have passed against.
 */
vkr_internal VkrHarnessExitCode vkr_harness_autotest_verdict(
    VkrHarnessExitCode primary, VkrHarnessExitCode snapshot,
    bool8_t baseline_available) {
  if (primary == VKR_HARNESS_EXIT_ERROR || snapshot == VKR_HARNESS_EXIT_ERROR) {
    return VKR_HARNESS_EXIT_ERROR;
  }
  if (primary == VKR_HARNESS_EXIT_INVALID ||
      snapshot == VKR_HARNESS_EXIT_INVALID) {
    return VKR_HARNESS_EXIT_INVALID;
  }
  if (primary == VKR_HARNESS_EXIT_UNAVAILABLE ||
      snapshot == VKR_HARNESS_EXIT_UNAVAILABLE) {
    return VKR_HARNESS_EXIT_UNAVAILABLE;
  }
  if (!baseline_available || primary == VKR_HARNESS_EXIT_MISSING_BASELINE ||
      snapshot == VKR_HARNESS_EXIT_MISSING_BASELINE) {
    return VKR_HARNESS_EXIT_MISSING_BASELINE;
  }
  return primary == VKR_HARNESS_EXIT_FAIL || snapshot == VKR_HARNESS_EXIT_FAIL
             ? VKR_HARNESS_EXIT_FAIL
             : VKR_HARNESS_EXIT_PASS;
}

int vkr_harness_autotest_run(const char *executable, const char *repo_root,
                             const char *case_path, const char *profile_path) {
  VkrHarnessError error = {0};
  VkrHarnessCase case_manifest = {0};
  VkrHarnessProfile profile = {0};
  VkrSubsystemPlan subsystem_plan = {0};
  if (!vkr_harness_case_load(repo_root, case_path, &case_manifest, &error) ||
      !vkr_harness_profile_load(repo_root, profile_path, &profile, &error) ||
      case_manifest.capture_count == 0u ||
      !vkr_harness_subsystem_plan(VKR_HARNESS_TOOL_AUTOTEST, &case_manifest,
                                  &subsystem_plan, &error)) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
    return VKR_HARNESS_EXIT_INVALID;
  }
  VkrRendererBackendType renderer_backend = VKR_RENDERER_BACKEND_TYPE_VULKAN;
  if (!vkr_harness_renderer_backend_resolve(
          &case_manifest.renderer, getenv("VKR_HARNESS_RENDERER_BACKEND"),
          &renderer_backend)) {
    vkr_harness_stderr(
        "Case renderer backend conflicts with the environment\n");
    return VKR_HARNESS_EXIT_INVALID;
  }
  (void)renderer_backend;
  Arena *arena = arena_create();
  if (!arena) {
    return VKR_HARNESS_EXIT_ERROR;
  }
  int result = VKR_HARNESS_EXIT_ERROR;
  VkrHarnessReport report = {
      .tool = VKR_HARNESS_TOOL_AUTOTEST,
      .authoritative = profile.authoritative,
      .case_manifest = case_manifest,
      .profile = profile,
      .profile_compatible = true_v,
      .subsystem_mask = subsystem_plan.effective_mask,
      .requested_repetitions = 2u,
      .provenance = {.actual_target = case_manifest.target,
                     .actual_present = VKR_HARNESS_PRESENT_UNKNOWN},
  };
  /* Assertions are evaluated against the metrics of the child that produced
     them and are published in that child's report. Folding them into a run
     that aggregates nothing would claim an evaluation this level never made. */
  report.case_manifest.assertion_count = 0u;
  char artifact_candidate[VKR_HARNESS_PATH_MAX];
  char artifact_root[VKR_HARNESS_PATH_MAX];
  char run_root[VKR_HARNESS_PATH_MAX];
  string_format(artifact_candidate, sizeof(artifact_candidate), "%s/%s",
                repo_root, VKR_HARNESS_AUTOTEST_ARTIFACT_ROOT);
  if (!vkr_harness_make_directories(artifact_candidate, &error) ||
      !vkr_harness_realpath(artifact_candidate, artifact_root) ||
      !vkr_harness_create_run_root(artifact_root, report.run_id, run_root) ||
      !vkr_harness_report_init_storage(&report, arena, 0u, 8u) ||
      !vkr_harness_report_init_auxiliary_runs(&report, arena, 1u)) {
    goto cleanup;
  }
  vkr_harness_timestamp_utc(report.provenance.started_at);
  vkr_harness_provenance_collect(executable, repo_root, &report.provenance);
  if (!profile.authoritative) {
    vkr_harness_report_add_authority_reason(&report, "profile.local_only");
  }
  if (report.provenance.dirty) {
    vkr_harness_report_add_authority_reason(&report, "provenance.dirty");
  }
  char run_relative[VKR_HARNESS_PATH_MAX];
  char primary_relative[VKR_HARNESS_PATH_MAX];
  char snapshot_relative[VKR_HARNESS_PATH_MAX];
  string_format(run_relative, sizeof(run_relative), "%s/%s",
                VKR_HARNESS_AUTOTEST_ARTIFACT_ROOT, report.run_id);
  string_format(primary_relative, sizeof(primary_relative), "%s/primary",
                run_relative);
  string_format(snapshot_relative, sizeof(snapshot_relative), "%s/snapshot",
                run_relative);
  char primary_stdout[VKR_HARNESS_PATH_MAX];
  char primary_stderr[VKR_HARNESS_PATH_MAX];
  char snapshot_stdout[VKR_HARNESS_PATH_MAX];
  char snapshot_stderr[VKR_HARNESS_PATH_MAX];
  string_format(primary_stdout, sizeof(primary_stdout), "%s/primary.stdout.log",
                run_root);
  string_format(primary_stderr, sizeof(primary_stderr), "%s/primary.stderr.log",
                run_root);
  string_format(snapshot_stdout, sizeof(snapshot_stdout),
                "%s/snapshot.stdout.log", run_root);
  string_format(snapshot_stderr, sizeof(snapshot_stderr),
                "%s/snapshot.stderr.log", run_root);
  /* One child runs every repetition the profile requires and the other one
     replay per capture checkpoint, so the timeout has to cover both shapes. */
  const uint64_t command_timeout_wide =
      (uint64_t)case_manifest.repetition_timeout_ms *
      (Max(case_manifest.repetitions, profile.minimum_repetitions) +
       case_manifest.capture_count + 2u);
  const uint32_t command_timeout =
      (uint32_t)Min(command_timeout_wide, (uint64_t)UINT32_MAX);
  const int primary_exit = vkr_harness_autotest_spawn(
      executable, repo_root, "profile", case_path, profile_path,
      primary_relative, primary_stdout, primary_stderr, command_timeout);
  const int snapshot_exit = vkr_harness_autotest_spawn(
      executable, repo_root, "snapshot", case_path, profile_path,
      snapshot_relative, snapshot_stdout, snapshot_stderr, command_timeout);
  VkrHarnessCommandResult primary = {0};
  VkrHarnessCommandResult snapshot = {0};
  if (!vkr_harness_autotest_result_parse(primary_stdout, arena, &primary,
                                         &error) ||
      !vkr_harness_autotest_result_parse(snapshot_stdout, arena, &snapshot,
                                         &error) ||
      primary_exit != primary.exit_code ||
      snapshot_exit != snapshot.exit_code) {
    vkr_harness_report_mark_incomplete(&report, "autotest.child_result");
    goto publish;
  }
  report.runs[0].index = 0u;
  report.auxiliary_runs[0].index = 0u;
  if (!vkr_harness_autotest_reference(repo_root, run_relative, &primary,
                                      run_root, arena, &report.runs[0]) ||
      !vkr_harness_autotest_reference(repo_root, run_relative, &snapshot,
                                      run_root, arena,
                                      &report.auxiliary_runs[0])) {
    vkr_harness_report_mark_incomplete(&report, "autotest.child_artifact");
    goto publish;
  }
  report.run_count = 1u;
  report.auxiliary_run_count = 1u;
  report.completed_repetitions = 2u;
  vkr_harness_autotest_combine_fingerprint(
      report.runs[0].environment_fingerprint,
      report.auxiliary_runs[0].environment_fingerprint,
      report.environment_fingerprint, &error);
  vkr_harness_autotest_combine_fingerprint(
      report.runs[0].workload_fingerprint,
      report.auxiliary_runs[0].workload_fingerprint,
      report.workload_fingerprint, &error);
  vkr_harness_autotest_combine_fingerprint(
      report.runs[0].policy_fingerprint,
      report.auxiliary_runs[0].policy_fingerprint, report.policy_fingerprint,
      &error);
  {
    char baseline_root[VKR_HARNESS_PATH_MAX];
    VkrHarnessCaptureSummary baseline = {0};
    VkrHarnessError baseline_error = {0};
    const bool8_t baseline_available = vkr_harness_baseline_current(
        repo_root, profile.id, case_manifest.id, arena, baseline_root,
        &baseline, &baseline_error);
    const VkrHarnessExitCode verdict = vkr_harness_autotest_verdict(
        primary.exit_code, snapshot.exit_code, baseline_available);
    vkr_harness_report_set_status(&report, vkr_harness_exit_code_name(verdict),
                                  verdict);
    if (!baseline_available) {
      vkr_harness_report_add_authority_reason(&report, "baseline.missing");
    }
  }
publish:
  vkr_harness_timestamp_utc(report.provenance.ended_at);
  const struct {
    const char *role;
    const char *relative;
    const char *absolute;
  } artifacts[] = {
      {"autotest.primary_stdout", "primary.stdout.log", primary_stdout},
      {"autotest.primary_stderr", "primary.stderr.log", primary_stderr},
      {"autotest.snapshot_stdout", "snapshot.stdout.log", snapshot_stdout},
      {"autotest.snapshot_stderr", "snapshot.stderr.log", snapshot_stderr},
  };
  for (uint32_t i = 0; i < ArrayCount(artifacts); ++i) {
    FilePath path = vkr_harness_file_path(artifacts[i].absolute);
    if (file_exists(&path)) {
      (void)vkr_harness_report_add_artifact(&report, artifacts[i].role,
                                            artifacts[i].relative, "text/plain",
                                            artifacts[i].absolute);
    }
  }
  if (primary.report[0] != '\0') {
    char primary_report_path[VKR_HARNESS_PATH_MAX];
    char primary_run_root[VKR_HARNESS_PATH_MAX];
    char primary_manifest_path[VKR_HARNESS_PATH_MAX];
    if (!vkr_harness_resolve_existing_path(repo_root, primary.report,
                                           primary_report_path, &error) ||
        !vkr_harness_path_parent(primary_report_path, primary_run_root) ||
        string_format(primary_manifest_path, sizeof(primary_manifest_path),
                      "%s/scene-content-manifest.json",
                      primary_run_root) <= 0 ||
        !vkr_harness_existing_path_is_below(run_root, primary_manifest_path) ||
        !vkr_harness_report_add_artifact(
            &report, "scene.content_manifest",
            primary_manifest_path + string_length(run_root) + 1u,
            "application/json", primary_manifest_path)) {
      vkr_harness_report_mark_incomplete(&report, "scene_manifest.unavailable");
    }
  } else {
    vkr_harness_report_mark_incomplete(&report, "scene_manifest.unavailable");
  }
  char report_path[VKR_HARNESS_PATH_MAX];
  char digest[VKR_HARNESS_DIGEST_MAX];
  string_format(report_path, sizeof(report_path), "%s/report.json", run_root);
  if (!vkr_harness_report_write(report_path, &report, &error) ||
      !vkr_harness_sha256_file(report_path, digest)) {
    goto cleanup;
  }
  vkr_harness_stdout(
      "{\"status\":\"%s\",\"exit_code\":%u,\"report\":\"%s/report.json\","
      "\"sha256\":\"%s\"}\n",
      report.status, report.exit_code, run_relative, digest);
  result = report.exit_code;
cleanup:
  if (result == VKR_HARNESS_EXIT_ERROR && error.message[0]) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
  }
  arena_destroy(arena);
  return result;
}
