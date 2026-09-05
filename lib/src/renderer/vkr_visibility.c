#include "renderer/vkr_visibility.h"

#include "math/vkr_math.h"

int vkr_transparent_draw_depth_compare(const void *lhs, const void *rhs) {
  const VkrTransparentDrawCandidate *a = lhs;
  const VkrTransparentDrawCandidate *b = rhs;
  if (a->sort_key > b->sort_key)
    return -1;
  if (a->sort_key < b->sort_key)
    return 1;
  return 0;
}

uint32_t
vkr_transparent_draw_emit(const VkrTransparentDrawCandidate *candidates,
                          uint32_t count, VkrDrawItem *out_draws,
                          VkrInstanceDataGPU *out_instances) {
  for (uint32_t i = 0; i < count; ++i) {
    out_draws[i] = (VkrDrawItem){
        .mesh = candidates[i].mesh,
        .geometry = candidates[i].geometry,
        .submesh_index = candidates[i].submesh_index,
        .material = candidates[i].material,
        .instance_count = 1u,
        .first_instance = i,
        .sort_key = candidates[i].sort_key,
    };
    out_instances[i] = candidates[i].instance;
    out_instances[i].temporal_flags =
        (candidates[i].submesh_index + 1u) <<
        VKR_INSTANCE_TEMPORAL_SURFACE_SHIFT;
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
