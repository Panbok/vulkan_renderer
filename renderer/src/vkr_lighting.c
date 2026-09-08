#include "vkr_lighting.h"
#include <math.h>

void vkr_point_light_pack(const VkrPointLight *light,
                          VkrGpuPointLightRow *row) {
  row->p0 = (Vec4){light->position.x, light->position.y, light->position.z,
                   light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                       ? cosf(light->inner_cone_angle)
                       : light->constant};
  row->p1 = (Vec4){light->color.x, light->color.y, light->color.z,
                   light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                       ? cosf(light->outer_cone_angle)
                       : light->linear};
  row->p2 = (Vec4){light->intensity, light->quadratic, light->range,
                   (float32_t)light->kind};
  row->p3 =
      (Vec4){light->direction.x, light->direction.y, light->direction.z, 0.0f};
}

bool8_t vkr_rectangle_light_valid(const VkrRectangleLight *light) {
  const Vec3 vectors[] = {light->position, light->right, light->up, light->color};
  for (uint32_t i = 0u; i < 4u; ++i)
    if (!isfinite(vectors[i].x) || !isfinite(vectors[i].y) ||
        !isfinite(vectors[i].z))
      return false_v;
  return isfinite(light->half_width) && light->half_width > 0.0f &&
         isfinite(light->half_height) && light->half_height > 0.0f &&
         isfinite(light->radiance) && light->radiance >= 0.0f &&
         light->color.x >= 0.0f && light->color.y >= 0.0f && light->color.z >= 0.0f &&
         fabsf(vec3_dot(light->right, light->right) - 1.0f) < 1e-4f &&
         fabsf(vec3_dot(light->up, light->up) - 1.0f) < 1e-4f &&
         fabsf(vec3_dot(light->right, light->up)) < 1e-4f;
}

void vkr_rectangle_light_pack(const VkrRectangleLight *light,
                             VkrGpuRectangleLightRow *row) {
  row->center_half_width = (Vec4){light->position.x, light->position.y,
                                 light->position.z, light->half_width};
  row->right_half_height = (Vec4){light->right.x, light->right.y,
                                 light->right.z, light->half_height};
  row->up_radiance = (Vec4){light->up.x, light->up.y, light->up.z, light->radiance};
  row->color = (Vec4){light->color.x, light->color.y, light->color.z, 0.0f};
}
