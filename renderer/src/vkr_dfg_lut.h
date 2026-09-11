#pragma once

#include <stdint.h>
#include "shaders/shared/dfg_contract.slangh"

// Immutable RG16F Schlick split-sum coefficients A/B. Rows are perceptual
// roughness, columns sqrt(N.V), both spanning [0,1] at texel centers. Each native
// renderer copies these bytes once into its own completion-retired texture.
#ifdef __cplusplus
extern "C" {
#endif
extern const uint16_t
    vkr_dfg_lut_pixels[VKR_DFG_LUT_SIZE * VKR_DFG_LUT_SIZE * 2u];
#ifdef __cplusplus
}
#endif
