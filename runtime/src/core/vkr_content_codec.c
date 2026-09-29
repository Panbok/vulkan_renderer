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
