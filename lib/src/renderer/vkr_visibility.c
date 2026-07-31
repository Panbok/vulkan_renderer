#include "renderer/vkr_visibility.h"

#include "math/vkr_math.h"

#include <stdlib.h>

int vkr_draw_merge_key_compare(const void *lhs, const void *rhs) {
  const VkrDrawMergeKey *a = (const VkrDrawMergeKey *)lhs;
  const VkrDrawMergeKey *b = (const VkrDrawMergeKey *)rhs;
  if (a->geometry != b->geometry) {
    return a->geometry < b->geometry ? -1 : 1;
  }
  if (a->material != b->material) {
    return a->material < b->material ? -1 : 1;
  }
  if (a->first_index != b->first_index) {
    return a->first_index < b->first_index ? -1 : 1;
  }
  if (a->index_count != b->index_count) {
    return a->index_count < b->index_count ? -1 : 1;
  }
  if (a->vertex_offset != b->vertex_offset) {
    return a->vertex_offset < b->vertex_offset ? -1 : 1;
  }
  if (a->domain != b->domain) {
    return a->domain < b->domain ? -1 : 1;
  }
  return 0;
}

void vkr_draw_measure_merge_opportunity(VkrDrawMergeKey *keys, uint32_t count,
                                        VkrVisibilityStats *stats) {
  if (!stats) {
    return;
  }
  if (!keys || count == 0) {
    stats->distinct_opaque_keys = 0;
    stats->mergeable_opaque_draws = 0;
    stats->largest_mergeable_run = 0;
    return;
  }

  qsort(keys, count, sizeof(VkrDrawMergeKey), vkr_draw_merge_key_compare);

  uint32_t distinct = 0;
  uint32_t largest_run = 0;
  uint32_t run = 1;
  for (uint32_t i = 1; i <= count; ++i) {
    const bool8_t same = (i < count) &&
                         vkr_draw_merge_key_compare(&keys[i - 1], &keys[i]) == 0;
    if (same) {
      run++;
      continue;
    }
    distinct++;
    largest_run = run > largest_run ? run : largest_run;
    run = 1;
  }

  stats->distinct_opaque_keys = distinct;
  stats->mergeable_opaque_draws = count - distinct;
  stats->largest_mergeable_run = largest_run;
}

void vkr_visibility_submesh_sphere(Mat4 model, Vec3 center, Vec3 min_extents,
                                   Vec3 max_extents, Vec3 *out_center,
                                   float32_t *out_radius) {
  *out_center = mat4_mul_vec3(model, center);

  Vec3 col0 = vec3_new(model.m00, model.m10, model.m20);
  Vec3 col1 = vec3_new(model.m01, model.m11, model.m21);
  Vec3 col2 = vec3_new(model.m02, model.m12, model.m22);
  const float32_t max_scale = vkr_max_f32(
      vkr_max_f32(vec3_length(col0), vec3_length(col1)), vec3_length(col2));

  Vec3 half = vec3_scale(vec3_sub(max_extents, min_extents), 0.5f);
  *out_radius = vec3_length(half) * max_scale;
}

uint8_t vkr_visibility_classify(const VkrFrustum *camera_frustum,
                                const VkrFrustum *shadow_frustum,
                                bool8_t bounds_valid, Vec3 center,
                                float32_t radius, VkrVisibilityStats *stats) {
  stats->objects_tested++;
  if (!bounds_valid) {
    stats->objects_without_bounds++;
    return (uint8_t)(VKR_VISIBLE_CAMERA | VKR_VISIBLE_SHADOW);
  }

  uint8_t flags = 0;
  if (!camera_frustum ||
      vkr_frustum_test_sphere(camera_frustum, center, radius)) {
    flags |= VKR_VISIBLE_CAMERA;
  } else {
    stats->objects_culled_camera++;
  }

  if (!shadow_frustum ||
      vkr_frustum_test_sphere(shadow_frustum, center, radius)) {
    flags |= VKR_VISIBLE_SHADOW;
  } else {
    stats->objects_culled_shadow++;
  }
  return flags;
}
