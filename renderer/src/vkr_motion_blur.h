#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"

#define VKR_MOTION_BLUR_DEFAULT_SHUTTER_ANGLE 180.0f
#define VKR_MOTION_BLUR_TILE_SIZE 16u
#define VKR_MOTION_BLUR_MAX_RADIUS_PIXELS 16.0f
#define VKR_MOTION_BLUR_SAMPLE_COUNT 32u

typedef struct VkrMotionBlurGpuParams {
  /* Full shutter fraction, near/far metres, reserved. Native preparation
   * scales x by current frame time / actual transform predecessor interval. */
  Vec4 motion;
  Vec4 depth_uv;
  uint32_t dimensions[4];
} VkrMotionBlurGpuParams;

_Static_assert(sizeof(VkrMotionBlurGpuParams) == 48u,
               "Motion blur parameter ABI must remain 48 bytes");

bool8_t vkr_motion_blur_controls_valid(float32_t shutter_angle,
                                       Mat4 projection);
VkrMotionBlurGpuParams
vkr_motion_blur_prepare(float32_t shutter_angle, Mat4 projection,
                        Vec2 jitter_pixels, uint32_t output_width,
                        uint32_t output_height, uint32_t depth_width,
                        uint32_t depth_height);
