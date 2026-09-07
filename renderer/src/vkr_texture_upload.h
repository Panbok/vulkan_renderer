#pragma once
#include "vkr_renderer.h"

/** Prepared bytes borrowed until texture publication returns. */
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
