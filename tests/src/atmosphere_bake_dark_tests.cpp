#include "atmosphere_bake_dark_tests.h"

#include "bake/vkr_bake_atmosphere.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

/* The scene skips atmosphere revisions while the sun stays past the dark
 * depression (ADR-058), which is exact only if that bake is black. The CPU
 * baker evaluates the same atmosphere model independently of the bound's
 * geometry, so it is the oracle: a skipped bake at the bound must have no
 * radiance in any source texel. */

static VkrAtmosphereSettings dark_test_settings(float32_t observer_altitude_m,
                                                float32_t depression_degrees) {
  VkrAtmosphereSettings settings = vkr_atmosphere_settings_defaults();
  settings.enabled = true_v;
  settings.observer_altitude_m = observer_altitude_m;
  const float64_t elevation =
      -(float64_t)depression_degrees * (3.14159265358979323846 / 180.0);
  settings.sun_direction =
      vec3_new((float32_t)cos(elevation), (float32_t)sin(elevation), 0.0f);
  return settings;
}

static float32_t
dark_test_peak_radiance(const VkrAtmosphereSettings *settings) {
  VkrBakeAtmosphere atmosphere;
  const bool built = vkr_bake_atmosphere_build(&atmosphere, settings);
  assert(built);
  (void)built;
  float32_t peak = 0.0f;
  for (const Vec3 &texel : atmosphere.source_rgb) {
    peak = fmaxf(peak, fmaxf(texel.x, fmaxf(texel.y, texel.z)));
  }
  return peak;
}

static bool32_t test_bake_is_black_at_the_dark_depression(void) {
  printf("  Running test_bake_is_black_at_the_dark_depression...\n");
  const float32_t altitudes_m[] = {0.0f, 100000.0f};
  for (float32_t altitude : altitudes_m) {
    const VkrAtmosphereSettings observer = dark_test_settings(altitude, 0.0f);
    const VkrAtmosphereGpuParams observer_params =
        vkr_atmosphere_prepare(&observer);
    /* Just past the bound: the float sun vector cannot place it exactly. */
    const float32_t depression =
        vkr_atmosphere_dark_depression_degrees(&observer_params) + 0.01f;
    const VkrAtmosphereSettings dark = dark_test_settings(altitude, depression);
    const VkrAtmosphereGpuParams dark_params = vkr_atmosphere_prepare(&dark);
    assert(vkr_atmosphere_bake_dark(&dark_params));
    assert(dark_test_peak_radiance(&dark) == 0.0f);
  }

  // Ten degrees short of the bound the sky still scatters sunlight, so the
  // oracle can distinguish a black bake from a broken one.
  const VkrAtmosphereSettings sea_level = dark_test_settings(0.0f, 0.0f);
  const VkrAtmosphereGpuParams sea_level_params =
      vkr_atmosphere_prepare(&sea_level);
  const VkrAtmosphereSettings dim = dark_test_settings(
      0.0f, vkr_atmosphere_dark_depression_degrees(&sea_level_params) - 10.0f);
  const VkrAtmosphereGpuParams dim_params = vkr_atmosphere_prepare(&dim);
  assert(!vkr_atmosphere_bake_dark(&dim_params));
  assert(dark_test_peak_radiance(&dim) > 0.0f);
  printf("  test_bake_is_black_at_the_dark_depression PASSED\n");
  return true_v;
}

/* The moon is the night's second atmosphere light (ADR-081): a sky whose sun
 * is past the dark depression must still bake while the moon lights it, and
 * is black only once the moon is past the bound too. */
static bool32_t test_moon_keeps_a_dark_sun_bake(void) {
  printf("  Running test_moon_keeps_a_dark_sun_bake...\n");
  const VkrAtmosphereSettings observer = dark_test_settings(0.0f, 0.0f);
  const VkrAtmosphereGpuParams observer_params =
      vkr_atmosphere_prepare(&observer);
  const float32_t depression =
      vkr_atmosphere_dark_depression_degrees(&observer_params) + 0.01f;
  VkrAtmosphereSettings night = dark_test_settings(0.0f, depression);
  const float64_t radians = 3.14159265358979323846 / 180.0;
  // A full moon opposite the sun's azimuth, 30 degrees up.
  night.moon_direction =
      vec3_new((float32_t)-cos(30.0 * radians), (float32_t)sin(30.0 * radians),
               0.0f);
  night.lunar_irradiance = vec3_new(4.5e-6f, 4.5e-6f, 4.5e-6f);
  const VkrAtmosphereGpuParams moonlit = vkr_atmosphere_prepare(&night);
  assert(!vkr_atmosphere_bake_dark(&moonlit));
  assert(dark_test_peak_radiance(&night) > 0.0f);

  night.moon_direction = vec3_new((float32_t)-cos(depression * radians),
                                  (float32_t)-sin(depression * radians), 0.0f);
  const VkrAtmosphereGpuParams moonless = vkr_atmosphere_prepare(&night);
  assert(vkr_atmosphere_bake_dark(&moonless));
  assert(dark_test_peak_radiance(&night) == 0.0f);
  printf("  test_moon_keeps_a_dark_sun_bake PASSED\n");
  return true_v;
}

bool32_t run_atmosphere_bake_dark_tests(void) {
  printf("--- Running atmosphere dark-bake tests... ---\n");
  const bool32_t passed = test_bake_is_black_at_the_dark_depression() &&
                          test_moon_keeps_a_dark_sun_bake();
  printf("--- Atmosphere dark-bake tests completed. ---\n");
  return passed;
}
