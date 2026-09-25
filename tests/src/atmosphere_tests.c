#include "atmosphere_tests.h"

#include "vkr_atmosphere.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static float32_t atmosphere_test_luminance(Vec3 rgb) {
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

/* CIE 1931 chromaticity of a linear Rec.709 colour. */
static void atmosphere_test_chromaticity(Vec3 rgb, float64_t *out_x,
                                         float64_t *out_y) {
  const float64_t x = 0.4124564 * rgb.x + 0.3575761 * rgb.y + 0.1804375 * rgb.z;
  const float64_t y = 0.2126729 * rgb.x + 0.7151522 * rgb.y + 0.0721750 * rgb.z;
  const float64_t z = 0.0193339 * rgb.x + 0.1191920 * rgb.y + 0.9503041 * rgb.z;
  *out_x = x / (x + y + z);
  *out_y = y / (x + y + z);
}

/* Kim et al.'s cubic-spline Planckian locus (2002), an independent oracle for
   the spectral integration, valid from 1667 K to 25000 K. */
static void atmosphere_test_planckian_locus(float64_t kelvin, float64_t *out_x,
                                            float64_t *out_y) {
  const float64_t t = kelvin;
  const float64_t x = t <= 4000.0
                          ? -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) +
                                0.8776956e3 / t + 0.179910
                          : -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) +
                                0.2226347e3 / t + 0.240390;
  float64_t y = 0.0;
  if (t <= 2222.0) {
    y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x -
        0.20219683;
  } else if (t <= 4000.0) {
    y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x -
        0.16748867;
  } else {
    y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x -
        0.37001483;
  }
  *out_x = x;
  *out_y = y;
}

static bool32_t test_atmosphere_blackbody_follows_planckian_locus(void) {
  printf("  Running test_atmosphere_blackbody_follows_planckian_locus...\n");
  /* In-gamut temperatures; the colour-matching fit stays within 0.004. */
  const float32_t temperatures[] = {2000.0f, 3000.0f,  4000.0f, 5000.0f,
                                    6500.0f, 10000.0f, 20000.0f};
  for (uint32_t i = 0; i < ArrayCount(temperatures); ++i) {
    const Vec3 rgb = vkr_atmosphere_blackbody_rgb(temperatures[i]);
    float64_t x = 0.0;
    float64_t y = 0.0;
    float64_t expected_x = 0.0;
    float64_t expected_y = 0.0;
    atmosphere_test_chromaticity(rgb, &x, &y);
    atmosphere_test_planckian_locus(temperatures[i], &expected_x, &expected_y);
    assert(fabs(x - expected_x) < 0.004);
    assert(fabs(y - expected_y) < 0.004);
    assert(fabsf(atmosphere_test_luminance(rgb) - 1.0f) < 1e-4f);
  }

  /* 6500 K lies near the Rec.709 white point; 2000 K is red-dominant. */
  const Vec3 near_white = vkr_atmosphere_blackbody_rgb(6500.0f);
  assert(fabsf(near_white.x - near_white.z) < 0.1f);
  const Vec3 candle = vkr_atmosphere_blackbody_rgb(2000.0f);
  assert(candle.x > candle.y && candle.y > candle.z);
  printf("  test_atmosphere_blackbody_follows_planckian_locus PASSED\n");
  return true_v;
}

static bool32_t test_atmosphere_blackbody_range_edges(void) {
  printf("  Running test_atmosphere_blackbody_range_edges...\n");
  /* 1000 K leaves the gamut; clamping must keep a finite red colour with unit
     luminance. 40000 K is blue-dominant. */
  const Vec3 low =
      vkr_atmosphere_blackbody_rgb(VKR_ATMOSPHERE_SUN_TEMPERATURE_MIN_K);
  assert(isfinite(low.x) && isfinite(low.y) && isfinite(low.z));
  assert(low.x > 0.0f && low.y >= 0.0f && low.z >= 0.0f);
  assert(low.x > low.y && low.y >= low.z);
  assert(fabsf(atmosphere_test_luminance(low) - 1.0f) < 1e-4f);
  const Vec3 high =
      vkr_atmosphere_blackbody_rgb(VKR_ATMOSPHERE_SUN_TEMPERATURE_MAX_K);
  assert(high.z > high.y && high.y > high.x && high.x > 0.0f);
  assert(fabsf(atmosphere_test_luminance(high) - 1.0f) < 1e-4f);
  printf("  test_atmosphere_blackbody_range_edges PASSED\n");
  return true_v;
}

static bool32_t test_atmosphere_sun_authoring(void) {
  printf("  Running test_atmosphere_sun_authoring...\n");
  const VkrAtmosphereSettings defaults = vkr_atmosphere_settings_defaults();
  const float32_t default_luminance =
      atmosphere_test_luminance(defaults.solar_irradiance);

  /* No authoring leaves the raw irradiance untouched. */
  VkrAtmosphereSettings settings = defaults;
  const VkrAtmosphereSunAuthoring none = {0};
  assert(vkr_atmosphere_apply_sun_authoring(&settings, &none));
  assert(settings.solar_irradiance.x == defaults.solar_irradiance.x);

  /* Temperature alone keeps the default brightness. */
  settings = defaults;
  const VkrAtmosphereSunAuthoring warm = {.has_temperature = true_v,
                                          .temperature_kelvin = 3000.0f};
  assert(vkr_atmosphere_apply_sun_authoring(&settings, &warm));
  assert(fabsf(atmosphere_test_luminance(settings.solar_irradiance) -
               default_luminance) < 1e-4f);
  assert(settings.solar_irradiance.x > settings.solar_irradiance.z);

  /* Illuminance alone keeps the default colour. */
  settings = defaults;
  const VkrAtmosphereSunAuthoring bright = {.has_illuminance = true_v,
                                            .illuminance = 3.0f};
  assert(vkr_atmosphere_apply_sun_authoring(&settings, &bright));
  assert(fabsf(atmosphere_test_luminance(settings.solar_irradiance) - 3.0f) <
         1e-4f);
  assert(fabsf(settings.solar_irradiance.z / settings.solar_irradiance.x -
               defaults.solar_irradiance.z / defaults.solar_irradiance.x) <
         1e-4f);

  /* Values outside the authorable domain are rejected. */
  const VkrAtmosphereSunAuthoring invalid[] = {
      {.has_temperature = true_v, .temperature_kelvin = 999.0f},
      {.has_temperature = true_v, .temperature_kelvin = 40001.0f},
      {.has_temperature = true_v, .temperature_kelvin = NAN},
      {.has_illuminance = true_v, .illuminance = -1.0f},
      {.has_illuminance = true_v, .illuminance = INFINITY},
  };
  for (uint32_t i = 0; i < ArrayCount(invalid); ++i) {
    settings = defaults;
    assert(!vkr_atmosphere_apply_sun_authoring(&settings, &invalid[i]));
  }
  printf("  test_atmosphere_sun_authoring PASSED\n");
  return true_v;
}

/* The camera stands on the planet below its world position: its altitude is
   the observer altitude plus world height times the authored scale, kept in
   the supported altitude domain. */
vkr_internal bool32_t test_atmosphere_camera_altitude(void) {
  printf("  Running test_atmosphere_camera_altitude...\n");
  VkrAtmosphereSettings settings = vkr_atmosphere_settings_defaults();
  settings.enabled = true_v;
  settings.observer_altitude_m = 1000.0f;
  settings.metres_per_world_unit = 2.0f;
  const Mat4 identity = mat4_identity();
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  const Vec2 wind = {0};

  VkrSkyGpuParams sky = vkr_atmosphere_prepare_sky(
      &settings, &clouds, wind, vec3_new(5.0f, 15.0f, -3.0f), identity, true_v);
  assert(fabsf(sky.atmosphere.planet.z - 1.03f) < 1e-5f);
  assert(fabsf(sky.camera_position.w - 0.002f) < 1e-9f);
  assert(sky.aerial.w == 1.0f);

  /* Horizontal position does not change the altitude. */
  VkrSkyGpuParams moved = vkr_atmosphere_prepare_sky(
      &settings, &clouds, wind, vec3_new(-400.0f, 15.0f, 900.0f), identity,
      true_v);
  assert(moved.atmosphere.planet.z == sky.atmosphere.planet.z);

  /* Below the planet surface and above the atmosphere both clamp. */
  sky = vkr_atmosphere_prepare_sky(&settings, &clouds, wind,
                                   vec3_new(0.0f, -900.0f, 0.0f), identity,
                                   false_v);
  assert(sky.atmosphere.planet.z == 0.0f);
  assert(sky.aerial.w == 0.0f);
  sky = vkr_atmosphere_prepare_sky(
      &settings, &clouds, wind, vec3_new(0.0f, 1.0e6f, 0.0f), identity, true_v);
  assert(fabsf(sky.atmosphere.planet.z - 100.0f) < 1e-4f);

  /* The world scale is authorable only inside its documented range. */
  const float32_t invalid_scales[] = {0.0f,    -1.0f, 1.0e-4f,
                                      2000.0f, NAN,   INFINITY};
  for (uint32_t i = 0; i < ArrayCount(invalid_scales); ++i) {
    VkrAtmosphereSettings invalid = settings;
    invalid.metres_per_world_unit = invalid_scales[i];
    assert(!vkr_atmosphere_settings_valid(&invalid));
  }
  assert(vkr_atmosphere_settings_valid(&settings));
  printf("  test_atmosphere_camera_altitude PASSED\n");
  return true_v;
}

/* The shadow map centres where the camera's sun ray enters the cloud base,
   snapped to its 8 km / 512 texel grid; wind offsets wrap at the 32 km weather
   period; clouds exist only under the aerial-perspective rule. */
vkr_internal bool32_t test_atmosphere_cloud_layer(void) {
  printf("  Running test_atmosphere_cloud_layer...\n");
  VkrAtmosphereSettings settings = vkr_atmosphere_settings_defaults();
  settings.enabled = true_v;
  settings.observer_altitude_m = 0.0f;
  settings.metres_per_world_unit = 1.0f;
  settings.sun_direction = vec3_new(1.0f, 1.0f, 0.0f);
  VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  clouds.enabled = true_v;
  clouds.base_altitude_m = 1500.0f;
  clouds.top_altitude_m = 4000.0f;
  clouds.coverage = 0.25f;
  clouds.density = 0.05f;
  const Mat4 identity = mat4_identity();
  const Vec2 wind = {.x = -1.0f, .y = 32001.0f};

  /* From 100 m east at ground level, a 45-degree sun ray rises 1.5 km and
     travels 1.5 km east: 1.6 km snaps to texel 102 of 0.015625 km. */
  VkrSkyGpuParams sky = vkr_atmosphere_prepare_sky(
      &settings, &clouds, wind, vec3_new(100.0f, 0.0f, 0.0f), identity, true_v);
  assert(fabsf(sky.clouds.shadow.x - 1.59375f) < 1e-5f);
  assert(fabsf(sky.clouds.shadow.y) < 1e-6f);
  assert(fabsf(sky.clouds.shadow.w - 8.0f) < 1e-6f);
  assert(fabsf(sky.clouds.layer.x - 6361.5f) < 1e-3f);
  assert(fabsf(sky.clouds.layer.y - 6364.0f) < 1e-3f);
  assert(fabsf(sky.clouds.layer.z - 50.0f) < 1e-4f);
  assert(sky.clouds.layer.w == 0.25f);
  assert(sky.clouds.noise.w == 1.0f);
  assert(fabsf(sky.clouds.wind.x - 31.999f) < 1e-4f);
  assert(fabsf(sky.clouds.wind.y - 0.001f) < 1e-5f);

  /* Above the base the map centres on the camera itself. */
  sky = vkr_atmosphere_prepare_sky(&settings, &clouds, wind,
                                   vec3_new(100.0f, 2000.0f, 0.0f), identity,
                                   true_v);
  assert(fabsf(sky.clouds.shadow.x - 0.09375f) < 1e-5f);

  sky = vkr_atmosphere_prepare_sky(&settings, &clouds, wind,
                                   vec3_new(100.0f, 0.0f, 0.0f), identity,
                                   false_v);
  assert(sky.clouds.noise.w == 0.0f);
  clouds.enabled = false_v;
  sky = vkr_atmosphere_prepare_sky(
      &settings, &clouds, wind, vec3_new(100.0f, 0.0f, 0.0f), identity, true_v);
  assert(sky.clouds.noise.w == 0.0f);

  clouds.enabled = true_v;
  assert(vkr_cloud_settings_valid(&clouds));
  VkrCloudSettings invalid = clouds;
  invalid.top_altitude_m = invalid.base_altitude_m + 50.0f;
  assert(!vkr_cloud_settings_valid(&invalid));
  invalid = clouds;
  invalid.top_altitude_m = 25000.0f;
  assert(!vkr_cloud_settings_valid(&invalid));
  invalid = clouds;
  invalid.base_altitude_m = -1.0f;
  assert(!vkr_cloud_settings_valid(&invalid));
  invalid = clouds;
  invalid.coverage = 1.5f;
  assert(!vkr_cloud_settings_valid(&invalid));
  invalid = clouds;
  invalid.density = NAN;
  assert(!vkr_cloud_settings_valid(&invalid));
  invalid = clouds;
  invalid.wind_mps.y = 300.0f;
  assert(!vkr_cloud_settings_valid(&invalid));
  printf("  test_atmosphere_cloud_layer PASSED\n");
  return true_v;
}

/* The direct sun's transmittance is a closed form for a Rayleigh-only
   atmosphere seen at the zenith from the ground: the optical depth
   beta * H * (1 - exp(-thickness / H)). The 40-sample midpoint rule
   underestimates it by about 0.4%, well inside the tolerance, while a wrong
   scale height, path or coefficient misses by far more. */
vkr_internal bool32_t test_atmosphere_observer_irradiance(void) {
  printf("  Running test_atmosphere_observer_irradiance...\n");
  VkrAtmosphereSettings settings = vkr_atmosphere_settings_defaults();
  settings.enabled = true_v;
  settings.sun_direction = vec3_new(0.0f, 1.0f, 0.0f);
  settings.mie_density_scale = 0.0f;
  settings.ozone_density_scale = 0.0f;
  const VkrAtmosphereGpuParams params = vkr_atmosphere_prepare(&settings);
  const Vec3 irradiance = vkr_atmosphere_observer_irradiance(&params);
  const float64_t thickness = params.planet.y - params.planet.x;
  const float64_t scale_height = params.rayleigh.w;
  const float64_t beta[3] = {params.rayleigh.x, params.rayleigh.y,
                             params.rayleigh.z};
  const float64_t solar[3] = {params.solar.x, params.solar.y, params.solar.z};
  const float64_t observed[3] = {irradiance.x, irradiance.y, irradiance.z};
  for (uint32_t channel = 0u; channel < 3u; ++channel) {
    const float64_t depth =
        beta[channel] * scale_height * (1.0 - exp(-thickness / scale_height));
    const float64_t expected = solar[channel] * exp(-depth);
    assert(fabs(observed[channel] - expected) <= 2e-3 * expected);
  }

  // A clear medium passes the calibrated top-of-atmosphere irradiance.
  settings.rayleigh_density_scale = 0.0f;
  const VkrAtmosphereGpuParams clear = vkr_atmosphere_prepare(&settings);
  const Vec3 unattenuated = vkr_atmosphere_observer_irradiance(&clear);
  assert(unattenuated.x == clear.solar.x && unattenuated.y == clear.solar.y &&
         unattenuated.z == clear.solar.z);

  // The planet blocks a sun below the horizon.
  settings.sun_direction = vec3_new(0.0f, -0.2f, 1.0f);
  const VkrAtmosphereGpuParams set = vkr_atmosphere_prepare(&settings);
  const Vec3 night = vkr_atmosphere_observer_irradiance(&set);
  assert(night.x == 0.0f && night.y == 0.0f && night.z == 0.0f);

  printf("  test_atmosphere_observer_irradiance PASSED\n");
  return true_v;
}

/* The glow travels only in the frame's sky record, so authored glow cannot
   change the revision bake or its recipe hash; the domain is [0, 100]. */
vkr_internal bool32_t test_atmosphere_sun_glow(void) {
  printf("  Running test_atmosphere_sun_glow...\n");
  VkrAtmosphereSettings settings = vkr_atmosphere_settings_defaults();
  settings.enabled = true_v;
  settings.sun_glow = 3.0f;
  const VkrAtmosphereGpuParams bake = vkr_atmosphere_prepare(&settings);
  assert(bake.mie_extinction.w == 0.0f);
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  const VkrSkyGpuParams sky =
      vkr_atmosphere_prepare_sky(&settings, &clouds, vec2_new(0.0f, 0.0f),
                                 vec3_zero(), mat4_identity(), true_v);
  assert(sky.atmosphere.mie_extinction.w == 3.0f);

  const float32_t invalid[] = {-0.5f, 101.0f, NAN};
  for (uint32_t i = 0u; i < ArrayCount(invalid); ++i) {
    VkrAtmosphereSettings rejected = settings;
    rejected.sun_glow = invalid[i];
    assert(!vkr_atmosphere_settings_valid(&rejected));
  }

  printf("  test_atmosphere_sun_glow PASSED\n");
  return true_v;
}

bool32_t run_atmosphere_tests(void) {
  printf("--- Starting Atmosphere Tests ---\n");
  bool32_t passed = true_v;
  passed &= test_atmosphere_blackbody_follows_planckian_locus();
  passed &= test_atmosphere_blackbody_range_edges();
  passed &= test_atmosphere_sun_authoring();
  passed &= test_atmosphere_camera_altitude();
  passed &= test_atmosphere_cloud_layer();
  passed &= test_atmosphere_observer_irradiance();
  passed &= test_atmosphere_sun_glow();
  printf("--- Atmosphere Tests Completed ---\n");
  return passed;
}
