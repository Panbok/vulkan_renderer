#include "renderer/vkr_transmission.h"

#include "math/vkr_math.h"

Vec3 vkr_transmission_compose(VkrTransmissionLobes lobes, Vec3 background,
                              Vec3 base_color, Vec3 fresnel,
                              float32_t transmission, float32_t metallic) {
  const float32_t resolved_transmission =
      vkr_clamp_f32(transmission, 0.0f, 1.0f);
  const float32_t dielectric = 1.0f - vkr_clamp_f32(metallic, 0.0f, 1.0f);
  const Vec3 transmitted =
      vec3_mul(vec3_mul(background, base_color),
               vec3_new(1.0f - vkr_clamp_f32(fresnel.x, 0.0f, 1.0f),
                        1.0f - vkr_clamp_f32(fresnel.y, 0.0f, 1.0f),
                        1.0f - vkr_clamp_f32(fresnel.z, 0.0f, 1.0f)));
  return vec3_add(
      vec3_add(lobes.specular,
               vec3_scale(lobes.diffuse, 1.0f - resolved_transmission)),
      vec3_add(vec3_scale(transmitted, resolved_transmission * dielectric),
               lobes.emissive));
}

float32_t vkr_transmission_resolve_factor(float32_t factor,
                                          float32_t texture_sample) {
  return vkr_clamp_f32(factor, 0.0f, 1.0f) *
         vkr_clamp_f32(texture_sample, 0.0f, 1.0f);
}

vkr_internal float32_t vkr_transmission_directional_scale(Vec3 direction,
                                                          Vec3 axis_x,
                                                          Vec3 axis_y,
                                                          Vec3 axis_z) {
  const float32_t scale_x = vkr_max_f32(vec3_length(axis_x), 1e-6f);
  const float32_t scale_y = vkr_max_f32(vec3_length(axis_y), 1e-6f);
  const float32_t scale_z = vkr_max_f32(vec3_length(axis_z), 1e-6f);
  const Vec3 local_direction = vec3_new(
      vec3_dot(direction, vec3_scale(axis_x, 1.0f / scale_x)) / scale_x,
      vec3_dot(direction, vec3_scale(axis_y, 1.0f / scale_y)) / scale_y,
      vec3_dot(direction, vec3_scale(axis_z, 1.0f / scale_z)) / scale_z);
  return 1.0f / vkr_max_f32(vec3_length(local_direction), 1e-6f);
}

VkrTransmissionExit
vkr_transmission_exit_point(Vec3 world_position, Vec3 camera_position,
                            Vec3 oriented_normal, Vec3 axis_x, Vec3 axis_y,
                            Vec3 axis_z, float32_t ior, float32_t thickness) {
  const Vec3 incident =
      vec3_normalize(vec3_sub(world_position, camera_position));
  const float32_t eta = 1.0f / vkr_max_f32(ior, 1.0f);
  const float32_t normal_incident = vec3_dot(oriented_normal, incident);
  const float32_t discriminant = vkr_max_f32(
      1.0f - eta * eta * (1.0f - normal_incident * normal_incident), 0.0f);
  const Vec3 direction =
      vec3_sub(vec3_scale(incident, eta),
               vec3_scale(oriented_normal,
                          eta * normal_incident + vkr_sqrt_f32(discriminant)));
  const float32_t path_length =
      vkr_max_f32(thickness, 0.0f) *
      vkr_transmission_directional_scale(direction, axis_x, axis_y, axis_z);
  return (VkrTransmissionExit){
      .position = vec3_add(world_position, vec3_scale(direction, path_length)),
      .direction = direction,
      .path_length = path_length,
  };
}

Vec2 vkr_transmission_project_uv(Vec4 clip, bool8_t flip_y) {
  const float32_t safe_w =
      vkr_copysign_f32(vkr_max_f32(vkr_abs_f32(clip.w), 1e-7f), clip.w);
  const float32_t inverse_w = 1.0f / safe_w;
  const float32_t ndc_x = clip.x * inverse_w;
  const float32_t ndc_y = clip.y * inverse_w;
  return vec2_new(ndc_x * 0.5f + 0.5f,
                  flip_y ? 0.5f - ndc_y * 0.5f : ndc_y * 0.5f + 0.5f);
}

float32_t vkr_transmission_rough_lod(float32_t roughness, float32_t ior,
                                     uint32_t mip_count) {
  const float32_t max_lod = (float32_t)(vkr_max_u32(mip_count, 1u) - 1u);
  const float32_t ior_scale =
      vkr_clamp_f32(vkr_max_f32(ior, 1.0f) / 1.5f, 0.0f, 1.0f);
  return vkr_clamp_f32(vkr_clamp_f32(roughness, 0.0f, 1.0f) * ior_scale *
                           max_lod,
                       0.0f, max_lod);
}

float32_t vkr_transmission_feedback_blend(float32_t roughness) {
  return vkr_ceil_f32(vkr_clamp_f32(roughness, 0.0f, 1.0f));
}
