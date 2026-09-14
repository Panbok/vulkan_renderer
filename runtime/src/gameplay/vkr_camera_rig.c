#include "gameplay/vkr_camera_rig.h"

#include <math.h>

static bool8_t camera_rig_config_valid(const VkrCameraRigConfig *config) {
  return config && isfinite(config->eye_height) && config->eye_height >= 0.0f &&
         isfinite(config->distance) && config->distance >= 0.0f &&
         isfinite(config->shoulder_offset) && config->shoulder_offset >= 0.0f &&
         isfinite(config->shoulder_height) && isfinite(config->sweep_radius) &&
         config->sweep_radius > 0.0f && isfinite(config->pitch_limit) &&
         config->pitch_limit > 0.0f && config->pitch_limit < 1.5707963267948966;
}

static bool8_t camera_rig_mode_valid(VkrCameraRigMode mode) {
  return mode == VKR_CAMERA_RIG_FIRST_PERSON ||
         mode == VKR_CAMERA_RIG_THIRD_PERSON || mode == VKR_CAMERA_RIG_SHOULDER;
}

static bool8_t camera_rig_vector_finite(Vec3 vector) {
  return isfinite(vector.x) && isfinite(vector.y) && isfinite(vector.z);
}

bool8_t vkr_camera_rig_initialize(VkrCameraRig *rig,
                                  const VkrCameraRigConfig *config) {
  if (!rig || !camera_rig_config_valid(config)) {
    return false_v;
  }
  *rig = (VkrCameraRig){.config = *config, .mode = VKR_CAMERA_RIG_FIRST_PERSON};
  return true_v;
}

bool8_t vkr_camera_rig_set_mode(VkrCameraRig *rig, VkrCameraRigMode mode,
                                bool8_t shoulder_left) {
  if (!rig || !camera_rig_config_valid(&rig->config) ||
      !camera_rig_mode_valid(mode)) {
    return false_v;
  }
  rig->mode = mode;
  rig->shoulder_left = shoulder_left ? true_v : false_v;
  return true_v;
}

bool8_t vkr_camera_rig_evaluate(const VkrCameraRig *rig, Vec3 target_foot,
                                float32_t yaw, float32_t pitch,
                                VkrCameraRigSweep sweep, void *context,
                                VkrCameraRigPose *pose) {
  if (!rig || !pose || !camera_rig_config_valid(&rig->config) ||
      !camera_rig_mode_valid(rig->mode) ||
      !camera_rig_vector_finite(target_foot) || !isfinite(yaw) ||
      !isfinite(pitch)) {
    return false_v;
  }
  const VkrCameraRigConfig *config = &rig->config;
  const float32_t clamped_pitch =
      Max(-config->pitch_limit, Min(config->pitch_limit, pitch));
  const float32_t yaw_cos = cosf(yaw);
  const float32_t yaw_sin = sinf(yaw);
  const float32_t pitch_cos = cosf(clamped_pitch);
  const float32_t pitch_sin = sinf(clamped_pitch);
  const Vec3 forward =
      vec3_new(yaw_cos * pitch_cos, pitch_sin, yaw_sin * pitch_cos);
  const Vec3 right = vec3_new(-yaw_sin, 0.0f, yaw_cos);
  const Vec3 up = vec3_cross(right, forward);
  const Vec3 eye =
      vec3_add(target_foot, vec3_new(0.0f, config->eye_height, 0.0f));
  Vec3 displacement = vec3_zero();
  if (rig->mode != VKR_CAMERA_RIG_FIRST_PERSON) {
    displacement = vec3_scale(forward, -config->distance);
    if (rig->mode == VKR_CAMERA_RIG_SHOULDER) {
      const float32_t side = rig->shoulder_left ? -config->shoulder_offset
                                                : config->shoulder_offset;
      displacement = vec3_add(displacement, vec3_scale(right, side));
      displacement.y += config->shoulder_height;
    }
  }
  Vec3 position = vec3_add(eye, displacement);
  if (!camera_rig_vector_finite(eye) ||
      !camera_rig_vector_finite(displacement) ||
      !camera_rig_vector_finite(position)) {
    return false_v;
  }
  if (rig->mode != VKR_CAMERA_RIG_FIRST_PERSON && sweep &&
      (displacement.x != 0.0f || displacement.y != 0.0f ||
       displacement.z != 0.0f)) {
    float32_t fraction = 1.0f;
    if (!sweep(eye, displacement, config->sweep_radius, context, &fraction) ||
        !isfinite(fraction) || fraction < 0.0f || fraction > 1.0f) {
      return false_v;
    }
    position = vec3_add(eye, vec3_scale(displacement, fraction));
    if (!camera_rig_vector_finite(position)) {
      return false_v;
    }
  }
  *pose = (VkrCameraRigPose){.position = position,
                             .forward = forward,
                             .up = up,
                             .pitch = clamped_pitch};
  return true_v;
}
