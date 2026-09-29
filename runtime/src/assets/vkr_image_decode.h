#pragma once

#include <stddef.h>
#include <stdint.h>

/* Decodes an encoded image (any format stb_image reads) to RGBA8 with rows
 * top first, or bottom first with `flip`. 8-bit RGB and RGBA PNGs without a
 * transparency key take a faster path (libdeflate and vectorized unfiltering)
 * that yields the same pixels as stb_image. Returns malloc storage the caller
 * releases with free() or stbi_image_free(), or NULL when decoding fails. */
uint8_t *vkr_image_decode_rgba8(const uint8_t *bytes, size_t size, int flip,
                                uint32_t *out_width, uint32_t *out_height);

/* Memory for vkr_image_decode_rgba8_into, all of it passed `context`:
 * `pixels` returns width * height * 4 writable bytes for the image, or NULL
 * to fail, and may be asked again, replacing the earlier bytes, when the PNG
 * path gives the image to stb_image; `allocate` and `release` provide
 * scratch, all released before the decode returns. */
typedef struct VkrImageDecodeTarget {
  uint8_t *(*pixels)(void *context, uint32_t width, uint32_t height);
  void *(*allocate)(void *context, size_t size);
  void (*release)(void *context, void *memory, size_t size);
  void *context;
} VkrImageDecodeTarget;

/* As vkr_image_decode_rgba8, into the memory `target` provides, so a caller
 * that decodes many images can reuse it. Returns nonzero on success. */
int vkr_image_decode_rgba8_into(const uint8_t *bytes, size_t size, int flip,
                                const VkrImageDecodeTarget *target);
