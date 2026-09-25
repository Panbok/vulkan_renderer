#pragma once

#include "defines.h"
#include "math/vec.h"

/** Noise the renderer generates once at startup. Every texture tiles: the
 * base volume holds a precombined Perlin-Worley shape, the detail volume a
 * Worley erosion field and the weather map a coverage field. */
#define VKR_CLOUD_BASE_NOISE_SIZE 128u
#define VKR_CLOUD_DETAIL_NOISE_SIZE 32u
#define VKR_CLOUD_WEATHER_SIZE 256u

/** World periods of the noise textures in kilometres. The weather period is a
 * multiple of the other two, so a wind offset wraps at it without a seam. */
#define VKR_CLOUD_BASE_PERIOD_KM 8.0f
#define VKR_CLOUD_DETAIL_PERIOD_KM 1.0f
#define VKR_CLOUD_WEATHER_PERIOD_KM 32.0f
#define VKR_CLOUD_WIND_PERIOD_M (VKR_CLOUD_WEATHER_PERIOD_KM * 1000.0f)

/** Sun-projected transmittance map: 512 texels over 8 km around the point
 * where the camera's sun ray enters the layer. */
#define VKR_CLOUD_SHADOW_SIZE 512u
#define VKR_CLOUD_SHADOW_EXTENT_KM 8.0f

/** Primary rays stop this far from the camera; aerial perspective hides the
 * layer beyond it. */
#define VKR_CLOUD_MARCH_DISTANCE_KM 60.0f

/** Authorable ranges. Altitudes are metres above the planet surface; density
 * is extinction per metre at unit noise density. */
#define VKR_CLOUD_ALTITUDE_MAX_M 20000.0f
#define VKR_CLOUD_THICKNESS_MIN_M 100.0f
#define VKR_CLOUD_DENSITY_MAX 1.0f
#define VKR_CLOUD_WIND_MAX_MPS 200.0f

/** One volumetric layer lit by the atmosphere's sun and sky light. Clouds
 * require an enabled atmosphere and publish with its revision. `wind_mps`
 * moves the layer along world X and Z. */
typedef struct VkrCloudSettings {
  bool8_t enabled;
  float32_t base_altitude_m;
  float32_t top_altitude_m;
  float32_t coverage;
  float32_t density;
  Vec2 wind_mps;
} VkrCloudSettings;

/** Shared with shaders as the tail of `VkrSkyGpuParams`. Radii are kilometres
 * from the planet centre, positions are kilometres in world X and Z.
 * `layer`: base radius, top radius, extinction per kilometre, coverage.
 * `noise`: reciprocal base, detail and weather periods, 1 when clouds render.
 * `wind`: wind offset in [0, weather period), march distance, reserved.
 * `shadow`: map centre, reciprocal extent, extent. */
typedef struct VkrCloudGpuParams {
  Vec4 layer;
  Vec4 noise;
  Vec4 wind;
  Vec4 shadow;
} VkrCloudGpuParams;

_Static_assert(sizeof(VkrCloudGpuParams) == 64u,
               "Cloud parameter ABI size drift");

VkrCloudSettings vkr_cloud_settings_defaults(void);
bool8_t vkr_cloud_settings_valid(const VkrCloudSettings *settings);

/** Prepares enabled, validated settings. `camera_km` is the camera's world
 * position in kilometres with its altitude in `y`; `sun` points toward the sun
 * and `bottom_radius_km` is the planet radius. The shadow map centre snaps to
 * its texel grid so a moving camera does not shimmer the shadows. */
VkrCloudGpuParams vkr_cloud_prepare(const VkrCloudSettings *settings,
                                    float32_t bottom_radius_km, Vec3 camera_km,
                                    Vec3 sun, Vec2 wind_offset_m);

/** Identity of the traced layer for history reuse; zero when clouds do not
 * render. The per-frame wind offset and shadow placement are excluded, so
 * accumulation follows a drifting layer instead of resetting every frame. */
uint64_t vkr_cloud_history_signature(const VkrCloudGpuParams *params,
                                     float32_t km_per_unit);
