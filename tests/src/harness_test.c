#include "harness_test.h"

#include "vkr_harness.h"
#include "vkr_harness_json.h"
#include "vkr_harness_runtime.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
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
      VKR_HARNESS_PASS_FLAG_OMITTED,
      VKR_HARNESS_PASS_FLAG_CPU_VALID | VKR_HARNESS_PASS_FLAG_GPU_VALID,
  };
  assert(vkr_harness_gpu_pass_samples_complete(pass_flags,
                                               ArrayCount(pass_flags)));
  pass_flags[0] = VKR_HARNESS_PASS_FLAG_CPU_VALID |
                  VKR_HARNESS_PASS_FLAG_GPU_UNSUPPORTED_SCOPE;
  assert(vkr_harness_gpu_pass_samples_complete(pass_flags,
                                               ArrayCount(pass_flags)));
  pass_flags[0] = VKR_HARNESS_PASS_FLAG_CPU_VALID |
                  VKR_HARNESS_PASS_FLAG_GPU_VALID |
                  VKR_HARNESS_PASS_FLAG_GPU_UNSUPPORTED_SCOPE;
  assert(!vkr_harness_gpu_pass_samples_complete(pass_flags,
                                                ArrayCount(pass_flags)));
  pass_flags[0] =
      VKR_HARNESS_PASS_FLAG_CPU_VALID | VKR_HARNESS_PASS_FLAG_GPU_VALID;
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

static void test_harness_current_frame_work_metrics(void) {
  printf("  Running test_harness_current_frame_work_metrics...\n");
  assert(vkr_harness_metric_is_current_frame_work("visibility.objects_tested"));
  assert(vkr_harness_metric_is_current_frame_work(
      "visibility.gpu_candidates.count"));
  assert(vkr_harness_metric_is_current_frame_work(
      "visibility.transmission.gpu_candidates.count"));
  assert(vkr_harness_metric_is_current_frame_work(
      "draw.shadow.cascade0.rendered"));
  assert(vkr_harness_metric_is_current_frame_work("instance_buffer.overflows"));
  assert(!vkr_harness_metric_is_current_frame_work(
      "visibility.gpu_visible.count"));
  assert(!vkr_harness_metric_is_current_frame_work(
      "visibility.transmission.covered_pixels.layer_0"));
  assert(!vkr_harness_metric_is_current_frame_work(
      "draw.world.indirect_commands_issued"));
  assert(!vkr_harness_metric_is_current_frame_work(
      "draw.shadow.cascade0.indirect_commands"));
  assert(!vkr_harness_metric_is_current_frame_work(
      "draw.shadow.cascade0.indirect_overflow"));
  assert(!vkr_harness_metric_is_current_frame_work(
      "visibility.gpu_visible.overflow"));
  printf("  test_harness_current_frame_work_metrics PASSED\n");
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
  assert(harness_parse_case("offscreen", "none", static_camera, "", &parsed));
  assert(parsed.target == VKR_HARNESS_TARGET_OFFSCREEN);
  assert(parsed.target_image_count == 3u);
  assert(!parsed.renderer.text_fixture);
  assert(parsed.renderer.taa_enabled);
  assert(parsed.renderer.backend[0] == '\0');
  assert(parsed.renderer.shadow_pcf_samples == 16u);
  assert(parsed.renderer.shadow_pcf_early_out);
  assert(!parsed.renderer.shadow_sdsm);
  assert(parsed.renderer.shadow_split_lambda == 0.80f);
  assert(parsed.renderer.shadow_map_size == 2048u);
  assert(strcmp(parsed.renderer.exposure_mode, "manual") == 0);
  assert(parsed.renderer.manual_exposure == VKR_DEFAULT_EXPOSURE);
  assert(parsed.renderer.exposure_compensation_ev == 0.0f);
  assert(parsed.renderer.exposure_reset_frame == UINT32_MAX);
  assert(harness_parse_case("windowed_hidden", "immediate", static_camera,
                            ",\"resize_round_trip\":[80,72]", &parsed));
  assert(parsed.resize_round_trip && parsed.resize_width == 80u &&
         parsed.resize_height == 72u);
  assert(!harness_parse_case("windowed_hidden", "immediate", static_camera,
                             ",\"resize_round_trip\":[64,64]", &parsed));
  assert(!harness_parse_case("offscreen", "none", static_camera,
                             ",\"resize_round_trip\":[80,72]", &parsed));
  const char *metal_case =
      "{\"schema_version\":1,\"id\":\"smoke.test.metal\",\"suite\":\"smoke\","
      "\"scene\":\"assets/scenes/default.scene.json\",\"seed\":1,"
      "\"resolution\":[64,64],\"boot\":\"full\",\"target\":\"offscreen\","
      "\"present\":\"none\",\"cache\":\"isolated_cold\",\"fixed_delta\":0.016,"
      "\"frames\":{\"measure\":3},\"renderer\":{\"editor\":false,"
      "\"skybox\":true,\"text_fixture\":true,\"taa_enabled\":false,"
      "\"backend\":\"metal\","
      "\"shadow_preset\":\"default\",\"shadow_cascades\":4,"
      "\"shadow_pcf_samples\":4,\"shadow_split_lambda\":0.25,"
      "\"shadow_map_size\":4096,\"shadow_pcf_early_out\":false,"
      "\"shadow_sdsm\":true,\"exposure_mode\":\"automatic\","
      "\"manual_exposure\":0.25,\"exposure_compensation_ev\":1.0,"
      "\"exposure_reset_frame\":1},"
      "\"camera\":{\"mode\":\"static\",\"position\":[1,2,3],\"yaw\":10,"
      "\"pitch\":-5}}";
  VkrHarnessError backend_error = {0};
  assert(vkr_harness_case_parse(metal_case, strlen(metal_case), "memory",
                                &parsed, &backend_error));
  assert(strcmp(parsed.renderer.backend, "metal") == 0);
  assert(!parsed.renderer.taa_enabled);
  assert(parsed.renderer.shadow_pcf_samples == 4u);
  assert(!parsed.renderer.shadow_pcf_early_out);
  assert(parsed.renderer.shadow_sdsm);
  assert(parsed.renderer.shadow_split_lambda == 0.25f);
  assert(parsed.renderer.shadow_map_size == 4096u);
  assert(strcmp(parsed.renderer.exposure_mode, "automatic") == 0);
  assert(parsed.renderer.manual_exposure == 0.25f);
  assert(parsed.renderer.exposure_compensation_ev == 1.0f);
  assert(parsed.renderer.exposure_reset_frame == 1u);
  VkrRendererBackendType resolved_backend = VKR_RENDERER_BACKEND_TYPE_VULKAN;
  assert(vkr_harness_renderer_backend_resolve(&parsed.renderer, NULL,
                                              &resolved_backend));
  assert(resolved_backend == VKR_RENDERER_BACKEND_TYPE_METAL);
  assert(vkr_harness_renderer_backend_resolve(&parsed.renderer, "metal",
                                              &resolved_backend));
  assert(!vkr_harness_renderer_backend_resolve(&parsed.renderer, "vulkan",
                                               &resolved_backend));
  const VkrHarnessRendererConfig unpinned_renderer = {0};
  assert(vkr_harness_renderer_backend_resolve(&unpinned_renderer, NULL,
                                              &resolved_backend));
#if defined(_WIN32)
  assert(resolved_backend == VKR_RENDERER_BACKEND_TYPE_VULKAN);
#else
  assert(resolved_backend == VKR_RENDERER_BACKEND_TYPE_METAL);
#endif
  assert(vkr_harness_renderer_backend_resolve(&unpinned_renderer, "vulkan",
                                              &resolved_backend));
  assert(resolved_backend == VKR_RENDERER_BACKEND_TYPE_VULKAN);
  char invalid_backend[2048];
  snprintf(invalid_backend, sizeof(invalid_backend), "%s", metal_case);
  char *metal_value = strstr(invalid_backend, "\"metal\"");
  assert(metal_value);
  memcpy(metal_value, "\"dx12x\"", 7u);
  assert(!vkr_harness_case_parse(invalid_backend, strlen(invalid_backend),
                                 "memory", &parsed, &backend_error));
  char invalid_pcf[2048];
  snprintf(invalid_pcf, sizeof(invalid_pcf), "%s", metal_case);
  char *pcf_value = strstr(invalid_pcf, "\"shadow_pcf_samples\":4");
  assert(pcf_value);
  pcf_value[strlen("\"shadow_pcf_samples\":")] = '7';
  assert(!vkr_harness_case_parse(invalid_pcf, strlen(invalid_pcf), "memory",
                                 &parsed, &backend_error));
  char invalid_lambda[2048];
  snprintf(invalid_lambda, sizeof(invalid_lambda), "%s", metal_case);
  char *lambda_value = strstr(invalid_lambda, "0.25");
  assert(lambda_value);
  memcpy(lambda_value, "1.25", 4u);
  assert(!vkr_harness_case_parse(invalid_lambda, strlen(invalid_lambda),
                                 "memory", &parsed, &backend_error));
  char invalid_map_size[2048];
  snprintf(invalid_map_size, sizeof(invalid_map_size), "%s", metal_case);
  char *map_size_value = strstr(invalid_map_size, "4096");
  assert(map_size_value);
  memcpy(map_size_value, "4095", 4u);
  assert(!vkr_harness_case_parse(invalid_map_size, strlen(invalid_map_size),
                                 "memory", &parsed, &backend_error));
  char invalid_exposure_reset[2048];
  snprintf(invalid_exposure_reset, sizeof(invalid_exposure_reset), "%s",
           metal_case);
  char *reset_value =
      strstr(invalid_exposure_reset, "\"exposure_reset_frame\":1");
  assert(reset_value);
  reset_value[strlen("\"exposure_reset_frame\":")] = '3';
  assert(!vkr_harness_case_parse(invalid_exposure_reset,
                                 strlen(invalid_exposure_reset), "memory",
                                 &parsed, &backend_error));
  char manual_with_reset[2048];
  snprintf(manual_with_reset, sizeof(manual_with_reset), "%s", metal_case);
  char *automatic_mode = strstr(manual_with_reset, "automatic");
  assert(automatic_mode);
  MemCopy(automatic_mode, "manual", strlen("manual"));
  memmove(automatic_mode + strlen("manual"),
          automatic_mode + strlen("automatic"),
          strlen(automatic_mode + strlen("automatic")) + 1u);
  assert(!vkr_harness_case_parse(manual_with_reset, strlen(manual_with_reset),
                                 "memory", &parsed, &backend_error));
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
      "\"warmup_stability_window\":10,\"warmup_stability_metric\":"
      "\"frame.wall\",\"warmup_max_drift_ratio\":0.1,"
      "\"require_warmup_stability\":true,\"exclusive_gpu_lane\":false},"
      "\"required_metrics\":[\"cpu.render_submit\"]}";
  VkrHarnessProfile parsed = {0};
  VkrHarnessError error = {0};
  assert(vkr_harness_profile_parse(profile, strlen(profile), "memory", &parsed,
                                   &error));
  assert(parsed.minimum_repetitions == 2u &&
         parsed.required_metric_count == 1u && !parsed.submission_gpu_timing);
  assert(string_equals(parsed.warmup_stability_metric, "frame.wall"));
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

static void test_harness_camera_warmup_holds_start_pose(void) {
  printf("  Running test_harness_camera_warmup_holds_start_pose...\n");
  const float64_t delta = 1.0 / 60.0;
  const uint32_t warmup = 120u;
  /* Every warmup frame maps to authored time zero, so the drift check in
     vkr_harness_warmup_stable() sees one pose instead of a moving view. */
  for (uint32_t frame = 0; frame < warmup; ++frame) {
    assert(vkr_harness_camera_script_time(frame, warmup, delta) == 0.0);
  }
  /* The measured window starts at the authored origin and advances one delta
     per frame from there. */
  assert(vkr_harness_camera_script_time(warmup, warmup, delta) == 0.0);
  assert(vkr_harness_camera_script_time(warmup + 1u, warmup, delta) == delta);
  assert(vkr_harness_camera_script_time(warmup + 300u, warmup, delta) ==
         300.0 * delta);
  /* A zero-warmup case keeps the frame index as its authored time. */
  assert(vkr_harness_camera_script_time(0u, 0u, delta) == 0.0);
  assert(vkr_harness_camera_script_time(7u, 0u, delta) == 7.0 * delta);
  printf("  test_harness_camera_warmup_holds_start_pose PASSED\n");
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
  snprintf(case_manifest.scene, sizeof(case_manifest.scene),
           "assets/scenes/fixtures/local_ibl_broad_mesh.scene.json");
  VkrHarnessProfile profile = {.required_present =
                                   VKR_HARNESS_PRESENT_IMMEDIATE};
  char environment[VKR_HARNESS_DIGEST_MAX];
  char workload[VKR_HARNESS_DIGEST_MAX];
  char policy[VKR_HARNESS_DIGEST_MAX];
  bool8_t fingerprinted = vkr_harness_case_fingerprints(
      ".", VKR_HARNESS_TOOL_PROFILE, &case_manifest, &profile,
      VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u, environment, workload, policy,
      &error);
  if (!fingerprinted) {
    printf("  fingerprint error %s: %s\n", error.code, error.message);
  }
  assert(fingerprinted);
  char default_policy[VKR_HARNESS_DIGEST_MAX];
  snprintf(default_policy, sizeof(default_policy), "%s", policy);
  snprintf(profile.warmup_stability_metric,
           sizeof(profile.warmup_stability_metric), "frame.wall");
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(default_policy, policy) != 0);
  snprintf(profile.warmup_stability_metric,
           sizeof(profile.warmup_stability_metric), "cpu.render_submit");
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(default_policy, policy) == 0);
  char original_workload[VKR_HARNESS_DIGEST_MAX];
  snprintf(original_workload, sizeof(original_workload), "%s", workload);
  string_copy(case_manifest.renderer.exposure_mode, "automatic");
  case_manifest.renderer.manual_exposure = 0.25f;
  case_manifest.renderer.exposure_compensation_ev = 1.0f;
  case_manifest.renderer.exposure_reset_frame = 1u;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  string_copy(case_manifest.renderer.exposure_mode, "manual");
  case_manifest.renderer.manual_exposure = VKR_DEFAULT_EXPOSURE;
  case_manifest.renderer.exposure_compensation_ev = 0.0f;
  case_manifest.renderer.exposure_reset_frame = UINT32_MAX;
  case_manifest.renderer.text_fixture = true_v;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.renderer.text_fixture = false_v;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) == 0);
  case_manifest.renderer.taa_enabled = false_v;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.renderer.taa_enabled = true_v;
  case_manifest.renderer.shadow_pcf_early_out = false_v;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.renderer.shadow_pcf_early_out = true_v;
  case_manifest.renderer.shadow_sdsm = true_v;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.renderer.shadow_sdsm = false_v;
  case_manifest.renderer.shadow_split_lambda = 0.25f;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.renderer.shadow_split_lambda = 0.80f;
  case_manifest.renderer.shadow_map_size = 4096u;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.renderer.shadow_map_size = 2048u;
  case_manifest.resize_round_trip = true_v;
  case_manifest.resize_width = 80u;
  case_manifest.resize_height = 72u;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.resize_round_trip = false_v;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) == 0);
  snprintf(case_manifest.description, sizeof(case_manifest.description),
           "provenance-only edit");
  snprintf(case_manifest.manifest_sha256, sizeof(case_manifest.manifest_sha256),
           "sha256:changed");
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) == 0);
  assert(vkr_harness_case_fingerprints(
      ".", VKR_HARNESS_TOOL_PROFILE, &case_manifest, &profile,
      VKR_RENDERER_SUBSYSTEM_ALL &
          ~VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_UI),
      NULL, 0u, environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  case_manifest.width++;
  assert(vkr_harness_case_fingerprints(".", VKR_HARNESS_TOOL_PROFILE,
                                       &case_manifest, &profile,
                                       VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u,
                                       environment, workload, policy, &error));
  assert(strcmp(original_workload, workload) != 0);
  printf("  test_harness_fingerprints PASSED\n");
}

#if !defined(_WIN32)
static void test_harness_scene_manifest_tracks_transitive_content(void) {
  printf(
      "  Running test_harness_scene_manifest_tracks_transitive_content...\n");
  char root[] = "/tmp/vkr_scene_manifest_XXXXXX";
  assert(mkdtemp(root));
  VkrHarnessError error = {0};
  char scenes[VKR_HARNESS_PATH_MAX];
  char models[VKR_HARNESS_PATH_MAX];
  char materials[VKR_HARNESS_PATH_MAX];
  char generated_materials[VKR_HARNESS_PATH_MAX];
  char textures[VKR_HARNESS_PATH_MAX];
  char generated_textures[VKR_HARNESS_PATH_MAX];
  snprintf(scenes, sizeof(scenes), "%s/assets/scenes", root);
  snprintf(models, sizeof(models), "%s/assets/models", root);
  snprintf(materials, sizeof(materials), "%s/assets/materials", root);
  snprintf(generated_materials, sizeof(generated_materials),
           "%s/assets/materials/test", root);
  snprintf(textures, sizeof(textures), "%s/assets/textures", root);
  snprintf(generated_textures, sizeof(generated_textures),
           "%s/assets/textures/generated", root);
  assert(vkr_harness_make_directories(scenes, &error));
  assert(vkr_harness_make_directories(models, &error));
  assert(vkr_harness_make_directories(materials, &error));
  assert(vkr_harness_make_directories(generated_materials, &error));
  assert(vkr_harness_make_directories(textures, &error));
  assert(vkr_harness_make_directories(generated_textures, &error));

  char scene_path[VKR_HARNESS_PATH_MAX];
  char gltf_path[VKR_HARNESS_PATH_MAX];
  char material_path[VKR_HARNESS_PATH_MAX];
  char generated_material_path[VKR_HARNESS_PATH_MAX];
  char texture_path[VKR_HARNESS_PATH_MAX];
  char generated_texture_path[VKR_HARNESS_PATH_MAX];
  char generated_packed_path[VKR_HARNESS_PATH_MAX];
  snprintf(scene_path, sizeof(scene_path), "%s/test.scene.json", scenes);
  snprintf(gltf_path, sizeof(gltf_path), "%s/test.gltf", models);
  snprintf(material_path, sizeof(material_path), "%s/test.mt", materials);
  snprintf(texture_path, sizeof(texture_path), "%s/test.png", textures);
  snprintf(generated_texture_path, sizeof(generated_texture_path),
           "%s/test.png", generated_textures);
  snprintf(generated_packed_path, sizeof(generated_packed_path), "%s.vkt",
           generated_texture_path);
  const char *gltf_relative = "assets/models/test.gltf";
  uint64_t source_hash = 0xcbf29ce484222325ull;
  for (uint64_t i = 0u; gltf_relative[i]; ++i) {
    source_hash ^= (uint64_t)(uint8_t)gltf_relative[i];
    source_hash *= 0x100000001b3ull;
  }
  snprintf(generated_material_path, sizeof(generated_material_path),
           "%s/gltf_mat_%016llx_0.mt", generated_materials,
           (unsigned long long)source_hash);
  const char *scene = "{\"material\":\"assets/materials/test.mt\","
                      "\"mesh\":\"assets/models/test.gltf\",\"entities\":[]}";
  const char *material =
      "base_color_texture=assets/textures/test.png?cs=srgb\n";
  const char *generated_material =
      "base_color_texture=assets/textures/generated/test.png?cs=srgb\n";
  assert(vkr_harness_atomic_write(scene_path, scene, strlen(scene), &error));
  assert(vkr_harness_atomic_write(gltf_path, "{}", 2u, &error));
  assert(vkr_harness_atomic_write(material_path, material, strlen(material),
                                  &error));
  assert(vkr_harness_atomic_write(generated_material_path, generated_material,
                                  strlen(generated_material), &error));
  assert(vkr_harness_atomic_write(texture_path, "first", 5u, &error));
  assert(vkr_harness_atomic_write(generated_texture_path, "generated", 9u,
                                  &error));
  assert(vkr_harness_atomic_write(generated_packed_path, "packed-first", 12u,
                                  &error));

  Arena *first_arena = arena_create(MB(8), MB(4));
  Arena *second_arena = arena_create(MB(8), MB(4));
  assert(first_arena && second_arena);
  VkrHarnessSceneManifest first = {0};
  VkrHarnessSceneManifest second = {0};
  assert(vkr_harness_scene_manifest_build(root, "assets/scenes/test.scene.json",
                                          first_arena, &first, &error));
  if (first.asset_count != 7u) {
    printf("  unexpected scene asset count %u\n", first.asset_count);
    for (uint32_t i = 0; i < first.asset_count; ++i) {
      printf("    %s\n", first.assets[i].path);
    }
  }
  assert(first.asset_count == 7u);
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_file_digest[VKR_HARNESS_DIGEST_MAX] = {0};
  snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", root);
  assert(vkr_harness_scene_manifest_write(manifest_path, &first, &error));
  assert(vkr_harness_sha256_file(manifest_path, manifest_file_digest));
  assert(vkr_harness_scene_manifest_verify_file(manifest_path,
                                                manifest_file_digest));
  assert(vkr_harness_atomic_write(manifest_path, "tampered", 8u, &error));
  assert(!vkr_harness_scene_manifest_verify_file(manifest_path,
                                                 manifest_file_digest));
  assert(vkr_harness_scene_manifest_write(manifest_path, &first, &error));
  assert(vkr_harness_scene_manifest_verify_file(manifest_path,
                                                manifest_file_digest));

  VkrHarnessCase case_manifest = {0};
  assert(harness_parse_case("windowed_hidden", "immediate",
                            "{\"mode\":\"static\",\"position\":[0,0,3],"
                            "\"yaw\":-90,\"pitch\":0}",
                            "", &case_manifest));
  snprintf(case_manifest.scene, sizeof(case_manifest.scene),
           "assets/scenes/test.scene.json");
  VkrHarnessProfile profile = {.required_present =
                                   VKR_HARNESS_PRESENT_IMMEDIATE};
  char environment[VKR_HARNESS_DIGEST_MAX];
  char workload_before[VKR_HARNESS_DIGEST_MAX];
  char workload_after[VKR_HARNESS_DIGEST_MAX];
  char policy[VKR_HARNESS_DIGEST_MAX];
  assert(vkr_harness_case_fingerprints(
      root, VKR_HARNESS_TOOL_PROFILE, &case_manifest, &profile,
      VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u, environment, workload_before,
      policy, &error));
  char workload_reused[VKR_HARNESS_DIGEST_MAX];
  assert(vkr_harness_case_fingerprints_with_scene_digest(
      VKR_HARNESS_TOOL_PROFILE, &case_manifest, &profile,
      VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u, first.sha256, environment,
      workload_reused, policy, &error));
  assert(strcmp(workload_before, workload_reused) == 0);

  assert(vkr_harness_atomic_write(generated_packed_path, "packed-second", 13u,
                                  &error));
  assert(vkr_harness_scene_manifest_build(root, "assets/scenes/test.scene.json",
                                          second_arena, &second, &error));
  assert(second.asset_count == 7u);
  assert(strcmp(first.sha256, second.sha256) != 0);
  assert(vkr_harness_case_fingerprints(
      root, VKR_HARNESS_TOOL_PROFILE, &case_manifest, &profile,
      VKR_RENDERER_SUBSYSTEM_ALL, NULL, 0u, environment, workload_after, policy,
      &error));
  assert(strcmp(workload_before, workload_after) != 0);
  arena_destroy(first_arena);
  arena_destroy(second_arena);

  assert(remove(manifest_path) == 0);
  assert(remove(generated_packed_path) == 0);
  assert(remove(generated_texture_path) == 0);
  assert(remove(texture_path) == 0);
  assert(remove(generated_material_path) == 0);
  assert(remove(material_path) == 0);
  assert(remove(gltf_path) == 0);
  assert(remove(scene_path) == 0);
  assert(rmdir(generated_textures) == 0);
  assert(rmdir(textures) == 0);
  assert(rmdir(generated_materials) == 0);
  assert(rmdir(materials) == 0);
  assert(rmdir(models) == 0);
  assert(rmdir(scenes) == 0);
  char assets[VKR_HARNESS_PATH_MAX];
  snprintf(assets, sizeof(assets), "%s/assets", root);
  assert(rmdir(assets) == 0);
  assert(rmdir(root) == 0);
  printf("  test_harness_scene_manifest_tracks_transitive_content PASSED\n");
}
#endif

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
  case_manifest.renderer.text_fixture = true_v;
  assert(vkr_harness_subsystem_plan(VKR_HARNESS_TOOL_PROFILE, &case_manifest,
                                    &plan, &harness_error));
  assert(
      vkr_renderer_subsystem_plan_includes(&plan, VKR_RENDERER_SUBSYSTEM_UI));
  case_manifest.renderer.text_fixture = false_v;
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
  assert(strcmp(mask_text, "0x000000000001ffff") == 0);
  vkr_harness_format_subsystem_mask(mask_text,
                                    VKR_RENDERER_SUBSYSTEM_MANDATORY);
  assert(strcmp(mask_text, "0x0000000000000fff") == 0);
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
  report.case_manifest.renderer.shadow_cascades = 4u;
  report.case_manifest.renderer.shadow_pcf_samples = 16u;
  report.case_manifest.renderer.shadow_pcf_early_out = true_v;
  report.case_manifest.renderer.shadow_sdsm = true_v;
  report.case_manifest.renderer.shadow_split_lambda = 0.25f;
  report.case_manifest.renderer.shadow_map_size = 4096u;
  report.case_manifest.renderer.taa_enabled = false_v;
  snprintf(report.case_manifest.renderer.exposure_mode,
           sizeof(report.case_manifest.renderer.exposure_mode), "automatic");
  report.case_manifest.renderer.manual_exposure = 0.25f;
  report.case_manifest.renderer.exposure_compensation_ev = 1.0f;
  report.case_manifest.renderer.exposure_reset_frame = 1u;
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
  snprintf(report.provenance.world_renderer,
           sizeof(report.provenance.world_renderer), "deferred");
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
  assert(strstr(json, "\"world_renderer\":\"deferred\"") != NULL);
  assert(strstr(json, "\"pcf_uniform_early_out\":true") != NULL);
  assert(strstr(json, "\"shadow_sdsm\":true") != NULL);
  assert(strstr(json, "\"shadow_split_lambda\":0.25") != NULL);
  assert(strstr(json, "\"shadow_map_size\":4096") != NULL);
  assert(strstr(json, "\"taa_enabled\":false") != NULL);
  assert(strstr(json, "\"exposure_mode\":\"automatic\"") != NULL);
  assert(strstr(json, "\"manual_exposure\":0.25") != NULL);
  assert(strstr(json, "\"exposure_compensation_ev\":1") != NULL);
  assert(strstr(json, "\"exposure_reset_frame\":1") != NULL);
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

static void harness_test_write_f32_le(uint8_t bytes[4], float32_t value) {
  uint32_t bits = 0u;
  memcpy(&bits, &value, sizeof(bits));
  bytes[0] = (uint8_t)bits;
  bytes[1] = (uint8_t)(bits >> 8u);
  bytes[2] = (uint8_t)(bits >> 16u);
  bytes[3] = (uint8_t)(bits >> 24u);
}

static void test_harness_capture_catalog_and_converters(void) {
  printf("  Running test_harness_capture_catalog_and_converters...\n");
  assert(vkr_renderer_capture_channel_count() == 17u);
  assert(vkr_renderer_capture_channel_from_name("missing") ==
         VKR_CAPTURE_CHANNEL_INVALID);
  const VkrCaptureChannelId final_color =
      vkr_renderer_capture_channel_from_name("final_color");
  const VkrCaptureChannelId depth =
      vkr_renderer_capture_channel_from_name("depth");
  const VkrCaptureChannelId picking =
      vkr_renderer_capture_channel_from_name("picking_ids");
  const VkrCaptureChannelId transmission_visibility =
      vkr_renderer_capture_channel_from_name("transmission_visibility_ids");
  assert(final_color != VKR_CAPTURE_CHANNEL_INVALID);
  assert(depth != VKR_CAPTURE_CHANNEL_INVALID);
  assert(picking != VKR_CAPTURE_CHANNEL_INVALID);
  assert(transmission_visibility != VKR_CAPTURE_CHANNEL_INVALID);

#if !defined(_WIN32)
  char first_dir[] = "/tmp/vkr-capture-first-XXXXXX";
  char second_dir[] = "/tmp/vkr-capture-second-XXXXXX";
  assert(mkdtemp(first_dir) && mkdtemp(second_dir));
  Arena *persistent = arena_create(MB(1), MB(1));
  Arena *transient = arena_create(MB(1), MB(1));
  assert(persistent && transient);
  VkrHarnessArenas arenas = {.persistent = persistent, .transient = transient};
  VkrHarnessReport *first = calloc(1u, sizeof(*first));
  VkrHarnessReport *second = calloc(1u, sizeof(*second));
  assert(first && second);
  assert(vkr_harness_report_init_storage(
      first, persistent, VKR_HARNESS_MAX_CAPTURE_CHANNELS,
      VKR_HARNESS_MAX_CAPTURE_CHANNELS * VKR_HARNESS_ARTIFACTS_PER_CAPTURE));
  assert(vkr_harness_report_init_storage(
      second, persistent, VKR_HARNESS_MAX_CAPTURE_CHANNELS,
      VKR_HARNESS_MAX_CAPTURE_CHANNELS * VKR_HARNESS_ARTIFACTS_PER_CAPTURE));

  const uint8_t bgra_bottom_left[] = {1,  2,  3,  4,  5,  6,  7,  8,
                                      99, 99, 99, 99, 9,  10, 11, 12,
                                      13, 14, 15, 16, 99, 99, 99, 99};
  const uint16_t depth_bottom_left[] = {65535u, 32768u, 0u, 0u,
                                        0u,     16384u, 0u, 0u};
  const uint32_t picking_bottom_left[] = {3u, 4u, 0u, 1u, 2u, 0u};
  VkrCaptureItemResult items[] = {
      {.channel = final_color,
       .width = 2u,
       .height = 2u,
       .row_pitch = 12u,
       .format = VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB,
       .value_kind = VKR_CAPTURE_VALUE_COLOR,
       .color_space = VKR_CAPTURE_COLOR_SPACE_SRGB,
       .origin = VKR_CAPTURE_ORIGIN_BOTTOM_LEFT,
       .data = bgra_bottom_left,
       .data_size = sizeof(bgra_bottom_left)},
      {.channel = depth,
       .width = 2u,
       .height = 2u,
       .row_pitch = 8u,
       .format = VKR_TEXTURE_FORMAT_D16_UNORM,
       .value_kind = VKR_CAPTURE_VALUE_DEPTH,
       .origin = VKR_CAPTURE_ORIGIN_BOTTOM_LEFT,
       .data = (const uint8_t *)depth_bottom_left,
       .data_size = sizeof(depth_bottom_left)},
      {.channel = picking,
       .width = 2u,
       .height = 2u,
       .row_pitch = 12u,
       .format = VKR_TEXTURE_FORMAT_R32_UINT,
       .value_kind = VKR_CAPTURE_VALUE_UINT,
       .origin = VKR_CAPTURE_ORIGIN_BOTTOM_LEFT,
       .data = (const uint8_t *)picking_bottom_left,
       .data_size = sizeof(picking_bottom_left)},
  };
  const VkrCapturePollResult poll = {
      .status = VKR_CAPTURE_STATUS_READY,
      .items = items,
      .item_count = ArrayCount(items),
      .source_frame_index = 7u,
      .submit_serial = 9u,
  };
  const char logical_channels[][64] = {"final_color", "depth", "picking_ids"};
  VkrHarnessError error = {0};
  assert(vkr_harness_capture_publish(first_dir, 1u, &poll, logical_channels,
                                     ArrayCount(logical_channels), &arenas,
                                     first, &error));
  assert(vkr_harness_capture_publish(second_dir, 1u, &poll, logical_channels,
                                     ArrayCount(logical_channels), &arenas,
                                     second, &error));
  assert(first->capture_count == 3u && second->capture_count == 3u);
  for (uint32_t i = 0; i < first->capture_count; ++i) {
    assert(strcmp(first->captures[i].data_sha256,
                  second->captures[i].data_sha256) == 0);
    assert(strcmp(first->captures[i].preview_sha256,
                  second->captures[i].preview_sha256) == 0);
  }

  char raw_path[VKR_HARNESS_PATH_MAX];
  snprintf(raw_path, sizeof(raw_path), "%s/%s", first_dir,
           first->captures[1].data_path);
  uint8_t *raw = NULL;
  uint64_t raw_size = 0u;
  assert(vkr_harness_read_file(raw_path, transient, &raw, &raw_size));
  assert(raw_size == 16u);
  uint8_t expected[16];
  harness_test_write_f32_le(expected + 0u, 0.0f);
  harness_test_write_f32_le(expected + 4u, 16384.0f / 65535.0f);
  harness_test_write_f32_le(expected + 8u, 1.0f);
  harness_test_write_f32_le(expected + 12u, 32768.0f / 65535.0f);
  assert(memcmp(raw, expected, sizeof(expected)) == 0);

  VkrHarnessReport *reports[] = {first, second};
  const char *roots[] = {first_dir, second_dir};
  for (uint32_t r = 0; r < ArrayCount(reports); ++r) {
    for (uint32_t i = 0; i < reports[r]->artifact_count; ++i) {
      char path[VKR_HARNESS_PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s", roots[r],
               reports[r]->artifacts[i].path);
      assert(unlink(path) == 0);
    }
    char captures[VKR_HARNESS_PATH_MAX];
    snprintf(captures, sizeof(captures), "%s/captures", roots[r]);
    assert(rmdir(captures) == 0);
    assert(rmdir(roots[r]) == 0);
  }
  free(first);
  free(second);
  arena_destroy(transient);
  arena_destroy(persistent);
#endif
  printf("  test_harness_capture_catalog_and_converters PASSED\n");
}

static void test_harness_capture_replays(void) {
  printf("  Running test_harness_capture_replays...\n");
  assert(sizeof(((VkrHarnessRendererConfig *)0)->render_mode) >
         strlen("temporal_history"));
  VkrHarnessCase case_manifest = {.capture_count = 1u};
  VkrHarnessCapture *capture = &case_manifest.captures[0];
  capture->at_frame = 2u;
  capture->channel_count = 7u;
  const char *channels[] = {"final_color",
                            "depth",
                            "normals",
                            "unlit",
                            "temporal_motion",
                            "temporal_history",
                            "shadow_debug_factor"};
  for (uint32_t i = 0; i < ArrayCount(channels); ++i) {
    snprintf(capture->channels[i], sizeof(capture->channels[i]), "%s",
             channels[i]);
  }
  VkrHarnessCaptureReplay replays[8] = {0};
  uint32_t replay_count = 0u;
  VkrHarnessError error = {0};
  assert(vkr_harness_capture_replays_build(
      &case_manifest, replays, ArrayCount(replays), &replay_count, &error));
  assert(replay_count == 6u);
  assert(strcmp(replays[0].mode, "direct") == 0 &&
         replays[0].channel_count == 2u);
  assert(strcmp(replays[1].mode, "normals") == 0 &&
         replays[1].render_mode == VKR_RENDER_MODE_NORMAL);
  assert(strcmp(replays[2].mode, "unlit") == 0 &&
         replays[2].render_mode == VKR_RENDER_MODE_UNLIT);
  assert(strcmp(replays[3].mode, "temporal_motion") == 0 &&
         replays[3].render_mode == VKR_RENDER_MODE_TEMPORAL_MOTION);
  assert(strcmp(replays[4].mode, "temporal_history") == 0 &&
         replays[4].render_mode == VKR_RENDER_MODE_TEMPORAL_HISTORY);
  assert(strcmp(replays[5].mode, "shadow_debug_factor") == 0 &&
         replays[5].shadow_debug_mode == 2u);
  VkrHarnessCaptureReplay found = {0};
  assert(vkr_harness_capture_replay_find(&case_manifest, 0u, "normals", &found,
                                         &error));
  assert(strcmp(found.logical_channels[0], "normals") == 0 &&
         strcmp(found.direct_channels[0], "final_color") == 0);
  printf("  test_harness_capture_replays PASSED\n");
}

static void test_harness_comparison_algorithms(void) {
  printf("  Running test_harness_comparison_algorithms...\n");
  const VkrHarnessCompareConfig thresholds = {
      .max_pixel_delta = 0.01,
      .max_mean_absolute_error = 0.01,
      .max_failed_pixel_ratio = 0.0,
  };
  const uint8_t baseline[] = {0, 10, 20, 255, 30, 40, 50, 255};
  uint8_t actual[sizeof(baseline)];
  memcpy(actual, baseline, sizeof(actual));
  VkrHarnessComparisonResult result =
      vkr_harness_compare_rgba8(actual, baseline, 2u, &thresholds, NULL);
  assert(result.outcome == VKR_HARNESS_COMPARISON_PASS);
  actual[0] = 255u;
  result = vkr_harness_compare_rgba8(actual, baseline, 2u, &thresholds, NULL);
  assert(result.outcome == VKR_HARNESS_COMPARISON_FAIL);
  assert(result.max_absolute_error == 1.0 && result.failing_pixel_count == 1u &&
         result.failed_pixel_ratio == 0.5);

  uint8_t floats[8];
  uint8_t float_baseline[8];
  harness_test_write_f32_le(floats, 0.25f);
  harness_test_write_f32_le(floats + 4u, 0.5f);
  memcpy(float_baseline, floats, sizeof(floats));
  result =
      vkr_harness_compare_f32_le(floats, float_baseline, 2u, &thresholds, NULL);
  assert(result.outcome == VKR_HARNESS_COMPARISON_PASS);
  harness_test_write_f32_le(floats + 4u, NAN);
  result =
      vkr_harness_compare_f32_le(floats, float_baseline, 2u, &thresholds, NULL);
  assert(result.outcome == VKR_HARNESS_COMPARISON_INCOMPATIBLE);

  const uint8_t ids[] = {1, 0, 0, 0, 2, 0, 0, 0};
  uint8_t changed_ids[sizeof(ids)];
  memcpy(changed_ids, ids, sizeof(ids));
  result = vkr_harness_compare_u32_le(ids, changed_ids, 2u, NULL);
  assert(result.outcome == VKR_HARNESS_COMPARISON_PASS);
  changed_ids[4] = 3u;
  result = vkr_harness_compare_u32_le(ids, changed_ids, 2u, NULL);
  assert(result.outcome == VKR_HARNESS_COMPARISON_FAIL &&
         result.failing_pixel_count == 1u);
  printf("  test_harness_comparison_algorithms PASSED\n");
}

#if !defined(_WIN32)
static bool8_t harness_remove_tree(const char *path) {
  struct stat status;
  if (lstat(path, &status) != 0) {
    return true_v;
  }
  if (!S_ISDIR(status.st_mode)) {
    return unlink(path) == 0;
  }
  DIR *directory = opendir(path);
  if (!directory) {
    return false_v;
  }
  bool8_t ok = true_v;
  struct dirent *entry = NULL;
  while (ok && (entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[VKR_HARNESS_PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    ok = harness_remove_tree(child);
  }
  closedir(directory);
  return ok && rmdir(path) == 0;
}

static bool8_t harness_find_only_child(const char *path, char out[64]) {
  DIR *directory = opendir(path);
  if (!directory) {
    return false_v;
  }
  uint32_t count = 0u;
  struct dirent *entry = NULL;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    snprintf(out, 64u, "%s", entry->d_name);
    count++;
  }
  closedir(directory);
  return count == 1u;
}
#endif

static void test_harness_guarded_baseline_accept(void) {
  printf("  Running test_harness_guarded_baseline_accept...\n");
#if defined(_WIN32)
  printf("  test_harness_guarded_baseline_accept SKIPPED (POSIX fixture)\n");
#else
  char root[] = "/tmp/vkr-harness-baseline-XXXXXX";
  assert(mkdtemp(root));
  VkrHarnessError error = {0};
  char source[VKR_HARNESS_PATH_MAX];
  char captures[VKR_HARNESS_PATH_MAX];
  snprintf(source, sizeof(source), "%s/build/_artifacts/snapshot/source", root);
  snprintf(captures, sizeof(captures), "%s/captures", source);
  assert(vkr_harness_make_directories(captures, &error));
  char raw_path[VKR_HARNESS_PATH_MAX];
  snprintf(raw_path, sizeof(raw_path), "%s/frame.raw", captures);
  const uint8_t rgba[] = {1u, 2u, 3u, 255u};
  assert(vkr_harness_atomic_write(raw_path, rgba, sizeof(rgba), &error));

  Arena *arena = arena_create();
  assert(arena);
  VkrHarnessReport report = {
      .tool = VKR_HARNESS_TOOL_SNAPSHOT,
      .profile_compatible = true_v,
      .requested_repetitions = 1u,
      .completed_repetitions = 1u,
  };
  assert(vkr_harness_report_init_storage(&report, arena, 1u, 1u));
  snprintf(report.run_id, sizeof(report.run_id), "source");
  snprintf(report.case_manifest.id, sizeof(report.case_manifest.id),
           "smoke.baseline-test");
  snprintf(report.profile.id, sizeof(report.profile.id), "local.baseline-test");
  snprintf(report.environment_fingerprint,
           sizeof(report.environment_fingerprint),
           "sha256:"
           "0000000000000000000000000000000000000000000000000000000000000001");
  snprintf(report.workload_fingerprint, sizeof(report.workload_fingerprint),
           "sha256:"
           "0000000000000000000000000000000000000000000000000000000000000002");
  snprintf(report.policy_fingerprint, sizeof(report.policy_fingerprint),
           "sha256:"
           "0000000000000000000000000000000000000000000000000000000000000003");
  vkr_harness_report_set_status(&report, "pass", VKR_HARNESS_EXIT_PASS);
  VkrHarnessCaptureResult *capture = &report.captures[report.capture_count++];
  snprintf(capture->channel, sizeof(capture->channel), "final_color");
  snprintf(capture->source_format, sizeof(capture->source_format), "rgba8");
  snprintf(capture->canonical_encoding, sizeof(capture->canonical_encoding),
           "rgba8");
  snprintf(capture->value_kind, sizeof(capture->value_kind), "color");
  snprintf(capture->color_space, sizeof(capture->color_space), "srgb");
  snprintf(capture->origin, sizeof(capture->origin), "top_left");
  snprintf(capture->data_path, sizeof(capture->data_path),
           "captures/frame.raw");
  capture->width = 1u;
  capture->height = 1u;
  assert(vkr_harness_sha256_file(raw_path, capture->data_sha256));
  VkrHarnessArtifact *artifact = &report.artifacts[report.artifact_count++];
  snprintf(artifact->role, sizeof(artifact->role), "capture.raw");
  snprintf(artifact->path, sizeof(artifact->path), "captures/frame.raw");
  snprintf(artifact->media_type, sizeof(artifact->media_type),
           "application/octet-stream");
  snprintf(artifact->sha256, sizeof(artifact->sha256), "%s",
           capture->data_sha256);

  char report_path[VKR_HARNESS_PATH_MAX];
  char summary_path[VKR_HARNESS_PATH_MAX];
  snprintf(report_path, sizeof(report_path), "%s/report.json", source);
  snprintf(summary_path, sizeof(summary_path), "%s/capture-summary.bin",
           source);
  assert(vkr_harness_atomic_write(report_path, "{}\n", 3u, &error));
  assert(
      vkr_harness_capture_summary_write(summary_path, &report, arena, &error));
  assert(vkr_harness_baseline_propose(
             root, "build/_artifacts/snapshot/source", "unit-test",
             "guarded acceptance fixture") == VKR_HARNESS_EXIT_PASS);

  char proposal_root[VKR_HARNESS_PATH_MAX];
  char proposal_id[64];
  snprintf(proposal_root, sizeof(proposal_root), "%s/build/_artifacts/baseline",
           root);
  assert(harness_find_only_child(proposal_root, proposal_id));
  char plan_relative[VKR_HARNESS_RELATIVE_PATH_MAX];
  char plan_absolute[VKR_HARNESS_PATH_MAX];
  snprintf(plan_relative, sizeof(plan_relative),
           "build/_artifacts/baseline/%s/plan.json", proposal_id);
  snprintf(plan_absolute, sizeof(plan_absolute), "%s/%s", root, plan_relative);
  const char *wrong_digest =
      "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  assert(vkr_harness_baseline_accept(root, plan_relative, wrong_digest) ==
         VKR_HARNESS_EXIT_ERROR);
  char plan_digest[VKR_HARNESS_DIGEST_MAX];
  assert(vkr_harness_sha256_file(plan_absolute, plan_digest));
  assert(vkr_harness_baseline_accept(root, plan_relative, plan_digest) ==
         VKR_HARNESS_EXIT_PASS);

  Arena *load_arena = arena_create();
  assert(load_arena);
  char generation_root[VKR_HARNESS_PATH_MAX];
  VkrHarnessCaptureSummary accepted = {0};
  assert(vkr_harness_baseline_current(root, "local.baseline-test",
                                      "smoke.baseline-test", load_arena,
                                      generation_root, &accepted, &error));
  assert(accepted.capture_count == 1u &&
         strcmp(accepted.captures[0].data_sha256, capture->data_sha256) == 0);
  arena_destroy(load_arena);
  arena_destroy(arena);
  assert(harness_remove_tree(root));
  printf("  test_harness_guarded_baseline_accept PASSED\n");
#endif
}

bool32_t run_harness_tests(void) {
  printf("--- Running Harness tests... ---\n");
  test_harness_hash_and_statistics();
  test_harness_current_frame_work_metrics();
  test_harness_case_parser();
  test_harness_profile_parser();
  test_harness_camera_determinism();
  test_harness_camera_warmup_holds_start_pose();
  test_harness_fingerprints();
#if !defined(_WIN32)
  test_harness_scene_manifest_tracks_transitive_content();
#endif
  test_harness_subsystem_plans();
  test_harness_case_profile_pairing();
  test_harness_assertion_verdict();
  test_harness_report_shape();
  test_harness_safe_paths();
  test_harness_platform_process_primitives();
  test_harness_capture_catalog_and_converters();
  test_harness_capture_replays();
  test_harness_comparison_algorithms();
  test_harness_guarded_baseline_accept();
  printf("--- Harness tests completed. ---\n");
  return true;
}
