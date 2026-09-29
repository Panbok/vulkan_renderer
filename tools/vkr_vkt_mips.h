#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

// The byte a linear value in [0, 1] encodes to in sRGB.
static inline uint8_t vkr_vkt_srgb_encode_byte(double value) {
  value = value <= 0.0031308 ? value * 12.92
                             : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
  return (uint8_t)floor(value * 255.0 + 0.5);
}

// thresholds[k - 1] is the smallest value vkr_vkt_srgb_encode_byte maps to k
// or more, found by bisecting over doubles with that function itself, so a
// search of the table returns its bytes without a pow per value. Checked
// against it on 151 million values, including 2,000 ulps around every
// threshold.
static inline void vkr_vkt_srgb_encode_thresholds(double thresholds[255]) {
  for (uint32_t k = 1u; k <= 255u; ++k) {
    double low = 0.0;
    double high = 1.0;
    for (;;) {
      const double mid = low + (high - low) * 0.5;
      if (mid <= low || mid >= high) {
        break;
      }
      if (vkr_vkt_srgb_encode_byte(mid) >= k) {
        high = mid;
      } else {
        low = mid;
      }
    }
    thresholds[k - 1u] = high;
  }
}

static inline uint8_t vkr_vkt_srgb_encode_search(const double thresholds[255],
                                                 double value) {
  uint32_t low = 0u;
  uint32_t high = 255u;
  while (low < high) {
    const uint32_t mid = (low + high) / 2u;
    if (thresholds[mid] <= value) {
      low = mid + 1u;
    } else {
      high = mid;
    }
  }
  return (uint8_t)low;
}

// Destinations at least this large encode sRGB through the threshold table;
// building it costs about as many pow calls as 5,000 destination texels.
#define VKR_VKT_SRGB_TABLE_MIN_TEXELS 16384u

// Buckets of [0, 1] narrower than the closest pair of sRGB thresholds (3.0e-4
// apart, near zero, against 2.4e-4), so each holds at most one threshold.
#define VKR_VKT_SRGB_BUCKETS 4096u

// The threshold table with, for each bucket, the byte its lower edge encodes
// to: a value's byte is that byte, or the next when the value reaches the
// bucket's one threshold. `exact` is zero if a bucket held two thresholds,
// and encodes then search the table instead.
typedef struct VkrVktSrgbEncodeTable {
  double thresholds[255];
  uint8_t bucket_low[VKR_VKT_SRGB_BUCKETS + 1u];
  int exact;
} VkrVktSrgbEncodeTable;

static inline void
vkr_vkt_srgb_encode_table_init(VkrVktSrgbEncodeTable *table) {
  vkr_vkt_srgb_encode_thresholds(table->thresholds);
  table->exact = 1;
  for (uint32_t k = 1u; k < 255u; ++k) {
    if (table->thresholds[k] - table->thresholds[k - 1u] <=
        1.0 / (double)VKR_VKT_SRGB_BUCKETS) {
      table->exact = 0;
    }
  }
  for (uint32_t bucket = 0u; bucket <= VKR_VKT_SRGB_BUCKETS; ++bucket) {
    table->bucket_low[bucket] = vkr_vkt_srgb_encode_search(
        table->thresholds, (double)bucket / (double)VKR_VKT_SRGB_BUCKETS);
  }
}

// The byte vkr_vkt_srgb_encode_search returns for a value in [0, 1].
static inline uint8_t
vkr_vkt_srgb_encode_bucket(const VkrVktSrgbEncodeTable *table, double value) {
  if (!table->exact) {
    return vkr_vkt_srgb_encode_search(table->thresholds, value);
  }
  const uint32_t bucket = value < 1.0
                              ? (uint32_t)(value * (double)VKR_VKT_SRGB_BUCKETS)
                              : VKR_VKT_SRGB_BUCKETS;
  const uint32_t low = table->bucket_low[bucket];
  return (uint8_t)(low + (low < 255u && table->thresholds[low] <= value));
}

// Offline RGBA8 filtering. The caller owns disjoint source/destination storage
// and supplies positive extents (at most the packer's 16384-texel limit).
// Destination dimensions are max(1, source / 2). Alpha is always linear.
// `table`, when not null, is an initialized sRGB encode table the caller
// shares between calls.
static inline void vkr_vkt_downsample_rgba8_with_table(
    const uint8_t *source, uint32_t width, uint32_t height,
    uint8_t *destination, uint32_t next_width, uint32_t next_height, int srgb,
    int alpha_weighted, const VkrVktSrgbEncodeTable *table) {
  double rgb_decode[256];
  double alpha_decode[256];
  for (uint32_t i = 0; i < 256u; ++i) {
    const double value = (double)i / 255.0;
    alpha_decode[i] = value;
    rgb_decode[i] = srgb
                        ? (value <= 0.04045 ? value / 12.92
                                            : pow((value + 0.055) / 1.055, 2.4))
                        : value;
  }

  double srgb_thresholds[255];
  const int srgb_table = srgb && (uint64_t)next_width * next_height >=
                                     VKR_VKT_SRGB_TABLE_MIN_TEXELS;
  if (srgb_table && !table) {
    vkr_vkt_srgb_encode_thresholds(srgb_thresholds);
  }

  // Halving both power-of-two extents weighs each of a texel's four sources
  // by the same power of two, and the inverse area is one too, so the
  // weighted sums below equal plain sums in the same order, times 1/4: the
  // same doubles without the overlap arithmetic.
  const int power_of_two = (next_width & (next_width - 1u)) == 0u &&
                           (next_height & (next_height - 1u)) == 0u;
  if (!alpha_weighted && power_of_two && width == next_width * 2u &&
      height == next_height * 2u) {
    for (uint32_t y = 0; y < next_height; ++y) {
      const uint8_t *top_row = source + (size_t)y * 2u * width * 4u;
      const uint8_t *bottom_row = top_row + (size_t)width * 4u;
      uint8_t *pixel = destination + (size_t)y * next_width * 4u;
      for (uint32_t x = 0; x < next_width; ++x, pixel += 4u) {
        const uint8_t *a = top_row + (size_t)x * 8u;
        const uint8_t *b = a + 4u;
        const uint8_t *c = bottom_row + (size_t)x * 8u;
        const uint8_t *d = c + 4u;
        for (uint32_t channel = 0; channel < 3u; ++channel) {
          const double value =
              (((rgb_decode[a[channel]] + rgb_decode[b[channel]]) +
                rgb_decode[c[channel]]) +
               rgb_decode[d[channel]]) *
              0.25;
          if (!srgb) {
            pixel[channel] = (uint8_t)floor(value * 255.0 + 0.5);
          } else if (!srgb_table) {
            pixel[channel] = vkr_vkt_srgb_encode_byte(value);
          } else if (table) {
            pixel[channel] = vkr_vkt_srgb_encode_bucket(table, value);
          } else {
            pixel[channel] = vkr_vkt_srgb_encode_search(srgb_thresholds, value);
          }
        }
        const double alpha =
            (((alpha_decode[a[3]] + alpha_decode[b[3]]) + alpha_decode[c[3]]) +
             alpha_decode[d[3]]) *
            0.25;
        pixel[3] = (uint8_t)floor(alpha * 255.0 + 0.5);
      }
    }
    return;
  }

  // Coordinates use units of 1/next_extent source texels. Integer overlap
  // weights retain every edge and split shared texels exactly for odd extents.
  // An exactly halved extent covers source texels 2i and 2i + 1, which the
  // divisions would also give.
  const double inverse_area = 1.0 / ((double)width * height);
  const int halved_x = width == next_width * 2u;
  const int halved_y = height == next_height * 2u;
  for (uint32_t y = 0; y < next_height; ++y) {
    const uint32_t top = y * height;
    const uint32_t bottom = top + height;
    const uint32_t first_y = halved_y ? y * 2u : top / next_height;
    const uint32_t end_y =
        halved_y ? y * 2u + 2u : (bottom + next_height - 1u) / next_height;
    for (uint32_t x = 0; x < next_width; ++x) {
      const uint32_t left = x * width;
      const uint32_t right = left + width;
      const uint32_t first_x = halved_x ? x * 2u : left / next_width;
      const uint32_t end_x =
          halved_x ? x * 2u + 2u : (right + next_width - 1u) / next_width;
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
          const double alpha = alpha_decode[pixel[3]];
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
          pixel[channel] =
              !srgb_table ? vkr_vkt_srgb_encode_byte(value)
              : table     ? vkr_vkt_srgb_encode_bucket(table, value)
                          : vkr_vkt_srgb_encode_search(srgb_thresholds, value);
          continue;
        }
        pixel[channel] = (uint8_t)floor(value * 255.0 + 0.5);
      }
    }
  }
}

static inline void
vkr_vkt_downsample_rgba8(const uint8_t *source, uint32_t width, uint32_t height,
                         uint8_t *destination, uint32_t next_width,
                         uint32_t next_height, int srgb, int alpha_weighted) {
  vkr_vkt_downsample_rgba8_with_table(source, width, height, destination,
                                      next_width, next_height, srgb,
                                      alpha_weighted, NULL);
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
