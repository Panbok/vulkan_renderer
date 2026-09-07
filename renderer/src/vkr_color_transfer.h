#pragma once

#include "defines.h"
#include "math/vec.h"

/** Converts an sRGB display value to linear RGB, clamping to [0, 1]. */
float32_t vkr_srgb_to_linear(float32_t value);

/** Converts a linear RGB value to sRGB display encoding, clamping to [0, 1]. */
float32_t vkr_linear_to_srgb(float32_t value);

/** Decodes sRGB RGB channels while preserving linear alpha. */
Vec4 vkr_srgb_color_to_linear(Vec4 color);
