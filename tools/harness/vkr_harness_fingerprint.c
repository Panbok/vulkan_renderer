#include "core/vkr_subsystem_plan.h"
#include "vkr_harness.h"

vkr_internal int32_t vkr_harness_fingerprint_field_compare(const void *a,
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
vkr_internal void vkr_harness_fingerprint_absorb(VkrSha256 *hash,
                                                 const char *text,
                                                 uint32_t length) {
  const uint8_t prefix[4] = {(uint8_t)(length >> 24u), (uint8_t)(length >> 16u),
                             (uint8_t)(length >> 8u), (uint8_t)length};
  vkr_sha256_update(hash, prefix, sizeof(prefix));
  vkr_sha256_update(hash, text, length);
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
  VkrSha256 hash;
  vkr_sha256_init(&hash);
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
vkr_internal bool8_t vkr_harness_add_field(
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

/* Appends one field, failing the enclosing field builder when the field or
   the field table is full. */
#define ADD(NAME, FORMAT, ...)                                                 \
  if (!vkr_harness_add_field(fields, count, NAME, FORMAT, __VA_ARGS__))        \
  return false_v

vkr_internal bool8_t vkr_harness_camera_fields(
    const VkrHarnessCamera *camera,
    VkrHarnessFingerprintField fields[VKR_HARNESS_MAX_FINGERPRINT_FIELDS],
    uint32_t *count) {
  ADD("camera.version", "%u", VKR_HARNESS_CAMERA_SCRIPT_VERSION);
  ADD("camera.mode", "%u", camera->mode);
  ADD("camera.interpolation", "%u", camera->interpolation);
  ADD("camera.speed", "%u", camera->speed);
  if (vkr_harness_camera_is_orthographic(camera->mode)) {
    ADD("camera.orthographic_lens", "%.9g,%.9g,%.9g",
        camera->orthographic_height, camera->near_plane, camera->far_plane);
  } else {
    ADD("camera.lens", "%.9g,%.9g,%.9g", camera->vertical_fov_degrees,
        camera->near_plane, camera->far_plane);
  }
  ADD("camera.static", "%.9g,%.9g,%.9g,%.9g,%.9g",
      camera->static_pose.position.x, camera->static_pose.position.y,
      camera->static_pose.position.z, camera->static_pose.yaw_degrees,
      camera->static_pose.pitch_degrees);
  ADD("camera.orbit", "%.9g,%.9g,%.9g,%.9g,%.9g,%.17g,%.9g,%.9g",
      camera->orbit_center.x, camera->orbit_center.y, camera->orbit_center.z,
      camera->orbit_radius, camera->orbit_height,
      camera->orbit_duration_seconds, camera->orbit_revolutions,
      camera->orbit_start_angle_degrees);
  for (uint32_t i = 0; i < camera->key_count; ++i) {
    char name[96];
    string_format(name, sizeof(name), "camera.key.%03u", i);
    ADD(name, "%.17g,%.9g,%.9g,%.9g,%.9g,%.9g", camera->keys[i].time_seconds,
        camera->keys[i].position.x, camera->keys[i].position.y,
        camera->keys[i].position.z, camera->keys[i].yaw_degrees,
        camera->keys[i].pitch_degrees);
  }
  return true_v;
}

vkr_internal bool8_t vkr_harness_renderer_fields(
    const VkrHarnessRendererConfig *renderer,
    VkrHarnessFingerprintField fields[VKR_HARNESS_MAX_FINGERPRINT_FIELDS],
    uint32_t *count) {
  ADD("renderer.editor", "%u", renderer->editor);
  if (renderer->editor_stop_frame != UINT32_MAX)
    ADD("renderer.editor_stop_frame", "%u", renderer->editor_stop_frame);
  if (renderer->editor_resume_frame != UINT32_MAX)
    ADD("renderer.editor_resume_frame", "%u", renderer->editor_resume_frame);
  ADD("renderer.skybox", "%u", renderer->skybox);
  ADD("renderer.text_fixture", "%u", renderer->text_fixture);
  if (renderer->physics_fixture) {
    ADD("renderer.physics_fixture", "%u", 1u);
  }
  ADD("renderer.taa_enabled", "%u", renderer->taa_enabled);
  /* Preserve fingerprints for manifests authored before these default-on
     controls existed. Only non-default state changes the workload identity. */
  if (!renderer->tonemap_enabled)
    ADD("renderer.tonemap_enabled", "%u", 0u);
  if (!renderer->fxaa_enabled)
    ADD("renderer.fxaa_enabled", "%u", 0u);
  ADD("renderer.backend", "%s",
      renderer->backend[0] ? renderer->backend : "external");
  /* Every resolved shadow experiment control belongs to the workload. */
  ADD("renderer.shadow", "%s,%u,%u,%.9g,%u,%u,%u", renderer->shadow_preset,
      renderer->shadow_cascades, renderer->shadow_pcf_samples,
      renderer->shadow_split_lambda, renderer->shadow_map_size,
      renderer->shadow_pcf_early_out, renderer->shadow_sdsm);
  ADD("renderer.render_mode", "%s", renderer->render_mode);
  ADD("renderer.display_transform", "%s", renderer->display_transform);
  ADD("renderer.white_balance", "%.9g,%.9g",
      renderer->white_balance_temperature, renderer->white_balance_tint);
  ADD("renderer.color_grading", "%.9g,%.9g", renderer->color_contrast,
      renderer->color_saturation);
  ADD("renderer.exposure", "%s,%.9g,%.9g,%u", renderer->exposure_mode,
      renderer->manual_exposure, renderer->exposure_compensation_ev,
      renderer->exposure_reset_frame);
  ADD("renderer.bloom", "%u,%.9g,%.9g,%.9g", renderer->bloom_enabled,
      renderer->bloom_threshold, renderer->bloom_knee,
      renderer->bloom_intensity);
  // Cache changes the placement of a nonlinear transform across filtering.
  // Disabled spellings retain the analytic reference workload identity.
  const char *post_cache = getenv("VKR_POST_TRANSFORM_CACHE");
  if (post_cache && post_cache[0] != '\0' && !string_equals(post_cache, "0") &&
      string_equals(renderer->render_mode, "default")) {
    ADD("renderer.post_transform_cache", "%u", 1u);
  }
  ADD("renderer.ssr", "%u", renderer->ssr_enabled);
  /* The tier is a cold process option inherited by every capture/profile child.
     Keep explicit high equivalent to the unset reference configuration. */
  const char *ssr_quality = getenv("VKR_SSR_QUALITY");
  if (renderer->ssr_enabled && ssr_quality && ssr_quality[0] != '\0' &&
      !string_equals(ssr_quality, "high")) {
    ADD("renderer.ssr_quality", "%s", ssr_quality);
  }
  if (renderer->ssgi_enabled)
    ADD("renderer.ssgi", "%u", renderer->ssgi_enabled);
  /* Disabled DoF has no output or workload effect, preserving legacy identity.
   */
  if (renderer->dof_enabled) {
    ADD("renderer.dof", "%u,%.9g,%.9g", renderer->dof_enabled,
        renderer->dof_focus_distance, renderer->dof_f_stop);
  }
  if (renderer->motion_blur_enabled &&
      renderer->motion_blur_shutter_angle > 0.0f) {
    ADD("renderer.motion_blur", "%.9g", renderer->motion_blur_shutter_angle);
  }
  if (renderer->motion_blur_entity[0] != '\0') {
    ADD("case.motion_entity", "%s,%.9g,%.9g,%.9g", renderer->motion_blur_entity,
        renderer->motion_blur_entity_velocity_x,
        renderer->motion_blur_entity_velocity_y,
        renderer->motion_blur_entity_velocity_z);
  }
  if (string_equals(renderer->display_output, "auto_extended_linear"))
    ADD("renderer.display_output", "%s", renderer->display_output);
  ADD("renderer.gtao", "%u,%.9g,%.9g", renderer->gtao_enabled,
      renderer->gtao_radius, renderer->gtao_power);
  /* Existing cases predate this opt-in control. Preserve their workload
     identity exactly when it is disabled. */
  if (renderer->image_sharpness != 0.0f) {
    ADD("renderer.image_sharpness", "%.9g", renderer->image_sharpness);
  }
  /* Preserve workload identities for manifests authored before render-scale
     support. Non-default scale remains a distinct deterministic workload. */
  if (renderer->render_scale != 1.0f) {
    ADD("renderer.render_scale", "%.9g", renderer->render_scale);
  }
  const char *upscaler = renderer->upscaler[0] ? renderer->upscaler : "spatial";
  if (!string_equals(upscaler, "spatial")) {
    ADD("renderer.upscaler", "%s", upscaler);
  }
  if (renderer->dynamic_resolution) {
    ADD("renderer.dynamic_resolution", "%.9g,%.9g,%.9g",
        renderer->dynamic_resolution_min_scale,
        renderer->dynamic_resolution_max_scale,
        renderer->dynamic_resolution_target_frame_ms);
  }
  ADD("renderer.shadow_debug_mode", "%u", renderer->shadow_debug_mode);
  /* The SH scaling fixture declares packed probe count as an independent
     workload variable. */
  ADD("renderer.ibl_probe_limit", "%u", renderer->ibl_probe_limit);
  return true_v;
}

/** Appends every field that identifies the workload a case executes. */
vkr_internal bool8_t vkr_harness_workload_fields(
    VkrHarnessTool tool, const VkrHarnessCase *case_manifest,
    const VkrHarnessProfile *profile, VkrSubsystemMask subsystem_mask,
    const char *scene_content_digest,
    VkrHarnessFingerprintField fields[VKR_HARNESS_MAX_FINGERPRINT_FIELDS],
    uint32_t *count) {
  if (!vkr_harness_camera_fields(&case_manifest->camera, fields, count)) {
    return false_v;
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
  if (!vkr_harness_renderer_fields(&case_manifest->renderer, fields, count)) {
    return false_v;
  }
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
          return false_v;
        }
        written +=
            string_format(value + written, sizeof(value) - (uint32_t)written,
                          ",%s", case_manifest->captures[i].channels[channel]);
      }
      if (written < 0 || (uint32_t)written >= sizeof(value)) {
        return false_v;
      }
      ADD(name, "%s", value);
    }
  }
  return true_v;
}

/** Appends every field of the measurement policy a run is judged by. */
vkr_internal bool8_t vkr_harness_policy_fields(
    const VkrHarnessCase *case_manifest, const VkrHarnessProfile *profile,
    VkrHarnessFingerprintField fields[VKR_HARNESS_MAX_FINGERPRINT_FIELDS],
    uint32_t *count) {
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
  return true_v;
}

#undef ADD

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
  if (!vkr_harness_workload_fields(tool, case_manifest, profile, subsystem_mask,
                                   scene_content_digest, fields, &count)) {
    goto too_many;
  }
  if (!vkr_harness_fingerprint(fields, count, out_workload, out_error)) {
    return false_v;
  }

  count = 0;
  if (!vkr_harness_policy_fields(case_manifest, profile, fields, &count)) {
    goto too_many;
  }
  if (!vkr_harness_fingerprint(fields, count, out_policy, out_error)) {
    return false_v;
  }
  return true_v;

too_many:
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
      vkr_harness_scene_manifest_build_context(repo_root, case_manifest->scene,
                                               case_manifest->asset_context,
                                               arena, &manifest, out_error) &&
      vkr_harness_case_fingerprints_with_scene_digest(
          tool, case_manifest, profile, subsystem_mask, environment_fields,
          environment_field_count, manifest.sha256, out_environment,
          out_workload, out_policy, out_error);
  if (arena) {
    arena_destroy(arena);
  }
  return ok;
}
