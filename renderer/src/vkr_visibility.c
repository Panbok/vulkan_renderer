#include "vkr_visibility.h"

#include "math/vkr_math.h"

uint32_t vkr_draw_parity_run_length(const VkrInstanceDataGPU *instances,
                                    uint32_t count, bool8_t *out_mirrored) {
  *out_mirrored = mat4_affine_mirrored(instances[0].model);
  uint32_t length = 1u;
  while (length < count &&
         mat4_affine_mirrored(instances[length].model) == *out_mirrored)
    ++length;
  return length;
}

uint64_t vkr_world_draw_parity_run_count(const VkrWorldPassPayload *world) {
  uint64_t count = 0u;
  for (uint32_t i = 0u; i < world->transparent_draw_count; ++i) {
    const VkrDrawItem *draw = &world->transparent_draws[i];
    uint32_t offset = 0u;
    while (offset < draw->instance_count) {
      bool8_t mirrored;
      offset += vkr_draw_parity_run_length(
          world->instances + draw->first_instance + offset,
          draw->instance_count - offset, &mirrored);
      ++count;
    }
  }
  return count;
}

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

  Vec3 half = vec3_scale(vec3_sub(max_extents, min_extents), 0.5f);
  *out_radius = vec3_length(half) * mat4_affine_sphere_scale(model);
}
