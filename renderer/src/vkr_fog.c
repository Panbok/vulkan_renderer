#include "vkr_fog.h"

#include <math.h>

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
  return isfinite(settings->color.x) && settings->color.x >= 0.0f &&
         isfinite(settings->color.y) && settings->color.y >= 0.0f &&
         isfinite(settings->color.z) && settings->color.z >= 0.0f &&
         isfinite(settings->density) && settings->density >= 0.0f &&
         isfinite(settings->base_height) &&
         isfinite(settings->height_falloff) &&
         settings->height_falloff >= 0.0f && isfinite(settings->max_distance) &&
         settings->max_distance > 0.0f && isfinite(settings->sky_distance) &&
         settings->sky_distance > 0.0f &&
         settings->sky_distance <= settings->max_distance;
}

VkrFogGpuParams vkr_fog_prepare(const VkrFogSettings *settings) {
  if (!settings->enabled || settings->density == 0.0f)
    return (VkrFogGpuParams){0};
  return (VkrFogGpuParams){.color_density = {.x = settings->color.x,
                                             .y = settings->color.y,
                                             .z = settings->color.z,
                                             .w = settings->density},
                           .height_distance = {.x = settings->base_height,
                                               .y = settings->height_falloff,
                                               .z = settings->max_distance,
                                               .w = settings->sky_distance}};
}
