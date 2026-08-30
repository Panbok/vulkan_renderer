#include "lighting_system_tests.h"

#include "renderer/systems/vkr_lighting_system.h"

static VkrPointLight make_gltf_point(uint32_t render_id, Vec3 position,
                                     float32_t range) {
  return (VkrPointLight){
      .position = position,
      .color = vec3_one(),
      .intensity = 10.0f,
      .range = range,
      .direction = vec3_new(0.0f, 0.0f, -1.0f),
      .inner_cone_angle = 0.0f,
      .outer_cone_angle = 0.785398163f,
      .influence_min = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX),
      .influence_max = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX),
      .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
      .render_id = render_id,
  };
}

static bool32_t test_point_light_grid_is_fragment_local(void) {
  printf("  Running test_point_light_grid_is_fragment_local...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = 2u;
  system.point_lights[0] =
      make_gltf_point(1u, vec3_new(-20.0f, 0.0f, 0.0f), 5.0f);
  system.point_lights[1] =
      make_gltf_point(2u, vec3_new(20.0f, 0.0f, 0.0f), 5.0f);

  vkr_lighting_system_build_point_light_grid(&system);
  const VkrPointLightMask left = vkr_lighting_system_point_light_mask_at(
      &system, vec3_new(-20.0f, 0.0f, 0.0f));
  const VkrPointLightMask right = vkr_lighting_system_point_light_mask_at(
      &system, vec3_new(20.0f, 0.0f, 0.0f));

  assert(vkr_lighting_system_point_light_mask_contains(&left, 0u));
  assert(!vkr_lighting_system_point_light_mask_contains(&left, 1u));
  assert(!vkr_lighting_system_point_light_mask_contains(&right, 0u));
  assert(vkr_lighting_system_point_light_mask_contains(&right, 1u));
  assert(system.point_light_grid.cell_count <= VKR_POINT_LIGHT_GRID_MAX_CELLS);
  printf("  test_point_light_grid_is_fragment_local PASSED\n");
  return true_v;
}

static bool32_t test_point_light_grid_range_coverage_is_conservative(void) {
  printf("  Running test_point_light_grid_range_coverage_is_conservative...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = 1u;
  system.point_lights[0] = make_gltf_point(1u, vec3_zero(), 5.0f);
  vkr_lighting_system_build_point_light_grid(&system);

  const Vec3 samples[] = {
      vec3_zero(),
      vec3_new(4.99f, 0.0f, 0.0f),
      vec3_new(-4.99f, 0.0f, 0.0f),
      vec3_new(0.0f, 4.99f, 0.0f),
      vec3_new(0.0f, 0.0f, -4.99f),
  };
  for (uint32_t i = 0u; i < ArrayCount(samples); ++i) {
    const VkrPointLightMask mask =
        vkr_lighting_system_point_light_mask_at(&system, samples[i]);
    assert(vkr_lighting_system_point_light_mask_contains(&mask, 0u));
  }
  printf("  test_point_light_grid_range_coverage_is_conservative PASSED\n");
  return true_v;
}

static bool32_t test_point_light_grid_rejects_disjoint_range_aabbs(void) {
  printf("  Running test_point_light_grid_rejects_disjoint_range_aabbs...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = 2u;
  system.point_lights[0] = make_gltf_point(1u, vec3_zero(), 5.0f);
  system.point_lights[1] =
      make_gltf_point(2u, vec3_new(20.0f, 20.0f, 20.0f), 1.0f);
  vkr_lighting_system_build_point_light_grid(&system);

  const VkrPointLightMask diagonal = vkr_lighting_system_point_light_mask_at(
      &system, vec3_new(4.9f, 4.9f, 4.9f));
  assert(!vkr_lighting_system_point_light_mask_contains(&diagonal, 0u));
  printf("  test_point_light_grid_rejects_disjoint_range_aabbs PASSED\n");
  return true_v;
}

static bool32_t test_point_light_grid_represents_full_scene_capacity(void) {
  printf("  Running test_point_light_grid_represents_full_scene_capacity...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = VKR_MAX_SCENE_POINT_LIGHTS;
  for (uint32_t i = 0u; i < system.point_light_count; ++i) {
    system.point_lights[i] = make_gltf_point(
        i + 1u, vec3_new((float32_t)(i % 4u) * 0.1f, 0.0f, 0.0f), 10.0f);
  }
  vkr_lighting_system_build_point_light_grid(&system);

  const VkrPointLightMask mask =
      vkr_lighting_system_point_light_mask_at(&system, vec3_zero());
  for (uint32_t i = 0u; i < VKR_MAX_SCENE_POINT_LIGHTS; ++i) {
    assert(vkr_lighting_system_point_light_mask_contains(&mask, i));
  }
  assert(system.point_light_grid.max_lights_per_cell ==
         VKR_MAX_SCENE_POINT_LIGHTS);
  printf("  test_point_light_grid_represents_full_scene_capacity PASSED\n");
  return true_v;
}

static bool32_t test_unbounded_point_lights_are_global(void) {
  printf("  Running test_unbounded_point_lights_are_global...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = 2u;
  system.point_lights[0] = (VkrPointLight){
      .position = vec3_zero(),
      .color = vec3_one(),
      .intensity = 1.0f,
      .constant = 1.0f,
      .quadratic = 1.0f,
      .influence_min = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX),
      .influence_max = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX),
      .kind = VKR_POINT_LIGHT_KIND_POLYNOMIAL,
      .render_id = 1u,
  };
  system.point_lights[1] =
      make_gltf_point(2u, vec3_new(20.0f, 0.0f, 0.0f), 5.0f);
  vkr_lighting_system_build_point_light_grid(&system);

  const VkrPointLightMask far = vkr_lighting_system_point_light_mask_at(
      &system, vec3_new(-1000.0f, 1000.0f, -1000.0f));
  assert(vkr_lighting_system_point_light_mask_contains(&far, 0u));
  assert(!vkr_lighting_system_point_light_mask_contains(&far, 1u));
  printf("  test_unbounded_point_lights_are_global PASSED\n");
  return true_v;
}

static bool32_t test_point_light_grid_build_is_deterministic(void) {
  printf("  Running test_point_light_grid_build_is_deterministic...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = 3u;
  system.point_lights[0] =
      make_gltf_point(1u, vec3_new(-10.0f, 2.0f, 1.0f), 7.5f);
  system.point_lights[1] =
      make_gltf_point(2u, vec3_new(3.0f, 5.0f, -4.0f), 15.0f);
  system.point_lights[2] =
      make_gltf_point(3u, vec3_new(25.0f, -1.0f, 8.0f), 5.0f);
  vkr_lighting_system_build_point_light_grid(&system);
  const VkrPointLightGrid first = system.point_light_grid;

  vkr_lighting_system_build_point_light_grid(&system);
  assert(MemCompare(&first, &system.point_light_grid, sizeof(first)) == 0);
  printf("  test_point_light_grid_build_is_deterministic PASSED\n");
  return true_v;
}

static bool32_t test_bounded_light_grid_clips_sphere_and_aabb(void) {
  printf("  Running test_bounded_light_grid_clips_sphere_and_aabb...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = 1u;
  system.point_lights[0] = make_gltf_point(1u, vec3_zero(), 5.0f);
  system.point_lights[0].influence_min = vec3_new(-1.0f, -1.0f, -1.0f);
  system.point_lights[0].influence_max = vec3_new(1.0f, 1.0f, 1.0f);
  vkr_lighting_system_build_point_light_grid(&system);

  VkrPointLightMask mask = vkr_lighting_system_point_light_mask_at(
      &system, vec3_new(1.0f, 0.0f, 0.0f));
  assert(vkr_lighting_system_point_light_mask_contains(&mask, 0u));
  mask = vkr_lighting_system_point_light_mask_at(&system,
                                                 vec3_new(3.01f, 0.0f, 0.0f));
  assert(!vkr_lighting_system_point_light_mask_contains(&mask, 0u));
  printf("  test_bounded_light_grid_clips_sphere_and_aabb PASSED\n");
  return true_v;
}

static bool32_t test_bounded_polynomial_light_is_local(void) {
  printf("  Running test_bounded_polynomial_light_is_local...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = 1u;
  system.point_lights[0] = (VkrPointLight){
      .position = vec3_zero(),
      .range = 0.0f,
      .influence_min = vec3_new(-2.0f, -2.0f, -2.0f),
      .influence_max = vec3_new(2.0f, 2.0f, 2.0f),
      .kind = VKR_POINT_LIGHT_KIND_POLYNOMIAL,
  };
  vkr_lighting_system_build_point_light_grid(&system);
  assert(system.point_light_grid.global_light_count == 0u);
  VkrPointLightMask mask = vkr_lighting_system_point_light_mask_at(
      &system, vec3_new(1.0f, 0.0f, 0.0f));
  assert(vkr_lighting_system_point_light_mask_contains(&mask, 0u));
  mask = vkr_lighting_system_point_light_mask_at(&system,
                                                 vec3_new(3.0f, 0.0f, 0.0f));
  assert(!vkr_lighting_system_point_light_mask_contains(&mask, 0u));
  printf("  test_bounded_polynomial_light_is_local PASSED\n");
  return true_v;
}

static bool32_t test_bounded_empty_intersection_has_no_cells(void) {
  printf("  Running test_bounded_empty_intersection_has_no_cells...\n");
  VkrLightingSystem system = {0};
  system.point_light_count = 1u;
  system.point_lights[0] = make_gltf_point(1u, vec3_zero(), 1.0f);
  system.point_lights[0].influence_min = vec3_new(3.0f, 3.0f, 3.0f);
  system.point_lights[0].influence_max = vec3_new(4.0f, 4.0f, 4.0f);
  vkr_lighting_system_build_point_light_grid(&system);
  assert(system.point_light_grid.global_light_count == 0u);
  assert(system.point_light_grid.reference_count == 0u);
  printf("  test_bounded_empty_intersection_has_no_cells PASSED\n");
  return true_v;
}

static bool32_t test_point_light_gpu_row_packing(void) {
  printf("  Running test_point_light_gpu_row_packing...\n");
  const VkrPointLight light = {
      .position = vec3_new(1.0f, 2.0f, 3.0f),
      .color = vec3_new(4.0f, 5.0f, 6.0f),
      .intensity = 7.0f,
      .constant = 8.0f,
      .linear = 9.0f,
      .quadratic = 10.0f,
      .range = 11.0f,
      .direction = vec3_new(12.0f, 13.0f, 14.0f),
      .influence_min = vec3_new(-1.0f, -2.0f, -3.0f),
      .influence_max = vec3_new(15.0f, 16.0f, 17.0f),
      .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
  };
  VkrGpuPointLightRow row = {0};
  vkr_lighting_system_pack_point_light(&light, &row);
  assert(row.p0.x == 1.0f && row.p0.w == 8.0f);
  assert(row.p1.z == 6.0f && row.p1.w == 9.0f);
  assert(row.p2.x == 7.0f && row.p2.y == 10.0f && row.p2.z == 11.0f);
  assert(row.p3.x == 12.0f && row.p3.z == 14.0f);
  assert(row.p4.x == -1.0f && row.p4.z == -3.0f);
  assert(row.p5.x == 15.0f && row.p5.z == 17.0f);
  const VkrGpuAbiRecord *record =
      vkr_gpu_abi_record(VKR_GPU_ABI_POINT_LIGHT_ROW);
  assert(record && record->expected_size == 96u &&
         record->expected_alignment == 16u && record->field_count == 6u);
  printf("  test_point_light_gpu_row_packing PASSED\n");
  return true_v;
}

bool32_t run_lighting_system_tests(void) {
  printf("--- Running Lighting System tests... ---\n");
  bool32_t passed = true_v;
  passed &= test_point_light_grid_is_fragment_local();
  passed &= test_point_light_grid_range_coverage_is_conservative();
  passed &= test_point_light_grid_rejects_disjoint_range_aabbs();
  passed &= test_point_light_grid_represents_full_scene_capacity();
  passed &= test_unbounded_point_lights_are_global();
  passed &= test_point_light_grid_build_is_deterministic();
  passed &= test_bounded_light_grid_clips_sphere_and_aabb();
  passed &= test_bounded_polynomial_light_is_local();
  passed &= test_bounded_empty_intersection_has_no_cells();
  passed &= test_point_light_gpu_row_packing();
  printf("--- Lighting System tests completed. ---\n");
  return passed;
}
