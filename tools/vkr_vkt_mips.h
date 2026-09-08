#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

// Offline RGBA8 filtering. The caller owns disjoint source/destination storage
// and supplies positive extents (at most the packer's 16384-texel limit).
// Destination dimensions are max(1, source / 2). Alpha is always linear.
static inline void
vkr_vkt_downsample_rgba8(const uint8_t *source, uint32_t width, uint32_t height,
                         uint8_t *destination, uint32_t next_width,
                         uint32_t next_height, int srgb, int alpha_weighted) {
  double rgb_decode[256];
  for (uint32_t i = 0; i < 256u; ++i) {
    const double value = (double)i / 255.0;
    rgb_decode[i] = srgb
                        ? (value <= 0.04045 ? value / 12.92
                                            : pow((value + 0.055) / 1.055, 2.4))
                        : value;
  }

  // Coordinates use units of 1/next_extent source texels. Integer overlap
  // weights retain every edge and split shared texels exactly for odd extents.
  const double inverse_area = 1.0 / ((double)width * height);
  for (uint32_t y = 0; y < next_height; ++y) {
    const uint32_t top = y * height;
    const uint32_t bottom = top + height;
    const uint32_t first_y = top / next_height;
    const uint32_t end_y = (bottom + next_height - 1u) / next_height;
    for (uint32_t x = 0; x < next_width; ++x) {
      const uint32_t left = x * width;
      const uint32_t right = left + width;
      const uint32_t first_x = left / next_width;
      const uint32_t end_x = (right + next_width - 1u) / next_width;
      double sum[4] = {0.0, 0.0, 0.0, 0.0};
      for (uint32_t sy = first_y; sy < end_y; ++sy) {
        const uint32_t pixel_top = sy * next_height;
        const uint32_t pixel_bottom = pixel_top + next_height;
        const uint32_t overlap_y =
            (bottom < pixel_bottom ? bottom : pixel_bottom) -
            (top > pixel_top ? top : pixel_top);
        for (uint32_t sx = first_x; sx < end_x; ++sx) {
          const uint32_t pixel_left = sx * next_width;
          const uint32_t pixel_right = pixel_left + next_width;
          const uint32_t overlap_x =
              (right < pixel_right ? right : pixel_right) -
              (left > pixel_left ? left : pixel_left);
          const double weight = (double)overlap_x * overlap_y;
          const uint8_t *pixel = source + ((size_t)sy * width + sx) * 4u;
          const double alpha = (double)pixel[3] / 255.0;
          const double color_weight = weight * (alpha_weighted ? alpha : 1.0);
          for (uint32_t channel = 0; channel < 3u; ++channel) {
            sum[channel] += rgb_decode[pixel[channel]] * color_weight;
          }
          sum[3] += alpha * weight;
        }
      }
      uint8_t *pixel = destination + ((size_t)y * next_width + x) * 4u;
      for (uint32_t channel = 0; channel < 4u; ++channel) {
        double value = sum[channel] * inverse_area;
        if (alpha_weighted && channel < 3u) {
          value = sum[3] > 0.0 ? sum[channel] / sum[3] : 0.0;
        }
        if (srgb && channel < 3u) {
          value = value <= 0.0031308 ? value * 12.92
                                     : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
        }
        pixel[channel] = (uint8_t)floor(value * 255.0 + 0.5);
      }
    }
  }
}

// Threshold and factor are normalized material inputs. Match the inclusive
// alpha test after multiplication by the base-color alpha factor.
static inline uint32_t vkr_vkt_alpha_pass_byte(float cutoff, float factor) {
  uint32_t value = 0u;
  while (value < 256u && ((float)value / 255.0f) * factor < cutoff) {
    ++value;
  }
  return value;
}

static inline uint64_t vkr_vkt_alpha_covered(const uint8_t *pixels,
                                             size_t count, uint32_t pass_byte) {
  uint64_t covered = 0u;
  for (size_t i = 0; i < count; ++i) {
    covered += pixels[i * 4u + 3u] >= pass_byte;
  }
  return covered;
}

// Choose the closest attainable texel-center coverage using one alpha scale.
// Equal alpha bins stay equal; tiny mips cannot represent arbitrary coverage.
// The caller applies this only after generating the unadjusted mip chain.
static inline void vkr_vkt_preserve_alpha_coverage(uint8_t *pixels,
                                                   size_t count,
                                                   uint32_t pass_byte,
                                                   uint64_t base_covered,
                                                   uint64_t base_count) {
  if (pass_byte == 0u || pass_byte == 256u) {
    return;
  }
  uint64_t histogram[256] = {0};
  for (size_t i = 0; i < count; ++i) {
    ++histogram[pixels[i * 4u + 3u]];
  }
  uint64_t covered = 0u;
  for (uint32_t i = pass_byte; i < 256u; ++i) {
    covered += histogram[i];
  }
  const uint64_t target = base_covered * count;
  const uint64_t current = covered * base_count;
  uint64_t best_error = current > target ? current - target : target - current;
  double best_scale = 1.0;
  covered = count - histogram[0];
  for (uint32_t first = 1u; first <= 256u; ++first) {
    const uint64_t candidate = covered * base_count;
    const uint64_t error =
        candidate > target ? candidate - target : target - candidate;
    // Map the boundary between byte bins onto the alpha-test boundary after
    // nearest-byte rounding. Zero alpha remains zero under every scale.
    const double scale = ((double)pass_byte - 0.5) / ((double)first - 0.5);
    if (error < best_error ||
        (error == best_error && fabs(scale - 1.0) < fabs(best_scale - 1.0))) {
      best_error = error;
      best_scale = scale;
    }
    if (first < 256u) {
      covered -= histogram[first];
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const double alpha = floor(pixels[i * 4u + 3u] * best_scale + 0.5);
    pixels[i * 4u + 3u] = (uint8_t)(alpha < 255.0 ? alpha : 255.0);
  }
}
