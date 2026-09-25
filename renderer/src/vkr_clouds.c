#include "vkr_clouds.h"

#include <math.h>

/* Near the horizon the sun-projected shadow offset grows without bound. The
   projection holds the sun at this elevation sine instead. */
#define VKR_CLOUD_SHADOW_SUN_Y_MIN 0.1f

VkrCloudSettings vkr_cloud_settings_defaults(void) {
  return (VkrCloudSettings){.base_altitude_m = 1500.0f,
                            .top_altitude_m = 4000.0f,
                            .coverage = 0.5f,
                            .density = 0.02f,
                            .wind_mps = {.x = 10.0f, .y = 0.0f}};
}

bool8_t vkr_cloud_settings_valid(const VkrCloudSettings *settings) {
  if (settings->enabled > true_v)
    return false_v;
  if (!settings->enabled)
    return true_v;
  return isfinite(settings->base_altitude_m) &&
         settings->base_altitude_m >= 0.0f &&
         isfinite(settings->top_altitude_m) &&
         settings->top_altitude_m <= VKR_CLOUD_ALTITUDE_MAX_M &&
         settings->top_altitude_m - settings->base_altitude_m >=
             VKR_CLOUD_THICKNESS_MIN_M &&
         settings->coverage >= 0.0f && settings->coverage <= 1.0f &&
         settings->density >= 0.0f &&
         settings->density <= VKR_CLOUD_DENSITY_MAX &&
         isfinite(settings->wind_mps.x) && isfinite(settings->wind_mps.y) &&
         fabsf(settings->wind_mps.x) <= VKR_CLOUD_WIND_MAX_MPS &&
         fabsf(settings->wind_mps.y) <= VKR_CLOUD_WIND_MAX_MPS;
}

vkr_internal uint64_t vkr_cloud_hash_bytes(uint64_t hash, const void *data,
                                           uint64_t size) {
  const uint8_t *bytes = data;
  for (uint64_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

vkr_internal float32_t vkr_cloud_wrap_km(float32_t offset_m) {
  const float32_t wrapped = fmodf(offset_m, VKR_CLOUD_WIND_PERIOD_M);
  return (wrapped < 0.0f ? wrapped + VKR_CLOUD_WIND_PERIOD_M : wrapped) *
         0.001f;
}

VkrCloudGpuParams vkr_cloud_prepare(const VkrCloudSettings *settings,
                                    float32_t bottom_radius_km, Vec3 camera_km,
                                    Vec3 sun, Vec2 wind_offset_m) {
  const float32_t base_km = settings->base_altitude_m * 0.001f;
  const float32_t top_km = settings->top_altitude_m * 0.001f;

  /* Centre the shadow map where the camera's sun ray enters the layer, so
     nearby surfaces project near its centre. */
  const float32_t sun_y = Max(sun.y, VKR_CLOUD_SHADOW_SUN_Y_MIN);
  const float32_t reach = Max(base_km - camera_km.y, 0.0f) / sun_y;
  const float32_t texel_km =
      VKR_CLOUD_SHADOW_EXTENT_KM / (float32_t)VKR_CLOUD_SHADOW_SIZE;
  const float32_t centre_x =
      floorf((camera_km.x + sun.x * reach) / texel_km + 0.5f) * texel_km;
  const float32_t centre_z =
      floorf((camera_km.z + sun.z * reach) / texel_km + 0.5f) * texel_km;

  return (VkrCloudGpuParams){
      .layer = {.x = bottom_radius_km + base_km,
                .y = bottom_radius_km + top_km,
                .z = settings->density * 1000.0f,
                .w = settings->coverage},
      .noise = {.x = 1.0f / VKR_CLOUD_BASE_PERIOD_KM,
                .y = 1.0f / VKR_CLOUD_DETAIL_PERIOD_KM,
                .z = 1.0f / VKR_CLOUD_WEATHER_PERIOD_KM,
                .w = 1.0f},
      .wind = {.x = vkr_cloud_wrap_km(wind_offset_m.x),
               .y = vkr_cloud_wrap_km(wind_offset_m.y),
               .z = VKR_CLOUD_MARCH_DISTANCE_KM},
      .shadow = {.x = centre_x,
                 .y = centre_z,
                 .z = 1.0f / VKR_CLOUD_SHADOW_EXTENT_KM,
                 .w = VKR_CLOUD_SHADOW_EXTENT_KM},
  };
}

uint64_t vkr_cloud_history_signature(const VkrCloudGpuParams *params,
                                     float32_t km_per_unit) {
  if (params->noise.w <= 0.0f)
    return 0u;
  uint64_t hash = UINT64_C(1469598103934665603);
  hash = vkr_cloud_hash_bytes(hash, &params->layer, sizeof(params->layer));
  hash = vkr_cloud_hash_bytes(hash, &params->noise, sizeof(params->noise));
  hash = vkr_cloud_hash_bytes(hash, &params->wind.z, sizeof(params->wind.z));
  return vkr_cloud_hash_bytes(hash, &km_per_unit, sizeof(km_per_unit));
}
