#include "vkr_ibl_math.h"

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
      // The mantissa carried into the exponent and is zero again.
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

#define VKR_RGB9E5_MANTISSA_BITS 9
#define VKR_RGB9E5_EXPONENT_BIAS 15
#define VKR_RGB9E5_MAX_EXPONENT 31

uint32_t vkr_rgb9e5_from_float3(float32_t r, float32_t g, float32_t b) {
  /* (2^9 - 1) / 2^9 * 2^(31 - 15). */
  const float64_t largest = 65408.0;
  const float64_t channels[3] = {r, g, b};
  float64_t clamped[3];
  float64_t brightest = 0.0;
  for (uint32_t i = 0u; i < 3u; ++i) {
    const float64_t value = channels[i];
    clamped[i] = value > 0.0 ? (value < largest ? value : largest) : 0.0;
    brightest = clamped[i] > brightest ? clamped[i] : brightest;
  }
  if (brightest <= 0.0) {
    return 0u;
  }
  int32_t exponent = (int32_t)floor(log2(brightest));
  if (exponent < -VKR_RGB9E5_EXPONENT_BIAS - 1) {
    exponent = -VKR_RGB9E5_EXPONENT_BIAS - 1;
  }
  exponent += 1 + VKR_RGB9E5_EXPONENT_BIAS;
  float64_t scale = ldexp(1.0, exponent - VKR_RGB9E5_EXPONENT_BIAS -
                                   VKR_RGB9E5_MANTISSA_BITS);
  /* Rounding the brightest channel up to 2^9 needs the next exponent. */
  if (floor(brightest / scale + 0.5) >= (float64_t)(1u << VKR_RGB9E5_MANTISSA_BITS)) {
    exponent += 1;
    scale *= 2.0;
  }
  if (exponent > VKR_RGB9E5_MAX_EXPONENT) {
    exponent = VKR_RGB9E5_MAX_EXPONENT;
  }
  uint32_t packed = (uint32_t)exponent << 27u;
  for (uint32_t i = 0u; i < 3u; ++i) {
    uint32_t mantissa = (uint32_t)floor(clamped[i] / scale + 0.5);
    mantissa = mantissa > 511u ? 511u : mantissa;
    packed |= mantissa << (9u * i);
  }
  return packed;
}

void vkr_rgb9e5_to_float3(uint32_t packed, float32_t out_rgb[3]) {
  const int32_t exponent = (int32_t)(packed >> 27u);
  const float64_t scale = ldexp(1.0, exponent - VKR_RGB9E5_EXPONENT_BIAS -
                                         VKR_RGB9E5_MANTISSA_BITS);
  for (uint32_t i = 0u; i < 3u; ++i) {
    out_rgb[i] = (float32_t)((float64_t)((packed >> (9u * i)) & 511u) * scale);
  }
}

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
