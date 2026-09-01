#include "vkr_harness.h"

static int32_t vkr_harness_fingerprint_field_compare(const void *a,
                                                     const void *b) {
  const VkrHarnessFingerprintField *lhs = a;
  const VkrHarnessFingerprintField *rhs = b;
  return string_compare(lhs->name, rhs->name);
}

/**
 * Canonical form is `<be32 name length><name><be32 value length><value>` per
 * field in name order, so neither field order nor a delimiter occurring inside
 * a value can change the digest. Absorbed incrementally: a contiguous
 * canonical buffer would have to be sized from the field struct's capacities
 * and silently overflows the moment either grows.
 */
static void vkr_harness_fingerprint_absorb(VkrHarnessSha256 *hash,
                                           const char *text, uint32_t length) {
  const uint8_t prefix[4] = {(uint8_t)(length >> 24u), (uint8_t)(length >> 16u),
                             (uint8_t)(length >> 8u), (uint8_t)length};
  vkr_harness_sha256_update(hash, prefix, sizeof(prefix));
  vkr_harness_sha256_update(hash, text, length);
}

bool8_t vkr_harness_fingerprint(const VkrHarnessFingerprintField *fields,
                                uint32_t field_count,
                                char out_digest[VKR_HARNESS_DIGEST_MAX],
                                VkrHarnessError *out_error) {
  if (!out_digest || field_count > VKR_HARNESS_MAX_FINGERPRINT_FIELDS ||
      (field_count > 0 && !fields)) {
    vkr_harness_error_set(out_error, "fingerprint.input", "$.comparison",
                          "Fingerprint field set is invalid");
    return false_v;
  }
  VkrHarnessFingerprintField sorted[VKR_HARNESS_MAX_FINGERPRINT_FIELDS];
  if (field_count > 0) {
    MemCopy(sorted, fields, sizeof(*sorted) * field_count);
    vkr_sort(sorted, field_count, sizeof(*sorted),
             vkr_harness_fingerprint_field_compare);
  }
  VkrHarnessSha256 hash;
  vkr_harness_sha256_begin(&hash);
  for (uint32_t i = 0; i < field_count; ++i) {
    if (sorted[i].name[0] == '\0' ||
        (i > 0 && string_equals(sorted[i - 1u].name, sorted[i].name))) {
      vkr_harness_error_set(
          out_error, "fingerprint.field", "$.comparison",
          "Fingerprint field names must be unique and nonempty");
      return false_v;
    }
    vkr_harness_fingerprint_absorb(&hash, sorted[i].name,
                                   (uint32_t)string_length(sorted[i].name));
    vkr_harness_fingerprint_absorb(&hash, sorted[i].value,
                                   (uint32_t)string_length(sorted[i].value));
  }
  vkr_harness_sha256_end(&hash, out_digest);
  return true_v;
}

/**
 * Fails rather than truncates: a clipped name or value would silently give two
 * different effective configurations the same comparison identity.
 */
static bool8_t vkr_harness_add_field(
    VkrHarnessFingerprintField fields[VKR_HARNESS_MAX_FINGERPRINT_FIELDS],
    uint32_t *count, const char *name, const char *format, ...) {
  if (*count >= VKR_HARNESS_MAX_FINGERPRINT_FIELDS) {
    return false_v;
  }
  VkrHarnessFingerprintField *field = &fields[*count];
  const int name_length =
      string_format(field->name, sizeof(field->name), "%s", name);
  va_list args;
  va_start(args, format);
  const int value_length =
      string_format_v(field->value, sizeof(field->value), format, args);
  va_end(args);
  if (name_length < 0 || (uint32_t)name_length >= sizeof(field->name) ||
      value_length < 0 || (uint32_t)value_length >= sizeof(field->value)) {
    return false_v;
  }
  (*count)++;
  return true_v;
}

bool8_t vkr_harness_case_fingerprints_with_scene_digest(
    VkrHarnessTool tool, const VkrHarnessCase *case_manifest,
    const VkrHarnessProfile *profile, VkrSubsystemMask subsystem_mask,
    const VkrHarnessFingerprintField *environment_fields,
    uint32_t environment_field_count, const char *scene_content_digest,
    char out_environment[VKR_HARNESS_DIGEST_MAX],
    char out_workload[VKR_HARNESS_DIGEST_MAX],
    char out_policy[VKR_HARNESS_DIGEST_MAX], VkrHarnessError *out_error) {
  if (!case_manifest || !profile || !scene_content_digest ||
      !string_n_equals(scene_content_digest, "sha256:", 7u) ||
      string_length(scene_content_digest) != VKR_HARNESS_DIGEST_MAX - 1u ||
      !out_environment || !out_workload || !out_policy ||
      environment_field_count > VKR_HARNESS_MAX_FINGERPRINT_FIELDS ||
      (environment_field_count > 0u && !environment_fields)) {
    vkr_harness_error_set(out_error, "fingerprint.input", "$.comparison",
                          "Fingerprint inputs are invalid");
    return false_v;
  }
  VkrHarnessFingerprintField
      effective_environment[VKR_HARNESS_MAX_FINGERPRINT_FIELDS];
  const VkrHarnessFingerprintField *environment = environment_fields;
  uint32_t environment_count = environment_field_count;
  if (case_manifest->target != VKR_HARNESS_TARGET_OFFSCREEN) {
    if (environment_field_count > 0u)
      MemCopy(effective_environment, environment_fields,
              sizeof(*effective_environment) * environment_field_count);
    if (!vkr_harness_add_field(effective_environment, &environment_count,
                               "window.content_scale", "%.9g",
                               case_manifest->content_scale)) {
      vkr_harness_error_set(
          out_error, "fingerprint.field_limit", "$.comparison",
          "Effective environment exceeds the fingerprint field count or "
          "field capacity");
      return false_v;
    }
    environment = effective_environment;
  }
  if (!vkr_harness_fingerprint(environment, environment_count, out_environment,
                               out_error)) {
    return false_v;
  }

  VkrHarnessFingerprintField fields[VKR_HARNESS_MAX_FINGERPRINT_FIELDS];
  uint32_t count = 0;
#define ADD(NAME, FORMAT, ...)                                                 \
  if (!vkr_harness_add_field(fields, &count, NAME, FORMAT, __VA_ARGS__))       \
  goto too_many
  ADD("camera.version", "%u", VKR_HARNESS_CAMERA_SCRIPT_VERSION);
  ADD("camera.mode", "%u", case_manifest->camera.mode);
  ADD("camera.interpolation", "%u", case_manifest->camera.interpolation);
  ADD("camera.speed", "%u", case_manifest->camera.speed);
  ADD("camera.lens", "%.9g,%.9g,%.9g",
      case_manifest->camera.vertical_fov_degrees,
      case_manifest->camera.near_plane, case_manifest->camera.far_plane);
  ADD("camera.static", "%.9g,%.9g,%.9g,%.9g,%.9g",
      case_manifest->camera.static_pose.position.x,
      case_manifest->camera.static_pose.position.y,
      case_manifest->camera.static_pose.position.z,
      case_manifest->camera.static_pose.yaw_degrees,
      case_manifest->camera.static_pose.pitch_degrees);
  ADD("camera.orbit", "%.9g,%.9g,%.9g,%.9g,%.9g,%.17g,%.9g,%.9g",
      case_manifest->camera.orbit_center.x,
      case_manifest->camera.orbit_center.y,
      case_manifest->camera.orbit_center.z, case_manifest->camera.orbit_radius,
      case_manifest->camera.orbit_height,
      case_manifest->camera.orbit_duration_seconds,
      case_manifest->camera.orbit_revolutions,
      case_manifest->camera.orbit_start_angle_degrees);
  for (uint32_t i = 0; i < case_manifest->camera.key_count; ++i) {
    char name[96];
    string_format(name, sizeof(name), "camera.key.%03u", i);
    ADD(name, "%.17g,%.9g,%.9g,%.9g,%.9g,%.9g",
        case_manifest->camera.keys[i].time_seconds,
        case_manifest->camera.keys[i].position.x,
        case_manifest->camera.keys[i].position.y,
        case_manifest->camera.keys[i].position.z,
        case_manifest->camera.keys[i].yaw_degrees,
        case_manifest->camera.keys[i].pitch_degrees);
  }
  ADD("case.cache", "%s", vkr_harness_cache_name(case_manifest->cache));
  ADD("case.boot", "%s", vkr_harness_boot_name(case_manifest->boot));
  char subsystem_text[VKR_HARNESS_SUBSYSTEM_MASK_MAX];
  vkr_harness_format_subsystem_mask(subsystem_text, subsystem_mask);
  ADD("case.subsystems", "%s", subsystem_text);
  ADD("case.fixed_delta", "%.17g", case_manifest->fixed_delta_seconds);
  ADD("case.frames", "%u,%u", case_manifest->warmup_frames,
      case_manifest->measure_frames);
  ADD("case.resolution", "%u,%u", case_manifest->width, case_manifest->height);
  if (case_manifest->target == VKR_HARNESS_TARGET_OFFSCREEN) {
    ADD("case.content_scale", "%.9g", case_manifest->content_scale);
  }
  if (case_manifest->resize_round_trip) {
    ADD("case.resize_round_trip", "%u,%u", case_manifest->resize_width,
        case_manifest->resize_height);
  }
  ADD("case.scene", "%s", case_manifest->scene);
  ADD("case.scene_content", "%s", scene_content_digest);
  ADD("case.seed", "%llu", (unsigned long long)case_manifest->seed);
  ADD("renderer.editor", "%u", case_manifest->renderer.editor);
  ADD("renderer.skybox", "%u", case_manifest->renderer.skybox);
  ADD("renderer.text_fixture", "%u", case_manifest->renderer.text_fixture);
  ADD("renderer.taa_enabled", "%u", case_manifest->renderer.taa_enabled);
  /* Preserve fingerprints for manifests authored before these default-on
     controls existed. Only non-default state changes the workload identity. */
  if (!case_manifest->renderer.tonemap_enabled)
    ADD("renderer.tonemap_enabled", "%u", 0u);
  if (!case_manifest->renderer.fxaa_enabled)
    ADD("renderer.fxaa_enabled", "%u", 0u);
  ADD("renderer.backend", "%s",
      case_manifest->renderer.backend[0] ? case_manifest->renderer.backend
                                         : "external");
  /* Every resolved shadow experiment control belongs to the workload. */
  ADD("renderer.shadow", "%s,%u,%u,%.9g,%u,%u,%u",
      case_manifest->renderer.shadow_preset,
      case_manifest->renderer.shadow_cascades,
      case_manifest->renderer.shadow_pcf_samples,
      case_manifest->renderer.shadow_split_lambda,
      case_manifest->renderer.shadow_map_size,
      case_manifest->renderer.shadow_pcf_early_out,
      case_manifest->renderer.shadow_sdsm);
  ADD("renderer.render_mode", "%s", case_manifest->renderer.render_mode);
  ADD("renderer.exposure", "%s,%.9g,%.9g,%u",
      case_manifest->renderer.exposure_mode,
      case_manifest->renderer.manual_exposure,
      case_manifest->renderer.exposure_compensation_ev,
      case_manifest->renderer.exposure_reset_frame);
  ADD("renderer.bloom", "%u,%.9g,%.9g,%.9g",
      case_manifest->renderer.bloom_enabled,
      case_manifest->renderer.bloom_threshold,
      case_manifest->renderer.bloom_knee,
      case_manifest->renderer.bloom_intensity);
  ADD("renderer.gtao", "%u,%.9g,%.9g", case_manifest->renderer.gtao_enabled,
      case_manifest->renderer.gtao_radius, case_manifest->renderer.gtao_power);
  /* Preserve workload identities for manifests authored before render-scale
     support. Non-default scale remains a distinct deterministic workload. */
  if (case_manifest->renderer.render_scale != 1.0f) {
    ADD("renderer.render_scale", "%.9g", case_manifest->renderer.render_scale);
  }
  const char *upscaler = case_manifest->renderer.upscaler[0]
                             ? case_manifest->renderer.upscaler
                             : "spatial";
  if (!string_equals(upscaler, "spatial")) {
    ADD("renderer.upscaler", "%s", upscaler);
  }
  if (case_manifest->renderer.dynamic_resolution) {
    ADD("renderer.dynamic_resolution", "%.9g,%.9g,%.9g",
        case_manifest->renderer.dynamic_resolution_min_scale,
        case_manifest->renderer.dynamic_resolution_max_scale,
        case_manifest->renderer.dynamic_resolution_target_frame_ms);
  }
  ADD("renderer.shadow_debug_mode", "%u",
      case_manifest->renderer.shadow_debug_mode);
  /* The SH scaling fixture declares packed probe count as an independent
     workload variable. */
  ADD("renderer.ibl_probe_limit", "%u",
      case_manifest->renderer.ibl_probe_limit);
  ADD("target", "%s,%s,%u", vkr_harness_target_name(case_manifest->target),
      vkr_harness_present_name(case_manifest->present),
      case_manifest->target_image_count);
  ADD("instrumentation", "%u,%u,%u", profile->gpu_timing,
      profile->submission_gpu_timing, profile->event_subjects);
  if (tool != VKR_HARNESS_TOOL_PROFILE) {
    for (uint32_t i = 0; i < case_manifest->capture_count; ++i) {
      char name[96];
      string_format(name, sizeof(name), "capture.%03u", i);
      char value[VKR_HARNESS_TEXT_MAX];
      int32_t written = string_format(value, sizeof(value), "%u",
                                      case_manifest->captures[i].at_frame);
      for (uint32_t channel = 0;
           channel < case_manifest->captures[i].channel_count; ++channel) {
        if (written < 0 || (uint32_t)written >= sizeof(value)) {
          goto too_many;
        }
        written +=
            string_format(value + written, sizeof(value) - (uint32_t)written,
                          ",%s", case_manifest->captures[i].channels[channel]);
      }
      if (written < 0 || (uint32_t)written >= sizeof(value)) {
        goto too_many;
      }
      ADD(name, "%s", value);
    }
  }
  if (!vkr_harness_fingerprint(fields, count, out_workload, out_error)) {
    return false_v;
  }

  count = 0;
  ADD("profile.authoritative", "%u", profile->authoritative);
  ADD("profile.allow_dirty", "%u", profile->allow_dirty);
  ADD("profile.target", "%s", vkr_harness_target_name(profile->target));
  ADD("profile.minimum_repetitions", "%u", profile->minimum_repetitions);
  ADD("profile.present", "%s,%u",
      vkr_harness_present_name(profile->required_present),
      profile->require_actual_present);
  if (!profile->warmup_stability_metric[0] ||
      string_equals(profile->warmup_stability_metric, "cpu.render_submit")) {
    /* Preserve the version-2 policy fingerprint for the historical default so
     * accepted snapshot baselines do not require promotion without a policy
     * change. Non-default metrics remain fingerprint-significant. */
    ADD("profile.stability", "%u,%.17g,%u", profile->warmup_stability_window,
        profile->warmup_max_drift_ratio, profile->require_warmup_stability);
  } else {
    ADD("profile.stability", "%s,%u,%.17g,%u", profile->warmup_stability_metric,
        profile->warmup_stability_window, profile->warmup_max_drift_ratio,
        profile->require_warmup_stability);
  }
  ADD("profile.gpu_lane", "%u", profile->require_exclusive_gpu_lane);
  ADD("profile.os", "%s", profile->required_os);
  ADD("profile.cpu", "%s", profile->required_cpu);
  ADD("profile.gpu", "%s", profile->required_gpu);
  ADD("profile.driver", "%s", profile->required_driver);
  ADD("profile.gpu_ids", "%u,%u", profile->required_gpu_vendor_id,
      profile->required_gpu_device_id);
  ADD("profile.power", "%s", profile->required_power_mode);
  ADD("profile.thermal", "%s", profile->required_thermal_state);
  ADD("profile.priority", "%u,%d", profile->has_required_process_priority,
      profile->required_process_priority);
  ADD("statistics.algorithm", "%s", "nearest-rank-v1,population-stddev-v1");
  for (uint32_t i = 0; i < profile->required_metric_count; ++i) {
    char name[96];
    string_format(name, sizeof(name), "required_metric.%03u", i);
    ADD(name, "%s", profile->required_metrics[i]);
  }
  for (uint32_t i = 0; i < case_manifest->assertion_count; ++i) {
    char name[96];
    string_format(name, sizeof(name), "assertion.%03u", i);
    ADD(name, "%s,%u,%u,%.17g,%.17g", case_manifest->assertions[i].metric,
        case_manifest->assertions[i].statistic,
        case_manifest->assertions[i].operation,
        case_manifest->assertions[i].limit,
        case_manifest->assertions[i].tolerance);
  }
  if (!vkr_harness_fingerprint(fields, count, out_policy, out_error)) {
    return false_v;
  }
#undef ADD
  return true_v;

too_many:
#undef ADD
  vkr_harness_error_set(out_error, "fingerprint.field_limit", "$.comparison",
                        "Effective configuration exceeds the fingerprint field "
                        "count or field capacity");
  return false_v;
}

bool8_t vkr_harness_case_fingerprints(
    const char *repo_root, VkrHarnessTool tool,
    const VkrHarnessCase *case_manifest, const VkrHarnessProfile *profile,
    VkrSubsystemMask subsystem_mask,
    const VkrHarnessFingerprintField *environment_fields,
    uint32_t environment_field_count,
    char out_environment[VKR_HARNESS_DIGEST_MAX],
    char out_workload[VKR_HARNESS_DIGEST_MAX],
    char out_policy[VKR_HARNESS_DIGEST_MAX], VkrHarnessError *out_error) {
  Arena *arena = arena_create(MB(8), MB(4));
  VkrHarnessSceneManifest manifest = {0};
  const bool8_t ok =
      repo_root && case_manifest && arena &&
      vkr_harness_scene_manifest_build(repo_root, case_manifest->scene, arena,
                                       &manifest, out_error) &&
      vkr_harness_case_fingerprints_with_scene_digest(
          tool, case_manifest, profile, subsystem_mask, environment_fields,
          environment_field_count, manifest.sha256, out_environment,
          out_workload, out_policy, out_error);
  if (arena) {
    arena_destroy(arena);
  }
  return ok;
}
