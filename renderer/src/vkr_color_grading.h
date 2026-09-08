#pragma once

#include "defines.h"
#include "math/vec.h"

/**
 * Per-frame display-grading parameters consumed after exposure and before the
 * display transform. White-balance rows multiply linear scene RGB; every w
 * component is zero so the record has one portable float4-based GPU layout.
 */
typedef struct VKR_SIMD_ALIGN VkrColorGradingGpu {
  Vec4 white_balance_rows[3];
  /** Contrast, saturation, enabled-as-float, reserved. */
  Vec4 controls;
} VkrColorGradingGpu;

_Static_assert(sizeof(VkrColorGradingGpu) == 64u,
               "Color-grading GPU record must occupy four float4 rows");
_Static_assert(AlignOf(VkrColorGradingGpu) == 16u,
               "Color-grading GPU record must retain float4 alignment");

/**
 * Packs validated normalized white balance and display controls. The neutral
 * tuple (0, 0, 1, 1) returns exact identity rows and an exact disabled flag.
 */
VkrColorGradingGpu vkr_color_grading_prepare(float32_t temperature,
                                              float32_t tint,
                                              float32_t contrast,
                                              float32_t saturation);
