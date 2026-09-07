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
