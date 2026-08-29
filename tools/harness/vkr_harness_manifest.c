#include "vkr_harness_json.h"

#include "renderer/systems/vkr_shadow_system.h"

#include <float.h>

#define VKR_HARNESS_MANIFEST_MAX_BYTES MB(1)

static bool8_t vkr_harness_manifest_field(const VkrHarnessJsonDocument *doc,
                                          int32_t object, const char *name,
                                          bool8_t required, int32_t *out,
                                          VkrHarnessError *error) {
  bool8_t duplicate = false_v;
  const int32_t token =
      vkr_harness_json_object_get(doc, object, name, &duplicate);
  if (duplicate) {
    vkr_harness_error_set(error, "manifest.duplicate_field", name,
                          "Duplicate field '%s'", name);
    return false_v;
  }
  if (required && token < 0) {
    vkr_harness_error_set(error, "manifest.required", name,
                          "Missing required field '%s'", name);
    return false_v;
  }
  *out = token;
  return true_v;
}

static bool8_t vkr_harness_manifest_string(const VkrHarnessJsonDocument *doc,
                                           int32_t object, const char *name,
                                           bool8_t required, char *out,
                                           uint32_t capacity,
                                           VkrHarnessError *error) {
  int32_t token = -1;
  if (!vkr_harness_manifest_field(doc, object, name, required, &token, error)) {
    return false_v;
  }
  return token < 0 ||
         vkr_harness_json_string(doc, token, out, capacity, name, error);
}

static bool8_t vkr_harness_manifest_u64(const VkrHarnessJsonDocument *doc,
                                        int32_t object, const char *name,
                                        bool8_t required, uint64_t *out,
                                        VkrHarnessError *error) {
  int32_t token = -1;
  if (!vkr_harness_manifest_field(doc, object, name, required, &token, error)) {
    return false_v;
  }
  return token < 0 || vkr_harness_json_u64(doc, token, out, name, error);
}

static bool8_t vkr_harness_manifest_f64(const VkrHarnessJsonDocument *doc,
                                        int32_t object, const char *name,
                                        bool8_t required, float64_t *out,
                                        VkrHarnessError *error) {
  int32_t token = -1;
  if (!vkr_harness_manifest_field(doc, object, name, required, &token, error)) {
    return false_v;
  }
  return token < 0 || vkr_harness_json_f64(doc, token, out, name, error);
}

static bool8_t vkr_harness_manifest_bool(const VkrHarnessJsonDocument *doc,
                                         int32_t object, const char *name,
                                         bool8_t required, bool8_t *out,
                                         VkrHarnessError *error) {
  int32_t token = -1;
  if (!vkr_harness_manifest_field(doc, object, name, required, &token, error)) {
    return false_v;
  }
  return token < 0 || vkr_harness_json_bool(doc, token, out, name, error);
}

static bool8_t vkr_harness_manifest_vec3(const VkrHarnessJsonDocument *doc,
                                         int32_t token, Vec3 *out,
                                         const char *field,
                                         VkrHarnessError *error) {
  if (token < 0 || doc->tokens[token].type != VKR_HARNESS_JSON_ARRAY ||
      doc->tokens[token].child_count != 3u) {
    vkr_harness_error_set(error, "manifest.vec3", field,
                          "Expected a three-number array");
    return false_v;
  }
  float64_t values[3];
  int32_t value_token = token + 1;
  for (uint32_t i = 0; i < 3; ++i) {
    if (!vkr_harness_json_f64(doc, value_token, &values[i], field, error)) {
      return false_v;
    }
    value_token = vkr_harness_json_next(doc, value_token);
  }
  *out = vec3_new((float32_t)values[0], (float32_t)values[1],
                  (float32_t)values[2]);
  return true_v;
}

static bool8_t vkr_harness_manifest_id_valid(const char *id) {
  if (!id || id[0] < 'a' || id[0] > 'z') {
    return false_v;
  }
  bool8_t saw_dot = false_v;
  for (const char *c = id; *c; ++c) {
    if (*c == '.') {
      if (c == id || c[1] == '\0' || c[-1] == '.') {
        return false_v;
      }
      saw_dot = true_v;
    } else if (!(*c >= 'a' && *c <= 'z') && !(*c >= '0' && *c <= '9') &&
               *c != '_') {
      return false_v;
    }
  }
  return saw_dot;
}

static bool8_t vkr_harness_manifest_component_valid(const char *value) {
  if (!value || value[0] == '\0') {
    return false_v;
  }
  for (const char *c = value; *c; ++c) {
    if (!(*c >= 'a' && *c <= 'z') && !(*c >= '0' && *c <= '9') && *c != '_') {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t vkr_harness_metric_name_valid(const char *name) {
  return vkr_harness_manifest_id_valid(name);
}

static bool8_t vkr_harness_parse_speed(const char *value,
                                       VkrHarnessSpeed *out) {
  if (string_equals(value, "slow")) {
    *out = VKR_HARNESS_SPEED_SLOW;
  } else if (string_equals(value, "medium")) {
    *out = VKR_HARNESS_SPEED_MEDIUM;
  } else if (string_equals(value, "fast")) {
    *out = VKR_HARNESS_SPEED_FAST;
  } else {
    return false_v;
  }
  return true_v;
}

static bool8_t vkr_harness_parse_target(const char *value,
                                        VkrHarnessTarget *out) {
  if (string_equals(value, "windowed_visible")) {
    *out = VKR_HARNESS_TARGET_WINDOWED_VISIBLE;
  } else if (string_equals(value, "windowed_hidden")) {
    *out = VKR_HARNESS_TARGET_WINDOWED_HIDDEN;
  } else if (string_equals(value, "offscreen")) {
    *out = VKR_HARNESS_TARGET_OFFSCREEN;
  } else {
    return false_v;
  }
  return true_v;
}

static bool8_t vkr_harness_parse_present(const char *value,
                                         VkrHarnessPresentMode *out) {
  if (string_equals(value, "immediate")) {
    *out = VKR_HARNESS_PRESENT_IMMEDIATE;
  } else if (string_equals(value, "fifo")) {
    *out = VKR_HARNESS_PRESENT_FIFO;
  } else if (string_equals(value, "none")) {
    *out = VKR_HARNESS_PRESENT_NONE;
  } else {
    return false_v;
  }
  return true_v;
}

static bool8_t vkr_harness_parse_camera_keys(const VkrHarnessJsonDocument *doc,
                                             int32_t token,
                                             VkrHarnessCamera *camera,
                                             VkrHarnessError *error) {
  if (token < 0 || doc->tokens[token].type != VKR_HARNESS_JSON_ARRAY ||
      doc->tokens[token].child_count < 2u ||
      doc->tokens[token].child_count > VKR_HARNESS_MAX_CAMERA_KEYS) {
    vkr_harness_error_set(error, "camera.keys", "$.camera.keys",
                          "Camera keys require 2..%u entries",
                          VKR_HARNESS_MAX_CAMERA_KEYS);
    return false_v;
  }
  static const char *const allowed[] = {"t", "position", "yaw", "pitch"};
  int32_t item = token + 1;
  while (item >= 0 && camera->key_count < doc->tokens[token].child_count) {
    if (!vkr_harness_json_object_validate(doc, item, allowed, 4u, allowed, 4u,
                                          "$.camera.keys[]", error)) {
      return false_v;
    }
    VkrHarnessCameraKey *key = &camera->keys[camera->key_count++];
    float64_t yaw = 0.0;
    float64_t pitch = 0.0;
    int32_t position = -1;
    if (!vkr_harness_manifest_f64(doc, item, "t", true_v, &key->time_seconds,
                                  error) ||
        !vkr_harness_manifest_field(doc, item, "position", true_v, &position,
                                    error) ||
        !vkr_harness_manifest_vec3(doc, position, &key->position,
                                   "$.camera.keys[].position", error) ||
        !vkr_harness_manifest_f64(doc, item, "yaw", true_v, &yaw, error) ||
        !vkr_harness_manifest_f64(doc, item, "pitch", true_v, &pitch, error)) {
      return false_v;
    }
    key->yaw_degrees = (float32_t)yaw;
    key->pitch_degrees = (float32_t)pitch;
    item = vkr_harness_json_next(doc, item);
  }
  return true_v;
}

static bool8_t vkr_harness_parse_camera(const VkrHarnessJsonDocument *doc,
                                        int32_t token, VkrHarnessCamera *camera,
                                        VkrHarnessError *error) {
  static const char *const allowed[] = {"mode",
                                        "speed",
                                        "position",
                                        "yaw",
                                        "pitch",
                                        "vertical_fov_degrees",
                                        "near_plane",
                                        "far_plane",
                                        "keys",
                                        "interpolation",
                                        "center",
                                        "radius",
                                        "height",
                                        "revolutions",
                                        "duration_seconds",
                                        "start_angle_degrees"};
  static const char *const required[] = {"mode"};
  if (!vkr_harness_json_object_validate(
          doc, token, allowed, ArrayCount(allowed), required,
          ArrayCount(required), "$.camera", error)) {
    return false_v;
  }
  *camera = (VkrHarnessCamera){
      .interpolation = VKR_HARNESS_CAMERA_INTERPOLATION_LINEAR,
      .speed = VKR_HARNESS_SPEED_MEDIUM,
      .vertical_fov_degrees = 70.0f,
      .near_plane = 0.1f,
      .far_plane = 500.0f,
  };
  char mode[32];
  char speed[16] = "medium";
  char interpolation[24] = "linear";
  if (!vkr_harness_manifest_string(doc, token, "mode", true_v, mode,
                                   sizeof(mode), error) ||
      !vkr_harness_manifest_string(doc, token, "speed", false_v, speed,
                                   sizeof(speed), error) ||
      !vkr_harness_manifest_string(doc, token, "interpolation", false_v,
                                   interpolation, sizeof(interpolation),
                                   error) ||
      !vkr_harness_parse_speed(speed, &camera->speed)) {
    vkr_harness_error_set(error, "camera.enum", "$.camera",
                          "Camera mode, speed, or interpolation is invalid");
    return false_v;
  }
  if (string_equals(interpolation, "catmull_rom")) {
    camera->interpolation = VKR_HARNESS_CAMERA_INTERPOLATION_CATMULL_ROM;
  } else if (!string_equals(interpolation, "linear")) {
    vkr_harness_error_set(error, "camera.interpolation",
                          "$.camera.interpolation", "Unknown interpolation");
    return false_v;
  }
  float64_t fov = camera->vertical_fov_degrees;
  float64_t near_plane = camera->near_plane;
  float64_t far_plane = camera->far_plane;
  if (!vkr_harness_manifest_f64(doc, token, "vertical_fov_degrees", false_v,
                                &fov, error) ||
      !vkr_harness_manifest_f64(doc, token, "near_plane", false_v, &near_plane,
                                error) ||
      !vkr_harness_manifest_f64(doc, token, "far_plane", false_v, &far_plane,
                                error)) {
    return false_v;
  }
  camera->vertical_fov_degrees = (float32_t)fov;
  camera->near_plane = (float32_t)near_plane;
  camera->far_plane = (float32_t)far_plane;

  int32_t keys = -1;
  int32_t position = -1;
  int32_t center = -1;
  int32_t yaw_token = -1;
  int32_t pitch_token = -1;
  int32_t interpolation_token = -1;
  int32_t radius_token = -1;
  int32_t height_token = -1;
  int32_t revolutions_token = -1;
  int32_t duration_token = -1;
  int32_t angle_token = -1;
  if (!vkr_harness_manifest_field(doc, token, "keys", false_v, &keys, error) ||
      !vkr_harness_manifest_field(doc, token, "position", false_v, &position,
                                  error) ||
      !vkr_harness_manifest_field(doc, token, "center", false_v, &center,
                                  error) ||
      !vkr_harness_manifest_field(doc, token, "yaw", false_v, &yaw_token,
                                  error) ||
      !vkr_harness_manifest_field(doc, token, "pitch", false_v, &pitch_token,
                                  error) ||
      !vkr_harness_manifest_field(doc, token, "interpolation", false_v,
                                  &interpolation_token, error) ||
      !vkr_harness_manifest_field(doc, token, "radius", false_v, &radius_token,
                                  error) ||
      !vkr_harness_manifest_field(doc, token, "height", false_v, &height_token,
                                  error) ||
      !vkr_harness_manifest_field(doc, token, "revolutions", false_v,
                                  &revolutions_token, error) ||
      !vkr_harness_manifest_field(doc, token, "duration_seconds", false_v,
                                  &duration_token, error) ||
      !vkr_harness_manifest_field(doc, token, "start_angle_degrees", false_v,
                                  &angle_token, error)) {
    return false_v;
  }
  if (string_equals(mode, "static")) {
    camera->mode = VKR_HARNESS_CAMERA_STATIC;
    float64_t yaw = 0.0;
    float64_t pitch = 0.0;
    if (position < 0 ||
        !vkr_harness_manifest_vec3(doc, position, &camera->static_pose.position,
                                   "$.camera.position", error) ||
        !vkr_harness_manifest_f64(doc, token, "yaw", true_v, &yaw, error) ||
        !vkr_harness_manifest_f64(doc, token, "pitch", true_v, &pitch, error) ||
        keys >= 0 || center >= 0 || interpolation_token >= 0 ||
        radius_token >= 0 || height_token >= 0 || revolutions_token >= 0 ||
        duration_token >= 0 || angle_token >= 0) {
      vkr_harness_error_set(error, "camera.static", "$.camera",
                            "Static camera requires only position/yaw/pitch");
      return false_v;
    }
    camera->static_pose.yaw_degrees = (float32_t)yaw;
    camera->static_pose.pitch_degrees = (float32_t)pitch;
  } else if (string_equals(mode, "keyframes") ||
             string_equals(mode, "flythrough")) {
    camera->mode = string_equals(mode, "keyframes")
                       ? VKR_HARNESS_CAMERA_KEYFRAMES
                       : VKR_HARNESS_CAMERA_FLYTHROUGH;
    if (keys < 0 || position >= 0 || center >= 0 || yaw_token >= 0 ||
        pitch_token >= 0 || radius_token >= 0 || height_token >= 0 ||
        revolutions_token >= 0 || duration_token >= 0 || angle_token >= 0 ||
        !vkr_harness_parse_camera_keys(doc, keys, camera, error)) {
      return false_v;
    }
  } else if (string_equals(mode, "orbit")) {
    camera->mode = VKR_HARNESS_CAMERA_ORBIT;
    float64_t radius = 0.0;
    float64_t height = 0.0;
    float64_t revolutions = 0.0;
    float64_t duration = 0.0;
    float64_t angle = 0.0;
    if (center < 0 || keys >= 0 || position >= 0 || yaw_token >= 0 ||
        pitch_token >= 0 || interpolation_token >= 0 ||
        !vkr_harness_manifest_vec3(doc, center, &camera->orbit_center,
                                   "$.camera.center", error) ||
        !vkr_harness_manifest_f64(doc, token, "radius", true_v, &radius,
                                  error) ||
        !vkr_harness_manifest_f64(doc, token, "height", false_v, &height,
                                  error) ||
        !vkr_harness_manifest_f64(doc, token, "revolutions", true_v,
                                  &revolutions, error) ||
        !vkr_harness_manifest_f64(doc, token, "duration_seconds", true_v,
                                  &duration, error) ||
        !vkr_harness_manifest_f64(doc, token, "start_angle_degrees", false_v,
                                  &angle, error)) {
      return false_v;
    }
    camera->orbit_radius = (float32_t)radius;
    camera->orbit_height = (float32_t)height;
    camera->orbit_revolutions = (float32_t)revolutions;
    camera->orbit_duration_seconds = duration;
    camera->orbit_start_angle_degrees = (float32_t)angle;
  } else {
    vkr_harness_error_set(error, "camera.mode", "$.camera.mode",
                          "Unknown camera mode '%s'", mode);
    return false_v;
  }
  if ((camera->mode == VKR_HARNESS_CAMERA_STATIC &&
       (camera->static_pose.pitch_degrees < -89.0f ||
        camera->static_pose.pitch_degrees > 89.0f)) ||
      !vkr_harness_camera_prepare(camera, error)) {
    return false_v;
  }
  return true_v;
}

static VkrHarnessCompareConfig vkr_harness_compare_defaults(void) {
  return (VkrHarnessCompareConfig){
      .max_pixel_delta = 2.0 / 255.0,
      .max_mean_absolute_error = 0.1 / 255.0,
      .max_failed_pixel_ratio = 0.001,
      .emit_diff = true_v,
  };
}

static bool8_t
vkr_harness_parse_compare(const VkrHarnessJsonDocument *doc, int32_t token,
                          const VkrHarnessCompareConfig *fallback,
                          VkrHarnessCompareConfig *compare, const char *field,
                          VkrHarnessError *error) {
  *compare = fallback ? *fallback : vkr_harness_compare_defaults();
  if (token < 0) {
    return true_v;
  }
  static const char *const allowed[] = {"max_pixel_delta",
                                        "max_mean_absolute_error",
                                        "max_failed_pixel_ratio", "emit_diff"};
  if (!vkr_harness_json_object_validate(
          doc, token, allowed, ArrayCount(allowed), NULL, 0u, field, error) ||
      !vkr_harness_manifest_f64(doc, token, "max_pixel_delta", false_v,
                                &compare->max_pixel_delta, error) ||
      !vkr_harness_manifest_f64(doc, token, "max_mean_absolute_error", false_v,
                                &compare->max_mean_absolute_error, error) ||
      !vkr_harness_manifest_f64(doc, token, "max_failed_pixel_ratio", false_v,
                                &compare->max_failed_pixel_ratio, error) ||
      !vkr_harness_manifest_bool(doc, token, "emit_diff", false_v,
                                 &compare->emit_diff, error) ||
      compare->max_pixel_delta < 0.0 || compare->max_pixel_delta > 1.0 ||
      compare->max_mean_absolute_error < 0.0 ||
      compare->max_mean_absolute_error > 1.0 ||
      compare->max_failed_pixel_ratio < 0.0 ||
      compare->max_failed_pixel_ratio > 1.0) {
    vkr_harness_error_set(error, "compare.config", field,
                          "Comparison thresholds must be in [0,1]");
    return false_v;
  }
  return true_v;
}

static bool8_t vkr_harness_parse_renderer(const VkrHarnessJsonDocument *doc,
                                          int32_t token,
                                          VkrHarnessRendererConfig *renderer,
                                          VkrHarnessError *error) {
  static const char *const allowed[] = {"editor",
                                        "skybox",
                                        "text_fixture",
                                        "taa_enabled",
                                        "backend",
                                        "shadow_preset",
                                        "shadow_cascades",
                                        "shadow_pcf_samples",
                                        "shadow_split_lambda",
                                        "shadow_map_size",
                                        "shadow_pcf_early_out",
                                        "shadow_sdsm",
                                        "render_mode",
                                        "exposure_mode",
                                        "manual_exposure",
                                        "exposure_compensation_ev",
                                        "exposure_reset_frame",
                                        "bloom_enabled",
                                        "bloom_threshold",
                                        "bloom_knee",
                                        "bloom_intensity",
                                        "gtao_enabled",
                                        "gtao_radius",
                                        "gtao_power"};
  static const char *const required[] = {"editor", "skybox", "shadow_preset",
                                         "shadow_cascades"};
  if (!vkr_harness_json_object_validate(
          doc, token, allowed, ArrayCount(allowed), required,
          ArrayCount(required), "$.renderer", error)) {
    return false_v;
  }
  string_copy(renderer->render_mode, "default");
  string_copy(renderer->exposure_mode, "manual");
  renderer->taa_enabled = true_v;
  renderer->manual_exposure = VKR_DEFAULT_EXPOSURE;
  renderer->exposure_reset_frame = UINT32_MAX;
  renderer->bloom_enabled = false_v;
  renderer->bloom_threshold = VKR_BLOOM_DEFAULT_THRESHOLD;
  renderer->bloom_knee = VKR_BLOOM_DEFAULT_KNEE;
  renderer->bloom_intensity = VKR_BLOOM_DEFAULT_INTENSITY;
  renderer->gtao_enabled = false_v;
  renderer->gtao_radius = VKR_GTAO_DEFAULT_RADIUS;
  renderer->gtao_power = VKR_GTAO_DEFAULT_POWER;
  uint64_t cascades = 0;
  uint64_t exposure_reset_frame = UINT32_MAX;
  float64_t manual_exposure = renderer->manual_exposure;
  float64_t exposure_compensation_ev = 0.0;
  float64_t bloom_threshold = renderer->bloom_threshold;
  float64_t bloom_knee = renderer->bloom_knee;
  float64_t bloom_intensity = renderer->bloom_intensity;
  float64_t gtao_radius = renderer->gtao_radius;
  float64_t gtao_power = renderer->gtao_power;
  int32_t manual_exposure_token = -1;
  int32_t exposure_reset_token = -1;
  int32_t bloom_threshold_token = -1;
  int32_t bloom_knee_token = -1;
  int32_t bloom_intensity_token = -1;
  int32_t gtao_radius_token = -1;
  int32_t gtao_power_token = -1;
  if (!vkr_harness_manifest_bool(doc, token, "editor", true_v,
                                 &renderer->editor, error) ||
      !vkr_harness_manifest_bool(doc, token, "skybox", true_v,
                                 &renderer->skybox, error) ||
      !vkr_harness_manifest_bool(doc, token, "text_fixture", false_v,
                                 &renderer->text_fixture, error) ||
      !vkr_harness_manifest_bool(doc, token, "taa_enabled", false_v,
                                 &renderer->taa_enabled, error) ||
      !vkr_harness_manifest_string(doc, token, "backend", false_v,
                                   renderer->backend, sizeof(renderer->backend),
                                   error) ||
      !vkr_harness_manifest_string(doc, token, "shadow_preset", true_v,
                                   renderer->shadow_preset,
                                   sizeof(renderer->shadow_preset), error) ||
      !vkr_harness_manifest_u64(doc, token, "shadow_cascades", true_v,
                                &cascades, error) ||
      !vkr_harness_manifest_string(doc, token, "render_mode", false_v,
                                   renderer->render_mode,
                                   sizeof(renderer->render_mode), error) ||
      !vkr_harness_manifest_string(doc, token, "exposure_mode", false_v,
                                   renderer->exposure_mode,
                                   sizeof(renderer->exposure_mode), error) ||
      !vkr_harness_manifest_field(doc, token, "manual_exposure", false_v,
                                  &manual_exposure_token, error) ||
      !vkr_harness_manifest_f64(doc, token, "manual_exposure", false_v,
                                &manual_exposure, error) ||
      !vkr_harness_manifest_f64(doc, token, "exposure_compensation_ev", false_v,
                                &exposure_compensation_ev, error) ||
      !vkr_harness_manifest_field(doc, token, "exposure_reset_frame", false_v,
                                  &exposure_reset_token, error) ||
      !vkr_harness_manifest_u64(doc, token, "exposure_reset_frame", false_v,
                                &exposure_reset_frame, error) ||
      !vkr_harness_manifest_bool(doc, token, "bloom_enabled", false_v,
                                 &renderer->bloom_enabled, error) ||
      !vkr_harness_manifest_field(doc, token, "bloom_threshold", false_v,
                                  &bloom_threshold_token, error) ||
      !vkr_harness_manifest_f64(doc, token, "bloom_threshold", false_v,
                                &bloom_threshold, error) ||
      !vkr_harness_manifest_field(doc, token, "bloom_knee", false_v,
                                  &bloom_knee_token, error) ||
      !vkr_harness_manifest_f64(doc, token, "bloom_knee", false_v, &bloom_knee,
                                error) ||
      !vkr_harness_manifest_field(doc, token, "bloom_intensity", false_v,
                                  &bloom_intensity_token, error) ||
      !vkr_harness_manifest_f64(doc, token, "bloom_intensity", false_v,
                                &bloom_intensity, error) ||
      !vkr_harness_manifest_bool(doc, token, "gtao_enabled", false_v,
                                 &renderer->gtao_enabled, error) ||
      !vkr_harness_manifest_field(doc, token, "gtao_radius", false_v,
                                  &gtao_radius_token, error) ||
      !vkr_harness_manifest_f64(doc, token, "gtao_radius", false_v,
                                &gtao_radius, error) ||
      !vkr_harness_manifest_field(doc, token, "gtao_power", false_v,
                                  &gtao_power_token, error) ||
      !vkr_harness_manifest_f64(doc, token, "gtao_power", false_v, &gtao_power,
                                error)) {
    return false_v;
  }
  renderer->shadow_cascades = (uint32_t)cascades;
  const bool8_t preset_valid =
      string_equals(renderer->shadow_preset, "default") ||
      string_equals(renderer->shadow_preset, "balanced") ||
      string_equals(renderer->shadow_preset, "high");
  const bool8_t mode_valid =
      string_equals(renderer->render_mode, "default") ||
      string_equals(renderer->render_mode, "lighting") ||
      string_equals(renderer->render_mode, "normal") ||
      string_equals(renderer->render_mode, "unlit") ||
      string_equals(renderer->render_mode, "direct_diffuse") ||
      string_equals(renderer->render_mode, "direct_specular") ||
      string_equals(renderer->render_mode, "material_params") ||
      string_equals(renderer->render_mode, "temporal_motion") ||
      string_equals(renderer->render_mode, "temporal_history") ||
      string_equals(renderer->render_mode, "indirect_diffuse");
  const bool8_t backend_valid = renderer->backend[0] == '\0' ||
                                string_equals(renderer->backend, "vulkan") ||
                                string_equals(renderer->backend, "metal");
  const bool8_t exposure_mode_valid =
      string_equals(renderer->exposure_mode, "manual") ||
      string_equals(renderer->exposure_mode, "automatic");
  const bool8_t automatic_exposure =
      string_equals(renderer->exposure_mode, "automatic");
  const bool8_t automatic_controls_valid =
      automatic_exposure
          ? manual_exposure_token >= 0 && exposure_reset_token >= 0
          : exposure_reset_token < 0;
  const bool8_t bloom_values_valid =
      isfinite(bloom_threshold) && bloom_threshold >= 0.0 &&
      bloom_threshold <= FLT_MAX && isfinite(bloom_knee) && bloom_knee >= 0.0 &&
      bloom_knee <= FLT_MAX && isfinite(bloom_intensity) &&
      bloom_intensity >= 0.0 && bloom_intensity <= FLT_MAX;
  const bool8_t bloom_controls_present = bloom_threshold_token >= 0 &&
                                         bloom_knee_token >= 0 &&
                                         bloom_intensity_token >= 0;
  const bool8_t bloom_controls_valid =
      bloom_values_valid &&
      (!renderer->bloom_enabled || bloom_controls_present);
  const bool8_t gtao_values_valid =
      isfinite(gtao_radius) && gtao_radius >= VKR_GTAO_RADIUS_MIN &&
      gtao_radius <= VKR_GTAO_RADIUS_MAX && isfinite(gtao_power) &&
      gtao_power > 0.0 && gtao_power <= FLT_MAX;
  const bool8_t gtao_controls_present =
      gtao_radius_token >= 0 && gtao_power_token >= 0;
  const bool8_t gtao_controls_valid =
      gtao_values_valid && (!renderer->gtao_enabled || gtao_controls_present);
  if (!preset_valid || !mode_valid || !backend_valid || cascades < 1u ||
      cascades > 8u || !exposure_mode_valid || !automatic_controls_valid ||
      !bloom_controls_valid || !gtao_controls_valid ||
      !isfinite(manual_exposure) || manual_exposure <= 0.0 ||
      manual_exposure > FLT_MAX || !isfinite(exposure_compensation_ev) ||
      exposure_compensation_ev < -FLT_MAX ||
      exposure_compensation_ev > FLT_MAX || exposure_reset_frame > UINT32_MAX) {
    vkr_harness_error_set(
        error, "renderer.config", "$.renderer",
        "Renderer backend, preset, render/exposure mode, "
        "exposure/bloom/GTAO controls, or cascade count is invalid");
    return false_v;
  }
  renderer->manual_exposure = (float32_t)manual_exposure;
  renderer->exposure_compensation_ev = (float32_t)exposure_compensation_ev;
  renderer->exposure_reset_frame = (uint32_t)exposure_reset_frame;
  renderer->bloom_threshold = (float32_t)bloom_threshold;
  renderer->bloom_knee = (float32_t)bloom_knee;
  renderer->bloom_intensity = (float32_t)bloom_intensity;
  renderer->gtao_radius = (float32_t)gtao_radius;
  renderer->gtao_power = (float32_t)gtao_power;

  /* Resolve optional fields from the preset before parsing them. Reports and
     fingerprints describe the effective workload, not whether the JSON
     omitted a default. */
  const VkrShadowConfig shadow_config =
      string_equals(renderer->shadow_preset, "balanced")
          ? VKR_SHADOW_CONFIG_BALANCED
          : VKR_SHADOW_CONFIG_DEFAULT;
  uint64_t pcf_samples = shadow_config.pcf_sample_count;
  uint64_t map_size = shadow_config.shadow_map_size;
  float64_t split_lambda = shadow_config.cascade_split_lambda;
  renderer->shadow_pcf_early_out = shadow_config.pcf_uniform_early_out;
  renderer->shadow_sdsm = shadow_config.sdsm_enabled;
  if (!vkr_harness_manifest_u64(doc, token, "shadow_pcf_samples", false_v,
                                &pcf_samples, error) ||
      !vkr_harness_manifest_f64(doc, token, "shadow_split_lambda", false_v,
                                &split_lambda, error) ||
      !vkr_harness_manifest_u64(doc, token, "shadow_map_size", false_v,
                                &map_size, error) ||
      !vkr_harness_manifest_bool(doc, token, "shadow_pcf_early_out", false_v,
                                 &renderer->shadow_pcf_early_out, error) ||
      !vkr_harness_manifest_bool(doc, token, "shadow_sdsm", false_v,
                                 &renderer->shadow_sdsm, error)) {
    return false_v;
  }

  /* Reject invalid experiment labels instead of normalizing them into a
     different workload. The map-size set is deliberately bounded because one
     four-layer D32 image is already 256 MiB at 4096. */
  const bool8_t pcf_valid =
      pcf_samples <= UINT32_MAX &&
      vkr_shadow_pcf_sample_count_supported((uint32_t)pcf_samples);
  const bool8_t map_size_valid =
      map_size == 1024u || map_size == 2048u || map_size == 4096u;
  if (!pcf_valid || !map_size_valid || split_lambda < 0.0 ||
      split_lambda > 1.0) {
    vkr_harness_error_set(
        error, "renderer.shadow_config", "$.renderer",
        "Shadow PCF samples, split lambda, or map size is unsupported");
    return false_v;
  }
  renderer->shadow_pcf_samples = (uint32_t)pcf_samples;
  renderer->shadow_map_size = (uint32_t)map_size;
  renderer->shadow_split_lambda = (float32_t)split_lambda;
  return true_v;
}

static bool8_t vkr_harness_capture_channel_known(const char *channel) {
  return vkr_harness_capture_channel_description(channel) != NULL;
}

static bool8_t vkr_harness_parse_captures(const VkrHarnessJsonDocument *doc,
                                          int32_t token,
                                          VkrHarnessCase *case_manifest,
                                          VkrHarnessError *error) {
  if (token < 0) {
    return true_v;
  }
  if (doc->tokens[token].type != VKR_HARNESS_JSON_ARRAY ||
      doc->tokens[token].child_count > VKR_HARNESS_MAX_CAPTURES) {
    vkr_harness_error_set(error, "capture.array", "$.captures",
                          "Capture list exceeds its fixed capacity");
    return false_v;
  }
  static const char *const allowed[] = {"at_frame", "channels", "compare"};
  static const char *const required[] = {"at_frame", "channels"};
  int32_t item = token + 1;
  while (item >= 0 &&
         case_manifest->capture_count < doc->tokens[token].child_count) {
    if (!vkr_harness_json_object_validate(
            doc, item, allowed, ArrayCount(allowed), required,
            ArrayCount(required), "$.captures[]", error)) {
      return false_v;
    }
    VkrHarnessCapture *capture =
        &case_manifest->captures[case_manifest->capture_count++];
    uint64_t frame = 0;
    int32_t channels = -1;
    int32_t compare = -1;
    if (!vkr_harness_manifest_u64(doc, item, "at_frame", true_v, &frame,
                                  error) ||
        frame >= case_manifest->measure_frames ||
        !vkr_harness_manifest_field(doc, item, "channels", true_v, &channels,
                                    error) ||
        !vkr_harness_manifest_field(doc, item, "compare", false_v, &compare,
                                    error) ||
        doc->tokens[channels].type != VKR_HARNESS_JSON_ARRAY ||
        doc->tokens[channels].child_count == 0 ||
        doc->tokens[channels].child_count > VKR_HARNESS_MAX_CAPTURE_CHANNELS) {
      vkr_harness_error_set(error, "capture.config", "$.captures[]",
                            "Capture frame or channels are invalid");
      return false_v;
    }
    if (!vkr_harness_parse_compare(doc, compare, &case_manifest->compare,
                                   &capture->compare, "$.captures[].compare",
                                   error)) {
      return false_v;
    }
    capture->at_frame = (uint32_t)frame;
    int32_t channel_token = channels + 1;
    while (channel_token >= 0 &&
           capture->channel_count < doc->tokens[channels].child_count) {
      char *channel = capture->channels[capture->channel_count];
      if (!vkr_harness_json_string(doc, channel_token, channel, 64u,
                                   "$.captures[].channels[]", error) ||
          !vkr_harness_capture_channel_known(channel)) {
        vkr_harness_error_set(error, "capture.channel",
                              "$.captures[].channels[]",
                              "Unknown capture channel '%s'", channel);
        return false_v;
      }
      for (uint32_t prior = 0; prior < capture->channel_count; ++prior) {
        if (string_equals(capture->channels[prior], channel)) {
          vkr_harness_error_set(error, "capture.duplicate_channel",
                                "$.captures[].channels[]",
                                "Duplicate capture channel '%s'", channel);
          return false_v;
        }
      }
      capture->channel_count++;
      channel_token = vkr_harness_json_next(doc, channel_token);
    }
    item = vkr_harness_json_next(doc, item);
  }
  return true_v;
}

static bool8_t vkr_harness_parse_assertions(const VkrHarnessJsonDocument *doc,
                                            int32_t token,
                                            VkrHarnessCase *case_manifest,
                                            VkrHarnessError *error) {
  if (token < 0) {
    return true_v;
  }
  if (doc->tokens[token].type != VKR_HARNESS_JSON_ARRAY ||
      doc->tokens[token].child_count > VKR_HARNESS_MAX_ASSERTIONS) {
    vkr_harness_error_set(error, "assertion.array", "$.assertions",
                          "Assertion list exceeds its fixed capacity");
    return false_v;
  }
  static const char *const allowed[] = {"metric", "stat",   "max",
                                        "min",    "equals", "tolerance"};
  static const char *const required[] = {"metric"};
  int32_t item = token + 1;
  while (item >= 0 &&
         case_manifest->assertion_count < doc->tokens[token].child_count) {
    if (!vkr_harness_json_object_validate(
            doc, item, allowed, ArrayCount(allowed), required,
            ArrayCount(required), "$.assertions[]", error)) {
      return false_v;
    }
    VkrHarnessAssertion *assertion =
        &case_manifest->assertions[case_manifest->assertion_count++];
    char statistic[16] = "mean";
    float64_t tolerance = 0.0;
    if (!vkr_harness_manifest_string(doc, item, "metric", true_v,
                                     assertion->metric,
                                     sizeof(assertion->metric), error) ||
        !vkr_harness_metric_name_valid(assertion->metric) ||
        !vkr_harness_manifest_string(doc, item, "stat", false_v, statistic,
                                     sizeof(statistic), error) ||
        !vkr_harness_manifest_f64(doc, item, "tolerance", false_v, &tolerance,
                                  error) ||
        tolerance < 0.0) {
      vkr_harness_error_set(error, "assertion.config", "$.assertions[]",
                            "Assertion metric/stat/tolerance is invalid");
      return false_v;
    }
    const bool8_t stat_found =
        vkr_harness_statistic_from_name(statistic, &assertion->statistic);
    int32_t limit_tokens[3];
    uint32_t present_count = 0;
    for (uint32_t i = 0; i < ArrayCount(limit_tokens); ++i) {
      const char *limit_name =
          vkr_harness_operator_name((VkrHarnessAssertionOperator)i);
      (void)vkr_harness_manifest_field(doc, item, limit_name, false_v,
                                       &limit_tokens[i], error);
      if (limit_tokens[i] >= 0) {
        present_count++;
        assertion->operation = (VkrHarnessAssertionOperator)i;
        if (!vkr_harness_json_f64(doc, limit_tokens[i], &assertion->limit,
                                  limit_name, error)) {
          return false_v;
        }
      }
    }
    assertion->tolerance = tolerance;
    if (!stat_found || present_count != 1u ||
        (tolerance > 0.0 &&
         assertion->operation != VKR_HARNESS_ASSERT_EQUALS)) {
      vkr_harness_error_set(error, "assertion.operator", "$.assertions[]",
                            "Exactly one max/min/equals is required; tolerance "
                            "belongs to equals");
      return false_v;
    }
    item = vkr_harness_json_next(doc, item);
  }
  return true_v;
}

bool8_t vkr_harness_case_parse(const char *json, uint64_t json_length,
                               const char *manifest_path,
                               VkrHarnessCase *out_case,
                               VkrHarnessError *out_error) {
  vkr_harness_error_clear(out_error);
  if (!out_case) {
    return false_v;
  }
  VkrHarnessJsonDocument doc;
  if (!vkr_harness_json_parse(&doc, json, json_length, out_error) ||
      doc.tokens[0].type != VKR_HARNESS_JSON_OBJECT) {
    return false_v;
  }
  static const char *const allowed[] = {"schema_version",
                                        "id",
                                        "suite",
                                        "description",
                                        "scene",
                                        "seed",
                                        "resolution",
                                        "resize_round_trip",
                                        "boot",
                                        "target",
                                        "present",
                                        "target_image_count",
                                        "cache",
                                        "fixed_delta",
                                        "frames",
                                        "repetitions",
                                        "repetition_timeout_ms",
                                        "asset_ready_timeout_ms",
                                        "camera",
                                        "renderer",
                                        "captures",
                                        "assertions",
                                        "compare"};
  static const char *const required[] = {
      "schema_version", "id",     "suite",    "scene",   "seed",
      "resolution",     "boot",   "target",   "present", "cache",
      "fixed_delta",    "frames", "renderer", "camera"};
  if (!vkr_harness_json_object_validate(&doc, 0, allowed, ArrayCount(allowed),
                                        required, ArrayCount(required), "$",
                                        out_error)) {
    return false_v;
  }
  *out_case = (VkrHarnessCase){
      .target_image_count = 3u,
      .repetitions = 1u,
      .repetition_timeout_ms = 60000u,
      .asset_ready_timeout_ms = 30000u,
      .warmup_frames = 120u,
      .compare = vkr_harness_compare_defaults(),
  };
  string_format(out_case->manifest_path, sizeof(out_case->manifest_path), "%s",
                manifest_path ? manifest_path : "<memory>");
  uint64_t schema = 0;
  uint64_t target_images = out_case->target_image_count;
  int32_t target_images_token = -1;
  uint64_t repetitions = out_case->repetitions;
  uint64_t repetition_timeout = out_case->repetition_timeout_ms;
  uint64_t asset_timeout = out_case->asset_ready_timeout_ms;
  char boot[16];
  char target[32];
  char present[16];
  char cache[24];
  if (!vkr_harness_manifest_u64(&doc, 0, "schema_version", true_v, &schema,
                                out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "id", true_v, out_case->id,
                                   sizeof(out_case->id), out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "suite", true_v, out_case->suite,
                                   sizeof(out_case->suite), out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "description", false_v,
                                   out_case->description,
                                   sizeof(out_case->description), out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "scene", true_v, out_case->scene,
                                   sizeof(out_case->scene), out_error) ||
      !vkr_harness_manifest_u64(&doc, 0, "seed", true_v, &out_case->seed,
                                out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "boot", true_v, boot, sizeof(boot),
                                   out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "target", true_v, target,
                                   sizeof(target), out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "present", true_v, present,
                                   sizeof(present), out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "cache", true_v, cache,
                                   sizeof(cache), out_error) ||
      !vkr_harness_manifest_f64(&doc, 0, "fixed_delta", true_v,
                                &out_case->fixed_delta_seconds, out_error) ||
      !vkr_harness_manifest_field(&doc, 0, "target_image_count", false_v,
                                  &target_images_token, out_error) ||
      (target_images_token >= 0 &&
       !vkr_harness_json_u64(&doc, target_images_token, &target_images,
                             "$.target_image_count", out_error)) ||
      !vkr_harness_manifest_u64(&doc, 0, "repetitions", false_v, &repetitions,
                                out_error) ||
      !vkr_harness_manifest_u64(&doc, 0, "repetition_timeout_ms", false_v,
                                &repetition_timeout, out_error) ||
      !vkr_harness_manifest_u64(&doc, 0, "asset_ready_timeout_ms", false_v,
                                &asset_timeout, out_error)) {
    return false_v;
  }
  out_case->schema_version = (uint32_t)schema;
  out_case->target_image_count = (uint32_t)target_images;
  out_case->repetitions = (uint32_t)repetitions;
  out_case->repetition_timeout_ms = (uint32_t)repetition_timeout;
  out_case->asset_ready_timeout_ms = (uint32_t)asset_timeout;
  if (schema != VKR_HARNESS_SCHEMA_VERSION ||
      !vkr_harness_manifest_id_valid(out_case->id) ||
      !vkr_harness_manifest_component_valid(out_case->suite) ||
      !vkr_harness_path_is_safe_relative(out_case->scene) ||
      !string_n_equals(out_case->scene, "assets/scenes/", 14u) ||
      !string_n_equals(out_case->id, out_case->suite,
                       string_length(out_case->suite)) ||
      out_case->id[string_length(out_case->suite)] != '.' ||
      out_case->seed > UINT32_MAX || out_case->fixed_delta_seconds <= 0.0 ||
      target_images < 1u || target_images > 8u || repetitions == 0u ||
      repetitions > VKR_HARNESS_MAX_RUNS || repetition_timeout == 0u ||
      repetition_timeout > UINT32_MAX || asset_timeout == 0u ||
      asset_timeout > UINT32_MAX ||
      (!string_equals(boot, "full") && !string_equals(boot, "automation")) ||
      !vkr_harness_parse_target(target, &out_case->target) ||
      !vkr_harness_parse_present(present, &out_case->present)) {
    vkr_harness_error_set(
        out_error, "case.semantic", "$",
        "Case identity, path, enum, timing, or count is invalid");
    return false_v;
  }
  out_case->boot = string_equals(boot, "full") ? VKR_HARNESS_BOOT_FULL
                                               : VKR_HARNESS_BOOT_AUTOMATION;
  if (string_equals(cache, "isolated_cold")) {
    out_case->cache = VKR_HARNESS_CACHE_ISOLATED_COLD;
  } else if (string_equals(cache, "isolated_warm")) {
    out_case->cache = VKR_HARNESS_CACHE_ISOLATED_WARM;
  } else if (string_equals(cache, "shared")) {
    out_case->cache = VKR_HARNESS_CACHE_SHARED;
  } else {
    vkr_harness_error_set(out_error, "case.cache", "$.cache",
                          "Unknown cache mode '%s'", cache);
    return false_v;
  }
  if ((out_case->target == VKR_HARNESS_TARGET_OFFSCREEN) !=
      (out_case->present == VKR_HARNESS_PRESENT_NONE)) {
    vkr_harness_error_set(
        out_error, "case.target_present", "$",
        "Offscreen requires present=none; windowed requires a present mode");
    return false_v;
  }
  if (out_case->target != VKR_HARNESS_TARGET_OFFSCREEN &&
      target_images_token >= 0) {
    vkr_harness_error_set(
        out_error, "case.target_image_count", "$.target_image_count",
        "Windowed cases must use the WSI-selected image count");
    return false_v;
  }
  int32_t resolution = -1;
  int32_t resize_round_trip = -1;
  int32_t frames = -1;
  int32_t renderer = -1;
  int32_t camera = -1;
  int32_t captures = -1;
  int32_t assertions = -1;
  int32_t compare = -1;
  if (!vkr_harness_manifest_field(&doc, 0, "resolution", true_v, &resolution,
                                  out_error) ||
      doc.tokens[resolution].type != VKR_HARNESS_JSON_ARRAY ||
      doc.tokens[resolution].child_count != 2u) {
    return false_v;
  }
  uint64_t width = 0;
  uint64_t height = 0;
  int32_t dimension = resolution + 1;
  if (!vkr_harness_json_u64(&doc, dimension, &width, "$.resolution[0]",
                            out_error) ||
      !vkr_harness_json_u64(&doc, vkr_harness_json_next(&doc, dimension),
                            &height, "$.resolution[1]", out_error) ||
      width == 0 || height == 0 || width > UINT32_MAX || height > UINT32_MAX) {
    return false_v;
  }
  out_case->width = (uint32_t)width;
  out_case->height = (uint32_t)height;
  static const char *const frame_allowed[] = {"warmup", "measure"};
  static const char *const frame_required[] = {"measure"};
  if (!vkr_harness_manifest_field(&doc, 0, "frames", true_v, &frames,
                                  out_error) ||
      !vkr_harness_json_object_validate(
          &doc, frames, frame_allowed, ArrayCount(frame_allowed),
          frame_required, ArrayCount(frame_required), "$.frames", out_error)) {
    return false_v;
  }
  uint64_t warmup = out_case->warmup_frames;
  uint64_t measure = 0;
  if (!vkr_harness_manifest_u64(&doc, frames, "warmup", false_v, &warmup,
                                out_error) ||
      !vkr_harness_manifest_u64(&doc, frames, "measure", true_v, &measure,
                                out_error) ||
      warmup > UINT32_MAX || measure == 0u || measure > UINT32_MAX ||
      warmup > UINT32_MAX - measure) {
    return false_v;
  }
  out_case->warmup_frames = (uint32_t)warmup;
  out_case->measure_frames = (uint32_t)measure;
  if (!vkr_harness_manifest_field(&doc, 0, "resize_round_trip", false_v,
                                  &resize_round_trip, out_error)) {
    return false_v;
  }
  if (resize_round_trip >= 0) {
    if (doc.tokens[resize_round_trip].type != VKR_HARNESS_JSON_ARRAY ||
        doc.tokens[resize_round_trip].child_count != 2u) {
      vkr_harness_error_set(out_error, "case.resize_round_trip",
                            "$.resize_round_trip",
                            "Resize round trip must be [width, height]");
      return false_v;
    }
    uint64_t resize_width = 0u;
    uint64_t resize_height = 0u;
    const int32_t resize_width_token = resize_round_trip + 1;
    if (!vkr_harness_json_u64(&doc, resize_width_token, &resize_width,
                              "$.resize_round_trip[0]", out_error) ||
        !vkr_harness_json_u64(
            &doc, vkr_harness_json_next(&doc, resize_width_token),
            &resize_height, "$.resize_round_trip[1]", out_error) ||
        resize_width == 0u || resize_height == 0u ||
        resize_width > UINT32_MAX || resize_height > UINT32_MAX ||
        (resize_width == width && resize_height == height) ||
        out_case->target != VKR_HARNESS_TARGET_WINDOWED_HIDDEN ||
        out_case->warmup_frames < 3u) {
      vkr_harness_error_set(
          out_error, "case.resize_round_trip", "$.resize_round_trip",
          "Resize round trip requires a different non-zero windowed-hidden "
          "extent and at least three warmup frames");
      return false_v;
    }
    out_case->resize_round_trip = true_v;
    out_case->resize_width = (uint32_t)resize_width;
    out_case->resize_height = (uint32_t)resize_height;
  }
  /* Case-level comparison thresholds are the fallback each capture inherits,
     so they must be resolved before the capture list is walked. */
  if (!vkr_harness_manifest_field(&doc, 0, "renderer", true_v, &renderer,
                                  out_error) ||
      !vkr_harness_parse_renderer(&doc, renderer, &out_case->renderer,
                                  out_error) ||
      !vkr_harness_manifest_field(&doc, 0, "camera", true_v, &camera,
                                  out_error) ||
      !vkr_harness_parse_camera(&doc, camera, &out_case->camera, out_error) ||
      !vkr_harness_manifest_field(&doc, 0, "compare", false_v, &compare,
                                  out_error) ||
      !vkr_harness_parse_compare(&doc, compare, NULL, &out_case->compare,
                                 "$.compare", out_error) ||
      !vkr_harness_manifest_field(&doc, 0, "captures", false_v, &captures,
                                  out_error) ||
      !vkr_harness_parse_captures(&doc, captures, out_case, out_error) ||
      !vkr_harness_manifest_field(&doc, 0, "assertions", false_v, &assertions,
                                  out_error) ||
      !vkr_harness_parse_assertions(&doc, assertions, out_case, out_error)) {
    return false_v;
  }
  if (out_case->renderer.exposure_reset_frame != UINT32_MAX &&
      out_case->renderer.exposure_reset_frame >= out_case->measure_frames) {
    vkr_harness_error_set(out_error, "renderer.exposure_reset_frame",
                          "$.renderer.exposure_reset_frame",
                          "Exposure reset frame must be inside the measured "
                          "frame range");
    return false_v;
  }
  vkr_harness_sha256_bytes(json, json_length, out_case->manifest_sha256);
  return true_v;
}

bool8_t vkr_harness_profile_parse(const char *json, uint64_t json_length,
                                  const char *manifest_path,
                                  VkrHarnessProfile *out_profile,
                                  VkrHarnessError *out_error) {
  vkr_harness_error_clear(out_error);
  if (!out_profile) {
    return false_v;
  }
  VkrHarnessJsonDocument doc;
  if (!vkr_harness_json_parse(&doc, json, json_length, out_error) ||
      doc.tokens[0].type != VKR_HARNESS_JSON_OBJECT) {
    return false_v;
  }
  static const char *const allowed[] = {
      "schema_version",  "id",           "description",
      "authoritative",   "dirty_policy", "environment",
      "instrumentation", "execution",    "required_metrics"};
  static const char *const required[] = {"schema_version", "id",
                                         "authoritative",  "dirty_policy",
                                         "environment",    "instrumentation",
                                         "execution",      "required_metrics"};
  if (!vkr_harness_json_object_validate(&doc, 0, allowed, ArrayCount(allowed),
                                        required, ArrayCount(required), "$",
                                        out_error)) {
    return false_v;
  }
  *out_profile = (VkrHarnessProfile){
      .minimum_repetitions = 1u,
      .warmup_stability_window = 30u,
      .warmup_max_drift_ratio = 0.10,
      .require_warmup_stability = true_v,
  };
  string_format(out_profile->warmup_stability_metric,
                sizeof(out_profile->warmup_stability_metric),
                "cpu.render_submit");
  string_format(out_profile->manifest_path, sizeof(out_profile->manifest_path),
                "%s", manifest_path ? manifest_path : "<memory>");
  uint64_t schema = 0;
  char dirty[24];
  if (!vkr_harness_manifest_u64(&doc, 0, "schema_version", true_v, &schema,
                                out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "id", true_v, out_profile->id,
                                   sizeof(out_profile->id), out_error) ||
      !vkr_harness_manifest_string(
          &doc, 0, "description", false_v, out_profile->description,
          sizeof(out_profile->description), out_error) ||
      !vkr_harness_manifest_bool(&doc, 0, "authoritative", true_v,
                                 &out_profile->authoritative, out_error) ||
      !vkr_harness_manifest_string(&doc, 0, "dirty_policy", true_v, dirty,
                                   sizeof(dirty), out_error) ||
      schema != VKR_HARNESS_SCHEMA_VERSION ||
      !vkr_harness_manifest_id_valid(out_profile->id)) {
    return false_v;
  }
  out_profile->schema_version = (uint32_t)schema;
  if (string_equals(dirty, "require_clean")) {
    out_profile->allow_dirty = false_v;
  } else if (string_equals(dirty, "allow")) {
    out_profile->allow_dirty = true_v;
  } else {
    vkr_harness_error_set(out_error, "profile.dirty_policy", "$.dirty_policy",
                          "Unknown dirty policy '%s'", dirty);
    return false_v;
  }
  int32_t environment = -1;
  int32_t instrumentation = -1;
  int32_t execution = -1;
  int32_t required_metrics = -1;
  static const char *const environment_fields[] = {"target",
                                                   "required_present",
                                                   "require_actual_present",
                                                   "os",
                                                   "cpu",
                                                   "gpu",
                                                   "driver",
                                                   "gpu_vendor_id",
                                                   "gpu_device_id",
                                                   "power_mode",
                                                   "thermal_state",
                                                   "process_priority"};
  static const char *const environment_required[] = {
      "target", "required_present", "require_actual_present"};
  if (!vkr_harness_manifest_field(&doc, 0, "environment", true_v, &environment,
                                  out_error) ||
      !vkr_harness_json_object_validate(
          &doc, environment, environment_fields, ArrayCount(environment_fields),
          environment_required, ArrayCount(environment_required),
          "$.environment", out_error)) {
    return false_v;
  }
  char target[32];
  char present[16];
  uint64_t vendor_id = 0;
  uint64_t device_id = 0;
  int32_t process_priority = -1;
  if (!vkr_harness_manifest_string(&doc, environment, "target", true_v, target,
                                   sizeof(target), out_error) ||
      !vkr_harness_manifest_string(&doc, environment, "required_present",
                                   true_v, present, sizeof(present),
                                   out_error) ||
      !vkr_harness_manifest_bool(&doc, environment, "require_actual_present",
                                 true_v, &out_profile->require_actual_present,
                                 out_error) ||
      !vkr_harness_manifest_string(
          &doc, environment, "os", false_v, out_profile->required_os,
          sizeof(out_profile->required_os), out_error) ||
      !vkr_harness_manifest_string(
          &doc, environment, "cpu", false_v, out_profile->required_cpu,
          sizeof(out_profile->required_cpu), out_error) ||
      !vkr_harness_manifest_string(
          &doc, environment, "gpu", false_v, out_profile->required_gpu,
          sizeof(out_profile->required_gpu), out_error) ||
      !vkr_harness_manifest_string(
          &doc, environment, "driver", false_v, out_profile->required_driver,
          sizeof(out_profile->required_driver), out_error) ||
      !vkr_harness_manifest_u64(&doc, environment, "gpu_vendor_id", false_v,
                                &vendor_id, out_error) ||
      !vkr_harness_manifest_u64(&doc, environment, "gpu_device_id", false_v,
                                &device_id, out_error) ||
      !vkr_harness_manifest_string(&doc, environment, "power_mode", false_v,
                                   out_profile->required_power_mode,
                                   sizeof(out_profile->required_power_mode),
                                   out_error) ||
      !vkr_harness_manifest_string(&doc, environment, "thermal_state", false_v,
                                   out_profile->required_thermal_state,
                                   sizeof(out_profile->required_thermal_state),
                                   out_error) ||
      !vkr_harness_parse_target(target, &out_profile->target) ||
      !vkr_harness_parse_present(present, &out_profile->required_present) ||
      vendor_id > UINT32_MAX || device_id > UINT32_MAX) {
    return false_v;
  }
  out_profile->required_gpu_vendor_id = (uint32_t)vendor_id;
  out_profile->required_gpu_device_id = (uint32_t)device_id;
  if (!vkr_harness_manifest_field(&doc, environment, "process_priority",
                                  false_v, &process_priority, out_error)) {
    return false_v;
  }
  if (process_priority >= 0) {
    float64_t priority = 0.0;
    if (!vkr_harness_json_f64(&doc, process_priority, &priority,
                              "$.environment.process_priority", out_error) ||
        priority < -20.0 || priority > 20.0 ||
        priority != (float64_t)(int32_t)priority) {
      return false_v;
    }
    out_profile->required_process_priority = (int32_t)priority;
    out_profile->has_required_process_priority = true_v;
  }
  static const char *const instrumentation_fields[] = {
      "gpu_timing", "submission_gpu_timing", "event_subjects"};
  static const char *const required_instrumentation_fields[] = {
      "gpu_timing", "event_subjects"};
  if (!vkr_harness_manifest_field(&doc, 0, "instrumentation", true_v,
                                  &instrumentation, out_error) ||
      !vkr_harness_json_object_validate(
          &doc, instrumentation, instrumentation_fields,
          ArrayCount(instrumentation_fields), required_instrumentation_fields,
          ArrayCount(required_instrumentation_fields), "$.instrumentation",
          out_error) ||
      !vkr_harness_manifest_bool(&doc, instrumentation, "gpu_timing", true_v,
                                 &out_profile->gpu_timing, out_error) ||
      !vkr_harness_manifest_bool(&doc, instrumentation, "submission_gpu_timing",
                                 false_v, &out_profile->submission_gpu_timing,
                                 out_error) ||
      !vkr_harness_manifest_bool(&doc, instrumentation, "event_subjects",
                                 true_v, &out_profile->event_subjects,
                                 out_error)) {
    return false_v;
  }
  static const char *const execution_fields[] = {
      "minimum_repetitions",      "warmup_stability_window",
      "warmup_stability_metric",  "warmup_max_drift_ratio",
      "require_warmup_stability", "exclusive_gpu_lane"};
  static const char *const required_execution_fields[] = {
      "minimum_repetitions", "warmup_stability_window",
      "warmup_max_drift_ratio", "require_warmup_stability",
      "exclusive_gpu_lane"};
  if (!vkr_harness_manifest_field(&doc, 0, "execution", true_v, &execution,
                                  out_error) ||
      !vkr_harness_json_object_validate(
          &doc, execution, execution_fields, ArrayCount(execution_fields),
          required_execution_fields, ArrayCount(required_execution_fields),
          "$.execution", out_error)) {
    return false_v;
  }
  uint64_t minimum_repetitions = 0;
  uint64_t stability_window = 0;
  if (!vkr_harness_manifest_u64(&doc, execution, "minimum_repetitions", true_v,
                                &minimum_repetitions, out_error) ||
      !vkr_harness_manifest_u64(&doc, execution, "warmup_stability_window",
                                true_v, &stability_window, out_error) ||
      !vkr_harness_manifest_string(
          &doc, execution, "warmup_stability_metric", false_v,
          out_profile->warmup_stability_metric,
          sizeof(out_profile->warmup_stability_metric), out_error) ||
      !vkr_harness_manifest_f64(&doc, execution, "warmup_max_drift_ratio",
                                true_v, &out_profile->warmup_max_drift_ratio,
                                out_error) ||
      !vkr_harness_manifest_bool(&doc, execution, "require_warmup_stability",
                                 true_v, &out_profile->require_warmup_stability,
                                 out_error) ||
      !vkr_harness_manifest_bool(&doc, execution, "exclusive_gpu_lane", true_v,
                                 &out_profile->require_exclusive_gpu_lane,
                                 out_error)) {
    return false_v;
  }
  if (minimum_repetitions == 0u || minimum_repetitions > VKR_HARNESS_MAX_RUNS) {
    vkr_harness_error_set(
        out_error, "profile.repetitions", "$.execution.minimum_repetitions",
        "minimum_repetitions must be between 1 and %u", VKR_HARNESS_MAX_RUNS);
    return false_v;
  }
  /* One process is an observation, not a result: an authoritative profile
     cannot describe fewer than two independent repetitions. */
  if (out_profile->authoritative && minimum_repetitions < 2u) {
    vkr_harness_error_set(out_error, "profile.authoritative_repetitions",
                          "$.execution.minimum_repetitions",
                          "An authoritative profile requires at least two "
                          "independent repetitions");
    return false_v;
  }
  if (stability_window < 2u || stability_window > UINT32_MAX) {
    vkr_harness_error_set(out_error, "profile.stability_window",
                          "$.execution.warmup_stability_window",
                          "warmup_stability_window must be between 2 and %u",
                          UINT32_MAX);
    return false_v;
  }
  if (out_profile->warmup_max_drift_ratio < 0.0) {
    vkr_harness_error_set(out_error, "profile.drift_ratio",
                          "$.execution.warmup_max_drift_ratio",
                          "warmup_max_drift_ratio must not be negative");
    return false_v;
  }
  if (!vkr_harness_metric_name_valid(out_profile->warmup_stability_metric)) {
    vkr_harness_error_set(out_error, "profile.stability_metric",
                          "$.execution.warmup_stability_metric",
                          "warmup_stability_metric must be a metric name");
    return false_v;
  }
  out_profile->minimum_repetitions = (uint32_t)minimum_repetitions;
  out_profile->warmup_stability_window = (uint32_t)stability_window;
  if (!vkr_harness_manifest_field(&doc, 0, "required_metrics", true_v,
                                  &required_metrics, out_error) ||
      doc.tokens[required_metrics].type != VKR_HARNESS_JSON_ARRAY ||
      doc.tokens[required_metrics].child_count >
          VKR_HARNESS_MAX_REQUIRED_METRICS) {
    return false_v;
  }
  int32_t metric = required_metrics + 1;
  while (metric >= 0 && out_profile->required_metric_count <
                            doc.tokens[required_metrics].child_count) {
    char *name =
        out_profile->required_metrics[out_profile->required_metric_count];
    if (!vkr_harness_json_string(&doc, metric, name, 128u,
                                 "$.required_metrics[]", out_error) ||
        !vkr_harness_metric_name_valid(name)) {
      return false_v;
    }
    for (uint32_t prior = 0; prior < out_profile->required_metric_count;
         ++prior) {
      if (string_equals(out_profile->required_metrics[prior], name)) {
        vkr_harness_error_set(out_error, "profile.duplicate_metric",
                              "$.required_metrics[]",
                              "Duplicate required metric '%s'", name);
        return false_v;
      }
    }
    out_profile->required_metric_count++;
    metric = vkr_harness_json_next(&doc, metric);
  }
  if ((out_profile->target == VKR_HARNESS_TARGET_OFFSCREEN) !=
      (out_profile->required_present == VKR_HARNESS_PRESENT_NONE)) {
    vkr_harness_error_set(out_error, "profile.target_present", "$.environment",
                          "Offscreen profiles require present=none and "
                          "windowed profiles require presentation");
    return false_v;
  }
  vkr_harness_sha256_bytes(json, json_length, out_profile->manifest_sha256);
  return true_v;
}

static bool8_t vkr_harness_load_text(const char *path, Arena *arena,
                                     char **out_data, uint64_t *out_length,
                                     VkrHarnessError *error) {
  FilePath file_path = vkr_harness_file_path(path);
  FileStats stats = {0};
  if (file_stats(&file_path, &stats) != FILE_ERROR_NONE) {
    vkr_harness_error_set(error, "manifest.open", "$",
                          "Unable to open manifest '%s'", path);
    return false_v;
  }
  if (stats.size == 0u || stats.size > VKR_HARNESS_MANIFEST_MAX_BYTES) {
    vkr_harness_error_set(error, "manifest.size", "$",
                          "Manifest is empty or exceeds %llu bytes",
                          (unsigned long long)VKR_HARNESS_MANIFEST_MAX_BYTES);
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_harness_read_file(path, arena, &bytes, &size) || size == 0u ||
      size > VKR_HARNESS_MANIFEST_MAX_BYTES) {
    vkr_harness_error_set(error, "manifest.read", "$",
                          "Unable to read manifest '%s'", path);
    return false_v;
  }
  char *data = arena_alloc(arena, size + 1u, ARENA_MEMORY_TAG_FILE);
  if (!data) {
    vkr_harness_error_set(error, "manifest.read", "$",
                          "Unable to allocate manifest storage");
    return false_v;
  }
  MemCopy(data, bytes, size);
  data[size] = '\0';
  *out_data = data;
  *out_length = size;
  return true_v;
}

bool8_t vkr_harness_case_load(const char *repository_root,
                              const char *relative_manifest_path,
                              VkrHarnessCase *out_case,
                              VkrHarnessError *out_error) {
  char path[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_resolve_existing_path(
          repository_root, relative_manifest_path, path, out_error)) {
    return false_v;
  }
  char *json = NULL;
  uint64_t length = 0;
  Arena *load_arena = arena_create(MB(2), KB(4));
  if (!load_arena ||
      !vkr_harness_load_text(path, load_arena, &json, &length, out_error)) {
    arena_destroy(load_arena);
    return false_v;
  }
  const bool8_t parsed = vkr_harness_case_parse(
      json, length, relative_manifest_path, out_case, out_error);
  arena_destroy(load_arena);
  if (!parsed) {
    return false_v;
  }
  char scene[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_resolve_existing_path(repository_root, out_case->scene,
                                         scene, out_error)) {
    return false_v;
  }
  const char prefix[] = "tools/cases/";
  if (!string_n_equals(relative_manifest_path, prefix, sizeof(prefix) - 1u)) {
    vkr_harness_error_set(out_error, "case.root", "$.suite",
                          "Cases must live under tools/cases/<suite>/");
    return false_v;
  }
  const char *suite = relative_manifest_path + sizeof(prefix) - 1u;
  const char *slash = string_find_char(suite, '/');
  if (!slash || (uint64_t)(slash - suite) != string_length(out_case->suite) ||
      MemCompare(suite, out_case->suite, (uint64_t)(slash - suite)) != 0) {
    vkr_harness_error_set(out_error, "case.suite", "$.suite",
                          "Case suite does not match its containing directory");
    return false_v;
  }
  return true_v;
}

bool8_t vkr_harness_profile_load(const char *repository_root,
                                 const char *relative_manifest_path,
                                 VkrHarnessProfile *out_profile,
                                 VkrHarnessError *out_error) {
  char path[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_resolve_existing_path(
          repository_root, relative_manifest_path, path, out_error)) {
    return false_v;
  }
  char *json = NULL;
  uint64_t length = 0;
  Arena *load_arena = arena_create(MB(2), KB(4));
  if (!load_arena ||
      !vkr_harness_load_text(path, load_arena, &json, &length, out_error)) {
    arena_destroy(load_arena);
    return false_v;
  }
  const bool8_t parsed = vkr_harness_profile_parse(
      json, length, relative_manifest_path, out_profile, out_error);
  arena_destroy(load_arena);
  if (!parsed) {
    return false_v;
  }
  const char prefix[] = "tools/profiles/";
  if (!string_n_equals(relative_manifest_path, prefix, sizeof(prefix) - 1u)) {
    vkr_harness_error_set(out_error, "profile.root", "$.id",
                          "Profiles must live under tools/profiles/");
    return false_v;
  }
  return true_v;
}
