#include "vkr_harness.h"

#include <math.h>

static float32_t vkr_harness_lerp_f32(float32_t a, float32_t b, float32_t t) {
  return a + (b - a) * t;
}

static Vec3 vkr_harness_lerp_vec3(Vec3 a, Vec3 b, float32_t t) {
  return vec3_new(vkr_harness_lerp_f32(a.x, b.x, t),
                  vkr_harness_lerp_f32(a.y, b.y, t),
                  vkr_harness_lerp_f32(a.z, b.z, t));
}

static float32_t vkr_harness_yaw_delta(float32_t from, float32_t to) {
  float32_t delta = fmodf(to - from, 360.0f);
  if (delta < -180.0f) {
    delta += 360.0f;
  } else if (delta > 180.0f) {
    delta -= 360.0f;
  } else if (delta == -180.0f) {
    delta = 180.0f;
  }
  return delta;
}

static float32_t vkr_harness_catmull(float32_t p0, float32_t p1, float32_t p2,
                                     float32_t p3, float32_t t) {
  const float32_t t2 = t * t;
  const float32_t t3 = t2 * t;
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                 (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

static Vec3 vkr_harness_catmull_vec3(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3,
                                     float32_t t) {
  return vec3_new(vkr_harness_catmull(p0.x, p1.x, p2.x, p3.x, t),
                  vkr_harness_catmull(p0.y, p1.y, p2.y, p3.y, t),
                  vkr_harness_catmull(p0.z, p1.z, p2.z, p3.z, t));
}

float64_t vkr_harness_speed_multiplier(VkrHarnessSpeed speed) {
  switch (speed) {
  case VKR_HARNESS_SPEED_SLOW:
    return 0.5;
  case VKR_HARNESS_SPEED_FAST:
    return 2.0;
  case VKR_HARNESS_SPEED_MEDIUM:
  default:
    return 1.0;
  }
}

static uint32_t vkr_harness_camera_segment(const VkrHarnessCamera *camera,
                                           float64_t time_seconds,
                                           float32_t *out_local_t) {
  if (time_seconds <= camera->keys[0].time_seconds) {
    *out_local_t = 0.0f;
    return 0;
  }
  const uint32_t last = camera->key_count - 1u;
  if (time_seconds >= camera->keys[last].time_seconds) {
    *out_local_t = 1.0f;
    return last - 1u;
  }
  for (uint32_t i = 0; i < last; ++i) {
    const float64_t a = camera->keys[i].time_seconds;
    const float64_t b = camera->keys[i + 1u].time_seconds;
    if (time_seconds <= b) {
      *out_local_t = (float32_t)((time_seconds - a) / (b - a));
      return i;
    }
  }
  *out_local_t = 1.0f;
  return last - 1u;
}

static void vkr_harness_camera_evaluate_keys(const VkrHarnessCamera *camera,
                                             float64_t time_seconds,
                                             VkrHarnessCameraPose *out_pose) {
  float32_t t = 0.0f;
  const uint32_t segment = vkr_harness_camera_segment(camera, time_seconds, &t);
  const uint32_t last = camera->key_count - 1u;
  const VkrHarnessCameraKey *a = &camera->keys[segment];
  const VkrHarnessCameraKey *b = &camera->keys[segment + 1u];
  if (camera->interpolation == VKR_HARNESS_CAMERA_INTERPOLATION_LINEAR) {
    out_pose->position = vkr_harness_lerp_vec3(a->position, b->position, t);
    out_pose->yaw_degrees =
        a->yaw_degrees +
        vkr_harness_yaw_delta(a->yaw_degrees, b->yaw_degrees) * t;
    out_pose->pitch_degrees =
        vkr_harness_lerp_f32(a->pitch_degrees, b->pitch_degrees, t);
    return;
  }
  const VkrHarnessCameraKey *p0 =
      &camera->keys[segment > 0 ? segment - 1u : 0u];
  const VkrHarnessCameraKey *p3 =
      &camera->keys[segment + 2u <= last ? segment + 2u : last];
  out_pose->position = vkr_harness_catmull_vec3(p0->position, a->position,
                                                b->position, p3->position, t);
  const float32_t yaw0 =
      a->yaw_degrees - vkr_harness_yaw_delta(p0->yaw_degrees, a->yaw_degrees);
  const float32_t yaw2 =
      a->yaw_degrees + vkr_harness_yaw_delta(a->yaw_degrees, b->yaw_degrees);
  const float32_t yaw3 =
      yaw2 + vkr_harness_yaw_delta(b->yaw_degrees, p3->yaw_degrees);
  out_pose->yaw_degrees =
      vkr_harness_catmull(yaw0, a->yaw_degrees, yaw2, yaw3, t);
  out_pose->pitch_degrees =
      vkr_harness_catmull(p0->pitch_degrees, a->pitch_degrees, b->pitch_degrees,
                          p3->pitch_degrees, t);
}

bool8_t vkr_harness_camera_prepare(VkrHarnessCamera *camera,
                                   VkrHarnessError *out_error) {
  if (!camera || camera->vertical_fov_degrees <= 0.0f ||
      camera->vertical_fov_degrees >= 180.0f || camera->near_plane <= 0.0f ||
      camera->far_plane <= camera->near_plane) {
    vkr_harness_error_set(out_error, "camera.lens", "$.camera",
                          "Camera lens values are invalid");
    return false_v;
  }
  if (camera->mode == VKR_HARNESS_CAMERA_STATIC) {
    return true_v;
  }
  if (camera->mode == VKR_HARNESS_CAMERA_ORBIT) {
    if (camera->orbit_radius <= 0.0f || camera->orbit_duration_seconds <= 0.0 ||
        camera->orbit_revolutions == 0.0f) {
      vkr_harness_error_set(
          out_error, "camera.orbit", "$.camera",
          "Orbit radius, duration, and revolutions must be nonzero");
      return false_v;
    }
    return true_v;
  }
  if (camera->key_count < 2u) {
    vkr_harness_error_set(out_error, "camera.keys", "$.camera.keys",
                          "Keyed cameras require at least two keys");
    return false_v;
  }
  for (uint32_t i = 0; i < camera->key_count; ++i) {
    if (camera->keys[i].pitch_degrees < -89.0f ||
        camera->keys[i].pitch_degrees > 89.0f ||
        (i > 0 &&
         camera->keys[i].time_seconds <= camera->keys[i - 1u].time_seconds)) {
      vkr_harness_error_set(
          out_error, "camera.keys", "$.camera.keys",
          "Camera key times must increase and pitch must be within [-89,89]");
      return false_v;
    }
  }
  if (camera->mode != VKR_HARNESS_CAMERA_FLYTHROUGH) {
    return true_v;
  }

  camera->fly_lookup_count = 0;
  camera->fly_total_length = 0.0f;
  VkrHarnessCameraPose previous = {0};
  vkr_harness_camera_evaluate_keys(camera, camera->keys[0].time_seconds,
                                   &previous);
  camera->fly_lookup_lengths[0] = 0.0f;
  camera->fly_lookup_parameters[0] = 0.0f;
  camera->fly_lookup_count = 1u;
  const uint32_t segments = camera->key_count - 1u;
  const uint32_t sample_count = segments * VKR_HARNESS_FLY_LOOKUP_SUBDIVISIONS;
  for (uint32_t sample = 1; sample <= sample_count; ++sample) {
    const float32_t parameter = (float32_t)sample / (float32_t)sample_count;
    const float64_t first = camera->keys[0].time_seconds;
    const float64_t last = camera->keys[camera->key_count - 1u].time_seconds;
    VkrHarnessCameraPose current = {0};
    vkr_harness_camera_evaluate_keys(camera, first + (last - first) * parameter,
                                     &current);
    camera->fly_total_length +=
        vec3_length(vec3_sub(current.position, previous.position));
    camera->fly_lookup_lengths[sample] = camera->fly_total_length;
    camera->fly_lookup_parameters[sample] = parameter;
    camera->fly_lookup_count++;
    previous = current;
  }
  if (camera->fly_total_length <= 0.0f) {
    vkr_harness_error_set(out_error, "camera.flythrough", "$.camera.keys",
                          "Flythrough path has zero length");
    return false_v;
  }
  return true_v;
}

bool8_t vkr_harness_camera_evaluate(const VkrHarnessCamera *camera,
                                    float64_t authored_time_seconds,
                                    VkrHarnessCameraPose *out_pose) {
  if (!camera || !out_pose) {
    return false_v;
  }
  authored_time_seconds *= vkr_harness_speed_multiplier(camera->speed);
  if (camera->mode == VKR_HARNESS_CAMERA_STATIC) {
    *out_pose = camera->static_pose;
    return true_v;
  }
  if (camera->mode == VKR_HARNESS_CAMERA_ORBIT) {
    float64_t normalized =
        authored_time_seconds / camera->orbit_duration_seconds;
    if (normalized < 0.0) {
      normalized = 0.0;
    } else if (normalized > 1.0) {
      normalized = 1.0;
    }
    const float32_t angle_degrees =
        camera->orbit_start_angle_degrees +
        (float32_t)normalized * camera->orbit_revolutions * 360.0f;
    const float32_t angle = angle_degrees * (float32_t)(M_PI / 180.0);
    out_pose->position =
        vec3_new(camera->orbit_center.x + cosf(angle) * camera->orbit_radius,
                 camera->orbit_center.y + camera->orbit_height,
                 camera->orbit_center.z + sinf(angle) * camera->orbit_radius);
    const Vec3 direction =
        vec3_normalize(vec3_sub(camera->orbit_center, out_pose->position));
    out_pose->yaw_degrees =
        atan2f(direction.z, direction.x) * (float32_t)(180.0 / M_PI);
    out_pose->pitch_degrees = asinf(direction.y) * (float32_t)(180.0 / M_PI);
    return true_v;
  }
  if (camera->key_count < 2u) {
    return false_v;
  }
  float64_t key_time = authored_time_seconds;
  if (camera->mode == VKR_HARNESS_CAMERA_FLYTHROUGH) {
    const float64_t duration =
        camera->keys[camera->key_count - 1u].time_seconds -
        camera->keys[0].time_seconds;
    float64_t normalized =
        duration > 0.0 ? authored_time_seconds / duration : 0.0;
    if (normalized < 0.0) {
      normalized = 0.0;
    } else if (normalized > 1.0) {
      normalized = 1.0;
    }
    const float32_t target = (float32_t)normalized * camera->fly_total_length;
    uint32_t hi = 0;
    while (hi + 1u < camera->fly_lookup_count &&
           camera->fly_lookup_lengths[hi] < target) {
      hi++;
    }
    const uint32_t lo = hi > 0 ? hi - 1u : 0u;
    const float32_t a = camera->fly_lookup_lengths[lo];
    const float32_t b = camera->fly_lookup_lengths[hi];
    const float32_t local = b > a ? (target - a) / (b - a) : 0.0f;
    const float32_t parameter =
        vkr_harness_lerp_f32(camera->fly_lookup_parameters[lo],
                             camera->fly_lookup_parameters[hi], local);
    key_time = camera->keys[0].time_seconds + duration * parameter;
  }
  vkr_harness_camera_evaluate_keys(camera, key_time, out_pose);
  return true_v;
}
