#include "look_volume_tests.h"

#include "math/mat.h"
#include "math/vkr_math.h"
#include "renderer/systems/vkr_scene_look.h"

#include <math.h>

static bool8_t look_test_near(float32_t a, float32_t b) {
  return fabsf(a - b) < 1e-4f;
}

/* A volume whose box has the given edge vectors and centre, overriding
   nothing yet. */
static VkrLookVolume look_test_volume(Vec3 x, Vec3 y, Vec3 z, Vec3 centre,
                                      float32_t blend_distance) {
  VkrLookVolume volume = {
      .value = {.enabled = true_v, .blend_distance = blend_distance},
      .box = mat4_identity(),
  };
  volume.box.cols[0] = vec4_new(x.x, x.y, x.z, 0.0f);
  volume.box.cols[1] = vec4_new(y.x, y.y, y.z, 0.0f);
  volume.box.cols[2] = vec4_new(z.x, z.y, z.z, 0.0f);
  volume.box.cols[3] = vec4_new(centre.x, centre.y, centre.z, 1.0f);
  return volume;
}

static VkrLook look_test_base(void) {
  return (VkrLook){
      .exposure_compensation_ev = 0.0f,
      .metering_min_ev = -8.0f,
      .metering_max_ev = 24.0f,
      .contrast = 1.0f,
      .saturation = 1.0f,
      .bloom_intensity = 0.1f,
      .fog_color = vec3_new(0.5f, 0.5f, 0.5f),
      .fog_density = 0.01f,
      .sky_light_intensity = 1.0f,
  };
}

/* The weight is 1 inside, and outside falls by the metric distance to the
   box, whatever the box's scale and rotation. */
static bool32_t test_look_volume_weight_at_boundaries(void) {
  /* A 4 x 2 x 2 m box turned 90 degrees about Y, centred at (10, 0, 0):
     its local X runs along world Z, so it spans world x 9..11, z -2..2. */
  const VkrLookVolume box = look_test_volume(
      vec3_new(0.0f, 0.0f, 4.0f), vec3_new(0.0f, 2.0f, 0.0f),
      vec3_new(-2.0f, 0.0f, 0.0f), vec3_new(10.0f, 0.0f, 0.0f), 2.0f);
  assert(look_test_near(vkr_look_volume_weight(&box, vec3_new(10, 0, 0)), 1));
  assert(look_test_near(vkr_look_volume_weight(&box, vec3_new(10.9f, 0, 1.9f)),
                        1));
  /* 1 m beyond the long face (z = 2) and beyond the short face (x = 11). */
  assert(
      look_test_near(vkr_look_volume_weight(&box, vec3_new(10, 0, 3)), 0.5f));
  assert(
      look_test_near(vkr_look_volume_weight(&box, vec3_new(12, 0, 0)), 0.5f));
  /* Diagonally past a corner: 3-4-5 metres from (11, 1, 2) at blend 2 is
     past the blend; at 0.6 x 0.8 = 1 m it is half. */
  assert(look_test_near(vkr_look_volume_weight(&box, vec3_new(14, 1, 6)), 0));
  assert(look_test_near(vkr_look_volume_weight(&box, vec3_new(11.6f, 1, 2.8f)),
                        0.5f));
  /* At the blend distance and without one. */
  assert(look_test_near(vkr_look_volume_weight(&box, vec3_new(10, 0, 4)), 0));
  VkrLookVolume hard = box;
  hard.value.blend_distance = 0.0f;
  assert(
      look_test_near(vkr_look_volume_weight(&hard, vec3_new(10, 0, 2.01f)), 0));
  assert(
      look_test_near(vkr_look_volume_weight(&hard, vec3_new(10, 0, 1.99f)), 1));
  VkrLookVolume disabled = box;
  disabled.value.enabled = false_v;
  assert(
      look_test_near(vkr_look_volume_weight(&disabled, vec3_new(10, 0, 0)), 0));
  VkrLookVolume flat = box;
  flat.box.cols[1] = vec4_new(0.0f, 0.0f, 0.0f, 0.0f);
  assert(look_test_near(vkr_look_volume_weight(&flat, vec3_new(10, 0, 0)), 0));
  printf("  test_look_volume_weight_at_boundaries PASSED\n");
  return true_v;
}

/* Higher priorities apply last; equal priorities keep their order; a
   partial weight moves a value part of the way; values no volume overrides
   keep the base. */
static bool32_t test_look_volume_blend_priorities(void) {
  const Vec3 x = vec3_new(10, 0, 0);
  const Vec3 y = vec3_new(0, 10, 0);
  const Vec3 z = vec3_new(0, 0, 10);
  VkrLookVolume volumes[3];
  volumes[0] = look_test_volume(x, y, z, vec3_new(0, 0, 0), 1.0f);
  volumes[0].value.priority = 5;
  volumes[0].value.override_contrast = true_v;
  volumes[0].value.contrast = 0.8f;
  volumes[1] = look_test_volume(x, y, z, vec3_new(0, 0, 0), 1.0f);
  volumes[1].value.priority = 0;
  volumes[1].value.override_contrast = true_v;
  volumes[1].value.contrast = 1.4f;
  volumes[1].value.override_saturation = true_v;
  volumes[1].value.saturation = 0.2f;
  /* Equal to the first's priority and after it: its value wins. */
  volumes[2] = look_test_volume(x, y, z, vec3_new(0, 0, 0), 1.0f);
  volumes[2].value.priority = 5;
  volumes[2].value.override_saturation = true_v;
  volumes[2].value.saturation = 0.6f;
  VkrLook look = look_test_base();
  vkr_look_blend(&look, volumes, 3u, vec3_new(0, 0, 0));
  assert(look_test_near(look.contrast, 0.8f));
  assert(look_test_near(look.saturation, 0.6f));
  assert(look_test_near(look.bloom_intensity, 0.1f));
  assert(look_test_near(look.exposure_compensation_ev, 0.0f));
  assert(!look.metering);

  /* Half way out of a 2 m blend: a half step toward each override. */
  VkrLookVolume partial = look_test_volume(x, y, z, vec3_new(0, 0, 0), 2.0f);
  partial.value.override_exposure = true_v;
  partial.value.exposure_compensation_ev = 2.0f;
  partial.value.override_metering = true_v;
  partial.value.metering_min_ev = 0.0f;
  partial.value.metering_max_ev = 12.0f;
  partial.value.override_fog_color = true_v;
  partial.value.fog_color = vec3_new(1.0f, 0.0f, 0.0f);
  partial.value.override_sky_light = true_v;
  partial.value.sky_light_intensity = 3.0f;
  look = look_test_base();
  vkr_look_blend(&look, &partial, 1u, vec3_new(6, 0, 0));
  assert(look_test_near(look.exposure_compensation_ev, 1.0f));
  assert(look.metering);
  assert(look_test_near(look.metering_min_ev, -4.0f));
  assert(look_test_near(look.metering_max_ev, 18.0f));
  assert(look_test_near(look.fog_color.x, 0.75f) &&
         look_test_near(look.fog_color.y, 0.25f));
  assert(look_test_near(look.sky_light_intensity, 2.0f));
  assert(look_test_near(look.fog_density, 0.01f));

  /* Beyond the blend the volume changes nothing, metering included. */
  look = look_test_base();
  vkr_look_blend(&look, &partial, 1u, vec3_new(8, 0, 0));
  assert(look_test_near(look.exposure_compensation_ev, 0.0f));
  assert(!look.metering);
  printf("  test_look_volume_blend_priorities PASSED\n");
  return true_v;
}

bool32_t run_look_volume_tests(void) {
  printf("--- Running Look volume tests... ---\n");
  bool32_t passed = true_v;
  passed &= test_look_volume_weight_at_boundaries();
  passed &= test_look_volume_blend_priorities();
  printf("--- Look volume tests completed. ---\n");
  return passed;
}
