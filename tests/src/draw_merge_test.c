#include "draw_merge_test.h"

#include "math/vkr_math.h"
#include "renderer/systems/vkr_camera_controller.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/vkr_visibility.h"

#include <assert.h>
#include <stdio.h>

/**
 * These tests pin the compatibility rules used by instancing/MDI, visibility
 * classification, and the renderer's coordinate convention against inputs
 * with known answers.
 */

static VkrDrawMergeKey merge_key(uint32_t geom, uint32_t mat, uint32_t first,
                                 uint32_t count) {
  return (VkrDrawMergeKey){
      .geometry = ((uint64_t)geom << 32) | 1u,
      .material = ((uint64_t)mat << 32) | 1u,
      .first_index = first,
      .index_count = count,
      .vertex_offset = 0,
      .domain = 0,
  };
}

static void test_alpha_mode_routes_world_and_shadow_independently(void) {
  printf(
      "  Running test_alpha_mode_routes_world_and_shadow_independently...\n");

  VkrDrawAlphaRouting opaque =
      vkr_draw_alpha_routing(VKR_MATERIAL_ALPHA_OPAQUE);
  assert(opaque.world_transparent == false_v);
  assert(opaque.shadow_alpha_tested == false_v);

  VkrDrawAlphaRouting cutout =
      vkr_draw_alpha_routing(VKR_MATERIAL_ALPHA_CUTOUT);
  assert(cutout.world_transparent == false_v);
  assert(cutout.shadow_alpha_tested == true_v);

  VkrDrawAlphaRouting blend = vkr_draw_alpha_routing(VKR_MATERIAL_ALPHA_BLEND);
  assert(blend.world_transparent == true_v);
  assert(blend.shadow_alpha_tested == false_v);

  printf("  test_alpha_mode_routes_world_and_shadow_independently PASSED\n");
}

static void assert_shadow_origin_alignment(Vec3 light_direction, Vec3 eye,
                                           float32_t left, float32_t bottom,
                                           Vec3 world_point) {
  Vec3 dir = vec3_normalize(light_direction);
  Vec3 up_ref = vkr_abs_f32(dir.y) > 0.99f ? vec3_new(0.0f, 0.0f, 1.0f)
                                           : vec3_new(0.0f, 1.0f, 0.0f);
  Vec3 right = vec3_normalize(vec3_cross(up_ref, dir));
  Vec3 up = vec3_cross(dir, right);
  Mat4 view = mat4_look_at(eye, vec3_add(eye, dir), up);
  Vec2 origin = vkr_shadow_light_space_origin_from_view(&view, left, bottom);
  Vec4 view_point = mat4_mul_vec4(view, vec3_to_vec4(world_point, 1.0f));

  float32_t shader_x = vec3_dot(world_point, right) - origin.x;
  float32_t shader_y = vec3_dot(world_point, up) - origin.y;
  assert(vkr_abs_f32(shader_x + (view_point.x - left)) < 0.0001f);
  assert(vkr_abs_f32(shader_y - (view_point.y - bottom)) < 0.0001f);
}

static void test_shadow_origin_matches_shader_basis(void) {
  printf("  Running test_shadow_origin_matches_shader_basis...\n");

  assert_shadow_origin_alignment(vec3_new(0.35f, -0.8f, 0.48f),
                                 vec3_new(12.0f, -7.0f, 3.0f), -9.5f, -4.25f,
                                 vec3_new(-2.0f, 5.0f, 11.0f));
  assert_shadow_origin_alignment(vec3_new(0.01f, -1.0f, 0.02f),
                                 vec3_new(-3.0f, 14.0f, 8.0f), -6.0f, -7.0f,
                                 vec3_new(9.0f, -2.0f, 1.0f));

  printf("  test_shadow_origin_matches_shader_basis PASSED\n");
}

static void test_shadow_scene_bounds_fit_only_relevant_casters(void) {
  printf("  Running test_shadow_scene_bounds_fit_only_relevant_casters...\n");

  VkrShadowSceneBounds bounds = {
      .min = {-4.0f, -2.0f, -4.0f},
      .max = {4.0f, 2.0f, 4.0f},
      .use_scene_bounds = true_v,
  };
  Mat4 view = mat4_euler_rotate_y(vkr_to_radians(45.0f));
  float32_t full_min = 0.0f;
  float32_t full_max = 0.0f;
  assert(vkr_shadow_fit_relevant_caster_z(&view, &bounds, -6.0f, 6.0f, -3.0f,
                                          3.0f, &full_min, &full_max));

  float32_t slice_min = 0.0f;
  float32_t slice_max = 0.0f;
  assert(vkr_shadow_fit_relevant_caster_z(&view, &bounds, 4.5f, 5.5f, -1.0f,
                                          1.0f, &slice_min, &slice_max));
  assert(slice_max - slice_min < full_max - full_min);

  assert(!vkr_shadow_fit_relevant_caster_z(&view, &bounds, 20.0f, 22.0f, -1.0f,
                                           1.0f, &slice_min, &slice_max));
  printf("  test_shadow_scene_bounds_fit_only_relevant_casters PASSED\n");
}

static void test_all_distinct_keys_are_unmergeable(void) {
  printf("  Running test_all_distinct_keys_are_unmergeable...\n");
  VkrDrawMergeKey keys[4] = {
      merge_key(1, 1, 0, 10),
      merge_key(2, 2, 0, 10),
      merge_key(3, 3, 0, 10),
      merge_key(4, 4, 0, 10),
  };
  VkrVisibilityStats stats = {0};
  vkr_draw_measure_merge_opportunity(keys, 4, &stats);
  assert(stats.distinct_opaque_keys == 4);
  assert(stats.mergeable_opaque_draws == 0);
  assert(stats.largest_mergeable_run == 1);
  printf("  test_all_distinct_keys_are_unmergeable PASSED\n");
}

static void test_repeated_asset_is_mergeable(void) {
  printf("  Running test_repeated_asset_is_mergeable...\n");
  // The same submesh of the same asset drawn three times, plus two unrelated
  // draws: exactly the shape instancing exists to collapse.
  VkrDrawMergeKey keys[5] = {
      merge_key(7, 3, 0, 120), merge_key(1, 1, 0, 10),  merge_key(7, 3, 0, 120),
      merge_key(2, 2, 0, 10),  merge_key(7, 3, 0, 120),
  };
  VkrVisibilityStats stats = {0};
  vkr_draw_measure_merge_opportunity(keys, 5, &stats);
  assert(stats.distinct_opaque_keys == 3);
  assert(stats.mergeable_opaque_draws == 2);
  assert(stats.largest_mergeable_run == 3);
  printf("  test_repeated_asset_is_mergeable PASSED\n");
}

static void test_same_geometry_different_range_does_not_merge(void) {
  printf("  Running test_same_geometry_different_range_does_not_merge...\n");
  // Two submeshes of one asset share geometry but occupy different index
  // ranges, so they are separate draws no matter how they are submitted.
  VkrDrawMergeKey keys[2] = {
      merge_key(9, 4, 0, 60),
      merge_key(9, 4, 60, 60),
  };
  VkrVisibilityStats stats = {0};
  vkr_draw_measure_merge_opportunity(keys, 2, &stats);
  assert(stats.distinct_opaque_keys == 2);
  assert(stats.mergeable_opaque_draws == 0);
  printf("  test_same_geometry_different_range_does_not_merge PASSED\n");
}

static void test_same_geometry_different_material_does_not_merge(void) {
  printf("  Running test_same_geometry_different_material_does_not_merge...\n");
  VkrDrawMergeKey keys[2] = {
      merge_key(9, 4, 0, 60),
      merge_key(9, 5, 0, 60),
  };
  VkrVisibilityStats stats = {0};
  vkr_draw_measure_merge_opportunity(keys, 2, &stats);
  assert(stats.distinct_opaque_keys == 2);
  assert(stats.mergeable_opaque_draws == 0);
  printf("  test_same_geometry_different_material_does_not_merge PASSED\n");
}

static void test_empty_input_is_safe(void) {
  printf("  Running test_empty_input_is_safe...\n");
  VkrVisibilityStats stats = {0};
  vkr_draw_measure_merge_opportunity(NULL, 0, &stats);
  assert(stats.distinct_opaque_keys == 0);
  assert(stats.mergeable_opaque_draws == 0);
  printf("  test_empty_input_is_safe PASSED\n");
}

/**
 * Culling that rejects nothing and culling that is broken look identical from a
 * frame-time graph, so the classifier is pinned here.
 *
 * The renderer's canonical convention is right-handed: camera forward is -Z,
 * Vulkan depth is [0,1], and points in front have positive clip W. These tests
 * pin both that declared convention and the independent safety invariant that
 * frustum extraction agrees with raster clip-space classification.
 */
static void frustum_test_matrices(Mat4 *out_view, Mat4 *out_proj) {
  *out_view =
      mat4_look_at(vec3_new(0.0f, 0.0f, 0.0f), vec3_new(0.0f, 0.0f, -1.0f),
                   vec3_new(0.0f, 1.0f, 0.0f));
  *out_proj = mat4_perspective(vkr_to_radians(60.0f), 1.0f, 0.1f, 100.0f);
}

static bool8_t clip_contains_point(Mat4 vp, Vec3 p) {
  Vec4 c = mat4_mul_vec4(vp, vec4_new(p.x, p.y, p.z, 1.0f));
  if (c.w <= 0.0f) {
    return false_v;
  }
  return (c.z >= 0.0f && c.z <= c.w && c.x >= -c.w && c.x <= c.w &&
          c.y >= -c.w && c.y <= c.w)
             ? true_v
             : false_v;
}

static void test_projection_and_view_face_declared_forward(void) {
  printf("  Running test_projection_and_view_face_declared_forward...\n");
  Mat4 view, proj;
  frustum_test_matrices(&view, &proj);
  Mat4 vp = mat4_mul(proj, view);

  Vec4 forward = mat4_mul_vec4(vp, vec4_new(0.0f, 0.0f, -10.0f, 1.0f));
  Vec4 backward = mat4_mul_vec4(vp, vec4_new(0.0f, 0.0f, 10.0f, 1.0f));
  assert(forward.w > 0.0f);
  assert(forward.z >= 0.0f && forward.z <= forward.w);
  assert(backward.w < 0.0f);
  assert(clip_contains_point(vp, vec3_new(0.0f, 0.0f, -10.0f)));
  assert(!clip_contains_point(vp, vec3_new(0.0f, 0.0f, 10.0f)));
  printf("  test_projection_and_view_face_declared_forward PASSED\n");
}

static void test_camera_controller_moves_along_declared_forward(void) {
  printf("  Running test_camera_controller_moves_along_declared_forward...\n");
  VkrCamera camera = {
      .position = vec3_zero(),
      .forward = vec3_forward(),
      .right = vec3_right(),
      .world_up = vec3_up(),
      .speed = 2.0f,
      .sensitivity = 1.0f,
  };
  VkrCameraController controller = {0};
  vkr_camera_controller_create(&controller, &camera, 60.0f);

  vkr_camera_controller_move_forward(&controller, 1.0f);
  vkr_camera_controller_update(&controller, 0.5);

  assert(vkr_abs_f32(camera.position.x) < 0.001f);
  assert(vkr_abs_f32(camera.position.y) < 0.001f);
  assert(vkr_abs_f32(camera.position.z + 1.0f) < 0.001f);
  printf("  test_camera_controller_moves_along_declared_forward PASSED\n");
}

/**
 * The safety property of culling: it must never reject geometry the renderer
 * would have drawn. Anything inside the clip volume must survive the frustum
 * test. The converse is allowed -- a conservative test may keep extra draws.
 */
static void test_frustum_never_rejects_visible_geometry(void) {
  printf("  Running test_frustum_never_rejects_visible_geometry...\n");
  Mat4 view, proj;
  frustum_test_matrices(&view, &proj);
  Mat4 vp = mat4_mul(proj, view);
  VkrFrustum frustum = vkr_frustum_from_view_projection(view, proj);

  uint32_t checked = 0;
  uint32_t inside = 0;
  for (int32_t x = -60; x <= 60; x += 5) {
    for (int32_t y = -60; y <= 60; y += 5) {
      for (int32_t z = -60; z <= 60; z += 5) {
        Vec3 p = vec3_new((float32_t)x, (float32_t)y, (float32_t)z);
        checked++;
        if (!clip_contains_point(vp, p)) {
          continue;
        }
        inside++;
        // A zero-radius sphere at a point the renderer would rasterize must
        // never be culled.
        assert(vkr_frustum_test_sphere(&frustum, p, 0.0f));
      }
    }
  }
  assert(inside > 0);
  printf(
      "  test_frustum_never_rejects_visible_geometry PASSED (%u/%u inside)\n",
      inside, checked);
}

/** The frustum must actually reject something, or culling is a no-op. */
static void test_frustum_rejects_far_offscreen_geometry(void) {
  printf("  Running test_frustum_rejects_far_offscreen_geometry...\n");
  Mat4 view, proj;
  frustum_test_matrices(&view, &proj);
  VkrFrustum frustum = vkr_frustum_from_view_projection(view, proj);
  VkrVisibilityStats stats = {0};

  // Far outside the horizontal field of view, whichever way the camera faces.
  uint8_t flags =
      vkr_visibility_classify(&frustum, NULL, 0u, true_v,
                              vec3_new(10000.0f, 0.0f, 10.0f), 1.0f, &stats);
  assert((flags & VKR_VISIBLE_CAMERA) == 0);
  assert(stats.objects_culled_camera == 1);
  printf("  test_frustum_rejects_far_offscreen_geometry PASSED\n");
}

/**
 * The property P2 item 14 exists for: camera and light visibility are decided
 * independently, so an object the camera cannot see still reaches the shadow
 * caster list. Reusing a camera-culled list for CSM drops its shadow.
 */
static void test_camera_and_shadow_visibility_are_independent(void) {
  printf("  Running test_camera_and_shadow_visibility_are_independent...\n");
  Mat4 view, proj;
  frustum_test_matrices(&view, &proj);
  VkrFrustum camera = vkr_frustum_from_view_projection(view, proj);

  // A light volume wide enough to contain the off-camera point.
  Mat4 light_view =
      mat4_look_at(vec3_new(0.0f, 200.0f, 0.0f), vec3_new(0.0f, 0.0f, 0.0f),
                   vec3_new(0.0f, 0.0f, 1.0f));
  Mat4 light_proj = mat4_ortho_zo_yinv(-20000.0f, 20000.0f, -20000.0f, 20000.0f,
                                       0.1f, 400.0f);
  VkrFrustum light = vkr_frustum_from_view_projection(light_view, light_proj);

  VkrVisibilityStats stats = {0};
  uint8_t flags =
      vkr_visibility_classify(&camera, &light, 1u, true_v,
                              vec3_new(10000.0f, 0.0f, 10.0f), 1.0f, &stats);
  assert((flags & VKR_VISIBLE_CAMERA) == 0);
  assert((flags & VKR_VISIBLE_SHADOW) != 0);
  assert(stats.objects_culled_camera == 1);
  assert(stats.objects_culled_shadow == 0);
  printf("  test_camera_and_shadow_visibility_are_independent PASSED\n");
}

static void test_visibility_without_bounds_is_conservative(void) {
  printf("  Running test_visibility_without_bounds_is_conservative...\n");
  Mat4 view, proj;
  frustum_test_matrices(&view, &proj);
  VkrFrustum camera = vkr_frustum_from_view_projection(view, proj);
  VkrVisibilityStats stats = {0};
  // Bounds not ready: keep it in both lists. Being conservative costs a draw,
  // being wrong drops geometry.
  uint8_t flags =
      vkr_visibility_classify(&camera, &camera, 1u, false_v,
                              vec3_new(99999.0f, 0.0f, 0.0f), 1.0f, &stats);
  assert((flags & VKR_VISIBLE_CAMERA) != 0);
  assert((flags & VKR_VISIBLE_SHADOW) != 0);
  assert(stats.objects_without_bounds == 1);
  assert(stats.objects_culled_camera == 0);
  printf("  test_visibility_without_bounds_is_conservative PASSED\n");
}

static void test_shadow_visibility_is_union_of_cascades(void) {
  printf("  Running test_shadow_visibility_is_union_of_cascades...\n");
  Mat4 projection = mat4_perspective(vkr_to_radians(60.0f), 1.0f, 0.1f, 100.0f);
  Mat4 left_view = mat4_look_at(vec3_new(-20.0f, 0.0f, 0.0f),
                                vec3_new(-20.0f, 0.0f, -1.0f), vec3_up());
  Mat4 right_view = mat4_look_at(vec3_new(20.0f, 0.0f, 0.0f),
                                 vec3_new(20.0f, 0.0f, -1.0f), vec3_up());
  VkrFrustum cascades[2] = {
      vkr_frustum_from_view_projection(left_view, projection),
      vkr_frustum_from_view_projection(right_view, projection),
  };
  VkrVisibilityStats stats = {0};

  // Inside cascade 0 and outside cascade 1. Testing only the last cascade
  // would incorrectly drop this caster.
  Vec3 caster = vec3_new(-20.0f, 0.0f, -10.0f);
  assert(vkr_frustum_test_sphere(&cascades[0], caster, 1.0f));
  assert(!vkr_frustum_test_sphere(&cascades[1], caster, 1.0f));
  uint8_t flags =
      vkr_visibility_classify(NULL, cascades, 2u, true_v, caster, 1.0f, &stats);
  assert((flags & VKR_VISIBLE_SHADOW) != 0);
  assert(stats.objects_culled_shadow == 0);
  printf("  test_shadow_visibility_is_union_of_cascades PASSED\n");
}

static void test_orthographic_frustum_uses_vulkan_depth(void) {
  printf("  Running test_orthographic_frustum_uses_vulkan_depth...\n");
  Mat4 view = mat4_look_at(vec3_zero(), vec3_forward(), vec3_up());
  Mat4 projection =
      mat4_ortho_zo_yinv(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f);
  VkrFrustum frustum = vkr_frustum_from_view_projection(view, projection);

  // Stay just inside the planes: exact zero-radius boundary tests are sensitive
  // to normalization roundoff, while mat_test pins the exact NDC endpoints.
  assert(vkr_frustum_test_sphere(&frustum, vec3_new(0.0f, 0.0f, -0.11f), 0.0f));
  assert(vkr_frustum_test_sphere(&frustum, vec3_new(0.0f, 0.0f, -99.9f), 0.0f));
  assert(!vkr_frustum_test_sphere(&frustum, vec3_new(0.0f, 0.0f, 1.0f), 0.0f));
  assert(
      !vkr_frustum_test_sphere(&frustum, vec3_new(0.0f, 0.0f, -101.0f), 0.0f));
  printf("  test_orthographic_frustum_uses_vulkan_depth PASSED\n");
}

static void test_submesh_sphere_is_conservative_under_scale(void) {
  printf("  Running test_submesh_sphere_is_conservative_under_scale...\n");
  Vec3 center = {0};
  float32_t radius = 0.0f;

  // Unit box, identity transform: half-diagonal of a 2x2x2 box is sqrt(3).
  vkr_visibility_submesh_sphere(mat4_identity(), vec3_new(0.0f, 0.0f, 0.0f),
                                vec3_new(-1.0f, -1.0f, -1.0f),
                                vec3_new(1.0f, 1.0f, 1.0f), &center, &radius);
  assert(radius > 1.73f && radius < 1.74f);

  // Non-uniform scale must expand by the LARGEST axis, never the smallest, or
  // geometry gets clipped away.
  Mat4 scaled = mat4_identity();
  scaled.m11 = 4.0f;
  vkr_visibility_submesh_sphere(scaled, vec3_new(0.0f, 0.0f, 0.0f),
                                vec3_new(-1.0f, -1.0f, -1.0f),
                                vec3_new(1.0f, 1.0f, 1.0f), &center, &radius);
  assert(radius > 6.92f && radius < 6.94f);
  printf("  test_submesh_sphere_is_conservative_under_scale PASSED\n");
}

static VkrDrawCandidate make_candidate(uint32_t geom, uint32_t mat,
                                       uint32_t first, uint32_t count,
                                       uint32_t object_id) {
  VkrDrawCandidate c = {0};
  c.key = merge_key(geom, mat, first, count);
  c.model = mat4_identity();
  c.model.m03 = (float32_t)object_id; // distinguishable per instance
  c.object_id = object_id;
  c.submesh_index = 0;
  return c;
}

/**
 * The property that makes an instanced draw legal: every instance of a merged
 * run must occupy consecutive slots starting at the draw's first_instance.
 * If emission and merging disagree the GPU reads another draw's transforms,
 * which renders plausibly wrong rather than obviously broken.
 */
static void test_merged_runs_have_contiguous_instances(void) {
  printf("  Running test_merged_runs_have_contiguous_instances...\n");
  VkrDrawCandidate cands[6] = {
      make_candidate(7, 3, 0, 120, 10), make_candidate(1, 1, 0, 10, 20),
      make_candidate(7, 3, 0, 120, 11), make_candidate(2, 2, 0, 10, 30),
      make_candidate(7, 3, 0, 120, 12), make_candidate(1, 1, 0, 10, 21),
  };
  VkrDrawItem draws[6] = {0};
  VkrInstanceDataGPU instances[6] = {0};
  uint32_t draw_count = 0;

  uint32_t written = vkr_draw_merge_candidates(cands, 6, 0u, draws, &draw_count,
                                               instances, NULL);
  assert(written == 6);
  // geom7 x3, geom1 x2, geom2 x1 -> three draws.
  assert(draw_count == 3);

  uint32_t total_instances = 0;
  for (uint32_t d = 0; d < draw_count; ++d) {
    assert(draws[d].instance_count >= 1);
    assert(draws[d].material.id != 0u);
    assert(draws[d].material.generation == 1u);
    total_instances += draws[d].instance_count;
    // Each draw's range must lie inside the array and not overlap the next.
    assert(draws[d].first_instance + draws[d].instance_count <= written);
    if (d > 0) {
      assert(draws[d].first_instance ==
             draws[d - 1].first_instance + draws[d - 1].instance_count);
    }
  }
  assert(total_instances == 6);

  // The run of three must exist and its instances must be the three distinct
  // objects that shared the key.
  uint32_t run_of_three = UINT32_MAX;
  for (uint32_t d = 0; d < draw_count; ++d) {
    if (draws[d].instance_count == 3) {
      run_of_three = d;
    }
  }
  assert(run_of_three != UINT32_MAX);
  uint32_t seen = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    uint32_t oid = instances[draws[run_of_three].first_instance + i].object_id;
    assert(oid >= 10 && oid <= 12);
    const VkrInstanceDataGPU *instance =
        &instances[draws[run_of_three].first_instance + i];
    assert(instance->reserved[0] == 0u);
    assert(instance->reserved[1] == 0u);
    assert(instance->reserved[2] == 0u);
    seen |= 1u << (oid - 10);
  }
  assert(seen == 0x7);
  printf("  test_merged_runs_have_contiguous_instances PASSED\n");
}

static void test_unmerged_emission_preserves_order(void) {
  printf("  Running test_unmerged_emission_preserves_order...\n");
  // Transparent draws must keep their sorted order and stay one instance each.
  VkrDrawCandidate cands[3] = {
      make_candidate(1, 1, 0, 10, 100),
      make_candidate(1, 1, 0, 10, 101),
      make_candidate(1, 1, 0, 10, 102),
  };
  VkrDrawItem draws[3] = {0};
  VkrInstanceDataGPU instances[8] = {0};

  uint32_t written = vkr_draw_emit_unmerged(cands, 3, 5u, draws, instances);
  assert(written == 3);
  for (uint32_t i = 0; i < 3; ++i) {
    assert(draws[i].instance_count == 1);
    assert(draws[i].first_instance == 5u + i);
    assert(draws[i].material.id == 1u);
    assert(draws[i].material.generation == 1u);
    assert(instances[5u + i].object_id == 100u + i);
  }
  printf("  test_unmerged_emission_preserves_order PASSED\n");
}

static void test_binding_context_prevents_unsafe_instancing(void) {
  printf("  Running test_binding_context_prevents_unsafe_instancing...\n");
  VkrDrawCandidate cands[2] = {
      make_candidate(7, 3, 0, 120, 10),
      make_candidate(7, 3, 0, 120, 11),
  };
  // Geometry/material/range match, but the draws require different
  // position-dependent reflection-probe descriptor state.
  cands[0].key.binding_context = 1u;
  cands[1].key.binding_context = 2u;

  VkrDrawItem draws[2] = {0};
  VkrInstanceDataGPU instances[2] = {0};
  VkrVisibilityStats stats = {0};
  uint32_t draw_count = 0;
  vkr_draw_merge_candidates(cands, 2, 0u, draws, &draw_count, instances,
                            &stats);
  assert(draw_count == 2);
  assert(stats.mergeable_opaque_draws == 0);
  printf("  test_binding_context_prevents_unsafe_instancing PASSED\n");
}

static void test_merge_of_all_distinct_emits_one_draw_each(void) {
  printf("  Running test_merge_of_all_distinct_emits_one_draw_each...\n");
  VkrDrawCandidate cands[3] = {
      make_candidate(1, 1, 0, 10, 1),
      make_candidate(2, 2, 0, 10, 2),
      make_candidate(3, 3, 0, 10, 3),
  };
  VkrDrawItem draws[3] = {0};
  VkrInstanceDataGPU instances[3] = {0};
  uint32_t draw_count = 0;
  vkr_draw_merge_candidates(cands, 3, 0u, draws, &draw_count, instances, NULL);
  assert(draw_count == 3);
  for (uint32_t i = 0; i < 3; ++i) {
    assert(draws[i].instance_count == 1);
  }
  printf("  test_merge_of_all_distinct_emits_one_draw_each PASSED\n");
}

bool32_t run_draw_merge_tests(void) {
  printf("--- Starting Draw Merge Tests ---\n");

  test_all_distinct_keys_are_unmergeable();
  test_repeated_asset_is_mergeable();
  test_same_geometry_different_range_does_not_merge();
  test_same_geometry_different_material_does_not_merge();
  test_empty_input_is_safe();
  test_alpha_mode_routes_world_and_shadow_independently();
  test_shadow_origin_matches_shader_basis();
  test_shadow_scene_bounds_fit_only_relevant_casters();
  test_merged_runs_have_contiguous_instances();
  test_unmerged_emission_preserves_order();
  test_binding_context_prevents_unsafe_instancing();
  test_merge_of_all_distinct_emits_one_draw_each();
  test_projection_and_view_face_declared_forward();
  test_camera_controller_moves_along_declared_forward();
  test_frustum_never_rejects_visible_geometry();
  test_frustum_rejects_far_offscreen_geometry();
  test_camera_and_shadow_visibility_are_independent();
  test_visibility_without_bounds_is_conservative();
  test_shadow_visibility_is_union_of_cascades();
  test_orthographic_frustum_uses_vulkan_depth();
  test_submesh_sphere_is_conservative_under_scale();

  printf("--- Draw Merge Tests Completed ---\n");
  return true;
}
