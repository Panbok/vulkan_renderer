#include "vkr_motion_blur.h"

#include <math.h>

bool8_t vkr_motion_blur_controls_valid(float32_t shutter_angle,
                                       Mat4 projection) {
  if (!isfinite(shutter_angle) || shutter_angle < 0.0f ||
      shutter_angle > 360.0f)
    return false_v;
  const float32_t near_distance = projection.m23 / projection.m22;
  const float32_t far_distance = projection.m23 / (projection.m22 + 1.0f);
  return projection.m32 == -1.0f && projection.m33 == 0.0f &&
         isfinite(near_distance) && near_distance > 0.0f &&
         isfinite(far_distance) && far_distance > near_distance;
}

VkrMotionBlurGpuParams
vkr_motion_blur_prepare(float32_t shutter_angle, Mat4 projection,
                        Vec2 jitter_pixels, uint32_t output_width,
                        uint32_t output_height, uint32_t depth_width,
                        uint32_t depth_height) {
  return (VkrMotionBlurGpuParams){
      .motion = {shutter_angle / 360.0f, projection.m23 / projection.m22,
                 projection.m23 / (projection.m22 + 1.0f), 0.0f},
      .depth_uv = {1.0f, 1.0f, jitter_pixels.x / depth_width,
                   jitter_pixels.y / depth_height},
      .dimensions = {output_width, output_height,
                     output_width / VKR_MOTION_BLUR_TILE_SIZE +
                         (output_width % VKR_MOTION_BLUR_TILE_SIZE != 0u),
                     output_height / VKR_MOTION_BLUR_TILE_SIZE +
                         (output_height % VKR_MOTION_BLUR_TILE_SIZE != 0u)},
  };
}
