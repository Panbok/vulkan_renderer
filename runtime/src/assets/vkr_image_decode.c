#include "assets/vkr_image_decode.h"

#include "defines.h"

#include <libdeflate.h>
#include <stb_image.h>
#include <stdlib.h>
#include <string.h>

/* SSSE3 (every x86-64-v2 and later core, and the builds' -march/arch levels)
 * reverses the per-pixel PNG filters with every channel in one register. */
#if defined(__SSSE3__) ||                                                      \
    (defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64))
#include <tmmintrin.h>
#define VKR_PNG_SIMD 1
#endif

vkr_internal uint32_t vkr_png_u32(const uint8_t *bytes) {
  return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
         ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

vkr_internal uint8_t vkr_png_paeth(uint8_t a, uint8_t b, uint8_t c) {
  const int32_t p = (int32_t)a + (int32_t)b - (int32_t)c;
  const int32_t pa = abs(p - (int32_t)a);
  const int32_t pb = abs(p - (int32_t)b);
  const int32_t pc = abs(p - (int32_t)c);
  if (pa <= pb && pa <= pc) {
    return a;
  }
  return pb <= pc ? b : c;
}

#if VKR_PNG_SIMD
/* One pixel as 16-bit lanes. A 3-byte pixel reads a fourth byte, which the
   raw buffer pads; its lane is never stored. */
vkr_internal inline __m128i vkr_png_load(const uint8_t *bytes) {
  uint32_t value;
  MemCopy(&value, bytes, 4u);
  return _mm_unpacklo_epi8(_mm_cvtsi32_si128((int32_t)value),
                           _mm_setzero_si128());
}

vkr_internal inline void vkr_png_store(uint8_t *bytes, __m128i pixel,
                                       uint32_t bpp) {
  const uint32_t value =
      (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(pixel, pixel));
  if (bpp == 4u) {
    MemCopy(bytes, &value, 4u);
  } else {
    MemCopy(bytes, &value, 2u);
    bytes[2] = (uint8_t)(value >> 16u);
  }
}

/* The Sub, Average and Paeth filters of one row after the first, a pixel at a
   time: each depends on the pixel just decoded. The lane arithmetic matches
   vkr_png_paeth and the byte loops, with sums taken modulo 256. */
vkr_internal inline void vkr_png_unfilter_pixels(uint8_t filter, uint8_t *row,
                                                 const uint8_t *prior,
                                                 size_t length, uint32_t bpp) {
  const __m128i mask = _mm_set1_epi16(0xff);
  __m128i left = _mm_setzero_si128();
  __m128i corner = _mm_setzero_si128();
  for (size_t i = 0; i + bpp <= length; i += bpp) {
    const __m128i current = vkr_png_load(row + i);
    const __m128i up = vkr_png_load(prior + i);
    __m128i predicted;
    if (filter == 1u) {
      predicted = left;
    } else if (filter == 3u) {
      predicted = _mm_srli_epi16(_mm_add_epi16(left, up), 1);
    } else {
      const __m128i pa = _mm_abs_epi16(_mm_sub_epi16(up, corner));
      const __m128i pb = _mm_abs_epi16(_mm_sub_epi16(left, corner));
      const __m128i pc = _mm_abs_epi16(_mm_sub_epi16(
          _mm_add_epi16(left, up), _mm_add_epi16(corner, corner)));
      /* a when pa <= pb and pa <= pc, else b when pb <= pc, else c. */
      const __m128i not_a =
          _mm_or_si128(_mm_cmpgt_epi16(pa, pb), _mm_cmpgt_epi16(pa, pc));
      const __m128i take_c = _mm_cmpgt_epi16(pb, pc);
      const __m128i b_or_c = _mm_or_si128(_mm_and_si128(take_c, corner),
                                          _mm_andnot_si128(take_c, up));
      predicted = _mm_or_si128(_mm_and_si128(not_a, b_or_c),
                               _mm_andnot_si128(not_a, left));
    }
    const __m128i decoded =
        _mm_and_si128(_mm_add_epi16(current, predicted), mask);
    vkr_png_store(row + i, decoded, bpp);
    left = decoded;
    corner = up;
  }
}
#endif

/* Reverses one row's filter in place; `prior` is the previous unfiltered row
   or NULL for the first, whose missing neighbours read as zero. */
vkr_internal bool8_t vkr_png_unfilter(uint8_t filter, uint8_t *row,
                                      const uint8_t *prior, size_t length,
                                      uint32_t bpp) {
#if VKR_PNG_SIMD
  if (prior && (filter == 1u || filter == 3u || filter == 4u)) {
    /* Constant pixel sizes let each call inline its loads and stores. */
    if (bpp == 4u) {
      vkr_png_unfilter_pixels(filter, row, prior, length, 4u);
    } else {
      vkr_png_unfilter_pixels(filter, row, prior, length, 3u);
    }
    return true_v;
  }
#endif
  switch (filter) {
  case 0u:
    return true_v;
  case 1u:
    for (size_t i = bpp; i < length; ++i) {
      row[i] = (uint8_t)(row[i] + row[i - bpp]);
    }
    return true_v;
  case 2u:
    for (size_t i = 0; prior && i < length; ++i) {
      row[i] = (uint8_t)(row[i] + prior[i]);
    }
    return true_v;
  case 3u:
    for (size_t i = 0; i < length; ++i) {
      const uint32_t left = i >= bpp ? row[i - bpp] : 0u;
      const uint32_t up = prior ? prior[i] : 0u;
      row[i] = (uint8_t)(row[i] + ((left + up) >> 1u));
    }
    return true_v;
  case 4u:
    for (size_t i = 0; i < length; ++i) {
      const uint8_t left = i >= bpp ? row[i - bpp] : 0u;
      const uint8_t up = prior ? prior[i] : 0u;
      const uint8_t corner = prior && i >= bpp ? prior[i - bpp] : 0u;
      row[i] = (uint8_t)(row[i] + vkr_png_paeth(left, up, corner));
    }
    return true_v;
  default:
    return false_v;
  }
}

/* The fast path: 8-bit, non-interlaced RGB or RGBA without a transparency
   key or Apple's CgBI variant. Returns false for anything else or on any
   error, leaving the image to stb_image. */
vkr_internal bool8_t vkr_png_decode_rgba8(const uint8_t *data, size_t size,
                                          int flip,
                                          const VkrImageDecodeTarget *target) {
  static const uint8_t signature[8] = {0x89, 'P',  'N',  'G',
                                       '\r', '\n', 0x1a, '\n'};
  if (!data || size < 8u + 25u || MemCompare(data, signature, 8u) != 0) {
    return false_v;
  }
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t bpp = 0u;
  size_t idat_size = 0u;
  for (size_t offset = 8u; offset + 12u <= size;) {
    const uint32_t length = vkr_png_u32(data + offset);
    const uint8_t *type = data + offset + 4u;
    const uint8_t *body = data + offset + 8u;
    if (length > size - offset - 12u) {
      return false_v;
    }
    if (offset == 8u) {
      if (MemCompare(type, "IHDR", 4u) != 0 || length != 13u) {
        return false_v;
      }
      width = vkr_png_u32(body);
      height = vkr_png_u32(body + 4u);
      if (body[8] != 8u || (body[9] != 2u && body[9] != 6u) || body[10] != 0u ||
          body[11] != 0u || body[12] != 0u || width == 0u || height == 0u ||
          width > (1u << 24u) || height > (1u << 24u)) {
        return false_v;
      }
      bpp = body[9] == 6u ? 4u : 3u;
    } else if (MemCompare(type, "IDAT", 4u) == 0) {
      idat_size += length;
    } else if (MemCompare(type, "tRNS", 4u) == 0 ||
               MemCompare(type, "CgBI", 4u) == 0) {
      return false_v;
    } else if (MemCompare(type, "IEND", 4u) == 0) {
      break;
    }
    offset += 12u + (size_t)length;
  }
  const size_t stride = (size_t)width * bpp;
  const size_t raw_size = (stride + 1u) * height;
  if (!bpp || !idat_size || raw_size / (stride + 1u) != height) {
    return false_v;
  }

  void *context = target->context;
  uint8_t *idat = (uint8_t *)target->allocate(context, idat_size);
  /* Padded so a 3-byte pixel at the end may read four bytes. */
  uint8_t *raw = (uint8_t *)target->allocate(context, raw_size + 4u);
  uint8_t *pixels = target->pixels(context, width, height);
  struct libdeflate_decompressor *decompressor =
      libdeflate_alloc_decompressor();
  bool8_t ok = idat && raw && pixels && decompressor;
  size_t gathered = 0u;
  for (size_t offset = 8u; ok && offset + 12u <= size;) {
    const uint32_t length = vkr_png_u32(data + offset);
    if (MemCompare(data + offset + 4u, "IDAT", 4u) == 0) {
      MemCopy(idat + gathered, data + offset + 8u, length);
      gathered += length;
    } else if (MemCompare(data + offset + 4u, "IEND", 4u) == 0) {
      break;
    }
    offset += 12u + (size_t)length;
  }

  size_t produced = 0u;
  ok = ok &&
       libdeflate_zlib_decompress(decompressor, idat, idat_size, raw, raw_size,
                                  &produced) == LIBDEFLATE_SUCCESS &&
       produced == raw_size;
  for (uint32_t y = 0u; ok && y < height; ++y) {
    uint8_t *row = raw + (size_t)y * (stride + 1u);
    const uint8_t *prior = y ? row - stride : NULL;
    ok = vkr_png_unfilter(row[0], row + 1u, prior, stride, bpp);
    if (!ok) {
      break;
    }
    uint8_t *out = pixels + (size_t)(flip ? height - 1u - y : y) * width * 4u;
    if (bpp == 4u) {
      MemCopy(out, row + 1u, stride);
    } else {
      const uint8_t *in = row + 1u;
      for (uint32_t x = 0u; x < width; ++x) {
        out[x * 4u] = in[x * 3u];
        out[x * 4u + 1u] = in[x * 3u + 1u];
        out[x * 4u + 2u] = in[x * 3u + 2u];
        out[x * 4u + 3u] = 255u;
      }
    }
  }

  if (decompressor) {
    libdeflate_free_decompressor(decompressor);
  }
  if (raw) {
    target->release(context, raw, raw_size + 4u);
  }
  if (idat) {
    target->release(context, idat, idat_size);
  }
  return ok;
}

int vkr_image_decode_rgba8_into(const uint8_t *bytes, size_t size, int flip,
                                const VkrImageDecodeTarget *target) {
  if (!bytes || !target || size == 0u || size > INT32_MAX) {
    return 0;
  }
  if (vkr_png_decode_rgba8(bytes, size, flip, target)) {
    return 1;
  }
  int32_t width = 0;
  int32_t height = 0;
  int32_t channels = 0;
  stbi_set_flip_vertically_on_load_thread(flip);
  uint8_t *decoded = stbi_load_from_memory(bytes, (int32_t)size, &width,
                                           &height, &channels, 4);
  stbi_set_flip_vertically_on_load_thread(0);
  uint8_t *pixels =
      decoded && width > 0 && height > 0
          ? target->pixels(target->context, (uint32_t)width, (uint32_t)height)
          : NULL;
  if (pixels) {
    MemCopy(pixels, decoded, (size_t)width * (size_t)height * 4u);
  }
  stbi_image_free(decoded);
  return pixels != NULL;
}

/* The target of vkr_image_decode_rgba8: malloc storage it hands over. */
typedef struct VkrImageDecodeMalloc {
  uint8_t *pixels;
  uint32_t width;
  uint32_t height;
} VkrImageDecodeMalloc;

vkr_internal uint8_t *
vkr_image_decode_malloc_pixels(void *context, uint32_t width, uint32_t height) {
  VkrImageDecodeMalloc *decoded = context;
  free(decoded->pixels);
  decoded->pixels = (uint8_t *)malloc((size_t)width * height * 4u);
  decoded->width = width;
  decoded->height = height;
  return decoded->pixels;
}

vkr_internal void *vkr_image_decode_malloc(void *context, size_t size) {
  (void)context;
  return malloc(size);
}

vkr_internal void vkr_image_decode_free(void *context, void *memory,
                                        size_t size) {
  (void)context;
  (void)size;
  free(memory);
}

uint8_t *vkr_image_decode_rgba8(const uint8_t *bytes, size_t size, int flip,
                                uint32_t *out_width, uint32_t *out_height) {
  if (!out_width || !out_height) {
    return NULL;
  }
  VkrImageDecodeMalloc decoded = {0};
  const VkrImageDecodeTarget target = {
      .pixels = vkr_image_decode_malloc_pixels,
      .allocate = vkr_image_decode_malloc,
      .release = vkr_image_decode_free,
      .context = &decoded,
  };
  if (!vkr_image_decode_rgba8_into(bytes, size, flip, &target)) {
    free(decoded.pixels);
    return NULL;
  }
  *out_width = decoded.width;
  *out_height = decoded.height;
  return decoded.pixels;
}
