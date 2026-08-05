#include "renderer/resources/loaders/vkr_gltf_material_conversion.h"

#include <math.h>

#define VKR_GLTF_DIELECTRIC_SPECULAR 0.04f

vkr_internal float32_t vkr_gltf_perceived_brightness(Vec3 value) {
  return sqrtf(0.299f * value.x * value.x + 0.587f * value.y * value.y +
               0.114f * value.z * value.z);
}

vkr_internal Vec3 vkr_gltf_vec3_scale(Vec3 value, float32_t scale) {
  return vec3_new(value.x * scale, value.y * scale, value.z * scale);
}

vkr_internal Vec3 vkr_gltf_vec3_add(Vec3 lhs, Vec3 rhs) {
  return vec3_new(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z);
}

vkr_internal Vec3 vkr_gltf_vec3_lerp(Vec3 lhs, Vec3 rhs, float32_t amount) {
  return vkr_gltf_vec3_add(vkr_gltf_vec3_scale(lhs, 1.0f - amount),
                           vkr_gltf_vec3_scale(rhs, amount));
}

float32_t vkr_gltf_srgb_to_linear(float32_t value) {
  value = Clamp(value, 0.0f, 1.0f);
  return value <= 0.04045f ? value / 12.92f
                           : powf((value + 0.055f) / 1.055f, 2.4f);
}

float32_t vkr_gltf_linear_to_srgb(float32_t value) {
  value = Clamp(value, 0.0f, 1.0f);
  return value <= 0.0031308f ? value * 12.92f
                             : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
}

VkrGltfMetalRoughSample
vkr_gltf_convert_spec_gloss_sample(VkrGltfSpecGlossSample sample) {
  Vec3 diffuse = vec3_new(Clamp(sample.diffuse.x, 0.0f, 1.0f),
                          Clamp(sample.diffuse.y, 0.0f, 1.0f),
                          Clamp(sample.diffuse.z, 0.0f, 1.0f));
  Vec3 specular = vec3_new(Clamp(sample.specular.x, 0.0f, 1.0f),
                           Clamp(sample.specular.y, 0.0f, 1.0f),
                           Clamp(sample.specular.z, 0.0f, 1.0f));
  const float32_t specular_strength =
      Max(specular.x, Max(specular.y, specular.z));
  const float32_t one_minus_specular_strength = 1.0f - specular_strength;
  const float32_t diffuse_brightness = vkr_gltf_perceived_brightness(diffuse);
  const float32_t specular_brightness = vkr_gltf_perceived_brightness(specular);

  float32_t metallic = 0.0f;
  if (specular_brightness >= VKR_GLTF_DIELECTRIC_SPECULAR) {
    const float32_t a = VKR_GLTF_DIELECTRIC_SPECULAR;
    const float32_t b = diffuse_brightness * one_minus_specular_strength /
                            (1.0f - VKR_GLTF_DIELECTRIC_SPECULAR) +
                        specular_brightness -
                        2.0f * VKR_GLTF_DIELECTRIC_SPECULAR;
    const float32_t c = VKR_GLTF_DIELECTRIC_SPECULAR - specular_brightness;
    const float32_t discriminant = Max(b * b - 4.0f * a * c, 0.0f);
    metallic = Clamp((-b + sqrtf(discriminant)) / (2.0f * a), 0.0f, 1.0f);
  }

  const float32_t diffuse_denominator =
      Max((1.0f - VKR_GLTF_DIELECTRIC_SPECULAR) * (1.0f - metallic), 1e-6f);
  Vec3 base_from_diffuse = vkr_gltf_vec3_scale(
      diffuse, one_minus_specular_strength / diffuse_denominator);

  const Vec3 dielectric =
      vec3_new(VKR_GLTF_DIELECTRIC_SPECULAR, VKR_GLTF_DIELECTRIC_SPECULAR,
               VKR_GLTF_DIELECTRIC_SPECULAR);
  Vec3 base_from_specular = vec3_zero();
  if (metallic > 1e-6f) {
    base_from_specular = vkr_gltf_vec3_scale(
        vkr_gltf_vec3_add(specular,
                          vkr_gltf_vec3_scale(dielectric, -(1.0f - metallic))),
        1.0f / metallic);
  }

  Vec3 base = vkr_gltf_vec3_lerp(base_from_diffuse, base_from_specular,
                                 metallic * metallic);
  return (VkrGltfMetalRoughSample){
      .base_color = vec4_new(
          Clamp(base.x, 0.0f, 1.0f), Clamp(base.y, 0.0f, 1.0f),
          Clamp(base.z, 0.0f, 1.0f), Clamp(sample.diffuse.w, 0.0f, 1.0f)),
      .metallic = metallic,
      .roughness = Clamp(1.0f - sample.glossiness, 0.0f, 1.0f),
      .dielectric_specular = metallic <= 1e-6f ? specular : dielectric,
  };
}
