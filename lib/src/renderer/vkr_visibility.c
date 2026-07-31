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

  // Keys are already sorted by geometry then material, so distinct counts fall
  // out of a single linear scan.
  uint32_t geometries = 1;
  uint32_t materials = 1;
  for (uint32_t i = 1; i < count; ++i) {
    if (keys[i].geometry != keys[i - 1].geometry) {
      geometries++;
    }
    if (keys[i].geometry != keys[i - 1].geometry ||
        keys[i].material != keys[i - 1].material) {
      materials++;
    }
  }
  stats->distinct_geometries = geometries;
  stats->distinct_materials = materials;
}


int vkr_draw_candidate_key_compare(const void *lhs, const void *rhs) {
  const VkrDrawCandidate *a = (const VkrDrawCandidate *)lhs;
  const VkrDrawCandidate *b = (const VkrDrawCandidate *)rhs;
  return vkr_draw_merge_key_compare(&a->key, &b->key);
}

int vkr_draw_candidate_depth_compare(const void *lhs, const void *rhs) {
  const VkrDrawCandidate *a = (const VkrDrawCandidate *)lhs;
  const VkrDrawCandidate *b = (const VkrDrawCandidate *)rhs;
  if (a->sort_key > b->sort_key) {
    return -1;
  }
  if (a->sort_key < b->sort_key) {
    return 1;
  }
  return 0;
}

uint32_t vkr_draw_merge_candidates(VkrDrawCandidate *candidates, uint32_t count,
                                   uint32_t instance_base,
                                   VkrDrawItem *out_draws,
                                   uint32_t *out_draw_count,
                                   VkrInstanceDataGPU *out_instances) {
  if (out_draw_count) {
    *out_draw_count = 0;
  }
  if (!candidates || count == 0 || !out_draws || !out_instances) {
    return 0;
  }

  qsort(candidates, count, sizeof(VkrDrawCandidate),
        vkr_draw_candidate_key_compare);

  uint32_t draw_count = 0;
  uint32_t written = 0;
  uint32_t run_start = 0;
  for (uint32_t i = 1; i <= count; ++i) {
    const bool8_t same =
        (i < count) && vkr_draw_merge_key_compare(&candidates[i - 1].key,
                                                  &candidates[i].key) == 0;
    if (same) {
      continue;
    }

    const uint32_t run_length = i - run_start;
    // Instance records are emitted in run order, so the whole run occupies
    // [instance_base + written, ... + run_length) and can be drawn at once.
    out_draws[draw_count] = (VkrDrawItem){
        .mesh = candidates[run_start].mesh,
        .submesh_index = candidates[run_start].submesh_index,
        .material = VKR_MATERIAL_HANDLE_INVALID,
        .instance_count = run_length,
        .first_instance = instance_base + written,
        .sort_key = 0u,
        .pipeline_override = VKR_PIPELINE_HANDLE_INVALID,
    };
    draw_count++;

    for (uint32_t r = run_start; r < i; ++r) {
      out_instances[instance_base + written] = (VkrInstanceDataGPU){
          .model = candidates[r].model,
          .object_id = candidates[r].object_id,
          .material_index = 0,
          .flags = 0,
          ._padding = 0,
      };
      written++;
    }
    run_start = i;
  }

  if (out_draw_count) {
    *out_draw_count = draw_count;
  }
  return written;
}

uint32_t vkr_draw_emit_unmerged(const VkrDrawCandidate *candidates,
                                uint32_t count, uint32_t instance_base,
                                VkrDrawItem *out_draws,
                                VkrInstanceDataGPU *out_instances) {
  if (!candidates || count == 0 || !out_draws || !out_instances) {
    return 0;
  }

  for (uint32_t i = 0; i < count; ++i) {
    out_draws[i] = (VkrDrawItem){
        .mesh = candidates[i].mesh,
        .submesh_index = candidates[i].submesh_index,
        .material = VKR_MATERIAL_HANDLE_INVALID,
        .instance_count = 1,
        .first_instance = instance_base + i,
        .sort_key = candidates[i].sort_key,
        .pipeline_override = VKR_PIPELINE_HANDLE_INVALID,
    };
    out_instances[instance_base + i] = (VkrInstanceDataGPU){
        .model = candidates[i].model,
        .object_id = candidates[i].object_id,
        .material_index = 0,
        .flags = 0,
        ._padding = 0,
    };
  }
  return count;
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
