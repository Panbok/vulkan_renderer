#include "renderer/vkr_ibl_math.h"

#include "math/vkr_math.h"

#include <math.h>

uint16_t vkr_float32_to_float16(float32_t value) {
  uint32_t bits = 0;
  MemCopy(&bits, &value, sizeof(bits));

  const uint16_t sign = (uint16_t)((bits >> 16u) & 0x8000u);
  const uint32_t exponent = (bits >> 23u) & 0xffu;
  uint32_t mantissa = bits & 0x007fffffu;

  if (exponent == 0xffu) {
    if (mantissa == 0u) {
      return (uint16_t)(sign | 0x7c00u);
    }
    return (uint16_t)(sign | 0x7e00u);
  }

  const int32_t half_exponent = (int32_t)exponent - 127 + 15;
  if (half_exponent >= 31) {
    return (uint16_t)(sign | 0x7c00u);
  }

  if (half_exponent <= 0) {
    if (half_exponent < -10) {
      return sign;
    }

    mantissa |= 0x00800000u;
    const uint32_t shift = (uint32_t)(14 - half_exponent);
    uint32_t rounded = mantissa >> shift;
    const uint32_t remainder_mask = (1u << shift) - 1u;
    const uint32_t remainder = mantissa & remainder_mask;
    const uint32_t halfway = 1u << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (rounded & 1u))) {
      rounded++;
    }
    return (uint16_t)(sign | rounded);
  }

  uint32_t rounded_mantissa = mantissa >> 13u;
  const uint32_t remainder = mantissa & 0x1fffu;
  if (remainder > 0x1000u ||
      (remainder == 0x1000u && (rounded_mantissa & 1u))) {
    rounded_mantissa++;
    if (rounded_mantissa == 0x400u) {
      rounded_mantissa = 0u;
      if (half_exponent + 1 >= 31) {
        return (uint16_t)(sign | 0x7c00u);
      }
      return (uint16_t)(sign | ((uint32_t)(half_exponent + 1) << 10u));
    }
  }

  return (uint16_t)(sign | ((uint32_t)half_exponent << 10u) | rounded_mantissa);
}

float32_t vkr_float16_to_float32(uint16_t value) {
  const bool8_t negative = (value & 0x8000u) != 0;
  const uint32_t exponent = (value >> 10u) & 0x1fu;
  const uint32_t mantissa = value & 0x03ffu;
  float32_t result = 0.0f;
  if (exponent == 0u) {
    result = ldexpf((float32_t)mantissa, -24);
  } else if (exponent == 0x1fu) {
    result = mantissa == 0u ? INFINITY : NAN;
  } else {
    result = ldexpf((float32_t)(1024u + mantissa), (int32_t)exponent - 25);
  }
  return negative ? -result : result;
}

bool8_t
vkr_ibl_derive_cubemap_size(uint32_t equirect_width, uint32_t equirect_height,
                            uint32_t max_cube_extent, uint32_t max_mip_levels,
                            uint32_t *out_face_size, uint32_t *out_mip_count) {
  if (!out_face_size || !out_mip_count || equirect_width == 0u ||
      equirect_height == 0u ||
      (uint64_t)equirect_width != (uint64_t)equirect_height * 2u ||
      max_cube_extent == 0u || max_mip_levels == 0u) {
    return false_v;
  }

  const uint32_t raw_size = Max(1u, equirect_width / 4u);
  const uint32_t limited_size = Min(raw_size, max_cube_extent);
  uint32_t face_size = 1u;
  while (face_size <= limited_size / 2u) {
    face_size *= 2u;
  }

  uint32_t mip_count = 1u;
  for (uint32_t extent = face_size; extent > 1u; extent >>= 1u) {
    mip_count++;
  }
  mip_count = Min(mip_count, max_mip_levels);

  *out_face_size = face_size;
  *out_mip_count = mip_count;
  return true_v;
}

VkrIblUv vkr_ibl_direction_to_equirect_uv(VkrIblDirection direction) {
  const float32_t length =
      sqrtf(direction.x * direction.x + direction.y * direction.y +
            direction.z * direction.z);
  if (!(length > 0.0f) || !isfinite(length)) {
    return (VkrIblUv){0.5f, 0.5f};
  }

  const float32_t inverse_length = 1.0f / length;
  const float32_t x = direction.x * inverse_length;
  const float32_t y = Clamp(direction.y * inverse_length, -1.0f, 1.0f);
  const float32_t z = direction.z * inverse_length;
  return (VkrIblUv){atan2f(z, x) * (0.5f / VKR_PI) + 0.5f, acosf(y) / VKR_PI};
}

VkrIblDirection vkr_ibl_equirect_uv_to_direction(VkrIblUv uv) {
  const float32_t longitude = (uv.u - 0.5f) * VKR_PI_2;
  const float32_t latitude = uv.v * VKR_PI;
  const float32_t sin_latitude = sinf(latitude);
  return (VkrIblDirection){cosf(longitude) * sin_latitude, cosf(latitude),
                           sinf(longitude) * sin_latitude};
}

VkrIblDirection vkr_ibl_cube_face_uv_to_direction(VkrIblCubeFace face,
                                                  VkrIblUv uv) {
  const float32_t s = uv.u * 2.0f - 1.0f;
  const float32_t t = uv.v * 2.0f - 1.0f;
  switch (face) {
  case VKR_IBL_CUBE_FACE_POSITIVE_X:
    return (VkrIblDirection){1.0f, -t, -s};
  case VKR_IBL_CUBE_FACE_NEGATIVE_X:
    return (VkrIblDirection){-1.0f, -t, s};
  case VKR_IBL_CUBE_FACE_POSITIVE_Y:
    return (VkrIblDirection){s, 1.0f, t};
  case VKR_IBL_CUBE_FACE_NEGATIVE_Y:
    return (VkrIblDirection){s, -1.0f, -t};
  case VKR_IBL_CUBE_FACE_POSITIVE_Z:
    return (VkrIblDirection){s, -t, 1.0f};
  case VKR_IBL_CUBE_FACE_NEGATIVE_Z:
    return (VkrIblDirection){-s, -t, -1.0f};
  default:
    return (VkrIblDirection){0.0f, 0.0f, 0.0f};
  }
}

float32_t vkr_ibl_prefilter_source_lod(float32_t no_h, float32_t vo_h,
                                       float32_t roughness,
                                       uint32_t sample_count,
                                       uint32_t source_face_size,
                                       uint32_t source_mip_count) {
  if (source_mip_count <= 1u || source_face_size == 0u || sample_count == 0u ||
      roughness <= 0.001f) {
    return 0.0f;
  }

  const float32_t epsilon = 1e-6f;
  no_h = Clamp(no_h, epsilon, 1.0f);
  vo_h = Clamp(vo_h, epsilon, 1.0f);
  const float32_t alpha = Max(roughness * roughness, epsilon);
  const float32_t alpha2 = alpha * alpha;
  const float32_t denominator = no_h * no_h * (alpha2 - 1.0f) + 1.0f;
  const float32_t d = alpha2 / Max(VKR_PI * denominator * denominator, epsilon);
  const float32_t pdf = d * no_h / Max(4.0f * vo_h, epsilon);
  const float32_t omega_s =
      1.0f / ((float32_t)sample_count * Max(pdf, epsilon));
  const float32_t source_texels =
      6.0f * (float32_t)source_face_size * (float32_t)source_face_size;
  const float32_t omega_p = 4.0f * VKR_PI / source_texels;
  const float32_t lod = 0.5f * log2f(4.0f * omega_s / omega_p);
  if (!isfinite(lod)) {
    return 0.0f;
  }
  return Clamp(lod, 0.0f, (float32_t)(source_mip_count - 1u));
}

/* Real L2 basis normalization constants, in basis order. */
#define VKR_SH_K0 0.28209479177387814f /* 0.5 * sqrt(1/pi)  */
#define VKR_SH_K1 0.48860251190291992f /* 0.5 * sqrt(3/pi)  */
#define VKR_SH_K2 1.09254843059207907f /* 0.5 * sqrt(15/pi) */
#define VKR_SH_K3 0.31539156525252005f /* 0.25 * sqrt(5/pi) */
#define VKR_SH_K4 0.54627421529603953f /* 0.25 * sqrt(15/pi) */

/** Band index of each coefficient in basis order. */
static const uint32_t k_sh_band[VKR_SH_L2_COEFFICIENT_COUNT] = {
    0u, 1u, 1u, 1u, 2u, 2u, 2u, 2u, 2u};

/**
 * Normalized clamped-cosine transfer per band: the raw irradiance factors pi,
 * 2pi/3, and pi/4 divided by pi, so the stored signal is D = E / pi.
 */
static const float32_t k_sh_transfer[3] = {1.0f, 2.0f / 3.0f, 0.25f};

void vkr_ibl_sh_basis(VkrIblDirection direction,
                      float32_t out_basis[VKR_SH_L2_COEFFICIENT_COUNT]) {
  const float32_t x = direction.x;
  const float32_t y = direction.y;
  const float32_t z = direction.z;
  out_basis[0] = VKR_SH_K0;
  out_basis[1] = VKR_SH_K1 * y;
  out_basis[2] = VKR_SH_K1 * z;
  out_basis[3] = VKR_SH_K1 * x;
  out_basis[4] = VKR_SH_K2 * x * y;
  out_basis[5] = VKR_SH_K2 * y * z;
  out_basis[6] = VKR_SH_K3 * (3.0f * z * z - 1.0f);
  out_basis[7] = VKR_SH_K2 * x * z;
  out_basis[8] = VKR_SH_K4 * (x * x - y * y);
}

static float64_t vkr_ibl_area_element(float64_t x, float64_t y) {
  return atan2(x * y, sqrt(x * x + y * y + 1.0));
}

float32_t vkr_ibl_cube_texel_solid_angle(float32_t s0, float32_t s1,
                                         float32_t t0, float32_t t1) {
  return (
      float32_t)(vkr_ibl_area_element(s0, t0) - vkr_ibl_area_element(s0, t1) -
                 vkr_ibl_area_element(s1, t0) + vkr_ibl_area_element(s1, t1));
}

/** sinc_pi(x) = sin(pi * x) / (pi * x), and 1 at x = 0. */
static float32_t vkr_ibl_sinc_pi(float32_t x) {
  if (x == 0.0f) {
    return 1.0f;
  }
  const float32_t argument = VKR_PI * x;
  return sinf(argument) / argument;
}

float32_t vkr_ibl_sh_window_factor(uint32_t band, float32_t deringing) {
  if (deringing == 0.0f) {
    return 1.0f;
  }
  return powf(vkr_ibl_sinc_pi((float32_t)band / 3.0f), deringing);
}

bool8_t vkr_ibl_sh_projection_mip(uint32_t source_face_size,
                                  uint32_t source_mip_count, uint32_t *out_mip,
                                  uint32_t *out_face_size) {
  if (!out_mip || !out_face_size || source_face_size == 0u ||
      source_mip_count == 0u) {
    return false_v;
  }

  uint32_t mip = 0u;
  uint32_t extent = source_face_size;
  while (extent > VKR_SH_PROJECTION_MAX_FACE_SIZE &&
         mip + 1u < source_mip_count) {
    extent >>= 1u;
    mip++;
  }

  *out_mip = mip;
  *out_face_size = Max(1u, extent);
  return true_v;
}

bool8_t vkr_ibl_sh_project_cube(uint32_t face_size, VkrIblCubeSampleFn sample,
                                void *user, VkrShL2Rgb *out_radiance,
                                float32_t *out_total_solid_angle) {
  if (!sample || !out_radiance || face_size == 0u) {
    return false_v;
  }

  float64_t accumulated[VKR_SH_L2_COEFFICIENT_COUNT][3] = {{0.0}};
  float64_t total_solid_angle = 0.0;
  const float64_t inverse_extent = 2.0 / (float64_t)face_size;

  for (uint32_t face = 0u; face < VKR_IBL_CUBE_FACE_COUNT; ++face) {
    for (uint32_t y = 0u; y < face_size; ++y) {
      const float64_t t0 = (float64_t)y * inverse_extent - 1.0;
      const float64_t t1 = t0 + inverse_extent;
      for (uint32_t x = 0u; x < face_size; ++x) {
        const float64_t s0 = (float64_t)x * inverse_extent - 1.0;
        const float64_t s1 = s0 + inverse_extent;
        const float64_t solid_angle =
            vkr_ibl_area_element(s0, t0) - vkr_ibl_area_element(s0, t1) -
            vkr_ibl_area_element(s1, t0) + vkr_ibl_area_element(s1, t1);

        const VkrIblUv uv = {((float32_t)x + 0.5f) / (float32_t)face_size,
                             ((float32_t)y + 0.5f) / (float32_t)face_size};
        VkrIblDirection direction =
            vkr_ibl_cube_face_uv_to_direction((VkrIblCubeFace)face, uv);
        const float32_t length =
            sqrtf(direction.x * direction.x + direction.y * direction.y +
                  direction.z * direction.z);
        const float32_t inverse_length = 1.0f / length;
        direction.x *= inverse_length;
        direction.y *= inverse_length;
        direction.z *= inverse_length;

        float32_t basis[VKR_SH_L2_COEFFICIENT_COUNT];
        vkr_ibl_sh_basis(direction, basis);

        float32_t radiance[3] = {0.0f, 0.0f, 0.0f};
        sample(user, (VkrIblCubeFace)face, x, y, radiance);

        total_solid_angle += solid_angle;
        for (uint32_t i = 0u; i < VKR_SH_L2_COEFFICIENT_COUNT; ++i) {
          const float64_t weight = (float64_t)basis[i] * solid_angle;
          accumulated[i][0] += (float64_t)radiance[0] * weight;
          accumulated[i][1] += (float64_t)radiance[1] * weight;
          accumulated[i][2] += (float64_t)radiance[2] * weight;
        }
      }
    }
  }

  // Correct the accumulated weight back to the exact sphere measure so a
  // partially cancelled texel sum cannot bias every coefficient.
  const float64_t normalization =
      total_solid_angle > 0.0 ? (4.0 * (float64_t)VKR_PI) / total_solid_angle
                              : 0.0;
  for (uint32_t i = 0u; i < VKR_SH_L2_COEFFICIENT_COUNT; ++i) {
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
      out_radiance->c[i][channel] =
          (float32_t)(accumulated[i][channel] * normalization);
    }
  }

  if (out_total_solid_angle) {
    *out_total_solid_angle = (float32_t)total_solid_angle;
  }
  return true_v;
}

void vkr_ibl_sh_apply_diffuse_transfer(VkrShL2Rgb *coefficients,
                                       float32_t deringing) {
  float32_t band_scale[3];
  for (uint32_t band = 0u; band < 3u; ++band) {
    band_scale[band] =
        k_sh_transfer[band] * vkr_ibl_sh_window_factor(band, deringing);
  }
  for (uint32_t i = 0u; i < VKR_SH_L2_COEFFICIENT_COUNT; ++i) {
    const float32_t scale = band_scale[k_sh_band[i]];
    coefficients->c[i][0] *= scale;
    coefficients->c[i][1] *= scale;
    coefficients->c[i][2] *= scale;
  }
}

void vkr_ibl_sh_pack(const VkrShL2Rgb *coefficients,
                     VkrShL2Packed *out_packed) {
  for (uint32_t channel = 0u; channel < 3u; ++channel) {
    float32_t *linear = out_packed->v[channel];
    linear[0] = VKR_SH_K1 * coefficients->c[3][channel];
    linear[1] = VKR_SH_K1 * coefficients->c[1][channel];
    linear[2] = VKR_SH_K1 * coefficients->c[2][channel];
    linear[3] = VKR_SH_K0 * coefficients->c[0][channel] -
                VKR_SH_K3 * coefficients->c[6][channel];

    float32_t *quadratic = out_packed->v[3u + channel];
    quadratic[0] = VKR_SH_K2 * coefficients->c[4][channel];
    quadratic[1] = VKR_SH_K2 * coefficients->c[5][channel];
    quadratic[2] = 3.0f * VKR_SH_K3 * coefficients->c[6][channel];
    quadratic[3] = VKR_SH_K2 * coefficients->c[7][channel];

    out_packed->v[6][channel] = VKR_SH_K4 * coefficients->c[8][channel];
  }
  out_packed->v[6][3] = 0.0f;
}

void vkr_ibl_sh_evaluate(const VkrShL2Rgb *coefficients,
                         VkrIblDirection direction, float32_t out_rgb[3]) {
  float32_t basis[VKR_SH_L2_COEFFICIENT_COUNT];
  vkr_ibl_sh_basis(direction, basis);
  out_rgb[0] = 0.0f;
  out_rgb[1] = 0.0f;
  out_rgb[2] = 0.0f;
  for (uint32_t i = 0u; i < VKR_SH_L2_COEFFICIENT_COUNT; ++i) {
    out_rgb[0] += coefficients->c[i][0] * basis[i];
    out_rgb[1] += coefficients->c[i][1] * basis[i];
    out_rgb[2] += coefficients->c[i][2] * basis[i];
  }
}

void vkr_ibl_sh_evaluate_packed(const VkrShL2Packed *packed,
                                VkrIblDirection direction,
                                float32_t out_rgb[3]) {
  const float32_t x = direction.x;
  const float32_t y = direction.y;
  const float32_t z = direction.z;
  const float32_t linear_operand[4] = {x, y, z, 1.0f};
  const float32_t quadratic_operand[4] = {x * y, y * z, z * z, z * x};
  const float32_t residual = x * x - y * y;

  for (uint32_t channel = 0u; channel < 3u; ++channel) {
    const float32_t *linear = packed->v[channel];
    const float32_t *quadratic = packed->v[3u + channel];
    out_rgb[channel] =
        linear[0] * linear_operand[0] + linear[1] * linear_operand[1] +
        linear[2] * linear_operand[2] + linear[3] * linear_operand[3] +
        quadratic[0] * quadratic_operand[0] +
        quadratic[1] * quadratic_operand[1] +
        quadratic[2] * quadratic_operand[2] +
        quadratic[3] * quadratic_operand[3] + packed->v[6][channel] * residual;
  }
}

void vkr_ibl_sh_clamp_diagnostics(const VkrShL2Packed *packed,
                                  uint32_t latitude_steps,
                                  VkrShL2ClampDiagnostics *out_diagnostics) {
  MemZero(out_diagnostics, sizeof(*out_diagnostics));
  if (latitude_steps == 0u) {
    return;
  }

  const uint32_t longitude_steps = latitude_steps * 2u;
  const float32_t latitude_step = VKR_PI / (float32_t)latitude_steps;
  const float32_t longitude_step = (VKR_PI_2) / (float32_t)longitude_steps;
  float64_t unclamped[3] = {0.0, 0.0, 0.0};
  float64_t clamped[3] = {0.0, 0.0, 0.0};
  float64_t total_weight = 0.0;
  float32_t minimum = 0.0f;

  for (uint32_t i = 0u; i < latitude_steps; ++i) {
    const float32_t theta = ((float32_t)i + 0.5f) * latitude_step;
    const float32_t sin_theta = sinf(theta);
    const float32_t cos_theta = cosf(theta);
    const float64_t weight =
        (float64_t)sin_theta * (float64_t)latitude_step * longitude_step;
    for (uint32_t j = 0u; j < longitude_steps; ++j) {
      const float32_t phi = ((float32_t)j + 0.5f) * longitude_step;
      const VkrIblDirection direction = {sin_theta * cosf(phi), cos_theta,
                                         sin_theta * sinf(phi)};
      float32_t response[3];
      vkr_ibl_sh_evaluate_packed(packed, direction, response);

      bool8_t negative = false_v;
      for (uint32_t channel = 0u; channel < 3u; ++channel) {
        negative |= response[channel] < 0.0f;
        minimum = Min(minimum, response[channel]);
        unclamped[channel] += (float64_t)response[channel] * weight;
        clamped[channel] += (float64_t)Max(response[channel], 0.0f) * weight;
      }

      total_weight += weight;
      out_diagnostics->sample_count++;
      out_diagnostics->negative_sample_count += negative ? 1u : 0u;
    }
  }

  const float64_t inverse_weight =
      total_weight > 0.0 ? 1.0 / total_weight : 0.0;
  for (uint32_t channel = 0u; channel < 3u; ++channel) {
    out_diagnostics->mean_unclamped[channel] =
        (float32_t)(unclamped[channel] * inverse_weight);
    out_diagnostics->mean_clamped[channel] =
        (float32_t)(clamped[channel] * inverse_weight);
  }
  out_diagnostics->min_value = minimum;
}
