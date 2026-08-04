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
