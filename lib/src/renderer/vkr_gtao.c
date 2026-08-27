#include "renderer/vkr_gtao.h"

#include <math.h>

#define VKR_GTAO_RADIUS_MULTIPLIER_MIN 0.3f
#define VKR_GTAO_RADIUS_MULTIPLIER_MAX 3.0f
#define VKR_GTAO_FALLOFF_RANGE_MIN 0.01f
#define VKR_GTAO_SAMPLE_DISTRIBUTION_MIN 1.0f
#define VKR_GTAO_SAMPLE_DISTRIBUTION_MAX 3.0f
#define VKR_GTAO_DEPTH_MIP_OFFSET_MAX 30.0f
#define VKR_GTAO_DENOISE_BETA_MIN 0.01f
#define VKR_GTAO_DENOISE_BETA_MAX 10000.0f
#define VKR_GTAO_DEPTH_FILTER_RADIUS_SCALE 0.75f

static float32_t gtao_clamp(float32_t value, float32_t minimum,
                            float32_t maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static float32_t gtao_normalize_float(float32_t value, float32_t fallback,
                                      float32_t minimum, float32_t maximum) {
  return gtao_clamp(isfinite(value) ? value : fallback, minimum, maximum);
}

VkrGtaoConfig vkr_gtao_config_default(void) {
  return (VkrGtaoConfig){
      .max_depth_mip_count = VKR_GTAO_MAX_DEPTH_MIP_COUNT,
      .slice_count = 3u,
      .steps_per_slice = 3u,
      .radius_multiplier = 1.457f,
      .falloff_range = 0.615f,
      .sample_distribution_power = 2.0f,
      .depth_mip_sampling_offset = 3.30f,
      .denoise_blur_beta = 1.2f,
  };
}

VkrGtaoConfig vkr_gtao_config_normalize(const VkrGtaoConfig *config) {
  const VkrGtaoConfig defaults = vkr_gtao_config_default();
  if (!config || config->max_depth_mip_count == 0u)
    return defaults;

  VkrGtaoConfig result = defaults;
  result.max_depth_mip_count =
      config->max_depth_mip_count > VKR_GTAO_MAX_DEPTH_MIP_COUNT
          ? VKR_GTAO_MAX_DEPTH_MIP_COUNT
          : config->max_depth_mip_count;
  result.slice_count =
      config->slice_count < 1u
          ? 1u
          : (config->slice_count > 3u ? 3u : config->slice_count);
  result.steps_per_slice =
      config->steps_per_slice < 1u
          ? 1u
          : (config->steps_per_slice > 3u ? 3u : config->steps_per_slice);
  result.radius_multiplier = gtao_normalize_float(
      config->radius_multiplier, defaults.radius_multiplier,
      VKR_GTAO_RADIUS_MULTIPLIER_MIN, VKR_GTAO_RADIUS_MULTIPLIER_MAX);
  result.falloff_range =
      gtao_normalize_float(config->falloff_range, defaults.falloff_range,
                           VKR_GTAO_FALLOFF_RANGE_MIN, 1.0f);
  result.sample_distribution_power = gtao_normalize_float(
      config->sample_distribution_power, defaults.sample_distribution_power,
      VKR_GTAO_SAMPLE_DISTRIBUTION_MIN, VKR_GTAO_SAMPLE_DISTRIBUTION_MAX);
  result.depth_mip_sampling_offset = gtao_normalize_float(
      config->depth_mip_sampling_offset, defaults.depth_mip_sampling_offset,
      0.0f, VKR_GTAO_DEPTH_MIP_OFFSET_MAX);
  result.denoise_blur_beta = gtao_normalize_float(
      config->denoise_blur_beta, defaults.denoise_blur_beta,
      VKR_GTAO_DENOISE_BETA_MIN, VKR_GTAO_DENOISE_BETA_MAX);
  return result;
}

uint32_t vkr_gtao_depth_mip_count(const VkrGtaoConfig *config, uint32_t width,
                                  uint32_t height) {
  if (width == 0u || height == 0u)
    return 0u;

  uint32_t max_extent = width > height ? width : height;
  uint32_t count = 0u;
  while (max_extent != 0u && count < config->max_depth_mip_count) {
    ++count;
    max_extent >>= 1u;
  }
  return count;
}

VkrGtaoFrame vkr_gtao_prepare(bool8_t enabled, float32_t radius,
                              float32_t power) {
  if (!enabled)
    return (VkrGtaoFrame){0};
  return (VkrGtaoFrame){.enabled = enabled, .radius = radius, .power = power};
}

VkrGtaoGpuParams vkr_gtao_gpu_params(const VkrGtaoConfig *config,
                                     const VkrGtaoFrame *frame, Mat4 view,
                                     Mat4 projection, uint32_t width,
                                     uint32_t height, uint32_t frame_index,
                                     bool8_t temporal_enabled) {
  const float32_t pixel_size_x = 1.0f / (float32_t)width;
  const float32_t pixel_size_y = 1.0f / (float32_t)height;
  const float32_t effective_radius = frame->radius * config->radius_multiplier;
  const float32_t inverse_falloff_distance =
      frame->enabled ? 1.0f / (effective_radius * config->falloff_range) : 0.0f;

  return (VkrGtaoGpuParams){
      .view = view,
      .viewport_width = width,
      .viewport_height = height,
      .depth_mip_count = vkr_gtao_depth_mip_count(config, width, height),
      .reserved_u32_0 = 0u,
      .viewport_pixel_size_x = pixel_size_x,
      .viewport_pixel_size_y = pixel_size_y,
      .projection_m22 = projection.m22,
      .projection_m23 = projection.m23,
      .projection_m32 = projection.m32,
      .projection_m33 = projection.m33,
      .projection_m00 = projection.m00,
      .projection_m11 = projection.m11,
      .projection_m02 = projection.m02,
      .projection_m12 = projection.m12,
      .projection_m03 = projection.m03,
      .projection_m13 = projection.m13,
      .effect_radius = frame->radius,
      .radius_multiplier = config->radius_multiplier,
      .falloff_range = config->falloff_range,
      .falloff_mul = -inverse_falloff_distance,
      .falloff_add = frame->enabled ? 1.0f / config->falloff_range : 0.0f,
      .depth_mip_falloff_mul =
          -inverse_falloff_distance / VKR_GTAO_DEPTH_FILTER_RADIUS_SCALE,
      .sample_distribution_power = config->sample_distribution_power,
      .final_value_power = frame->power,
      .depth_mip_sampling_offset = config->depth_mip_sampling_offset,
      .denoise_blur_beta = config->denoise_blur_beta,
      .slice_count = config->slice_count,
      .steps_per_slice = config->steps_per_slice,
      .noise_index = temporal_enabled ? frame_index % 64u : 0u,
  };
}

float32_t vkr_gtao_linearize_depth(const VkrGtaoGpuParams *params,
                                   float32_t device_depth) {
  const float32_t numerator =
      device_depth * params->projection_m33 - params->projection_m23;
  const float32_t denominator =
      device_depth * params->projection_m32 - params->projection_m22;
  return fminf(numerator / denominator, VKR_GTAO_VIEW_DEPTH_MAX);
}

Vec3 vkr_gtao_view_position(const VkrGtaoGpuParams *params, float32_t screen_x,
                            float32_t screen_y, float32_t view_depth) {
  const float32_t view_z = -view_depth;
  const float32_t clip_w =
      params->projection_m32 * view_z + params->projection_m33;
  const float32_t ndc_x = screen_x * 2.0f - 1.0f;
  const float32_t ndc_y = screen_y * 2.0f - 1.0f;
  return vec3_new((ndc_x * clip_w - params->projection_m02 * view_z -
                   params->projection_m03) /
                      params->projection_m00,
                  (ndc_y * clip_w - params->projection_m12 * view_z -
                   params->projection_m13) /
                      params->projection_m11,
                  view_z);
}

float32_t vkr_gtao_depth_mip_filter(const VkrGtaoGpuParams *params,
                                    float32_t d0, float32_t d1, float32_t d2,
                                    float32_t d3) {
  const float32_t farthest = fmaxf(fmaxf(d0, d1), fmaxf(d2, d3));
  const float32_t w0 = gtao_clamp(
      (farthest - d0) * params->depth_mip_falloff_mul + params->falloff_add,
      0.0f, 1.0f);
  const float32_t w1 = gtao_clamp(
      (farthest - d1) * params->depth_mip_falloff_mul + params->falloff_add,
      0.0f, 1.0f);
  const float32_t w2 = gtao_clamp(
      (farthest - d2) * params->depth_mip_falloff_mul + params->falloff_add,
      0.0f, 1.0f);
  const float32_t w3 = gtao_clamp(
      (farthest - d3) * params->depth_mip_falloff_mul + params->falloff_add,
      0.0f, 1.0f);
  const float32_t weight_sum = w0 + w1 + w2 + w3;
  return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / weight_sum;
}
