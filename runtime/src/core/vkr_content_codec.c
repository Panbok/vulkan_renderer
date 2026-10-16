#include "core/vkr_content_codec.h"

#include "filesystem/vkr_vfs.h"

#include "ktx-software/external/basisu/zstd/zstd.h"

vkr_internal bool8_t vkr_content_codec_decompress(const uint8_t *source,
                                                  uint64_t source_size,
                                                  uint8_t *destination,
                                                  uint64_t destination_size) {
  const size_t decoded = ZSTD_decompress(destination, (size_t)destination_size,
                                         source, (size_t)source_size);
  return !ZSTD_isError(decoded) && decoded == destination_size;
}

void vkr_content_codec_install(void) {
  vkr_vfs_set_decompressor(vkr_content_codec_decompress);
}

vkr_internal uint64_t vkr_content_codec_decode(void *destination,
                                               uint64_t capacity,
                                               const void *source,
                                               uint64_t size) {
  const size_t decoded =
      ZSTD_decompress(destination, (size_t)capacity, source, (size_t)size);
  return ZSTD_isError(decoded) ? UINT64_MAX : (uint64_t)decoded;
}

static const VkrCodec vkr_content_codec_zstd = {
    .decompress = vkr_content_codec_decode,
};

const VkrCodec *vkr_content_codec_zstd_decoder(void) {
  return &vkr_content_codec_zstd;
}
