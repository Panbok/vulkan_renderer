#include "vkr_frame_input.h"

/* Preparation owns light selection and all projection validation. The payload
 * stays in application frame storage; native uploads borrow it until render
 * returns, then GPU slot completion owns the uploaded bytes. */
void vkr_local_shadow_prepare(const VkrPointLight *lights, uint32_t light_count,
                              uint32_t face_budget, uint32_t map_size,
                              VkrLocalShadowPassPayload *out) {
  MemZero(out, sizeof(*out));
  out->face_budget = Min(face_budget, VKR_LOCAL_SHADOW_FACE_COUNT_MAX);
  out->map_size = map_size;
  if (!lights || !out->face_budget || !map_size || map_size > 1024u)
    return;

  static const Vec3 face_direction[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                         {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  for (uint32_t i = 0; i < Min(light_count, VKR_MAX_SCENE_POINT_LIGHTS); ++i) {
    const VkrPointLight *light = &lights[i];
    const bool8_t spot = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT;
    const uint32_t count = spot ? 1u : 6u;
    if (!light->casts_shadow || !isfinite(light->range) ||
        light->range <= 0.0f || out->view_count + count > out->face_budget)
      continue;
    const float32_t half_fov = spot ? light->outer_cone_angle : 0.78539816339f;
    if (!isfinite(half_fov) || half_fov <= 0.0f || half_fov >= 1.57079632679f ||
        !isfinite(light->position.x) || !isfinite(light->position.y) ||
        !isfinite(light->position.z))
      continue;
    const float32_t direction_length = vec3_length(light->direction);
    if (spot && (!isfinite(direction_length) || direction_length < 0.000001f))
      continue;
    /* Keep the near plane inside every positive range; cap it at 5 cm. */
    const float32_t near_clip = Min(0.05f, light->range * 0.01f);
    const Mat4 projection =
        mat4_perspective(2.0f * half_fov, 1.0f, near_clip, light->range);
    out->light_first_view[i] = out->view_count + 1u;
    for (uint32_t face = 0; face < count; ++face) {
      const Vec3 direction =
          spot ? vec3_normalize(light->direction) : face_direction[face];
      const Vec3 up =
          fabsf(direction.y) > 0.99f ? (Vec3){0, 0, 1} : (Vec3){0, 1, 0};
      const Mat4 view = mat4_look_at(light->position,
                                     vec3_add(light->position, direction), up);
      out->views[out->view_count++] = (VkrLocalShadowView){
          .light_view_projection = mat4_mul(projection, view),
          .light_position_near = {light->position.x, light->position.y,
                                  light->position.z, near_clip},
          .light_direction_far = {direction.x, direction.y, direction.z,
                                  light->range},
          .projection_params = {tanf(half_fov), 1.0f / (float32_t)map_size,
                                1.0f, 2.0f},
      };
    }
  }
}
