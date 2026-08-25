#include "renderer/vkr_color_transfer.h"

#include <math.h>

float32_t vkr_srgb_to_linear(float32_t value) {
  value = Clamp(value, 0.0f, 1.0f);
  return value <= 0.04045f ? value / 12.92f
                           : powf((value + 0.055f) / 1.055f, 2.4f);
}

float32_t vkr_linear_to_srgb(float32_t value) {
  value = Clamp(value, 0.0f, 1.0f);
  return value <= 0.0031308f ? value * 12.92f
                             : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
}

Vec4 vkr_srgb_color_to_linear(Vec4 color) {
  return vec4_new(vkr_srgb_to_linear(color.x), vkr_srgb_to_linear(color.y),
                  vkr_srgb_to_linear(color.z), color.w);
}
