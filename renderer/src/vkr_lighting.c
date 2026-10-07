#include "vkr_lighting.h"
#include <math.h>

void vkr_point_light_pack(const VkrPointLight *light, float32_t radiance_scale,
                          VkrGpuPointLightRow *row) {
  row->p0 = (Vec4){light->position.x, light->position.y, light->position.z,
                   light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                       ? cosf(light->inner_cone_angle)
                       : light->constant};
  row->p1 = (Vec4){light->color.x, light->color.y, light->color.z,
                   light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                       ? cosf(light->outer_cone_angle)
                       : light->linear};
  row->p2 = (Vec4){light->intensity * radiance_scale, light->quadratic,
                   light->range, (float32_t)light->kind};
  row->p3 =
      (Vec4){light->direction.x, light->direction.y, light->direction.z, 0.0f};
}

bool8_t vkr_rectangle_light_valid(const VkrRectangleLight *light) {
  const Vec3 vectors[] = {light->position, light->right, light->up,
                          light->color};
  for (uint32_t i = 0u; i < 4u; ++i)
    if (!isfinite(vectors[i].x) || !isfinite(vectors[i].y) ||
        !isfinite(vectors[i].z))
      return false_v;
  return isfinite(light->half_width) && light->half_width > 0.0f &&
         isfinite(light->half_height) && light->half_height > 0.0f &&
         isfinite(light->radiance) && light->radiance >= 0.0f &&
         light->color.x >= 0.0f && light->color.y >= 0.0f &&
         light->color.z >= 0.0f &&
         fabsf(vec3_dot(light->right, light->right) - 1.0f) < 1e-4f &&
         fabsf(vec3_dot(light->up, light->up) - 1.0f) < 1e-4f &&
         fabsf(vec3_dot(light->right, light->up)) < 1e-4f;
}

void vkr_rectangle_light_pack(const VkrRectangleLight *light,
                              float32_t radiance_scale,
                              VkrGpuRectangleLightRow *row) {
  row->center_half_width = (Vec4){light->position.x, light->position.y,
                                  light->position.z, light->half_width};
  row->right_half_height = (Vec4){light->right.x, light->right.y,
                                  light->right.z, light->half_height};
  row->up_radiance = (Vec4){light->up.x, light->up.y, light->up.z,
                            light->radiance * radiance_scale};
  row->color = (Vec4){light->color.x, light->color.y, light->color.z, 0.0f};
}

static uint32_t world_grid_dimension(float32_t extent, float32_t cell_size) {
  return Max((uint32_t)ceilf(Max(extent, 0.0f) / cell_size), 1u);
}

static uint64_t world_grid_dimensions_for_size(Vec3 extent, float32_t cell_size,
                                               uint32_t dimensions[3]) {
  dimensions[0] = world_grid_dimension(extent.x, cell_size);
  dimensions[1] = world_grid_dimension(extent.y, cell_size);
  dimensions[2] = world_grid_dimension(extent.z, cell_size);
  return (uint64_t)dimensions[0] * (uint64_t)dimensions[1] *
         (uint64_t)dimensions[2];
}

uint32_t vkr_world_grid_fit(Vec3 extent, float32_t min_cell_size,
                            uint32_t max_cells, float32_t *out_cell_size,
                            uint32_t out_dimensions[3]) {
  float32_t cell_size = min_cell_size;
  uint64_t cell_count =
      world_grid_dimensions_for_size(extent, cell_size, out_dimensions);
  if (cell_count > max_cells) {
    // Assigned by the first growth step, which always runs.
    float32_t rejected_size;
    do {
      rejected_size = cell_size;
      cell_size *= 1.25f;
      cell_count =
          world_grid_dimensions_for_size(extent, cell_size, out_dimensions);
    } while (cell_count > max_cells);

    // The coarse growth step only brackets the answer. Bisection chooses the
    // densest representable grid so a quantization jump cannot leave a large
    // part of the fixed cell budget unused.
    for (uint32_t iteration = 0u; iteration < 16u; ++iteration) {
      const float32_t candidate_size = (rejected_size + cell_size) * 0.5f;
      uint32_t candidate_dimensions[3];
      const uint64_t candidate_count = world_grid_dimensions_for_size(
          extent, candidate_size, candidate_dimensions);
      if (candidate_count > max_cells) {
        rejected_size = candidate_size;
      } else {
        cell_size = candidate_size;
        out_dimensions[0] = candidate_dimensions[0];
        out_dimensions[1] = candidate_dimensions[1];
        out_dimensions[2] = candidate_dimensions[2];
        cell_count = candidate_count;
      }
    }
  }
  *out_cell_size = cell_size;
  return (uint32_t)cell_count;
}
