#include "vkr_anisotropy_lut.h"
#include <math.h>

const uint16_t vkr_anisotropy_lut_pixels[VKR_ANISOTROPY_LUT_TABLE_COUNT]
                                        [VKR_ANISOTROPY_LUT_TABLE_TEXEL_COUNT *
                                         VKR_ANISOTROPY_LUT_CHANNEL_COUNT] = {
#include "vkr_anisotropy_lut_data.inc"
};

_Static_assert(sizeof(vkr_anisotropy_lut_pixels) == 6u * 1024u * 1024u,
               "Anisotropy tables must remain three 64x64x64 RGBA16F arrays");

static float decode_half(uint16_t h) {
  const uint32_t exponent = (h >> 10u) & 31u;
  const float magnitude =
      exponent ? ldexpf(1.0f + (h & 1023u) / 1024.0f, (int)exponent - 15)
               : ldexpf((float)(h & 1023u), -24);
  return h & 0x8000u ? -magnitude : magnitude;
}

void vkr_anisotropy_dfg_sample(float no_v, float roughness, float strength,
                               float azimuth, float out_ab[2]) {
  const float coordinates[4] = {
      fminf(fmaxf((roughness - 0.04f) / 0.96f, 0.0f), 1.0f) * 63.0f,
      (1.0f - sqrtf(fminf(fmaxf(no_v, 0.0001f), 1.0f))) * (63.0f / 0.99f),
      fminf(fmaxf(strength, 0.0f), 1.0f) * 7.0f,
      fminf(fmaxf(azimuth * 0.63661977236758134f, 0.0f), 1.0f) * 7.0f};
  const uint32_t maxima[4] = {63u, 63u, 7u, 7u};
  uint32_t low[4], high[4];
  float blend[4];
  for (uint32_t axis = 0; axis < 4u; ++axis) {
    low[axis] = (uint32_t)coordinates[axis];
    high[axis] = low[axis] < maxima[axis] ? low[axis] + 1u : low[axis];
    blend[axis] = coordinates[axis] - (float)low[axis];
  }
  out_ab[0] = out_ab[1] = 0.0f;
  for (uint32_t corner = 0; corner < 16u; ++corner) {
    uint32_t index[4];
    float weight = 1.0f;
    for (uint32_t axis = 0; axis < 4u; ++axis) {
      const uint32_t upper = (corner >> axis) & 1u;
      index[axis] = upper ? high[axis] : low[axis];
      weight *= upper ? blend[axis] : 1.0f - blend[axis];
    }
    const uint32_t offset =
        (((index[2] * 8u + index[3]) * 64u + index[1]) * 64u + index[0]) * 4u;
    out_ab[0] +=
        weight * decode_half(vkr_anisotropy_lut_pixels[2][offset + 2u]);
    out_ab[1] +=
        weight * decode_half(vkr_anisotropy_lut_pixels[2][offset + 3u]);
  }
}
