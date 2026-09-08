#include "vkr_sheen_lut.h"

const uint16_t
    vkr_sheen_energy_lut_pixels[VKR_SHEEN_ENERGY_LUT_TEXEL_COUNT] = {
#define VKR_SHEEN_LUT_EMIT_ENERGY 1
#include "vkr_sheen_lut_data.inc"
#undef VKR_SHEEN_LUT_EMIT_ENERGY
};

const uint16_t vkr_sheen_ltc_lut_pixels[VKR_SHEEN_LTC_LUT_TABLE_COUNT]
                                        [VKR_SHEEN_LTC_LUT_TABLE_TEXEL_COUNT *
                                         VKR_SHEEN_LTC_LUT_CHANNEL_COUNT] = {
#define VKR_SHEEN_LUT_EMIT_LTC 1
#include "vkr_sheen_lut_data.inc"
#undef VKR_SHEEN_LUT_EMIT_LTC
};

_Static_assert(sizeof(vkr_sheen_energy_lut_pixels) == 128u * 1024u,
               "Sheen energy LUT must remain 256x256 R16F");
_Static_assert(sizeof(vkr_sheen_ltc_lut_pixels) == 128u * 1024u,
               "Sheen LTC LUTs must remain four 64x64 RGBA16F tables");
