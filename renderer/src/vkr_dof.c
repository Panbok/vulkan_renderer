#include "vkr_dof.h"

#include <float.h>
#include <math.h>

bool8_t vkr_dof_controls_valid(float32_t focus_distance, float32_t f_stop,
                               Mat4 projection) {
  const float64_t focal =
      0.5 * VKR_DOF_SENSOR_HEIGHT_METRES * fabs((float64_t)projection.m11);
  const float32_t near_distance = projection.m23 / projection.m22;
  const float32_t far_distance = projection.m23 / (projection.m22 + 1.0f);
  if (!isfinite(focus_distance) || !isfinite(f_stop) || f_stop <= 0.0f ||
      !isfinite(focal) || focal <= 0.0 || focus_distance <= focal ||
      projection.m32 != -1.0f || projection.m33 != 0.0f ||
      !isfinite(near_distance) || near_distance <= 0.0f ||
      !isfinite(far_distance) || far_distance <= near_distance)
    return false_v;
  const float64_t radius_per_pixel =
      focal * focal /
      (2.0 * f_stop * VKR_DOF_SENSOR_HEIGHT_METRES * (focus_distance - focal));
  return isfinite(radius_per_pixel) &&
         radius_per_pixel <= (float64_t)FLT_MAX / UINT32_MAX;
}

VkrDofGpuParams vkr_dof_prepare(float32_t focus_distance, float32_t f_stop,
                                Mat4 projection, Vec2 jitter_pixels,
                                uint32_t output_width, uint32_t output_height,
                                uint32_t depth_width, uint32_t depth_height) {
  const float64_t focal =
      0.5 * VKR_DOF_SENSOR_HEIGHT_METRES * fabs((float64_t)projection.m11);
  const float32_t radius_scale =
      (float32_t)(output_height * focal * focal /
                  (2.0 * f_stop * VKR_DOF_SENSOR_HEIGHT_METRES *
                   (focus_distance - focal)));
  return (VkrDofGpuParams){
      .lens = {radius_scale, focus_distance, projection.m23 / projection.m22,
               projection.m23 / (projection.m22 + 1.0f)},
      .depth_uv = {1.0f, 1.0f, jitter_pixels.x / depth_width,
                   jitter_pixels.y / depth_height},
      .dimensions = {output_width, output_height, Max(output_width / 2u, 1u),
                     Max(output_height / 2u, 1u)},
  };
}
