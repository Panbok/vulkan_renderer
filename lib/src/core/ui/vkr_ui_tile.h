/**
 * @file vkr_ui_tile.h
 * @brief Fixed-grid UI command bins, ordered content hashes, and damage.
 */
#pragma once

#include "core/ui/vkr_ui_draw.h"
#include "memory/vkr_allocator.h"

#define VKR_UI_TILE_SIZE_PX 64u

/** Previous and current filter-expanded draw bounds for one retained widget. */
typedef struct VkrUiTileDamage {
  VkrUiRect previous_aabb_px;
  VkrUiRect current_aabb_px;
} VkrUiTileDamage;

/** Retained tile hashes and command bins. Storage belongs to
 * retained_allocator. */
typedef struct VkrUiTileCache {
  VkrAllocator *retained_allocator;
  uint64_t *previous_hashes;
  uint32_t *tile_offsets;
  uint32_t *command_indices;
  uint64_t previous_stream_hash;
  uint32_t column_count;
  uint32_t row_count;
  uint32_t tile_count;
  uint32_t tile_size_px;
  uint32_t target_width;
  uint32_t target_height;
  uint32_t command_index_capacity;
  uint32_t bin_entry_count;
  bool8_t initialized;
} VkrUiTileCache;

/** Retained bins plus scratch-backed damage for one frame. */
typedef struct VkrUiTileFrame {
  uint32_t *tile_offsets;
  uint32_t *command_indices;
  uint8_t *dirty_tiles;
  uint32_t column_count;
  uint32_t row_count;
  uint32_t tile_count;
  uint32_t bin_entry_count;
  uint32_t dirty_tile_count;
  float32_t dirty_tile_ratio;
} VkrUiTileFrame;

void vkr_ui_tile_cache_init(VkrUiTileCache *cache,
                            VkrAllocator *retained_allocator);
void vkr_ui_tile_cache_destroy(VkrUiTileCache *cache);

/** Returns a command's clipped AABB, including the MTSDF filter footprint. */
VkrUiRect vkr_ui_tile_command_aabb(const VkrUiDrawCommand *command,
                                   uint32_t target_width,
                                   uint32_t target_height);

/** Builds or reuses ordered tile bins and updates retained hashes. */
bool8_t
vkr_ui_tile_build(VkrUiTileCache *cache, VkrAllocator *frame_allocator,
                  uint32_t target_width, uint32_t target_height,
                  uint32_t tile_size_px, const VkrUiDrawCommand *commands,
                  uint32_t command_count, const VkrUiTileDamage *motion_damage,
                  uint32_t motion_damage_count, VkrUiTileFrame *out_frame);
