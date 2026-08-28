#pragma once

#include <stddef.h>
#include <stdint.h>

static inline uint8_t vkr_vkt_ascii_lower(uint8_t value) {
  return value >= 'A' && value <= 'Z' ? (uint8_t)(value + ('a' - 'A')) : value;
}

static inline int vkr_vkt_name_contains_ci(const char *name, size_t name_length,
                                           const char *token,
                                           size_t token_length) {
  if (token_length > name_length) {
    return 0;
  }
  for (size_t offset = 0; offset <= name_length - token_length; ++offset) {
    size_t index = 0;
    for (; index < token_length; ++index) {
      if (vkr_vkt_ascii_lower((uint8_t)name[offset + index]) !=
          vkr_vkt_ascii_lower((uint8_t)token[index])) {
        break;
      }
    }
    if (index == token_length) {
      return 1;
    }
  }
  return 0;
}

static inline int vkr_vkt_filename_is_normal_rg(const char *name,
                                                size_t name_length) {
  static const char *tokens[] = {"normal", "_n.", "norm", "ddn", "bump"};
  static const size_t token_lengths[] = {6u, 3u, 4u, 3u, 4u};
  for (size_t index = 0; index < sizeof(tokens) / sizeof(tokens[0]); ++index) {
    if (vkr_vkt_name_contains_ci(name, name_length, tokens[index],
                                 token_lengths[index])) {
      return 1;
    }
  }
  return 0;
}

// BasisU's BC5/EAC_RG11 targets source their second output channel from alpha.
// Retain G for RGBA targets and mirror it to A for two-channel targets.
static inline void vkr_vkt_prepare_normal_rg_for_basis(uint8_t *pixels,
                                                       size_t pixel_count) {
  for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
    uint8_t *pixel = pixels + pixel_index * 4u;
    pixel[3] = pixel[1];
  }
}
