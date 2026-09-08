#pragma once

#include "defines.h"
#include "math/vec.h"

#define VKR_ATMOSPHERE_TRANSMITTANCE_WIDTH 256u
#define VKR_ATMOSPHERE_TRANSMITTANCE_HEIGHT 64u
#define VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE 32u
#define VKR_ATMOSPHERE_SOURCE_SIZE 256u

/** Earth atmosphere baked at a fixed observer altitude, in metres.
 * Density multipliers are in [0,100], solar diameter in [1e-16,5] degrees,
 * altitude in [0,100000] metres, and Mie anisotropy in [-.95,.95]. */
typedef struct VkrAtmosphereSettings {
  bool8_t enabled;
  Vec3 sun_direction;
  Vec3 solar_irradiance;
  Vec3 ground_albedo;
  float32_t observer_altitude_m;
  float32_t sun_angular_diameter_degrees;
  float32_t rayleigh_density_scale;
  float32_t mie_density_scale;
  float32_t ozone_density_scale;
  float32_t mie_anisotropy;
} VkrAtmosphereSettings;

/** Shared bake constants. Lengths are kilometres; extinction is per kilometre.
 */
typedef struct VkrAtmosphereGpuParams {
  Vec4 planet;
  Vec4 rayleigh;
  Vec4 mie_scattering;
  Vec4 mie_extinction;
  Vec4 ozone;
  Vec4 ground;
  Vec4 sun;
  Vec4 solar;
} VkrAtmosphereGpuParams;

_Static_assert(sizeof(VkrAtmosphereGpuParams) == 128u,
               "Atmosphere parameter ABI size drift");

typedef enum VkrAtmosphereBakeStatus {
  VKR_ATMOSPHERE_BAKE_PENDING = 0,
  VKR_ATMOSPHERE_BAKE_READY,
  VKR_ATMOSPHERE_BAKE_FAILED,
} VkrAtmosphereBakeStatus;

/** Valid only when the complete candidate environment is GPU-complete. */
typedef struct VkrAtmosphereBakeResult {
  Vec3 solar_irradiance;
  Vec3 solar_disk_radiance;
} VkrAtmosphereBakeResult;

VkrAtmosphereSettings vkr_atmosphere_settings_defaults(void);
bool8_t vkr_atmosphere_settings_valid(const VkrAtmosphereSettings *settings);
VkrAtmosphereGpuParams
vkr_atmosphere_prepare(const VkrAtmosphereSettings *settings);
