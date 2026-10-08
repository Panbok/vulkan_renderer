#pragma once
#include "vkr_renderer.h"

/** Who holds a prepared texture's `upload_data` and `upload_regions` while
 * the renderer's table (vkr_publication_queue.c) records its publication. A
 * native table uploads before it returns and ignores the ownership. */
typedef enum VkrTextureUploadOwnership {
  /* Borrowed until publication returns: the renderer's table copies them. */
  VKR_TEXTURE_UPLOAD_COPIED = 0,
  /* The caller keeps them alive and unchanged until this publication's
     completion, so the renderer's table records them without copying. */
  VKR_TEXTURE_UPLOAD_RETAINED,
  /* A publication the renderer's table accepts takes them without copying
     and frees them once its batch has run, after the native upload copied
     them into backend staging. vkr_texture_system_finalize_prepared_load()
     clears the caller's pointers when this happens; a payload too large to
     hold twice, such as a lightmap set (ADR-088), loads this way. */
  VKR_TEXTURE_UPLOAD_TRANSFERRED,
} VkrTextureUploadOwnership;

/** Prepared bytes for one texture publication; `upload_ownership` says how
 * the publication holds them. `upload_data` and `upload_regions` are
 * malloc-owned: decode workers produce them in parallel without a shared
 * allocator lock, payloads reach hundreds of MB, and freeing returns those
 * pages, which VkrDMemory never decommits. While the caller owns them,
 * vkr_texture_system_release_prepared_load() frees them. */
typedef struct VkrTexturePreparedLoad {
  VkrTextureDescription description;
  uint8_t *upload_data;
  uint64_t upload_data_size;
  VkrTextureUploadRegion *upload_regions;
  uint32_t upload_region_count;
  uint32_t upload_mip_levels;
  uint32_t upload_array_layers;
  bool8_t upload_is_compressed;
  VkrTextureUploadOwnership upload_ownership;
  /* The stored chain's larger base side and level count when a load limit
     can drop its mips, else zeros. The texture system keeps them with the
     texture; the renderer ignores them. */
  uint32_t limit_source_extent;
  uint32_t limit_source_mip_levels;
} VkrTexturePreparedLoad;
