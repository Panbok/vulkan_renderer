#pragma once
#include "vkr_renderer.h"

/** Prepared bytes borrowed until texture publication returns; the publisher
 * copies them before it returns. `upload_data` and `upload_regions` are
 * malloc-owned: decode workers produce them in parallel without a shared
 * allocator lock, payloads reach tens of MB, and freeing returns those pages,
 * which VkrDMemory never decommits. vkr_texture_system_release_prepared_load()
 * frees them. */
typedef struct VkrTexturePreparedLoad {
  VkrTextureDescription description;
  uint8_t *upload_data;
  uint64_t upload_data_size;
  VkrTextureUploadRegion *upload_regions;
  uint32_t upload_region_count;
  uint32_t upload_mip_levels;
  uint32_t upload_array_layers;
  bool8_t upload_is_compressed;
} VkrTexturePreparedLoad;
