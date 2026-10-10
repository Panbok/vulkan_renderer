#include "vkr_depot_zstd.h"

#include <zstd.h>

/* The depot's codec over zstd. Executables link the zstd they already carry:
   the depot tool its own copy, the editor and tests libktx's. */

static uint64_t depot_zstd_bound(uint64_t size) {
  return (uint64_t)ZSTD_compressBound((size_t)size);
}

static uint64_t depot_zstd_compress(void *dst, uint64_t capacity,
                                    const void *src, uint64_t size,
                                    int32_t level) {
  const size_t result =
      ZSTD_compress(dst, (size_t)capacity, src, (size_t)size, level);
  return ZSTD_isError(result) ? 0u : (uint64_t)result;
}

static uint64_t depot_zstd_decompress(void *dst, uint64_t capacity,
                                      const void *src, uint64_t size) {
  const size_t result =
      ZSTD_decompress(dst, (size_t)capacity, src, (size_t)size);
  return ZSTD_isError(result) ? UINT64_MAX : (uint64_t)result;
}

static const VkrDepotCodec depot_zstd = {
    .bound = depot_zstd_bound,
    .compress = depot_zstd_compress,
    .decompress = depot_zstd_decompress,
};

const VkrDepotCodec *vkr_depot_zstd_codec(void) { return &depot_zstd; }
