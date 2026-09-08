#pragma once

#include <stdint.h>

#define VKR_LTC_LUT_SIZE 64u
#define VKR_LTC_LUT_TABLE_COUNT 2u
#define VKR_LTC_LUT_CHANNEL_COUNT 4u
#define VKR_LTC_LUT_TABLE_TEXEL_COUNT (VKR_LTC_LUT_SIZE * VKR_LTC_LUT_SIZE)
#define VKR_LTC_LUT_TABLE_BYTE_SIZE \
  (VKR_LTC_LUT_TABLE_TEXEL_COUNT * VKR_LTC_LUT_CHANNEL_COUNT * sizeof(uint16_t))

/* Immutable RGBA16F tables. Index 0 holds the fitted inverse LTC matrix
 * entries (m00,m20,m02,m22); index 1 holds GGX norm and Schlick-weighted
 * Fresnel integrals in R and G. Native renderers upload one table per texture. */
extern const uint16_t vkr_ltc_lut_pixels
    [VKR_LTC_LUT_TABLE_COUNT]
    [VKR_LTC_LUT_TABLE_TEXEL_COUNT * VKR_LTC_LUT_CHANNEL_COUNT];
