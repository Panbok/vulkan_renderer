#include "vkr_atmosphere.h"

#include <math.h>

VkrAtmosphereSettings vkr_atmosphere_settings_defaults(void) {
  return (VkrAtmosphereSettings){
      .sun_direction = {.x = 0.0f, .y = 0.70710678f, .z = -0.70710678f},
      .solar_irradiance = {.x = 1.474f, .y = 1.8504f, .z = 1.91198f},
      .ground_albedo = {.x = 0.3f, .y = 0.3f, .z = 0.3f},
      .sun_angular_diameter_degrees = 0.53f,
      .rayleigh_density_scale = 1.0f,
      .mie_density_scale = 1.0f,
      .ozone_density_scale = 1.0f,
      .mie_anisotropy = 0.8f,
  };
}

bool8_t vkr_atmosphere_settings_valid(const VkrAtmosphereSettings *settings) {
  if (settings->enabled > true_v)
    return false_v;
  if (!settings->enabled)
    return true_v;
  const Vec3 sun = settings->sun_direction;
  const Vec3 solar = settings->solar_irradiance;
  const Vec3 ground = settings->ground_albedo;
  return isfinite(sun.x) && isfinite(sun.y) && isfinite(sun.z) &&
         (sun.x != 0.0f || sun.y != 0.0f || sun.z != 0.0f) &&
         isfinite(solar.x) && solar.x >= 0.0f && isfinite(solar.y) &&
         solar.y >= 0.0f && isfinite(solar.z) && solar.z >= 0.0f &&
         isfinite(ground.x) && ground.x >= 0.0f && ground.x <= 1.0f &&
         isfinite(ground.y) && ground.y >= 0.0f && ground.y <= 1.0f &&
         isfinite(ground.z) && ground.z >= 0.0f && ground.z <= 1.0f &&
         isfinite(settings->observer_altitude_m) &&
         settings->observer_altitude_m >= 0.0f &&
         settings->observer_altitude_m <= 100000.0f &&
         isfinite(settings->sun_angular_diameter_degrees) &&
         settings->sun_angular_diameter_degrees > 0.0f &&
         settings->sun_angular_diameter_degrees <= 5.0f &&
         settings->sun_angular_diameter_degrees >= 1e-16f &&
         isfinite(settings->rayleigh_density_scale) &&
         settings->rayleigh_density_scale >= 0.0f &&
         settings->rayleigh_density_scale <= 100.0f &&
         isfinite(settings->mie_density_scale) &&
         settings->mie_density_scale >= 0.0f &&
         settings->mie_density_scale <= 100.0f &&
         isfinite(settings->ozone_density_scale) &&
         settings->ozone_density_scale >= 0.0f &&
         settings->ozone_density_scale <= 100.0f &&
         isfinite(settings->mie_anisotropy) &&
         settings->mie_anisotropy >= -0.95f &&
         settings->mie_anisotropy <= 0.95f;
}

VkrAtmosphereGpuParams
vkr_atmosphere_prepare(const VkrAtmosphereSettings *settings) {
  if (!settings->enabled)
    return (VkrAtmosphereGpuParams){0};
  const float64_t x = settings->sun_direction.x;
  const float64_t y = settings->sun_direction.y;
  const float64_t z = settings->sun_direction.z;
  const float64_t length = sqrt(x * x + y * y + z * z);
  const float64_t radius =
      settings->sun_angular_diameter_degrees * 3.14159265358979323846 / 360.0;
  const float32_t projected_solid_angle =
      (float32_t)(3.14159265358979323846 * sin(radius) * sin(radius));
  /* Normalize the solar calibration once so both direct light and the visual
     disc fit scene-linear RGBA16F. Atmospheric in-scatter retains headroom. */
  const float32_t irradiance_limit = 60000.0f * projected_solid_angle;
  const float32_t peak =
      Max(settings->solar_irradiance.x,
          Max(settings->solar_irradiance.y, settings->solar_irradiance.z));
  const float32_t calibration =
      peak > irradiance_limit ? irradiance_limit / peak : 1.0f;
  return (VkrAtmosphereGpuParams){
      .planet = {6360.0f, 6460.0f, settings->observer_altitude_m * 0.001f,
                 settings->mie_anisotropy},
      .rayleigh = {0.005802f * settings->rayleigh_density_scale,
                   0.013558f * settings->rayleigh_density_scale,
                   0.033100f * settings->rayleigh_density_scale, 8.0f},
      .mie_scattering = {0.003996f * settings->mie_density_scale,
                         0.003996f * settings->mie_density_scale,
                         0.003996f * settings->mie_density_scale, 1.2f},
      .mie_extinction = {0.004440f * settings->mie_density_scale,
                         0.004440f * settings->mie_density_scale,
                         0.004440f * settings->mie_density_scale, 0.0f},
      .ozone = {0.000650f * settings->ozone_density_scale,
                0.001881f * settings->ozone_density_scale,
                0.000085f * settings->ozone_density_scale, 25.0f},
      .ground = {settings->ground_albedo.x, settings->ground_albedo.y,
                 settings->ground_albedo.z, 15.0f},
      .sun = {(float32_t)(x / length), (float32_t)(y / length),
              (float32_t)(z / length), (float32_t)cos(radius)},
      .solar = {settings->solar_irradiance.x * calibration,
                settings->solar_irradiance.y * calibration,
                settings->solar_irradiance.z * calibration,
                projected_solid_angle},
  };
}
