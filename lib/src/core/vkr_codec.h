#pragma once

#include "defines.h"

/**
 * A byte compressor passed in by the executable that links one. Libraries
 * that store compressed data take a codec instead of linking a compressor,
 * so a process carries one copy (the depot and recordings use zstd).
 */
typedef struct VkrCodec {
  uint64_t (*bound)(uint64_t size);
  /* Returns the compressed size, or zero on failure. */
  uint64_t (*compress)(void *dst, uint64_t capacity, const void *src,
                       uint64_t size, int32_t level);
  /* Returns the decoded size, or UINT64_MAX on failure. */
  uint64_t (*decompress)(void *dst, uint64_t capacity, const void *src,
                         uint64_t size);
} VkrCodec;
