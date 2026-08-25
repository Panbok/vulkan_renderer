#pragma once

#include "containers/str.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_renderer.h"

/** GPU-ready texture payload persisted across renderer processes. */
typedef struct VkrTextureTranscodeCacheRecord {
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  VkrTextureFormat format;
  uint32_t mip_levels;
  uint32_t array_layers;
  bool8_t is_compressed;
  bool8_t has_transparency;
  bool8_t alpha_mask;
  uint8_t *data;
  uint64_t data_size;
  VkrTextureUploadRegion *regions;
  uint32_t region_count;
} VkrTextureTranscodeCacheRecord;

/**
 * Loads a source- and target-validated persistent transcode payload.
 * Returned data and regions are malloc-owned and released with
 * vkr_texture_transcode_cache_release(). A miss or corrupt entry returns false.
 */
bool8_t vkr_texture_transcode_cache_load(
    VkrAllocator *scratch, String8 source_path, const uint8_t *source_data,
    uint64_t source_size, VkrTextureFormat target_format,
    uint32_t expected_width, uint32_t expected_height,
    uint32_t expected_mip_levels, uint32_t expected_array_layers,
    VkrTextureTranscodeCacheRecord *out_record);

/** Atomically writes one validated GPU-ready texture payload. */
bool8_t vkr_texture_transcode_cache_store(
    VkrAllocator *scratch, String8 source_path, const uint8_t *source_data,
    uint64_t source_size, const VkrTextureTranscodeCacheRecord *record);

/** Releases malloc-owned data returned by the cache loader. */
void vkr_texture_transcode_cache_release(
    VkrTextureTranscodeCacheRecord *record);

/** Returns the deterministic cache path for diagnostics and focused tests. */
bool8_t vkr_texture_transcode_cache_path(VkrAllocator *allocator,
                                         String8 source_path,
                                         VkrTextureFormat target_format,
                                         String8 *out_path);
