#include "defines.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include <ktx.h>
#include <math.h>
#include <stb_image.h>
#include <stb_image_write.h>

#define PREVIEW_INPUT_CAP MB(64)
#define PREVIEW_PIXEL_CAP (4u * 1024u * 1024u)

static float32_t preview_half(uint16_t value) {
  uint32_t exponent = (value >> 10) & 31u;
  uint32_t fraction = value & 1023u;
  float32_t decoded = exponent == 0 ? ldexpf((float32_t)fraction, -24)
                      : exponent == 31
                          ? (fraction ? NAN : INFINITY)
                          : ldexpf(1.0f + (float32_t)fraction / 1024.0f,
                                   (int32_t)exponent - 15);
  return value & 0x8000 ? -decoded : decoded;
}

static float32_t preview_linear(float32_t value) {
  return value <= 0.04045f ? value / 12.92f
                           : powf((value + 0.055f) / 1.055f, 2.4f);
}

static uint8_t preview_byte(float32_t linear) {
  linear = isfinite(linear) ? Clamp(linear, 0.0f, 1.0f) : 0.0f;
  float32_t srgb = linear <= 0.0031308f
                       ? linear * 12.92f
                       : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
  return (uint8_t)roundf(Clamp(srgb * 255.0f, 0.0f, 255.0f));
}

typedef struct PreviewImage {
  void *pixels;
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  bool8_t hdr;
  bool8_t half;
  bool8_t linear;
} PreviewImage;

static float32_t preview_component(const PreviewImage *image, uint32_t x,
                                   uint32_t y, uint32_t channel) {
  if (channel >= image->channels) {
    return channel == 3 ? 1.0f : 0.0f;
  }
  uint64_t index = ((uint64_t)y * image->width + x) * image->channels + channel;
  float32_t value =
      image->half  ? preview_half(((uint16_t *)image->pixels)[index])
      : image->hdr ? ((float32_t *)image->pixels)[index]
                   : (float32_t)((uint8_t *)image->pixels)[index] / 255.0f;
  if (channel == 3) {
    return Clamp(value, 0.0f, 1.0f);
  }
  if (!image->linear && !image->hdr) {
    value = preview_linear(value);
  }
  return isfinite(value) ? Max(0.0f, value) : 0.0f;
}

typedef struct PreviewWrite {
  FileHandle file;
  bool8_t success;
} PreviewWrite;

static void preview_write(void *context, void *bytes, int32_t count) {
  PreviewWrite *writer = context;
  uint64_t written = 0;
  if (writer->success && (file_write(&writer->file, (uint64_t)count, bytes,
                                     &written) != FILE_ERROR_NONE ||
                          written != (uint64_t)count)) {
    writer->success = false_v;
  }
}

int main(int argc, char **argv) {
  const char *input = NULL;
  const char *output = NULL;
  uint32_t size = 128;
  for (int32_t i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--input") && i + 1 < argc) {
      input = argv[++i];
    } else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
      output = argv[++i];
    } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
      const char *number = argv[++i];
      size = !strcmp(number, "128") ? 128 : !strcmp(number, "256") ? 256 : 0;
    } else {
      fprintf(stderr, "Usage: vkr_asset_preview --input image --output "
                      "preview.png --size 128|256\n");
      return 2;
    }
  }
  if (!input || !output || !size) {
    return 2;
  }
  Arena *arena = arena_create(MB(72), MB(1));
  if (!arena) {
    return 1;
  }
  VkrAllocator allocator = {.ctx = arena};
  if (!vkr_allocator_arena(&allocator)) {
    arena_destroy(arena);
    return 1;
  }
  bool8_t success = false_v;
  FilePath source =
      file_path_create(input, &allocator, FILE_PATH_TYPE_ABSOLUTE);
  FileStats stats = {0};
  FileHandle file = {0};
  void *stb_pixels = NULL;
  ktxTexture2 *ktx = NULL;
  PreviewWrite writer = {0};
  char temporary[4096] = {0};
  if (file_stats(&source, &stats) != FILE_ERROR_NONE || !stats.size ||
      stats.size > PREVIEW_INPUT_CAP) {
    fprintf(
        stderr,
        "Preview input is missing or exceeds the 64 MiB encoded-input cap.\n");
    goto cleanup;
  }
  FileMode read_mode = bitset8_create();
  bitset8_set(&read_mode, FILE_MODE_READ);
  uint8_t *encoded = vkr_allocator_alloc(&allocator, stats.size,
                                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  uint64_t read = 0;
  if (!encoded || file_open(&source, read_mode, &file) != FILE_ERROR_NONE ||
      file_read_into(&file, encoded, stats.size, &read) != FILE_ERROR_NONE ||
      read != stats.size) {
    goto cleanup;
  }
  file_close(&file);
  PreviewImage image = {0};
  static const uint8_t ktx_magic[12] = {0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
                                        0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
  if (read >= 12 && MemCompare(encoded, ktx_magic, 12) == 0) {
    if (ktxTexture2_CreateFromMemory(encoded, (ktx_size_t)read,
                                     KTX_TEXTURE_CREATE_NO_FLAGS,
                                     &ktx) != KTX_SUCCESS ||
        ktx->numFaces != 1 || ktx->numLayers > 1 || ktx->baseDepth > 1 ||
        (uint64_t)ktx->baseWidth * ktx->baseHeight > PREVIEW_PIXEL_CAP ||
        ktx->dataSize > PREVIEW_INPUT_CAP) {
      fprintf(
          stderr,
          "Preview requires a 2D texture within the 4-megapixel decode cap.\n");
      goto cleanup;
    }
    if (ktxTexture_LoadImageData(ktxTexture(ktx), NULL, 0) != KTX_SUCCESS) {
      goto cleanup;
    }
    if (ktxTexture2_NeedsTranscoding(ktx) &&
        ktxTexture2_TranscodeBasis(ktx, KTX_TTF_RGBA32, 0) != KTX_SUCCESS) {
      goto cleanup;
    }
    image.width = ktx->baseWidth;
    image.height = ktx->baseHeight;
    // Vulkan format numeric values are part of the KTX2 file contract.
    image.channels = ktx->vkFormat == 9 ? 1 : ktx->vkFormat == 16 ? 2 : 4;
    image.half = ktx->vkFormat == 97;
    image.hdr = image.half;
    image.linear = ktx->vkFormat != 43;
    if (ktx->vkFormat != 9 && ktx->vkFormat != 16 && ktx->vkFormat != 37 &&
        ktx->vkFormat != 43 && ktx->vkFormat != 97) {
      fprintf(stderr, "This cooked texture format has no CPU preview decoder; "
                      "preview its imported source.\n");
      goto cleanup;
    }
    ktx_size_t offset = 0;
    if (ktxTexture_GetImageOffset(ktxTexture(ktx), 0, 0, 0, &offset) !=
            KTX_SUCCESS ||
        offset + (uint64_t)image.width * image.height * image.channels *
                     (image.half ? 2 : 1) >
            ktx->dataSize) {
      goto cleanup;
    }
    image.pixels = ktx->pData + offset;
  } else {
    int32_t width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(encoded, (int32_t)read, &width, &height,
                               &channels) ||
        width <= 0 || height <= 0 ||
        (uint64_t)width * height > PREVIEW_PIXEL_CAP) {
      fprintf(stderr, "Preview image is invalid or exceeds the 4-megapixel "
                      "decode cap (64 MiB float scratch).\n");
      goto cleanup;
    }
    image.hdr = stbi_is_hdr_from_memory(encoded, (int32_t)read);
    image.linear = image.hdr;
    stb_pixels =
        image.hdr
            ? (void *)stbi_loadf_from_memory(encoded, (int32_t)read, &width,
                                             &height, &channels, 4)
            : (void *)stbi_load_from_memory(encoded, (int32_t)read, &width,
                                            &height, &channels, 4);
    if (!stb_pixels) {
      goto cleanup;
    }
    image.pixels = stb_pixels;
    image.width = (uint32_t)width;
    image.height = (uint32_t)height;
    image.channels = 4;
  }
  uint32_t width = image.width >= image.height
                       ? size
                       : Max(1u, size * image.width / image.height);
  uint32_t height = image.height >= image.width
                        ? size
                        : Max(1u, size * image.height / image.width);
  uint8_t *pixels =
      vkr_allocator_alloc(&allocator, (uint64_t)width * height * 4,
                          VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!pixels) {
    goto cleanup;
  }
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t x0 = x * image.width / width;
      uint32_t x1 = Max(x0 + 1, (x + 1) * image.width / width);
      uint32_t y0 = y * image.height / height;
      uint32_t y1 = Max(y0 + 1, (y + 1) * image.height / height);
      float32_t sum[4] = {0};
      for (uint32_t sy = y0; sy < y1; ++sy) {
        for (uint32_t sx = x0; sx < x1; ++sx) {
          float32_t alpha = preview_component(&image, sx, sy, 3);
          for (uint32_t c = 0; c < 3; ++c) {
            sum[c] += preview_component(&image, sx, sy, c) * alpha;
          }
          sum[3] += alpha;
        }
      }
      for (uint32_t c = 0; c < 3; ++c) {
        float32_t value = sum[3] > 0 ? sum[c] / sum[3] : 0;
        // Fixed exposure 1, Reinhard tonemap; independent of the active scene.
        if (image.hdr) {
          value = value / (1.0f + value);
        }
        pixels[((uint64_t)y * width + x) * 4 + c] = preview_byte(value);
      }
      pixels[((uint64_t)y * width + x) * 4 + 3] =
          (uint8_t)roundf(255.0f * sum[3] / ((x1 - x0) * (y1 - y0)));
    }
  }
  int32_t length = snprintf(temporary, sizeof(temporary), "%s.part.%u", output,
                            vkr_platform_get_process_id());
  if (length <= 0 || (uint32_t)length >= sizeof(temporary)) {
    goto cleanup;
  }
  FilePath destination =
      file_path_create(temporary, &allocator, FILE_PATH_TYPE_ABSOLUTE);
  FileMode write_mode = bitset8_create();
  bitset8_set(&write_mode, FILE_MODE_WRITE);
  bitset8_set(&write_mode, FILE_MODE_CREATE);
  bitset8_set(&write_mode, FILE_MODE_TRUNCATE);
  writer.success =
      file_open(&destination, write_mode, &writer.file) == FILE_ERROR_NONE;
  if (!writer.success ||
      !stbi_write_png_to_func(preview_write, &writer, (int32_t)width,
                              (int32_t)height, 4, pixels, (int32_t)width * 4) ||
      !writer.success) {
    goto cleanup;
  }
  if (file_sync(&writer.file) != FILE_ERROR_NONE) {
    goto cleanup;
  }
  file_close(&writer.file);
  FilePath final =
      file_path_create(output, &allocator, FILE_PATH_TYPE_ABSOLUTE);
  success = file_rename(&destination, &final, true_v) == FILE_ERROR_NONE;
  if (success) {
    printf("Preview %ux%u from %ux%u; fixed exposure=1 Reinhard for HDR.\n",
           width, height, image.width, image.height);
  }
cleanup:
  file_close(&file);
  file_close(&writer.file);
  if (!success && temporary[0]) {
    remove(temporary);
  }
  if (ktx) {
    ktxTexture_Destroy(ktxTexture(ktx));
  }
  stbi_image_free(stb_pixels);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  return success ? 0 : 1;
}
