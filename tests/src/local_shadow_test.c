#include "defines.h"

#include "math/mat.h"
#include "vkr_frame_input.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static VkrPointLight local_shadow_test_light(VkrPointLightKind kind,
                                             bool8_t casts_shadow,
                                             float32_t range) {
  return (VkrPointLight){
      .position = vec3_zero(),
      .color = vec3_one(),
      .intensity = 1.0f,
      .constant = 1.0f,
      .linear = 0.0f,
      .quadratic = 0.0f,
      .range = range,
      .direction = vec3_new(0.0f, 0.0f, -1.0f),
      .inner_cone_angle = 0.2f,
      .outer_cone_angle = 0.6f,
      .kind = kind,
      .casts_shadow = casts_shadow,
  };
}

static void assert_near(float32_t actual, float32_t expected) {
  assert(fabsf(actual - expected) < 0.0001f);
}

static Vec3 local_shadow_project(const VkrLocalShadowView *view, Vec3 point) {
  const Vec4 clip = mat4_mul_vec4(view->light_view_projection,
                                  vec4_new(point.x, point.y, point.z, 1.0f));
  assert(fabsf(clip.w) > 0.000001f);
  return vec3_new(clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f,
                  clip.z / clip.w);
}

static bool32_t test_local_shadow_cardinal_centers(void) {
  printf("  Running test_local_shadow_cardinal_centers...\n");
  VkrPointLight light =
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f);
  VkrLocalShadowPassPayload payload = {0};
  vkr_local_shadow_prepare(&light, 1u, 6u, 1024u, &payload);
  assert(payload.view_count == 6u && payload.light_first_view[0] == 1u);

  const Vec3 directions[6] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
  };
  const float32_t near_clip = payload.views[0].light_position_near.w;
  for (uint32_t face = 0u; face < 6u; ++face) {
    const VkrLocalShadowView *view = &payload.views[face];
    const Vec3 near_point = vec3_scale(directions[face], near_clip);
    const Vec3 far_point = vec3_scale(directions[face], light.range);
    const Vec3 near_uvz = local_shadow_project(view, near_point);
    const Vec3 far_uvz = local_shadow_project(view, far_point);
    assert_near(near_uvz.x, 0.5f);
    assert_near(near_uvz.y, 0.5f);
    assert_near(near_uvz.z, 0.0f);
    assert_near(far_uvz.x, 0.5f);
    assert_near(far_uvz.y, 0.5f);
    assert_near(far_uvz.z, 1.0f);
  }
  printf("  test_local_shadow_cardinal_centers PASSED\n");
  return true_v;
}

static bool32_t test_local_shadow_edge_uv_orientation(void) {
  printf("  Running test_local_shadow_edge_uv_orientation...\n");
  VkrPointLight light =
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f);
  VkrLocalShadowPassPayload payload = {0};
  vkr_local_shadow_prepare(&light, 1u, 6u, 1024u, &payload);
  /* Literal rays in front of each face with known right/up components. */
  const float32_t radius = 1.0f;
  const float32_t tangent = 0.5f;
  const Vec3 edge_points[6] = {
      {radius, tangent, 0},  {-radius, -tangent, 0}, {tangent, radius, 0},
      {tangent, -radius, 0}, {tangent, 0, radius},   {tangent, 0, -radius},
  };
  const float32_t expected_u[6] = {0.5f, 0.5f, 0.75f, 0.25f, 0.25f, 0.75f};
  const float32_t expected_v[6] = {0.25f, 0.75f, 0.5f, 0.5f, 0.5f, 0.5f};
  for (uint32_t face = 0u; face < 6u; ++face) {
    const Vec3 uvz =
        local_shadow_project(&payload.views[face], edge_points[face]);
    assert_near(uvz.x, expected_u[face]);
    assert_near(uvz.y, expected_v[face]);
  }
  printf("  test_local_shadow_edge_uv_orientation PASSED\n");
  return true_v;
}

static bool32_t test_local_shadow_budget_is_complete(void) {
  printf("  Running test_local_shadow_budget_is_complete...\n");
  VkrPointLight lights[3] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_SPOT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
  };
  VkrLocalShadowPassPayload payload = {0};
  vkr_local_shadow_prepare(lights, 3u, 7u, 1024u, &payload);
  assert(payload.view_count == 7u);
  assert(payload.light_first_view[0] == 1u);
  assert(payload.light_first_view[1] == 7u);
  assert(payload.light_first_view[2] == 0u);
  printf("  test_local_shadow_budget_is_complete PASSED\n");
  return true_v;
}

static bool32_t test_local_shadow_rejects_disabled_and_invalid(void) {
  printf("  Running test_local_shadow_rejects_disabled_and_invalid...\n");
  VkrPointLight lights[4] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, false_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 0.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, NAN),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
  };
  lights[3].position.x = NAN;
  VkrLocalShadowPassPayload payload = {0};
  vkr_local_shadow_prepare(lights, 4u, 6u, 1024u, &payload);
  assert(payload.view_count == 0u);
  assert(payload.light_first_view[0] == 0u &&
         payload.light_first_view[3] == 0u);
  printf("  test_local_shadow_rejects_disabled_and_invalid PASSED\n");
  return true_v;
}

static bool32_t test_local_shadow_map_size_and_zero_config(void) {
  printf("  Running test_local_shadow_map_size_and_zero_config...\n");
  VkrPointLight light =
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f);
  const uint32_t sizes[] = {128u, 256u, 1024u};
  for (uint32_t i = 0u; i < ArrayCount(sizes); ++i) {
    VkrLocalShadowPassPayload payload = {0};
    vkr_local_shadow_prepare(&light, 1u, 6u, sizes[i], &payload);
    assert(payload.view_count == 6u);
    assert_near(payload.views[0].projection_params.y,
                1.0f / (float32_t)sizes[i]);
    assert_near(payload.views[0].atlas_rect.z,
                (float32_t)sizes[i] / (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE);
  }
  /* Faces are atlas squares, so sizes must be powers of two in range. */
  const uint32_t invalid_sizes[] = {1u, 64u, 300u, 2048u};
  for (uint32_t i = 0u; i < ArrayCount(invalid_sizes); ++i) {
    VkrLocalShadowPassPayload payload = {0};
    vkr_local_shadow_prepare(&light, 1u, 6u, invalid_sizes[i], &payload);
    assert(payload.view_count == 0u);
  }
  VkrLocalShadowPassPayload zero_budget = {0};
  vkr_local_shadow_prepare(&light, 1u, 0u, 1024u, &zero_budget);
  assert(zero_budget.view_count == 0u);
  VkrLocalShadowPassPayload zero_map = {0};
  vkr_local_shadow_prepare(&light, 1u, 6u, 0u, &zero_map);
  assert(zero_map.view_count == 0u);
  printf("  test_local_shadow_map_size_and_zero_config PASSED\n");
  return true_v;
}

/* Three point lights at the largest face need 18 squares of 1024, more than
 * the 16 the atlas holds, so one light shrinks; every face must still land in
 * a disjoint, aligned square inside the atlas. */
static bool32_t test_local_shadow_atlas_faces_are_disjoint(void) {
  printf("  Running test_local_shadow_atlas_faces_are_disjoint...\n");
  VkrPointLight lights[3] = {
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
      local_shadow_test_light(VKR_POINT_LIGHT_KIND_GLTF_POINT, true_v, 4.0f),
  };
  VkrLocalShadowPassPayload payload = {0};
  vkr_local_shadow_prepare(lights, 3u, 18u, 1024u, &payload);
  assert(payload.view_count == 18u);

  const float32_t atlas = (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE;
  uint64_t texels = 0u;
  for (uint32_t i = 0u; i < payload.view_count; ++i) {
    const Vec4 a = payload.views[i].atlas_rect;
    const float32_t size = a.z * atlas;
    assert(size >= (float32_t)VKR_LOCAL_SHADOW_FACE_SIZE_MIN &&
           size <= 1024.0f);
    assert(fmodf(a.x * atlas, size) == 0.0f &&
           fmodf(a.y * atlas, size) == 0.0f);
    assert(a.x >= 0.0f && a.y >= 0.0f && a.x + a.z <= 1.0f &&
           a.y + a.z <= 1.0f && a.w == 0.0f);
    assert_near(payload.views[i].projection_params.y, 1.0f / size);
    texels += (uint64_t)size * (uint64_t)size;
    for (uint32_t j = 0u; j < i; ++j) {
      const Vec4 b = payload.views[j].atlas_rect;
      const bool8_t apart = a.x + a.z <= b.x || b.x + b.z <= a.x ||
                            a.y + a.z <= b.y || b.y + b.z <= a.y;
      assert(apart);
    }
  }
  assert(texels <=
         (uint64_t)VKR_LOCAL_SHADOW_ATLAS_SIZE * VKR_LOCAL_SHADOW_ATLAS_SIZE);
  assert(payload.views[0].atlas_rect.z * atlas == 1024.0f);
  assert(payload.views[17].atlas_rect.z * atlas == 512.0f);
  printf("  test_local_shadow_atlas_faces_are_disjoint PASSED\n");
  return true_v;
}

bool32_t run_local_shadow_tests(void) {
  printf("--- Running Local Shadow tests... ---\n");
  bool32_t passed = true_v;
  passed &= test_local_shadow_cardinal_centers();
  passed &= test_local_shadow_edge_uv_orientation();
  passed &= test_local_shadow_budget_is_complete();
  passed &= test_local_shadow_rejects_disabled_and_invalid();
  passed &= test_local_shadow_map_size_and_zero_config();
  passed &= test_local_shadow_atlas_faces_are_disjoint();
  return passed;
}
