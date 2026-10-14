#include "vkr_fog.h"

VkrFogSettings vkr_fog_settings_defaults(void) {
  return (VkrFogSettings){.color = {.x = 0.5f, .y = 0.6f, .z = 0.7f},
                          .density = 0.01f,
                          .height_falloff = 0.1f,
                          .max_distance = 10000.0f,
                          .sky_distance = 10000.0f};
}

bool8_t vkr_fog_settings_valid(const VkrFogSettings *settings) {
  if (settings->enabled > true_v)
    return false_v;
  if (!settings->enabled)
    return true_v;
  return vkr_is_finite_f32(settings->color.x) && settings->color.x >= 0.0f &&
         vkr_is_finite_f32(settings->color.y) && settings->color.y >= 0.0f &&
         vkr_is_finite_f32(settings->color.z) && settings->color.z >= 0.0f &&
         vkr_is_finite_f32(settings->density) && settings->density >= 0.0f &&
         vkr_is_finite_f32(settings->base_height) &&
         vkr_is_finite_f32(settings->height_falloff) &&
         settings->height_falloff >= 0.0f &&
         vkr_is_finite_f32(settings->max_distance) &&
         settings->max_distance > 0.0f &&
         vkr_is_finite_f32(settings->sky_distance) &&
         settings->sky_distance > 0.0f &&
         settings->sky_distance <= settings->max_distance &&
         settings->sky_lighting >= 0.0f && settings->sky_lighting <= 1.0f &&
         settings->anisotropy >= -0.95f && settings->anisotropy <= 0.95f;
}

VkrFogGpuParams vkr_fog_pre_exposed(const VkrFogGpuParams *params,
                                    float32_t pre_exposure) {
  VkrFogGpuParams scaled = *params;
  scaled.color_density.x *= pre_exposure;
  scaled.color_density.y *= pre_exposure;
  scaled.color_density.z *= pre_exposure;
  return scaled;
}

VkrFogGpuParams vkr_fog_prepare(const VkrFogSettings *settings) {
  if (!settings->enabled || settings->density == 0.0f)
    return (VkrFogGpuParams){0};
  return (VkrFogGpuParams){
      .color_density = {.x = settings->color.x,
                        .y = settings->color.y,
                        .z = settings->color.z,
                        .w = settings->density},
      .height_distance = {.x = settings->base_height,
                          .y = settings->height_falloff,
                          .z = settings->max_distance,
                          .w = settings->sky_distance},
      .sky_lighting = {.x = settings->sky_lighting, .y = settings->anisotropy}};
}
