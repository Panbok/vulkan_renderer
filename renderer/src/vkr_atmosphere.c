#include "vkr_atmosphere.h"

#include <math.h>

VkrAtmosphereSettings vkr_atmosphere_settings_defaults(void) {
  return (VkrAtmosphereSettings){
      .sun_direction = {.x = 0.0f, .y = 0.70710678f, .z = -0.70710678f},
      .solar_irradiance = {.x = 1.474f, .y = 1.8504f, .z = 1.91198f},
      .ground_albedo = {.x = 0.3f, .y = 0.3f, .z = 0.3f},
      .sun_angular_diameter_degrees = 0.53f,
      .sun_glow = VKR_ATMOSPHERE_SUN_GLOW_DEFAULT,
      .rayleigh_density_scale = 1.0f,
      .mie_density_scale = 1.0f,
      .ozone_density_scale = 1.0f,
      .mie_anisotropy = 0.8f,
      .metres_per_world_unit = 1.0f,
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
         isfinite(settings->sun_glow) && settings->sun_glow >= 0.0f &&
         settings->sun_glow <= VKR_ATMOSPHERE_SUN_GLOW_MAX &&
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
         settings->mie_anisotropy <= 0.95f &&
         isfinite(settings->metres_per_world_unit) &&
         settings->metres_per_world_unit >=
             VKR_ATMOSPHERE_METRES_PER_UNIT_MIN &&
         settings->metres_per_world_unit <= VKR_ATMOSPHERE_METRES_PER_UNIT_MAX;
}

vkr_internal float32_t vkr_atmosphere_luminance(Vec3 rgb) {
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

/* One asymmetric lobe of Wyman, Sloan and Shirley's multi-lobe fit to the CIE
   1931 2-degree colour matching functions (JCGT 2(2), 2013). */
vkr_internal float64_t vkr_atmosphere_cie_lobe(float64_t wavelength_nm,
                                               float64_t mean_nm,
                                               float64_t below_inverse_width,
                                               float64_t above_inverse_width) {
  const float64_t inverse_width =
      wavelength_nm < mean_nm ? below_inverse_width : above_inverse_width;
  const float64_t t = (wavelength_nm - mean_nm) * inverse_width;
  return exp(-0.5 * t * t);
}

vkr_internal float64_t vkr_atmosphere_cie_x(float64_t nm) {
  return 1.056 * vkr_atmosphere_cie_lobe(nm, 599.8, 0.0264, 0.0323) +
         0.362 * vkr_atmosphere_cie_lobe(nm, 442.0, 0.0624, 0.0374) -
         0.065 * vkr_atmosphere_cie_lobe(nm, 501.1, 0.0490, 0.0382);
}

vkr_internal float64_t vkr_atmosphere_cie_y(float64_t nm) {
  return 0.821 * vkr_atmosphere_cie_lobe(nm, 568.8, 0.0213, 0.0247) +
         0.286 * vkr_atmosphere_cie_lobe(nm, 530.9, 0.0613, 0.0322);
}

vkr_internal float64_t vkr_atmosphere_cie_z(float64_t nm) {
  return 1.217 * vkr_atmosphere_cie_lobe(nm, 437.0, 0.0845, 0.0278) +
         0.681 * vkr_atmosphere_cie_lobe(nm, 459.0, 0.0385, 0.0725);
}

Vec3 vkr_atmosphere_blackbody_rgb(float32_t kelvin) {
  /* Second radiation constant hc/k in metre-kelvin. */
  const float64_t second_radiation_constant = 1.438776877e-2;
  float64_t x = 0.0;
  float64_t y = 0.0;
  float64_t z = 0.0;
  for (uint32_t nm = 360u; nm <= 830u; ++nm) {
    const float64_t wavelength_m = (float64_t)nm * 1e-9;
    /* Planck spectral radiance up to a constant factor. */
    const float64_t exponent =
        second_radiation_constant / (wavelength_m * (float64_t)kelvin);
    const float64_t radiance =
        1.0 / (pow(wavelength_m, 5.0) * (exp(exponent) - 1.0));
    x += radiance * vkr_atmosphere_cie_x((float64_t)nm);
    y += radiance * vkr_atmosphere_cie_y((float64_t)nm);
    z += radiance * vkr_atmosphere_cie_z((float64_t)nm);
  }

  /* XYZ to linear Rec.709. Temperatures below about 1900 K leave the gamut;
     clamping keeps the hue family before luminance normalization. */
  const float64_t r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
  const float64_t g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
  const float64_t b = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
  const Vec3 rgb = vec3_new((float32_t)Max(r, 0.0), (float32_t)Max(g, 0.0),
                            (float32_t)Max(b, 0.0));
  return vec3_scale(rgb, 1.0f / vkr_atmosphere_luminance(rgb));
}

bool8_t
vkr_atmosphere_apply_sun_authoring(VkrAtmosphereSettings *settings,
                                   const VkrAtmosphereSunAuthoring *authoring) {
  if (!authoring->has_temperature && !authoring->has_illuminance) {
    return true_v;
  }

  const float32_t kelvin = authoring->temperature_kelvin;
  if (authoring->has_temperature &&
      !(isfinite(kelvin) && kelvin >= VKR_ATMOSPHERE_SUN_TEMPERATURE_MIN_K &&
        kelvin <= VKR_ATMOSPHERE_SUN_TEMPERATURE_MAX_K)) {
    return false_v;
  }
  if (authoring->has_illuminance &&
      !(isfinite(authoring->illuminance) && authoring->illuminance >= 0.0f)) {
    return false_v;
  }

  const Vec3 default_irradiance =
      vkr_atmosphere_settings_defaults().solar_irradiance;
  const float32_t default_luminance =
      vkr_atmosphere_luminance(default_irradiance);
  const Vec3 colour =
      authoring->has_temperature
          ? vkr_atmosphere_blackbody_rgb(kelvin)
          : vec3_scale(default_irradiance, 1.0f / default_luminance);
  const float32_t illuminance =
      authoring->has_illuminance ? authoring->illuminance : default_luminance;
  settings->solar_irradiance = vec3_scale(colour, illuminance);
  return true_v;
}

VkrAtmosphereSettings
vkr_atmosphere_with_sun(const VkrAtmosphereSettings *medium,
                        const VkrAtmosphereSettings *sun) {
  VkrAtmosphereSettings lit = *medium;
  lit.sun_direction = sun->sun_direction;
  lit.solar_irradiance = sun->solar_irradiance;
  lit.sun_angular_diameter_degrees = sun->sun_angular_diameter_degrees;
  lit.sun_glow = sun->sun_glow;
  return lit;
}

bool8_t vkr_atmosphere_apply_sun_light(VkrAtmosphereSettings *settings,
                                       Vec3 light_direction, Vec3 irradiance,
                                       float32_t sun_angular_diameter_degrees) {
  VkrAtmosphereSettings lit = *settings;
  lit.sun_direction = vec3_negate(light_direction);
  lit.solar_irradiance = irradiance;
  if (sun_angular_diameter_degrees > 0.0f &&
      sun_angular_diameter_degrees <= 5.0f) {
    lit.sun_angular_diameter_degrees = sun_angular_diameter_degrees;
  }

  if (!vkr_atmosphere_settings_valid(&lit)) {
    return false_v;
  }

  *settings = lit;
  return true_v;
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

VkrSkyGpuParams
vkr_atmosphere_prepare_sky(const VkrAtmosphereSettings *settings,
                           const VkrCloudSettings *clouds,
                           Vec2 cloud_wind_offset_m, Vec3 camera_position,
                           Mat4 view_projection, bool8_t aerial_perspective) {
  VkrSkyGpuParams sky = {
      .atmosphere = vkr_atmosphere_prepare(settings),
      .view_projection = view_projection,
      .inverse_view_projection = mat4_inverse(view_projection),
      .camera_position = {camera_position.x, camera_position.y,
                          camera_position.z,
                          settings->metres_per_world_unit * 0.001f},
      .aerial = {VKR_ATMOSPHERE_AERIAL_KM_PER_SLICE,
                 (float32_t)VKR_ATMOSPHERE_AERIAL_SIZE,
                 1.0f / (float32_t)VKR_ATMOSPHERE_AERIAL_SIZE,
                 aerial_perspective ? 1.0f : 0.0f},
  };
  /* The camera stands on the planet directly below its world position, so
     only its height changes the atmosphere it looks through. */
  const float32_t altitude_m =
      settings->observer_altitude_m +
      camera_position.y * settings->metres_per_world_unit;
  sky.atmosphere.planet.z = Clamp(altitude_m, 0.0f, 100000.0f) * 0.001f;
  sky.atmosphere.mie_extinction.w = settings->sun_glow;
  if (clouds->enabled && aerial_perspective) {
    const float32_t km_per_unit = sky.camera_position.w;
    const Vec3 camera_km =
        vec3_new(camera_position.x * km_per_unit, sky.atmosphere.planet.z,
                 camera_position.z * km_per_unit);
    const Vec3 sun = vec3_new(sky.atmosphere.sun.x, sky.atmosphere.sun.y,
                              sky.atmosphere.sun.z);
    sky.clouds = vkr_cloud_prepare(clouds, sky.atmosphere.planet.x, camera_km,
                                   sun, cloud_wind_offset_m);
  }
  return sky;
}

vkr_internal float32_t vkr_atmosphere_density_exponential(float32_t height,
                                                          float32_t scale) {
  return scale > 1e-6f ? expf(-Max(height, 0.0f) / scale) : 0.0f;
}

VkrAtmosphereMedium vkr_atmosphere_medium(const VkrAtmosphereGpuParams *params,
                                          Vec3 position) {
  const float32_t height = Max(vec3_length(position) - params->planet.x, 0.0f);
  const float32_t rayleigh =
      vkr_atmosphere_density_exponential(height, params->rayleigh.w);
  const float32_t mie =
      vkr_atmosphere_density_exponential(height, params->mie_scattering.w);
  const float32_t ozone = Clamp(1.0f - fabsf(height - params->ozone.w) /
                                           Max(params->ground.w, 1e-6f),
                                0.0f, 1.0f);

  VkrAtmosphereMedium medium = {
      .rayleigh_scattering = vec3_scale(
          vec3_new(params->rayleigh.x, params->rayleigh.y, params->rayleigh.z),
          rayleigh),
      .mie_scattering = vec3_scale(vec3_new(params->mie_scattering.x,
                                            params->mie_scattering.y,
                                            params->mie_scattering.z),
                                   mie),
  };
  medium.scattering =
      vec3_add(medium.rayleigh_scattering, medium.mie_scattering);
  medium.extinction = vec3_add(
      vec3_add(medium.rayleigh_scattering,
               vec3_scale(vec3_new(params->mie_extinction.x,
                                   params->mie_extinction.y,
                                   params->mie_extinction.z),
                          mie)),
      vec3_scale(vec3_new(params->ozone.x, params->ozone.y, params->ozone.z),
                 ozone));
  return medium;
}

/* Nearest nonnegative hit, or -1 when the ray misses the sphere. */
vkr_internal float32_t vkr_atmosphere_ray_sphere_nearest(Vec3 origin,
                                                         Vec3 direction,
                                                         float32_t radius) {
  const float32_t b = vec3_dot(origin, direction);
  const float32_t c = vec3_dot(origin, origin) - radius * radius;
  const float32_t discriminant = b * b - c;
  if (discriminant < 0.0f) {
    return -1.0f;
  }

  const float32_t root = sqrtf(discriminant);
  const float32_t near_hit = -b - root;
  const float32_t far_hit = -b + root;
  return near_hit >= 0.0f ? near_hit : (far_hit >= 0.0f ? far_hit : -1.0f);
}

float32_t vkr_atmosphere_segment_limit(const VkrAtmosphereGpuParams *params,
                                       Vec3 origin, Vec3 direction,
                                       bool8_t *out_hits_ground) {
  const float32_t b = vec3_dot(origin, direction);
  const float32_t origin_squared = vec3_dot(origin, origin);
  /* At ground level the inward segment is empty; -b-sqrt(b*b) can round
     slightly negative and select the far planet hit. */
  if (b < 0.0f && origin_squared <= params->planet.x * params->planet.x) {
    *out_hits_ground = true_v;
    return 0.0f;
  }

  const float32_t top =
      Max(-b + sqrtf(Max(b * b - origin_squared +
                             params->planet.y * params->planet.y,
                         0.0f)),
          0.0f);
  const float32_t bottom =
      vkr_atmosphere_ray_sphere_nearest(origin, direction, params->planet.x);
  *out_hits_ground = b < 0.0f && bottom >= 0.0f && bottom < top;
  return *out_hits_ground ? bottom : top;
}

bool8_t vkr_atmosphere_sun_occluded(const VkrAtmosphereGpuParams *params,
                                    Vec3 position, Vec3 sun_direction) {
  return vec3_dot(position, sun_direction) < 0.0f &&
         vkr_atmosphere_ray_sphere_nearest(position, sun_direction,
                                           params->planet.x) >= 0.0f;
}

Vec3 vkr_atmosphere_transmittance(const VkrAtmosphereGpuParams *params,
                                  Vec3 position, Vec3 direction) {
  bool8_t hits_ground = false_v;
  const float32_t distance =
      vkr_atmosphere_segment_limit(params, position, direction, &hits_ground);
  if (distance <= 0.0f || hits_ground) {
    return vec3_zero();
  }

  const float32_t step =
      distance / (float32_t)VKR_ATMOSPHERE_TRANSMITTANCE_SAMPLES;
  Vec3 optical_depth = vec3_zero();
  for (uint32_t i = 0; i < VKR_ATMOSPHERE_TRANSMITTANCE_SAMPLES; ++i) {
    const Vec3 sample =
        vec3_add(position, vec3_scale(direction, ((float32_t)i + 0.5f) * step));
    optical_depth = vec3_add(
        optical_depth,
        vec3_scale(vkr_atmosphere_medium(params, sample).extinction, step));
  }
  return vec3_new(
      expf(-Min(optical_depth.x, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH)),
      expf(-Min(optical_depth.y, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH)),
      expf(-Min(optical_depth.z, VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH)));
}

Vec3 vkr_atmosphere_observer_irradiance(const VkrAtmosphereGpuParams *params) {
  const Vec3 observer =
      vec3_new(0.0f, params->planet.x + params->planet.z, 0.0f);
  const Vec3 sun = vec3_new(params->sun.x, params->sun.y, params->sun.z);
  if (vkr_atmosphere_sun_occluded(params, observer, sun)) {
    return vec3_zero();
  }

  return vec3_mul(vec3_new(params->solar.x, params->solar.y, params->solar.z),
                  vkr_atmosphere_transmittance(params, observer, sun));
}
