#pragma once

#include "defines.h"
#include "math/vec.h"

/**
 * @brief One fully factored specular-glossiness sample in linear space.
 *
 * Diffuse RGB and specular RGB are linear. Diffuse alpha and glossiness are
 * linear scalar channels. Callers multiply glTF factors into sampled texels
 * before conversion.
 */
typedef struct VkrGltfSpecGlossSample {
  Vec4 diffuse;
  Vec3 specular;
  float32_t glossiness;
} VkrGltfSpecGlossSample;

/** @brief Equivalent metallic-roughness sample in linear space. */
typedef struct VkrGltfMetalRoughSample {
  Vec4 base_color;
  float32_t metallic;
  float32_t roughness;
  /** Dielectric F0 retained when the converted sample is non-metallic. */
  Vec3 dielectric_specular;
} VkrGltfMetalRoughSample;

/**
 * @brief Lowers one glTF specular-glossiness sample to metallic-roughness.
 *
 * This is the Khronos reference approximation with a dielectric F0 of 0.04.
 * It is deterministic and allocation-free, so the prepared importer and
 * numeric tests share one conversion authority.
 */
VkrGltfMetalRoughSample
vkr_gltf_convert_spec_gloss_sample(VkrGltfSpecGlossSample sample);
