#include "harness_test.h"

#include "vkr_harness.h"
#include "vkr_harness_json.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

static const char *HARNESS_CASE_FORMAT =
    "{\"schema_version\":1,\"id\":\"smoke.test.static\",\"suite\":\"smoke\","
    "\"scene\":\"assets/scenes/default.scene.json\",\"seed\":1,"
    "\"resolution\":[64,64],\"boot\":\"full\",\"target\":\"%s\","
    "\"present\":\"%s\",\"cache\":\"isolated_cold\",\"fixed_delta\":0.016,"
    "\"frames\":{\"measure\":3},\"renderer\":{\"editor\":false,"
    "\"skybox\":true,\"shadow_preset\":\"default\",\"shadow_cascades\":4},"
    "\"camera\":%s%s}";

static bool8_t harness_parse_case(const char *target, const char *present,
                                  const char *camera, const char *tail,
                                  VkrHarnessCase *out_case) {
  char json[8192];
  snprintf(json, sizeof(json), HARNESS_CASE_FORMAT, target, present, camera,
           tail ? tail : "");
  VkrHarnessError error = {0};
  return vkr_harness_case_parse(json, strlen(json), "memory", out_case, &error);
}

static void test_harness_hash_and_statistics(void) {
  printf("  Running test_harness_hash_and_statistics...\n");
  char digest[VKR_HARNESS_DIGEST_MAX];
  vkr_harness_sha256_bytes("abc", 3u, digest);
  assert(strcmp(digest,
                "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
                "b410ff61f20015ad") == 0);
  const float64_t samples[] = {4.0, 1.0, 3.0, 2.0};
  float64_t sort_scratch[ArrayCount(samples)];
  VkrHarnessStatistics stats = {0};
  assert(vkr_harness_statistics_compute(samples, ArrayCount(samples), 1u,
                                        sort_scratch, &stats));
  assert(stats.mean == 2.5 && stats.p50 == 2.0 && stats.p95 == 4.0);
  assert(stats.invalid_count == 1u);
  uint8_t pass_flags[] = {
      VKR_HARNESS_PASS_FLAG_CPU_VALID | VKR_HARNESS_PASS_FLAG_GPU_VALID,
      VKR_HARNESS_PASS_FLAG_CULLED,
      VKR_HARNESS_PASS_FLAG_CPU_VALID | VKR_HARNESS_PASS_FLAG_GPU_VALID,
  };
  assert(vkr_harness_gpu_pass_samples_complete(pass_flags,
                                               ArrayCount(pass_flags)));
  assert(!vkr_harness_gpu_pass_samples_complete(NULL, 0u));
  pass_flags[1] = 0u;
  assert(!vkr_harness_gpu_pass_samples_complete(pass_flags,
                                                ArrayCount(pass_flags)));
  pass_flags[1] = VKR_HARNESS_PASS_FLAG_DISABLED;
  pass_flags[2] = VKR_HARNESS_PASS_FLAG_CPU_VALID;
  assert(!vkr_harness_gpu_pass_samples_complete(pass_flags,
                                                ArrayCount(pass_flags)));
  printf("  test_harness_hash_and_statistics PASSED\n");
}

static void test_harness_case_parser(void) {
  printf("  Running test_harness_case_parser...\n");
  const char *static_camera =
      "{\"mode\":\"static\",\"position\":[1,2,3],\"yaw\":10,\"pitch\":-5}";
  VkrHarnessCase parsed = {0};
  assert(harness_parse_case("windowed_hidden", "immediate", static_camera, "",
                            &parsed));
  assert(parsed.warmup_frames == 120u && parsed.repetitions == 1u);
  assert(parsed.camera.speed == VKR_HARNESS_SPEED_MEDIUM);
  assert(!harness_parse_case("offscreen", "fifo", static_camera, "", &parsed));
  assert(!harness_parse_case(
      "windowed_hidden", "immediate",
      "{\"mode\":\"keyframes\",\"keys\":[{\"t\":1,\"position\":[0,0,0],"
      "\"yaw\":0,\"pitch\":0},{\"t\":0,\"position\":[1,0,0],\"yaw\":0,"
      "\"pitch\":0}]}",
      "", &parsed));
  assert(!harness_parse_case(
      "windowed_hidden", "immediate", static_camera,
      ",\"captures\":[{\"at_frame\":3,\"channels\":[\"final_color\"]}]",
      &parsed));
  assert(!harness_parse_case(
      "windowed_hidden", "immediate", static_camera,
      ",\"captures\":[{\"at_frame\":1,\"channels\":[\"final_color\","
      "\"final_color\"]}]",
      &parsed));
  assert(!harness_parse_case(
      "windowed_hidden", "immediate", static_camera,
      ",\"captures\":[{\"at_frame\":1,\"channels\":[\"not_a_channel\"]}]",
      &parsed));
  assert(!harness_parse_case("windowed_hidden", "immediate", static_camera,
                             ",\"unknown\":1", &parsed));
  printf("  test_harness_case_parser PASSED\n");
}

static void test_harness_profile_parser(void) {
  printf("  Running test_harness_profile_parser...\n");
  const char *profile =
      "{\"schema_version\":1,\"id\":\"local.test\",\"authoritative\":false,"
      "\"dirty_policy\":\"allow\",\"environment\":{\"target\":"
      "\"windowed_hidden\",\"required_present\":\"immediate\","
      "\"require_actual_present\":true},\"instrumentation\":{\"gpu_timing\":"
      "false,\"event_subjects\":false},\"execution\":{\"minimum_repetitions\":"
      "2,"
      "\"warmup_stability_window\":10,\"warmup_max_drift_ratio\":0.1,"
      "\"require_warmup_stability\":true,\"exclusive_gpu_lane\":false},"
      "\"required_metrics\":[\"cpu.render_submit\"]}";
  VkrHarnessProfile parsed = {0};
  VkrHarnessError error = {0};
  assert(vkr_harness_profile_parse(profile, strlen(profile), "memory", &parsed,
                                   &error));
  assert(parsed.minimum_repetitions == 2u &&
         parsed.required_metric_count == 1u);
  const char *one_authoritative =
      "{\"schema_version\":1,\"id\":\"performance.test\","
      "\"authoritative\":true,\"dirty_policy\":\"require_clean\","
      "\"environment\":{\"target\":\"windowed_hidden\","
      "\"required_present\":\"immediate\",\"require_actual_present\":true},"
      "\"instrumentation\":{\"gpu_timing\":false,"
      "\"event_subjects\":false},\"execution\":{"
      "\"minimum_repetitions\":1,\"warmup_stability_window\":20,"
      "\"warmup_max_drift_ratio\":0.1,"
      "\"require_warmup_stability\":true,"
      "\"exclusive_gpu_lane\":true},\"required_metrics\":[]}";
  assert(!vkr_harness_profile_parse(
      one_authoritative, strlen(one_authoritative), "memory", &parsed, &error));
  /* A rejected policy must say which rule it broke; an empty code reaches the
     operator as a bare "invalid manifest". */
  assert(string_equals(error.code, "profile.authoritative_repetitions"));
  char duplicate[4096];
  snprintf(duplicate, sizeof(duplicate), "%.*s,\"extra\":true}",
           (int)strlen(profile) - 1, profile);
  assert(!vkr_harness_profile_parse(duplicate, strlen(duplicate), "memory",
                                    &parsed, &error));
  printf("  test_harness_profile_parser PASSED\n");
}

static void test_harness_camera_determinism(void) {
  printf("  Running test_harness_camera_determinism...\n");
  VkrHarnessCamera camera = {
      .mode = VKR_HARNESS_CAMERA_KEYFRAMES,
      .interpolation = VKR_HARNESS_CAMERA_INTERPOLATION_CATMULL_ROM,
      .speed = VKR_HARNESS_SPEED_FAST,
      .vertical_fov_degrees = 70.0f,
      .near_plane = 0.1f,
      .far_plane = 100.0f,
      .key_count = 3u,
      .keys =
          {{.time_seconds = 0.0, .position = {0, 0, 0}, .yaw_degrees = 170},
           {.time_seconds = 1.0, .position = {1, 2, 0}, .yaw_degrees = -170},
           {.time_seconds = 2.0, .position = {2, 0, 1}, .yaw_degrees = -90}},
  };
  VkrHarnessError error = {0};
  assert(vkr_harness_camera_prepare(&camera, &error));
  for (uint32_t frame = 0; frame < 100u; ++frame) {
    VkrHarnessCameraPose first = {0};
    VkrHarnessCameraPose second = {0};
    assert(vkr_harness_camera_evaluate(&camera, frame / 60.0, &first));
    assert(vkr_harness_camera_evaluate(&camera, frame / 60.0, &second));
    assert(memcmp(&first, &second, sizeof(first)) == 0);
  }
  printf("  test_harness_camera_determinism PASSED\n");
}

static void test_harness_fingerprints(void) {
  printf("  Running test_harness_fingerprints...\n");
  VkrHarnessFingerprintField a[] = {{.name = "z", .value = "2"},
                                    {.name = "a", .value = "1"}};
  VkrHarnessFingerprintField b[] = {{.name = "a", .value = "1"},
                                    {.name = "z", .value = "2"}};
  VkrHarnessError error = {0};
  char first[VKR_HARNESS_DIGEST_MAX];
  char second[VKR_HARNESS_DIGEST_MAX];
  assert(vkr_harness_fingerprint(a, 2u, first, &error));
  assert(vkr_harness_fingerprint(b, 2u, second, &error));
  assert(strcmp(first, second) == 0);
  b[1].value[0] = '3';
  assert(vkr_harness_fingerprint(b, 2u, second, &error));
  assert(strcmp(first, second) != 0);

  const char *static_camera =
      "{\"mode\":\"static\",\"position\":[1,2,3],\"yaw\":10,\"pitch\":-5}";
  VkrHarnessCase case_manifest = {0};
  assert(harness_parse_case("windowed_hidden", "immediate", static_camera, "",
                            &case_manifest));
  VkrHarnessProfile profile = {.required_present =
                                   VKR_HARNESS_PRESENT_IMMEDIATE};
  char environment[VKR_HARNESS_DIGEST_MAX];
  char workload[VKR_HARNESS_DIGEST_MAX];
  char policy[VKR_HARNESS_DIGEST_MAX];
  assert(vkr_harness_case_fingerprints(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                       &profile, VKR_RENDERER_SUBSYSTEM_ALL,
                                       NULL, 0u, environment, workload, policy,
                                       &error));
  char original_workload[VKR_HARNESS_DIGEST_MAX];
  snprintf(original_workload, sizeof(original_workload), "%s", workload);
  snprintf(case_manifest.description, sizeof(case_manifest.description),
           "provenance-only edit");
  snprintf(case_manifest.manifest_sha256, sizeof(case_manifest.manifest_sha256),
           "sha256:changed");
  assert(vkr_harness_case_fingerprints(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                       &profile, VKR_RENDERER_SUBSYSTEM_ALL,
                                       NULL, 0u, environment, workload, policy,
                                       &error));
  assert(strcmp(original_workload, workload) == 0);
  assert(vkr_harness_case_fingerprints(
      VKR_HARNESS_TOOL_PROFILE, &case_manifest, &profile,
      VKR_RENDERER_SUBSYSTEM_ALL &
          ~VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_UI),
      NULL, 0u, environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.width++;
  assert(vkr_harness_case_fingerprints(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                       &profile, VKR_RENDERER_SUBSYSTEM_ALL,
                                       NULL, 0u, environment, workload, policy,
                                       &error));
  assert(strcmp(original_workload, workload) != 0);
  printf("  test_harness_fingerprints PASSED\n");
}

static void test_harness_subsystem_plans(void) {
  printf("  Running test_harness_subsystem_plans...\n");
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  VkrSubsystemPlan plan = {0};
  assert(vkr_renderer_subsystem_plan_build(VKR_BOOT_PROFILE_FULL, 0u, 0u, &plan,
                                           &renderer_error));
  assert(plan.effective_mask == VKR_RENDERER_SUBSYSTEM_ALL);

  /* The two published masks must partition the enum, or the harness would
     exclude a unit the renderer initializes unconditionally. */
  assert((VKR_RENDERER_SUBSYSTEM_MANDATORY & VKR_RENDERER_SUBSYSTEM_OPTIONAL) ==
         0u);
  assert((VKR_RENDERER_SUBSYSTEM_MANDATORY | VKR_RENDERER_SUBSYSTEM_OPTIONAL) ==
         VKR_RENDERER_SUBSYSTEM_ALL);

  /* The rejection reason is optional; a NULL out_error is not itself an error.
   */
  assert(vkr_renderer_subsystem_plan_build(VKR_BOOT_PROFILE_AUTOMATION, 0u, 0u,
                                           &plan, NULL));
  assert(plan.effective_mask == VKR_RENDERER_SUBSYSTEM_MANDATORY);

  const VkrSubsystemMask skybox =
      VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_SKYBOX);
  assert(vkr_renderer_subsystem_plan_build(
      VKR_BOOT_PROFILE_AUTOMATION, skybox,
      VKR_RENDERER_SUBSYSTEM_OPTIONAL & ~skybox, &plan, &renderer_error));
  assert(vkr_renderer_subsystem_plan_includes(&plan,
                                              VKR_RENDERER_SUBSYSTEM_SKYBOX));
  assert(vkr_renderer_subsystem_plan_includes(&plan,
                                              VKR_RENDERER_SUBSYSTEM_FONTS));
  assert(
      !vkr_renderer_subsystem_plan_includes(&plan, VKR_RENDERER_SUBSYSTEM_UI));

  assert(!vkr_renderer_subsystem_plan_build(
      VKR_BOOT_PROFILE_AUTOMATION,
      VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_UI),
      VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_FONTS), &plan,
      &renderer_error));
  assert(renderer_error == VKR_RENDERER_ERROR_INVALID_PARAMETER);

  const char *static_camera =
      "{\"mode\":\"static\",\"position\":[1,2,3],\"yaw\":10,\"pitch\":-5}";
  VkrHarnessCase case_manifest = {0};
  assert(harness_parse_case("windowed_hidden", "immediate", static_camera, "",
                            &case_manifest));
  case_manifest.boot = VKR_HARNESS_BOOT_AUTOMATION;
  VkrHarnessError harness_error = {0};
  assert(vkr_harness_subsystem_plan(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                    &plan, &harness_error));
  assert(vkr_renderer_subsystem_plan_includes(&plan,
                                              VKR_RENDERER_SUBSYSTEM_SKYBOX));
  assert(!vkr_renderer_subsystem_plan_includes(&plan,
                                               VKR_RENDERER_SUBSYSTEM_EDITOR));
  case_manifest.assertion_count = 1u;
  snprintf(case_manifest.assertions[0].metric,
           sizeof(case_manifest.assertions[0].metric), "draw.ui.calls_issued");
  assert(vkr_harness_subsystem_plan(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                    &plan, &harness_error));
  assert(
      vkr_renderer_subsystem_plan_includes(&plan, VKR_RENDERER_SUBSYSTEM_UI));
  assert(vkr_renderer_subsystem_plan_includes(&plan,
                                              VKR_RENDERER_SUBSYSTEM_FONTS));
  /* Both spellings of a subsystem's own metric family request it, and a name
     that merely contains the word does not. */
  snprintf(case_manifest.assertions[0].metric,
           sizeof(case_manifest.assertions[0].metric), "picking.readbacks");
  assert(vkr_harness_subsystem_plan(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                    &plan, &harness_error));
  assert(vkr_renderer_subsystem_plan_includes(&plan,
                                              VKR_RENDERER_SUBSYSTEM_PICKING));
  snprintf(case_manifest.assertions[0].metric,
           sizeof(case_manifest.assertions[0].metric),
           "draw.world.picking_ish");
  assert(vkr_harness_subsystem_plan(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                    &plan, &harness_error));
  assert(!vkr_renderer_subsystem_plan_includes(&plan,
                                               VKR_RENDERER_SUBSYSTEM_PICKING));

  /* The report schema pins this spelling; the workload fingerprint hashes it.
   */
  char mask_text[VKR_HARNESS_SUBSYSTEM_MASK_MAX];
  vkr_harness_format_subsystem_mask(mask_text, VKR_RENDERER_SUBSYSTEM_ALL);
  assert(strcmp(mask_text, "0x000000000007ffff") == 0);
  vkr_harness_format_subsystem_mask(mask_text,
                                    VKR_RENDERER_SUBSYSTEM_MANDATORY);
  assert(strcmp(mask_text, "0x0000000000003fff") == 0);
  printf("  test_harness_subsystem_plans PASSED\n");
}

static void test_harness_case_profile_pairing(void) {
  printf("  Running test_harness_case_profile_pairing...\n");
  const char *static_camera =
      "{\"mode\":\"static\",\"position\":[1,2,3],\"yaw\":10,\"pitch\":-5}";
  VkrHarnessCase case_manifest = {0};
  assert(harness_parse_case("windowed_hidden", "immediate", static_camera, "",
                            &case_manifest));
  VkrHarnessProfile profile = {
      .target = case_manifest.target,
      .required_present = case_manifest.present,
      .require_warmup_stability = true_v,
      .warmup_stability_window = case_manifest.warmup_frames,
  };
  assert(vkr_harness_case_profile_mismatch(&case_manifest, &profile) == NULL);

  /* A window wider than the warmup can never be answered, so the pairing is
     rejected instead of running every repetition into an incomplete verdict. */
  profile.warmup_stability_window = case_manifest.warmup_frames + 1u;
  assert(vkr_harness_case_profile_mismatch(&case_manifest, &profile) != NULL);
  profile.require_warmup_stability = false_v;
  assert(vkr_harness_case_profile_mismatch(&case_manifest, &profile) == NULL);

  profile.required_present = VKR_HARNESS_PRESENT_FIFO;
  assert(vkr_harness_case_profile_mismatch(&case_manifest, &profile) != NULL);
  profile.required_present = case_manifest.present;
  profile.target = VKR_HARNESS_TARGET_WINDOWED_VISIBLE;
  assert(vkr_harness_case_profile_mismatch(&case_manifest, &profile) != NULL);
  printf("  test_harness_case_profile_pairing PASSED\n");
}

static void test_harness_assertion_verdict(void) {
  printf("  Running test_harness_assertion_verdict...\n");
  for (uint32_t i = 0; i < VKR_HARNESS_STAT_COUNT; ++i) {
    VkrHarnessStatisticKind parsed = VKR_HARNESS_STAT_COUNT;
    assert(vkr_harness_statistic_from_name(
        vkr_harness_statistic_name((VkrHarnessStatisticKind)i), &parsed));
    assert(parsed == (VkrHarnessStatisticKind)i);
  }
  assert(!vkr_harness_statistic_from_name("p99", NULL));

  VkrHarnessMetricResult metric = {
      .statistics = {.sample_count = 10u, .mean = 5.0, .min = 4.0}};
  snprintf(metric.name, sizeof(metric.name), "draw.calls_issued");
  snprintf(metric.unit, sizeof(metric.unit), "count");
  VkrHarnessReport report = {.metrics = &metric, .metric_count = 1u};

  VkrHarnessAssertion assertion = {.statistic = VKR_HARNESS_STAT_MEAN,
                                   .operation = VKR_HARNESS_ASSERT_MAX,
                                   .limit = 5.0};
  snprintf(assertion.metric, sizeof(assertion.metric), "draw.calls_issued");
  float64_t actual = 0.0;
  assert(vkr_harness_assertion_evaluate(&report, &assertion, &actual) ==
         VKR_HARNESS_ASSERTION_PASS);
  assert(actual == 5.0);
  assertion.limit = 4.5;
  assert(vkr_harness_assertion_evaluate(&report, &assertion, &actual) ==
         VKR_HARNESS_ASSERTION_FAIL);
  assertion.operation = VKR_HARNESS_ASSERT_EQUALS;
  assertion.limit = 5.25;
  assertion.tolerance = 0.5;
  assert(vkr_harness_assertion_evaluate(&report, &assertion, &actual) ==
         VKR_HARNESS_ASSERTION_PASS);
  assertion.tolerance = 0.1;
  assert(vkr_harness_assertion_evaluate(&report, &assertion, &actual) ==
         VKR_HARNESS_ASSERTION_FAIL);

  /* Missing or partially invalid evidence is never a pass and never a fail. */
  snprintf(assertion.metric, sizeof(assertion.metric), "no.such.metric");
  assert(vkr_harness_assertion_evaluate(&report, &assertion, &actual) ==
         VKR_HARNESS_ASSERTION_INCOMPLETE);
  assert(actual == 0.0);
  snprintf(assertion.metric, sizeof(assertion.metric), "draw.calls_issued");
  metric.statistics.invalid_count = 1u;
  assert(vkr_harness_assertion_evaluate(&report, &assertion, &actual) ==
         VKR_HARNESS_ASSERTION_INCOMPLETE);
  metric.statistics.invalid_count = 0u;
  metric.statistics.sample_count = 0u;
  assert(vkr_harness_assertion_evaluate(&report, &assertion, &actual) ==
         VKR_HARNESS_ASSERTION_INCOMPLETE);
  printf("  test_harness_assertion_verdict PASSED\n");
}

static void test_harness_report_shape(void) {
  printf("  Running test_harness_report_shape...\n");
#if !defined(_WIN32)
  char directory[] = "/tmp/vkr-harness-report-XXXXXX";
  assert(mkdtemp(directory));
  char path[512];
  snprintf(path, sizeof(path), "%s/report.json", directory);
  VkrHarnessReport report = {
      .tool = VKR_HARNESS_TOOL_PROFILE,
      .exit_code = VKR_HARNESS_EXIT_PASS,
      .profile_compatible = true_v,
      .subsystem_mask = UINT64_C(0x1234),
  };
  snprintf(report.run_id, sizeof(report.run_id), "test-run");
  snprintf(report.status, sizeof(report.status), "pass");
  snprintf(report.case_manifest.id, sizeof(report.case_manifest.id),
           "smoke.test.static");
  snprintf(report.case_manifest.suite, sizeof(report.case_manifest.suite),
           "smoke");
  snprintf(report.case_manifest.manifest_sha256,
           sizeof(report.case_manifest.manifest_sha256),
           "sha256:"
           "0000000000000000000000000000000000000000000000000000000000000000");
  snprintf(report.profile.id, sizeof(report.profile.id), "local.test");
  snprintf(report.profile.manifest_sha256,
           sizeof(report.profile.manifest_sha256),
           "sha256:"
           "0000000000000000000000000000000000000000000000000000000000000000");
  snprintf(report.environment_fingerprint,
           sizeof(report.environment_fingerprint),
           "sha256:"
           "0000000000000000000000000000000000000000000000000000000000000000");
  snprintf(report.workload_fingerprint, sizeof(report.workload_fingerprint),
           "%s", report.environment_fingerprint);
  snprintf(report.policy_fingerprint, sizeof(report.policy_fingerprint), "%s",
           report.environment_fingerprint);
  snprintf(report.authority_reasons[0], sizeof(report.authority_reasons[0]),
           "profile.local_only");
  report.authority_reason_count = 1u;
  VkrHarnessError error = {0};
  assert(vkr_harness_report_write(path, &report, &error));
  FILE *file = fopen(path, "rb");
  assert(file && fseek(file, 0, SEEK_END) == 0);
  const long length = ftell(file);
  assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
  char *json = malloc((size_t)length);
  assert(json && fread(json, 1u, (size_t)length, file) == (size_t)length);
  fclose(file);
  assert(strstr(json, "\"subsystem_mask\":\"0x0000000000001234\"") != NULL);
  VkrHarnessJsonDocument document = {0};
  assert(vkr_harness_json_parse(&document, json, (uint64_t)length, &error));
  static const char *const fields[] = {"schema_version",
                                       "kind",
                                       "tool",
                                       "tool_version",
                                       "run_id",
                                       "status",
                                       "exit_code",
                                       "authoritative",
                                       "authority_reasons",
                                       "case",
                                       "profile",
                                       "provenance",
                                       "comparison",
                                       "effective_config",
                                       "execution",
                                       "runs",
                                       "auxiliary_runs",
                                       "aggregate",
                                       "events",
                                       "captures",
                                       "assertions",
                                       "diagnostics",
                                       "artifacts"};
  assert(vkr_harness_json_object_validate(&document, 0, fields,
                                          ArrayCount(fields), fields,
                                          ArrayCount(fields), "$", &error));
  free(json);
  unlink(path);
  rmdir(directory);
#endif
  printf("  test_harness_report_shape PASSED\n");
}

static void test_harness_safe_paths(void) {
  printf("  Running test_harness_safe_paths...\n");
  assert(vkr_harness_path_is_safe_relative("tools/cases/smoke/case.json"));
  assert(!vkr_harness_path_is_safe_relative("../case.json"));
  assert(!vkr_harness_path_is_safe_relative("/tmp/case.json"));
  assert(!vkr_harness_path_is_safe_relative("a//b"));
#if !defined(_WIN32)
  char root[] = "/tmp/vkr-harness-test-XXXXXX";
  assert(mkdtemp(root));
  char link_path[512];
  snprintf(link_path, sizeof(link_path), "%s/escape", root);
  assert(symlink("/tmp", link_path) == 0);
  char resolved[VKR_HARNESS_PATH_MAX];
  VkrHarnessError error = {0};
  assert(!vkr_harness_resolve_existing_path(root, "escape", resolved, &error));
  assert(!vkr_harness_resolve_output_path(root, "escape/report.json", resolved,
                                          &error));
  unlink(link_path);
  rmdir(root);
#endif
  printf("  test_harness_safe_paths PASSED\n");
}

static void test_harness_platform_process_primitives(void) {
  printf("  Running test_harness_platform_process_primitives...\n");
  const char *arguments[] = {"--version"};
  char output[256];
  int32_t exit_code = -1;
  assert(vkr_platform_process_capture("git", arguments, ArrayCount(arguments),
                                      NULL, output, sizeof(output),
                                      &exit_code));
  assert(exit_code == 0);
  assert(string_find(output, "git version") != NULL);

  char lock_name[96];
  string_format(lock_name, sizeof(lock_name), "VkrHarnessTest%u",
                vkr_platform_get_process_id());
  VkrPlatformProcessLock first = {0};
  VkrPlatformProcessLock second = {0};
  assert(
      vkr_platform_process_lock_acquire(lock_name, PROJECT_SOURCE_DIR, &first));
  assert(!vkr_platform_process_lock_acquire(lock_name, PROJECT_SOURCE_DIR,
                                            &second));
  vkr_platform_process_lock_release(&first);
  assert(vkr_platform_process_lock_acquire(lock_name, PROJECT_SOURCE_DIR,
                                           &second));
  vkr_platform_process_lock_release(&second);
#if !defined(_WIN32)
  char lock_path[VKR_HARNESS_PATH_MAX];
  string_format(lock_path, sizeof(lock_path), "%s/%s.lock", PROJECT_SOURCE_DIR,
                lock_name);
  FilePath lock_file = vkr_harness_file_path(lock_path);
  assert(file_remove(&lock_file) == FILE_ERROR_NONE);
#endif
  printf("  test_harness_platform_process_primitives PASSED\n");
}

bool32_t run_harness_tests(void) {
  printf("--- Running Harness tests... ---\n");
  test_harness_hash_and_statistics();
  test_harness_case_parser();
  test_harness_profile_parser();
  test_harness_camera_determinism();
  test_harness_fingerprints();
  test_harness_subsystem_plans();
  test_harness_case_profile_pairing();
  test_harness_assertion_verdict();
  test_harness_report_shape();
  test_harness_safe_paths();
  test_harness_platform_process_primitives();
  printf("--- Harness tests completed. ---\n");
  return true;
}
