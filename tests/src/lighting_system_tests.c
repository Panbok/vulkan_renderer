#include "lighting_system_tests.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_lighting_system.h"

vkr_internal bool8_t lighting_test_vec3_near(Vec3 left, Vec3 right) {
  return fabsf(left.x - right.x) < 0.00001f &&
         fabsf(left.y - right.y) < 0.00001f &&
         fabsf(left.z - right.z) < 0.00001f;
}

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
      .kind = VKR_POINT_LIGHT_KIND_POLYNOMIAL,
      .render_id = 1u,
  };
  system.point_lights[1] =
      make_gltf_point(2u, vec3_new(20.0f, 0.0f, 0.0f), 5.0f);
  vkr_lighting_system_build_point_light_grid(&system);

  const VkrPointLightMask far_mask = vkr_lighting_system_point_light_mask_at(
      &system, vec3_new(-1000.0f, 1000.0f, -1000.0f));
  assert(vkr_lighting_system_point_light_mask_contains(&far_mask, 0u));
  assert(!vkr_lighting_system_point_light_mask_contains(&far_mask, 1u));
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

static bool32_t test_point_light_gpu_row_packing(void) {
  printf("  Running test_point_light_gpu_row_packing...\n");
  VkrPointLight light = {
      .position = vec3_new(1.0f, 2.0f, 3.0f),
      .color = vec3_new(4.0f, 5.0f, 6.0f),
      .intensity = 7.0f,
      .constant = 8.0f,
      .linear = 9.0f,
      .quadratic = 10.0f,
      .range = 11.0f,
      .direction = vec3_new(12.0f, 13.0f, 14.0f),
      .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
  };
  VkrGpuPointLightRow row = {0};
  vkr_point_light_pack(&light, &row);
  assert(row.p0.x == 1.0f && row.p0.w == 8.0f);
  assert(row.p1.z == 6.0f && row.p1.w == 9.0f);
  assert(row.p2.x == 7.0f && row.p2.y == 10.0f && row.p2.z == 11.0f);
  assert(row.p3.x == 12.0f && row.p3.z == 14.0f && row.p3.w == 0.0f);

  light.kind = VKR_POINT_LIGHT_KIND_GLTF_SPOT;
  light.inner_cone_angle = 0.25f;
  light.outer_cone_angle = 0.75f;
  vkr_point_light_pack(&light, &row);
  assert(fabsf(row.p0.w - cosf(light.inner_cone_angle)) < 0.000001f);
  assert(fabsf(row.p1.w - cosf(light.outer_cone_angle)) < 0.000001f);

  printf("  test_point_light_gpu_row_packing PASSED\n");
  return true_v;
}

/* A scaled parent must move the rectangle's center, but must not distort its
 * authored area or replace the rigid parent/child orientation with world-matrix
 * axes. This independently catches the local-rotation-only regression. */
static bool32_t test_rectangle_light_uses_rigid_parent_rotation(void) {
  printf("  Running test_rectangle_light_uses_rigid_parent_rotation...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 31u, 8u, &error));
  VkrLightingSystem system;
  assert(vkr_lighting_system_init(&system));

  const VkrQuat parent_rotation =
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), 1.57079632679f);
  const VkrQuat child_rotation =
      vkr_quat_from_axis_angle(vec3_new(1.0f, 0.0f, 0.0f), 1.57079632679f);
  VkrEntityId parent = vkr_scene_create_entity(&scene, &error);
  VkrEntityId child = vkr_scene_create_entity(&scene, &error);
  assert(parent.u64 != VKR_ENTITY_ID_INVALID.u64 &&
         child.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(&scene, parent, vec3_new(3.0f, 4.0f, 5.0f),
                                 parent_rotation, vec3_new(2.0f, 3.0f, 4.0f)));
  assert(vkr_scene_set_transform(&scene, child, vec3_new(1.0f, 2.0f, 3.0f),
                                 child_rotation, vec3_new(5.0f, 7.0f, 11.0f)));
  vkr_scene_set_parent(&scene, child, parent);
  assert(vkr_scene_set_rectangle_light(&scene, child,
                                       &(SceneRectangleLight){
                                           .color = vec3_one(),
                                           .radiance = 2.0f,
                                           .size = vec2_new(2.0f, 4.0f),
                                           .enabled = true_v,
                                       }));
  vkr_scene_update(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene);

  assert(system.rectangle_light_count == 1u);
  const VkrRectangleLight *light = &system.rectangle_lights[0];
  // Ry(90) * Rx(90) maps +X to -Z and +Y to +X. Parent TRS maps
  // child translation (1,2,3) to (15,10,3); child scale cannot change it.
  assert(lighting_test_vec3_near(light->right, vec3_new(0.0f, 0.0f, -1.0f)));
  assert(lighting_test_vec3_near(light->up, vec3_new(1.0f, 0.0f, 0.0f)));
  assert(
      lighting_test_vec3_near(light->position, vec3_new(15.0f, 10.0f, 3.0f)));
  assert(light->half_width == 1.0f && light->half_height == 2.0f);

  vkr_lighting_system_shutdown(&system);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_rectangle_light_uses_rigid_parent_rotation PASSED\n");
  return true_v;
}

/* An enabled atmosphere turns the sun light into its sun: the light must not
 * light the frame directly before or besides the published atmosphere sun. */
static bool32_t test_atmosphere_replaces_direct_sun_light(void) {
  printf("  Running test_atmosphere_replaces_direct_sun_light...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 31u, 8u, &error));
  VkrLightingSystem system;
  assert(vkr_lighting_system_init(&system));

  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_directional_light(
      &scene, sun,
      &(SceneDirectionalLight){
          .color = vec3_one(),
          .intensity = 1.0f,
          .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
          .enabled = true_v,
      }));
  vkr_scene_update(&scene, 0.0);
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene);
  assert(system.directional.enabled == true_v);

  VkrAtmosphereSettings atmosphere = vkr_atmosphere_settings_defaults();
  atmosphere.enabled = true_v;
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &atmosphere, &clouds, 0.0f));
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene);
  assert(system.directional.enabled == false_v);

  vkr_lighting_system_shutdown(&system);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_atmosphere_replaces_direct_sun_light PASSED\n");
  return true_v;
}

/* The atmosphere sun light drives an enabled atmosphere: the sun points
 * against the light's rotated direction with its tinted colour times
 * intensity. Other directional lights are ignored, an unchanged light queues
 * no revision, a hard-edged light keeps the authored disc, and without an
 * enabled sun light the authored sun returns. */
static bool32_t test_sun_light_drives_atmosphere_sun(void) {
  printf("  Running test_sun_light_drives_atmosphere_sun...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 31u, 8u, &error));

  VkrAtmosphereSettings authored = vkr_atmosphere_settings_defaults();
  authored.enabled = true_v;
  authored.sun_direction = vec3_new(0.0f, 1.0f, 0.0f);
  authored.sun_angular_diameter_degrees = 0.75f;
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &authored, &clouds, 0.0f));
  const VkrSceneAtmosphere *atmosphere = &scene.atmosphere;
  uint64_t revision = atmosphere->requested_revision;

  // An imported directional light, such as a glTF file's sun, is not one.
  VkrEntityId imported = vkr_scene_create_entity(&scene, &error);
  assert(imported.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_directional_light(
      &scene, imported,
      &(SceneDirectionalLight){
          .intensity = 0.0f,
          .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
          .enabled = true_v,
      }));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(scene.sun.found == false_v);
  assert(atmosphere->requested_revision == revision);

  // A quarter turn about +Y carries the local ray (0, -0.6, -0.8) to
  // (-0.8, -0.6, 0), so the sun sits toward +X at elevation asin(0.6).
  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(
      &scene, sun, vec3_zero(),
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), 1.57079632679f),
      vec3_one()));
  SceneDirectionalLight light = {
      .color = vec3_new(1.0f, 0.5f, 0.25f),
      .intensity = 2.0f,
      .direction_local = vec3_new(0.0f, -0.6f, -0.8f),
      .enabled = true_v,
      .atmosphere_sun = true_v,
  };
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == ++revision);
  const VkrAtmosphereSettings *requested = &atmosphere->requested_settings;
  assert(lighting_test_vec3_near(requested->sun_direction,
                                 vec3_new(0.8f, 0.6f, 0.0f)));
  assert(lighting_test_vec3_near(requested->solar_irradiance,
                                 vec3_new(2.0f, 1.0f, 0.5f)));
  assert(requested->sun_angular_diameter_degrees == 0.75f);

  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == revision);

  // A 3000 K white light keeps its luminance and turns warm.
  light.color = vec3_one();
  light.temperature_kelvin = 3000.0f;
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == ++revision);
  const Vec3 warm = requested->solar_irradiance;
  assert(fabsf(0.2126f * warm.x + 0.7152f * warm.y + 0.0722f * warm.z - 2.0f) <
         1e-3f);
  assert(warm.x > warm.y && warm.y > warm.z);

  light.sun_angular_diameter_degrees = 2.0f;
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == ++revision);
  assert(requested->sun_angular_diameter_degrees == 2.0f);

  light.enabled = false_v;
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(scene.sun.found == false_v);
  assert(atmosphere->requested_revision == ++revision);
  assert(lighting_test_vec3_near(requested->sun_direction,
                                 vec3_new(0.0f, 1.0f, 0.0f)));
  assert(lighting_test_vec3_near(requested->solar_irradiance,
                                 authored.solar_irradiance));
  assert(requested->sun_angular_diameter_degrees == 0.75f);

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_sun_light_drives_atmosphere_sun PASSED\n");
  return true_v;
}

/* A moving sun light draws at once: the frame pairs the published medium with
 * the live sun. Once a revision is published the sky light follows at most
 * once per refresh interval, a request that has not started baking follows the
 * light for free, and the last sun always gets its own revision. */
static bool32_t test_sun_refresh_follows_moving_light(void) {
  printf("  Running test_sun_refresh_follows_moving_light...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 31u, 8u, &error));
  VkrAtmosphereSettings authored = vkr_atmosphere_settings_defaults();
  authored.enabled = true_v;
  authored.sun_direction = vec3_new(0.0f, 1.0f, 0.0f);
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &authored, &clouds, 0.0f));

  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  SceneDirectionalLight light = {
      .color = vkr_atmosphere_settings_defaults().solar_irradiance,
      .intensity = 1.0f,
      .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
      .enabled = true_v,
      .atmosphere_sun = true_v,
  };
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);

  // Publish the first revision with a medium the authored settings lack.
  VkrSceneAtmosphere *atmosphere = &scene.atmosphere;
  atmosphere->active_settings = atmosphere->requested_settings;
  atmosphere->active_settings.rayleigh_density_scale = 2.0f;
  atmosphere->active_revision = atmosphere->requested_revision;
  atmosphere->candidate_revision = 0u;
  uint64_t revision = atmosphere->requested_revision;

  light.direction_local = vec3_new(0.0f, -0.6f, -0.8f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.1);
  assert(atmosphere->requested_revision == revision);
  const VkrAtmosphereSettings frame =
      vkr_scene_atmosphere_frame_settings(&scene);
  assert(
      lighting_test_vec3_near(frame.sun_direction, vec3_new(0.0f, 0.6f, 0.8f)));
  assert(frame.rayleigh_density_scale == 2.0f);
  vkr_scene_sync_sun(&scene, 0.1);
  assert(atmosphere->requested_revision == revision);
  vkr_scene_sync_sun(&scene, 0.1);
  assert(atmosphere->requested_revision == ++revision);
  assert(lighting_test_vec3_near(atmosphere->requested_settings.sun_direction,
                                 vec3_new(0.0f, 0.6f, 0.8f)));

  // Replacing the waiting request costs nothing, so it tracks every frame.
  light.direction_local = vec3_new(0.0f, -0.8f, -0.6f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == ++revision);

  // A baking request holds the next one back until the interval passes.
  atmosphere->candidate_revision = atmosphere->requested_revision;
  light.direction_local = vec3_new(0.0f, -0.5f, -0.866f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.1);
  assert(atmosphere->requested_revision == revision);
  vkr_scene_sync_sun(&scene, 0.2);
  assert(atmosphere->requested_revision == ++revision);
  assert(lighting_test_vec3_near(atmosphere->requested_settings.sun_direction,
                                 vec3_normalize(vec3_new(0.0f, 0.5f, 0.866f))));

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_sun_refresh_follows_moving_light PASSED\n");
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
  passed &= test_atmosphere_replaces_direct_sun_light();
  passed &= test_sun_light_drives_atmosphere_sun();
  passed &= test_sun_refresh_follows_moving_light();
  passed &= test_point_light_grid_build_is_deterministic();
  passed &= test_point_light_gpu_row_packing();
  passed &= test_rectangle_light_uses_rigid_parent_rotation();
  printf("--- Lighting System tests completed. ---\n");
  return passed;
}
