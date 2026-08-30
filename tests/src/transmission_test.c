#include "transmission_test.h"

#include "renderer/vkr_transmission.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void transmission_assert_near(Vec3 actual, Vec3 expected) {
  assert(fabsf(actual.x - expected.x) < 1e-6f);
  assert(fabsf(actual.y - expected.y) < 1e-6f);
  assert(fabsf(actual.z - expected.z) < 1e-6f);
}

static void test_transmission_lobe_boundaries(void) {
  printf("  Running test_transmission_lobe_boundaries...\n");
  const VkrTransmissionLobes lobes = {
      .diffuse = {0.4f, 0.3f, 0.2f},
      .specular = {0.1f, 0.15f, 0.2f},
      .emissive = {0.02f, 0.03f, 0.04f},
  };
  const Vec3 background = {0.8f, 0.7f, 0.6f};
  const Vec3 base = {0.5f, 0.75f, 1.0f};
  const Vec3 fresnel = {0.04f, 0.1f, 0.25f};

  transmission_assert_near(
      vkr_transmission_compose(lobes, background, base, fresnel, 0.0f, 0.0f),
      vec3_add(vec3_add(lobes.diffuse, lobes.specular), lobes.emissive));
  transmission_assert_near(
      vkr_transmission_compose(lobes, background, base, fresnel, 1.0f, 1.0f),
      vec3_add(lobes.specular, lobes.emissive));

  const Vec3 expected_full = {
      0.1f + 0.8f * 0.5f * 0.96f + 0.02f,
      0.15f + 0.7f * 0.75f * 0.9f + 0.03f,
      0.2f + 0.6f * 1.0f * 0.75f + 0.04f,
  };
  transmission_assert_near(
      vkr_transmission_compose(lobes, background, base, fresnel, 1.0f, 0.0f),
      expected_full);

  const Vec3 expected_half = {
      0.1f + 0.5f * 0.4f + 0.5f * 0.8f * 0.5f * 0.96f + 0.02f,
      0.15f + 0.5f * 0.3f + 0.5f * 0.7f * 0.75f * 0.9f + 0.03f,
      0.2f + 0.5f * 0.2f + 0.5f * 0.6f * 1.0f * 0.75f + 0.04f,
  };
  transmission_assert_near(
      vkr_transmission_compose(lobes, background, base, fresnel, 0.5f, 0.0f),
      expected_half);
  printf("  test_transmission_lobe_boundaries PASSED\n");
}

static void test_transmission_factor_product(void) {
  printf("  Running test_transmission_factor_product...\n");
  assert(vkr_transmission_resolve_factor(0.0f, 1.0f) == 0.0f);
  assert(vkr_transmission_resolve_factor(1.0f, 0.0f) == 0.0f);
  assert(vkr_transmission_resolve_factor(0.5f, 0.25f) == 0.125f);
  assert(vkr_transmission_resolve_factor(2.0f, 2.0f) == 1.0f);
  printf("  test_transmission_factor_product PASSED\n");
}

static void test_transmission_exit_point_and_projection(void) {
  printf("  Running test_transmission_exit_point_and_projection...\n");
  const Vec3 world = {0.0f, 0.0f, 0.0f};
  const Vec3 camera = {0.0f, 0.0f, 1.0f};
  const Vec3 normal = {0.0f, 0.0f, 1.0f};
  const Vec3 axis_x = {2.0f, 0.0f, 0.0f};
  const Vec3 axis_y = {0.0f, 3.0f, 0.0f};
  const Vec3 axis_z = {0.0f, 0.0f, 4.0f};
  VkrTransmissionExit exit = vkr_transmission_exit_point(
      world, camera, normal, axis_x, axis_y, axis_z, 1.5f, 0.0f);
  transmission_assert_near(exit.position, world);
  assert(exit.path_length == 0.0f);

  exit = vkr_transmission_exit_point(world, camera, normal, axis_x, axis_y,
                                     axis_z, 1.5f, 0.5f);
  transmission_assert_near(exit.direction, vec3_new(0.0f, 0.0f, -1.0f));
  transmission_assert_near(exit.position, vec3_new(0.0f, 0.0f, -2.0f));
  assert(fabsf(exit.path_length - 2.0f) < 1e-6f);

  const VkrTransmissionExit identity_scale = vkr_transmission_exit_point(
      world, vec3_new(-1.0f, 0.0f, 1.0f), normal, vec3_new(1.0f, 0.0f, 0.0f),
      vec3_new(0.0f, 1.0f, 0.0f), vec3_new(0.0f, 0.0f, 1.0f), 1.5f, 1.0f);
  const VkrTransmissionExit stretched = vkr_transmission_exit_point(
      world, vec3_new(-1.0f, 0.0f, 1.0f), normal, vec3_new(3.0f, 0.0f, 0.0f),
      vec3_new(0.0f, 1.0f, 0.0f), vec3_new(0.0f, 0.0f, 1.0f), 1.5f, 1.0f);
  assert(stretched.direction.x > 0.0f && stretched.direction.z < 0.0f);
  assert(stretched.path_length > identity_scale.path_length);

  const Vec4 clip = {1.0f, -1.0f, 0.0f, 2.0f};
  const Vec2 vulkan_uv = vkr_transmission_project_uv(clip, false_v);
  const Vec2 metal_uv = vkr_transmission_project_uv(clip, true_v);
  assert(fabsf(vulkan_uv.x - 0.75f) < 1e-6f &&
         fabsf(vulkan_uv.y - 0.25f) < 1e-6f);
  assert(fabsf(metal_uv.x - 0.75f) < 1e-6f &&
         fabsf(metal_uv.y - 0.75f) < 1e-6f);

  const Vec2 zero_w_uv =
      vkr_transmission_project_uv(vec4_new(0.0f, 0.0f, 0.0f, 0.0f), false_v);
  assert(isfinite(zero_w_uv.x) && isfinite(zero_w_uv.y));
  assert(zero_w_uv.x == 0.5f && zero_w_uv.y == 0.5f);
  printf("  test_transmission_exit_point_and_projection PASSED\n");
}

static void test_transmission_rough_lod(void) {
  printf("  Running test_transmission_rough_lod...\n");
  assert(vkr_transmission_rough_lod(1.0f, 1.5f, 1u) == 0.0f);
  assert(vkr_transmission_rough_lod(0.0f, 1.5f, 8u) == 0.0f);
  assert(fabsf(vkr_transmission_rough_lod(0.5f, 1.5f, 8u) - 3.5f) < 1e-6f);
  assert(fabsf(vkr_transmission_rough_lod(0.5f, 1.0f, 8u) - (7.0f / 3.0f)) <
         1e-6f);
  assert(vkr_transmission_rough_lod(2.0f, 2.0f, 8u) == 7.0f);
  assert(vkr_transmission_feedback_blend(-1.0f) == 0.0f);
  assert(vkr_transmission_feedback_blend(0.0f) == 0.0f);
  assert(vkr_transmission_feedback_blend(1e-7f) == 1.0f);
  assert(vkr_transmission_feedback_blend(1.0f) == 1.0f);
  printf("  test_transmission_rough_lod PASSED\n");
}

bool32_t run_transmission_tests(void) {
  printf("Running transmission tests...\n");
  test_transmission_lobe_boundaries();
  test_transmission_factor_product();
  test_transmission_exit_point_and_projection();
  test_transmission_rough_lod();
  printf("Transmission tests PASSED\n");
  return true_v;
}
