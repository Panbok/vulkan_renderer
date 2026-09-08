#include "vkr_ssr.h"

#include <math.h>

#define VKR_SSR_THICKNESS_MIN 0.0001f
#define VKR_SSR_THICKNESS_MAX 100.0f
#define VKR_SSR_MAX_DISTANCE_MIN 0.001f
#define VKR_SSR_MAX_DISTANCE_MAX 100000.0f
#define VKR_SSR_EDGE_FADE_MIN 0.0f
#define VKR_SSR_EDGE_FADE_MAX 4096.0f
#define VKR_SSR_TEMPORAL_DEPTH_RELATIVE_MAX 1.0f
#define VKR_SSR_TEMPORAL_DEPTH_ABSOLUTE_MAX 1000.0f

static float32_t ssr_clamp(float32_t value, float32_t minimum,
                           float32_t maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static float32_t ssr_normalize_float(float32_t value, float32_t fallback,
                                     float32_t minimum, float32_t maximum) {
  return ssr_clamp(isfinite(value) ? value : fallback, minimum, maximum);
}

VkrSsrConfig vkr_ssr_config_default(void) {
  return (VkrSsrConfig){
      .max_steps = VKR_SSR_MAX_STEPS,
      .thickness = VKR_SSR_DEFAULT_THICKNESS,
      .max_distance = VKR_SSR_DEFAULT_MAX_DISTANCE,
      .roughness_cutoff = VKR_SSR_DEFAULT_ROUGHNESS_CUTOFF,
      .edge_fade_pixels = VKR_SSR_DEFAULT_EDGE_FADE_PIXELS,
      .temporal_weight = VKR_SSR_DEFAULT_TEMPORAL_WEIGHT,
      .temporal_depth_relative = VKR_SSR_DEFAULT_TEMPORAL_DEPTH_RELATIVE,
      .temporal_depth_absolute = VKR_SSR_DEFAULT_TEMPORAL_DEPTH_ABSOLUTE,
  };
}

VkrSsrConfig vkr_ssr_config_normalize(const VkrSsrConfig *config) {
  const VkrSsrConfig defaults = vkr_ssr_config_default();
  if (!config)
    return defaults;

  VkrSsrConfig result = defaults;
  result.max_steps =
      config->max_steps < 1u
          ? 1u
          : (config->max_steps > VKR_SSR_MAX_STEPS ? VKR_SSR_MAX_STEPS
                                                   : config->max_steps);
  result.thickness =
      ssr_normalize_float(config->thickness, defaults.thickness,
                          VKR_SSR_THICKNESS_MIN, VKR_SSR_THICKNESS_MAX);
  result.max_distance =
      ssr_normalize_float(config->max_distance, defaults.max_distance,
                          VKR_SSR_MAX_DISTANCE_MIN, VKR_SSR_MAX_DISTANCE_MAX);
  result.roughness_cutoff =
      ssr_normalize_float(config->roughness_cutoff, defaults.roughness_cutoff,
                          0.0f, VKR_SSR_DEFAULT_ROUGHNESS_CUTOFF);
  result.edge_fade_pixels =
      ssr_normalize_float(config->edge_fade_pixels, defaults.edge_fade_pixels,
                          VKR_SSR_EDGE_FADE_MIN, VKR_SSR_EDGE_FADE_MAX);
  result.temporal_weight = ssr_normalize_float(
      config->temporal_weight, defaults.temporal_weight, 0.0f, 1.0f);
  result.temporal_depth_relative = ssr_normalize_float(
      config->temporal_depth_relative, defaults.temporal_depth_relative, 0.0f,
      VKR_SSR_TEMPORAL_DEPTH_RELATIVE_MAX);
  result.temporal_depth_absolute = ssr_normalize_float(
      config->temporal_depth_absolute, defaults.temporal_depth_absolute, 0.0f,
      VKR_SSR_TEMPORAL_DEPTH_ABSOLUTE_MAX);
  return result;
}

uint32_t vkr_ssr_reduced_extent(uint32_t extent) {
  return extent > 1u ? extent / 2u : extent;
}

uint32_t vkr_ssr_depth_mip_count(uint32_t source_width,
                                 uint32_t source_height) {
  if (source_width == 0u || source_height == 0u)
    return 0u;

  uint32_t width = vkr_ssr_reduced_extent(source_width);
  uint32_t height = vkr_ssr_reduced_extent(source_height);
  uint32_t count = 1u;
  while ((width > 1u || height > 1u) && count < VKR_SSR_MAX_DEPTH_MIP_COUNT) {
    width = vkr_ssr_reduced_extent(width);
    height = vkr_ssr_reduced_extent(height);
    ++count;
  }
  return count;
}

VkrSsrGpuParams vkr_ssr_gpu_params(const VkrSsrConfig *config, Mat4 projection,
                                   Mat4 inverse_projection, Mat4 view,
                                   Mat4 previous_projection,
                                   uint32_t source_width,
                                   uint32_t source_height,
                                   bool8_t history_valid) {
  if (source_width == 0u || source_height == 0u)
    return (VkrSsrGpuParams){0};

  const VkrSsrConfig prepared = config ? *config : vkr_ssr_config_default();
  const uint32_t trace_width = vkr_ssr_reduced_extent(source_width);
  const uint32_t trace_height = vkr_ssr_reduced_extent(source_height);
  return (VkrSsrGpuParams){
      .projection = projection,
      .inverse_projection = inverse_projection,
      .view = view,
      .source_width = source_width,
      .source_height = source_height,
      .trace_width = trace_width,
      .trace_height = trace_height,
      .depth_mip_count = vkr_ssr_depth_mip_count(source_width, source_height),
      .max_steps = prepared.max_steps,
      .history_valid = history_valid ? 1u : 0u,
      .thickness = prepared.thickness,
      .max_distance = prepared.max_distance,
      .roughness_cutoff = prepared.roughness_cutoff,
      .edge_fade_pixels = prepared.edge_fade_pixels,
      .temporal_weight = prepared.temporal_weight,
      .temporal_depth_relative = prepared.temporal_depth_relative,
      .temporal_depth_absolute = prepared.temporal_depth_absolute,
      .previous_projection_m22 = previous_projection.m22,
      .previous_projection_m23 = previous_projection.m23,
      .previous_projection_m32 = previous_projection.m32,
      .previous_projection_m33 = previous_projection.m33,
      .trace_texel_size_x = 1.0f / (float32_t)trace_width,
      .trace_texel_size_y = 1.0f / (float32_t)trace_height,
      .trace_extent_x = (float32_t)trace_width,
      .trace_extent_y = (float32_t)trace_height,
  };
}
