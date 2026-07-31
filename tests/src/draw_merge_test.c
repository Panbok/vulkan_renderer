#include "draw_merge_test.h"

#include "math/vkr_math.h"
#include "renderer/vkr_visibility.h"

#include <assert.h>
#include <stdio.h>

/**
 * The merge measurement decides whether instancing and MDI are worth building.
 * A broken measurement would report "nothing to merge" for a scene full of
 * merge opportunities and silently justify skipping both, so the counter is
 * pinned here against inputs with known answers.
 */

static VkrDrawMergeKey merge_key(uint32_t geom, uint32_t mat, uint32_t first,
                                 uint32_t count) {
  return (VkrDrawMergeKey){
      .geometry = ((uint64_t)geom << 32) | 1u,
      .material = mat,
      .first_index = first,
      .index_count = count,
      .vertex_offset = 0,
      .domain = 0,
  };
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
      merge_key(7, 3, 0, 120), merge_key(1, 1, 0, 10),
      merge_key(7, 3, 0, 120), merge_key(2, 2, 0, 10),
      merge_key(7, 3, 0, 120),
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
 * These tests deliberately avoid asserting which world axis is "in front".
 * mat4_perspective is a +Z-forward (left-handed) projection while mat4_look_at
 * is -Z-forward (right-handed), so the pair's effective view direction is not
 * what VKR_DEFAULT_CAMERA_FORWARD suggests. The frustum is extracted from the
 * same P*V product the shaders use, so it describes the real clip volume; the
 * invariant worth pinning is that the two agree, not which axis they agree on.
 */
static void frustum_test_matrices(Mat4 *out_view, Mat4 *out_proj) {
  *out_view = mat4_look_at(vec3_new(0.0f, 0.0f, 0.0f),
                           vec3_new(0.0f, 0.0f, -1.0f),
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
  printf("  test_frustum_never_rejects_visible_geometry PASSED (%u/%u inside)\n",
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
  uint8_t flags = vkr_visibility_classify(&frustum, NULL, true_v,
                                          vec3_new(10000.0f, 0.0f, 10.0f), 1.0f,
                                          &stats);
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
  Mat4 light_view = mat4_look_at(vec3_new(0.0f, 200.0f, 0.0f),
                                 vec3_new(0.0f, 0.0f, 0.0f),
                                 vec3_new(0.0f, 0.0f, 1.0f));
  Mat4 light_proj =
      mat4_ortho(-20000.0f, 20000.0f, -20000.0f, 20000.0f, -20000.0f, 20000.0f);
  VkrFrustum light = vkr_frustum_from_view_projection(light_view, light_proj);

  VkrVisibilityStats stats = {0};
  uint8_t flags = vkr_visibility_classify(&camera, &light, true_v,
                                          vec3_new(10000.0f, 0.0f, 10.0f), 1.0f,
                                          &stats);
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
  uint8_t flags = vkr_visibility_classify(&camera, &camera, false_v,
                                          vec3_new(99999.0f, 0.0f, 0.0f), 1.0f,
                                          &stats);
  assert((flags & VKR_VISIBLE_CAMERA) != 0);
  assert((flags & VKR_VISIBLE_SHADOW) != 0);
  assert(stats.objects_without_bounds == 1);
  assert(stats.objects_culled_camera == 0);
  printf("  test_visibility_without_bounds_is_conservative PASSED\n");
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
                                               instances);
  assert(written == 6);
  // geom7 x3, geom1 x2, geom2 x1 -> three draws.
  assert(draw_count == 3);

  uint32_t total_instances = 0;
  for (uint32_t d = 0; d < draw_count; ++d) {
    assert(draws[d].instance_count >= 1);
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
    assert(instances[5u + i].object_id == 100u + i);
  }
  printf("  test_unmerged_emission_preserves_order PASSED\n");
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
  vkr_draw_merge_candidates(cands, 3, 0u, draws, &draw_count, instances);
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
  test_merged_runs_have_contiguous_instances();
  test_unmerged_emission_preserves_order();
  test_merge_of_all_distinct_emits_one_draw_each();
  test_frustum_never_rejects_visible_geometry();
  test_frustum_rejects_far_offscreen_geometry();
  test_camera_and_shadow_visibility_are_independent();
  test_visibility_without_bounds_is_conservative();
  test_submesh_sphere_is_conservative_under_scale();

  printf("--- Draw Merge Tests Completed ---\n");
  return true;
}
