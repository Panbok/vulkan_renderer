#pragma once

#include <math.h>
#include <stdint.h>

// Offline moments stay unnormalized between reductions. Byte-encoded normals
// and broadened roughness are output only, never inputs to the next mip.
typedef struct VkrVktMaterialMoment {
  double x, y, z;
  double roughness_fourth;
} VkrVktMaterialMoment;

static inline VkrVktMaterialMoment
vkr_vkt_material_moment(const uint8_t *normal, uint8_t roughness,
                        float normal_scale, float roughness_factor) {
  double x = (double)normal[0] * (2.0 / 255.0) - 1.0;
  double y = (double)normal[1] * (2.0 / 255.0) - 1.0;
  const double z = sqrt(fmax(0.0, 1.0 - x * x - y * y));
  x *= normal_scale;
  y *= normal_scale;
  const double length_squared = x * x + y * y + z * z;
  const double inverse_length =
      length_squared > 1e-12 ? 1.0 / sqrt(length_squared) : 0.0;
  const double r = (double)roughness / 255.0 * roughness_factor;
  // Reflection of tangent Y commutes with filtering, so keep source-image Y
  // here; both native shaders perform their existing Y flip at decode.
  VkrVktMaterialMoment result = {
      x * inverse_length,
      y * inverse_length,
      length_squared > 1e-12 ? z * inverse_length : 1.0,
      r * r * r * r,
  };
  return result;
}

static inline void vkr_vkt_encode_material_moment(VkrVktMaterialMoment moment,
                                                  uint8_t *normal,
                                                  uint8_t *roughness) {
  const double length_squared = fmin(
      1.0, moment.x * moment.x + moment.y * moment.y + moment.z * moment.z);
  const double length = sqrt(length_squared);
  // Approximate vMF concentration from mean normal length. Convert its spread
  // to GGX alpha^2 and use the runtime filter's .25 isotropic variance cap.
  // This is a bounded lobe-width approximation, not exact GGX convolution.
  const double variance =
      length > 0.0 ? fmin(0.25, 2.0 * (1.0 - length_squared) /
                                    (length * (3.0 - length_squared)))
                   : 0.25;
  const double inverse_length = length > 0.0 ? 1.0 / length : 0.0;
  normal[0] = (uint8_t)floor(
      fmin(1.0, fmax(0.0, moment.x * inverse_length * 0.5 + 0.5)) * 255.0 +
      0.5);
  normal[1] = (uint8_t)floor(
      fmin(1.0, fmax(0.0, moment.y * inverse_length * 0.5 + 0.5)) * 255.0 +
      0.5);
  normal[2] = 0u;
  // Basis normal-RG contract carries source G through alpha.
  normal[3] = normal[1];
  *roughness = (uint8_t)floor(
      sqrt(sqrt(fmin(1.0, moment.roughness_fourth + variance))) * 255.0 + 0.5);
}
